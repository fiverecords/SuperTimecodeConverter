// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

#pragma once
#include <JuceHeader.h>
#include "TimecodeCore.h"
#include "MidiOutputHub.h"
#include <atomic>
#include <cstdlib>

class MtcOutput : public juce::HighResolutionTimer
{
public:
    MtcOutput() = default;

    ~MtcOutput() override
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
        availableDevices = juce::MidiOutput::getAvailableDevices();
    }

    //==============================================================================
    /// Name shown to another engine whose MTC is refused on this port
    /// ("IN USE BY <name>").  Message thread; kept current on rename.
    void setOwnerName(const juce::String& name) { ownerName = name; }

    /// After a failed start(): the name of the sender already streaming MTC on
    /// that port, or empty if the device simply could not be opened.
    const juce::String& getBlockedBy() const { return blockedBy; }

    //==============================================================================
    bool start(int deviceIndex)
    {
        stop();
        blockedBy.clear();

        if (deviceIndex < 0 || deviceIndex >= availableDevices.size())
            return false;

        // One port per device for the whole application (MidiOutputHub,
        // DESIGN D32).
        // MTC has no channel: a second MTC stream in the same cable cannot be
        // received, so the port refuses it and names the stream it carries.
        auto p = MidiOutputHub::get().acquire(availableDevices[deviceIndex]);
        if (p != nullptr && !p->claimMtc(this, [this] { return ownerName; }, blockedBy))
            p = nullptr;   // releases our reference; the other stream is untouched

        port = std::move(p);

        if (port != nullptr)
        {
            currentDeviceIndex = deviceIndex;
            isRunningFlag.store(true, std::memory_order_relaxed);
            paused.store(false, std::memory_order_relaxed);
            currentQFIndex.store(0, std::memory_order_relaxed);
            mtcSeeded = false;
            resyncRequested.store(false, std::memory_order_relaxed);

            // No Full Frame here: pendingTimecode may not hold the source's
            // value yet (the engine sets it on its next tick), and a
            // misleading 00:00:00.00 is worse than none.  Receivers lock from
            // the quarter frames within a sequence (2 frames); the next
            // resume (setPaused(false)) or seek (forceResync) sends one.
            // The quarter frames do start now, though, and the first cycle
            // is seeded from whatever pendingTimecode holds when its piece 0
            // goes out.

            lastQfSendTime.store(juce::Time::getMillisecondCounterHiRes(), std::memory_order_relaxed);
            updateTimerRate();
            return true;
        }

        return false;
    }

    void stop()
    {
        stopTimer();   // blocks until an in-flight callback returns
        if (port != nullptr)
        {
            port->releaseMtc(this);
            port = nullptr;   // the device closes when its last sender lets go
        }

        isRunningFlag.store(false, std::memory_order_relaxed);
        paused.store(false, std::memory_order_relaxed);
        currentDeviceIndex = -1;
    }

    bool getIsRunning() const { return isRunningFlag.load(std::memory_order_relaxed); }

    /// The port this output streams on (for TriggerOutput to share), or nullptr
    /// if not running.  The holder keeps the port open for as long as it holds
    /// it.  Sends go through Port::send, which serialises every thread on the
    /// port: juce::MidiOutput::sendMessageNow itself is NOT thread-safe
    /// (JUCE 8.0.11 onwards; see MidiOutputHub.h).
    MidiOutputHub::PortPtr getMidiOutputPtr() const { return port; }

    //==============================================================================
    // Called from UI thread - thread-safe via SpinLock
    void setTimecode(const Timecode& tc)
    {
        const juce::SpinLock::ScopedLockType lock(tcLock);
        pendingTimecode = tc;
    }

    // Called from UI thread.  startTimer() is internally serialised in JUCE's
    // HighResolutionTimer, so calling it from the message thread is safe.
    void setFrameRate(FrameRate fps)
    {
        auto prev = currentFps.load(std::memory_order_relaxed);
        if (prev != fps)
        {
            currentFps.store(fps, std::memory_order_relaxed);
            if (isRunningFlag.load(std::memory_order_relaxed) && !paused.load(std::memory_order_relaxed))
                updateTimerRate();
        }
    }

    void setPaused(bool shouldPause)
    {
        if (paused.load(std::memory_order_relaxed) == shouldPause)
            return;

        if (shouldPause)
        {
            paused.store(true, std::memory_order_relaxed);
            stopTimer();
        }
        else if (isRunningFlag.load(std::memory_order_relaxed))
        {
            stopTimer();   // the cycle state is the timer's: reset it with the timer stopped
            currentQFIndex.store(0, std::memory_order_relaxed);
            paused.store(false, std::memory_order_relaxed);
            mtcSeeded = false;
            resyncRequested.store(false, std::memory_order_relaxed);

            // Re-sync receivers after pause with a Full Frame message
            sendFullFrame();

            lastQfSendTime.store(juce::Time::getMillisecondCounterHiRes(), std::memory_order_relaxed);
            updateTimerRate();
        }
        else
        {
            paused.store(false, std::memory_order_relaxed);
        }
    }

    bool isPaused() const { return paused.load(std::memory_order_relaxed); }

    /// Force immediate Full Frame re-sync.
    /// Call on seek/hot cue/track change so receivers know the new position
    /// instantly instead of waiting 8 QFs (2 frames) to reconstruct it.
    /// Message thread, with the timer running.
    void forceResync()
    {
        if (!isRunningFlag.load(std::memory_order_relaxed)
            || paused.load(std::memory_order_relaxed))
            return;
        // Restart the QF cycle from the new position at the next quarter
        // frame.  The timer thread does the reset itself: the cycle state is
        // its own while it runs, and written from here mtcSeeded was a data
        // race and the index reset could be lost under the timer's
        // load-increment-store (AUDIT C5).
        resyncRequested.store(true, std::memory_order_release);
        sendFullFrame();
    }

    //==============================================================================
    void sendFullFrame()
    {
        if (port == nullptr)
            return;

        Timecode tc;
        {
            const juce::SpinLock::ScopedLockType lock(tcLock);
            tc = pendingTimecode;
        }

        // Single atomic read -- guarantees maxFrames and rateCode are consistent
        FrameRate fps = currentFps.load(std::memory_order_relaxed);

        // Validate ranges -- don't send corrupt data to MIDI devices
        int maxFrames = frameRateToInt(fps);
        if (tc.hours > 23 || tc.minutes > 59 || tc.seconds > 59 || tc.frames >= maxFrames)
            return;

        int rateCode = fpsToRateCode(fps);
        uint8_t hr = (uint8_t)((tc.hours & 0x1F) | (rateCode << 5));

        uint8_t sysex[] = {
            0xF0, 0x7F, 0x7F, 0x01, 0x01,
            hr,
            (uint8_t)tc.minutes,
            (uint8_t)tc.seconds,
            (uint8_t)tc.frames,
            0xF7
        };

        port->send(juce::MidiMessage(sysex, sizeof(sysex)));
    }

private:
    //==============================================================================
    // Runs on HighResolutionTimer thread (~1ms precision)
    void hiResTimerCallback() override
    {
        if (port == nullptr
            || paused.load(std::memory_order_relaxed))
        {
            stopTimer();   // Don't spin at 1000Hz when there's nothing to send
            return;
        }

        // Single atomic read -- guarantees QF interval and rate code are consistent
        FrameRate fps = currentFps.load(std::memory_order_relaxed);

        // Fractional accumulator: compare real elapsed time against ideal QF interval
        // to eliminate drift caused by integer-ms timer resolution
        double now = juce::Time::getMillisecondCounterHiRes();
        // MTC is a digital protocol -- always send QFs at nominal frame rate.
        // The timecode VALUES advance slower at low pitch (PLL handles that),
        // which produces repeated frames. This is correct and keeps receivers
        // in sync. Scaling the interval caused MA3 to lose lock at low pitch.
        double qfInterval = 1000.0 / (frameRateToDouble(fps) * 4.0);

        // Guard against sending too many QFs if the timer fires in a burst
        // (allow up to 2 catch-up QFs per callback to handle jitter)
        int sent = 0;
        double lastSend = lastQfSendTime.load(std::memory_order_relaxed);
        while ((now - lastSend) >= qfInterval && sent < 2)
        {
            // At QF index 0, determine the timecode for this entire 8-QF cycle:
            // the last cycle's value advanced by 2 frames (one QF cycle = 2
            // frame durations), steered toward pendingTimecode by the shared
            // tracking policy (trackPublishedValue: one frame at a time, a
            // snap only beyond kTrackingHardResync), so a frame of jitter in
            // the published value does not become a cycle of wrong data.
            // Same architectural pattern as the LTC encoder's auto-increment.
            if (resyncRequested.exchange(false, std::memory_order_acquire))
            {
                // forceResync(): restart the cycle, seeded from pendingTimecode
                currentQFIndex.store(0, std::memory_order_relaxed);
                mtcSeeded = false;
            }
            int qfIdx = currentQFIndex.load(std::memory_order_relaxed);
            if (qfIdx == 0)
            {
                Timecode pending;
                {
                    const juce::SpinLock::ScopedLockType lock(tcLock);
                    pending = pendingTimecode;
                }

                if (!mtcSeeded)
                {
                    cycleTimecode = pending;
                    mtcSeeded = true;
                }
                else
                {
                    // Auto-increment by 2 frames (1 QF cycle = 2 frame durations),
                    // then the shared tracking policy: a cycle of 1 or 3 frames
                    // when the source has drifted a frame (pitch, clock), a
                    // snap only on a real seek.  Quarter frames keep their
                    // nominal rate; scaling them made MA3 lose lock.
                    cycleTimecode = trackPublishedValue(
                        incrementFrame(incrementFrame(cycleTimecode, fps), fps), pending, 2, fps);
                }
            }

            sendQuarterFrame(qfIdx, fps);

            qfIdx++;
            if (qfIdx >= 8)
                qfIdx = 0;
            currentQFIndex.store(qfIdx, std::memory_order_relaxed);

            // Advance by ideal interval (not by 'now') to prevent cumulative drift
            lastSend += qfInterval;
            sent++;
        }
        lastQfSendTime.store(lastSend, std::memory_order_relaxed);

        // If we fell too far behind (>50ms), reset to avoid a burst of catch-up sends
        if ((now - lastSend) > 50.0)
            lastQfSendTime.store(now, std::memory_order_relaxed);
    }

    void sendQuarterFrame(int index, FrameRate fps)
    {
        // MTC spec: QF messages encode the timecode that was current
        // at the START of the 8-QF sequence (2 frames ago from receiver's perspective)
        // The receiver compensates internally.
        int value = 0;

        switch (index)
        {
            case 0: value = cycleTimecode.frames & 0x0F;          break;
            case 1: value = (cycleTimecode.frames >> 4) & 0x01;   break;
            case 2: value = cycleTimecode.seconds & 0x0F;         break;
            case 3: value = (cycleTimecode.seconds >> 4) & 0x03;  break;
            case 4: value = cycleTimecode.minutes & 0x0F;         break;
            case 5: value = (cycleTimecode.minutes >> 4) & 0x03;  break;
            case 6: value = cycleTimecode.hours & 0x0F;           break;
            case 7:
            {
                int rateCode = fpsToRateCode(fps);
                value = ((cycleTimecode.hours >> 4) & 0x01) | (rateCode << 1);
                break;
            }
        }

        uint8_t dataByte = (uint8_t)((index << 4) | (value & 0x0F));
        port->send(juce::MidiMessage(0xF1, (int)dataByte));
    }

    void updateTimerRate()
    {
        // Run timer at 1ms fixed rate -- the fractional accumulator in
        // hiResTimerCallback handles exact QF timing to avoid drift.
        // The cadence is anchored here, on the CPU clock, at start(), on
        // resume and on a rate change -- not at the source's frame boundary.
        // So the quarter frames' phase against the source is arbitrary, up
        // to a frame, for each run: LTC out locks to the source's phase
        // (DESIGN D24), MTC out does not (AUDIT LTC-14, deferred: it would
        // change the sender's timing on the wire).
        lastQfSendTime.store(juce::Time::getMillisecondCounterHiRes(), std::memory_order_relaxed);
        startTimer(1);
    }

    //==============================================================================
    // Set in start()/stop() on the message thread, always with the timer
    // stopped; the timer thread only reads it while it runs.
    MidiOutputHub::PortPtr port;
    juce::String ownerName;     // message thread
    juce::String blockedBy;     // message thread
    juce::Array<juce::MidiDeviceInfo> availableDevices;
    int currentDeviceIndex = -1;
    std::atomic<bool> isRunningFlag { false };
    std::atomic<bool> paused { false };

    juce::SpinLock tcLock;
    Timecode pendingTimecode;   // Written by UI thread, read under tcLock
    std::atomic<FrameRate> currentFps { FrameRate::FPS_25 };

    // The cycle state -- cycleTimecode, mtcSeeded, currentQFIndex -- belongs
    // to the timer thread while the timer runs.  The message thread writes it
    // only with the timer stopped (start(), setPaused(false): stopTimer()
    // returns after an in-flight callback, under JUCE's callback mutex), and
    // forceResync() asks the timer thread to reset it through
    // resyncRequested instead of writing it (AUDIT C5).
    Timecode cycleTimecode;     // snapshot taken at QF index 0, read through QF 1-7
    bool     mtcSeeded = false; // Auto-increment: false until first QF0 seeds cycleTimecode
    std::atomic<int> currentQFIndex { 0 };
    std::atomic<bool> resyncRequested { false };   // set by forceResync(), consumed by the timer
    std::atomic<double> lastQfSendTime { 0.0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MtcOutput)
};
