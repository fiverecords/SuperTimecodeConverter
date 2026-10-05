// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

#pragma once
#include <JuceHeader.h>
#include "TimecodeCore.h"
#include <atomic>

class MtcInput : public juce::MidiInputCallback
{
public:
    MtcInput() = default;

    ~MtcInput() override
    {
        // stop() stops the device, so nothing calls this object once it
        // returns.  The retired devices are then destroyed with the members,
        // at once: there is no later tick to defer them to (see stop()).
        stop();
    }

    //==============================================================================
    juce::StringArray getDeviceNames() const
    {
        juce::StringArray names;
        for (auto& d : availableDevices)
            names.add(d.name);
        return names;
    }

    int getDeviceCount() const { return availableDevices.size(); }

    juce::String getCurrentDeviceName() const
    {
        if (currentDeviceIndex >= 0 && currentDeviceIndex < availableDevices.size())
            return availableDevices[currentDeviceIndex].name;
        return "None";
    }

    void refreshDeviceList()
    {
        availableDevices = juce::MidiInput::getAvailableDevices();
    }

    //==============================================================================
    bool start(int deviceIndex)
    {
        stop();

        if (deviceIndex < 0 || deviceIndex >= availableDevices.size())
            return false;

        auto device = juce::MidiInput::openDevice(availableDevices[deviceIndex].identifier, this);

        if (device != nullptr)
        {
            // Clear the decoder BEFORE the device delivers anything.  A
            // MidiInput calls back only between its start() and stop() (JUCE
            // drops what arrives while it is not started), and the previous
            // device was stopped by stop() above, so nothing runs
            // handleIncomingMidiMessage while resetState() writes the
            // MIDI-thread state; the release store and the device's start()
            // publish it to the MIDI thread.
            resetState();
            midiInput = std::move(device);
            currentDeviceIndex = deviceIndex;
            isRunningFlag.store(true, std::memory_order_release);
            midiInput->start();
            return true;
        }

        return false;
    }

    void stop()
    {
        isRunningFlag.store(false, std::memory_order_release);
        if (midiInput != nullptr)
        {
            // Stop the device, then retire it -- do NOT destroy it here.
            //
            // Stopping is safe and is what makes a restart clean: in JUCE 9,
            // MidiInput::stop() only clears the input's "active" flag under
            // the spin lock its dispatcher holds (try-lock) while it calls us,
            // so when it returns no callback from this device is in flight and
            // none will start.  Without it a retired device kept delivering
            // into this object until it was destroyed, and a start() right
            // after (a device change) set isRunningFlag again and let those
            // messages in, from another device and possibly another thread,
            // while resetState() and the new device's messages ran.
            //
            // Destroying is what crashed on macOS (1.8.0): CoreMIDI's
            // MIDIPortDisconnectSource does not wait for an in-flight read
            // callback, and the platform MIDI thread could still be inside
            // JUCE's UMP dispatcher.  So the device goes to a retirement list
            // and is destroyed by drainRetiredDevices() on a later tick.
            midiInput->stop();
            retiredDevices.push_back(std::move(midiInput));
        }
        currentDeviceIndex = -1;
    }

    /// Call periodically from the message thread (the engine's tick, 60 Hz)
    /// to destroy the MidiInput devices retired by stop().  They are already
    /// stopped; deferring their destruction to the next tick gives an
    /// in-flight platform MIDI callback time to return.  The next tick
    /// normally comes within ~16 ms of stop() (a 60 Hz juce::Timer); it can
    /// come much sooner, since stop() runs from UI handlers between ticks,
    /// or later if the message thread is busy.
    void drainRetiredDevices()
    {
        if (!retiredDevices.empty())
            retiredDevices.clear();
    }

    bool getIsRunning() const { return isRunningFlag.load(std::memory_order_relaxed); }

    //==============================================================================
    // True if QF messages are actively arriving
    /// Freewheel (D10): how long after the last frame/packet the source still
    /// counts as present.  The senders count on their own through it, so a
    /// short dropout -- a USB stall, a display wake -- never reaches the
    /// wire; the price is that a real stop takes this long to reach the
    /// outputs.  The operator sets it (engine setting), default
    /// kSourceTimeoutMs.
    void setTimeoutMs(double ms) { timeoutMs.store(juce::jmax(50.0, ms), std::memory_order_relaxed); }
    double getTimeoutMs() const  { return timeoutMs.load(std::memory_order_relaxed); }

    bool isReceiving() const
    {
        if (!synced.load(std::memory_order_acquire))
            return false;

        bool locate;
        double locateUntil;
        {
            const juce::SpinLock::ScopedLockType lock(tcLock);
            locate = syncIsLocate;
            locateUntil = locatePresentUntilMs;
        }
        return presentAt(juce::Time::getMillisecondCounterHiRes(), locate, locateUntil);
    }

    /// Wall-clock instant (hi-res ms counter) of the current sync point: for
    /// a quarter-frame sequence, the start of the frame it stands for (a
    /// quarter frame after piece 7 arrived, see reconstructAndSync); for a
    /// Full Frame, its arrival -- or, if the transport was parked on it, the
    /// arrival of the quarter frame that starts it running.  0 if nothing
    /// has been decoded yet.  The engine no longer uses it (it takes value
    /// and phase together from getCurrentTimecode(nowMs, phase), DESIGN D29).
    double getLastFrameArrivalMs() const
    {
        const juce::SpinLock::ScopedLockType lock(tcLock);
        return syncTimeMs;
    }

    Timecode getCurrentTimecode() const
    {
        double phaseIgnored = 0.0;
        return getCurrentTimecode(juce::Time::getMillisecondCounterHiRes(), phaseIgnored);
    }

    /// The live value at `nowMs`, and how far into that frame the source is
    /// (`phaseMsOut`, 0 .. one frame).  Both come from ONE elapsed against ONE
    /// sync point, read under one lock -- that is the whole point of this
    /// overload (D29, issue #22).
    ///
    /// The sync point is dated a quarter frame into the future on purpose
    /// (piece 7 arrives at N + 1.75 frames; the value N + 2 begins a quarter
    /// frame later), so for the first 10 ms after every sequence `elapsed` is
    /// NEGATIVE.  The old code returned the sync value outright for a negative
    /// elapsed, while the engine took the phase with an fmod and wrapped the
    /// negative remainder up by a frame: value N + 2, phase 30 ms, when the
    /// truth was N + 1 at 30 ms.  A pair one frame high for a quarter frame
    /// out of every two, sampled by a 60 Hz tick more often than not, and the
    /// LTC encoder's tracking let one through now and then as a skipped frame
    /// with a repeat to come back.  floor() on the same number gives N + 1 at
    /// 30 ms, which is where the stream is.
    ///
    /// A Full Frame is a locate (AUDIT LTC-13).  Received while the source
    /// is absent -- a sender locating while stopped -- it does not make it
    /// present: the value stays frozen on the located frame, and the first
    /// quarter frame starts it running from that quarter frame's arrival
    /// (resumeFromLocate) -- unless it is a piece 0 that names none of the
    /// located frame and the next two: then the source stays absent until
    /// the first complete sequence.  Received while the source is present,
    /// it runs from its arrival like any sync point, but keeps the source
    /// present for two frames only unless quarter frames follow (presentAt).
    /// Taken as a source, a locate while stopped counted up through the
    /// freewheel window and jumped back when it closed, and a locate
    /// followed by play jumped ahead by the time the transport had been
    /// parked.
    Timecode getCurrentTimecode(double nowMs, double& phaseMsOut) const
    {
        phaseMsOut = 0.0;
        if (!synced.load(std::memory_order_acquire))
            return Timecode();

        Timecode syncTc;
        double syncMs;
        FrameRate fps;
        bool locate;
        double locateUntil;
        {
            const juce::SpinLock::ScopedLockType lock(tcLock);
            syncTc = lastSyncTimecode;
            syncMs = syncTimeMs;
            fps = detectedFps;
            locate = syncIsLocate;
            locateUntil = locatePresentUntilMs;
        }

        // Presence from the same snapshot and the same clock reading: the
        // quarter frame that ends a locate moves the sync point and clears
        // the locate in one go.
        if (!presentAt(nowMs, locate, locateUntil))
            return syncTc;   // frozen: the last value, no phase to speak of

        const double msPerFrame = 1000.0 / frameRateToDouble(fps);
        const double elapsed = nowMs - syncMs;

        // Interpolate from the last sync point on the drop-frame-aware frame
        // index, so landing across a 59;29 -> 00;02 crossing yields the next
        // valid address instead of a patched one.  floor(), not truncation:
        // a negative elapsed is the frame BEFORE the sync value, part-way
        // through, and the remainder must say so.
        const double whole = std::floor(elapsed / msPerFrame);
        phaseMsOut = elapsed - whole * msPerFrame;
        return frameIndexToTimecode(timecodeToFrameIndex(syncTc, fps) + (int64_t) whole, fps);
    }

    FrameRate getDetectedFrameRate() const
    {
        const juce::SpinLock::ScopedLockType lock(tcLock);
        return detectedFps;
    }

    //==============================================================================
    void handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage& message) override
    {
        // Guard: stop() clears the flag before it stops the device, so a
        // message already being dispatched in between is ignored.
        if (!isRunningFlag.load(std::memory_order_acquire)) return;

        auto rawData = message.getRawData();
        int rawSize = message.getRawDataSize();

        if (rawSize >= 2 && rawData[0] == 0xF1)
        {
            const double now = juce::Time::getMillisecondCounterHiRes();
            lastQfReceiveTime.store(now, std::memory_order_relaxed);

            int dataByte = rawData[1];
            int index = (dataByte >> 4) & 0x07;  // 0-7 guaranteed by mask
            int value = dataByte & 0x0F;

            mtcData[index] = value;
            nibbleMask |= (uint8_t)(1 << index);

            // The first quarter frame after a Full Frame, unless that one
            // already contradicted it (then the locate ends with the first
            // complete sequence, in reconstructAndSync).  (syncIsLocate is
            // written only on this thread while a device is started, so this
            // thread reads it without the lock.)
            if (syncIsLocate && !locateContradicted)
                resumeFromLocate(index, value, now);

            if (index == 7)
                reconstructAndSync();
        }
        else if (message.isSysEx())
        {
            auto* sysex = message.getSysExData();
            int sysexSize = message.getSysExDataSize();

            if (sysexSize >= 8 &&
                sysex[0] == 0x7F &&                     // Universal Real Time
                                                         // sysex[1] = device ID (0x00-0x7F, accept any)
                sysex[2] == 0x01 && sysex[3] == 0x01)   // MTC Full Frame
            {
                int hr = sysex[4];
                int mn = sysex[5];
                int sc = sysex[6];
                int fr = sysex[7];

                int rateCode = (hr >> 5) & 0x03;
                hr &= 0x1F;

                // Same range sanity as the quarter-frame path: a malformed
                // Full Frame must not become the sync point.
                if (hr > 23 || mn > 59 || sc > 59 || fr > 29)
                    return;

                // A Full Frame message is an explicit locate (AUDIT LTC-13).
                // It is not a quarter frame: it does not refresh
                // lastQfReceiveTime, and until a quarter frame follows
                // (syncIsLocate) the source is present only until
                // locatePresentUntilMs.  Received while the source is absent
                // -- a locate while stopped -- that is never: the value stays
                // frozen on the located frame and the outputs paused until
                // the transport plays.  Received while present, it runs from
                // here like any sync point for kLocateGraceFrames, or the
                // freewheel if shorter: a sender playing through it resumes
                // the quarter frames in that time, and one that has stopped
                // and located stops the outputs at the located frame instead
                // of freewheeling past it.
                const double now = juce::Time::getMillisecondCounterHiRes();
                const bool wasPresent = isReceiving();
                {
                    const juce::SpinLock::ScopedLockType lock(tcLock);
                    updateDetectedFps(rateCode);

                    lastSyncTimecode.hours   = hr;
                    lastSyncTimecode.minutes = mn;
                    lastSyncTimecode.seconds = sc;
                    lastSyncTimecode.frames  = fr;
                    syncTimeMs = now;
                    syncIsLocate = true;
                    locatePresentUntilMs = wasPresent
                        ? now + juce::jmin(kLocateGraceFrames * 1000.0 / frameRateToDouble(detectedFps),
                                           timeoutMs.load(std::memory_order_relaxed))
                        : 0.0;
                }
                // Drop the quarter-frame continuity anchor so the next
                // assembled sequence is accepted at face value wherever it
                // lands.
                continuityValid = false;
                pendingValid    = false;
                nibbleMask      = 0;
                locateContradicted = false;

                synced.store(true, std::memory_order_release);
            }
        }
    }

private:
    /// How long a Full Frame keeps a present source present without quarter
    /// frames.  Pico-Timecode sends Full Frames during play and resumes the
    /// quarter frames 1.14-1.17 frames later (the #16 captures, BENCH B5);
    /// two frames leave room for that and for MIDI jitter.
    static constexpr double kLocateGraceFrames = 2.0;

    /// Whether the source is present at `now`.  While a Full Frame waits for
    /// quarter frames (`locate`), until `locateUntil` -- never, if it came
    /// while the source was absent (AUDIT LTC-13).  Otherwise, while the
    /// last quarter frame is within the freewheel window.
    bool presentAt(double now, bool locate, double locateUntil) const
    {
        if (locate)
            return now < locateUntil;

        // MTC at 24fps sends QF every ~10.4ms, at 30fps ~8.3ms
        return now - lastQfReceiveTime.load(std::memory_order_relaxed)
             < timeoutMs.load(std::memory_order_relaxed);
    }

    /// MIDI thread: the first quarter frame after a Full Frame.  If it came
    /// within the Full Frame's grace, the sender played on through it and
    /// its value has been running since it arrived: nothing to change.
    /// Otherwise the transport was parked on the located frame and starts
    /// running now, at this quarter frame's arrival.  Piece 0 is sent at the
    /// start of the frame whose low nibble it carries, so when the first
    /// piece is a 0 naming the located frame or one of the next two, that
    /// frame is the value here (Pico-Timecode resumes with piece 0 of the
    /// frame after the one its Full Frame named); any other piece starts
    /// from the located frame.  The first complete sequence replaces the
    /// sync point as usual.
    ///
    /// A piece 0 naming none of those three contradicts the Full Frame: the
    /// stream is somewhere else (Pico-Timecode opens mtc_25fps_even with a
    /// Full Frame of 00:59:30:09 and then streams 00:59:33:20).  The located
    /// value is then not started: the locate stays (syncIsLocate) and the
    /// source absent, frozen on that value, until the first complete
    /// sequence ends the locate in reconstructAndSync, about two frames
    /// later.  Within the grace nothing changes even then: the value and
    /// the outputs are already running, and that sequence corrects them, as
    /// before AUDIT LTC-13.
    void resumeFromLocate(int piece, int nibble, double nowMs)
    {
        const juce::SpinLock::ScopedLockType lock(tcLock);
        if (nowMs >= locatePresentUntilMs)
        {
            if (piece == 0)
            {
                const int64_t located = timecodeToFrameIndex(lastSyncTimecode, detectedFps);
                bool named = false;
                for (int k = 0; k <= 2 && !named; ++k)
                {
                    const Timecode tc = frameIndexToTimecode(located + k, detectedFps);
                    if ((tc.frames & 0x0F) == nibble)
                    {
                        lastSyncTimecode = tc;
                        named = true;
                    }
                }
                if (!named)
                {
                    locateContradicted = true;   // absent: locatePresentUntilMs <= nowMs
                    return;
                }
            }
            syncTimeMs = nowMs;
        }
        syncIsLocate = false;
    }

    void reconstructAndSync()
    {
        // --- Sequence integrity ---
        // Reconstruct only when all eight nibbles have arrived since the
        // last reconstruction.  Without this, a dropped or delayed quarter
        // frame silently mixes stale nibbles into the assembled value.
        // On failure keep accumulating (do NOT clear): the mask completes
        // naturally once a full sequence has passed through.
        if (nibbleMask != 0xFF)
            return;
        nibbleMask = 0;

        Timecode assembled;
        assembled.frames  = mtcData[0] | (mtcData[1] << 4);
        assembled.seconds = mtcData[2] | (mtcData[3] << 4);
        assembled.minutes = mtcData[4] | (mtcData[5] << 4);
        assembled.hours   = mtcData[6] | ((mtcData[7] & 0x01) << 4);

        int rateCode = (mtcData[7] >> 1) & 0x03;

        {
            const juce::SpinLock::ScopedLockType lock(tcLock);
            updateDetectedFps(rateCode);

            const int maxFrames = frameRateToInt(detectedFps);

            // Range sanity: a malformed sequence is dropped outright rather
            // than propagated as a position.
            if (assembled.frames  >= maxFrames || assembled.frames  < 0
                || assembled.seconds > 59 || assembled.seconds < 0
                || assembled.minutes > 59 || assembled.minutes < 0
                || assembled.hours   > 23 || assembled.hours   < 0)
            {
                continuityValid = false;
                return;
            }

            // --- Continuity guard (issue #16) ---
            // The eight quarter frames of one sequence span two frame
            // periods.  Some generators latch the timecode at the first
            // quarter frame and send the nibbles of that single value;
            // others emit each nibble from the live counter.  With the
            // latter, a sequence that straddles a second boundary carries
            // its low-order nibbles (frames, seconds -- sent first) from
            // before the boundary and its high-order nibbles (minutes,
            // hours) from after it, so the assembled value is a splice of
            // two different times.
            //
            // At 30fps this only happens when the first quarter frame falls
            // on an ODD frame: 30 is even, so even-aligned sequences pair
            // as (0,1)(2,3)...(28,29) and never cross a second boundary,
            // while odd-aligned ones pair as (1,2)...(29,0) and cross once
            // per second.  Once per minute that crossing is also a minute
            // rollover, and the splice reads e.g. minutes=01 with
            // seconds=59 -- roughly a minute out, for one sequence, which
            // is the reported glitch.  25fps alternates alignment every
            // second because 25 is odd, so it depends on generator phase.
            //
            // Consecutive sequences are two frames apart, so any assembled
            // value that is not within tolerance of the previous one plus
            // two frames is treated as suspect and replaced by the
            // continuity-derived value.  A genuine jump (a locate that
            // arrives without a Full Frame message) repeats consistently,
            // so a suspect value confirmed by the next sequence is accepted
            // -- costing at most one extra sequence of latency on a
            // quarter-frame-only locate.
            // Distances are measured on the drop-frame-aware frame index, so
            // a 29.97DF minute rollover reads as the two-frame advance it is.
            static constexpr int64_t kToleranceFrames = 4;

            if (continuityValid)
            {
                const Timecode expected = advanceTwoFrames(prevAssembled, detectedFps);
                const int64_t delta = frameDistance(assembled, expected, detectedFps);

                if (delta > kToleranceFrames || delta < -kToleranceFrames)
                {
                    bool confirmed = false;
                    if (pendingValid)
                    {
                        const Timecode pexp = advanceTwoFrames(pendingAssembled, detectedFps);
                        const int64_t pdelta = frameDistance(assembled, pexp, detectedFps);
                        confirmed = (pdelta <= kToleranceFrames && pdelta >= -kToleranceFrames);
                    }

                    if (!confirmed)
                    {
                        // Reject this sequence: hold the timeline together
                        // with the continuity-derived value and remember
                        // the rejected one in case the next sequence
                        // confirms it as a real jump.
                        pendingAssembled = assembled;
                        pendingValid     = true;
                        assembled        = expected;
                    }
                    else
                    {
                        pendingValid = false;
                    }
                }
                else
                {
                    pendingValid = false;
                }
            }

            prevAssembled   = assembled;
            continuityValid = true;

            // MTC quarter-frame messages describe the timecode from 2 frames
            // prior (8 QFs x 1/4 frame = 2 frames of latency).  Advancing by
            // two compensates so the reported position matches the present.
            // Uses incrementFrame so 29.97 drop-frame skips are honoured --
            // linear arithmetic here used to emit frames 00/01 of a
            // non-tenth minute, which do not exist in drop-frame numbering.
            // NOTE: this compensation assumes forward playback.  Reverse
            // operations may briefly show a +/-4 frame discrepancy until the
            // next full 8-QF cycle completes.
            lastSyncTimecode = advanceTwoFrames(assembled, detectedFps);

            // Piece 7 is sent at the start of the last quarter of the second
            // frame, so it arrives at N + 1.75 frames; the value N + 2 above
            // begins a quarter frame later.  The sync instant is that start,
            // so the phase published to the LTC encoder is not a quarter
            // frame early.
            syncTimeMs = juce::Time::getMillisecondCounterHiRes()
                       + 0.25 * 1000.0 / frameRateToDouble(detectedFps);

            // A complete sequence ends a locate its first quarter frame
            // contradicted (resumeFromLocate): presence is the freewheel
            // window's again.
            syncIsLocate = false;
        }
        locateContradicted = false;
        synced.store(true, std::memory_order_release);
    }

    static Timecode advanceTwoFrames(const Timecode& tc, FrameRate fps)
    {
        return incrementFrame(incrementFrame(tc, fps), fps);
    }

    void updateDetectedFps(int rateCode)
    {
        switch (rateCode)
        {
            case 0:
                // MTC rate code 0 means "24fps". SMPTE MTC has no code for
                // 23.976, so if the user has selected FPS_2398 we preserve
                // it rather than silently overwriting with FPS_24.
                if (detectedFps != FrameRate::FPS_2398)
                    detectedFps = FrameRate::FPS_24;
                break;
            case 1: detectedFps = FrameRate::FPS_25;   break;
            case 2: detectedFps = FrameRate::FPS_2997; break;
            case 3: detectedFps = FrameRate::FPS_30;   break;
            default: break;  // Unknown rate code: keep previous value
        }
    }

    void resetState()
    {
        for (int i = 0; i < 8; i++)
            mtcData[i] = 0;
        nibbleMask      = 0;
        continuityValid = false;
        pendingValid    = false;
        prevAssembled    = Timecode();
        pendingAssembled = Timecode();
        locateContradicted = false;
        synced.store(false, std::memory_order_relaxed);
        {
            const juce::SpinLock::ScopedLockType lock(tcLock);
            syncTimeMs = 0.0;
            lastSyncTimecode = Timecode();
            syncIsLocate = false;
            locatePresentUntilMs = 0.0;
        }
        lastQfReceiveTime.store(0.0, std::memory_order_relaxed);
    }

    std::unique_ptr<juce::MidiInput> midiInput;
    std::vector<std::unique_ptr<juce::MidiInput>> retiredDevices;  // deferred destruction (see stop())
    juce::Array<juce::MidiDeviceInfo> availableDevices;
    int currentDeviceIndex = -1;
    std::atomic<bool> isRunningFlag { false };
    std::atomic<double> timeoutMs { kSourceTimeoutMs };   // freewheel window (D10)

    // Quarter-frame accumulator -- MIDI-callback-thread-only (resetState()
    // writes it on the message thread, but only while no device is started)
    int mtcData[8] = {};
    uint8_t nibbleMask = 0;          // which pieces arrived since the last reconstruction

    // Quarter-frame continuity state -- MIDI-callback-thread-only, as above.
    // prevAssembled is the piece-0-time value of the last accepted sequence;
    // pendingAssembled holds a rejected value awaiting confirmation.
    Timecode prevAssembled;
    Timecode pendingAssembled;
    bool continuityValid = false;
    bool pendingValid    = false;
    bool locateContradicted = false;   // the locate's first quarter frame was a piece 0 naming another frame

    // Protected by tcLock (written from MIDI thread, read from UI thread)
    mutable juce::SpinLock tcLock;
    Timecode lastSyncTimecode;
    double syncTimeMs = 0.0;
    bool syncIsLocate = false;            // the sync point is a Full Frame not yet started by a quarter frame or replaced by a sequence
    double locatePresentUntilMs = 0.0;    // while syncIsLocate: present until then (0: absent)
    FrameRate detectedFps = FrameRate::FPS_25;

    // Atomic cross-thread fields
    std::atomic<double> lastQfReceiveTime { 0.0 };
    std::atomic<bool> synced { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MtcInput)
};
