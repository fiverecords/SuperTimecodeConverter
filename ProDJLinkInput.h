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
//   - Bridge join sequence: hello 0x0A (player=5) -> IP claim 0x02 (player=5)
//   - Two keepalives sent in parallel:
//     1) 54B BROADCAST (player=0xF9) -- DJM discovers bridge, activates fader delivery
//     2) 95B UNICAST to each CDJ (player=5, PIONEER DJ CORP strings) -- CDJ registers
//        us as a valid peer and sends AbsPos/Status unicast data
//   - The DJM NEVER receives the 95B (it's unicast to CDJ IPs only)
//     -> DJM sees single identity (0xF9) -> faders work
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
    static constexpr int kBridgeSubPort = 50006;  // real bridge's 0x57/0x55 source port (Prueba_prodjlink_bridge.pcapng)

    // Announcement-port packet types (byte 10) -- received on port 50000.
    //
    // There are two overlapping type-number namespaces on this port:
    //
    //   - Device lifecycle packets, documented in the published Pro DJ Link
    //     protocol analysis: 0x00 / 0x02 / 0x04 / 0x05 / 0x08 / 0x0a.
    //   - The standard keepalive (0x06) that every device broadcasts ~1 Hz
    //     once it has finished claiming a number.
    //
    // Older STC builds treated 0x02 as an "IP keepalive" and read a player
    // number out of it.  That was an incorrect classification: on port 50000,
    // type 0x02 is a DEVICE_NUMBER_CLAIM_STAGE_2 packet from a CDJ that is
    // still negotiating its number -- the byte at offset 0x2e is the number
    // it is *trying to claim*, not a stable assignment.  Registering the
    // device as "discovered" from that packet made us show the player as
    // online for the ~1 s claim window before the real keepalive arrived.
    //
    // We now only register devices from the 0x06 keepalive, and treat the
    // other types as announcement-lifecycle events used for diagnostics.
    static constexpr uint8_t kKeepAliveTypeClaimStage1 = 0x00;  // CDJ starting claim, bursts at 300 ms
    static constexpr uint8_t kKeepAliveTypeClaimStage2 = 0x02;  // claim-byte at offset 0x2e
    static constexpr uint8_t kKeepAliveTypeClaimStage3 = 0x04;  // claim-byte at offset 0x24
    static constexpr uint8_t kKeepAliveTypeStatus      = 0x06;  // standard 1 Hz keepalive
    static constexpr uint8_t kKeepAliveTypeInUse       = 0x08;  // device-number-in-use defense
    static constexpr uint8_t kKeepAliveTypeHello       = 0x0a;  // initial Hello

    // Legacy alias -- older code referenced "kKeepAliveTypeIP" believing 0x02
    // was an IP-style keepalive.  Kept as an alias to avoid breaking other TUs
    // that may still use it, but nothing new should reference this name.
    static constexpr uint8_t kKeepAliveTypeIP     = kKeepAliveTypeClaimStage2;

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
    static constexpr double  kAutoListenMs      = 3500.0; // AUTO profile: listen-before-announce window (>= 1 full peer keepalive cycle)
    static constexpr double  kBridgeSubInterval = 1.0;   // seconds between 0x57 re-subscriptions (real bridge: exactly 1.0s, Prueba_prodjlink_bridge.pcapng)

    // Bridge subscription
    static constexpr uint8_t kBridgeSubType     = 0x57;  // mixer subscribe packet type

    //==========================================================================
    // Bridge identity profiles
    //==========================================================================
    // THREE captures of real, working bridges show THREE different values for
    // the two "identity" bytes of the 54B keepalive (0x24 = device number,
    // 0x30) -- everything else in the packet is byte-identical:
    //
    //   F9 profile: 0x24=0xF9, 0x30=0x04 -- Bridge_Original.pcapng (A9 rig).
    //   C0 profile: 0x24=0xC0, 0x30=0x03 -- 2000nxs2_PDL_bridge.pcapng
    //               (2x CDJ-2000NXS2, no DJM).
    //   E4 profile: 0x24=0xE4, 0x30=0x05 -- Prueba_prodjlink_bridge.pcapng
    //               (3x CDJ-3000 + DJM-A9).  On that rig the A9 unicasts
    //               0x39 faders 96ms after this keepalive; the SAME A9 sent
    //               nothing in 7+ minutes to STC 1.9.9's 0xF9/0x04
    //               (Prueba_stc.pcapng) -- which falsifies the old claim
    //               that the A9 "requires 0xF9".
    //
    // Conclusion: the device number is DYNAMIC (the bridge picks a value in
    // the high range per session), so peers cannot be whitelisting specific
    // values.  Byte 0x30 varies with the rig; in the two fully-enumerable
    // captures it equals the total device count including the bridge itself
    // (NXS2 rig: 2 CDJs + bridge = 3; fresh rig: 3 CDJs + A9 + bridge = 5).
    // "Count" is a hypothesis, not proven causation -- hence AUTO below is
    // an experimental profile, and the three observed pairs remain available
    // as-captured for controlled A/B.  Pairs are never mixed.
    //
    // Field A/B on the A9 + 3x3000 rig (beta16): E4 unlocked the DJM; F9,
    // C0 and the then-per-tick AUTO did not.  AUTO's failure despite
    // converging to 0xE4/0x05 shows the DJM judges the FIRST keepalive it
    // sees and does not re-evaluate -- AUTO is listen-then-announce since
    // beta17 (see run()).
    //
    // REFINEMENT (Prueba_stc_beta.pcapng, the beta16 success capture): b30
    // is a network HIGH-WATER MARK, not a count.  The CDJ-3000s' own b30
    // moved from 0x04 to 0x05 after a bridge announcing 0x05 had been on
    // the wire (devices adopt the highest value seen), and the A9 accepted
    // STC's 0x05 on a 4-device network -- falsifying exact-count
    // acceptance.  Model fitting all sessions: acceptance requires
    // b30 >= current mark; AUTO emits max(mark, count incl. self), which
    // reproduces every accepted value on record (0x03, 0x05, 0x05).
    static constexpr int kBridgeIdentityF9   = 0;  // 0xF9 / 0x04 (default, shipping behaviour)
    static constexpr int kBridgeIdentityC0   = 1;  // 0xC0 / 0x03 (NXS2 reference)
    static constexpr int kBridgeIdentityE4   = 2;  // 0xE4 / 0x05 (A9 + 3x3000 reference)
    static constexpr int kBridgeIdentityAuto = 3;  // 0xE4 / b30 = max(network mark, count incl. self) (experimental)

    // 95B dbserver-keepalive scope (see sendDbServerKeepaliveToAll):
    static constexpr int kDbKeepaliveAll     = 0; // every discovered player (beta14 behaviour)
    static constexpr int kDbKeepalive3000    = 1; // CDJ-3000 models only (v1.9.10 behaviour)
    static constexpr int kDbKeepaliveOff     = 2; // never (reference-bridge behaviour)

    // NOTE (v1.9.11-beta15): the dual CDJ identity (B21=02, self-assigned
    // player 7-15, added in beta4) has been REMOVED.  It never ran against
    // hardware, its claim packets were malformed (sendCdjJoinHello had an
    // out-of-bounds stack write), and the reference capture proves a single
    // correctly-announced bridge identity is sufficient for the NXS2 to
    // stream status + abs position.  See STC_PRODJLINK_AUDIT.md section 6.

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
    // Status-based ("phantom") discovery scratch -- network thread only.
    // Counts consecutive status packets from a stable IP for a player that
    // never keepalives (multi-deck all-in-one units).  See handleStatusPacket.
    int  statusSeenCount = 0;
    char statusSeenIp[16] = {0};
    std::atomic<double>   absPositionTs  { 0.0 };  // timestamp of last abs position (for interpolation)

    // Diagnostic counters (v1.9.11-beta15) -- the single most diagnostic
    // numbers for the NXS2 identity question: a player streaming 0x0a and
    // 0x0b to us has accepted our bridge identity.  Displayed in the PDL
    // View toolbar so a tester screenshot answers the whole hypothesis.
    std::atomic<uint32_t> cntStatusPkts { 0 };     // 0x0a unicast status received
    std::atomic<uint32_t> cntAbsPosPkts { 0 };     // 0x0b abs position received

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
        statusSeenCount = 0;
        statusSeenIp[0] = '\0';
        absPositionTs.store(0.0, std::memory_order_relaxed);
        cntStatusPkts.store(0, std::memory_order_relaxed);
        cntAbsPosPkts.store(0, std::memory_order_relaxed);
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

        // Extra send-only socket for 0x57 subscribe / 0x55 notify.
        // The real bridge sends these from exactly UDP 50006 (confirmed in
        // Prueba_prodjlink_bridge.pcapng); STC previously bound this socket
        // to an ephemeral port, which is one of the observable deltas vs the
        // working reference on a DJM-A9.  Bind 50006 first; fall back to
        // ephemeral if it is taken (another DJ Link tool on the machine).
        // Non-fatal: if all binds fail, subscribe/notify go via beatSock.
        bridgeSock = std::make_unique<juce::DatagramSocket>(false);
        if (!bridgeSock->bindToPort(ProDJLink::kBridgeSubPort, bindIp)
            && !bridgeSock->bindToPort(ProDJLink::kBridgeSubPort))
        {
            DBG("ProDJLink: could not bind bridgeSock to UDP "
                << ProDJLink::kBridgeSubPort << " -- falling back to ephemeral");
            if (!bridgeSock->bindToPort(0, bindIp) && !bridgeSock->bindToPort(0))
            {
                DBG("ProDJLink: bridgeSock creation failed (non-fatal)");
                bridgeSock = nullptr;
            }
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
        djmOnAirLastMs.store(0, std::memory_order_relaxed);
    }

    //==========================================================================
    // Virtual CDJ configuration
    //==========================================================================
    int  getVCDJPlayerNumber() const    { return vCDJPlayerNumber; }

    /// Called when a player disappears from the network (10s keepalive timeout).
    /// Wire this to DbServerClient::invalidatePlayer() to close stale connections.
    std::function<void(const juce::String& playerIP)> onPlayerLost;
    void setVCDJPlayerNumber(int n)     { vCDJPlayerNumber = juce::jlimit(1, 127, n); }

    /// Bridge identity profile for the 54B keepalive (see constants above).
    /// kBridgeIdentityF9 / C0 / E4 = the three as-captured byte pairs;
    /// kBridgeIdentityAuto = dev 0xE4 + b30 computed as the discovered device
    /// count including ourselves (experimental "count" hypothesis).
    /// Applied on the next keepalive tick; for a clean protocol A/B the caller
    /// should stop() + start() so the join replays under the new identity and
    /// peers re-register us from scratch.
    void setBridgeIdentityProfile(int p)
    {
        bridgeIdentityProfile.store(juce::jlimit((int)ProDJLink::kBridgeIdentityF9,
                                                 (int)ProDJLink::kBridgeIdentityAuto, p),
                                    std::memory_order_release);
    }
    int getBridgeIdentityProfile() const { return bridgeIdentityProfile.load(std::memory_order_acquire); }
    /// The identity bytes last emitted on the wire (0x24 / 0x30 of the 54B).
    uint8_t getLastKeepaliveDev() const { return lastKaDev.load(std::memory_order_relaxed); }
    uint8_t getLastKeepaliveB30() const { return lastKaB30.load(std::memory_order_relaxed); }

    /// 95B dbserver-keepalive scope (see constants above).
    /// kDbKeepaliveAll = every discovered player (beta14 behaviour, default),
    /// kDbKeepalive3000 = CDJ-3000 models only (v1.9.10 behaviour),
    /// kDbKeepaliveOff = never (reference-bridge behaviour).
    /// Applied live on the next keepalive tick -- no restart required, which
    /// deliberately allows the "does status keep flowing once the 95B stops"
    /// experiment mid-session.
    void setDbKeepaliveMode(int m)
    {
        dbKeepaliveMode.store(juce::jlimit((int)ProDJLink::kDbKeepaliveAll,
                                           (int)ProDJLink::kDbKeepaliveOff, m),
                              std::memory_order_release);
    }
    int getDbKeepaliveMode() const { return dbKeepaliveMode.load(std::memory_order_acquire); }


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
    /// Abs position (0x0b) provides ms playhead directly -- CDJ-3000 always,
    /// NXS2 once it accepts our bridge identity (see handleBeatPacket).
    /// Fallback: position derived from beatCount x (60000/BPM) in status packets.
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

    /// True if the status-flag PLAYING bit is set (F byte, bit 0x40).
    ///
    /// This is the authoritative "is the motor turning the disc" signal per
    /// documented reference implementations.  The playState enum lags by up
    /// to ~500ms during vinyl-mode pause ramps (P still reports PLAYING while
    /// F has already cleared the bit), so this accessor is what source-active
    /// gating should use on non-CDJ-3000 models -- see TimecodeEngine below.
    bool isPlayingFlagSet(int playerNum) const
    {
        int idx = playerNum - 1;
        if (idx < 0 || idx >= ProDJLink::kMaxPlayers) return false;
        return players[idx].isPlaying.load(std::memory_order_relaxed);
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

    /// Diagnostic counters per player (v1.9.11-beta15): unicast status (0x0a)
    /// and abs position (0x0b) packets received.  Non-zero values mean the
    /// player has accepted our bridge identity -- the decisive evidence for
    /// the identity-profile A/B test.  Displayed in the PDL View toolbar.
    uint32_t getPlayerStatusCount(int playerNum) const
    {
        int idx = playerNum - 1;
        if (idx < 0 || idx >= ProDJLink::kMaxPlayers) return 0;
        return players[idx].cntStatusPkts.load(std::memory_order_relaxed);
    }
    uint32_t getPlayerAbsPosCount(int playerNum) const
    {
        int idx = playerNum - 1;
        if (idx < 0 || idx >= ProDJLink::kMaxPlayers) return 0;
        return players[idx].cntAbsPosPkts.load(std::memory_order_relaxed);
    }

    /// Suggest a player number for dbserver queries to NXS2.
    /// NXS2 dbserver only accepts player numbers 1-4 that are actually present
    /// on the network.  Returns a discovered player (1-4) that is not
    /// `excludePlayer`, or 0 if none found (query will fail).
    int suggestDbPlayerNumber(int excludePlayer) const
    {
        const char* srcIp = (excludePlayer >= 1 && excludePlayer <= ProDJLink::kMaxPlayers)
                                ? players[excludePlayer - 1].ipStr : "";

        // Prefer another discovered player (ideal: the CDJ knows it exists)
        // -- but NEVER one that shares the source player's IP.  Multi-deck
        // all-in-one units (XDJ-XZ) expose several player numbers on ONE
        // box; a dbserver query claiming to come from the box's own other
        // deck is silently ignored (audit 12.10: the XZ completed the
        // handshake, then never answered a 0x2002 metadata query sent with
        // context player 2 -- its own deck B).
        for (int pn = 1; pn <= 4; ++pn)
        {
            if (pn == excludePlayer) continue;
            int idx = pn - 1;
            if (!players[idx].discovered.load(std::memory_order_relaxed)) continue;
            if (srcIp[0] != '\0'
                && std::strncmp(players[idx].ipStr, srcIp, 15) == 0) continue;
            return pn;
        }
        // No usable discovered player.  Pick the highest number 4->1 that is
        // genuinely FREE (not discovered -- a discovered-but-skipped number
        // here would be a same-box deck, the exact collision to avoid).
        // NXS2 dbserver accepts queries from non-existent player numbers
        // 1-4 but rejects or ignores queries claiming to be FROM a number
        // the box itself owns.  Starting from 4 minimises collision with
        // real players (1 and 2 are the most common in a 2-deck setup).
        for (int pn = 4; pn >= 1; --pn)
        {
            if (pn == excludePlayer) continue;
            if (players[pn - 1].discovered.load(std::memory_order_relaxed)) continue;
            return pn;
        }
        // Pathological: players 1-4 all real devices.  Any non-source number
        // is equally (in)valid; keep the old behaviour.
        return (excludePlayer != 4) ? 4 : 3;
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

        // --- No join sequence (v1.9.11-beta16) ---
        // The fresh reference capture (Prueba_prodjlink_bridge.pcapng, real
        // bridge + DJM-A9 + 3x CDJ-3000) shows the bridge sending ZERO join
        // packets: no hello, no claims -- its very first packet is the 54B
        // keepalive, and the A9 starts unicasting 0x39 faders 96ms later.
        // STC's old join (2x hello 0x0a + 11x claim 0x02, all BROADCAST,
        // all claiming device 5) leaked a second identity to the DJM before
        // the 0xF9 keepalives began -- exactly the "two identities from one
        // IP/MAC" condition the 54B/95B split was designed to avoid -- and
        // added ~6.1s of blocking sleeps before the first keepalive.
        // (Bridge_Original.pcapng did show a hello+claim join, so bridge
        // versions differ; the version that demonstrably works with this
        // DJM-A9 sends none, and CDJs need none either.)

        autoIdentityB30.store(0, std::memory_order_release);
        maxPeerB30.store(0, std::memory_order_release);
        const double threadStartMs = juce::Time::getMillisecondCounterHiRes();

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
            // 1) 54B bridge keepalive BROADCAST (device number per the active
            //    bridge identity profile, see sendBridgeKeepalive)
            //    -> DJM discovers us as bridge -> activates fader (0x39) delivery
            //    -> This is the standard bridge broadcast keepalive
            //
            // 2) 95B dbserver keepalive UNICAST to each CDJ (player=5)
            //    -> CDJ registers us as a valid peer with PIONEER identification
            //    -> CDJ sends unicast Status + AbsPos data to our IP
            //    -> Scope controlled by dbKeepaliveMode; the reference bridge
            //       never sends it (see sendDbServerKeepaliveToAll)
            //
            // CRITICAL: The 95B MUST be unicast to CDJ IPs only -- NEVER broadcast.
            // If the DJM receives both player=5 (95B) and the bridge device
            // number (54B) from the same IP/MAC, it detects conflicting
            // identities and refuses faders.  By unicasting the 95B, the DJM
            // only ever sees the 54B broadcast.
            if ((now - lastKeepaliveSend) >= ProDJLink::kKeepaliveInterval * 1000.0)
            {
                // AUTO profile: listen-before-announce (v1.9.11-beta17).
                // Field result on the A9 + 3x CDJ-3000 rig: static E4 (0xE4/
                // 0x05) unlocked the DJM, but AUTO-as-shipped-in-beta16 did
                // NOT -- even though it converges to the same pair -- because
                // it computed b30 per tick and the first keepalives went out
                // with b30=0x02 before discovery had registered anyone.  The
                // DJM evidently judges a bridge on its first keepalive and
                // does not re-evaluate.  The real bridge never has this
                // problem: it observes the network before announcing (its
                // first packet in the reference capture arrives with the
                // correct count already).  So AUTO now stays silent for
                // kAutoListenMs (discovery keeps running below), computes
                // b30 once, freezes it for the session, and only then starts
                // announcing -- first impression correct, value stable, like
                // the reference (x9 identical keepalives in the capture).
                const bool autoProfile =
                    bridgeIdentityProfile.load(std::memory_order_acquire)
                        == ProDJLink::kBridgeIdentityAuto;
                bool readyToAnnounce = true;
                if (autoProfile
                    && autoIdentityB30.load(std::memory_order_acquire) == 0)
                {
                    if ((now - threadStartMs) < ProDJLink::kAutoListenMs)
                        readyToAnnounce = false;   // still listening
                    else
                    {
                        // b30 = max(network high-water mark, device count+1),
                        // clamped.  This reproduces every accepted value on
                        // record: NXS2 rig (mark ~0x02, count+1=3 -> 0x03),
                        // first A9 rig session (mark 0x04, count+1=5 -> 0x05),
                        // and the beta16 success capture (mark 0x05,
                        // count+1=4 -> 0x05, the exact value the A9
                        // accepted on a 4-device network -- count+1 alone
                        // would have emitted a rejected 0x04).
                        const int mark  = (int) maxPeerB30.load(std::memory_order_relaxed);
                        const int count = countNetworkDevices() + 1;
                        const uint8_t b30 = (uint8_t) juce::jlimit(
                            2, 7, juce::jmax(mark, count));
                        autoIdentityB30.store(b30, std::memory_order_release);
                        DBG("ProDJLink: AUTO identity computed -- b30=0x"
                            << juce::String::toHexString((int) b30)
                            << " (mark=0x" << juce::String::toHexString(mark)
                            << ", peers+self=" << count << ")");
                    }
                }

                if (readyToAnnounce)
                {
                    sendBridgeKeepalive();           // 54B BROADCAST -> DJM faders
                    sendDbServerKeepaliveToAll();     // 95B UNICAST to CDJs -> CDJ status data
                    lastKeepaliveSend = now;
                    if (firstKeepaliveSent == 0.0)
                        firstKeepaliveSent = now;
                }
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
                        handleBeatPacket(buf, n, sender);
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
                        handleStatusPacket(buf, n, sender);
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
    //   1) sendBridgeKeepalive()       -- 54B BROADCAST (device number from the
    //      active bridge identity profile: 0xF9 or 0xC0, see that function)
    //      -> DJM discovers us as bridge -> activates fader (0x39) delivery
    //      -> Standard bridge broadcast keepalive
    //
    //   2) sendDbServerKeepaliveToAll() -- 95B UNICAST to each CDJ (player=5)
    //      -> CDJ validates PIONEER DJ CORP / PRODJLINK BRIDGE strings
    //      -> CDJ registers us as a valid peer -> sends Status + AbsPos unicast
    //      -> Scope controlled by dbKeepaliveMode (all / 3000-only / off).
    //         The reference bridge sends NO 95B at all -- see the function.
    //
    // The DJM NEVER sees the 95B packet (it's sent unicast to CDJ IPs only).
    // From the DJM's perspective, we have a single identity: the 54B device
    // number of the active profile.
    //
    // History (device number was 0xC1 in the v1.5 era):
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
    // The DJM must only see the 54B bridge keepalive (profile device number).
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
        // The 95 B unicast keepalive carries the "PIONEER DJ CORP" /
        // "PRODJLINK BRIDGE" identification strings starting at byte 54
        // and exists to make Pioneer CDJs accept the sender as a Pro DJ Link
        // Bridge peer.  Without it the CDJ-2000NXS2 sees STC's standard 54 B
        // bridge keepalive but never adds STC to the unicast status
        // destination list, so STC receives beat packets (which are
        // broadcast and contain pitch + nominal BPM) but no status packets
        // (which are unicast and contain trackId, play state, abspos, etc.).
        //
        // ============================================================
        //  v1.9.10-beta history (reverted in v1.9.11, RE-READ in beta15)
        // ============================================================
        // v1.9.10-beta suppressed this 95 B unicast for non-CDJ-3000
        // hardware on the theory that the captures of the official
        // PRO DJ LINK Bridge talking to CDJ-2000NXS2s did not show 95 B
        // unicasts, so the 95 B was deemed unnecessary.  Field testing
        // (Joren2087 traces 1.9.10_no_waveform_no_metadata.pcapng and
        // 1.9.10-no-waveform-pitch-fine-bpm-not-no-metadata-player1.pcapng)
        // showed that with the 95 B suppressed STC receives zero unicast
        // packets from the CDJ-2000NXS2 -- no status, no abspos -- only
        // the broadcast beat stream.  The conclusion drawn at the time was
        // "the 95 B is our only working subscription mechanism for the
        // NXS2", and v1.9.11 reverted the suppression.
        //
        // The reference capture (2000nxs2_PDL_bridge.pcapng, rev 3 audit)
        // forces a re-read of that result: the real bridge sends ZERO 95 B
        // packets and the NXS2 streams it status + abs position anyway,
        // within 10ms of the first 54 B keepalive.  So the correct reading
        // of v1.9.10 is not "the 95 B is necessary" -- it is "STC's 54 B
        // bridge identity is not accepted by the NXS2, and the 95 B was
        // papering over that".  The 95 B is also the trigger for the TCP
        // 12523 SYN storm (the NXS2 believes the sender runs a dbserver and
        // probes for it at ~25 SYN/s, exhausting its port table -> freeze).
        // See STC_PRODJLINK_AUDIT.md sections 0-4.
        //
        // Hence dbKeepaliveMode: the 95 B must NOT be removed before the
        // 54 B identity is fixed (that reproduces v1.9.10 exactly), so both
        // knobs ship together and the tester flips them in one session:
        //   kDbKeepaliveAll  (default) -- every discovered player (beta14)
        //   kDbKeepalive3000           -- CDJ-3000 models only   (v1.9.10)
        //   kDbKeepaliveOff            -- never                  (reference bridge)
        //
        // The TCP listener on port 12523 in DbServerClient (answers
        // "no dbserver here", port = 0) stays as a belt-and-braces
        // mitigation regardless of mode.  See dbPortListenerLoop().
        const int mode = dbKeepaliveMode.load(std::memory_order_acquire);
        if (mode == ProDJLink::kDbKeepaliveOff)
            return;

        // Multi-deck units expose several player numbers on one IP
        // (XDJ-XZ: players 1+2, one box) -- send each physical box ONE 95B
        // per tick, not one per deck.
        const char* sentIps[ProDJLink::kMaxPlayers] = {};
        int numSent = 0;
        for (int i = 0; i < ProDJLink::kMaxPlayers; ++i)
        {
            if (!players[i].discovered.load(std::memory_order_relaxed)) continue;

            // 3000-only mode: model[] is written once by this same network
            // thread when the player is first registered, so reading it
            // here is race-free.
            if (mode == ProDJLink::kDbKeepalive3000
                && std::strstr(players[i].model, "3000") == nullptr)
                continue;

            bool dup = false;
            for (int k = 0; k < numSent; ++k)
                if (std::strncmp(sentIps[k], players[i].ipStr, 15) == 0) { dup = true; break; }
            if (dup) continue;

            juce::String ip(players[i].ipStr);
            if (ip.isNotEmpty())
            {
                sendDbServerKeepalive(ip);
                sentIps[numSent++] = players[i].ipStr;
            }
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
    /// Count of PDL devices currently visible on the wire: discovered CDJ
    /// players + registered DJMs.  Used by the AUTO identity profile
    /// (b30 = this + 1 for ourselves).  Rekordbox instances are not tracked
    /// by STC and therefore not counted -- a known limitation of the
    /// experimental profile, documented in the audit addendum.
    int countNetworkDevices()
    {
        int n = 0;
        for (int i = 0; i < ProDJLink::kMaxPlayers; ++i)
            if (players[i].discovered.load(std::memory_order_relaxed)) ++n;
        {
            const juce::ScopedLock sl(djmIpLock);
            n += (int) djmIps.size();
        }
        return n;
    }

    void sendBridgeKeepalive()
    {
        if (!keepaliveSock) return;
        uint8_t pkt[54];
        std::memset(pkt, 0, sizeof(pkt));
        std::memcpy(pkt, ProDJLink::kMagic, ProDJLink::kMagicLen);
        pkt[0x0a] = 0x06;
        std::strncpy(reinterpret_cast<char*>(pkt + 0x0c), "TCS-SHOWKONTROL", 19);
        pkt[0x20] = 0x01;  pkt[0x21] = 0x01;  pkt[0x23] = 0x36;
        // Bytes 0x24 (device number) and 0x30 are the ONLY two bytes where
        // real-bridge captures vary -- three captures, three pairs -- and the
        // prime suspect for peers refusing STC as a bridge.  See the profile
        // constants in namespace ProDJLink for the full provenance table and
        // the falsification of the old "A9 requires 0xF9" claim.
        // AUTO implements the "count" hypothesis: b30 = discovered devices
        // (CDJs + DJMs) + 1 for ourselves, which matches both fully
        // enumerable reference captures (3 on the 2-NXS2 rig, 5 on the
        // 3x3000+A9 rig).  Clamped [2,7] defensively.
        uint8_t dev = 0xF9, b30 = 0x04;
        switch (bridgeIdentityProfile.load(std::memory_order_acquire))
        {
            case ProDJLink::kBridgeIdentityC0:   dev = 0xC0; b30 = 0x03; break;
            case ProDJLink::kBridgeIdentityE4:   dev = 0xE4; b30 = 0x05; break;
            case ProDJLink::kBridgeIdentityAuto:
            {
                dev = 0xE4;
                // Use the session-frozen value computed after the listen
                // window (see run()); never recompute per tick -- the ramp
                // is exactly what failed in the beta16 field test.
                uint8_t frozen = autoIdentityB30.load(std::memory_order_acquire);
                if (frozen == 0)   // defensive: any path that announces
                {                  // before the gate computes it here, once
                    frozen = (uint8_t) juce::jlimit(2, 7,
                        juce::jmax((int) maxPeerB30.load(std::memory_order_relaxed),
                                   countNetworkDevices() + 1));
                    autoIdentityB30.store(frozen, std::memory_order_release);
                }
                b30 = frozen;
                break;
            }
            default: break;  // kBridgeIdentityF9
        }
        pkt[0x24] = dev;   pkt[0x25] = 0x00;
        std::memcpy(pkt + 0x26, ownMacBytes, 6);
        std::memcpy(pkt + 0x2c, ownIpBytes, 4);
        pkt[0x30] = b30;   pkt[0x34] = 0x05;  pkt[0x35] = 0x20;
        // Record the bytes actually emitted for the PDL View diagnostics line
        // (with AUTO the pair is computed, so screenshots must show reality).
        lastKaDev.store(dev, std::memory_order_relaxed);
        lastKaB30.store(b30, std::memory_order_relaxed);

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
    // NOTE (v1.9.11-beta16): the bridge join sequence (hello 0x0a x2 +
    // claim 0x02 x11, broadcast, claiming device 5) has been REMOVED.
    // The working reference (Prueba_prodjlink_bridge.pcapng) sends zero
    // join packets; see the note at the top of run().
    //==========================================================================

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

        // Send from bridgeSock (ephemeral port) if available, matching Pioneer Bridge
        // behavior (~port 50006). Fallback to statusSock if bridgeSock unavailable.
        // CRITICAL: do NOT send from both -- CDJ registers each source port as a
        // separate subscriber, doubling (or worse) the status traffic it sends back.
        auto* sock = bridgeSock ? bridgeSock.get() : statusSock.get();
        sock->write(cdjIp, ProDJLink::kStatusPort, pkt, sizeof(pkt));
    }

    void sendBridgeNotifyToAll()
    {
        // Send 0x55 to discovered CDJ players (not DJMs) -- but NOT to
        // CDJ-3000s: the reference bridge sends zero 0x55 on an all-3000 rig
        // (Prueba_prodjlink_bridge.pcapng), and in the paired STC capture the
        // 3000s ignored all 648 of ours (zero 0x56 replies).  The 0x55/0x56
        // exchange is an NXS2-era metadata mechanism (see audit section 5);
        // on 3000s STC gets metadata via the TCP dbserver instead.
        for (int i = 0; i < ProDJLink::kMaxPlayers; ++i)
        {
            if (!players[i].discovered.load(std::memory_order_relaxed)) continue;
            if (std::strstr(players[i].model, "3000") != nullptr) continue;
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
            // Mark that we have a DJM-sourced on-air signal so the CDJ
            // self-reported flag is suppressed in the status handler.
            djmOnAirLastMs.store(juce::Time::getMillisecondCounter(),
                                  std::memory_order_relaxed);
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
    // Triggers DJM VU meter (0x58->50001) delivery; fader (0x39->50002)
    // delivery is gated by the keepalive identity, not by this packet
    // (Prueba_prodjlink_bridge.pcapng: first 0x39 arrives 96ms after the
    // bridge's first keepalive, BEFORE its first 0x57; first 0x58 arrives
    // 10ms after the first 0x57).
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
        // Byte 33: the ONLY content byte where STC's subscribe differed from
        // the real bridge's (Prueba_prodjlink_bridge.pcapng, DJM-A9 rig,
        // x8 identical packets).  The reference sends 0x98; STC's previous
        // 0x87 (Windows) / 0xFE (macOS) produced total 0x58 silence from the
        // same DJM-A9 in the paired capture (Prueba_stc.pcapng, x216
        // subscribes, zero replies).  The DJM-900NXS2 also ignored 0x87
        // (BACKLOG) -- plausibly the same fix.  Single value, no platform
        // split: the reference is one product emitting one byte.
        pkt[33] = 0x98;
        pkt[34] = 0x00;
        pkt[35] = 0x04;  // payload length (bytes 34-35 = 0x0004 BE)
        pkt[36] = 0x01;  // subscribe = 1

        auto djmStr = juce::String(djmIp);

        // Send ONLY from bridgeSock (port 50006), matching the real Bridge
        // which subscribes from exactly UDP 50006 (confirmed in
        // Prueba_prodjlink_bridge.pcapng; STC previously used an ephemeral
        // port despite intending 50006).  NOT from beatSock (50001) -- that
        // port is for receiving beats, and the DJM may reject subscribes
        // from a port it also sends data to.  Fallback to beatSock only if
        // bridgeSock is unavailable.
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

        // Device-claim diagnostics -- these are the announcement-lifecycle
        // packets a CDJ broadcasts while it is negotiating a number on the
        // network.  We do not register devices from these (their number is
        // not yet stable); we only observe them, log them, and flag any
        // claim that collides with an identity we are using.
        //
        // A full beat-link-style implementation would respond with a
        // DEVICE_NUMBER_IN_USE (type 0x08) packet to defend our assigned
        // number.  That emission path is intentionally not implemented yet
        // -- it needs hardware validation that we do not have, and STC's
        // own identities (player 5 on the 95 B keepalive, 0xC1 / 0xF9 on
        // the 54 B bridge keepalive) do not collide with any legal CDJ
        // number (1-6), so in practice a collision would require another
        // bridge on the network or a misconfigured device.  For now we
        // surface the event in the log so operators can see it.
        if (type == ProDJLink::kKeepAliveTypeClaimStage1
            || type == ProDJLink::kKeepAliveTypeClaimStage2
            || type == ProDJLink::kKeepAliveTypeClaimStage3
            || type == ProDJLink::kKeepAliveTypeInUse
            || type == ProDJLink::kKeepAliveTypeHello)
        {
            // Claim byte offset varies by stage (per documented protocol):
            //   stage 2 (0x02): byte at offset 0x2e (46)
            //   stage 3 (0x04): byte at offset 0x24 (36)
            //   stage 1 (0x00): number not yet chosen
            uint8_t claimingNumber = 0;
            if (type == ProDJLink::kKeepAliveTypeClaimStage2 && len > 0x2e)
                claimingNumber = data[0x2e];
            else if (type == ProDJLink::kKeepAliveTypeClaimStage3 && len > 0x24)
                claimingNumber = data[0x24];

            // Collision here means another peer is claiming one of our bridge
            // identities (player 5 on the 95B, 0xC0/0xC1/0xF9 on the 54B).
            // Those are not standard CDJ numbers, so a collision is operator
            // error (two STC instances, or another bridge on the network) --
            // silent recovery is not the right behaviour, so we only surface
            // it in the log.
            const bool collidesWithUs =
                   claimingNumber != 0
                && (claimingNumber == uint8_t(vCDJPlayerNumber)
                    || claimingNumber == 0xC0
                    || claimingNumber == 0xC1
                    || claimingNumber == 0xF9);
            (void)collidesWithUs;

#if JUCE_DEBUG
            const char* stageName =
                  type == ProDJLink::kKeepAliveTypeClaimStage1 ? "CLAIM_STAGE_1"
                : type == ProDJLink::kKeepAliveTypeClaimStage2 ? "CLAIM_STAGE_2"
                : type == ProDJLink::kKeepAliveTypeClaimStage3 ? "CLAIM_STAGE_3"
                : type == ProDJLink::kKeepAliveTypeInUse       ? "IN_USE_DEFENSE"
                :                                                "HELLO";
            DBG("ProDJLink: announcement " << stageName
                << " from " << sender
                << (claimingNumber ? (" claiming player " + juce::String((int)claimingNumber)) : juce::String())
                << (collidesWithUs ? " -- COLLISION WITH OUR IDENTITY" : juce::String()));
#endif
            return;  // do not register the device yet; wait for keepalive (0x06)
        }

        // Everything below is for the stable keepalive (0x06).  Ignore any
        // other type byte we do not explicitly recognise (e.g. type 0x05).
        if (type != ProDJLink::kKeepAliveTypeStatus)
            return;

        pktCountKeepalive.fetch_add(1, std::memory_order_relaxed);

        // Track the network's b30 high-water mark (byte 0x30 of peer
        // keepalives).  Prueba_stc_beta.pcapng revealed that the CDJ-3000s'
        // own b30 is NOT a firmware constant: they broadcast 0x04 in the
        // early captures and 0x05 after a bridge announcing 0x05 had been on
        // the wire -- devices ADOPT the highest value seen.  The A9's
        // acceptance behaviour across the beta16 A/B (0x05 accepted at both
        // 5 and 4 devices; 0x04, 0x03 and a 0x02 ramp all rejected while the
        // mark was 0x05) fits "bridge b30 must be >= the current mark" and
        // falsifies exact-count acceptance.  The AUTO profile therefore needs
        // this mark, not just the device count.  Single writer (the network
        // thread), so load/compare/store is race-free.
        if (len >= 54 && sender != bindIp)
        {
            const uint8_t peerB30 = data[0x30];
            if (peerB30 > maxPeerB30.load(std::memory_order_relaxed)
                && peerB30 <= 0x10)   // sanity: reject garbage from malformed packets
                maxPeerB30.store(peerB30, std::memory_order_relaxed);
        }

        uint8_t pn = 0;
        if (len >= 54)
            pn = data[36];  // player_number in content

        // Identity-collision diagnostic: another peer keepalive-advertising
        // one of our bridge identities means a second STC instance or a real
        // bridge is on the network.  Surface it in the log; do not fight it.
        if (pn != 0
            && (pn == uint8_t(vCDJPlayerNumber) || pn == 0xC0 || pn == 0xC1 || pn == 0xF9)
            && sender != bindIp)
        {
#if JUCE_DEBUG
            DBG("ProDJLink: another device at " << sender
                << " is keepalive-advertising player number "
                << juce::String((int)pn)
                << " -- potential conflict with our identity");
#endif
        }

        if (pn == 0
            || pn == uint8_t(vCDJPlayerNumber)
            || pn == 0xC0 || pn == 0xC1 || pn == 0xF9)
            return;  // ignore self + bridge identities

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

    /// Find the player slot registered (via keepalive discovery) at the given
    /// source IP.  Returns the 0-based index, or -1 if no discovered player
    /// matches.  Network thread only: ipStr is written by this same thread at
    /// discovery time, so the read is race-free.
    int findPlayerIndexByIp(const juce::String& ip) const
    {
        for (int i = 0; i < ProDJLink::kMaxPlayers; ++i)
        {
            if (!players[i].discovered.load(std::memory_order_relaxed)) continue;
            if (ip == players[i].ipStr) return i;
        }
        return -1;
    }

    void handleBeatPacket(const uint8_t* data, int len, const juce::String& senderIp)
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

        // --- 0x0b Absolute Position: two wire formats share this type/size ---
        //
        // CDJ-3000 format (parsed further below):
        //   byte[33] = player number (1-6); playhead ms UNSIGNED at [40];
        //   track_len at [36]; bpm at [56].
        //
        // CDJ-2000NXS2 format (reference capture 2000nxs2_PDL_bridge.pcapng,
        // STC_PRODJLINK_AUDIT.md section 2 -- 60 B, ~55 Hz, unicast):
        //   bytes [32-33] = ROLLING COUNTER (not a player number!);
        //   [40-43] = int32 BE = NEGATED playback position in ms
        //             (position_ms = -int32_BE(pkt + 0x28));
        //   remaining fields are constants -- no track_len, no bpm, no pitch.
        //
        // Discriminator: int32_BE(data+40) < 0 -> NXS2 format.  A genuine
        // CDJ-3000 playhead would need to exceed 2^31 ms (~596 hours) to read
        // negative, which cannot happen; the NXS2 value observed in the
        // capture is always <= -48 while a track is loaded.
        //
        // Attribution MUST be by source IP for this format: byte[33] cycles
        // 0x00-0xFF, so routing it through the player-number gate below would
        // silently drop most packets and misattribute the rest to random
        // players (with a garbage position of ~4.29e9 ms from the unsigned
        // read).  This is why "just un-gating the NXS2" was never enough.
        if (type == ProDJLink::kBeatTypeAbsPosition && len >= 60)
        {
            int32_t rawPos = (int32_t) ProDJLink::readU32BE(data + 40);
            if (rawPos < 0)
            {
                int idx = findPlayerIndexByIp(senderIp);
                if (idx < 0)
                    return;  // 0x0b from an IP we have not registered via keepalive yet
                auto& p = players[idx];

                // CDJ-3000s ALSO emit a second 0x0b variant alongside the
                // primary one: byte[33] in 0x80-0xFF and a negative int32 at
                // [40] -- structurally indistinguishable from the NXS2 format.
                // beta14 dropped those via the player-number gate; plausibly
                // they are the legacy-encoded position kept for ecosystem
                // backwards compatibility, but until a capture proves the
                // negated value matches the primary packet's position, keep
                // dropping them for 3000 models.  Their primary 0x0b already
                // provides the position, and mis-parsing here would corrupt
                // the one path that currently works on real hardware.
                // (Verification recipe: in any CDJ-3000 capture, compare
                // -int32_BE(+40) of the high-byte[33] packets against
                // uint32_BE(+40) of the adjacent low-byte[33] packets.)
                if (std::strstr(p.model, "3000") != nullptr)
                    return;

                uint32_t playhead = (uint32_t)(-(int64_t)rawPos);

                p.hasAbsolutePosition.store(true, std::memory_order_relaxed);
                // Real abs position supersedes the beatCount*60000/BPM
                // derivation -- clear the flag so position-source reporting
                // and any beat-derived consumers treat this player exactly
                // like a CDJ-3000 from here on.  (TimecodeEngine's beat-grid
                // correction already keys on hasAbsolutePosition and becomes
                // a fallback automatically.)
                p.hasBeatDerivedPosition.store(false, std::memory_order_relaxed);

                double absNow = juce::Time::getMillisecondCounterHiRes();
                p.absPositionTs.store(absNow, std::memory_order_relaxed);
                p.lastPacketTime.store(absNow, std::memory_order_relaxed);

                // Reverse play detection -- same logic as the CDJ-3000 path.
                // NXS2 status packets never populate loopStartMs/loopEndMs
                // (extended loop fields are 0x200-byte 3000 status only), so
                // inLoop stays false here, which is the correct behaviour.
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

                // Deliberately NOT touched here: trackLenSec, bpmRaw, pitchRaw.
                // In this format those offsets hold constants, not data --
                // BPM/pitch keep coming from status (0x0a) and beat (0x28)
                // packets, track length from metadata as before.

                pktCountAbsPos.fetch_add(1, std::memory_order_relaxed);
                p.cntAbsPosPkts.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            // rawPos >= 0: fall through to the classic player-number-addressed
            // CDJ-3000 parse below.
        }

        uint8_t pn   = data[33];  // player_number

        if (pn == 0 || pn > ProDJLink::kMaxPlayers) return;
        int idx = pn - 1;
        auto& p = players[idx];

        if (type == ProDJLink::kBeatTypeAbsPosition && len >= 60)
        {
            // Source-IP cross-check (v1.9.11-beta15): a 0x0b claiming player N
            // must arrive from player N's registered IP.  This closes the
            // residual misattribution vector left by the sign discriminator
            // above: a rolling-counter-format 0x0b whose int32 at [40] reads
            // exactly 0 (e.g. an NXS2 with no track loaded -- the capture only
            // proves <= -48 WITH a track) would fall through to here, and if
            // its counter byte happens to land on 1-6 it would write the
            // constant at [36] (0x27ffff50 = 671M "seconds") into a random
            // player's trackLen plus garbage bpm.  A genuine CDJ-3000-format
            // 0x0b always comes from the player it names, so this check is
            // free for the working path.  (Pre-discovery packets are dropped
            // too: ipStr is empty until the player's first keepalive, but a
            // CDJ only starts unicasting 0x0b after it has registered US,
            // which is always after we have registered IT.)
            if (senderIp != p.ipStr)
                return;

            // Absolute Position packet (CDJ-3000 format)
            //
            // The CDJ-3000 sends PAIRS of 0x0b packets every ~30ms:
            //   1) Real position data: byte[33] = player number (1-6)
            //   2) Legacy/NXS2-format variant: byte[33] = rolling counter
            //      (0x80-0xFF range observed), negative int32 at [40].
            // The second variant is sign-discriminated and dropped for 3000
            // models in the block above (see the rationale there); packets
            // reaching this point have byte[33] = a valid player number.
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
            p.cntAbsPosPkts.fetch_add(1, std::memory_order_relaxed);
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

                // --- NXS2 beat-derived position advancement (FALLBACK) ---
                // Beat packets arrive at the exact moment of each beat (~2Hz at 120BPM).
                // While no abs position flows (identity not yet accepted by the
                // player, or 0x0b lost), advance beatCount by 1 and derive a fresh
                // playhead position.  This gives additional position anchors between
                // the 5Hz status packets, reducing the interpolation gap from ~200ms
                // to ~80ms at 120BPM.  The next status packet corrects beatCount to
                // the authoritative CDJ value (self-correcting).
                // NOTE (beta15): the NXS2 DOES send 0x0b (~55Hz) once it accepts
                // our bridge identity -- see the NXS2-format parser in this
                // function.  The hasAbsolutePosition gate above makes this whole
                // block dormant the moment real abs position arrives.
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

    void handleStatusPacket(const uint8_t* data, int len, const juce::String& sender)
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

        // --- Status-based ("phantom") discovery (v1.9.11-beta19) ---
        // Multi-deck all-in-one units announce ONE keepalive identity but
        // stream status and beat packets for TWO player numbers.  Field
        // case: the XDJ-XZ keepalives only as device 1 yet unicasts full
        // 0x0a status for players 1 AND 2 from the same IP
        // (Wireshark_Capture_XDJ_XZ_after_IP_Change.pcapng, audit 12.10).
        // Keepalive-only discovery left deck 2 permanently undiscovered:
        // dead PDL View tile, no metadata requests, no 95B, no timecode
        // routing.  Register a player from its status stream after 3
        // consecutive packets from a stable source IP -- the threshold
        // keeps one corrupted datagram from creating a ghost, and at ~5Hz
        // status cadence adds <1s of discovery latency.  Liveness is
        // already status-driven (lastPacketTime below), so no flapping.
        if (!p.discovered.load(std::memory_order_relaxed))
        {
            const auto senderStd = sender.toStdString();
            if (p.statusSeenCount > 0
                && std::strncmp(p.statusSeenIp, senderStd.c_str(), 15) != 0)
                p.statusSeenCount = 0;   // source IP changed: start over
            if (p.statusSeenCount == 0)
            {
                std::strncpy(p.statusSeenIp, senderStd.c_str(), 15);
                p.statusSeenIp[15] = '\0';
            }
            if (++p.statusSeenCount >= 3)
            {
                p.playerNumber.store(pn, std::memory_order_relaxed);
                // Device name travels in the status packet too (bytes 11-30,
                // one byte earlier than in keepalives: no subtype byte).
                std::memset(p.model, 0, sizeof(p.model));
                {
                    int copyLen = std::min(20, len - 11);
                    if (copyLen > 0)
                        std::memcpy(p.model, data + 11, copyLen);
                    p.model[20] = '\0';
                    for (int c = 0; c < 20 && p.model[c] != '\0'; ++c)
                        if (static_cast<unsigned char>(p.model[c]) > 127)
                            p.model[c] = '?';
                }
                std::strncpy(p.ipStr, senderStd.c_str(), 15);
                p.ipStr[15] = '\0';
                p.discovered.store(true, std::memory_order_release);
                DBG("ProDJLink: Player " << (int)pn << " STATUS-discovered ("
                    << p.model << ") at " << sender
                    << " -- multi-deck device, this deck sends no keepalive");
            }
        }

        pktCountStatus.fetch_add(1, std::memory_order_relaxed);
        p.cntStatusPkts.fetch_add(1, std::memory_order_relaxed);

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
            // The CDJ's own on-air bit (0x08) is gated by its UTILITY -> "On
            // Air Display" setting -- when the DJ disables that, the bit goes
            // to 0 even when the channel is genuinely on-air at the mixer.
            // The DJM's own report (0x03 broadcast or 0x29 unicast) is the
            // authoritative source.  If we've heard from the DJM in the last
            // 5 seconds we trust it exclusively and skip writing isOnAir from
            // the CDJ status, otherwise the two writers race and the flag
            // visibly flickers.
            const uint32_t now      = juce::Time::getMillisecondCounter();
            const uint32_t lastDjm  = djmOnAirLastMs.load(std::memory_order_relaxed);
            const bool     djmFresh = (lastDjm != 0) && ((now - lastDjm) < 5000u);
            if (! djmFresh)
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

            // --- Beat-derived playhead (FALLBACK while no abs position flows) ---
            // Historically documented as "NXS2 and older players don't send
            // 0x0b" -- disproved by the reference capture (rev 3 audit): the
            // NXS2 streams 0x0b at ~55Hz to a bridge identity it accepts.  It
            // just never sent it to *us* while our 54B identity was rejected.
            // This derivation (beatCount x 60000/BPM) therefore remains as the
            // fallback for players that have not (yet) accepted our identity.
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
                    juce::String lostIp(players[i].ipStr);
                    DBG("ProDJLink: Player " << (i + 1) << " timed out (" << lostIp << ")");
                    players[i].reset();

                    // Notify DbServerClient to close stale TCP connections
                    // and clear cached metadata for this player
                    if (onPlayerLost && lostIp.isNotEmpty())
                        onPlayerLost(lostIp);
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
        // Mark that we have a DJM-sourced on-air signal so the CDJ
        // self-reported flag is suppressed in the status handler.
        djmOnAirLastMs.store(juce::Time::getMillisecondCounter(),
                              std::memory_order_relaxed);

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

    // Bridge identity profile for the 54B keepalive + 95B keepalive scope.
    // See the ProDJLink namespace constants for rationale and provenance.
    // Both are user settings applied by MainComponent; they intentionally
    // survive reset() so a stop()/start() cycle keeps the operator's choice.
    std::atomic<int> bridgeIdentityProfile { ProDJLink::kBridgeIdentityF9 };
    std::atomic<int> dbKeepaliveMode       { ProDJLink::kDbKeepaliveAll };
    // Last identity bytes actually emitted in the 54B keepalive (for the PDL
    // View diagnostics line; with AUTO these are computed per tick).
    std::atomic<uint8_t> lastKaDev { 0xF9 };
    std::atomic<uint8_t> lastKaB30 { 0x04 };
    // AUTO profile: b30 computed ONCE per session after the listen window,
    // then frozen (0 = not yet computed).  Cleared at thread start so a
    // stop()/start() re-listens and re-announces with a fresh count.
    std::atomic<uint8_t> autoIdentityB30 { 0 };
    // Highest b30 observed in peer keepalives this session (the network's
    // "high-water mark").  See handleKeepalivePacket for provenance.
    std::atomic<uint8_t> maxPeerB30 { 0 };

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

    // Diagnostics
    std::atomic<uint32_t> pktCountKeepalive  { 0 };
    std::atomic<uint32_t> pktCountBeat       { 0 };
    std::atomic<uint32_t> pktCountAbsPos     { 0 };
    std::atomic<uint32_t> pktCountStatus     { 0 };
    std::atomic<uint32_t> pktCountMixer      { 0 };
    std::atomic<uint32_t> pktCountDJMStatus  { 0 };  // type 0x29 packets received
    // Timestamp (juce::Time::getMillisecondCounter) of the most recent on-air
    // update from a DJM-sourced packet (0x03 broadcast OR 0x29 unicast).
    // The CDJ status (0x0a) also reports an on-air bit, but that bit reflects
    // the CDJ's "On Air Display" setting in its own UTILITY menu -- when the
    // user switches that off, the CDJ's bit goes to 0 even though the channel
    // is genuinely on-air.  Two writers racing for the same isOnAir variable
    // produces visible flicker.  When this timestamp is recent we trust the
    // DJM exclusively and ignore the CDJ-self-reported bit.
    std::atomic<uint32_t> djmOnAirLastMs    { 0 };

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
