// Super Timecode Converter
// Copyright (c) 2026 Fiverecords — MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

#pragma once
#include <JuceHeader.h>
#include "TimecodeCore.h"
#include <atomic>

class MtcOutput : public juce::HighResolutionTimer
{
public:
    MtcOutput()
    {
        refreshDeviceList();
    }

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
    bool start(int deviceIndex)
    {
        stop();

        if (deviceIndex < 0 || deviceIndex >= availableDevices.size())
            return false;

        midiOutput = juce::MidiOutput::openDevice(availableDevices[deviceIndex].identifier);

        if (midiOutput != nullptr)
        {
            currentDeviceIndex = deviceIndex;
            isRunningFlag.store(true, std::memory_order_relaxed);
            paused.store(false, std::memory_order_relaxed);
            currentQFIndex.store(0, std::memory_order_relaxed);

            // Full Frame is sent after the first setTimecode() call populates
            // pendingTimecode -- avoids transmitting a misleading 00:00:00.00
            // to receivers before the real timecode is available.
            // Receivers will sync within 8 QFs (~2 frames) regardless.

            lastQfSendTime.store(juce::Time::getMillisecondCounterHiRes(), std::memory_order_relaxed);
            updateTimerRate();
            return true;
        }

        return false;
    }

    void stop()
    {
        stopTimer();
        midiOutput = nullptr;

        isRunningFlag.store(false, std::memory_order_relaxed);
        paused.store(false, std::memory_order_relaxed);
        currentDeviceIndex = -1;
    }

    bool getIsRunning() const { return isRunningFlag.load(std::memory_order_relaxed); }

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
            stopTimer();
            currentQFIndex.store(0, std::memory_order_relaxed);
            paused.store(false, std::memory_order_relaxed);

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

    //==============================================================================
    void sendFullFrame()
    {
        if (midiOutput == nullptr)
            return;

        Timecode tc;
        {
            const juce::SpinLock::ScopedLockType lock(tcLock);
            tc = pendingTimecode;
        }

        // Single atomic read — guarantees maxFrames and rateCode are consistent
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

        midiOutput->sendMessageNow(juce::MidiMessage(sysex, sizeof(sysex)));
    }

private:
    //==============================================================================
    // Runs on HighResolutionTimer thread (~1ms precision)
    void hiResTimerCallback() override
    {
        if (midiOutput == nullptr
            || paused.load(std::memory_order_relaxed))
            return;

        // Single atomic read — guarantees QF interval and rate code are consistent
        FrameRate fps = currentFps.load(std::memory_order_relaxed);

        // Fractional accumulator: compare real elapsed time against ideal QF interval
        // to eliminate drift caused by integer-ms timer resolution
        double now = juce::Time::getMillisecondCounterHiRes();
        double qfInterval = 1000.0 / (frameRateToDouble(fps) * 4.0);

        // Guard against sending too many QFs if the timer fires in a burst
        // (allow up to 2 catch-up QFs per callback to handle jitter)
        int sent = 0;
        double lastSend = lastQfSendTime.load(std::memory_order_relaxed);
        while ((now - lastSend) >= qfInterval && sent < 2)
        {
            // At QF index 0, snapshot the timecode for this entire 8-QF cycle
            // This guarantees all 8 QFs describe the SAME timecode - no jumps
            int qfIdx = currentQFIndex.load(std::memory_order_relaxed);
            if (qfIdx == 0)
            {
                const juce::SpinLock::ScopedLockType lock(tcLock);
                cycleTimecode = pendingTimecode;
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
        midiOutput->sendMessageNow(juce::MidiMessage(0xF1, (int)dataByte));
    }

    void updateTimerRate()
    {
        // Run timer at 1ms fixed rate -- the fractional accumulator in
        // hiResTimerCallback handles exact QF timing to avoid drift
        lastQfSendTime.store(juce::Time::getMillisecondCounterHiRes(), std::memory_order_relaxed);
        startTimer(1);
    }

    //==============================================================================
    std::unique_ptr<juce::MidiOutput> midiOutput;
    juce::Array<juce::MidiDeviceInfo> availableDevices;
    int currentDeviceIndex = -1;
    std::atomic<bool> isRunningFlag { false };
    std::atomic<bool> paused { false };

    juce::SpinLock tcLock;
    Timecode pendingTimecode;   // Written by UI thread, read under tcLock
    Timecode cycleTimecode;     // Timer-thread-only: snapshot taken at QF index 0, read through QF 1-7
    std::atomic<FrameRate> currentFps { FrameRate::FPS_25 };
    // currentQFIndex is primarily accessed from the timer thread, but reset from
    // the UI thread in start()/setPaused() after stopTimer().  JUCE guarantees
    // stopTimer() blocks until the current callback completes, but we use atomic
    // as belt-and-suspenders safety against platform-specific timing.
    std::atomic<int> currentQFIndex { 0 };
    std::atomic<double> lastQfSendTime { 0.0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MtcOutput)
};
