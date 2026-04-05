// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

#pragma once
#include <JuceHeader.h>
#include "TimecodeCore.h"
#include "NetworkUtils.h"
#include <atomic>

#ifdef _WIN32
    #include <winsock2.h>
#else
    #include <sys/socket.h>
#endif

//==============================================================================
// HippotizerOutput -- sends timecode to Green Hippo Hippotizer media server.
//
// Transmits 42-byte packets on UDP port 6091 at ~82 Hz, replicating the same
// packet format that a real Hippotizer uses (magic c65ee500, ms since midnight
// as uint32 LE at offset 38).
//
// Destination can be unicast (specific Hippotizer IP) or broadcast.
//
// Caveat: Based on a single Wireshark capture. Unknown whether the Hippotizer
// requires additional handshake/response packets to accept incoming timecode.
//==============================================================================
class HippotizerOutput : public juce::HighResolutionTimer
{
public:
    HippotizerOutput()
    {
        refreshNetworkInterfaces();
        buildPacketTemplate();
    }

    ~HippotizerOutput() override
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
        for (auto& ni : availableInterfaces)
            names.add(ni.name + " (" + ni.ip + ")");
        return names;
    }

    int getInterfaceCount() const { return availableInterfaces.size(); }
    int getSelectedInterface() const { return selectedInterface; }
    juce::String getDestination() const { return destIp + ":" + juce::String(destPort); }
    uint32_t getSendErrors() const { return sendErrors.load(std::memory_order_relaxed); }

    //==============================================================================
    /// Start output.
    /// @param targetIp   Hippotizer IP address (e.g. "192.168.0.2") or
    ///                   "255.255.255.255" for broadcast.
    /// @param interfaceIndex  Network interface to bind, or -1 for all.
    /// @param port       Destination UDP port (default 6091).
    bool start(const juce::String& targetIp, int interfaceIndex = -1, int port = 6091)
    {
        stop();

        destIp   = targetIp.trim();
        destPort = port;

        if (destIp.isEmpty())
            destIp = "255.255.255.255";

        if (interfaceIndex >= 0 && interfaceIndex < availableInterfaces.size())
        {
            selectedInterface = interfaceIndex;
            bindIp = availableInterfaces[interfaceIndex].ip;
            // If user chose broadcast destination, use subnet broadcast for the selected NIC
            if (destIp == "255.255.255.255")
                destIp = availableInterfaces[interfaceIndex].broadcast;
        }
        else
        {
            selectedInterface = -1;
            bindIp = "0.0.0.0";
        }

        socket = std::make_unique<juce::DatagramSocket>(false);

        if (!socket->bindToPort(0, bindIp))
        {
            if (!socket->bindToPort(0))
            {
                socket = nullptr;
                return false;
            }
        }

        // Enable SO_BROADCAST for broadcast destinations
        auto rawSock = socket->getRawSocketHandle();
        if (rawSock >= 0)
        {
            int broadcastFlag = 1;
#ifdef _WIN32
            setsockopt(rawSock, SOL_SOCKET, SO_BROADCAST,
                       (const char*)&broadcastFlag, sizeof(broadcastFlag));
#else
            setsockopt(rawSock, SOL_SOCKET, SO_BROADCAST,
                       &broadcastFlag, sizeof(broadcastFlag));
#endif
        }

        isRunningFlag.store(true, std::memory_order_relaxed);
        paused.store(false, std::memory_order_relaxed);
        sendErrors.store(0, std::memory_order_relaxed);
        lastSendTime.store(juce::Time::getMillisecondCounterHiRes(), std::memory_order_relaxed);
        startTimer(1);  // 1ms resolution, actual send rate controlled by accumulator
        return true;
    }

    void stop()
    {
        stopTimer();
        isRunningFlag.store(false, std::memory_order_relaxed);
        paused.store(false, std::memory_order_relaxed);

        if (socket != nullptr)
        {
            socket->shutdown();
            socket = nullptr;
        }
    }

    bool getIsRunning() const { return isRunningFlag.load(std::memory_order_relaxed); }

    //==============================================================================
    void setTimecode(const Timecode& tc)
    {
        const juce::SpinLock::ScopedLockType lock(tcLock);
        timecodeToSend = tc;
    }

    void setFrameRate(FrameRate fps)
    {
        currentFps.store(fps, std::memory_order_relaxed);
    }

    void setPaused(bool shouldPause)
    {
        if (paused.load(std::memory_order_relaxed) == shouldPause)
            return;
        paused.store(shouldPause, std::memory_order_relaxed);

        if (!shouldPause && isRunningFlag.load(std::memory_order_relaxed))
        {
            lastSendTime.store(juce::Time::getMillisecondCounterHiRes(), std::memory_order_relaxed);
            if (!isTimerRunning())
                startTimer(1);
        }
        else if (shouldPause)
        {
            stopTimer();
        }
    }

    bool isPaused() const { return paused.load(std::memory_order_relaxed); }

    /// Force immediate packet send (e.g. on seek).
    void forceResync()
    {
        if (!isRunningFlag.load(std::memory_order_relaxed)
            || paused.load(std::memory_order_relaxed)
            || socket == nullptr)
            return;
        sendHippoPacket();
    }

private:
    //==============================================================================
    // Packet template -- constant fields filled once, only offset 38-41 changes.
    //==============================================================================
    void buildPacketTemplate()
    {
        std::memset(packetTemplate, 0, sizeof(packetTemplate));

        // Magic: c6 5e e5 00
        packetTemplate[0] = 0xC6;
        packetTemplate[1] = 0x5E;
        packetTemplate[2] = 0xE5;
        packetTemplate[3] = 0x00;

        // Protocol marker (LE): 0x006a
        packetTemplate[4] = 0x6A;
        packetTemplate[5] = 0x00;

        // Payload length (LE): 42
        packetTemplate[6] = 0x2A;
        packetTemplate[7] = 0x00;

        // Source ID (GUID): fixed identifier for STC
        // "STC-TIMECODE-OUT" in ASCII (16 bytes)
        const char* guid = "STC-TIMECODE-OUT";
        std::memcpy(packetTemplate + 8, guid, 16);

        // Message type: 06 00
        packetTemplate[24] = 0x06;
        packetTemplate[25] = 0x00;

        // Sub-length: 12 00 (=18)
        packetTemplate[26] = 0x12;
        packetTemplate[27] = 0x00;

        // Unknown constant: 03 00 00 00
        packetTemplate[28] = 0x03;
        packetTemplate[29] = 0x00;
        packetTemplate[30] = 0x00;
        packetTemplate[31] = 0x00;

        // Unknown constant: 1a 03
        packetTemplate[32] = 0x1A;
        packetTemplate[33] = 0x03;

        // Padding: 00 00
        packetTemplate[34] = 0x00;
        packetTemplate[35] = 0x00;

        // Data type marker: 04 00
        packetTemplate[36] = 0x04;
        packetTemplate[37] = 0x00;

        // Offset 38-41: ms since midnight (filled per-send)
    }

    void hiResTimerCallback() override
    {
        if (!isRunningFlag.load(std::memory_order_relaxed)
            || paused.load(std::memory_order_relaxed)
            || socket == nullptr)
        {
            stopTimer();
            return;
        }

        // ~82 Hz = 12.195 ms interval.  Use fractional accumulator to avoid drift.
        static constexpr double kSendIntervalMs = 12.195;

        double now = juce::Time::getMillisecondCounterHiRes();
        double lastSend = lastSendTime.load(std::memory_order_relaxed);

        int sent = 0;
        while ((now - lastSend) >= kSendIntervalMs && sent < 2)
        {
            sendHippoPacket();
            lastSend += kSendIntervalMs;
            sent++;
        }
        lastSendTime.store(lastSend, std::memory_order_relaxed);

        // Reset if fallen too far behind
        if ((now - lastSend) > 100.0)
            lastSendTime.store(now, std::memory_order_relaxed);
    }

    void sendHippoPacket()
    {
        Timecode tc;
        {
            const juce::SpinLock::ScopedLockType lock(tcLock);
            tc = timecodeToSend;
        }

        FrameRate fps = currentFps.load(std::memory_order_relaxed);

        // Convert timecode to ms since midnight
        double ms = timecodeToMs(tc, fps);
        if (ms < 0.0) ms = 0.0;
        if (ms > 86400000.0) ms = 86400000.0;
        uint32_t msInt = (uint32_t)ms;

        // Copy template and fill in the ms value (LE uint32 at offset 38)
        uint8_t packet[42];
        std::memcpy(packet, packetTemplate, 42);
        packet[38] = (uint8_t)(msInt & 0xFF);
        packet[39] = (uint8_t)((msInt >> 8) & 0xFF);
        packet[40] = (uint8_t)((msInt >> 16) & 0xFF);
        packet[41] = (uint8_t)((msInt >> 24) & 0xFF);

        int written = socket->write(destIp, destPort, packet, 42);
        if (written < 0)
            sendErrors.fetch_add(1, std::memory_order_relaxed);
    }

    //==============================================================================
    std::unique_ptr<juce::DatagramSocket> socket;
    juce::String destIp   = "255.255.255.255";
    juce::String bindIp   = "0.0.0.0";
    int destPort          = 6091;
    int selectedInterface = -1;
    std::atomic<bool> isRunningFlag { false };
    std::atomic<bool> paused { false };

    juce::Array<NetworkInterface> availableInterfaces;

    juce::SpinLock tcLock;
    Timecode timecodeToSend;
    std::atomic<FrameRate> currentFps { FrameRate::FPS_25 };
    std::atomic<double> lastSendTime { 0.0 };
    std::atomic<uint32_t> sendErrors { 0 };

    uint8_t packetTemplate[42] {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HippotizerOutput)
};
