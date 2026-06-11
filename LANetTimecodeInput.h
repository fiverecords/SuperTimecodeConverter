// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

// LANetTimecodeInput
// Copyright (c) 2026 LaserAnimation Sollinger GmbH, https://www.laseranimation.com
// Written by Ingo Randolf (based on ArtnetInput)

#pragma once
#include <JuceHeader.h>
#include "TimecodeCore.h"
#include "NetworkUtils.h"
#include <atomic>

class LANetTimecodeInput : public juce::Thread
{
public:
    LANetTimecodeInput()
        : Thread("LA-Net Input")
    {
    }

    ~LANetTimecodeInput() override
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
    int getSelectedInterface() const { return selectedInterface; }

    //==============================================================================
    bool start(int interfaceIndex = 0, int port = 8201)
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


        // Enable SO_REUSEADDR before binding
        auto rawSock = socket->getRawSocketHandle();
        if (rawSock >= 0)
        {
            const int flag = 1;
#ifdef _WIN32
            setsockopt(rawSock, SOL_SOCKET, SO_REUSEADDR,
                       (const char*)&flag, sizeof(flag));
#else
            setsockopt(rawSock, SOL_SOCKET, SO_REUSEADDR,
                       &flag, sizeof(flag));
#endif
        }


        bool bound = false;
        bool fellBack = false;

        if (bindIp != "0.0.0.0")
        {
            bound = socket->bindToPort(listenPort, bindIp);
        }

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

        while (!threadShouldExit() && isRunningFlag.load(std::memory_order_relaxed))
        {
            // Capture local pointer: stop() may nullify `socket` from another thread
            // after calling socket->shutdown().  The shutdown unblocks waitUntilReady,
            // and then the while-condition will fail on the next iteration.  The local
            // pointer ensures we don't dereference a null between the check and use.
            auto* sock = socket.get();
            if (sock == nullptr)
                break;

            // Wait up to 100ms for data -- allows periodic threadShouldExit() checks
            // so the thread can shut down cleanly even if no packets are arriving
            if (!sock->waitUntilReady(true, 100))
                continue;

            int bytesRead = sock->read(buffer, sizeof(buffer), false);

            if (bytesRead == 28)
                parseLANetTimecodePacket(buffer, bytesRead);
        }
    }

    void parseLANetTimecodePacket(const uint8_t* data, int size)
    {
        if (size != 28)
        {
            // invalid data
            return;
        }


        const uint32_t* networkData = reinterpret_cast<const uint32_t*>(data);

        // message type
        uint32_t message_type = ByteOrder::swapIfLittleEndian(networkData[0]);

        if (message_type != 1)
        {
            return;
        }

        // version parsing
        uint32_t version = ByteOrder::swapIfLittleEndian(networkData[1]);

        if (version != 1)
        {
            return;
        }

        uint32_t len = ByteOrder::swapIfLittleEndian(networkData[3]);

        if (len != 12)
        {
            return;
        }


        uint32_t fps = ByteOrder::swapIfLittleEndian(networkData[4]);

        if (fps == 0)
        {
            fps = 25;
        }

        uint32_t timestamp = ByteOrder::swapIfLittleEndian(networkData[6]);

        if (timestamp == 0xffffffff)
        {
            return;
        }

        // check valid fps
        // ignore not supported fps: 50, 60, 100
        if (fps != 24 && fps != 25 && fps != 30)
        {
            // unsupported fps could be converted
            return;
        }

        int frames = timestamp % fps;
        timestamp -= frames;

        int seconds = (timestamp / fps) % 60;
        timestamp -= seconds * fps;

        int minutes = (timestamp / (60 * fps)) % 60;
        timestamp -= minutes * fps * 60;

        int hours = (timestamp / (60 * 60 * fps));

        // Validate ranges -- discard malformed packets
        // (lastPacketTime is updated AFTER validation so isReceiving()
        //  only returns true when we actually accepted valid data)
        if (hours > 23 || minutes > 59 || seconds > 59 || frames > 29)
        {
            return;
        }

        lastPacketTime.store(juce::Time::getMillisecondCounterHiRes(), std::memory_order_relaxed);

        switch (fps)
        {
            case 24: detectedFps.store(FrameRate::FPS_24, std::memory_order_relaxed);   break;
            case 25: detectedFps.store(FrameRate::FPS_25, std::memory_order_relaxed);   break;
            case 30: detectedFps.store(FrameRate::FPS_30, std::memory_order_relaxed);   break;
            default: break;
        }

        packedTimecode.store(packTimecode(hours, minutes, seconds, frames),
                             std::memory_order_relaxed);
    }

    std::unique_ptr<juce::DatagramSocket> socket;
    juce::String bindIp = "0.0.0.0";
    int listenPort = 8201;
    int selectedInterface = 0;
    std::atomic<bool> isRunningFlag { false };
    std::atomic<bool> bindFellBack { false };

    juce::Array<NetworkInterface> availableInterfaces;
    std::atomic<double> lastPacketTime { 0.0 };

    std::atomic<uint64_t> packedTimecode { 0 };
    std::atomic<FrameRate> detectedFps { FrameRate::FPS_25 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LANetTimecodeInput)
};
