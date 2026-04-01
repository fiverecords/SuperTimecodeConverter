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
//   - Bridge join sequence: hello 0x0A (player=5) -> IP claim 0x02 (player=0xC0)
//   - Two keepalives sent in parallel:
//     1) 54B BROADCAST (player=0xC1) -- DJM discovers bridge, activates fader delivery
//     2) 95B UNICAST to each CDJ (player=5, PIONEER DJ CORP strings) -- CDJ registers
//        us as a valid peer and sends AbsPos/Status unicast data
//   - The DJM NEVER receives the 95B (it's unicast to CDJ IPs only)
//     -> DJM sees single identity (0xC1) -> faders work
//   - The CDJ sees both, but both are from the same bridge -> no conflict
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
// Protocol analysis: DJ Link Ecosystem Analysis
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
  #include <sys/socket.h>      // SO_BROADCAST setsockopt
#elif defined(__linux__)
  #include <sys/ioctl.h>
  #include <net/if.h>
  #include <unistd.h>
  #include <sys/socket.h>    // SO_BROADCAST setsockopt
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

    // Maximum mixer channels (DJM-V10 has 6, DJM-900NXS2 has 4)
    static constexpr int kMaxMixerChannels = 6;
    // VU meter indices: channels first, then master L/R
    static constexpr int kVuMasterL = kMaxMixerChannels;      // index 6
    static constexpr int kVuMasterR = kMaxMixerChannels + 1;  // index 7
    static constexpr int kVuSlots   = kMaxMixerChannels + 2;  // 8 total

    // Device type bytes (byte [33] in keepalive packets) -- confirmed from captures:
    //   0x01 = bridge / lighting controller
    //   0x02 = DJM mixer  (player_number >= 0x21)
    //   0x03 = CDJ / XDJ player (player_number 1-6)
    static constexpr uint8_t kDeviceTypeBridge  = 0x01;
    static constexpr uint8_t kDeviceTypeMixer   = 0x02;  // DJM: device_type=0x02, pn >= 0x21
    static constexpr uint8_t kMixerPlayerNumMin = 0x21;  // DJM player numbers start at 33

    // Virtual CDJ defaults
    // Player 5 is used as bridge slot -- does NOT occupy CDJ slots 1-4.
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

    // CDJ-3000 extended fields (from 0x200-byte status packets)
    // Loop start/end in ms.  Both non-zero when a loop is active (stored
    // from rekordbox OR dynamically set by the DJ during live performance).
    // Raw value from bytes 0x1B6-0x1B9 / 0x1BE-0x1C1 is encoded as
    // position * 1000 / 65536, so: ms = raw * 65536 / 1000.
    std::atomic<uint32_t> loopStartMs  { 0 };
    std::atomic<uint32_t> loopEndMs    { 0 };

    // Reverse play detection (CDJ-3000 abspos: position decreasing while playing)
    std::atomic<bool>     isReverse    { false };
    uint32_t              prevAbsPosMs { 0 };     // previous abspos (non-atomic, network thread only)

    // Track change detection
    std::atomic<uint32_t> trackVersion { 0 };    // incremented on track change

    // Timing
    std::atomic<double>   lastPacketTime { 0.0 };  // juce::Time::getMillisecondCounterHiRes()
    std::atomic<double>   absPositionTs  { 0.0 };  // timestamp of last abs position (for interpolation)

    void reset()
    {
        // Store discovered=false FIRST with release ordering.
        // UI-thread getters check discovered with acquire ordering --
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
        loopStartMs.store(0, std::memory_order_relaxed);
        loopEndMs.store(0, std::memory_order_relaxed);
        isReverse.store(false, std::memory_order_relaxed);
        prevAbsPosMs = 0;
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
        if (!getMacAddress(iface.name, ownMacBytes, iface.ip))
        {
            // Fallback: use a plausible MAC
            std::memset(ownMacBytes, 0, 6);
            ownMacBytes[0] = 0x02;  // locally administered
            ownMacBytes[5] = uint8_t(vCDJPlayerNumber);
        }

        // --- Create sockets ---
        // IMPORTANT: beatSock and statusSock must NOT use setEnablePortReuse().
        // On macOS, JUCE's setEnablePortReuse(true) enables SO_REUSEPORT, which
        // tells the kernel to distribute incoming packets among ALL sockets bound
        // to the same port. If a previous STC instance didn't close cleanly, or
        // another Pro DJ Link app (Bridge, rekordbox) has a socket on port 50001,
        // the kernel splits packets between them -- causing packet reordering and
        // stale playhead values that appear as timecode fluctuation.
        //
        // beatSock, statusSock, and bridgeSock bind to bindIp to ensure packets
        // go out the correct NIC and to receive only from the selected interface.
        //
        // keepaliveSock binding is PLATFORM-SPECIFIC:
        //   Windows: bind to bindIp.  On multi-adapter systems (two NICs on the
        //     same subnet), INADDR_ANY lets the OS routing table pick the outgoing
        //     interface for broadcasts -- which may be the wrong one.  The DJM sees
        //     the keepalive arriving from a different MAC than the payload and
        //     rejects the bridge identity.
        //   macOS: bind to INADDR_ANY.  macOS does NOT deliver broadcast packets
        //     to sockets bound to a specific IP -- only to INADDR_ANY.  Without
        //     broadcast reception on port 50000, no CDJ or DJM keepalives arrive
        //     and device discovery fails completely.
        // keepaliveSock keeps SO_REUSEPORT for coexistence with other Pro DJ Link
        // software on port 50000.

        keepaliveSock = std::make_unique<juce::DatagramSocket>(true);
        keepaliveSock->setEnablePortReuse(true);
#ifdef _WIN32
        // Windows: bind to specific interface to force correct outgoing NIC
        if (!keepaliveSock->bindToPort(ProDJLink::kKeepalivePort, bindIp)
            && !keepaliveSock->bindToPort(ProDJLink::kKeepalivePort))
#else
        // macOS/Linux: INADDR_ANY required for broadcast reception
        if (!keepaliveSock->bindToPort(ProDJLink::kKeepalivePort))
#endif
        {
            DBG("ProDJLink: Failed to bind keepalive socket to port " << ProDJLink::kKeepalivePort);
            keepaliveSock = nullptr;
            return false;
        }
        ensureSoBroadcast(keepaliveSock.get(), "keepalive");

        beatSock = std::make_unique<juce::DatagramSocket>(true);
        // NO setEnablePortReuse -- see comment above
        //
        // PLATFORM-SPECIFIC binding (same reasoning as keepaliveSock):
        //   Windows: bind to bindIp so packets go out the correct NIC.
        //   macOS: bind to INADDR_ANY because macOS does NOT deliver broadcast
        //     packets to sockets bound to a specific IP.  Beat packets (type 0x28)
        //     are broadcast by the CDJ.  AbsPos (0x0b) is unicast and would arrive
        //     on a specific-IP socket, but beat packets silently don't -- causing
        //     beatInBar to never update on macOS while everything else works.
#ifdef _WIN32
        if (!beatSock->bindToPort(ProDJLink::kBeatPort, bindIp)
            && !beatSock->bindToPort(ProDJLink::kBeatPort))  // fallback to INADDR_ANY
#else
        if (!beatSock->bindToPort(ProDJLink::kBeatPort))      // INADDR_ANY for broadcast
#endif
        {
            DBG("ProDJLink: WARNING -- Failed to bind beat socket to port " << ProDJLink::kBeatPort
                << " (beat sync unavailable, but status/keepalive will work)");
            beatSock = nullptr;  // non-fatal: continue without beat data
        }
        if (beatSock) ensureSoBroadcast(beatSock.get(), "beat");

        statusSock = std::make_unique<juce::DatagramSocket>(false);
        // NO setEnablePortReuse -- see comment above
        if (!statusSock->bindToPort(ProDJLink::kStatusPort, bindIp)
            && !statusSock->bindToPort(ProDJLink::kStatusPort))  // fallback to INADDR_ANY
        {
            DBG("ProDJLink: Failed to bind status socket to port " << ProDJLink::kStatusPort);
            keepaliveSock = nullptr;
            beatSock = nullptr;
            statusSock = nullptr;
            return false;
        }

        // Extra send-only socket on ephemeral port for 0x57 subscribe / 0x55 notify.
        // Pioneer Bridge uses a separate port (~50006) for these; some DJM firmware
        // ignores subscribes whose source port matches a well-known ProDJLink port.
        // Non-fatal: if it fails, subscribe/notify still go via beatSock/statusSock.
        bridgeSock = std::make_unique<juce::DatagramSocket>(false);
        if (!bridgeSock->bindToPort(0, bindIp) && !bridgeSock->bindToPort(0))
        {
            DBG("ProDJLink: bridgeSock creation failed (non-fatal)");
            bridgeSock = nullptr;
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
        if (bridgeSock)    bridgeSock->shutdown();

        stopThread(2000);

        keepaliveSock = nullptr;
        beatSock      = nullptr;
        statusSock    = nullptr;
        bridgeSock    = nullptr;

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
    /// never ramps to zero -- unlike pause where actualSpeed decelerates.
    bool isEndOfTrack(int playerNum) const
    {
        int idx = playerNum - 1;
        if (idx < 0 || idx >= ProDJLink::kMaxPlayers) return false;
        return players[idx].playState.load(std::memory_order_relaxed)
            == ProDJLink::kPlayEndTrack;
    }

    /// CDJ-3000: active loop start position in ms (0 = no loop active)
    uint32_t getLoopStartMs(int playerNum) const
    {
        int idx = playerNum - 1;
        if (idx < 0 || idx >= ProDJLink::kMaxPlayers) return 0;
        return players[idx].loopStartMs.load(std::memory_order_relaxed);
    }

    /// CDJ-3000: active loop end position in ms (0 = no loop active)
    uint32_t getLoopEndMs(int playerNum) const
    {
        int idx = playerNum - 1;
        if (idx < 0 || idx >= ProDJLink::kMaxPlayers) return 0;
        return players[idx].loopEndMs.load(std::memory_order_relaxed);
    }

    /// CDJ-3000: true if a loop is currently active (stored or dynamic)
    bool hasActiveLoop(int playerNum) const
    {
        int idx = playerNum - 1;
        if (idx < 0 || idx >= ProDJLink::kMaxPlayers) return false;
        return players[idx].loopStartMs.load(std::memory_order_relaxed) != 0
            && players[idx].loopEndMs.load(std::memory_order_relaxed) != 0;
    }

    /// CDJ-3000: true if playing in reverse (detected from decreasing abspos)
    bool isPlayingReverse(int playerNum) const
    {
        int idx = playerNum - 1;
        if (idx < 0 || idx >= ProDJLink::kMaxPlayers) return false;
        return players[idx].isReverse.load(std::memory_order_relaxed);
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

    /// Monotonic counter of received 0x39 mixer packets. Used by forwarding
    /// logic to skip iteration when no new data has arrived since last check.
    uint32_t getMixerPacketCount() const { return pktCountMixer.load(std::memory_order_relaxed); }

    /// Channel fader position (0=closed/bottom, 255=fully open/top).
    /// Only valid after hasMixerFaderData() returns true.
    uint8_t getChannelFader(int channel) const
    {
        int idx = channel - 1;
        if (idx < 0 || idx >= ProDJLink::kMaxMixerChannels) return 255;
        return mixerFader[idx].load(std::memory_order_relaxed);
    }

    /// Trim/gain knob (0=min, 128=unity, 255=max).
    uint8_t getChannelTrim(int channel) const
    {
        int idx = channel - 1;
        if (idx < 0 || idx >= ProDJLink::kMaxMixerChannels) return 128;
        return mixerTrim[idx].load(std::memory_order_relaxed);
    }

    /// Per-channel Compressor knob (0=off, 255=max).  V10 only; always 0 on 900NXS2.
    /// Confirmed from Comp__V10.pcapng: per-channel offset +2.
    uint8_t getChannelComp(int channel) const
    {
        int idx = channel - 1;
        if (idx < 0 || idx >= ProDJLink::kMaxMixerChannels) return 0;
        return mixerComp[idx].load(std::memory_order_relaxed);
    }

    /// EQ High knob (0=full cut, 128=center/flat, 255=full boost).
    uint8_t getChannelEqHi(int channel) const
    {
        int idx = channel - 1;
        if (idx < 0 || idx >= ProDJLink::kMaxMixerChannels) return 128;
        return mixerEqHi[idx].load(std::memory_order_relaxed);
    }

    /// EQ Mid knob (0=full cut, 128=center/flat, 255=full boost).
    /// On V10 this is "Hi Mid" (4-band EQ).
    uint8_t getChannelEqMid(int channel) const
    {
        int idx = channel - 1;
        if (idx < 0 || idx >= ProDJLink::kMaxMixerChannels) return 128;
        return mixerEqMid[idx].load(std::memory_order_relaxed);
    }

    /// EQ Low Mid knob (0=full cut, 128=center/flat, 255=full boost).
    /// V10 only (4-band EQ); always 0 on 900NXS2.
    uint8_t getChannelEqLoMid(int channel) const
    {
        int idx = channel - 1;
        if (idx < 0 || idx >= ProDJLink::kMaxMixerChannels) return 128;
        return mixerEqLoMid[idx].load(std::memory_order_relaxed);
    }

    /// EQ Low knob (0=full cut, 128=center/flat, 255=full boost).
    uint8_t getChannelEqLo(int channel) const
    {
        int idx = channel - 1;
        if (idx < 0 || idx >= ProDJLink::kMaxMixerChannels) return 128;
        return mixerEqLo[idx].load(std::memory_order_relaxed);
    }

    /// Color/FX knob (0=min, 128=center/off, 255=max).
    uint8_t getChannelColor(int channel) const
    {
        int idx = channel - 1;
        if (idx < 0 || idx >= ProDJLink::kMaxMixerChannels) return 128;
        return mixerColor[idx].load(std::memory_order_relaxed);
    }

    /// Per-channel Send knob (0=off, 255=max).  V10 only; always 0 on 900NXS2.
    /// Confirmed from Send_V10.pcapng: per-channel offset +8.
    uint8_t getChannelSend(int channel) const
    {
        int idx = channel - 1;
        if (idx < 0 || idx >= ProDJLink::kMaxMixerChannels) return 0;
        return mixerSend[idx].load(std::memory_order_relaxed);
    }

    /// CUE headphone button (0=off, 1=on).
    uint8_t getChannelCue(int channel) const
    {
        int idx = channel - 1;
        if (idx < 0 || idx >= ProDJLink::kMaxMixerChannels) return 0;
        return mixerCueBtn[idx].load(std::memory_order_relaxed);
    }

    /// CUE B headphone button (0=off, 1=on).  A9/V10 dual-cue; always 0 on 900NXS2.
    uint8_t getChannelCueB(int channel) const
    {
        int idx = channel - 1;
        if (idx < 0 || idx >= ProDJLink::kMaxMixerChannels) return 0;
        return mixerCueBtnB[idx].load(std::memory_order_relaxed);
    }

    /// Input source selector per channel.
    /// 900NXS2: 0=PC USB A, 1=PC USB B, 2=DIGITAL, 3=LINE, 4=PHONO, 8=RET/AUX
    /// A9: 0=PC USB A, 1=PC USB B, 2=DIGITAL, 3=LINE, 4=PHONO, 7=USB, 8=RETURN, 10=BLUETOOTH
    /// V10: 0=PC USB A, 1=PC USB B, 2=DIGITAL, 3=LINE, 4=PHONO, 5=BUILT-IN,
    ///      6=EXT1, 7=EXT2, 8=MULTI I/O, 9=COMBO
    uint8_t getChannelInputSrc(int channel) const
    {
        int idx = channel - 1;
        if (idx < 0 || idx >= ProDJLink::kMaxMixerChannels) return 3;
        return mixerInputSrc[idx].load(std::memory_order_relaxed);
    }

    /// Crossfader assign (0=THRU, 1=A, 2=B).
    uint8_t getChannelXfAssign(int channel) const
    {
        int idx = channel - 1;
        if (idx < 0 || idx >= ProDJLink::kMaxMixerChannels) return 0;
        return mixerXfAssign[idx].load(std::memory_order_relaxed);
    }

    /// Crossfader position (0=fully side-A, 128=center, 255=fully side-B).
    uint8_t getCrossfader()    const { return mixerCrossfader.load(std::memory_order_relaxed); }
    /// Master fader position (0=min, 255=max).
    uint8_t getMasterFader()   const { return mixerMasterFader.load(std::memory_order_relaxed); }
    /// Master CUE button (0=off, 1=on).
    uint8_t getMasterCue()     const { return mixerMasterCue.load(std::memory_order_relaxed); }
    uint8_t getMasterCueB()    const { return mixerMasterCueB.load(std::memory_order_relaxed); }   // A9/V10 dual-cue
    /// Isolator On/Off (0/1).  V10 only; always 0 on 900NXS2.
    uint8_t getIsolatorOn()    const { return mixerIsolatorOn.load(std::memory_order_relaxed); }
    /// Isolator Hi (0-255, 128=center).  V10 only.
    uint8_t getIsolatorHi()    const { return mixerIsolatorHi.load(std::memory_order_relaxed); }
    /// Isolator Mid (0-255, 128=center).  V10 only.
    uint8_t getIsolatorMid()   const { return mixerIsolatorMid.load(std::memory_order_relaxed); }
    /// Isolator Lo (0-255, 128=center).  V10 only.
    uint8_t getIsolatorLo()    const { return mixerIsolatorLo.load(std::memory_order_relaxed); }
    /// Fader curve (0/1/2).
    uint8_t getFaderCurve()    const { return mixerFaderCurve.load(std::memory_order_relaxed); }
    /// Crossfader curve (0/1/2).
    uint8_t getXfCurve()       const { return mixerXfCurve.load(std::memory_order_relaxed); }
    /// Booth monitor level (0-255).
    uint8_t getBoothLevel()    const { return mixerBooth.load(std::memory_order_relaxed); }
    uint8_t getBoothEqHi()     const { return mixerBoothEqHi.load(std::memory_order_relaxed); }   // A9/V10
    uint8_t getBoothEqLo()     const { return mixerBoothEqLo.load(std::memory_order_relaxed); }   // A9/V10

    /// Headphone Cue Link (0=off, 1=on).
    uint8_t getHpCueLink()     const { return mixerHpCueLink.load(std::memory_order_relaxed); }
    /// Headphone mixing knob (0=CUE, 255=Master).
    uint8_t getHpMixing()      const { return mixerHpMixing.load(std::memory_order_relaxed); }
    /// Headphone level (0-255).
    uint8_t getHpLevel()       const { return mixerHpLevel.load(std::memory_order_relaxed); }
    /// HP A Pre EQ button (0=off, 1=on).  V10 only; always 0 on 900NXS2.
    /// Booth EQ button (0=off, 1=on).  A9 and V10; always 0 on 900NXS2.
    uint8_t getBoothEq()    const { return mixerBoothEq.load(std::memory_order_relaxed); }
    /// Headphone B Cue Link (0=off, 1=on).  A9 and V10; always 0 on 900NXS2.
    uint8_t getHpCueLinkB()    const { return mixerHpCueLinkB.load(std::memory_order_relaxed); }
    /// Headphone B mixing knob (0=CUE, 255=Master).  A9 and V10.
    uint8_t getHpMixingB()     const { return mixerHpMixingB.load(std::memory_order_relaxed); }
    /// Headphone B level (0-255).  A9 and V10.
    uint8_t getHpLevelB()      const { return mixerHpLevelB.load(std::memory_order_relaxed); }

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
    /// Multi I/O Select.  A9 and V10; 0 on 900NXS2.
    /// A9: 0-3=CH1-CH4, 6=MIC, 7=Master, 8=XF-A, 9=XF-B
    /// V10: 0=Mic, 1-6=CH1-CH6, 7=Master
    uint8_t getMultiIoSelect() const { return mixerMultiIoSelect.load(std::memory_order_relaxed); }
    /// Multi I/O Level (0-255).  A9 and V10; 0 on 900NXS2.
    uint8_t getMultiIoLevel()  const { return mixerMultiIoLevel.load(std::memory_order_relaxed); }

    /// Color FX selector (255=OFF, 0=Space,1=DubEcho,2=Sweep,3=Noise,4=Crush,5=Filter).
    uint8_t getColorFxSelect() const { return mixerColorFxSel.load(std::memory_order_relaxed); }
    /// Color FX parameter knob (0-255).
    uint8_t getColorFxParam()  const { return mixerColorFxParam.load(std::memory_order_relaxed); }
    /// Color FX channel assign (same enum as Beat FX assign).
    uint8_t getColorFxAssign() const { return mixerColorFxAssign.load(std::memory_order_relaxed); }
    /// Send Ext1 On/Off (0/1).  V10 only; always 0 on 900NXS2.
    uint8_t getSendExt1()      const { return mixerSendExt1.load(std::memory_order_relaxed); }
    /// Send Ext2 On/Off (0/1).  V10 only; always 0 on 900NXS2.
    uint8_t getSendExt2()      const { return mixerSendExt2.load(std::memory_order_relaxed); }

    /// Master Mix On/Off (0/1).  V10 only; always 0 on 900NXS2.
    uint8_t getMasterMixOn()    const { return mixerMasterMixOn.load(std::memory_order_relaxed); }
    /// Master Mix Size/Feedback (0-255).  V10 only.
    uint8_t getMasterMixSize()  const { return mixerMasterMixSize.load(std::memory_order_relaxed); }
    /// Master Mix Time (0-255).  V10 only.
    uint8_t getMasterMixTime()  const { return mixerMasterMixTime.load(std::memory_order_relaxed); }
    /// Master Mix Tone (0-255).  V10 only.
    uint8_t getMasterMixTone()  const { return mixerMasterMixTone.load(std::memory_order_relaxed); }
    /// Master Mix Level (0-255).  V10 only.  Shares offset 0x0e2 with Color FX Param.
    uint8_t getMasterMixLevel() const { return mixerMasterMixLevel.load(std::memory_order_relaxed); }

    /// Mic EQ High (0-255, 128=center).
    uint8_t getMicEqHi()       const { return mixerMicEqHi.load(std::memory_order_relaxed); }
    /// Mic EQ Low (0-255, 128=center).
    uint8_t getMicEqLo()       const { return mixerMicEqLo.load(std::memory_order_relaxed); }

    /// Filter LPF button (0=off, 1=on).  V10 only; always 0 on 900NXS2.
    uint8_t getFilterLPF()     const { return mixerFilterLPF.load(std::memory_order_relaxed); }
    /// Filter HPF button (0=off, 1=on).  V10 only; always 0 on 900NXS2.
    uint8_t getFilterHPF()     const { return mixerFilterHPF.load(std::memory_order_relaxed); }
    /// Filter Resonance knob (0-255).  V10 only; always 0 on 900NXS2.
    uint8_t getFilterResonance() const { return mixerFilterReso.load(std::memory_order_relaxed); }

    /// Has VU meter data been received recently?
    bool hasVuMeterData() const
    {
        if (!hasVuData.load(std::memory_order_relaxed)) return false;
        double last = lastVuPacketTime.load(std::memory_order_relaxed);
        double now  = juce::Time::getMillisecondCounterHiRes();
        return (now - last) < 5000.0;
    }

    /// VU peak level for a channel (0=silence, 32767=clip).
    /// ch: 0-5=CH1-CH6, kVuMasterL=Master L, kVuMasterR=Master R
    uint16_t getVuPeak(int ch) const
    {
        if (ch < 0 || ch >= ProDJLink::kVuSlots) return 0;
        return vuPeak[ch].load(std::memory_order_relaxed);
    }

    /// VU peak as normalised float 0.0-1.0.
    float getVuPeakNorm(int ch) const { return float(getVuPeak(ch)) / 32767.0f; }

    /// Copy all 15 VU segments for one channel into dst[15].
    /// ch: 0-5=CH1-CH6, kVuMasterL=Master L, kVuMasterR=Master R
    void getVuSegments(int ch, uint16_t dst[15]) const
    {
        if (ch < 0 || ch >= ProDJLink::kVuSlots) { std::memset(dst, 0, 15 * sizeof(uint16_t)); return; }
        const juce::SpinLock::ScopedLockType sl(vuDataLock);
        std::memcpy(dst, vuSegments[ch], 15 * sizeof(uint16_t));
    }

    /// Model name of the first known DJM mixer (empty if none discovered yet).
    juce::String getDJMModel() const
    {
        const juce::ScopedLock sl(djmIpLock);
        return djmModels.empty() ? juce::String() : juce::String(djmModels[0]);
    }

    /// Number of mixer channels based on detected DJM model.
    /// DJM-V10 / V10-LF = 6 channels; all others (900NXS2, A9) = 4.
    int getMixerChannelCount() const
    {
        juce::String model = getDJMModel();
        if (model.containsIgnoreCase("V10")) return 6;
        return 4;
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

    /// Suggest a player number for dbserver queries to NXS2.
    /// NXS2 dbserver only accepts player numbers 1-4 that are actually present
    /// on the network.  Returns a discovered player (1-4) that is not
    /// `excludePlayer`, or 0 if none found (query will fail).
    int suggestDbPlayerNumber(int excludePlayer) const
    {
        for (int pn = 1; pn <= 4; ++pn)
        {
            if (pn == excludePlayer) continue;
            int idx = pn - 1;
            if (players[idx].discovered.load(std::memory_order_relaxed))
                return pn;
        }
        // No other player 1-4 found.  Last resort: use excludePlayer itself
        // (may work if the CDJ allows self-referencing queries).
        if (excludePlayer >= 1 && excludePlayer <= 4)
            return excludePlayer;
        return 0;
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

    /// Running beat counter (1-based) from CDJ status packets.
    /// Used by TimecodeEngine for beat grid lookup on NXS2 (beatCount → ms).
    uint32_t getBeatCount(int playerNum) const
    {
        int idx = playerNum - 1;
        if (idx < 0 || idx >= ProDJLink::kMaxPlayers) return 0;
        return players[idx].beatCount.load(std::memory_order_relaxed);
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
        // Bridge announce: hello 0x0A (player=5) -> claim 0x02 (player=0xC0)
        // -> then starts 54B keepalive 0x06 (player=0xC1).
        // The DJM needs to see this announce before it activates fader delivery.
        performBridgeJoinSequence();

        double lastKeepaliveSend  = 0.0;
        double lastBridgeSubSend  = 0.0;
        double lastBridgeNotify   = 0.0;
        double firstKeepaliveSent = 0.0;   // timestamp of very first keepalive
        bool   initialSubSent     = false;  // true after the first subscribe burst

        while (!threadShouldExit() && isRunningFlag.load(std::memory_order_acquire))
        {
            double now = juce::Time::getMillisecondCounterHiRes();

            // --- Dual keepalive system ---
            // Two keepalives serve different purposes:
            //
            // 1) 54B bridge keepalive BROADCAST (player=0xC1)
            //    -> DJM discovers us as bridge -> activates fader (0x39) delivery
            //    -> This is the standard bridge broadcast keepalive
            //
            // 2) 95B dbserver keepalive UNICAST to each CDJ (player=5)
            //    -> CDJ-3000 registers us as a valid peer with PIONEER identification
            //    -> CDJ sends unicast Status + AbsPos data to our IP
            //
            // CRITICAL: The 95B MUST be unicast to CDJ IPs only -- NEVER broadcast.
            // If the DJM receives both player=5 (95B) and player=0xC1 (54B) from
            // the same IP/MAC, it detects conflicting identities and refuses faders.
            // By unicasting the 95B, the DJM only ever sees the 54B broadcast.
            if ((now - lastKeepaliveSend) >= ProDJLink::kKeepaliveInterval * 1000.0)
            {
                sendBridgeKeepalive();           // 54B BROADCAST -> DJM faders
                sendDbServerKeepaliveToAll();     // 95B UNICAST to CDJs -> CDJ status data
                lastKeepaliveSend = now;
                if (firstKeepaliveSent == 0.0)
                    firstKeepaliveSent = now;
            }

            // --- Send bridge subscribe (0x57) to all known DJMs ---
            // This triggers the DJM to send type-0x39 mixer fader packets and
            // type-0x29 channel on-air status packets.
            //
            // IMPORTANT: The first subscribe must be DELAYED after the first
            // keepalive broadcast.  The DJM-900NXS2 needs time to register the
            // bridge identity from the 54B broadcast before it will honour a
            // 0x57 subscribe.  On macOS, if the subscribe arrives before the
            // DJM has fully processed the keepalive registration, it silently
            // ignores it and never activates fader delivery.
            //
            // Sequence: keepalive broadcasts begin -> DJM registers bridge
            // (counter increments) -> wait ~3s -> send subscribe burst.
            {
                bool djmsKnown = false;
                {
                    const juce::ScopedLock sl(djmIpLock);
                    djmsKnown = !djmIps.empty();
                }

                // Delay first subscribe: wait at least 3s after the first
                // keepalive so the DJM has seen 2+ keepalive broadcasts.
                bool readyForFirstSub = (firstKeepaliveSent > 0.0
                                         && (now - firstKeepaliveSent) >= 3000.0);

                if (djmsKnown && !initialSubSent && readyForFirstSub)
                {
                    // First contact: send subscribe (will be repeated on next
                    // loop iterations via the normal re-subscribe timer).
                    // Previously this used a burst of 3 with Thread::sleep(200)
                    // between each, but that blocked the network thread for 400ms
                    // causing packet accumulation and stale playhead values.
                    DBG("ProDJLink: Initial DJM subscribe");
                    sendBridgeSubscribeToAll();
                    initialSubSent    = true;
                    lastBridgeSubSend = now;
                }
                else if (initialSubSent
                         && (now - lastBridgeSubSend) >= ProDJLink::kBridgeSubInterval * 1000.0)
                {
                    sendBridgeSubscribeToAll();
                    lastBridgeSubSend = now;
                }
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
            // Strategy: block on beatSock for up to 5ms (the busiest socket,
            // carrying abspos at ~15ms intervals and DJM mixer data).
            // This drives the loop rate from ~1000 iter/sec down to ~200 iter/sec,
            // reducing syscall count from ~31,000/sec to ~600/sec.
            // The loop wakes immediately when any beat packet arrives, so there
            // is no added latency. keepaliveSock and statusSock are polled
            // non-blocking afterwards -- they are low-rate (~1Hz and ~5Hz).

            juce::String sender;
            int port = 0;

            // Block up to 5ms waiting for a beat/abspos/mixer packet.
            // Falls through immediately if a packet is already waiting.
            if (beatSock)
                beatSock->waitUntilReady(true, 5);

            // Keepalive (port 50000) -- ~1Hz per player
            if (keepaliveSock && keepaliveSock->waitUntilReady(true, 0))
            {
                uint8_t buf[256];
                int n = keepaliveSock->read(buf, sizeof(buf), false, sender, port);
                if (n > 0)
                    handleKeepalivePacket(buf, n, sender);
            }

            // Beat (port 50001) -- abspos ~67Hz, DJM mixer ~33Hz, drain all ready
            int beatDrained = 0;
            if (beatSock)
            {
                while (beatDrained < 20 && beatSock->waitUntilReady(true, 0))
                {
                    uint8_t buf[600];
                    int n = beatSock->read(buf, sizeof(buf), false, sender, port);
                    if (n > 0)
                        handleBeatPacket(buf, n);
                    ++beatDrained;
                }
            }

            // Status (port 50002) -- ~5Hz per player
            int statusDrained = 0;
            if (statusSock)
            {
                while (statusDrained < 10 && statusSock->waitUntilReady(true, 0))
                {
                    uint8_t buf[1200];
                    int n = statusSock->read(buf, sizeof(buf), false, sender, port);
                    if (n > 0)
                        handleStatusPacket(buf, n);
                    ++statusDrained;
                }
            }

            // GC: remove stale players
            gcPlayers(now);
        }

        DBG("ProDJLink: Thread stopped");
    }

    //==========================================================================
    // Dual keepalive system -- CDJ + DJM compatible
    //
    // Two keepalives are sent in parallel, each serving a different device:
    //
    //   1) sendBridgeKeepalive()       -- 54B BROADCAST (player=0xC1)
    //      -> DJM discovers us as bridge -> activates fader (0x39) delivery
    //      -> Standard bridge broadcast keepalive
    //
    //   2) sendDbServerKeepaliveToAll() -- 95B UNICAST to each CDJ (player=5)
    //      -> CDJ-3000 validates PIONEER DJ CORP / PRODJLINK BRIDGE strings
    //      -> CDJ registers us as a valid peer -> sends Status + AbsPos unicast
    //
    // The DJM NEVER sees the 95B packet (it's sent unicast to CDJ IPs only).
    // From the DJM's perspective, we have a single identity: player=0xC1.
    //
    // History:
    //   - v1.5a: 95B BROADCAST + 54B UNICAST -> CDJ[OK] DJM[FAIL] (DJM saw 2 identities)
    //   - v1.5b: 54B BROADCAST only          -> CDJ[FAIL] DJM[OK] (CDJ ignored player=0xC1)
    //   - v1.5c: 54B BROADCAST + 95B UNICAST -> CDJ[OK] DJM[OK] (DJM only sees broadcast)
    //==========================================================================

    // dbserver keepalive (0x06) -- 95B UNICAST to each discovered CDJ.
    // Contains "PIONEER DJ CORP" / "PRODJLINK BRIDGE" identification strings
    // that CDJ-3000 validates for granting dbserver metadata access and
    // for sending unicast Status + AbsPos data to our IP.
    //
    // CRITICAL: This must be UNICAST to CDJ IPs only -- NEVER broadcast.
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

        // Pioneer bridge identification (bytes 54-94) -- REQUIRED for dbserver access
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

    // Bridge keepalive (0x06) -- 54B BROADCAST
    // Standard bridge format: variant=0x01, player=0xC1,
    // class=0x00, flags=0x05, last=0x20. DJM activates faders on seeing this.
    //
    // CRITICAL: This must be BROADCAST, not unicast to the DJM.
    // The bridge broadcasts this and the DJM discovers it via
    // standard keepalive broadcast monitoring on port 50000. Sending as
    // unicast does not register in the DJM's device table.
    //
    // macOS fix: ALSO send as unicast to each known DJM IP.
    // Some macOS network stacks/NIC drivers have subtle issues with broadcast
    // UDP delivery (checksum offloading, interface routing, firewall interaction)
    // that can cause the DJM to never see the broadcast keepalive even though it
    // reaches the wire. Unicasting a copy ensures the DJM sees our bridge identity.
    // The DJM receives the same player=0xC1 identity both ways -- no conflict.
    void sendBridgeKeepalive()
    {
        if (!keepaliveSock) return;
        uint8_t pkt[54];
        std::memset(pkt, 0, sizeof(pkt));
        std::memcpy(pkt, ProDJLink::kMagic, ProDJLink::kMagicLen);
        pkt[0x0a] = 0x06;
        std::strncpy(reinterpret_cast<char*>(pkt + 0x0c), "TCS-SHOWKONTROL", 19);
        pkt[0x20] = 0x01;  pkt[0x21] = 0x01;  pkt[0x23] = 0x36;
#ifdef __APPLE__
        pkt[0x24] = 0xF9;  pkt[0x25] = 0x00;
#else
        pkt[0x24] = 0xC1;  pkt[0x25] = 0x00;
#endif
        std::memcpy(pkt + 0x26, ownMacBytes, 6);
        std::memcpy(pkt + 0x2c, ownIpBytes, 4);
        pkt[0x30] = 0x03;  pkt[0x34] = 0x05;  pkt[0x35] = 0x20;

        // 1) Standard broadcast (all devices see it)
        keepaliveSock->write(broadcastIp, ProDJLink::kKeepalivePort, pkt, sizeof(pkt));

        // NOTE: No unicast keepalive to DJM.
        // The real Pioneer Bridge only broadcasts keepalives -- it never sends
        // unicast copies to the DJM. Confirmed from full startup capture
        // (Captura_larga_bridge_por_wifi.pcapng): the DJM activates faders
        // 0.2s after seeing the broadcast keepalive alone.
        // The previous unicast copy may have confused the DJM on WiFi
        // (two keepalives from same name but different source ports).
    }

    // (Old CDJ 4-phase join sequence removed -- STC uses bridge join only)

    //==========================================================================
    // Bridge join sequence -- announces our presence on the network.
    //
    // From capture analysis: the DJM responds in <0.2s after the FIRST
    // 54B keepalive broadcast. The 21 claims the reference implementation sends are its
    // own slot-reservation process -- not a DJM requirement.
    //
    // Pioneer Bridge sends 2 hellos + 11 claims (~6s). More claims gives the
    // DJM more time to register the bridge identity. Additive -- won't break
    // platforms where fewer claims already worked.
    //==========================================================================
    void performBridgeJoinSequence()
    {
        if (!keepaliveSock) return;
        DBG("ProDJLink: Starting bridge join sequence (2 hello + 11 claims)");

        // Hello announce x 2 (Pioneer Bridge does this)
        for (int h = 0; h < 2; ++h)
        {
            if (threadShouldExit()) return;
            sendBridgeJoinHello();
            juce::Thread::sleep(300);
        }

        // 11 IP claims (~500ms apart, matching Pioneer Bridge timing)
        for (int n = 1; n <= 11; ++n)
        {
            if (threadShouldExit()) return;
            sendBridgeJoinClaim(n);
            juce::Thread::sleep(500);
        }

        joinCompleted.store(true, std::memory_order_release);
        DBG("ProDJLink: Bridge join complete - keepalive starting");
    }

    //--------------------------------------------------------------------------
    // Bridge hello (0x0A) -- 37 bytes broadcast
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
    // Bridge IP claim (0x02) -- 50 bytes broadcast
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
        // [0x2e] = auto-increment token (XOR-based hash -- the exact value
        // doesn't appear to affect DJM fader activation, only the overall
        // claim structure matters)
        p[0x2e] = uint8_t(ownMacBytes[5] ^ uint8_t(counter * 3 + 0xFB));
        p[0x2f] = uint8_t(counter);              // counter
        p[0x30] = 0xC0;                           // bridge claim identity
#ifdef __APPLE__
        p[0x30] = uint8_t(vCDJPlayerNumber);      // macOS needs player 5
#endif
        p[0x31] = 0x00;
        keepaliveSock->write(broadcastIp, ProDJLink::kKeepalivePort, p, sizeof(p));
    }

    //==========================================================================
    // Bridge notify (0x55) -- sent to each CDJ on port 50002.
    //
    // The bridge sends this periodically to maintain the session.
    // Packet format (44 bytes) from capture:
    //   [0-9]   Magic "Qspt1WmJOL"
    //   [10]    0x55 (type)
    //   [11-30] Device name + null padding
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

        // Original path: statusSock (sport=50002) -> CDJ:50002  -- works on Windows
        statusSock->write(cdjIp, ProDJLink::kStatusPort, pkt, sizeof(pkt));

        // Additional: bridgeSock (ephemeral port) -> CDJ:50002  -- for macOS compat
        if (bridgeSock)
            bridgeSock->write(cdjIp, ProDJLink::kStatusPort, pkt, sizeof(pkt));
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

        // On-air flags at bytes 36-41 (4-ch DJMs use 36-39, V10 may use 36-41)
        bool chOnAir[6] = { false };
        int numCh = juce::jmin(6, len - 36);  // how many channel flags fit
        for (int i = 0; i < numCh; ++i)
            chOnAir[i] = (data[36 + i] != 0x00);

        // Update player on-air state (only if we don't have 0x29 unicast data,
        // which is more reliable since it's DJM->bridge specific)
        if (pktCountDJMStatus.load(std::memory_order_relaxed) == 0)
        {
            for (int i = 0; i < ProDJLink::kMaxPlayers; ++i)
            {
                if (!players[i].discovered.load(std::memory_order_relaxed)) continue;
                uint8_t pn = players[i].playerNumber.load(std::memory_order_relaxed);
                if (pn < 1 || pn > 6) continue;
                players[i].isOnAir.store(chOnAir[pn - 1], std::memory_order_relaxed);
            }
        }

        DBG("ProDJLink: 0x03 on-air broadcast ch1=" << (int)chOnAir[0] << " ch2=" << (int)chOnAir[1]
            << " ch3=" << (int)chOnAir[2] << " ch4=" << (int)chOnAir[3]);
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

            // New DJM -- add to list (subscribe outside the lock)
            djmIps.push_back(ip);
            djmModels.push_back(model);
            djmLastSeen.push_back(juce::Time::getMillisecondCounterHiRes());
            isNew = true;
        }
        // Socket write OUTSIDE the lock -- avoids blocking UI thread on getDJMModel()
        if (isNew)
        {
            DBG("ProDJLink: Registered DJM [" << juce::String(model) << "] at " << juce::String(ip));
            // NOTE: Do NOT send subscribe here.  The main loop handles the
            // initial subscribe with a deliberate delay (3s after first
            // keepalive) so the DJM has time to fully register our bridge
            // identity before receiving the 0x57.  An immediate subscribe
            // here would race against the DJM's keepalive processing.
        }
    }

    // Bridge subscribe (0x57) -- 40B sent to DJM on beat port (50001).
    // Triggers DJM fader (0x39->50002) and VU meter (0x58->50001) delivery.
    // Byte [33] is the subscription bitmask.
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
#ifdef __APPLE__
        pkt[33] = 0xFE;  // macOS: full subscription (faders + VU)
#else
        pkt[33] = 0x87;  // Windows: proven bitmask (faders + VU)
#endif
        pkt[34] = 0x00;
        pkt[35] = 0x04;  // subtype
        pkt[36] = 0x01;  // subscribe = 1

        auto djmStr = juce::String(djmIp);

        // Send ONLY from bridgeSock (ephemeral port), matching the real Bridge
        // which uses a dedicated port (~50006) for subscribe/notify traffic.
        // NOT from beatSock (50001) -- that port is for receiving beats.
        // The DJM may reject subscribes from a port it also sends data to.
        // Fallback to beatSock only if bridgeSock is unavailable.
        auto* sock = bridgeSock ? bridgeSock.get() : beatSock.get();
        sock->write(djmStr, ProDJLink::kBeatPort, pkt, sizeof(pkt));

        DBG("ProDJLink: Sent 0x57 subscribe to " << djmStr
            << " port " << ProDJLink::kBeatPort);
    }

    void sendBridgeSubscribeToAll()
    {
        // Copy IPs under lock, then write outside -- same pattern as registerDJM.
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

        if (pn == 0 || pn == uint8_t(vCDJPlayerNumber) || pn == 0xC0 || pn == 0xC1 || pn == 0xF9) return;  // ignore self + bridge identities

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
            //
            // The CDJ-3000 sends PAIRS of 0x0b packets every ~30ms:
            //   1) Real position data: byte[33] = player number (1-6)
            //   2) Unknown variant:    byte[33] = high random value (0x80-0xFF)
            // The second variant has garbage in the position fields and is
            // already filtered by the pn > kMaxPlayers check above.
            // Confirmed from Wireshark captures on both Mac and Windows.
            //
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

            // Reverse play detection: position decreasing while playing.
            // prevAbsPosMs is non-atomic (only written from network thread).
            // Require at least 10ms movement to avoid noise at boundaries.
            // Suppress during active loops (loop reset jumps backward).
            if (p.prevAbsPosMs > 0 && p.isPlaying.load(std::memory_order_relaxed))
            {
                bool inLoop = (p.loopStartMs.load(std::memory_order_relaxed) != 0
                            && p.loopEndMs.load(std::memory_order_relaxed) != 0);
                bool rev = !inLoop && (playhead + 10 < p.prevAbsPosMs);
                p.isReverse.store(rev, std::memory_order_relaxed);
            }
            else
            {
                p.isReverse.store(false, std::memory_order_relaxed);
            }
            p.prevAbsPosMs = playhead;

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

                // --- NXS2 beat-derived position advancement ---
                // Beat packets arrive at the exact moment of each beat (~2Hz at 120BPM).
                // For NXS2 (no abspos), advance beatCount by 1 and derive a fresh
                // playhead position.  This gives additional position anchors between
                // the 5Hz status packets, reducing the interpolation gap from ~200ms
                // to ~80ms at 120BPM.  The next status packet corrects beatCount to
                // the authoritative CDJ value (self-correcting).
                if (p.hasBeatDerivedPosition.load(std::memory_order_relaxed))
                {
                    uint32_t bc = p.beatCount.load(std::memory_order_relaxed);
                    if (bc > 0 && bpm > 0 && bpm != 0xFFFF)
                    {
                        bc++;
                        p.beatCount.store(bc, std::memory_order_relaxed);
                        double bpmReal = double(bpm) / 100.0;
                        double msPerBeat = 60000.0 / bpmReal;
                        uint32_t derivedMs = uint32_t(double(bc) * msPerBeat);
                        p.playheadMs.store(derivedMs, std::memory_order_relaxed);
                        p.absPositionTs.store(juce::Time::getMillisecondCounterHiRes(),
                                              std::memory_order_relaxed);
                    }
                }
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

            // --- Beat-derived playhead for non-CDJ-3000 models ---
            // NXS2 and older players don't send Absolute Position packets (0x0b).
            // Derive playhead from: beatCount x (60000 / BPM).
            // Updated at ~5Hz (status packet rate). PLL smooths between updates
            // using actualSpeed (offset 152) or dp/dt fallback.
            //
            // Guard: beat packets (processed earlier in the same loop iteration)
            // may have already advanced beatCount beyond what this status packet
            // reports (status is generated by the CDJ at ~5Hz and may be stale
            // relative to a beat that just fired).  Only accept if bc >= stored
            // to prevent backward position jumps.  On first activation
            // (hasBeatDerivedPosition==false), always accept to establish baseline.
            if (!p.hasAbsolutePosition.load(std::memory_order_relaxed) && bc > 0)
            {
                uint32_t storedBc = p.beatCount.load(std::memory_order_relaxed);
                bool alreadyActive = p.hasBeatDerivedPosition.load(std::memory_order_relaxed);

                // Accept if: first activation (baseline), forward/equal (normal),
                // or large backward jump (>4 beats = seek/scratch/cue, not stale).
                // At 120BPM + 5Hz status, beat packets advance at most ~3 beats
                // between status updates, so staleness is <=3 beats.  A jump of
                // 4+ beats backward is a real CDJ event, not packet ordering.
                bool accept = !alreadyActive
                           || bc >= storedBc
                           || (storedBc - bc) > 4;

                if (accept)
                {
                    p.beatCount.store(bc, std::memory_order_relaxed);
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
            else
            {
                // CDJ-3000 or no beat count yet: store unconditionally
                p.beatCount.store(bc, std::memory_order_relaxed);
            }

            // NOTE: beatInBar is NOT updated here.  Status packets arrive at ~5Hz
            // and may carry a stale beat-in-bar value from before the latest beat
            // transition, causing the UI to briefly flicker backwards (e.g. 3->2->3).
            // Beat packets (type 0x28, port 50001) are the authoritative source --
            // they arrive at the exact moment of each beat and are handled in
            // handleBeatPacket().
        }

        // CDJ-3000 extended fields (0x200-byte status packets)
        if (len >= 0x1C2)
        {
            // Active loop start/end -- non-zero when any loop is active
            // (stored from rekordbox or dynamically created by the DJ).
            // Raw encoding: position_ms = raw_value * 65536 / 1000
            uint32_t rawStart = ProDJLink::readU32BE(data + 0x1B6);
            uint32_t rawEnd   = ProDJLink::readU32BE(data + 0x1BE);
            uint32_t loopStart = (rawStart != 0)
                ? (uint32_t)((uint64_t)rawStart * 65536ULL / 1000ULL) : 0;
            uint32_t loopEnd = (rawEnd != 0)
                ? (uint32_t)((uint64_t)rawEnd * 65536ULL / 1000ULL) : 0;
            p.loopStartMs.store(loopStart, std::memory_order_relaxed);
            p.loopEndMs.store(loopEnd, std::memory_order_relaxed);
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
        //   - Its keepalive triggers registerDJM() -> re-adds IP + immediate subscribe
        //   - Our 54B broadcast keepalive is still going -> DJM rediscovers us
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
    // Socket helpers
    //==========================================================================

    /// Explicitly re-assert SO_BROADCAST on a JUCE DatagramSocket after bind.
    /// Applied on ALL platforms -- harmless re-confirmation on Windows,
    /// critical on macOS where the flag can be lost after bindToPort().
    static void ensureSoBroadcast(juce::DatagramSocket* sock, const char* label)
    {
        juce::ignoreUnused(label);
        if (!sock) return;
        auto fd = sock->getRawSocketHandle();
        if (fd < 0)
        {
            DBG("ProDJLink: WARNING -- " << label << " has no raw socket handle");
            return;
        }
        int flag = 1;
#ifdef _WIN32
        int rc = ::setsockopt((SOCKET)fd, SOL_SOCKET, SO_BROADCAST,
                              (const char*)&flag, sizeof(flag));
        int verify = 0;
        int vlen = (int)sizeof(verify);
        ::getsockopt((SOCKET)fd, SOL_SOCKET, SO_BROADCAST, (char*)&verify, &vlen);
#else
        socklen_t flen = sizeof(flag);
        int rc = ::setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &flag, flen);
        int verify = 0;
        socklen_t vlen = sizeof(verify);
        ::getsockopt(fd, SOL_SOCKET, SO_BROADCAST, &verify, &vlen);
#endif
        (void)rc; (void)verify;  // used only in debug logging below
#if JUCE_DEBUG
        if (rc != 0 || verify != 1)
            DBG("ProDJLink: WARNING -- SO_BROADCAST on " << label
                << " fd=" << fd << " set_rc=" << rc << " readback=" << verify);
        else
            DBG("ProDJLink: SO_BROADCAST OK on " << label << " fd=" << fd);
#endif
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

    /// Get MAC address for a network interface.
    /// On Windows, matches by IP address (reliable -- adapter FriendlyName can
    /// change with driver/OS updates, breaking name-based matching).
    /// On macOS/Linux, matches by interface name (stable, e.g. "en0").
    /// The interfaceIp parameter is used on Windows for IP-based matching.
    static bool getMacAddress(const juce::String& ifaceName, uint8_t out[6],
                              const juce::String& interfaceIp = {})
    {
        std::memset(out, 0, 6);

#ifdef _WIN32
        ULONG bufSize = 15000;
        std::vector<uint8_t> buffer(bufSize);
        auto* addresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
        ULONG result = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, nullptr, addresses, &bufSize);
        if (result == ERROR_BUFFER_OVERFLOW)
        {
            buffer.resize(bufSize);
            addresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
            result = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, nullptr, addresses, &bufSize);
        }
        if (result != NO_ERROR) return false;

        // Primary: match by IP address (most reliable on Windows).
        // Adapter FriendlyName can change with driver/OS updates, regional
        // settings, or user renaming. The IP is what we actually bound to.
        if (interfaceIp.isNotEmpty())
        {
            for (auto* adapter = addresses; adapter; adapter = adapter->Next)
            {
                for (auto* unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next)
                {
                    if (unicast->Address.lpSockaddr->sa_family == AF_INET)
                    {
                        auto* addr = reinterpret_cast<sockaddr_in*>(unicast->Address.lpSockaddr);
                        char ipStr[INET_ADDRSTRLEN];
                        inet_ntop(AF_INET, &addr->sin_addr, ipStr, sizeof(ipStr));
                        if (interfaceIp == ipStr && adapter->PhysicalAddressLength >= 6)
                        {
                            std::memcpy(out, adapter->PhysicalAddress, 6);
                            return true;
                        }
                    }
                }
            }
        }

        // Fallback: match by adapter friendly name or description
        for (auto* adapter = addresses; adapter; adapter = adapter->Next)
        {
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
    // COMPLETE OFFSET MAP -- confirmed from DJM-900NXS2, DJM-A9, and DJM-V10
    // packet captures.
    //
    // Per-channel block: 24 bytes, base=[0x024,0x03c,0x054,0x06c]
    //   +0   Input source:
    //          900NXS2: 0=PC USB A, 1=PC USB B, 2=DIGITAL, 3=LINE,
    //                   4=PHONO, 8=RET/AUX
    //          A9:      0=PC USB A, 1=PC USB B, 2=DIGITAL, 3=LINE,
    //                   4=PHONO, 7=USB, 8=RETURN, 10=BLUETOOTH
    //          V10:     0=PC USB A, 1=PC USB B, 2=DIGITAL, 3=LINE,
    //                   4=PHONO, 5=BUILT-IN, 6=EXT1, 7=EXT2,
    //                   8=MULTI I/O, 9=COMBO
    //   +1   Trim          (0-255, 128=unity)
    //   +2   Compressor    (0-255; V10 per-ch Comp knob, 0 on 900NXS2/A9)
    //   +3   EQ High       (0-255, 128=center)
    //   +4   EQ Mid        (0-255, 128=center; "Hi Mid" on V10 4-band EQ)
    //   +5   EQ Low Mid    (0-255, 128=center; V10 only, 0 on 900NXS2/A9)
    //   +6   EQ Low        (0-255, 128=center)
    //   +7   Color/FX      (0-255, 128=center)
    //   +8   Send          (0-255, V10 per-ch send knob; 0 on 900NXS2/A9)
    //   +9   CUE button    (0=off, 1=on)
    //   +10  CUE B button  (0=off, 1=on; A9/V10 dual-cue, 0 on 900NXS2)
    //   +11  Channel fader (0=closed, 255=open)
    //   +12  XF Assign     (0=THRU, 1=A, 2=B)
    //
    // Global / Master:
    //   [0x0b4]  Crossfader       (0=side-A, 255=side-B)
    //   [0x0b5]  Fader curve      (0/1/2)
    //   [0x0b6]  Crossfader curve (0/1/2)
    //   [0x0b7]  Master fader     (0-255)
    //   [0x0b9]  Master CUE btn   (0/1)
    //   [0x0ba]  Master CUE B btn (0/1; A9/V10 dual-cue, 0 on 900NXS2)
    //   [0x0bb]  Isolator On      (0/1; V10 only)
    //   [0x0bc]  Isolator Hi      (0-255, 128=center; V10 only)
    //   [0x0bd]  Isolator Mid     (0-255, 128=center; V10 only)
    //   [0x0be]  Isolator Lo      (0-255, 128=center; V10 only)
    //   [0x0bf]  Booth monitor    (0-255)
    //   [0x0c0]  Booth EQ Hi      (0-255, 128=center; A9/V10, 0 on 900NXS2)
    //   [0x0c1]  Booth EQ Lo      (0-255, 128=center; A9/V10, 0 on 900NXS2)
    //
    // Headphones:
    //   [0x0c4]  HP A Cue Link    (0/1)
    //   [0x0c5]  HP B Cue Link    (0/1; A9/V10, 0 on 900NXS2)
    //   [0x0e3]  HP A Mixing      (0=CUE, 255=Master)
    //   [0x0e4]  HP A Level       (0-255)
    //   [0x0e5]  Booth EQ         (0/1; A9/V10, 0 on 900NXS2)
    //   [0x0e6]  HP B Mixing      (0=CUE, 255=Master; A9/V10)
    //   [0x0e7]  HP B Level       (0-255; A9/V10)
    //
    // Beat FX:
    //   [0x0c6]  FX Freq Low      (0/1)
    //   [0x0c7]  FX Freq Mid      (0/1)
    //   [0x0c8]  FX Freq Hi       (0/1)
    //   [0x0c9]  Beat FX Select   (0-13)
    //            900NXS2: Delay,Echo,PingPong,Spiral,Reverb,
    //                     Trans,Filter,Flanger,Phaser,Pitch,
    //                     SlipRoll,Roll,VinylBrake,Helix
    //            A9:      Delay,Echo,PingPong,Spiral,Helix,Reverb,
    //                     Flanger,Phaser,Filter,TripletFilter,
    //                     Trans,Roll,TripletRoll,Mobius
    //            V10:     Delay,Echo,PingPong,Spiral,Helix,
    //                     Reverb,Shimmer,Flanger,Phaser,Filter,
    //                     Trans,Roll,Pitch,VinylBrake
    //   [0x0ca]  FX Assign        900NXS2: (0=Mic,1=CH1,2=CH2,3=CH3,7=CH4,
    //                               6=XF-A,8=XF-B,9=Master)
    //                            V10: (0-5=CH1-CH6, 6=Mic, 7=Master)
    //   [0x0cb]  Beat FX Level    (0-255)
    //   [0x0cc]  Beat FX ON/OFF   (0/1)
    //   [0x0ce]  900NXS2: Beat FX Assign (mirrors 0x0ca)
    //            A9: Multi I/O Select (0-3=CH1-CH4, 6=MIC, 7=Master,
    //                                  8=XF-A, 9=XF-B)
    //            V10: Multi I/O Select (0=Mic, 1-6=CH1-CH6, 7=Master)
    //   [0x0cf]  900NXS2: Send/Return Lvl (0-255)
    //            A9/V10: Multi I/O Level (0-255)
    //
    // Color FX / Sends:
    //   [0x0db]  900NXS2: Color FX Select (255=OFF, 0=Space,1=DubEcho,2=Sweep,
    //                                      3=Noise,4=Crush,5=Filter)
    //            V10: Send Built-IN Select (255=OFF, 0=ShortDelay,1=LongDelay,
    //                                       3=DubEcho,4=Reverb; val 2 unknown)
    //   [0x0dc]  Send Ext1 On/Off  (0/1; V10 only)
    //   [0x0dd]  Send Ext2 On/Off  (0/1; V10 only)
    //   [0x0e2]  Color FX Param   (0-255; 900NXS2 only)
    //
    // Master Mix (V10 only):
    //   [0x0de]  Master Mix On    (0/1)
    //   [0x0df]  Master Mix Size/Feedback (0-255)
    //   [0x0e0]  Master Mix Time  (0-255)
    //   [0x0e1]  Master Mix Tone  (0-255)
    //   [0x0e2]  Master Mix Level (0-255; shares offset with Color FX Param)
    //
    // Mic:
    //   [0x0d6]  Mic EQ Hi        (0-255, 128=center)
    //   [0x0d7]  Mic EQ Lo        (0-255, 128=center)
    //
    // Filter (V10 only):
    //   [0x0d8]  Filter LPF       (0/1)
    //   [0x0d9]  Filter HPF       (0/1)
    //   [0x0da]  Filter Resonance (0-255)
    //==========================================================================
    void handleMixerPacket(const uint8_t* data, int len)
    {
        if (len < 0xe6) return;  // need up to HP A Pre EQ at 0x0e5

        // --- Per-channel block (24 bytes each) ---
        // Confirmed offsets for 4-channel and 6-channel DJMs.
        // V10 CH5/CH6 at 0x084/0x09c confirmed working (same 0x18 stride).
        // Global offsets (crossfader, master, HP, FX) are identical on both
        // 4-ch and 6-ch -- confirmed from V10__nuevo_crossfader.pcapng.
        static constexpr int chBase[6] = {
            0x024, 0x03c, 0x054, 0x06c,   // CH1-CH4 (confirmed)
            0x084, 0x09c                   // CH5-CH6 (V10 -- confirmed working)
        };
        int numCh = getMixerChannelCount();
        for (int ch = 0; ch < numCh; ++ch)
        {
            int b = chBase[ch];
            if (b + 13 > len) break;  // not enough data for this channel
            mixerInputSrc[ch].store (data[b + 0],  std::memory_order_relaxed);
            mixerTrim[ch].store     (data[b + 1],  std::memory_order_relaxed);
            mixerComp[ch].store     (data[b + 2],  std::memory_order_relaxed);  // V10 Compressor (0 on 900NXS2)
            mixerEqHi[ch].store     (data[b + 3],  std::memory_order_relaxed);
            mixerEqMid[ch].store    (data[b + 4],  std::memory_order_relaxed);
            mixerEqLoMid[ch].store  (data[b + 5],  std::memory_order_relaxed);  // V10 Lo Mid (0 on 900NXS2)
            mixerEqLo[ch].store     (data[b + 6],  std::memory_order_relaxed);
            mixerColor[ch].store    (data[b + 7],  std::memory_order_relaxed);
            mixerSend[ch].store     (data[b + 8],  std::memory_order_relaxed);  // V10 per-ch Send knob (0 on 900NXS2)
            mixerCueBtn[ch].store   (data[b + 9],  std::memory_order_relaxed);
            mixerCueBtnB[ch].store  (data[b + 10], std::memory_order_relaxed);  // A9/V10 CUE B (0 on 900NXS2)
            mixerFader[ch].store    (data[b + 11], std::memory_order_relaxed);
            mixerXfAssign[ch].store (data[b + 12], std::memory_order_relaxed);
        }

        // --- Global / Master ---
        // These offsets are the same for both 4-channel and 6-channel DJMs.
        // Confirmed: V10 0x39 packets are 248 bytes (same as 900NXS2) with
        // globals at identical absolute offsets (V10__nuevo_crossfader.pcapng).
        if (0x0b4 + 3 <= len)
        {
            mixerCrossfader.store   (data[0x0b4], std::memory_order_relaxed);
            mixerFaderCurve.store   (data[0x0b5], std::memory_order_relaxed);
            mixerXfCurve.store      (data[0x0b6], std::memory_order_relaxed);
            mixerMasterFader.store  (data[0x0b7], std::memory_order_relaxed);
            mixerMasterCue.store    (data[0x0b9], std::memory_order_relaxed);
            mixerMasterCueB.store   (data[0x0ba], std::memory_order_relaxed);  // A9/V10 CUE B (0 on 900NXS2)
            mixerIsolatorOn.store   (data[0x0bb], std::memory_order_relaxed);  // A9/V10 (0 on 900NXS2)
            mixerIsolatorHi.store   (data[0x0bc], std::memory_order_relaxed);  // V10 only
            mixerIsolatorMid.store  (data[0x0bd], std::memory_order_relaxed);  // V10 only
            mixerIsolatorLo.store   (data[0x0be], std::memory_order_relaxed);  // V10 only
            mixerBooth.store        (data[0x0bf], std::memory_order_relaxed);
            mixerBoothEqHi.store    (data[0x0c0], std::memory_order_relaxed);  // A9/V10 (0 on 900NXS2)
            mixerBoothEqLo.store    (data[0x0c1], std::memory_order_relaxed);  // A9/V10 (0 on 900NXS2)
        }

        // --- Headphones ---
        if (0x0e5 < len)
        {
            mixerHpCueLink.store    (data[0x0c4], std::memory_order_relaxed);
            mixerHpMixing.store     (data[0x0e3], std::memory_order_relaxed);
            mixerHpLevel.store      (data[0x0e4], std::memory_order_relaxed);
            mixerBoothEq.store   (data[0x0e5], std::memory_order_relaxed);  // Booth EQ (A9 and V10; 0 on 900NXS2)
        }
        if (0x0e7 < len)  // HP B (A9/V10; 0 on 900NXS2)
        {
            mixerHpCueLinkB.store   (data[0x0c5], std::memory_order_relaxed);
            mixerHpMixingB.store    (data[0x0e6], std::memory_order_relaxed);
            mixerHpLevelB.store     (data[0x0e7], std::memory_order_relaxed);
        }

        // --- Beat FX ---
        if (0x0cf < len)
        {
            mixerFxFreqLo.store     (data[0x0c6], std::memory_order_relaxed);
            mixerFxFreqMid.store    (data[0x0c7], std::memory_order_relaxed);
            mixerFxFreqHi.store     (data[0x0c8], std::memory_order_relaxed);
            mixerBeatFxSel.store    (data[0x0c9], std::memory_order_relaxed);
            mixerColorFxAssign.store(data[0x0ca], std::memory_order_relaxed);
            mixerBeatFxLevel.store  (data[0x0cb], std::memory_order_relaxed);
            mixerBeatFxOn.store     (data[0x0cc], std::memory_order_relaxed);
            mixerBeatFxAssign.store (data[0x0ca], std::memory_order_relaxed);  // 900NXS2: same as 0x0ce; A9/V10: 0x0ce is Multi I/O
            mixerSendReturn.store   (data[0x0cf], std::memory_order_relaxed);
            mixerMultiIoSelect.store(data[0x0ce], std::memory_order_relaxed);  // A9/V10: Multi I/O (900NXS2: mirrors 0x0ca)
            mixerMultiIoLevel.store (data[0x0cf], std::memory_order_relaxed);  // A9/V10: Multi I/O (900NXS2: Send/Return)
        }

        // --- Color FX / Sends ---
        if (0x0e2 < len)
        {
            mixerColorFxSel.store   (data[0x0db], std::memory_order_relaxed);  // V10: Send Built-IN select
            mixerSendExt1.store     (data[0x0dc], std::memory_order_relaxed);  // A9/V10 (0 on 900NXS2)
            mixerSendExt2.store     (data[0x0dd], std::memory_order_relaxed);  // A9/V10 (0 on 900NXS2)
            mixerColorFxParam.store (data[0x0e2], std::memory_order_relaxed);
        }

        // --- Master Mix (V10 only; 0 on 900NXS2) ---
        if (0x0e2 < len)
        {
            mixerMasterMixOn.store       (data[0x0de], std::memory_order_relaxed);
            mixerMasterMixSize.store     (data[0x0df], std::memory_order_relaxed);
            mixerMasterMixTime.store     (data[0x0e0], std::memory_order_relaxed);
            mixerMasterMixTone.store     (data[0x0e1], std::memory_order_relaxed);
            mixerMasterMixLevel.store    (data[0x0e2], std::memory_order_relaxed);  // shares offset with ColorFxParam
        }

        // --- Mic ---
        if (0x0d7 < len)
        {
            mixerMicEqHi.store      (data[0x0d6], std::memory_order_relaxed);
            mixerMicEqLo.store      (data[0x0d7], std::memory_order_relaxed);
        }

        // --- Filter (V10 only; 0 on 900NXS2) ---
        if (0x0da < len)
        {
            mixerFilterLPF.store    (data[0x0d8], std::memory_order_relaxed);
            mixerFilterHPF.store    (data[0x0d9], std::memory_order_relaxed);
            mixerFilterReso.store   (data[0x0da], std::memory_order_relaxed);
        }

        hasMixerData.store(true, std::memory_order_relaxed);
        lastMixerPacketTime.store(juce::Time::getMillisecondCounterHiRes(), std::memory_order_relaxed);
        pktCountMixer.fetch_add(1, std::memory_order_relaxed);
    }

    //==========================================================================
    // DJM VU meter handler (type 0x58, 524+ bytes, port 50001)
    //
    // Contains blocks of 15 u16 big-endian peak-level segments.
    // Each block represents one VU meter strip on the DJM.
    // Range: 0 = silence, 32767 = clip (+15 dB).
    // Stride: 0x3c (60 bytes) per block.  15 u16 = 30 bytes data + 30 pad.
    //
    // 4-channel DJMs (900NXS2, A9, etc) -- confirmed from capture:
    //   Block 0 [0x02c]: CH1    Block 1 [0x068]: CH2
    //   Block 2 [0x0a4]: CH3    Block 3 [0x0e0]: CH4
    //   Block 4 [0x11c]: Master L    Block 5 [0x158]: Master R
    //
    // 6-channel DJMs (V10) -- confirmed from capture (Vumetro_5.pcapng):
    //   Block 0 [0x02c]: CH1    Block 1 [0x068]: CH2
    //   Block 2 [0x0a4]: CH3    Block 3 [0x0e0]: CH4
    //   Block 4 [0x11c]: Master L    Block 5 [0x158]: Master R  (same as 4-ch)
    //   Block 6 [0x194]: CH5    Block 7 [0x1d0]: CH6
    //==========================================================================
    void handleVuMeterPacket(const uint8_t* data, int len)
    {
        if (len < 0x176) return;  // need up to Master R last segment (4-ch layout)

        // 4-channel layout: 4 channels + master L/R = 6 blocks
        // Map to our slot indices: 0-3=CH1-4, kVuMasterL=MasterL, kVuMasterR=MasterR
        static constexpr int kVu4chOffsets[6] = {
            0x02c, 0x068, 0x0a4, 0x0e0, 0x11c, 0x158
        };
        static constexpr int kVu4chSlots[6] = {
            0, 1, 2, 3, ProDJLink::kVuMasterL, ProDJLink::kVuMasterR
        };

        // 6-channel layout (V10): Master L/R stay at 4-ch offsets,
        // CH5/CH6 appended AFTER Master R.  Confirmed from Wireshark
        // capture (Vumetro_5.pcapng): audio on CH5 only, 0x194 has
        // highest peak (pre-master), 0x11c/0x158 show attenuated master.
        static constexpr int kVu6chOffsets[8] = {
            0x02c, 0x068, 0x0a4, 0x0e0,                         // CH1-CH4
            0x11c, 0x158,                                        // Master L/R (same as 4-ch)
            0x194, 0x1d0                                         // CH5, CH6
        };
        static constexpr int kVu6chSlots[8] = {
            0, 1, 2, 3,
            ProDJLink::kVuMasterL, ProDJLink::kVuMasterR,
            4, 5
        };

        bool is6ch = (getMixerChannelCount() > 4);
        int numBlocks       = is6ch ? 8 : 6;
        const int* offsets  = is6ch ? kVu6chOffsets : kVu4chOffsets;
        const int* slots    = is6ch ? kVu6chSlots   : kVu4chSlots;

        for (int i = 0; i < numBlocks; ++i)
        {
            int base = offsets[i];
            int slot = slots[i];
            if (base + 30 > len) break;  // 15 segments x 2 bytes
            uint16_t peak = 0;
            for (int seg = 0; seg < 15; ++seg)
            {
                uint16_t val = ProDJLink::readU16BE(data + base + seg * 2);
                if (val > peak) peak = val;
            }
            vuPeak[slot].store(peak, std::memory_order_relaxed);
        }

        // Copy raw segment data under lock for UI spectrum display
        {
            const juce::SpinLock::ScopedLockType sl(vuDataLock);
            for (int i = 0; i < numBlocks; ++i)
            {
                int base = offsets[i];
                int slot = slots[i];
                if (base + 30 > len) break;
                for (int seg = 0; seg < 15; ++seg)
                    vuSegments[slot][seg] = ProDJLink::readU16BE(data + base + seg * 2);
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
        // Per-channel on-air flags at 4-byte stride starting at 0x27
        // CH1=0x27, CH2=0x2b, CH3=0x2f, CH4=0x33, CH5=0x37, CH6=0x3b
        static constexpr int kOnAirOffsets[6] = { 0x27, 0x2b, 0x2f, 0x33, 0x37, 0x3b };

        bool chOnAir[6] = { false };
        for (int ch = 0; ch < 6; ++ch)
        {
            if (kOnAirOffsets[ch] < len)
                chOnAir[ch] = (data[kOnAirOffsets[ch]] != 0x00);
        }

        // Propagate on-air state to matching player slots.
        // Channel number == player number for standard setups.
        for (int i = 0; i < ProDJLink::kMaxPlayers; ++i)
        {
            if (!players[i].discovered.load(std::memory_order_relaxed)) continue;
            uint8_t pn = players[i].playerNumber.load(std::memory_order_relaxed);
            if (pn < 1 || pn > 6) continue;
            players[i].isOnAir.store(chOnAir[pn - 1], std::memory_order_relaxed);
        }

        pktCountDJMStatus.fetch_add(1, std::memory_order_relaxed);

        DBG("ProDJLink: 0x29 on-air ch1=" << (int)chOnAir[0] << " ch2=" << (int)chOnAir[1]
            << " ch3=" << (int)chOnAir[2] << " ch4=" << (int)chOnAir[3]);
    }

    // Player state array -- indexed 0-5 for players 1-6
    std::array<ProDJLinkPlayerState, ProDJLink::kMaxPlayers> players;

    // Sockets
    std::unique_ptr<juce::DatagramSocket> keepaliveSock;
    std::unique_ptr<juce::DatagramSocket> beatSock;
    std::unique_ptr<juce::DatagramSocket> statusSock;
    std::unique_ptr<juce::DatagramSocket> bridgeSock;   // ephemeral port for 0x57/0x55 (macOS compat)

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

    // DJM mixer state (from type 0x39 packets, 248+ bytes)
    // --- Per-channel arrays (indexed 0-5 for CH1-CH6) ---
    // 4-channel DJMs (900NXS2, A9, etc): only indices 0-3 are populated.
    // 6-channel DJMs (V10, V10-LF): all 6 populated.
    std::atomic<uint8_t> mixerFader[6]     {{ 255 }, { 255 }, { 255 }, { 255 }, { 255 }, { 255 }};
    std::atomic<uint8_t> mixerTrim[6]      {{ 128 }, { 128 }, { 128 }, { 128 }, { 128 }, { 128 }};
    std::atomic<uint8_t> mixerComp[6]      {{ 0 }, { 0 }, { 0 }, { 0 }, { 0 }, { 0 }};           // V10 per-ch Compressor (0 on 900NXS2)
    std::atomic<uint8_t> mixerEqHi[6]      {{ 128 }, { 128 }, { 128 }, { 128 }, { 128 }, { 128 }};
    std::atomic<uint8_t> mixerEqMid[6]     {{ 128 }, { 128 }, { 128 }, { 128 }, { 128 }, { 128 }};
    std::atomic<uint8_t> mixerEqLoMid[6]   {{ 128 }, { 128 }, { 128 }, { 128 }, { 128 }, { 128 }};  // V10 Lo Mid (0 on 900NXS2)
    std::atomic<uint8_t> mixerEqLo[6]      {{ 128 }, { 128 }, { 128 }, { 128 }, { 128 }, { 128 }};
    std::atomic<uint8_t> mixerColor[6]     {{ 128 }, { 128 }, { 128 }, { 128 }, { 128 }, { 128 }};
    std::atomic<uint8_t> mixerSend[6]      {{ 0 }, { 0 }, { 0 }, { 0 }, { 0 }, { 0 }};         // V10 per-ch Send (0 on 900NXS2)
    std::atomic<uint8_t> mixerCueBtn[6]    {{ 0 }, { 0 }, { 0 }, { 0 }, { 0 }, { 0 }};
    std::atomic<uint8_t> mixerCueBtnB[6]   {{ 0 }, { 0 }, { 0 }, { 0 }, { 0 }, { 0 }};           // A9/V10 CUE B (0 on 900NXS2)
    std::atomic<uint8_t> mixerInputSrc[6]  {{ 3 }, { 3 }, { 3 }, { 3 }, { 3 }, { 3 }};       // per-channel input selector (see offset map for per-model enum)
    std::atomic<uint8_t> mixerXfAssign[6]  {{ 0 }, { 0 }, { 0 }, { 0 }, { 0 }, { 0 }};       // 0=THRU,1=A,2=B
    // --- Global / Master ---
    std::atomic<uint8_t> mixerCrossfader   { 128 };   // 0=A, 255=B
    std::atomic<uint8_t> mixerMasterFader  { 255 };
    std::atomic<uint8_t> mixerMasterCue    { 0 };
    std::atomic<uint8_t> mixerMasterCueB   { 0 };     // A9/V10 CUE B (0 on 900NXS2)
    // --- Isolator (V10 only) ---
    std::atomic<uint8_t> mixerIsolatorOn   { 0 };     // 0/1
    std::atomic<uint8_t> mixerIsolatorHi   { 128 };   // 0-255, 128=center
    std::atomic<uint8_t> mixerIsolatorMid  { 128 };   // 0-255, 128=center
    std::atomic<uint8_t> mixerIsolatorLo   { 128 };   // 0-255, 128=center
    std::atomic<uint8_t> mixerFaderCurve   { 1 };     // 0/1/2
    std::atomic<uint8_t> mixerXfCurve      { 1 };     // 0/1/2
    std::atomic<uint8_t> mixerBooth        { 0 };     // 0-255
    std::atomic<uint8_t> mixerBoothEqHi   { 128 };   // 0-255, 128=center (A9/V10)
    std::atomic<uint8_t> mixerBoothEqLo   { 128 };   // 0-255, 128=center (A9/V10)
    // --- Headphones ---
    std::atomic<uint8_t> mixerHpCueLink    { 0 };
    std::atomic<uint8_t> mixerHpMixing     { 0 };     // 0=CUE, 255=Master
    std::atomic<uint8_t> mixerHpLevel      { 0 };
    std::atomic<uint8_t> mixerBoothEq  { 0 };     // Booth EQ (A9 and V10; 0 on 900NXS2)
    // --- Headphones B (V10 only) ---
    std::atomic<uint8_t> mixerHpCueLinkB   { 0 };
    std::atomic<uint8_t> mixerHpMixingB    { 0 };     // 0=CUE, 255=Master
    std::atomic<uint8_t> mixerHpLevelB     { 0 };
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
    // --- Multi I/O (A9/V10; on 900NXS2 these offsets are Beat FX Assign / Send Return) ---
    std::atomic<uint8_t> mixerMultiIoSelect { 0 };    // 0=Mic,1-6=CH1-CH6,7=Master
    std::atomic<uint8_t> mixerMultiIoLevel  { 0 };    // 0-255
    // --- Color FX ---
    std::atomic<uint8_t> mixerColorFxSel   { 255 };   // 255=OFF, 0-5 (900NXS2) / Send Built-IN (V10)
    std::atomic<uint8_t> mixerSendExt1     { 0 };     // V10 Send Ext1 On/Off
    std::atomic<uint8_t> mixerSendExt2     { 0 };     // V10 Send Ext2 On/Off
    std::atomic<uint8_t> mixerColorFxParam { 128 };
    // --- Master Mix (V10 only) ---
    std::atomic<uint8_t> mixerMasterMixOn    { 0 };    // 0/1
    std::atomic<uint8_t> mixerMasterMixSize  { 0 };    // 0-255 (Size/Feedback)
    std::atomic<uint8_t> mixerMasterMixTime  { 0 };    // 0-255
    std::atomic<uint8_t> mixerMasterMixTone  { 0 };    // 0-255
    std::atomic<uint8_t> mixerMasterMixLevel { 0 };    // 0-255 (shares 0x0e2 with ColorFxParam)
    // --- Mic ---
    std::atomic<uint8_t> mixerMicEqHi      { 128 };
    std::atomic<uint8_t> mixerMicEqLo      { 128 };
    // --- Filter (V10 only) ---
    std::atomic<uint8_t> mixerFilterLPF    { 0 };     // 0/1
    std::atomic<uint8_t> mixerFilterHPF    { 0 };     // 0/1
    std::atomic<uint8_t> mixerFilterReso   { 0 };     // 0-255
    // --- Flag ---
    std::atomic<bool>    hasMixerData      { false };
    std::atomic<double>  lastMixerPacketTime { 0.0 };  // for staleness detection

    // VU meter data (from type 0x58 packets, 524+ bytes)
    // Indices 0-5 = CH1-CH6 (4-ch DJMs only populate 0-3)
    // Index 6 = Master L, Index 7 = Master R (ProDJLink::kVuMasterL/R)
    std::atomic<uint16_t> vuPeak[8]       {{ 0 }, { 0 }, { 0 }, { 0 }, { 0 }, { 0 }, { 0 }, { 0 }};
    mutable juce::SpinLock vuDataLock;
    uint16_t              vuSegments[8][15] {};  // protected by vuDataLock
    std::atomic<bool>     hasVuData        { false };
    std::atomic<double>   lastVuPacketTime { 0.0 };   // for staleness detection
    std::atomic<uint32_t> pktCountVU       { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ProDJLinkInput)
};
