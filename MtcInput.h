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
            midiInput = std::move(device);
            midiInput->start();
            currentDeviceIndex = deviceIndex;
            isRunningFlag.store(true, std::memory_order_relaxed);
            resetState();
            return true;
        }

        return false;
    }

    void stop()
    {
        isRunningFlag.store(false, std::memory_order_release);
        if (midiInput != nullptr)
        {
            // Do NOT call midiInput->stop() or destroy it here.  On macOS,
            // CoreMIDI's MIDI thread may be mid-callback inside JUCE's UMP
            // dispatcher.  Both stop() and ~MidiInput() can deadlock or crash.
            //
            // Move the device to a retirement list.  It stays alive until
            // drainRetiredDevices() is called from the message thread timer
            // (16ms later), by which time any in-flight callback has returned.
            retiredDevices.push_back(std::move(midiInput));
        }
        currentDeviceIndex = -1;
    }

    /// Call periodically from the message thread (e.g. timerCallback at 60Hz)
    /// to safely destroy MidiInput devices that were retired by stop().
    /// By the time this runs (~16ms after stop()), CoreMIDI callbacks have
    /// finished and the destructors are safe to call.
    void drainRetiredDevices()
    {
        if (!retiredDevices.empty())
            retiredDevices.clear();
    }

    bool getIsRunning() const { return isRunningFlag.load(std::memory_order_relaxed); }

    //==============================================================================
    // True if QF messages are actively arriving
    bool isReceiving() const
    {
        if (!synced.load(std::memory_order_acquire))
            return false;

        double now = juce::Time::getMillisecondCounterHiRes();
        double elapsed = now - lastQfReceiveTime.load(std::memory_order_relaxed);

        // MTC at 24fps sends QF every ~10.4ms, at 30fps ~8.3ms
        return elapsed < kSourceTimeoutMs;
    }

    /// Wall-clock instant (hi-res ms counter) at which the last complete
    /// timecode was reconstructed.  MTC is a frame-based source, so this is
    /// the frame boundary as far as the rest of STC is concerned -- and it is
    /// far more precise than watching the value change from the 60Hz tick.
    /// 0 if nothing has been decoded yet.
    double getLastFrameArrivalMs() const
    {
        const juce::SpinLock::ScopedLockType lock(tcLock);
        return syncTimeMs;
    }

    Timecode getCurrentTimecode() const
    {
        if (!synced.load(std::memory_order_acquire))
            return Timecode();

        if (!isReceiving())
        {
            const juce::SpinLock::ScopedLockType lock(tcLock);
            return lastSyncTimecode; // Frozen
        }

        Timecode syncTc;
        double syncMs;
        FrameRate fps;
        {
            const juce::SpinLock::ScopedLockType lock(tcLock);
            syncTc = lastSyncTimecode;
            syncMs = syncTimeMs;
            fps = detectedFps;
        }

        double now = juce::Time::getMillisecondCounterHiRes();
        double elapsed = now - syncMs;

        if (elapsed < 0.0)
            return syncTc;

        double fpsDouble = frameRateToDouble(fps);
        int maxFrames = frameRateToInt(fps);
        double msPerFrame = 1000.0 / fpsDouble;

        // Linear interpolation from last sync point.
        // NOTE: for 29.97 DF, this uses simple frame counting (maxFrames per second)
        // rather than true DF-aware counting.  The DF correction at the end patches
        // any landing on skipped frame numbers 0-1.  This is exact for the typical
        // interpolation range (a few dozen frames between QF syncs), because DF skips
        // only occur at minute boundaries which are always >1798 frames apart.
        int extraFrames = (int)(elapsed / msPerFrame);

        int64_t syncTotal = (int64_t)syncTc.hours * 3600 * maxFrames
                          + (int64_t)syncTc.minutes * 60 * maxFrames
                          + (int64_t)syncTc.seconds * maxFrames
                          + (int64_t)syncTc.frames;

        int64_t currentTotal = syncTotal + extraFrames;

        // Wrap at 24h so interpolation across midnight stays valid
        int64_t dayFrames = (int64_t)24 * 3600 * maxFrames;
        currentTotal = ((currentTotal % dayFrames) + dayFrames) % dayFrames;

        Timecode result;
        result.frames  = (int)(currentTotal % maxFrames);
        result.seconds = (int)((currentTotal / maxFrames) % 60);
        result.minutes = (int)((currentTotal / (maxFrames * 60)) % 60);
        result.hours   = (int)((currentTotal / (maxFrames * 3600)) % 24);

        // Drop-frame correction: interpolation may land on frames 0/1 at the
        // start of a non-10th minute -- these frame numbers don't exist in DF
        if (fps == FrameRate::FPS_2997
            && result.frames < 2
            && result.seconds == 0
            && (result.minutes % 10) != 0)
        {
            result.frames = 2;
        }

        return result;
    }

    FrameRate getDetectedFrameRate() const
    {
        const juce::SpinLock::ScopedLockType lock(tcLock);
        return detectedFps;
    }

    //==============================================================================
    void handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage& message) override
    {
        // Guard: after stop(), the device may still deliver a queued message
        // before CoreMIDI fully disconnects.  Ignore it.
        if (!isRunningFlag.load(std::memory_order_acquire)) return;

        auto rawData = message.getRawData();
        int rawSize = message.getRawDataSize();

        if (rawSize >= 2 && rawData[0] == 0xF1)
        {
            lastQfReceiveTime.store(juce::Time::getMillisecondCounterHiRes(), std::memory_order_relaxed);

            int dataByte = rawData[1];
            int index = (dataByte >> 4) & 0x07;  // 0-7 guaranteed by mask
            int value = dataByte & 0x0F;

            mtcData[index] = value;
            nibbleMask |= (uint8_t)(1 << index);

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
                lastQfReceiveTime.store(juce::Time::getMillisecondCounterHiRes(), std::memory_order_relaxed);

                int hr = sysex[4];
                int mn = sysex[5];
                int sc = sysex[6];
                int fr = sysex[7];

                int rateCode = (hr >> 5) & 0x03;
                hr &= 0x1F;

                {
                    const juce::SpinLock::ScopedLockType lock(tcLock);
                    updateDetectedFps(rateCode);

                    lastSyncTimecode.hours   = hr;
                    lastSyncTimecode.minutes = mn;
                    lastSyncTimecode.seconds = sc;
                    lastSyncTimecode.frames  = fr;
                    syncTimeMs = juce::Time::getMillisecondCounterHiRes();
                }
                // A Full Frame message is an explicit locate: drop the
                // quarter-frame continuity anchor so the next assembled
                // sequence is accepted at face value wherever it lands.
                continuityValid = false;
                pendingValid    = false;
                nibbleMask      = 0;

                synced.store(true, std::memory_order_release);
            }
        }
    }

private:
    /// Linear frame index of a timecode, used only as a continuity metric.
    /// Deliberately ignores drop-frame skips: at a 29.97DF minute rollover
    /// this reads as a 4-frame advance instead of 2, which is inside the
    /// tolerance below.
    static int64_t linearFrames(const Timecode& t, int maxFrames)
    {
        return ((int64_t)t.hours * 3600 + (int64_t)t.minutes * 60
                + (int64_t)t.seconds) * maxFrames + (int64_t)t.frames;
    }

    /// Shortest signed distance a - b on the 24h circle, in frames.
    static int64_t frameDelta(int64_t a, int64_t b, int maxFrames)
    {
        const int64_t day = (int64_t)24 * 3600 * maxFrames;
        int64_t d = ((a - b) % day + day) % day;
        if (d > day / 2) d -= day;
        return d;
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
            static constexpr int64_t kToleranceFrames = 4;
            const int64_t assembledLin = linearFrames(assembled, maxFrames);

            if (continuityValid)
            {
                const Timecode expected = advanceTwoFrames(prevAssembled, detectedFps);
                const int64_t delta = frameDelta(assembledLin,
                                                 linearFrames(expected, maxFrames),
                                                 maxFrames);

                if (delta > kToleranceFrames || delta < -kToleranceFrames)
                {
                    bool confirmed = false;
                    if (pendingValid)
                    {
                        const Timecode pexp = advanceTwoFrames(pendingAssembled, detectedFps);
                        const int64_t pdelta = frameDelta(assembledLin,
                                                          linearFrames(pexp, maxFrames),
                                                          maxFrames);
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

            syncTimeMs = juce::Time::getMillisecondCounterHiRes();
        }
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
        synced.store(false, std::memory_order_relaxed);
        {
            const juce::SpinLock::ScopedLockType lock(tcLock);
            syncTimeMs = 0.0;
            lastSyncTimecode = Timecode();
        }
        lastQfReceiveTime.store(0.0, std::memory_order_relaxed);
    }

    std::unique_ptr<juce::MidiInput> midiInput;
    std::vector<std::unique_ptr<juce::MidiInput>> retiredDevices;  // deferred destruction (see stop())
    juce::Array<juce::MidiDeviceInfo> availableDevices;
    int currentDeviceIndex = -1;
    std::atomic<bool> isRunningFlag { false };

    // Quarter-frame accumulator -- MIDI-callback-thread-only
    int mtcData[8] = {};
    uint8_t nibbleMask = 0;          // which pieces arrived since the last reconstruction

    // Quarter-frame continuity state -- MIDI-callback-thread-only.
    // prevAssembled is the piece-0-time value of the last accepted sequence;
    // pendingAssembled holds a rejected value awaiting confirmation.
    Timecode prevAssembled;
    Timecode pendingAssembled;
    bool continuityValid = false;
    bool pendingValid    = false;

    // Protected by tcLock (written from MIDI thread, read from UI thread)
    mutable juce::SpinLock tcLock;
    Timecode lastSyncTimecode;
    double syncTimeMs = 0.0;
    FrameRate detectedFps = FrameRate::FPS_25;

    // Atomic cross-thread fields
    std::atomic<double> lastQfReceiveTime { 0.0 };
    std::atomic<bool> synced { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MtcInput)
};
