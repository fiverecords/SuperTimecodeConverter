#pragma once
#include <JuceHeader.h>
#include "TimecodeCore.h"
#include <atomic>
#include <cmath>

class LtcOutput : private juce::AudioIODeviceCallback
{
public:
    LtcOutput() = default;
    ~LtcOutput() { stop(); }

    //==============================================================================
    // channel: 0+ = specific channel, -1 = Ch 1 + Ch 2
    // sampleRate: preferred sample rate (0 = device default)
    // bufferSize: preferred buffer size (0 = device default)
    //==============================================================================
    bool start(const juce::String& typeName, const juce::String& devName,
               int channel = 0, double sampleRate = 0, int bufferSize = 0)
    {
        stop();
        selectedChannel = channel;
        currentDeviceName = devName;

        deviceManager.closeAudioDevice();

        // Register all device types without opening anything
        deviceManager.initialise(0, 128, nullptr, false);

        // Switch to requested type WITHOUT auto-opening a device (false)
        if (typeName.isNotEmpty())
            deviceManager.setCurrentAudioDeviceType(typeName, false);

        // Scan so the device name is recognised
        if (auto* type = deviceManager.getCurrentDeviceTypeObject())
            type->scanForDevices();

        // Open the specific device â€“ this is the ONLY open call
        auto setup = deviceManager.getAudioDeviceSetup();
        setup.outputDeviceName  = devName;
        setup.inputDeviceName   = "";
        setup.useDefaultInputChannels  = false;
        setup.useDefaultOutputChannels = true;
        if (sampleRate > 0)  setup.sampleRate = sampleRate;
        if (bufferSize > 0)  setup.bufferSize = bufferSize;
        auto err = deviceManager.setAudioDeviceSetup(setup, true);
        if (err.isNotEmpty())
            return false;

        auto* device = deviceManager.getCurrentAudioDevice();
        if (!device)
            return false;

        numChannelsAvailable = device->getActiveOutputChannels().countNumberOfSetBits();
        if (selectedChannel >= 0 && selectedChannel >= numChannelsAvailable)
            selectedChannel = 0;
        if (selectedChannel == -1 && numChannelsAvailable < 2)
            selectedChannel = 0;

        currentSampleRate = device->getCurrentSampleRate();
        currentBufferSize = device->getCurrentBufferSizeSamples();

        resetEncoder();
        deviceManager.addAudioCallback(this);
        isRunningFlag = true;
        return true;
    }

    void stop()
    {
        if (isRunningFlag)
        {
            deviceManager.removeAudioCallback(this);
            deviceManager.closeAudioDevice();
            isRunningFlag = false;
        }
    }

    bool getIsRunning() const { return isRunningFlag; }
    juce::String getCurrentDeviceName() const { return currentDeviceName; }
    int getSelectedChannel() const { return selectedChannel; }
    int getChannelCount() const { return numChannelsAvailable; }
    bool isStereoMode() const { return selectedChannel == -1; }
    double getActualSampleRate() const { return currentSampleRate; }
    int getActualBufferSize() const { return currentBufferSize; }

    void setTimecode(const Timecode& tc)
    {
        pendingHours.store(tc.hours, std::memory_order_relaxed);
        pendingMinutes.store(tc.minutes, std::memory_order_relaxed);
        pendingSeconds.store(tc.seconds, std::memory_order_relaxed);
        pendingFrames.store(tc.frames, std::memory_order_relaxed);
    }

    void setFrameRate(FrameRate fps)  { pendingFps.store(fps, std::memory_order_relaxed); }
    void setPaused(bool p)            { paused.store(p, std::memory_order_relaxed); }
    bool isPaused() const             { return paused.load(std::memory_order_relaxed); }

    void setOutputGain(float gain)    { outputGain.store(juce::jlimit(0.0f, 4.0f, gain), std::memory_order_relaxed); }
    float getOutputGain() const       { return outputGain.load(std::memory_order_relaxed); }

    float getPeakLevel() const        { return peakLevel.load(std::memory_order_relaxed); }

private:
    juce::AudioDeviceManager deviceManager;
    juce::String currentDeviceName;
    bool isRunningFlag = false;
    int selectedChannel = 0;
    int numChannelsAvailable = 0;
    double currentSampleRate = 48000.0;
    int currentBufferSize = 512;

    std::atomic<int> pendingHours   { 0 };
    std::atomic<int> pendingMinutes { 0 };
    std::atomic<int> pendingSeconds { 0 };
    std::atomic<int> pendingFrames  { 0 };
    std::atomic<FrameRate> pendingFps { FrameRate::FPS_25 };
    std::atomic<bool> paused { false };
    std::atomic<float> outputGain { 1.0f };
    std::atomic<float> peakLevel { 0.0f };

    static constexpr int LTC_FRAME_BITS = 80;
    uint8_t frameBits[LTC_FRAME_BITS] = {};
    int currentBitIndex = 0;
    int halfCellIndex = 0;
    double samplePositionInHalfBit = 0.0;
    double samplesPerHalfBit = 0.0;
    float currentLevel = 1.0f;
    bool needNewFrame = true;
    static constexpr float baseAmplitude = 0.8f;

    void resetEncoder()
    {
        currentBitIndex = 0;
        halfCellIndex = 0;
        samplePositionInHalfBit = 0.0;
        currentLevel = 1.0f;
        needNewFrame = true;
        updateSamplesPerBit();
    }

    void updateSamplesPerBit()
    {
        double fps = frameRateToDouble(pendingFps.load(std::memory_order_relaxed));
        samplesPerHalfBit = currentSampleRate / (fps * LTC_FRAME_BITS * 2.0);
    }

    void packFrame()
    {
        int frames  = pendingFrames.load(std::memory_order_relaxed);
        int seconds = pendingSeconds.load(std::memory_order_relaxed);
        int minutes = pendingMinutes.load(std::memory_order_relaxed);
        int hours   = pendingHours.load(std::memory_order_relaxed);
        FrameRate fps = pendingFps.load(std::memory_order_relaxed);

        int frameUnits = frames % 10,  frameTens = frames / 10;
        int secUnits   = seconds % 10, secTens   = seconds / 10;
        int minUnits   = minutes % 10, minTens   = minutes / 10;
        int hourUnits  = hours % 10,   hourTens  = hours / 10;
        bool dropFrame = (fps == FrameRate::FPS_2997);

        std::memset(frameBits, 0, LTC_FRAME_BITS);

        frameBits[0] = (frameUnits >> 0) & 1;
        frameBits[1] = (frameUnits >> 1) & 1;
        frameBits[2] = (frameUnits >> 2) & 1;
        frameBits[3] = (frameUnits >> 3) & 1;

        frameBits[8] = (frameTens >> 0) & 1;
        frameBits[9] = (frameTens >> 1) & 1;

        frameBits[10] = dropFrame ? 1 : 0;

        frameBits[16] = (secUnits >> 0) & 1;
        frameBits[17] = (secUnits >> 1) & 1;
        frameBits[18] = (secUnits >> 2) & 1;
        frameBits[19] = (secUnits >> 3) & 1;

        frameBits[24] = (secTens >> 0) & 1;
        frameBits[25] = (secTens >> 1) & 1;
        frameBits[26] = (secTens >> 2) & 1;

        frameBits[32] = (minUnits >> 0) & 1;
        frameBits[33] = (minUnits >> 1) & 1;
        frameBits[34] = (minUnits >> 2) & 1;
        frameBits[35] = (minUnits >> 3) & 1;

        frameBits[40] = (minTens >> 0) & 1;
        frameBits[41] = (minTens >> 1) & 1;
        frameBits[42] = (minTens >> 2) & 1;

        frameBits[48] = (hourUnits >> 0) & 1;
        frameBits[49] = (hourUnits >> 1) & 1;
        frameBits[50] = (hourUnits >> 2) & 1;
        frameBits[51] = (hourUnits >> 3) & 1;

        frameBits[56] = (hourTens >> 0) & 1;
        frameBits[57] = (hourTens >> 1) & 1;

        frameBits[64] = 0; frameBits[65] = 0; frameBits[66] = 1; frameBits[67] = 1;
        frameBits[68] = 1; frameBits[69] = 1; frameBits[70] = 1; frameBits[71] = 1;
        frameBits[72] = 1; frameBits[73] = 1; frameBits[74] = 1; frameBits[75] = 1;
        frameBits[76] = 1; frameBits[77] = 1; frameBits[78] = 0; frameBits[79] = 1;

        int onesCount = 0;
        for (int i = 0; i < 59; i++)
            onesCount += frameBits[i];
        frameBits[59] = (onesCount & 1) ? 1 : 0;
    }

    //==============================================================================
    void audioDeviceIOCallbackWithContext(const float* const*, int,
                                          float* const* outputChannelData,
                                          int numOutputChannels, int numSamples,
                                          const juce::AudioIODeviceCallbackContext&) override
    {
        for (int ch = 0; ch < numOutputChannels; ch++)
            if (outputChannelData[ch])
                std::memset(outputChannelData[ch], 0, sizeof(float) * (size_t)numSamples);

        if (paused.load(std::memory_order_relaxed))
            return;

        bool stereoMode = (selectedChannel == -1);
        int primaryCh = stereoMode ? 0 : selectedChannel;
        if (primaryCh >= numOutputChannels || !outputChannelData[primaryCh])
            return;

        float* output  = outputChannelData[primaryCh];
        float* output2 = (stereoMode && numOutputChannels >= 2 && outputChannelData[1])
                            ? outputChannelData[1] : nullptr;
        const float amplitude = baseAmplitude * outputGain.load(std::memory_order_relaxed);

        float peak = 0.0f;
        for (int i = 0; i < numSamples; ++i)
        {
            if (needNewFrame)
            {
                updateSamplesPerBit();
                packFrame();
                currentBitIndex = 0;
                halfCellIndex = 0;
                samplePositionInHalfBit = 0.0;
                needNewFrame = false;
                currentLevel = -currentLevel;
            }

            float sample = currentLevel * amplitude;
            output[i] = sample;
            if (output2) output2[i] = sample;
            float a = std::abs(sample);
            if (a > peak) peak = a;
            samplePositionInHalfBit += 1.0;

            if (samplePositionInHalfBit >= samplesPerHalfBit)
            {
                samplePositionInHalfBit -= samplesPerHalfBit;

                if (halfCellIndex == 0)
                {
                    halfCellIndex = 1;
                    if (frameBits[currentBitIndex] == 1)
                        currentLevel = -currentLevel;
                }
                else
                {
                    halfCellIndex = 0;
                    currentBitIndex++;

                    if (currentBitIndex >= LTC_FRAME_BITS)
                        needNewFrame = true;
                    else
                        currentLevel = -currentLevel;
                }
            }
        }
        peakLevel.store(peak, std::memory_order_relaxed);
    }

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override
    {
        if (device)
        {
            currentSampleRate = device->getCurrentSampleRate();
            currentBufferSize = device->getCurrentBufferSizeSamples();
            numChannelsAvailable = device->getActiveOutputChannels().countNumberOfSetBits();
        }
        resetEncoder();
    }

    void audioDeviceStopped() override {}
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LtcOutput)
};
