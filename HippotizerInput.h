// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

#pragma once
#include <JuceHeader.h>
#include "TimecodeCore.h"
#include "NetworkUtils.h"
#include <atomic>

//==============================================================================
// HippotizerInput -- receives timecode from Green Hippo Hippotizer media server.
//
// Protocol (from Wireshark capture of Hippotizer v4.8.4.23374):
//
//   Timecode (UDP port 6091):
//     - 42-byte packets at ~82 Hz
//     - Magic: c6 5e e5 00 (offset 0-3)
//     - Payload: milliseconds since midnight as uint32 LE at offset 38-41
//
//   Discovery (UDP port 9009):
//     - ~0.5 Hz broadcast
//     - Plaintext: "GHUpdateMonitor_NAME;x.x.x;FIRMWARE;FIRMWARE;NAME;OK"
//     - Identifies machine name and firmware version
//
// Single capture available -- field offsets are tentative until confirmed
// with a second capture or official documentation.
//==============================================================================
class HippotizerInput : public juce::Thread
{
public:
    HippotizerInput()
        : Thread("Hippotizer Input")
    {
    }

    ~HippotizerInput() override
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
    bool start(int interfaceIndex = 0, int port = 6091)
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

        // --- Timecode socket ---
        socket = std::make_unique<juce::DatagramSocket>(false);

        // Allow port sharing with other applications (e.g. Hippotizer PLAY on same machine)
        {
            auto rawSock = socket->getRawSocketHandle();
            int reuse = 1;
#ifdef _WIN32
            setsockopt(rawSock, SOL_SOCKET, SO_REUSEADDR,
                       reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#else
            setsockopt(rawSock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
        }

        bool bound = false;
        bool fellBack = false;
        if (bindIp != "0.0.0.0")
            bound = socket->bindToPort(listenPort, bindIp);
        if (!bound)
        {
            bound = socket->bindToPort(listenPort);
            if (bound)
            {
                fellBack = (bindIp != "0.0.0.0");
                bindIp = "0.0.0.0";
            }
        }
        bindFellBack.store(fellBack, std::memory_order_relaxed);

        if (!bound)
        {
            socket = nullptr;
            return false;
        }

        // --- Discovery socket (port 9009, broadcast) ---
        // Best-effort: failure to bind does not prevent timecode reception.
        // Uses INADDR_ANY to see broadcasts from all subnets.
        discoverySocket = std::make_unique<juce::DatagramSocket>(true);
        if (!discoverySocket->bindToPort(kDiscoveryPort))
        {
            DBG("HippotizerInput: discovery bind failed on port "
                + juce::String(kDiscoveryPort) + " -- discovery disabled");
            discoverySocket = nullptr;
        }

        isRunningFlag.store(true, std::memory_order_relaxed);
        startThread();
        return true;
    }

    void stop()
    {
        isRunningFlag.store(false, std::memory_order_relaxed);
        bindFellBack.store(false, std::memory_order_relaxed);

        if (socket != nullptr)
            socket->shutdown();
        if (discoverySocket != nullptr)
            discoverySocket->shutdown();

        if (isThreadRunning())
            stopThread(1000);

        socket = nullptr;
        discoverySocket = nullptr;

        {
            const juce::SpinLock::ScopedLockType sl(discoveryLock);
            discoveredName.clear();
            discoveredFirmware.clear();
            discoveredIp.clear();
        }
        {
            const juce::SpinLock::ScopedLockType sl(channelLock);
            channels.clear();
        }
    }

    bool getIsRunning() const { return isRunningFlag.load(std::memory_order_relaxed); }
    int getListenPort() const { return listenPort; }

    //==============================================================================
    /// True if Hippotizer timecode packets are actively arriving
    bool isReceiving() const
    {
        double lpt = lastPacketTime.load(std::memory_order_relaxed);
        if (lpt == 0.0)
            return false;

        double now = juce::Time::getMillisecondCounterHiRes();
        double elapsed = now - lpt;

        return elapsed < kSourceTimeoutMs;
    }

    /// Raw milliseconds since midnight from the selected TC channel.
    uint32_t getMsSinceMidnight() const
    {
        const juce::SpinLock::ScopedLockType sl(channelLock);
        if (selectedTcIndex < (int)channels.size())
            return channels[(size_t)selectedTcIndex].lastMs;
        if (!channels.empty())
            return channels[0].lastMs;
        return msSinceMidnight.load(std::memory_order_relaxed);
    }

    /// Set which TC channel to use (0-based: 0=TC1, 1=TC2, ...)
    void setSelectedTcIndex(int idx) { selectedTcIndex = idx; }
    int  getSelectedTcIndex() const  { return selectedTcIndex; }

    /// Number of discovered TC channels
    int getDiscoveredChannelCount() const
    {
        const juce::SpinLock::ScopedLockType sl(channelLock);
        return (int)channels.size();
    }

    /// Get info about a discovered channel (thread-safe)
    struct ChannelInfo { uint32_t channelId; uint32_t lastMs; int type; };
    ChannelInfo getChannelInfo(int index) const
    {
        const juce::SpinLock::ScopedLockType sl(channelLock);
        if (index >= 0 && index < (int)channels.size())
            return { channels[(size_t)index].channelId, channels[(size_t)index].lastMs, channels[(size_t)index].type };
        return { 0, 0, 0 };
    }

    //==============================================================================
    // Discovery results
    //==============================================================================

    /// True if a Hippotizer has been discovered via port 9009.
    bool isDiscovered() const
    {
        const juce::SpinLock::ScopedLockType sl(discoveryLock);
        return discoveredIp.isNotEmpty();
    }

    /// Machine name (e.g. "GOHIPPO").
    juce::String getDiscoveredName() const
    {
        const juce::SpinLock::ScopedLockType sl(discoveryLock);
        return discoveredName;
    }

    /// Firmware version (e.g. "4.8.4.23374").
    juce::String getDiscoveredFirmware() const
    {
        const juce::SpinLock::ScopedLockType sl(discoveryLock);
        return discoveredFirmware;
    }

    /// IP address of the discovered Hippotizer.
    juce::String getDiscoveredIp() const
    {
        const juce::SpinLock::ScopedLockType sl(discoveryLock);
        return discoveredIp;
    }

private:
    static constexpr int kDiscoveryPort = 9009;

    void run() override
    {
        uint8_t buffer[512];

        while (!threadShouldExit() && isRunningFlag.load(std::memory_order_relaxed))
        {
            // --- Check discovery socket (non-blocking) ---
            auto* discSock = discoverySocket.get();
            if (discSock != nullptr && discSock->waitUntilReady(true, 0))
            {
                juce::String senderIp;
                int senderPort = 0;
                int discBytes = discSock->read(buffer, sizeof(buffer), false,
                                               senderIp, senderPort);
                if (discBytes > 16)
                    parseDiscoveryPacket(buffer, discBytes, senderIp);
            }

            // --- Wait for timecode data (100ms timeout) ---
            auto* sock = socket.get();
            if (sock == nullptr)
                break;

            if (!sock->waitUntilReady(true, 100))
                continue;

            int bytesRead = sock->read(buffer, sizeof(buffer), false);

            if (bytesRead >= 42)
                parseHippoPacket(buffer, bytesRead);
        }
    }

    void parseHippoPacket(const uint8_t* data, int size)
    {
        if (size < 42)
            return;

        if (data[0] != 0xC6 || data[1] != 0x5E || data[2] != 0xE5 || data[3] != 0x00)
            return;

        lastPacketTime.store(juce::Time::getMillisecondCounterHiRes(), std::memory_order_relaxed);

        // Parse all 18-byte TC blocks starting at offset 24
        const int kHeaderSize = 24;
        const int kBlockSize  = 18;

        // Collect blocks from this packet into a temp list
        struct Block { uint32_t channelId; uint32_t ms; int type; };
        std::vector<Block> blocks;

        for (int off = kHeaderSize; off + kBlockSize <= size; off += kBlockSize)
        {
            int btype = (int)((uint32_t)data[off+4]
                            | ((uint32_t)data[off+5] << 8)
                            | ((uint32_t)data[off+6] << 16)
                            | ((uint32_t)data[off+7] << 24));

            uint32_t cid = (uint32_t)data[off+8]
                         | ((uint32_t)data[off+9] << 8)
                         | ((uint32_t)data[off+10] << 16)
                         | ((uint32_t)data[off+11] << 24);

            uint32_t ms = (uint32_t)data[off+14]
                        | ((uint32_t)data[off+15] << 8)
                        | ((uint32_t)data[off+16] << 16)
                        | ((uint32_t)data[off+17] << 24);

            if (btype > 100 || ms > 86400000u)
                continue;

            blocks.push_back({ cid, ms, btype });
        }

        // Deduplicate by ms value: keep first occurrence of each unique ms
        std::vector<Block> unique;
        for (auto& b : blocks)
        {
            bool dup = false;
            for (auto& u : unique)
                if (u.ms == b.ms) { dup = true; break; }
            if (!dup)
                unique.push_back(b);
        }

        // Reverse order: Hippotizer's internal block order is inverted
        // relative to the TC1/TC2 numbering in its UI
        std::reverse(unique.begin(), unique.end());

        // Update channel list (lock)
        {
            const juce::SpinLock::ScopedLockType sl(channelLock);

            for (auto& blk : unique)
            {
                bool found = false;
                for (auto& ch : channels)
                {
                    if (ch.channelId == blk.channelId)
                    {
                        ch.lastMs = blk.ms;
                        ch.type   = blk.type;
                        found = true;
                        break;
                    }
                }
                if (!found)
                    channels.push_back({ blk.channelId, blk.ms, blk.type });
            }
        }

        // Backward compat: also store first block's ms in the atomic
        if (!unique.empty())
            msSinceMidnight.store(unique[0].ms, std::memory_order_relaxed);
    }

    /// Parse discovery broadcast from port 9009.
    /// Format: "GHUpdateMonitor_NAME;x.x.x;FIRMWARE;FIRMWARE;NAME;OK"
    void parseDiscoveryPacket(const uint8_t* data, int size, const juce::String& senderIp)
    {
        static const char* kPrefix = "GHUpdateMonitor_";
        static const int kPrefixLen = 16;

        if (size < kPrefixLen + 5)
            return;
        if (std::memcmp(data, kPrefix, (size_t)kPrefixLen) != 0)
            return;

        juce::String payload(reinterpret_cast<const char*>(data + kPrefixLen),
                             (size_t)(size - kPrefixLen));
        auto fields = juce::StringArray::fromTokens(payload, ";", "");

        if (fields.size() < 3)
            return;

        juce::String name     = fields[0].trim();
        juce::String firmware = fields[2].trim();

        if (name.isEmpty())
            return;

        {
            const juce::SpinLock::ScopedLockType sl(discoveryLock);
            discoveredName     = name;
            discoveredFirmware = firmware;
            discoveredIp       = senderIp;
        }
    }

    std::unique_ptr<juce::DatagramSocket> socket;
    std::unique_ptr<juce::DatagramSocket> discoverySocket;
    juce::String bindIp = "0.0.0.0";
    int listenPort = 6091;
    int selectedInterface = 0;
    std::atomic<bool> isRunningFlag { false };
    std::atomic<bool> bindFellBack { false };

    juce::Array<NetworkInterface> availableInterfaces;
    std::atomic<double> lastPacketTime { 0.0 };
    std::atomic<uint32_t> msSinceMidnight { 0 };

    // Multi-layer channel tracking
    struct DiscoveredChannel { uint32_t channelId; uint32_t lastMs; int type; };
    mutable juce::SpinLock channelLock;
    std::vector<DiscoveredChannel> channels;
    int selectedTcIndex = 0;

    // Discovery state
    mutable juce::SpinLock discoveryLock;
    juce::String discoveredName;
    juce::String discoveredFirmware;
    juce::String discoveredIp;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HippotizerInput)
};
