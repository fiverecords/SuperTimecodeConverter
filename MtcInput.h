// Super Timecode Converter
// Copyright (c) 2026 Fiverecords — MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

#pragma once
#include <JuceHeader.h>
#include "TimecodeCore.h"
#include <atomic>

class MtcInput : public juce::MidiInputCallback
{
public:
    MtcInput()
    {
        refreshDeviceList();
    }

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
        if (midiInput != nullptr)
        {
            midiInput->stop();
            midiInput = nullptr;
        }
        isRunningFlag.store(false, std::memory_order_relaxed);
        currentDeviceIndex = -1;
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
        auto rawData = message.getRawData();
        int rawSize = message.getRawDataSize();

        if (rawSize >= 2 && rawData[0] == 0xF1)
        {
            lastQfReceiveTime.store(juce::Time::getMillisecondCounterHiRes(), std::memory_order_relaxed);

            int dataByte = rawData[1];
            int index = (dataByte >> 4) & 0x07;
            int value = dataByte & 0x0F;

            if (index >= 0 && index <= 7)
            {
                mtcData[index] = value;

                if (index == 7)
                    reconstructAndSync();
            }
        }
        else if (message.isSysEx())
        {
            auto* sysex = message.getSysExData();
            int sysexSize = message.getSysExDataSize();

            if (sysexSize >= 8 &&
                sysex[0] == 0x7F && sysex[1] == 0x7F &&
                sysex[2] == 0x01 && sysex[3] == 0x01)
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
                synced.store(true, std::memory_order_release);
            }
        }
    }

private:
    void reconstructAndSync()
    {
        int frames  = mtcData[0] | (mtcData[1] << 4);
        int seconds = mtcData[2] | (mtcData[3] << 4);
        int minutes = mtcData[4] | (mtcData[5] << 4);
        int hours   = mtcData[6] | ((mtcData[7] & 0x01) << 4);

        int rateCode = (mtcData[7] >> 1) & 0x03;

        {
            const juce::SpinLock::ScopedLockType lock(tcLock);
            updateDetectedFps(rateCode);

            int maxFrames = frameRateToInt(detectedFps);

            // MTC quarter-frame messages describe the timecode from 2 frames
            // prior (8 QFs × ¼ frame = 2 frames of latency).  Adding 2 compensates
            // so the displayed timecode matches the current position.
            // NOTE: this compensation assumes forward playback.  Reverse or locate
            // operations may briefly show a ±4 frame discrepancy until the next
            // full 8-QF cycle completes.
            int64_t totalFrames = (int64_t)hours * 3600 * maxFrames
                                + (int64_t)minutes * 60 * maxFrames
                                + (int64_t)seconds * maxFrames
                                + (int64_t)frames
                                + 2;

            lastSyncTimecode.frames  = (int)(totalFrames % maxFrames);
            lastSyncTimecode.seconds = (int)((totalFrames / maxFrames) % 60);
            lastSyncTimecode.minutes = (int)((totalFrames / (maxFrames * 60)) % 60);
            lastSyncTimecode.hours   = (int)((totalFrames / (maxFrames * 3600)) % 24);

            syncTimeMs = juce::Time::getMillisecondCounterHiRes();
        }
        synced.store(true, std::memory_order_release);
    }

    void updateDetectedFps(int rateCode)
    {
        switch (rateCode)
        {
            case 0: detectedFps = FrameRate::FPS_24;   break;
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
        synced.store(false, std::memory_order_relaxed);
        {
            const juce::SpinLock::ScopedLockType lock(tcLock);
            syncTimeMs = 0.0;
            lastSyncTimecode = Timecode();
        }
        lastQfReceiveTime.store(0.0, std::memory_order_relaxed);
    }

    std::unique_ptr<juce::MidiInput> midiInput;
    juce::Array<juce::MidiDeviceInfo> availableDevices;
    int currentDeviceIndex = -1;
    std::atomic<bool> isRunningFlag { false };

    // Quarter-frame accumulator -- MIDI-callback-thread-only
    int mtcData[8] = {};

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
