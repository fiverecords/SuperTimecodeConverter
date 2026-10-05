// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

#pragma once
#include <JuceHeader.h>
#include "TimecodeCore.h"
#include "AudioDeviceHub.h"
#include <atomic>
#include <cmath>
#include <cstring>
#include <functional>

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
        currentTypeName   = typeName;

        // Open (or share) the device through the hub: one open per physical
        // interface, this component registered on its fan-out for input.
        // The hub calls audioDeviceAboutToStart() before returning, which is
        // where resetDecoder() and resetPassthruBuffer() run.
        juce::String err;
        auto* device = AudioDeviceHub::get().acquire(this, typeName, devName, true, sampleRate, bufferSize, err);
        if (device == nullptr)
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

        // The rate, the buffer size, the latency, resetDecoder() and
        // resetPassthruBuffer() were done in audioDeviceAboutToStart(), before
        // the hub registered this callback.  From here on the audio thread
        // may be running it, so nothing it reads is written here (it used to
        // rewrite the sample rate; AUDIT LTC-17).  Only the peak levels,
        // which are atomics, are reset.
        ltcPeakLevel.store(0.0f, std::memory_order_relaxed);
        thruPeakLevel.store(0.0f, std::memory_order_relaxed);
        isRunningFlag.store(true, std::memory_order_relaxed);
        return true;
    }

    void stop()
    {
        if (isRunningFlag.load(std::memory_order_relaxed))
        {
            AudioDeviceHub::get().release(this);   // closes the device when nobody else uses it
            isRunningFlag.store(false, std::memory_order_relaxed);
        }
    }

    //==============================================================================
    bool getIsRunning() const { return isRunningFlag.load(std::memory_order_relaxed); }
    juce::String getCurrentDeviceName() const { return currentDeviceName; }
    juce::String getCurrentTypeName() const  { return currentTypeName; }
    int getSelectedChannel() const { return selectedChannel.load(std::memory_order_relaxed); }
    int getPassthruChannel() const { return passthruChannel.load(std::memory_order_relaxed); }
    int getChannelCount() const { return numChannelsAvailable; }
    double getActualSampleRate() const { return currentSampleRate; }
    int getActualBufferSize() const { return currentBufferSize; }

    /// The last frame read (DESIGN D19: not the live value, which is the next
    /// one).  For display; the engine should take getLiveTimecode(), which
    /// ages it and pairs it with its phase (AUDIT LTC-10).
    Timecode getCurrentTimecode() const
    {
        return unpackTimecode(packedTimecode.load(std::memory_order_relaxed));
    }

    FrameRate getDetectedFrameRate() const
    {
        return detectedFps.load(std::memory_order_relaxed);
    }

    /// User bits recovered from the incoming LTC (32-bit binary groups).
    /// 0 until the first good frame is decoded.
    uint32_t getUserBits() const
    {
        return userBitsIn.load(std::memory_order_relaxed);
    }

    /// Wall-clock instant (hi-res ms counter) at which the last good LTC
    /// frame was decoded -- the frame boundary of the incoming signal.
    /// 0 if nothing has been decoded yet.
    double getLastFrameArrivalMs() const
    {
        return lastFrameTime.load(std::memory_order_relaxed);
    }

    /// Binary group flags of the last decoded frame (BGF0 in bit 0).
    uint8_t getBinaryGroupFlags() const { return binaryGroupFlags.load(std::memory_order_relaxed); }

    /// One decoded frame as the audio thread published it: the value read,
    /// when it arrived, the user bits and flags it carried, and how the
    /// source was moving.  The getters above read one field each; a frame
    /// decoded between two of them pairs a new value with an old arrival, or
    /// one frame's user bits with another's flags.  This one reads them as
    /// one record (AUDIT LTC-8).
    struct DecodedFrame
    {
        Timecode tc;                   // the frame read (DESIGN D19)
        double   arrivalMs = 0.0;      // its end on the wire; 0 = nothing decoded yet
        uint32_t userBits = 0;
        uint8_t  binaryGroupFlags = 0;
        double   periodMs = 0.0;       // the source's own frame length, last measured; 0 = not yet
        int      repeats = 0;          // frames in a row that repeated the value before them (0-2)
    };

    /// Any thread.  A sequence lock: the writer (the audio thread, once per
    /// frame) never waits; a reader that overlaps a write reads again.
    DecodedFrame getLastFrame() const
    {
        DecodedFrame f;
        for (;;)
        {
            const uint32_t before = frameSeq.load(std::memory_order_acquire);
            if ((before & 1u) != 0)
            {
                juce::Thread::yield();   // a write is in progress
                continue;
            }
            f.tc               = unpackTimecode(packedTimecode.load(std::memory_order_relaxed));
            f.arrivalMs        = lastFrameTime.load(std::memory_order_relaxed);
            f.userBits         = userBitsIn.load(std::memory_order_relaxed);
            f.binaryGroupFlags = binaryGroupFlags.load(std::memory_order_relaxed);
            f.periodMs         = framePeriodMs.load(std::memory_order_relaxed);
            f.repeats          = frameRepeats.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (frameSeq.load(std::memory_order_relaxed) == before)
                return f;
        }
    }

    /// What the engine publishes from this input: the live value, its phase
    /// inside the frame, and whether the source counts as present -- all
    /// from one snapshot and one clock reading (AUDIT LTC-10; the principle
    /// of DESIGN D29, there for MTC).
    struct LiveTimecode
    {
        Timecode tc;
        double   phaseMs = 0.0;
        bool     phaseValid = false;   // false until a frame has been decoded
        bool     receiving = false;
    };

    /// `atMs` is the hi-res ms counter, `fps` the engine's rate.  Any thread.
    ///
    /// The decoder delivers a frame when its sync word has gone by, which is
    /// the instant the next frame begins: the live value at the arrival is
    /// the decoded one plus one (DESIGN D19).  The engine reads it up to a
    /// device period plus the input latency later, and with a buffer longer
    /// than a frame that is several frames on: the value is aged by the
    /// whole frames elapsed since the arrival and the phase is what remains,
    /// one measurement expressed two ways.  Before, the value was the
    /// decoded one plus one whatever its age, and the phase an fmod of the
    /// same age -- the pair named the wrong frame on a third of the ticks at
    /// 512 samples and on all of them from 2048 with a period of latency.
    ///
    /// The frames counted are the source's own.  Their length is the one
    /// measured on the sample clock, which under varispeed is not the nominal
    /// one (at 0.92 or 1.08, aging by the nominal length put the value a
    /// frame off on two thirds of the ticks at 8192 samples with a period of
    /// latency).  However long it is, it is what 80 whole bits of the source
    /// took, so a slow source is aged at its own speed.  It is never taken as
    /// shorter than half the nominal: a source above twice the speed is aged
    /// at twice, and reads behind rather than ahead.  The phase is that
    /// frame's fraction in nominal milliseconds, the source time LtcOutput
    /// ages it in.  And only a source that advances is aged.  One that has
    /// sent the same value three times running is holding a static frame: it
    /// reads as that frame plus one at any age, as DESIGN D19 and BENCH B27
    /// say, where aging made it a sawtooth up to eleven frames high.  A
    /// single repeat is a running source that lost a frame, and is aged like
    /// any other.
    ///
    /// The value is aged for as long as the source counts as receiving: the
    /// freewheel (AUDIT D10) on top of the device period and the input
    /// latency.  Through a dropout shorter than that it counts on at the
    /// source's last speed, as MtcInput's does, and carries on where the
    /// source does when the frames come back.  The price is at a real stop:
    /// the value runs on through the whole window and then, once the source
    /// no longer counts as receiving, steps back to the last frame read plus
    /// one.  At 30 fps that is 4 or 5 frames at the default 150 ms with 512
    /// samples, 14 at 8192 samples with a period of latency, 60 to 70 with a
    /// 2 s freewheel.  A source that slows down before it stops (a brake) is
    /// aged at the last frame's length, which is shorter than the next one:
    /// at 8192 samples with a period of latency the value can run a frame
    /// ahead and back while it slows.
    ///
    /// While not receiving the value is the last one read plus one and the
    /// phase 0, as MtcInput does: the source has stopped.
    LiveTimecode getLiveTimecode(double atMs, FrameRate fps) const
    {
        LiveTimecode live;
        const DecodedFrame f = getLastFrame();
        const double window = timeoutMs.load(std::memory_order_relaxed)
                            + deliveryMs.load(std::memory_order_relaxed);
        live.receiving = (atMs - f.arrivalMs) < window;   // isReceivingAt() on this snapshot

        int64_t whole = 0;
        if (f.arrivalMs > 0.0)
        {
            live.phaseValid = true;
            const double nominalMs = 1000.0 / frameRateToDouble(fps);
            if (live.receiving)
            {
                const double frameMs = (f.periodMs > 0.0)
                                     ? juce::jmax(nominalMs * 0.5, f.periodMs)
                                     : nominalMs;
                const double frames = (atMs - f.arrivalMs) / frameMs;
                const double fl = std::floor(frames);
                live.phaseMs = (frames - fl) * nominalMs;
                if (f.repeats < kHeldRepeats)
                    whole = (int64_t) fl;
            }
        }
        live.tc = offsetTimecode(f.tc, (int) (1 + whole), fps);
        return live;
    }

    /// Freewheel (AUDIT D10): how long a frame may be missing before the
    /// source stops counting as present, counted from when the device could
    /// have delivered it (see isReceivingAt).  The live value counts on
    /// through it (getLiveTimecode), and the senders with it, so a short
    /// dropout -- a USB stall, a display wake -- never reaches the wire; the
    /// price is that a real stop takes this long to reach the outputs, which
    /// run on through it and then step back to where the source stopped.
    /// The operator sets it (engine setting), default kSourceTimeoutMs.
    void setTimeoutMs(double ms) { timeoutMs.store(juce::jmax(50.0, ms), std::memory_order_relaxed); }
    double getTimeoutMs() const  { return timeoutMs.load(std::memory_order_relaxed); }

    bool isReceiving() const { return isReceivingAt(nowMs()); }

    /// Whether the source counts as present at `atMs` (hi-res ms counter).
    /// Any thread.
    ///
    /// The arrival stamp is the instant the frame crossed the converter:
    /// dated back from the callback by the samples still to process and the
    /// input latency.  The engine only learns of a frame when the callback
    /// carrying it runs, so between two healthy callbacks the stamp ages by
    /// up to one frame (waiting for the next one to end) plus one device
    /// period plus the input latency.  Compared with the freewheel alone, at
    /// 8192 samples (171 ms at 48 kHz) the source read as lost for part of
    /// every period, or all of it when the driver reports a period of
    /// latency -- and each flip paused the outputs, re-seeded LTC OUT and
    /// sent an MTC Full Frame (AUDIT LTC-9).  The window is therefore the
    /// freewheel PLUS the device period and the input latency: never shorter
    /// than what a healthy device takes to hand a frame over, and the
    /// freewheel keeps its meaning at every buffer size.  The margin over the
    /// healthy worst case is the freewheel itself less one frame: at least
    /// 8 ms at the 50 ms minimum and 24 fps (a frame is 41.7 ms), 108 ms at
    /// the default -- callback jitter, or a frame or two the decoder could
    /// not read.
    bool isReceivingAt(double atMs) const
    {
        const double window = timeoutMs.load(std::memory_order_relaxed)
                            + deliveryMs.load(std::memory_order_relaxed);
        return (atMs - lastFrameTime.load(std::memory_order_relaxed)) < window;
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

        // The producer resets both positions when its device (re)starts
        // (resetPassthruBuffer); if this consumer runs on another device it
        // may be mid-read with a stale rp, and "available" then wraps to a
        // huge count that would replay the whole ring for the next 2^32
        // samples.  More than the ring can hold is not a state the ring can
        // be in: resynchronise to the producer and carry on.
        if (available > RING_SIZE)
        {
            rp = wp;
            available = 0;
        }

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
    juce::String currentDeviceName;
    juce::String currentTypeName;
    std::atomic<bool> isRunningFlag { false };
    std::atomic<double> timeoutMs { kSourceTimeoutMs };   // freewheel window (D10)
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
    // LTC user bits recovered from the incoming stream (32-bit binary
    // groups; 0 until the first good frame).  Written by the audio thread
    // once per decoded frame, read by the UI for display and by the FROM LTC
    // IN user-bits mode.
    std::atomic<uint32_t> userBitsIn { 0 };
    std::atomic<FrameRate> detectedFps { FrameRate::FPS_25 };
    std::atomic<double> lastFrameTime  { 0.0 };
    std::atomic<uint8_t> binaryGroupFlags { 0 };   // BGF0 | BGF1<<1 | BGF2<<2 of the last frame
    // Sequence lock over packedTimecode, lastFrameTime, userBitsIn,
    // binaryGroupFlags, framePeriodMs and frameRepeats: odd while the audio
    // thread is storing a frame (getLastFrame).
    std::atomic<uint32_t> frameSeq { 0 };
    std::atomic<double> framePeriodMs { 0.0 };   // DecodedFrame::periodMs
    std::atomic<int> frameRepeats { 0 };         // DecodedFrame::repeats
    // Repeats after which a value counts as a static frame and is no longer
    // aged (getLiveTimecode).
    static constexpr int kHeldRepeats = 2;
    double decodeCallbackStartMs = 0.0;   // audio thread only: timestamp base for lastFrameTime
    int    decodeSamplesRemaining = 0;    // audio thread only: samples after the one being decoded
    double inputLatencyMs = 0.0;          // set in audioDeviceAboutToStart
    // Device period plus input latency, ms: how long after crossing the
    // converter a frame can reach the engine (see isReceivingAt).  Set when
    // the device starts; raised by the callback if the driver hands over
    // more samples than the buffer it announced.
    std::atomic<double> deliveryMs { 0.0 };

    /// Wall clock for the arrival stamps and the receive window.  Injectable
    /// so the decoder can be simulated against a device clock (tools/audit),
    /// as LtcOutput's is; production uses the high-resolution counter.
    std::function<double()> timeSource;
    double nowMs() const { return timeSource ? timeSource() : juce::Time::getMillisecondCounterHiRes(); }

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
    static constexpr int LTC_FRAME_BITS = 80;
    int bitsSinceSync = 0;          // bits since the last sync word (or the reset); 81 = frame broken
    double samplesSinceLastSync = 0.0;
    int consecutiveGoodFrames = 0;
    FrameRate candidateFps = FrameRate::FPS_25;   // rate the consecutive count refers to
    bool   lastSyncClosedFrame = false;   // the last sync word ended a whole, valid frame
    double sourcePeriodMs = 0.0;          // the source's frame length, last measured; 0 = not yet
    int    valueRepeats = 0;              // frames in a row that repeated the value before them (0-2)

    void resetDecoder()
    {
        signalHigh = false;
        samplesSinceEdge = 0;
        halfBitPending = false;
        firstEdgeAfterReset = true;
        shiftRegLow = 0;
        shiftRegHigh = 0;
        bitsSinceSync = 0;
        samplesSinceLastSync = 0.0;
        consecutiveGoodFrames = 0;
        // Initial bit period estimate: use ~27fps midpoint (2160 transitions/sec)
        // to minimize convergence time across all frame rates (24-30fps)
        bitPeriodEstimate = currentSampleRate / 2160.0;
        lastSyncClosedFrame = false;
        sourcePeriodMs = 0.0;
        valueRepeats = 0;
    }

    void pushBit(int bit)
    {
        shiftRegLow  = (shiftRegLow >> 1) | (static_cast<uint64_t>(shiftRegHigh & 1) << 63);
        shiftRegHigh = static_cast<uint16_t>((shiftRegHigh >> 1) | ((bit & 1) << 15));
        if (bitsSinceSync <= LTC_FRAME_BITS)
            ++bitsSinceSync;   // saturates past 80: "not a whole frame"
        if (shiftRegHigh == LTC_SYNC_WORD)
        {
            // A frame is exactly 80 bits, sync word included.  Anything else
            // between two sync words -- decoding that began part-way through
            // a frame, bits lost to a dropout or a jump in the stream, extra
            // ones from noise -- leaves bits of another frame, or zeros, in
            // the register, and it still passes the range check: it decoded
            // as 01:23:40.00 or 00:00:00.00 where 01:23:45.13 was on the
            // wire, and that frame alone made the source "receiving" and the
            // outputs snap to it and back (AUDIT LTC-12).  Such a frame is
            // dropped, and so is one in which an edge came at an interval no
            // bit has (onEdgeDetected); the sync word itself is real, so the
            // period to the next one still measures the rate.
            const bool whole = (bitsSinceSync == LTC_FRAME_BITS);
            bitsSinceSync = 0;
            if (whole)
                onSyncWordDetected();
            else
            {
                consecutiveGoodFrames = 0;
                samplesSinceLastSync = 0.0;
                lastSyncClosedFrame = false;
            }
        }
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
            lastSyncClosedFrame = false;
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
            // NOTE: LTC cannot distinguish 23.976fps from 24fps -- both use 80 bits
            // per frame with no drop-frame flag.  The ~0.1% rate difference is too
            // small to measure reliably from frame-to-frame period.  If 23.976 support
            // is needed, the user must manually select the frame rate.
            if (measuredFps < 24.5)       detected = FrameRate::FPS_24;
            else if (measuredFps < 27.0)  detected = FrameRate::FPS_25;
            else if (dropFrame)           detected = FrameRate::FPS_2997;
            else                          detected = FrameRate::FPS_30;

            // The rate is adopted only after three consecutive frames agree
            // on the SAME classification; before, three good frames of any
            // rate were enough and the value then followed every single
            // frame period, so one jittery period flipped the rate.
            if (detected == candidateFps)
                consecutiveGoodFrames++;
            else
            {
                candidateFps = detected;
                consecutiveGoodFrames = 1;
            }
            if (consecutiveGoodFrames >= 3)
                detectedFps.store(detected, std::memory_order_relaxed);
        }
        else
        {
            consecutiveGoodFrames = 0;
        }

        // The source's own frame length, for getLiveTimecode: the samples
        // between this sync word and the last one, when that one also ended
        // a whole frame -- exactly 80 of the source's bits, at whatever speed
        // it runs.  Kept until the next such measurement.
        if (lastSyncClosedFrame)
            sourcePeriodMs = samplesSinceLastSync * 1000.0 / currentSampleRate;
        lastSyncClosedFrame = true;
        samplesSinceLastSync = 0.0;

        // --- Binary group flags (12M-1 Table 2) ---
        // BGF1 is bit 58 at every rate; BGF0/BGF2 are bits 43/59 at 24 and
        // 30 fps and 27/43 at 25 fps.  Reported alongside the user bits so
        // the reader can decode what the sender declared (ST 309 date, eight-
        // bit characters).  The rate used is the adopted one.
        uint8_t bgf = 0;
        {
            const bool is25 = (detectedFps.load(std::memory_order_relaxed) == FrameRate::FPS_25);
            const int  b0 = is25 ? 27 : 43;
            const int  b2 = is25 ? 43 : 59;
            bgf = (uint8_t)(((d >> b0) & 1) | (((d >> 58) & 1) << 1) | (((d >> b2) & 1) << 2));
        }

        // --- User bits (SMPTE 12M binary groups) ---
        // Same layout as the encoder: eight 4-bit groups at bit offsets
        // 4,12,20,28,36,44,52,60, LSB-first within each group, group 1 as
        // the LEAST significant hex digit.  Reassembled into a 32-bit word
        // so a value written by an STC output reads back identically.
        // Only stored on good frames (we are past the range check above).
        uint32_t ub = 0;
        {
            static constexpr int kUserGroupStart[8] =
                { 4, 12, 20, 28, 36, 44, 52, 60 };
            for (int g = 0; g < 8; ++g)
            {
                const uint32_t nibble =
                    static_cast<uint32_t>((d >> kUserGroupStart[g]) & 0xF);
                ub |= nibble << (g * 4);
            }
        }

        // Arrival instant of the frame end, at sample precision: the
        // callback runs after the buffer was captured, so the sample being
        // processed occurred (samples still to process in this buffer)
        // earlier than the callback start, plus the device's input latency.
        // Stamping with the callback time put the frame boundary up to a
        // buffer late and made it jitter by a buffer, which the phase
        // published to the LTC encoder (D5) inherited.
        const double arrivalMs = decodeCallbackStartMs
                               - (double)(decodeSamplesRemaining) * 1000.0 / currentSampleRate
                               - inputLatencyMs;

        // Frames in a row that repeated the value before them.  A source
        // holding a static frame repeats every one; a running source that
        // lost a frame repeats one (getLiveTimecode tells them apart).
        const uint64_t packed = packTimecode(hours, minutes, seconds, frames);
        const bool repeated = lastFrameTime.load(std::memory_order_relaxed) > 0.0
                           && packed == packedTimecode.load(std::memory_order_relaxed);
        valueRepeats = repeated ? juce::jmin(valueRepeats + 1, kHeldRepeats) : 0;

        // Value, arrival, user bits, flags, period and repeats go out as one
        // record: the sequence is odd while they are being stored, and a
        // reader that saw it odd or changed reads again (getLastFrame, AUDIT
        // LTC-8).  Only this thread writes, so it never waits.
        const uint32_t seq = frameSeq.load(std::memory_order_relaxed);
        frameSeq.store(seq + 1u, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        packedTimecode.store(packed, std::memory_order_relaxed);
        lastFrameTime.store(arrivalMs, std::memory_order_relaxed);
        userBitsIn.store(ub, std::memory_order_relaxed);
        binaryGroupFlags.store(bgf, std::memory_order_relaxed);
        framePeriodMs.store(sourcePeriodMs, std::memory_order_relaxed);
        frameRepeats.store(valueRepeats, std::memory_order_relaxed);
        frameSeq.store(seq + 2u, std::memory_order_release);
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
            // No bit lasts this long or this short: the signal dropped out,
            // jumped or took a spike, and the frame in progress cannot come
            // out whole however many bits follow (see pushBit).
            halfBitPending = false;
            bitsSinceSync = LTC_FRAME_BITS + 1;
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

            // Reserve 1 sentinel slot to distinguish full from empty.
            // Need freeSlots >= 2 because 1 is the sentinel -- only (freeSlots-1)
            // are actually writable.  Effective ring capacity is RING_SIZE-1
            // (32767 samples ~ 682ms @48kHz), which is plenty for bridging
            // the latency between input and AudioThru output callbacks.
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
        decodeCallbackStartMs = nowMs();

        // The receive window must cover what this device actually hands
        // over (isReceivingAt): a driver that delivers more than it
        // announced widens it.
        const double deliveryNowMs = (double) numSamples * 1000.0 / currentSampleRate + inputLatencyMs;
        if (deliveryNowMs > deliveryMs.load(std::memory_order_relaxed))
            deliveryMs.store(deliveryNowMs, std::memory_order_relaxed);

        for (int i = 0; i < numSamples; ++i)
        {
            decodeSamplesRemaining = numSamples - i;
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
            // The input latency the driver reports dates every arrival and
            // widens the receive window (isReceivingAt).  It is not clamped
            // the way LtcOutput clamps its output latency (0-100 ms, DESIGN
            // D6): a driver at 8192 samples can truly report a period of it
            // (171 ms at 48 kHz), and a 100 ms cap would date every arrival
            // there 71 ms late, two or three frames.  A driver that
            // over-reports dates them early by its error instead.
            inputLatencyMs = (double)device->getInputLatencyInSamples() * 1000.0 / currentSampleRate;
            deliveryMs.store((double) currentBufferSize * 1000.0 / currentSampleRate + inputLatencyMs,
                             std::memory_order_relaxed);
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
