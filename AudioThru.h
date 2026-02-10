#pragma once
#include <JuceHeader.h>
#include "LtcInput.h"
#include <atomic>

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

        selectedChannel = channel;
        currentDeviceName = devName;
        sourceInput = source;

        deviceManager.closeAudioDevice();

        // Register all device types without opening anything
        deviceManager.initialise(0, 128, nullptr, false);

        // Switch to requested type WITHOUT auto-opening (false)
        if (typeName.isNotEmpty())
            deviceManager.setCurrentAudioDeviceType(typeName, false);

        // Scan so the device name is recognised
        if (auto* type = deviceManager.getCurrentDeviceTypeObject())
            type->scanForDevices();

        // Open the specific device â€“ single open call
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
        if (selectedChannel >= 0 && selectedChannel >= numChannelsAvailable)
            selectedChannel = 0;
        if (selectedChannel == -1 && numChannelsAvailable < 2)
            selectedChannel = 0;

        currentSampleRate = device->getCurrentSampleRate();
        currentBufferSize = device->getCurrentBufferSizeSamples();

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
            sourceInput = nullptr;
        }
    }

    bool getIsRunning() const { return isRunningFlag; }
    juce::String getCurrentDeviceName() const { return currentDeviceName; }
    int getSelectedChannel() const { return selectedChannel; }
    int getChannelCount() const { return numChannelsAvailable; }
    bool isStereoMode() const { return selectedChannel == -1; }
    double getActualSampleRate() const { return currentSampleRate; }
    int getActualBufferSize() const { return currentBufferSize; }

    void setOutputGain(float gain) { outputGain.store(juce::jlimit(0.0f, 4.0f, gain), std::memory_order_relaxed); }
    float getOutputGain() const { return outputGain.load(std::memory_order_relaxed); }

    float getPeakLevel() const { return peakLevel.load(std::memory_order_relaxed); }

private:
    juce::AudioDeviceManager deviceManager;
    juce::String currentDeviceName;
    bool isRunningFlag = false;
    int selectedChannel = 0;
    int numChannelsAvailable = 0;
    double currentSampleRate = 48000.0;
    int currentBufferSize = 512;
    LtcInput* sourceInput = nullptr;
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

        if (!sourceInput) return;

        bool stereoMode = (selectedChannel == -1);
        int primaryCh = stereoMode ? 0 : selectedChannel;
        if (primaryCh >= numOutputCh || !outputChannelData[primaryCh])
            return;

        float* output = outputChannelData[primaryCh];
        sourceInput->readPassthruSamples(output, numSamples);

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

    void audioDeviceStopped() override {}
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioThru)
};
