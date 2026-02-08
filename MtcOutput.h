#pragma once
#include <JuceHeader.h>
#include "TimecodeCore.h"

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
            isRunning = true;
            paused = false;
            currentQFIndex = 0;
            cycleFrameCount = 0;
            updateTimerRate();
            return true;
        }

        return false;
    }

    void stop()
    {
        stopTimer();

        if (midiOutput != nullptr)
            midiOutput = nullptr;

        isRunning = false;
        paused = false;
        currentDeviceIndex = -1;
    }

    bool getIsRunning() const { return isRunning; }

    //==============================================================================
    // Called from UI thread - thread-safe via SpinLock
    void setTimecode(const Timecode& tc)
    {
        const juce::SpinLock::ScopedLockType lock(tcLock);
        pendingTimecode = tc;
    }

    void setFrameRate(FrameRate fps)
    {
        if (currentFps != fps)
        {
            currentFps = fps;
            if (isRunning && !paused)
                updateTimerRate();
        }
    }

    void setEnabled(bool enabled) { outputEnabled = enabled; }

    void setPaused(bool shouldPause)
    {
        if (paused == shouldPause)
            return;

        paused = shouldPause;

        if (paused)
        {
            stopTimer();
        }
        else if (isRunning)
        {
            currentQFIndex = 0;
            updateTimerRate();
        }
    }

    bool isPaused() const { return paused; }

    //==============================================================================
    void sendFullFrame()
    {
        if (midiOutput == nullptr || !outputEnabled)
            return;

        Timecode tc;
        {
            const juce::SpinLock::ScopedLockType lock(tcLock);
            tc = pendingTimecode;
        }

        int rateCode = fpsToRateCode(currentFps);
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
        if (midiOutput == nullptr || !outputEnabled || paused)
            return;

        // At QF index 0, snapshot the timecode for this entire 8-QF cycle
        // This guarantees all 8 QFs describe the SAME timecode - no jumps
        if (currentQFIndex == 0)
        {
            const juce::SpinLock::ScopedLockType lock(tcLock);
            cycleTimecode = pendingTimecode;
        }

        sendQuarterFrame(currentQFIndex);

        currentQFIndex++;

        // After sending all 8 QFs, advance cycle timecode by 2 frames
        // (MTC transmits 8 QFs over 2 frame periods)
        if (currentQFIndex >= 8)
        {
            currentQFIndex = 0;
            cycleFrameCount += 2;
        }
    }

    void sendQuarterFrame(int index)
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
                int rateCode = fpsToRateCode(currentFps);
                value = ((cycleTimecode.hours >> 4) & 0x01) | (rateCode << 1);
                break;
            }
        }

        uint8_t dataByte = (uint8_t)((index << 4) | (value & 0x0F));
        midiOutput->sendMessageNow(juce::MidiMessage(0xF1, (int)dataByte));
    }

    void updateTimerRate()
    {
        double fps = frameRateToDouble(currentFps);
        // MTC sends 4 Quarter Frame messages per frame
        double qfPerSecond = fps * 4.0;
        int intervalMs = juce::jmax(1, (int)std::round(1000.0 / qfPerSecond));
        startTimer(intervalMs);
    }

    static int fpsToRateCode(FrameRate fps)
    {
        switch (fps)
        {
            case FrameRate::FPS_24:   return 0;
            case FrameRate::FPS_25:   return 1;
            case FrameRate::FPS_2997: return 2;
            case FrameRate::FPS_30:   return 3;
        }
        return 1;
    }

    //==============================================================================
    std::unique_ptr<juce::MidiOutput> midiOutput;
    juce::Array<juce::MidiDeviceInfo> availableDevices;
    int currentDeviceIndex = -1;
    bool isRunning = false;
    bool outputEnabled = true;
    bool paused = false;

    juce::SpinLock tcLock;
    Timecode pendingTimecode;   // Written by UI thread
    Timecode cycleTimecode;     // Snapshot, read only by timer thread
    FrameRate currentFps = FrameRate::FPS_30;
    int currentQFIndex = 0;
    int64_t cycleFrameCount = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MtcOutput)
};
