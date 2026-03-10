// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter
//
// ProDJLinkInput -- Direct Pro DJ Link protocol implementation.
//
// Connects natively to Pioneer CDJ/XDJ/DJM hardware without requiring
// PRO DJ LINK Bridge or third-party software.  Creates a Virtual CDJ on the
// network and receives player state, beat, and position data directly
// from the players.
//
// Position sources:
//   CDJ-3000:   Absolute Position packets (type 0x0b, 30Hz, ms precision)
//   NXS2/older: Beat-derived from status (beatCount x 60000/BPM, ~5Hz)
//
// Phase 1: UDP monitoring + Dual-keepalive bridge
//   - Bridge join sequence: hello 0x0A (player=5) → IP claim 0x02 (player=5)
//   - Two keepalives sent in parallel:
//     1) 54B BROADCAST (player=0xC1) — DJM discovers bridge, activates fader delivery
//     2) 95B UNICAST to each CDJ (player=5, PIONEER DJ CORP strings) — CDJ registers
//        us as a valid peer and sends AbsPos/Status unicast data
//   - The DJM NEVER receives the 95B (it's unicast to CDJ IPs only)
//     → DJM sees single identity (0xC1) → faders work
//   - The CDJ sees both, but both are from the same bridge → no conflict
//   - Sends type-0x57 subscribe to each DJM (port 50001)
//     -> triggers DJM to send type-0x39 mixer fader packets
//   - Sends type-0x55 bridge notify to each CDJ (port 50002)
//   - Player discovery via keepalive packets  (port 50000)
//   - Beat packets with BPM/pitch/beat info   (port 50001)
//   - Absolute Position from CDJ-3000         (port 50001, type 0x0b)
//   - CDJ status: track ID, play state, pitch (port 50002)
//   - DJM on-air broadcast                    (port 50001, type 0x03)
//   - DJM on-air unicast                      (type 0x29, handled on both 50001 & 50002)
//   - DJM mixer fader data                    (type 0x39, handled on both 50001 & 50002)
//   - Timecode derived from playhead position in ms
//
// Protocol analysis: Deep Symmetry "DJ Link Ecosystem Analysis"
//   https://djl-analysis.deepsymmetry.org/djl-analysis/
// Reference impl: python-prodj-link (flesniak, Apache-2.0)
//   https://github.com/flesniak/python-prodj-link

#pragma once
#include <JuceHeader.h>
#include "TimecodeCore.h"
#include "NetworkUtils.h"
#include <atomic>
#include <array>
#include <cstring>

#ifdef _WIN32
  // Windows includes pulled in by JuceHeader (iphlpapi etc.)
#elif defined(__APPLE__)
  #include <ifaddrs.h>
  #include <net/if_dl.h>
  #include <net/if_types.h>
#elif defined(__linux__)
  #include <sys/ioctl.h>
  #include <net/if.h>
  #include <unistd.h>
#endif

//==============================================================================
// Protocol constants
//==============================================================================
namespace ProDJLink
{
    // Magic header for all Pro DJ Link UDP packets
    static constexpr char kMagic[11] = "Qspt1WmJOL";  // 10 bytes + null
    static constexpr int  kMagicLen   = 10;

    // UDP ports
    static constexpr int kKeepalivePort = 50000;
    static constexpr int kBeatPort      = 50001;
    static constexpr int kStatusPort    = 50002;

    // Keepalive packet types (byte 10)
    static constexpr uint8_t kKeepAliveTypeHello  = 0x0a;
    static constexpr uint8_t kKeepAliveTypeIP     = 0x02;
    static constexpr uint8_t kKeepAliveTypeStatus = 0x06;

    // Beat packet types (byte 10)
    static constexpr uint8_t kBeatTypeBeat        = 0x28;
    static constexpr uint8_t kBeatTypeAbsPosition = 0x0b;
    static constexpr uint8_t kBeatTypeMixer       = 0x03;

    // Status packet types (byte 10)
    static constexpr uint8_t kStatusTypeCDJ    = 0x0a;
    static constexpr uint8_t kStatusTypeMixer  = 0x39;  // DJM fader status (unicast, bridge-triggered)
    static constexpr uint8_t kStatusTypeDJM    = 0x29;  // DJM channel on-air status (unicast, bridge-triggered)
    static constexpr uint8_t kStatusTypeVU     = 0x58;  // DJM VU meter data (unicast, 524B, port 50001)

    // Play states (from status packet, bytes 120-123)
    static constexpr uint32_t kPlayNoTrack   = 0x00;
    static constexpr uint32_t kPlayLoading   = 0x02;
    static constexpr uint32_t kPlayPlaying   = 0x03;
    static constexpr uint32_t kPlayLooping   = 0x04;
    static constexpr uint32_t kPlayPaused    = 0x05;
    static constexpr uint32_t kPlayCued      = 0x06;
    static constexpr uint32_t kPlayCueing    = 0x07;
    static constexpr uint32_t kPlaySeeking   = 0x09;
    static constexpr uint32_t kPlayEndTrack  = 0x11;
    static constexpr uint32_t kPlayEmergency = 0x12;

    // Maximum supported players (CDJ-3000 supports channels 1-6)
    static constexpr int kMaxPlayers = 6;

    // Device type bytes (byte [33] in keepalive packets) -- confirmed from captures:
    //   0x01 = bridge / lighting controller (STC, TCS-SHOWKONTROL)
    //   0x02 = DJM mixer  (player_number >= 0x21)
    //   0x03 = CDJ / XDJ player (player_number 1-6)
    static constexpr uint8_t kDeviceTypeBridge  = 0x01;
    static constexpr uint8_t kDeviceTypeMixer   = 0x02;  // DJM: device_type=0x02, pn >= 0x21
    static constexpr uint8_t kMixerPlayerNumMin = 0x21;  // DJM player numbers start at 33

    // Virtual CDJ defaults
    // Player 5 is used as bridge slot — does NOT occupy CDJ slots 1-4.
    // The CDJ-3000 grants dbserver access to player 5 when the 95-byte
    // keepalive includes the Pioneer bridge identification strings.
    static constexpr int     kDefaultVCDJNumber = 5;
    static constexpr double  kKeepaliveInterval = 1.5;   // seconds
    static constexpr double  kBridgeSubInterval = 2.0;   // seconds between 0x57 re-subscriptions (DJM requires ~2s keepalive)

    // Bridge subscription
    static constexpr uint8_t kBridgeSubType     = 0x57;  // mixer subscribe packet type

    //==========================================================================
    // Byte-level helpers (big-endian)
    //==========================================================================
    inline uint16_t readU16BE(const uint8_t* p) { return (uint16_t(p[0]) << 8) | p[1]; }
    inline uint32_t readU32BE(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16)
                                                       | (uint32_t(p[2]) << 8)  | p[3]; }

    inline void writeU16BE(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
    inline void writeU32BE(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16);
                                                      p[2] = uint8_t(v >> 8);  p[3] = uint8_t(v); }

    //==========================================================================
    // Convert pitch field from protocol value to multiplier
    // Status packets: pitch / 0x100000 (e.g. 0x100000 = 1.0 = 0%)
    // Abs position:   pitch / 100      (e.g. 100 = 1.0 = 0%)
    //==========================================================================
    inline double pitchFromStatus(uint32_t raw) { return double(raw) / double(0x100000); }

    //==========================================================================
    // Convert playhead position (ms) to SMPTE Timecode
    //==========================================================================
    inline Timecode playheadToTimecode(uint32_t playheadMs, FrameRate fps)
    {
        return wallClockToTimecode(double(playheadMs), fps);
    }

    //==========================================================================
    // Play state to human-readable string
    //==========================================================================
    inline const char* playStateToString(uint32_t ps)
    {
        switch (ps)
        {
            case kPlayNoTrack:   return "NO TRACK";
            case kPlayLoading:   return "LOADING";
            case kPlayPlaying:   return "PLAYING";
            case kPlayLooping:   return "LOOPING";
            case kPlayPaused:    return "PAUSED";
            case kPlayCued:      return "CUE";
            case kPlayCueing:    return "CUE PLAY";
            case kPlaySeeking:   return "SEEKING";
            case kPlayEndTrack:  return "END";
            case kPlayEmergency: return "EMERGENCY";
            default:             return "UNKNOWN";
        }
    }
}

//==============================================================================
// Per-player state -- atomics for cross-thread access
//==============================================================================
struct ProDJLinkPlayerState
{
    // Identity (set once on discovery, read-only after)
    std::atomic<bool>     discovered { false };
    std::atomic<uint8_t>  playerNumber { 0 };
    char                  model[21] {};          // null-terminated, written once
    char                  ipStr[16] {};          // "x.x.x.x", written once
    uint8_t               macAddr[6] {};         // written once

    // Timing -- from Absolute Position packets (CDJ-3000, every 30ms)
    // or beat-derived (NXS2: beatCount * 60000 / BPM, from status packets ~5Hz)
    std::atomic<uint32_t> playheadMs   { 0 };    // position in track (ms)
    std::atomic<uint32_t> trackLenSec  { 0 };    // track length (seconds)
    std::atomic<bool>     hasAbsolutePosition { false };    // CDJ-3000: native ms playhead
    std::atomic<bool>     hasBeatDerivedPosition { false }; // NXS2: computed from beatCount+BPM

    // From beat packets
    std::atomic<uint32_t> bpmRaw       { 0 };    // BPM x 100 (from beat) or x10 (from abs pos)
    std::atomic<uint32_t> pitchRaw     { 0x100000 }; // fader pitch multiplier raw (offset 140)
    std::atomic<uint32_t> actualSpeedRaw { 0 };   // real playback speed raw (offset 152)
                                                    // Includes motor ramp -- 0 when stopped,
                                                    // ramps 0->0x100000 during play start,
                                                    // ramps 0x100000->0 during pause.
    std::atomic<uint8_t>  beatInBar    { 1 };    // 1-4

    // From status packets
    std::atomic<uint32_t> trackId      { 0 };    // rekordbox database ID
    std::atomic<uint32_t> playState    { 0 };    // ProDJLink::kPlay* enum
    std::atomic<uint32_t> beatCount    { 0 };    // beat counter
    std::atomic<uint8_t>  loadedPlayer { 0 };    // player where track was loaded from
    std::atomic<uint8_t>  loadedSlot   { 0 };    // 0=empty, 2=SD, 3=USB
    std::atomic<bool>     isMaster     { false };
    std::atomic<bool>     isOnAir      { false };
    std::atomic<bool>     isPlaying    { false }; // derived from state flags

    // Track change detection
    std::atomic<uint32_t> trackVersion { 0 };    // incremented on track change

    // Timing
    std::atomic<double>   lastPacketTime { 0.0 };  // juce::Time::getMillisecondCounterHiRes()
    std::atomic<double>   absPositionTs  { 0.0 };  // timestamp of last abs position (for interpolation)

    void reset()
    {
        // Store discovered=false FIRST with release ordering.
        // UI-thread getters check discovered with acquire ordering —
        // release guarantees they see discovered=false before we zero
        // the non-atomic fields (model, ipStr, macAddr).
        discovered.store(false, std::memory_order_release);

        playerNumber.store(0, std::memory_order_relaxed);
        std::memset(model, 0, sizeof(model));
        std::memset(ipStr, 0, sizeof(ipStr));
        std::memset(macAddr, 0, sizeof(macAddr));
        playheadMs.store(0, std::memory_order_relaxed);
        trackLenSec.store(0, std::memory_order_relaxed);
        hasAbsolutePosition.store(false, std::memory_order_relaxed);
        hasBeatDerivedPosition.store(false, std::memory_order_relaxed);
        bpmRaw.store(0, std::memory_order_relaxed);
        pitchRaw.store(0x100000, std::memory_order_relaxed);
        actualSpeedRaw.store(0, std::memory_order_relaxed);
        beatInBar.store(1, std::memory_order_relaxed);
        trackId.store(0, std::memory_order_relaxed);
        playState.store(0, std::memory_order_relaxed);
        beatCount.store(0, std::memory_order_relaxed);
        loadedPlayer.store(0, std::memory_order_relaxed);
        loadedSlot.store(0, std::memory_order_relaxed);
        isMaster.store(false, std::memory_order_relaxed);
        isOnAir.store(false, std::memory_order_relaxed);
        isPlaying.store(false, std::memory_order_relaxed);
        trackVersion.store(0, std::memory_order_relaxed);
        lastPacketTime.store(0.0, std::memory_order_relaxed);
        absPositionTs.store(0.0, std::memory_order_relaxed);
    }
};

//==============================================================================
// ProDJLinkInput -- main network handler
//==============================================================================
class ProDJLinkInput : public juce::Thread
{
public:
    //--------------------------------------------------------------------------
    // TrackInfo -- per-player track metadata
    //--------------------------------------------------------------------------
    struct TrackInfo
    {
        juce::String artist;
        juce::String title;
    };

    //--------------------------------------------------------------------------
    ProDJLinkInput()
        : Thread("ProDJLink Input")
    {
        for (auto& p : players) p.reset();
    }

    ~ProDJLinkInput() override
    {
        stop();
    }

    //==========================================================================
    // Network interface management
    //==========================================================================
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

    juce::String getBindInfo() const
    {
        return bindIp;
    }

    //==========================================================================
    // Start / Stop
    //==========================================================================
    bool start(int interfaceIndex = 0)
    {
        if (isRunningFlag.load(std::memory_order_relaxed))
            return true;

        refreshNetworkInterfaces();
        if (availableInterfaces.isEmpty())
        {
            DBG("ProDJLink: No network interfaces available");
            return false;
        }

        int idx = juce::jlimit(0, availableInterfaces.size() - 1, interfaceIndex);
        selectedInterface = idx;
        const auto& iface = availableInterfaces[idx];

        bindIp        = iface.ip;
        broadcastIp   = iface.broadcast;
        subnetMask    = iface.subnet;

        // Parse our own IP and MAC for keepalive packets
        parseIpString(iface.ip, ownIpBytes);

        // Get MAC address for this interface
        if (!getMacAddress(iface.name, ownMacBytes))
        {
            // Fallback: use a plausible MAC
            std::memset(ownMacBytes, 0, 6);
            ownMacBytes[0] = 0x02;  // locally administered
            ownMacBytes[5] = uint8_t(vCDJPlayerNumber);
        }

        // --- Create sockets ---
        keepaliveSock = std::make_unique<juce::DatagramSocket>(true);
        keepaliveSock->setEnablePortReuse(true);
        if (!keepaliveSock->bindToPort(ProDJLink::kKeepalivePort))
        {
            DBG("ProDJLink: Failed to bind keepalive socket to port " << ProDJLink::kKeepalivePort);
            keepaliveSock = nullptr;
            return false;
        }

        beatSock = std::make_unique<juce::DatagramSocket>(true);
        beatSock->setEnablePortReuse(true);
        if (!beatSock->bindToPort(ProDJLink::kBeatPort))
        {
            DBG("ProDJLink: WARNING -- Failed to bind beat socket to port " << ProDJLink::kBeatPort
                << " (beat sync unavailable, but status/keepalive will work)");
            beatSock = nullptr;  // non-fatal: continue without beat data
        }

        statusSock = std::make_unique<juce::DatagramSocket>(false);
        statusSock->setEnablePortReuse(true);
        if (!statusSock->bindToPort(ProDJLink::kStatusPort))
        {
            DBG("ProDJLink: Failed to bind status socket to port " << ProDJLink::kStatusPort);
            keepaliveSock = nullptr;
            beatSock = nullptr;
            statusSock = nullptr;
            return false;
        }

        // Reset player states
        for (auto& p : players)
            p.reset();

        isRunningFlag.store(true, std::memory_order_release);
        startThread(juce::Thread::Priority::high);
        return true;
    }

    void stop()
    {
        isRunningFlag.store(false, std::memory_order_release);

        if (keepaliveSock) keepaliveSock->shutdown();
        if (beatSock)      beatSock->shutdown();
        if (statusSock)    statusSock->shutdown();

        stopThread(2000);

        keepaliveSock = nullptr;
        beatSock      = nullptr;
        statusSock    = nullptr;

        vCDJPlayerNumber = ProDJLink::kDefaultVCDJNumber;
        joinCompleted.store(false, std::memory_order_release);
        { const juce::ScopedLock sl(djmIpLock); djmIps.clear(); djmModels.clear(); djmLastSeen.clear(); }

        // Reset mixer state + packet counters for clean restart
        hasMixerData.store(false, std::memory_order_relaxed);
        lastMixerPacketTime.store(0.0, std::memory_order_relaxed);
        for (auto& f : mixerFader) f.store(255, std::memory_order_relaxed);
        mixerCrossfader.store(128, std::memory_order_relaxed);
        mixerMasterFader.store(255, std::memory_order_relaxed);
        hasVuData.store(false, std::memory_order_relaxed);
        lastVuPacketTime.store(0.0, std::memory_order_relaxed);
        pktCountKeepalive.store(0, std::memory_order_relaxed);
        pktCountBeat.store(0, std::memory_order_relaxed);
        pktCountAbsPos.store(0, std::memory_order_relaxed);
        pktCountStatus.store(0, std::memory_order_relaxed);
        pktCountMixer.store(0, std::memory_order_relaxed);
        pktCountVU.store(0, std::memory_order_relaxed);
        pktCountDJMStatus.store(0, std::memory_order_relaxed);
    }

    //==========================================================================
    // Virtual CDJ configuration
    //==========================================================================
    int  getVCDJPlayerNumber() const    { return vCDJPlayerNumber; }
    void setVCDJPlayerNumber(int n)     { vCDJPlayerNumber = juce::jlimit(1, 127, n); }


    //==========================================================================
    // Query API -- per-player state accessors for TimecodeEngine
    //==========================================================================

    /// Is the network thread running?
    bool getIsRunning() const { return isRunningFlag.load(std::memory_order_acquire); }
    int  getSelectedInterface() const { return selectedInterface; }

    /// Are we receiving packets from any player?
    bool isReceiving() const
    {
        double now = juce::Time::getMillisecondCounterHiRes();
        for (int i = 0; i < ProDJLink::kMaxPlayers; ++i)
        {
            if (players[i].discovered.load(std::memory_order_relaxed))
            {
                double last = players[i].lastPacketTime.load(std::memory_order_relaxed);
                if ((now - last) < 5000.0)
                    return true;
            }
        }
        return false;
    }

    /// Get timecode for a given player (1-based player number).
    /// CDJ-3000: converts absolute playhead (ms) to SMPTE timecode.
    /// NXS2/older: converts beat-derived playhead (beatCount x 60000/BPM) to SMPTE.
    /// Raw timecode from CDJ playhead (no smoothing -- use engine PLL for output).
    Timecode getCurrentTimecode(int playerNum) const
    {
        int idx = playerNum - 1;
        if (idx < 0 || idx >= ProDJLink::kMaxPlayers)
            return {};

        const auto& p = players[idx];
        if (!p.discovered.load(std::memory_order_relaxed))
            return {};

        uint32_t baseMs = p.playheadMs.load(std::memory_order_relaxed);
        FrameRate fps = outputFps.load(std::memory_order_relaxed);
        return ProDJLink::playheadToTimecode(baseMs, fps);
    }

    /// Get detected frame rate. ProDJLink doesn't transmit frame rate,
    /// so this returns the user-configured output rate.
    FrameRate getDetectedFrameRate(int /*playerNum*/) const
    {
        return outputFps.load(std::memory_order_relaxed);
    }

    void setOutputFrameRate(FrameRate fps)
    {
        outputFps.store(fps, std::memory_order_relaxed);
    }

    /// Name of the master player (if any)
    juce::String getMasterNodeName() const
    {
        for (int i = 0; i < ProDJLink::kMaxPlayers; ++i)
        {
            if (players[i].discovered.load(std::memory_order_acquire)
                && players[i].isMaster.load(std::memory_order_relaxed))
            {
                return juce::String(players[i].model) + " #"
                     + juce::String((int)players[i].playerNumber.load(std::memory_order_relaxed));
            }
        }
        return {};
    }

    /// Returns the player number (1-6) of the current master, or 0 if no master.
    int getMasterPlayerNumber() const
    {
        for (int i = 0; i < ProDJLink::kMaxPlayers; ++i)
        {
            if (players[i].discovered.load(std::memory_order_acquire)
                && players[i].isMaster.load(std::memory_order_relaxed))
                return (int)players[i].playerNumber.load(std::memory_order_relaxed);
        }
        return 0;
    }

    /// Returns the BPM of the current master player, or 0.0 if no master.
    double getMasterBPM() const
    {
        int master = getMasterPlayerNumber();
        return (master > 0) ? getBPM(master) : 0.0;
    }

    /// Does this player have usable timecode data?
    /// CDJ-3000: absolute position packets provide ms playhead directly.
    /// NXS2/older: position derived from beatCount x (60000/BPM) in status packets.
    bool hasTimecodeData(int playerNum) const
    {
        int idx = playerNum - 1;
        if (idx < 0 || idx >= ProDJLink::kMaxPlayers) return false;
        const auto& p = players[idx];
        return p.discovered.load(std::memory_order_relaxed)
            && (p.hasAbsolutePosition.load(std::memory_order_relaxed)
                || p.hasBeatDerivedPosition.load(std::memory_order_relaxed))
            && p.trackId.load(std::memory_order_relaxed) != 0;
    }

    /// Is the player's playhead actively advancing?
    bool isPositionMoving(int playerNum) const
    {
        int idx = playerNum - 1;
        if (idx < 0 || idx >= ProDJLink::kMaxPlayers) return false;
        uint32_t ps = players[idx].playState.load(std::memory_order_relaxed);
        return ps == ProDJLink::kPlayPlaying
            || ps == ProDJLink::kPlayLooping
            || ps == ProDJLink::kPlayCueing;
    }

    /// Get play state as uint8 (for status display)
    uint8_t getLayerPlayState(int playerNum) const
    {
        int idx = playerNum - 1;
        if (idx < 0 || idx >= ProDJLink::kMaxPlayers) return 0;
        uint32_t ps = players[idx].playState.load(std::memory_order_relaxed);
        // Map to simplified state for display
        switch (ps)
        {
            case ProDJLink::kPlayPlaying:
            case ProDJLink::kPlayLooping:
            case ProDJLink::kPlayCueing:  return 1; // playing
            case ProDJLink::kPlayPaused:
            case ProDJLink::kPlayCued:    return 2; // paused/cued
            default:                      return 0; // idle
        }
    }

    /// Get the play state string for display
    juce::String getPlayStateString(int playerNum) const
    {
        int idx = playerNum - 1;
        if (idx < 0 || idx >= ProDJLink::kMaxPlayers) return "?";
        return ProDJLink::playStateToString(
            players[idx].playState.load(std::memory_order_relaxed));
    }

    /// True if player has reached end of track (CDJ reports playState 0x11).
    /// In this state, actualSpeed freezes at the last playing value and
    /// never ramps to zero — unlike pause where actualSpeed decelerates.
    bool isEndOfTrack(int playerNum) const
    {
        int idx = playerNum - 1;
        if (idx < 0 || idx >= ProDJLink::kMaxPlayers) return false;
        return players[idx].playState.load(std::memory_order_relaxed)
            == ProDJLink::kPlayEndTrack;
    }

    /// Track ID (rekordbox database ID)
    uint32_t getTrackID(int playerNum) const
    {
        int idx = playerNum - 1;
        if (idx < 0 || idx >= ProDJLink::kMaxPlayers) return 0;
        return players[idx].trackId.load(std::memory_order_relaxed);
    }

    /// Track version -- incremented each time the track changes
    uint32_t getTrackVersion(int playerNum) const
    {
        int idx = playerNum - 1;
        if (idx < 0 || idx >= ProDJLink::kMaxPlayers) return 0;
        return players[idx].trackVersion.load(std::memory_order_relaxed);
    }

    /// Track info -- Phase 1 only provides what we can extract from status packets.
    /// Full metadata (artist/title) requires Phase 2 DB queries.
    TrackInfo getTrackInfo(int playerNum) const
    {
        int idx = playerNum - 1;
        if (idx < 0 || idx >= ProDJLink::kMaxPlayers) return {};
        // Phase 1: no metadata yet -- return track ID as title placeholder
        TrackInfo info;
        uint32_t tid = players[idx].trackId.load(std::memory_order_relaxed);
        if (tid != 0)
            info.title = "Track #" + juce::String(tid);
        return info;
    }

    /// BPM for a player (0.0 if unknown)
    double getBPM(int playerNum) const
    {
        int idx = playerNum - 1;
        if (idx < 0 || idx >= ProDJLink::kMaxPlayers) return 0.0;
        const auto& p = players[idx];
        if (p.hasAbsolutePosition.load(std::memory_order_relaxed))
        {
            // Abs position: BPM x 10
            return double(p.bpmRaw.load(std::memory_order_relaxed)) / 10.0;
        }
        // Beat/status packets: BPM x 100
        return double(p.bpmRaw.load(std::memory_order_relaxed)) / 100.0;
    }

    /// Fader pitch multiplier (1.0 = 0%, from status offset 140)
    /// This is the DJ's physical fader setting -- does NOT include motor ramp.
    double getFaderPitch(int playerNum) const
    {
        int idx = playerNum - 1;
        if (idx < 0 || idx >= ProDJLink::kMaxPlayers) return 1.0;
        return ProDJLink::pitchFromStatus(
            players[idx].pitchRaw.load(std::memory_order_relaxed));
    }

    /// Actual playback speed (0.0 = stopped, 1.0 = full speed at fader, from status offset 152)
    /// Includes motor ramp: ramps 0->target on play, target->0 on pause.
    /// This is what the CDJ is ACTUALLY doing right now.
    double getActualSpeed(int playerNum) const
    {
        int idx = playerNum - 1;
        if (idx < 0 || idx >= ProDJLink::kMaxPlayers) return 0.0;
        return ProDJLink::pitchFromStatus(
            players[idx].actualSpeedRaw.load(std::memory_order_relaxed));
    }

    /// Legacy alias -- returns fader pitch for backward compatibility
    double getActualPitch(int playerNum) const { return getFaderPitch(playerNum); }

    /// Returns true if DJM mixer fader data is actively being received.
    /// Goes stale after 5 seconds without a 0x39 packet (DJM offline/disconnected).
    bool hasMixerFaderData() const
    {
        if (!hasMixerData.load(std::memory_order_relaxed)) return false;
        double last = lastMixerPacketTime.load(std::memory_order_relaxed);
        double now  = juce::Time::getMillisecondCounterHiRes();
        return (now - last) < 5000.0;
    }

    /// Channel fader position (0=closed/bottom, 255=fully open/top).
    /// Only valid after hasMixerFaderData() returns true.
    uint8_t getChannelFader(int channel) const
    {
        int idx = channel - 1;
        if (idx < 0 || idx > 3) return 255;
        return mixerFader[idx].load(std::memory_order_relaxed);
    }

    /// Trim/gain knob (0=min, 128=unity, 255=max).
    uint8_t getChannelTrim(int channel) const
    {
        int idx = channel - 1;
        if (idx < 0 || idx > 3) return 128;
        return mixerTrim[idx].load(std::memory_order_relaxed);
    }

    /// EQ High knob (0=full cut, 128=center/flat, 255=full boost).
    uint8_t getChannelEqHi(int channel) const
    {
        int idx = channel - 1;
        if (idx < 0 || idx > 3) return 128;
        return mixerEqHi[idx].load(std::memory_order_relaxed);
    }

    /// EQ Mid knob (0=full cut, 128=center/flat, 255=full boost).
    uint8_t getChannelEqMid(int channel) const
    {
        int idx = channel - 1;
        if (idx < 0 || idx > 3) return 128;
        return mixerEqMid[idx].load(std::memory_order_relaxed);
    }

    /// EQ Low knob (0=full cut, 128=center/flat, 255=full boost).
    uint8_t getChannelEqLo(int channel) const
    {
        int idx = channel - 1;
        if (idx < 0 || idx > 3) return 128;
        return mixerEqLo[idx].load(std::memory_order_relaxed);
    }

    /// Color/FX knob (0=min, 128=center/off, 255=max).
    uint8_t getChannelColor(int channel) const
    {
        int idx = channel - 1;
        if (idx < 0 || idx > 3) return 128;
        return mixerColor[idx].load(std::memory_order_relaxed);
    }

    /// CUE headphone button (0=off, 1=on).
    uint8_t getChannelCue(int channel) const
    {
        int idx = channel - 1;
        if (idx < 0 || idx > 3) return 0;
        return mixerCueBtn[idx].load(std::memory_order_relaxed);
    }

    /// Input source selector (0=USB_A,1=USB_B,2=DIGITAL,3=LINE,4=PHONO,8=RET/AUX).
    uint8_t getChannelInputSrc(int channel) const
    {
        int idx = channel - 1;
        if (idx < 0 || idx > 3) return 3;
        return mixerInputSrc[idx].load(std::memory_order_relaxed);
    }

    /// Crossfader assign (0=THRU, 1=A, 2=B).
    uint8_t getChannelXfAssign(int channel) const
    {
        int idx = channel - 1;
        if (idx < 0 || idx > 3) return 0;
        return mixerXfAssign[idx].load(std::memory_order_relaxed);
    }

    /// Crossfader position (0=fully side-A, 128=center, 255=fully side-B).
    uint8_t getCrossfader()    const { return mixerCrossfader.load(std::memory_order_relaxed); }
    /// Master fader position (0=min, 255=max).
    uint8_t getMasterFader()   const { return mixerMasterFader.load(std::memory_order_relaxed); }
    /// Master CUE button (0=off, 1=on).
    uint8_t getMasterCue()     const { return mixerMasterCue.load(std::memory_order_relaxed); }
    /// Fader curve (0/1/2).
    uint8_t getFaderCurve()    const { return mixerFaderCurve.load(std::memory_order_relaxed); }
    /// Crossfader curve (0/1/2).
    uint8_t getXfCurve()       const { return mixerXfCurve.load(std::memory_order_relaxed); }
    /// Booth monitor level (0-255).
    uint8_t getBoothLevel()    const { return mixerBooth.load(std::memory_order_relaxed); }

    /// Headphone Cue Link (0=off, 1=on).
    uint8_t getHpCueLink()     const { return mixerHpCueLink.load(std::memory_order_relaxed); }
    /// Headphone mixing knob (0=CUE, 255=Master).
    uint8_t getHpMixing()      const { return mixerHpMixing.load(std::memory_order_relaxed); }
    /// Headphone level (0-255).
    uint8_t getHpLevel()       const { return mixerHpLevel.load(std::memory_order_relaxed); }

    /// Beat FX selector (0-13: Delay,Echo,PingPong,Spiral,Reverb,Trans,Filter,Flanger,Phaser,Pitch,SlipRoll,Roll,VinylBrake,Helix).
    uint8_t getBeatFxSelect()  const { return mixerBeatFxSel.load(std::memory_order_relaxed); }
    /// Beat FX level/depth (0-255).
    uint8_t getBeatFxLevel()   const { return mixerBeatFxLevel.load(std::memory_order_relaxed); }
    /// Beat FX on/off (0/1).
    uint8_t getBeatFxOn()      const { return mixerBeatFxOn.load(std::memory_order_relaxed); }
    /// Beat FX channel assign (0=Mic,1=CH1,2=CH2,3=CH3,7=CH4,6=XF-A,8=XF-B,9=Master).
    uint8_t getBeatFxAssign()  const { return mixerBeatFxAssign.load(std::memory_order_relaxed); }
    /// FX frequency band buttons.
    uint8_t getFxFreqLo()      const { return mixerFxFreqLo.load(std::memory_order_relaxed); }
    uint8_t getFxFreqMid()     const { return mixerFxFreqMid.load(std::memory_order_relaxed); }
    uint8_t getFxFreqHi()      const { return mixerFxFreqHi.load(std::memory_order_relaxed); }
    /// Send/Return level (0-255).
    uint8_t getSendReturnLevel() const { return mixerSendReturn.load(std::memory_order_relaxed); }

    /// Color FX selector (255=OFF, 0=Space,1=DubEcho,2=Sweep,3=Noise,4=Crush,5=Filter).
    uint8_t getColorFxSelect() const { return mixerColorFxSel.load(std::memory_order_relaxed); }
    /// Color FX parameter knob (0-255).
    uint8_t getColorFxParam()  const { return mixerColorFxParam.load(std::memory_order_relaxed); }
    /// Color FX channel assign (same enum as Beat FX assign).
    uint8_t getColorFxAssign() const { return mixerColorFxAssign.load(std::memory_order_relaxed); }

    /// Mic EQ High (0-255, 128=center).
    uint8_t getMicEqHi()       const { return mixerMicEqHi.load(std::memory_order_relaxed); }
    /// Mic EQ Low (0-255, 128=center).
    uint8_t getMicEqLo()       const { return mixerMicEqLo.load(std::memory_order_relaxed); }

    /// Has VU meter data been received recently?
    bool hasVuMeterData() const
    {
        if (!hasVuData.load(std::memory_order_relaxed)) return false;
        double last = lastVuPacketTime.load(std::memory_order_relaxed);
        double now  = juce::Time::getMillisecondCounterHiRes();
        return (now - last) < 5000.0;
    }

    /// VU peak level for a channel (0=silence, 32767=clip).
    /// ch: 0=CH1, 1=CH2, 2=CH3, 3=CH4, 4=MasterL, 5=MasterR
    uint16_t getVuPeak(int ch) const
    {
        if (ch < 0 || ch > 5) return 0;
        return vuPeak[ch].load(std::memory_order_relaxed);
    }

    /// VU peak as normalised float 0.0-1.0.
    float getVuPeakNorm(int ch) const { return float(getVuPeak(ch)) / 32767.0f; }

    /// Copy all 15 VU segments for one channel into dst[15].
    /// ch: 0=CH1, 1=CH2, 2=CH3, 3=CH4, 4=MasterL, 5=MasterR
    void getVuSegments(int ch, uint16_t dst[15]) const
    {
        if (ch < 0 || ch > 5) { std::memset(dst, 0, 15 * sizeof(uint16_t)); return; }
        const juce::SpinLock::ScopedLockType sl(vuDataLock);
        std::memcpy(dst, vuSegments[ch], 15 * sizeof(uint16_t));
    }

    /// Model name of the first known DJM mixer (empty if none discovered yet).
    juce::String getDJMModel() const
    {
        const juce::ScopedLock sl(djmIpLock);
        return djmModels.empty() ? juce::String() : juce::String(djmModels[0]);
    }

    /// Playhead position in milliseconds
    uint32_t getPlayheadMs(int playerNum) const
    {
        int idx = playerNum - 1;
        if (idx < 0 || idx >= ProDJLink::kMaxPlayers) return 0;
        return players[idx].playheadMs.load(std::memory_order_relaxed);
    }

    /// Timestamp (ms hi-res) of last absolute position packet -- for PLL new-packet detection
    double getAbsPositionTs(int playerNum) const
    {
        int idx = playerNum - 1;
        if (idx < 0 || idx >= ProDJLink::kMaxPlayers) return 0.0;
        return players[idx].absPositionTs.load(std::memory_order_relaxed);
    }

    /// True if player is actively playing (for PLL advance)
    bool isPlayerPlaying(int playerNum) const
    {
        int idx = playerNum - 1;
        if (idx < 0 || idx >= ProDJLink::kMaxPlayers) return false;
        uint32_t ps = players[idx].playState.load(std::memory_order_relaxed);
        return (ps == ProDJLink::kPlayPlaying || ps == ProDJLink::kPlayLooping);
    }

    /// Track length in seconds
    uint32_t getTrackLengthSec(int playerNum) const
    {
        int idx = playerNum - 1;
        if (idx < 0 || idx >= ProDJLink::kMaxPlayers) return 0;
        return players[idx].trackLenSec.load(std::memory_order_relaxed);
    }

    /// Set track length from external source (e.g. dbserver metadata).
    /// Used by NXS2/older players that don't report duration in protocol packets.
    void setTrackLengthSec(int playerNum, uint32_t sec)
    {
        int idx = playerNum - 1;
        if (idx < 0 || idx >= ProDJLink::kMaxPlayers) return;
        // Only write if not already set by absolute position packets (CDJ-3000)
        if (!players[idx].hasAbsolutePosition.load(std::memory_order_relaxed))
            players[idx].trackLenSec.store(sec, std::memory_order_relaxed);
    }

    /// Play position as 0.0 -> 1.0 ratio (for waveform cursor)
    float getPlayPositionRatio(int playerNum) const
    {
        int idx = playerNum - 1;
        if (idx < 0 || idx >= ProDJLink::kMaxPlayers) return 0.0f;
        const auto& p = players[idx];
        uint32_t len = p.trackLenSec.load(std::memory_order_relaxed);
        if (len == 0) return 0.0f;
        uint32_t baseMs = p.playheadMs.load(std::memory_order_relaxed);
        return float(double(baseMs) / (double(len) * 1000.0));
    }

    /// Get list of discovered player numbers
    juce::Array<int> getDiscoveredPlayers() const
    {
        juce::Array<int> result;
        for (int i = 0; i < ProDJLink::kMaxPlayers; ++i)
        {
            if (players[i].discovered.load(std::memory_order_relaxed))
                result.add(i + 1);
        }
        return result;
    }

    /// Get model name for a player
    juce::String getPlayerModel(int playerNum) const
    {
        int idx = playerNum - 1;
        if (idx < 0 || idx >= ProDJLink::kMaxPlayers) return {};
        if (!players[idx].discovered.load(std::memory_order_acquire)) return {};
        return juce::String(players[idx].model);
    }

    /// Get IP address string for a player (Phase 2: needed by DbServerClient)
    juce::String getPlayerIP(int playerNum) const
    {
        int idx = playerNum - 1;
        if (idx < 0 || idx >= ProDJLink::kMaxPlayers) return {};
        if (!players[idx].discovered.load(std::memory_order_acquire)) return {};
        return juce::String(players[idx].ipStr);
    }

    /// Get loaded media slot for a player (0=empty, 2=SD, 3=USB, 4=CD)
    /// Phase 2: needed by DbServerClient to form DMST argument
    uint8_t getLoadedSlot(int playerNum) const
    {
        int idx = playerNum - 1;
        if (idx < 0 || idx >= ProDJLink::kMaxPlayers) return 0;
        return players[idx].loadedSlot.load(std::memory_order_relaxed);
    }

    /// Get the player that holds the media from which the track was loaded.
    /// If CDJ-2 loads a track from CDJ-1's USB, this returns 1.
    /// Phase 2: DbServerClient connects to THIS player for metadata queries.
    uint8_t getLoadedPlayer(int playerNum) const
    {
        int idx = playerNum - 1;
        if (idx < 0 || idx >= ProDJLink::kMaxPlayers) return 0;
        return players[idx].loadedPlayer.load(std::memory_order_relaxed);
    }

    /// Does this player support absolute position? (CDJ-3000 only)
    bool playerHasAbsolutePosition(int playerNum) const
    {
        int idx = playerNum - 1;
        if (idx < 0 || idx >= ProDJLink::kMaxPlayers) return false;
        return players[idx].hasAbsolutePosition.load(std::memory_order_relaxed);
    }

    /// Does this player use beat-derived position? (NXS2 / older models)
    bool playerHasBeatDerivedPosition(int playerNum) const
    {
        int idx = playerNum - 1;
        if (idx < 0 || idx >= ProDJLink::kMaxPlayers) return false;
        return players[idx].hasBeatDerivedPosition.load(std::memory_order_relaxed);
    }

    /// Position source string for display/debug
    juce::String getPositionSourceString(int playerNum) const
    {
        if (playerHasAbsolutePosition(playerNum))  return "ABS";
        if (playerHasBeatDerivedPosition(playerNum)) return "BEAT";
        return "NONE";
    }

    /// Is the given player on-air? (from CDJ status flags or DJM 0x29 packets)
    bool isPlayerOnAir(int playerNum) const
    {
        int idx = playerNum - 1;
        if (idx < 0 || idx >= ProDJLink::kMaxPlayers) return false;
        return players[idx].isOnAir.load(std::memory_order_relaxed);
    }

    /// Is the given player the current master?
    bool isPlayerMaster(int playerNum) const
    {
        int idx = playerNum - 1;
        if (idx < 0 || idx >= ProDJLink::kMaxPlayers) return false;
        return players[idx].isMaster.load(std::memory_order_relaxed);
    }

    /// Beat position within bar (1-4)
    uint8_t getBeatInBar(int playerNum) const
    {
        int idx = playerNum - 1;
        if (idx < 0 || idx >= ProDJLink::kMaxPlayers) return 0;
        return players[idx].beatInBar.load(std::memory_order_relaxed);
    }

    /// Is a given player discovered on the network?
    bool isPlayerDiscovered(int playerNum) const
    {
        int idx = playerNum - 1;
        if (idx < 0 || idx >= ProDJLink::kMaxPlayers) return false;
        return players[idx].discovered.load(std::memory_order_relaxed);
    }

    /// Currently not applicable to ProDJLink -- always returns false
    bool isUsingCurrentTimeFallback(int /*playerNum*/) const { return false; }
    bool isAutoAB() const { return false; }
    int  getDisplayLayer() const { return selectedPlayer.load(std::memory_order_relaxed); }

    /// Which player to follow (1-based, used by TimecodeEngine)
    int  getSelectedPlayer() const { return selectedPlayer.load(std::memory_order_relaxed); }
    void setSelectedPlayer(int p)  { selectedPlayer.store(juce::jlimit(1, ProDJLink::kMaxPlayers, p),
                                                           std::memory_order_relaxed); }

    //==========================================================================
    // Diagnostic info
    //==========================================================================
    juce::String getPacketStats() const
    {
        return "KA:" + juce::String((int)pktCountKeepalive.load(std::memory_order_relaxed))
             + " BT:" + juce::String((int)pktCountBeat.load(std::memory_order_relaxed))
             + " AP:" + juce::String((int)pktCountAbsPos.load(std::memory_order_relaxed))
             + " ST:" + juce::String((int)pktCountStatus.load(std::memory_order_relaxed))
             + " MX:" + juce::String((int)pktCountMixer.load(std::memory_order_relaxed))
             + " VU:" + juce::String((int)pktCountVU.load(std::memory_order_relaxed))
             + " DJ:" + juce::String((int)pktCountDJMStatus.load(std::memory_order_relaxed));
    }

private:
    //==========================================================================
    // Thread loop
    //==========================================================================
    void run() override
    {
        DBG("ProDJLink: Thread started, bound to " << bindIp);

        // --- Bridge announce sequence (must happen before keepalives) ---
        // Bridge announce: hello 0x0A (player=5) → claim 0x02 (player=0xC0)
        // → then starts 54B keepalive 0x06 (player=0xC1).
        // The DJM needs to see this announce before it activates fader delivery.
        performBridgeJoinSequence();

        double lastKeepaliveSend  = 0.0;
        double lastBridgeSubSend  = 0.0;
        double lastBridgeNotify   = 0.0;

        while (!threadShouldExit() && isRunningFlag.load(std::memory_order_acquire))
        {
            double now = juce::Time::getMillisecondCounterHiRes();

            // --- Dual keepalive system ---
            // Two keepalives serve different purposes:
            //
            // 1) 54B bridge keepalive BROADCAST (player=0xC1)
            //    → DJM discovers us as bridge → activates fader (0x39) delivery
            //    → This is the standard bridge broadcast keepalive
            //
            // 2) 95B dbserver keepalive UNICAST to each CDJ (player=5)
            //    → CDJ-3000 registers us as a valid peer with PIONEER identification
            //    → CDJ sends unicast Status + AbsPos data to our IP
            //
            // CRITICAL: The 95B MUST be unicast to CDJ IPs only — NEVER broadcast.
            // If the DJM receives both player=5 (95B) and player=0xC1 (54B) from
            // the same IP/MAC, it detects conflicting identities and refuses faders.
            // By unicasting the 95B, the DJM only ever sees the 54B broadcast.
            if ((now - lastKeepaliveSend) >= ProDJLink::kKeepaliveInterval * 1000.0)
            {
                sendBridgeKeepalive();           // 54B BROADCAST → DJM faders
                sendDbServerKeepaliveToAll();     // 95B UNICAST to CDJs → CDJ status data
                lastKeepaliveSend = now;
            }

            // --- Send bridge subscribe (0x57) to all known DJMs ---
            // This triggers the DJM to send type-0x39 mixer fader packets and
            // type-0x29 channel on-air status packets.
            // Sent immediately on first DJM discovery, then every 2s to renew.
            if ((now - lastBridgeSubSend) >= ProDJLink::kBridgeSubInterval * 1000.0)
            {
                sendBridgeSubscribeToAll();
                lastBridgeSubSend = now;
            }

            // --- Send 0x55 bridge notify to all known CDJs ---
            // The bridge sends this to CDJs on port 50002 periodically.
            // May be required to maintain the bridge session.
            if ((now - lastBridgeNotify) >= 2000.0)
            {
                sendBridgeNotifyToAll();
                lastBridgeNotify = now;
            }

            // --- Poll sockets ---

            // Keepalive (port 50000) -- ~1Hz per player, low priority
            if (keepaliveSock && keepaliveSock->waitUntilReady(true, 2))
            {
                uint8_t buf[256];
                juce::String sender;
                int port = 0;
                int n = keepaliveSock->read(buf, sizeof(buf), false, sender, port);
                if (n > 0)
                    handleKeepalivePacket(buf, n, sender);
            }

            // Beat (port 50001) -- 30ms interval for abs position, drain multiple
            // Buffer must accommodate: 0x0b abs pos (~60B), 0x28 beat (~96B),
            // 0x03 on-air (45B), 0x39 fader (248B), 0x58 DJM response (524B)
            if (beatSock)
            {
                int drained = 0;
                while (drained < 20 && beatSock->waitUntilReady(true, 1))
                {
                    uint8_t buf[600];
                    juce::String sender;
                    int port = 0;
                    int n = beatSock->read(buf, sizeof(buf), false, sender, port);
                    if (n > 0)
                        handleBeatPacket(buf, n);
                    ++drained;
                }
            }

            // Status (port 50002) -- ~5Hz per player
            if (statusSock)
            {
                int drained = 0;
                while (drained < 10 && statusSock->waitUntilReady(true, 1))
                {
                    uint8_t buf[1200];  // CDJ-3000 sends 0x200 = 512 bytes
                    juce::String sender;
                    int port = 0;
                    int n = statusSock->read(buf, sizeof(buf), false, sender, port);
                    if (n > 0)
                        handleStatusPacket(buf, n);
                    ++drained;
                }
            }

            // GC: remove stale players
            gcPlayers(now);
        }

        DBG("ProDJLink: Thread stopped");
    }

    //==========================================================================
    // Dual keepalive system — CDJ + DJM compatible
    //
    // Two keepalives are sent in parallel, each serving a different device:
    //
    //   1) sendBridgeKeepalive()       — 54B BROADCAST (player=0xC1)
    //      → DJM discovers us as bridge → activates fader (0x39) delivery
    //      → Identical to TCS-SHOWKONTROL's broadcast keepalive
    //
    //   2) sendDbServerKeepaliveToAll() — 95B UNICAST to each CDJ (player=5)
    //      → CDJ-3000 validates PIONEER DJ CORP / PRODJLINK BRIDGE strings
    //      → CDJ registers us as a valid peer → sends Status + AbsPos unicast
    //
    // The DJM NEVER sees the 95B packet (it's sent unicast to CDJ IPs only).
    // From the DJM's perspective, we have a single identity: player=0xC1.
    //
    // History:
    //   - v1.5a: 95B BROADCAST + 54B UNICAST → CDJ✅ DJM❌ (DJM saw 2 identities)
    //   - v1.5b: 54B BROADCAST only          → CDJ❌ DJM✅ (CDJ ignored player=0xC1)
    //   - v1.5c: 54B BROADCAST + 95B UNICAST → CDJ✅ DJM✅ (DJM only sees broadcast)
    //==========================================================================

    // dbserver keepalive (0x06) — 95B UNICAST to each discovered CDJ.
    // Contains "PIONEER DJ CORP" / "PRODJLINK BRIDGE" identification strings
    // that CDJ-3000 validates for granting dbserver metadata access and
    // for sending unicast Status + AbsPos data to our IP.
    //
    // CRITICAL: This must be UNICAST to CDJ IPs only — NEVER broadcast.
    // The DJM must only see the 54B bridge keepalive (player=0xC1).
    // If the DJM sees this 95B packet (player=5), it registers a conflicting
    // identity from the same IP and refuses to activate fader delivery.
    void sendDbServerKeepalive(const juce::String& cdjIp)
    {
        if (!keepaliveSock) return;

        uint8_t pkt[95];
        std::memset(pkt, 0, sizeof(pkt));

        std::memcpy(pkt, ProDJLink::kMagic, ProDJLink::kMagicLen);

        pkt[10] = ProDJLink::kKeepAliveTypeStatus;  // 0x06
        std::strncpy(reinterpret_cast<char*>(pkt + 12), "TCS-SHOWKONTROL", 19);

        pkt[32] = 0x01;
        pkt[33] = ProDJLink::kDeviceTypeBridge;  // 0x01
        pkt[35] = 0x36;

        pkt[36] = uint8_t(vCDJPlayerNumber);     // 5
        std::memcpy(pkt + 38, ownMacBytes, 6);
        std::memcpy(pkt + 44, ownIpBytes, 4);
        pkt[53] = 0x20;

        // Pioneer bridge identification (bytes 54-94) — REQUIRED for dbserver access
        std::strncpy(reinterpret_cast<char*>(pkt + 54), "PIONEER DJ CORP", 19);
        std::strncpy(reinterpret_cast<char*>(pkt + 74), "PRODJLINK BRIDGE", 19);
        pkt[94] = 0x43;  // 'C'

        keepaliveSock->write(cdjIp, ProDJLink::kKeepalivePort, pkt, sizeof(pkt));
    }

    void sendDbServerKeepaliveToAll()
    {
        for (int i = 0; i < ProDJLink::kMaxPlayers; ++i)
        {
            if (!players[i].discovered.load(std::memory_order_relaxed)) continue;
            juce::String ip(players[i].ipStr);
            if (ip.isNotEmpty())
                sendDbServerKeepalive(ip);
        }
    }

    // Bridge keepalive (0x06) — 54B BROADCAST
    // Byte-for-byte clone of TCS-SHOWKONTROL: variant=0x01, player=0xC1,
    // class=0x00, flags=0x05, last=0x20. DJM activates faders on seeing this.
    //
    // CRITICAL: This must be BROADCAST, not unicast to the DJM.
    // The bridge broadcasts this and the DJM discovers it via
    // standard keepalive broadcast monitoring on port 50000. Sending as
    // unicast does not register in the DJM's device table.
    void sendBridgeKeepalive()
    {
        if (!keepaliveSock) return;
        uint8_t pkt[54];
        std::memset(pkt, 0, sizeof(pkt));
        std::memcpy(pkt, ProDJLink::kMagic, ProDJLink::kMagicLen);
        pkt[0x0a] = 0x06;
        std::strncpy(reinterpret_cast<char*>(pkt + 0x0c), "TCS-SHOWKONTROL", 19);
        pkt[0x20] = 0x01;  pkt[0x21] = 0x01;  pkt[0x23] = 0x36;
        pkt[0x24] = 0xC1;  pkt[0x25] = 0x00;
        std::memcpy(pkt + 0x26, ownMacBytes, 6);
        std::memcpy(pkt + 0x2c, ownIpBytes, 4);
        pkt[0x30] = 0x03;  pkt[0x34] = 0x05;  pkt[0x35] = 0x20;
        keepaliveSock->write(broadcastIp, ProDJLink::kKeepalivePort, pkt, sizeof(pkt));
    }

    // (Old CDJ 4-phase join sequence removed — STC uses bridge join only)

    //==========================================================================
    // Bridge join sequence — announces our presence on the network.
    //
    // From capture analysis: the DJM responds in <0.2s after the FIRST
    // 54B keepalive broadcast. The 21 claims the reference implementation sends are its
    // own slot-reservation process — not a DJM requirement.
    //
    // We send 1 hello + 3 claims (standard Pro DJ Link network announce)
    // then immediately start the keepalive loop. Total: ~3 seconds.
    //==========================================================================
    void performBridgeJoinSequence()
    {
        if (!keepaliveSock) return;
        DBG("ProDJLink: Starting bridge join sequence");

        // Hello announce
        if (threadShouldExit()) return;
        sendBridgeJoinHello();
        juce::Thread::sleep(500);

        // 3 IP claims (~1/sec, standard network announce)
        for (int n = 1; n <= 3; ++n)
        {
            if (threadShouldExit()) return;
            sendBridgeJoinClaim(n);
            juce::Thread::sleep(800);
        }

        joinCompleted.store(true, std::memory_order_release);
        DBG("ProDJLink: Bridge join complete - keepalive starting");
    }

    //--------------------------------------------------------------------------
    // Bridge hello (0x0A) — 37 bytes broadcast
    //
    // Packet format:
    //   [0x00-0x09] Magic "Qspt1WmJOL"
    //   [0x0a]      0x0A (type = hello)
    //   [0x0b]      0x00
    //   [0x0c-0x1f] Device name (20 bytes, null-padded)
    //   [0x20]      0x01
    //   [0x21]      0x01  (subtype = bridge)
    //   [0x22]      0x00
    //   [0x23]      0x25  (packet length = 37)
    //   [0x24]      player number (5)
    //--------------------------------------------------------------------------
    void sendBridgeJoinHello()
    {
        uint8_t p[37] = {};
        std::memcpy(p, ProDJLink::kMagic, 10);
        p[0x0a] = 0x0a;  // type = hello
        // p[0x0b] = 0x00 (already zero)
        std::strncpy(reinterpret_cast<char*>(p + 0x0c), "TCS-SHOWKONTROL", 19);
        p[0x20] = 0x01;
        p[0x21] = 0x01;  // subtype = bridge
        // p[0x22] = 0x00;
        p[0x23] = 0x25;  // length = 37
        p[0x24] = uint8_t(vCDJPlayerNumber);  // player = 5
        keepaliveSock->write(broadcastIp, ProDJLink::kKeepalivePort, p, sizeof(p));
    }

    //--------------------------------------------------------------------------
    // Bridge IP claim (0x02) — 50 bytes broadcast
    //
    // Packet format:
    //   [0x00-0x09] Magic
    //   [0x0a]      0x02 (type = IP claim)
    //   [0x0b]      0x00
    //   [0x0c-0x1f] Device name (20 bytes)
    //   [0x20]      0x01
    //   [0x21]      0x01  (subtype = bridge)
    //   [0x22]      0x00
    //   [0x23]      0x32  (packet length = 50)
    //   [0x24-0x27] IP address
    //   [0x28-0x2d] MAC address
    //   [0x2e]      auto-increment byte (varies, protocol-internal)
    //   [0x2f]      counter (1, 2, 3, ... up to claim round count)
    //   [0x30]      player number (5)
    //   [0x31]      0x00
    //--------------------------------------------------------------------------
    void sendBridgeJoinClaim(int counter)
    {
        uint8_t p[50] = {};
        std::memcpy(p, ProDJLink::kMagic, 10);
        p[0x0a] = 0x02;  // type = IP claim
        std::strncpy(reinterpret_cast<char*>(p + 0x0c), "TCS-SHOWKONTROL", 19);
        p[0x20] = 0x01;
        p[0x21] = 0x01;  // subtype = bridge
        p[0x23] = 0x32;  // length = 50
        std::memcpy(p + 0x24, ownIpBytes, 4);   // IP
        std::memcpy(p + 0x28, ownMacBytes, 6);   // MAC
        // [0x2e] = auto-increment token (XOR-based hash — the exact value
        // doesn't appear to affect DJM fader activation, only the overall
        // claim structure matters)
        p[0x2e] = uint8_t(ownMacBytes[5] ^ uint8_t(counter * 3 + 0xFB));
        p[0x2f] = uint8_t(counter);              // counter
        p[0x30] = uint8_t(vCDJPlayerNumber);      // player = 5
        p[0x31] = 0x00;
        keepaliveSock->write(broadcastIp, ProDJLink::kKeepalivePort, p, sizeof(p));
    }

    //==========================================================================
    // Bridge notify (0x55) — sent to each CDJ on port 50002.
    //
    // The bridge sends this periodically to maintain the session.
    // Packet format (44 bytes) from capture:
    //   [0-9]   Magic "Qspt1WmJOL"
    //   [10]    0x55 (type)
    //   [11-30] Device name "TCS-SHOWKONTROL" + null padding
    //   [31]    0x01
    //   [32-33] 0x00 0x8B
    //   [34]    0x08
    //   [35-39] 0x00 0x00 0x00 0x00 0x01
    //   [40]    player_number (5)
    //   [41]    0x01
    //   [42]    0x03
    //   [43]    0x01
    //==========================================================================
    void sendBridgeNotify(const juce::String& cdjIp)
    {
        if (!statusSock) return;
        uint8_t pkt[44] = {};
        std::memcpy(pkt, ProDJLink::kMagic, ProDJLink::kMagicLen);
        pkt[10] = 0x55;
        std::strncpy(reinterpret_cast<char*>(pkt + 11), "TCS-SHOWKONTROL", 15);
        pkt[31] = 0x01;
        pkt[32] = 0x00;  pkt[33] = 0x8B;
        pkt[34] = 0x08;
        // bytes 35-38 = 0x00
        pkt[39] = 0x01;
        pkt[40] = uint8_t(vCDJPlayerNumber);  // 5
        pkt[41] = 0x01;
        pkt[42] = 0x03;
        pkt[43] = 0x01;
        statusSock->write(cdjIp, ProDJLink::kStatusPort, pkt, sizeof(pkt));
    }

    void sendBridgeNotifyToAll()
    {
        // Send 0x55 to all discovered CDJ players (not DJMs)
        for (int i = 0; i < ProDJLink::kMaxPlayers; ++i)
        {
            if (!players[i].discovered.load(std::memory_order_relaxed)) continue;
            juce::String ip(players[i].ipStr);
            if (ip.isNotEmpty())
                sendBridgeNotify(ip);
        }
    }

    //==========================================================================
    // DJM on-air broadcast handler (type 0x03, port 50001)
    //
    // The DJM broadcasts this on port 50001 regardless of bridge presence.
    // Contains per-channel on-air flags (simpler than 0x29 unicast).
    // Format (45 bytes, from capture):
    //   [10]    0x03 (type)
    //   [33]    player_number (0x21 = DJM)
    //   [35]    0x09 (payload length?)
    //   [36]    ch1 on-air (0x01=on, 0x00=off)
    //   [37]    ch2 on-air
    //   [38]    ch3 on-air
    //   [39]    ch4 on-air
    //==========================================================================
    void handleOnAirBroadcast(const uint8_t* data, int len)
    {
        if (len < 40) return;

        bool ch1 = (data[36] != 0x00);
        bool ch2 = (data[37] != 0x00);
        bool ch3 = (data[38] != 0x00);
        bool ch4 = (data[39] != 0x00);

        // Update player on-air state (only if we don't have 0x29 unicast data,
        // which is more reliable since it's DJM→bridge specific)
        if (pktCountDJMStatus.load(std::memory_order_relaxed) == 0)
        {
            for (int i = 0; i < ProDJLink::kMaxPlayers; ++i)
            {
                if (!players[i].discovered.load(std::memory_order_relaxed)) continue;
                uint8_t pn = players[i].playerNumber.load(std::memory_order_relaxed);
                bool onAir = false;
                switch (pn)
                {
                    case 1: onAir = ch1; break;
                    case 2: onAir = ch2; break;
                    case 3: onAir = ch3; break;
                    case 4: onAir = ch4; break;
                    default: continue;
                }
                players[i].isOnAir.store(onAir, std::memory_order_relaxed);
            }
        }

        DBG("ProDJLink: 0x03 on-air broadcast ch1=" << (int)ch1 << " ch2=" << (int)ch2
            << " ch3=" << (int)ch3 << " ch4=" << (int)ch4);
    }

    /// Register a DJM IP + model for bridge subscription.
    /// Thread-safe: called from handleKeepalivePacket (network thread).
    void registerDJM(const std::string& ip, const std::string& model)
    {
        bool isNew = false;
        {
            const juce::ScopedLock sl(djmIpLock);

            // Update timestamp if already known
            for (size_t i = 0; i < djmIps.size(); ++i)
            {
                if (djmIps[i] == ip)
                {
                    djmLastSeen[i] = juce::Time::getMillisecondCounterHiRes();
                    return;
                }
            }

            // New DJM — add to list (subscribe outside the lock)
            djmIps.push_back(ip);
            djmModels.push_back(model);
            djmLastSeen.push_back(juce::Time::getMillisecondCounterHiRes());
            isNew = true;
        }
        // Socket write OUTSIDE the lock — avoids blocking UI thread on getDJMModel()
        if (isNew)
        {
            DBG("ProDJLink: Registered DJM [" << juce::String(model) << "] at " << juce::String(ip));
            sendBridgeSubscribe(ip);
        }
    }

    // Bridge subscribe (0x57) — 40B sent to DJM on beat port (50001) ONLY.
    // This is the format that triggers DJM fader (0x39) and on-air (0x29) delivery.
    // The DJM sends 0x39 faders back to us on port 50002 (status port).
    // NOTE: Bridge subscribe goes to 50001 only — sending to 50000 is wrong.
    void sendBridgeSubscribe(const std::string& djmIp)
    {
        if (!beatSock) return;
        uint8_t pkt[40];
        std::memset(pkt, 0, sizeof(pkt));
        std::memcpy(pkt, ProDJLink::kMagic, ProDJLink::kMagicLen);
        pkt[10] = ProDJLink::kBridgeSubType;  // 0x57
        const char* name = "TCS-SHOWKONTROL";
        std::strncpy(reinterpret_cast<char*>(pkt + 11), name, 15);
        pkt[31] = 0x01;
        pkt[32] = 0x00;
        pkt[33] = 0x87;  // payload_len
        pkt[34] = 0x00;
        pkt[35] = 0x04;  // subtype
        pkt[36] = 0x01;  // subscribe = 1

        beatSock->write(juce::String(djmIp), ProDJLink::kBeatPort, pkt, sizeof(pkt));
        DBG("ProDJLink: Sent 0x57 subscribe to " << juce::String(djmIp)
            << " port " << ProDJLink::kBeatPort);
    }

    void sendBridgeSubscribeToAll()
    {
        // Copy IPs under lock, then write outside — same pattern as registerDJM.
        // Avoids blocking UI thread (getDJMModel) during socket writes.
        std::vector<std::string> ipsCopy;
        {
            const juce::ScopedLock sl(djmIpLock);
            ipsCopy = djmIps;
        }
        for (auto& ip : ipsCopy)
            sendBridgeSubscribe(ip);
    }

    //==========================================================================
    // Packet handlers
    //==========================================================================

    void handleKeepalivePacket(const uint8_t* data, int len, const juce::String& sender)
    {
        // Minimum keepalive size: 10 (magic) + 1 (type) + ... >= 36 bytes for type_status
        if (len < 36) return;
        if (std::memcmp(data, ProDJLink::kMagic, ProDJLink::kMagicLen) != 0) return;

        uint8_t type = data[10];

        // Only process type_status (0x06) -- the standard keepalive
        if (type != ProDJLink::kKeepAliveTypeStatus && type != ProDJLink::kKeepAliveTypeIP)
            return;

        pktCountKeepalive.fetch_add(1, std::memory_order_relaxed);

        uint8_t pn = 0;
        if (type == ProDJLink::kKeepAliveTypeStatus && len >= 54)
            pn = data[36];  // player_number in content
        else if (type == ProDJLink::kKeepAliveTypeIP && len >= 48)
            pn = data[46];  // player_number at different offset in type_ip

        if (pn == 0 || pn == uint8_t(vCDJPlayerNumber) || pn == 0xC0 || pn == 0xC1) return;  // ignore self + bridge identities

        // Detect DJM mixers: device_type=0x02 AND player_number >= 0x21.
        // (CDJs also use device_type=0x02 but have player_number 1-6;
        //  DJM-900NXS2 uses player_number=0x21=33, other DJMs may vary but
        //  all confirmed > 6, so pn >= kMixerPlayerNumMin is reliable.)
        if (type == ProDJLink::kKeepAliveTypeStatus && len >= 54)
        {
            uint8_t devType = data[33];
            uint8_t djmPn  = data[36];
            if (devType == ProDJLink::kDeviceTypeMixer
                && djmPn >= ProDJLink::kMixerPlayerNumMin)
            {
                auto ipStr = sender.toStdString();
                // Extract model name from packet bytes [12..26]
                char modelBuf[21] = {};
                int copyLen = std::min(20, len - 12);
                if (copyLen > 0) std::memcpy(modelBuf, data + 12, copyLen);
                for (int ci = 0; ci < 20 && modelBuf[ci]; ++ci)
                    if (static_cast<unsigned char>(modelBuf[ci]) > 127) modelBuf[ci] = '?';
                registerDJM(ipStr, std::string(modelBuf));
            }
        }

        if (pn > ProDJLink::kMaxPlayers) return;  // only track players 1-6

        int idx = pn - 1;
        auto& p = players[idx];

        if (!p.discovered.load(std::memory_order_relaxed))
        {
            p.playerNumber.store(pn, std::memory_order_relaxed);
            // Copy model name -- sanitize to pure ASCII for safe String construction
            std::memset(p.model, 0, sizeof(p.model));
            {
                int copyLen = std::min(20, len - 12);
                if (copyLen > 0)
                    std::memcpy(p.model, data + 12, copyLen);
                p.model[20] = '\0';
                // Replace any byte > 127 with '?' to avoid jassert in String(const char*)
                for (int c = 0; c < 20 && p.model[c] != '\0'; ++c)
                    if (static_cast<unsigned char>(p.model[c]) > 127)
                        p.model[c] = '?';
            }
            // Store IP string
            auto ipStr = sender.toStdString();
            std::strncpy(p.ipStr, ipStr.c_str(), 15);
            p.ipStr[15] = '\0';
            // MAC (only in type_status)
            if (type == ProDJLink::kKeepAliveTypeStatus && len >= 44)
                std::memcpy(p.macAddr, data + 38, 6);

            p.discovered.store(true, std::memory_order_release);
            DBG("ProDJLink: Discovered Player " << (int)pn << " (" << p.model << ") at " << sender);
        }

        p.lastPacketTime.store(juce::Time::getMillisecondCounterHiRes(),
                               std::memory_order_relaxed);
    }

    void handleBeatPacket(const uint8_t* data, int len)
    {
        // Beat packet minimum: 36 bytes header + content
        if (len < 36) return;
        if (std::memcmp(data, ProDJLink::kMagic, ProDJLink::kMagicLen) != 0) return;

        uint8_t type = data[10];

        // Mixer fader status (0x39) -- sent by DJM unicast when Pioneer bridge is present.
        // Not player-addressed; route directly to the mixer handler.
        if (type == ProDJLink::kStatusTypeMixer)
        {
            handleMixerPacket(data, len);
            return;
        }

        // DJM channel on-air status (0x29) -- sent by DJM unicast after the extended
        // 95-byte bridge keepalive with Pioneer identification fields is accepted.
        // Not player-addressed; route directly to the DJM status handler.
        if (type == ProDJLink::kStatusTypeDJM)
        {
            handleDJMStatusPacket(data, len);
            return;
        }

        // DJM on-air broadcast (0x03) -- DJM broadcasts this on port 50001 always.
        // Contains per-channel on-air flags. Used as fallback when 0x29 is unavailable.
        if (type == ProDJLink::kBeatTypeMixer)
        {
            handleOnAirBroadcast(data, len);
            return;
        }

        // DJM VU meter data (0x58) -- 524B unicast, contains 6 blocks of 15 u16
        // peak-level segments for CH1-4 (mono) and Master L/R (stereo).
        if (type == ProDJLink::kStatusTypeVU)
        {
            handleVuMeterPacket(data, len);
            return;
        }

        uint8_t pn   = data[33];  // player_number

        if (pn == 0 || pn > ProDJLink::kMaxPlayers) return;
        int idx = pn - 1;
        auto& p = players[idx];

        if (type == ProDJLink::kBeatTypeAbsPosition && len >= 60)
        {
            // Absolute Position packet (CDJ-3000)
            // Content at offset 36:
            //   [36-39] track_len   (uint32be, seconds)
            //   [40-43] playhead    (uint32be, milliseconds)
            //   [44-47] pitch       (int32be, signed, /100 = percentage)
            //   [48-55] padding
            //   [56-59] bpm         (uint32be, BPM x 10)

            uint32_t trackLen = ProDJLink::readU32BE(data + 36);
            uint32_t playhead = ProDJLink::readU32BE(data + 40);
            int32_t  pitchPct = (int32_t)ProDJLink::readU32BE(data + 44); // signed! /100 = %
            uint32_t bpm      = ProDJLink::readU32BE(data + 56);

            p.trackLenSec.store(trackLen, std::memory_order_relaxed);
            p.bpmRaw.store(bpm, std::memory_order_relaxed);
            // NOTE: The pitch field in abs position packets (0x0b) reports the
            // fader SETTING, which is always 0 on CDJ-3000 regardless of actual
            // fader position. The real fader pitch comes from status packets (0x0a)
            // at offset 140/152. Do NOT overwrite pitchRaw here -- it would clobber
            // the correct value from status packets (which arrive 6x less often).
            if (pitchPct != 0)
            {
                double multiplier = 1.0 + double(pitchPct) / 10000.0;
                uint32_t statusPitch = uint32_t(multiplier * double(0x100000));
                p.pitchRaw.store(statusPitch, std::memory_order_relaxed);
            }
            p.hasAbsolutePosition.store(true, std::memory_order_relaxed);

            double absNow = juce::Time::getMillisecondCounterHiRes();
            p.absPositionTs.store(absNow, std::memory_order_relaxed);
            p.lastPacketTime.store(absNow, std::memory_order_relaxed);
            p.playheadMs.store(playhead, std::memory_order_relaxed);

            pktCountAbsPos.fetch_add(1, std::memory_order_relaxed);
        }
        else if (type == ProDJLink::kBeatTypeBeat && len >= 96)
        {
            // Standard beat packet
            // Content at offset 36:
            //   [36-59] distances (6 x uint32be = 24 bytes)
            //   [60-83] padding (24 bytes)
            //   [84-87] pitch   (uint32be, x 0x100000)
            //   [88-89] padding
            //   [90-91] bpm     (uint16be, x 100)
            //   [92]    beat    (1-4)

            uint32_t pitch = ProDJLink::readU32BE(data + 84);
            uint16_t bpm   = ProDJLink::readU16BE(data + 90);
            uint8_t  beat  = data[92];

            // Only update from beat packets if we don't have abs position
            if (!p.hasAbsolutePosition.load(std::memory_order_relaxed))
            {
                p.pitchRaw.store(pitch, std::memory_order_relaxed);
                p.bpmRaw.store(bpm, std::memory_order_relaxed);
            }
            p.beatInBar.store(beat, std::memory_order_relaxed);
            p.lastPacketTime.store(juce::Time::getMillisecondCounterHiRes(),
                                   std::memory_order_relaxed);

            pktCountBeat.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void handleStatusPacket(const uint8_t* data, int len)
    {
        // DJM packets can also arrive on port 50002 depending on firmware/network.
        // Route them to the appropriate handler before the CDJ length check.
        if (len >= 36 && std::memcmp(data, ProDJLink::kMagic, ProDJLink::kMagicLen) == 0)
        {
            uint8_t earlyType = data[10];
            if (earlyType == ProDJLink::kStatusTypeMixer)  // 0x39
            {
                handleMixerPacket(data, len);
                return;
            }
            if (earlyType == ProDJLink::kStatusTypeDJM)    // 0x29
            {
                handleDJMStatusPacket(data, len);
                return;
            }
        }

        // CDJ status packet: minimum ~205 bytes for nexus, 512 for CDJ-3000
        if (len < 180) return;
        if (std::memcmp(data, ProDJLink::kMagic, ProDJLink::kMagicLen) != 0) return;

        uint8_t type = data[10];
        if (type != ProDJLink::kStatusTypeCDJ) return;  // only CDJ status for now

        uint8_t pn = data[33];  // player_number
        if (pn == 0 || pn > ProDJLink::kMaxPlayers) return;

        int idx = pn - 1;
        auto& p = players[idx];

        pktCountStatus.fetch_add(1, std::memory_order_relaxed);

        // --- Parse key fields ---
        // Byte offsets determined from python-prodj-link Construct struct:
        //   [38-39] activity
        //   [40]    loaded_player_number
        //   [41]    loaded_slot
        //   [44-47] track_id  (uint32be)
        //   [120-123] play_state (uint32be)
        //   [136-137] state flags (uint16be: bit3=on_air, bit5=master, bit6=play)
        //   [146-147] bpm (uint16be / 100)
        //   [152-155] actual_pitch (uint32be / 0x100000)
        //   [160-163] beat_count (uint32be)
        //   [166]     beat (1-4)

        // Track ID
        uint32_t trackId = ProDJLink::readU32BE(data + 44);
        uint32_t prevTrackId = p.trackId.load(std::memory_order_relaxed);
        p.trackId.store(trackId, std::memory_order_relaxed);

        if (trackId != prevTrackId && trackId != 0)
        {
            // Track changed -- bump version for TrackMap detection
            p.trackVersion.fetch_add(1, std::memory_order_relaxed);
            // Reset beat-derived position since beatCount restarts on new track
            p.hasBeatDerivedPosition.store(false, std::memory_order_relaxed);
        }

        // Loaded info
        p.loadedPlayer.store(data[40], std::memory_order_relaxed);
        p.loadedSlot.store(data[41], std::memory_order_relaxed);

        // Play state
        if (len > 123)
        {
            uint32_t ps = ProDJLink::readU32BE(data + 120);
            p.playState.store(ps, std::memory_order_relaxed);
        }

        // State flags
        if (len > 137)
        {
            uint16_t flags = ProDJLink::readU16BE(data + 136);
            p.isOnAir.store((flags & 0x08) != 0, std::memory_order_relaxed);
            p.isMaster.store((flags & 0x20) != 0, std::memory_order_relaxed);
            p.isPlaying.store((flags & 0x40) != 0, std::memory_order_relaxed);
        }

        // BPM (only use if no abs position, to avoid source confusion)
        if (len > 147 && !p.hasAbsolutePosition.load(std::memory_order_relaxed))
        {
            uint16_t bpm = ProDJLink::readU16BE(data + 146);
            if (bpm != 0xFFFF)
                p.bpmRaw.store(bpm, std::memory_order_relaxed);
        }

        // Pitch and actual playback speed from status packets.
        // Offset 140: fader pitch (0x100000 = 0% = 1.0x multiplier).
        //   This is the DJ's physical fader setting. Does NOT include
        //   motor ramp -- jumps to target instantly.
        // Offset 152: actual playback speed (0 = stopped, 0x100000 = 1.0x).
        //   Includes motor ramp -- ramps 0->target on play, target->0 on pause.
        //   This is what the CDJ is ACTUALLY doing right now.
        if (len > 143)
        {
            uint32_t fader = ProDJLink::readU32BE(data + 140);
            if (fader != 0)
                p.pitchRaw.store(fader, std::memory_order_relaxed);
        }
        if (len > 155)
        {
            uint32_t speed = ProDJLink::readU32BE(data + 152);
            p.actualSpeedRaw.store(speed, std::memory_order_relaxed);
        }

        // Beat count and beat
        if (len > 166)
        {
            uint32_t bc = ProDJLink::readU32BE(data + 160);
            p.beatCount.store(bc, std::memory_order_relaxed);
            p.beatInBar.store(data[166], std::memory_order_relaxed);

            // --- Beat-derived playhead for non-CDJ-3000 models ---
            // NXS2 and older players don't send Absolute Position packets (0x0b).
            // Derive playhead from: beatCount x (60000 / BPM).
            // Updated at ~5Hz (status packet rate). PLL smooths between updates
            // using actualSpeed (offset 152) or dp/dt fallback.
            if (!p.hasAbsolutePosition.load(std::memory_order_relaxed) && bc > 0)
            {
                uint16_t bpm = ProDJLink::readU16BE(data + 146);  // BPM x 100
                if (bpm > 0 && bpm != 0xFFFF)
                {
                    double bpmReal = double(bpm) / 100.0;
                    double msPerBeat = 60000.0 / bpmReal;
                    uint32_t derivedMs = uint32_t(double(bc) * msPerBeat);

                    p.playheadMs.store(derivedMs, std::memory_order_relaxed);
                    p.absPositionTs.store(juce::Time::getMillisecondCounterHiRes(),
                                          std::memory_order_relaxed);
                    p.hasBeatDerivedPosition.store(true, std::memory_order_relaxed);
                }
            }
        }

        p.lastPacketTime.store(juce::Time::getMillisecondCounterHiRes(),
                               std::memory_order_relaxed);
    }

    //==========================================================================
    // Garbage collection -- drop stale players and DJMs
    //==========================================================================
    void gcPlayers(double now)
    {
        // CDJ players: drop after 10s without any packet
        for (int i = 0; i < ProDJLink::kMaxPlayers; ++i)
        {
            if (players[i].discovered.load(std::memory_order_relaxed))
            {
                double last = players[i].lastPacketTime.load(std::memory_order_relaxed);
                if ((now - last) > 10000.0)
                {
                    DBG("ProDJLink: Player " << (i + 1) << " timed out");
                    players[i].reset();
                }
            }
        }

        // DJM mixers: drop after 10s without a keepalive.
        // This ensures that when a DJM goes offline:
        //   - hasMixerFaderData() goes stale (5s timeout on lastMixerPacketTime)
        //   - DJM IP is removed from subscribe list (this GC, 10s)
        // When the DJM comes back:
        //   - Its keepalive triggers registerDJM() → re-adds IP + immediate subscribe
        //   - Our 54B broadcast keepalive is still going → DJM rediscovers us
        //   - Fader delivery reactivates automatically
        {
            const juce::ScopedLock sl(djmIpLock);
            for (int i = (int)djmIps.size() - 1; i >= 0; --i)
            {
                if ((now - djmLastSeen[i]) > 10000.0)
                {
                    DBG("ProDJLink: DJM [" << juce::String(djmModels[i]) << "] at "
                        << juce::String(djmIps[i]) << " timed out");
                    djmIps.erase(djmIps.begin() + i);
                    djmModels.erase(djmModels.begin() + i);
                    djmLastSeen.erase(djmLastSeen.begin() + i);
                }
            }
        }
    }

    //==========================================================================
    // IP/MAC helpers
    //==========================================================================
    static void parseIpString(const juce::String& ip, uint8_t out[4])
    {
        auto tokens = juce::StringArray::fromTokens(ip, ".", "");
        for (int i = 0; i < 4 && i < tokens.size(); ++i)
            out[i] = uint8_t(tokens[i].getIntValue());
    }

    static bool getMacAddress(const juce::String& ifaceName, uint8_t out[6])
    {
        std::memset(out, 0, 6);

#ifdef _WIN32
        ULONG bufSize = 15000;
        std::vector<uint8_t> buffer(bufSize);
        auto* addresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
        if (GetAdaptersAddresses(AF_INET, 0, nullptr, addresses, &bufSize) != NO_ERROR)
            return false;
        for (auto* adapter = addresses; adapter; adapter = adapter->Next)
        {
            // Match by adapter friendly name or description
            juce::String friendlyName(adapter->FriendlyName);
            juce::String description(adapter->Description);
            if (friendlyName.containsIgnoreCase(ifaceName)
                || description.containsIgnoreCase(ifaceName)
                || ifaceName.containsIgnoreCase(friendlyName))
            {
                if (adapter->PhysicalAddressLength >= 6)
                {
                    std::memcpy(out, adapter->PhysicalAddress, 6);
                    return true;
                }
            }
        }
        return false;
#else
        // macOS: use getifaddrs + AF_LINK to find link-layer address
        // Linux: use SIOCGIFHWADDR ioctl

  #if defined(__APPLE__)
        struct ifaddrs* ifList = nullptr;
        if (getifaddrs(&ifList) != 0) return false;

        for (auto* ifa = ifList; ifa != nullptr; ifa = ifa->ifa_next)
        {
            if (ifa->ifa_addr == nullptr) continue;
            if (ifa->ifa_addr->sa_family != AF_LINK) continue;

            juce::String name(ifa->ifa_name);
            // Match interface name (e.g. "en0") against the NetworkUtils name
            if (!name.containsIgnoreCase(ifaceName) && !ifaceName.containsIgnoreCase(name))
                continue;

            auto* sdl = reinterpret_cast<struct sockaddr_dl*>(ifa->ifa_addr);
            if (sdl->sdl_type == IFT_ETHER && sdl->sdl_alen >= 6)
            {
                std::memcpy(out, LLADDR(sdl), 6);
                freeifaddrs(ifList);
                return true;
            }
        }
        freeifaddrs(ifList);
        return false;

  #elif defined(__linux__)
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) return false;

        struct ifreq ifr;
        std::memset(&ifr, 0, sizeof(ifr));
        std::strncpy(ifr.ifr_name, ifaceName.toRawUTF8(), IFNAMSIZ - 1);

        bool ok = (ioctl(sock, SIOCGIFHWADDR, &ifr) == 0);
        close(sock);

        if (ok)
        {
            std::memcpy(out, ifr.ifr_hwaddr.sa_data, 6);
            return true;
        }
        return false;

  #else
        (void)ifaceName;
        return false;  // unsupported platform
  #endif
#endif
    }

    //==========================================================================
    // Member data
    //==========================================================================

    //==========================================================================
    // DJM Mixer status handler (type 0x39, 248 bytes)
    //
    // COMPLETE OFFSET MAP — confirmed from DJM-900NXS2 packet captures.
    //
    // Per-channel block: 24 bytes, base=[0x024,0x03c,0x054,0x06c]
    //   +0   Input source  (0=USB_A,1=USB_B,2=DIGITAL,3=LINE,4=PHONO,8=RET/AUX)
    //   +1   Trim          (0-255, 128=unity)
    //   +3   EQ High       (0-255, 128=center)
    //   +4   EQ Mid        (0-255, 128=center)
    //   +6   EQ Low        (0-255, 128=center)
    //   +7   Color/FX      (0-255, 128=center)
    //   +9   CUE button    (0=off, 1=on)
    //   +11  Channel fader (0=closed, 255=open)
    //   +12  XF Assign     (0=THRU, 1=A, 2=B)
    //
    // Global / Master:
    //   [0x0b4]  Crossfader       (0=side-A, 255=side-B)
    //   [0x0b5]  Fader curve      (0/1/2)
    //   [0x0b6]  Crossfader curve (0/1/2)
    //   [0x0b7]  Master fader     (0-255)
    //   [0x0b9]  Master CUE btn   (0/1)
    //   [0x0bf]  Booth monitor    (0-255)
    //
    // Headphones:
    //   [0x0c4]  HP Cue Link      (0/1)
    //   [0x0e3]  HP Mixing        (0=CUE, 255=Master)
    //   [0x0e4]  HP Level         (0-255)
    //
    // Beat FX:
    //   [0x0c6]  FX Freq Low      (0/1)
    //   [0x0c7]  FX Freq Mid      (0/1)
    //   [0x0c8]  FX Freq Hi       (0/1)
    //   [0x0c9]  Beat FX Select   (0-13: Delay,Echo,PingPong,Spiral,Reverb,
    //                               Trans,Filter,Flanger,Phaser,Pitch,
    //                               SlipRoll,Roll,VinylBrake,Helix)
    //   [0x0ca]  Color FX Assign  (0=Mic,1=CH1,2=CH2,3=CH3,7=CH4,
    //                               6=XF-A,8=XF-B,9=Master)
    //   [0x0cb]  Beat FX Level    (0-255)
    //   [0x0cc]  Beat FX ON/OFF   (0/1)
    //   [0x0ce]  Beat FX Assign   (same enum as 0x0ca)
    //   [0x0cf]  Send/Return Lvl  (0-255)
    //
    // Color FX:
    //   [0x0db]  Color FX Select  (255=OFF, 0=Space,1=DubEcho,2=Sweep,
    //                               3=Noise,4=Crush,5=Filter)
    //   [0x0e2]  Color FX Param   (0-255)
    //
    // Mic:
    //   [0x0d6]  Mic EQ Hi        (0-255, 128=center)
    //   [0x0d7]  Mic EQ Lo        (0-255, 128=center)
    //==========================================================================
    void handleMixerPacket(const uint8_t* data, int len)
    {
        if (len < 0xe5) return;  // need up to HP level at 0x0e4

        // --- Per-channel block (24 bytes each) ---
        static constexpr int chBase[4] = { 0x024, 0x03c, 0x054, 0x06c };
        for (int ch = 0; ch < 4; ++ch)
        {
            int b = chBase[ch];
            mixerInputSrc[ch].store (data[b + 0],  std::memory_order_relaxed);
            mixerTrim[ch].store     (data[b + 1],  std::memory_order_relaxed);
            mixerEqHi[ch].store     (data[b + 3],  std::memory_order_relaxed);
            mixerEqMid[ch].store    (data[b + 4],  std::memory_order_relaxed);
            mixerEqLo[ch].store     (data[b + 6],  std::memory_order_relaxed);
            mixerColor[ch].store    (data[b + 7],  std::memory_order_relaxed);
            mixerCueBtn[ch].store   (data[b + 9],  std::memory_order_relaxed);
            mixerFader[ch].store    (data[b + 11], std::memory_order_relaxed);
            mixerXfAssign[ch].store (data[b + 12], std::memory_order_relaxed);
        }

        // --- Global / Master ---
        mixerCrossfader.store   (data[0x0b4], std::memory_order_relaxed);
        mixerFaderCurve.store   (data[0x0b5], std::memory_order_relaxed);
        mixerXfCurve.store      (data[0x0b6], std::memory_order_relaxed);
        mixerMasterFader.store  (data[0x0b7], std::memory_order_relaxed);
        mixerMasterCue.store    (data[0x0b9], std::memory_order_relaxed);
        mixerBooth.store        (data[0x0bf], std::memory_order_relaxed);

        // --- Headphones ---
        mixerHpCueLink.store    (data[0x0c4], std::memory_order_relaxed);
        mixerHpMixing.store     (data[0x0e3], std::memory_order_relaxed);
        mixerHpLevel.store      (data[0x0e4], std::memory_order_relaxed);

        // --- Beat FX ---
        mixerFxFreqLo.store     (data[0x0c6], std::memory_order_relaxed);
        mixerFxFreqMid.store    (data[0x0c7], std::memory_order_relaxed);
        mixerFxFreqHi.store     (data[0x0c8], std::memory_order_relaxed);
        mixerBeatFxSel.store    (data[0x0c9], std::memory_order_relaxed);
        mixerColorFxAssign.store(data[0x0ca], std::memory_order_relaxed);
        mixerBeatFxLevel.store  (data[0x0cb], std::memory_order_relaxed);
        mixerBeatFxOn.store     (data[0x0cc], std::memory_order_relaxed);
        mixerBeatFxAssign.store (data[0x0ce], std::memory_order_relaxed);
        mixerSendReturn.store   (data[0x0cf], std::memory_order_relaxed);

        // --- Color FX ---
        mixerColorFxSel.store   (data[0x0db], std::memory_order_relaxed);
        mixerColorFxParam.store (data[0x0e2], std::memory_order_relaxed);

        // --- Mic ---
        mixerMicEqHi.store      (data[0x0d6], std::memory_order_relaxed);
        mixerMicEqLo.store      (data[0x0d7], std::memory_order_relaxed);

        hasMixerData.store(true, std::memory_order_relaxed);
        lastMixerPacketTime.store(juce::Time::getMillisecondCounterHiRes(), std::memory_order_relaxed);
        pktCountMixer.fetch_add(1, std::memory_order_relaxed);
    }

    //==========================================================================
    // DJM VU meter handler (type 0x58, 524 bytes, port 50001)
    //
    // Contains 6 blocks of 15 u16 big-endian peak-level segments.
    // Each block represents one VU meter strip on the DJM-900NXS2.
    // Range: 0 = silence, 32767 = clip (+15 dB).
    //
    //   Block A [0x02c-0x049]: CH1 (mono)
    //   Block B [0x068-0x085]: CH2 (mono)
    //   Block C [0x0a4-0x0c1]: CH3 (mono)
    //   Block D [0x0e0-0x0fd]: CH4 (mono)
    //   Block E [0x11c-0x139]: Master L (stereo)
    //   Block F [0x158-0x175]: Master R (stereo)
    //
    // Confirmed from DJM-900NXS2 capture: Vumetros.pcapng
    //==========================================================================
    void handleVuMeterPacket(const uint8_t* data, int len)
    {
        if (len < 0x176) return;  // need up to Master R last segment

        static constexpr int kVuBlockOffsets[6] = {
            0x02c, 0x068, 0x0a4, 0x0e0, 0x11c, 0x158
        };

        for (int ch = 0; ch < 6; ++ch)
        {
            int base = kVuBlockOffsets[ch];
            uint16_t peak = 0;
            for (int seg = 0; seg < 15; ++seg)
            {
                int off = base + seg * 2;
                uint16_t val = ProDJLink::readU16BE(data + off);
                if (val > peak) peak = val;
            }
            vuPeak[ch].store(peak, std::memory_order_relaxed);
        }

        // Copy raw segment data under lock for UI spectrum display
        {
            const juce::SpinLock::ScopedLockType sl(vuDataLock);
            for (int ch = 0; ch < 6; ++ch)
            {
                int base = kVuBlockOffsets[ch];
                for (int seg = 0; seg < 15; ++seg)
                    vuSegments[ch][seg] = ProDJLink::readU16BE(data + base + seg * 2);
            }
        }

        hasVuData.store(true, std::memory_order_relaxed);
        lastVuPacketTime.store(juce::Time::getMillisecondCounterHiRes(), std::memory_order_relaxed);
        pktCountVU.fetch_add(1, std::memory_order_relaxed);
    }

    //==========================================================================
    // DJM channel on-air status handler (type 0x29)
    //
    // Delivered unicast by the DJM once the extended 95-byte bridge keepalive
    // (with "PIONEER DJ CORP" / "PRODJLINK BRIDGE" identification fields) has
    // been accepted. Contains per-channel on-air flags indicating which channels
    // are currently faded up.
    //
    // Offset map (to be confirmed from live captures once 0x29 delivery is active;
    // offsets below are derived from protocol analysis):
    //   [0x24]  Channel count (uint8, typically 4)
    //   [0x27]  Channel 1 on-air flag (0x00=off-air, 0xff=on-air)
    //   [0x2b]  Channel 2 on-air flag
    //   [0x2f]  Channel 3 on-air flag
    //   [0x33]  Channel 4 on-air flag
    //
    // Note: CDJ status packets (port 50002, offset 136, bit 3) carry the same
    // per-player on-air flag from the player's perspective. The 0x29 packet
    // mirrors this from the mixer side and also covers non-CDJ inputs.
    // When both sources are available the 0x29 value takes precedence here
    // because the mixer has authoritative knowledge of fader position.
    //==========================================================================
    void handleDJMStatusPacket(const uint8_t* data, int len)
    {
        // Need at least channel 4 flag at offset 0x33 + 1 = 52 bytes
        if (len < 0x34) return;

        // Extract per-channel on-air flags (0xff=on-air, 0x00=off-air)
        bool ch1OnAir = (data[0x27] != 0x00);
        bool ch2OnAir = (data[0x2b] != 0x00);
        bool ch3OnAir = (data[0x2f] != 0x00);
        bool ch4OnAir = (data[0x33] != 0x00);

        // Propagate on-air state to matching player slots.
        // Channel number == player number for standard 4-deck setups.
        for (int i = 0; i < ProDJLink::kMaxPlayers; ++i)
        {
            if (!players[i].discovered.load(std::memory_order_relaxed)) continue;
            uint8_t pn = players[i].playerNumber.load(std::memory_order_relaxed);
            bool onAir = false;
            switch (pn)
            {
                case 1: onAir = ch1OnAir; break;
                case 2: onAir = ch2OnAir; break;
                case 3: onAir = ch3OnAir; break;
                case 4: onAir = ch4OnAir; break;
                default: continue;  // players 5-6 have no DJM channel mapping
            }
            players[i].isOnAir.store(onAir, std::memory_order_relaxed);
        }

        pktCountDJMStatus.fetch_add(1, std::memory_order_relaxed);

        DBG("ProDJLink: 0x29 on-air ch1=" << (int)ch1OnAir << " ch2=" << (int)ch2OnAir
            << " ch3=" << (int)ch3OnAir << " ch4=" << (int)ch4OnAir);
    }

    // Player state array -- indexed 0-5 for players 1-6
    std::array<ProDJLinkPlayerState, ProDJLink::kMaxPlayers> players;

    // Sockets
    std::unique_ptr<juce::DatagramSocket> keepaliveSock;
    std::unique_ptr<juce::DatagramSocket> beatSock;
    std::unique_ptr<juce::DatagramSocket> statusSock;

    // Network config
    juce::Array<NetworkInterface> availableInterfaces;
    int         selectedInterface = 0;
    juce::String bindIp;
    juce::String broadcastIp;
    juce::String subnetMask;
    uint8_t     ownIpBytes[4]  {};
    uint8_t     ownMacBytes[6] {};

    // Bridge config
    int vCDJPlayerNumber = ProDJLink::kDefaultVCDJNumber;  // always 5 for bridge mode

    // Known DJM mixers for bridge subscription (protected by djmIpLock)
    juce::CriticalSection         djmIpLock;
    std::vector<std::string>      djmIps;
    std::vector<std::string>      djmModels;    // model name per DJM, parallel to djmIps
    std::vector<double>           djmLastSeen;  // millisecond timestamp, parallel to djmIps

    // Selected player for timecode output (1-based)
    std::atomic<int> selectedPlayer { 1 };

    // Output frame rate (user-configured, not auto-detected)
    std::atomic<FrameRate> outputFps { FrameRate::FPS_25 };

    // Run flag
    std::atomic<bool> isRunningFlag { false };
    std::atomic<bool> joinCompleted { false };

    // Diagnostics
    std::atomic<uint32_t> pktCountKeepalive  { 0 };
    std::atomic<uint32_t> pktCountBeat       { 0 };
    std::atomic<uint32_t> pktCountAbsPos     { 0 };
    std::atomic<uint32_t> pktCountStatus     { 0 };
    std::atomic<uint32_t> pktCountMixer      { 0 };
    std::atomic<uint32_t> pktCountDJMStatus  { 0 };  // type 0x29 packets received

    // DJM mixer state (from type 0x39 packets, 248 bytes)
    // --- Per-channel arrays (indexed 0-3 for CH1-CH4) ---
    std::atomic<uint8_t> mixerFader[4]     {{ 255 }, { 255 }, { 255 }, { 255 }};
    std::atomic<uint8_t> mixerTrim[4]      {{ 128 }, { 128 }, { 128 }, { 128 }};
    std::atomic<uint8_t> mixerEqHi[4]      {{ 128 }, { 128 }, { 128 }, { 128 }};
    std::atomic<uint8_t> mixerEqMid[4]     {{ 128 }, { 128 }, { 128 }, { 128 }};
    std::atomic<uint8_t> mixerEqLo[4]      {{ 128 }, { 128 }, { 128 }, { 128 }};
    std::atomic<uint8_t> mixerColor[4]     {{ 128 }, { 128 }, { 128 }, { 128 }};
    std::atomic<uint8_t> mixerCueBtn[4]    {{ 0 }, { 0 }, { 0 }, { 0 }};
    std::atomic<uint8_t> mixerInputSrc[4]  {{ 3 }, { 3 }, { 3 }, { 3 }};       // 0=USB_A,1=USB_B,2=DIG,3=LINE,4=PHONO,8=RET
    std::atomic<uint8_t> mixerXfAssign[4]  {{ 0 }, { 0 }, { 0 }, { 0 }};       // 0=THRU,1=A,2=B
    // --- Global / Master ---
    std::atomic<uint8_t> mixerCrossfader   { 128 };   // 0=A, 255=B
    std::atomic<uint8_t> mixerMasterFader  { 255 };
    std::atomic<uint8_t> mixerMasterCue    { 0 };
    std::atomic<uint8_t> mixerFaderCurve   { 1 };     // 0/1/2
    std::atomic<uint8_t> mixerXfCurve      { 1 };     // 0/1/2
    std::atomic<uint8_t> mixerBooth        { 0 };     // 0-255
    // --- Headphones ---
    std::atomic<uint8_t> mixerHpCueLink    { 0 };
    std::atomic<uint8_t> mixerHpMixing     { 0 };     // 0=CUE, 255=Master
    std::atomic<uint8_t> mixerHpLevel      { 0 };
    // --- Beat FX ---
    std::atomic<uint8_t> mixerFxFreqLo     { 1 };
    std::atomic<uint8_t> mixerFxFreqMid    { 1 };
    std::atomic<uint8_t> mixerFxFreqHi     { 1 };
    std::atomic<uint8_t> mixerBeatFxSel    { 0 };     // 0-13
    std::atomic<uint8_t> mixerBeatFxLevel  { 0 };
    std::atomic<uint8_t> mixerBeatFxOn     { 0 };
    std::atomic<uint8_t> mixerBeatFxAssign { 9 };     // 0=Mic..9=Master
    std::atomic<uint8_t> mixerColorFxAssign{ 9 };
    std::atomic<uint8_t> mixerSendReturn   { 0 };
    // --- Color FX ---
    std::atomic<uint8_t> mixerColorFxSel   { 255 };   // 255=OFF, 0-5
    std::atomic<uint8_t> mixerColorFxParam { 128 };
    // --- Mic ---
    std::atomic<uint8_t> mixerMicEqHi      { 128 };
    std::atomic<uint8_t> mixerMicEqLo      { 128 };
    // --- Flag ---
    std::atomic<bool>    hasMixerData      { false };
    std::atomic<double>  lastMixerPacketTime { 0.0 };  // for staleness detection

    // VU meter data (from type 0x58 packets, 524 bytes)
    // 6 channels: 0=CH1, 1=CH2, 2=CH3, 3=CH4, 4=MasterL, 5=MasterR
    std::atomic<uint16_t> vuPeak[6]       {{ 0 }, { 0 }, { 0 }, { 0 }, { 0 }, { 0 }};
    mutable juce::SpinLock vuDataLock;
    uint16_t              vuSegments[6][15] {};  // protected by vuDataLock
    std::atomic<bool>     hasVuData        { false };
    std::atomic<double>   lastVuPacketTime { 0.0 };   // for staleness detection
    std::atomic<uint32_t> pktCountVU       { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ProDJLinkInput)
};
