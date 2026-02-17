// Super Timecode Converter
// Copyright (c) 2026 Fiverecords — MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

#pragma once
#include <JuceHeader.h>
#include "TimecodeCore.h"
#include "NetworkUtils.h"
#include <atomic>

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
        availableInterfaces = ::getNetworkInterfaces();
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
    bool didFallBackToAllInterfaces() const { return bindFellBack.load(std::memory_order_relaxed); }

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
        bool fellBack = false;
        if (bindIp != "0.0.0.0")
            bound = socket->bindToPort(listenPort, bindIp);
        if (!bound)
        {
            bound = socket->bindToPort(listenPort);
            if (bound)
            {
                fellBack = (bindIp != "0.0.0.0");  // only a fallback if we tried a specific IP
                bindIp = "0.0.0.0";   // reflect actual bind address
            }
        }
        bindFellBack.store(fellBack, std::memory_order_relaxed);

        if (bound)
        {
            isRunningFlag.store(true, std::memory_order_relaxed);
            startThread();
            return true;
        }

        socket = nullptr;
        return false;
    }

    void stop()
    {
        isRunningFlag.store(false, std::memory_order_relaxed);
        bindFellBack.store(false, std::memory_order_relaxed);

        if (socket != nullptr)
            socket->shutdown();

        if (isThreadRunning())
            stopThread(1000);

        socket = nullptr;
    }

    bool getIsRunning() const { return isRunningFlag.load(std::memory_order_relaxed); }
    int getListenPort() const { return listenPort; }

    //==============================================================================
    // True if Art-Net TC packets are actively arriving
    bool isReceiving() const
    {
        double lpt = lastPacketTime.load(std::memory_order_relaxed);
        if (lpt == 0.0)
            return false;

        double now = juce::Time::getMillisecondCounterHiRes();
        double elapsed = now - lpt;

        // At 24fps a packet arrives every ~41ms, at 30fps ~33ms
        return elapsed < kSourceTimeoutMs;
    }

    Timecode getCurrentTimecode() const
    {
        return unpackTimecode(packedTimecode.load(std::memory_order_relaxed));
    }
    FrameRate getDetectedFrameRate() const { return detectedFps.load(std::memory_order_relaxed); }

private:
    void run() override
    {
        uint8_t buffer[1024];

        while (!threadShouldExit() && isRunningFlag.load(std::memory_order_relaxed) && socket != nullptr)
        {
            // Wait up to 100ms for data -- allows periodic threadShouldExit() checks
            // so the thread can shut down cleanly even if no packets are arriving
            if (!socket->waitUntilReady(true, 100))
                continue;

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

        uint16_t opcode = (uint16_t)((uint16_t)data[8] | ((uint16_t)data[9] << 8));
        if (opcode != 0x9700)
            return;

        // ProtVer is big-endian (Hi byte at offset 10, Lo at 11)
        // Art-Net 4 requires ProtVer >= 14; accept anything >= 14 for compatibility
        uint16_t protVer = (uint16_t)(((uint16_t)data[10] << 8) | (uint16_t)data[11]);
        if (protVer < 14)
            return;

        int frames  = data[14];
        int seconds = data[15];
        int minutes = data[16];
        int hours   = data[17];
        int rateCode = data[18] & 0x03;

        // Art-Net 4 spec: bits 2-7 of the Type field are reserved and must be 0.
        // Log a warning if they are non-zero (malformed sender), but still
        // process the packet — the frame-rate bits 0-1 remain valid.
        if ((data[18] & 0xFC) != 0)
        {
            DBG("ArtTimeCode: reserved bits in Type field are non-zero (0x"
                + juce::String::toHexString(data[18]) + "). Packet may be malformed.");
        }

        // Validate ranges -- discard malformed packets
        // (lastPacketTime is updated AFTER validation so isReceiving()
        //  only returns true when we actually accepted valid data)
        if (hours > 23 || minutes > 59 || seconds > 59 || frames > 29)
            return;

        lastPacketTime.store(juce::Time::getMillisecondCounterHiRes(), std::memory_order_relaxed);

        switch (rateCode)
        {
            case 0: detectedFps.store(FrameRate::FPS_24, std::memory_order_relaxed);   break;
            case 1: detectedFps.store(FrameRate::FPS_25, std::memory_order_relaxed);   break;
            case 2: detectedFps.store(FrameRate::FPS_2997, std::memory_order_relaxed); break;
            case 3: detectedFps.store(FrameRate::FPS_30, std::memory_order_relaxed);   break;
            default: break;  // mask guarantees 0-3, but be explicit
        }

        packedTimecode.store(packTimecode(hours, minutes, seconds, frames),
                             std::memory_order_relaxed);
    }

    std::unique_ptr<juce::DatagramSocket> socket;
    juce::String bindIp = "0.0.0.0";
    int listenPort = 6454;
    int selectedInterface = 0;
    std::atomic<bool> isRunningFlag { false };
    std::atomic<bool> bindFellBack { false };

    juce::Array<NetworkInterface> availableInterfaces;
    std::atomic<double> lastPacketTime { 0.0 };

    std::atomic<uint64_t> packedTimecode { 0 };
    std::atomic<FrameRate> detectedFps { FrameRate::FPS_25 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ArtnetInput)
};
