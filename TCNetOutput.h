// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter
//
// TCNetOutput -- Full TCNet server (broadcast + unicast).
//
// Broadcast:
//   OptIn  (68B,  port 60000, 1Hz)   -- node keepalive
//   Status (300B, port 60000, 1Hz)   -- layer states + track IDs + names
//   Time   (162B, port 60001, 60Hz)  -- playhead ms + beat + state + fader per layer
//
// Unicast server:
//   Listen on port 60000 for slave OptIn broadcasts (discovery)
//   Listen on NodeListenerPort (65023) for Request packets from slaves
//   Respond with Metadata (548B) + Metrics (122B) + Artwork (204, JPEG chunks)
//   Stream Metrics at 30Hz to all known slaves
//
// Packet formats verified against PRO DJ LINK Bridge v1.1 Wireshark capture
// and cross-referenced with TCNet Link Specification V3.5.1B.

#pragma once
#include <JuceHeader.h>
#include "TimecodeCore.h"
#include "NetworkUtils.h"
#include "StcLogoData.h"

#ifdef _WIN32
    #include <winsock2.h>
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
#endif

class TCNetOutput : public juce::Timer
{
public:
    static constexpr int kMaxLayers          = 4;
    static constexpr int kPacketLayers       = 8;
    static constexpr uint16_t kPortBroadcast = 60000;
    static constexpr uint16_t kPortTime      = 60001;
    static constexpr uint16_t kListenerPort  = 65023;
    static constexpr int kTimerHz            = 60;
    static constexpr int kMaxSlaves          = 8;

    // Layer states (spec)
    static constexpr uint8_t kStateIdle    = 0;
    static constexpr uint8_t kStatePlaying = 3;
    static constexpr uint8_t kStatePaused  = 5;
    static constexpr uint8_t kStateStopped = 6;

    // TC state
    static constexpr uint8_t kTcStopped = 0;
    static constexpr uint8_t kTcRunning = 1;

    // SMPTE modes
    static constexpr uint8_t kSmpte24 = 24;
    static constexpr uint8_t kSmpte25 = 25;
    static constexpr uint8_t kSmpteDf = 29;
    static constexpr uint8_t kSmpte30 = 30;

    static constexpr uint32_t kDefaultDurationMs = 86399000;

    // Message types
    static constexpr uint8_t kMsgOptIn   = 2;
    static constexpr uint8_t kMsgOptOut  = 3;
    static constexpr uint8_t kMsgStatus  = 5;
    static constexpr uint8_t kMsgError   = 13;
    static constexpr uint8_t kMsgRequest = 20;
    static constexpr uint8_t kMsgApp     = 30;
    static constexpr uint8_t kMsgData    = 200;
    static constexpr uint8_t kMsgTime    = 254;

    struct LayerData
    {
        uint32_t currentTimeMs = 0;
        uint8_t  beatMarker    = 0;
        uint8_t  layerState    = kStateIdle;
        uint8_t  onAir         = 0;     // fader position 0-255 (0=off, >=1=on-air)
        uint32_t trackId       = 0;
        uint32_t bpm100        = 0;
        uint32_t trackLenMs    = 0;    // real duration or kDefaultDurationMs (23:59:59)
        uint32_t speed         = 0;    // 32768 = 100%, 0 = stopped
        juce::String artist;
        juce::String title;
        bool metadataDirty     = false;

        // Artwork (JPEG bytes for TCNet type 204 packet)
        std::vector<uint8_t> artworkJpeg;
        bool artworkDirty      = false;
    };

    struct SlaveNode
    {
        juce::String ip;
        uint16_t listenerPort = 0;
        uint16_t nodeId       = 0;
        int64_t  lastSeen     = 0;     // ms since epoch
        bool     active       = false;
    };

    TCNetOutput()
    {
        refreshNetworkInterfaces();
        std::memset(nodeName, 0, sizeof(nodeName));
        nodeName[0]='S'; nodeName[1]='T'; nodeName[2]='C';
    }
    ~TCNetOutput() override { stop(); }

    void refreshNetworkInterfaces() { availableInterfaces = ::getNetworkInterfaces(); }
    int  getInterfaceCount() const  { return availableInterfaces.size(); }
    bool getIsRunning() const       { return running; }
    int  getSelectedInterface() const { return selectedInterface; }

    bool start(int interfaceIndex = -1)
    {
        stop();
        if (interfaceIndex >= 0 && interfaceIndex < availableInterfaces.size())
        {
            selectedInterface = interfaceIndex;
            broadcastIp = availableInterfaces[interfaceIndex].broadcast;
            bindIp      = availableInterfaces[interfaceIndex].ip;
        }
        else
        {
            selectedInterface = -1;
            broadcastIp = "255.255.255.255";
            bindIp      = "0.0.0.0";
        }

        // Socket 1: broadcast sender for OptIn/Status (port 60000)
        broadcastSocket = std::make_unique<juce::DatagramSocket>(false);
        if (!broadcastSocket->bindToPort(0, bindIp) && !broadcastSocket->bindToPort(0))
            { broadcastSocket = nullptr; return false; }
        setBroadcast(broadcastSocket.get());

        // Socket 2: broadcast sender for Time (port 60001)
        timeSocket = std::make_unique<juce::DatagramSocket>(false);
        if (!timeSocket->bindToPort(0, bindIp) && !timeSocket->bindToPort(0))
            { timeSocket = nullptr; broadcastSocket = nullptr; return false; }
        setBroadcast(timeSocket.get());

        // Socket 3: listener on our NodeListenerPort for incoming Requests
        listenerSocket = std::make_unique<juce::DatagramSocket>(false);
        if (!listenerSocket->bindToPort(kListenerPort, bindIp) && !listenerSocket->bindToPort(kListenerPort))
            { listenerSocket = nullptr; /* non-fatal, unicast won't work */ }

        // Socket 4: listener on port 60000 for slave OptIn discovery
        // (separate from our broadcast sender)
        discoverySocket = std::make_unique<juce::DatagramSocket>(false);
        discoverySocket->setEnablePortReuse(true);
        if (!discoverySocket->bindToPort(kPortBroadcast))
            discoverySocket = nullptr;  // non-fatal

        // Socket 5: unicast sender for Metrics/Metadata responses
        unicastSocket = std::make_unique<juce::DatagramSocket>(false);
        if (!unicastSocket->bindToPort(0, bindIp) && !unicastSocket->bindToPort(0))
            unicastSocket = nullptr;  // non-fatal

        running = true; seq = 0; uptimeSeconds = 0; tickCount = 0;
        for (auto& s : slaves) s.active = false;

        sendOptIn();
        sendStatus();

        startTimer(1000 / kTimerHz);
        return true;
    }

    void stop()
    {
        stopTimer();
        if (running && broadcastSocket) sendOptOut();
        running = false;
        auto shutdown = [](auto& s) { if (s) { s->shutdown(); s = nullptr; } };
        shutdown(unicastSocket);
        shutdown(listenerSocket);
        shutdown(discoverySocket);
        shutdown(timeSocket);
        shutdown(broadcastSocket);
    }

    void setLayerFromEngine(int idx, const Timecode& tc, FrameRate fps,
                            uint32_t playheadMs, uint32_t durationMs,
                            bool isPlaying, uint8_t onAirFader,
                            uint8_t beatInBar, uint32_t bpm100 = 0)
    {
        if (idx < 0 || idx >= kMaxLayers) return;
        auto& L       = layers[idx];
        L.currentTimeMs = (playheadMs > 0) ? playheadMs : tcToMs(tc, fps);
        L.beatMarker    = beatInBar;
        L.layerState    = isPlaying ? kStatePlaying : kStatePaused;
        L.onAir         = onAirFader;
        L.trackId       = (uint32_t)(idx + 1);
        L.bpm100        = bpm100;
        L.trackLenMs    = (durationMs > 0) ? durationMs : kDefaultDurationMs;
        L.speed         = isPlaying ? 32768 : 0;
    }

    void clearLayer(int idx)
    {
        if (idx >= 0 && idx < kMaxLayers)
        {
            bool hadContent = layers[idx].artist.isNotEmpty() || !layers[idx].artworkJpeg.empty();
            layers[idx] = LayerData{};
            if (hadContent)
            {
                layers[idx].metadataDirty = true;
                layers[idx].artworkDirty = true;
            }
        }
    }

    void setLayerMetadata(int idx, const juce::String& artist, const juce::String& title)
    {
        if (idx < 0 || idx >= kMaxLayers) return;
        auto& L = layers[idx];
        if (L.artist != artist || L.title != title)
        {
            L.artist = artist;
            L.title = title;
            L.metadataDirty = true;
        }
    }

    /// Set artwork JPEG for a layer. Pass empty vector to use default STC logo.
    void setLayerArtwork(int idx, const void* jpegData, size_t jpegSize)
    {
        if (idx < 0 || idx >= kMaxLayers) return;
        auto& L = layers[idx];
        if (jpegData != nullptr && jpegSize > 0)
        {
            auto* src = static_cast<const uint8_t*>(jpegData);
            if (L.artworkJpeg.size() != jpegSize || std::memcmp(L.artworkJpeg.data(), src, jpegSize) != 0)
            {
                L.artworkJpeg.assign(src, src + jpegSize);
                L.artworkDirty = true;
            }
        }
        else if (!L.artworkJpeg.empty())
        {
            L.artworkJpeg.clear();
            L.artworkDirty = true;
        }
    }

private:
    void timerCallback() override
    {
        if (!running) return;

        // 1. Poll incoming packets (discovery + requests)
        pollDiscovery();
        pollRequests();

        // 2. Broadcast Time at 60Hz
        sendTime();

        // 3. Unicast Metrics to all known slaves at 30Hz (every other tick)
        bool metricsThisTick = (tickCount & 1) == 0;
        if (metricsThisTick)
            sendMetricsToSlaves();

        // 4. Send metadata when dirty
        for (int i = 0; i < kMaxLayers; ++i)
        {
            if (layers[i].metadataDirty)
            {
                sendMetadataToSlaves(i);
                layers[i].metadataDirty = false;
            }
            if (layers[i].artworkDirty)
            {
                sendArtworkToSlaves(i);
                layers[i].artworkDirty = false;
            }
        }

        // 5. 1Hz tasks: OptIn, Status, slave expiry
        if (++tickCount >= (uint32_t)kTimerHz)
        {
            tickCount = 0;
            if (++uptimeSeconds >= 43200) uptimeSeconds = 0;
            sendOptIn();
            sendStatus();
            sendOptInToSlaves();  // unicast OptIn to each slave
            expireSlaves();
        }
    }

    // ============================================================
    // Slave discovery -- listen on port 60000 for OptIn from slaves
    // ============================================================
    void pollDiscovery()
    {
        if (!discoverySocket) return;
        uint8_t buf[512];
        juce::String srcIp;
        int srcPort = 0;
        while (discoverySocket->waitUntilReady(true, 0) == 1)
        {
            int n = discoverySocket->read(buf, sizeof(buf), false, srcIp, srcPort);
            if (n < 24) continue;
            if (buf[4] != 'T' || buf[5] != 'C' || buf[6] != 'N') continue;
            if (buf[7] != kMsgOptIn) continue;
            // Don't register ourselves
            uint16_t remoteId = buf[0] | (buf[1] << 8);
            if (remoteId == nodeId) continue;
            // Extract slave's listener port
            if (n >= 28)
            {
                uint16_t slavePort = buf[26] | (buf[27] << 8);
                registerSlave(srcIp, slavePort, remoteId);
            }
        }
    }

    void registerSlave(const juce::String& ip, uint16_t port, uint16_t nid)
    {
        int64_t now = juce::Time::currentTimeMillis();
        // Update existing
        for (auto& s : slaves)
        {
            if (s.active && s.ip == ip)
            {
                s.listenerPort = port;
                s.lastSeen = now;
                return;
            }
        }
        // Add new
        for (auto& s : slaves)
        {
            if (!s.active)
            {
                s.ip = ip;
                s.listenerPort = port;
                s.nodeId = nid;
                s.lastSeen = now;
                s.active = true;
                // Send initial data burst to new slave
                sendInitialDataToSlave(s);
                return;
            }
        }
    }

    void expireSlaves()
    {
        int64_t now = juce::Time::currentTimeMillis();
        for (auto& s : slaves)
            if (s.active && (now - s.lastSeen) > 10000)
                s.active = false;
    }

    void sendInitialDataToSlave(const SlaveNode& slave)
    {
        if (!unicastSocket) return;
        for (int i = 0; i < kPacketLayers; ++i)
        {
            sendMetadataUnicast(slave.ip, slave.listenerPort, i);
            sendMetricsUnicast(slave.ip, slave.listenerPort, i);
        }
        // Send artwork for active layers
        for (int i = 0; i < kMaxLayers; ++i)
            if (layers[i].trackId != 0)
                sendArtworkUnicast(slave.ip, slave.listenerPort, i);
    }

    // ============================================================
    // Request handler -- listen on our NodeListenerPort
    // ============================================================
    void pollRequests()
    {
        if (!listenerSocket) return;
        uint8_t buf[512];
        juce::String srcIp;
        int srcPort = 0;
        while (listenerSocket->waitUntilReady(true, 0) == 1)
        {
            int n = listenerSocket->read(buf, sizeof(buf), false, srcIp, srcPort);
            if (n < 24) continue;
            if (buf[4] != 'T' || buf[5] != 'C' || buf[6] != 'N') continue;

            uint8_t msgType = buf[7];

            if (msgType == kMsgRequest && n >= 26)
            {
                uint8_t dataType = buf[24];
                uint8_t layerId  = buf[25];  // 1-based
                handleRequest(srcIp, dataType, layerId);
            }
            else if (msgType == kMsgApp)
            {
                handleApplication(srcIp, buf, n);
            }
        }
    }

    void handleRequest(const juce::String& srcIp, uint8_t dataType, uint8_t layerId)
    {
        // Find slave's listener port
        uint16_t dstPort = 0;
        for (auto& s : slaves)
            if (s.active && s.ip == srcIp)
                { dstPort = s.listenerPort; break; }
        if (dstPort == 0) return;

        int layerIdx = layerId - 1;  // 0-based

        if (dataType == 4)  // Metadata
            sendMetadataUnicast(srcIp, dstPort, layerIdx);
        else if (dataType == 2)  // Metrics
            sendMetricsUnicast(srcIp, dstPort, layerIdx);
        else if (dataType == 128)  // Artwork (low-res JPEG)
            sendArtworkUnicast(srcIp, dstPort, layerIdx);
    }

    void handleApplication(const juce::String& srcIp, const uint8_t* /*buf*/, int /*len*/)
    {
        // Bridge responds with Application + then Error(255). We just send Error.
        uint16_t dstPort = 0;
        for (auto& s : slaves)
            if (s.active && s.ip == srcIp)
                { dstPort = s.listenerPort; break; }
        if (dstPort == 0 || !unicastSocket) return;

        // Error packet (30 bytes): "not supported"
        uint8_t p[30] = {};
        hdr(p, kMsgError, 5);
        p[24] = 0xFF;  // datatype = 255
        p[25] = 0xFF;  // layer = 255
        p[26] = 0xFF; p[27] = 0x00;  // code = 255
        p[28] = kMsgApp; p[29] = 0x00;  // rejected msg type = 30
        unicastSocket->write(srcIp, (int)dstPort, p, 30);
    }

    // ============================================================
    // Unicast Metrics (122 bytes, type 200 datatype 2)
    // Sent continuously at 30Hz to each slave -- this is what
    // Resolume uses for play/pause, track loaded, position.
    // ============================================================
    void sendMetricsUnicast(const juce::String& ip, uint16_t port, int layerIdx)
    {
        if (!unicastSocket || layerIdx < 0 || layerIdx >= kPacketLayers) return;

        uint8_t p[122] = {};
        hdr(p, kMsgData, 5);

        auto& L = (layerIdx < kMaxLayers) ? layers[layerIdx] : emptyLayer;

        p[24] = 2;                           // DataType = Metrics
        p[25] = (uint8_t)(layerIdx + 1);     // Layer ID (1-based)
        // [26] reserved
        p[27] = L.layerState;                // Layer State
        // [28] reserved
        p[29] = (layerIdx == 0) ? 1 : 0;    // Sync Master
        // [30] reserved
        p[31] = L.beatMarker;                // Beat Marker
        w32(p + 32, L.trackLenMs);           // Track Length ms (0 when no track)
        w32(p + 36, L.currentTimeMs);        // Current Position ms
        w32(p + 40, L.speed);                // Speed
        // [44-56] reserved
        // [57-60] Beat Number
        // [61-111] reserved
        w32(p + 112, L.bpm100);              // BPM * 100
        w16(p + 116, 32768);                 // Pitch Bend (neutral)
        w32(p + 118, L.trackId);             // Track ID

        unicastSocket->write(ip, (int)port, p, 122);
    }

    void sendMetricsToSlaves()
    {
        for (auto& s : slaves)
        {
            if (!s.active) continue;
            // Send metrics for all active layers
            for (int i = 0; i < kMaxLayers; ++i)
                if (layers[i].layerState != kStateIdle)
                    sendMetricsUnicast(s.ip, s.listenerPort, i);
        }
    }

    // ============================================================
    // Unicast Metadata (548 bytes, type 200 datatype 4)
    // Sent on track change and in response to Requests.
    // Bridge uses UTF-32LE for strings (4 bytes per char).
    // ============================================================
    void sendMetadataUnicast(const juce::String& ip, uint16_t port, int layerIdx)
    {
        if (!unicastSocket || layerIdx < 0 || layerIdx >= kPacketLayers) return;

        uint8_t p[548] = {};
        hdr(p, kMsgData, 5);

        p[24] = 4;                           // DataType = Metadata
        p[25] = (uint8_t)(layerIdx + 1);     // Layer ID

        auto& L = (layerIdx < kMaxLayers) ? layers[layerIdx] : emptyLayer;

        // Artist at offset 29, up to 128 chars in UTF-32LE (128*4=512 max, but field is 256 bytes)
        // Actually from capture: fields are 256 bytes each, UTF-32LE encoded
        writeUtf32LE(p + 29, L.artist, 64);    // 64 chars * 4 bytes = 256 bytes
        writeUtf32LE(p + 285, L.title, 64);    // 64 chars * 4 bytes = 256 bytes
        // [541-542] Track Key
        w32(p + 543, L.trackId);               // Track ID

        unicastSocket->write(ip, (int)port, p, 548);
    }

    void sendMetadataToSlaves(int layerIdx)
    {
        for (auto& s : slaves)
            if (s.active)
                sendMetadataUnicast(s.ip, s.listenerPort, layerIdx);
    }

    // ============================================================
    // Unicast Artwork (type 204, datatype 128, JPEG in chunks)
    // Spec page 29: max 4800 bytes per chunk, header at offset 42.
    // ============================================================
    static constexpr uint8_t  kMsgFile        = 204;
    static constexpr uint8_t  kDataArtwork    = 128;
    static constexpr uint32_t kArtClusterSize = 4800;

    void sendArtworkUnicast(const juce::String& ip, uint16_t port, int layerIdx)
    {
        if (!unicastSocket || layerIdx < 0 || layerIdx >= kPacketLayers) return;

        // Choose JPEG source: layer artwork if available, else STC logo
        const uint8_t* jpegData;
        size_t jpegSize;
        auto& L = (layerIdx < kMaxLayers) ? layers[layerIdx] : emptyLayer;
        if (!L.artworkJpeg.empty())
        {
            jpegData = L.artworkJpeg.data();
            jpegSize = L.artworkJpeg.size();
        }
        else
        {
            jpegData = kStcLogoJpeg;
            jpegSize = kStcLogoJpegSize;
        }

        uint32_t totalPackets = (uint32_t)((jpegSize + kArtClusterSize - 1) / kArtClusterSize);

        for (uint32_t pktNo = 0; pktNo < totalPackets; ++pktNo)
        {
            size_t offset = (size_t)pktNo * kArtClusterSize;
            size_t chunkSize = std::min((size_t)kArtClusterSize, jpegSize - offset);
            size_t pktSize = 42 + chunkSize;

            std::vector<uint8_t> p(pktSize, 0);
            hdr(p.data(), kMsgFile, 5);

            p[24] = kDataArtwork;                        // DataType = 128
            p[25] = (uint8_t)(layerIdx + 1);             // Layer ID (1-based)
            w32(p.data() + 26, (uint32_t)jpegSize);      // Total Data Size
            w32(p.data() + 30, totalPackets);             // Total Packets
            w32(p.data() + 34, pktNo);                    // Packet No
            w32(p.data() + 38, kArtClusterSize);          // Data Cluster Size
            std::memcpy(p.data() + 42, jpegData + offset, chunkSize);  // File Data

            unicastSocket->write(ip, (int)port, p.data(), (int)pktSize);
        }
    }

    void sendArtworkToSlaves(int layerIdx)
    {
        for (auto& s : slaves)
            if (s.active)
                sendArtworkUnicast(s.ip, s.listenerPort, layerIdx);
    }

    // ============================================================
    // Unicast OptIn to each slave (Bridge does this ~every second)
    // ============================================================
    void sendOptInToSlaves()
    {
        if (!unicastSocket) return;
        uint8_t p[68] = {};
        hdr(p, kMsgOptIn, 5);
        w16(p + 24, 1);
        w16(p + 26, kListenerPort);
        w16(p + 28, (uint16_t)(uptimeSeconds & 0xFFFF));
        wstr(p + 32, "Fiverecords", 16);
        wstr(p + 48, "STC", 16);
        p[64] = 1; p[65] = 7; p[66] = 0;
        for (auto& s : slaves)
            if (s.active)
                unicastSocket->write(s.ip, (int)s.listenerPort, p, 68);
    }

    // ============================================================
    // Broadcast packets (same as before)
    // ============================================================
    void hdr(uint8_t* p, uint8_t msg, uint8_t minor)
    {
        w16(p, nodeId);
        p[2] = 3; p[3] = minor;
        p[4] = 'T'; p[5] = 'C'; p[6] = 'N';
        p[7] = msg;
        std::memcpy(p + 8, nodeName, 8);
        p[16] = seq++;
        p[17] = 2;  // Master
        w16(p + 18, 0x0007);  // NodeOptions (matches Bridge)
        w32(p + 20, 0);
    }

    void sendOptIn()
    {
        if (!broadcastSocket) return;
        uint8_t p[68] = {};
        hdr(p, kMsgOptIn, 5);
        w16(p + 24, 1);
        w16(p + 26, kListenerPort);
        w16(p + 28, (uint16_t)(uptimeSeconds & 0xFFFF));
        wstr(p + 32, "Fiverecords", 16);
        wstr(p + 48, "STC", 16);
        p[64] = 1; p[65] = 7; p[66] = 0;
        broadcastSocket->write(broadcastIp, kPortBroadcast, p, 68);
    }

    void sendOptOut()
    {
        if (!broadcastSocket) return;
        uint8_t p[28] = {};
        hdr(p, kMsgOptOut, 5);
        w16(p + 24, 1);
        w16(p + 26, kListenerPort);
        broadcastSocket->write(broadcastIp, kPortBroadcast, p, 28);
    }

    void sendStatus()
    {
        if (!broadcastSocket) return;
        uint8_t p[300] = {};
        hdr(p, kMsgStatus, 5);
        w16(p + 24, 1);
        w16(p + 26, kListenerPort);
        static const char* nm[4] = {"STC Layer 1","STC Layer 2","STC Layer 3","STC Layer 4"};
        for (int i = 0; i < kMaxLayers; ++i)
        {
            if (layers[i].trackId != 0)
                p[34 + i] = (uint8_t)(i + 1);
            p[42 + i] = layers[i].layerState;
            w32(p + 50 + i * 4, layers[i].trackId);
            wstr(p + 172 + i * 16, nm[i], 16);
        }
        p[83] = kSmpte30;
        broadcastSocket->write(broadcastIp, kPortBroadcast, p, 300);
    }

    void sendTime()
    {
        if (!timeSocket) return;
        uint8_t p[162] = {};
        hdr(p, kMsgTime, 5);
        for (int i = 0; i < kPacketLayers; ++i)
        {
            auto& L = layers[i];
            w32(p + 24 + i*4, L.currentTimeMs);
            // totalTime at 56+i*4 = 0
            p[88 + i]  = L.beatMarker;
            p[96 + i]  = L.layerState;
            // TC at 106+i*6 = all zeros
            p[154 + i] = L.onAir;  // fader position 0-255 (spec V3.3.3+)
        }
        p[104] = 1;
        p[105] = kSmpte30;
        timeSocket->write(broadcastIp, kPortTime, p, 162);
    }

    // ============================================================
    // Helpers
    // ============================================================
    static void setBroadcast(juce::DatagramSocket* s)
    {
        auto h = s->getRawSocketHandle();
        if (h >= 0) {
            int f = 1;
#ifdef _WIN32
            setsockopt(h, SOL_SOCKET, SO_BROADCAST, (const char*)&f, sizeof(f));
#else
            setsockopt(h, SOL_SOCKET, SO_BROADCAST, &f, sizeof(f));
#endif
        }
    }

    static void w16(uint8_t* p, uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
    static void w32(uint8_t* p, uint32_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
    static void wstr(uint8_t* p, const char* s, int m) { int n=(int)std::strlen(s); if(n>m)n=m; std::memcpy(p,s,(size_t)n); }

    static void writeUtf32LE(uint8_t* dst, const juce::String& str, int maxChars)
    {
        auto utf16 = str.toUTF16();
        const juce::CharPointer_UTF16 ptr = utf16;
        int written = 0;
        for (auto it = str.begin(); it != str.end() && written < maxChars; ++it, ++written)
        {
            uint32_t cp = (uint32_t)(*it);
            dst[written * 4 + 0] = (uint8_t)(cp);
            dst[written * 4 + 1] = (uint8_t)(cp >> 8);
            dst[written * 4 + 2] = (uint8_t)(cp >> 16);
            dst[written * 4 + 3] = (uint8_t)(cp >> 24);
        }
    }

    static uint32_t tcToMs(const Timecode& tc, FrameRate fps)
    {
        double fms;
        switch (fps) {
            case FrameRate::FPS_2398: fms = 1000.0/23.976; break;
            case FrameRate::FPS_24:   fms = 1000.0/24.0;   break;
            case FrameRate::FPS_25:   fms = 1000.0/25.0;   break;
            case FrameRate::FPS_2997: fms = 1000.0/29.97;  break;
            case FrameRate::FPS_30:   fms = 1000.0/30.0;   break;
            default:                  fms = 1000.0/30.0;   break;
        }
        return (uint32_t)(tc.hours*3600000 + tc.minutes*60000 + tc.seconds*1000 + tc.frames*fms);
    }

    // ============================================================
    juce::Array<NetworkInterface> availableInterfaces;
    std::unique_ptr<juce::DatagramSocket> broadcastSocket, timeSocket;
    std::unique_ptr<juce::DatagramSocket> listenerSocket;     // receives Requests on kListenerPort
    std::unique_ptr<juce::DatagramSocket> discoverySocket;    // receives OptIns on port 60000
    std::unique_ptr<juce::DatagramSocket> unicastSocket;      // sends Metrics/Metadata to slaves
    juce::String broadcastIp = "255.255.255.255", bindIp = "0.0.0.0";
    int selectedInterface = -1;
    bool running = false;
    LayerData layers[kPacketLayers] = {};
    LayerData emptyLayer;
    SlaveNode slaves[kMaxSlaves] = {};
    char nodeName[9] = {};
    uint16_t nodeId = 0x5443;
    uint8_t seq = 0;
    uint32_t uptimeSeconds = 0, tickCount = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TCNetOutput)
};
