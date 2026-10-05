// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter
//
// Audio BPM detection powered by Beat-and-Tempo-Tracking (BTT)
// Copyright (c) 2021 Michael Krzyzaniak -- MIT License
// https://github.com/michaelkrzyzaniak/Beat-and-Tempo-Tracking

#pragma once
#include <JuceHeader.h>
#include "AudioDeviceHub.h"
#include <atomic>
#include <cstring>
#include <cmath>

// BTT is pure C with extern "C" wrappers
#include "BTT.h"

//==============================================================================
// Threading:
//  - start / stop / setSmoothing / setInputGain / resetDetection and the
//    getters run on the message thread.
//  - audioDeviceIOCallbackWithContext runs on the audio thread, called by
//    AudioDeviceHub's fan-out.  It is the only place that runs BTT.
//  - audioDeviceAboutToStart runs when the hub registers us (on the message
//    thread, inside acquire(), BEFORE it puts us on the fan-out) and whenever
//    the device restarts (the hub's reconfigureAll on the message thread, or
//    a restart the driver starts itself).  On a restart JUCE announces the
//    device before the stream runs, under the AudioDeviceManager's callback
//    lock and the hub's fan-out lock, which the audio callback takes as well.
//    Either way it never runs at the same time as our audio callback, so
//    that is where BTT is built (and rebuilt for a new sample rate) and the
//    buffers are sized: allocation is fine there, and the callback never
//    sees a half-built instance.
//  - Settings from the UI reach BTT through atomics read by the audio
//    callback, never by calling BTT from the message thread.
//==============================================================================
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

        // Not registered (stop() released us), so no callback can run yet.
        peakLevel.store(0.0f, std::memory_order_relaxed);
        detectedBpm.store(0.0, std::memory_order_relaxed);
        beatDue.store(false, std::memory_order_relaxed);
        confidence.store(0.0, std::memory_order_relaxed);
        beatCounter.store(0, std::memory_order_relaxed);
        resetRequested.store(false, std::memory_order_relaxed);
        emaState = 0.0;

        // acquire() calls audioDeviceAboutToStart, which builds and configures
        // BTT for the device's actual rate, before it puts us on the fan-out.
        juce::String err;
        auto* device = AudioDeviceHub::get().acquire(this, typeName, devName, true, sampleRate, bufferSize, err);
        if (device == nullptr) return false;

        if (bttInstance == nullptr)
        {
            // btt_new failed (out of memory): do not keep the device open for
            // a client that cannot analyse anything.
            AudioDeviceHub::get().release(this);
            return false;
        }

        numChannelsAvailable = device->getActiveInputChannels().countNumberOfSetBits();
        {
            int ch = selectedChannel.load(std::memory_order_relaxed);
            if (ch >= 0 && ch >= numChannelsAvailable) ch = 0;
            if (ch == -1 && numChannelsAvailable < 2)  ch = 0;
            selectedChannel.store(ch, std::memory_order_relaxed);
        }

        isRunningFlag.store(true, std::memory_order_relaxed);
        return true;
    }

    void stop()
    {
        if (isRunningFlag.load(std::memory_order_relaxed))
        {
            AudioDeviceHub::get().release(this);   // blocks until an in-flight callback returns
            isRunningFlag.store(false, std::memory_order_relaxed);
        }
        // Off the fan-out now (or never got on it), so nothing else touches BTT.
        bttInstance = btt_destroy(bttInstance);    // btt_destroy(nullptr) is a no-op; returns nullptr
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

    // BPM smoothing: 0.0 = fast tracking, 1.0 = very stable.  Sets the output
    // EMA and BTT's tempo histogram (see applySmoothingToBtt); the audio
    // callback applies a new value to BTT before its next btt_process.
    void setSmoothing(float s)
    {
        smoothing.store(juce::jlimit(0.0f, 1.0f, s), std::memory_order_relaxed);
    }
    float getSmoothing() const { return smoothing.load(std::memory_order_relaxed); }

    //==============================================================================
    // Reset detection state (e.g. on source switch).  The published results
    // clear at once; BTT and the EMA are cleared by the audio callback at its
    // next block, never from here.
    //==============================================================================
    void resetDetection()
    {
        detectedBpm.store(0.0, std::memory_order_relaxed);
        confidence.store(0.0, std::memory_order_relaxed);
        beatDue.store(false, std::memory_order_relaxed);
        beatCounter.store(0, std::memory_order_relaxed);
        resetRequested.store(true, std::memory_order_release);
    }

    //==============================================================================
    // hasBpm() threshold on BTT's certainty, which is the height of its tempo
    // histogram at the winning lag.  That height is mostly the log-Gaussian
    // tempo prior (centred on 128 BPM) at that tempo, not a measure of how
    // clean the beat is: on a clean click track (bpm_rate_change harness,
    // 48 kHz, smoothing 0.5) it settles at about 1.0 at 128 BPM, 0.95 at 140,
    // 0.56 at 174, 0.47 at 90, 0.41 at 100 and 0.26 at 80; a 70 BPM click is
    // reported at its double, 139.75, with 0.28.  At 70 BPM itself the prior
    // alone gives about 0.11, below this threshold.  What real programme
    // material gives is not known, so the value is kept until field data
    // says otherwise (AUDIT UI-5, deferred).
    static constexpr double kMinConfidence = 0.15;

private:
    juce::String currentDeviceName;
    juce::String currentTypeName;
    std::atomic<bool> isRunningFlag { false };
    std::atomic<int> selectedChannel { 0 };
    int numChannelsAvailable = 0;
    double currentSampleRate = 48000.0;
    int currentBufferSize = 512;

    BTT* bttInstance = nullptr;   // built/rebuilt in audioDeviceAboutToStart, destroyed in stop()

    // Detection results (written from audio thread, read from UI/engine)
    std::atomic<double>   detectedBpm  { 0.0 };
    std::atomic<double>   confidence   { 0.0 };
    std::atomic<bool>     beatDue      { false };
    std::atomic<uint32_t> beatCounter  { 0 };
    std::atomic<float>    peakLevel    { 0.0f };
    std::atomic<float>    inputGain    { 1.0f };
    std::atomic<float>    smoothing    { 0.5f };  // BPM smoothing: 0=fast, 1=stable
    std::atomic<bool>     resetRequested { false };  // resetDetection() -> audio callback
    double                emaState     = 0.0;     // EMA accumulator (audio thread only)
    float                 appliedSmoothing = -1.0f;  // smoothing last applied to BTT (audio thread / aboutToStart)

    // The EMA's time constant is defined at this callback period: 512 samples
    // at 48 kHz, where the per-callback alpha below is exactly the one it has
    // always been.  Other block sizes and rates get the alpha that gives the
    // same time constant.
    static constexpr double kEmaReferenceSeconds = 512.0 / 48000.0;

    // Mono mix buffer, sized in audioDeviceAboutToStart to the device's
    // buffer.  JUCE's AudioDeviceManager already splits a larger driver block
    // into blocks of that size before the hub sees it; a larger block would
    // still be processed in pieces, never by growing this on the audio thread.
    std::vector<float> monoBuffer;

    //==============================================================================
    static void beatCallbackStatic(void* self, unsigned long long /*sample_time*/)
    {
        auto* me = static_cast<AudioBpmInput*>(self);
        me->beatDue.store(true, std::memory_order_relaxed);
        me->beatCounter.fetch_add(1, std::memory_order_relaxed);
    }

    // BTT for `sampleRate`, configured for DJ/electronic music: the tempo
    // prior is centred on 128 BPM and the range is 60-200 BPM.  Called only
    // from audioDeviceAboutToStart.  nullptr if btt_new fails.
    BTT* createBtt(double sampleRate)
    {
        BTT* b = btt_new(
            BTT_SUGGESTED_SPECTRAL_FLUX_STFT_LEN,
            BTT_SUGGESTED_SPECTRAL_FLUX_STFT_OVERLAP,
            BTT_SUGGESTED_OSS_FILTER_ORDER,
            BTT_SUGGESTED_OSS_LENGTH,
            BTT_SUGGESTED_ONSET_THRESHOLD_N,
            BTT_SUGGESTED_CBSS_LENGTH,
            sampleRate,
            BTT_DEFAULT_ANALYSIS_LATENCY_ONSET_ADJUSTMENT,
            BTT_DEFAULT_ANALYSIS_LATENCY_BEAT_ADJUSTMENT
        );
        if (b == nullptr) return nullptr;

        btt_set_log_gaussian_tempo_weight_mean(b, 128.0);
        btt_set_min_tempo(b, 60.0);
        btt_set_max_tempo(b, 200.0);

        // Beat callback for beat-in-bar tracking
        btt_set_beat_tracking_callback(b, beatCallbackStatic, this);
        return b;
    }

    // BTT's tempo histogram follows the smoothing setting: decay 0.999 (fast)
    // .. 0.9999 (stable), Gaussian kernel width 5 .. 15 lags.  Called only
    // where nothing else can be inside BTT: the audio callback (before its
    // btt_process) or audioDeviceAboutToStart.
    void applySmoothingToBtt(float s)
    {
        appliedSmoothing = s;
        if (bttInstance == nullptr) return;
        btt_set_gaussian_tempo_histogram_decay(bttInstance, 0.999 + 0.0009 * (double) s);
        btt_set_gaussian_tempo_histogram_width(bttInstance, 5.0 + 10.0 * (double) s);
    }

    //==============================================================================
    void audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                          int numInputCh, float* const*, int,
                                          int numSamples,
                                          const juce::AudioIODeviceCallbackContext&) override
    {
        if (!bttInstance || numInputCh <= 0 || numSamples <= 0 || monoBuffer.empty()) return;

        // Requests from the message thread, applied here so they never run
        // while btt_process does.
        if (resetRequested.exchange(false, std::memory_order_acquire))
        {
            btt_clear(bttInstance);
            emaState = 0.0;
        }
        const float sm = smoothing.load(std::memory_order_relaxed);
        if (sm != appliedSmoothing)
            applySmoothingToBtt(sm);

        int selCh = selectedChannel.load(std::memory_order_relaxed);
        bool stereoMix = (selCh == -1);
        const float gain = inputGain.load(std::memory_order_relaxed);

        // Source channel(s) for the mono signal fed to BTT
        const float* srcA = nullptr;
        const float* srcB = nullptr;   // set for the stereo mix
        if (stereoMix && numInputCh >= 2
            && inputChannelData[0] && inputChannelData[1])
        {
            srcA = inputChannelData[0];
            srcB = inputChannelData[1];
        }
        else
        {
            int ch = stereoMix ? 0 : selCh;
            if (ch >= numInputCh || !inputChannelData[ch]) return;
            srcA = inputChannelData[ch];
        }

        // Mix, meter and feed BTT (dft_sample_t = float, matches JUCE) in
        // pieces of at most the preallocated buffer.  BTT is fed sample by
        // sample internally, so the split changes nothing in its result.
        float* const mono = monoBuffer.data();
        const int    chunkMax = (int) monoBuffer.size();
        float peak = 0.0f;
        for (int offset = 0; offset < numSamples; offset += chunkMax)
        {
            const int n = juce::jmin(chunkMax, numSamples - offset);
            if (srcB != nullptr)
                for (int i = 0; i < n; i++)
                    mono[i] = (srcA[offset + i] + srcB[offset + i]) * 0.5f * gain;
            else
                for (int i = 0; i < n; i++)
                    mono[i] = srcA[offset + i] * gain;

            for (int i = 0; i < n; i++)
            {
                float a = std::abs(mono[i]);
                if (a > peak) peak = a;
            }
            btt_process(bttInstance, mono, n);
        }
        peakLevel.store(peak, std::memory_order_relaxed);

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
                // EMA: alpha controls reactivity.  At the reference period
                // (512 samples at 48 kHz, kEmaReferenceSeconds):
                // smoothing 0.0 → alpha 0.30 (fast tracking, tau ~30 ms)
                // smoothing 1.0 → alpha 0.02 (very stable, tau ~0.53 s)
                // For a callback of another length the alpha is the one with
                // the same time constant, so the response no longer depends
                // on the buffer size or the rate.
                //
                // An octave jump in BTT's estimate (64 <-> 128) is averaged
                // through like any other change: the output passes through
                // the tempi in between for a few time constants instead of
                // switching.  Whether to snap on octave jumps is deferred
                // until field data shows how often BTT flips on real
                // material (AUDIT UI-5).
                const double alphaAtReference = 0.30 - 0.28 * (double) sm;  // 0.30 .. 0.02
                const double elapsed = (double) numSamples / currentSampleRate;
                const double alpha = 1.0 - std::pow(1.0 - alphaAtReference, elapsed / kEmaReferenceSeconds);
                emaState = alpha * rawBpm + (1.0 - alpha) * prev;
            }
            detectedBpm.store(emaState, std::memory_order_relaxed);
        }
        confidence.store(cert, std::memory_order_relaxed);
    }

    // Never concurrent with the audio callback (see the class comment), so
    // BTT and the buffer are (re)built here.  A new sample rate needs a new
    // BTT: its rate is fixed at btt_new, and one built for 44.1 kHz fed at
    // 48 kHz reports 128 BPM as 117.5 (AUDIT UI-4).
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override
    {
        if (device)
        {
            numChannelsAvailable = device->getActiveInputChannels().countNumberOfSetBits();
            currentSampleRate = device->getCurrentSampleRate();
            currentBufferSize = device->getCurrentBufferSizeSamples();
        }
        monoBuffer.assign((size_t)(currentBufferSize > 0 ? currentBufferSize : 1024), 0.0f);

        if (bttInstance == nullptr || btt_get_sample_rate(bttInstance) != currentSampleRate)
        {
            bttInstance = btt_destroy(bttInstance);
            if (currentSampleRate > 0.0)
                bttInstance = createBtt(currentSampleRate);   // stays nullptr if btt_new fails: the callback idles
            applySmoothingToBtt(smoothing.load(std::memory_order_relaxed));

            // Results from the old instance no longer apply; detection starts over.
            resetRequested.store(false, std::memory_order_relaxed);
            emaState = 0.0;
            detectedBpm.store(0.0, std::memory_order_relaxed);
            confidence.store(0.0, std::memory_order_relaxed);
            beatDue.store(false, std::memory_order_relaxed);
        }
    }

    void audioDeviceStopped() override
    {
        peakLevel.store(0.0f, std::memory_order_relaxed);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioBpmInput)
};
