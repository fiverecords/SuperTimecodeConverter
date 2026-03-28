// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter
//
// NfsAnlzFetcher -- Downloads and parses rekordbox ANLZ files from CDJ USB/SD
// via the non-standard NFSv2 server running on Pioneer players.
//
// This replaces the failing dbserver ANLZ tag queries (0x2c04 for PQTZ/PCO2/PSSI)
// which don't work reliably on CDJ-3000.  Instead, we download the .EXT file
// directly over NFS and parse the PMAI container locally.
//
// Protocol stack:
//   ONC RPC v2 over UDP (RFC 1057)
//     -> Mount v1 (program 100005): mount filesystem, get root FHandle
//     -> NFS v2 (program 100003): LOOKUP path elements, READ file data
//
// Data format:
//   PMAI container (rekordbox_anlz.ksy from Deep Symmetry / Crate Digger)
//     -> PQTZ: beat grid
//     -> PCO2: extended cue list (nxs2+, with colors/comments)
//     -> PCOB: standard cue list (fallback)
//     -> PSSI: song structure / phrase analysis (XOR masked)
//     -> PWV5/PWV7: detail waveform (already fetched via dbserver, but available here too)
//
// References:
//   - Deep Symmetry Crate Digger (EPL-2.0): https://github.com/Deep-Symmetry/crate-digger
//   - Kaitai Struct rekordbox_anlz.ksy format specification
//   - IETF RFC 1094 (NFS v2), RFC 1057 (ONC RPC v2)
//
// Pioneer NFS quirks:
//   - Path components are UTF-16LE encoded (not ASCII)
//   - Mount paths: /B/ = SD slot, /C/ = USB slot
//   - Standard NFSv2 port (2049) is used
//   - File handles are 32 bytes (standard FHSIZE)

#pragma once
#include <JuceHeader.h>
#include <vector>
#include <cstring>
#include <algorithm>
#include <map>

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
    // Result struct -- all ANLZ data extracted from one .EXT file
    //==========================================================================
    struct AnlzResult
    {
        bool ok = false;
        std::vector<BeatEntry>   beatGrid;
        std::vector<CueEntry>    cueList;
        std::vector<PhraseEntry> songStructure;
        uint16_t phraseMood = 0;
        std::vector<uint8_t> detailData;
        int detailEntryCount = 0;
        int detailBytesPerEntry = 0;
    };

    //==========================================================================
    // High-level API: fetch ANLZ by track ID (full Crate Digger flow)
    //==========================================================================

    /// Complete NFS pipeline: download export.pdb, find ANLZ path for the
    /// given track ID, download BOTH .DAT and .EXT files, parse and merge.
    /// .DAT has: PQTZ (beat grid), PCOB (standard cues), PPTH, PVBR, PWAV
    /// .EXT has: PCO2 (extended cues), PSSI (song structure), PWV4, PWV5
    AnlzResult fetchByTrackId(const juce::String& playerIP, uint8_t slot,
                              uint32_t trackId)
    {
        AnlzResult result;
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

        // Ensure we have the .DAT base path
        juce::String datPath = anlzPath;
        if (!datPath.endsWithIgnoreCase(".DAT"))
            datPath = datPath.upToLastOccurrenceOf(".", false, true) + ".DAT";
        juce::String extPath = datPath.dropLastCharacters(4) + ".EXT";

        // Step 2: Download .DAT (has beat grid PQTZ + standard cues PCOB)
        juce::MemoryBlock datData;
        if (nfsDownloadFile(playerIP, mountPath, datPath, datData))
        {
            DBG("NfsAnlzFetcher: .DAT downloaded " + juce::String((int)datData.getSize()) + " bytes");
            result = parseAnlzFile(datData);
        }
        else
        {
            DBG("NfsAnlzFetcher: .DAT download failed");
        }

        // Step 3: Download .EXT (has extended cues PCO2 + song structure PSSI)
        juce::MemoryBlock extData;
        if (nfsDownloadFile(playerIP, mountPath, extPath, extData))
        {
            DBG("NfsAnlzFetcher: .EXT downloaded " + juce::String((int)extData.getSize()) + " bytes");
            auto extResult = parseAnlzFile(extData);

            // Merge: .EXT data overwrites .DAT data where available
            if (!extResult.cueList.empty())
                result.cueList = std::move(extResult.cueList);
            if (!extResult.songStructure.empty())
            {
                result.songStructure = std::move(extResult.songStructure);
                result.phraseMood = extResult.phraseMood;
            }
            if (extResult.detailEntryCount > 0 && result.detailEntryCount == 0)
            {
                result.detailData = std::move(extResult.detailData);
                result.detailEntryCount = extResult.detailEntryCount;
                result.detailBytesPerEntry = extResult.detailBytesPerEntry;
            }
            // Beat grid stays from .DAT (PQTZ is only in .DAT)
            result.ok = true;
        }
        else
        {
            DBG("NfsAnlzFetcher: .EXT download failed");
        }

        if (!result.ok && !result.beatGrid.empty())
            result.ok = true;

        DBG("NfsAnlzFetcher: final merged -- beats=" + juce::String((int)result.beatGrid.size())
            + " cues=" + juce::String((int)result.cueList.size())
            + " phrases=" + juce::String((int)result.songStructure.size()));
        return result;
    }

    //==========================================================================
    // High-level API: fetch and parse an ANLZ .EXT file from a CDJ
    //==========================================================================

    /// Fetch the ANLZ .EXT file for a track and parse all tags.
    /// @param playerIP   IP address of the CDJ
    /// @param slot        Media slot (2=SD, 3=USB)
    /// @param anlzPath   Path from dbserver metadata, e.g. "PIONEER/USBANLZ/P053/0000/ANLZ0006.DAT"
    ///                   The .DAT extension is replaced with .EXT automatically.
    /// @return Parsed ANLZ data, or result with ok=false on failure.
    AnlzResult fetchAndParse(const juce::String& playerIP, uint8_t slot,
                             const juce::String& anlzPath)
    {
        AnlzResult result;

        if (playerIP.isEmpty() || anlzPath.isEmpty())
            return result;

        // Determine NFS mount path from slot
        juce::String mountPath = slotToMountPath(slot);
        if (mountPath.isEmpty())
        {
            DBG("NfsAnlzFetcher: unknown slot " + juce::String(slot));
            return result;
        }

        // Convert .DAT path to .EXT
        juce::String extPath = anlzPath;
        if (extPath.endsWithIgnoreCase(".DAT"))
            extPath = extPath.dropLastCharacters(4) + ".EXT";
        else if (!extPath.endsWithIgnoreCase(".EXT"))
            extPath = extPath + ".EXT";

        DBG("NfsAnlzFetcher: fetching " + mountPath + extPath + " from " + playerIP);

        // Download file via NFS
        juce::MemoryBlock fileData;
        if (!nfsDownloadFile(playerIP, mountPath, extPath, fileData))
        {
            DBG("NfsAnlzFetcher: NFS download failed");
            return result;
        }

        DBG("NfsAnlzFetcher: downloaded " + juce::String((int)fileData.getSize()) + " bytes");

        // Parse the PMAI container
        result = parseAnlzFile(fileData);
        return result;
    }

    /// Clear cached NFS mount handles for a player (call when player disappears).
    void removePlayer(const juce::String& playerIP)
    {
        mountCache.erase(playerIP);
        portCache.erase(playerIP);
        pdbAnlzCache.clear();
    }

private:
    //==========================================================================
    // Constants
    //==========================================================================
    static constexpr int kFHandleSize      = 32;
    static constexpr int kNfsReadChunk     = 8192;  // NFS v2 max (was 2048 = 4x more round-trips)
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

    /// Send an RPC call and receive the reply. Returns reply body (after accept_stat).
    /// Returns empty MemoryBlock on failure.
    juce::MemoryBlock rpcCall(const juce::String& host, int port,
                              uint32_t program, uint32_t version, uint32_t procedure,
                              const juce::MemoryBlock& args)
    {
        auto msg = buildRpcCall(nextXid++, program, version, procedure, args);

        juce::DatagramSocket sock(false);
        sock.bindToPort(0);

        // Send with retransmit (exponential backoff, up to 3 attempts)
        uint8_t recvBuf[65536];
        int timeoutMs = 250;

        for (int attempt = 0; attempt < 3; attempt++)
        {
            if (sock.write(host, port, msg.getData(), (int)msg.getSize()) < 0)
                return {};

            if (sock.waitUntilReady(true, timeoutMs) > 0)
            {
                juce::String senderIP;
                int senderPort = 0;
                int bytesRead = sock.read(recvBuf, sizeof(recvBuf), false, senderIP, senderPort);
                if (bytesRead >= 24)
                {
                    // Parse RPC reply header
                    // [0-3] XID, [4-7] msg_type=1(REPLY), [8-11] reply_stat=0(ACCEPTED)
                    // [12-15] verf_flavor, [16-19] verf_length, [20-23] accept_stat=0(SUCCESS)
                    // [24+] reply body
                    uint32_t replyType = xdrRead32(recvBuf + 4);
                    if (replyType != 1) continue;  // not a REPLY

                    uint32_t replyStat = xdrRead32(recvBuf + 8);
                    if (replyStat != 0) continue;  // not ACCEPTED

                    uint32_t verfLen = xdrRead32(recvBuf + 16);
                    int bodyOffset = 20 + (int)verfLen + 4;  // skip verifier + accept_stat

                    if (bodyOffset > bytesRead) continue;

                    uint32_t acceptStat = xdrRead32(recvBuf + bodyOffset - 4);
                    if (acceptStat != 0)
                    {
                        DBG("NfsAnlzFetcher: RPC accept_stat=" + juce::String(acceptStat));
                        return {};
                    }

                    return juce::MemoryBlock(recvBuf + bodyOffset, bytesRead - bodyOffset);
                }
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

    /// Cache of discovered ports: playerIP -> {mountPort, nfsPort}
    struct PlayerPorts { int mountPort = 0; int nfsPort = 0; };
    std::map<juce::String, PlayerPorts> portCache;

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
        auto it = portCache.find(playerIP);
        if (it != portCache.end() && it->second.mountPort > 0 && it->second.nfsPort > 0)
            return it->second;

        PlayerPorts ports;
        ports.mountPort = portmapperGetPort(playerIP, kMountProgram, kMountVersion);
        ports.nfsPort   = portmapperGetPort(playerIP, kNfsProgram, kNfsVersion);

        if (ports.mountPort > 0 && ports.nfsPort > 0)
        {
            DBG("NfsAnlzFetcher: discovered ports on " + playerIP
                + " -- mount=" + juce::String(ports.mountPort)
                + " nfs=" + juce::String(ports.nfsPort));
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

    struct FHandle { uint8_t data[kFHandleSize] = {}; };

    /// Cache of mounted filesystem handles: playerIP -> (mountPath -> FHandle)
    std::map<juce::String, std::map<juce::String, FHandle>> mountCache;

    /// Encode a path as UTF-16LE for Pioneer NFS
    static std::vector<uint8_t> encodeUtf16LE(const juce::String& str)
    {
        std::vector<uint8_t> result;
        for (int i = 0; i < str.length(); i++)
        {
            juce::juce_wchar ch = str[i];
            result.push_back((uint8_t)(ch & 0xFF));
            result.push_back((uint8_t)((ch >> 8) & 0xFF));
        }
        return result;
    }

    /// Mount a filesystem on the CDJ. Returns true + sets outHandle on success.
    bool nfsMount(const juce::String& playerIP, const juce::String& mountPath, FHandle& outHandle)
    {
        // Check cache first
        auto& playerMounts = mountCache[playerIP];
        auto it = playerMounts.find(mountPath);
        if (it != playerMounts.end())
        {
            outHandle = it->second;
            return true;
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
        playerMounts[mountPath] = outHandle;
        DBG("NfsAnlzFetcher: mounted " + mountPath + " on " + playerIP);
        return true;
    }

    //==========================================================================
    // NFS v2 LOOKUP
    //==========================================================================

    struct LookupResult
    {
        bool ok = false;
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

    /// Read a chunk of a file. Returns data read, empty on failure.
    juce::MemoryBlock nfsRead(const juce::String& playerIP, const FHandle& fileHandle,
                              uint32_t offset, uint32_t count)
    {
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
        uint32_t status = xdrRead32(r);
        if (status != 0) return {};

        // ReadResBody: FAttr(68) + data(opaque variable: len(4) + bytes)
        int dataLenOff = 4 + 68;  // after status + FAttr
        if ((int)reply.getSize() < dataLenOff + 4) return {};

        uint32_t dataLen = xdrRead32(r + dataLenOff);
        if ((int)reply.getSize() < dataLenOff + 4 + (int)dataLen) return {};

        return juce::MemoryBlock(r + dataLenOff + 4, dataLen);
    }

    //==========================================================================
    // NFS high-level: download a complete file
    //==========================================================================

    bool nfsDownloadFile(const juce::String& playerIP, const juce::String& mountPath,
                         const juce::String& filePath, juce::MemoryBlock& outData)
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

        // Step 3: Read file in chunks
        outData.ensureSize(totalSize, false);
        outData.setSize(0);

        uint32_t offset = 0;
        while (offset < totalSize)
        {
            uint32_t chunkSize = std::min((uint32_t)kNfsReadChunk, totalSize - offset);
            auto chunk = nfsRead(playerIP, currentHandle, offset, chunkSize);
            if (chunk.getSize() == 0)
            {
                DBG("NfsAnlzFetcher: read failed at offset " + juce::String(offset));
                return false;
            }
            outData.append(chunk.getData(), chunk.getSize());
            offset += (uint32_t)chunk.getSize();
        }

        return true;
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
        // Check cache first
        auto cacheIt = pdbAnlzCache.find(targetTrackId);
        if (cacheIt != pdbAnlzCache.end())
            return cacheIt->second;

        // Download export.pdb
        juce::MemoryBlock pdb;
        if (!nfsDownloadFile(playerIP, mountPath, "PIONEER/rekordbox/export.pdb", pdb))
        {
            DBG("NfsAnlzFetcher: failed to download export.pdb");
            return {};
        }

        DBG("NfsAnlzFetcher: downloaded export.pdb -- " + juce::String((int)pdb.getSize()) + " bytes");

        // Parse all tracks and cache their ANLZ paths
        parsePdbTrackPaths(pdb);

        cacheIt = pdbAnlzCache.find(targetTrackId);
        if (cacheIt != pdbAnlzCache.end())
            return cacheIt->second;

        DBG("NfsAnlzFetcher: track " + juce::String(targetTrackId) + " not found in PDB");
        return {};
    }

    /// Cache of trackId -> anlzPath (populated from export.pdb)
    std::map<uint32_t, juce::String> pdbAnlzCache;

    /// Clear PDB cache (call on media unmount or player disconnect)
    void clearPdbCache() { pdbAnlzCache.clear(); }

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
            if (len < 4 || offset + 3 + (int)(len - 4) > pageSize) return {};
            return juce::String((const char*)(pageData + offset + 4), (size_t)(len - 4));
        }
        else if (kind == 0x90)
        {
            // Long UTF-16LE: u2(len) + u1(pad) + UTF16LE[len-4 bytes]
            if (offset + 4 > pageSize) return {};
            uint16_t len = readLE16(pageData + offset + 1);
            if (len < 4 || offset + 3 + (int)(len - 4) > pageSize) return {};
            int numChars = (int)(len - 4) / 2;
            juce::String result;
            for (int i = 0; i < numChars; i++)
            {
                uint16_t ch = readLE16(pageData + offset + 4 + i * 2);
                if (ch == 0) break;
                result += juce::String::charToString((juce::juce_wchar)ch);
            }
            return result;
        }
        else
        {
            // Short ASCII: actual_len = kind >> 1, text is actual_len-1 bytes
            int actualLen = kind >> 1;
            if (actualLen <= 1 || offset + 1 + actualLen - 1 > pageSize) return {};
            return juce::String((const char*)(pageData + offset + 1), (size_t)(actualLen - 1));
        }
    }

    void parsePdbTrackPaths(const juce::MemoryBlock& pdb)
    {
        const uint8_t* d = static_cast<const uint8_t*>(pdb.getData());
        int fileSize = (int)pdb.getSize();

        if (fileSize < 28) return;

        uint32_t lenPage   = readLE32(d + 4);
        uint32_t numTables = readLE32(d + 8);

        if (lenPage < 256 || lenPage > 65536 || numTables > 100) return;

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
            return;
        }

        DBG("NfsAnlzFetcher: tracks table pages " + juce::String(tracksFirstPage)
            + " to " + juce::String(tracksLastPage));

        // Walk the page chain
        uint32_t pageIdx = tracksFirstPage;
        int maxPages = 5000;  // safety limit

        while (maxPages-- > 0)
        {
            int pageOff = (int)(pageIdx * lenPage);
            if (pageOff + (int)lenPage > fileSize) break;

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
                            pdbAnlzCache[trackId] = anlzPath;
                    }
                }
            }

            // Move to next page
            if (pageIdx == tracksLastPage) break;
            if (nextPageIdx == pageIdx) break;  // avoid infinite loop
            pageIdx = nextPageIdx;
        }

        DBG("NfsAnlzFetcher: indexed " + juce::String((int)pdbAnlzCache.size()) + " track ANLZ paths from PDB");
    }

    //==========================================================================
    // ANLZ PMAI Container Parser
    //==========================================================================

    /// Parse a complete ANLZ .EXT file (PMAI container with tagged sections).
    static AnlzResult parseAnlzFile(const juce::MemoryBlock& fileData)
    {
        AnlzResult result;
        const uint8_t* d = static_cast<const uint8_t*>(fileData.getData());
        int size = (int)fileData.getSize();

        // Verify PMAI magic
        if (size < 12 || d[0] != 'P' || d[1] != 'M' || d[2] != 'A' || d[3] != 'I')
        {
            DBG("NfsAnlzFetcher: not a PMAI file");
            return result;
        }

        uint32_t headerLen = readBE32(d + 4);
        int pos = (int)headerLen;

        // Iterate tagged sections
        while (pos + 12 <= size)
        {
            char tag[5] = { (char)d[pos], (char)d[pos+1], (char)d[pos+2], (char)d[pos+3], 0 };
            // uint32_t lenHeader = readBE32(d + pos + 4);  // not needed for real ANLZ files
            uint32_t lenTag    = readBE32(d + pos + 8);

            if (lenTag < 12 || pos + (int)lenTag > size)
            {
                DBG("NfsAnlzFetcher: invalid section at offset " + juce::String(pos)
                    + " tag=" + juce::String(tag) + " len=" + juce::String(lenTag));
                break;
            }

            const uint8_t* body = d + pos + 12;
            int bodyLen = (int)lenTag - 12;

            if (std::strcmp(tag, "PQTZ") == 0)
                result.beatGrid = parsePQTZ(body, bodyLen, 0);
            else if (std::strcmp(tag, "PCO2") == 0)
                result.cueList = parsePCO2(body, bodyLen);
            else if (std::strcmp(tag, "PCOB") == 0 && result.cueList.empty())
                result.cueList = parsePCOB(body, bodyLen);
            else if (std::strcmp(tag, "PSSI") == 0)
                parsePSSI(body, bodyLen, 0, result.songStructure, result.phraseMood);
            else if (std::strcmp(tag, "PWV7") == 0 && result.detailEntryCount == 0)
                parseDetailWaveform(body, bodyLen, 3, result);
            else if (std::strcmp(tag, "PWV5") == 0 && result.detailEntryCount == 0)
                parseDetailWaveform(body, bodyLen, 2, result);

            pos += (int)lenTag;
        }

        result.ok = true;
        DBG("NfsAnlzFetcher: parsed ANLZ -- beats=" + juce::String((int)result.beatGrid.size())
            + " cues=" + juce::String((int)result.cueList.size())
            + " phrases=" + juce::String((int)result.songStructure.size())
            + " detailEntries=" + juce::String(result.detailEntryCount));
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
    static std::vector<CueEntry> parsePCO2(const uint8_t* body, int bodyLen)
    {
        // Format: type(u4) + numCues(u2) + padding(u2) + PCP2 entries...
        if (bodyLen < 8) return {};

        // uint32_t listType = readBE32(body);  // 0=memory, 1=hot cues (unused)
        uint16_t numCues  = readBE16(body + 4);
        if (numCues == 0) return {};

        std::vector<CueEntry> cues;
        int pos = 8;  // start of PCP2 entries

        for (int i = 0; i < numCues && pos + 12 <= bodyLen; i++)
        {
            // Each PCP2 entry starts with "PCP2" magic
            if (body[pos] != 'P' || body[pos+1] != 'C' || body[pos+2] != 'P' || body[pos+3] != '2')
                break;

            uint32_t entryLen = readBE32(body + pos + 8);  // total entry size incl header
            if (entryLen < 0x1D || entryLen > 4096 || pos + (int)entryLen > bodyLen) break;

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
            if ((int)entryLen >= 0x2C)
            {
                commentBytes = readBE32(e + 0x28);
                if (commentBytes > 0 && commentBytes < 512
                    && 0x2C + (int)commentBytes <= (int)entryLen)
                {
                    int numChars = (int)commentBytes / 2;
                    juce::String comment;
                    for (int ci = 0; ci < numChars; ci++)
                    {
                        uint16_t ch = readBE16(e + 0x2C + ci * 2);
                        if (ch == 0) break;
                        comment += juce::String::charToString((juce::juce_wchar)ch);
                    }
                    cue.comment = comment.trimEnd();
                }
            }

            // Color RGB after comment
            int colorOff = 0x2C + (int)commentBytes;
            if (colorOff + 4 <= (int)entryLen)
            {
                cue.colorCode = e[colorOff];
                cue.colorR    = e[colorOff + 1];
                cue.colorG    = e[colorOff + 2];
                cue.colorB    = e[colorOff + 3];
                cue.hasColor  = (cue.colorR != 0 || cue.colorG != 0 || cue.colorB != 0);
            }

            cues.push_back(cue);
            pos += (int)entryLen;  // entryLen = total size including PCP2 header
        }

        return cues;
    }

    //==========================================================================
    // PCOB: Standard Cue List (fallback, no colors/comments)
    //==========================================================================
    static std::vector<CueEntry> parsePCOB(const uint8_t* body, int bodyLen)
    {
        // Format: type(u4) + pad(u2) + numCues(u2) + memoryCount(u4) + PCPT entries
        if (bodyLen < 12) return {};

        uint16_t numCues = readBE16(body + 6);
        if (numCues == 0) return {};

        std::vector<CueEntry> cues;
        int pos = 12;

        for (int i = 0; i < numCues && pos + 12 <= bodyLen; i++)
        {
            if (body[pos] != 'P' || body[pos+1] != 'C' || body[pos+2] != 'P' || body[pos+3] != 'T')
                break;

            uint32_t entryLen = readBE32(body + pos + 8);  // total PCPT size incl header
            if (entryLen < 0x24 || entryLen > 4096 || pos + (int)entryLen > bodyLen) break;

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

        return cues;
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

        uint32_t entrySize = readBE32(body + offset);       // usually 24
        uint16_t numEntries = readBE16(body + offset + 4);
        if (numEntries == 0 || entrySize < 24) return;

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
            int eOff = entriesOff + i * (int)entrySize;
            if (eOff + 24 > (int)unmasked.size()) break;

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
                int nextOff = entriesOff + (i + 1) * (int)entrySize;
                if (nextOff + 4 <= (int)unmasked.size())
                    phrase.beatCount = readBE16(unmasked.data() + nextOff + 2) - phrase.beatNumber;
            }

            phrases.push_back(phrase);
        }
    }

    //==========================================================================
    // Detail Waveform (PWV5 / PWV7)
    //==========================================================================
    static void parseDetailWaveform(const uint8_t* body, int bodyLen, int bpe,
                                    AnlzResult& result)
    {
        // Format: wordSize(u4) + entryCount(u4) + pad(u4) + data[entryCount*wordSize]
        if (bodyLen < 12) return;

        uint32_t wordSize   = readBE32(body);
        uint32_t entryCount = readBE32(body + 4);
        if ((int)wordSize != bpe || entryCount == 0 || entryCount > 500000) return;

        int dataOff = 12;
        int64_t dataLen64 = (int64_t)wordSize * (int64_t)entryCount;
        if (dataLen64 > (int64_t)(bodyLen - dataOff) || dataLen64 <= 0) return;

        int dataLen = (int)dataLen64;
        result.detailData.assign(body + dataOff, body + dataOff + dataLen);
        result.detailEntryCount = (int)entryCount;
        result.detailBytesPerEntry = bpe;
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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NfsAnlzFetcher)
};
