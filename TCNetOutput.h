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
    static constexpr int kMaxLayers          = 4;  // Layers 1-4 (matches MagicQ menu options)
    static constexpr int kPacketLayers       = 8;  // Time/Status packet has 8 slots regardless
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

    // Fake "no track" duration sent when an engine has no real track loaded
    // (Generator, MTC, Art-Net, LTC).
    // Empirical test history with ChamSys MagicQ:
    //   86400000 (24h)        -> rejected as invalid duration
    //   86399000 (23:59:59)   -> rejected as invalid duration
    //   21600000 (6h)         -> rejected as invalid duration
    //   14400000 (4h)         -> rejected as invalid duration
    //  10800000  (3h)         -> rejected as invalid duration
    //   9000000  (2h30)       -> rejected as invalid duration
    //   7200000  (2h)         -> accepted (highest validated value)
    //   3600000  (1h)         -> accepted
    // ChamSys hard limit is somewhere between 2h00 and 2h30. We lock in 2h.
    static constexpr uint32_t kDefaultDurationMs = 7200000;  // 2 hours

    // Message types
    static constexpr uint8_t kMsgOptIn   = 2;
    static constexpr uint8_t kMsgOptOut  = 3;
    static constexpr uint8_t kMsgStatus  = 5;
    static constexpr uint8_t kMsgTimeSync = 10;
    static constexpr uint8_t kMsgError   = 13;
    static constexpr uint8_t kMsgRequest = 20;
    static constexpr uint8_t kMsgApp     = 30;
    static constexpr uint8_t kMsgData    = 200;
    static constexpr uint8_t kMsgTime    = 254;

    struct LayerData
    {
        uint32_t currentTimeMs = 0;
        uint8_t  beatMarker    = 0;
        uint32_t beatNumber    = 0;   // continuously incrementing beat count
        uint8_t  layerState    = kStateIdle;
        uint8_t  tcHours       = 0;
        uint8_t  tcMinutes     = 0;
        uint8_t  tcSeconds     = 0;
        uint8_t  tcFrames      = 0;
        uint8_t  onAir         = 0;     // fader position 0-255 (0=off, >=1=on-air)
        uint32_t trackId       = 0;
        uint32_t bpm100        = 0;
        uint32_t trackLenMs    = 0;    // real duration or kDefaultDurationMs (24h)
        uint32_t speed         = 0;    // 2^20 fixed-point (1048576 = 100%). Matches Bridge.
        uint8_t  masterPlayerNum = 0;  // Sync Master value: 0=not master, 1-8=master player number
        juce::String artist;
        juce::String title;
        bool metadataDirty     = false;

        // Artwork (JPEG bytes for TCNet type 204 packet)
        std::vector<uint8_t> artworkJpeg;
        bool artworkDirty      = false;

        // Small Waveform cache: 2400 bytes of PWV5-formatted data (2 bytes
        // per entry = 1200 entries: [height, color]). Filled from the CDJ's
        // analysed waveform when available, zeros otherwise.
        std::vector<uint8_t> smallWaveform;  // 2400 bytes
        // Real beat grid from CDJ (beat number, time stamp in ms). When
        // populated, takes priority over BPM-synthesized grid.
        struct RealBeat { uint16_t beatNumber; uint32_t timeMs; uint8_t type; };
        std::vector<RealBeat> realBeatGrid;

        // When a layer becomes active (CDJ connected, Generator playing,
        // etc.) this gets set true. The real Bridge keeps emitting Metrics
        // and Status for any layer that has ever been active, even after
        // the source disappears (layerState transitions to idle but the
        // layer "exists"). This stable identity prevents MagicQ from
        // entering an error state when sources change.
        bool hasEverBeenActive = false;

        // Jog anti-jitter: committed position for deadband filter.
        // During jog/scratch the CDJ's abspos oscillates +/-several ms due to
        // mechanical jog wheel resolution.  Without filtering, Resolume sees
        // sub-frame position reversals and renders a visible frame vibration.
        // The deadband suppresses direction changes smaller than one video frame
        // (~33ms at 30fps) while passing through sustained movement and playback.
        uint32_t committedMs   = 0;
        bool     wasPlaying    = false;
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

    // Global TCNet offset (ms), summed with the per-engine offset inside
    // setLayerFromEngine.  Use case: compensating for venue capture /
    // encoding chains that add a uniform delay to every engine.  Setting
    // 0 reproduces v1.9.11-beta6 behaviour exactly.  Both setter and the
    // read inside setLayerFromEngine run on the message thread (timer
    // callback + UI callback), so no synchronisation is required; the
    // field is a plain int matching the per-engine offset's style.
    void setGlobalOffsetMs(int v) { globalOffsetMs = juce::jlimit(-2000, 2000, v); }
    int  getGlobalOffsetMs() const { return globalOffsetMs; }

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
                            uint8_t beatInBar, uint32_t bpm100 = 0,
                            int offsetMs = 0, uint32_t beatNumber = 0,
                            uint8_t masterPlayerNum = 0,
                            uint32_t trackId = 0)
    {
        if (idx < 0 || idx >= kMaxLayers) return;
        auto& L       = layers[idx];
        L.masterPlayerNum = masterPlayerNum;  // Sync Master value for Metrics
        int64_t baseMs = (int64_t)((playheadMs > 0) ? playheadMs : tcToMs(tc, fps));
        // Engine-side per-layer offset + venue-wide global offset.
        // Both are signed ms; the sum is clamped into the uint32 wire
        // range below so negative excursions parked at 0 instead of
        // wrapping to ~49 days.
        int64_t adjusted = baseMs + offsetMs + (int64_t)globalOffsetMs;
        uint32_t newMs = (uint32_t)juce::jlimit((int64_t)0, (int64_t)0xFFFFFFFF, adjusted);

        // Jog anti-jitter deadband.
        // During playback: pass through directly (position advances monotonically).
        // On play->pause transition: commit current position as anchor.
        // While paused/jogging: only update if movement exceeds one video frame
        // (~33ms).  This suppresses the +/-few-ms oscillation from the CDJ's jog
        // wheel that causes visible frame vibration in Resolume.
        static constexpr uint32_t kJogDeadband = 33;  // ~1 frame at 30fps

        if (isPlaying)
        {
            L.currentTimeMs = newMs;
            L.committedMs   = newMs;
        }
        else
        {
            // Transition from play to pause: anchor at current position
            if (L.wasPlaying)
                L.committedMs = newMs;

            int64_t delta = (int64_t)newMs - (int64_t)L.committedMs;
            if (delta < 0) delta = -delta;
            if ((uint64_t)delta >= kJogDeadband)
            {
                L.committedMs   = newMs;
                L.currentTimeMs = newMs;
            }
            // else: hold L.currentTimeMs at the last committed value
        }
        L.wasPlaying    = isPlaying;
        L.hasEverBeenActive = true;

        L.beatMarker    = beatInBar;
        L.beatNumber    = beatNumber;
        L.layerState    = isPlaying ? kStatePlaying : kStatePaused;
        L.onAir         = onAirFader;
        L.trackId       = (trackId != 0) ? trackId : (uint32_t)(idx + 1);
        L.bpm100        = bpm100;
        L.trackLenMs    = (durationMs > 0) ? durationMs : kDefaultDurationMs;
        L.speed         = isPlaying ? 1048576u : 0u;  // 2^20 fixed-point = 100%. Bridge sends this range.

        // Derive HH:MM:SS:FF from the committed currentTimeMs (post-deadband)
        // so all fields in the Time Packet stay internally consistent.
        // ChamSys and similar consoles read the TC H/M/S/F fields (starting
        // at byte 108 for L1) as the advancing timecode; if left at 0 the
        // receiver sees "timecode stopped at 00:00:00:00" and ignores it.
        int fpsInt = 30;
        if      (fps == FrameRate::FPS_2398) fpsInt = 24;
        else if (fps == FrameRate::FPS_24)   fpsInt = 24;
        else if (fps == FrameRate::FPS_25)   fpsInt = 25;
        else if (fps == FrameRate::FPS_2997) fpsInt = 30;
        else if (fps == FrameRate::FPS_30)   fpsInt = 30;
        uint32_t totalSec = L.currentTimeMs / 1000;
        uint32_t ms       = L.currentTimeMs % 1000;
        L.tcHours   = (uint8_t)((totalSec / 3600) % 24);
        L.tcMinutes = (uint8_t)((totalSec % 3600) / 60);
        L.tcSeconds = (uint8_t)(totalSec % 60);
        L.tcFrames  = (uint8_t)((ms * fpsInt) / 1000);
    }

    void clearLayer(int idx)
    {
        if (idx >= 0 && idx < kMaxLayers)
        {
            // Preserve identity (hasEverBeenActive, masterPlayerNum, trackLenMs)
            // and just transition the layer state to idle. Real Bridge keeps
            // emitting Metrics for the layer indefinitely after the source
            // disappears, with layerState=0 -- this stable identity is what
            // MagicQ tracks. If we wiped everything, the receiver would see
            // the master "vanish" and enter an error state.
            bool hadContent = layers[idx].artist.isNotEmpty() || !layers[idx].artworkJpeg.empty();
            layers[idx].layerState     = kStateIdle;
            layers[idx].beatMarker     = 0;
            layers[idx].beatNumber     = 0;
            layers[idx].speed          = 0;
            layers[idx].onAir          = 0;
            layers[idx].currentTimeMs  = 0;
            layers[idx].committedMs    = 0;
            layers[idx].wasPlaying     = false;
            layers[idx].tcHours        = 0;
            layers[idx].tcMinutes      = 0;
            layers[idx].tcSeconds      = 0;
            layers[idx].tcFrames       = 0;
            layers[idx].artist.clear();
            layers[idx].title.clear();
            layers[idx].artworkJpeg.clear();
            layers[idx].smallWaveform.clear();
            layers[idx].realBeatGrid.clear();
            if (hadContent)
            {
                layers[idx].metadataDirty = true;
                layers[idx].artworkDirty = true;
            }
            // Note: hasEverBeenActive, masterPlayerNum, trackId, bpm100,
            // trackLenMs are intentionally NOT cleared so the layer keeps
            // a stable identity for the receiver.
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

    /// Set small waveform data for a layer.
    ///
    /// srcData is the raw waveform bytes from the CDJ. format identifies
    /// the encoding:
    ///   0 = none (clear the cached waveform)
    ///   3 = ThreeBand (3 bytes/entry: mid, high, low) -- CDJ-3000
    ///        Each band is 5-bit (0-31), encoded as a byte 0x00-0x1F.
    ///   6 = ColorNxs2 (6 bytes/entry: d0,d1,d2,R,G,B) -- NXS2 PWV4
    ///        d0/d1/d2 are 0-31 magnitude, RGB are 0-7 colour intensity.
    ///   2 = PWV5 pre-encoded (2 bytes/entry: height, color) -- pass through
    ///
    /// The output is always 2400 bytes in PWV5 format (1200 entries, 2 bytes
    /// each: height + color). PWV5 colour byte packs (high_band << 4) | mid_band
    /// with each nibble 0-15 (5-bit -> 4-bit by >> 1). Heights are mapped from
    /// 5-bit (0-31) to 8-bit (0-255) by left-shift 3 so MagicQ sees full
    /// amplitudes matching what the Bridge sends.
    void setLayerSmallWaveform(int idx, const uint8_t* srcData, int srcEntryCount,
                               int srcBytesPerEntry)
    {
        if (idx < 0 || idx >= kMaxLayers) return;
        auto& L = layers[idx];

        if (srcData == nullptr || srcEntryCount <= 0 || srcBytesPerEntry <= 0)
        {
            L.smallWaveform.clear();
            return;
        }

        constexpr int kTargetEntries = 1200;
        constexpr int kTargetBytes = kTargetEntries * 2;  // 2400
        L.smallWaveform.assign(kTargetBytes, 0);

        // Resample source -> 1200 entries.
        for (int i = 0; i < kTargetEntries; ++i)
        {
            int srcIdx = (int)((double)i * srcEntryCount / kTargetEntries);
            if (srcIdx >= srcEntryCount) srcIdx = srcEntryCount - 1;
            const uint8_t* e = srcData + srcIdx * srcBytesPerEntry;

            uint8_t height = 0;
            uint8_t color = 0;
            switch (srcBytesPerEntry)
            {
                case 3:  // ThreeBand: bands are 5-bit (0-31). Scale to 0-255.
                {
                    uint8_t mid = e[0] & 0x1F;
                    uint8_t hi  = e[1] & 0x1F;
                    uint8_t lo  = e[2] & 0x1F;
                    uint8_t maxBand = std::max({mid, hi, lo});
                    // 5-bit -> 8-bit: multiply by ~8.23. Use shift + carry for
                    // a slight saturation toward max instead of simple <<3
                    // which caps at 248 instead of 255.
                    height = (uint8_t)std::min(255, (int)maxBand * 8 + (maxBand >> 2));
                    // PWV5 color byte: pack high-band and mid-band intensities
                    // into nibbles. 5-bit -> 4-bit by >>1.
                    color = (uint8_t)(((hi >> 1) << 4) | (mid >> 1));
                    break;
                }
                case 6:  // ColorNxs2: d0-d2 magnitude bands (5-bit), d3-d5 RGB.
                {
                    uint8_t d0 = e[0] & 0x1F;
                    uint8_t d1 = e[1] & 0x1F;
                    uint8_t d2 = e[2] & 0x1F;
                    uint8_t maxBand = std::max({d0, d1, d2});
                    height = (uint8_t)std::min(255, (int)maxBand * 8 + (maxBand >> 2));
                    // Encode RGB into PWV5 colour: pack G into upper nibble,
                    // R into lower nibble. RGB are typically 0-7, fits in 4 bits.
                    uint8_t r = e[3] & 0x0F;
                    uint8_t g = e[4] & 0x0F;
                    color = (uint8_t)((g << 4) | r);
                    break;
                }
                case 2:  // already PWV5
                    height = e[0];
                    color = e[1];
                    break;
                default: break;
            }
            L.smallWaveform[i * 2] = height;
            L.smallWaveform[i * 2 + 1] = color;
        }
    }

    /// Set real beat grid from CDJ metadata.
    /// entries are (beat number 1-based, time in ms). Beat type (up/downbeat)
    /// is inferred from beat number (every 4th is a downbeat at 4/4 time).
    void setLayerBeatGrid(int idx, const uint16_t* beatNumbers,
                          const uint32_t* timesMs, int count)
    {
        if (idx < 0 || idx >= kMaxLayers) return;
        auto& L = layers[idx];
        L.realBeatGrid.clear();
        if (beatNumbers == nullptr || timesMs == nullptr || count <= 0) return;
        L.realBeatGrid.reserve((size_t)count);
        for (int i = 0; i < count; ++i)
        {
            LayerData::RealBeat rb;
            rb.beatNumber = beatNumbers[i];
            rb.timeMs = timesMs[i];
            // Downbeat if beatNumber % 4 == 1 (first beat of bar in 1-based).
            rb.type = (beatNumbers[i] > 0 && (beatNumbers[i] % 4 == 1)) ? 0x14 : 0x0a;
            L.realBeatGrid.push_back(rb);
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

            if (msgType == kMsgTimeSync && n >= 32)
            {
                handleTimeSync(srcIp, buf, n);
            }
            else if (msgType == kMsgRequest && n >= 26)
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
        else if (dataType == 8)   // Beat Grid
            sendBeatGridUnicast(srcIp, dstPort, layerIdx);
        else if (dataType == 16)  // Small Waveform
            sendSmallWaveformUnicast(srcIp, dstPort, layerIdx);
        else
        {
            // Other unsupported data types (Cue=12, Big WF=32). Reply with
            // Error so the requester doesn't keep waiting.
            if (!unicastSocket) return;
            uint8_t p[30] = {};
            hdr(p, kMsgError, 5);
            p[24] = dataType;
            p[25] = layerId;
            p[26] = 0xFF; p[27] = 0x00;
            p[28] = kMsgRequest; p[29] = 0x00;
            unicastSocket->write(srcIp, (int)dstPort, p, 30);
        }
    }

    // ============================================================
    // Synthesize a Beat Grid based on current BPM. The real PRO DJ LINK
    // Bridge always serves Beat Grid Data when requested (never Error),
    // and ChamSys MagicQ requires this to trust the node and update its
    // beat tracker display. Format (from observed Bridge traffic):
    //   Header: TCNet Data Packet type 200, DataType=8
    //   Bytes 26-29: Data Size = numBeats * 8
    //   Bytes 30-33: Total Packets = 1 (we send short grid in one packet)
    //   Bytes 34-37: Packet No = 0
    //   Bytes 38-41: Data Cluster Size = numBeats * 8
    //   Bytes 42+: N entries of 8 bytes each:
    //     [0-1] Beat Number (LE uint16)
    //     [2]   Beat Type: 0x0a (10) = upbeat, 0x14 (20) = downbeat
    //     [3]   Reserved
    //     [4-7] Beat Time Stamp in ms (LE uint32)
    //   First entry (beat 0) has all zeros. Downbeats every 4 beats.
    // ============================================================
    void sendBeatGridUnicast(const juce::String& ip, uint16_t port, int layerIdx)
    {
        if (!unicastSocket || layerIdx < 0 || layerIdx >= kPacketLayers) return;
        auto& L = layers[layerIdx];

        // Beat Grid format (observed from real Bridge):
        //   8 bytes per entry: Beat Number (2B LE) + Beat Type (1B: 0x0a upbeat,
        //   0x14 downbeat) + Reserved (1B) + Beat Time Stamp (4B LE ms).
        //   First entry is all zeros (Bridge convention).
        //   Max 2400 bytes of data per packet = 300 entries per packet.
        //   Multi-packet: Total Packets > 1, Packet No 0..N-1.
        //
        // We need the grid to cover the ENTIRE track duration so MagicQ's beat
        // counter doesn't stall at the end of the grid. For a 1-hour default
        // at 120 BPM = 7201 entries = 57608 bytes = ~24 packets.
        constexpr int kEntrySize = 8;
        constexpr int kMaxEntriesPerPacket = 300;  // 2400 bytes / 8
        constexpr int kClusterSize = kMaxEntriesPerPacket * kEntrySize;  // 2400

        // Compute total beats to emit.
        int totalBeats = 0;
        std::vector<std::pair<uint32_t, uint8_t>> entries;  // (timeMs, type) for beat 1..N
        if (!L.realBeatGrid.empty())
        {
            entries.reserve(L.realBeatGrid.size());
            for (const auto& b : L.realBeatGrid)
                entries.push_back({ b.timeMs, b.type });
            totalBeats = (int)entries.size();
        }
        else
        {
            uint32_t bpm100 = (L.bpm100 > 0) ? L.bpm100 : 12000;
            double beatMs = 60000.0 * 100.0 / (double)bpm100;
            uint32_t durationMs = (L.trackLenMs > 0) ? L.trackLenMs : kDefaultDurationMs;
            int beatCount = (int)((double)durationMs / beatMs);
            if (beatCount < 1) beatCount = 1;
            // Cap at a sane upper bound to prevent pathological traffic.
            // 2h at 220 BPM = 26400 beats. 30000 covers it with margin.
            // At 8 bytes/entry + multi-packet overhead, 30k beats =
            // 100 packets of 2400 bytes = ~240 KB sent on each Beat Grid request.
            if (beatCount > 30000) beatCount = 30000;
            entries.reserve((size_t)beatCount);
            for (int i = 1; i <= beatCount; ++i)
            {
                uint32_t ts = (uint32_t)(beatMs * (double)(i - 1) + 62.0);
                bool isDownbeat = ((i - 2) % 4 == 0) && (i >= 2);
                entries.push_back({ ts, (uint8_t)(isDownbeat ? 0x14 : 0x0a) });
            }
            totalBeats = beatCount;
        }

        // Total entries to send = 1 (zero entry) + totalBeats.
        int totalEntries = 1 + totalBeats;
        int totalDataSize = totalEntries * kEntrySize;
        int totalPackets = (totalDataSize + kClusterSize - 1) / kClusterSize;
        if (totalPackets < 1) totalPackets = 1;

        // Emit each packet in the multi-packet sequence.
        for (int pktNo = 0; pktNo < totalPackets; ++pktNo)
        {
            int bytesIntoData = pktNo * kClusterSize;
            int bytesRemaining = totalDataSize - bytesIntoData;
            int thisChunkSize = std::min(bytesRemaining, kClusterSize);
            int thisEntryCount = thisChunkSize / kEntrySize;
            int packetSize = 42 + thisChunkSize;

            std::vector<uint8_t> p((size_t)packetSize, 0);
            hdr(p.data(), kMsgData, 5);

            p[24] = 8;                                // DataType = Beat Grid
            p[25] = (uint8_t)(layerIdx + 1);          // Layer ID
            w32(p.data() + 26, (uint32_t)totalDataSize);   // Total data size
            w32(p.data() + 30, (uint32_t)totalPackets);    // Total packets
            w32(p.data() + 34, (uint32_t)pktNo);           // Packet No (0-based)
            w32(p.data() + 38, (uint32_t)kClusterSize);    // Cluster size

            // Starting entry index in the flattened [entry 0 zero + entries[]].
            int startEntry = pktNo * kMaxEntriesPerPacket;
            for (int i = 0; i < thisEntryCount; ++i)
            {
                int flatIdx = startEntry + i;
                int off = 42 + i * kEntrySize;
                if (flatIdx == 0)
                {
                    // Entry 0: all zeros (Bridge convention).
                    continue;
                }
                int entryIdx = flatIdx - 1;  // into entries[]
                if (entryIdx >= (int)entries.size()) break;
                uint16_t beatNum = (uint16_t)(entryIdx + 1);  // 1-based beat number
                w16(p.data() + off, beatNum);
                p[off + 2] = entries[(size_t)entryIdx].second;  // type
                p[off + 3] = 0;
                w32(p.data() + off + 4, entries[(size_t)entryIdx].first);  // timeMs
            }

            unicastSocket->write(ip, (int)port, p.data(), packetSize);
        }
    }

    // Synthesize a Small Waveform Data packet. If we have real CDJ waveform
    // data cached (set via setLayerSmallWaveform), use that; otherwise send
    // a packet of zeros with valid headers so MagicQ considers the node
    // compliant.
    void sendSmallWaveformUnicast(const juce::String& ip, uint16_t port, int layerIdx)
    {
        if (!unicastSocket || layerIdx < 0 || layerIdx >= kPacketLayers) return;
        auto& L = layers[layerIdx];

        constexpr int kWfDataSize = 2400;
        constexpr int kWfPacketSize = 42 + kWfDataSize;  // 2442

        uint8_t p[kWfPacketSize] = {};
        hdr(p, kMsgData, 5);

        p[24] = 16;                           // DataType = Small Waveform
        p[25] = (uint8_t)(layerIdx + 1);      // Layer ID
        w32(p + 26, kWfDataSize);             // Data Size
        w32(p + 30, 1);                       // Total Packets
        w32(p + 34, 1);                       // Packet No (1-based, per Bridge)
        w32(p + 38, 0);                       // Data Cluster Size (Bridge sends 0 here)
        // [42..2441] waveform samples: use cached data if available.
        if (L.smallWaveform.size() == (size_t)kWfDataSize)
        {
            std::memcpy(p + 42, L.smallWaveform.data(), kWfDataSize);
        }

        unicastSocket->write(ip, (int)port, p, kWfPacketSize);
    }

    void handleTimeSync(const juce::String& srcIp, const uint8_t* buf, int len)
    {
        if (len < 32 || !unicastSocket) return;

        // Parse request
        uint8_t  step          = buf[24];
        uint16_t initiatorPort = (uint16_t)(buf[26] | (buf[27] << 8));
        uint32_t initiatorTs   = (uint32_t)buf[28]
                               | ((uint32_t)buf[29] << 8)
                               | ((uint32_t)buf[30] << 16)
                               | ((uint32_t)buf[31] << 24);
        if (initiatorPort < 65023) return;  // invalid listener port per spec

        // Build response. The spec says:
        //   Step 1: initiator sends with STEP=0, timestamp=own current timer
        //   Step 2: remote responds with STEP=1, timestamp=remote's current
        //           timer, Remote Timestamp=initiator's original timestamp
        // In practice we see the initiator sending STEP=1 (MagicQ) -- we just
        // echo back STEP incremented, our timestamp, and their timestamp
        // in the Remote Timestamp field, so the initiator can compute the
        // round-trip delay.
        uint8_t p[32] = {};
        hdr(p, kMsgTimeSync, 5);

        // Timestamp in microseconds since start. TCNet spec describes this
        // as "microseconds within a second" (0-999999) but observed traffic
        // from ChamSys MagicQ uses an accumulating microsecond counter (the
        // values grow monotonically into the billions). Emit an accumulating
        // counter to match: use our uptime in microseconds, which fits in
        // uint32_t for about 71 minutes before wrapping and that's fine for
        // sync -- both sides just need consistent time bases to compute
        // relative delay.
        uint32_t nowUs = (uint32_t)((juce::Time::getHighResolutionTicks()
                                   * 1000000LL)
                                  / juce::Time::getHighResolutionTicksPerSecond());
        w32(p + 20, nowUs);

        p[24] = (uint8_t)(step + 1);  // advance step
        p[25] = 0;                     // reserved
        w16(p + 26, (uint16_t)kListenerPort);
        w32(p + 28, initiatorTs);      // echo initiator's timestamp

        unicastSocket->write(srcIp, (int)initiatorPort, p, 32);
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
        // Sync Master: 0 if this layer is not master, or the player number
        // (1-8) of the master if this layer holds master status. Observed
        // Bridge traffic sets this to the Layer ID of the master (e.g.
        // CDJ-3 on air → Sync Master = 3). MagicQ uses this field to
        // identify which layer's beat to follow.
        p[29] = L.masterPlayerNum;
        // [30] reserved
        p[31] = L.beatMarker;                // Beat Marker
        w32(p + 32, L.trackLenMs);           // Track Length ms
        w32(p + 36, L.currentTimeMs);        // Current Position ms
        w32(p + 40, L.speed);                // Speed (2^20 fixed-point, 1048576=100%)
        // Byte 45 = SMPTE framerate (30 = 30fps). Bridge observed sending 30 here
        // in what the spec calls "reserved [44-56]". Apparently not fully reserved.
        p[45] = kSmpte30;
        // [46-56] reserved
        w32(p + 57, L.beatNumber);           // Beat Number
        // [61-111] reserved
        w32(p + 112, L.bpm100);              // BPM * 100
        // Pitch Bend: observed Bridge sends ~0 for neutral (e.g. 95 for tiny
        // variation), NOT 32768. The spec says "16-bit 0-FFFF*" but 32768 as
        // "neutral" is wrong -- that would look like a 50% pitch adjustment
        // to consumers using the Bridge convention.
        w16(p + 116, 0);                     // Pitch Bend (neutral = 0)
        w32(p + 118, L.trackId);             // Track ID

        unicastSocket->write(ip, (int)port, p, 122);
    }

    void sendMetricsToSlaves()
    {
        for (auto& s : slaves)
        {
            if (!s.active) continue;
            // Only emit Metrics for layers that are currently active
            // (Playing/Paused). Idle layers stop emitting -- this matches
            // historical behaviour and keeps Resolume's transport indicator
            // responsive (Resolume reads Time Packet TC State which can get
            // confused if multiple layers report state simultaneously).
            for (int i = 0; i < kMaxLayers; ++i)
            {
                if (layers[i].layerState != kStateIdle)
                    sendMetricsUnicast(s.ip, s.listenerPort, i);
            }
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
        hdr(p, kMsgOptIn, 5);  // Real Bridge uses 3.5
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

        // Timestamp: microseconds within the current second (0-999999) per
        // spec. This is what receivers use to compute network latency and
        // to discriminate "live" nodes from stale ones. Was previously 0
        // which may cause some consoles (ChamSys MagicQ) to treat the node
        // as not active.
        uint32_t nowUs = (uint32_t)(((juce::Time::getHighResolutionTicks()
                                    * 1000000LL)
                                   / juce::Time::getHighResolutionTicksPerSecond())
                                  % 1000000);
        w32(p + 20, nowUs);
    }

    void sendOptIn()
    {
        if (!broadcastSocket) return;
        uint8_t p[68] = {};
        hdr(p, kMsgOptIn, 5);  // Real Bridge uses 3.5, not spec's 3.6
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
        static const char* nm[4] = {
            "STC Layer 1", "STC Layer 2", "STC Layer 3", "STC Layer 4"
        };
        for (int i = 0; i < kMaxLayers; ++i)
        {
            auto& L = layers[i];
            if (L.hasEverBeenActive)
                p[34 + i] = (uint8_t)(i + 1);
            // Layer Status per spec: pure enum (same as Time Packet byte 96).
            // Resolume reads this for play/pause display.
            p[42 + i] = L.layerState;
            w32(p + 50 + i * 4, L.trackId);
            wstr(p + 172 + i * 16, nm[i], 16);
        }
        p[83] = kSmpte30;
        broadcastSocket->write(broadcastIp, kPortBroadcast, p, 300);
    }

    void sendTime()
    {
        if (!timeSocket) return;
        // Time Packet size: 162 bytes (includes V3-3-3 Layer OnAir bytes
        // 154-161 for DJM fader data). The PRO DJ LINK Bridge omits these
        // when no DJM is on the network (size 154), but we keep them so
        // real DJ setups can get fader/crossfader position. Receivers that
        // don't care can just ignore the extra 8 bytes.
        uint8_t p[162] = {};
        hdr(p, kMsgTime, 5);
        for (int i = 0; i < kPacketLayers; ++i)
        {
            auto& L = layers[i];
            w32(p + 24 + i*4, L.currentTimeMs);
            w32(p + 56 + i*4, L.trackLenMs);  // Total Time (track length) in ms
            p[88 + i]  = L.beatMarker;
            // Layer State per spec: pure enum (0=IDLE, 3=PLAYING, 4=LOOPING,
            // 5=PAUSED, 6=STOPPED, ...). Resolume reads this byte to drive
            // its play/pause indicator -- it expects literal enum values, so
            // no bit-OR with master flag.
            p[96 + i]  = L.layerState;

            // Per-layer time code block: 6 bytes starting at byte 106.
            // [0] SMPTE Mode: 0 = "use general SMPTE mode from byte 105"
            // [1] TC State:   0=Stopped, 1=Running, 2=Force Resync
            // [2..5] HH:MM:SS:FF
            const int tcBase = 106 + i * 6;
            p[tcBase + 0] = 0;  // defer to general SMPTE mode
            p[tcBase + 1] = (L.layerState == kStatePlaying) ? 1 : 0;
            p[tcBase + 2] = L.tcHours;
            p[tcBase + 3] = L.tcMinutes;
            p[tcBase + 4] = L.tcSeconds;
            p[tcBase + 5] = L.tcFrames;

            p[154 + i] = L.onAir;  // fader position 0-255 (spec V3.3.3+)
        }
        // p[104] is RESERVED per spec; leave at 0.
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
    int globalOffsetMs = 0;  // venue-wide latency comp, see setGlobalOffsetMs
    LayerData layers[kPacketLayers] = {};
    LayerData emptyLayer;
    SlaveNode slaves[kMaxSlaves] = {};
    char nodeName[9] = {};
    uint16_t nodeId = 0x5443;
    uint8_t seq = 0;
    uint32_t uptimeSeconds = 0, tickCount = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TCNetOutput)
};
