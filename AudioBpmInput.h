// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter
//
// Audio BPM detection powered by Beat-and-Tempo-Tracking (BTT)
// Copyright (c) 2021 Michael Krzyzaniak -- MIT License
// https://github.com/michaelkrzyzaniak/Beat-and-Tempo-Tracking

#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <cstring>
#include <cmath>

// BTT is pure C with extern "C" wrappers
#include "BTT.h"

class AudioBpmInput : private juce::AudioIODeviceCallback
{
public:
    AudioBpmInput() = default;
    ~AudioBpmInput() { stop(); }

    //==============================================================================
    // Start capturing audio for BPM detection.
    //   typeName:  audio device type (e.g. "Windows Audio", "ASIO")
    //   devName:   raw device name
    //   channel:   input channel to analyse (-1 = stereo mix of Ch 1+2)
    //   sampleRate: preferred sample rate (0 = device default)
    //   bufferSize: preferred buffer size (0 = device default)
    //==============================================================================
    bool start(const juce::String& typeName, const juce::String& devName,
               int channel, double sampleRate = 0, int bufferSize = 0)
    {
        stop();

        selectedChannel.store(channel, std::memory_order_relaxed);
        currentDeviceName = devName;
        currentTypeName   = typeName;

        deviceManager.closeAudioDevice();
        deviceManager.initialise(128, 0, nullptr, false);

        if (typeName.isNotEmpty())
            deviceManager.setCurrentAudioDeviceType(typeName, false);

        if (auto* type = deviceManager.getCurrentDeviceTypeObject())
            type->scanForDevices();

        auto setup = deviceManager.getAudioDeviceSetup();
        setup.inputDeviceName   = devName;
        setup.outputDeviceName  = "";
        setup.useDefaultInputChannels  = true;
        setup.useDefaultOutputChannels = false;
        if (sampleRate > 0)  setup.sampleRate = sampleRate;
        if (bufferSize > 0)  setup.bufferSize = bufferSize;
        auto err = deviceManager.setAudioDeviceSetup(setup, true);
        if (err.isNotEmpty()) return false;

        auto* device = deviceManager.getCurrentAudioDevice();
        if (!device) return false;

        numChannelsAvailable = device->getActiveInputChannels().countNumberOfSetBits();
        {
            int ch = selectedChannel.load(std::memory_order_relaxed);
            if (ch >= 0 && ch >= numChannelsAvailable) ch = 0;
            if (ch == -1 && numChannelsAvailable < 2)  ch = 0;
            selectedChannel.store(ch, std::memory_order_relaxed);
        }

        currentSampleRate = device->getCurrentSampleRate();
        currentBufferSize = device->getCurrentBufferSizeSamples();

        // Create BTT with actual device sample rate
        bttInstance = btt_new(
            BTT_SUGGESTED_SPECTRAL_FLUX_STFT_LEN,
            BTT_SUGGESTED_SPECTRAL_FLUX_STFT_OVERLAP,
            BTT_SUGGESTED_OSS_FILTER_ORDER,
            BTT_SUGGESTED_OSS_LENGTH,
            BTT_SUGGESTED_ONSET_THRESHOLD_N,
            BTT_SUGGESTED_CBSS_LENGTH,
            currentSampleRate,
            BTT_DEFAULT_ANALYSIS_LATENCY_ONSET_ADJUSTMENT,
            BTT_DEFAULT_ANALYSIS_LATENCY_BEAT_ADJUSTMENT
        );
        if (!bttInstance) return false;

        // Optimise for DJ/electronic music: favour 120 BPM centre, 50-200 range
        btt_set_log_gaussian_tempo_weight_mean(bttInstance, 128.0);
        btt_set_min_tempo(bttInstance, 60.0);
        btt_set_max_tempo(bttInstance, 200.0);

        // Beat callback for beat-in-bar tracking
        btt_set_beat_tracking_callback(bttInstance, beatCallbackStatic, this);

        peakLevel.store(0.0f, std::memory_order_relaxed);
        detectedBpm.store(0.0, std::memory_order_relaxed);
        beatDue.store(false, std::memory_order_relaxed);
        confidence.store(0.0, std::memory_order_relaxed);
        beatCounter.store(0, std::memory_order_relaxed);
        emaState = 0.0;

        // Apply current smoothing settings to BTT
        setSmoothing(smoothing.load(std::memory_order_relaxed));

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
        if (bttInstance)
        {
            btt_destroy(bttInstance);
            bttInstance = nullptr;
        }
    }

    //==============================================================================
    bool getIsRunning() const { return isRunningFlag.load(std::memory_order_relaxed); }
    juce::String getCurrentDeviceName() const { return currentDeviceName; }
    juce::String getCurrentTypeName() const   { return currentTypeName; }
    int getSelectedChannel() const { return selectedChannel.load(std::memory_order_relaxed); }
    int getChannelCount() const    { return numChannelsAvailable; }
    double getActualSampleRate() const { return currentSampleRate; }
    int getActualBufferSize() const    { return currentBufferSize; }

    //==============================================================================
    // BPM detection results (thread-safe reads from any thread)
    //==============================================================================

    // Current estimated BPM (0.0 if not yet detected)
    double getBpm() const { return detectedBpm.load(std::memory_order_relaxed); }

    // Confidence of tempo estimate (0.0-1.0, higher = more certain)
    double getConfidence() const { return confidence.load(std::memory_order_relaxed); }

    // Returns true once when a beat is detected, then resets.
    // Call from the engine tick (60Hz) to consume beat events.
    bool consumeBeat()
    {
        return beatDue.exchange(false, std::memory_order_relaxed);
    }

    // Monotonic beat counter (increments on every detected beat)
    uint32_t getBeatCount() const { return beatCounter.load(std::memory_order_relaxed); }

    // Audio peak level for metering
    float getPeakLevel() const { return peakLevel.load(std::memory_order_relaxed); }

    // True if BPM is stable enough to use (>= kMinConfidence and > 0)
    bool hasBpm() const
    {
        return detectedBpm.load(std::memory_order_relaxed) > 0.0
            && confidence.load(std::memory_order_relaxed) >= kMinConfidence;
    }

    //==============================================================================
    // Gain control for input sensitivity
    //==============================================================================
    void setInputGain(float gain) { inputGain.store(juce::jlimit(0.0f, 4.0f, gain), std::memory_order_relaxed); }
    float getInputGain() const    { return inputGain.load(std::memory_order_relaxed); }

    // BPM smoothing: 0.0 = fast tracking, 1.0 = very stable
    void setSmoothing(float s)
    {
        smoothing.store(juce::jlimit(0.0f, 1.0f, s), std::memory_order_relaxed);
        // Also adjust BTT internal histogram decay: 0.999 (fast) .. 0.9999 (stable)
        if (bttInstance)
        {
            double decay = 0.999 + 0.0009 * (double)juce::jlimit(0.0f, 1.0f, s);
            btt_set_gaussian_tempo_histogram_decay(bttInstance, decay);
            // Widen histogram kernel for extra stability at high smoothing
            double width = 5.0 + 10.0 * (double)juce::jlimit(0.0f, 1.0f, s);  // 5..15
            btt_set_gaussian_tempo_histogram_width(bttInstance, width);
        }
    }
    float getSmoothing() const { return smoothing.load(std::memory_order_relaxed); }

    //==============================================================================
    // Reset detection state (e.g. on source switch)
    //==============================================================================
    void resetDetection()
    {
        detectedBpm.store(0.0, std::memory_order_relaxed);
        confidence.store(0.0, std::memory_order_relaxed);
        beatDue.store(false, std::memory_order_relaxed);
        beatCounter.store(0, std::memory_order_relaxed);
        emaState = 0.0;
        if (bttInstance)
            btt_clear(bttInstance);
    }

    //==============================================================================
    static constexpr double kMinConfidence = 0.15;

private:
    juce::AudioDeviceManager deviceManager;
    juce::String currentDeviceName;
    juce::String currentTypeName;
    std::atomic<bool> isRunningFlag { false };
    std::atomic<int> selectedChannel { 0 };
    int numChannelsAvailable = 0;
    double currentSampleRate = 48000.0;
    int currentBufferSize = 512;

    BTT* bttInstance = nullptr;

    // Detection results (written from audio thread, read from UI/engine)
    std::atomic<double>   detectedBpm  { 0.0 };
    std::atomic<double>   confidence   { 0.0 };
    std::atomic<bool>     beatDue      { false };
    std::atomic<uint32_t> beatCounter  { 0 };
    std::atomic<float>    peakLevel    { 0.0f };
    std::atomic<float>    inputGain    { 1.0f };
    std::atomic<float>    smoothing    { 0.5f };  // BPM smoothing: 0=fast, 1=stable
    double                emaState     = 0.0;     // EMA accumulator (audio thread only)

    // Mono mix buffer (allocated on device start)
    std::vector<float> monoBuffer;

    //==============================================================================
    static void beatCallbackStatic(void* self, unsigned long long /*sample_time*/)
    {
        auto* me = static_cast<AudioBpmInput*>(self);
        me->beatDue.store(true, std::memory_order_relaxed);
        me->beatCounter.fetch_add(1, std::memory_order_relaxed);
    }

    //==============================================================================
    void audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                          int numInputCh, float* const*, int,
                                          int numSamples,
                                          const juce::AudioIODeviceCallbackContext&) override
    {
        if (!bttInstance || numInputCh <= 0) return;

        int selCh = selectedChannel.load(std::memory_order_relaxed);
        bool stereoMix = (selCh == -1);
        const float gain = inputGain.load(std::memory_order_relaxed);

        // Prepare mono signal for BTT
        float* mono = nullptr;
        if (stereoMix && numInputCh >= 2
            && inputChannelData[0] && inputChannelData[1])
        {
            // Stereo mix to mono
            if ((int)monoBuffer.size() < numSamples)
                monoBuffer.resize((size_t)numSamples);
            mono = monoBuffer.data();
            for (int i = 0; i < numSamples; i++)
                mono[i] = (inputChannelData[0][i] + inputChannelData[1][i]) * 0.5f * gain;
        }
        else
        {
            int ch = stereoMix ? 0 : selCh;
            if (ch >= numInputCh || !inputChannelData[ch]) return;

            if ((int)monoBuffer.size() < numSamples)
                monoBuffer.resize((size_t)numSamples);
            mono = monoBuffer.data();
            for (int i = 0; i < numSamples; i++)
                mono[i] = inputChannelData[ch][i] * gain;
        }

        // Peak meter
        float peak = 0.0f;
        for (int i = 0; i < numSamples; i++)
        {
            float a = std::abs(mono[i]);
            if (a > peak) peak = a;
        }
        peakLevel.store(peak, std::memory_order_relaxed);

        // Feed BTT (dft_sample_t = float, matches JUCE)
        btt_process(bttInstance, mono, numSamples);

        // Read raw results
        double rawBpm = btt_get_tempo_bpm(bttInstance);
        double cert   = btt_get_tempo_certainty(bttInstance);

        // Apply EMA smoothing to BPM output
        if (rawBpm > 0.0)
        {
            double prev = emaState;
            if (prev <= 0.0)
            {
                // First valid reading — snap immediately
                emaState = rawBpm;
            }
            else
            {
                // EMA: alpha controls reactivity.
                // smoothing 0.0 → alpha 0.30 (fast tracking)
                // smoothing 1.0 → alpha 0.02 (very stable)
                float sm = smoothing.load(std::memory_order_relaxed);
                double alpha = 0.30 - 0.28 * (double)sm;  // 0.30 .. 0.02
                emaState = alpha * rawBpm + (1.0 - alpha) * prev;
            }
            detectedBpm.store(emaState, std::memory_order_relaxed);
        }
        confidence.store(cert, std::memory_order_relaxed);
    }

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override
    {
        if (device)
        {
            numChannelsAvailable = device->getActiveInputChannels().countNumberOfSetBits();
            currentSampleRate = device->getCurrentSampleRate();
            currentBufferSize = device->getCurrentBufferSizeSamples();
        }
        monoBuffer.resize((size_t)(currentBufferSize > 0 ? currentBufferSize : 1024));
    }

    void audioDeviceStopped() override
    {
        peakLevel.store(0.0f, std::memory_order_relaxed);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioBpmInput)
};
