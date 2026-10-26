// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter
//
// StageLinQDbClient -- Database client for Denon StageLinQ devices.
//
// Connects to the FileTransfer service ("fltx" protocol), downloads the
// Engine Library SQLite database, and provides track metadata + artwork
// lookup.  Parallels DbServerClient for Pioneer.
//
// Protocol reference: chrisle/StageLinq (MIT) -- services/FileTransfer.ts
//
// Flow:
//   1. Connect to FileTransfer service port (from service handshake)
//   2. getSources() -> list media locations (USB/SD)
//   3. Download the first source's /{source}/Engine Library/Database2/m.db
//      (or v1 fallback); another source's when a track on it is asked for
//   4. Open each with SQLite, cache track metadata + artwork per source
//   5. On TrackNetworkPath change: lookup Track -> AlbumArt -> juce::Image
//
// Requires: sqlite3 amalgamation (sqlite3.h + sqlite3.c) in the project.

#pragma once
#include <JuceHeader.h>
#include "StageLinQInput.h"
#include "ImageGuard.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <map>
#include <memory>
#include <system_error>
#include <thread>
#include <vector>
#include <cstring>

// SQLite amalgamation -- add sqlite3.c to your build (CMake: add_library)
#include "sqlite3.h"

//==============================================================================
// FileTransfer protocol constants ("fltx")
//==============================================================================
namespace StageLinQ
{
    // FileTransfer magic
    static constexpr uint8_t kFltxMagic[4] = { 'f', 'l', 't', 'x' };

    // Request sub-types (us -> device)
    static constexpr uint32_t kFltxRequestStat        = 0x000007D1;
    static constexpr uint32_t kFltxRequestSources      = 0x000007D2;
    static constexpr uint32_t kFltxRequestTransferId   = 0x000007D4;
    static constexpr uint32_t kFltxRequestChunkRange   = 0x000007D5;
    static constexpr uint32_t kFltxTransferComplete    = 0x000007D6;

    // Response message IDs (device -> us, after fltx[4] + 0x00000000[4])
    static constexpr uint32_t kFltxRespFileStat        = 1;
    static constexpr uint32_t kFltxRespEndOfMessage    = 2;
    static constexpr uint32_t kFltxRespSourceLocations = 3;
    static constexpr uint32_t kFltxRespTransferId      = 4;
    static constexpr uint32_t kFltxRespChunk           = 5;
    static constexpr uint32_t kFltxRespUnknown8        = 8;   // unsolicited, the string "1" (see fltxWaitFor)
    static constexpr uint32_t kFltxRespDisconnect      = 9;

    static constexpr int kFltxChunkSize = 4096;

    // Caps on sizes that come off the wire or out of the device's database
    // (AUDIT WIRE-1, WIRE-2).  Each is compared in unsigned arithmetic of the
    // field's own width before any cast, multiply or add.
    //  - A downloaded database is held in memory before it is written to a
    //    temp file: 1 GiB is well above any Engine library seen, and keeps
    //    a corrupt size from asking for 4 GiB.
    //  - A FileTransfer message body: the device picks its chunk size
    //    (chrisle's FileTransfer.ts sets no upper bound).  16 MiB, not
    //    1 MiB: a chunk over 1 MiB was taken for corrupt data and the
    //    download lost (DENON26 harness, C3).
    //  - An Engine DJ BLOB declares its uncompressed size; nothing decoded
    //    here (overview waveform, cues, beat grid) comes near 10 MiB.
    static constexpr uint32_t kMaxDbFileBytes   = 1u << 30;
    static constexpr uint32_t kMaxFltxBodyBytes = 16u << 20;
    static constexpr int32_t  kMaxBlobBytes     = 10 * 1024 * 1024;

    // How long a reply is waited for (chrisle's MESSAGE_TIMEOUT is 3 s,
    // refs/StageLinq/types/common.ts:4), and the download limits: 30 s plus
    // 2 ms per 1000 bytes of the file (a link of 0.5 MB/s still completes),
    // and at most 10 s without a chunk.  chrisle's getFile() has no gap
    // limit, only 60 s in all; the gap was 5 s, which ended a transfer the
    // device paused for 6 s (C8), and the deadline that grows with the file
    // would otherwise be the only end of a transfer that stopped.
    static constexpr int    kFltxReplyWaitMs     = 3000;
    static constexpr double kDownloadBaseMs      = 30000.0;
    static constexpr double kDownloadMsPerByte   = 2.0 / 1000.0;
    static constexpr double kDownloadGapMs       = 10000.0;

    //==========================================================================
    // Build fltx request frames (with length prefix)
    //==========================================================================

    // requestStat: fltx + 0x0 + 0x7D1 + path_netstr
    inline std::vector<uint8_t> buildFltxStat(const juce::String& path)
    {
        // Body
        std::vector<uint8_t> body;
        body.insert(body.end(), kFltxMagic, kFltxMagic + 4);
        uint8_t zero4[4] = {}; body.insert(body.end(), zero4, zero4 + 4);
        uint8_t sub[4]; writeU32BE(sub, kFltxRequestStat);
        body.insert(body.end(), sub, sub + 4);
        appendNetworkString(body, path);

        // Length prefix + body
        std::vector<uint8_t> frame;
        uint8_t len[4]; writeU32BE(len, (uint32_t)body.size());
        frame.insert(frame.end(), len, len + 4);
        frame.insert(frame.end(), body.begin(), body.end());
        return frame;
    }

    // requestSources: fltx + 0x0 + 0x7D2 + 0x0
    inline std::vector<uint8_t> buildFltxSources()
    {
        std::vector<uint8_t> body;
        body.insert(body.end(), kFltxMagic, kFltxMagic + 4);
        uint8_t zero4[4] = {}; body.insert(body.end(), zero4, zero4 + 4);
        uint8_t sub[4]; writeU32BE(sub, kFltxRequestSources);
        body.insert(body.end(), sub, sub + 4);
        body.insert(body.end(), zero4, zero4 + 4);

        std::vector<uint8_t> frame;
        uint8_t len[4]; writeU32BE(len, (uint32_t)body.size());
        frame.insert(frame.end(), len, len + 4);
        frame.insert(frame.end(), body.begin(), body.end());
        return frame;
    }

    // requestFileTransferId: fltx + 0x0 + 0x7D4 + path_netstr + 0x0
    inline std::vector<uint8_t> buildFltxTransferId(const juce::String& path)
    {
        std::vector<uint8_t> body;
        body.insert(body.end(), kFltxMagic, kFltxMagic + 4);
        uint8_t zero4[4] = {}; body.insert(body.end(), zero4, zero4 + 4);
        uint8_t sub[4]; writeU32BE(sub, kFltxRequestTransferId);
        body.insert(body.end(), sub, sub + 4);
        appendNetworkString(body, path);
        body.insert(body.end(), zero4, zero4 + 4);

        std::vector<uint8_t> frame;
        uint8_t len[4]; writeU32BE(len, (uint32_t)body.size());
        frame.insert(frame.end(), len, len + 4);
        frame.insert(frame.end(), body.begin(), body.end());
        return frame;
    }

    // requestChunkRange: fltx + 0x0 + 0x7D5 + 0x0 + txid + 0x0 + startChunk + 0x0 + endChunk
    inline std::vector<uint8_t> buildFltxChunkRange(uint32_t txid, uint32_t startChunk, uint32_t endChunk)
    {
        std::vector<uint8_t> body;
        body.insert(body.end(), kFltxMagic, kFltxMagic + 4);
        uint8_t zero4[4] = {}; body.insert(body.end(), zero4, zero4 + 4);
        uint8_t sub[4]; writeU32BE(sub, kFltxRequestChunkRange);
        body.insert(body.end(), sub, sub + 4);
        body.insert(body.end(), zero4, zero4 + 4);
        uint8_t txBytes[4]; writeU32BE(txBytes, txid);
        body.insert(body.end(), txBytes, txBytes + 4);
        body.insert(body.end(), zero4, zero4 + 4);
        uint8_t startBytes[4]; writeU32BE(startBytes, startChunk);
        body.insert(body.end(), startBytes, startBytes + 4);
        body.insert(body.end(), zero4, zero4 + 4);
        uint8_t endBytes[4]; writeU32BE(endBytes, endChunk);
        body.insert(body.end(), endBytes, endBytes + 4);

        std::vector<uint8_t> frame;
        uint8_t len[4]; writeU32BE(len, (uint32_t)body.size());
        frame.insert(frame.end(), len, len + 4);
        frame.insert(frame.end(), body.begin(), body.end());
        return frame;
    }

    // signalTransferComplete: fltx + 0x0 + 0x7D6
    inline std::vector<uint8_t> buildFltxComplete()
    {
        std::vector<uint8_t> body;
        body.insert(body.end(), kFltxMagic, kFltxMagic + 4);
        uint8_t zero4[4] = {}; body.insert(body.end(), zero4, zero4 + 4);
        uint8_t sub[4]; writeU32BE(sub, kFltxTransferComplete);
        body.insert(body.end(), sub, sub + 4);

        std::vector<uint8_t> frame;
        uint8_t len[4]; writeU32BE(len, (uint32_t)body.size());
        frame.insert(frame.end(), len, len + 4);
        frame.insert(frame.end(), body.begin(), body.end());
        return frame;
    }
}

//==============================================================================
// Parsed fltx response
//==============================================================================
struct FltxResponse
{
    bool received = false;     // a complete message was read
    uint32_t code = 0;         // non-zero: the device's periodic message (chrisle: "timecode")
    uint32_t messageId = 0;

    // SourceLocations
    juce::StringArray sources;

    // FileStat
    uint32_t fileSize = 0;

    // FileTransferId
    uint32_t txFileSize = 0;
    uint32_t txId = 0;

    // FileTransferChunk
    uint32_t chunkOffset = 0;
    uint32_t chunkSize = 0;
    std::vector<uint8_t> chunkData;
};

//==============================================================================
// Cached track metadata from SQLite
//==============================================================================
struct DenonTrackMeta
{
    juce::String artist;
    juce::String title;
    juce::String album;
    juce::String genre;
    juce::String key;
    double bpm = 0.0;
    double length = 0.0;
    int albumArtId = 0;
    bool valid = false;
};

//==============================================================================
// Decoded overview waveform (3 bytes per entry: mid, high, low for WaveformDisplay)
//==============================================================================
struct DenonWaveformData
{
    std::vector<uint8_t> data;   // 3 bytes per entry, reordered to mid/high/low
    int entryCount = 0;
    bool valid = false;
};

//==============================================================================
// Quick cue point (from quickCues BLOB)
//==============================================================================
struct DenonQuickCue
{
    juce::String label;
    double sampleOffset = -1.0;  // -1 = not set
    uint8_t r = 0, g = 0, b = 0, a = 255;
    bool isSet() const { return sampleOffset >= 0.0; }
};

//==============================================================================
// Loop region (from loops BLOB)
//==============================================================================
struct DenonLoop
{
    juce::String label;
    double startSampleOffset = 0.0;
    double endSampleOffset = 0.0;
    bool startSet = false;
    bool endSet = false;
    uint8_t r = 0, g = 0, b = 0, a = 255;
};

//==============================================================================
// Beat grid marker (from beatData BLOB)
//==============================================================================
struct DenonBeatGridMarker
{
    double sampleOffset = 0.0;
    int64_t beatNumber = 0;
    int32_t numBeats = 0;
};

//==============================================================================
// Complete performance data for a track
//==============================================================================
struct DenonPerformanceData
{
    // Quick cues (up to 8)
    std::vector<DenonQuickCue> quickCues;
    double mainCueSampleOffset = 0.0;

    // Loops (up to 8)
    std::vector<DenonLoop> loops;

    // Beat grid
    std::vector<DenonBeatGridMarker> beatGrid;
    double sampleRate = 0.0;
    double totalSamples = 0.0;

    bool valid = false;
};

//==============================================================================
// Musical key index -> string (from libdjinterop musical_key enum)
// Engine DJ stores key as integer in Track table column 'key'
//==============================================================================
inline juce::String musicalKeyToString(int keyIndex)
{
    // Camelot-style ordering from Engine DJ (same as libdjinterop::musical_key)
    static const char* const keys[] = {
        "C",  "Am",  "G",   "Em",   "D",   "Bm",
        "A",  "F#m", "E",   "Dbm",  "B",   "Abm",
        "F#", "Ebm", "Db",  "Bbm",  "Ab",  "Fm",
        "Eb", "Cm",  "Bb",  "Gm",   "F",   "Dm"
    };
    if (keyIndex >= 0 && keyIndex < 24)
        return keys[keyIndex];
    return {};
}

//==============================================================================
// StageLinQDbClient -- FileTransfer + SQLite database client
//==============================================================================
// One medium of the unit (a source of its list: USB 1, SD, "... (Internal)")
// and its database in a session
struct StageLinQSourceDb
{
    enum class State { notTried, ready, noLibrary, failed };
    juce::String name;
    State state = State::notTried;
    sqlite3* db = nullptr;              // open while ready
    juce::File file;                    // the downloaded copy (deleted on close)
    bool perfDataIsTable = false;       // PerformanceData is an ordinary table
    int trackCount = 0;
    juce::String failure;               // why it failed
};

class StageLinQDbClient : public juce::Thread
{
public:
    using SourceDb = StageLinQSourceDb;

    StageLinQDbClient()
        : Thread("SLQ-DB")
    {
    }

    ~StageLinQDbClient() override
    {
        stop();
    }

    //--------------------------------------------------------------------------
    // Start: serve the database of the device at ip, FileTransfer port
    // fileTransferPort.  Called on a StageLinQ connection thread
    // (onFileTransferAvailable) each time a device's services are found,
    // i.e. on every (re)connection.  It only records what is wanted and
    // wakes this client's thread, which runs the sessions: it never waits
    // for one (StageLinQ::kConnectionStopMs counts on that).
    //  - While a session is wanted, another device's offer is ignored: one
    //    database, the first device's (AUDIT SLQ-3).  The same device on
    //    the same port (a reconnection) keeps what is loaded.  A device is
    //    its IP: a unit that restarts on another address (DHCP) counts as
    //    another device, and the database it had before stays in use for
    //    as long as that session is wanted -- a rebooted peer does not end
    //    it.
    //  - The same device on another port has restarted its services: the
    //    session ends and the database is fetched again (AUDIT SLQ-9).  The
    //    two #23 captures saw the PRIME 4+ on different ports (and
    //    addresses); within the second one port served two of STC's
    //    connections, so a reconnection on the same port is not taken for
    //    a restart.
    //  - After a session failed (no sources, download or open failed, the
    //    device gone), the last offer ignored while a session was wanted --
    //    another device's, or the same device's again -- starts a new one,
    //    also when it was ignored before the session's own device restarted
    //    (the bullet above); without one, the next device to offer
    //    FileTransfer does.  An ignored offer of the session's own device
    //    (a reconnection on its port) does not replace another device's:
    //    when the session fails, the other device is tried, not the one
    //    that just failed.  Trade-off: when the session's device itself
    //    recovers (the failure was transient) after re-offering on its
    //    port, the other unit's deferred offer is still served, not the
    //    session's device again, and that unit's database stays in use
    //    while its session holds -- AUDIT SLQ-3's first-device rule gives
    //    way.  Before, an offer made during a session that then failed was
    //    lost for good: a second unit was not fetched until it reconnected
    //    (AUDIT SLQ-9).
    // start() and stop() are serialised.  MainComponent calls stop() only
    // after StageLinQInput::stop() has joined the connection threads, so a
    // start() does not wait on a stop() there.
    //--------------------------------------------------------------------------
    bool start(const juce::String& ip, uint16_t fileTransferPort,
               const uint8_t tkn[StageLinQ::kTokenLen])
    {
        std::lock_guard<std::mutex> life(lifecycleMutex);
        {
            std::lock_guard<std::mutex> lock(targetMutex);
            if (isRunningFlag.load())
            {
                if (ip != targetIp || fileTransferPort == targetPort)
                {
                    // Ignored while this session is wanted; kept in case it
                    // fails (run).  The latest one replaces an earlier one,
                    // except that the session's own device does not replace
                    // another device's (AUDIT SLQ-9).
                    if (ip != targetIp || !haveDeferred || deferredIp == ip)
                    {
                        deferredIp = ip;
                        deferredPort = fileTransferPort;
                        std::memcpy(deferredToken, tkn, StageLinQ::kTokenLen);
                        haveDeferred = true;
                    }
                    return true;
                }
                DBG("StageLinQ DB: " + ip + " is back on FileTransfer port "
                    + juce::String(fileTransferPort) + " -- fetching its database again");
            }
            // The offer served now supersedes an earlier one of its own
            // device; another device's is kept, in case this session fails.
            if (haveDeferred && deferredIp == ip)
                haveDeferred = false;
            targetIp = ip;
            targetPort = fileTransferPort;
            std::memcpy(targetToken, tkn, StageLinQ::kTokenLen);
            targetSerial.store(targetSerial.load() + 1);
            isRunningFlag.store(true);
        }
        // The thread lives from the first start() to stop(), between
        // sessions too, so it is either running or fully stopped here
        if (isThreadRunning())
            notify();                       // idle, or in a session to end
        else if (!startThread(juce::Thread::Priority::normal))
            isRunningFlag.store(false);     // no thread: the next offer tries again
        return true;
    }

    // Message thread.  Ends the session and the thread; the database and
    // its temp file are closed, the caches kept until the next session.
    void stop()
    {
        std::lock_guard<std::mutex> life(lifecycleMutex);
        {
            std::lock_guard<std::mutex> lock(targetMutex);
            isRunningFlag.store(false);
            haveDeferred = false;
        }
        if (!isThreadRunning()) return;
        signalThreadShouldExit();
        closeFtSocket();                    // ends a blocking read at once

        if (!stopThread(5000))
            closeDatabase();                // the thread closes it, unless it was killed

        DBG("StageLinQ DB: Stopped");
    }

    bool getIsRunning() const { return isRunningFlag.load(); }

    //--------------------------------------------------------------------------
    // Public getters (thread-safe; the message thread calls them)
    //--------------------------------------------------------------------------
    // Each looks the track up by its TrackNetworkPath in what the database
    // of its own source gave (net://<uuid>/<source>/...: USB 1, SD, ...); a
    // streaming track by its URI.  No track is answered from another
    // source's database.  The uuid in the path (the unit that holds the
    // medium) is not compared: a track of another unit's USB 1 is looked
    // up in the served unit's USB 1, as before 1.9.16.

    // Lookup track by network path (e.g. from StateMap TrackNetworkPath)
    DenonTrackMeta getTrackByNetworkPath(const juce::String& networkPath) const
    {
        const auto key = parseNetworkPath(networkPath).key();
        if (key.empty()) return {};

        std::lock_guard<std::mutex> lock(cacheMutex);
        auto it = trackCache.find(key);
        if (it != trackCache.end())
            return it->second;
        return {};
    }

    // Get artwork image by network path.  Album art ids are per database,
    // so the artwork is kept per track.
    juce::Image getArtworkForTrack(const juce::String& networkPath) const
    {
        const auto key = parseNetworkPath(networkPath).key();
        if (key.empty()) return {};

        std::lock_guard<std::mutex> lock(cacheMutex);
        auto it = artworkCache.find(key);
        if (it != artworkCache.end())
            return it->second;
        return {};
    }

    // Get overview waveform for a track (3 bytes per entry: mid/high/low)
    DenonWaveformData getWaveformForTrack(const juce::String& networkPath) const
    {
        const auto key = parseNetworkPath(networkPath).key();
        if (key.empty()) return {};

        std::lock_guard<std::mutex> lock(cacheMutex);
        auto it = waveformCache.find(key);
        if (it != waveformCache.end())
            return it->second;
        return {};
    }

    // Get performance data (quick cues, loops, beat grid) for a track
    DenonPerformanceData getPerformanceData(const juce::String& networkPath) const
    {
        const auto key = parseNetworkPath(networkPath).key();
        if (key.empty()) return {};

        std::lock_guard<std::mutex> lock(cacheMutex);
        auto it = perfCache.find(key);
        if (it != perfCache.end())
            return it->second;
        return {};
    }

    // Request metadata load for a track (triggers background DB query)
    void requestMetadata(const juce::String& networkPath)
    {
        if (networkPath.isEmpty()) return;
        std::lock_guard<std::mutex> lock(requestMutex);
        pendingRequests.add(networkPath);
    }

    // True once a source's database has been downloaded and opened in this
    // session (another source's may still be downloading)
    bool isDatabaseReady() const { return dbReady.load(); }

private:
    // Threads: everything below runs on the client's thread (SLQ-DB)
    // unless its comment says otherwise.  The static helpers
    // (parseNetworkPath, oldFormDbPath, sqliteImmutableUri, the table
    // checks, the BLOB decoders) keep no state and run on any thread: the
    // getters call parseNetworkPath on the message thread.

    // A track as its TrackNetworkPath names it (parseNetworkPath)
    struct TrackRef
    {
        juce::String source;                // parts[3]; empty for a streaming track
        juce::String dbPath;                // Track.path, or Track.uri when streaming
        bool streaming = false;

        bool valid() const { return dbPath.isNotEmpty(); }
        // The cache key: the source and the path (one medium's row never
        // stands for another's)
        std::string key() const
        {
            if (!valid()) return {};
            return ((streaming ? juce::String("streaming") : source) + "\n" + dbPath).toStdString();
        }
    };

    //==========================================================================
    // Thread main loop
    //==========================================================================
    // Runs from the first start() to stop(): one session for each device
    // and port start() asks for, and a wait for the next one in between.
    // A session returns when it fails, when stop() is called, or when
    // start() wants another (a restarted device); it leaves no socket,
    // database or temp file behind.
    void run() override
    {
        while (!threadShouldExit())
        {
            bool haveTarget = false;
            {
                std::lock_guard<std::mutex> lock(targetMutex);
                if (isRunningFlag.load() && targetSerial.load() != servedSerial)
                {
                    deviceIp = targetIp;
                    ftPort = targetPort;
                    std::memcpy(token, targetToken, StageLinQ::kTokenLen);
                    servedSerial = targetSerial.load();
                    haveTarget = true;
                }
            }
            if (!haveTarget)
            {
                wait(-1);                   // until start() or stop()
                continue;
            }

            fltxReadBuf.clear();            // nothing of the last session's stream
            unansweredStats.clear();
            seenRequests.clear();
            lastSourcesAskMs = 0.0;
            sourcesAskIntervalMs = kSourcesAskFirstMs;
            nextConnectMs = 0.0;
            {
                std::lock_guard<std::mutex> lock(cacheMutex);   // built from the last database
                trackCache.clear();
                artworkCache.clear();
                waveformCache.clear();
                perfCache.clear();
            }
            runSession();
            closeFtSocket();
            closeDatabase();

            std::lock_guard<std::mutex> lock(targetMutex);
            if (targetSerial.load() == servedSerial)
            {
                // It failed (or stop()).  An offer that start() ignored while
                // it ran is served now; otherwise the next start() begins
                // anew.  stop() clears both the flag and the offer.
                if (haveDeferred && isRunningFlag.load() && !threadShouldExit())
                {
                    targetIp = deferredIp;
                    targetPort = deferredPort;
                    std::memcpy(targetToken, deferredToken, StageLinQ::kTokenLen);
                    targetSerial.store(targetSerial.load() + 1);
                    haveDeferred = false;
                }
                else
                    isRunningFlag.store(false);
            }
        }
    }

    // True when the session must end: stop(), or start() wants another
    bool sessionShouldEnd() const
    {
        return threadShouldExit() || targetSerial.load() != servedSerial;
    }

    //==========================================================================
    // A session: one device and port, as start() asks for.
    //  - At the start the first source that has a database is downloaded,
    //    as before 1.9.16, so the first track is not delayed.
    //  - A track names its source in its TrackNetworkPath (parts[3] of
    //    net://<uuid>/<source>/...: USB 1, SD, "(Internal)", ...).  A
    //    request for a source whose database is not loaded yet has that
    //    database downloaded, and waits meanwhile: it is never answered from
    //    another medium's database.  Requests the loaded databases can
    //    answer are answered first, and also while a download runs.
    //  - A source the last source list did not name has the list asked for
    //    again, at most once every 5 s, and less often (10, 20, 40, then
    //    every 60 s) while that brings no new source; a new request starts
    //    over.  This has no end while such a request waits (a deck holds a
    //    track of another unit's medium, say): it is how a medium inserted
    //    later is found.
    //  - A streaming track is looked up by Track.uri in the database of the
    //    unit's internal storage (a source named "... (Internal)") when the
    //    unit lists one and it has a database, else in each one loaded.
    //  - The databases stay open until the session ends.  When the
    //    FileTransfer connection closes they stay in use; a connection is
    //    opened again only when a request needs the device.
    // A connection on which a request went unanswered is not used again (a
    // late reply would be taken for the next request's, VERIFY V10): when
    // that request was a stat, the path is noted as unanswered and the next
    // connection goes on with the paths not tried yet.  Likewise after a
    // transfer that did not complete (its chunks could still come, and be
    // placed in the next file): the source is noted as failed, and while no
    // database is loaded yet a new connection goes on with the next source,
    // when the last list names one not tried yet (at most kMaxTransfersLost
    // times in a session).
    //==========================================================================
    void runSession()
    {
        bool firstDone = false;             // the first database is loaded
        int transfersLost = 0;              // before it was
        while (!sessionShouldEnd())
        {
            if (!firstDone)
            {
                const DbSearch found = connectAndLoadFirst();
                if ((found == DbSearch::statUnanswered && unansweredStats.size() <= kMaxUnansweredStats)
                    || (found == DbSearch::transferLost && untriedSourceListed()
                        && ++transfersLost <= kMaxTransfersLost))
                {
                    closeFtSocket();        // a new connection goes on with the next path or source
                    continue;
                }
                if (found != DbSearch::found) break;
                firstDone = true;
                continue;
            }

            drainFltx();
            takeRequests();
            const Needs needs = answerFromLoaded();
            if (sessionShouldEnd()) break;
            if (!doDeviceWork(needs))
                wait(100);                  // cut short by stop() and start()
        }

        // Left for the next session: a request made just after a restarted
        // device's start() is for its new database
        std::lock_guard<std::mutex> lock(requestMutex);
        for (auto& r : waitingRequests)
            pendingRequests.addIfNotAlreadyThere(r);
        waitingRequests.clear();
    }

    // What a database search came to
    enum class DbSearch
    {
        found,              // a database is loaded
        none,               // none there (or none usable)
        statUnanswered,     // a stat got no reply: the path is noted, the connection is not used again
        transferLost,       // a transfer did not complete: the source is noted as failed (its
                            // chunks could still come), the connection is not used again
        lost                // the connection failed, or the session ends
    };

    // Opens a FileTransfer connection, sends the service announcement and
    // asks for the source list (updateSources); false when any of it fails
    bool openConnection()
    {
        closeFtSocket();
        fltxReadBuf.clear();                // nothing of an earlier connection's stream
        DBG("StageLinQ DB: Connecting to FileTransfer at " + deviceIp + ":" + juce::String(ftPort));

        // --- Connect to FileTransfer service ---
        {
            auto sock = connectToUnit();
            if (sessionShouldEnd()) return false;   // stop() or start() came during the connect
            if (sock == nullptr)
            {
                DBG("StageLinQ DB: FileTransfer connect failed");
                return false;
            }

            // Send service announcement (same as StateMap/BeatInfo)
            auto announce = StageLinQ::buildServiceAnnouncement(token, "FileTransfer", 0);
            sock->write(announce.data(), (int)announce.size());

            {
                std::lock_guard<std::mutex> lock(sockMutex);
                ftSocket = std::move(sock);
            }
        }

        // Per chrisle/StageLinq: 500 ms before the first request.  Waited to
        // a deadline, not with one wait(): this thread's event is also
        // signalled by start(), and a notify() left unconsumed (JUCE's
        // startThread() does not reset the event) would end a single wait()
        // at once.  stop() still cuts it short.
        const double requestsAt = juce::Time::getMillisecondCounterHiRes() + 500.0;
        for (double left; !sessionShouldEnd()
                          && (left = requestsAt - juce::Time::getMillisecondCounterHiRes()) > 0.0;)
            wait((int)std::ceil(left));
        if (sessionShouldEnd()) return false;

        return askSources();
    }

    //--------------------------------------------------------------------------
    // A TCP connection to the unit's FileTransfer port, or nullptr.  JUCE's
    // StreamingSocket::connect() cannot be cut short from another thread
    // (JUCE 9.0.3 juce_Socket.cpp: the socket's handle is set only once the
    // connect is done), and to a unit that is gone it takes the whole
    // kSocketTimeoutMs.  So it runs on a thread of its own, a detached
    // std::thread that touches nothing of this object, and this thread
    // waits for it in steps of 10 ms.  At stop(), or when start() wants
    // another session, it stops waiting and returns nullptr, so stop()
    // does not wait for the connect (it did, up to 2 s on the message
    // thread, when it came during a connect to a unit switched off).  An
    // abandoned connect ends within kSocketTimeoutMs and closes its socket;
    // a unit that answers meanwhile sees a connection opened and closed
    // with nothing sent on it, as when stop() came during the connect
    // before.  This thread.
    //--------------------------------------------------------------------------
    std::unique_ptr<juce::StreamingSocket> connectToUnit()
    {
        struct Attempt
        {
            std::mutex mutex;
            std::condition_variable doneCv;
            bool done = false;
            std::unique_ptr<juce::StreamingSocket> sock;   // connected, or null
        };
        auto attempt = std::make_shared<Attempt>();
        try
        {
            std::thread([attempt, ip = deviceIp, port = (int)ftPort]
            {
                auto sock = std::make_unique<juce::StreamingSocket>();
                if (!sock->connect(ip, port, StageLinQ::kSocketTimeoutMs))
                    sock.reset();
                std::lock_guard<std::mutex> lock(attempt->mutex);
                attempt->sock = std::move(sock);
                attempt->done = true;
                attempt->doneCv.notify_all();
            }).detach();
        }
        catch (const std::system_error&)
        {
            return nullptr;                 // no thread to connect on
        }
        std::unique_lock<std::mutex> lock(attempt->mutex);
        while (!attempt->doneCv.wait_for(lock, std::chrono::milliseconds(10),
                                         [&attempt] { return attempt->done; }))
        {
            if (sessionShouldEnd())
                return nullptr;             // the connect's thread closes what it opens
        }
        return std::move(attempt->sock);
    }

    // Asks for the source list on the open connection; false when it is not
    // answered (the connection is then closed) or is empty (it stays open)
    bool askSources()
    {
        lastSourcesAskMs = juce::Time::getMillisecondCounterHiRes();
        bool answered = false;
        juce::StringArray sources = fetchSources(answered);
        if (!answered)
        {
            DBG("StageLinQ DB: No source list from the device");
            closeFtSocket();
            return false;
        }
        if (sources.isEmpty())
        {
            DBG("StageLinQ DB: No sources found on device");
            return false;
        }
        updateSources(sources);
        return true;
    }

    // The unit's media as its last list names them; a source no longer
    // listed keeps its database (loaded ones stay open for the session).
    // The list comes off the wire: only its first kMaxSources distinct
    // names are taken, and a session knows at most kMaxSources (a name
    // past that is not listed).  A unit lists a handful; a list of 50,000
    // names, each looked up among the others, kept this thread busy past
    // stop()'s 5 s and the thread was killed (VERIFY-DB-3 Y01).
    void updateSources(const juce::StringArray& sources)
    {
        listedSources.clear();
        for (auto& name : sources)
        {
            if (listedSources.size() >= kMaxSources) break;
            if (listedSources.contains(name)) continue;
            if (findSource(name) == nullptr)
            {
                if ((int)sessionSources.size() >= kMaxSources) continue;
                SourceDb s;
                s.name = name;
                sessionSources.push_back(std::move(s));
            }
            DBG("StageLinQ DB: Source: " + name);
            listedSources.add(name);
        }
    }

    SourceDb* findSource(const juce::String& name)
    {
        for (auto& s : sessionSources)
            if (s.name == name) return &s;
        return nullptr;
    }

    // A source of the unit's last list that this session has not tried yet
    bool untriedSourceListed()
    {
        for (auto& name : listedSources)
            if (auto* s = findSource(name); s != nullptr && s->state == SourceDb::State::notTried)
                return true;
        return false;
    }

    // Connects and loads the first listed source that has a database.  A
    // source tried earlier in this session (on a connection that ended
    // with a stat unanswered or a transfer lost) is not tried again here:
    // the search goes on with the next one, as before 1.9.16, where a
    // download that failed moved on to the next source.
    DbSearch connectAndLoadFirst()
    {
        if (!openConnection())
            return DbSearch::none;
        for (int i = 0; i < listedSources.size(); ++i)
        {
            auto* s = findSource(listedSources[i]);
            if (s == nullptr || s->state != SourceDb::State::notTried) continue;
            const DbSearch r = loadSource(*s);
            if (r == DbSearch::none) continue;
            return r;
        }
        DBG("StageLinQ DB: Failed to download database");
        return DbSearch::none;
    }

    // Requests made since the last look, after the ones still waiting
    void takeRequests()
    {
        std::lock_guard<std::mutex> lock(requestMutex);
        for (auto& r : pendingRequests)
        {
            if (!seenRequests.contains(r))
            {
                // A track not asked for before: the source list may be
                // asked for again 5 s after the last time
                seenRequests.add(r);
                sourcesAskIntervalMs = kSourcesAskFirstMs;
            }
            waitingRequests.addIfNotAlreadyThere(r);
        }
        pendingRequests.clear();
    }

    // What the requests still waiting need from the device
    struct Needs
    {
        juce::String source;                // the first source to download
        bool sources = false;               // the source list, asked again
    };

    // Answers every waiting request the loaded databases can answer (no
    // device I/O); the others stay waiting.  It stops between two requests
    // when the session must end: stop() waits for this thread (5 s, then it
    // is killed), and one lookup can take over a second: an artwork at
    // ImageGuard's limits (64 MiB of pixel data) is checked by ImageGuard
    // and decoded by JUCE, up to 0.8 s each here.  stop() cuts the
    // check short (queryArtwork), not the decode.  The requests not looked
    // at stay waiting.
    Needs answerFromLoaded()
    {
        Needs needs;
        for (int i = 0; i < waitingRequests.size() && !sessionShouldEnd();)
        {
            juce::String needSource;
            bool needSources = false;
            if (resolveRequest(waitingRequests[i], needSource, needSources))
            {
                waitingRequests.remove(i);
                continue;
            }
            if (needSource.isNotEmpty() && needs.source.isEmpty()) needs.source = needSource;
            needs.sources = needs.sources || needSources;
            ++i;
        }
        return needs;
    }

    // One request: true when it is answered (found, or not to be found in
    // this session); else what it waits for
    bool resolveRequest(const juce::String& networkPath, juce::String& needSource, bool& needSources)
    {
        const TrackRef ref = parseNetworkPath(networkPath);
        if (!ref.valid()) return true;
        const std::string key = ref.key();
        {
            std::lock_guard<std::mutex> lock(cacheMutex);
            if (trackCache.count(key) > 0) return true;
        }

        if (ref.streaming)
        {
            // The unit's internal storage when it lists one ("... (Internal)",
            // as chrisle's Databases.getDbPath picks it) and it has a database
            for (auto& name : listedSources)
            {
                if (!name.contains("(Internal)")) continue;
                auto* s = findSource(name);
                if (s != nullptr && s->state == SourceDb::State::notTried)
                {
                    needSource = name;
                    return false;
                }
                if (s != nullptr && s->state == SourceDb::State::ready)
                {
                    lookUpTrack(*s, ref, key);
                    return true;
                }
                break;
            }
            bool anyLoaded = false;                 // else each database loaded
            for (auto& s : sessionSources)
            {
                if (s.state != SourceDb::State::ready) continue;
                anyLoaded = true;
                if (lookUpTrack(s, ref, key)) break;
            }
            return anyLoaded;                       // none loaded yet (the first download runs): it waits
        }

        auto* s = findSource(ref.source);
        if (s == nullptr || (s->state == SourceDb::State::notTried && !listedSources.contains(ref.source)))
        {
            needSources = true;                     // not on the unit's last list
            return false;
        }
        switch (s->state)
        {
            case SourceDb::State::ready:
                lookUpTrack(*s, ref, key);
                return true;
            case SourceDb::State::notTried:
                needSource = s->name;
                return false;
            case SourceDb::State::noLibrary:
            case SourceDb::State::failed:
            default:
                return true;                        // its medium has no database here
        }
    }

    // Downloads one source's database, or asks for the source list again,
    // when the requests waiting need it; false when there was nothing to do.
    // A list that brings no new source doubles the wait before the next
    // ask, also when it was asked on a new connection (openConnection asks
    // for it).
    bool doDeviceWork(const Needs& needs)
    {
        const double now = juce::Time::getMillisecondCounterHiRes();
        const bool askList = needs.source.isEmpty();
        if (askList && !(needs.sources && now - lastSourcesAskMs >= sourcesAskIntervalMs))
            return false;
        const int sourcesBefore = (int)sessionSources.size();
        bool listed = false;                        // the list was asked for and answered
        if (!fltxConnected())
        {
            if (now < nextConnectMs) return false;
            const bool ok = openConnection();
            if (!ok)
            {
                closeFtSocket();
                nextConnectMs = juce::Time::getMillisecondCounterHiRes() + kReconnectWaitMs;
                return true;
            }
            listed = true;
        }
        else if (askList)
        {
            listed = askSources();
        }
        if (askList)
        {
            if (listed && (int)sessionSources.size() == sourcesBefore)   // nothing new: asked less often
                sourcesAskIntervalMs = juce::jmin(sourcesAskIntervalMs * 2.0, kSourcesAskMaxMs);
            return true;
        }
        if (auto* s = findSource(needs.source))
        {
            const DbSearch r = loadSource(*s);
            if (r != DbSearch::found && r != DbSearch::none)
                closeFtSocket();                    // no further request on this connection
        }
        return true;
    }

    // Looks for a source's database (v2 path first, then v1), downloads it
    // and opens it.  A path the device did not answer a stat for earlier in
    // this session is skipped (unansweredStats).
    DbSearch loadSource(SourceDb& s)
    {
        const juce::String paths[] = {
            "/" + s.name + "/Engine Library/Database2/m.db",
            "/" + s.name + "/Engine Library/m.db"
        };
        bool skipped = false;
        for (auto& dbPath : paths)
        {
            if (sessionShouldEnd()) return DbSearch::lost;
            if (unansweredStats.contains(dbPath)) { skipped = true; continue; }

            // Check if file exists (stat)
            uint32_t fileSize = 0;
            const Reply stat = fetchFileStat(dbPath, fileSize);
            if (stat == Reply::lost)
            {
                if (!fltxConnected() || sessionShouldEnd()) return DbSearch::lost;
                DBG("StageLinQ DB: No reply to the stat of " + dbPath + " -- not asked again on this unit");
                unansweredStats.add(dbPath);
                return DbSearch::statUnanswered;
            }
            if (stat == Reply::refused) continue;

            DBG("StageLinQ DB: Found database " + dbPath + " (" + juce::String(fileSize) + " bytes)");

            // Download the file
            std::vector<uint8_t> data;
            const Reply got = downloadFile(dbPath, data);
            if (got == Reply::lost)
            {
                s.state = SourceDb::State::failed;
                s.failure = "download failed";
                return sessionShouldEnd() ? DbSearch::lost : DbSearch::transferLost;
            }
            if (got == Reply::refused) continue;

            // Save to a temp file of its own, one per source (it was always
            // STC_Denon/m.db, shared by every STC instance and never
            // deleted); deleted again by closeDatabase()
            removeStaleTempCopies();
            auto tempFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                .getNonexistentChildFile("STC_Denon_m", ".db", false);
            if (!tempFile.replaceWithData(data.data(), data.size()))
            {
                DBG("StageLinQ DB: Could not write " + tempFile.getFullPathName());
                tempFile.deleteFile();
                s.state = SourceDb::State::failed;
                s.failure = "could not write a copy";
                return DbSearch::none;
            }
            DBG("StageLinQ DB: Downloaded " + juce::String((int)data.size()) + " bytes to " + tempFile.getFullPathName());

            // --- Open SQLite database ---
            if (!openSourceDatabase(s, tempFile))
            {
                DBG("StageLinQ DB: Failed to open database");
                s.state = SourceDb::State::failed;
                s.failure = "not a usable Engine database";
                return DbSearch::none;
            }
            s.state = SourceDb::State::ready;
            dbReady.store(true);
            DBG("StageLinQ DB: Database of " + s.name + " ready (" + tempFile.getFullPathName() + ")");
            return DbSearch::found;
        }
        s.state = skipped ? SourceDb::State::failed : SourceDb::State::noLibrary;
        if (skipped) s.failure = "the unit did not answer";
        return DbSearch::none;
    }

    //==========================================================================
    // FileTransfer protocol: fetch sources
    //==========================================================================
    // answered: the device replied (an empty list is a reply)
    juce::StringArray fetchSources(bool& answered)
    {
        answered = false;
        auto frame = StageLinQ::buildFltxSources();
        if (!fltxWrite(frame)) return {};

        auto resp = fltxWaitFor(StageLinQ::kFltxRespSourceLocations, StageLinQ::kFltxReplyWaitMs);
        answered = resp.received;
        return resp.sources;
    }

    //--------------------------------------------------------------------------
    // Copies left by a session that never closed its database (STC killed
    // or crashed, or this thread killed by force): each download has a name
    // of its own, so nothing else deletes them, and Windows never cleans
    // its temp directory.  Removed before each download once they are a day
    // old, except this session's own: the other sources' databases, open
    // while the session lasts (a day, maybe).  A copy another running STC
    // still has open cannot be deleted on Windows, and on POSIX stays
    // readable through that STC's handle.  The STC_Denon/m.db of earlier
    // versions (and any -journal/-wal/-shm beside it) goes the same way,
    // then its folder if that leaves it empty.  On this thread.
    //--------------------------------------------------------------------------
    void removeStaleTempCopies() const
    {
        const auto tempDir = juce::File::getSpecialLocation(juce::File::tempDirectory);
        const auto legacyDir = tempDir.getChildFile("STC_Denon");
        const auto dayAgo = juce::Time::getCurrentTime() - juce::RelativeTime::days(1.0);
        auto copies = tempDir.findChildFiles(juce::File::findFiles, false, "STC_Denon_m*.db");
        if (legacyDir.isDirectory())
            copies.addArray(legacyDir.findChildFiles(juce::File::findFiles, false, "m.db*"));
        for (const auto& f : copies)
        {
            bool ours = false;
            for (const auto& s : sessionSources)
                ours = ours || (s.file != juce::File() && f == s.file);
            if (!ours && f.getLastModificationTime() < dayAgo)
                f.deleteFile();
        }
        if (legacyDir.isDirectory())
            legacyDir.deleteFile();   // removes the folder only when empty
    }

    //==========================================================================
    // FileTransfer protocol: get file size
    //==========================================================================
    // A request's outcome on the FileTransfer connection
    enum class Reply
    {
        answered,   // the device answered
        refused,    // it answered "no": a size of 0 (no such file), or over a cap
        lost        // no reply in time, or the connection failed: no further
                    // request is sent on this connection (its reply could
                    // still come, and be taken for the next one's)
    };

    Reply fetchFileStat(const juce::String& path, uint32_t& fileSize)
    {
        fileSize = 0;
        auto frame = StageLinQ::buildFltxStat(path);
        if (!fltxWrite(frame)) return Reply::lost;

        auto resp = fltxWaitFor(StageLinQ::kFltxRespFileStat, StageLinQ::kFltxReplyWaitMs);
        if (!resp.received) return Reply::lost;
        fileSize = resp.fileSize;
        return fileSize > 0 ? Reply::answered : Reply::refused;
    }

    //==========================================================================
    // FileTransfer protocol: download complete file
    //==========================================================================
    // The file's bytes in fileData (its size from the transfer-id reply)
    Reply downloadFile(const juce::String& path, std::vector<uint8_t>& fileData)
    {
        fileData.clear();
        // Request transfer ID
        auto frame = StageLinQ::buildFltxTransferId(path);
        if (!fltxWrite(frame)) return Reply::lost;

        auto resp = fltxWaitFor(StageLinQ::kFltxRespTransferId, StageLinQ::kFltxReplyWaitMs);
        if (!resp.received)
            return Reply::lost;
        if (resp.txFileSize == 0)
            return Reply::refused;

        uint32_t fileSize = resp.txFileSize;
        uint32_t txId = resp.txId;
        if (fileSize > StageLinQ::kMaxDbFileBytes)
        {
            DBG("StageLinQ DB: " + path + " is " + juce::String((juce::int64)fileSize)
                + " bytes, over the " + juce::String((juce::int64)StageLinQ::kMaxDbFileBytes) + " cap -- skipped");
            return Reply::refused;
        }
        uint32_t totalChunks = (fileSize + StageLinQ::kFltxChunkSize - 1) / StageLinQ::kFltxChunkSize;

        DBG("StageLinQ DB: Transfer ID " + juce::String(txId) + ", size " + juce::String(fileSize)
            + ", chunks " + juce::String(totalChunks));

        // The buffer first (fileSize is capped above; an allocation that
        // still fails ends this download, not the thread, and nothing has
        // been asked for yet), then all chunks
        try { fileData.assign(fileSize, 0); }
        catch (const std::bad_alloc&) { fileData = {}; return Reply::refused; }
        auto chunkReq = StageLinQ::buildFltxChunkRange(txId, 0, totalChunks > 0 ? totalChunks - 1 : 0);
        if (!fltxWrite(chunkReq)) { fileData = {}; return Reply::lost; }

        uint32_t bytesReceived = 0;
        const double deadline = juce::Time::getMillisecondCounterHiRes() + StageLinQ::kDownloadBaseMs
                                + fileSize * StageLinQ::kDownloadMsPerByte;
        double lastChunkTime = juce::Time::getMillisecondCounterHiRes();

        // Anything but a chunk is skipped, as chrisle's getFile() does: the
        // device's periodic code != 0 message, which comes every 4 s whatever
        // STC asks (#23 PRIME 4+ capture), ended the download as a "timeout"
        // (AUDIT SLQ-1), and so did an end-of-message, which the unit also
        // sends after a reply (after the source list in the capture).  The
        // download ends when the file is complete, when no chunk has come for
        // 10 s, at the deadline above, or when the device disconnects.
        double lastAnswerMs = juce::Time::getMillisecondCounterHiRes();
        while (bytesReceived < fileSize
               && juce::Time::getMillisecondCounterHiRes() < deadline
               && !sessionShouldEnd())
        {
            // Requests the databases already loaded can answer are answered
            // while the download runs (that needs no device I/O)
            if (juce::Time::getMillisecondCounterHiRes() - lastAnswerMs > 250.0)
            {
                lastAnswerMs = juce::Time::getMillisecondCounterHiRes();
                takeRequests();
                answerFromLoaded();
            }
            auto chunkResp = fltxReadResponse(1000);
            if (chunkResp.received && chunkResp.code == 0
                && chunkResp.messageId == StageLinQ::kFltxRespChunk && !chunkResp.chunkData.empty())
            {
                // Placed at the offset the device gives, compared without
                // the sum overflowing uint32 (AUDIT WIRE-2: offset + size
                // wrapped and the memcpy wrote past the buffer).  A chunk
                // that runs past the declared size keeps the part that fits:
                // chrisle's FileTransfer.ts has seen devices overrun the
                // file size on the final chunk.
                const uint32_t offset = chunkResp.chunkOffset;
                if (offset < fileSize)
                {
                    const uint32_t n = (uint32_t)juce::jmin((size_t)(fileSize - offset),
                                                            chunkResp.chunkData.size());
                    std::memcpy(fileData.data() + offset, chunkResp.chunkData.data(), n);
                    bytesReceived += n;
                }
                lastChunkTime = juce::Time::getMillisecondCounterHiRes();
                continue;
            }
            if (chunkResp.messageId == StageLinQ::kFltxRespDisconnect || !fltxConnected())
                break;
            if (juce::Time::getMillisecondCounterHiRes() - lastChunkTime > StageLinQ::kDownloadGapMs)
                break;
        }

        // Signal transfer complete
        fltxWrite(StageLinQ::buildFltxComplete());

        if (bytesReceived < fileSize)
        {
            DBG("StageLinQ DB: Incomplete download: " + juce::String(bytesReceived)
                + "/" + juce::String(fileSize) + " -- discarding (truncated SQLite is unusable)");
            fileData = {};
            return Reply::lost;     // chunks of it may still come
        }

        return Reply::answered;
    }

    //==========================================================================
    // fltx TCP I/O
    //==========================================================================
    bool fltxWrite(const std::vector<uint8_t>& data)
    {
        std::lock_guard<std::mutex> lock(sockMutex);
        if (!ftSocket || !ftSocket->isConnected()) return false;
        int written = ftSocket->write(data.data(), (int)data.size());
        return written == (int)data.size();
    }

    bool fltxConnected()
    {
        std::lock_guard<std::mutex> lock(sockMutex);
        return ftSocket != nullptr && ftSocket->isConnected();
    }

    void closeFtSocket()
    {
        std::lock_guard<std::mutex> lock(sockMutex);
        if (ftSocket)
        {
            ftSocket->close();
            ftSocket.reset();
        }
    }

    //--------------------------------------------------------------------------
    // Wait for the reply to a request: the first message with that ID.
    // Anything else on the connection is skipped, as chrisle's
    // waitForMessage() does.  A PRIME 4+ sends message 0x8 (the string "1")
    // as soon as the connection opens and a code != 0 message every 4 s
    // (#23 capture); the first of those was taken for the reply to the
    // source request, so STC concluded "no sources" and never asked for the
    // database (AUDIT SLQ-1).  Returns an empty response on timeout, on the
    // device's disconnect message (0x9) and when the socket is gone.
    //--------------------------------------------------------------------------
    FltxResponse fltxWaitFor(uint32_t wantedId, int timeoutMs)
    {
        const double deadline = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
        while (!sessionShouldEnd())
        {
            const double left = deadline - juce::Time::getMillisecondCounterHiRes();
            if (left <= 0.0) break;
            auto resp = fltxReadResponse((int)std::ceil(left));
            if (!resp.received || resp.messageId == StageLinQ::kFltxRespDisconnect)
                break;
            if (resp.code == 0 && resp.messageId == wantedId)
                return resp;
        }
        return {};
    }

    //--------------------------------------------------------------------------
    // Read the next complete message (received = false on timeout, or when
    // the socket is gone).  What is already buffered is parsed before the
    // socket is waited on.  A failed read or the peer's close closes the
    // socket: JUCE's isConnected() stays true after a FIN.
    //--------------------------------------------------------------------------
    FltxResponse fltxReadResponse(int timeoutMs)
    {
        FltxResponse resp;
        double deadline = juce::Time::getMillisecondCounterHiRes() + timeoutMs;

        while (!sessionShouldEnd())
        {
            // Try to parse a complete message
            while (fltxReadBuf.size() >= 4)
            {
                uint32_t bodyLen = StageLinQ::readU32BE(fltxReadBuf.data());

                if (bodyLen == 0)
                {
                    // Service announcement or empty frame -- skip the 4-byte header
                    fltxReadBuf.erase(fltxReadBuf.begin(), fltxReadBuf.begin() + 4);
                    continue;
                }

                if (bodyLen > StageLinQ::kMaxFltxBodyBytes)
                {
                    // Malformed data -- discard first byte and try to resync
                    fltxReadBuf.erase(fltxReadBuf.begin());
                    continue;
                }

                if (fltxReadBuf.size() < bodyLen + 4)
                    break;   // incomplete, read more

                resp = parseFltxBody(fltxReadBuf.data() + 4, bodyLen);
                resp.received = true;
                fltxReadBuf.erase(fltxReadBuf.begin(), fltxReadBuf.begin() + 4 + bodyLen);

                // Device sent FileTransfer disconnect -- close socket to
                // prevent stale reads.  chrisle changelog: "Handle Shutdown
                // msg (0x9) from FileTransfer svc".
                if (resp.messageId == StageLinQ::kFltxRespDisconnect)
                    closeFtSocket();

                return resp;
            }

            const double left = deadline - juce::Time::getMillisecondCounterHiRes();
            if (left <= 0.0) break;

            std::lock_guard<std::mutex> lock(sockMutex);
            if (!ftSocket || !ftSocket->isConnected()) break;

            const int ready = ftSocket->waitUntilReady(true, juce::jmin(100, (int)std::ceil(left)));
            if (ready != 0)
            {
                uint8_t tmp[8192];
                int bytesRead = ready > 0 ? ftSocket->read(tmp, sizeof(tmp), false) : -1;
                if (bytesRead <= 0)
                {
                    ftSocket->close();   // closed by the device, or failed
                    break;
                }
                fltxReadBuf.insert(fltxReadBuf.end(), tmp, tmp + bytesRead);
            }
        }
        return resp;
    }

    //--------------------------------------------------------------------------
    // Once a database is open, the connection carries a request only when a
    // waiting track needs one (another source's stat and download, the
    // source list asked again: doDeviceWork); in between the device keeps
    // sending its code != 0 message every 4 s.  Read and drop what comes
    // between requests so the socket's receive buffer never fills: before,
    // nothing read the connection again, and in a few hours the device's
    // sends would have blocked.  A failed read or the device's close closes
    // the socket.  What is read goes to fltxReadBuf and only whole messages
    // are dropped: a message split across reads keeps its head there for
    // the next read, as fltxReadResponse expects (dropping raw bytes left
    // the next reply read out of frame, and a source failed as "the unit
    // did not answer", VERIFY-DB-3 Y02).  At most 512 KiB a call.
    //--------------------------------------------------------------------------
    void drainFltx()
    {
        for (int i = 0; i < 64; ++i)
        {
            std::lock_guard<std::mutex> lock(sockMutex);
            if (!ftSocket || !ftSocket->isConnected()) break;
            const int ready = ftSocket->waitUntilReady(true, 0);
            if (ready == 0) break;
            uint8_t tmp[8192];
            const int n = ready > 0 ? ftSocket->read(tmp, sizeof(tmp), false) : -1;
            if (n <= 0)
            {
                ftSocket->close();
                break;
            }
            fltxReadBuf.insert(fltxReadBuf.end(), tmp, tmp + n);
        }
        while (fltxReadResponse(0).received) {}   // parses what is buffered; waits for nothing
    }

    FltxResponse parseFltxBody(const uint8_t* body, uint32_t bodyLen)
    {
        FltxResponse resp;
        if (bodyLen < 8) return resp;

        // Verify fltx magic
        if (std::memcmp(body, StageLinQ::kFltxMagic, 4) != 0) return resp;

        uint32_t code = StageLinQ::readU32BE(body + 4);

        // code > 0: the device's periodic message ("timecode" in chrisle's
        // FileTransfer.ts: fltx, code, 0x7D2, 0), not a reply -- the caller
        // skips it.  A PRIME 4+ sends one every 4 s (#23 capture).
        resp.code = code;
        if (code > 0) return resp;

        // code == 0: read message ID
        if (bodyLen < 12) return resp;
        uint32_t msgId = StageLinQ::readU32BE(body + 8);
        resp.messageId = msgId;

        switch (msgId)
        {
            case StageLinQ::kFltxRespSourceLocations:
            {
                // count[4] + strings... + 0x01 0x01 0x01
                if (bodyLen < 16) break;
                uint32_t count = StageLinQ::readU32BE(body + 12);
                int pos = 16;
                for (uint32_t i = 0; i < count && pos < (int)bodyLen; ++i)
                {
                    juce::String src;
                    pos = StageLinQ::readNetworkString(body, (int)bodyLen, pos, src);
                    if (pos < 0) break;
                    resp.sources.add(src);
                }
                break;
            }

            case StageLinQ::kFltxRespFileStat:
            {
                // 53 bytes payload, last 4 = file size
                if (bodyLen >= 12 + 53)
                    resp.fileSize = StageLinQ::readU32BE(body + 12 + 49);
                break;
            }

            case StageLinQ::kFltxRespTransferId:
            {
                // 0x0[4] + filesize[4] + txid[4]
                if (bodyLen >= 24)
                {
                    resp.txFileSize = StageLinQ::readU32BE(body + 16);
                    resp.txId = StageLinQ::readU32BE(body + 20);
                }
                break;
            }

            case StageLinQ::kFltxRespChunk:
            {
                // 0x0[4] + offset[4] + chunksize[4] + data
                if (bodyLen >= 24)
                {
                    resp.chunkOffset = StageLinQ::readU32BE(body + 16);
                    resp.chunkSize = StageLinQ::readU32BE(body + 20);
                    // bodyLen >= 24 here; 24 + chunkSize wrapped (AUDIT WIRE-2)
                    if (resp.chunkSize <= bodyLen - 24)
                    {
                        resp.chunkData.assign(body + 24, body + 24 + resp.chunkSize);
                    }
                }
                break;
            }

            case StageLinQ::kFltxRespEndOfMessage:  // after a reply (the source list, in the #23 capture)
            case StageLinQ::kFltxRespUnknown8:      // unsolicited on connect: the string "1"
                break;

            default:   // includes kFltxRespDisconnect, which the reader acts on
                DBG("StageLinQ DB: fltx message " + juce::String(msgId)
                    + (msgId == StageLinQ::kFltxRespDisconnect ? " (device disconnect)" : " (unknown)"));
                break;
        }

        return resp;
    }

    //==========================================================================
    // SQLite database operations
    //==========================================================================
    //--------------------------------------------------------------------------
    // The database comes from the device: it is opened as untrusted input
    // (SQLite's "defense against the dark arts", AUDIT DEP-1, SLQ-9).
    //  - Read-only, and immutable: the file is STC's own downloaded copy,
    //    which nothing changes, so SQLite takes no locks and creates no
    //    -wal/-shm files beside it.  A database the device keeps in WAL
    //    mode opens this way without its -wal, which is not downloaded:
    //    what had not been checkpointed on the device is not seen.  If the
    //    URI form fails to open, the plain read-only open is tried.
    //  - SQLITE_DBCONFIG_DEFENSIVE, and no trusted schema (the C API form
    //    of PRAGMA trusted_schema=OFF): SQL functions with side effects
    //    cannot run from the database's own schema.
    //  - Track (and AlbumArt, when present) must be ordinary tables: a view
    //    or a virtual table of that name would run SQL the device supplied
    //    whenever STC queries it.  Anything else refuses the database.
    //  - PerformanceData (schema 3.x, below) is queried only when it is an
    //    ordinary table too; a view or virtual table of that name is
    //    ignored, not queried, and does not refuse the database.
    // The handle is closed on every failure path, and the file deleted.
    //--------------------------------------------------------------------------
    static std::string sqliteImmutableUri(const juce::File& f)
    {
        juce::String path = f.getFullPathName().replaceCharacter('\\', '/');
        if (!path.startsWithChar('/')) path = "/" + path;      // Windows: /C:/...
        std::string uri = "file://";
        for (const char* p = path.toRawUTF8(); *p != 0; ++p)
        {
            const char c = *p;
            if (c == '%' || c == '?' || c == '#')
            {
                static const char* const hexDigits = "0123456789ABCDEF";
                uri += '%';
                uri += hexDigits[((unsigned char)c) >> 4];
                uri += hexDigits[((unsigned char)c) & 15];
            }
            else
            {
                uri += c;
            }
        }
        return uri + "?immutable=1";
    }

    static bool queriedObjectsAreTables(sqlite3* handle)
    {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(handle,
                "SELECT lower(name), type, coalesce(sql, '') FROM sqlite_master "
                "WHERE lower(name) IN ('track', 'albumart')", -1, &stmt, nullptr) != SQLITE_OK)
            return false;
        bool haveTrack = false, ok = true;
        while (ok && sqlite3_step(stmt) == SQLITE_ROW)
        {
            const char* name = (const char*)sqlite3_column_text(stmt, 0);
            const char* type = (const char*)sqlite3_column_text(stmt, 1);
            const char* sql  = (const char*)sqlite3_column_text(stmt, 2);
            const bool isTable = type != nullptr && std::strcmp(type, "table") == 0
                && !juce::String(juce::CharPointer_UTF8(sql != nullptr ? sql : ""))
                        .trimStart().startsWithIgnoreCase("CREATE VIRTUAL");
            ok = isTable;
            if (name != nullptr && std::strcmp(name, "track") == 0) haveTrack = true;
        }
        sqlite3_finalize(stmt);
        return ok && haveTrack;
    }

    // An ordinary table of that (lower-case) name exists, by the rule above
    static bool isPlainTable(sqlite3* handle, const char* lowerName)
    {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(handle, "SELECT type, coalesce(sql, '') FROM sqlite_master WHERE lower(name) = ?",
                               -1, &stmt, nullptr) != SQLITE_OK)
            return false;
        sqlite3_bind_text(stmt, 1, lowerName, -1, SQLITE_STATIC);
        bool found = false, ok = true;
        while (ok && sqlite3_step(stmt) == SQLITE_ROW)
        {
            const char* type = (const char*)sqlite3_column_text(stmt, 0);
            const char* sql  = (const char*)sqlite3_column_text(stmt, 1);
            found = true;
            ok = type != nullptr && std::strcmp(type, "table") == 0
                 && !juce::String(juce::CharPointer_UTF8(sql != nullptr ? sql : ""))
                        .trimStart().startsWithIgnoreCase("CREATE VIRTUAL");
        }
        sqlite3_finalize(stmt);
        return found && ok;
    }

    // Opens a source's downloaded copy (it is deleted if refused)
    bool openSourceDatabase(SourceDb& src, const juce::File& dbFile)
    {
        sqlite3* db = nullptr;
        int rc = sqlite3_open_v2(sqliteImmutableUri(dbFile).c_str(), &db,
                                 SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr);
        if (rc != SQLITE_OK)
        {
            DBG("StageLinQ DB: SQLite open error (immutable URI): " + juce::String(sqlite3_errmsg(db)));
            sqlite3_close(db);   // a handle is returned even when the open fails
            db = nullptr;
            rc = sqlite3_open_v2(dbFile.getFullPathName().toRawUTF8(), &db, SQLITE_OPEN_READONLY, nullptr);
        }

        if (rc == SQLITE_OK)
        {
            sqlite3_db_config(db, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr);
            sqlite3_db_config(db, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, nullptr);
        }

        if (rc != SQLITE_OK || !queriedObjectsAreTables(db))
        {
            DBG("StageLinQ DB: " + (rc != SQLITE_OK ? "SQLite open error: " + juce::String(sqlite3_errmsg(db))
                                                    : juce::String("Track/AlbumArt are not plain tables -- refused")));
            sqlite3_close(db);
            dbFile.deleteFile();
            return false;
        }
        src.db = db;
        src.file = dbFile;
        src.perfDataIsTable = isPlainTable(db, "performancedata");
        DBG(juce::String("StageLinQ DB: PerformanceData ")
            + (src.perfDataIsTable ? "is an ordinary table" : "absent, or not an ordinary table -- not queried"));

        // Count tracks for logging
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM Track", -1, &stmt, nullptr) == SQLITE_OK)
        {
            if (sqlite3_step(stmt) == SQLITE_ROW)
            {
                src.trackCount = sqlite3_column_int(stmt, 0);
                DBG("StageLinQ DB: " + juce::String(src.trackCount) + " tracks in database");
            }
            sqlite3_finalize(stmt);
        }

        return true;
    }

    // Closes every source's database and deletes its copy (this thread, or
    // stop() once the thread is gone); each handle is closed once
    void closeDatabase()
    {
        dbReady.store(false);
        for (auto& s : sessionSources)
        {
            if (s.db != nullptr)
            {
                sqlite3_close(s.db);
                s.db = nullptr;
            }
            if (s.file != juce::File())
            {
                s.file.deleteFile();
                s.file = juce::File();
            }
        }
        sessionSources.clear();
        listedSources.clear();
    }

    //==========================================================================
    // Looks a track up in one source's database and caches what it has;
    // false when that database has no such track
    //==========================================================================
    bool lookUpTrack(const SourceDb& src, const TrackRef& ref, const std::string& key)
    {
        // --- All DB queries OUTSIDE the lock (may take time) ---

        // Query Track table: by the path relative to Engine Library, then
        // in STC's old form (oldFormDbPath).  The caches are keyed by the
        // source and the first form, which is what the getters compute; the
        // queries use the path that found the row.
        juce::String dbPath = ref.dbPath;
        juce::int64 trackId = 0;
        DenonTrackMeta meta = queryTrack(src, dbPath, trackId);
        if (!meta.valid && !ref.streaming && oldFormDbPath(ref.dbPath).isNotEmpty())
        {
            dbPath = oldFormDbPath(ref.dbPath);
            meta = queryTrack(src, dbPath, trackId);
        }
        if (!meta.valid) return false;

        // Query artwork (no lock held during I/O)
        juce::Image artImg;
        if (meta.albumArtId > 0)
            artImg = queryArtwork(src, meta.albumArtId);

        // Query overview waveform
        auto wf = queryOverviewWaveform(src, dbPath, trackId);

        // Query performance data (cues, loops, beat grid)
        auto perf = queryPerformanceData(src, dbPath, trackId);

        // --- Insert results into caches (lock once) ---
        {
            std::lock_guard<std::mutex> lock(cacheMutex);
            trackCache[key] = meta;

            if (artImg.isValid() && artworkCache.count(key) == 0)
                artworkCache[key] = artImg;

            if (wf.valid && waveformCache.count(key) == 0)
                waveformCache[key] = std::move(wf);

            if (perf.valid && perfCache.count(key) == 0)
                perfCache[key] = std::move(perf);
        }

        DBG("StageLinQ DB: Loaded metadata for " + meta.artist + " - " + meta.title
            + " (art=" + juce::String(meta.albumArtId) + ", from " + src.name + ")");
        return true;
    }

    //==========================================================================
    // SQLite query: Track table
    //==========================================================================
    // trackId: the row's Track.id, which PerformanceData is keyed by
    DenonTrackMeta queryTrack(const SourceDb& src, const juce::String& trackPath, juce::int64& trackId)
    {
        DenonTrackMeta meta;
        trackId = 0;
        sqlite3* const db = src.db;
        if (!db) return meta;

        // Streaming tracks (Beatsource/Tidal) are queried by "uri" column
        // instead of "path" (matching chrisle/StageLinq DbConnection.ts)
        bool isStreaming = trackPath.startsWith("streaming://");

        // Engine DJ schema v1.x uses "idAlbumArt", v2.x uses "albumArtId".
        // We download v2 first (Database2/m.db), falling back to v1.
        // Try v2 column name first, fall back to v1 on failure.
        juce::String whereCol = isStreaming ? "uri" : "path";
        juce::String sqlV2Str = "SELECT title, artist, album, genre, key, bpm, length, albumArtId, id "
                                "FROM Track WHERE " + whereCol + " = ? LIMIT 1";
        juce::String sqlV1Str = "SELECT title, artist, album, genre, key, bpm, length, idAlbumArt, id "
                                "FROM Track WHERE " + whereCol + " = ? LIMIT 1";

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sqlV2Str.toRawUTF8(), -1, &stmt, nullptr) != SQLITE_OK)
        {
            // v2 column not found -- try v1
            if (sqlite3_prepare_v2(db, sqlV1Str.toRawUTF8(), -1, &stmt, nullptr) != SQLITE_OK)
                return meta;
        }

        sqlite3_bind_text(stmt, 1, trackPath.toRawUTF8(), -1, SQLITE_TRANSIENT);

        if (sqlite3_step(stmt) == SQLITE_ROW)
        {
            // sqlite3_column_text returns NULL for SQL NULL -- must check
            auto safeText = [](sqlite3_stmt* s, int col) -> juce::String {
                const char* p = (const char*)sqlite3_column_text(s, col);
                return p ? juce::String::fromUTF8(p) : juce::String();
            };

            meta.title      = safeText(stmt, 0);
            meta.artist     = safeText(stmt, 1);
            meta.album      = safeText(stmt, 2);
            meta.genre      = safeText(stmt, 3);

            // Key: stored as integer in Engine DJ (0-23 = musical key index)
            // Convert to string using musicalKeyToString()
            int keyType = sqlite3_column_type(stmt, 4);
            if (keyType == SQLITE_INTEGER)
                meta.key = musicalKeyToString(sqlite3_column_int(stmt, 4));
            else if (keyType == SQLITE_TEXT)
            {
                const char* kp = (const char*)sqlite3_column_text(stmt, 4);
                if (kp) meta.key = juce::String::fromUTF8(kp);
            }

            meta.bpm        = sqlite3_column_double(stmt, 5);
            meta.length     = sqlite3_column_double(stmt, 6);
            meta.albumArtId = sqlite3_column_int(stmt, 7);
            trackId         = sqlite3_column_int64(stmt, 8);
            meta.valid = true;
        }

        sqlite3_finalize(stmt);
        return meta;
    }

    //==========================================================================
    // SQLite query: AlbumArt table -> juce::Image
    //==========================================================================
    juce::Image queryArtwork(const SourceDb& src, int albumArtId)
    {
        sqlite3* const db = src.db;
        if (!db || albumArtId <= 0) return {};

        const char* sql = "SELECT albumArt FROM AlbumArt WHERE id = ? AND albumArt IS NOT NULL LIMIT 1";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return {};

        sqlite3_bind_int(stmt, 1, albumArtId);

        juce::Image result;
        if (sqlite3_step(stmt) == SQLITE_ROW)
        {
            const void* blob = sqlite3_column_blob(stmt, 0);
            int blobSize = sqlite3_column_bytes(stmt, 0);

            // AlbumArt BLOBs are standard JPEG/PNG images, from the device:
            // a damaged PNG is refused before JUCE's reader sees it (it
            // crashed this thread, #26; ImageGuard.h), and so is one over
            // ImageGuard's artwork limits.  The check ends (a refusal) when
            // stop() is called: stop() does not wait for it.
            if (blob && blobSize > 0)
                result = ImageGuard::loadUntrustedImage(blob, (size_t)blobSize, {},
                                                        [this] { return threadShouldExit(); });
        }

        sqlite3_finalize(stmt);
        return result;
    }

    //==========================================================================
    // A BLOB of the track: its Track column (Engine schema 2.x), or else --
    // Track has no such column, or it is NULL or empty for this row -- the
    // same column of PerformanceData by trackId, if PerformanceData is an
    // ordinary table (openSourceDatabase).  Schema 3.x (Engine DJ 4.5 and 5.x:
    // chrisle/StageLinq 3.5.0 types/models/Track.ts, engine-dj-mcp
    // tools/performance.js) moved overviewWaveFormData, beatData, quickCues
    // and loops out of Track into PerformanceData.  The bytes are copied
    // out (SQLite's pointer lives only as long as the statement).
    // rowFound: the track has a row in the table the bytes were read from.
    //==========================================================================
    std::vector<uint8_t> queryTrackBlob(const SourceDb& src, const char* column, const juce::String& trackPath,
                                        juce::int64 trackId, bool& rowFound)
    {
        rowFound = false;
        std::vector<uint8_t> out;
        sqlite3* const db = src.db;
        if (!db) return out;

        auto readOne = [&](const juce::String& sql, bool byTrackId)
        {
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db, sql.toRawUTF8(), -1, &stmt, nullptr) != SQLITE_OK)
                return false;   // no such column (or table)
            if (byTrackId)
                sqlite3_bind_int64(stmt, 1, trackId);
            else
                sqlite3_bind_text(stmt, 1, trackPath.toRawUTF8(), -1, SQLITE_TRANSIENT);
            bool row = false;
            if (sqlite3_step(stmt) == SQLITE_ROW)
            {
                row = true;
                const void* blob = sqlite3_column_blob(stmt, 0);
                const int blobSize = sqlite3_column_bytes(stmt, 0);
                if (blob != nullptr && blobSize > 0)
                    out.assign(static_cast<const uint8_t*>(blob), static_cast<const uint8_t*>(blob) + blobSize);
            }
            sqlite3_finalize(stmt);
            return row;
        };

        const char* col = trackPath.startsWith("streaming://") ? "uri" : "path";
        rowFound = readOne(juce::String("SELECT ") + column + " FROM Track WHERE " + col + " = ? LIMIT 1", false);
        if (out.empty() && src.perfDataIsTable && trackId > 0)
        {
            const bool perfRow = readOne(juce::String("SELECT ") + column
                                         + " FROM PerformanceData WHERE trackId = ? LIMIT 1", true);
            rowFound = rowFound || perfRow;
        }
        return out;
    }

    //==========================================================================
    // SQLite query: overviewWaveFormData -> DenonWaveformData
    //
    // BLOB format (from libdjinterop, LGPL, by xsco):
    //   [uncompressed_size:i32be][zlib_data...]
    //
    // After zlib decompression:
    //   numEntries   : int64_be (8 bytes)
    //   numEntries   : int64_be (8 bytes)  -- duplicate
    //   samplesPerPt : double_be (8 bytes)
    //   entries[N]   : { low:u8, mid:u8, high:u8 }  -- 3 bytes per point
    //   maximum      : { low:u8, mid:u8, high:u8 }  -- global peak
    //
    // We reorder bytes to mid/high/low per entry to match our
    // WaveformDisplay::renderThreeBandBars() (Pioneer CDJ-3000 order).
    //==========================================================================
    DenonWaveformData queryOverviewWaveform(const SourceDb& src, const juce::String& trackPath, juce::int64 trackId)
    {
        bool rowFound = false;
        const auto blob = queryTrackBlob(src, "overviewWaveFormData", trackPath, trackId, rowFound);
        if (blob.size() > 4)
            return decodeOverviewWaveform(blob.data(), (int)blob.size());
        return {};
    }

    //==========================================================================
    // Decode overview waveform BLOB
    //==========================================================================
    static DenonWaveformData decodeOverviewWaveform(const uint8_t* blob, int blobSize)
    {
        DenonWaveformData result;
        if (blobSize <= 4) return result;

        auto decompressed = zlibDecompress(blob, blobSize);
        const uint8_t* data = decompressed.data();
        int dataSize = (int)decompressed.size();

        // Minimum: 24 bytes header + 3 bytes max = 27 bytes
        if (dataSize < 27) return result;

        // Parse header.  The count is compared with the bytes there are in
        // its own 64-bit width before any multiply or cast (AUDIT WIRE-1:
        // (int)(numEntries1 * 3) truncated, so 0x55555556 entries passed the
        // check and the loop below ran ~1.4 G entries past the buffer).
        const uint64_t numEntries1 = StageLinQ::readU64BE(data);
        const uint64_t numEntries2 = StageLinQ::readU64BE(data + 8);
        // double samplesPerPoint = StageLinQ::readF64BE(data + 16);  // not used

        if (numEntries1 != numEntries2 || numEntries1 == 0) return result;
        if (numEntries1 > (uint64_t)(dataSize - 27) / 3) return result;   // dataSize >= 27 here

        // At most (kMaxBlobBytes - 27) / 3 entries: the decompressed size is capped
        const int numEntries = (int)numEntries1;

        // Decode entries, reordering from Denon (low/mid/high) to
        // WaveformDisplay (mid/high/low) to match renderThreeBandBars()
        result.data.resize((size_t)(numEntries * 3));
        result.entryCount = numEntries;

        const uint8_t* src = data + 24;
        for (int i = 0; i < numEntries; ++i)
        {
            uint8_t low  = src[i * 3 + 0];
            uint8_t mid  = src[i * 3 + 1];
            uint8_t high = src[i * 3 + 2];
            // Pioneer ThreeBand order: mid[0], high[1], low[2]
            result.data[(size_t)(i * 3 + 0)] = mid;
            result.data[(size_t)(i * 3 + 1)] = high;
            result.data[(size_t)(i * 3 + 2)] = low;
        }

        result.valid = true;
        DBG("StageLinQ DB: Decoded waveform: " + juce::String(numEntries) + " points");
        return result;
    }

    //==========================================================================
    // SQLite query: performance data (quickCues, loops, beatData BLOBs),
    // each from Track or PerformanceData (queryTrackBlob).  valid: the
    // track has a row with these columns.
    //==========================================================================
    DenonPerformanceData queryPerformanceData(const SourceDb& src, const juce::String& trackPath, juce::int64 trackId)
    {
        DenonPerformanceData result;
        bool cueRow = false, loopRow = false, beatRow = false;

        // Quick cues (zlib compressed)
        const auto cueBlob = queryTrackBlob(src, "quickCues", trackPath, trackId, cueRow);
        if (cueBlob.size() > 4)
            decodeQuickCues(cueBlob.data(), (int)cueBlob.size(), result);

        // Loops (NOT compressed -- raw binary)
        const auto loopBlob = queryTrackBlob(src, "loops", trackPath, trackId, loopRow);
        if (loopBlob.size() >= 8)
            decodeLoops(loopBlob.data(), (int)loopBlob.size(), result);

        // Beat data (zlib compressed)
        const auto beatBlob = queryTrackBlob(src, "beatData", trackPath, trackId, beatRow);
        if (beatBlob.size() > 4)
            decodeBeatData(beatBlob.data(), (int)beatBlob.size(), result);

        result.valid = cueRow || loopRow || beatRow;
        return result;
    }

    //==========================================================================
    // Decode quickCues BLOB (zlib compressed, from libdjinterop)
    // Format: zlib([count:i64be, {labelLen:u8, label, offset:f64be, a,r,g,b}*N,
    //               adjustedMainCue:f64be, isAdjusted:u8, defaultMainCue:f64be])
    //==========================================================================
    static void decodeQuickCues(const uint8_t* blob, int blobSize, DenonPerformanceData& out)
    {
        auto data = zlibDecompress(blob, blobSize);
        if (data.size() < 25) return;

        const uint8_t* p = data.data();
        const uint8_t* end = p + data.size();

        int64_t count = (int64_t)StageLinQ::readU64BE(p); p += 8;
        if (count < 0 || count > 16) return;

        for (int64_t i = 0; i < count && p < end; ++i)
        {
            DenonQuickCue cue;
            if (p >= end) break;
            uint8_t labelLen = *p++;
            if (p + labelLen + 12 > end) break;

            if (labelLen > 0)
            {
                cue.label = juce::String::fromUTF8((const char*)p, labelLen);
                p += labelLen;
            }

            cue.sampleOffset = StageLinQ::readF64BE(p); p += 8;
            cue.a = *p++;
            cue.r = *p++;
            cue.g = *p++;
            cue.b = *p++;

            out.quickCues.push_back(cue);
        }

        // Main cue
        if (p + 17 <= end)
        {
            out.mainCueSampleOffset = StageLinQ::readF64BE(p); p += 8;
            p += 1;  // isAdjusted
            double defaultCue = StageLinQ::readF64BE(p);
            if (out.mainCueSampleOffset <= 0.0)
                out.mainCueSampleOffset = defaultCue;
        }

        DBG("StageLinQ DB: Decoded " + juce::String((int)out.quickCues.size()) + " quick cues");
    }

    //==========================================================================
    // Decode loops BLOB (NOT compressed, from libdjinterop)
    // Format: [count:i64le, {labelLen:u8, label, start:f64le, end:f64le,
    //           startSet:u8, endSet:u8, a,r,g,b}*N]
    //==========================================================================
    static void decodeLoops(const uint8_t* blob, int blobSize, DenonPerformanceData& out)
    {
        // Loops are NOT zlib compressed (confirmed by libdjinterop)
        const uint8_t* p = blob;
        const uint8_t* end = p + blobSize;

        if (blobSize < 8) return;
        int64_t count = readI64LE(p); p += 8;
        if (count < 0 || count > 16) return;

        for (int64_t i = 0; i < count && p < end; ++i)
        {
            DenonLoop loop;
            if (p >= end) break;
            uint8_t labelLen = *p++;
            if (p + labelLen + 22 > end) break;

            if (labelLen > 0)
            {
                loop.label = juce::String::fromUTF8((const char*)p, labelLen);
                p += labelLen;
            }

            loop.startSampleOffset = readF64LE(p); p += 8;
            loop.endSampleOffset = readF64LE(p); p += 8;
            loop.startSet = (*p++ != 0);
            loop.endSet = (*p++ != 0);
            loop.a = *p++;
            loop.r = *p++;
            loop.g = *p++;
            loop.b = *p++;

            out.loops.push_back(loop);
        }

        DBG("StageLinQ DB: Decoded " + juce::String((int)out.loops.size()) + " loops");
    }

    //==========================================================================
    // Decode beatData BLOB (zlib compressed, from libdjinterop)
    // Format: zlib([sampleRate:f64be, samples:f64be, isSet:u8,
    //               count:i64be, {offset:f64le, beatNum:i64le, numBeats:i32le, unk:i32le}*N,
    //               count2:i64be, markers2[]])
    //==========================================================================
    static void decodeBeatData(const uint8_t* blob, int blobSize, DenonPerformanceData& out)
    {
        auto data = zlibDecompress(blob, blobSize);
        if (data.size() < 33) return;

        const uint8_t* p = data.data();
        const uint8_t* end = p + data.size();

        out.sampleRate = StageLinQ::readF64BE(p); p += 8;
        out.totalSamples = StageLinQ::readF64BE(p); p += 8;
        p += 1;  // is_beatgrid_set

        if (p + 8 > end) return;
        int64_t count = (int64_t)StageLinQ::readU64BE(p); p += 8;
        if (count < 0 || count > 100000) return;

        // Each marker: offset(f64le) + beatNum(i64le) + numBeats(i32le) + unk(i32le) = 24 bytes
        for (int64_t i = 0; i < count && p + 24 <= end; ++i)
        {
            DenonBeatGridMarker m;
            m.sampleOffset = readF64LE(p); p += 8;
            m.beatNumber = readI64LE(p); p += 8;
            m.numBeats = readI32LE(p); p += 4;
            p += 4;  // unknown_value_1
            out.beatGrid.push_back(m);
        }

        // Skip adjusted beat grid (same format, we only need default)

        DBG("StageLinQ DB: Decoded " + juce::String((int)out.beatGrid.size())
            + " beat grid markers, sr=" + juce::String(out.sampleRate, 0));
    }

    //==========================================================================
    // zlib decompress helper (Engine DJ format: [size:i32be][zlib_data])
    //==========================================================================
    static std::vector<uint8_t> zlibDecompress(const uint8_t* blob, int blobSize)
    {
        if (blobSize <= 4) return {};
        // The declared size caps the output (a decompression bomb stops there)
        int32_t expectedSize = (int32_t)StageLinQ::readU32BE(blob);
        if (expectedSize <= 0 || expectedSize > StageLinQ::kMaxBlobBytes) return {};

        juce::MemoryInputStream compressedStream(blob + 4, (size_t)(blobSize - 4), false);
        juce::GZIPDecompressorInputStream decompressor(
            &compressedStream, false,
            juce::GZIPDecompressorInputStream::zlibFormat,
            (juce::int64)expectedSize);

        juce::MemoryBlock block;
        {
            juce::MemoryOutputStream tempOut(block, false);
            tempOut.writeFromInputStream(decompressor, expectedSize);
        }

        std::vector<uint8_t> result((size_t)block.getSize());
        if (!result.empty())
            std::memcpy(result.data(), block.getData(), result.size());
        return result;
    }

    //==========================================================================
    // Little-endian helpers (beat grid and loops use LE, not BE)
    //==========================================================================
    static int64_t readI64LE(const uint8_t* p)
    {
        uint64_t v = uint64_t(p[0]) | (uint64_t(p[1]) << 8) | (uint64_t(p[2]) << 16) | (uint64_t(p[3]) << 24)
                   | (uint64_t(p[4]) << 32) | (uint64_t(p[5]) << 40) | (uint64_t(p[6]) << 48) | (uint64_t(p[7]) << 56);
        return (int64_t)v;
    }
    static int32_t readI32LE(const uint8_t* p)
    {
        return (int32_t)(uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24));
    }
    static double readF64LE(const uint8_t* p)
    {
        uint64_t raw = uint64_t(p[0]) | (uint64_t(p[1]) << 8) | (uint64_t(p[2]) << 16) | (uint64_t(p[3]) << 24)
                     | (uint64_t(p[4]) << 32) | (uint64_t(p[5]) << 40) | (uint64_t(p[6]) << 48) | (uint64_t(p[7]) << 56);
        double result;
        std::memcpy(&result, &raw, 8);
        return result;
    }

    //==========================================================================
    // A TrackNetworkPath as the source that holds the track and its Track.path
    //
    // Format: net://uuid/source/Engine Library/Music/Artist/Track.mp3
    //   -> source "source", DB path Music/Artist/Track.mp3 (relative to
    //      Engine Library)
    //
    // Engine keeps Track.path relative to the Engine Library folder
    // (engine-dj-mcp dist/paths.js; chrisle/StageLinq Player.ts keeps
    // "Music/", parts.slice(5)), so:
    //   - Inside Engine Library ("Music/" or another folder): the path
    //     relative to it.  The form relative to Music that STC looked up
    //     before (chrisle's utils/trackPath.ts strips "Music/") is tried
    //     second: oldFormDbPath().
    //   - Outside Engine Library: prefix with "../"
    //   - Streaming (Beatsource, Tidal, SoundCloud): "streaming://..." as it
    //     comes, or chrisle's net://uuid/(Unknown)streaming://... form
    //     (Player.ts), looked up by Track.uri; no source.
    //==========================================================================
    static TrackRef parseNetworkPath(const juce::String& networkPath)
    {
        TrackRef ref;
        if (networkPath.isEmpty()) return ref;

        if (networkPath.startsWith("streaming://"))
        {
            ref.dbPath = networkPath;
            ref.streaming = true;
            return ref;
        }

        if (!networkPath.startsWith("net://"))
            return ref;

        auto parts = juce::StringArray::fromTokens(networkPath, "/", "");
        // parts[0]="net:", [1]="", [2]=uuid, [3]=source, [4+]=path

        if (parts.size() < 5) return ref;

        if (parts[3].startsWith("(Unknown)streaming:"))
        {
            const int at = networkPath.indexOf("streaming://");
            if (at > 0)
            {
                ref.dbPath = networkPath.substring(at);
                ref.streaming = true;
            }
            return ref;
        }

        ref.source = parts[3];
        juce::StringArray trackParts;
        if (parts.size() > 5 && parts[4] == "Engine Library")
        {
            // Inside Engine Library (Music/ or another folder): relative to it
            for (int i = 5; i < parts.size(); ++i)
                trackParts.add(parts[i]);
            ref.dbPath = trackParts.joinIntoString("/");
        }
        else
        {
            // Outside Engine Library
            for (int i = 4; i < parts.size(); ++i)
                trackParts.add(parts[i]);
            ref.dbPath = "../" + trackParts.joinIntoString("/");
        }
        return ref;
    }

    // The same track in the form STC looked up before 1.9.16 (relative to
    // the Music folder: "Music/A/x.mp3" -> "A/x.mp3"), tried when the path
    // relative to Engine Library finds no row; empty for any other path.
    static juce::String oldFormDbPath(const juce::String& dbPath)
    {
        return dbPath.startsWith("Music/") && dbPath.length() > 6 ? dbPath.substring(6) : juce::String();
    }

    //==========================================================================
    // Member data
    //==========================================================================
    // The session's device: written and read on this client's thread only
    juce::String deviceIp;
    uint16_t ftPort = 0;
    uint8_t token[StageLinQ::kTokenLen] = {};
    uint32_t servedSerial = 0;           // the targetSerial of that session

    // What start() wants served (guarded by targetMutex; targetSerial is
    // also read lock-free, by sessionShouldEnd)
    std::mutex targetMutex;
    juce::String targetIp;
    uint16_t targetPort = 0;
    uint8_t targetToken[StageLinQ::kTokenLen] = {};
    std::atomic<uint32_t> targetSerial { 0 };
    // The last offer start() ignored while a session was wanted (also under
    // targetMutex): served if that session fails (run).  One of the
    // session's own device does not replace another device's (start()).
    juce::String deferredIp;
    uint16_t deferredPort = 0;
    uint8_t deferredToken[StageLinQ::kTokenLen] = {};
    bool haveDeferred = false;

    std::mutex lifecycleMutex;           // serialises start() and stop()

    std::unique_ptr<juce::StreamingSocket> ftSocket;
    std::mutex sockMutex;
    std::vector<uint8_t> fltxReadBuf;

    // Database paths whose stat the device did not answer in this session
    // (this thread only; cleared when another device or port is served)
    juce::StringArray unansweredStats;
    static constexpr int kMaxUnansweredStats = 16;
    // Connections opened again, in one session, after a transfer was lost
    // before the first database was loaded (runSession); each one goes on
    // with a source the last list named and this session had not tried
    static constexpr int kMaxTransfersLost = 16;

    // The session's media (this thread only; closeDatabase): every source
    // listed so far, in the unit's order, with its database once loaded,
    // and the source list as the unit last gave it
    std::vector<SourceDb> sessionSources;
    juce::StringArray listedSources;
    static constexpr int kMaxSources = 64;   // updateSources
    std::atomic<bool> dbReady { false };     // a source's database is open

    // Requests (this thread only): those not answered yet, oldest first, and
    // every one seen in this session
    juce::StringArray waitingRequests, seenRequests;

    // When the source list was last asked for, and how long until it may be
    // asked again for a source it did not name (this thread only)
    static constexpr double kSourcesAskFirstMs = 5000.0;
    static constexpr double kSourcesAskMaxMs   = 60000.0;
    static constexpr double kReconnectWaitMs   = 5000.0;
    double lastSourcesAskMs = 0.0;
    double sourcesAskIntervalMs = kSourcesAskFirstMs;
    double nextConnectMs = 0.0;              // no new connection before (after one failed)

    // Caches (protected by cacheMutex), keyed by TrackRef::key()
    mutable std::mutex cacheMutex;
    std::map<std::string, DenonTrackMeta> trackCache;
    std::map<std::string, juce::Image> artworkCache;
    std::map<std::string, DenonWaveformData> waveformCache;
    std::map<std::string, DenonPerformanceData> perfCache;

    // Pending requests (protected by requestMutex)
    std::mutex requestMutex;
    juce::StringArray pendingRequests;

    // A session is wanted: from start() until it fails, or stop()
    std::atomic<bool> isRunningFlag { false };
};
