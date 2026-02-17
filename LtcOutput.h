// Super Timecode Converter
// Copyright (c) 2026 Fiverecords — MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

#pragma once
#include <JuceHeader.h>
#include "TimecodeCore.h"
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>

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
        selectedChannel.store(channel, std::memory_order_relaxed);
        currentDeviceName = devName;
        currentTypeName = typeName;

        deviceManager.closeAudioDevice();

        // Register all device types without opening anything
        deviceManager.initialise(0, 128, nullptr, false);

        // Switch to requested type WITHOUT auto-opening a device (false)
        if (typeName.isNotEmpty())
            deviceManager.setCurrentAudioDeviceType(typeName, false);

        // Scan so the device name is recognised
        if (auto* type = deviceManager.getCurrentDeviceTypeObject())
            type->scanForDevices();

        // Open the specific device -- this is the ONLY open call
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
        {
            int ch = selectedChannel.load(std::memory_order_relaxed);
            if (ch >= 0 && ch >= numChannelsAvailable)
                ch = 0;
            if (ch == -1 && numChannelsAvailable < 2)
                ch = 0;
            selectedChannel.store(ch, std::memory_order_relaxed);
        }

        currentSampleRate = device->getCurrentSampleRate();
        currentBufferSize = device->getCurrentBufferSizeSamples();

        resetEncoder();
        peakLevel.store(0.0f, std::memory_order_relaxed);
        deviceManager.addAudioCallback(this);
        isRunningFlag.store(true, std::memory_order_relaxed);
        return true;
    }

    void stop()
    {
        if (isRunningFlag.load(std::memory_order_relaxed))
        {
            deviceManager.removeAudioCallback(this);
            deviceManager.closeAudioDevice();
            isRunningFlag.store(false, std::memory_order_relaxed);
        }
    }

    bool getIsRunning() const { return isRunningFlag.load(std::memory_order_relaxed); }
    juce::String getCurrentDeviceName() const { return currentDeviceName; }
    juce::String getCurrentTypeName() const { return currentTypeName; }
    int getSelectedChannel() const { return selectedChannel.load(std::memory_order_relaxed); }
    int getChannelCount() const { return numChannelsAvailable; }
    bool isStereoMode() const { return selectedChannel.load(std::memory_order_relaxed) == -1; }
    double getActualSampleRate() const { return currentSampleRate; }
    int getActualBufferSize() const { return currentBufferSize; }

    void setTimecode(const Timecode& tc)
    {
        packedPendingTc.store(packTimecode(tc.hours, tc.minutes, tc.seconds, tc.frames),
                              std::memory_order_relaxed);
    }

    void setFrameRate(FrameRate fps)  { pendingFps.store(fps, std::memory_order_relaxed); }
    void setPaused(bool p)
    {
        paused.store(p, std::memory_order_relaxed);
        if (p)
            peakLevel.store(0.0f, std::memory_order_relaxed);  // meter drops immediately
    }
    bool isPaused() const             { return paused.load(std::memory_order_relaxed); }

    void setOutputGain(float gain)    { outputGain.store(juce::jlimit(0.0f, 2.0f, gain), std::memory_order_relaxed); }
    float getOutputGain() const       { return outputGain.load(std::memory_order_relaxed); }

    float getPeakLevel() const        { return peakLevel.load(std::memory_order_relaxed); }

private:
    juce::AudioDeviceManager deviceManager;
    juce::String currentDeviceName;
    juce::String currentTypeName;
    std::atomic<bool> isRunningFlag { false };
    // selectedChannel is written in start() (UI thread) and read in
    // audioDeviceIOCallbackWithContext() (audio thread).  JUCE's
    // addAudioCallback provides a happens-before, but atomic makes
    // the cross-thread contract explicit.
    std::atomic<int> selectedChannel { 0 };
    int numChannelsAvailable = 0;
    double currentSampleRate = 48000.0;
    int currentBufferSize = 512;

    std::atomic<uint64_t> packedPendingTc { 0 };
    std::atomic<FrameRate> pendingFps { FrameRate::FPS_25 };
    std::atomic<bool> paused { false };
    std::atomic<float> outputGain { 1.0f };
    std::atomic<float> peakLevel { 0.0f };

    // LTC encoder state -- audio-callback-thread-only (no synchronisation needed)
    static constexpr int LTC_FRAME_BITS = 80;
    uint8_t frameBits[LTC_FRAME_BITS] = {};
    int currentBitIndex = 0;
    int halfCellIndex = 0;
    double samplePositionInHalfBit = 0.0;
    double samplesPerHalfBit = 0.0;
    float currentLevel = 1.0f;
    bool needNewFrame = true;
    static constexpr float baseAmplitude = 0.8f;

    // Auto-increment state: the encoder maintains its own running timecode
    // to avoid repeating frames when the UI thread lags behind the audio clock
    Timecode encoderTc;
    bool encoderSeeded = false;

    void resetEncoder()
    {
        currentBitIndex = 0;
        halfCellIndex = 0;
        samplePositionInHalfBit = 0.0;
        currentLevel = 1.0f;
        needNewFrame = true;
        encoderSeeded = false;
        encoderTc = Timecode();
        updateSamplesPerBit();
    }

    void updateSamplesPerBit()
    {
        double fps = frameRateToDouble(pendingFps.load(std::memory_order_relaxed));
        samplesPerHalfBit = currentSampleRate / (fps * LTC_FRAME_BITS * 2.0);
    }

    void packFrame()
    {
        FrameRate fps = pendingFps.load(std::memory_order_relaxed);
        Timecode pendingTc = unpackTimecode(packedPendingTc.load(std::memory_order_relaxed));

        if (!encoderSeeded)
        {
            // First frame: seed from UI
            encoderTc = pendingTc;
            encoderSeeded = true;
        }
        else
        {
            // Auto-increment from the last encoded frame
            encoderTc = incrementFrame(encoderTc, fps);

            // If the UI-provided timecode differs significantly (>1 frame),
            // re-sync to the UI value (handles seeks, source switches, jumps)
            int maxFrames = frameRateToInt(fps);
            auto toTotal = [maxFrames](const Timecode& t) -> int64_t {
                return (int64_t)t.hours * 3600 * maxFrames
                     + (int64_t)t.minutes * 60 * maxFrames
                     + (int64_t)t.seconds * maxFrames
                     + (int64_t)t.frames;
            };
            int64_t dayFrames = (int64_t)24 * 3600 * maxFrames;
            int64_t rawDiff = toTotal(pendingTc) - toTotal(encoderTc);
            // Modular distance: shortest path around the 24h wheel
            int64_t diff = ((rawDiff % dayFrames) + dayFrames) % dayFrames;
            if (diff > dayFrames / 2) diff = dayFrames - diff;
            if (diff > 1)
                encoderTc = pendingTc;
        }

        int frames  = encoderTc.frames;
        int seconds = encoderTc.seconds;
        int minutes = encoderTc.minutes;
        int hours   = encoderTc.hours;

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

        // Biphase polarity correction bit 27 (BGF0):
        // even parity over bits 0-26 so total 1s in bits 0-27 is even.
        // User bits at positions 4-7, 12-15, 20-23 are within this range
        // and ARE correctly included in the sum.  User bits group 4 (bits
        // 28-31) are outside this parity region and do not participate.
        int parityLow = 0;
        for (int i = 0; i < 27; i++)
            parityLow += frameBits[i];
        frameBits[27] = (parityLow & 1) ? 1 : 0;

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

        // Biphase polarity correction bit 59 (BGF2):
        // even parity over bits 32-58 so total 1s in bits 32-59 is even.
        // User bits at positions 36-39, 44-47, 52-55 are within this range
        // and ARE correctly included in the sum.  User bits group 8 (bits
        // 60-63) are outside this parity region and do not participate.
        int parityHigh = 0;
        for (int i = 32; i < 59; i++)
            parityHigh += frameBits[i];
        frameBits[59] = (parityHigh & 1) ? 1 : 0;
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

        int selCh = selectedChannel.load(std::memory_order_relaxed);
        bool stereoMode = (selCh == -1);
        int primaryCh = stereoMode ? 0 : selCh;
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
                // Do NOT invert currentLevel here. In biphase-mark encoding
                // the mandatory transition at the start of each bit cell is
                // already handled by the halfCellIndex == 0 branch below.
                // An extra inversion here would create a double-transition
                // (no-transition) at the frame boundary, causing biphase
                // parity errors on strict LTC decoders.
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

    void audioDeviceStopped() override
    {
        peakLevel.store(0.0f, std::memory_order_relaxed);
    }
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LtcOutput)
};
