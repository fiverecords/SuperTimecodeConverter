// Super Timecode Converter
// Copyright (c) 2026 Fiverecords — MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

#pragma once
#include <JuceHeader.h>
#include "TimecodeCore.h"
#include <atomic>
#include <cmath>
#include <cstring>

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

        selectedChannel.store(ltcChannel, std::memory_order_relaxed);
        passthruChannel.store(thruChannel, std::memory_order_relaxed);
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

        // Open the specific device -- single open call
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
        int selCh = selectedChannel.load(std::memory_order_relaxed);
        int thruCh = passthruChannel.load(std::memory_order_relaxed);
        if (selCh >= numChannelsAvailable)
            selCh = 0;
        if (thruCh >= numChannelsAvailable)
            thruCh = -1;
        // Prevent passthrough from using the same channel as LTC decode
        if (thruCh >= 0 && thruCh == selCh)
            thruCh = -1;
        selectedChannel.store(selCh, std::memory_order_relaxed);
        passthruChannel.store(thruCh, std::memory_order_relaxed);

        currentSampleRate = device->getCurrentSampleRate();
        currentBufferSize = device->getCurrentBufferSizeSamples();

        // resetDecoder() and resetPassthruBuffer() are called by
        // audioDeviceAboutToStart() when addAudioCallback triggers the device;
        // only peak levels need explicit reset here since they're not part of
        // the device-start callback.
        ltcPeakLevel.store(0.0f, std::memory_order_relaxed);
        thruPeakLevel.store(0.0f, std::memory_order_relaxed);
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

    //==============================================================================
    bool getIsRunning() const { return isRunningFlag.load(std::memory_order_relaxed); }
    juce::String getCurrentDeviceName() const { return currentDeviceName; }
    int getSelectedChannel() const { return selectedChannel.load(std::memory_order_relaxed); }
    int getPassthruChannel() const { return passthruChannel.load(std::memory_order_relaxed); }
    int getChannelCount() const { return numChannelsAvailable; }
    double getActualSampleRate() const { return currentSampleRate; }
    int getActualBufferSize() const { return currentBufferSize; }

    Timecode getCurrentTimecode() const
    {
        return unpackTimecode(packedTimecode.load(std::memory_order_relaxed));
    }

    FrameRate getDetectedFrameRate() const
    {
        return detectedFps.load(std::memory_order_relaxed);
    }

    bool isReceiving() const
    {
        auto now = juce::Time::getMillisecondCounterHiRes();
        return (now - lastFrameTime.load(std::memory_order_relaxed)) < kSourceTimeoutMs;
    }

    //==============================================================================
    // Independent gain controls
    //==============================================================================
    void setInputGain(float gain)    { inputGain.store(juce::jlimit(0.0f, 2.0f, gain), std::memory_order_relaxed); }
    float getInputGain() const       { return inputGain.load(std::memory_order_relaxed); }

    void setPassthruGain(float gain) { passthruGain.store(juce::jlimit(0.0f, 2.0f, gain), std::memory_order_relaxed); }
    float getPassthruGain() const    { return passthruGain.load(std::memory_order_relaxed); }

    //==============================================================================
    // Peak levels for metering (0.0 - 1.0+)
    //==============================================================================
    float getLtcPeakLevel() const    { return ltcPeakLevel.load(std::memory_order_relaxed); }
    float getThruPeakLevel() const   { return thruPeakLevel.load(std::memory_order_relaxed); }
    void resetPeakLevels()           { ltcPeakLevel.store(0.0f, std::memory_order_relaxed); thruPeakLevel.store(0.0f, std::memory_order_relaxed); }

    //==============================================================================
    // Passthrough ring buffer
    //==============================================================================
    int readPassthruSamples(float* dest, int numSamples)
    {
        if (!passthruBuffer)
        {
            std::memset(dest, 0, sizeof(float) * (size_t)numSamples);
            return 0;
        }

        uint32_t wp = passthruWritePos.load(std::memory_order_acquire);
        uint32_t rp = passthruReadPos.load(std::memory_order_relaxed);
        uint32_t available = wp - rp;  // works correctly with unsigned wrap-around

        int toRead = (int)juce::jmin((uint32_t)numSamples, available);

        // Track underruns: if we can't supply all requested samples, it's an underrun
        if (toRead < numSamples)
            passthruUnderruns.fetch_add(1, std::memory_order_relaxed);

        for (int i = 0; i < toRead; i++)
            dest[i] = passthruBuffer[(rp + (uint32_t)i) & RING_MASK];

        // Zero-fill any samples we couldn't supply (silence instead of old data)
        for (int i = toRead; i < numSamples; i++)
            dest[i] = 0.0f;

        passthruReadPos.store(rp + (uint32_t)toRead, std::memory_order_release);
        return toRead;
    }

    bool hasPassthruChannel() const { return passthruChannel.load(std::memory_order_relaxed) >= 0; }
    uint32_t getPassthruUnderruns() const { return passthruUnderruns.load(std::memory_order_relaxed); }
    uint32_t getPassthruOverruns() const  { return passthruOverruns.load(std::memory_order_relaxed); }
    void resetPassthruCounters() { passthruUnderruns.store(0, std::memory_order_relaxed); passthruOverruns.store(0, std::memory_order_relaxed); }

    // Snap the read position to the current write position so the next reader
    // starts from fresh data instead of consuming stale buffered samples.
    // Call this just before starting AudioThru while LtcInput is already running.
    void syncPassthruReadPosition()
    {
        passthruReadPos.store(passthruWritePos.load(std::memory_order_acquire),
                              std::memory_order_release);
    }

private:
    juce::AudioDeviceManager deviceManager;
    juce::String currentDeviceName;
    std::atomic<bool> isRunningFlag { false };
    std::atomic<int> selectedChannel { 0 };
    std::atomic<int> passthruChannel { -1 };
    int numChannelsAvailable = 0;
    double currentSampleRate = 48000.0;
    int currentBufferSize = 512;

    std::atomic<float> inputGain    { 1.0f };
    std::atomic<float> passthruGain { 1.0f };
    std::atomic<float> ltcPeakLevel  { 0.0f };
    std::atomic<float> thruPeakLevel { 0.0f };

    //==============================================================================
    // Passthrough ring buffer (SPSC: single producer = audio callback,
    // single consumer = AudioThru callback).  Uses unsigned wrap-around
    // arithmetic so writePos/readPos never need resetting during operation.
    // Heap-allocated to keep class size reasonable (~128KB buffer).
    //==============================================================================
    static constexpr int RING_SIZE = 32768;
    static constexpr uint32_t RING_MASK = RING_SIZE - 1;
    std::unique_ptr<float[]> passthruBuffer;
    std::atomic<uint32_t> passthruWritePos { 0 };
    std::atomic<uint32_t> passthruReadPos  { 0 };
    std::atomic<uint32_t> passthruUnderruns { 0 };
    std::atomic<uint32_t> passthruOverruns  { 0 };

    // Safe to call from audioDeviceAboutToStart(): JUCE guarantees no audio
    // callbacks are active during device start, so no concurrent reader/writer.
    void resetPassthruBuffer()
    {
        passthruWritePos.store(0, std::memory_order_relaxed);
        passthruReadPos.store(0, std::memory_order_relaxed);
        if (!passthruBuffer)
            passthruBuffer = std::make_unique<float[]>(RING_SIZE);
        std::memset(passthruBuffer.get(), 0, sizeof(float) * RING_SIZE);
    }

    std::atomic<uint64_t> packedTimecode { 0 };
    std::atomic<FrameRate> detectedFps { FrameRate::FPS_25 };
    std::atomic<double> lastFrameTime  { 0.0 };

    // LTC decoder state -- audio-callback-thread-only (no synchronisation needed)
    bool signalHigh = false;
    static constexpr float kHysteresisThreshold = 0.05f;
    int64_t samplesSinceEdge = 0;  // int64_t: prevents overflow at 192kHz without signal (~3h with int)
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
        // Initial bit period estimate: use ~27fps midpoint (2160 transitions/sec)
        // to minimize convergence time across all frame rates (24-30fps)
        bitPeriodEstimate = currentSampleRate / 2160.0;
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

        if (hours > 23 || minutes > 59 || seconds > 59 || frames > 29)
        {
            consecutiveGoodFrames = 0;
            samplesSinceLastSync = 0.0;
            return;
        }

        // Only compute fps from inter-frame period if the gap is reasonable
        // (< 2 seconds).  Longer gaps mean signal was lost/corrupt and the
        // measured period would be meaningless for rate detection.
        if (samplesSinceLastSync > 0.0 && samplesSinceLastSync < currentSampleRate * 2.0)
        {
            double framePeriodSec = samplesSinceLastSync / currentSampleRate;
            double measuredFps = 1.0 / framePeriodSec;

            FrameRate detected = FrameRate::FPS_25;
            // NOTE: LTC cannot distinguish 23.976fps from 24fps — both use 80 bits
            // per frame with no drop-frame flag.  The ~0.1% rate difference is too
            // small to measure reliably from frame-to-frame period.  If 23.976 support
            // is needed, the user must manually select the frame rate.
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

        packedTimecode.store(packTimecode(hours, minutes, seconds, frames),
                             std::memory_order_relaxed);
        lastFrameTime.store(juce::Time::getMillisecondCounterHiRes(), std::memory_order_relaxed);
    }

    void onEdgeDetected(int64_t intervalSamples)
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
        int pCh = passthruChannel.load(std::memory_order_relaxed);
        if (pCh >= 0 && pCh < numInputCh
            && inputChannelData[pCh] && passthruBuffer)
        {
            const float* thruData = inputChannelData[pCh];
            const float pGain = passthruGain.load(std::memory_order_relaxed);
            uint32_t wp = passthruWritePos.load(std::memory_order_relaxed);
            uint32_t rp = passthruReadPos.load(std::memory_order_acquire);
            uint32_t used = wp - rp;                    // unsigned wrap-around gives correct count
            uint32_t freeSlots = RING_SIZE - used;      // includes the 1 reserved sentinel slot

            float thruPeak = 0.0f;

            // Reserve 1 slot to distinguish full from empty; require >= 2 free slots to write
            int toWrite = (freeSlots >= 2)
                        ? (int)juce::jmin((uint32_t)numSamples, freeSlots - 1)
                        : 0;

            // Track overruns: if we can't write all samples, input is outpacing output
            if (toWrite < numSamples)
                passthruOverruns.fetch_add(1, std::memory_order_relaxed);

            // Measure peak over ALL incoming samples (including those that won't
            // fit in the ring buffer) so the meter reflects the true input level
            // even during overruns.  Write only the samples that fit.
            for (int i = 0; i < numSamples; i++)
            {
                float s = thruData[i] * pGain;
                float a = std::abs(s);
                if (a > thruPeak) thruPeak = a;
                if (i < toWrite)
                    passthruBuffer[(wp + (uint32_t)i) & RING_MASK] = s;
            }

            passthruWritePos.store(wp + (uint32_t)toWrite, std::memory_order_release);
            thruPeakLevel.store(thruPeak, std::memory_order_relaxed);
        }

        // --- LTC decode on the selected channel ---
        int sCh = selectedChannel.load(std::memory_order_relaxed);
        if (numInputCh <= 0 || sCh >= numInputCh)
            return;

        const float* data = inputChannelData[sCh];
        if (!data)
            return;

        const float gain = inputGain.load(std::memory_order_relaxed);

        // Fixed threshold: the gain slider amplifies the signal before edge
        // detection, so raising gain genuinely helps decode weak LTC signals.
        // A fixed threshold keeps the hysteresis behaviour predictable while
        // letting the user compensate for low-level inputs.
        const float effectiveThreshold = kHysteresisThreshold;

        float ltcPeak = 0.0f;
        for (int i = 0; i < numSamples; ++i)
        {
            float sample = data[i] * gain;
            float a = std::abs(sample);
            if (a > ltcPeak) ltcPeak = a;
            samplesSinceEdge++;
            samplesSinceLastSync += 1.0;

            bool edgeDetected = false;

            if (signalHigh)
            {
                if (sample < -effectiveThreshold)
                {
                    signalHigh = false;
                    edgeDetected = true;
                }
            }
            else
            {
                if (sample > effectiveThreshold)
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
        ltcPeakLevel.store(ltcPeak, std::memory_order_relaxed);
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

    void audioDeviceStopped() override
    {
        ltcPeakLevel.store(0.0f, std::memory_order_relaxed);
        thruPeakLevel.store(0.0f, std::memory_order_relaxed);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LtcInput)
};
