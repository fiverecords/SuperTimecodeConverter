// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

#pragma once
#include <JuceHeader.h>
#include "TimecodeCore.h"
#include "AudioDeviceHub.h"
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

        // Open (or share) the device through the hub for output; the hub
        // calls audioDeviceAboutToStart() -- sample rate, latency, encoder
        // reset -- before returning.
        juce::String err;
        auto* device = AudioDeviceHub::get().acquire(this, typeName, devName, false, sampleRate, bufferSize, err);
        if (device == nullptr)
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
        isRunningFlag.store(true, std::memory_order_relaxed);
        return true;
    }

    void stop()
    {
        if (isRunningFlag.load(std::memory_order_relaxed))
        {
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

    void setTimecode(const Timecode& tc)
    {
        const uint64_t packed = packTimecode(tc.hours, tc.minutes, tc.seconds, tc.frames);
        packedPendingTc.store(packed, std::memory_order_relaxed);
    }

    /// Publish where the timecode clock currently sits inside its frame, in
    /// milliseconds, as measured by the source itself (see the phase section
    /// in TimecodeEngine).  Called once per engine tick; the encoder uses it
    /// only when it (re)seeds, to start its frame at the matching position
    /// instead of at bit 0 of whichever audio buffer arrives first.
    ///
    /// Deriving this from the source rather than from the tick matters: the
    /// engine ticks at 60Hz, which at 30fps is exactly twice the frame rate,
    /// so simply timing the value change would give a constant-but-arbitrary
    /// error per session -- precisely the random start-up phase reported in
    /// issue #15.
    void setFramePhaseMs(double msIntoFrame)
    {
        framePhaseMs.store(msIntoFrame, std::memory_order_relaxed);
        framePhaseAtMs.store(juce::Time::getMillisecondCounterHiRes(),
                             std::memory_order_relaxed);
        haveFramePhase.store(true, std::memory_order_release);
    }

    /// The output latency compensation actually in force, in milliseconds.
    /// Set automatically from the device when it opens (see
    /// audioDeviceAboutToStart); exposed read-only for the status display so
    /// the applied value is visible without being an operator control.
    double getLatencyCompensationMs() const { return latencyCompMs.load(std::memory_order_relaxed); }

    /// DEBUG user-bits mode: every group carries the low nibble of the
    /// callback (buffer) count -- see packFrame.  Off in normal use.
    void setBufferCounterUserBits(bool on) { bufferCounterUserBits.store(on, std::memory_order_relaxed); }

    /// Output gaps seen since start (device ran dry between callbacks; the
    /// encoder re-seeds at the published phase each time).  For the status
    /// line: an interface that keeps doing this is worth knowing about.
    int    getOutputGapCount() const { return outputGapCount.load(std::memory_order_relaxed); }
    double getLastGapMs() const      { return lastGapMs.load(std::memory_order_relaxed); }

    /// Output latency the device reports, in milliseconds (0 if unknown).
    /// Used by the AUTO compensation mode; ASIO drivers are not always
    /// truthful about this, which is why a manual trim also exists.
    double getDeviceReportedLatencyMs() const
    {
        return deviceLatencyMs.load(std::memory_order_relaxed);
    }

    void setFrameRate(FrameRate fps)  { pendingFps.store(fps, std::memory_order_relaxed); }
    void setPitchMultiplier(double p) { pitchMultiplier.store(p, std::memory_order_relaxed); }

    /// Force encoder to re-seed from current pendingTc on next audio callback.
    /// Call when resuming from pause to avoid continuing a stale half-encoded frame.
    void reseed()
    {
        needNewFrame.store(true, std::memory_order_relaxed);
        encoderSeeded.store(false, std::memory_order_relaxed);
    }

    void setPaused(bool p)
    {
        paused.store(p, std::memory_order_relaxed);
        if (p)
            peakLevel.store(0.0f, std::memory_order_relaxed);  // meter drops immediately
    }
    bool isPaused() const             { return paused.load(std::memory_order_relaxed); }

    void setHoldOnPause(bool h)       { holdOnPause.store(h, std::memory_order_relaxed); }
    bool getHoldOnPause() const       { return holdOnPause.load(std::memory_order_relaxed); }

    void setOutputGain(float gain)    { outputGain.store(juce::jlimit(0.0f, 2.0f, gain), std::memory_order_relaxed); }
    float getOutputGain() const       { return outputGain.load(std::memory_order_relaxed); }

    float getPeakLevel() const        { return peakLevel.load(std::memory_order_relaxed); }

    /// LTC user bits (32-bit "binary groups").  Displayed/entered as an
    /// 8-digit hex value; the least significant digit occupies binary group
    /// 1 and the most significant group 8 (see packFrame).
    /// Takes effect on the next encoded frame; no reseed needed.
    void setUserBits(uint32_t bits)   { userBits.store(bits, std::memory_order_relaxed); }
    /// Binary group flags (bit0=BGF0, bit1=BGF1, bit2=BGF2); see SMPTE
    /// 12M-1-2008 Table 1.  0 = user-defined data / unspecified clock.
    void setBinaryGroupFlags(uint8_t f) { binaryGroupFlags.store(f & 0x7, std::memory_order_relaxed); }
    uint8_t getBinaryGroupFlags() const { return binaryGroupFlags.load(std::memory_order_relaxed); }
    uint32_t getUserBits() const      { return userBits.load(std::memory_order_relaxed); }

private:
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
    // Frame-phase alignment (issue #15): see setFramePhaseMs above.
    // Sub-frame phase published by the engine: how far into the frame the
    // timecode clock was (framePhaseMs) at the instant it was published
    // (framePhaseAtMs).  The encoder ages it forward to the moment it seeds.
    // Output gap detector (#19): audio thread state and counters for the UI.
    double lastCallbackMs = 0.0;
    uint32_t callbackCounter = 0;                       // audio thread only
    std::atomic<bool> bufferCounterUserBits { false };  // DEBUG user-bits mode
    std::atomic<int>    outputGapCount { 0 };
    std::atomic<double> lastGapMs      { 0.0 };

    std::atomic<double> framePhaseMs   { 0.0 };
    std::atomic<double> framePhaseAtMs { 0.0 };
    std::atomic<bool>   haveFramePhase { false };
    std::atomic<double> latencyCompMs { 0.0 };
    std::atomic<double> deviceLatencyMs { 0.0 };
    std::atomic<FrameRate> pendingFps { FrameRate::FPS_25 };
    std::atomic<bool> paused { false };
    std::atomic<bool> holdOnPause { false };
    std::atomic<float> outputGain { 1.0f };
    std::atomic<float> peakLevel { 0.0f };
    std::atomic<double> pitchMultiplier { 1.0 };
    // LTC user bits (SMPTE 12M "binary groups"): 32 bits = eight 4-bit
    // groups carried in every frame alongside the timecode.  Free for the
    // operator to use however they like (reel/scene/take, date, or -- as
    // requested in issue #13 -- a value read by other equipment).  Written
    // from the message thread, read once per frame in packFrame(); the
    // whole word is one atomic so a frame never carries a torn value.
    // Default 0 reproduces the previous all-zero user bits exactly.
    std::atomic<uint32_t> userBits { 0 };
    // Binary group flags, packed as bit0=BGF0, bit1=BGF1, bit2=BGF2.
    // Default 0 = user-defined data, unspecified clock (12M-1 sec. 8.4.1).
    std::atomic<uint8_t> binaryGroupFlags { 0 };

    // LTC encoder state -- mostly audio-callback-thread-only.
    // EXCEPTION: needNewFrame and encoderSeeded are also written by reseed()
    // from the message thread, so they must be atomic to avoid data races
    // (especially on ARM / Apple Silicon where non-atomic cross-thread writes
    // can produce torn reads).
    static constexpr int LTC_FRAME_BITS = 80;
    uint8_t frameBits[LTC_FRAME_BITS] = {};
    int currentBitIndex = 0;
    int halfCellIndex = 0;
    double samplePositionInHalfBit = 0.0;
    double samplesPerHalfBit = 0.0;
    float currentLevel = 1.0f;
    std::atomic<bool> needNewFrame { true };
    static constexpr float baseAmplitude = 0.8f;

    // Auto-increment state: the encoder maintains its own running timecode
    // to avoid repeating frames when the UI thread lags behind the audio clock
    Timecode encoderTc;
    std::atomic<bool> encoderSeeded { false };

    void resetEncoder()
    {
        lastCallbackMs = 0.0;
        currentBitIndex = 0;
        halfCellIndex = 0;
        samplePositionInHalfBit = 0.0;
        currentLevel = 1.0f;
        needNewFrame.store(true, std::memory_order_relaxed);
        encoderSeeded.store(false, std::memory_order_relaxed);
        encoderTc = Timecode();
        updateSamplesPerBit();
    }

    void updateSamplesPerBit()
    {
        double fps = frameRateToDouble(pendingFps.load(std::memory_order_relaxed));
        double pitch = pitchMultiplier.load(std::memory_order_relaxed);
        if (pitch <= 0.0) pitch = 1.0;
        // Scale bit duration by pitch: faster pitch -> shorter bits -> more frames/sec
        samplesPerHalfBit = currentSampleRate / (fps * pitch * LTC_FRAME_BITS * 2.0);
    }

    /// Assemble the next 80-bit codeword.  seedAdvanceFrames is only used on
    /// the first frame after a (re)seed: the number of whole frames the
    /// timecode clock has moved since the value in pendingTc was published
    /// (see the phase alignment in the audio callback).
    void packFrame(int seedAdvanceFrames = 0)
    {
        FrameRate fps = pendingFps.load(std::memory_order_relaxed);
        Timecode pendingTc = unpackTimecode(packedPendingTc.load(std::memory_order_relaxed));

        if (!encoderSeeded.load(std::memory_order_relaxed))
        {
            encoderTc = pendingTc;
            for (int i = 0; i < seedAdvanceFrames; ++i)
                encoderTc = incrementFrame(encoderTc, fps);
            encoderSeeded.store(true, std::memory_order_relaxed);
        }
        else if (paused.load(std::memory_order_relaxed))
        {
            // Hold on pause: the engine froze pendingTc at the stop point, and
            // the signal must carry exactly that value on every frame.  No
            // auto-increment here -- with it, the resync rule below pulled the
            // value back every other frame and the output alternated V / V+1.
            encoderTc = pendingTc;
        }
        else
        {
            // Auto-increment from the last encoded frame, then let the shared
            // tracking policy (TimecodeCore) correct one frame at a time
            // towards the engine value, or snap on a real seek.  The bit clock
            // already follows pitch (pitchMultiplier), so here only clock
            // drift between the audio device and the source shows up.
            encoderTc = trackPublishedValue(incrementFrame(encoderTc, fps), pendingTc, 1, fps);
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

        // --- User bits (SMPTE 12M binary groups) ---
        // 32 bits laid into eight 4-bit groups.  Within each group the bits
        // are LSB-first (same convention as the BCD digits above), and the
        // groups follow the same habit the standards use when they assign
        // digits to groups (time address, ST 309 date): binary group 1 holds
        // the LEAST significant hex digit, group 8 the most significant.
        // So userBits 0x12345678 puts '8' in group 1 and '1' in group 8, and
        // a reader that prints the groups as a number -- libltc, Sidus,
        // Pico-Timecode -- shows "12345678".  (Until 2026-09 STC did the
        // opposite, the GoPro convention, and those readers showed the value
        // reversed; the MANUAL mode has a REVERSE DIGIT ORDER option for
        // readers that still expect it.)
        // Debug mode (#19): every binary group carries the low four bits of
        // the count of audio callbacks so far, so a decoded capture shows in
        // which buffer each frame was packed and where the buffer boundaries
        // fall.  With buffers of a frame or more, consecutive frames step the
        // digit; with smaller buffers the digit tells the buffer the frame
        // STARTED in.
        uint32_t ub = userBits.load(std::memory_order_relaxed);
        if (bufferCounterUserBits.load(std::memory_order_relaxed))
            ub = 0x11111111u * (uint32_t)(callbackCounter & 0xF);
        static constexpr int kUserGroupStart[8] =
            { 4, 12, 20, 28, 36, 44, 52, 60 };
        for (int g = 0; g < 8; ++g)
        {
            const int shift  = g * 4;                 // group 0 (BG1) = low nibble
            const int nibble = (ub >> shift) & 0xF;
            const int base   = kUserGroupStart[g];
            frameBits[base + 0] = (nibble >> 0) & 1;
            frameBits[base + 1] = (nibble >> 1) & 1;
            frameBits[base + 2] = (nibble >> 2) & 1;
            frameBits[base + 3] = (nibble >> 3) & 1;
        }

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

        // --- Binary group flags and biphase polarity correction ---
        // Per SMPTE 12M-1-2008 Table 3, three of these bit positions MOVE
        // with the frame rate:
        //
        //            30/24-frame     25-frame
        //   polarity     27              59
        //   BGF0         43              27
        //   BGF1         58              58
        //   BGF2         59              43
        //
        // BUG FIXED HERE (v1.9.11-beta22): STC previously wrote a computed
        // half-word parity into BOTH bit 27 and bit 59.  At 30/24 fps bit 59
        // is not a parity bit at all -- it is BGF2 -- so roughly half of all
        // emitted frames carried BGF2=1, BGF1=0, BGF0=0, which per Table 1
        // declares "the binary groups contain date and time zone data
        // encoded as described in SMPTE 309M".  STC was therefore telling
        // 309M-aware receivers to decode its user bits as a date, with the
        // claim flickering frame by frame according to the timecode value.
        // At 25 fps the same happened to BGF0 (declaring ISO 8-bit character
        // data).  Timecode decoding was unaffected -- biphase mark is
        // polarity insensitive and most gear ignores the BGFs -- but the
        // signalling was wrong, and it becomes actively harmful now that the
        // binary groups carry meaningful user data.
        const bool is25 = (fps == FrameRate::FPS_25);
        const int bitPolarity = is25 ? 59 : 27;
        const int bitBGF0     = is25 ? 27 : 43;
        const int bitBGF1     = 58;
        const int bitBGF2     = is25 ? 43 : 59;

        // Binary group flags.  Default 0/0/0 = "character set not specified
        // and unspecified clock time" (12M-1 sec. 8.4.1), under which the 32
        // user bits "may be assigned in any manner without restriction" --
        // exactly what STC's user-bits feature provides.  The setter exists
        // so future work (e.g. real SMPTE ST 309 date encoding, which
        // requires BGF2=1) can signal correctly.
        const uint8_t bgf = binaryGroupFlags.load(std::memory_order_relaxed);
        frameBits[bitBGF0] = (bgf >> 0) & 1;
        frameBits[bitBGF1] = (bgf >> 1) & 1;
        frameBits[bitBGF2] = (bgf >> 2) & 1;

        // Biphase mark polarity correction (12M-1 sec. 9.2.3): the bit is set
        // so the whole 80-bit codeword contains an even number of logical
        // zeros.  Normative rule: if the number of logical zeros in bits
        // 0-63, excluding the correction bit itself, is odd, set it to 1.
        // Must run last -- every other bit in 0..63 has to be final.
        frameBits[bitPolarity] = 0;
        int zeros = 0;
        for (int i = 0; i <= 63; ++i)
            if (i != bitPolarity && frameBits[i] == 0)
                ++zeros;
        frameBits[bitPolarity] = (zeros & 1) ? 1 : 0;
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

        // Wall-clock instant of this callback, used by the phase alignment
        // and by the gap detector below.  Taken once per callback.
        const double callbackStartMs = juce::Time::getMillisecondCounterHiRes();
        ++callbackCounter;

        // --- Output gap detector (issue #19) ---
        // A callback that arrives much later than one period after the
        // previous one means the device ran dry in between: it played
        // silence (or stale data) for the missing time and then resumed our
        // stream where it left off, so everything after it is late by the
        // length of the hole.  Seen on a display wake with two USB
        // interfaces: a 10.0 ms hole on one, a 24.5 ms one on the other --
        // the hole is the length of the stall, not a whole number of device
        // periods, so it is not measured and skipped; the encoder re-seeds
        // at the phase the engine publishes (D5), which is where the frame
        // boundary should be NOW whatever the hole was.  The frame in
        // progress is cut short: one glitchy frame on top of the glitch the
        // hole already was, then the phase is back on the wire.
        {
            const double expectedMs = (double) numSamples * 1000.0 / currentSampleRate;
            if (lastCallbackMs > 0.0 && expectedMs > 0.0)
            {
                const double gapMs = (callbackStartMs - lastCallbackMs) - expectedMs;
                // Three quarters of a period, but never under 4 ms: at 64 or
                // 128 samples a period is 1-3 ms and ordinary scheduling
                // jitter would read as holes; a hole that short is a few
                // percent of a frame and not worth a re-seed anyway.
                if (gapMs > juce::jmax(4.0, 0.75 * expectedMs))
                {
                    outputGapCount.fetch_add(1, std::memory_order_relaxed);
                    lastGapMs.store(gapMs, std::memory_order_relaxed);
                    if (haveFramePhase.load(std::memory_order_acquire))
                    {
                        needNewFrame.store(true, std::memory_order_relaxed);
                        encoderSeeded.store(false, std::memory_order_relaxed);
                    }
                }
            }
            lastCallbackMs = callbackStartMs;
        }

        if (paused.load(std::memory_order_relaxed) && !holdOnPause.load(std::memory_order_relaxed))
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
            if (needNewFrame.load(std::memory_order_relaxed))
            {
                const bool seeding = !encoderSeeded.load(std::memory_order_relaxed);

                updateSamplesPerBit();

                // --- Frame-phase alignment (issue #15) ---
                // Only on the FIRST frame after a (re)seed.  Previously the
                // encoder always began a fresh frame at this point, which
                // anchored the LTC frame boundary to whenever the audio
                // device happened to start -- giving a phase that was
                // constant while running but randomly different on every
                // restart, spread across the whole frame.  Here we instead
                // start part-way through the frame, at the position matching
                // how long ago the engine advanced to this frame (plus any
                // output latency compensation, so the phase is correct at
                // the connector rather than in the buffer).  The remaining
                // bits of the current frame go out as a partial codeword,
                // which decoders simply ignore before locking to the next
                // full frame.
                //
                // The elapsed time can exceed one frame (phase + age of the
                // published value + latency): the whole frames it contains
                // are carried into the seeded value, otherwise the bit
                // position is right but the value is a frame late -- which
                // is what happened before, at random depending on the phase
                // at the instant of seeding.
                int    seedAdvance = 0;
                double seedHalfCellsIn = -1.0;
                if (seeding && haveFramePhase.load(std::memory_order_acquire)
                    && samplesPerHalfBit > 0.0)
                {
                    const double frameMs = samplesPerHalfBit * LTC_FRAME_BITS * 2.0
                                           * 1000.0 / currentSampleRate;
                    if (frameMs > 0.0)
                    {
                        // Phase at publication, plus however long ago that
                        // was, plus where this sample sits in the buffer,
                        // plus the output latency so the alignment holds at
                        // the connector rather than in the buffer.
                        double elapsed = framePhaseMs.load(std::memory_order_relaxed)
                                       + (callbackStartMs
                                          - framePhaseAtMs.load(std::memory_order_relaxed))
                                       + (double) i * 1000.0 / currentSampleRate
                                       + latencyCompMs.load(std::memory_order_relaxed);
                        // Whole frames carry into the value; the remainder
                        // positions the bit pointer.  Both negative-safe.
                        double whole = std::floor(elapsed / frameMs);
                        elapsed -= whole * frameMs;
                        if (elapsed < 0.0) { elapsed += frameMs; whole -= 1.0; }
                        seedAdvance = (int) juce::jlimit(0.0, 8.0, whole);
                        seedHalfCellsIn = (elapsed / frameMs) * (double)(LTC_FRAME_BITS * 2);
                    }
                }

                packFrame(seedAdvance);
                currentBitIndex = 0;
                halfCellIndex = 0;
                samplePositionInHalfBit = 0.0;

                if (seedHalfCellsIn >= 0.0)
                {
                    int wholeHalfCells = (int)seedHalfCellsIn;
                    if (wholeHalfCells < 0) wholeHalfCells = 0;
                    if (wholeHalfCells > LTC_FRAME_BITS * 2 - 1)
                        wholeHalfCells = LTC_FRAME_BITS * 2 - 1;

                    currentBitIndex = wholeHalfCells / 2;
                    halfCellIndex   = wholeHalfCells % 2;
                    samplePositionInHalfBit =
                        (seedHalfCellsIn - (double)wholeHalfCells) * samplesPerHalfBit;
                }

                needNewFrame.store(false, std::memory_order_relaxed);
                // Do NOT invert currentLevel here -- the mandatory start-of-bit
                // transition for bit 0 was already applied when the previous
                // frame's last bit completed (halfCellIndex 1 -> 0 branch).
                // An extra inversion here would cancel it out, creating a
                // biphase parity error.
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

                    // Mandatory biphase-mark transition at start of every
                    // bit cell -- including bit 0 of the next frame.
                    currentLevel = -currentLevel;

                    if (currentBitIndex >= LTC_FRAME_BITS)
                        needNewFrame.store(true, std::memory_order_relaxed);
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
            // Latency the driver claims for its output path, in ms.  Offered
            // to the UI as the AUTO compensation value; ASIO drivers are
            // often optimistic here, hence the manual trim alongside it.
            const int latSamples = device->getOutputLatencyInSamples();
            const double latMs = currentSampleRate > 0.0
                                     ? (double)latSamples * 1000.0 / currentSampleRate
                                     : 0.0;
            deviceLatencyMs.store(latMs, std::memory_order_relaxed);

            // Apply it automatically.  Without this the LTC leaves the
            // interface systematically late by the output latency; with it,
            // the frame phase is right at the connector.  There is no user
            // control for this on purpose -- it is not something an operator
            // should have to reason about, and a wrong automatic value is
            // still closer than no compensation at all.
            // Clamped to a sane window: a driver reporting an absurd figure
            // (they sometimes do) must not be able to throw the phase off by
            // more than it would have been without compensation.
            latencyCompMs.store(juce::jlimit(0.0, 100.0, latMs),
                                std::memory_order_relaxed);
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
