// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

#pragma once
#include <JuceHeader.h>
#include "TimecodeCore.h"
#include "NetworkUtils.h"
#include "HippoNetNodeData.h"
#include <atomic>

//==============================================================================
// HippotizerInput -- receives timecode from Green Hippo Hippotizer media server.
//
// Protocol (from Wireshark captures):
//
//   Peer discovery & negotiation:
//     1. UDP broadcast port 9009: "GHUpdateMonitor_NAME;ver;fw;fw;NAME;OK" (~2s)
//     2. UDP broadcast port 6092: HippoNet announcement (block type 0x0001) (~3s)
//        - Both sides announce: "NAME,HippoEngineHost" from port 6091
//     3. Sender sees receiver's 6092 announcement → TCP connect to receiver:6091
//        - Sender sends: 8 bytes (IP_BE + randomID) + HippoNet announcement
//        - Receiver responds: 8 bytes (IP_BE + ID) + HippoNet announcement
//        - Sender sends: ~13KB Zookeeper node tree (consumed, not parsed)
//     4. TC flows: UDP unicast 6091→6091
//
//   Timecode packets (UDP port 6091):
//     - 24-byte header: magic c65ee500(4) + pad(2) + pkt_length(2) + source_GUID(16)
//     - Variable-length TC blocks (self-describing block_length at block[2:4]):
//         18B block: 1 TC at block+14 (Hippotizer PLAY)
//         26B block: 2 TCs at block+18 and block+22 (real hardware)
//     - TCs as uint32 LE ms-since-midnight
//     - ~82 Hz packet rate
//
// Validated with Hippotizer PLAY and real Hippotizer hardware captures.
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

        // Enable SO_BROADCAST so we can send announcements
        {
            auto rawSock = socket->getRawSocketHandle();
            int bcast = 1;
#ifdef _WIN32
            setsockopt(rawSock, SOL_SOCKET, SO_BROADCAST,
                       reinterpret_cast<const char*>(&bcast), sizeof(bcast));
#else
            setsockopt(rawSock, SOL_SOCKET, SO_BROADCAST, &bcast, sizeof(bcast));
#endif
        }

        // Determine broadcast address for announcements
        if (selectedInterface > 0 && (selectedInterface - 1) < availableInterfaces.size())
            announceBroadcastIp = availableInterfaces[selectedInterface - 1].broadcast;
        else
            announceBroadcastIp = "255.255.255.255";

        // --- TCP listener on port 6091 for HippoNet negotiation ---
        tcpListener = std::make_unique<juce::StreamingSocket>();
        if (!tcpListener->createListener(listenPort, bindIp))
        {
            DBG("HippoNet: TCP listener failed on port " + juce::String(listenPort));
            tcpListener = nullptr;
        }

        // --- Zookeeper TCP listener on a random high port ---
        zkTcpListener = std::make_unique<juce::StreamingSocket>();
        zkTcpPort = 0;
        {
            juce::Random portRng;
            for (int attempt = 0; attempt < 20; ++attempt)
            {
                int tryPort = 49152 + portRng.nextInt(16000);  // ephemeral range
                if (zkTcpListener->createListener(tryPort, bindIp))
                {
                    zkTcpPort = tryPort;
                    DBG("HippoNet: Zookeeper TCP listener on port " + juce::String(zkTcpPort));
                    break;
                }
            }
            if (zkTcpPort == 0)
            {
                DBG("HippoNet: Zookeeper TCP listener failed");
                zkTcpListener = nullptr;
            }
        }

        // --- Zookeeper UDP socket on a random high port (for announcement source) ---
        zkUdpSocket = std::make_unique<juce::DatagramSocket>(false);
        zkUdpPort = 0;
        {
            juce::Random portRng;
            for (int attempt = 0; attempt < 20; ++attempt)
            {
                int tryPort = 49152 + portRng.nextInt(16000);
                if (tryPort == zkTcpPort) continue;
                if (zkUdpSocket->bindToPort(tryPort))
                {
                    zkUdpPort = tryPort;
                    auto rawSockZk = zkUdpSocket->getRawSocketHandle();
                    int bcastZk = 1;
#ifdef _WIN32
                    setsockopt(rawSockZk, SOL_SOCKET, SO_BROADCAST,
                               reinterpret_cast<const char*>(&bcastZk), sizeof(bcastZk));
#else
                    setsockopt(rawSockZk, SOL_SOCKET, SO_BROADCAST, &bcastZk, sizeof(bcastZk));
#endif
                    DBG("HippoNet: Zookeeper UDP socket on port " + juce::String(zkUdpPort));
                    break;
                }
            }
            if (zkUdpPort == 0)
            {
                DBG("HippoNet: Zookeeper UDP socket failed");
                zkUdpSocket = nullptr;
            }
        }

        // Build announcement packets (needs port numbers to be known)
        buildAnnouncementPackets();

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

        if (socket != nullptr)           socket->shutdown();
        if (discoverySocket != nullptr)  discoverySocket->shutdown();
        if (tcpListener != nullptr)      tcpListener->close();
        if (zkTcpListener != nullptr)    zkTcpListener->close();
        if (zkUdpSocket != nullptr)      zkUdpSocket->shutdown();

        if (isThreadRunning())
            stopThread(2000);

        socket = nullptr;
        discoverySocket = nullptr;
        tcpListener = nullptr;
        zkTcpListener = nullptr;
        zkUdpSocket = nullptr;

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
        double lastAnnounceTime = 0.0;
        static constexpr double kAnnounceIntervalMs = 3000.0;

        // Active TCP connections — each tracks handshake and drip-feed state
        struct TcpClient {
            std::unique_ptr<juce::StreamingSocket> sock;
            bool handshakeSent = false;
            bool isMainConnection = false;
            int totalReceived = 0;
            // Reactive drip-feed: send node messages in response to Hippo data
            bool nodeFeedStarted = false;
            int nodeFeedIdx = 0;                     // next message to send (0..78)
            std::vector<uint8_t> preparedNodeData;   // GUID-replaced copy
            bool subscribesSent = false;
            double lastKeepaliveTime = 0.0;
        };
        std::vector<TcpClient> tcpClients;

        while (!threadShouldExit() && isRunningFlag.load(std::memory_order_relaxed))
        {
            double now = juce::Time::getMillisecondCounterHiRes();

            // --- Send periodic announcements on port 6092 ---
            if (now - lastAnnounceTime >= kAnnounceIntervalMs)
            {
                // HippoEngineHost announcement from port 6091
                auto* sock = socket.get();
                if (sock != nullptr && !announcePacket.empty())
                    sock->write(announceBroadcastIp, kAnnouncePort,
                                announcePacket.data(), (int)announcePacket.size());

                // Zookeeper announcement from random UDP port
                auto* zkSock = zkUdpSocket.get();
                if (zkSock != nullptr && !zkAnnouncePacket.empty())
                    zkSock->write(announceBroadcastIp, kAnnouncePort,
                                  zkAnnouncePacket.data(), (int)zkAnnouncePacket.size());

                lastAnnounceTime = now;
            }

            // --- Accept ALL pending TCP connections on BOTH listeners ---
            for (auto* listener : { tcpListener.get(), zkTcpListener.get() })
            {
                if (listener == nullptr) continue;
                while (listener->waitUntilReady(true, 0))
                {
                    auto client = std::unique_ptr<juce::StreamingSocket>(listener->waitForNextConnection());
                    if (client != nullptr)
                    {
                        DBG("HippoNet: TCP connection accepted on port " + juce::String(listener->getPort()));
                        tcpClients.push_back({ std::move(client), false });
                    }
                }
            }

            // --- Process all TCP clients: read data, respond immediately ---
            for (auto it = tcpClients.begin(); it != tcpClients.end(); )
            {
                auto& c = *it;
                if (c.sock == nullptr || !c.sock->isConnected())
                {
                    it = tcpClients.erase(it);
                    continue;
                }

                if (c.sock->waitUntilReady(true, 0))
                {
                    uint8_t tcpBuf[16384];
                    int n = c.sock->read(tcpBuf, sizeof(tcpBuf), false);
                    if (n <= 0)
                    {
                        it = tcpClients.erase(it);
                        continue;
                    }

                    c.totalReceived += n;

                    // Extract Hippotizer's Zookeeper UDP port from any announcement
                    // that contains "Zookeeper" in the name (needed for UDP config exchange)
                    if (hippoZkUdpPort == 0)
                    {
                        for (int i = 0; i + 30 <= n; ++i)
                        {
                            if (tcpBuf[i] == 0xC6 && tcpBuf[i+1] == 0x5E && tcpBuf[i+2] == 0xE5 && tcpBuf[i+3] == 0x00)
                            {
                                // Check if name contains "Zookeeper"
                                int pktLen = (int)tcpBuf[i+6] | ((int)tcpBuf[i+7] << 8);
                                if (i + pktLen <= n)
                                {
                                    for (int j = i + 52; j + 9 <= i + pktLen; ++j)
                                    {
                                        if (std::memcmp(tcpBuf + j, "Zookeeper", 9) == 0)
                                        {
                                            // UDP port at block offset 10-11 (block starts at i+24)
                                            hippoZkUdpPort = (int)tcpBuf[i+34] | ((int)tcpBuf[i+35] << 8);
                                            DBG("HippoNet: discovered Hippotizer Zookeeper UDP port " + juce::String(hippoZkUdpPort));
                                            break;
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // Check for HippoNet announcement on EVERY data read.
                    // The 8-byte handshake and the announcement may arrive in separate reads,
                    // so we can't decide main vs keepalive on the first packet alone.
                    {
                        bool hasAnnouncement = false;
                        for (int i = 0; i + 4 <= n; ++i)
                        {
                            if (tcpBuf[i] == 0xC6 && tcpBuf[i+1] == 0x5E && tcpBuf[i+2] == 0xE5 && tcpBuf[i+3] == 0x00)
                            {
                                hasAnnouncement = true;
                                break;
                            }
                        }

                        if (hasAnnouncement && !c.isMainConnection && !announcePacket.empty())
                        {
                            if (!c.handshakeSent)
                                c.sock->write(announcePacket.data(), (int)announcePacket.size());
                            c.handshakeSent = true;
                            c.isMainConnection = true;
                            DBG("HippoNet: TCP announcement sent on main connection");
                        }
                    }

                    // If no announcement seen after receiving some data, treat as keepalive.
                    // Only send announcement, NOT node tree — only main connection gets that.
                    if (!c.handshakeSent && c.totalReceived >= 8)
                    {
                        bool otherIsMain = false;
                        for (auto& other : tcpClients)
                            if (&other != &c && other.isMainConnection) { otherIsMain = true; break; }

                        if (otherIsMain)
                        {
                            if (!announcePacket.empty())
                                c.sock->write(announcePacket.data(), (int)announcePacket.size());
                            c.handshakeSent = true;
                            // NOT isMainConnection — keepalive only gets announcement
                            DBG("HippoNet: TCP keepalive connection: sent announcement only");
                        }
                    }

                    // --- Reactive node tree exchange: emulate PLAY2's behavior ---
                    // PLAY2 sends nodes IN RESPONSE to receiving data from the Hippo.
                    // Each time the Hippo sends us data (nodes, TYPE4s, keepalives),
                    // we ack TYPE4s and send our next batch of nodes.

                    // Step 1: After receiving HIPPO's node tree (>1KB), prepare buffer
                    if (c.isMainConnection && !c.nodeFeedStarted && c.totalReceived > 1000)
                    {
                        static const uint8_t kPlay2Guid[16] = {
                            0x4C,0x5B,0xFE,0xA0,0xBE,0x76,0x17,0x4C,
                            0x82,0x8C,0x23,0x8E,0xFE,0x96,0x06,0x63
                        };
                        static const uint8_t kCapturedSyncGuid[16] = {
                            0xDC,0x83,0xBF,0xB3,0x3B,0x63,0x96,0x41,
                            0xBD,0x77,0xF5,0x48,0xEA,0x0A,0xCF,0x12
                        };
                        c.preparedNodeData.assign(kHippoNodeTreeData,
                                                   kHippoNodeTreeData + sizeof(kHippoNodeTreeData));
                        for (size_t i = 0; i + 16 <= c.preparedNodeData.size(); ++i)
                        {
                            if (std::memcmp(c.preparedNodeData.data() + i, kPlay2Guid, 16) == 0)
                                std::memcpy(c.preparedNodeData.data() + i, sessionGuid, 16);
                            else if (std::memcmp(c.preparedNodeData.data() + i, kCapturedSyncGuid, 16) == 0)
                                std::memcpy(c.preparedNodeData.data() + i, syncManagerGuid, 16);
                        }
                        c.nodeFeedStarted = true;
                        c.nodeFeedIdx = 0;
                        DBG("HippoNet: reactive node exchange started (" +
                            juce::String((int)c.preparedNodeData.size()) + "B, " +
                            juce::String(kNodeMsgCount) + " messages)");
                    }

                    // Step 2: Ack TYPE4 subscribes + send next node batch (reactive)
                    if (c.isMainConnection && c.nodeFeedStarted)
                    {
                        // 2a. Ack every TYPE4 with TYPE5.
                        // STATUS rule from capture: flags=-3 (0xFFFFFFFD at offset 16)
                        // → status 0xFFFFFFFF (node not hosted). Otherwise → status 1.
                        for (int off = 0; off + 44 <= n; )
                        {
                            if (tcpBuf[off] == 0x04 && tcpBuf[off+1] == 0x00
                                && tcpBuf[off+2] == 0x2C && tcpBuf[off+3] == 0x00)
                            {
                                // Check flags at offset 16: FD FF FF FF = -3 → reject
                                bool reject = (tcpBuf[off+16] == 0xFD && tcpBuf[off+17] == 0xFF
                                            && tcpBuf[off+18] == 0xFF && tcpBuf[off+19] == 0xFF);
                                uint8_t ack[12] = { 0x05, 0x00, 0x0C, 0x00,
                                                    tcpBuf[off+12], tcpBuf[off+13],
                                                    tcpBuf[off+14], tcpBuf[off+15],
                                                    0x00, 0x00, 0x00, 0x00 };
                                if (reject) {
                                    ack[8] = 0xFF; ack[9] = 0xFF; ack[10] = 0xFF; ack[11] = 0xFF;
                                } else {
                                    ack[8] = 0x01; ack[9] = 0x00; ack[10] = 0x00; ack[11] = 0x00;
                                }
                                c.sock->write(ack, 12);
                                off += 44;
                            }
                            else
                            {
                                ++off;
                            }
                        }

                        // 2b. Send next node batch — triggered by receiving ANY data.
                        // PLAY2 sends: 1 msg first, then 2, then batches of 3-5.
                        if (c.nodeFeedIdx < kNodeMsgCount)
                        {
                            int batchSize;
                            if (c.nodeFeedIdx == 0) batchSize = 1;       // Host Manager alone
                            else if (c.nodeFeedIdx < 3) batchSize = 2;   // String + Settings
                            else batchSize = 5;                           // larger batches

                            for (int i = 0; i < batchSize && c.nodeFeedIdx < kNodeMsgCount; ++i)
                            {
                                int moff = kNodeMsgTable[c.nodeFeedIdx][0];
                                int mlen = kNodeMsgTable[c.nodeFeedIdx][1];
                                c.sock->write(c.preparedNodeData.data() + moff, mlen);
                                c.nodeFeedIdx++;
                            }
                            #if JUCE_DEBUG
                            if (c.nodeFeedIdx >= kNodeMsgCount)
                                DBG("HippoNet: all " + juce::String(kNodeMsgCount) + " node messages sent");
                            #endif
                        }

                        // Step 3: After all nodes sent, subscribe to Hippo's nodes 1-25
                        // using our Sync Manager GUID. In the working capture, PLAY2 sends
                        // 25 TYPE4 subscribes with GUID dc83bfb3... to trigger TC flow.
                        // The Sync Manager GUID is NOT declared in the node tree — it's
                        // only used for subscribes.
                        if (c.nodeFeedIdx >= kNodeMsgCount && !c.subscribesSent)
                        {
                            sendSubscribes(*c.sock);
                            c.subscribesSent = true;
                        }
                    }
                }

                // Send TCP keepalive every ~5s on all handshaked connections
                if (c.handshakeSent && now - c.lastKeepaliveTime >= 5000.0)
                {
                    static const uint8_t ka[4] = { 0x08, 0x00, 0x04, 0x00 };
                    c.sock->write(ka, 4);
                    c.lastKeepaliveTime = now;
                }

                ++it;
            }

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

        // HippoNet packet: 24-byte header + variable-length TC blocks.
        // Block format: header(2) + length(2) + type(4) + channelId(4) + metadata + TC(s)
        // Block[2:4] = block length as uint16 LE (self-describing).
        //
        // Hippotizer PLAY:  18-byte blocks, 1 TC at block+14  (last 4 bytes)
        // Real Hippotizer:  26-byte blocks, 2 TCs at block+18 and block+22  (last 8 bytes)

        const int kHeaderSize = 24;

        struct TcValue { uint32_t ms; int index; };
        std::vector<TcValue> tcValues;

        int off = kHeaderSize;
        while (off + 4 <= size)
        {
            // Read block length from bytes 2-3 of the block
            if (off + 4 > size) break;
            uint16_t blockLen = (uint16_t)data[off + 2] | ((uint16_t)data[off + 3] << 8);

            // Sanity: block length must be at least 18 and fit in packet
            if (blockLen < 18 || off + blockLen > size)
                break;

            // Extract TC values from the end of the block.
            // Last 4 bytes = always a TC. If block >= 22 bytes, preceding 4 bytes = second TC.
            int tcIdx = (int)tcValues.size();

            if (blockLen >= 22)
            {
                // Two TCs: at (block + blockLen - 8) and (block + blockLen - 4)
                int off1 = off + blockLen - 8;
                int off2 = off + blockLen - 4;
                uint32_t ms1 = (uint32_t)data[off1] | ((uint32_t)data[off1+1]<<8) | ((uint32_t)data[off1+2]<<16) | ((uint32_t)data[off1+3]<<24);
                uint32_t ms2 = (uint32_t)data[off2] | ((uint32_t)data[off2+1]<<8) | ((uint32_t)data[off2+2]<<16) | ((uint32_t)data[off2+3]<<24);
                if (ms1 <= 86400000u) tcValues.push_back({ ms1, tcIdx });
                if (ms2 <= 86400000u) tcValues.push_back({ ms2, tcIdx + 1 });
            }
            else
            {
                // Single TC: last 4 bytes
                int off1 = off + blockLen - 4;
                uint32_t ms1 = (uint32_t)data[off1] | ((uint32_t)data[off1+1]<<8) | ((uint32_t)data[off1+2]<<16) | ((uint32_t)data[off1+3]<<24);
                if (ms1 <= 86400000u) tcValues.push_back({ ms1, tcIdx });
            }

            off += blockLen;
        }

        // Deduplicate by ms value: keep first occurrence of each unique ms
        std::vector<TcValue> unique;
        for (auto& v : tcValues)
        {
            bool dup = false;
            for (auto& u : unique)
                if (u.ms == v.ms) { dup = true; break; }
            if (!dup)
                unique.push_back(v);
        }

        // Reverse order: Hippotizer's internal block order is inverted
        // relative to the TC1/TC2 numbering in its UI
        std::reverse(unique.begin(), unique.end());

        // Update channel list (lock) — use index as synthetic channel ID
        {
            const juce::SpinLock::ScopedLockType sl(channelLock);

            // Resize channels to match discovered count
            while (channels.size() < unique.size())
                channels.push_back({ (uint32_t)channels.size(), 0, 0 });

            for (size_t i = 0; i < unique.size(); ++i)
                channels[i].lastMs = unique[i].ms;
        }

        // Backward compat
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
    std::unique_ptr<juce::StreamingSocket> tcpListener;
    std::unique_ptr<juce::StreamingSocket> zkTcpListener;
    std::unique_ptr<juce::DatagramSocket> zkUdpSocket;
    juce::String bindIp = "0.0.0.0";
    int listenPort = 6091;
    int zkTcpPort = 0;
    int zkUdpPort = 0;
    int selectedInterface = 0;
    std::atomic<bool> isRunningFlag { false };
    std::atomic<bool> bindFellBack { false };

    juce::Array<NetworkInterface> availableInterfaces;
    std::atomic<double> lastPacketTime { 0.0 };
    std::atomic<uint32_t> msSinceMidnight { 0 };

    // Announcement packets for HippoNet peer discovery (broadcast to port 6092)
    static constexpr int kAnnouncePort = 6092;
    std::vector<uint8_t> announcePacket;      // "STC,HippoEngineHost" from port 6091
    std::vector<uint8_t> zkAnnouncePacket;    // "STC - Zookeeper,Zookeeper" from random port
    juce::String announceBroadcastIp { "255.255.255.255" };
    uint8_t sessionGuid[16] = {};
    uint8_t zkSessionGuid[16] = {};
    uint8_t syncManagerGuid[16] = {};  // used in node tree + TYPE4 subscribes

    /// Build a HippoNet announcement packet.
    /// Block[8:10] = TCP listen port (uint16 LE), Block[10:12] = UDP source port (uint16 LE)
    static std::vector<uint8_t> buildAnnouncePacket(const uint8_t guid[16], const uint8_t innerGuid[16],
                                                     const juce::String& ip, int tcpPort, int udpPort,
                                                     const char* name)
    {
        int nameLen = (int)std::strlen(name);
        int blockLen = 2 + 2 + 4 + 4 + 16 + nameLen;  // type + len + IP + ports + GUID + name
        int pktLen   = 24 + blockLen;

        std::vector<uint8_t> pkt((size_t)pktLen, 0);
        auto* p = pkt.data();

        // Header
        p[0] = 0xC6; p[1] = 0x5E; p[2] = 0xE5; p[3] = 0x00;
        p[4] = 0x6A; p[5] = 0x00;
        p[6] = (uint8_t)(pktLen & 0xFF); p[7] = (uint8_t)((pktLen >> 8) & 0xFF);
        std::memcpy(p + 8, guid, 16);

        // Block
        int b = 24;
        p[b] = 0x01; p[b+1] = 0x00;  // block type = announcement
        p[b+2] = (uint8_t)(blockLen & 0xFF); p[b+3] = (uint8_t)((blockLen >> 8) & 0xFF);

        // IP (big-endian)
        auto ipParts = juce::StringArray::fromTokens(ip, ".", "");
        if (ipParts.size() == 4)
        {
            p[b+4] = (uint8_t)ipParts[0].getIntValue();
            p[b+5] = (uint8_t)ipParts[1].getIntValue();
            p[b+6] = (uint8_t)ipParts[2].getIntValue();
            p[b+7] = (uint8_t)ipParts[3].getIntValue();
        }

        // TCP listen port + UDP source port (uint16 LE each)
        p[b+8]  = (uint8_t)(tcpPort & 0xFF);
        p[b+9]  = (uint8_t)((tcpPort >> 8) & 0xFF);
        p[b+10] = (uint8_t)(udpPort & 0xFF);
        p[b+11] = (uint8_t)((udpPort >> 8) & 0xFF);

        // Inner GUID
        std::memcpy(p + b + 12, innerGuid, 16);

        // Name string (no null terminator)
        std::memcpy(p + b + 28, name, (size_t)nameLen);

        return pkt;
    }

    void buildAnnouncementPackets()
    {
        // Generate random GUIDs (once per session)
        juce::Random rng;
        uint8_t innerGuid1[16], innerGuid2[16];
        for (int i = 0; i < 16; ++i) sessionGuid[i]   = (uint8_t)rng.nextInt(256);
        for (int i = 0; i < 16; ++i) zkSessionGuid[i]  = (uint8_t)rng.nextInt(256);
        for (int i = 0; i < 16; ++i) innerGuid1[i]     = (uint8_t)rng.nextInt(256);
        for (int i = 0; i < 16; ++i) innerGuid2[i]     = (uint8_t)rng.nextInt(256);
        for (int i = 0; i < 16; ++i) syncManagerGuid[i] = (uint8_t)rng.nextInt(256);

        // Resolve our IP
        juce::String ip = bindIp;
        if (ip == "0.0.0.0" && !availableInterfaces.isEmpty())
            ip = availableInterfaces[0].ip;

        // HippoEngineHost: TCP=6091, UDP=6091
        announcePacket = buildAnnouncePacket(sessionGuid, innerGuid1,
                                             ip, listenPort, listenPort,
                                             "STC,HippoEngineHost");

        // Zookeeper: TCP=zkTcpPort, UDP=zkUdpPort
        if (zkTcpPort > 0 && zkUdpPort > 0)
        {
            zkAnnouncePacket = buildAnnouncePacket(zkSessionGuid, innerGuid2,
                                                    ip, zkTcpPort, zkUdpPort,
                                                    "STC - Zookeeper,Zookeeper");
        }
    }

    // Multi-layer channel tracking
    struct DiscoveredChannel { uint32_t channelId; uint32_t lastMs; int type; };
    mutable juce::SpinLock channelLock;
    std::vector<DiscoveredChannel> channels;
    int selectedTcIndex = 0;

    /// Send UDP config exchange packets to the Hippotizer's Zookeeper UDP port.
    /// Captured from a working PLAY session — 13 packets, 2222 bytes total.
    /// PLAY2's GUID is replaced with ours.
    void sendUdpConfig(const juce::String& hippoIp, int zkPort)
    {
        static const uint8_t kUdpConfigData[2222] = {
            0xC6,0x5E,0xE5,0x00,0x6A,0x00,0x63,0x00,0x4C,0x5B,0xFE,0xA0,0xBE,0x76,0x17,0x4C,0x82,0x8C,0x23,0x8E,
            0xFE,0x96,0x06,0x63,0x06,0x00,0x4B,0x00,0x01,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x3D,0x00,0x3C,0x43,
            0x6F,0x6D,0x70,0x53,0x74,0x61,0x74,0x75,0x73,0x20,0x73,0x74,0x61,0x74,0x65,0x3D,0x22,0x52,0x75,0x6E,
            0x22,0x20,0x69,0x6E,0x66,0x6F,0x3D,0x22,0x22,0x20,0x74,0x69,0x6D,0x65,0x73,0x74,0x61,0x6D,0x70,0x3D,
            0x22,0x34,0x31,0x31,0x32,0x39,0x39,0x35,0x35,0x32,0x33,0x33,0x30,0x30,0x22,0x2F,0x3E,0x0D,0x0A,0xC6,
            0x5E,0xE5,0x00,0x6A,0x00,0xC4,0x00,0x4C,0x5B,0xFE,0xA0,0xBE,0x76,0x17,0x4C,0x82,0x8C,0x23,0x8E,0xFE,
            0x96,0x06,0x63,0x06,0x00,0x61,0x00,0x03,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x53,0x00,0x3C,0x43,0x6F,
            0x6D,0x70,0x53,0x74,0x61,0x74,0x75,0x73,0x20,0x73,0x74,0x61,0x74,0x65,0x3D,0x22,0x43,0x6F,0x6E,0x66,
            0x69,0x67,0x22,0x20,0x69,0x6E,0x66,0x6F,0x3D,0x22,0x49,0x6E,0x69,0x74,0x69,0x61,0x6C,0x69,0x7A,0x69,
            0x6E,0x67,0x20,0x65,0x6E,0x67,0x69,0x6E,0x65,0x22,0x20,0x74,0x69,0x6D,0x65,0x73,0x74,0x61,0x6D,0x70,
            0x3D,0x22,0x34,0x31,0x31,0x33,0x35,0x31,0x35,0x39,0x34,0x38,0x34,0x30,0x30,0x22,0x2F,0x3E,0x0D,0x0A,
            0x06,0x00,0x4B,0x00,0x02,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x3D,0x00,0x3C,0x43,0x6F,0x6D,0x70,0x53,
            0x74,0x61,0x74,0x75,0x73,0x20,0x73,0x74,0x61,0x74,0x65,0x3D,0x22,0x52,0x75,0x6E,0x22,0x20,0x69,0x6E,
            0x66,0x6F,0x3D,0x22,0x22,0x20,0x74,0x69,0x6D,0x65,0x73,0x74,0x61,0x6D,0x70,0x3D,0x22,0x34,0x31,0x31,
            0x33,0x34,0x34,0x34,0x34,0x34,0x30,0x38,0x30,0x30,0x22,0x2F,0x3E,0x0D,0x0A,0xC6,0x5E,0xE5,0x00,0x6A,
            0x00,0x63,0x00,0x4C,0x5B,0xFE,0xA0,0xBE,0x76,0x17,0x4C,0x82,0x8C,0x23,0x8E,0xFE,0x96,0x06,0x63,0x06,
            0x00,0x4B,0x00,0x02,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x3D,0x00,0x3C,0x43,0x6F,0x6D,0x70,0x53,0x74,
            0x61,0x74,0x75,0x73,0x20,0x73,0x74,0x61,0x74,0x65,0x3D,0x22,0x52,0x75,0x6E,0x22,0x20,0x69,0x6E,0x66,
            0x6F,0x3D,0x22,0x22,0x20,0x74,0x69,0x6D,0x65,0x73,0x74,0x61,0x6D,0x70,0x3D,0x22,0x34,0x31,0x31,0x33,
            0x34,0x34,0x34,0x34,0x34,0x30,0x38,0x30,0x30,0x22,0x2F,0x3E,0x0D,0x0A,0xC6,0x5E,0xE5,0x00,0x6A,0x00,
            0x71,0x00,0x4C,0x5B,0xFE,0xA0,0xBE,0x76,0x17,0x4C,0x82,0x8C,0x23,0x8E,0xFE,0x96,0x06,0x63,0x06,0x00,
            0x59,0x00,0x03,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x4B,0x00,0x3C,0x43,0x6F,0x6D,0x70,0x53,0x74,0x61,
            0x74,0x75,0x73,0x20,0x73,0x74,0x61,0x74,0x65,0x3D,0x22,0x52,0x75,0x6E,0x22,0x20,0x69,0x6E,0x66,0x6F,
            0x3D,0x22,0x45,0x6E,0x67,0x69,0x6E,0x65,0x20,0x72,0x75,0x6E,0x6E,0x69,0x6E,0x67,0x22,0x20,0x74,0x69,
            0x6D,0x65,0x73,0x74,0x61,0x6D,0x70,0x3D,0x22,0x34,0x31,0x31,0x36,0x35,0x32,0x32,0x35,0x34,0x34,0x35,
            0x30,0x30,0x22,0x2F,0x3E,0x0D,0x0A,0xC6,0x5E,0xE5,0x00,0x6A,0x00,0x45,0x01,0x4C,0x5B,0xFE,0xA0,0xBE,
            0x76,0x17,0x4C,0x82,0x8C,0x23,0x8E,0xFE,0x96,0x06,0x63,0x06,0x00,0x12,0x00,0x03,0x00,0x00,0x00,0x36,
            0x00,0x00,0x00,0x04,0x00,0x01,0x00,0x00,0x00,0x06,0x00,0x1B,0x01,0x03,0x00,0x00,0x00,0x37,0x00,0x00,
            0x00,0x0D,0x01,0x3C,0x56,0x61,0x72,0x49,0x6E,0x66,0x6F,0x20,0x6C,0x61,0x62,0x65,0x6C,0x3D,0x22,0x52,
            0x65,0x66,0x72,0x65,0x73,0x68,0x52,0x61,0x74,0x65,0x22,0x3E,0x0D,0x0A,0x3C,0x51,0x75,0x69,0x63,0x6B,
            0x49,0x6E,0x66,0x6F,0x3E,0x43,0x75,0x72,0x72,0x65,0x6E,0x74,0x20,0x72,0x65,0x66,0x72,0x65,0x73,0x68,
            0x72,0x61,0x74,0x65,0x3C,0x2F,0x51,0x75,0x69,0x63,0x6B,0x49,0x6E,0x66,0x6F,0x3E,0x0D,0x0A,0x3C,0x49,
            0x6E,0x74,0x49,0x6E,0x66,0x6F,0x20,0x64,0x65,0x66,0x61,0x75,0x6C,0x74,0x3D,0x22,0x31,0x22,0x3E,0x0D,
            0x0A,0x3C,0x45,0x6E,0x75,0x6D,0x4C,0x61,0x62,0x65,0x6C,0x20,0x76,0x61,0x6C,0x75,0x65,0x3D,0x22,0x30,
            0x22,0x20,0x6C,0x61,0x62,0x65,0x6C,0x3D,0x22,0x4D,0x61,0x78,0x22,0x2F,0x3E,0x0D,0x0A,0x3C,0x45,0x6E,
            0x75,0x6D,0x4C,0x61,0x62,0x65,0x6C,0x20,0x76,0x61,0x6C,0x75,0x65,0x3D,0x22,0x32,0x22,0x20,0x6C,0x61,
            0x62,0x65,0x6C,0x3D,0x22,0x4C,0x6F,0x77,0x22,0x2F,0x3E,0x0D,0x0A,0x3C,0x45,0x6E,0x75,0x6D,0x4C,0x61,
            0x62,0x65,0x6C,0x20,0x76,0x61,0x6C,0x75,0x65,0x3D,0x22,0x31,0x22,0x20,0x6C,0x61,0x62,0x65,0x6C,0x3D,
            0x22,0x4D,0x65,0x64,0x69,0x75,0x6D,0x22,0x2F,0x3E,0x0D,0x0A,0x3C,0x45,0x6E,0x75,0x6D,0x4C,0x61,0x62,
            0x65,0x6C,0x20,0x76,0x61,0x6C,0x75,0x65,0x3D,0x22,0x33,0x22,0x20,0x6C,0x61,0x62,0x65,0x6C,0x3D,0x22,
            0x4F,0x66,0x66,0x22,0x2F,0x3E,0x0D,0x0A,0x3C,0x2F,0x49,0x6E,0x74,0x49,0x6E,0x66,0x6F,0x3E,0x0D,0x0A,
            0x3C,0x2F,0x56,0x61,0x72,0x49,0x6E,0x66,0x6F,0x3E,0x0D,0x0A,0xC6,0x5E,0xE5,0x00,0x6A,0x00,0xDA,0x00,
            0x4C,0x5B,0xFE,0xA0,0xBE,0x76,0x17,0x4C,0x82,0x8C,0x23,0x8E,0xFE,0x96,0x06,0x63,0x06,0x00,0xB3,0x00,
            0x03,0x00,0x00,0x00,0x1D,0x00,0x00,0x00,0xA5,0x00,0x3C,0x56,0x61,0x72,0x49,0x6E,0x66,0x6F,0x20,0x6C,
            0x61,0x62,0x65,0x6C,0x3D,0x22,0x48,0x69,0x67,0x68,0x50,0x65,0x72,0x66,0x6F,0x72,0x6D,0x61,0x6E,0x63,
            0x65,0x4D,0x6F,0x64,0x65,0x22,0x3E,0x0D,0x0A,0x3C,0x51,0x75,0x69,0x63,0x6B,0x49,0x6E,0x66,0x6F,0x3E,
            0x54,0x6F,0x67,0x67,0x6C,0x65,0x20,0x48,0x69,0x67,0x68,0x20,0x50,0x65,0x72,0x66,0x6F,0x72,0x6D,0x61,
            0x6E,0x63,0x65,0x20,0x6D,0x6F,0x64,0x65,0x3C,0x2F,0x51,0x75,0x69,0x63,0x6B,0x49,0x6E,0x66,0x6F,0x3E,
            0x0D,0x0A,0x3C,0x42,0x6F,0x6F,0x6C,0x49,0x6E,0x66,0x6F,0x20,0x74,0x72,0x75,0x65,0x6C,0x61,0x62,0x65,
            0x6C,0x3D,0x22,0x4F,0x6E,0x22,0x20,0x66,0x61,0x6C,0x73,0x65,0x6C,0x61,0x62,0x65,0x6C,0x3D,0x22,0x4F,
            0x66,0x66,0x22,0x20,0x64,0x65,0x66,0x61,0x75,0x6C,0x74,0x3D,0x22,0x66,0x61,0x6C,0x73,0x65,0x22,0x2F,
            0x3E,0x0D,0x0A,0x3C,0x2F,0x56,0x61,0x72,0x49,0x6E,0x66,0x6F,0x3E,0x0D,0x0A,0x06,0x00,0x0F,0x00,0x03,
            0x00,0x00,0x00,0x1C,0x00,0x00,0x00,0x01,0x00,0x00,0xC6,0x5E,0xE5,0x00,0x6A,0x00,0x68,0x00,0x4C,0x5B,
            0xFE,0xA0,0xBE,0x76,0x17,0x4C,0x82,0x8C,0x23,0x8E,0xFE,0x96,0x06,0x63,0x06,0x00,0x50,0x00,0x04,0x00,
            0x00,0x00,0x02,0x00,0x00,0x00,0x42,0x00,0x3C,0x43,0x6F,0x6D,0x70,0x53,0x74,0x61,0x74,0x75,0x73,0x20,
            0x73,0x74,0x61,0x74,0x65,0x3D,0x22,0x52,0x75,0x6E,0x22,0x20,0x69,0x6E,0x66,0x6F,0x3D,0x22,0x52,0x65,
            0x61,0x64,0x79,0x22,0x20,0x74,0x69,0x6D,0x65,0x73,0x74,0x61,0x6D,0x70,0x3D,0x22,0x34,0x31,0x31,0x39,
            0x31,0x30,0x32,0x35,0x37,0x30,0x35,0x30,0x30,0x22,0x2F,0x3E,0x0D,0x0A,0xC6,0x5E,0xE5,0x00,0x6A,0x00,
            0x63,0x00,0x4C,0x5B,0xFE,0xA0,0xBE,0x76,0x17,0x4C,0x82,0x8C,0x23,0x8E,0xFE,0x96,0x06,0x63,0x06,0x00,
            0x4B,0x00,0x05,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x3D,0x00,0x3C,0x43,0x6F,0x6D,0x70,0x53,0x74,0x61,
            0x74,0x75,0x73,0x20,0x73,0x74,0x61,0x74,0x65,0x3D,0x22,0x52,0x75,0x6E,0x22,0x20,0x69,0x6E,0x66,0x6F,
            0x3D,0x22,0x22,0x20,0x74,0x69,0x6D,0x65,0x73,0x74,0x61,0x6D,0x70,0x3D,0x22,0x34,0x31,0x32,0x32,0x35,
            0x38,0x34,0x32,0x32,0x39,0x37,0x30,0x30,0x22,0x2F,0x3E,0x0D,0x0A,0xC6,0x5E,0xE5,0x00,0x6A,0x00,0xC7,
            0x00,0x4C,0x5B,0xFE,0xA0,0xBE,0x76,0x17,0x4C,0x82,0x8C,0x23,0x8E,0xFE,0x96,0x06,0x63,0x06,0x00,0x64,
            0x00,0x07,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x56,0x00,0x3C,0x43,0x6F,0x6D,0x70,0x53,0x74,0x61,0x74,
            0x75,0x73,0x20,0x73,0x74,0x61,0x74,0x65,0x3D,0x22,0x4C,0x69,0x6D,0x69,0x74,0x65,0x64,0x22,0x20,0x69,
            0x6E,0x66,0x6F,0x3D,0x22,0x57,0x61,0x69,0x74,0x69,0x6E,0x67,0x20,0x66,0x6F,0x72,0x20,0x45,0x6E,0x67,
            0x69,0x6E,0x65,0x2E,0x2E,0x2E,0x22,0x20,0x74,0x69,0x6D,0x65,0x73,0x74,0x61,0x6D,0x70,0x3D,0x22,0x34,
            0x31,0x32,0x32,0x37,0x33,0x38,0x35,0x31,0x36,0x37,0x30,0x30,0x22,0x2F,0x3E,0x0D,0x0A,0x06,0x00,0x4B,
            0x00,0x06,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x3D,0x00,0x3C,0x43,0x6F,0x6D,0x70,0x53,0x74,0x61,0x74,
            0x75,0x73,0x20,0x73,0x74,0x61,0x74,0x65,0x3D,0x22,0x52,0x75,0x6E,0x22,0x20,0x69,0x6E,0x66,0x6F,0x3D,
            0x22,0x22,0x20,0x74,0x69,0x6D,0x65,0x73,0x74,0x61,0x6D,0x70,0x3D,0x22,0x34,0x31,0x32,0x32,0x36,0x38,
            0x34,0x38,0x33,0x31,0x33,0x30,0x30,0x22,0x2F,0x3E,0x0D,0x0A,0xC6,0x5E,0xE5,0x00,0x6A,0x00,0x63,0x00,
            0x4C,0x5B,0xFE,0xA0,0xBE,0x76,0x17,0x4C,0x82,0x8C,0x23,0x8E,0xFE,0x96,0x06,0x63,0x06,0x00,0x4B,0x00,
            0x08,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x3D,0x00,0x3C,0x43,0x6F,0x6D,0x70,0x53,0x74,0x61,0x74,0x75,
            0x73,0x20,0x73,0x74,0x61,0x74,0x65,0x3D,0x22,0x52,0x75,0x6E,0x22,0x20,0x69,0x6E,0x66,0x6F,0x3D,0x22,
            0x22,0x20,0x74,0x69,0x6D,0x65,0x73,0x74,0x61,0x6D,0x70,0x3D,0x22,0x34,0x31,0x32,0x32,0x38,0x32,0x31,
            0x31,0x30,0x38,0x31,0x30,0x30,0x22,0x2F,0x3E,0x0D,0x0A,0xC6,0x5E,0xE5,0x00,0x6A,0x00,0x63,0x00,0x4C,
            0x5B,0xFE,0xA0,0xBE,0x76,0x17,0x4C,0x82,0x8C,0x23,0x8E,0xFE,0x96,0x06,0x63,0x06,0x00,0x4B,0x00,0x06,
            0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x3D,0x00,0x3C,0x43,0x6F,0x6D,0x70,0x53,0x74,0x61,0x74,0x75,0x73,
            0x20,0x73,0x74,0x61,0x74,0x65,0x3D,0x22,0x52,0x75,0x6E,0x22,0x20,0x69,0x6E,0x66,0x6F,0x3D,0x22,0x22,
            0x20,0x74,0x69,0x6D,0x65,0x73,0x74,0x61,0x6D,0x70,0x3D,0x22,0x34,0x31,0x32,0x32,0x36,0x38,0x34,0x38,
            0x33,0x31,0x33,0x30,0x30,0x22,0x2F,0x3E,0x0D,0x0A,0xC6,0x5E,0xE5,0x00,0x6A,0x00,0x1E,0x01,0x4C,0x5B,
            0xFE,0xA0,0xBE,0x76,0x17,0x4C,0x82,0x8C,0x23,0x8E,0xFE,0x96,0x06,0x63,0x06,0x00,0x59,0x00,0x03,0x00,
            0x00,0x00,0x02,0x00,0x00,0x00,0x4B,0x00,0x3C,0x43,0x6F,0x6D,0x70,0x53,0x74,0x61,0x74,0x75,0x73,0x20,
            0x73,0x74,0x61,0x74,0x65,0x3D,0x22,0x52,0x75,0x6E,0x22,0x20,0x69,0x6E,0x66,0x6F,0x3D,0x22,0x45,0x6E,
            0x67,0x69,0x6E,0x65,0x20,0x72,0x75,0x6E,0x6E,0x69,0x6E,0x67,0x22,0x20,0x74,0x69,0x6D,0x65,0x73,0x74,
            0x61,0x6D,0x70,0x3D,0x22,0x34,0x31,0x31,0x36,0x35,0x32,0x32,0x35,0x34,0x34,0x35,0x30,0x30,0x22,0x2F,
            0x3E,0x0D,0x0A,0x06,0x00,0x12,0x00,0x03,0x00,0x00,0x00,0x7D,0x09,0x00,0x00,0x04,0x00,0x00,0x00,0x80,
            0x3F,0x06,0x00,0x9B,0x00,0x03,0x00,0x00,0x00,0x7E,0x09,0x00,0x00,0x8D,0x00,0x3C,0x56,0x61,0x72,0x49,
            0x6E,0x66,0x6F,0x20,0x6C,0x61,0x62,0x65,0x6C,0x3D,0x22,0x4C,0x65,0x76,0x65,0x6C,0x22,0x3E,0x0D,0x0A,
            0x3C,0x51,0x75,0x69,0x63,0x6B,0x49,0x6E,0x66,0x6F,0x3E,0x4C,0x65,0x76,0x65,0x6C,0x20,0x66,0x6C,0x6F,
            0x61,0x74,0x20,0x70,0x61,0x72,0x61,0x6D,0x65,0x74,0x65,0x72,0x3C,0x2F,0x51,0x75,0x69,0x63,0x6B,0x49,
            0x6E,0x66,0x6F,0x3E,0x0D,0x0A,0x3C,0x46,0x6C,0x6F,0x61,0x74,0x49,0x6E,0x66,0x6F,0x20,0x6D,0x69,0x6E,
            0x64,0x69,0x73,0x70,0x6C,0x61,0x79,0x3D,0x22,0x30,0x22,0x20,0x6D,0x61,0x78,0x64,0x69,0x73,0x70,0x6C,
            0x61,0x79,0x3D,0x22,0x31,0x30,0x30,0x22,0x20,0x64,0x65,0x66,0x61,0x75,0x6C,0x74,0x3D,0x22,0x31,0x22,
            0x2F,0x3E,0x0D,0x0A,0x3C,0x2F,0x56,0x61,0x72,0x49,0x6E,0x66,0x6F,0x3E,0x0D,0x0A,0xC6,0x5E,0xE5,0x00,
            0x6A,0x00,0x1E,0x01,0x4C,0x5B,0xFE,0xA0,0xBE,0x76,0x17,0x4C,0x82,0x8C,0x23,0x8E,0xFE,0x96,0x06,0x63,
            0x06,0x00,0x12,0x00,0x03,0x00,0x00,0x00,0x7D,0x09,0x00,0x00,0x04,0x00,0x00,0x00,0x80,0x3F,0x06,0x00,
            0x9B,0x00,0x03,0x00,0x00,0x00,0x7E,0x09,0x00,0x00,0x8D,0x00,0x3C,0x56,0x61,0x72,0x49,0x6E,0x66,0x6F,
            0x20,0x6C,0x61,0x62,0x65,0x6C,0x3D,0x22,0x4C,0x65,0x76,0x65,0x6C,0x22,0x3E,0x0D,0x0A,0x3C,0x51,0x75,
            0x69,0x63,0x6B,0x49,0x6E,0x66,0x6F,0x3E,0x4C,0x65,0x76,0x65,0x6C,0x20,0x66,0x6C,0x6F,0x61,0x74,0x20,
            0x70,0x61,0x72,0x61,0x6D,0x65,0x74,0x65,0x72,0x3C,0x2F,0x51,0x75,0x69,0x63,0x6B,0x49,0x6E,0x66,0x6F,
            0x3E,0x0D,0x0A,0x3C,0x46,0x6C,0x6F,0x61,0x74,0x49,0x6E,0x66,0x6F,0x20,0x6D,0x69,0x6E,0x64,0x69,0x73,
            0x70,0x6C,0x61,0x79,0x3D,0x22,0x30,0x22,0x20,0x6D,0x61,0x78,0x64,0x69,0x73,0x70,0x6C,0x61,0x79,0x3D,
            0x22,0x31,0x30,0x30,0x22,0x20,0x64,0x65,0x66,0x61,0x75,0x6C,0x74,0x3D,0x22,0x31,0x22,0x2F,0x3E,0x0D,
            0x0A,0x3C,0x2F,0x56,0x61,0x72,0x49,0x6E,0x66,0x6F,0x3E,0x0D,0x0A,0x06,0x00,0x59,0x00,0x03,0x00,0x00,
            0x00,0x02,0x00,0x00,0x00,0x4B,0x00,0x3C,0x43,0x6F,0x6D,0x70,0x53,0x74,0x61,0x74,0x75,0x73,0x20,0x73,
            0x74,0x61,0x74,0x65,0x3D,0x22,0x52,0x75,0x6E,0x22,0x20,0x69,0x6E,0x66,0x6F,0x3D,0x22,0x45,0x6E,0x67,
            0x69,0x6E,0x65,0x20,0x72,0x75,0x6E,0x6E,0x69,0x6E,0x67,0x22,0x20,0x74,0x69,0x6D,0x65,0x73,0x74,0x61,
            0x6D,0x70,0x3D,0x22,0x34,0x31,0x31,0x36,0x35,0x32,0x32,0x35,0x34,0x34,0x35,0x30,0x30,0x22,0x2F,0x3E,
            0x0D,0x0A
        };
        static const int kUdpConfigOffsets[13][2] = {
            {0, 99}, {99, 196}, {295, 99}, {394, 113}, {507, 325},
            {832, 218}, {1050, 104}, {1154, 99}, {1253, 199},
            {1452, 99}, {1551, 99}, {1650, 286}, {1936, 286}
        };
        static const uint8_t kPlay2Guid[16] = {
            0x4C,0x5B,0xFE,0xA0,0xBE,0x76,0x17,0x4C,0x82,0x8C,0x23,0x8E,0xFE,0x96,0x06,0x63
        };

        // Send from our main UDP socket (port 6091), same as PLAY2 does
        if (socket != nullptr)
        {
            for (int i = 0; i < 13; ++i)
            {
                std::vector<uint8_t> pkt(kUdpConfigData + kUdpConfigOffsets[i][0],
                                         kUdpConfigData + kUdpConfigOffsets[i][0] + kUdpConfigOffsets[i][1]);

                for (size_t j = 0; j + 16 <= pkt.size(); ++j)
                {
                    if (std::memcmp(pkt.data() + j, kPlay2Guid, 16) == 0)
                        std::memcpy(pkt.data() + j, sessionGuid, 16);
                }

                socket->write(hippoIp, zkPort, pkt.data(), (int)pkt.size());
            }
        }

        DBG("HippoNet: sent 13 UDP config packets (2222B) to " + hippoIp + ":" + juce::String(zkPort));
    }


    /// Send TYPE4 subscribe messages for nodes 1-25 on the main TCP connection.
    /// In working captures, PLAY2 creates a "Sync Manager" component and subscribes
    /// to the Hippotizer's nodes using its GUID. This triggers the TC flow.
    ///
    /// TYPE4 format (44 bytes each):
    ///   04 00 2C 00  seq(4)  counter(4)  node_id(4)  flags(4)  guid(16)  extra1(4) extra2(4)
    void sendSubscribes(juce::StreamingSocket& sock)
    {
        // Use the same syncManagerGuid that we registered in the node tree

        // Subscribe to nodes 1-25 with the same pattern as the working capture:
        //   Nodes 1-15: groups of 3 (flags -2, -1, -1)
        //   Node 16: flags -5
        //   Nodes 17-25: flags -5
        struct SubInfo { int nodeId; int32_t flags; };
        std::vector<SubInfo> subs;

        // Nodes 1-15 in groups of 3
        for (int group = 0; group < 5; ++group)
        {
            int base = group * 3 + 1;
            subs.push_back({ base,     -2 });
            subs.push_back({ base + 1, -1 });
            subs.push_back({ base + 2, -1 });
        }
        // Nodes 16-25
        for (int i = 16; i <= 25; ++i)
            subs.push_back({ i, -5 });

        // Build all TYPE4 messages into one buffer
        std::vector<uint8_t> buf;
        int seq = 8;  // sequence number observed in captures

        for (size_t i = 0; i < subs.size(); ++i)
        {
            uint8_t msg[44] = {};
            msg[0] = 0x04; msg[1] = 0x00;
            msg[2] = 0x2C; msg[3] = 0x00;  // length = 44

            // seq
            msg[4] = (uint8_t)(seq & 0xFF);
            msg[5] = (uint8_t)((seq >> 8) & 0xFF);

            // counter (incrementing)
            int counter = 0x29 + (int)i;
            msg[8] = (uint8_t)(counter & 0xFF);
            msg[9] = (uint8_t)((counter >> 8) & 0xFF);

            // node_id
            msg[12] = (uint8_t)(subs[i].nodeId & 0xFF);
            msg[13] = (uint8_t)((subs[i].nodeId >> 8) & 0xFF);

            // flags
            int32_t f = subs[i].flags;
            std::memcpy(msg + 16, &f, 4);

            // GUID
            std::memcpy(msg + 20, syncManagerGuid, 16);

            // extra fields (observed values from capture)
            msg[36] = 0x09; // extra1
            int counter2 = 0x31 + (int)i;
            msg[40] = (uint8_t)(counter2 & 0xFF);
            msg[41] = (uint8_t)((counter2 >> 8) & 0xFF);

            buf.insert(buf.end(), msg, msg + 44);
        }

        sock.write(buf.data(), (int)buf.size());
        DBG("HippoNet: sent " + juce::String((int)subs.size()) + " TYPE4 subscribes ("
            + juce::String((int)buf.size()) + "B)");
    }

    /// Send Zookeeper node tree response (15KB first wave from working PLAY capture).
    // Discovery state
    mutable juce::SpinLock discoveryLock;
    juce::String discoveredName;
    juce::String discoveredFirmware;
    juce::String discoveredIp;
    int hippoZkUdpPort = 0;  // Hippotizer's Zookeeper UDP port (from TCP announcement)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HippotizerInput)
};
