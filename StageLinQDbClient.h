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
//   3. Download /{source}/Engine Library/Database2/m.db (or v1 fallback)
//   4. Open with SQLite, cache track metadata + artwork
//   5. On TrackNetworkPath change: lookup Track -> AlbumArt -> juce::Image
//
// Requires: sqlite3 amalgamation (sqlite3.h + sqlite3.c) in the project.

#pragma once
#include <JuceHeader.h>
#include "StageLinQInput.h"
#include <atomic>
#include <mutex>
#include <map>
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
    static constexpr uint32_t kFltxRespDisconnect      = 9;

    static constexpr int kFltxChunkSize = 4096;

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
class StageLinQDbClient : public juce::Thread
{
public:
    StageLinQDbClient()
        : Thread("SLQ-DB")
    {
    }

    ~StageLinQDbClient() override
    {
        stop();
    }

    //--------------------------------------------------------------------------
    // Start: connect to FileTransfer service at given IP:port
    //--------------------------------------------------------------------------
    bool start(const juce::String& ip, uint16_t fileTransferPort,
               const uint8_t tkn[StageLinQ::kTokenLen])
    {
        if (isRunningFlag.load()) return true;

        deviceIp = ip;
        ftPort = fileTransferPort;
        std::memcpy(token, tkn, StageLinQ::kTokenLen);
        fltxReadBuf.clear();  // clear stale data from any previous session

        isRunningFlag.store(true);
        startThread(juce::Thread::Priority::normal);
        return true;
    }

    void stop()
    {
        if (!isRunningFlag.load()) return;
        isRunningFlag.store(false);
        signalThreadShouldExit();

        {
            std::lock_guard<std::mutex> lock(sockMutex);
            if (ftSocket)
            {
                ftSocket->close();
                ftSocket.reset();
            }
        }

        stopThread(5000);
        closeDatabase();

        DBG("StageLinQ DB: Stopped");
    }

    bool getIsRunning() const { return isRunningFlag.load(); }

    //--------------------------------------------------------------------------
    // Public getters (thread-safe)
    //--------------------------------------------------------------------------

    // Lookup track by network path (e.g. from StateMap TrackNetworkPath)
    DenonTrackMeta getTrackByNetworkPath(const juce::String& networkPath) const
    {
        // Parse network path: net://uuid/source/Engine Library/Music/path
        auto trackPath = parseNetworkPathToDbPath(networkPath);
        if (trackPath.isEmpty()) return {};

        std::lock_guard<std::mutex> lock(cacheMutex);
        auto it = trackCache.find(trackPath.toStdString());
        if (it != trackCache.end())
            return it->second;
        return {};
    }

    // Get artwork image for a track
    juce::Image getArtwork(int albumArtId) const
    {
        if (albumArtId <= 0) return {};
        std::lock_guard<std::mutex> lock(cacheMutex);
        auto it = artworkCache.find(albumArtId);
        if (it != artworkCache.end())
            return it->second;
        return {};
    }

    // Get artwork image by network path (convenience)
    juce::Image getArtworkForTrack(const juce::String& networkPath) const
    {
        auto meta = getTrackByNetworkPath(networkPath);
        if (!meta.valid || meta.albumArtId <= 0) return {};
        return getArtwork(meta.albumArtId);
    }

    // Get overview waveform for a track (3 bytes per entry: mid/high/low)
    DenonWaveformData getWaveformForTrack(const juce::String& networkPath) const
    {
        auto trackPath = parseNetworkPathToDbPath(networkPath);
        if (trackPath.isEmpty()) return {};

        std::lock_guard<std::mutex> lock(cacheMutex);
        auto it = waveformCache.find(trackPath.toStdString());
        if (it != waveformCache.end())
            return it->second;
        return {};
    }

    // Get performance data (quick cues, loops, beat grid) for a track
    DenonPerformanceData getPerformanceData(const juce::String& networkPath) const
    {
        auto trackPath = parseNetworkPathToDbPath(networkPath);
        if (trackPath.isEmpty()) return {};

        std::lock_guard<std::mutex> lock(cacheMutex);
        auto it = perfCache.find(trackPath.toStdString());
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

    // True if database has been downloaded and opened
    bool isDatabaseReady() const { return dbReady.load(); }

private:
    //==========================================================================
    // Thread main loop
    //==========================================================================
    void run() override
    {
        DBG("StageLinQ DB: Connecting to FileTransfer at " + deviceIp + ":" + juce::String(ftPort));

        // --- Connect to FileTransfer service ---
        {
            auto sock = std::make_unique<juce::StreamingSocket>();
            if (!sock->connect(deviceIp, ftPort, StageLinQ::kSocketTimeoutMs))
            {
                DBG("StageLinQ DB: FileTransfer connect failed");
                isRunningFlag.store(false);
                return;
            }

            // Send service announcement (same as StateMap/BeatInfo)
            auto announce = StageLinQ::buildServiceAnnouncement(token, "FileTransfer", 0);
            sock->write(announce.data(), (int)announce.size());

            {
                std::lock_guard<std::mutex> lock(sockMutex);
                ftSocket = std::move(sock);
            }
        }

        juce::Thread::sleep(500);  // per chrisle/StageLinq: delay before requests

        // --- Get source locations ---
        juce::StringArray sources = fetchSources();
        if (sources.isEmpty())
        {
            DBG("StageLinQ DB: No sources found on device");
            isRunningFlag.store(false);
            return;
        }

        for (int si = 0; si < sources.size(); ++si)
        {
            DBG("StageLinQ DB: Source: " + sources[si]);
        }

        // --- Download database ---
        juce::File dbFile = downloadDatabase(sources);
        if (dbFile == juce::File() || !dbFile.existsAsFile())
        {
            DBG("StageLinQ DB: Failed to download database");
            isRunningFlag.store(false);
            return;
        }

        // --- Open SQLite database ---
        if (!openDatabase(dbFile))
        {
            DBG("StageLinQ DB: Failed to open database");
            isRunningFlag.store(false);
            return;
        }

        dbReady.store(true);
        DBG("StageLinQ DB: Database ready (" + dbFile.getFullPathName() + ")");

        // --- Process metadata requests ---
        while (!threadShouldExit() && isRunningFlag.load())
        {
            juce::StringArray requests;
            {
                std::lock_guard<std::mutex> lock(requestMutex);
                requests = pendingRequests;
                pendingRequests.clear();
            }

            for (auto& networkPath : requests)
            {
                if (threadShouldExit()) break;
                processTrackRequest(networkPath);
            }

            juce::Thread::sleep(100);
        }

        closeDatabase();
        isRunningFlag.store(false);
    }

    //==========================================================================
    // FileTransfer protocol: fetch sources
    //==========================================================================
    juce::StringArray fetchSources()
    {
        auto frame = StageLinQ::buildFltxSources();
        if (!fltxWrite(frame)) return {};

        // Read response with timeout
        auto resp = fltxReadResponse(3000);
        if (resp.messageId == StageLinQ::kFltxRespSourceLocations)
            return resp.sources;

        return {};
    }

    //==========================================================================
    // FileTransfer protocol: download database
    //==========================================================================
    juce::File downloadDatabase(const juce::StringArray& sources)
    {
        for (auto& source : sources)
        {
            // Try v2 database first, then v1
            juce::String paths[] = {
                "/" + source + "/Engine Library/Database2/m.db",
                "/" + source + "/Engine Library/m.db"
            };

            for (auto& dbPath : paths)
            {
                if (threadShouldExit()) return {};

                // Check if file exists (stat)
                uint32_t fileSize = fetchFileStat(dbPath);
                if (fileSize == 0) continue;

                DBG("StageLinQ DB: Found database " + dbPath + " (" + juce::String(fileSize) + " bytes)");

                // Download the file
                auto data = downloadFile(dbPath, fileSize);
                if (data.empty()) continue;

                // Save to temp file
                auto tempDir = juce::File::getSpecialLocation(
                    juce::File::tempDirectory).getChildFile("STC_Denon");
                tempDir.createDirectory();
                auto tempFile = tempDir.getChildFile("m.db");
                tempFile.replaceWithData(data.data(), data.size());

                DBG("StageLinQ DB: Downloaded " + juce::String((int)data.size()) + " bytes to " + tempFile.getFullPathName());
                return tempFile;
            }
        }
        return {};
    }

    //==========================================================================
    // FileTransfer protocol: get file size
    //==========================================================================
    uint32_t fetchFileStat(const juce::String& path)
    {
        auto frame = StageLinQ::buildFltxStat(path);
        if (!fltxWrite(frame)) return 0;

        auto resp = fltxReadResponse(2000);
        if (resp.messageId == StageLinQ::kFltxRespFileStat)
            return resp.fileSize;
        return 0;
    }

    //==========================================================================
    // FileTransfer protocol: download complete file
    //==========================================================================
    std::vector<uint8_t> downloadFile(const juce::String& path, uint32_t expectedSize)
    {
        juce::ignoreUnused(expectedSize);  // actual size comes from TransferId response
        // Request transfer ID
        auto frame = StageLinQ::buildFltxTransferId(path);
        if (!fltxWrite(frame)) return {};

        auto resp = fltxReadResponse(3000);
        if (resp.messageId != StageLinQ::kFltxRespTransferId || resp.txFileSize == 0)
            return {};

        uint32_t fileSize = resp.txFileSize;
        uint32_t txId = resp.txId;
        uint32_t totalChunks = (fileSize + StageLinQ::kFltxChunkSize - 1) / StageLinQ::kFltxChunkSize;

        DBG("StageLinQ DB: Transfer ID " + juce::String(txId) + ", size " + juce::String(fileSize)
            + ", chunks " + juce::String(totalChunks));

        // Request all chunks
        auto chunkReq = StageLinQ::buildFltxChunkRange(txId, 0, totalChunks > 0 ? totalChunks - 1 : 0);
        if (!fltxWrite(chunkReq)) return {};

        // Receive chunks into buffer
        std::vector<uint8_t> fileData(fileSize, 0);
        uint32_t bytesReceived = 0;
        double deadline = juce::Time::getMillisecondCounterHiRes() + 30000.0;  // 30s timeout

        while (bytesReceived < fileSize
               && juce::Time::getMillisecondCounterHiRes() < deadline
               && !threadShouldExit())
        {
            auto chunkResp = fltxReadResponse(5000);
            if (chunkResp.messageId == StageLinQ::kFltxRespChunk && !chunkResp.chunkData.empty())
            {
                uint32_t offset = chunkResp.chunkOffset;
                uint32_t size = chunkResp.chunkSize;
                if (offset + size <= fileSize)
                {
                    std::memcpy(fileData.data() + offset, chunkResp.chunkData.data(), size);
                    bytesReceived += size;
                }
            }
            else if (chunkResp.messageId == StageLinQ::kFltxRespEndOfMessage)
            {
                break;
            }
            else if (chunkResp.messageId == 0)
            {
                // Timeout or error
                break;
            }
        }

        // Signal transfer complete
        fltxWrite(StageLinQ::buildFltxComplete());

        if (bytesReceived < fileSize)
        {
            DBG("StageLinQ DB: Incomplete download: " + juce::String(bytesReceived)
                + "/" + juce::String(fileSize) + " -- discarding (truncated SQLite is unusable)");
            return {};
        }

        return fileData;
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

    FltxResponse fltxReadResponse(int timeoutMs)
    {
        FltxResponse resp;
        double deadline = juce::Time::getMillisecondCounterHiRes() + timeoutMs;

        while (juce::Time::getMillisecondCounterHiRes() < deadline && !threadShouldExit())
        {
            {
                std::lock_guard<std::mutex> lock(sockMutex);
                if (!ftSocket || !ftSocket->isConnected()) return resp;

                if (ftSocket->waitUntilReady(true, 100))
                {
                    uint8_t tmp[8192];
                    int bytesRead = ftSocket->read(tmp, sizeof(tmp), false);
                    if (bytesRead <= 0) return resp;
                    fltxReadBuf.insert(fltxReadBuf.end(), tmp, tmp + bytesRead);
                }
            }

            // Try to parse a complete message
            if (fltxReadBuf.size() >= 4)
            {
                uint32_t bodyLen = StageLinQ::readU32BE(fltxReadBuf.data());

                if (bodyLen == 0)
                {
                    // Service announcement or empty frame -- skip the 4-byte header
                    fltxReadBuf.erase(fltxReadBuf.begin(), fltxReadBuf.begin() + 4);
                    continue;
                }

                if (bodyLen > 1048576)
                {
                    // Malformed data -- discard first byte and try to resync
                    fltxReadBuf.erase(fltxReadBuf.begin());
                    continue;
                }

                if (fltxReadBuf.size() >= bodyLen + 4)
                {
                    resp = parseFltxBody(fltxReadBuf.data() + 4, bodyLen);
                    fltxReadBuf.erase(fltxReadBuf.begin(), fltxReadBuf.begin() + 4 + bodyLen);

                    // Device sent FileTransfer disconnect -- close socket to
                    // prevent stale reads.  chrisle changelog: "Handle Shutdown
                    // msg (0x9) from FileTransfer svc".
                    if (resp.messageId == StageLinQ::kFltxRespDisconnect)
                    {
                        std::lock_guard<std::mutex> lock2(sockMutex);
                        if (ftSocket)
                        {
                            ftSocket->close();
                            ftSocket.reset();
                        }
                    }

                    return resp;
                }
            }
        }
        return resp;
    }

    FltxResponse parseFltxBody(const uint8_t* body, uint32_t bodyLen)
    {
        FltxResponse resp;
        if (bodyLen < 8) return resp;

        // Verify fltx magic
        if (std::memcmp(body, StageLinQ::kFltxMagic, 4) != 0) return resp;

        uint32_t code = StageLinQ::readU32BE(body + 4);

        // If code > 0, it's a timecode message (ignored)
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
                    if (24 + resp.chunkSize <= bodyLen)
                    {
                        resp.chunkData.assign(body + 24, body + 24 + resp.chunkSize);
                    }
                }
                break;
            }

            case StageLinQ::kFltxRespEndOfMessage:
                break;

            case StageLinQ::kFltxRespDisconnect:
                DBG("StageLinQ DB: Device sent FileTransfer disconnect (0x9)");
                break;

            default:
                DBG("StageLinQ DB: Unknown fltx response " + juce::String(msgId));
                break;
        }

        return resp;
    }

    //==========================================================================
    // SQLite database operations
    //==========================================================================
    bool openDatabase(const juce::File& dbFile)
    {
        closeDatabase();

        int rc = sqlite3_open_v2(dbFile.getFullPathName().toRawUTF8(),
                                  &db, SQLITE_OPEN_READONLY, nullptr);
        if (rc != SQLITE_OK)
        {
            DBG("StageLinQ DB: SQLite open error: " + juce::String(sqlite3_errmsg(db)));
            db = nullptr;
            return false;
        }

        // Count tracks for logging
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM Track", -1, &stmt, nullptr) == SQLITE_OK)
        {
            if (sqlite3_step(stmt) == SQLITE_ROW)
            {
                DBG("StageLinQ DB: " + juce::String(sqlite3_column_int(stmt, 0)) + " tracks in database");
            }
            sqlite3_finalize(stmt);
        }

        return true;
    }

    void closeDatabase()
    {
        if (db)
        {
            sqlite3_close(db);
            db = nullptr;
        }
        dbReady.store(false);
    }

    //==========================================================================
    // Process a track metadata request
    //==========================================================================
    void processTrackRequest(const juce::String& networkPath)
    {
        if (!db) return;

        auto trackPath = parseNetworkPathToDbPath(networkPath);
        if (trackPath.isEmpty()) return;

        // Check cache (quick, under lock)
        {
            std::lock_guard<std::mutex> lock(cacheMutex);
            if (trackCache.count(trackPath.toStdString()) > 0) return;
        }

        // --- All DB queries OUTSIDE the lock (may take time) ---

        // Query Track table
        DenonTrackMeta meta = queryTrack(trackPath);
        if (!meta.valid) return;

        // Query artwork (no lock held during I/O)
        juce::Image artImg;
        if (meta.albumArtId > 0)
            artImg = queryArtwork(meta.albumArtId);

        // Query overview waveform
        auto wf = queryOverviewWaveform(trackPath);

        // Query performance data (cues, loops, beat grid)
        auto perf = queryPerformanceData(trackPath);

        // --- Insert results into caches (lock once) ---
        {
            std::lock_guard<std::mutex> lock(cacheMutex);
            trackCache[trackPath.toStdString()] = meta;

            if (meta.albumArtId > 0 && artImg.isValid()
                && artworkCache.count(meta.albumArtId) == 0)
                artworkCache[meta.albumArtId] = artImg;

            if (wf.valid && waveformCache.count(trackPath.toStdString()) == 0)
                waveformCache[trackPath.toStdString()] = std::move(wf);

            if (perf.valid && perfCache.count(trackPath.toStdString()) == 0)
                perfCache[trackPath.toStdString()] = std::move(perf);
        }

        DBG("StageLinQ DB: Loaded metadata for " + meta.artist + " - " + meta.title
            + " (art=" + juce::String(meta.albumArtId) + ")");
    }

    //==========================================================================
    // SQLite query: Track table
    //==========================================================================
    DenonTrackMeta queryTrack(const juce::String& trackPath)
    {
        DenonTrackMeta meta;
        if (!db) return meta;

        // Streaming tracks (Beatsource/Tidal) are queried by "uri" column
        // instead of "path" (matching chrisle/StageLinq DbConnection.ts)
        bool isStreaming = trackPath.startsWith("streaming://");

        // Engine DJ schema v1.x uses "idAlbumArt", v2.x uses "albumArtId".
        // We download v2 first (Database2/m.db), falling back to v1.
        // Try v2 column name first, fall back to v1 on failure.
        juce::String whereCol = isStreaming ? "uri" : "path";
        juce::String sqlV2Str = "SELECT title, artist, album, genre, key, bpm, length, albumArtId "
                                "FROM Track WHERE " + whereCol + " = ? LIMIT 1";
        juce::String sqlV1Str = "SELECT title, artist, album, genre, key, bpm, length, idAlbumArt "
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
            meta.valid = true;
        }

        sqlite3_finalize(stmt);
        return meta;
    }

    //==========================================================================
    // SQLite query: AlbumArt table -> juce::Image
    //==========================================================================
    juce::Image queryArtwork(int albumArtId)
    {
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

            if (blob && blobSize > 0)
            {
                // AlbumArt BLOBs are standard JPEG/PNG images
                juce::MemoryInputStream stream(blob, (size_t)blobSize, false);
                auto format = juce::ImageFileFormat::findImageFormatForStream(stream);
                if (format)
                {
                    stream.setPosition(0);
                    result = format->decodeImage(stream);
                }
            }
        }

        sqlite3_finalize(stmt);
        return result;
    }

    //==========================================================================
    // SQLite query: Track.overviewWaveFormData -> DenonWaveformData
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
    DenonWaveformData queryOverviewWaveform(const juce::String& trackPath)
    {
        DenonWaveformData result;
        if (!db) return result;

        const char* col = trackPath.startsWith("streaming://") ? "uri" : "path";
        juce::String sqlStr = juce::String("SELECT overviewWaveFormData FROM Track WHERE ")
                            + col + " = ? LIMIT 1";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sqlStr.toRawUTF8(), -1, &stmt, nullptr) != SQLITE_OK) return result;

        sqlite3_bind_text(stmt, 1, trackPath.toRawUTF8(), -1, SQLITE_TRANSIENT);

        if (sqlite3_step(stmt) == SQLITE_ROW)
        {
            const void* blob = sqlite3_column_blob(stmt, 0);
            int blobSize = sqlite3_column_bytes(stmt, 0);

            if (blob && blobSize > 4)
            {
                result = decodeOverviewWaveform(static_cast<const uint8_t*>(blob), blobSize);
            }
        }

        sqlite3_finalize(stmt);
        return result;
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

        // Parse header
        int64_t numEntries1 = (int64_t)StageLinQ::readU64BE(data);
        int64_t numEntries2 = (int64_t)StageLinQ::readU64BE(data + 8);
        // double samplesPerPoint = StageLinQ::readF64BE(data + 16);  // not used

        if (numEntries1 != numEntries2 || numEntries1 <= 0) return result;
        if (dataSize < 24 + (int)(numEntries1 * 3) + 3) return result;

        int numEntries = (int)numEntries1;

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
    // SQLite query: performance data (quickCues, loops, beatData BLOBs)
    //==========================================================================
    DenonPerformanceData queryPerformanceData(const juce::String& trackPath)
    {
        DenonPerformanceData result;
        if (!db) return result;

        const char* col = trackPath.startsWith("streaming://") ? "uri" : "path";
        juce::String sqlStr = juce::String("SELECT quickCues, loops, beatData FROM Track WHERE ")
                            + col + " = ? LIMIT 1";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sqlStr.toRawUTF8(), -1, &stmt, nullptr) != SQLITE_OK) return result;

        sqlite3_bind_text(stmt, 1, trackPath.toRawUTF8(), -1, SQLITE_TRANSIENT);

        if (sqlite3_step(stmt) == SQLITE_ROW)
        {
            // Quick cues (zlib compressed)
            const void* cueBlob = sqlite3_column_blob(stmt, 0);
            int cueBlobSize = sqlite3_column_bytes(stmt, 0);
            if (cueBlob && cueBlobSize > 4)
                decodeQuickCues(static_cast<const uint8_t*>(cueBlob), cueBlobSize, result);

            // Loops (NOT compressed -- raw binary)
            const void* loopBlob = sqlite3_column_blob(stmt, 1);
            int loopBlobSize = sqlite3_column_bytes(stmt, 1);
            if (loopBlob && loopBlobSize >= 8)
                decodeLoops(static_cast<const uint8_t*>(loopBlob), loopBlobSize, result);

            // Beat data (zlib compressed)
            const void* beatBlob = sqlite3_column_blob(stmt, 2);
            int beatBlobSize = sqlite3_column_bytes(stmt, 2);
            if (beatBlob && beatBlobSize > 4)
                decodeBeatData(static_cast<const uint8_t*>(beatBlob), beatBlobSize, result);

            result.valid = true;
        }

        sqlite3_finalize(stmt);
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
            double defaultCue = StageLinQ::readF64BE(p); p += 8;
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
        int32_t expectedSize = (int32_t)StageLinQ::readU32BE(blob);
        if (expectedSize <= 0 || expectedSize > 10 * 1024 * 1024) return {};

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
    // Parse TrackNetworkPath to DB-compatible track path
    //
    // Format: net://uuid/source/Engine Library/Music/Artist/Track.mp3
    //   -> DB path: Artist/Track.mp3  (relative to Music folder)
    //
    // Per chrisle/StageLinq trackPath.ts:
    //   - Standard: strip "net://uuid/source/Engine Library/Music/"
    //   - Non-standard: prefix with "../" (outside Engine Library)
    //==========================================================================
    static juce::String parseNetworkPathToDbPath(const juce::String& networkPath)
    {
        if (networkPath.isEmpty()) return {};

        // Streaming tracks (Beatsource, Tidal, SoundCloud) use "streaming://" prefix.
        // chrisle/StageLinq queries these by URI column instead of path.
        // We return the raw streaming URL and queryTrack handles the WHERE clause.
        if (networkPath.startsWith("streaming://"))
            return networkPath;

        if (!networkPath.startsWith("net://"))
            return {};

        auto parts = juce::StringArray::fromTokens(networkPath, "/", "");
        // parts[0]="net:", [1]="", [2]=uuid, [3]=source, [4+]=path

        if (parts.size() < 5) return {};

        // Check if Engine Library/Music path
        if (parts.size() > 6 && parts[4] == "Engine Library" && parts[5] == "Music")
        {
            // Standard: relative to Music folder
            juce::StringArray trackParts;
            for (int i = 6; i < parts.size(); ++i)
                trackParts.add(parts[i]);
            return trackParts.joinIntoString("/");
        }
        else if (parts.size() > 5 && parts[4] == "Engine Library")
        {
            // Engine Library but not Music
            juce::StringArray trackParts;
            for (int i = 5; i < parts.size(); ++i)
                trackParts.add(parts[i]);
            return trackParts.joinIntoString("/");
        }
        else
        {
            // Outside Engine Library
            juce::StringArray trackParts;
            for (int i = 4; i < parts.size(); ++i)
                trackParts.add(parts[i]);
            return "../" + trackParts.joinIntoString("/");
        }
    }

    //==========================================================================
    // Member data
    //==========================================================================
    juce::String deviceIp;
    uint16_t ftPort = 0;
    uint8_t token[StageLinQ::kTokenLen] = {};

    std::unique_ptr<juce::StreamingSocket> ftSocket;
    std::mutex sockMutex;
    std::vector<uint8_t> fltxReadBuf;

    // SQLite handle
    sqlite3* db = nullptr;
    std::atomic<bool> dbReady { false };

    // Caches (protected by cacheMutex)
    mutable std::mutex cacheMutex;
    std::map<std::string, DenonTrackMeta> trackCache;
    std::map<int, juce::Image> artworkCache;
    std::map<std::string, DenonWaveformData> waveformCache;
    std::map<std::string, DenonPerformanceData> perfCache;

    // Pending requests (protected by requestMutex)
    std::mutex requestMutex;
    juce::StringArray pendingRequests;

    std::atomic<bool> isRunningFlag { false };
};
