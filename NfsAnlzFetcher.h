// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter
//
// NfsAnlzFetcher -- Downloads and parses rekordbox ANLZ files from CDJ USB/SD
// via the non-standard NFSv2 server running on Pioneer players.
//
// DbServerClient runs it on its NFS thread for the analysis data its dbserver
// queries did not deliver (DbServerClient::launchNfsAsync).  Two entries:
// fetchByTrackId looks the track up in the slot's export.pdb, as Crate Digger
// does, and is the one DbServerClient uses; fetchAndParse takes an ANLZ path
// from its caller and has none since AUDIT META-1 (the dbserver item once
// read as that path, 0x000E, is the record label).  Either way the
// track's .DAT and .EXT are downloaded, parsed and merged (mergeDatExt).  The
// .2EX (CDJ-3000 3-band waveforms, PWV6/PWV7) is not downloaded.
//
// Protocol stack:
//   ONC RPC v2 over UDP (RFC 5531, which obsoletes RFC 1057)
//     -> Portmapper (program 100000, port 111): GETPORT for the two below
//     -> Mount v1 (program 100005): mount filesystem, get root FHandle
//     -> NFS v2 (program 100003): LOOKUP path elements, READ file data
//
// Data format:
//   export.pdb (rekordbox_pdb.ksy): tracks table -> analyze_path (string 14)
//   PMAI container (rekordbox_anlz.ksy from Deep Symmetry / Crate Digger)
//     .DAT -> PQTZ: beat grid
//          -> PCOB: standard cue lists (used when the file has no PCO2)
//     .EXT -> PCO2: extended cue lists (nxs2+, with colors/comments)
//          -> PCOB: standard cue lists
//          -> PSSI: song structure / phrase analysis (XOR masked)
//          -> PWV4: colour preview waveform (6 bytes per column)
//          -> PWV5: colour detail waveform (2 bytes per half-frame)
//   PWV7 (3-band detail) is read too if a file has it, but it lives in the
//   .2EX, which is not fetched.  The other tags are skipped.
//
// References:
//   - Deep Symmetry Crate Digger (EPL-2.0): https://github.com/Deep-Symmetry/crate-digger
//     (rekordbox_anlz.ksy, rekordbox_pdb.ksy, doc/.../anlz.adoc)
//   - Deep Symmetry beat-link: CrateDigger.java (which file holds what),
//     CueList.java (PCO2 before PCOB)
//   - IETF RFC 1094 (NFS v2, Mount v1), RFC 5531 (ONC RPC v2), RFC 4506 (XDR)
//
// Pioneer NFS quirks:
//   - Path components are UTF-16LE encoded (not ASCII)
//   - Mount paths: /B/ = SD slot, /C/ = USB slot
//   - The mount and NFS ports are asked of the portmapper, not assumed
//   - File handles are 32 bytes (standard FHSIZE)
//
// Threading: fetches run on DbServerClient's NFS thread, one at a time;
// cancel(), removePlayer() and clearPdbCache() may be called from any thread.

#pragma once
#include <JuceHeader.h>
#include <vector>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <mutex>
#include <utility>

class NfsAnlzFetcher
{
public:
    NfsAnlzFetcher() = default;

    //==========================================================================
    // Self-contained result types (avoid circular dependency with DbServerClient)
    //==========================================================================
    struct BeatEntry  { uint16_t beatNumber = 0; uint16_t bpmTimes100 = 0; uint32_t timeMs = 0; };
    struct PhraseEntry { uint16_t index = 0; uint16_t beatNumber = 0; uint16_t kind = 0;
                         uint16_t beatCount = 0; uint8_t fill = 0; uint16_t beatFill = 0; };
    struct CueEntry {
        enum Type { HotCue, MemoryPoint, Loop };
        Type type = MemoryPoint;
        uint32_t positionMs = 0;
        uint32_t loopEndMs = 0;
        int hotCueNumber = 0;
        uint8_t colorR = 0, colorG = 0, colorB = 0, colorCode = 0;
        bool hasColor = false;
        juce::String comment;
    };

    //==========================================================================
    // Result struct -- the ANLZ data extracted from one file, or merged from a
    // track's .DAT and .EXT
    //==========================================================================
    struct AnlzResult
    {
        bool ok = false;
        std::vector<BeatEntry>   beatGrid;
        std::vector<CueEntry>    cueList;        // memory points, hot cues and loops, by position
        bool cueTagsFound = false;               // the file had a PCO2 or PCOB tag (possibly empty)
        std::vector<PhraseEntry> songStructure;
        uint16_t phraseMood = 0;
        std::vector<uint8_t> detailData;
        int detailEntryCount = 0;
        int detailBytesPerEntry = 0;             // 2 = PWV5, 3 = PWV7
        std::vector<uint8_t> previewData;        // PWV4 colour preview (in .EXT), as TrackMetadata::waveformData
        int previewEntryCount = 0;
        int previewBytesPerEntry = 0;            // 6 = PWV4
    };

    //==========================================================================
    // High-level API: fetch ANLZ by track ID (full Crate Digger flow)
    //==========================================================================

    /// Complete NFS pipeline: download export.pdb, find ANLZ path for the
    /// given track ID, download BOTH .DAT and .EXT files, parse and merge
    /// (see mergeDatExt).
    /// .DAT has: PQTZ (beat grid), PCOB (standard cues), PPTH, PVBR, PWAV
    /// .EXT has: PCO2 and PCOB (cues), PSSI (song structure), PWV3, PWV4, PWV5
    AnlzResult fetchByTrackId(const juce::String& playerIP, uint8_t slot,
                              uint32_t trackId)
    {
        return fetchByTrackId(playerIP, slot, trackId, cancelToken());
    }

    /// As above, for a fetch decided when @p token was taken (cancelToken()):
    /// a cancel() after that moment ends it, even one that lands before the
    /// thread running the fetch has begun (AUDIT WIRE-6).
    AnlzResult fetchByTrackId(const juce::String& playerIP, uint8_t slot,
                              uint32_t trackId, uint32_t token)
    {
        AnlzResult result;
        fetchGeneration = token;
        if (playerIP.isEmpty() || trackId == 0) return result;

        juce::String mountPath = slotToMountPath(slot);
        if (mountPath.isEmpty()) return result;

        DBG("NfsAnlzFetcher: NFS pipeline for track " + juce::String(trackId)
            + " on " + playerIP + " slot=" + juce::String(slot));

        // Step 1: Download export.pdb to find the ANLZ path
        juce::String anlzPath = findAnlzPathFromPdb(playerIP, mountPath, trackId);
        if (anlzPath.isEmpty())
        {
            DBG("NfsAnlzFetcher: could not find ANLZ path for track " + juce::String(trackId));
            return result;
        }

        DBG("NfsAnlzFetcher: found ANLZ path: " + anlzPath);

        // Steps 2-3: .DAT and .EXT
        bool sawStale = false;
        result = fetchAnalysisFiles(playerIP, mountPath, anlzPath, &sawStale);
        if (sawStale)
        {
            // A handle went stale during the download: the slot's filesystem
            // changed (new media), so the cached index the path came from is
            // gone (forgetSlot).  Look the track up in the new export.pdb and
            // download both files again, even under the same path: the .DAT
            // may have come from the old media before the .EXT went stale,
            // and the same path on another export is not the same analysis
            // (AUDIT META-4).
            const juce::String freshPath = findAnlzPathFromPdb(playerIP, mountPath, trackId);
            if (freshPath.isEmpty())
                return {};
            result = fetchAnalysisFiles(playerIP, mountPath, freshPath, nullptr);
        }
        return result;
    }

    //==========================================================================
    // High-level API: fetch ANLZ by a path the caller gives
    //==========================================================================

    /// Fetch the ANLZ .DAT and .EXT files of a track and merge them, as
    /// fetchByTrackId does once it has the path.
    /// @param playerIP   IP address of the CDJ
    /// @param slot        Media slot (2=SD, 3=USB)
    /// @param anlzPath   An ANLZ path as export.pdb stores it, e.g.
    ///                   "PIONEER/USBANLZ/P053/0000/ANLZ0006.DAT" (.DAT, .EXT or
    ///                   .2EX; the other extension is derived).  dbserver gives
    ///                   no such path (AUDIT META-1); no caller in the app.
    /// @return Parsed ANLZ data, or result with ok=false on failure.
    AnlzResult fetchAndParse(const juce::String& playerIP, uint8_t slot,
                             const juce::String& anlzPath)
    {
        AnlzResult result;
        fetchGeneration = cancelGeneration.load(std::memory_order_relaxed);

        if (playerIP.isEmpty() || anlzPath.isEmpty())
            return result;

        // Determine NFS mount path from slot
        juce::String mountPath = slotToMountPath(slot);
        if (mountPath.isEmpty())
        {
            DBG("NfsAnlzFetcher: unknown slot " + juce::String(slot));
            return result;
        }

        return fetchAnalysisFiles(playerIP, mountPath, anlzPath);
    }

    /// Merge what a track's .DAT and .EXT gave, file by file as beat-link's
    /// CrateDigger reads them: the beat grid from the .DAT (PQTZ is a .DAT
    /// tag); the cues from the .EXT when it has cue tags (its PCO2 lists, or
    /// its PCOB when it has no PCO2 -- see parseAnlzFile), else from the .DAT;
    /// the song structure (PSSI), the detail (PWV5) and the colour preview
    /// (PWV4) from the .EXT.  Each falls back to the other file when the
    /// preferred one does not have it.  ok if either file parsed.
    static AnlzResult mergeDatExt(AnlzResult dat, AnlzResult ext)
    {
        if (!dat.ok) return ext;
        if (!ext.ok) return dat;

        AnlzResult m = std::move(dat);
        if (m.beatGrid.empty())
            m.beatGrid = std::move(ext.beatGrid);
        if (ext.cueTagsFound)
        {
            m.cueList = std::move(ext.cueList);
            m.cueTagsFound = true;
        }
        if (!ext.songStructure.empty())
        {
            m.songStructure = std::move(ext.songStructure);
            m.phraseMood = ext.phraseMood;
        }
        if (ext.detailEntryCount > 0)
        {
            m.detailData = std::move(ext.detailData);
            m.detailEntryCount = ext.detailEntryCount;
            m.detailBytesPerEntry = ext.detailBytesPerEntry;
        }
        if (ext.previewEntryCount > 0)
        {
            m.previewData = std::move(ext.previewData);
            m.previewEntryCount = ext.previewEntryCount;
            m.previewBytesPerEntry = ext.previewBytesPerEntry;
        }
        return m;
    }

    /// Abandon the fetch in progress, if any, and a fetch whose token was taken
    /// before the call but which has not begun yet: it returns within ~50 ms
    /// with what it has (ok=false when a file is missing).  Any thread; a
    /// fetch whose token is taken after the call is not affected.  For
    /// DbServerClient::stop(), before it joins the NFS thread.
    void cancel() noexcept { cancelGeneration.fetch_add(1, std::memory_order_relaxed); }

    /// The token for a fetch decided now (any thread): DbServerClient takes it
    /// on its worker before it starts the NFS thread, and passes it to
    /// fetchByTrackId.
    uint32_t cancelToken() const noexcept { return cancelGeneration.load(std::memory_order_relaxed); }

    //==========================================================================
    // Cache invalidation.  What the fetcher learns is cached per player IP
    // (RPC ports) and per player IP + slot (the mount handle, and the
    // export.pdb index of track ID -> ANLZ path).  rekordbox IDs are per
    // export, so a cached index is only right for the media it came from.
    // All three may be called from any thread while a fetch runs; a fetch
    // running at the time does not store what it learnt about the player or
    // slot the call forgets, and still stores what it learnt about others
    // (AUDIT META-4).
    //==========================================================================

    /// Forget everything learnt from one player: its RPC ports, its mount
    /// handles and the export.pdb index of each of its slots.  Call when the
    /// player leaves the network (or changes IP).
    void removePlayer(const juce::String& playerIP)
    {
        const std::lock_guard<std::mutex> lock(cacheMutex);
        mountCache.erase(playerIP);
        portCache.erase(playerIP);
        for (auto it = pdbAnlzCache.begin(); it != pdbAnlzCache.end();)
            it = (it->first.first == playerIP) ? pdbAnlzCache.erase(it) : std::next(it);
        ++playerEpoch[playerIP];
    }

    /// Forget the export.pdb index and the mount handle of one slot of one
    /// player (2 = SD, 3 = USB).  Call when the media in that slot changes.
    void clearPdbCache(const juce::String& playerIP, uint8_t slot)
    {
        const juce::String mountPath = slotToMountPath(slot);
        if (mountPath.isEmpty()) return;
        const std::lock_guard<std::mutex> lock(cacheMutex);
        auto m = mountCache.find(playerIP);
        if (m != mountCache.end())
            m->second.erase(mountPath);
        pdbAnlzCache.erase(SlotKey { playerIP, mountPath });
        ++slotEpoch[SlotKey { playerIP, mountPath }];
    }

    /// Forget every export.pdb index and mount handle, for all players.
    void clearPdbCache()
    {
        const std::lock_guard<std::mutex> lock(cacheMutex);
        pdbAnlzCache.clear();
        mountCache.clear();
        ++allSlotsEpoch;
    }

private:
    //==========================================================================
    // Constants
    //==========================================================================
    static constexpr int kFHandleSize      = 32;
    // NFSv2 READ chunk size.
    //
    // NFSv2 allows up to 8192 bytes per READ reply, but a single 8192-byte UDP
    // datagram crosses the 1500 B Ethernet MTU and is delivered to the player
    // as ~6 IP fragments that the kernel must reassemble.  On clean wired
    // networks this works; on busy or multi-NIC topologies, switches that
    // misroute fragments, autoIP setups with heavy ARP traffic, or older
    // player firmware revisions, fragment reassembly is unreliable and a
    // missing fragment kills the whole READ (NFSv2 has no per-fragment retry).
    //
    // flesniak's python-prodj-link -- the most field-tested NXS2 reference
    // we have, explicitly confirmed working against CDJ-2000NXS2 and
    // DJM-900NXS2 -- uses 1280 bytes per READ, sized so that the full UDP
    // datagram (1280 NFS payload + ~142 B of NFS/RPC/UDP/IP headers) fits
    // inside a single 1500 B Ethernet frame and never fragments.  We match
    // that value here.  The trade-off is ~6.4x more round-trips per ANLZ
    // file, which is negligible: ANLZ files are 10-40 KB, so the practical
    // cost is roughly 100-400 ms more per track load, well inside the time
    // the player itself spends on the same load.
    static constexpr int kNfsReadChunk     = 1280;
    // Largest file STC will download.  The size comes from the LOOKUP reply
    // (fattr.size, a wire value up to 4 GB), and it sizes the buffer.  ANLZ
    // files are tens of KB (a few MB for the detail of a very long track);
    // export.pdb is the large one, ~20 MB for a 20,000-track library.
    static constexpr uint32_t kMaxFileBytes = 128u * 1024u * 1024u;
    // How often a wait for an RPC reply looks at the cancel flag.
    static constexpr int kCancelPollMs     = 50;
    static constexpr int kRpcTimeoutMs     = 2000;
    static constexpr int kMountProgram     = 100005;
    static constexpr int kMountVersion     = 1;
    static constexpr int kMountProc_Mnt    = 1;
    static constexpr int kNfsProgram       = 100003;
    static constexpr int kNfsVersion       = 2;
    static constexpr int kNfsProc_Lookup   = 4;
    static constexpr int kNfsProc_Read     = 6;
    static constexpr int kPortmapperPort   = 111;
    static constexpr int kPortmapperProg   = 100000;
    static constexpr int kPortmapperVers   = 2;
    static constexpr int kPortmapperGetPort = 3;
    static constexpr int kProtocolUDP      = 17;

    //==========================================================================
    // Slot to NFS mount path mapping
    //==========================================================================
    static juce::String slotToMountPath(uint8_t slot)
    {
        switch (slot)
        {
            case 2: return "/B/";   // SD
            case 3: return "/C/";   // USB
            default: return {};
        }
    }

    //==========================================================================
    // XDR Encoding Helpers (big-endian, 4-byte aligned)
    //==========================================================================

    static void xdrWrite32(juce::MemoryOutputStream& out, uint32_t val)
    {
        uint8_t buf[4] = {
            (uint8_t)(val >> 24), (uint8_t)(val >> 16),
            (uint8_t)(val >> 8),  (uint8_t)(val)
        };
        out.write(buf, 4);
    }

    /// Write variable-length opaque data: length(4) + data + padding to 4-byte boundary
    static void xdrWriteOpaque(juce::MemoryOutputStream& out, const void* data, int len)
    {
        xdrWrite32(out, (uint32_t)len);
        out.write(data, len);
        int pad = (4 - (len % 4)) % 4;
        for (int i = 0; i < pad; i++)
            out.writeByte(0);
    }

    /// Write fixed-length opaque data (no length prefix): data + padding
    static void xdrWriteOpaqueFixed(juce::MemoryOutputStream& out, const void* data, int len)
    {
        out.write(data, len);
        int pad = (4 - (len % 4)) % 4;
        for (int i = 0; i < pad; i++)
            out.writeByte(0);
    }

    static uint32_t xdrRead32(const uint8_t* p)
    {
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16)
             | (uint32_t(p[2]) << 8)  | p[3];
    }

    //==========================================================================
    // ONC RPC v2 over UDP
    //==========================================================================

    /// Build an ONC RPC CALL message.
    static juce::MemoryBlock buildRpcCall(uint32_t xid, uint32_t program,
                                          uint32_t version, uint32_t procedure,
                                          const juce::MemoryBlock& args)
    {
        juce::MemoryOutputStream out;
        xdrWrite32(out, xid);          // XID
        xdrWrite32(out, 0);            // msg_type = CALL
        xdrWrite32(out, 2);            // rpc_version = 2
        xdrWrite32(out, program);
        xdrWrite32(out, version);
        xdrWrite32(out, procedure);
        // Auth: AUTH_NULL (flavor=0, length=0)
        xdrWrite32(out, 0);  // credential flavor
        xdrWrite32(out, 0);  // credential length
        xdrWrite32(out, 0);  // verifier flavor
        xdrWrite32(out, 0);  // verifier length
        // Append procedure-specific args
        out.write(args.getData(), args.getSize());
        return out.getMemoryBlock();
    }

    /// What one datagram on the RPC socket is to the call waiting for an xid.
    struct RpcReply
    {
        enum Status : uint8_t { NotOurs, Rejected, Accepted };
        Status status = NotOurs;
        int bodyOffset = 0;   // Accepted: the procedure's results start here
    };

    /// Read an ONC RPC v2 reply header (RFC 5531 section 9):
    ///   [0] xid, [4] msg_type (1 = REPLY), [8] reply_stat (0 = MSG_ACCEPTED,
    ///   1 = MSG_DENIED); accepted: [12] verifier flavor, [16] verifier length,
    ///   the verifier body padded to 4 bytes (XDR opaque, RFC 4506), then
    ///   accept_stat (0 = SUCCESS) and the results.
    /// NotOurs: not the reply to this call (another xid, not a REPLY, or
    /// truncated) -- keep waiting.  Rejected: MSG_DENIED (RPC_MISMATCH or
    /// AUTH_ERROR), an accept_stat other than SUCCESS (PROG_UNAVAIL,
    /// PROG_MISMATCH, PROC_UNAVAIL, GARBAGE_ARGS, SYSTEM_ERR) or a verifier
    /// over RFC 5531's 400 bytes -- sending the same call again cannot help.
    static RpcReply parseRpcReply(const uint8_t* buf, int len, uint32_t xid)
    {
        RpcReply r;
        if (len < 12) return r;
        if (xdrRead32(buf) != xid) return r;
        if (xdrRead32(buf + 4) != 1) return r;              // not a REPLY
        if (xdrRead32(buf + 8) != 0)                         // MSG_DENIED
        {
            r.status = RpcReply::Rejected;
            return r;
        }
        if (len < 20) return r;
        const uint32_t verfLen = xdrRead32(buf + 16);
        if (verfLen > 400)
        {
            r.status = RpcReply::Rejected;
            return r;
        }
        const int acceptOff = 20 + (int)((verfLen + 3) & ~3u);
        if (acceptOff + 4 > len) return r;
        if (xdrRead32(buf + acceptOff) != 0)                 // accept_stat
        {
            r.status = RpcReply::Rejected;
            return r;
        }
        r.status = RpcReply::Accepted;
        r.bodyOffset = acceptOff + 4;
        return r;
    }

    /// Send an RPC call and receive the reply. Returns reply body (after accept_stat).
    /// Returns empty MemoryBlock on failure.
    ///
    /// A reply counts only if it carries this call's xid and comes from
    /// @p host (an IP literal: the player's address from its status packets);
    /// anything else on the socket is skipped while the attempt's timeout
    /// runs.  A rejected call fails at once.
    juce::MemoryBlock rpcCall(const juce::String& host, int port,
                              uint32_t program, uint32_t version, uint32_t procedure,
                              const juce::MemoryBlock& args)
    {
        const uint32_t xid = nextXid++;
        auto msg = buildRpcCall(xid, program, version, procedure, args);

        juce::DatagramSocket sock(false);
        sock.bindToPort(0);

        // Send with retransmit (exponential backoff, up to 3 attempts)
        uint8_t recvBuf[65536];
        int timeoutMs = 250;

        for (int attempt = 0; attempt < 3; attempt++)
        {
            if (isCancelled())
                return {};
            if (sock.write(host, port, msg.getData(), (int)msg.getSize()) < 0)
                return {};

            const double deadline = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
            for (;;)
            {
                const int waitMs = (int)std::ceil(deadline - juce::Time::getMillisecondCounterHiRes());
                if (waitMs <= 0) break;
                if (isCancelled()) return {};
                const int ready = sock.waitUntilReady(true, juce::jmin(waitMs, kCancelPollMs));
                if (ready < 0) return {};
                if (ready == 0) continue;   // re-check the deadline and the cancel flag

                juce::String senderIP;
                int senderPort = 0;
                const int bytesRead = sock.read(recvBuf, (int)sizeof(recvBuf), false, senderIP, senderPort);
                if (bytesRead < 0) return {};
                if (senderIP != host) continue;   // not the player we called

                const auto reply = parseRpcReply(recvBuf, bytesRead, xid);
                if (reply.status == RpcReply::NotOurs) continue;
                if (reply.status == RpcReply::Rejected)
                {
                    DBG("NfsAnlzFetcher: RPC call rejected (prog=" + juce::String(program)
                        + " proc=" + juce::String(procedure) + ")");
                    return {};
                }
                return juce::MemoryBlock(recvBuf + reply.bodyOffset, (size_t)(bytesRead - reply.bodyOffset));
            }
            timeoutMs *= 2;  // exponential backoff
        }

        DBG("NfsAnlzFetcher: RPC call timed out (prog=" + juce::String(program)
            + " proc=" + juce::String(procedure) + ")");
        return {};
    }

    //==========================================================================
    // Portmapper -- discover actual ports for Mount and NFS services
    //==========================================================================

    struct PlayerPorts { int mountPort = 0; int nfsPort = 0; };

    //==========================================================================
    // Caches (see removePlayer).  cacheMutex guards the three maps and the
    // invalidation counters below; nothing is sent or received while it is
    // held.
    //==========================================================================
    std::mutex cacheMutex;
    /// playerIP -> {mountPort, nfsPort}
    std::map<juce::String, PlayerPorts> portCache;
    struct FHandle { uint8_t data[kFHandleSize] = {}; };
    /// playerIP -> (mountPath -> root FHandle)
    std::map<juce::String, std::map<juce::String, FHandle>> mountCache;
    /// (playerIP, mountPath) -> (track ID -> ANLZ path), from that slot's export.pdb
    using SlotKey = std::pair<juce::String, juce::String>;
    std::map<SlotKey, std::map<uint32_t, juce::String>> pdbAnlzCache;

    // Invalidation counters, only ever increased.  A fetch notes the ones
    // covering what it is about to learn and stores it only if none moved
    // meanwhile.  One counter for everything made a removePlayer() of one
    // player throw away another player's freshly downloaded export.pdb
    // index, ports and mount handle (AUDIT META-4).
    uint32_t allSlotsEpoch = 0;                         // clearPdbCache()
    std::map<juce::String, uint32_t> playerEpoch;       // removePlayer(ip)
    std::map<SlotKey, uint32_t> slotEpoch;              // clearPdbCache(ip, slot)

    /// Caller holds cacheMutex.  What a port lookup on playerIP notes: only
    /// removePlayer forgets ports.
    uint64_t portEpochLocked(const juce::String& playerIP) const
    {
        auto p = playerEpoch.find(playerIP);
        return p != playerEpoch.end() ? p->second : 0u;
    }

    /// Caller holds cacheMutex.  What a mount or export.pdb lookup on one
    /// slot notes: removePlayer(ip), clearPdbCache(ip, slot) and
    /// clearPdbCache() all forget a slot.  The counters only increase, so
    /// their sum moves whenever one of them does.
    uint64_t slotEpochLocked(const juce::String& playerIP, const juce::String& mountPath) const
    {
        auto s = slotEpoch.find(SlotKey { playerIP, mountPath });
        return (uint64_t) allSlotsEpoch + portEpochLocked(playerIP)
             + (s != slotEpoch.end() ? s->second : 0u);
    }

    /// After NFSERR_STALE (on the NFS thread): drop the slot's mount handle
    /// and export.pdb index.  Not an invalidation in the counters' sense --
    /// the fetch that saw it learns the new state itself.
    void forgetSlot(const juce::String& playerIP, const juce::String& mountPath)
    {
        const std::lock_guard<std::mutex> lock(cacheMutex);
        auto m = mountCache.find(playerIP);
        if (m != mountCache.end())
            m->second.erase(mountPath);
        pdbAnlzCache.erase(SlotKey { playerIP, mountPath });
    }

    /// Query the portmapper (RFC 1057) on port 111 for the port of a given program.
    /// Returns 0 on failure.
    int portmapperGetPort(const juce::String& playerIP, uint32_t program, uint32_t version)
    {
        // Build PMAPPROC_GETPORT args: mapping { prog(4), vers(4), prot(4), port(4) }
        juce::MemoryOutputStream args;
        xdrWrite32(args, program);
        xdrWrite32(args, version);
        xdrWrite32(args, kProtocolUDP);
        xdrWrite32(args, 0);  // port=0 when querying

        auto reply = rpcCall(playerIP, kPortmapperPort, kPortmapperProg,
                             kPortmapperVers, kPortmapperGetPort, args.getMemoryBlock());

        if (reply.getSize() < 4) return 0;

        uint32_t port = xdrRead32(static_cast<const uint8_t*>(reply.getData()));
        return (port > 0 && port < 65536) ? (int)port : 0;
    }

    /// Get (or discover) the mount and NFS ports for a player.
    PlayerPorts getPlayerPorts(const juce::String& playerIP)
    {
        uint64_t epoch = 0;
        {
            const std::lock_guard<std::mutex> lock(cacheMutex);
            auto it = portCache.find(playerIP);
            if (it != portCache.end() && it->second.mountPort > 0 && it->second.nfsPort > 0)
                return it->second;
            epoch = portEpochLocked(playerIP);
        }

        PlayerPorts ports;
        ports.mountPort = portmapperGetPort(playerIP, kMountProgram, kMountVersion);
        ports.nfsPort   = portmapperGetPort(playerIP, kNfsProgram, kNfsVersion);

        if (ports.mountPort > 0 && ports.nfsPort > 0)
        {
            DBG("NfsAnlzFetcher: discovered ports on " + playerIP
                + " -- mount=" + juce::String(ports.mountPort)
                + " nfs=" + juce::String(ports.nfsPort));
            const std::lock_guard<std::mutex> lock(cacheMutex);
            if (portEpochLocked(playerIP) == epoch)
                portCache[playerIP] = ports;
        }
        else
        {
            DBG("NfsAnlzFetcher: portmapper failed on " + playerIP
                + " mount=" + juce::String(ports.mountPort)
                + " nfs=" + juce::String(ports.nfsPort));
        }
        return ports;
    }

    //==========================================================================
    // Mount protocol
    //==========================================================================

    /// Encode a path as UTF-16LE for Pioneer NFS.  Characters outside the
    /// BMP become surrogate pairs (they were cut to their low 16 bits).
    static std::vector<uint8_t> encodeUtf16LE(const juce::String& str)
    {
        std::vector<uint8_t> result;
        auto putUnit = [&result](uint32_t u)
        {
            result.push_back((uint8_t)(u & 0xFF));
            result.push_back((uint8_t)((u >> 8) & 0xFF));
        };
        for (auto t = str.getCharPointer(); !t.isEmpty();)
        {
            // A code point: UTF-8 decodes to at most 21 bits
            const uint32_t ch = (uint32_t)(t.getAndAdvance() & 0x1FFFFF);
            if (ch >= 0x10000 && ch <= 0x10FFFF)
            {
                putUnit(0xD800 + ((ch - 0x10000) >> 10));
                putUnit(0xDC00 + ((ch - 0x10000) & 0x3FF));
            }
            else
            {
                putUnit(ch <= 0xFFFF ? ch : 0xFFFD);
            }
        }
        return result;
    }

    /// Decode @p numUnits UTF-16 code units (big- or little-endian), stopping
    /// at the first NUL.  Surrogate pairs become one character; an unpaired
    /// surrogate becomes U+FFFD.
    static juce::String decodeUtf16(const uint8_t* p, int numUnits, bool bigEndian)
    {
        std::vector<juce::juce_wchar> chars;
        chars.reserve((size_t)juce::jmax(0, numUnits) + 1);
        auto unitAt = [p, bigEndian](int i) -> uint32_t
        {
            return bigEndian ? (uint32_t)((p[i * 2] << 8) | p[i * 2 + 1])
                             : (uint32_t)(p[i * 2] | (p[i * 2 + 1] << 8));
        };
        for (int i = 0; i < numUnits; ++i)
        {
            uint32_t u = unitAt(i);
            if (u == 0) break;
            if (u >= 0xD800 && u <= 0xDBFF && i + 1 < numUnits)
            {
                const uint32_t lo = unitAt(i + 1);
                if (lo >= 0xDC00 && lo <= 0xDFFF)
                {
                    chars.push_back((juce::juce_wchar)(0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00)));
                    ++i;
                    continue;
                }
            }
            if (u >= 0xD800 && u <= 0xDFFF)
                u = 0xFFFD;
            chars.push_back((juce::juce_wchar)u);
        }
        chars.push_back(0);
        return juce::String(juce::CharPointer_UTF32(chars.data()));
    }

    /// Mount a filesystem on the CDJ. Returns true + sets outHandle on success.
    bool nfsMount(const juce::String& playerIP, const juce::String& mountPath, FHandle& outHandle)
    {
        // Check cache first
        uint64_t epoch = 0;
        {
            const std::lock_guard<std::mutex> lock(cacheMutex);
            auto pm = mountCache.find(playerIP);
            if (pm != mountCache.end())
            {
                auto it = pm->second.find(mountPath);
                if (it != pm->second.end())
                {
                    outHandle = it->second;
                    return true;
                }
            }
            epoch = slotEpochLocked(playerIP, mountPath);
        }

        // Build MOUNTPROC_MNT args: DirPath (variable-length opaque, UTF-16LE)
        auto pathBytes = encodeUtf16LE(mountPath);
        juce::MemoryOutputStream args;
        xdrWriteOpaque(args, pathBytes.data(), (int)pathBytes.size());

        auto ports = getPlayerPorts(playerIP);
        if (ports.mountPort == 0)
        {
            DBG("NfsAnlzFetcher: could not discover mount port");
            return false;
        }

        auto reply = rpcCall(playerIP, ports.mountPort, kMountProgram, kMountVersion,
                             kMountProc_Mnt, args.getMemoryBlock());

        if (reply.getSize() < 4)
        {
            DBG("NfsAnlzFetcher: mount reply too short");
            return false;
        }

        const uint8_t* r = static_cast<const uint8_t*>(reply.getData());
        uint32_t status = xdrRead32(r);
        if (status != 0)
        {
            DBG("NfsAnlzFetcher: mount failed, status=" + juce::String(status));
            return false;
        }

        if ((int)reply.getSize() < 4 + kFHandleSize)
        {
            DBG("NfsAnlzFetcher: mount reply missing FHandle");
            return false;
        }

        std::memcpy(outHandle.data, r + 4, kFHandleSize);
        {
            const std::lock_guard<std::mutex> lock(cacheMutex);
            if (slotEpochLocked(playerIP, mountPath) == epoch)
                mountCache[playerIP][mountPath] = outHandle;
        }
        DBG("NfsAnlzFetcher: mounted " + mountPath + " on " + playerIP);
        return true;
    }

    //==========================================================================
    // NFS v2 LOOKUP
    //==========================================================================

    static constexpr uint32_t kNoReply       = 0xFFFFFFFF;  // no (usable) RPC reply
    static constexpr uint32_t kNfsErrStale   = 70;          // NFSERR_STALE (RFC 1094 2.3.1)

    struct LookupResult
    {
        bool ok = false;
        uint32_t status = kNoReply;  // NFS status of the reply
        FHandle handle;
        uint32_t fileSize = 0;
        uint32_t fileType = 0;  // 1=regular, 2=directory
    };

    /// Lookup a single path element within a directory.
    LookupResult nfsLookup(const juce::String& playerIP, const FHandle& dirHandle,
                           const juce::String& name)
    {
        LookupResult lr;

        // Build NFSPROC_LOOKUP args: DirOpArgs = FHandle(32) + Filename(opaque<255>)
        auto nameBytes = encodeUtf16LE(name);
        juce::MemoryOutputStream args;
        xdrWriteOpaqueFixed(args, dirHandle.data, kFHandleSize);
        xdrWriteOpaque(args, nameBytes.data(), (int)nameBytes.size());

        auto reply = rpcCall(playerIP, getPlayerPorts(playerIP).nfsPort, kNfsProgram, kNfsVersion,
                             kNfsProc_Lookup, args.getMemoryBlock());

        if (reply.getSize() < 4) return lr;

        const uint8_t* r = static_cast<const uint8_t*>(reply.getData());
        uint32_t status = xdrRead32(r);
        lr.status = status;
        if (status != 0)
        {
            DBG("NfsAnlzFetcher: lookup failed for '" + name + "', status=" + juce::String(status));
            return lr;
        }

        // DirOpResBody: FHandle(32) + FAttr(68)
        if ((int)reply.getSize() < 4 + kFHandleSize + 68) return lr;

        std::memcpy(lr.handle.data, r + 4, kFHandleSize);
        lr.fileType = xdrRead32(r + 4 + kFHandleSize);     // FAttr.type
        lr.fileSize = xdrRead32(r + 4 + kFHandleSize + 20); // FAttr.size (offset 20 within FAttr)
        lr.ok = true;
        return lr;
    }

    //==========================================================================
    // NFS v2 READ
    //==========================================================================

    /// Read a chunk of a file. Returns data read, empty on failure (@p status:
    /// the NFS status of the reply, kNoReply without one).
    juce::MemoryBlock nfsRead(const juce::String& playerIP, const FHandle& fileHandle,
                              uint32_t offset, uint32_t count, uint32_t& status)
    {
        status = kNoReply;
        // Build NFSPROC_READ args: FHandle(32) + offset(4) + count(4) + totalcount(4)
        juce::MemoryOutputStream args;
        xdrWriteOpaqueFixed(args, fileHandle.data, kFHandleSize);
        xdrWrite32(args, offset);
        xdrWrite32(args, count);
        xdrWrite32(args, 0);  // totalcount (unused)

        auto reply = rpcCall(playerIP, getPlayerPorts(playerIP).nfsPort, kNfsProgram, kNfsVersion,
                             kNfsProc_Read, args.getMemoryBlock());

        if (reply.getSize() < 4) return {};

        const uint8_t* r = static_cast<const uint8_t*>(reply.getData());
        status = xdrRead32(r);
        if (status != 0) return {};

        // ReadResBody: FAttr(68) + data(opaque variable: len(4) + bytes)
        int dataLenOff = 4 + 68;  // after status + FAttr
        if ((int)reply.getSize() < dataLenOff + 4) return {};

        uint32_t dataLen = xdrRead32(r + dataLenOff);
        // Unsigned comparison: the wire length must fit in what was received.
        if (dataLen > (uint32_t)((int)reply.getSize() - dataLenOff - 4)) return {};

        return juce::MemoryBlock(r + dataLenOff + 4, dataLen);
    }

    //==========================================================================
    // NFS high-level: download a complete file
    //==========================================================================

    /// Download a file.  If a handle turns out stale (NFSERR_STALE: the slot's
    /// filesystem changed, as when new media goes in), what was cached for the
    /// slot is dropped and the download starts again once from MOUNT;
    /// @p sawStale (optional) is then set.
    bool nfsDownloadFile(const juce::String& playerIP, const juce::String& mountPath,
                         const juce::String& filePath, juce::MemoryBlock& outData,
                         bool* sawStale = nullptr)
    {
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            uint32_t nfsStatus = kNoReply;
            if (nfsDownloadOnce(playerIP, mountPath, filePath, outData, nfsStatus))
                return true;
            if (nfsStatus != kNfsErrStale || isCancelled())
                return false;
            DBG("NfsAnlzFetcher: stale handle on " + playerIP + mountPath + " -- remounting");
            forgetSlot(playerIP, mountPath);
            if (sawStale != nullptr)
                *sawStale = true;
        }
        return false;
    }

    bool nfsDownloadOnce(const juce::String& playerIP, const juce::String& mountPath,
                         const juce::String& filePath, juce::MemoryBlock& outData,
                         uint32_t& nfsStatus)
    {
        // Step 1: Mount
        FHandle rootHandle;
        if (!nfsMount(playerIP, mountPath, rootHandle))
            return false;

        // Step 2: Traverse path elements with LOOKUP
        juce::StringArray elements;
        elements.addTokens(filePath, "/\\", "");
        elements.removeEmptyStrings();

        FHandle currentHandle = rootHandle;
        LookupResult lr;

        for (int i = 0; i < elements.size(); i++)
        {
            lr = nfsLookup(playerIP, currentHandle, elements[i]);
            if (!lr.ok)
            {
                DBG("NfsAnlzFetcher: lookup failed at element '" + elements[i] + "'");
                nfsStatus = lr.status;
                return false;
            }
            currentHandle = lr.handle;
        }

        // Verify it's a regular file
        if (lr.fileType != 1)
        {
            DBG("NfsAnlzFetcher: target is not a regular file (type=" + juce::String(lr.fileType) + ")");
            return false;
        }

        uint32_t totalSize = lr.fileSize;
        DBG("NfsAnlzFetcher: file size=" + juce::String(totalSize) + " bytes");
        if (totalSize > kMaxFileBytes)
        {
            DBG("NfsAnlzFetcher: file too large to download");
            return false;
        }

        // Step 3: Read file in chunks
        outData.ensureSize(totalSize, false);
        outData.setSize(0);

        uint32_t offset = 0;
        while (offset < totalSize)
        {
            if (isCancelled())
            {
                DBG("NfsAnlzFetcher: download cancelled at offset " + juce::String(offset));
                return false;
            }
            uint32_t chunkSize = std::min((uint32_t)kNfsReadChunk, totalSize - offset);
            auto chunk = nfsRead(playerIP, currentHandle, offset, chunkSize, nfsStatus);
            if (chunk.getSize() == 0)
            {
                DBG("NfsAnlzFetcher: read failed at offset " + juce::String(offset));
                return false;
            }
            // A server that answers with more than asked cannot grow the file.
            const uint32_t take = (uint32_t)std::min<size_t>(chunk.getSize(), totalSize - offset);
            outData.append(chunk.getData(), take);
            offset += take;
        }

        return true;
    }

    //==========================================================================
    // A track's .DAT and .EXT
    //==========================================================================

    /// Download the .DAT and the .EXT of the analysis at @p anlzPath, parse
    /// both and merge them (mergeDatExt).  crate-digger documents which tags
    /// live in which file; beat-link's CrateDigger opens the .DAT for the beat
    /// grid and the .EXT first for cues and waveforms.
    AnlzResult fetchAnalysisFiles(const juce::String& playerIP, const juce::String& mountPath,
                                  const juce::String& anlzPath, bool* sawStale = nullptr)
    {
        juce::String basePath = anlzPath;
        if (basePath.endsWithIgnoreCase(".DAT") || basePath.endsWithIgnoreCase(".EXT")
            || basePath.endsWithIgnoreCase(".2EX"))
            basePath = basePath.dropLastCharacters(4);
        const juce::String datPath = basePath + ".DAT";
        const juce::String extPath = basePath + ".EXT";

        AnlzResult dat, ext;

        // .DAT: beat grid (PQTZ), standard cues (PCOB)
        juce::MemoryBlock datData;
        if (nfsDownloadFile(playerIP, mountPath, datPath, datData, sawStale))
        {
            DBG("NfsAnlzFetcher: .DAT downloaded " + juce::String((int)datData.getSize()) + " bytes");
            dat = parseAnlzFile(datData);
        }
        else
        {
            DBG("NfsAnlzFetcher: .DAT download failed");
        }

        // .EXT: cues (PCO2/PCOB), song structure (PSSI), waveforms (PWV4, PWV5)
        juce::MemoryBlock extData;
        if (nfsDownloadFile(playerIP, mountPath, extPath, extData, sawStale))
        {
            DBG("NfsAnlzFetcher: .EXT downloaded " + juce::String((int)extData.getSize()) + " bytes");
            ext = parseAnlzFile(extData);
        }
        else
        {
            DBG("NfsAnlzFetcher: .EXT download failed");
        }

        auto result = mergeDatExt(std::move(dat), std::move(ext));
        DBG("NfsAnlzFetcher: final merged -- beats=" + juce::String((int)result.beatGrid.size())
            + " cues=" + juce::String((int)result.cueList.size())
            + " phrases=" + juce::String((int)result.songStructure.size()));
        return result;
    }

    //==========================================================================
    // PDB Parser -- find analyze_path for a track ID from export.pdb
    //==========================================================================
    // Format: rekordbox DeviceSQL (all LITTLE-ENDIAN, unlike ANLZ which is BE).
    // File = pages of fixed size. Header at page 0 lists tables.
    // Tracks table (type=0) is a linked list of pages with row groups
    // built backwards from the end of each page.
    // Track row has fixed fields + ofs_strings[21] array of u2 offsets.
    // analyze_path = ofs_strings[14] -> device_sql_string.

    juce::String findAnlzPathFromPdb(const juce::String& playerIP,
                                     const juce::String& mountPath,
                                     uint32_t targetTrackId)
    {
        // Check this slot's index first
        const SlotKey key { playerIP, mountPath };
        uint64_t epoch = 0;
        {
            const std::lock_guard<std::mutex> lock(cacheMutex);
            auto idx = pdbAnlzCache.find(key);
            if (idx != pdbAnlzCache.end())
            {
                auto t = idx->second.find(targetTrackId);
                if (t != idx->second.end())
                    return t->second;
            }
            epoch = slotEpochLocked(playerIP, mountPath);
        }

        // Not there (or no index yet): download export.pdb
        juce::MemoryBlock pdb;
        if (!nfsDownloadFile(playerIP, mountPath, "PIONEER/rekordbox/export.pdb", pdb))
        {
            DBG("NfsAnlzFetcher: failed to download export.pdb");
            return {};
        }

        DBG("NfsAnlzFetcher: downloaded export.pdb -- " + juce::String((int)pdb.getSize()) + " bytes");

        // Index all tracks of this export; it replaces the slot's old index
        auto index = parsePdbTrackPaths(pdb);
        juce::String path;
        auto t = index.find(targetTrackId);
        if (t != index.end())
            path = t->second;
        {
            const std::lock_guard<std::mutex> lock(cacheMutex);
            if (slotEpochLocked(playerIP, mountPath) == epoch)
                pdbAnlzCache[key] = std::move(index);
        }

        if (path.isEmpty())
        {
            DBG("NfsAnlzFetcher: track " + juce::String(targetTrackId) + " not found in PDB");
        }
        return path;
    }

    static uint32_t readLE32(const uint8_t* p)
    {
        return p[0] | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
    }
    static uint16_t readLE16(const uint8_t* p)
    {
        return p[0] | (uint16_t(p[1]) << 8);
    }

    /// Parse a DeviceSQL string at the given offset within a page.
    static juce::String readDeviceSqlString(const uint8_t* pageData, int pageSize, int offset)
    {
        if (offset < 0 || offset >= pageSize) return {};

        uint8_t kind = pageData[offset];
        if (kind == 0x40)
        {
            // Long ASCII: u2(len) + u1(pad) + ASCII[len-4]
            if (offset + 4 > pageSize) return {};
            uint16_t len = readLE16(pageData + offset + 1);
            if (len < 4 || offset + (int)len > pageSize) return {};   // data is [offset+4, offset+len)
            return juce::String((const char*)(pageData + offset + 4), (size_t)(len - 4));
        }
        else if (kind == 0x90)
        {
            // Long UTF-16LE: u2(len) + u1(pad) + UTF16LE[len-4 bytes]
            if (offset + 4 > pageSize) return {};
            uint16_t len = readLE16(pageData + offset + 1);
            if (len < 4 || offset + (int)len > pageSize) return {};   // data is [offset+4, offset+len)
            int numChars = (int)(len - 4) / 2;
            return decodeUtf16(pageData + offset + 4, numChars, false);
        }
        else
        {
            // Short ASCII: actual_len = kind >> 1, text is actual_len-1 bytes
            int actualLen = kind >> 1;
            if (actualLen <= 1 || offset + 1 + actualLen - 1 > pageSize) return {};
            return juce::String((const char*)(pageData + offset + 1), (size_t)(actualLen - 1));
        }
    }

    /// Index the tracks table of an export.pdb: track ID -> ANLZ path.
    static std::map<uint32_t, juce::String> parsePdbTrackPaths(const juce::MemoryBlock& pdb)
    {
        std::map<uint32_t, juce::String> index;
        const uint8_t* d = static_cast<const uint8_t*>(pdb.getData());
        int fileSize = (int)pdb.getSize();

        if (fileSize < 28) return index;

        uint32_t lenPage   = readLE32(d + 4);
        uint32_t numTables = readLE32(d + 8);

        if (lenPage < 256 || lenPage > 65536 || numTables > 100) return index;

        // Find the tracks table (type=0)
        uint32_t tracksFirstPage = 0, tracksLastPage = 0;
        bool foundTracks = false;

        for (uint32_t t = 0; t < numTables; t++)
        {
            int tableOff = 28 + (int)t * 16;
            if (tableOff + 16 > fileSize) break;

            uint32_t tableType      = readLE32(d + tableOff);
            // uint32_t emptyCandidate = readLE32(d + tableOff + 4);
            uint32_t firstPageIdx   = readLE32(d + tableOff + 8);
            uint32_t lastPageIdx    = readLE32(d + tableOff + 12);

            if (tableType == 0)  // tracks
            {
                tracksFirstPage = firstPageIdx;
                tracksLastPage  = lastPageIdx;
                foundTracks = true;
                break;
            }
        }

        if (!foundTracks)
        {
            DBG("NfsAnlzFetcher: tracks table not found in PDB");
            return index;
        }

        DBG("NfsAnlzFetcher: tracks table pages " + juce::String(tracksFirstPage)
            + " to " + juce::String(tracksLastPage));

        // Walk the page chain
        uint32_t pageIdx = tracksFirstPage;
        // A chain longer than the file has pages is a loop in the links.
        int maxPages = fileSize / (int)lenPage;

        while (maxPages-- > 0)
        {
            // pageIdx and lenPage come from the file: multiply in 64 bits so a
            // corrupt index cannot wrap into a negative offset.
            const uint64_t pageOff64 = (uint64_t)pageIdx * (uint64_t)lenPage;
            if (pageOff64 + lenPage > (uint64_t)fileSize) break;
            int pageOff = (int)pageOff64;

            const uint8_t* page = d + pageOff;

            // Page header
            // [0] gap(4), [4] pageIndex(4), [8] type(4), [12] nextPage(4),
            // [16] sequence(4), [20] unk(4)
            // [24-26] packed: num_row_offsets(13 bits) + num_rows(11 bits) LE
            // [27] page_flags
            uint32_t pageType    = readLE32(page + 8);
            uint32_t nextPageIdx = readLE32(page + 12);
            uint8_t  pageFlags   = page[27];

            // Only process data pages of type 0 (tracks)
            bool isDataPage = (pageFlags & 0x40) == 0;
            if (isDataPage && pageType == 0)
            {
                uint32_t packed = page[24] | ((uint32_t)page[25] << 8) | ((uint32_t)page[26] << 16);
                int numRowOffsets = (int)(packed & 0x1FFF);
                // int numRows = (int)((packed >> 13) & 0x7FF);

                int numGroups = (numRowOffsets > 0) ? ((numRowOffsets - 1) / 16 + 1) : 0;
                static constexpr int kHeapPos = 40;  // heap starts at offset 40 within page

                for (int gi = 0; gi < numGroups; gi++)
                {
                    int groupBase = (int)lenPage - (gi * 0x24);
                    if (groupBase < 6 || groupBase > (int)lenPage) break;

                    // Row present flags (u2 LE at groupBase - 4)
                    int flagsOff = groupBase - 4;
                    if (flagsOff < 0 || flagsOff + 2 > (int)lenPage) break;
                    uint16_t presentFlags = readLE16(page + flagsOff);

                    int rowsInGroup = juce::jmin(16, numRowOffsets - gi * 16);

                    for (int ri = 0; ri < rowsInGroup; ri++)
                    {
                        if (!((presentFlags >> ri) & 1)) continue;  // row not present

                        // Row offset (u2 LE) at groupBase - 6 - (ri * 2)
                        int rowPtrOff = groupBase - 6 - (ri * 2);
                        if (rowPtrOff < 0 || rowPtrOff + 2 > (int)lenPage) continue;
                        uint16_t rowOfs = readLE16(page + rowPtrOff);

                        int rowAbs = kHeapPos + (int)rowOfs;  // absolute offset within page

                        // Track row: id at offset 72 (u4 LE), ofs_strings at offset 94 (u2[21] LE)
                        // We need: id(4) at rowAbs+72 and ofs_strings[14](2) at rowAbs+94+14*2 = rowAbs+122
                        if (rowAbs + 136 > (int)lenPage) continue;  // 94 + 21*2 = 136

                        uint32_t trackId = readLE32(page + rowAbs + 72);
                        if (trackId == 0) continue;

                        // analyze_path is ofs_strings[14]
                        uint16_t anlzStringOfs = readLE16(page + rowAbs + 94 + 14 * 2);
                        int stringAbsOff = rowAbs + (int)anlzStringOfs;

                        juce::String anlzPath = readDeviceSqlString(page, (int)lenPage, stringAbsOff);
                        if (anlzPath.isNotEmpty())
                            index[trackId] = anlzPath;
                    }
                }
            }

            // Move to next page
            if (pageIdx == tracksLastPage) break;
            if (nextPageIdx == pageIdx) break;  // avoid infinite loop
            pageIdx = nextPageIdx;
        }

        DBG("NfsAnlzFetcher: indexed " + juce::String((int)index.size()) + " track ANLZ paths from PDB");
        return index;
    }

    //==========================================================================
    // ANLZ PMAI Container Parser
    //==========================================================================

    /// Parse one ANLZ file (.DAT or .EXT): a PMAI header, then tagged sections.
    ///
    /// Layout per crate-digger's rekordbox_anlz.ksy: the file header is
    /// len_header bytes long; each section is fourcc(4) + len_header(4) +
    /// len_tag(4) and its body starts at byte 12 of the section (the ksy and
    /// crate-digger's anlz doc ignore a section's len_header, and so does this
    /// parser).  Every length comes from the file, so each one is checked as an
    /// unsigned value against what is left before it is used (AUDIT WIRE-5: a
    /// len_tag of 0xFFFFFFF4 used to step backwards and loop for ever).
    ///
    /// Cues follow beat-link's CueList: a file holds two lists of each kind
    /// (memory points and hot cues); the entries of every PCO2 tag are kept,
    /// and the PCOB tags are used only when the file has no PCO2.  The result
    /// is sorted by position, as the dbserver parsers sort theirs.
    static AnlzResult parseAnlzFile(const juce::MemoryBlock& fileData)
    {
        AnlzResult result;
        const uint8_t* d = static_cast<const uint8_t*>(fileData.getData());
        const size_t size = fileData.getSize();

        // Verify PMAI magic
        if (size < 12 || d[0] != 'P' || d[1] != 'M' || d[2] != 'A' || d[3] != 'I')
        {
            DBG("NfsAnlzFetcher: not a PMAI file");
            return result;
        }

        const uint32_t headerLen = readBE32(d + 4);
        if (headerLen < 12 || headerLen > size)
        {
            DBG("NfsAnlzFetcher: bad PMAI header length " + juce::String(headerLen));
            return result;
        }

        std::vector<CueEntry> extendedCues, standardCues;
        bool sawPco2 = false, sawPcob = false;

        // Iterate tagged sections
        size_t pos = headerLen;
        while (size - pos >= 12)
        {
            char tag[5] = { (char)d[pos], (char)d[pos+1], (char)d[pos+2], (char)d[pos+3], 0 };
            const uint32_t lenTag = readBE32(d + pos + 8);

            if (lenTag < 12 || lenTag > size - pos)
            {
                DBG("NfsAnlzFetcher: invalid section at offset " + juce::String((int64_t)pos)
                    + " tag=" + juce::String(tag) + " len=" + juce::String(lenTag));
                break;
            }

            const uint8_t* body = d + pos + 12;
            const int bodyLen = (int)(lenTag - 12);

            if (std::strcmp(tag, "PQTZ") == 0)
                result.beatGrid = parsePQTZ(body, bodyLen, 0);
            else if (std::strcmp(tag, "PCO2") == 0)
                { sawPco2 = true; appendPCO2(body, bodyLen, extendedCues); }
            else if (std::strcmp(tag, "PCOB") == 0)
                { sawPcob = true; appendPCOB(body, bodyLen, standardCues); }
            else if (std::strcmp(tag, "PSSI") == 0 && result.songStructure.empty())
                parsePSSI(body, bodyLen, 0, result.songStructure, result.phraseMood);
            else if (std::strcmp(tag, "PWV7") == 0 && result.detailEntryCount == 0)
                parseEntryTable(body, bodyLen, 3, kMaxDetailEntries,
                                result.detailData, result.detailEntryCount, result.detailBytesPerEntry);
            else if (std::strcmp(tag, "PWV5") == 0 && result.detailEntryCount == 0)
                parseEntryTable(body, bodyLen, 2, kMaxDetailEntries,
                                result.detailData, result.detailEntryCount, result.detailBytesPerEntry);
            else if (std::strcmp(tag, "PWV4") == 0 && result.previewEntryCount == 0)
                parseEntryTable(body, bodyLen, 6, kMaxPreviewEntries,
                                result.previewData, result.previewEntryCount, result.previewBytesPerEntry);

            pos += lenTag;
        }

        result.cueList = std::move(sawPco2 ? extendedCues : standardCues);
        std::stable_sort(result.cueList.begin(), result.cueList.end(),
                         [](const CueEntry& a, const CueEntry& b) { return a.positionMs < b.positionMs; });
        result.cueTagsFound = sawPco2 || sawPcob;

        result.ok = true;
        DBG("NfsAnlzFetcher: parsed ANLZ -- beats=" + juce::String((int)result.beatGrid.size())
            + " cues=" + juce::String((int)result.cueList.size())
            + " phrases=" + juce::String((int)result.songStructure.size())
            + " detailEntries=" + juce::String(result.detailEntryCount)
            + " previewEntries=" + juce::String(result.previewEntryCount));
        return result;
    }

    //==========================================================================
    // PQTZ: Beat Grid
    //==========================================================================
    static std::vector<BeatEntry> parsePQTZ(const uint8_t* body, int bodyLen,
                                                           int headerExtra)
    {
        // body starts after the 12-byte section header (tag + lenHeader + lenTag)
        // Format: u4(unk) + u4(unk, 0x80000) + numBeats(u4) + beats[numBeats]
        if (bodyLen < 12) return {};

        // The header may be larger than 12, skip extra header bytes
        int offset = (headerExtra > 0) ? headerExtra : 0;
        if (offset + 12 > bodyLen) return {};

        uint32_t numBeats = readBE32(body + offset + 8);
        if (numBeats == 0 || numBeats > 200000) return {};

        int entriesOff = offset + 12;
        int needed = (int)numBeats * 8;
        if (entriesOff + needed > bodyLen) return {};

        std::vector<BeatEntry> grid;
        grid.reserve(numBeats);
        for (uint32_t i = 0; i < numBeats; i++)
        {
            const uint8_t* e = body + entriesOff + i * 8;
            BeatEntry entry;
            entry.beatNumber  = readBE16(e);
            entry.bpmTimes100 = readBE16(e + 2);
            entry.timeMs      = readBE32(e + 4);
            grid.push_back(entry);
        }
        return grid;
    }

    //==========================================================================
    // PCO2: Extended Cue List (nxs2+ with colors and comments)
    //==========================================================================
    /// Append the entries of one PCO2 tag (a file has one for memory points
    /// and one for hot cues) to @p cues.
    static void appendPCO2(const uint8_t* body, int bodyLen, std::vector<CueEntry>& cues)
    {
        // Format: type(u4) + numCues(u2) + padding(u2) + PCP2 entries...
        if (bodyLen < 8) return;

        // uint32_t listType = readBE32(body);  // 0=memory, 1=hot cues (unused)
        uint16_t numCues  = readBE16(body + 4);
        if (numCues == 0) return;

        int pos = 8;  // start of PCP2 entries

        for (int i = 0; i < numCues && pos + 12 <= bodyLen; i++)
        {
            // Each PCP2 entry starts with "PCP2" magic
            if (body[pos] != 'P' || body[pos+1] != 'C' || body[pos+2] != 'P' || body[pos+3] != '2')
                break;

            uint32_t entryLen = readBE32(body + pos + 8);  // total entry size incl header
            if (entryLen < 0x1D || entryLen > 4096 || entryLen > (uint32_t)(bodyLen - pos)) break;

            const uint8_t* e = body + pos;  // points to PCP2 magic

            uint32_t hotCue = readBE32(e + 0x0C);  // after PCP2(4)+lenH(4)+lenE(4)
            uint8_t  type   = e[0x10];               // 1=cue, 2=loop
            uint32_t timeMs = readBE32(e + 0x14);
            uint32_t loopMs = readBE32(e + 0x18);

            CueEntry cue;
            cue.positionMs = timeMs;
            cue.loopEndMs  = (type == 2) ? loopMs : 0;
            cue.hotCueNumber = (hotCue > 0) ? (int)hotCue : 0;
            cue.type = (type == 2) ? CueEntry::Loop
                     : (hotCue > 0) ? CueEntry::HotCue
                     : CueEntry::MemoryPoint;

            // Color and comment (variable position fields)
            // len_comment at offset 0x28 (u4, byte count of UTF-16BE string)
            uint32_t commentBytes = 0;
            if (entryLen >= 0x2C)
            {
                commentBytes = readBE32(e + 0x28);
                if (commentBytes > 0 && commentBytes < 512
                    && commentBytes <= entryLen - 0x2C)
                {
                    int numChars = (int)commentBytes / 2;
                    cue.comment = decodeUtf16(e + 0x2C, numChars, true).trimEnd();
                }
            }

            // Color RGB after the comment: code, R, G, B.  Entries that end
            // before them (crate-digger's anlz doc warns some do) have none.
            // Unsigned arithmetic: len_comment is a wire value.
            if (entryLen >= 0x2C && commentBytes <= entryLen - 0x2C
                && entryLen - 0x2C - commentBytes >= 4)
            {
                const int colorOff = 0x2C + (int)commentBytes;
                cue.colorCode = e[colorOff];
                cue.colorR    = e[colorOff + 1];
                cue.colorG    = e[colorOff + 2];
                cue.colorB    = e[colorOff + 3];
                cue.hasColor  = (cue.colorR != 0 || cue.colorG != 0 || cue.colorB != 0);
            }

            cues.push_back(cue);
            pos += (int)entryLen;  // entryLen = total size including PCP2 header
        }
    }

    //==========================================================================
    // PCOB: Standard Cue List (no colors/comments; used when there is no PCO2)
    //==========================================================================
    /// Append the entries of one PCOB tag (memory points or hot cues) to @p cues.
    static void appendPCOB(const uint8_t* body, int bodyLen, std::vector<CueEntry>& cues)
    {
        // Format: type(u4) + pad(u2) + numCues(u2) + memoryCount(u4) + PCPT entries
        if (bodyLen < 12) return;

        uint16_t numCues = readBE16(body + 6);
        if (numCues == 0) return;

        int pos = 12;

        for (int i = 0; i < numCues && pos + 12 <= bodyLen; i++)
        {
            if (body[pos] != 'P' || body[pos+1] != 'C' || body[pos+2] != 'P' || body[pos+3] != 'T')
                break;

            // total PCPT size incl header (0x38 in every file crate-digger has
            // seen); the fields read below end at 0x28
            uint32_t entryLen = readBE32(body + pos + 8);
            if (entryLen < 0x28 || entryLen > 4096 || entryLen > (uint32_t)(bodyLen - pos)) break;

            const uint8_t* e = body + pos;  // points to PCPT magic

            uint32_t hotCue   = readBE32(e + 0x0C);
            // e[0x10..0x13] = status (unused)
            // e[0x14..0x17] = 0x10000, [0x18..0x1B] = order
            uint8_t  type     = e[0x1C];                 // 1=cue, 2=loop
            uint32_t timeMs   = readBE32(e + 0x20);
            uint32_t loopMs   = readBE32(e + 0x24);

            CueEntry cue;
            cue.positionMs = timeMs;
            cue.loopEndMs  = (type == 2) ? loopMs : 0;
            cue.hotCueNumber = (hotCue > 0) ? (int)hotCue : 0;
            cue.type = (type == 2) ? CueEntry::Loop
                     : (hotCue > 0) ? CueEntry::HotCue
                     : CueEntry::MemoryPoint;

            cues.push_back(cue);
            pos += (int)entryLen;  // entryLen = total size including PCPT header
        }
    }

    //==========================================================================
    // PSSI: Song Structure (XOR masked)
    //==========================================================================
    static void parsePSSI(const uint8_t* body, int bodyLen, int headerExtra,
                          std::vector<PhraseEntry>& phrases,
                          uint16_t& mood)
    {
        // body format: lenEntryBytes(u4) + numEntries(u2) + masked_body...
        // The body after numEntries is XOR masked if raw_mood > 20
        if (bodyLen < 6) return;

        int offset = (headerExtra > 0) ? headerExtra : 0;
        if (offset + 6 > bodyLen) return;

        // rekordbox_anlz.ksy reads the phrases as consecutive 24-byte
        // song_structure_entry records; len_entry_bytes "seems to always be
        // 24".  A file that says otherwise has a layout nobody has described,
        // so it is not read (the dbserver parser applies the same rule).  The
        // stride is the fixed 24 -- never the wire value (AUDIT WIRE-5: a
        // len_entry_bytes >= 2^31 cast to int read before the buffer).
        static constexpr int kPhraseEntrySize = 24;
        uint32_t entrySize = readBE32(body + offset);
        uint16_t numEntries = readBE16(body + offset + 4);
        if (numEntries == 0 || entrySize != (uint32_t)kPhraseEntrySize) return;

        int dataStart = offset + 6;
        int dataLen = bodyLen - dataStart;
        if (dataLen < 2) return;

        // Check if masked: read raw_mood (first u2 of body after numEntries)
        uint16_t rawMood = readBE16(body + dataStart);
        bool isMasked = (rawMood > 20);

        // Build unmasked copy
        std::vector<uint8_t> unmasked(body + dataStart, body + dataStart + dataLen);
        if (isMasked)
        {
            // XOR mask from Kaitai spec, derived from numEntries
            uint8_t mask[19];
            uint8_t c = (uint8_t)(numEntries & 0xFF);
            mask[0]  = (uint8_t)(0xCB + c);
            mask[1]  = (uint8_t)(0xE1 + c);
            mask[2]  = (uint8_t)(0xEE + c);
            mask[3]  = (uint8_t)(0xFA + c);
            mask[4]  = (uint8_t)(0xE5 + c);
            mask[5]  = (uint8_t)(0xEE + c);
            mask[6]  = (uint8_t)(0xAD + c);
            mask[7]  = (uint8_t)(0xEE + c);
            mask[8]  = (uint8_t)(0xE9 + c);
            mask[9]  = (uint8_t)(0xD2 + c);
            mask[10] = (uint8_t)(0xE9 + c);
            mask[11] = (uint8_t)(0xEB + c);
            mask[12] = (uint8_t)(0xE1 + c);
            mask[13] = (uint8_t)(0xE9 + c);
            mask[14] = (uint8_t)(0xF3 + c);
            mask[15] = (uint8_t)(0xE8 + c);
            mask[16] = (uint8_t)(0xE9 + c);
            mask[17] = (uint8_t)(0xF4 + c);
            mask[18] = (uint8_t)(0xE1 + c);

            for (int i = 0; i < (int)unmasked.size(); i++)
                unmasked[(size_t)i] ^= mask[i % 19];
        }

        // Parse unmasked body: mood(u2) + pad(6) + endBeat(u2) + pad(2) + bank(u1) + pad(1) + entries[numEntries*24]
        if ((int)unmasked.size() < 14) return;

        mood = readBE16(unmasked.data());
        // uint16_t endBeat = readBE16(unmasked.data() + 8);

        int entriesOff = 14;
        for (int i = 0; i < numEntries; i++)
        {
            int eOff = entriesOff + i * kPhraseEntrySize;
            if (eOff + kPhraseEntrySize > (int)unmasked.size()) break;

            const uint8_t* e = unmasked.data() + eOff;
            PhraseEntry phrase;
            phrase.index      = readBE16(e);
            phrase.beatNumber = readBE16(e + 2);
            phrase.kind       = readBE16(e + 4);
            // [6] pad, [7] k1, [8] pad, [9] k2, [10] pad, [11] b
            // [12-13] beat2, [14-15] beat3, [16-17] beat4
            // [18] pad, [19] k3, [20] pad
            phrase.fill       = e[21];
            phrase.beatFill   = readBE16(e + 22);

            // Calculate beat count from next phrase's beat number
            if (i + 1 < numEntries)
            {
                int nextOff = entriesOff + (i + 1) * kPhraseEntrySize;
                if (nextOff + 4 <= (int)unmasked.size())
                    phrase.beatCount = readBE16(unmasked.data() + nextOff + 2) - phrase.beatNumber;
            }

            phrases.push_back(phrase);
        }
    }

    //==========================================================================
    // Waveform tables: PWV5 / PWV7 detail, PWV4 colour preview
    //==========================================================================
    // Detail entries are 150 per second of audio.  5,000,000 is the dbserver
    // parser's cap (9 h 15 min; 10 MB of PWV5): the 500,000 used here before
    // dropped the detail of every track longer than 55 min (AUDIT DEBT-7).
    static constexpr uint32_t kMaxDetailEntries  = 5000000;
    // The colour preview is 1,200 columns in every file crate-digger documents.
    static constexpr uint32_t kMaxPreviewEntries = 65536;

    /// Read a len_entry_bytes(u4) + len_entries(u4) + unknown(u4) + entries
    /// table (PWV4, PWV5 and PWV7 share this layout in rekordbox_anlz.ksy).
    static void parseEntryTable(const uint8_t* body, int bodyLen, int bpe, uint32_t maxEntries,
                                std::vector<uint8_t>& data, int& entryCount, int& bytesPerEntry)
    {
        if (bodyLen < 12) return;

        uint32_t wordSize = readBE32(body);
        uint32_t count    = readBE32(body + 4);
        if (wordSize != (uint32_t)bpe || count == 0 || count > maxEntries) return;

        const int dataOff = 12;
        const int64_t dataLen64 = (int64_t)wordSize * (int64_t)count;
        if (dataLen64 > (int64_t)(bodyLen - dataOff)) return;

        const int dataLen = (int)dataLen64;
        data.assign(body + dataOff, body + dataOff + dataLen);
        entryCount = (int)count;
        bytesPerEntry = bpe;
    }

    //==========================================================================
    // Helpers
    //==========================================================================
    static uint32_t readBE32(const uint8_t* p)
    {
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16)
             | (uint32_t(p[2]) << 8)  | p[3];
    }
    static uint16_t readBE16(const uint8_t* p)
    {
        return (uint16_t(p[0]) << 8) | p[1];
    }

    uint32_t nextXid = 1;  // RPC transaction ID counter

    // Cancellation (cancel()).  A fetch runs on DbServerClient's NFS thread,
    // one at a time; it begins from the value of cancelGeneration its token
    // holds (taken at launch, or when it begins) and stops when the value
    // moves.
    std::atomic<uint32_t> cancelGeneration { 0 };
    uint32_t fetchGeneration = 0;   // the running fetch's token; NFS thread only
    bool isCancelled() const noexcept
    {
        return cancelGeneration.load(std::memory_order_relaxed) != fetchGeneration;
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NfsAnlzFetcher)
};
