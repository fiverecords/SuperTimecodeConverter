#pragma once
#include <JuceHeader.h>
#include "TimecodeCore.h"

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
            isRunning = true;
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
        isRunning = false;
        currentDeviceIndex = -1;
    }

    bool getIsRunning() const { return isRunning; }

    //==============================================================================
    // True if QF messages are actively arriving
    bool isReceiving() const
    {
        if (!synced)
            return false;

        double now = juce::Time::getMillisecondCounterHiRes();
        double elapsed = now - lastQfReceiveTime;

        // MTC at 24fps sends QF every ~10.4ms, at 30fps ~8.3ms
        // 150ms silence = source paused
        return elapsed < 150.0;
    }

    Timecode getCurrentTimecode() const
    {
        if (!synced)
            return Timecode();

        if (!isReceiving())
            return lastSyncTimecode; // Frozen

        double now = juce::Time::getMillisecondCounterHiRes();
        double elapsed = now - syncTimeMs;

        if (elapsed < 0.0)
            return lastSyncTimecode;

        double fps = frameRateToDouble(detectedFps);
        int maxFrames = frameRateToInt(detectedFps);
        double msPerFrame = 1000.0 / fps;

        int extraFrames = (int)(elapsed / msPerFrame);

        int64_t syncTotal = (int64_t)lastSyncTimecode.hours * 3600 * maxFrames
                          + (int64_t)lastSyncTimecode.minutes * 60 * maxFrames
                          + (int64_t)lastSyncTimecode.seconds * maxFrames
                          + (int64_t)lastSyncTimecode.frames;

        int64_t currentTotal = syncTotal + extraFrames;

        Timecode result;
        result.frames  = (int)(currentTotal % maxFrames);
        result.seconds = (int)((currentTotal / maxFrames) % 60);
        result.minutes = (int)((currentTotal / (maxFrames * 60)) % 60);
        result.hours   = (int)((currentTotal / (maxFrames * 3600)) % 24);

        return result;
    }

    FrameRate getDetectedFrameRate() const { return detectedFps; }

    //==============================================================================
    void handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage& message) override
    {
        auto rawData = message.getRawData();
        int rawSize = message.getRawDataSize();

        if (rawSize >= 2 && rawData[0] == 0xF1)
        {
            lastQfReceiveTime = juce::Time::getMillisecondCounterHiRes();

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
                lastQfReceiveTime = juce::Time::getMillisecondCounterHiRes();

                int hr = sysex[4];
                int mn = sysex[5];
                int sc = sysex[6];
                int fr = sysex[7];

                int rateCode = (hr >> 5) & 0x03;
                hr &= 0x1F;

                updateDetectedFps(rateCode);

                lastSyncTimecode.hours   = hr;
                lastSyncTimecode.minutes = mn;
                lastSyncTimecode.seconds = sc;
                lastSyncTimecode.frames  = fr;
                syncTimeMs = juce::Time::getMillisecondCounterHiRes();
                synced = true;
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
        updateDetectedFps(rateCode);

        int maxFrames = frameRateToInt(detectedFps);

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
        synced = true;
    }

    void updateDetectedFps(int rateCode)
    {
        switch (rateCode)
        {
            case 0: detectedFps = FrameRate::FPS_24;   break;
            case 1: detectedFps = FrameRate::FPS_25;   break;
            case 2: detectedFps = FrameRate::FPS_2997; break;
            case 3: detectedFps = FrameRate::FPS_30;   break;
        }
    }

    void resetState()
    {
        for (int i = 0; i < 8; i++)
            mtcData[i] = 0;
        synced = false;
        syncTimeMs = 0.0;
        lastQfReceiveTime = 0.0;
        lastSyncTimecode = Timecode();
    }

    std::unique_ptr<juce::MidiInput> midiInput;
    juce::Array<juce::MidiDeviceInfo> availableDevices;
    int currentDeviceIndex = -1;
    bool isRunning = false;

    int mtcData[8] = {};
    Timecode lastSyncTimecode;
    double syncTimeMs = 0.0;
    double lastQfReceiveTime = 0.0;
    bool synced = false;
    FrameRate detectedFps = FrameRate::FPS_25;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MtcInput)
};
