// Super Timecode Converter
// Copyright (c) 2026 Fiverecords — MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

#pragma once
#include <JuceHeader.h>
#include "LtcInput.h"
#include <atomic>
#include <cstring>

class AudioThru : private juce::AudioIODeviceCallback
{
public:
    AudioThru() = default;
    ~AudioThru() { stop(); }

    //==============================================================================
    // channel: 0+ = specific channel, -1 = Ch 1 + Ch 2
    // sampleRate: preferred sample rate (0 = device default)
    // bufferSize: preferred buffer size (0 = device default)
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

        deviceManager.closeAudioDevice();

        // Register all device types without opening anything
        deviceManager.initialise(0, 128, nullptr, false);

        // Switch to requested type WITHOUT auto-opening (false)
        if (typeName.isNotEmpty())
            deviceManager.setCurrentAudioDeviceType(typeName, false);

        // Scan so the device name is recognised
        if (auto* type = deviceManager.getCurrentDeviceTypeObject())
            type->scanForDevices();

        // Open the specific device -- single open call
        auto setup = deviceManager.getAudioDeviceSetup();
        setup.outputDeviceName  = devName;
        setup.inputDeviceName   = "";
        setup.useDefaultInputChannels  = false;
        setup.useDefaultOutputChannels = true;
        if (sampleRate > 0)  setup.sampleRate = sampleRate;
        if (bufferSize > 0)  setup.bufferSize = bufferSize;
        auto err = deviceManager.setAudioDeviceSetup(setup, true);
        if (err.isNotEmpty()) return false;

        auto* device = deviceManager.getCurrentAudioDevice();
        if (!device) return false;

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
        deviceManager.addAudioCallback(this);
        isRunningFlag.store(true, std::memory_order_relaxed);
        return true;
    }

    void stop()
    {
        if (isRunningFlag.load(std::memory_order_relaxed))
        {
            // Null the source pointer BEFORE removing the callback so that
            // any in-flight callback will see nullptr and exit early.  The
            // release ordering pairs with the acquire load in the callback.
            sourceInput.store(nullptr, std::memory_order_release);
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

    void setOutputGain(float gain) { outputGain.store(juce::jlimit(0.0f, 2.0f, gain), std::memory_order_relaxed); }
    float getOutputGain() const { return outputGain.load(std::memory_order_relaxed); }

    float getPeakLevel() const { return peakLevel.load(std::memory_order_relaxed); }

private:
    juce::AudioDeviceManager deviceManager;
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
    // sourceInput points to a LtcInput owned by MainComponent.  Lifetime is
    // guaranteed because ~MainComponent() calls stopThruOutput() (which nulls
    // this pointer via stop()) BEFORE destroying LtcInput.
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
