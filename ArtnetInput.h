#pragma once
#include <JuceHeader.h>
#include "TimecodeCore.h"
#include "ArtnetOutput.h"

class ArtnetInput : public juce::Thread
{
public:
    ArtnetInput()
        : Thread("ArtNet Input")
    {
    }

    ~ArtnetInput() override
    {
        stop();
    }

    //==============================================================================
    void refreshNetworkInterfaces()
    {
        availableInterfaces = ArtnetOutput::getNetworkInterfaces();
    }

    juce::StringArray getInterfaceNames() const
    {
        juce::StringArray names;
        names.add("ALL INTERFACES (0.0.0.0)");
        for (auto& ni : availableInterfaces)
            names.add(ni.name + " (" + ni.ip + ")");
        return names;
    }

    int getInterfaceCount() const { return availableInterfaces.size() + 1; }

    juce::String getBindInfo() const { return bindIp + ":" + juce::String(listenPort); }

    //==============================================================================
    bool start(int interfaceIndex = 0, int port = 6454)
    {
        stop();

        listenPort = port;

        if (interfaceIndex > 0 && (interfaceIndex - 1) < availableInterfaces.size())
        {
            selectedInterface = interfaceIndex;
            bindIp = availableInterfaces[interfaceIndex - 1].ip;
        }
        else
        {
            selectedInterface = 0;
            bindIp = "0.0.0.0";
        }

        socket = std::make_unique<juce::DatagramSocket>(false);

        bool bound = false;
        if (bindIp != "0.0.0.0")
            bound = socket->bindToPort(listenPort, bindIp);
        if (!bound)
            bound = socket->bindToPort(listenPort);

        if (bound)
        {
            isRunning = true;
            startThread();
            return true;
        }

        socket = nullptr;
        return false;
    }

    void stop()
    {
        isRunning = false;

        if (socket != nullptr)
            socket->shutdown();

        if (isThreadRunning())
            stopThread(1000);

        socket = nullptr;
    }

    bool getIsRunning() const { return isRunning; }
    int getListenPort() const { return listenPort; }

    //==============================================================================
    // True if Art-Net TC packets are actively arriving
    bool isReceiving() const
    {
        if (lastPacketTime == 0.0)
            return false;

        double now = juce::Time::getMillisecondCounterHiRes();
        double elapsed = now - lastPacketTime;

        // At 24fps a packet arrives every ~41ms, at 30fps ~33ms
        // 150ms silence = source paused
        return elapsed < 150.0;
    }

    Timecode getCurrentTimecode() const { return currentTimecode; }
    FrameRate getDetectedFrameRate() const { return detectedFps; }

private:
    void run() override
    {
        uint8_t buffer[1024];

        while (!threadShouldExit() && isRunning && socket != nullptr)
        {
            int bytesRead = socket->read(buffer, sizeof(buffer), false);

            if (bytesRead >= 19)
                parseArtNetPacket(buffer, bytesRead);
        }
    }

    void parseArtNetPacket(const uint8_t* data, int size)
    {
        if (size < 19)
            return;

        if (data[0] != 'A' || data[1] != 'r' || data[2] != 't' ||
            data[3] != '-' || data[4] != 'N' || data[5] != 'e' ||
            data[6] != 't' || data[7] != 0)
            return;

        uint16_t opcode = (uint16_t)(data[8] | (data[9] << 8));
        if (opcode != 0x9700)
            return;

        lastPacketTime = juce::Time::getMillisecondCounterHiRes();

        int frames  = data[14];
        int seconds = data[15];
        int minutes = data[16];
        int hours   = data[17];
        int rateCode = data[18] & 0x03;

        switch (rateCode)
        {
            case 0: detectedFps = FrameRate::FPS_24;   break;
            case 1: detectedFps = FrameRate::FPS_25;   break;
            case 2: detectedFps = FrameRate::FPS_2997; break;
            case 3: detectedFps = FrameRate::FPS_30;   break;
        }

        currentTimecode.hours   = hours;
        currentTimecode.minutes = minutes;
        currentTimecode.seconds = seconds;
        currentTimecode.frames  = frames;
    }

    std::unique_ptr<juce::DatagramSocket> socket;
    juce::String bindIp = "0.0.0.0";
    int listenPort = 6454;
    int selectedInterface = 0;
    bool isRunning = false;

    juce::Array<NetworkInterface> availableInterfaces;
    double lastPacketTime = 0.0;

    Timecode currentTimecode;
    FrameRate detectedFps = FrameRate::FPS_25;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ArtnetInput)
};
