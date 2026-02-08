#pragma once
#include <JuceHeader.h>
#include "TimecodeCore.h"
#include <atomic>
#include <cmath>

class LtcInput : private juce::AudioIODeviceCallback
{
public:
    LtcInput() = default;
    ~LtcInput() { stop(); }

    //==============================================================================
    // Start with explicit device type and name
    //   typeName:    audio device type (e.g. "Windows Audio", "ASIO")
    //   deviceName:  raw device name
    //   ltcChannel:  channel index carrying LTC signal
    //   thruChannel: channel index to capture for passthrough (-1 = disabled)
    //   sampleRate:  preferred sample rate (0 = device default)
    //   bufferSize:  preferred buffer size (0 = device default)
    //==============================================================================
    bool start(const juce::String& typeName, const juce::String& devName,
               int ltcChannel, int thruChannel = -1,
               double sampleRate = 0, int bufferSize = 0)
    {
        stop();

        selectedChannel = ltcChannel;
        passthruChannel = thruChannel;
        currentDeviceName = devName;

        deviceManager.closeAudioDevice();

        // Register all device types without opening anything
        deviceManager.initialise(128, 0, nullptr, false);

        // Switch to requested type WITHOUT auto-opening (false)
        if (typeName.isNotEmpty())
            deviceManager.setCurrentAudioDeviceType(typeName, false);

        // Scan so the device name is recognised
        if (auto* type = deviceManager.getCurrentDeviceTypeObject())
            type->scanForDevices();

        // Open the specific device – single open call
        auto setup = deviceManager.getAudioDeviceSetup();
        setup.inputDeviceName   = devName;
        setup.outputDeviceName  = "";
        setup.useDefaultInputChannels  = true;
        setup.useDefaultOutputChannels = false;
        if (sampleRate > 0)  setup.sampleRate = sampleRate;
        if (bufferSize > 0)  setup.bufferSize = bufferSize;
        auto err = deviceManager.setAudioDeviceSetup(setup, true);
        if (err.isNotEmpty())
            return false;

        auto* device = deviceManager.getCurrentAudioDevice();
        if (!device)
            return false;

        numChannelsAvailable = device->getActiveInputChannels().countNumberOfSetBits();
        if (selectedChannel >= numChannelsAvailable)
            selectedChannel = 0;
        if (passthruChannel >= numChannelsAvailable)
            passthruChannel = -1;

        currentSampleRate = device->getCurrentSampleRate();
        currentBufferSize = device->getCurrentBufferSizeSamples();

        resetDecoder();
        resetPassthruBuffer();
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

    //==============================================================================
    bool getIsRunning() const { return isRunningFlag; }
    juce::String getCurrentDeviceName() const { return currentDeviceName; }
    int getSelectedChannel() const { return selectedChannel; }
    int getPassthruChannel() const { return passthruChannel; }
    int getChannelCount() const { return numChannelsAvailable; }
    double getActualSampleRate() const { return currentSampleRate; }
    int getActualBufferSize() const { return currentBufferSize; }

    Timecode getCurrentTimecode() const
    {
        Timecode tc;
        tc.hours   = decodedHours.load(std::memory_order_relaxed);
        tc.minutes = decodedMinutes.load(std::memory_order_relaxed);
        tc.seconds = decodedSeconds.load(std::memory_order_relaxed);
        tc.frames  = decodedFrames.load(std::memory_order_relaxed);
        return tc;
    }

    FrameRate getDetectedFrameRate() const
    {
        return detectedFps.load(std::memory_order_relaxed);
    }

    bool isReceiving() const
    {
        auto now = juce::Time::getMillisecondCounterHiRes();
        return (now - lastFrameTime.load(std::memory_order_relaxed)) < 200.0;
    }

    //==============================================================================
    // Independent gain controls
    //==============================================================================
    void setInputGain(float gain)    { inputGain.store(juce::jlimit(0.0f, 4.0f, gain), std::memory_order_relaxed); }
    float getInputGain() const       { return inputGain.load(std::memory_order_relaxed); }

    void setPassthruGain(float gain) { passthruGain.store(juce::jlimit(0.0f, 4.0f, gain), std::memory_order_relaxed); }
    float getPassthruGain() const    { return passthruGain.load(std::memory_order_relaxed); }

    //==============================================================================
    // Passthrough ring buffer
    //==============================================================================
    int readPassthruSamples(float* dest, int numSamples)
    {
        int wp = passthruWritePos.load(std::memory_order_acquire);
        int rp = passthruReadPos.load(std::memory_order_relaxed);
        int available = wp - rp;
        if (available < 0) available = 0;

        int toRead = juce::jmin(numSamples, available);
        for (int i = 0; i < toRead; i++)
            dest[i] = passthruBuffer[(rp + i) & RING_MASK];

        passthruReadPos.store(rp + toRead, std::memory_order_release);
        return toRead;
    }

    bool hasPassthruChannel() const { return passthruChannel >= 0; }

private:
    juce::AudioDeviceManager deviceManager;
    juce::String currentDeviceName;
    bool isRunningFlag = false;
    int selectedChannel = 0;
    int passthruChannel = -1;
    int numChannelsAvailable = 0;
    double currentSampleRate = 48000.0;
    int currentBufferSize = 512;

    std::atomic<float> inputGain    { 1.0f };
    std::atomic<float> passthruGain { 1.0f };

    static constexpr int RING_SIZE = 32768;
    static constexpr int RING_MASK = RING_SIZE - 1;
    float passthruBuffer[RING_SIZE] = {};
    std::atomic<int> passthruWritePos { 0 };
    std::atomic<int> passthruReadPos  { 0 };

    void resetPassthruBuffer()
    {
        passthruWritePos.store(0, std::memory_order_relaxed);
        passthruReadPos.store(0, std::memory_order_relaxed);
        std::memset(passthruBuffer, 0, sizeof(passthruBuffer));
    }

    std::atomic<int> decodedHours   { 0 };
    std::atomic<int> decodedMinutes { 0 };
    std::atomic<int> decodedSeconds { 0 };
    std::atomic<int> decodedFrames  { 0 };
    std::atomic<FrameRate> detectedFps { FrameRate::FPS_25 };
    std::atomic<double> lastFrameTime  { 0.0 };

    bool signalHigh = false;
    static constexpr float kHysteresisThreshold = 0.05f;
    int samplesSinceEdge = 0;
    double bitPeriodEstimate = 0.0;
    bool halfBitPending = false;
    bool firstEdgeAfterReset = true;
    uint64_t shiftRegLow  = 0;
    uint16_t shiftRegHigh = 0;
    static constexpr uint16_t LTC_SYNC_WORD = 0xBFFC;
    double samplesSinceLastSync = 0.0;
    int consecutiveGoodFrames = 0;

    void resetDecoder()
    {
        signalHigh = false;
        samplesSinceEdge = 0;
        halfBitPending = false;
        firstEdgeAfterReset = true;
        shiftRegLow = 0;
        shiftRegHigh = 0;
        samplesSinceLastSync = 0.0;
        consecutiveGoodFrames = 0;
        bitPeriodEstimate = currentSampleRate / 2000.0;
    }

    void pushBit(int bit)
    {
        shiftRegLow  = (shiftRegLow >> 1) | (static_cast<uint64_t>(shiftRegHigh & 1) << 63);
        shiftRegHigh = static_cast<uint16_t>((shiftRegHigh >> 1) | ((bit & 1) << 15));
        if (shiftRegHigh == LTC_SYNC_WORD)
            onSyncWordDetected();
    }

    void onSyncWordDetected()
    {
        uint64_t d = shiftRegLow;

        int frameUnits = static_cast<int>( d        & 0x0F);
        int frameTens  = static_cast<int>((d >> 8)  & 0x03);
        int secUnits   = static_cast<int>((d >> 16) & 0x0F);
        int secTens    = static_cast<int>((d >> 24) & 0x07);
        int minUnits   = static_cast<int>((d >> 32) & 0x0F);
        int minTens    = static_cast<int>((d >> 40) & 0x07);
        int hourUnits  = static_cast<int>((d >> 48) & 0x0F);
        int hourTens   = static_cast<int>((d >> 56) & 0x03);
        bool dropFrame = ((d >> 10) & 0x01) != 0;

        int frames  = frameTens * 10 + frameUnits;
        int seconds = secTens   * 10 + secUnits;
        int minutes = minTens   * 10 + minUnits;
        int hours   = hourTens  * 10 + hourUnits;

        if (hours > 23 || minutes > 59 || seconds > 59 || frames > 30)
        {
            consecutiveGoodFrames = 0;
            samplesSinceLastSync = 0.0;
            return;
        }

        if (samplesSinceLastSync > 0.0)
        {
            double framePeriodSec = samplesSinceLastSync / currentSampleRate;
            double measuredFps = 1.0 / framePeriodSec;

            FrameRate detected = FrameRate::FPS_25;
            if (measuredFps < 24.5)       detected = FrameRate::FPS_24;
            else if (measuredFps < 27.0)  detected = FrameRate::FPS_25;
            else if (dropFrame)           detected = FrameRate::FPS_2997;
            else                          detected = FrameRate::FPS_30;

            consecutiveGoodFrames++;
            if (consecutiveGoodFrames >= 3)
                detectedFps.store(detected, std::memory_order_relaxed);
        }
        else
        {
            consecutiveGoodFrames = 1;
        }

        samplesSinceLastSync = 0.0;

        decodedFrames.store(frames,   std::memory_order_relaxed);
        decodedSeconds.store(seconds, std::memory_order_relaxed);
        decodedMinutes.store(minutes, std::memory_order_relaxed);
        decodedHours.store(hours,     std::memory_order_relaxed);
        lastFrameTime.store(juce::Time::getMillisecondCounterHiRes(), std::memory_order_relaxed);
    }

    void onEdgeDetected(int intervalSamples)
    {
        if (firstEdgeAfterReset)
        {
            firstEdgeAfterReset = false;
            return;
        }

        double interval = static_cast<double>(intervalSamples);
        double halfBit  = bitPeriodEstimate * 0.5;
        double threshold = bitPeriodEstimate * 0.75;

        if (interval < halfBit * 0.4 || interval > bitPeriodEstimate * 1.8)
        {
            halfBitPending = false;
            return;
        }

        if (interval < threshold)
        {
            if (halfBitPending)
            {
                pushBit(1);
                halfBitPending = false;
                double measured = interval * 2.0;
                bitPeriodEstimate = bitPeriodEstimate * 0.95 + measured * 0.05;
            }
            else
            {
                halfBitPending = true;
            }
        }
        else
        {
            if (halfBitPending)
                halfBitPending = false;
            pushBit(0);
            bitPeriodEstimate = bitPeriodEstimate * 0.95 + interval * 0.05;
        }
    }

    //==============================================================================
    void audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                          int numInputCh, float* const*, int,
                                          int numSamples,
                                          const juce::AudioIODeviceCallbackContext&) override
    {
        // --- Passthrough: capture channel into ring buffer ---
        if (passthruChannel >= 0 && passthruChannel < numInputCh
            && inputChannelData[passthruChannel])
        {
            const float* thruData = inputChannelData[passthruChannel];
            const float pGain = passthruGain.load(std::memory_order_relaxed);
            int wp = passthruWritePos.load(std::memory_order_relaxed);

            for (int i = 0; i < numSamples; i++)
                passthruBuffer[(wp + i) & RING_MASK] = thruData[i] * pGain;

            passthruWritePos.store(wp + numSamples, std::memory_order_release);
        }

        // --- LTC decode on the selected channel ---
        if (numInputCh <= 0 || selectedChannel >= numInputCh)
            return;

        const float* data = inputChannelData[selectedChannel];
        if (!data)
            return;

        const float gain = inputGain.load(std::memory_order_relaxed);

        for (int i = 0; i < numSamples; ++i)
        {
            float sample = data[i] * gain;
            samplesSinceEdge++;
            samplesSinceLastSync += 1.0;

            bool edgeDetected = false;

            if (signalHigh)
            {
                if (sample < -kHysteresisThreshold)
                {
                    signalHigh = false;
                    edgeDetected = true;
                }
            }
            else
            {
                if (sample > kHysteresisThreshold)
                {
                    signalHigh = true;
                    edgeDetected = true;
                }
            }

            if (edgeDetected)
            {
                onEdgeDetected(samplesSinceEdge);
                samplesSinceEdge = 0;
            }
        }
    }

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override
    {
        if (device)
        {
            currentSampleRate = device->getCurrentSampleRate();
            currentBufferSize = device->getCurrentBufferSizeSamples();
            numChannelsAvailable = device->getActiveInputChannels().countNumberOfSetBits();
        }
        resetDecoder();
        resetPassthruBuffer();
    }

    void audioDeviceStopped() override {}

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LtcInput)
};
