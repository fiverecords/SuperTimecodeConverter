// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

#pragma once
#include <JuceHeader.h>
#include "AudioDeviceHub.h"
#include "LtcInput.h"
#include <atomic>
#include <cstring>

//==============================================================================
// AudioThru -- plays the LTC input's passthrough channel (LtcInput's ring
// buffer) on an output device, through AudioDeviceHub.
//
// On the LTC input's own device both sides run on one clock.  On another
// device nothing matches the two clocks (AUDIT LTC-16, deferred): a rate
// mismatch at start is only flagged on the status line (RATE MISMATCH, in
// TimecodeEngine::startThruOutput), and there is no resampling, no drift
// compensation, no prefill or target latency.  The ring
// (LtcInput::RING_SIZE samples) absorbs the difference until it
// runs dry -- silence for the missing samples, counted as underruns -- or
// full -- the newest input dropped, counted as overruns, after the latency
// has crept up to the ring's length -- so drifting clocks give periodic
// clicks, and two different sample rates play at the wrong pitch.
//==============================================================================
class AudioThru : private juce::AudioIODeviceCallback
{
public:
    AudioThru() = default;
    ~AudioThru() { stop(); }

    //==============================================================================
    // channel: 0+ = specific channel, -1 = Ch 1 + Ch 2
    // sampleRate: preferred sample rate (0 = device default)
    // bufferSize: preferred buffer size (0 = device default)
    //   Both apply only if this opens the device; on a device already open
    //   the running rate and buffer are kept (AudioDeviceHub, DESIGN D23).
    //==============================================================================
    bool start(const juce::String& typeName, const juce::String& devName,
               int channel, LtcInput* source,
               double sampleRate = 0, int bufferSize = 0)
    {
        stop();
        if (!source) return false;

        selectedChannel.store(channel, std::memory_order_relaxed);
        currentDeviceName = devName;
        currentTypeName = typeName;
        sourceInput.store(source, std::memory_order_relaxed);

        juce::String err;
        auto* device = AudioDeviceHub::get().acquire(this, typeName, devName, false, sampleRate, bufferSize, err);
        if (device == nullptr) return false;

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

        peakLevel.store(0.0f, std::memory_order_relaxed);
        isRunningFlag.store(true, std::memory_order_relaxed);
        return true;
    }

    void stop()
    {
        if (isRunningFlag.load(std::memory_order_relaxed))
        {
            // Null the source pointer first, so a callback that starts from
            // here on returns early (the release pairs with the acquire load
            // in the callback).  One already past that load finishes with the
            // pointer; release() below returns only after it has, because the
            // hub holds its lock for the whole fan-out -- which is what lets
            // the caller stop and destroy the LtcInput next.
            sourceInput.store(nullptr, std::memory_order_release);
            AudioDeviceHub::get().release(this);
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

    void setOutputGain(float gain) { outputGain.store(juce::jlimit(0.0f, 2.0f, gain), std::memory_order_relaxed); }
    float getOutputGain() const { return outputGain.load(std::memory_order_relaxed); }

    float getPeakLevel() const { return peakLevel.load(std::memory_order_relaxed); }

private:
    juce::String currentDeviceName;
    juce::String currentTypeName;
    std::atomic<bool> isRunningFlag { false };
    // selectedChannel is written in start() (UI thread) and read in
    // audioDeviceIOCallbackWithContext() (audio thread).  Atomic makes
    // the cross-thread contract explicit.
    std::atomic<int> selectedChannel { 0 };
    int numChannelsAvailable = 0;
    double currentSampleRate = 48000.0;
    int currentBufferSize = 512;
    // sourceInput points to the LtcInput of the TimecodeEngine that owns this
    // AudioThru.  stop() returns only when no callback can use it any more
    // (see stop()), and the engine stops the thru before the LtcInput:
    // stopLtcInput() calls stopThruOutput() first, ~TimecodeEngine and
    // ~MainComponent stop the thru before the input, and reindex() stops and
    // destroys the thru when the engine stops being the primary one.
    std::atomic<LtcInput*> sourceInput { nullptr };
    std::atomic<float> outputGain { 1.0f };
    std::atomic<float> peakLevel { 0.0f };

    void audioDeviceIOCallbackWithContext(const float* const*, int,
                                          float* const* outputChannelData,
                                          int numOutputCh, int numSamples,
                                          const juce::AudioIODeviceCallbackContext&) override
    {
        for (int ch = 0; ch < numOutputCh; ch++)
            if (outputChannelData[ch])
                std::memset(outputChannelData[ch], 0, sizeof(float) * (size_t)numSamples);

        auto* src = sourceInput.load(std::memory_order_acquire);
        if (!src) return;

        int selCh = selectedChannel.load(std::memory_order_relaxed);
        bool stereoMode = (selCh == -1);
        int primaryCh = stereoMode ? 0 : selCh;
        if (primaryCh >= numOutputCh || !outputChannelData[primaryCh])
            return;

        float* output = outputChannelData[primaryCh];
        src->readPassthruSamples(output, numSamples);

        const float gain = outputGain.load(std::memory_order_relaxed);
        float peak = 0.0f;
        for (int i = 0; i < numSamples; i++)
        {
            if (gain != 1.0f) output[i] *= gain;
            float a = std::abs(output[i]);
            if (a > peak) peak = a;
        }
        peakLevel.store(peak, std::memory_order_relaxed);

        if (stereoMode && numOutputCh >= 2 && outputChannelData[1])
            std::memcpy(outputChannelData[1], output, sizeof(float) * (size_t)numSamples);
    }

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override
    {
        if (device)
        {
            numChannelsAvailable = device->getActiveOutputChannels().countNumberOfSetBits();
            currentSampleRate = device->getCurrentSampleRate();
            currentBufferSize = device->getCurrentBufferSizeSamples();
        }
    }

    void audioDeviceStopped() override
    {
        peakLevel.store(0.0f, std::memory_order_relaxed);
    }
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioThru)
};
