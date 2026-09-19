// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

#pragma once
#include <JuceHeader.h>
#include "TimecodeCore.h"
#include "AudioDeviceHub.h"
#include <atomic>
#include <functional>
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
        framePhaseAtMs.store(nowMs(), std::memory_order_relaxed);
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
    bool   getLastGapReseeded() const { return lastGapReseeded.load(std::memory_order_relaxed); }

    /// Every value correction and re-seed the encoder makes, counted on the
    /// audio thread so the engine can write each one to ltc_gaps.log next
    /// to the holes (D30): a logic-analyser capture of a skipped or repeated
    /// frame can then be lined up with what STC saw at that instant.
    /// kind: 1 the encoder was ahead (a frame repeated), 2 behind (a frame
    /// skipped), 3 a seek (snapped), 4 re-seeded after a hole, 5 re-seeded
    /// after a snap.
    int      getTrackEventCount() const { return trackEventCount.load(std::memory_order_relaxed); }
    int      getLastTrackKind() const   { return lastTrackKind.load(std::memory_order_relaxed); }
    int64_t  getLastTrackD() const      { return lastTrackD.load(std::memory_order_relaxed); }
    Timecode getLastTrackNext() const   { return unpackTimecode(lastTrackNext.load(std::memory_order_relaxed)); }
    Timecode getLastTrackRef() const    { return unpackTimecode(lastTrackRef.load(std::memory_order_relaxed)); }

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
    // Phase lock (D9), audio thread only except the atomic for the bench.
    bool valueSnapped = false;    // packFrame saw the tracking policy snap (source seek)
    double lockAdj = 0.0;         // fraction of the bit clock, +/- kLockMaxAdj
    double lockIntegral = 0.0;
    double lockErrFilt = 0.0;
    bool   lockSlewing = false;   // absorbing a step the PI cannot reach (D27)
    int     trackOffFrames = 0;   // boundaries in a row the reference has disagreed (D30)
    int64_t trackOffD      = -1;  // the disagreement being counted
    std::atomic<double> lastLockErrorMs { 0.0 };

    // Output gap detector (#19): audio thread state and counters for the UI.
    double lastCallbackMs = 0.0;
    uint32_t callbackCounter = 0;                       // audio thread only
    std::atomic<bool> bufferCounterUserBits { false };  // DEBUG user-bits mode
    std::atomic<int>    outputGapCount { 0 };
    std::atomic<double> lastGapMs      { 0.0 };
    std::atomic<bool>   lastGapReseeded { false };
    std::atomic<int>      trackEventCount { 0 };
    std::atomic<int>      lastTrackKind   { 0 };
    std::atomic<int64_t>  lastTrackD      { 0 };
    std::atomic<uint64_t> lastTrackNext   { 0 };
    std::atomic<uint64_t> lastTrackRef    { 0 };

    void noteTrackEvent(int kind, int64_t d, const Timecode& next, const Timecode& ref)
    {
        lastTrackKind.store(kind, std::memory_order_relaxed);
        lastTrackD.store(d, std::memory_order_relaxed);
        lastTrackNext.store(packTimecode(next.hours, next.minutes, next.seconds, next.frames), std::memory_order_relaxed);
        lastTrackRef.store(packTimecode(ref.hours, ref.minutes, ref.seconds, ref.frames), std::memory_order_relaxed);
        trackEventCount.fetch_add(1, std::memory_order_relaxed);
    }

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

    /// Wall clock for the phase alignment, the lock and the gap detector.
    /// Injectable so the encoder can be simulated with a source clock that
    /// drifts against the sample clock (tools/audit); production uses the
    /// high-resolution counter.
    std::function<double()> timeSource;
    double nowMs() const { return timeSource ? timeSource() : juce::Time::getMillisecondCounterHiRes(); }

    void resetEncoder()
    {
        lastCallbackMs = 0.0;
        lockAdj = 0.0; lockIntegral = 0.0; lockErrFilt = 0.0; lockSlewing = false;
        trackOffFrames = 0; trackOffD = -1;
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
        // Scale bit duration by pitch: faster pitch -> shorter bits -> more
        // frames/sec.  lockAdj (D9) trims the bit clock by up to +/-200 ppm
        // to hold the frame phase on the source's clock: positive when the
        // encoder is late (shorter bits catch up).
        samplesPerHalfBit = currentSampleRate / (fps * pitch * LTC_FRAME_BITS * 2.0) * (1.0 - lockAdj);
    }

    //==========================================================================
    // Phase lock (D9).
    //
    // The bit stream runs on the audio device's clock; the timecode it
    // carries runs on the source's (the CPU for the generator, the deck for
    // Pro DJ Link, the sender for MTC/LTC in).  Seeding aligns the two at one
    // instant; after that they drift apart at the difference between the
    // clocks -- 50 ppm is a frame every 11 minutes -- and until now the only
    // correction was a whole-frame step when the values disagreed, with the
    // phase sawtoothing through a full frame in between.
    //
    // At every frame boundary the encoder now measures where the source's
    // frame boundary is (the published phase, aged to the instant this
    // boundary reaches the connector) and trims its bit clock with a slow PI
    // loop: proportional 0.1/s, integral for the standing clock difference,
    // +/-200 ppm limit (a receiver's tolerance, and more than any pair of
    // clocks differs).  Drift is absorbed by the integral, jitter in the
    // publication (MTC arrivals, deck packets) is filtered by the loop's
    // ~10 s time constant, and a persistent error of more than
    // kLockSlewEnterMs -- a hole the interface made without delaying the
    // callback, which the cadence detector cannot see -- is absorbed by
    // running the bit clock fast or slow until it is gone (D27), rather than
    // by a 50-second crawl at 200 ppm or by jumping.  Everything here runs on
    // the audio thread, once per frame.
    //==========================================================================
    static constexpr double kLockKp          = 0.1;      // per second
    static constexpr double kLockKi          = 0.0025;   // per second squared (critically damped)
    static constexpr double kLockMaxAdj      = 200e-6;   // +/- fraction of the bit clock

    // Slew, not jump (D27).  A phase error this loop cannot absorb at 200 ppm
    // used to re-seed, and a seed starts the codeword part-way through by
    // design (issue #15), so it costs one frame no decoder can read -- which
    // is what @mungewell's captures show as a short unreadable stretch with
    // the buffer counter running straight through it.  Nothing about a phase
    // error requires breaking the frame: running the bit clock fast or slow
    // moves the boundary just as well and every frame stays whole.  A source
    // that genuinely jumped still re-seeds, through the value snapping, which
    // is a different path.
    // Persistence gate on value corrections (D30): how many boundaries in a
    // row the reference has to disagree with the encoder before the shared
    // policy is consulted.  An engine tick is a fraction of a frame, so a
    // reference that is wrong for one tick can never get here; a real offset
    // is corrected within this many frames.
    static constexpr int kTrackPersistFrames = 3;

    static constexpr double kLockSlewEnterMs = 4.0;        // filtered error beyond this: slew
    static constexpr double kLockSlewExitMs  = 0.5;        // back under this: hand it to the PI
    static constexpr double kLockSlewMaxAdj  = 20000e-6;   // 2 %, varispeed territory for any reader
    static constexpr double kLockSlewMs      = 1000.0;     // clear the error in about a second

    // How long the encoder keeps carrying the source's value forward on its
    // own when the engine's publication stops arriving (D26).  The engine
    // ticks at 60 Hz, so a publication older than this by any margin means its
    // thread is not running -- a display wake on Windows costs a quarter of a
    // second of it.  A deliberate freeze never gets here: the engine marks the
    // output paused and republishes every tick, which is the branch above.
    //
    // The limit is not precision.  Extrapolating on the interface's clock, a
    // clock difference of 50 ppm is 50 microseconds of error in a second.  It
    // is how long the encoder may keep assuming the source is still moving
    // when nothing has confirmed it.  Past it the value holds, the tracking
    // repeats frames, and a stalled timecode on the wire is the visible sign
    // that the application is not running -- which is the failure worth
    // seeing.
    static constexpr double kPublicationGraceMs = 1000.0;

    /// Called at a frame boundary that is not a seed.  `boundaryConnectorMs`
    /// is the instant this boundary reaches the connector.
    void lockStep(double boundaryConnectorMs)
    {
        if (! haveFramePhase.load(std::memory_order_acquire)) return;
        // Hold on pause: the source's phase is frozen while the encoder
        // free-runs by design; there is nothing to lock to until it resumes.
        if (paused.load(std::memory_order_relaxed)) return;

        // The published phase is in source (position) time; wall time since
        // the publication advances it at the source's speed.  The frame is
        // one source frame.
        const double fps     = frameRateToDouble(pendingFps.load(std::memory_order_relaxed));
        const double frameMs = 1000.0 / fps;
        double pitch = pitchMultiplier.load(std::memory_order_relaxed);
        if (pitch <= 0.0) pitch = 1.0;

        double p = framePhaseMs.load(std::memory_order_relaxed)
                 + (boundaryConnectorMs - framePhaseAtMs.load(std::memory_order_relaxed)) * pitch;
        p = std::fmod(p, frameMs); if (p < 0.0) p += frameMs;
        // Error, positive when the encoder is late (the source's boundary
        // passed p ms ago), in wall time.
        const double eMs = ((p > frameMs * 0.5) ? p - frameMs : p) / pitch;

        // The source's timeline moved under us without the engine announcing
        // a seek (a clock-mode re-anchor, an MTC locate the decoder absorbed,
        // a publication hiccup).  A slow crawl at 200 ppm is the wrong answer
        // to a step, but so is a jump: absorb it by running the bit clock at
        // whatever rate clears it in about a second, and every frame on the
        // wire stays whole while it happens.  (A DAC-side hole is NOT what
        // this sees: that moves the actual boundary without touching the
        // encoder's timeline, and nothing inside STC can measure it -- D24.)
        lockErrFilt += 0.1 * (eMs - lockErrFilt);

        const double absErr = std::abs(lockErrFilt);
        if (absErr > kLockSlewEnterMs)      lockSlewing = true;
        else if (absErr < kLockSlewExitMs)  lockSlewing = false;

        if (lockSlewing)
        {
            const double rate = juce::jlimit(kLockMaxAdj, kLockSlewMaxAdj, absErr / kLockSlewMs);
            lockAdj = (lockErrFilt > 0.0) ? rate : -rate;   // positive = late = shorter bits
            // The integral holds its standing value: it carries the clock
            // difference between this interface and the source, which the
            // step did not change.
            lastLockErrorMs.store(eMs, std::memory_order_relaxed);
            return;
        }

        const double eS = eMs * 0.001;
        const double dt = frameMs * 0.001 / pitch;
        lockIntegral = juce::jlimit(-kLockMaxAdj, kLockMaxAdj, lockIntegral + kLockKi * eS * dt);
        lockAdj      = juce::jlimit(-kLockMaxAdj, kLockMaxAdj, kLockKp * eS + lockIntegral);
        lastLockErrorMs.store(eMs, std::memory_order_relaxed);
    }

    /// Last measured phase error at a frame boundary, ms, positive = late.
    /// For the bench.
    double getLockErrorMs() const { return lastLockErrorMs.load(std::memory_order_relaxed); }
    double getLockAdjPpm()  const { return lockAdj * 1e6; }

    /// Assemble the next 80-bit codeword.  seedAdvanceFrames is only used on
    /// the first frame after a (re)seed: the number of whole frames the
    /// timecode clock has moved since the value in pendingTc was published
    /// (see the phase alignment in the audio callback).
    /// publishedAheadFrames is that same count for THIS frame, seed or not:
    /// the frame being packed is the one that reaches the connector that many
    /// source frames after the publication, and the tracking below has to age
    /// the published value by it (issue #21).
    void packFrame(int seedAdvanceFrames = 0, int publishedAheadFrames = 0)
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
            // already follows pitch (pitchMultiplier) and the phase lock
            // (D24) holds the phase, so here only what the lock cannot
            // absorb shows up.  A snap means the source jumped: the phase
            // has to follow the value, so the boundary is re-seeded too
            // (see valueSnapped in the callback).
            // Compare like with like.  pendingTc is the source's value at
            // the instant the engine published it; this frame carries the
            // value for the instant it reaches the connector,
            // publishedAheadFrames source frames later.  Comparing the two
            // directly made every frame after the first one in a buffer look
            // like the sender running ahead: with a buffer longer than a
            // frame the policy repeated a frame at the end of each buffer and
            // skipped one at the start of the next, which is issue #21.  The
            // count is taken where the shared policy expects its reference to
            // sit (see the audio callback); the policy itself is untouched,
            // and so are the senders that do not age anything.
            const Timecode publishedAtConnector =
                offsetTimecode(pendingTc, publishedAheadFrames, fps);
            const Timecode next = incrementFrame(encoderTc, fps);
            const int64_t d = frameDistance(publishedAtConnector, next, fps);   // reference - next

            // Persistence gate (D30).  The reference is a fresh measurement at
            // every boundary, and a wrong one for a single engine tick used
            // to cost a frame on the wire: the shared policy corrects at
            // once.  So it is only consulted when the same disagreement has
            // stood for kTrackPersistFrames boundaries in a row.  A blip
            // lasts one tick and never reaches that; a real offset is fixed
            // within it.  A seek (past the hard-resync distance) still snaps
            // at once -- waiting would be wrong for a real jump.
            //
            // And no dead band.  The reference sits half a frame back (see
            // the callback), so the exact state is d == -1.  The policy
            // treats 0 as aligned too, which for THIS reference means the
            // wire running one frame behind the source indefinitely -- which
            // is what a repeat left behind until something else happened to
            // push it forward.  A persistent 0 is handed to the policy with
            // the reference one frame up, so it reads as behind and skips.
            const bool seek = (d > kTrackingHardResync || d < -kTrackingHardResync);
            if (d == -1) { trackOffFrames = 0; trackOffD = -1; }
            else if (d == trackOffD) ++trackOffFrames;
            else { trackOffD = d; trackOffFrames = 1; }

            if (seek || trackOffFrames >= kTrackPersistFrames)
            {
                const Timecode ref = (d == 0) ? incrementFrame(publishedAtConnector, fps)
                                              : publishedAtConnector;
                encoderTc = trackPublishedValue(next, ref, 1, fps);
                trackOffFrames = 0; trackOffD = -1;
                noteTrackEvent(seek ? 3 : (d > -1 ? 2 : 1), d, next, publishedAtConnector);
            }
            else
                encoderTc = next;

            const int64_t moved = frameDistance(encoderTc, next, fps);
            valueSnapped = (moved > 1 || moved < -1);
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
        const double callbackStartMs = nowMs();
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
                // Logged from three quarters of a period, never under 4 ms:
                // at 64 or 128 samples a period is 1-3 ms and ordinary
                // scheduling jitter would read as holes.
                if (gapMs > juce::jmax(4.0, 0.75 * expectedMs))
                {
                    outputGapCount.fetch_add(1, std::memory_order_relaxed);
                    lastGapMs.store(gapMs, std::memory_order_relaxed);

                    // Re-seeded only for a hole (D30).  A double-buffered
                    // driver has one period of slack, so a callback less
                    // than a period late did not starve the DAC: the
                    // samples on the wire are still contiguous, and a
                    // re-seed would be the only thing to break them -- it
                    // starts the codeword part-way through.  And a hole
                    // under half a frame is one the lock slews out with
                    // every frame whole (D27); only a longer one is worth
                    // an instant re-alignment.  At a MOTU's 512-sample
                    // period the old three-quarter threshold was 8 ms,
                    // which Windows scheduling reaches on its own.
                    const double frameMsNow = 1000.0 / juce::jmax(1.0, frameRateToDouble(pendingFps.load(std::memory_order_relaxed)));
                    const bool hole = gapMs > juce::jmax(expectedMs, 0.5 * frameMsNow);
                    lastGapReseeded.store(hole, std::memory_order_relaxed);
                    if (hole && haveFramePhase.load(std::memory_order_acquire))
                    {
                        needNewFrame.store(true, std::memory_order_relaxed);
                        encoderSeeded.store(false, std::memory_order_relaxed);
                        noteTrackEvent(4, (int64_t) gapMs, encoderTc, encoderTc);
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

                // The instant this frame reaches the connector: this
                // callback, plus where this sample sits in the buffer, plus
                // the output latency.  The lock, the seed and the value the
                // source will be showing are all measured against it, because
                // that is when this frame is heard -- not now.
                const double connectorMs = callbackStartMs
                                         + (double) i * 1000.0 / currentSampleRate
                                         + latencyCompMs.load(std::memory_order_relaxed);

                // Whole source frames from the publication of pendingTc to
                // that instant, and where inside its frame the source will be
                // then.  The seed uses both; the tracking in packFrame uses
                // the count (issue #21).
                const double frameMs = 1000.0 / frameRateToDouble(pendingFps.load(std::memory_order_relaxed));
                int    aheadFrames      = 0;   // to the leading edge: the seed
                int    aheadValueFrames = 0;   // to mid-frame: the value tracking
                double aheadRemainderMs = 0.0;
                bool   haveAhead        = false;
                {
                    double pitchNow = pitchMultiplier.load(std::memory_order_relaxed);
                    if (pitchNow <= 0.0) pitchNow = 1.0;
                    if (frameMs > 0.0 && haveFramePhase.load(std::memory_order_acquire))
                    {
                        // Phase at publication, aged to the connector at the
                        // source's speed, so the position lands right under
                        // pitch too.
                        double elapsed = framePhaseMs.load(std::memory_order_relaxed)
                                       + (connectorMs - framePhaseAtMs.load(std::memory_order_relaxed)) * pitchNow;
                        const double elapsedRaw = elapsed;
                        // Whole frames carry into the value; the remainder
                        // positions the bit pointer.  Both negative-safe.
                        double whole = std::floor(elapsed / frameMs);
                        elapsed -= whole * frameMs;
                        if (elapsed < 0.0) { elapsed += frameMs; whole -= 1.0; }
                        // A stale or absurd publication must not be able to
                        // throw the value forward without limit.  What the
                        // source can legitimately have moved is what this
                        // buffer and the output latency hold -- plus, while
                        // the engine's publication is not arriving at all,
                        // whatever grace D26 allows for carrying it forward.
                        // The fixed ceiling of 8 frames this replaces was
                        // itself short of an 8192-sample buffer at 30 fps.
                        const double maxAhead =
                            std::floor((((double) numSamples * 1000.0 / currentSampleRate)
                                        + latencyCompMs.load(std::memory_order_relaxed)
                                        + kPublicationGraceMs) / frameMs) + 2.0;
                        // The seed positions the bit pointer at the leading
                        // edge, so it wants floor().  The tracking wants a
                        // reference the shared policy can read with the band
                        // it already has: "aligned" there is 0 or -1, because
                        // every other sender compares a value published up to
                        // a tick ago against the frame it is emitting -- half
                        // a frame of lag, by construction.  Aging to the
                        // leading edge would hand it a reference centred on
                        // zero instead, and the first frame of drift would
                        // read as being behind.  Taking the count half a frame
                        // further back puts it back on that centre AND moves
                        // its own rounding boundary to the middle of the frame
                        // being emitted, which is the point furthest from this
                        // boundary: once the lock has put the source's
                        // boundary on top of ours, a count taken at the edge
                        // flickers between k-1 and k on nothing but rounding,
                        // and each flicker costs a skipped or repeated frame.
                        aheadFrames      = (int) juce::jlimit(0.0, maxAhead, whole);
                        // -1 is legitimate here and must not be clamped away:
                        // with a small buffer and no latency the connector is
                        // a fraction of a frame ahead of the publication, and
                        // half a frame back from that is the frame BEFORE the
                        // published one.  Clamped to 0 the reference sat a
                        // frame high in exactly that case, the aligned state
                        // read as d == 0 instead of -1, and the dead band the
                        // policy used to have was the only thing hiding it.
                        aheadValueFrames = (int) juce::jlimit(-1.0, maxAhead,
                                                              std::floor(elapsedRaw / frameMs - 0.5));
                        aheadRemainderMs = elapsed;
                        haveAhead        = true;
                    }
                }

                if (! seeding && samplesPerHalfBit > 0.0)
                {
                    // Phase lock step at this boundary (D9): where is the
                    // source's boundary relative to the one we are about to
                    // put on the wire?  Recomputes the bit clock if it trims.
                    lockStep(connectorMs);
                    updateSamplesPerBit();
                    if (! encoderSeeded.load(std::memory_order_relaxed))
                    {
                        // The watchdog asked for a re-seed: fall through to
                        // the seeding path on this same boundary.
                        seedHalfCellsIn = -1.0;
                    }
                }
                const bool seedNow = ! encoderSeeded.load(std::memory_order_relaxed);
                if (seedNow && haveAhead && samplesPerHalfBit > 0.0)
                {
                    lockErrFilt = 0.0;   // fresh alignment; the integral (clock difference) stays
                    seedAdvance     = aheadFrames;
                    seedHalfCellsIn = (aheadRemainderMs / frameMs) * (double)(LTC_FRAME_BITS * 2);
                }

                packFrame(seedAdvance, aheadValueFrames);
                if (valueSnapped)
                {
                    noteTrackEvent(5, 0, encoderTc, encoderTc);
                    // The value just jumped to follow a source seek nobody
                    // announced: re-seed on this same boundary so the phase
                    // follows too, instead of crawling there at 200 ppm.
                    valueSnapped = false;
                    encoderSeeded.store(false, std::memory_order_relaxed);
                    needNewFrame.store(true, std::memory_order_relaxed);
                    --i;        // redo this sample as the seeding boundary
                    continue;
                }
                currentBitIndex = 0;
                halfCellIndex = 0;
                // Keep the fractional sample carried over from the last
                // half-cell of the previous frame.  Zeroing it here rounded
                // every frame up to whole samples, which erased any bit-clock
                // trim smaller than one sample per frame -- the +/-200 ppm of
                // the phase lock, and part of the pitch multiplier (1778
                // samples per frame at +8 % on 25 fps instead of 1777.8).
                // A seed sets the position explicitly below.

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
