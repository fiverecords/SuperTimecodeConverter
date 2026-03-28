// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter
//
// DbServerClient -- TCP client for Pioneer dbserver protocol (port 12523).
// Queries CDJ internal databases to retrieve track metadata (title, artist,
// album, key, genre, artwork) for rekordbox tracks loaded on the decks.
//
// Protocol reference: DJ Link Ecosystem Analysis
//   https://djl-analysis.deepsymmetry.org/djl-analysis/track_metadata.html
//
// Thread model:
//   - UI thread calls requestMetadata() / getCachedMetadata() / getCachedArtwork()
//   - Background thread (juce::Thread) processes requests via SPSC queue
//   - Results stored in SpinLock-protected LRU cache
//
// Player number constraint:
//   The dbserver only responds to queries from player numbers 1-4.
//   If the VCDJ is using number >=5, metadata queries will fail gracefully
//   (returns empty metadata, UI falls back to "Track #12345").

#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <array>
#include <unordered_map>
#include <vector>
#include <utility>
#include <cstring>
#include <thread>
#include "NfsAnlzFetcher.h"
#include "WaveformCache.h"
#include "AppSettings.h"

//==============================================================================
// TrackMetadata -- POD-like struct for cached metadata results
//==============================================================================
struct TrackMetadata
{
    uint32_t trackId = 0;
    juce::String title;
    juce::String artist;
    juce::String album;
    juce::String genre;
    juce::String key;
    juce::String comment;
    juce::String dateAdded;        // "yyyy-mm-dd"
    juce::String anlzPath;         // ANLZ file path from dbserver (e.g. "PIONEER/USBANLZ/P053/0000/ANLZ0006.DAT")
    int durationSeconds = 0;
    int bpmTimes100 = 0;           // e.g. 12800 = 128.00 BPM
    int rating = 0;                // 0-5 stars
    uint32_t artworkId = 0;        // for separate artwork request
    double cacheTime = 0.0;        // when this entry was cached (for LRU)

    // Color preview waveform data from CDJ
    //   ThreeBand (PWV6): 3 bytes/entry = {mid, high, low} frequency heights
    //   ColorNxs2 (PWV4): 6 bytes/entry = {d0, d1, d2, d3(R), d4(G), d5(B)}
    std::vector<uint8_t> waveformData;
    int waveformEntryCount = 0;
    int waveformBytesPerEntry = 0;  // 3 = ThreeBand, 6 = ColorNxs2
    bool waveformQueried = false;   // true once waveform fetch has been attempted
    bool nfsAttempted = false;      // true once NFS ANLZ fetch has been attempted
    uint32_t cacheVersion = 0;      // incremented each time any field is updated

    // Detail waveform (high-res scrolling view, 150 entries/sec of audio)
    //   PWV5 (NXS2 color): 2 bytes/entry -- height + color encoding
    //   PWV7 (CDJ-3000 3-band): 3 bytes/entry -- {mid, high, low} heights
    std::vector<uint8_t> detailData;
    int detailEntryCount = 0;
    int detailBytesPerEntry = 0;    // 2 = PWV5, 3 = PWV7
    static constexpr int kDetailEntriesPerSecond = 150;

    // Beat grid (PQTZ tag -- positions of every beat in the track)
    struct BeatEntry
    {
        uint16_t beatNumber;
        uint16_t bpmTimes100;   // BPM * 100
        uint32_t timeMs;        // position in ms from start of track
    };
    std::vector<BeatEntry> beatGrid;

    // Song structure / phrase analysis (PSSI tag -- rekordbox 6+)
    struct PhraseEntry
    {
        uint16_t index;          // phrase sequential index
        uint16_t beatNumber;     // first beat of this phrase
        uint16_t kind;           // phrase type (see kPhrase* constants)
        uint8_t  fill;           // fill-in type (0-3)
        uint16_t beatCount;      // number of beats in this phrase
        uint16_t beatFill;       // beat number at which fill starts
    };
    static constexpr uint16_t kPhraseMoodHigh = 1;
    static constexpr uint16_t kPhraseMoodMid  = 2;
    static constexpr uint16_t kPhraseMoodLow  = 3;
    uint16_t phraseMood = 0;     // overall mood (1=high, 2=mid, 3=low)
    std::vector<PhraseEntry> songStructure;

    // Rekordbox cue list (hot cues + memory points + loops from CDJ)
    // Downloaded from ANLZ PCO2 (nxs2 extended) or PCOB (standard) tags.
    struct RekordboxCue
    {
        enum Type : uint8_t { MemoryPoint = 0, HotCue = 1, Loop = 2 };
        Type     type         = MemoryPoint;
        uint8_t  hotCueNumber = 0;      // 0=memory, 1=A, 2=B, 3=C, ...
        uint32_t positionMs   = 0;
        uint32_t loopEndMs    = 0;      // non-zero for loops
        uint8_t  colorR = 30, colorG = 200, colorB = 60;  // default green
        uint8_t  colorCode = 0;           // rekordbox color table index
        bool     hasColor     = false;  // true if DJ assigned a custom color
        juce::String comment;           // DJ-assigned label text

        /// Hot cue letter (A, B, C...) or empty for memory points
        juce::String hotCueLetter() const
        {
            if (hotCueNumber == 0) return {};
            if (hotCueNumber <= 26)
                return juce::String::charToString((juce::juce_wchar)('A' + hotCueNumber - 1));
            return juce::String(hotCueNumber);
        }

        juce::Colour getColour() const
        {
            return juce::Colour(colorR, colorG, colorB);
        }
    };
    std::vector<RekordboxCue> cueList;

    bool isValid() const { return trackId != 0 && title.isNotEmpty(); }
    bool hasWaveform() const { return waveformEntryCount > 0 && !waveformData.empty(); }
    bool hasDetailWaveform() const { return detailEntryCount > 0 && !detailData.empty(); }
    bool hasBeatGrid() const { return !beatGrid.empty(); }
    bool hasSongStructure() const { return !songStructure.empty(); }
    bool hasCueList() const { return !cueList.empty(); }
    bool isFullyCached() const { return isValid() && waveformQueried; }
    bool isThreeBandWaveform() const { return waveformBytesPerEntry == 3; }

    /// Find the beat grid entry closest to a given ms position.
    /// Returns nullptr if beat grid is empty.
    const BeatEntry* getBeatAt(uint32_t ms) const
    {
        if (beatGrid.empty()) return nullptr;
        // Binary search for the last beat <= ms
        int lo = 0, hi = (int)beatGrid.size() - 1, best = 0;
        while (lo <= hi)
        {
            int mid = (lo + hi) / 2;
            if (beatGrid[(size_t)mid].timeMs <= ms)
                { best = mid; lo = mid + 1; }
            else
                hi = mid - 1;
        }
        return &beatGrid[(size_t)best];
    }

    /// Convert ms position to detail waveform entry index.
    int msToDetailIndex(uint32_t ms) const
    {
        return (int)((uint64_t)ms * kDetailEntriesPerSecond / 1000);
    }
};

//==============================================================================
// DbServerClient -- background TCP client for CDJ metadata queries
//==============================================================================
class DbServerClient : private juce::Thread
{
public:
    DbServerClient()
        : Thread("DbServer Client")
    {
    }

    ~DbServerClient() override
    {
        stop();
    }

    //==========================================================================
    // Lifecycle
    //==========================================================================
    void start()
    {
        if (isThreadRunning()) return;
        DBG("DbServerClient: starting background thread...");
        isRunningFlag.store(true, std::memory_order_relaxed);
        startThread(juce::Thread::Priority::low);
    }

    void stop()
    {
        isRunningFlag.store(false, std::memory_order_relaxed);

        // Wake the thread if it's waiting
        requestSemaphore.signal();

        if (isThreadRunning())
            stopThread(3000);

        // Wait for any in-flight NFS download to finish
        if (nfsThread.joinable())
            nfsThread.join();

        // Close all connections
        for (auto& conn : connections)
            conn.close();
    }

    bool getIsRunning() const { return isRunningFlag.load(std::memory_order_relaxed); }

    //==========================================================================
    // Request metadata (called from UI/engine thread)
    //
    //   playerIP:  IP address of the CDJ that loaded the track
    //   slot:      media slot (2=SD, 3=USB)
    //   trackType: 1=rekordbox, 2=non-rekordbox, 5=CD
    //   trackId:   rekordbox database ID (from CDJ Status packet)
    //   ourPlayer: our VCDJ player number (MUST be 1-4 for queries to work)
    //==========================================================================
    void requestMetadata(const juce::String& playerIP, uint8_t slot,
                         uint8_t trackType, uint32_t trackId, int ourPlayer,
                         const juce::String& playerModel = {})
    {
        if (trackId == 0 || playerIP.isEmpty()) return;
        if (ourPlayer < 1 || ourPlayer > 6)
        {
            // dbserver typically requires 1-4, but try 5-6 as fallback
            DBG("DbServerClient: player number " + juce::String(ourPlayer)
                + " is outside expected range, metadata query may fail");
        }

        DBG("DbServerClient: requestMetadata trackId=" + juce::String(trackId)
            + " from " + playerIP + " slot=" + juce::String(slot)
            + " ourPlayer=" + juce::String(ourPlayer)
            + " model=" + playerModel);

        // Check cache first -- avoid unnecessary requests.
        // Use isFullyCached() so that entries with metadata but no waveform
        // (partial cache from an early query before CDJ was fully ready)
        // are re-requested to fetch the missing waveform data.
        uint64_t cacheKey = makeCacheKey(playerIP, trackId);
        {
            const juce::SpinLock::ScopedLockType lock(cacheLock);
            auto it = metadataCache.find(cacheKey);
            if (it != metadataCache.end() && it->second.isFullyCached())
                return;  // fully cached (metadata + waveform attempted)
        }

        // Enqueue request (producer lock: any thread may call this)
        const juce::SpinLock::ScopedLockType producerLock(queueProducerLock);
        uint32_t wp = reqWritePos.load(std::memory_order_relaxed);
        uint32_t rp = reqReadPos.load(std::memory_order_acquire);
        if (wp - rp >= kRequestQueueSize)
            return;  // queue full, drop request (will retry on next track change)

        auto& req = requestQueue[wp & kRequestQueueMask];
        req.playerIP    = playerIP;
        req.playerModel = playerModel;
        req.slot        = slot;
        req.trackType   = trackType;
        req.trackId     = trackId;
        req.ourPlayer   = (uint8_t)ourPlayer;
        req.wantArt     = true;
        req.wantWaveform = true;

        reqWritePos.store(wp + 1, std::memory_order_release);
        requestSemaphore.signal();
    }

    //==========================================================================
    // Request artwork only (if metadata is cached but art isn't)
    //==========================================================================
    void requestArtwork(const juce::String& playerIP, uint8_t slot,
                        uint8_t trackType, uint32_t artworkId, int ourPlayer)
    {
        if (artworkId == 0 || playerIP.isEmpty()) return;
        if (ourPlayer < 1 || ourPlayer > 4) return;

        // Check artwork cache
        {
            const juce::SpinLock::ScopedLockType lock(artCacheLock);
            if (artworkCache.count(artworkId) > 0)
                return;  // already cached
        }

        const juce::SpinLock::ScopedLockType producerLock(queueProducerLock);
        uint32_t wp = reqWritePos.load(std::memory_order_relaxed);
        uint32_t rp = reqReadPos.load(std::memory_order_acquire);
        if (wp - rp >= kRequestQueueSize) return;

        auto& req = requestQueue[wp & kRequestQueueMask];
        req.playerIP  = playerIP;
        req.slot      = slot;
        req.trackType = trackType;
        req.trackId   = 0;       // signals "artwork-only request"
        req.artworkId = artworkId;
        req.ourPlayer = (uint8_t)ourPlayer;
        req.wantArt   = true;

        reqWritePos.store(wp + 1, std::memory_order_release);
        requestSemaphore.signal();
    }

    //==========================================================================
    // Retrieve cached metadata (called from UI/engine thread)
    //==========================================================================
    TrackMetadata getCachedMetadata(const juce::String& playerIP, uint32_t trackId) const
    {
        uint64_t key = makeCacheKey(playerIP, trackId);
        const juce::SpinLock::ScopedLockType lock(cacheLock);
        auto it = metadataCache.find(key);
        if (it != metadataCache.end())
            return it->second;
        return {};
    }

    /// Convenience: look up by trackId alone (searches all players)
    TrackMetadata getCachedMetadataByTrackId(uint32_t trackId) const
    {
        if (trackId == 0) return {};
        const juce::SpinLock::ScopedLockType lock(cacheLock);
        for (auto& [key, meta] : metadataCache)
        {
            if (meta.trackId == trackId)
                return meta;
        }
        return {};
    }

    /// Lightweight version check -- returns the cache version counter for a
    /// track without copying any data.  Use to skip expensive getCachedMetadata
    /// calls when nothing has changed in the background thread.
    uint32_t getMetadataVersion(const juce::String& playerIP, uint32_t trackId) const
    {
        uint64_t key = makeCacheKey(playerIP, trackId);
        const juce::SpinLock::ScopedLockType lock(cacheLock);
        auto it = metadataCache.find(key);
        return (it != metadataCache.end()) ? it->second.cacheVersion : 0;
    }

    /// Same as getMetadataVersion but by trackId only (searches all players).
    uint32_t getMetadataVersionByTrackId(uint32_t trackId) const
    {
        if (trackId == 0) return 0;
        const juce::SpinLock::ScopedLockType lock(cacheLock);
        for (auto& [key, meta] : metadataCache)
            if (meta.trackId == trackId) return meta.cacheVersion;
        return 0;
    }

    /// Lightweight check: does the cached metadata have detail waveform data?
    /// Used by the UI timer to avoid expensive full-copy getCachedMetadata()
    /// when only checking if detail data has arrived yet.
    bool hasDetailWaveformCached(const juce::String& playerIP, uint32_t trackId) const
    {
        uint64_t key = makeCacheKey(playerIP, trackId);
        const juce::SpinLock::ScopedLockType lock(cacheLock);
        auto it = metadataCache.find(key);
        if (it != metadataCache.end()) return it->second.hasDetailWaveform();
        for (auto& [k, m] : metadataCache)
            if (m.trackId == trackId) return m.hasDetailWaveform();
        return false;
    }

    /// Lightweight metadata lookup -- returns text fields and IDs only,
    /// skipping the waveform vector copy.  Use when you only need artist/title/
    /// key/BPM/artworkId (e.g. from getActiveTrackInfo at 60Hz).
    struct MetadataLight
    {
        uint32_t trackId = 0;
        juce::String title, artist, album, genre, key;
        int bpmTimes100 = 0;
        uint32_t artworkId = 0;
        int durationSeconds = 0;
        bool valid = false;
        bool isValid() const { return valid; }
    };

    MetadataLight getCachedMetadataLightById(uint32_t trackId) const
    {
        if (trackId == 0) return {};
        const juce::SpinLock::ScopedLockType lock(cacheLock);
        for (auto& [key, meta] : metadataCache)
        {
            if (meta.trackId == trackId && meta.isValid())
            {
                MetadataLight m;
                m.trackId         = meta.trackId;
                m.title           = meta.title;
                m.artist          = meta.artist;
                m.album           = meta.album;
                m.genre           = meta.genre;
                m.key             = meta.key;
                m.bpmTimes100     = meta.bpmTimes100;
                m.artworkId       = meta.artworkId;
                m.durationSeconds = meta.durationSeconds;
                m.valid           = true;
                return m;
            }
        }
        return {};
    }

    //==========================================================================
    // Retrieve cached artwork (returns null Image if not cached)
    //==========================================================================
    juce::Image getCachedArtwork(uint32_t artworkId) const
    {
        if (artworkId == 0) return {};
        const juce::SpinLock::ScopedLockType lock(artCacheLock);
        auto it = artworkCache.find(artworkId);
        if (it != artworkCache.end())
            return it->second;
        return {};
    }

    //==========================================================================
    // Invalidate cache for a player (call on media eject / player disconnect)
    //==========================================================================
    void invalidatePlayer(const juce::String& playerIP)
    {
        uint64_t prefix = ipToUint32(playerIP);
        prefix <<= 32;
        {
            const juce::SpinLock::ScopedLockType lock(cacheLock);
            for (auto it = metadataCache.begin(); it != metadataCache.end(); )
            {
                if ((it->first & 0xFFFFFFFF00000000ULL) == prefix)
                    it = metadataCache.erase(it);
                else
                    ++it;
            }
        }
        // Close connection to this player
        for (auto& conn : connections)
        {
            if (conn.playerIP == playerIP)
                conn.close();
        }
    }

    /// Clear all caches
    void clearCache()
    {
        {
            const juce::SpinLock::ScopedLockType lock(cacheLock);
            metadataCache.clear();
        }
        {
            const juce::SpinLock::ScopedLockType lock(artCacheLock);
            artworkCache.clear();
        }
    }

    /// Stats for UI/debugging
    int getCacheSize() const
    {
        const juce::SpinLock::ScopedLockType lock(cacheLock);
        return (int)metadataCache.size();
    }

    uint32_t getQueryCount() const  { return queryCount.load(std::memory_order_relaxed); }
    uint32_t getErrorCount() const  { return errorCount.load(std::memory_order_relaxed); }

private:
    /// Internal enqueue (called from background thread for phase 2 re-enqueue).
    void enqueueInternal(const juce::String& playerIP, const juce::String& playerModel,
                         uint8_t slot, uint8_t trackType, uint32_t trackId,
                         uint8_t ourPlayer, uint8_t phase)
    {
        const juce::SpinLock::ScopedLockType producerLock(queueProducerLock);
        uint32_t wp = reqWritePos.load(std::memory_order_relaxed);
        uint32_t rp = reqReadPos.load(std::memory_order_acquire);
        if (wp - rp >= kRequestQueueSize)
            return;  // queue full

        auto& r = requestQueue[wp & kRequestQueueMask];
        r.playerIP    = playerIP;
        r.playerModel = playerModel;
        r.slot        = slot;
        r.trackType   = trackType;
        r.trackId     = trackId;
        r.ourPlayer   = ourPlayer;
        r.wantArt     = false;
        r.wantWaveform = true;
        r.artworkId   = 0;
        r.phase       = phase;

        reqWritePos.store(wp + 1, std::memory_order_release);
        requestSemaphore.signal();
    }

    //==========================================================================
    // Constants
    //==========================================================================
    static constexpr int kPortDiscoveryPort = 12523;
    static constexpr int kDefaultDbPort     = 1051;
    static constexpr int kConnectTimeoutMs  = 3000;
    static constexpr int kReadTimeoutMs     = 3000;
    static constexpr int kMaxCacheEntries   = 256;
    static constexpr int kMaxArtCacheEntries = 64;
    static constexpr int kMaxConnections    = 6;
    static constexpr int kReconnectCooldownMs = 5000;

    // NXS2 extension constants for color waveform
    static constexpr uint32_t kNxs2ExtRequest  = 0x2c04;
    static constexpr uint32_t kNxs2ExtResponse = 0x4f02;
    // NXS2 color preview waveform (PWV4 from .EXT file)
    static constexpr uint32_t kMagic4VWP = 0x34565750;  // "PWV4" reversed
    static constexpr uint32_t kMagic5VWP = 0x35565750;  // "PWV5" reversed
    static constexpr uint32_t kMagicTXE  = 0x00545845;  // "EXT" reversed
    // CDJ-3000 3-band waveform (PWV6/PWV7 from .2EX file)
    static constexpr uint32_t kMagic6VWP = 0x36565750;  // "PWV6" reversed -- 3-band preview
    static constexpr uint32_t kMagic7VWP = 0x37565750;  // "PWV7" reversed -- 3-band detail
    static constexpr uint32_t kMagicXE2  = 0x00584532;  // "2EX" reversed

    // Beat grid query (standard dbserver request type)
    static constexpr uint16_t kBeatGridRequest = 0x2204;

    // ANLZ tag magic values for 0x2c04 ext requests (little-endian uint32 of ASCII)
    // Same reversed convention as PWV4 etc.: "PQTZ" -> Z=5A T=54 Q=51 P=50
    static constexpr uint32_t kMagicPQTZ = 0x5A545150;  // "PQTZ" reversed -- beat grid
    static constexpr uint32_t kMagicPSSI = 0x49535350;  // "PSSI" reversed -- song structure
    static constexpr uint32_t kMagicPCO2 = 0x324F4350;  // "PCO2" reversed -- extended cue list (nxs2+)
    static constexpr uint32_t kMagicPCOB = 0x424F4350;  // "PCOB" reversed -- standard cue list

    static constexpr uint32_t kRequestQueueSize = 32;
    static constexpr uint32_t kRequestQueueMask = kRequestQueueSize - 1;

    // Protocol magic
    static constexpr uint32_t kMessageMagic = 0x872349AE;

    //==========================================================================
    // Request queue entry
    //==========================================================================
    struct MetadataRequest
    {
        juce::String playerIP;
        juce::String playerModel;  // e.g. "CDJ-3000", "CDJ-2000NXS2"
        uint32_t trackId   = 0;
        uint32_t artworkId = 0;
        uint8_t  slot      = 0;
        uint8_t  trackType = 1;
        uint8_t  ourPlayer = 1;
        bool     wantArt   = false;
        bool     wantWaveform = false;
        uint8_t  phase     = 1;    // 1=critical (meta+art+preview), 2=supplementary (beats+detail+cues+NFS)
    };

    //==========================================================================
    // Per-player TCP connection
    //==========================================================================
    struct PlayerConnection
    {
        juce::String playerIP;
        std::unique_ptr<juce::StreamingSocket> socket;
        int dbPort = 0;
        bool contextSetUp = false;
        uint32_t txId = 0;
        double lastFailTime = 0.0;

        bool isConnected() const { return socket && socket->isConnected(); }

        void close()
        {
            if (socket)
            {
                socket->close();
                socket = nullptr;
            }
            contextSetUp = false;
            txId = 0;
            dbPort = 0;
            playerIP.clear();
        }
    };

    //==========================================================================
    // Cache key: high 32 bits = IP, low 32 bits = trackId
    //==========================================================================
    static uint64_t makeCacheKey(const juce::String& ip, uint32_t trackId)
    {
        return (static_cast<uint64_t>(ipToUint32(ip)) << 32) | trackId;
    }

    static uint32_t ipToUint32(const juce::String& ip)
    {
        juce::StringArray parts;
        parts.addTokens(ip, ".", "");
        if (parts.size() != 4) return 0;
        return (uint32_t(parts[0].getIntValue() & 0xFF) << 24)
             | (uint32_t(parts[1].getIntValue() & 0xFF) << 16)
             | (uint32_t(parts[2].getIntValue() & 0xFF) << 8)
             | (uint32_t(parts[3].getIntValue() & 0xFF));
    }

    //==========================================================================
    // BINARY PROTOCOL -- Message building helpers
    //==========================================================================

    /// Write a 4-byte number field: [0x11, big-endian uint32]
    static void writeField32(juce::MemoryOutputStream& out, uint32_t value)
    {
        out.writeByte(0x11);
        out.writeByte((int)((value >> 24) & 0xFF));
        out.writeByte((int)((value >> 16) & 0xFF));
        out.writeByte((int)((value >> 8)  & 0xFF));
        out.writeByte((int)(value & 0xFF));
    }

    /// Write a 2-byte number field: [0x10, big-endian uint16]
    static void writeField16(juce::MemoryOutputStream& out, uint16_t value)
    {
        out.writeByte(0x10);
        out.writeByte((int)((value >> 8) & 0xFF));
        out.writeByte((int)(value & 0xFF));
    }

    /// Write a 1-byte number field: [0x0F, uint8]
    static void writeField8(juce::MemoryOutputStream& out, uint8_t value)
    {
        out.writeByte(0x0F);
        out.writeByte((int)value);
    }

    /// Build a complete dbserver message
    /// args: vector of (tag, value) for number args
    static juce::MemoryBlock buildMessage(uint32_t txId, uint16_t type,
                                           const std::vector<uint32_t>& numArgs)
    {
        juce::MemoryOutputStream out;

        // Magic
        writeField32(out, kMessageMagic);
        // TxID
        writeField32(out, txId);
        // Type (2-byte field)
        writeField16(out, type);
        // Argument count (1-byte field)
        writeField8(out, (uint8_t)numArgs.size());

        // Argument type tags -- always a 12-byte blob
        out.writeByte(0x14);  // blob type
        // Length: 12 (always)
        out.writeByte(0x00);
        out.writeByte(0x00);
        out.writeByte(0x00);
        out.writeByte(0x0C);
        // 12 tag bytes (0x06 = 4-byte int)
        for (int i = 0; i < 12; i++)
            out.writeByte(i < (int)numArgs.size() ? 0x06 : 0x00);

        // Argument fields
        for (auto val : numArgs)
            writeField32(out, val);

        return out.getMemoryBlock();
    }

    /// Build the DMST argument (first arg of most queries)
    static uint32_t makeDMST(uint8_t ourPlayer, uint8_t menuLoc, uint8_t slot, uint8_t trackType)
    {
        return (uint32_t(ourPlayer) << 24)
             | (uint32_t(menuLoc)   << 16)
             | (uint32_t(slot)      << 8)
             | uint32_t(trackType);
    }

    //==========================================================================
    // BINARY PROTOCOL -- Message reading helpers
    //==========================================================================

    /// Read exactly N bytes from socket with timeout.  Returns false on failure.
    static bool readExact(juce::StreamingSocket& sock, void* dest, int numBytes, int timeoutMs)
    {
        auto* ptr = static_cast<uint8_t*>(dest);
        int remaining = numBytes;
        auto deadline = juce::Time::getMillisecondCounterHiRes() + timeoutMs;

        while (remaining > 0)
        {
            if (juce::Time::getMillisecondCounterHiRes() > deadline)
                return false;

            if (!sock.waitUntilReady(true, 100))
                continue;

            int n = sock.read(ptr, remaining, false);
            if (n <= 0) return false;
            ptr += n;
            remaining -= n;
        }
        return true;
    }

    /// Read a single field from the socket.  Returns the numeric value
    /// (for blobs/strings returns the length), and fills `fieldType`.
    /// For blobs, the data is appended to `blobOut`.
    struct FieldResult
    {
        uint8_t type = 0;
        uint32_t numericValue = 0;
        juce::String stringValue;
        juce::MemoryBlock blobData;     // raw blob bytes (for artwork)
        bool ok = false;
    };

    static FieldResult readField(juce::StreamingSocket& sock, int timeoutMs)
    {
        FieldResult result;
        uint8_t typeByte = 0;
        if (!readExact(sock, &typeByte, 1, timeoutMs))
            return result;

        result.type = typeByte;

        switch (typeByte)
        {
            case 0x0F:  // 1-byte int
            {
                uint8_t v = 0;
                if (!readExact(sock, &v, 1, timeoutMs)) return result;
                result.numericValue = v;
                result.ok = true;
                break;
            }
            case 0x10:  // 2-byte int (big-endian)
            {
                uint8_t buf[2];
                if (!readExact(sock, buf, 2, timeoutMs)) return result;
                result.numericValue = (uint32_t(buf[0]) << 8) | buf[1];
                result.ok = true;
                break;
            }
            case 0x11:  // 4-byte int (big-endian)
            {
                uint8_t buf[4];
                if (!readExact(sock, buf, 4, timeoutMs)) return result;
                result.numericValue = (uint32_t(buf[0]) << 24) | (uint32_t(buf[1]) << 16)
                                    | (uint32_t(buf[2]) << 8)  | buf[3];
                result.ok = true;
                break;
            }
            case 0x14:  // binary blob -- CAPTURE data for artwork etc.
            {
                uint8_t lenBuf[4];
                if (!readExact(sock, lenBuf, 4, timeoutMs)) return result;
                uint32_t len = (uint32_t(lenBuf[0]) << 24) | (uint32_t(lenBuf[1]) << 16)
                             | (uint32_t(lenBuf[2]) << 8)  | lenBuf[3];
                result.numericValue = len;
                if (len > 2 * 1024 * 1024)  // over 2MB -- protocol error
                    return result;
                if (len > 0)
                {
                    result.blobData.setSize(len);
                    if (!readExact(sock, result.blobData.getData(), (int)len, timeoutMs))
                        return result;
                }
                result.ok = true;
                break;
            }
            case 0x26:  // UTF-16BE string
            {
                uint8_t lenBuf[4];
                if (!readExact(sock, lenBuf, 4, timeoutMs)) return result;
                uint32_t charCount = (uint32_t(lenBuf[0]) << 24) | (uint32_t(lenBuf[1]) << 16)
                                   | (uint32_t(lenBuf[2]) << 8)  | lenBuf[3];
                result.numericValue = charCount;
                if (charCount > 0x10000)  // over 64K chars -- protocol error
                    return result;
                if (charCount > 0)
                {
                    uint32_t byteCount = charCount * 2;
                    std::vector<uint8_t> data(byteCount);
                    if (!readExact(sock, data.data(), (int)byteCount, timeoutMs))
                        return result;
                    // Decode UTF-16BE, skip trailing NUL
                    juce::String decoded;
                    for (uint32_t i = 0; i + 1 < byteCount; i += 2)
                    {
                        juce::juce_wchar ch = (juce::juce_wchar)((uint16_t(data[i]) << 8) | data[i + 1]);
                        if (ch != 0)
                            decoded += juce::String::charToString(ch);
                    }
                    result.stringValue = decoded;
                }
                result.ok = true;
                break;
            }
            default:
                // Unknown field type -- protocol error
                DBG("DbServerClient: unknown field type 0x" + juce::String::toHexString(typeByte));
                break;
        }
        return result;
    }

    //==========================================================================
    // Response message structure
    //==========================================================================
    struct ResponseMessage
    {
        uint32_t txId = 0;
        uint16_t type = 0;
        uint8_t  argCount = 0;
        uint8_t  argTags[12] {};

        // Up to 12 arguments -- numbers, strings, and blobs
        uint32_t numArgs[12] {};
        juce::String strArgs[12];       // for string-type arguments
        juce::MemoryBlock blobArgs[12]; // for blob-type arguments (artwork JPEG etc.)
        bool ok = false;
    };

    /// Read a complete dbserver response message from the socket.
    static ResponseMessage readMessage(juce::StreamingSocket& sock, int timeoutMs)
    {
        ResponseMessage msg;

        // 1. Magic field (4-byte int = 0x872349AE)
        auto magic = readField(sock, timeoutMs);
        if (!magic.ok || magic.numericValue != kMessageMagic) return msg;

        // 2. TxID (4-byte int)
        auto txField = readField(sock, timeoutMs);
        if (!txField.ok) return msg;
        msg.txId = txField.numericValue;

        // 3. Type (2-byte int)
        auto typeField = readField(sock, timeoutMs);
        if (!typeField.ok) return msg;
        msg.type = (uint16_t)typeField.numericValue;

        // 4. Arg count (1-byte int)
        auto argcField = readField(sock, timeoutMs);
        if (!argcField.ok) return msg;
        msg.argCount = (uint8_t)argcField.numericValue;

        // 5. Tags blob (always 12 bytes inside a blob field)
        auto tagsField = readField(sock, timeoutMs);
        if (!tagsField.ok) return msg;
        // Tags blob data is discarded -- we don't need it because each argument
        // field is self-typed via its leading type byte (0x0F/0x10/0x11/0x14/0x26).
        // The tag bytes in the header are redundant per the protocol analysis.

        // Read arguments -- field type byte tells us whether it's int, string, or blob
        for (int i = 0; i < (int)msg.argCount && i < 12; i++)
        {
            auto arg = readField(sock, timeoutMs);
            if (!arg.ok) return msg;
            msg.numArgs[i] = arg.numericValue;
            if (arg.type == 0x26)
                msg.strArgs[i] = arg.stringValue;
            if (arg.type == 0x14 && arg.blobData.getSize() > 0)
                msg.blobArgs[i] = std::move(arg.blobData);
        }

        msg.ok = true;
        return msg;
    }

    //==========================================================================
    // Read a blob field and return the raw data
    //==========================================================================
    static bool readBlobField(juce::StreamingSocket& sock, juce::MemoryBlock& out, int timeoutMs)
    {
        uint8_t typeByte = 0;
        if (!readExact(sock, &typeByte, 1, timeoutMs) || typeByte != 0x14)
            return false;

        uint8_t lenBuf[4];
        if (!readExact(sock, lenBuf, 4, timeoutMs))
            return false;

        uint32_t len = (uint32_t(lenBuf[0]) << 24) | (uint32_t(lenBuf[1]) << 16)
                     | (uint32_t(lenBuf[2]) << 8)  | lenBuf[3];

        if (len == 0 || len > 2 * 1024 * 1024)  // max 2MB for artwork
            return false;

        out.setSize(len);
        return readExact(sock, out.getData(), (int)len, timeoutMs);
    }

    //==========================================================================
    // CONNECTION MANAGEMENT
    //==========================================================================

    /// Get or create a connection to a player.  Returns nullptr on failure.
    PlayerConnection* getConnection(const juce::String& playerIP, uint8_t ourPlayer)
    {
        // Find existing connection
        for (auto& conn : connections)
        {
            if (conn.playerIP == playerIP && conn.isConnected())
                return &conn;
        }

        // Find empty slot or recycle oldest
        PlayerConnection* slot = nullptr;
        for (auto& conn : connections)
        {
            if (conn.playerIP.isEmpty())
            {
                slot = &conn;
                break;
            }
        }
        if (!slot)
        {
            // Recycle first non-matching connection
            for (auto& conn : connections)
            {
                if (conn.playerIP != playerIP)
                {
                    conn.close();
                    conn.lastFailTime = 0.0;  // don't apply old player's cooldown
                    slot = &conn;
                    break;
                }
            }
        }
        if (!slot) return nullptr;

        // Check reconnect cooldown
        double now = juce::Time::getMillisecondCounterHiRes();
        if (now - slot->lastFailTime < kReconnectCooldownMs)
            return nullptr;

        // Step 1: Discover database port via port 12523
        int dbPort = discoverDbPort(playerIP);
        if (dbPort <= 0)
        {
            DBG("DbServerClient: no valid db port found for " + playerIP);
            slot->lastFailTime = now;
            return nullptr;
        }

        // Step 2: Connect to database port
        auto sock = std::make_unique<juce::StreamingSocket>();
        if (!sock->connect(playerIP, dbPort, kConnectTimeoutMs))
        {
            DBG("DbServerClient: TCP connect to " + playerIP + ":"
                + juce::String(dbPort) + " failed");
            slot->lastFailTime = now;
            return nullptr;
        }

        // Step 3: Send initial handshake -- [0x11, 0x00000001]
        {
            uint8_t hello[] = { 0x11, 0x00, 0x00, 0x00, 0x01 };
            if (sock->write(hello, sizeof(hello)) != sizeof(hello))
            {
                DBG("DbServerClient: handshake write failed to " + playerIP);
                slot->lastFailTime = now;
                return nullptr;
            }
            // Expect same 5 bytes back
            uint8_t reply[5];
            if (!readExact(*sock, reply, 5, kReadTimeoutMs))
            {
                DBG("DbServerClient: handshake reply timeout from " + playerIP);
                slot->lastFailTime = now;
                return nullptr;
            }
            DBG("DbServerClient: handshake OK with " + playerIP + ":" + juce::String(dbPort));
        }

        slot->socket = std::move(sock);
        slot->playerIP = playerIP;
        slot->dbPort = dbPort;
        slot->contextSetUp = false;
        slot->txId = 0;
        slot->lastFailTime = 0.0;

        // Step 4: Setup query context
        if (!setupQueryContext(*slot, ourPlayer))
        {
            slot->close();
            slot->lastFailTime = juce::Time::getMillisecondCounterHiRes();
            return nullptr;
        }

        DBG("DbServerClient: connected to " + playerIP + ":" + juce::String(dbPort));
        return slot;
    }

    /// Discover the dbserver port by querying TCP 12523
    int discoverDbPort(const juce::String& playerIP)
    {
        DBG("DbServerClient: discovering db port on " + playerIP + ":12523");

        juce::StreamingSocket sock;
        if (!sock.connect(playerIP, kPortDiscoveryPort, kConnectTimeoutMs))
        {
            DBG("DbServerClient: port discovery TCP connect to " + playerIP + ":12523 failed");
            return 0;
        }

        // Query: length=0x0F (as Int32ub) + "RemoteDBServer\0"
        const uint8_t query[] = {
            0x00, 0x00, 0x00, 0x0F,   // length = 15
            0x52, 0x65, 0x6d, 0x6f,   // "Remo"
            0x74, 0x65, 0x44, 0x42,   // "teDB"
            0x53, 0x65, 0x72, 0x76,   // "Serv"
            0x65, 0x72, 0x00          // "er\0"
        };

        if (sock.write(query, sizeof(query)) != sizeof(query))
        {
            DBG("DbServerClient: port discovery write failed");
            return 0;
        }

        // Response: exactly 2 bytes -- uint16 big-endian port number
        // (confirmed by python-prodj-link: DBServerReply = Int16ub)
        uint8_t response[2];
        if (!readExact(sock, response, 2, 5000))
        {
            DBG("DbServerClient: port discovery read failed (expected 2 bytes)");
            return 0;
        }

        int port = (int(response[0]) << 8) | response[1];
        DBG("DbServerClient: discovered db port " + juce::String(port) + " on " + playerIP);

        if (port <= 0 || port > 65535)
        {
            DBG("DbServerClient: invalid port " + juce::String(port));
            return 0;
        }

        return port;
    }

    /// Setup query context (type 0x0000, TxID 0xFFFFFFFE)
    bool setupQueryContext(PlayerConnection& conn, uint8_t ourPlayer)
    {
        if (!conn.isConnected()) return false;

        DBG("DbServerClient: setting up query context on " + conn.playerIP
            + " as player " + juce::String(ourPlayer));

        auto msg = buildMessage(0xFFFFFFFE, 0x0000, { uint32_t(ourPlayer) });
        if (conn.socket->write(msg.getData(), (int)msg.getSize()) != (int)msg.getSize())
        {
            DBG("DbServerClient: query context write failed");
            return false;
        }

        auto resp = readMessage(*conn.socket, kReadTimeoutMs);
        if (!resp.ok || resp.type != 0x4000)
        {
            DBG("DbServerClient: query context setup REJECTED (type=0x"
                + juce::String::toHexString(resp.type)
                + ", ok=" + juce::String(resp.ok ? "true" : "false")
                + ") -- CDJ may not recognize our player number");
            return false;
        }

        DBG("DbServerClient: query context established, CDJ reports player="
            + juce::String(resp.numArgs[1]));
        conn.contextSetUp = true;
        conn.txId = 1;
        return true;
    }

    //==========================================================================
    // METADATA QUERY
    //==========================================================================

    TrackMetadata queryTrackMetadata(PlayerConnection& conn, uint8_t slot,
                                     uint8_t trackType, uint32_t trackId,
                                     uint8_t ourPlayer)
    {
        TrackMetadata meta;
        meta.trackId = trackId;

        if (!conn.isConnected() || !conn.contextSetUp) return meta;

        // Step 1: Request metadata (type 0x2002 for rekordbox, 0x2202 for non-rb)
        uint16_t reqType = (trackType == 1) ? uint16_t(0x2002) : uint16_t(0x2202);
        uint32_t dmst = makeDMST(ourPlayer, 0x01, slot, trackType);
        uint32_t currentTxId = conn.txId++;

        auto reqMsg = buildMessage(currentTxId, reqType, { dmst, trackId });
        if (conn.socket->write(reqMsg.getData(), (int)reqMsg.getSize()) != (int)reqMsg.getSize())
            return meta;

        // Read response -- should be type 0x4000 with item count
        auto resp = readMessage(*conn.socket, kReadTimeoutMs);
        if (!resp.ok || resp.type != 0x4000)
        {
            DBG("DbServerClient: metadata request failed for trackId=" + juce::String(trackId)
                + " (resp type=0x" + juce::String::toHexString(resp.type) + ")");
            return meta;
        }

        int itemCount = (int)resp.numArgs[1];
        if (itemCount <= 0 || resp.numArgs[1] == 0xFFFFFFFF)
            return meta;  // track not found

        // Step 2: Render menu (type 0x3000) to get actual data
        uint32_t renderTxId = conn.txId++;
        auto renderMsg = buildMessage(renderTxId, 0x3000, {
            dmst,
            0,                        // offset
            (uint32_t)itemCount,      // limit
            0,                        // unknown (docs: "sending 0 works")
            (uint32_t)itemCount,      // total
            0                         // unknown
        });

        if (conn.socket->write(renderMsg.getData(), (int)renderMsg.getSize())
            != (int)renderMsg.getSize())
            return meta;

        // Read header (type 0x4001)
        auto header = readMessage(*conn.socket, kReadTimeoutMs);
        if (!header.ok || header.type != 0x4001)
            return meta;

        // Read menu items (type 0x4101 each)
        for (int i = 0; i < itemCount; i++)
        {
            auto item = readMessage(*conn.socket, kReadTimeoutMs);
            if (!item.ok) break;
            if (item.type != 0x4101) break;

            // item.argCount should be 12
            // Arg indices:  0=parentId  1=mainId  2=label1Len  3=label1(str)
            //               4=label2Len  5=label2(str)  6=itemType
            //               7=flags  8=artworkId  9=playlistPos  10=unk  11=unk
            uint16_t itemType = uint16_t(item.numArgs[6] & 0xFFFF);  // mask for CDJ-3000

            switch (itemType)
            {
                case 0x0004:  // Track Title
                    meta.title = item.strArgs[3];
                    meta.artworkId = item.numArgs[8];
                    break;
                case 0x0007:  // Artist
                    meta.artist = item.strArgs[3];
                    break;
                case 0x0002:  // Album
                    meta.album = item.strArgs[3];
                    break;
                case 0x000B:  // Duration
                    meta.durationSeconds = (int)item.numArgs[1];
                    break;
                case 0x000D:  // Tempo
                    meta.bpmTimes100 = (int)item.numArgs[1];
                    break;
                case 0x0023:  // Comment
                    meta.comment = item.strArgs[3];
                    break;
                case 0x000F:  // Key
                    meta.key = item.strArgs[3];
                    break;
                case 0x000A:  // Rating
                    meta.rating = (int)item.numArgs[1];
                    break;
                case 0x0006:  // Genre
                    meta.genre = item.strArgs[3];
                    break;
                case 0x002E:  // Date Added
                    meta.dateAdded = item.strArgs[3];
                    break;
                case 0x000E:  // Analysis path (ANLZ file)
                    meta.anlzPath = item.strArgs[3];
#if JUCE_DEBUG
                    if (meta.anlzPath.isNotEmpty())
                        DBG("DbServerClient: anlzPath=" + meta.anlzPath);
#endif
                    break;
                default:
#if JUCE_DEBUG
                    // Log unknown types to discover new fields
                    if (itemType != 0 && item.strArgs[3].isNotEmpty())
                        DBG("DbServerClient: render item type=0x"
                            + juce::String::toHexString(itemType)
                            + " str=" + item.strArgs[3].substring(0, 60));
#endif
                    break;
            }
        }

        // Read footer (type 0x4201)
        auto footer = readMessage(*conn.socket, kReadTimeoutMs);
        // Footer is optional to verify -- some CDJs may differ
        (void)footer;

        meta.cacheTime = juce::Time::getMillisecondCounterHiRes();
        return meta;
    }

    //==========================================================================
    // ARTWORK QUERY
    //==========================================================================

    juce::Image queryArtwork(PlayerConnection& conn, uint8_t slot,
                              uint8_t trackType, uint32_t artworkId,
                              uint8_t ourPlayer)
    {
        if (!conn.isConnected() || !conn.contextSetUp || artworkId == 0)
            return {};

        DBG("DbServerClient: requesting artwork id=" + juce::String(artworkId));

        uint32_t dmst = makeDMST(ourPlayer, 0x08, slot, trackType);  // menu loc 8 for art
        uint32_t currentTxId = conn.txId++;

        auto reqMsg = buildMessage(currentTxId, 0x2003, { dmst, artworkId });
        if (conn.socket->write(reqMsg.getData(), (int)reqMsg.getSize()) != (int)reqMsg.getSize())
        {
            DBG("DbServerClient: artwork request write failed");
            return {};
        }

        // Response: type 0x4002 with blob containing JPEG data
        auto resp = readMessage(*conn.socket, kReadTimeoutMs);
        if (!resp.ok)
        {
            DBG("DbServerClient: artwork response read failed");
            return {};
        }
        if (resp.type != 0x4002)
        {
            DBG("DbServerClient: unexpected artwork response type=0x"
                + juce::String::toHexString(resp.type)
                + " (expected 0x4002)");
            return {};
        }

        // Scan all blob arguments for valid JPEG data
        for (int i = 0; i < (int)resp.argCount && i < 12; i++)
        {
            if (resp.blobArgs[i].getSize() > 100)  // JPEG must be at least ~100 bytes
            {
                auto img = juce::ImageFileFormat::loadFrom(
                    resp.blobArgs[i].getData(),
                    resp.blobArgs[i].getSize());

                if (img.isValid())
                {
                    DBG("DbServerClient: decoded artwork " + juce::String(artworkId)
                        + " -- " + juce::String(img.getWidth()) + "x"
                        + juce::String(img.getHeight()) + " from arg " + juce::String(i));
                    return img;
                }
            }
        }

        DBG("DbServerClient: artwork response had no valid JPEG (args="
            + juce::String(resp.argCount) + ")");
        return {};
    }

    //==========================================================================
    // COLOR PREVIEW WAVEFORM QUERY (NXS2+ CDJ-3000)
    //==========================================================================

    /// Waveform format indicator
    enum class WaveformFormat { None, ThreeBand, ColorNxs2 };

    /// Query preview waveform from CDJ.
    /// Strategy: try CDJ-3000 3-band (PWV6+2EX) first, then NXS2 color (PWV4+EXT).
    struct WaveformResult
    {
        std::vector<uint8_t> data;
        int entryCount = 0;
        WaveformFormat format = WaveformFormat::None;
    };

    /// Returns true if the model string indicates a CDJ-3000 (or newer) player
    /// that uses PWV6/PWV7 3-band waveforms from .2EX files.
    static bool isThreeBandPlayer(const juce::String& model)
    {
        // CDJ-3000, CDJ-3000NXS2, or any future model with "3000" in the name
        return model.contains("3000");
    }

    WaveformResult queryPreviewWaveform(
        PlayerConnection& conn, uint8_t slot, uint8_t trackType,
        uint32_t trackId, uint8_t ourPlayer, const juce::String& playerModel)
    {
        if (!conn.isConnected() || !conn.contextSetUp || trackId == 0)
            return {};

        bool threeBandFirst = isThreeBandPlayer(playerModel);

        // Try the expected format first based on model
        auto result = queryOneWaveformFormat(conn, slot, trackType, trackId,
                                              ourPlayer, threeBandFirst);
        if (result.entryCount > 0)
            return result;

        // Fallback: try the other format (covers unknown models, CDJ-3000X, etc.)
        DBG("DbServerClient: primary waveform format failed, trying fallback");
        return queryOneWaveformFormat(conn, slot, trackType, trackId,
                                       ourPlayer, !threeBandFirst);
    }

    /// Issue a single waveform ext query for one format.
    WaveformResult queryOneWaveformFormat(
        PlayerConnection& conn, uint8_t slot, uint8_t trackType,
        uint32_t trackId, uint8_t ourPlayer, bool threeBand)
    {
        uint32_t dmst = makeDMST(ourPlayer, 0x01, slot, trackType);
        uint32_t txId = conn.txId++;

        uint32_t tagMagic = threeBand ? kMagic6VWP : kMagic4VWP;
        uint32_t extMagic = threeBand ? kMagicXE2  : kMagicTXE;
        const char* tagName = threeBand ? "PWV6" : "PWV4";
        int expectedWordSize = threeBand ? 3 : 6;

        DBG("DbServerClient: requesting " + juce::String(tagName)
            + " waveform for track " + juce::String(trackId));

        auto reqMsg = buildMessage(txId, kNxs2ExtRequest,
                                    { dmst, trackId, tagMagic, extMagic });
        if (conn.socket->write(reqMsg.getData(), (int)reqMsg.getSize()) != (int)reqMsg.getSize())
        {
            DBG("DbServerClient: waveform request write failed");
            return {};
        }

        auto resp = readMessage(*conn.socket, kReadTimeoutMs);
        if (!resp.ok || resp.type == 0x4003)
            return {};
        if (resp.argCount < 3 || resp.numArgs[2] == 0)
            return {};

        auto blob = extractBlob(resp);
        if (!blob)
            return {};

        auto result = parseAnlzPreview(*blob, tagName, expectedWordSize);
        if (result.entryCount > 0)
        {
            result.format = threeBand ? WaveformFormat::ThreeBand : WaveformFormat::ColorNxs2;
            DBG("DbServerClient: got " + juce::String(tagName) + " waveform -- "
                + juce::String(result.entryCount) + " entries");
        }
        return result;
    }

    /// Extract the first substantial blob from a response message.
    /// Blob is typically in arg[3] (index 3) per the protocol spec.
    static const juce::MemoryBlock* extractBlob(const ResponseMessage& resp)
    {
        // Prefer arg[3] (per protocol spec)
        if (resp.argCount >= 4 && resp.blobArgs[3].getSize() > 32)
            return &resp.blobArgs[3];
        // Fallback scan
        for (int i = 0; i < (int)resp.argCount && i < 12; i++)
            if (resp.blobArgs[i].getSize() > 32)
                return &resp.blobArgs[i];
        return nullptr;
    }

    /// Parse ANLZ preview waveform from blob data.
    /// tagName: "PWV6" (3-band) or "PWV4" (NXS2 color)
    /// expectedWordSize: 3 for PWV6, 6 for PWV4
    static WaveformResult parseAnlzPreview(const juce::MemoryBlock& blob,
                                            const char* tagName, int expectedWordSize)
    {
        const uint8_t* d = static_cast<const uint8_t*>(blob.getData());
        int size = (int)blob.getSize();

        if (size < 20)
        {
            DBG("DbServerClient: ANLZ blob too small (" + juce::String(size) + " bytes)");
            return {};
        }

        // Search for tag signature (e.g. "PWV6" or "PWV4")
        for (int i = 0; i <= size - 20; ++i)
        {
            if (d[i] == tagName[0] && d[i+1] == tagName[1]
                && d[i+2] == tagName[2] && d[i+3] == tagName[3])
            {
                // Tag header: tag(4) + len_header(4) + len_tag(4)
                int hdrOff = i + 12;
                if (hdrOff + 8 > size) continue;

                uint32_t wordSize   = readBE32(d + hdrOff);
                uint32_t entryCount = readBE32(d + hdrOff + 4);

                if ((int)wordSize != expectedWordSize) continue;

                // PWV6 has 2 bytes of header after entryCount (len_header=14, so entries at +14)
                // PWV4 has 4 bytes of unknown after entryCount (word_size+count+unknown = 12)
                int entriesOff = i + readBE32(d + i + 4);  // use len_header to find data start
                // Fallback: if len_header seems wrong, try hdrOff + 8 (skip wordSize+entryCount)
                // then for PWV4 skip 4 more unknown bytes
                if (entriesOff <= i || entriesOff >= size)
                {
                    entriesOff = hdrOff + 8;
                    if (expectedWordSize == 6) entriesOff += 4;  // PWV4 has extra unknown field
                }

                int dataLen = (int)(wordSize * entryCount);
                if (entriesOff + dataLen > size)
                {
                    DBG("DbServerClient: " + juce::String(tagName) + " data overflows blob (need "
                        + juce::String(entriesOff + dataLen) + ", have " + juce::String(size) + ")");
                    continue;
                }

                WaveformResult result;
                result.data.assign(d + entriesOff, d + entriesOff + dataLen);
                result.entryCount = (int)entryCount;
                DBG("DbServerClient: parsed " + juce::String(tagName) + " -- "
                    + juce::String(entryCount) + " entries x " + juce::String(wordSize)
                    + " bytes = " + juce::String(dataLen) + " bytes");
                return result;
            }
        }

        // Fallback: try treating blob as raw data
        if (size >= expectedWordSize * 10 && size % expectedWordSize == 0)
        {
            int entryCount = size / expectedWordSize;
            WaveformResult result;
            result.data.assign(d, d + size);
            result.entryCount = entryCount;
            DBG("DbServerClient: using raw blob as " + juce::String(tagName)
                + " -- " + juce::String(entryCount) + " entries");
            return result;
        }

        DBG("DbServerClient: " + juce::String(tagName) + " tag not found in "
            + juce::String(size) + " byte blob");
        return {};
    }
    static uint32_t readBE32(const uint8_t* p)
    {
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16)
             | (uint32_t(p[2]) << 8)  | p[3];
    }
    static uint16_t readBE16(const uint8_t* p)
    {
        return (uint16_t(p[0]) << 8) | p[1];
    }

    /// Hex dump for debug logging (first N bytes).
#if JUCE_DEBUG
    static juce::String hexDump(const void* data, int size, int maxBytes = 64)
    {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        int n = juce::jmin(size, maxBytes);
        juce::String s;
        for (int i = 0; i < n; i++)
        {
            if (i > 0 && i % 16 == 0) s += "\n  ";
            else if (i > 0) s += " ";
            s += juce::String::toHexString(p[i]).paddedLeft('0', 2);
        }
        if (n < size) s += " ...(" + juce::String(size - n) + " more)";
        return s;
    }
#endif

    //==========================================================================
    // BEAT GRID QUERY (PQTZ tag from .EXT via 0x2c04)
    //==========================================================================

    /// Query beat grid from CDJ. Returns vector of BeatEntry.
    std::vector<TrackMetadata::BeatEntry> queryBeatGrid(
        PlayerConnection& conn, uint8_t slot, uint8_t trackType,
        uint32_t trackId, uint8_t ourPlayer)
    {
        if (!conn.isConnected() || !conn.contextSetUp || trackId == 0)
            return {};

        DBG("DbServerClient: requesting beat grid for track " + juce::String(trackId));

        uint32_t dmst = makeDMST(ourPlayer, 0x01, slot, trackType);
        uint32_t txId = conn.txId++;
        auto reqMsg = buildMessage(txId, kNxs2ExtRequest,
                                    { dmst, trackId, kMagicPQTZ, kMagicTXE });
        if (conn.socket->write(reqMsg.getData(), (int)reqMsg.getSize()) != (int)reqMsg.getSize())
            return {};

        auto resp = readMessage(*conn.socket, kReadTimeoutMs);
        if (!resp.ok || resp.type == 0x4003)
        {
            DBG("DbServerClient: beat grid response failed -- ok=" + juce::String((int)resp.ok)
                + " type=0x" + juce::String::toHexString((int)resp.type)
                + " args=" + juce::String(resp.argCount));
            return {};
        }
        if (resp.argCount < 3 || resp.numArgs[2] == 0)
        {
            DBG("DbServerClient: beat grid response empty -- args=" + juce::String(resp.argCount)
                + " numArgs[2]=" + juce::String(resp.argCount >= 3 ? (int)resp.numArgs[2] : -1));
            return {};
        }

        auto blob = extractBlob(resp);
        if (!blob)
        {
            DBG("DbServerClient: beat grid extractBlob failed");
            return {};
        }

        return parseBeatGrid(*blob);
    }

    /// Parse PQTZ tag from ANLZ blob.
    /// Format: "PQTZ" + len_header(4be) + len_tag(4be) + unk(4) + unk(4) + num_beats(4be)
    /// Entries at offset len_header, 8 bytes each: beat(2be) + tempo(2be) + time(4be)
    static std::vector<TrackMetadata::BeatEntry> parseBeatGrid(const juce::MemoryBlock& blob)
    {
        const uint8_t* d = static_cast<const uint8_t*>(blob.getData());
        int size = (int)blob.getSize();

        for (int i = 0; i <= size - 24; ++i)
        {
            if (d[i] == 'P' && d[i+1] == 'Q' && d[i+2] == 'T' && d[i+3] == 'Z')
            {
                uint32_t lenHeader = readBE32(d + i + 4);
                if (lenHeader < 24 || (int)(i + lenHeader) > size) continue;

                uint32_t numBeats = readBE32(d + i + 20);
                if (numBeats == 0 || numBeats > 100000) continue;

                int entriesOff = (int)(i + lenHeader);
                int dataNeeded = (int)(numBeats * 8);
                if (entriesOff + dataNeeded > size)
                {
                    DBG("DbServerClient: PQTZ data overflows blob");
                    continue;
                }

                std::vector<TrackMetadata::BeatEntry> grid;
                grid.reserve(numBeats);
                for (uint32_t b = 0; b < numBeats; ++b)
                {
                    const uint8_t* e = d + entriesOff + b * 8;
                    TrackMetadata::BeatEntry entry;
                    entry.beatNumber  = readBE16(e);
                    entry.bpmTimes100 = readBE16(e + 2);
                    entry.timeMs      = readBE32(e + 4);
                    grid.push_back(entry);
                }

                DBG("DbServerClient: parsed PQTZ -- " + juce::String(numBeats) + " beats");
                return grid;
            }
        }

        DBG("DbServerClient: PQTZ tag not found in " + juce::String(size) + " byte blob");
        return {};
    }

    //==========================================================================
    // DETAIL WAVEFORM QUERY (PWV5 from .EXT or PWV7 from .2EX via 0x2c04)
    //==========================================================================

    struct DetailWaveformResult
    {
        std::vector<uint8_t> data;
        int entryCount = 0;
        int bytesPerEntry = 0;   // 2 = PWV5 (NXS2), 3 = PWV7 (CDJ-3000)
    };

    /// Query detail waveform. Tries CDJ-3000 3-band (PWV7) first if model
    /// indicates CDJ-3000, then falls back to NXS2 color (PWV5).
    DetailWaveformResult queryDetailWaveform(
        PlayerConnection& conn, uint8_t slot, uint8_t trackType,
        uint32_t trackId, uint8_t ourPlayer, const juce::String& playerModel)
    {
        if (!conn.isConnected() || !conn.contextSetUp || trackId == 0)
            return {};

        bool threeBandFirst = isThreeBandPlayer(playerModel);

        auto result = queryOneDetailFormat(conn, slot, trackType, trackId,
                                            ourPlayer, threeBandFirst);
        if (result.entryCount > 0) return result;

        // Fallback to other format
        DBG("DbServerClient: primary detail format failed, trying fallback");
        return queryOneDetailFormat(conn, slot, trackType, trackId,
                                     ourPlayer, !threeBandFirst);
    }

    DetailWaveformResult queryOneDetailFormat(
        PlayerConnection& conn, uint8_t slot, uint8_t trackType,
        uint32_t trackId, uint8_t ourPlayer, bool threeBand)
    {
        uint32_t dmst = makeDMST(ourPlayer, 0x01, slot, trackType);
        uint32_t txId = conn.txId++;

        uint32_t tagMagic = threeBand ? kMagic7VWP : kMagic5VWP;
        uint32_t extMagic = threeBand ? kMagicXE2  : kMagicTXE;
        const char* tagName = threeBand ? "PWV7" : "PWV5";
        int expectedWordSize = threeBand ? 3 : 2;

        DBG("DbServerClient: requesting " + juce::String(tagName)
            + " detail waveform for track " + juce::String(trackId));

        auto reqMsg = buildMessage(txId, kNxs2ExtRequest,
                                    { dmst, trackId, tagMagic, extMagic });
        if (conn.socket->write(reqMsg.getData(), (int)reqMsg.getSize()) != (int)reqMsg.getSize())
            return {};

        auto resp = readMessage(*conn.socket, kReadTimeoutMs);
        if (!resp.ok || resp.type == 0x4003) return {};
        if (resp.argCount < 3 || resp.numArgs[2] == 0) return {};

        auto blob = extractBlob(resp);
        if (!blob) return {};

        return parseDetailWaveform(*blob, tagName, expectedWordSize);
    }

    /// Parse PWV5 or PWV7 detail waveform from ANLZ blob.
    /// Same structure as preview tags: tag(4) + len_header(4be) + len_tag(4be)
    /// then wordSize(4be) + entryCount(4be) at offset 12, data at len_header.
    static DetailWaveformResult parseDetailWaveform(
        const juce::MemoryBlock& blob, const char* tagName, int expectedWordSize)
    {
        const uint8_t* d = static_cast<const uint8_t*>(blob.getData());
        int size = (int)blob.getSize();

        for (int i = 0; i <= size - 20; ++i)
        {
            if (d[i] == tagName[0] && d[i+1] == tagName[1]
                && d[i+2] == tagName[2] && d[i+3] == tagName[3])
            {
                uint32_t lenHeader = readBE32(d + i + 4);
                int hdrOff = i + 12;
                if (hdrOff + 8 > size) continue;

                uint32_t wordSize   = readBE32(d + hdrOff);
                uint32_t entryCount = readBE32(d + hdrOff + 4);

                if ((int)wordSize != expectedWordSize) continue;
                if (entryCount == 0 || entryCount > 5000000) continue;

                int entriesOff = (int)(i + lenHeader);
                if (entriesOff <= i || entriesOff >= size)
                    entriesOff = hdrOff + 8;

                int dataLen = (int)(wordSize * entryCount);
                if (entriesOff + dataLen > size)
                {
                    DBG("DbServerClient: " + juce::String(tagName)
                        + " detail data overflows blob");
                    continue;
                }

                DetailWaveformResult result;
                result.data.assign(d + entriesOff, d + entriesOff + dataLen);
                result.entryCount = (int)entryCount;
                result.bytesPerEntry = (int)wordSize;
                DBG("DbServerClient: parsed " + juce::String(tagName) + " detail -- "
                    + juce::String(entryCount) + " entries x " + juce::String(wordSize)
                    + " bytes (" + juce::String(entryCount / 150) + "s of audio)");
                return result;
            }
        }

        DBG("DbServerClient: " + juce::String(tagName) + " detail tag not found in "
            + juce::String(size) + " byte blob");
        return {};
    }

    //==========================================================================
    // SONG STRUCTURE QUERY (PSSI tag from .EXT via 0x2c04)
    //==========================================================================

    struct SongStructureResult
    {
        uint16_t mood = 0;
        std::vector<TrackMetadata::PhraseEntry> phrases;
    };

    /// Query song structure (phrase analysis) from CDJ.
    SongStructureResult querySongStructure(
        PlayerConnection& conn, uint8_t slot, uint8_t trackType,
        uint32_t trackId, uint8_t ourPlayer)
    {
        if (!conn.isConnected() || !conn.contextSetUp || trackId == 0)
            return {};

        DBG("DbServerClient: requesting song structure for track " + juce::String(trackId));

        uint32_t dmst = makeDMST(ourPlayer, 0x01, slot, trackType);
        uint32_t txId = conn.txId++;
        auto reqMsg = buildMessage(txId, kNxs2ExtRequest,
                                    { dmst, trackId, kMagicPSSI, kMagicTXE });
        if (conn.socket->write(reqMsg.getData(), (int)reqMsg.getSize()) != (int)reqMsg.getSize())
            return {};

        auto resp = readMessage(*conn.socket, kReadTimeoutMs);
        if (!resp.ok || resp.type == 0x4003)
        {
            DBG("DbServerClient: song structure response failed -- ok=" + juce::String((int)resp.ok)
                + " type=0x" + juce::String::toHexString((int)resp.type));
            return {};
        }
        if (resp.argCount < 3 || resp.numArgs[2] == 0)
        {
            DBG("DbServerClient: song structure response empty -- args=" + juce::String(resp.argCount)
                + " numArgs[2]=" + juce::String(resp.argCount >= 3 ? (int)resp.numArgs[2] : -1));
            return {};
        }

        auto blob = extractBlob(resp);
        if (!blob)
        {
            DBG("DbServerClient: song structure extractBlob failed");
            return {};
        }

        DBG("DbServerClient: song structure blob=" + juce::String((int)blob->getSize()) + " bytes");

        return parseSongStructure(*blob);
    }

    /// Parse PSSI tag from ANLZ blob (with XOR unmasking).
    /// Blob from dbserver has 4-byte LE length wrapper before "PSSI".
    /// Verified structure from CDJ-3000 hex dumps + rekordbox_anlz.ksy:
    ///   [+0]  "PSSI" magic
    ///   [+4]  lenHeader (u4be, typically 32)
    ///   [+8]  lenTag (u4be)
    ///   [+12] lenEntryBytes (u4be, =24)
    ///   [+16] numEntries (u2be)
    ///   [+18] masked body: mood(u2) + pad(6) + endBeat(u2) + pad(2) + bank(u1) + pad(1) + entries[24 each]
    ///
    /// The body from offset +18 is XOR masked when raw_mood > 20.
    /// Mask key: 19-byte sequence derived from numEntries (see rekordbox_anlz.ksy).
    static SongStructureResult parseSongStructure(const juce::MemoryBlock& blob)
    {
        const uint8_t* d = static_cast<const uint8_t*>(blob.getData());
        int size = (int)blob.getSize();

        for (int i = 0; i <= size - 20; ++i)
        {
            if (d[i] != 'P' || d[i+1] != 'S' || d[i+2] != 'S' || d[i+3] != 'I')
                continue;

            uint32_t lenHeader = readBE32(d + i + 4);
            if (lenHeader < 20 || i + 12 + 6 > size) continue;

            uint32_t entrySize  = readBE32(d + i + 12);  // u4, NOT u2
            uint16_t numEntries = readBE16(d + i + 16);
            if (entrySize != 24 || numEntries == 0 || numEntries > 1000) continue;

            // Masked body starts at i + 18
            int bodyOff = i + 18;
            int bodyLen = size - bodyOff;
            if (bodyLen < 2) continue;

            // Check if masked: raw_mood (first u2 of body)
            uint16_t rawMood = readBE16(d + bodyOff);
            bool isMasked = (rawMood > 20);

            // Unmask the body
            std::vector<uint8_t> body(d + bodyOff, d + bodyOff + bodyLen);
            if (isMasked)
            {
                uint8_t c = (uint8_t)(numEntries & 0xFF);
                const uint8_t maskBase[19] = {
                    0xCB, 0xE1, 0xEE, 0xFA, 0xE5, 0xEE, 0xAD, 0xEE,
                    0xE9, 0xD2, 0xE9, 0xEB, 0xE1, 0xE9, 0xF3, 0xE8,
                    0xE9, 0xF4, 0xE1
                };
                for (int j = 0; j < (int)body.size(); j++)
                    body[(size_t)j] ^= (uint8_t)((maskBase[j % 19] + c) & 0xFF);
            }

            // Parse unmasked body:
            //   [0-1] mood, [2-7] pad, [8-9] endBeat, [10-11] pad, [12] bank, [13] pad
            //   [14+] entries (numEntries * 24 each)
            if ((int)body.size() < 14) continue;

            uint16_t mood = readBE16(body.data());

            SongStructureResult result;
            result.mood = mood;
            result.phrases.reserve(numEntries);

            int entriesOff = 14;
            for (uint16_t p = 0; p < numEntries; ++p)
            {
                int eOff = entriesOff + p * 24;
                if (eOff + 24 > (int)body.size()) break;

                const uint8_t* e = body.data() + eOff;
                TrackMetadata::PhraseEntry phrase;
                phrase.index      = readBE16(e);
                phrase.beatNumber = readBE16(e + 2);
                phrase.kind       = readBE16(e + 4);
                phrase.fill       = e[21];
                phrase.beatFill   = readBE16(e + 22);

                // Beat count: difference to next phrase
                if (p + 1 < numEntries && eOff + 24 + 4 <= (int)body.size())
                    phrase.beatCount = readBE16(body.data() + eOff + 24 + 2) - phrase.beatNumber;
                else
                    phrase.beatCount = 0;

                result.phrases.push_back(phrase);
            }

            DBG("DbServerClient: parsed PSSI -- " + juce::String(numEntries)
                + " phrases, mood=" + juce::String(mood)
                + (isMasked ? " (unmasked)" : " (plain)"));
            return result;
        }

        DBG("DbServerClient: PSSI tag not found in " + juce::String(size) + " byte blob");
        return {};
    }

    //==========================================================================
    // CUE LIST QUERY (PCO2 extended or PCOB standard from .EXT via 0x2c04)
    //==========================================================================

    /// Query rekordbox cue list (hot cues, memory points, loops with colors).
    /// Tries extended nxs2 format (PCO2) first, falls back to standard (PCOB).
    std::vector<TrackMetadata::RekordboxCue> queryCueList(
        PlayerConnection& conn, uint8_t slot, uint8_t trackType,
        uint32_t trackId, uint8_t ourPlayer)
    {
        if (!conn.isConnected() || !conn.contextSetUp || trackId == 0)
            return {};

        DBG("DbServerClient: requesting cue list for track " + juce::String(trackId));

        // Try extended (PCO2) first -- has colors and comments
        {
            uint32_t dmst = makeDMST(ourPlayer, 0x01, slot, trackType);
            uint32_t txId = conn.txId++;
            auto reqMsg = buildMessage(txId, kNxs2ExtRequest,
                                        { dmst, trackId, kMagicPCO2, kMagicTXE });
            if (conn.socket->write(reqMsg.getData(), (int)reqMsg.getSize()) == (int)reqMsg.getSize())
            {
                auto resp = readMessage(*conn.socket, kReadTimeoutMs);
                if (resp.ok && resp.type != 0x4003 && resp.argCount >= 3 && resp.numArgs[2] != 0)
                {
                    auto blob = extractBlob(resp);
                    if (blob)
                    {
                        DBG("DbServerClient: PCO2 blob=" + juce::String((int)blob->getSize()) + " bytes");
                        auto cues = parseCueListExtended(*blob);
                        if (!cues.empty())
                        {
                            DBG("DbServerClient: got PCO2 extended cue list");
                            return cues;
                        }
                        DBG("DbServerClient: PCO2 parse returned 0 cues");
                    }
                    else
                        DBG("DbServerClient: PCO2 extractBlob failed");
                }
                else
                    DBG("DbServerClient: PCO2 response -- ok=" + juce::String((int)resp.ok)
                        + " type=0x" + juce::String::toHexString((int)resp.type)
                        + " args=" + juce::String(resp.argCount)
                        + " numArgs[2]=" + juce::String(resp.argCount >= 3 ? (int)resp.numArgs[2] : -1));
            }
        }

        // Fallback: standard (PCOB) -- no colors/comments but has positions
        if (!conn.isConnected()) return {};
        {
            uint32_t dmst = makeDMST(ourPlayer, 0x01, slot, trackType);
            uint32_t txId = conn.txId++;
            auto reqMsg = buildMessage(txId, kNxs2ExtRequest,
                                        { dmst, trackId, kMagicPCOB, kMagicTXE });
            if (conn.socket->write(reqMsg.getData(), (int)reqMsg.getSize()) == (int)reqMsg.getSize())
            {
                auto resp = readMessage(*conn.socket, kReadTimeoutMs);
                if (resp.ok && resp.type != 0x4003 && resp.argCount >= 3 && resp.numArgs[2] != 0)
                {
                    auto blob = extractBlob(resp);
                    if (blob)
                    {
                        DBG("DbServerClient: PCOB blob=" + juce::String((int)blob->getSize()) + " bytes");
                        auto cues = parseCueListStandard(*blob);
                        if (!cues.empty())
                        {
                            DBG("DbServerClient: got PCOB standard cue list");
                            return cues;
                        }
                        DBG("DbServerClient: PCOB parse returned 0 cues");
                    }
                    else
                        DBG("DbServerClient: PCOB extractBlob failed");
                }
                else
                    DBG("DbServerClient: PCOB response -- ok=" + juce::String((int)resp.ok)
                        + " type=0x" + juce::String::toHexString((int)resp.type)
                        + " args=" + juce::String(resp.argCount)
                        + " numArgs[2]=" + juce::String(resp.argCount >= 3 ? (int)resp.numArgs[2] : -1));
            }
        }

        DBG("DbServerClient: cue list query returned no data");
        return {};
    }

    /// Parse extended cue list (PCO2 tag with PCP2 entries).
    /// Offsets verified against CDJ-3000 Wireshark captures + rekordbox_anlz.ksy:
    ///   PCP2 entry (offsets from "PCP2" magic):
    ///     [0x0C] hot_cue (u4be, 0=memory, 1=A, 2=B...)
    ///     [0x10] type (u1, 1=cue, 2=loop)
    ///     [0x14] time (u4be, ms)
    ///     [0x18] loop_time (u4be, ms, 0xFFFFFFFF if not loop)
    ///     [0x1C] color_id (u1)
    ///     [0x24] loop_numerator (u2), [0x26] loop_denominator (u2)
    ///     [0x28] len_comment (u4, byte count of UTF-16BE string)
    ///     [0x2C] comment (UTF-16BE, len_comment bytes)
    ///     [0x2C + len_comment] color_code (u1)
    ///     [0x2C + len_comment + 1] color_r, +2 color_g, +3 color_b
    static std::vector<TrackMetadata::RekordboxCue> parseCueListExtended(const juce::MemoryBlock& blob)
    {
        const uint8_t* d = static_cast<const uint8_t*>(blob.getData());
        int size = (int)blob.getSize();
        std::vector<TrackMetadata::RekordboxCue> result;

        // Search for all PCO2 sections (one for memory points, one for hot cues)
        for (int i = 0; i <= size - 20; ++i)
        {
            if (d[i] != 'P' || d[i+1] != 'C' || d[i+2] != 'O' || d[i+3] != '2')
                continue;

            uint32_t lenHeader = readBE32(d + i + 4);
            // numCues at offset 16 from tag start (body[4-5] after 12-byte section header + 4-byte type)
            uint16_t numCues   = readBE16(d + i + 16);
            if (numCues == 0 || numCues > 200) continue;

            DBG("DbServerClient: PCO2 section found at " + juce::String(i)
                + " lenHeader=" + juce::String(lenHeader)
                + " numCues=" + juce::String(numCues));

            int entryOff = (int)(i + lenHeader);

            for (uint16_t c = 0; c < numCues; ++c)
            {
                // Find next PCP2 tag
                if (entryOff + 12 > size) break;
                if (d[entryOff] != 'P' || d[entryOff+1] != 'C'
                    || d[entryOff+2] != 'P' || d[entryOff+3] != '2')
                {
                    bool found = false;
                    for (int scan = entryOff; scan <= size - 12 && scan < entryOff + 200; ++scan)
                    {
                        if (d[scan] == 'P' && d[scan+1] == 'C' && d[scan+2] == 'P' && d[scan+3] == '2')
                            { entryOff = scan; found = true; break; }
                    }
                    if (!found) break;
                }

                uint32_t entryLen = readBE32(d + entryOff + 8);  // total PCP2 entry size
                if (entryLen > 4096 || entryLen < 0x1D
                    || entryOff + (int)entryLen > size) 
                    { entryOff += juce::jmax(12, (int)entryLen); continue; }

                const uint8_t* e = d + entryOff;  // points to "PCP2" magic
                TrackMetadata::RekordboxCue cue;

                uint32_t hotCue = readBE32(e + 0x0C);  // hot cue number
                uint8_t  ctype  = e[0x10];               // 1=cue, 2=loop
                uint32_t timeMs = readBE32(e + 0x14);    // position ms
                uint32_t loopMs = readBE32(e + 0x18);    // loop end ms

                if (ctype == 0) { entryOff += (int)entryLen; continue; }

                cue.hotCueNumber = (uint8_t)hotCue;
                cue.positionMs   = timeMs;

                if (ctype == 2 || (loopMs != 0 && loopMs != 0xFFFFFFFF))
                {
                    cue.type = TrackMetadata::RekordboxCue::Loop;
                    cue.loopEndMs = loopMs;
                }
                else if (hotCue > 0)
                    cue.type = TrackMetadata::RekordboxCue::HotCue;
                else
                    cue.type = TrackMetadata::RekordboxCue::MemoryPoint;

                // Color ID (fixed offset)
                if (entryLen >= 0x1D)
                    cue.colorCode = e[0x1C];

                // Comment (variable length)
                uint32_t commentBytes = 0;
                if (entryLen >= 0x2C)
                {
                    commentBytes = readBE32(e + 0x28);  // byte count of UTF-16BE string
                    if (commentBytes > 0 && commentBytes < 512
                        && 0x2C + (int)commentBytes <= (int)entryLen)
                    {
                        int numChars = (int)commentBytes / 2;
                        for (int ci = 0; ci < numChars; ++ci)
                        {
                            uint16_t ch = readBE16(e + 0x2C + ci * 2);
                            if (ch != 0)
                                cue.comment += juce::String::charToString((juce::juce_wchar)ch);
                        }
                    }
                }

                // Color RGB (after comment)
                int colorOff = 0x2C + (int)commentBytes;
                if (colorOff + 4 <= (int)entryLen)
                {
                    cue.colorCode = e[colorOff];
                    cue.colorR    = e[colorOff + 1];
                    cue.colorG    = e[colorOff + 2];
                    cue.colorB    = e[colorOff + 3];
                    cue.hasColor  = (cue.colorR != 0 || cue.colorG != 0 || cue.colorB != 0);
                }

                result.push_back(cue);
                entryOff += (int)entryLen;
            }
        }

        // Sort by position
        std::sort(result.begin(), result.end(),
                  [](const auto& a, const auto& b) { return a.positionMs < b.positionMs; });

#if JUCE_DEBUG
        if (!result.empty())
            DBG("DbServerClient: parsed PCO2 -- " + juce::String((int)result.size()) + " cues");
#endif
        return result;
    }

    /// Parse standard cue list (PCOB tag with PCPT entries).
    /// PCPT entry (0x38 bytes each):
    ///   [0x0C-0x0F] hot_cue (u4be)
    ///   [0x1C] type (u1, 1=cue, 2=loop)
    ///   [0x20-0x23] time (u4be, ms)
    ///   [0x24-0x27] loop_time (u4be, ms)
    static std::vector<TrackMetadata::RekordboxCue> parseCueListStandard(const juce::MemoryBlock& blob)
    {
        const uint8_t* d = static_cast<const uint8_t*>(blob.getData());
        int size = (int)blob.getSize();
        std::vector<TrackMetadata::RekordboxCue> result;

        for (int i = 0; i <= size - 20; ++i)
        {
            if (d[i] != 'P' || d[i+1] != 'C' || d[i+2] != 'O' || d[i+3] != 'B')
                continue;

            uint32_t lenHeader = readBE32(d + i + 4);
            uint16_t numCues   = readBE16(d + i + 18);
            if (numCues == 0 || numCues > 200) continue;

            int entryOff = (int)(i + lenHeader);
            static constexpr int kPcptSize = 0x38;

            for (uint16_t c = 0; c < numCues; ++c)
            {
                if (entryOff + kPcptSize > size) break;
                if (d[entryOff] != 'P' || d[entryOff+1] != 'C'
                    || d[entryOff+2] != 'P' || d[entryOff+3] != 'T')
                    { entryOff += kPcptSize; continue; }

                const uint8_t* e = d + entryOff;
                uint32_t hotCue = readBE32(e + 0x0C);
                uint8_t  ctype  = e[0x1C];
                uint32_t timeMs = readBE32(e + 0x20);
                uint32_t loopMs = readBE32(e + 0x24);

                if (ctype == 0) { entryOff += kPcptSize; continue; }

                TrackMetadata::RekordboxCue cue;
                cue.hotCueNumber = (uint8_t)hotCue;
                cue.positionMs   = timeMs;

                if (ctype == 2 || loopMs > 0)
                {
                    cue.type = TrackMetadata::RekordboxCue::Loop;
                    cue.loopEndMs = loopMs;
                }
                else if (hotCue > 0)
                    cue.type = TrackMetadata::RekordboxCue::HotCue;
                else
                    cue.type = TrackMetadata::RekordboxCue::MemoryPoint;

                // No color in standard format -- use defaults
                // Hot cues default green, memory points red, loops orange
                if (cue.type == TrackMetadata::RekordboxCue::MemoryPoint)
                    { cue.colorR = 200; cue.colorG = 30; cue.colorB = 30; }
                else if (cue.type == TrackMetadata::RekordboxCue::Loop)
                    { cue.colorR = 255; cue.colorG = 136; cue.colorB = 0; }

                result.push_back(cue);
                entryOff += kPcptSize;
            }
        }

        std::sort(result.begin(), result.end(),
                  [](const auto& a, const auto& b) { return a.positionMs < b.positionMs; });
        return result;
    }

    //==========================================================================
    // CACHE MANAGEMENT
    //==========================================================================

    void cacheMetadata(const juce::String& playerIP, const TrackMetadata& meta)
    {
        if (!meta.isValid()) return;

        uint64_t key = makeCacheKey(playerIP, meta.trackId);
        const juce::SpinLock::ScopedLockType lock(cacheLock);

        // Evict oldest if cache is full
        if ((int)metadataCache.size() >= kMaxCacheEntries)
        {
            uint64_t oldestKey = 0;
            double oldestTime = std::numeric_limits<double>::max();
            for (auto& [k, m] : metadataCache)
            {
                if (m.cacheTime < oldestTime)
                {
                    oldestTime = m.cacheTime;
                    oldestKey = k;
                }
            }
            if (oldestKey != 0)
                metadataCache.erase(oldestKey);
        }

        metadataCache[key] = meta;
    }

    void cacheArtwork(uint32_t artworkId, const juce::Image& img)
    {
        if (artworkId == 0 || !img.isValid()) return;

        const juce::SpinLock::ScopedLockType lock(artCacheLock);

        if ((int)artworkCache.size() >= kMaxArtCacheEntries)
        {
            // Simple eviction: remove first entry
            artworkCache.erase(artworkCache.begin());
        }

        artworkCache[artworkId] = img;
    }

    //==========================================================================
    // BACKGROUND THREAD
    //==========================================================================

    void run() override
    {
        DBG("DbServerClient: background thread started");
        while (!threadShouldExit() && isRunningFlag.load(std::memory_order_relaxed))
        {
            // Wait for a request (with timeout for shutdown checks)
            requestSemaphore.wait(500);

            if (threadShouldExit()) break;

            // Process all queued requests
            while (true)
            {
                uint32_t rp = reqReadPos.load(std::memory_order_relaxed);
                uint32_t wp = reqWritePos.load(std::memory_order_acquire);
                if (rp == wp) break;  // queue empty

                auto req = requestQueue[rp & kRequestQueueMask];
                reqReadPos.store(rp + 1, std::memory_order_release);

                if (threadShouldExit()) break;

                processRequest(req);
            }
        }
    }

    void processRequest(const MetadataRequest& req)
    {
        queryCount.fetch_add(1, std::memory_order_relaxed);

        auto* conn = getConnection(req.playerIP, req.ourPlayer);
        if (!conn)
        {
            // Phase 2 can still run NFS (direct UDP, no dbserver connection needed)
            if (req.phase == 2 && req.trackId != 0)
            {
                DBG("DbServerClient: no connection for phase 2 -- trying NFS only");
                uint64_t cacheKey = makeCacheKey(req.playerIP, req.trackId);
                processNfsFallback(req, cacheKey);
                return;
            }
            errorCount.fetch_add(1, std::memory_order_relaxed);
            DBG("DbServerClient: failed to get connection to " + req.playerIP);
            return;
        }

        if (req.trackId != 0)
        {
            // Check if metadata text is already cached (partial cache --
            // metadata succeeded on a previous request but waveform or
            // artwork may still be missing).
            uint64_t cacheKey = makeCacheKey(req.playerIP, req.trackId);
            bool metaAlreadyCached = false;
            TrackMetadata meta;
            {
                const juce::SpinLock::ScopedLockType lock(cacheLock);
                auto it = metadataCache.find(cacheKey);
                if (it != metadataCache.end() && it->second.isValid())
                {
                    metaAlreadyCached = true;
                    meta = it->second;
                }
            }

            if (!metaAlreadyCached)
            {
                // First time: query metadata from CDJ dbserver
                meta = queryTrackMetadata(*conn, req.slot, req.trackType,
                                          req.trackId, req.ourPlayer);
                if (meta.isValid())
                {
                    cacheMetadata(req.playerIP, meta);
                    DBG("DbServerClient: cached metadata for track "
                        + juce::String(req.trackId) + " -- \""
                        + meta.artist + " - " + meta.title + "\""
                        + (meta.anlzPath.isNotEmpty() ? " anlz=" + meta.anlzPath : " (no anlzPath)"));
                }
                else
                {
                    errorCount.fetch_add(1, std::memory_order_relaxed);
                    DBG("DbServerClient: metadata query failed for trackId="
                        + juce::String(req.trackId));
                    if (!conn->isConnected())
                        conn->close();

                    // Create a minimal cache entry so phase 2 (disk cache + NFS)
                    // can still run.  NFS uses trackId to find ANLZ path in PDB.
                    meta.trackId = req.trackId;
                    cacheMetadata(req.playerIP, meta);
                }
            }

            // Artwork and waveform queries -- only if we have valid metadata
            // (skip when dbserver connection failed to avoid hanging on dead socket)
            if (meta.isValid())
            {
                // Artwork: fetch if not yet cached
                if (req.wantArt && meta.artworkId != 0)
                {
                    bool artCached;
                    {
                        const juce::SpinLock::ScopedLockType lock(artCacheLock);
                        artCached = artworkCache.count(meta.artworkId) > 0;
                    }
                    if (!artCached)
                    {
                        auto img = queryArtwork(*conn, req.slot, req.trackType,
                                                 meta.artworkId, req.ourPlayer);
                        if (img.isValid())
                            cacheArtwork(meta.artworkId, img);
                    }
                }

                // Waveform: fetch if not yet cached
                if (req.wantWaveform && !meta.hasWaveform())
                {
                    auto wfResult = queryPreviewWaveform(
                        *conn, req.slot, req.trackType, req.trackId,
                        req.ourPlayer, req.playerModel);
                    if (wfResult.entryCount > 0)
                    {
                        const juce::SpinLock::ScopedLockType lock(cacheLock);
                        auto it = metadataCache.find(cacheKey);
                        if (it != metadataCache.end())
                        {
                            it->second.waveformData = std::move(wfResult.data);
                            it->second.waveformEntryCount = wfResult.entryCount;
                            it->second.waveformBytesPerEntry =
                                (wfResult.format == WaveformFormat::ThreeBand) ? 3 : 6;
                            ++it->second.cacheVersion;
                        }
                    }
                }
            }

            // --- End of Phase 1 (critical) ---
            // Publish metadata+artwork+preview immediately so the display
            // shows track info while supplementary data loads.
            if (req.phase == 1 && req.wantWaveform)
            {
                // Mark waveform as attempted so the display picks up what we have
                {
                    const juce::SpinLock::ScopedLockType lock(cacheLock);
                    auto it = metadataCache.find(cacheKey);
                    if (it != metadataCache.end())
                    {
                        it->second.waveformQueried = true;
                        ++it->second.cacheVersion;
                    }
                }

                // Disk cache check (instant, ~1ms) -- load beats/cues/phrases/detail
                // from previous session BEFORE re-enqueueing phase 2.
                std::string diskKey;
                {
                    const juce::SpinLock::ScopedLockType lock(cacheLock);
                    auto it = metadataCache.find(cacheKey);
                    if (it != metadataCache.end()
                        && it->second.artist.isNotEmpty() && it->second.title.isNotEmpty())
                        diskKey = TrackMapEntry::makeKey(
                            it->second.artist, it->second.title, it->second.durationSeconds);
                }
                if (!diskKey.empty() && WaveformCache::anlzExists(diskKey))
                {
                    auto cached = WaveformCache::loadAnlz(diskKey);
                    if (cached.valid)
                    {
                        const juce::SpinLock::ScopedLockType lock(cacheLock);
                        auto it = metadataCache.find(cacheKey);
                        if (it != metadataCache.end())
                        {
                            applyCachedAnlz(it->second, cached);
                            ++it->second.cacheVersion;
                            DBG("DbServerClient: phase 1 loaded ANLZ from disk cache");
                        }
                    }
                }

                // Re-enqueue as phase 2 for NFS refresh + any missing dbserver data
                enqueueInternal(req.playerIP, req.playerModel, req.slot,
                                req.trackType, req.trackId, req.ourPlayer, 2);
                return;
            }

            // --- Phase 2: supplementary queries + NFS ---
            // This runs AFTER phase 1 has published. If a newer track request
            // arrived while we were waiting in queue, skip slow dbserver queries
            // and only do the essential NFS async refresh.

            // Re-read meta in case disk cache already filled it in phase 1
            {
                const juce::SpinLock::ScopedLockType lock(cacheLock);
                auto it = metadataCache.find(cacheKey);
                if (it != metadataCache.end())
                    meta = it->second;
            }

            // Check if a newer request is waiting -- if so, skip slow dbserver
            // queries and jump directly to NFS async (which doesn't block).
            bool hasNewerRequests = (reqWritePos.load(std::memory_order_acquire)
                                     != reqReadPos.load(std::memory_order_relaxed));

            if (hasNewerRequests)
            {
                DBG("DbServerClient: phase2 SKIPPING dbserver queries (newer requests in queue)");
            }

            if (!hasNewerRequests)
            {

            // Beat grid: fetch if not yet cached
            if (req.wantWaveform && !meta.hasBeatGrid() && conn->isConnected())
            {
                auto grid = queryBeatGrid(*conn, req.slot, req.trackType,
                                           req.trackId, req.ourPlayer);
                if (!grid.empty())
                {
                    const juce::SpinLock::ScopedLockType lock(cacheLock);
                    auto it = metadataCache.find(cacheKey);
                    if (it != metadataCache.end())
                        { it->second.beatGrid = std::move(grid); ++it->second.cacheVersion; }
                }
            }

            // Detail waveform: fetch if not yet cached
            if (req.wantWaveform && !meta.hasDetailWaveform() && conn->isConnected())
            {
                auto detail = queryDetailWaveform(*conn, req.slot, req.trackType,
                                                   req.trackId, req.ourPlayer,
                                                   req.playerModel);
                DBG("DbServerClient: phase2 queryDetailWaveform trackId=" + juce::String(req.trackId)
                    + " entries=" + juce::String(detail.entryCount)
                    + " bpe=" + juce::String(detail.bytesPerEntry)
                    + " dataSz=" + juce::String((int)detail.data.size()));
                if (detail.entryCount > 0)
                {
                    const juce::SpinLock::ScopedLockType lock(cacheLock);
                    auto it = metadataCache.find(cacheKey);
                    if (it != metadataCache.end())
                    {
                        it->second.detailData = std::move(detail.data);
                        it->second.detailEntryCount = detail.entryCount;
                        it->second.detailBytesPerEntry = detail.bytesPerEntry;
                        ++it->second.cacheVersion;
                    }
                }
            }
            else
            {
                DBG("DbServerClient: phase2 SKIP detail query -- wantWf=" + juce::String((int)req.wantWaveform)
                    + " hasDetail=" + juce::String((int)meta.hasDetailWaveform())
                    + " connected=" + juce::String((int)conn->isConnected()));
            }

            // Song structure (phrase analysis): fetch if not yet cached
            if (req.wantWaveform && !meta.hasSongStructure() && conn->isConnected())
            {
                auto ss = querySongStructure(*conn, req.slot, req.trackType,
                                              req.trackId, req.ourPlayer);
                if (!ss.phrases.empty())
                {
                    const juce::SpinLock::ScopedLockType lock(cacheLock);
                    auto it = metadataCache.find(cacheKey);
                    if (it != metadataCache.end())
                    {
                        it->second.phraseMood = ss.mood;
                        it->second.songStructure = std::move(ss.phrases);
                        ++it->second.cacheVersion;
                    }
                }
            }

            // Cue list (rekordbox hot cues, memory points, loops with colors)
            if (req.wantWaveform && !meta.hasCueList() && conn->isConnected())
            {
                auto cues = queryCueList(*conn, req.slot, req.trackType,
                                          req.trackId, req.ourPlayer);
                if (!cues.empty())
                {
                    const juce::SpinLock::ScopedLockType lock(cacheLock);
                    auto it = metadataCache.find(cacheKey);
                    if (it != metadataCache.end())
                        { it->second.cueList = std::move(cues); ++it->second.cacheVersion; }
                }
            }

            } // end if (!hasNewerRequests)

            // Save to disk cache ALWAYS (not gated by hasNewerRequests).
            // Even if dbserver queries were skipped, save whatever we got
            // from disk cache + phase 1 + NFS async.
            {
                std::string saveDiskKey;
                bool hasNewData = false;
                {
                    const juce::SpinLock::ScopedLockType lock(cacheLock);
                    auto it = metadataCache.find(cacheKey);
                    if (it != metadataCache.end())
                    {
                        hasNewData = (it->second.hasBeatGrid() || it->second.hasCueList()
                                      || it->second.hasSongStructure() || it->second.hasDetailWaveform());
                        if (hasNewData && it->second.artist.isNotEmpty() && it->second.title.isNotEmpty())
                            saveDiskKey = TrackMapEntry::makeKey(
                                it->second.artist, it->second.title, it->second.durationSeconds);
                    }
                }
                if (!saveDiskKey.empty() && hasNewData)
                    saveAnlzToDisk(cacheKey, saveDiskKey);
            }

            // --- NFS ANLZ Fallback ---
            // If dbserver queries AND disk cache both failed to provide beat grid,
            // cues, or song structure, download via NFS from CDJ USB/SD.
            {
                bool needsNfs = false;
                juce::String anlzPath;
                uint32_t trackIdForNfs = 0;
                std::string diskCacheKey;
                {
                    const juce::SpinLock::ScopedLockType lock(cacheLock);
                    auto it = metadataCache.find(cacheKey);
                    if (it != metadataCache.end()
                        && !it->second.nfsAttempted
                        && (!it->second.hasBeatGrid() || !it->second.hasCueList()
                            || !it->second.hasSongStructure()
                            || !it->second.hasDetailWaveform()))
                    {
                        needsNfs = true;
                        anlzPath = it->second.anlzPath;
                        trackIdForNfs = req.trackId;

                        if (it->second.artist.isNotEmpty() && it->second.title.isNotEmpty())
                            diskCacheKey = TrackMapEntry::makeKey(
                                it->second.artist, it->second.title,
                                it->second.durationSeconds);
                    }
                }

                if (needsNfs)
                {
                    DBG("DbServerClient: NFS LAUNCH trackId=" + juce::String(trackIdForNfs)
                        + " anlzPath=" + anlzPath
                        + " diskKey=" + juce::String(diskCacheKey.substr(0, 40)));
                    {
                        const juce::SpinLock::ScopedLockType lock(cacheLock);
                        auto it = metadataCache.find(cacheKey);
                        if (it != metadataCache.end())
                            it->second.nfsAttempted = true;
                    }

                    juce::String nfsPlayerIP = req.playerIP;
                    uint8_t nfsSlot = req.slot;
                    launchNfsAsync(cacheKey, nfsPlayerIP, nfsSlot,
                                   trackIdForNfs, anlzPath, diskCacheKey);
                }
            }
        }
        else if (req.artworkId != 0)
        {
            // Artwork-only request
            auto img = queryArtwork(*conn, req.slot, req.trackType,
                                     req.artworkId, req.ourPlayer);
            if (img.isValid())
                cacheArtwork(req.artworkId, img);
        }
    }

    //==========================================================================
    // Member data
    //==========================================================================
    std::atomic<bool> isRunningFlag { false };

    // Request queue (MPSC: any thread produces via queueLock, DB thread consumes)
    juce::SpinLock queueProducerLock;  // serialises requestMetadata/requestArtwork callers
    std::array<MetadataRequest, kRequestQueueSize> requestQueue;
    std::atomic<uint32_t> reqWritePos { 0 };
    std::atomic<uint32_t> reqReadPos  { 0 };
    juce::WaitableEvent requestSemaphore { false };  // auto-reset: prevents busy-loop

    // TCP connections (one per CDJ, max 6)
    std::array<PlayerConnection, kMaxConnections> connections;

    // Metadata cache (protected by SpinLock)
    mutable juce::SpinLock cacheLock;
    std::unordered_map<uint64_t, TrackMetadata> metadataCache;

    // Artwork cache (separate lock for independent access)
    mutable juce::SpinLock artCacheLock;
    std::unordered_map<uint32_t, juce::Image> artworkCache;

    // Stats
    std::atomic<uint32_t> queryCount { 0 };
    std::atomic<uint32_t> errorCount { 0 };

    // NFS ANLZ fetcher -- downloads .EXT files directly from CDJ USB/SD
    // Used as fallback when dbserver ANLZ tag queries fail (CDJ-3000).
    // Runs on its own thread to avoid blocking metadata requests.
    NfsAnlzFetcher nfsAnlzFetcher;
    std::thread nfsThread;

    /// Launch NFS download on a separate thread.
    /// Only one NFS download at a time (joins previous if still running).
    /// Run NFS fallback only (used when dbserver connection fails).
    /// Ensures a cache entry exists and launches NFS async download.
    void processNfsFallback(const MetadataRequest& req, uint64_t cacheKey)
    {
        // Ensure cache entry exists (may have been created by a previous failed attempt)
        {
            const juce::SpinLock::ScopedLockType lock(cacheLock);
            auto it = metadataCache.find(cacheKey);
            if (it == metadataCache.end())
            {
                TrackMetadata meta;
                meta.trackId = req.trackId;
                meta.waveformQueried = true;
                metadataCache[cacheKey] = std::move(meta);
            }
        }

        bool needsNfs = false;
        juce::String anlzPath;
        std::string diskCacheKey;
        {
            const juce::SpinLock::ScopedLockType lock(cacheLock);
            auto it = metadataCache.find(cacheKey);
            if (it != metadataCache.end()
                && !it->second.nfsAttempted
                && (!it->second.hasBeatGrid() || !it->second.hasCueList()
                    || !it->second.hasSongStructure()
                    || !it->second.hasDetailWaveform()))
            {
                needsNfs = true;
                anlzPath = it->second.anlzPath;
                it->second.nfsAttempted = true;

                if (it->second.artist.isNotEmpty() && it->second.title.isNotEmpty())
                    diskCacheKey = TrackMapEntry::makeKey(
                        it->second.artist, it->second.title,
                        it->second.durationSeconds);
            }
        }

        if (needsNfs)
        {
            DBG("DbServerClient: NFS LAUNCH (no conn) trackId=" + juce::String(req.trackId));
            launchNfsAsync(cacheKey, req.playerIP, req.slot,
                           req.trackId, anlzPath, diskCacheKey);
        }
    }

    void launchNfsAsync(uint64_t cacheKey, const juce::String& playerIP,
                        uint8_t slot, uint32_t trackId,
                        const juce::String& anlzPath,
                        const std::string& diskCacheKey)
    {
        // Join previous NFS thread if still running
        if (nfsThread.joinable())
            nfsThread.join();

        nfsThread = std::thread([this, cacheKey, playerIP, slot, trackId, anlzPath, diskCacheKey]()
        {
            NfsAnlzFetcher::AnlzResult anlz;

            if (anlzPath.isNotEmpty())
            {
                DBG("DbServerClient: NFS async (with path) -- " + anlzPath);
                anlz = nfsAnlzFetcher.fetchAndParse(playerIP, slot, anlzPath);
            }
            else
            {
                DBG("DbServerClient: NFS async (PDB lookup) -- trackId=" + juce::String(trackId));
                anlz = nfsAnlzFetcher.fetchByTrackId(playerIP, slot, trackId);
            }

            if (anlz.ok && isRunningFlag.load(std::memory_order_relaxed))
            {
                {
                    const juce::SpinLock::ScopedLockType lock(cacheLock);
                    auto it = metadataCache.find(cacheKey);
                    if (it != metadataCache.end())
                    {
                        // NFS data is authoritative (from USB) -- always overwrite
                        applyNfsAnlzResult(it->second, anlz, true);
                        ++it->second.cacheVersion;
                        DBG("DbServerClient: NFS ANLZ applied -- beats="
                            + juce::String((int)it->second.beatGrid.size())
                            + " cues=" + juce::String((int)it->second.cueList.size())
                            + " phrases=" + juce::String((int)it->second.songStructure.size())
                            + " detailEntries=" + juce::String(it->second.detailEntryCount)
                            + " detailBpe=" + juce::String(it->second.detailBytesPerEntry)
                            + " detailDataSz=" + juce::String((int)it->second.detailData.size()));
                    }
                }

                // Persist to disk cache for next session
                if (!diskCacheKey.empty())
                    saveAnlzToDisk(cacheKey, diskCacheKey);
            }
        });
    }

    /// Build a CachedAnlz from in-memory TrackMetadata and save to disk.
    void saveAnlzToDisk(uint64_t cacheKey, const std::string& diskKey)
    {
        WaveformCache::CachedAnlz ca;

        {
            const juce::SpinLock::ScopedLockType lock(cacheLock);
            auto it = metadataCache.find(cacheKey);
            if (it == metadataCache.end()) return;
            auto& m = it->second;

            for (auto& b : m.beatGrid)
                ca.beatGrid.push_back({ b.beatNumber, b.bpmTimes100, b.timeMs });

            for (auto& c : m.cueList)
            {
                WaveformCache::CachedAnlz::Cue cc;
                cc.type = (uint8_t)c.type;
                cc.hotCueNumber = c.hotCueNumber;
                cc.positionMs = c.positionMs;
                cc.loopEndMs = c.loopEndMs;
                cc.colorR = c.colorR;  cc.colorG = c.colorG;  cc.colorB = c.colorB;
                cc.colorCode = c.colorCode;
                cc.hasColor = c.hasColor;
                cc.comment = c.comment;
                ca.cueList.push_back(std::move(cc));
            }

            for (auto& p : m.songStructure)
                ca.songStructure.push_back({ p.index, p.beatNumber, p.kind, p.fill, p.beatCount, p.beatFill });

            ca.phraseMood = m.phraseMood;
            ca.detailData = m.detailData;
            ca.detailEntryCount = m.detailEntryCount;
            ca.detailBytesPerEntry = m.detailBytesPerEntry;
        }

        ca.valid = true;
        WaveformCache::saveAnlz(diskKey, ca);
        DBG("DbServerClient: ANLZ SAVE beats=" + juce::String((int)ca.beatGrid.size())
            + " cues=" + juce::String((int)ca.cueList.size())
            + " phrases=" + juce::String((int)ca.songStructure.size())
            + " detailEntries=" + juce::String(ca.detailEntryCount)
            + " detailBpe=" + juce::String(ca.detailBytesPerEntry)
            + " detailDataSz=" + juce::String((int)ca.detailData.size())
            + " key=" + juce::String(diskKey.substr(0, 40)));
    }

    /// Apply disk-cached ANLZ data to in-memory TrackMetadata.
    static void applyCachedAnlz(TrackMetadata& meta, const WaveformCache::CachedAnlz& ca)
    {
        if (!ca.beatGrid.empty() && meta.beatGrid.empty())
        {
            for (auto& b : ca.beatGrid)
            {
                TrackMetadata::BeatEntry e;
                e.beatNumber = b.beatNumber;  e.bpmTimes100 = b.bpmTimes100;  e.timeMs = b.timeMs;
                meta.beatGrid.push_back(e);
            }
        }
        if (!ca.cueList.empty() && meta.cueList.empty())
        {
            for (auto& c : ca.cueList)
            {
                TrackMetadata::RekordboxCue rc;
                rc.type = (TrackMetadata::RekordboxCue::Type)c.type;
                rc.hotCueNumber = c.hotCueNumber;
                rc.positionMs = c.positionMs;
                rc.loopEndMs = c.loopEndMs;
                rc.colorR = c.colorR;  rc.colorG = c.colorG;  rc.colorB = c.colorB;
                rc.colorCode = c.colorCode;
                rc.hasColor = c.hasColor;
                rc.comment = c.comment;
                meta.cueList.push_back(rc);
            }
        }
        if (!ca.songStructure.empty() && meta.songStructure.empty())
        {
            for (auto& p : ca.songStructure)
            {
                TrackMetadata::PhraseEntry pe;
                pe.index = p.index;  pe.beatNumber = p.beatNumber;
                pe.kind = p.kind;  pe.fill = p.fill;
                pe.beatCount = p.beatCount;  pe.beatFill = p.beatFill;
                meta.songStructure.push_back(pe);
            }
            meta.phraseMood = ca.phraseMood;
        }
        if (ca.detailEntryCount > 0 && meta.detailEntryCount == 0)
        {
            meta.detailData = ca.detailData;
            meta.detailEntryCount = ca.detailEntryCount;
            meta.detailBytesPerEntry = ca.detailBytesPerEntry;
        }
    }

    //==========================================================================
    // NFS result -> TrackMetadata conversion helpers
    //==========================================================================
    static void applyNfsAnlzResult(TrackMetadata& meta, const NfsAnlzFetcher::AnlzResult& anlz,
                                    bool forceOverwrite = false)
    {
        if (!anlz.beatGrid.empty() && (forceOverwrite || meta.beatGrid.empty()))
        {
            meta.beatGrid.clear();
            meta.beatGrid.reserve(anlz.beatGrid.size());
            for (auto& b : anlz.beatGrid)
            {
                TrackMetadata::BeatEntry e;
                e.beatNumber  = b.beatNumber;
                e.bpmTimes100 = b.bpmTimes100;
                e.timeMs      = b.timeMs;
                meta.beatGrid.push_back(e);
            }
        }

        if (!anlz.cueList.empty() && (forceOverwrite || meta.cueList.empty()))
        {
            meta.cueList.clear();
            for (auto& c : anlz.cueList)
            {
                TrackMetadata::RekordboxCue cue;
                cue.positionMs   = c.positionMs;
                cue.loopEndMs    = c.loopEndMs;
                cue.hotCueNumber = (uint8_t)c.hotCueNumber;
                cue.colorR       = c.colorR;
                cue.colorG       = c.colorG;
                cue.colorB       = c.colorB;
                cue.colorCode    = c.colorCode;
                cue.hasColor     = c.hasColor;
                cue.comment      = c.comment;
                cue.type = (c.type == NfsAnlzFetcher::CueEntry::Loop)      ? TrackMetadata::RekordboxCue::Loop
                         : (c.type == NfsAnlzFetcher::CueEntry::HotCue)    ? TrackMetadata::RekordboxCue::HotCue
                         : TrackMetadata::RekordboxCue::MemoryPoint;
                meta.cueList.push_back(cue);
            }
        }

        if (!anlz.songStructure.empty() && (forceOverwrite || meta.songStructure.empty()))
        {
            meta.songStructure.clear();
            meta.phraseMood = anlz.phraseMood;
            for (auto& p : anlz.songStructure)
            {
                TrackMetadata::PhraseEntry pe;
                pe.index      = p.index;
                pe.beatNumber = p.beatNumber;
                pe.kind       = p.kind;
                pe.beatCount  = p.beatCount;
                pe.fill       = p.fill;
                pe.beatFill   = p.beatFill;
                meta.songStructure.push_back(pe);
            }
        }

        if (!anlz.detailData.empty())
        {
            // Never downgrade: PWV7 (3 bytes/entry) is better than PWV5 (2 bytes/entry).
            // NFS .EXT only has PWV5, but dbserver can serve PWV7 on CDJ-3000.
            bool shouldReplace = meta.detailData.empty()
                || (forceOverwrite && anlz.detailBytesPerEntry >= meta.detailBytesPerEntry);
            if (shouldReplace)
            {
                meta.detailData         = anlz.detailData;
                meta.detailEntryCount   = anlz.detailEntryCount;
                meta.detailBytesPerEntry = anlz.detailBytesPerEntry;
            }
        }
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DbServerClient)
};
