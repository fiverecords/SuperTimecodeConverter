// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter
//
// DbServerClient -- TCP client for the Pioneer dbserver protocol (its port
// is found by asking TCP 12523).  Queries the players' databases for track
// metadata (title, artist, album, key, genre, artwork) of rekordbox tracks
// loaded on the decks, and the analysis data (preview and detail waveform,
// beat grid, cues, phrases) from the dbserver (CDJ-3000 class) or over NFS
// (NfsAnlzFetcher), with a disk cache (WaveformCache).  Also answers other
// devices' queries on TCP 12523 with "port 0" (dbPortListenerLoop).
//
// Protocol reference: DJ Link Ecosystem Analysis
//   https://djl-analysis.deepsymmetry.org/djl-analysis/track_metadata.html
//
// Thread model:
//   - The message thread (TimecodeEngine::tick, the PDL View, MainComponent)
//     calls requestMetadata() and the getCached*() lookups; requestMetadata
//     may be called from any thread.
//   - A worker thread (this juce::Thread) consumes the requests from a
//     32-entry ring: many producers, serialised by queueProducerLock, one
//     consumer.  It owns the dbserver connections.
//   - One NFS thread at a time (nfsThread) downloads analysis files.
//   - A listener thread answers TCP 12523.
//   - Results go to a metadata cache (cacheLock, 256 entries, the one
//     fetched longest ago is evicted -- not LRU) and an artwork cache
//     (artCacheLock, 64 entries, an arbitrary one is evicted).
//
// Player number constraint (AUDIT A20, AUDIT C19):
//   A CDJ-3000 or XDJ-AZ answers queries from any player number; older
//   players answer only a number 1-4 that is on the network.  The caller
//   chooses it (TimecodeEngine::requestDbMetadata, ProDJLinkView).  A refused
//   query leaves the track with no title (the UI shows "Track #12345"); the
//   NFS route still runs (AUDIT META-7).

#pragma once
#include <JuceHeader.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <unordered_map>
#include <vector>
#include <utility>
#include <cstring>
#include <thread>
#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <netinet/tcp.h>
#endif
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
    int durationSeconds = 0;
    int bpmTimes100 = 0;           // e.g. 12800 = 128.00 BPM
    int rating = 0;                // 0-5 stars
    uint32_t artworkId = 0;        // for separate artwork request
    double cacheTime = 0.0;        // when this entry was fetched (eviction drops the oldest)

    // Color preview waveform data from CDJ
    //   ThreeBand (PWV6): 3 bytes/entry = {mid, high, low} frequency heights
    //   ColorNxs2 (PWV4): 6 bytes/entry = {d0, d1, d2, d3, d4, d5}: d0-d1
    //     "whiteness", d2 energy below ~10 kHz, d3/d4/d5 low/mid/high energy,
    //     which beat-link combines into a colour (dysentery, "Track metadata",
    //     colour preview analysis) -- they are not R, G, B
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
        uint16_t kind;           // phrase kind; its meaning depends on the mood (rekordbox_anlz.ksy mood_*_phrase)
        uint8_t  fill;           // nonzero: the phrase ends with a fill-in
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
        dbPortInboundCount.store(0, std::memory_order_relaxed);

        // Spawn the dbserver-port-discovery listener (TCP 12523).
        // See dbPortListenerLoop() for protocol details.  Non-fatal on bind
        // failure (port may be in use by another DJ Link tool on the same
        // machine, in which case that tool takes care of replying).
        if (!dbPortListenSock)
        {
            dbPortListenSock = std::make_unique<juce::StreamingSocket>();
            if (dbPortListenSock->createListener(kPortDiscoveryPort, /*localHost*/ {}))
            {
                DBG("DbServerClient: listening on TCP " + juce::String(kPortDiscoveryPort)
                    + " (replies port 0 / no service)");
                dbPortListenerThread = std::thread([this]() { dbPortListenerLoop(); });
            }
            else
            {
                DBG("DbServerClient: could not bind TCP listener on port "
                    + juce::String(kPortDiscoveryPort)
                    + " (another DJ Link tool already running?)");
                dbPortListenSock.reset();
            }
        }

        startThread(juce::Thread::Priority::low);
    }

    void stop()
    {
        isRunningFlag.store(false, std::memory_order_relaxed);

        // Wake the thread if it's waiting
        requestSemaphore.signal();

        // The worker checks threadShouldExit() between steps and every wait
        // it makes is bounded, the longest being one TCP connect
        // (kConnectTimeoutMs), so it exits within that plus a few hundred ms
        // and stopThread never has to kill it (AUDIT META-14).
        if (isThreadRunning())
            stopThread(kStopTimeoutMs);

        // End any in-flight NFS download (NfsAnlzFetcher::cancel, AUDIT
        // WIRE-6) and wait for its thread.  The worker took the download's
        // cancel token before it started the thread, so a download whose
        // thread has not begun yet ends too.
        nfsAnlzFetcher.cancel();
        if (nfsThread.joinable())
            nfsThread.join();

        // Tear down the dbserver-port listener.  Closing the socket unblocks
        // waitForNextConnection() inside dbPortListenerLoop so it can exit.
        if (dbPortListenSock)
            dbPortListenSock->close();
        if (dbPortListenerThread.joinable())
            dbPortListenerThread.join();
        dbPortListenSock.reset();

        // Close all connections
        for (auto& conn : connections)
            conn.close();

        // Clear any pending invalidation requests
        {
            const juce::ScopedLock sl(pendingInvalidateLock);
            pendingInvalidateIPs.clear();
        }
    }

    bool getIsRunning() const { return isRunningFlag.load(std::memory_order_relaxed); }

    //==========================================================================
    // Request metadata (called from UI/engine thread)
    //
    //   playerIP:  IP of the player whose media holds the track (the source
    //              player when another deck loaded it over Link); with slot
    //              and trackId it is the cache key (AUDIT META-5)
    //   slot:      media slot (2=SD, 3=USB)
    //   trackType: 1=rekordbox, 2=non-rekordbox, 5=CD
    //   trackId:   rekordbox database ID (from CDJ Status packet)
    //   ourPlayer: the player number to query as (see "Player number
    //              constraint" at the top of this file)
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
        const CacheKey cacheKey = makeCacheKey(playerIP, slot, trackId);
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

        // Every field is set: a reused slot must not keep the phase of the
        // phase-2 request it last held (AUDIT META-6).
        auto& req = requestQueue[wp & kRequestQueueMask];
        req = MetadataRequest{};
        req.playerIP    = playerIP;
        req.playerModel = playerModel;
        req.slot        = slot;
        req.trackType   = trackType;
        req.trackId     = trackId;
        req.ourPlayer   = (uint8_t)ourPlayer;
        req.wantArt     = true;
        req.wantWaveform = true;
        req.phase       = 1;

        reqWritePos.store(wp + 1, std::memory_order_release);
        requestSemaphore.signal();
    }

    //==========================================================================
    // Retrieve cached metadata (called from UI/engine thread)
    //
    // rekordbox IDs are per export: the same ID on another player, or in the
    // other slot of the same player, is another track.  An entry is therefore
    // keyed by the player that holds the media (the IP requestMetadata was
    // given), the slot and the ID (AUDIT META-5).  The (playerIP, slot, id)
    // lookups below are exact; use them.  The older lookups without the slot,
    // or by ID alone, answer only when every entry they match is the same
    // track (same artist, title and duration, title not empty) and return
    // nothing when the ID is cached for different tracks -- they never guess.
    //==========================================================================
    TrackMetadata getCachedMetadata(const juce::String& playerIP, uint8_t slot, uint32_t trackId) const
    {
        const juce::SpinLock::ScopedLockType lock(cacheLock);
        auto it = metadataCache.find(makeCacheKey(playerIP, slot, trackId));
        if (it != metadataCache.end())
            return it->second;
        return {};
    }

    /// Without the slot: the entry for this player and ID, unless its two
    /// slots hold different tracks under that ID.
    TrackMetadata getCachedMetadata(const juce::String& playerIP, uint32_t trackId) const
    {
        const juce::SpinLock::ScopedLockType lock(cacheLock);
        auto* m = findUniqueLocked(ipToUint32(playerIP), false, trackId);
        return m != nullptr ? *m : TrackMetadata{};
    }

    /// By ID alone (all players): nothing when the ID is ambiguous.
    TrackMetadata getCachedMetadataByTrackId(uint32_t trackId) const
    {
        if (trackId == 0) return {};
        const juce::SpinLock::ScopedLockType lock(cacheLock);
        auto* m = findUniqueLocked(0, true, trackId);
        return m != nullptr ? *m : TrackMetadata{};
    }

    /// Lightweight version check -- returns the cache version counter for a
    /// track without copying any data.  Use to skip expensive getCachedMetadata
    /// calls when nothing has changed in the background thread.
    uint32_t getMetadataVersion(const juce::String& playerIP, uint8_t slot, uint32_t trackId) const
    {
        const juce::SpinLock::ScopedLockType lock(cacheLock);
        auto it = metadataCache.find(makeCacheKey(playerIP, slot, trackId));
        return (it != metadataCache.end()) ? it->second.cacheVersion : 0;
    }

    /// Without the slot (see getCachedMetadata above).
    uint32_t getMetadataVersion(const juce::String& playerIP, uint32_t trackId) const
    {
        const juce::SpinLock::ScopedLockType lock(cacheLock);
        auto* m = findUniqueLocked(ipToUint32(playerIP), false, trackId);
        return m != nullptr ? m->cacheVersion : 0;
    }

    /// By ID alone (all players): 0 when the ID is ambiguous.
    uint32_t getMetadataVersionByTrackId(uint32_t trackId) const
    {
        if (trackId == 0) return 0;
        const juce::SpinLock::ScopedLockType lock(cacheLock);
        auto* m = findUniqueLocked(0, true, trackId);
        return m != nullptr ? m->cacheVersion : 0;
    }

    /// Lightweight check: does the cached metadata have detail waveform data?
    /// Used by the UI timer to avoid expensive full-copy getCachedMetadata()
    /// when only checking if detail data has arrived yet.
    bool hasDetailWaveformCached(const juce::String& playerIP, uint8_t slot, uint32_t trackId) const
    {
        const juce::SpinLock::ScopedLockType lock(cacheLock);
        auto it = metadataCache.find(makeCacheKey(playerIP, slot, trackId));
        return it != metadataCache.end() && it->second.hasDetailWaveform();
    }

    /// Without the slot: this player's entry, else the ID alone (both
    /// subject to the same-track rule above).
    bool hasDetailWaveformCached(const juce::String& playerIP, uint32_t trackId) const
    {
        const juce::SpinLock::ScopedLockType lock(cacheLock);
        auto* m = findUniqueLocked(ipToUint32(playerIP), false, trackId);
        if (m == nullptr)
            m = findUniqueLocked(0, true, trackId);
        return m != nullptr && m->hasDetailWaveform();
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

    MetadataLight getCachedMetadataLight(const juce::String& playerIP, uint8_t slot, uint32_t trackId) const
    {
        const juce::SpinLock::ScopedLockType lock(cacheLock);
        auto it = metadataCache.find(makeCacheKey(playerIP, slot, trackId));
        return it != metadataCache.end() ? makeLight(it->second) : MetadataLight{};
    }

    /// By ID alone (all players): nothing when the ID is ambiguous.
    MetadataLight getCachedMetadataLightById(uint32_t trackId) const
    {
        if (trackId == 0) return {};
        const juce::SpinLock::ScopedLockType lock(cacheLock);
        auto* m = findUniqueLocked(0, true, trackId);
        return m != nullptr ? makeLight(*m) : MetadataLight{};
    }

    //==========================================================================
    // Retrieve cached artwork (returns null Image if not cached).  Artwork IDs
    // are per export too, so artwork is keyed like metadata (AUDIT META-5).
    //==========================================================================
    juce::Image getCachedArtwork(const juce::String& playerIP, uint8_t slot, uint32_t artworkId) const
    {
        if (artworkId == 0) return {};
        const juce::SpinLock::ScopedLockType lock(artCacheLock);
        auto it = artworkCache.find(makeCacheKey(playerIP, slot, artworkId));
        if (it != artworkCache.end())
            return it->second;
        return {};
    }

    /// By artwork ID alone: only when exactly one medium has art under that
    /// ID.  With two, nothing tells whether the ID names the same art on both
    /// (two copies of one export) or different art, so nothing is returned:
    /// two identical USBs show no artwork here until the callers use the
    /// lookup above with the player and slot (AUDIT META-5).
    juce::Image getCachedArtwork(uint32_t artworkId) const
    {
        if (artworkId == 0) return {};
        const juce::SpinLock::ScopedLockType lock(artCacheLock);
        const juce::Image* found = nullptr;
        for (auto& [k, img] : artworkCache)
        {
            if (k.id != artworkId) continue;
            if (found != nullptr) return {};
            found = &img;
        }
        return found != nullptr ? *found : juce::Image();
    }

    //==========================================================================
    // Invalidate cache for a player (called when ProDJLinkInput reports the
    // player lost; a media change in one slot is mediaChanged below)
    //==========================================================================
    void invalidatePlayer(const juce::String& playerIP)
    {
        const uint32_t ip = ipToUint32(playerIP);
        {
            const juce::SpinLock::ScopedLockType lock(cacheLock);
            for (auto it = metadataCache.begin(); it != metadataCache.end(); )
            {
                if (it->first.ip == ip)
                    it = metadataCache.erase(it);
                else
                    ++it;
            }
        }
        {
            const juce::SpinLock::ScopedLockType lock(artCacheLock);
            for (auto it = artworkCache.begin(); it != artworkCache.end(); )
            {
                if (it->first.ip == ip)
                    it = artworkCache.erase(it);
                else
                    ++it;
            }
        }
        forgetAddress(playerIP);
    }

    //==========================================================================
    // A player's SD (2) or USB (3) slot reported empty, or media was mounted
    // there after it did (ProDJLinkInput::onMediaChanged, AUDIT META-4).
    // rekordbox IDs are per export, so the metadata and artwork cached for
    // that player and slot, and the NFS fetcher's export.pdb index and mount
    // handle for it, describe the media that was there: they are dropped,
    // and the next request fetches the new media's.  A track still loaded
    // from the old media (a deck in emergency loop) loses its metadata as
    // well, as beat-link's MetadataFinder flushes its cache when the slot
    // reports empty.  An NFS download already running for that slot does
    // not store its result (slotMediaGeneration).  Any thread (the Pro DJ
    // Link network thread).
    //==========================================================================
    void mediaChanged(const juce::String& playerIP, uint8_t slot)
    {
        const uint32_t ip = ipToUint32(playerIP);
        {
            const juce::SpinLock::ScopedLockType lock(cacheLock);
            for (auto it = metadataCache.begin(); it != metadataCache.end(); )
            {
                if (it->first.ip == ip && it->first.slot == slot)
                    it = metadataCache.erase(it);
                else
                    ++it;
            }
            ++slotMediaGeneration[slotKey(ip, slot)];
        }
        {
            const juce::SpinLock::ScopedLockType lock(artCacheLock);
            for (auto it = artworkCache.begin(); it != artworkCache.end(); )
            {
                if (it->first.ip == ip && it->first.slot == slot)
                    it = artworkCache.erase(it);
                else
                    ++it;
            }
        }
        nfsAnlzFetcher.clearPdbCache(playerIP, slot);   // thread-safe
    }

    //==========================================================================
    // A player moved to a new address (ProDJLinkInput::onPlayerMoved, AUDIT
    // PDL-3).  It is the same unit with the same media, so what is cached for
    // it -- metadata and artwork, keyed by the address of the player holding
    // the media -- is kept and re-keyed to newIp, where the callers look from
    // now on (getPlayerIP gives newIp).  An entry newIp already has with a
    // title is kept over the moved one.  The connections to oldIp are closed
    // and its dbserver port and NFS state forgotten, as for a lost player.
    // invalidatePlayer() here erased the current track's metadata, and
    // nothing asked for it again.  Any thread (the Pro DJ Link network
    // thread).
    //==========================================================================
    void movePlayer(const juce::String& oldIp, const juce::String& newIp)
    {
        const uint32_t from = ipToUint32(oldIp), to = ipToUint32(newIp);
        if (from == to)
            return;
        {
            const juce::SpinLock::ScopedLockType lock(cacheLock);
            std::vector<std::pair<CacheKey, TrackMetadata>> moved;
            for (auto it = metadataCache.begin(); it != metadataCache.end(); )
            {
                if (it->first.ip == from)
                {
                    moved.emplace_back(it->first, std::move(it->second));
                    it = metadataCache.erase(it);
                }
                else
                    ++it;
            }
            for (auto& [key, meta] : moved)
            {
                CacheKey newKey = key;
                newKey.ip = to;
                auto it = metadataCache.find(newKey);
                if (it == metadataCache.end() || !it->second.isValid())
                    metadataCache[newKey] = std::move(meta);
            }
        }
        {
            const juce::SpinLock::ScopedLockType lock(artCacheLock);
            std::vector<std::pair<CacheKey, juce::Image>> moved;
            for (auto it = artworkCache.begin(); it != artworkCache.end(); )
            {
                if (it->first.ip == from)
                {
                    moved.emplace_back(it->first, it->second);
                    it = artworkCache.erase(it);
                }
                else
                    ++it;
            }
            for (auto& [key, img] : moved)
            {
                CacheKey newKey = key;
                newKey.ip = to;
                if (artworkCache.find(newKey) == artworkCache.end())
                    artworkCache[newKey] = img;
            }
        }
        forgetAddress(oldIp);
    }

    /// Stats for UI/debugging
    int getCacheSize() const
    {
        const juce::SpinLock::ScopedLockType lock(cacheLock);
        return (int)metadataCache.size();
    }

    uint32_t getErrorCount() const  { return errorCount.load(std::memory_order_relaxed); }

    /// Inbound TCP connections accepted on port 12523 since start().
    /// Diagnostic for the NXS2 dbserver-probe storm (v1.9.11-beta15):
    /// see dbPortListenerLoop().  Displayed in the PDL View toolbar.
    uint32_t getDbPortInboundCount() const { return dbPortInboundCount.load(std::memory_order_relaxed); }

private:
    /// An address no player uses any more (lost, or moved away): queue it
    /// for the worker, which closes its connections and drops its cooldown,
    /// retries and NFS state; and drop its dbserver port -- if a player
    /// appears there again, the port may have changed.  Any thread.
    void forgetAddress(const juce::String& playerIP)
    {
        // Closing the socket directly from here would race against the worker
        // which may be in the middle of a TCP query using that socket.
        {
            const juce::ScopedLock sl(pendingInvalidateLock);
            pendingInvalidateIPs.addIfNotAlreadyThere(playerIP);
        }
        {
            const juce::ScopedLock sl(knownDbPortsLock);
            knownDbPorts.erase(playerIP.toStdString());
        }
    }

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
        r.phase       = phase;

        reqWritePos.store(wp + 1, std::memory_order_release);
        requestSemaphore.signal();
    }

    //==========================================================================
    // Constants
    //==========================================================================
    static constexpr int kPortDiscoveryPort = 12523;
    static constexpr int kConnectTimeoutMs  = 3000;
    static constexpr int kStopTimeoutMs     = kConnectTimeoutMs + 2000;   // see stop()
    static constexpr int kReadTimeoutMs     = 1000;  // CDJ responds in <10ms; 3000 was wasteful
    static constexpr int kMaxCacheEntries   = 256;
    static constexpr int kMaxArtCacheEntries = 64;
    static constexpr int kMaxConnections    = 6;
    static constexpr int kReconnectCooldownMs = 5000;   // per player IP, after a failed attempt or fresh session (AUDIT META-8)
    static constexpr int kCooldownRetries     = 3;      // asks again after the cooldown, per request (AUDIT META-8)
    static constexpr int kMaxCooldownRetryRequests = 16;  // remembered requests at most; the oldest is dropped
    // Idle connection lifetime.  Set to 30 s to mirror the way Beat Link's
    // ConnectionManager keeps a Client open between successive queries to the
    // same player (see ConnectionManager.invokeWithClientSession in the
    // open-source reference implementation).  At 1 s -- the value used by all
    // releases up to and including v1.9.9 -- back-to-back metadata requests
    // every ~1.7 s for the same loaded track caused a full close + reconnect
    // every cycle: STC's polite teardown waits 200 ms, then the next query
    // re-runs discoverDbPort (one TCP to 12523), opens dbserver (one TCP to
    // 1051 or 1052), exchanges the 5-byte greeting, and re-runs
    // setupQueryContext, all before any payload moves.  In Joren2087's
    // capture this produced 72 separate dbserver TCP sessions to the
    // USB-holder NXS2 in 124 s.  The old comment justifying the 1 s value
    // claimed it freed "CDJ NFS slots", but the NFSv2 server runs over UDP
    // (its port from the player's portmapper) and is independent of the
    // dbserver TCP slot count, so the 1 s value was free pressure on the
    // player with no corresponding benefit.  A session that stops answering
    // ("mute but TCP alive", seen on NXS2s) is not kept by this timeout: the
    // first read that fails closes it (readMessageOrInvalidate).
    static constexpr double kIdleTimeoutMs   = 30000.0;

    // Analysis-tag request (beat-link ANLZ_TAG_REQ), used here for every
    // ANLZ tag: waveforms, beat grid, cues, phrases.  Its reply (ANLZ_TAG,
    // 0x4f02) is matched by txId, not by type.
    static constexpr uint32_t kNxs2ExtRequest  = 0x2c04;
    // NXS2 color preview waveform (PWV4 from .EXT file)
    static constexpr uint32_t kMagic4VWP = 0x34565750;  // "PWV4" reversed
    static constexpr uint32_t kMagic5VWP = 0x35565750;  // "PWV5" reversed
    static constexpr uint32_t kMagicTXE  = 0x00545845;  // "EXT" reversed
    // CDJ-3000 3-band waveform (PWV6/PWV7 from .2EX file)
    static constexpr uint32_t kMagic6VWP = 0x36565750;  // "PWV6" reversed -- 3-band preview
    static constexpr uint32_t kMagic7VWP = 0x37565750;  // "PWV7" reversed -- 3-band detail
    static constexpr uint32_t kMagicXE2  = 0x00584532;  // "2EX" reversed

    // ANLZ tag magic values for 0x2c04 ext requests (little-endian uint32 of ASCII)
    // Same reversed convention as PWV4 etc.: "PQTZ" -> Z=5A T=54 Q=51 P=50
    static constexpr uint32_t kMagicPQTZ = 0x5A545150;  // "PQTZ" reversed -- beat grid
    static constexpr uint32_t kMagicPSSI = 0x49535350;  // "PSSI" reversed -- song structure
    static constexpr uint32_t kMagicPCO2 = 0x324F4350;  // "PCO2" reversed -- extended cue list (nxs2+)
    static constexpr uint32_t kMagicPCOB = 0x424F4350;  // "PCOB" reversed -- standard cue list

    // Caps on what a reply from a player can make STC allocate or loop over.
    // Every length or count read from the wire is checked against what is
    // left of the blob, in unsigned arithmetic, before it is used (AUDIT WIRE-4).
    static constexpr uint32_t kMaxFieldBlobBytes   = 2 * 1024 * 1024; // one 0x14 field (artwork JPEG, one ANLZ tag)
    static constexpr uint32_t kMaxFieldStringChars = 0x10000;         // one 0x26 field
    static constexpr uint32_t kMaxBeats            = 100000;          // PQTZ: 11 h at 150 BPM
    static constexpr uint32_t kMaxDetailEntries    = 5000000;         // PWV5/PWV7 at 150/s: 9.2 h (the blob cap binds first)
    static constexpr uint16_t kMaxPhrases          = 1000;            // PSSI
    static constexpr uint16_t kMaxCuesPerList      = 200;             // PCOB/PCO2
    static constexpr uint32_t kMaxPcp2EntryBytes   = 4096;            // one PCP2 entry, comment included
    static constexpr uint32_t kMaxCueCommentBytes  = 512;             // PCP2 comment (UTF-16BE)

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
        uint8_t contextPlayer = 0;  // player number used in setupQueryContext
        uint32_t txId = 0;
        double lastActivityTime = 0.0;  // hiRes ms -- for idle timeout
        bool reused = false;            // a later request than the one that opened it is using it (AUDIT META-8)

        bool isConnected() const { return socket && socket->isConnected(); }

        /// Graceful close: send teardown, wait for CDJ to close its side, then
        /// close our socket. Used for idle timeout and clean shutdown.
        void close()
        {
            if (socket && socket->isConnected())
            {
                // Polite dbserver protocol teardown so the CDJ closes the connection
                // from its side and properly releases its session resources.
                // Format: magic | TxID=0xfffffffe | type=0x0100 | argc=0 | 12 zero tag bytes.
                // Documented in the published Pro DJ Link protocol analysis.
                static const uint8_t teardown[] = {
                    0x11, 0x87,0x23,0x49,0xae,        // magic field (0x11-prefixed 4-byte number)
                    0x11, 0xff,0xff,0xff,0xfe,        // TxID = 0xfffffffe
                    0x10, 0x01,0x00,                  // type = 0x0100 (2-byte)
                    0x0f, 0x00,                       // argc = 0
                    0x14, 0x00,0x00,0x00,0x0c,        // blob, length = 12
                    0,0,0,0,0,0,0,0,0,0,0,0           // 12 zero tag bytes
                };
                socket->write(teardown, (int)sizeof(teardown));

                // After teardown, the CDJ closes the connection from its end,
                // so we can't read anything more. We wait briefly for that
                // close so our local close() doesn't fire while the teardown
                // bytes are still unacked, which on Windows/JUCE causes the
                // kernel to emit RST instead of a clean shutdown.
                // 200 ms is generous: the CDJ typically responds within 2 ms.
                // We drain any pending bytes so the socket is empty when we
                // close it, further reducing the chance of a reset.  read()
                // with shouldBlock=false never blocks (JUCE puts the socket in
                // non-blocking mode for it); the same 200 ms also bounds a
                // peer that keeps sending (AUDIT META-14).
                const double drainUntil = juce::Time::getMillisecondCounterHiRes() + 200.0;
                if (socket->waitUntilReady(true, 200) == 1)
                {
                    uint8_t drain[256];
                    while (juce::Time::getMillisecondCounterHiRes() < drainUntil
                           && socket->getRawSocketHandle() >= 0
                           && socket->read(drain, sizeof(drain), false) > 0)
                    {
                        // keep draining until empty, connection gone, or time up
                    }
                }
            }
            releaseSocket();
        }

        /// Abrupt close: skip the teardown. Used after a read failure, when
        /// sending more bytes on a socket where a previous query went
        /// unanswered could confuse the firmware further. The OS-level RST
        /// that results is less of a concern here: the CDJ dbserver is
        /// already in a bad state, and we need the connection gone so the
        /// next query opens a fresh one.
        void closeAbrupt()
        {
            releaseSocket();
        }

    private:
        void releaseSocket()
        {
            if (socket)
            {
                socket->close();
                socket = nullptr;
            }
            contextSetUp = false;
            contextPlayer = 0;
            txId = 0;
            dbPort = 0;
            lastActivityTime = 0.0;
            reused = false;
            playerIP.clear();
        }
    };

    //==========================================================================
    // Cache key: the player holding the media (IPv4 as a number), the slot,
    // and the rekordbox ID -- a track ID for metadata, an artwork ID for
    // artwork (AUDIT META-5)
    //==========================================================================
    struct CacheKey
    {
        uint32_t ip = 0;
        uint32_t id = 0;
        uint8_t  slot = 0;
        bool operator==(const CacheKey& o) const noexcept
        {
            return ip == o.ip && id == o.id && slot == o.slot;
        }
    };
    struct CacheKeyHash
    {
        size_t operator()(const CacheKey& k) const noexcept
        {
            return std::hash<uint64_t>{}(((uint64_t(k.ip) << 32) | k.id) ^ (uint64_t(k.slot) << 61));
        }
    };

    static CacheKey makeCacheKey(const juce::String& ip, uint8_t slot, uint32_t id)
    {
        return { ipToUint32(ip), id, slot };
    }

    /// Key of slotMediaGeneration: a player's IPv4 as a number, and a slot.
    static uint64_t slotKey(uint32_t ip, uint8_t slot)
    {
        return (uint64_t(ip) << 8) | slot;
    }

    /// Caller holds cacheLock.  Media changes seen so far in this slot.
    uint32_t slotMediaGenerationLocked(uint32_t ip, uint8_t slot) const
    {
        auto it = slotMediaGeneration.find(slotKey(ip, slot));
        return it != slotMediaGeneration.end() ? it->second : 0u;
    }

    /// Two entries describe the same track: same artist, title (not empty)
    /// and duration.  Copies of one export on two sticks pass; two exports
    /// that gave one ID to different tracks do not.
    static bool isSameTrack(const TrackMetadata& a, const TrackMetadata& b)
    {
        return a.title.isNotEmpty() && a.title == b.title && a.artist == b.artist
            && a.durationSeconds == b.durationSeconds;
    }

    /// Caller holds cacheLock.  The entry for trackId -- on player `ip`, or on
    /// any player when anyPlayer is true -- when every entry cached under that
    /// ID is the same track; nullptr when none is, or when they differ.
    const TrackMetadata* findUniqueLocked(uint32_t ip, bool anyPlayer, uint32_t trackId) const
    {
        const TrackMetadata* found = nullptr;
        for (auto& [k, m] : metadataCache)
        {
            if (k.id != trackId || (!anyPlayer && k.ip != ip))
                continue;
            if (found == nullptr)
                found = &m;
            else if (!isSameTrack(*found, m))
                return nullptr;
        }
        return found;
    }

    static MetadataLight makeLight(const TrackMetadata& meta)
    {
        MetadataLight m;
        if (!meta.isValid())
            return m;
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
            if (juce::Time::getMillisecondCounterHiRes() > deadline
                || juce::Thread::currentThreadShouldExit())   // stop() (AUDIT META-14)
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

    /// Read and drop exactly N bytes, all within one timeout.  Used for a blob
    /// field too large to keep, so the stream stays aligned on the next field.
    static bool skipExact(juce::StreamingSocket& sock, uint32_t numBytes, int timeoutMs)
    {
        uint8_t sink[4096];
        auto deadline = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
        while (numBytes > 0)
        {
            if (juce::Time::getMillisecondCounterHiRes() > deadline
                || juce::Thread::currentThreadShouldExit())
                return false;
            if (!sock.waitUntilReady(true, 100))
                continue;
            const int n = sock.read(sink, (int) std::min<uint32_t>(numBytes, (uint32_t) sizeof(sink)), false);
            if (n <= 0) return false;
            numBytes -= (uint32_t) n;
        }
        return true;
    }

    /// Decode big-endian UTF-16 code units into a String.  A surrogate pair
    /// becomes one code point (AUDIT D6, AUDIT WIRE-9); an unpaired surrogate
    /// becomes U+FFFD; NUL units (the trailing terminator) are skipped.
    static juce::String decodeUtf16BE(const uint8_t* p, size_t numUnits)
    {
        std::vector<juce::juce_wchar> cps;
        cps.reserve(numUnits + 1);
        for (size_t k = 0; k < numUnits; ++k)
        {
            uint32_t u = readBE16(p + 2 * k);
            if (u == 0)
                continue;
            if (u >= 0xD800 && u <= 0xDBFF && k + 1 < numUnits)
            {
                const uint32_t lo = readBE16(p + 2 * (k + 1));
                if (lo >= 0xDC00 && lo <= 0xDFFF)
                {
                    cps.push_back((juce::juce_wchar) (0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00)));
                    ++k;
                    continue;
                }
            }
            if (u >= 0xD800 && u <= 0xDFFF)
                u = 0xFFFD;
            cps.push_back((juce::juce_wchar) u);
        }
        cps.push_back(0);
        return juce::String(juce::CharPointer_UTF32(cps.data()));
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
            case 0x14:  // binary blob -- artwork JPEG, ANLZ tag, the 12 argument tags
            {
                uint8_t lenBuf[4];
                if (!readExact(sock, lenBuf, 4, timeoutMs)) return result;
                uint32_t len = (uint32_t(lenBuf[0]) << 24) | (uint32_t(lenBuf[1]) << 16)
                             | (uint32_t(lenBuf[2]) << 8)  | lenBuf[3];
                result.numericValue = len;
                if (len > kMaxFieldBlobBytes)
                {
                    // Too large to keep (a PWV7 of a track over ~77 min, or a
                    // misbehaving device): read past it and return the field
                    // with no data, so the message and the connection survive
                    // and the caller sees "no blob" (AUDIT WIRE-4).
                    if (!skipExact(sock, len, timeoutMs))
                        return result;
                    result.ok = true;
                    break;
                }
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
                if (charCount > kMaxFieldStringChars)  // protocol error
                    return result;
                if (charCount > 0)
                {
                    uint32_t byteCount = charCount * 2;
                    std::vector<uint8_t> data(byteCount);
                    if (!readExact(sock, data.data(), (int)byteCount, timeoutMs))
                        return result;
                    // Decode UTF-16BE, skip trailing NUL
                    result.stringValue = decodeUtf16BE(data.data(), charCount);
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
        // Capture the tag bytes -- we use them below to apply the same
        // zero-length-blob protection beat-link's Message.read() implements.
        // Without this, a CDJ that responds with argTag=3 (blob) preceded by
        // a 0-length argument would cause us to consume bytes from the next
        // argument instead, desynchronizing the stream.
        const uint8_t* tagBytes = static_cast<const uint8_t*>(tagsField.blobData.getData());
        int tagsAvailable = (int)tagsField.blobData.getSize();
        if (tagsAvailable > 12) tagsAvailable = 12;

        // Read arguments -- field type byte tells us whether it's int, string, or blob
        FieldResult lastArg;
        for (int i = 0; i < (int)msg.argCount && i < 12; i++)
        {
            // Beat-link Message.read() applies this safety check: when the
            // header says argument i is a binary blob (tag 0x03) but the
            // immediately preceding argument was a 4-byte int with value 0
            // (i.e. blob length = 0), don't try to read a zero-length blob
            // field from the wire -- some firmware paths emit nothing in
            // that position and the next byte already belongs to argument
            // i+1.  Reading would consume the next arg's type byte and
            // misalign the rest of the message.
            if (i > 0 && i < tagsAvailable && tagBytes[i] == 0x03
                && lastArg.ok && lastArg.type == 0x11
                && lastArg.numericValue == 0)
            {
                msg.numArgs[i] = 0;  // blobArgs[i] stays empty
                lastArg = FieldResult{};
                lastArg.type = 0x14;  // synthesised blob, length 0
                lastArg.ok = true;
                continue;
            }

            auto arg = readField(sock, timeoutMs);
            if (!arg.ok) return msg;
            msg.numArgs[i] = arg.numericValue;
            if (arg.type == 0x26)
                msg.strArgs[i] = arg.stringValue;
            if (arg.type == 0x14 && arg.blobData.getSize() > 0)
                msg.blobArgs[i] = std::move(arg.blobData);

            // The next-iteration safety check (zero-length blob detection)
            // only needs lastArg.type, lastArg.numericValue, and lastArg.ok.
            // Do NOT copy `arg` whole here: when the branch above moved
            // arg.blobData out, JUCE's MemoryBlock move-assignment leaves
            // arg.blobData with data=nullptr but size=originalSize, so a
            // subsequent copy-assignment of the MemoryBlock would call
            // memcpy(dst, nullptr, size) inside MemoryBlock::operator=,
            // crashing the whole dbserver worker thread.  Manifests when
            // reading any blob field (artwork JPEG, ANLZ waveform tags,
            // etc.) -- exactly the path that loads images in the PDL view.
            lastArg = FieldResult{};
            lastArg.type         = arg.type;
            lastArg.numericValue = arg.numericValue;
            lastArg.ok           = arg.ok;
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

    /// Read a dbserver response; if the read fails, close the connection so
    /// the next query opens a fresh socket instead of re-using a stale one.
    ///
    /// The common failure mode on NXS2 firmware (seen in packet captures)
    /// is that a read times out even though the TCP socket is still alive.
    /// Without this helper, a stream of failing queries would keep re-arming
    /// lastActivityTime via getConnection(), so the idle closer would never
    /// fire -- we would keep hammering a dbserver that has stopped responding.
    /// Force the close inline here instead.
    ///
    /// We use closeAbrupt() because writing a teardown to a socket where the
    /// firmware already failed to respond risks confusing it further -- the
    /// previous query bytes never got a reply, adding more bytes on top
    /// doesn't help.
    ///
    /// If expectedTxId is non-zero, the response txId is verified against it.
    /// This catches a firmware-state-corruption case where the CDJ returns an
    /// out-of-order response (answer to query N delivered when we expected
    /// answer to query N+1) -- accepting such a response would attach
    /// metadata to the wrong track.
    ///
    /// On a session opened for the current request, either failure also
    /// starts the player's reconnect cooldown, so the next request does not
    /// open a new session at once (AUDIT META-8).  On a session kept from an
    /// earlier request it does not: such a session can go stale between
    /// requests (the player closed it, or went mute meanwhile), and the
    /// request's phase 2 -- or the next request -- opens a fresh one at once.
    /// If that fresh one fails too, the cooldown starts.  So a failure with
    /// no cooldown costs one session that had answered before, and a player
    /// that does not answer still gets at most one session per cooldown.
    /// Worker thread.
    ResponseMessage readMessageOrInvalidate(PlayerConnection& conn, int timeoutMs,
                                            uint32_t expectedTxId = 0)
    {
        ResponseMessage msg;
        if (!conn.socket) return msg;  // already closed by a prior failure
        msg = readMessage(*conn.socket, timeoutMs);
        if (!msg.ok)
        {
            DBG("DbServerClient: read failed, closing connection to " + conn.playerIP);
            if (!conn.reused)
                noteFailure(conn.playerIP);
            conn.closeAbrupt();
            return msg;
        }
        if (expectedTxId != 0 && msg.txId != expectedTxId)
        {
            DBG("DbServerClient: unexpected txId 0x"
                + juce::String::toHexString((int)msg.txId)
                + " (expected 0x" + juce::String::toHexString((int)expectedTxId)
                + ") -- connection is out of sync, closing " + conn.playerIP);
            msg.ok = false;
            if (!conn.reused)
                noteFailure(conn.playerIP);
            conn.closeAbrupt();
        }
        return msg;
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
            {
                // If the query identity changed (e.g. NXS2 network topology
                // change caused suggestDbPlayerNumber to pick a different
                // player), the existing context is stale -- close and reconnect
                // so setupQueryContext uses the new identity.
                if (conn.contextPlayer != 0 && conn.contextPlayer != ourPlayer)
                {
                    DBG("DbServerClient: context player mismatch ("
                        + juce::String(conn.contextPlayer) + " vs "
                        + juce::String(ourPlayer) + ") -- reconnecting to " + playerIP);
                    conn.close();   // intentional close, not a failure
                    break;  // fall through to new connection below
                }
                conn.lastActivityTime = juce::Time::getMillisecondCounterHiRes();
                conn.reused = true;
                return &conn;
            }
        }

        // A player whose last attempt failed gets no new session until its
        // cooldown has passed.  Checked before a slot is chosen, so it never
        // closes another player's session for nothing (AUDIT META-8).
        double now = juce::Time::getMillisecondCounterHiRes();
        if (inFailureCooldown(playerIP, now))
            return nullptr;

        // Find empty slot or recycle the first one held by another player
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
                    slot = &conn;
                    break;
                }
            }
        }
        if (!slot || threadShouldExit()) return nullptr;

        // Step 1: Discover database port via port 12523 (cached after first success)
        int dbPort = 0;
        {
            const juce::ScopedLock sl(knownDbPortsLock);
            auto it = knownDbPorts.find(playerIP.toStdString());
            if (it != knownDbPorts.end()) dbPort = it->second;
        }
        if (dbPort <= 0)
        {
            dbPort = discoverDbPort(playerIP);
            if (dbPort <= 0)
            {
                DBG("DbServerClient: no valid db port found for " + playerIP);
                noteFailure(playerIP);
                return nullptr;
            }
            const juce::ScopedLock sl(knownDbPortsLock);
            knownDbPorts[playerIP.toStdString()] = dbPort;
        }

        if (threadShouldExit()) return nullptr;

        // Step 2: Connect to database port
        auto sock = std::make_unique<juce::StreamingSocket>();
        if (!sock->connect(playerIP, dbPort, kConnectTimeoutMs))
        {
            DBG("DbServerClient: TCP connect to " + playerIP + ":"
                + juce::String(dbPort) + " failed");
            noteFailure(playerIP);
            return nullptr;
        }

        // Disable Nagle's algorithm -- ensure each dbserver message is sent as
        // a single TCP segment. Some players fail to parse messages that arrive
        // split across multiple packets.
        {
            auto fd = sock->getRawSocketHandle();
            if (fd >= 0)
            {
                int flag = 1;
                setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                           reinterpret_cast<const char*>(&flag), sizeof(flag));
            }
        }

        // Step 3: Send initial handshake -- [0x11, 0x00000001]
        {
            uint8_t hello[] = { 0x11, 0x00, 0x00, 0x00, 0x01 };
            if (sock->write(hello, sizeof(hello)) != sizeof(hello))
            {
                DBG("DbServerClient: handshake write failed to " + playerIP);
                noteFailure(playerIP);
                return nullptr;
            }
            // Expect same 5 bytes back
            uint8_t reply[5];
            if (!readExact(*sock, reply, 5, kReadTimeoutMs))
            {
                DBG("DbServerClient: handshake reply timeout from " + playerIP);
                noteFailure(playerIP);
                return nullptr;
            }
            DBG("DbServerClient: handshake OK with " + playerIP + ":" + juce::String(dbPort));
        }

        slot->socket = std::move(sock);
        slot->playerIP = playerIP;
        slot->dbPort = dbPort;
        slot->contextSetUp = false;
        slot->lastActivityTime = now;
        slot->txId = 0;
        slot->reused = false;

        // Step 4: Setup query context
        if (!setupQueryContext(*slot, ourPlayer))
        {
            // Setup was rejected -- the CDJ won't accept our player identity.
            // Sending a teardown on a socket the CDJ just refused would only
            // add noise; close abruptly.
            noteFailure(playerIP);
            slot->closeAbrupt();
            return nullptr;
        }

        failedAtMs.erase(playerIP.toStdString());
        DBG("DbServerClient: connected to " + playerIP + ":" + juce::String(dbPort));
        return slot;
    }

    /// Worker thread.  Is this player inside its reconnect cooldown?
    bool inFailureCooldown(const juce::String& playerIP, double nowMs) const
    {
        auto it = failedAtMs.find(playerIP.toStdString());
        return it != failedAtMs.end() && nowMs - it->second < kReconnectCooldownMs;
    }

    /// Worker thread.  A connection attempt to this player, or a session
    /// opened for the current request, failed: start its cooldown.
    void noteFailure(const juce::String& playerIP)
    {
        if (playerIP.isNotEmpty())
            failedAtMs[playerIP.toStdString()] = juce::Time::getMillisecondCounterHiRes();
    }

    /// Worker thread.  req found no dbserver session.  If that is because
    /// its player is in the reconnect cooldown and the entry still has no
    /// title, remember req: requeueCooldownRetries() asks again once the
    /// cooldown has passed.  Nothing else would while the PDL View is
    /// closed -- TimecodeEngine asks once per track change; the PDL View,
    /// while open, asks again about every 3 s while a deck still lacks its
    /// metadata -- so without this a title (and the Track Map offset that
    /// depends on it) missed during a cooldown stayed missing (AUDIT
    /// META-8).  A request already remembered keeps its count.
    void retryAfterCooldown(const MetadataRequest& req, const CacheKey& cacheKey)
    {
        if (!inFailureCooldown(req.playerIP, juce::Time::getMillisecondCounterHiRes()))
            return;
        {
            const juce::SpinLock::ScopedLockType lock(cacheLock);
            auto it = metadataCache.find(cacheKey);
            if (it != metadataCache.end() && it->second.isValid())
                return;   // the title is known; the NFS route does the rest
        }
        for (auto& r : cooldownRetries)
            if (r.req.playerIP == req.playerIP && r.req.slot == req.slot
                && r.req.trackId == req.trackId)
                return;
        if ((int)cooldownRetries.size() >= kMaxCooldownRetryRequests)
            cooldownRetries.erase(cooldownRetries.begin());   // drop the oldest
        CooldownRetry r;
        r.req = req;
        r.retriesLeft = kCooldownRetries;
        cooldownRetries.push_back(std::move(r));
    }

    /// Worker thread, before the queue is drained.  Each remembered request
    /// whose player is out of its cooldown is queued again as a new phase-1
    /// request (requestMetadata), at most kCooldownRetries times; it is
    /// forgotten once its entry has a title or it has no retries left.  A
    /// retry that fails starts the cooldown again, so a player that does not
    /// answer gets at most one session per cooldown (AUDIT META-8).
    void requeueCooldownRetries()
    {
        const double now = juce::Time::getMillisecondCounterHiRes();
        for (size_t i = 0; i < cooldownRetries.size(); )
        {
            auto& r = cooldownRetries[i];
            if (inFailureCooldown(r.req.playerIP, now))
            {
                ++i;
                continue;
            }
            bool hasTitle = false;
            {
                const juce::SpinLock::ScopedLockType lock(cacheLock);
                auto it = metadataCache.find(makeCacheKey(r.req.playerIP, r.req.slot, r.req.trackId));
                hasTitle = it != metadataCache.end() && it->second.isValid();
            }
            if (hasTitle || r.retriesLeft <= 0)
            {
                cooldownRetries.erase(cooldownRetries.begin() + (std::ptrdiff_t)i);
                continue;
            }
            --r.retriesLeft;
            DBG("DbServerClient: asking " + r.req.playerIP + " again for trackId="
                + juce::String(r.req.trackId) + " after its cooldown");
            requestMetadata(r.req.playerIP, r.req.slot, r.req.trackType, r.req.trackId,
                            r.req.ourPlayer, r.req.playerModel);
            ++i;
        }
    }

    /// Discover the dbserver port by querying TCP 12523.
    /// Retries a few times because the player may not be ready to respond
    /// immediately after booting or loading media.
    int discoverDbPort(const juce::String& playerIP)
    {
        static constexpr int kMaxRetries = 3;
        static constexpr int kRetryDelayMs = 1000;

        for (int attempt = 0; attempt < kMaxRetries; ++attempt)
        {
            if (attempt > 0)
            {
                DBG("DbServerClient: port discovery retry " + juce::String(attempt)
                    + "/" + juce::String(kMaxRetries - 1) + " for " + playerIP);
                wait(kRetryDelayMs);   // stopThread() ends the wait (AUDIT META-14)
            }
            if (threadShouldExit() || !isRunningFlag.load(std::memory_order_relaxed))
                return 0;

            int port = discoverDbPortOnce(playerIP);
            if (port > 0)
                return port;
        }

        DBG("DbServerClient: port discovery failed after " + juce::String(kMaxRetries)
            + " attempts for " + playerIP);
        return 0;
    }

    /// Single attempt at port discovery via TCP 12523
    int discoverDbPortOnce(const juce::String& playerIP)
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

        if (port <= 0 || port >= 65535)
        {
            DBG("DbServerClient: invalid port " + juce::String(port)
                + " (0xFFFF: no database offered, \"not yet ready?\" in beat-link)");
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

        auto resp = readMessageOrInvalidate(conn, kReadTimeoutMs, 0xFFFFFFFE);
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
        conn.contextPlayer = ourPlayer;
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
        auto resp = readMessageOrInvalidate(conn, kReadTimeoutMs, currentTxId);
        if (!resp.ok || resp.type != 0x4000)
        {
            DBG("DbServerClient: metadata request failed for trackId=" + juce::String(trackId)
                + " (resp type=0x" + juce::String::toHexString(resp.type) + ")");
            return meta;
        }

        int itemCount = (int)resp.numArgs[1];
        if (itemCount <= 0 || resp.numArgs[1] == 0xFFFFFFFF)
            return meta;  // track not found

        // Defensive cap on item count. For single-track metadata the count
        // is typically 14-15 so this rarely applies, but if the firmware
        // ever reports a bogus count we avoid a huge read loop that could
        // push the CDJ into an odd state. 64 is a safe upper bound for
        // NXS2 based on protocol behaviour.
        static constexpr int kMenuBatchSize = 64;
        if (itemCount > kMenuBatchSize)
        {
            DBG("DbServerClient: metadata item count " + juce::String(itemCount)
                + " exceeds batch size, capping at " + juce::String(kMenuBatchSize));
            itemCount = kMenuBatchSize;
        }

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
        auto header = readMessageOrInvalidate(conn, kReadTimeoutMs, renderTxId);
        if (!header.ok || header.type != 0x4001)
            return meta;

        // Read menu items (type 0x4101 each)
        for (int i = 0; i < itemCount; i++)
        {
            auto item = readMessageOrInvalidate(conn, kReadTimeoutMs);
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
                case 0x000E:  // Label (record label; beat-link MenuItemType LABEL).
                    // Not kept.  It is not the ANLZ path (AUDIT META-1): that
                    // comes only from export.pdb, on the NFS route.
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
        auto footer = readMessageOrInvalidate(conn, kReadTimeoutMs);
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
        auto resp = readMessageOrInvalidate(conn, kReadTimeoutMs, currentTxId);
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
        // CDJ-3000, CDJ-3000X, or any future model with "3000" in the name
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

        // The first attempt may have closed the socket via closeAbrupt() -- e.g.
        // the CDJ returned an out-of-order response (NXS2 firmware sometimes
        // delivers a stale 0x4e02 reply when we asked for 0x4f02), or any read
        // failure.  Verify the connection survived before dispatching the
        // fallback; otherwise queryOneWaveformFormat would dereference a null
        // socket.
        if (!conn.isConnected())
            return {};

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
        // Defensive: a previous query in the same processRequest() pass may
        // have invalidated this connection (closeAbrupt() on read failure or
        // txId mismatch).  Without this check, conn.socket->write() below
        // would dereference null.
        if (!conn.isConnected())
            return {};

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

        auto resp = readMessageOrInvalidate(conn, kReadTimeoutMs, txId);
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
        const size_t size = blob.getSize();
        const size_t wordSize = (size_t) expectedWordSize;

        if (size < 20)
        {
            DBG("DbServerClient: ANLZ blob too small (" + juce::String((int) size) + " bytes)");
            return {};
        }

        // Search for tag signature (e.g. "PWV6" or "PWV4")
        for (size_t i = 0; i + 20 <= size; ++i)
        {
            if (d[i] == tagName[0] && d[i+1] == tagName[1]
                && d[i+2] == tagName[2] && d[i+3] == tagName[3])
            {
                // Tag header: tag(4) + len_header(4) + len_tag(4), then
                // len_entry_bytes(4) + len_entries(4) (crate-digger rekordbox_anlz.ksy)
                const uint32_t lenHeader  = readBE32(d + i + 4);
                const uint32_t wordSizeW  = readBE32(d + i + 12);
                const uint32_t entryCount = readBE32(d + i + 16);

                if (wordSizeW != wordSize) continue;

                // Entries start at len_header: 0x14 for PWV6 (nothing after the
                // count), 0x18 for PWV4 (one more u4) -- crate-digger anlz.adoc.
                // If len_header does not point inside the blob, assume that layout.
                const size_t avail = size - i;
                size_t entriesRel = lenHeader;
                if (lenHeader == 0 || lenHeader >= avail)
                    entriesRel = (wordSize == 6) ? 24 : 20;
                if (entriesRel > avail || entryCount > (avail - entriesRel) / wordSize)
                {
                    DBG("DbServerClient: " + juce::String(tagName) + " data overflows blob ("
                        + juce::String((juce::int64) entryCount) + " entries, "
                        + juce::String((int) size) + " bytes)");
                    continue;
                }

                const size_t entriesOff = i + entriesRel;
                const size_t dataLen = (size_t) entryCount * wordSize;
                WaveformResult result;
                result.data.assign(d + entriesOff, d + entriesOff + dataLen);
                result.entryCount = (int)entryCount;
                DBG("DbServerClient: parsed " + juce::String(tagName) + " -- "
                    + juce::String(entryCount) + " entries x " + juce::String((int) wordSize)
                    + " bytes = " + juce::String((int) dataLen) + " bytes");
                return result;
            }
        }

        // Fallback: try treating blob as raw data
        if (size >= wordSize * 10 && size % wordSize == 0)
        {
            int entryCount = (int) (size / wordSize);
            WaveformResult result;
            result.data.assign(d, d + size);
            result.entryCount = entryCount;
            DBG("DbServerClient: using raw blob as " + juce::String(tagName)
                + " -- " + juce::String(entryCount) + " entries");
            return result;
        }

        DBG("DbServerClient: " + juce::String(tagName) + " tag not found in "
            + juce::String((int) size) + " byte blob");
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

    //==========================================================================
    // BEAT GRID QUERY (PQTZ tag, asked for in the .EXT via 0x2c04)
    //==========================================================================

    /// Query beat grid from CDJ. Returns vector of BeatEntry.
    ///
    /// crate-digger's anlz.adoc puts PWV3-PWV5 in the .EXT and PWV6 in the
    /// .2EX; its rekordbox_anlz.ksy notes PCO2, PSSI and PWV3-PWV5 as seen in
    /// the .EXT and PWV6/PWV7 in the .2EX (section_tags).  PQTZ has no such
    /// note, and beat-link reads the grid from the .DAT
    /// (CrateDigger.getBeatGrid).  This asks for it in the .EXT, so a player
    /// may well answer "not found"; beat-link asks the dbserver with its own
    /// request instead (BEAT_GRID_REQ, 0x2204).  Left as it is: changing it
    /// changes what STC asks a player for, and needs a capture (DESIGN D2;
    /// AUDIT META-13, deferred).  The NFS route reads the grid from the .DAT.
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

        auto resp = readMessageOrInvalidate(conn, kReadTimeoutMs, txId);
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
        const size_t size = blob.getSize();

        for (size_t i = 0; i + 24 <= size; ++i)
        {
            if (d[i] == 'P' && d[i+1] == 'Q' && d[i+2] == 'T' && d[i+3] == 'Z')
            {
                uint32_t lenHeader = readBE32(d + i + 4);
                if (lenHeader < 24 || lenHeader > size - i) continue;

                uint32_t numBeats = readBE32(d + i + 20);
                if (numBeats == 0 || numBeats > kMaxBeats) continue;

                const size_t entriesOff = i + lenHeader;
                if (numBeats > (size - entriesOff) / 8)
                {
                    DBG("DbServerClient: PQTZ data overflows blob");
                    continue;
                }

                std::vector<TrackMetadata::BeatEntry> grid;
                grid.reserve(numBeats);
                for (uint32_t b = 0; b < numBeats; ++b)
                {
                    const uint8_t* e = d + entriesOff + (size_t) b * 8;
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

        DBG("DbServerClient: PQTZ tag not found in " + juce::String((int) size) + " byte blob");
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

        // The first attempt may have closed the socket via closeAbrupt() -- e.g.
        // the CDJ returned an out-of-order response (NXS2 firmware sometimes
        // delivers a stale 0x4e02 reply when we asked for 0x4f02), or any read
        // failure.  Verify the connection survived before dispatching the
        // fallback; otherwise queryOneDetailFormat would dereference a null
        // socket.
        if (!conn.isConnected())
            return {};

        // Fallback to other format
        DBG("DbServerClient: primary detail format failed, trying fallback");
        return queryOneDetailFormat(conn, slot, trackType, trackId,
                                     ourPlayer, !threeBandFirst);
    }

    DetailWaveformResult queryOneDetailFormat(
        PlayerConnection& conn, uint8_t slot, uint8_t trackType,
        uint32_t trackId, uint8_t ourPlayer, bool threeBand)
    {
        // Defensive: a previous query in the same processRequest() pass may
        // have invalidated this connection (closeAbrupt() on read failure or
        // txId mismatch).  Without this check, conn.socket->write() below
        // would dereference null.
        if (!conn.isConnected())
            return {};

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

        auto resp = readMessageOrInvalidate(conn, kReadTimeoutMs, txId);
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
        const size_t size = blob.getSize();

        for (size_t i = 0; i + 20 <= size; ++i)
        {
            if (d[i] == tagName[0] && d[i+1] == tagName[1]
                && d[i+2] == tagName[2] && d[i+3] == tagName[3])
            {
                uint32_t lenHeader = readBE32(d + i + 4);

                uint32_t wordSize   = readBE32(d + i + 12);
                uint32_t entryCount = readBE32(d + i + 16);

                if (wordSize != (uint32_t) expectedWordSize) continue;
                if (entryCount == 0 || entryCount > kMaxDetailEntries) continue;

                // Entries at len_header; if it does not point inside the blob,
                // right after the count.
                const size_t avail = size - i;
                const size_t entriesRel = (lenHeader == 0 || lenHeader >= avail) ? 20 : lenHeader;

                if (entryCount > (avail - entriesRel) / wordSize)
                {
                    DBG("DbServerClient: " + juce::String(tagName)
                        + " detail data overflows blob");
                    continue;
                }
                const size_t entriesOff = i + entriesRel;
                const size_t dataLen = (size_t) wordSize * entryCount;

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
            + juce::String((int) size) + " byte blob");
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

        auto resp = readMessageOrInvalidate(conn, kReadTimeoutMs, txId);
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
        const size_t size = blob.getSize();

        for (size_t i = 0; i + 20 <= size; ++i)
        {
            if (d[i] != 'P' || d[i+1] != 'S' || d[i+2] != 'S' || d[i+3] != 'I')
                continue;

            uint32_t lenHeader = readBE32(d + i + 4);
            if (lenHeader < 20) continue;

            uint32_t entrySize  = readBE32(d + i + 12);  // u4, NOT u2
            uint16_t numEntries = readBE16(d + i + 16);
            if (entrySize != 24 || numEntries == 0 || numEntries > kMaxPhrases) continue;

            // Masked body starts at i + 18 and runs to the end of the tag
            // (len_tag, rekordbox_anlz.ksy: size-eos), or to the end of the
            // blob when len_tag does not fit in it.
            const size_t bodyOff = i + 18;
            const uint32_t lenTag = readBE32(d + i + 8);
            const size_t bodyLen = (lenTag >= 18 + 2 && lenTag <= size - i) ? (size_t) lenTag - 18
                                                                         : size - bodyOff;
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

        DBG("DbServerClient: PSSI tag not found in " + juce::String((int) size) + " byte blob");
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
                auto resp = readMessageOrInvalidate(conn, kReadTimeoutMs, txId);
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
                    {
                        DBG("DbServerClient: PCO2 extractBlob failed");
                    }
                }
                else
                {
                    DBG("DbServerClient: PCO2 response -- ok=" + juce::String((int)resp.ok)
                        + " type=0x" + juce::String::toHexString((int)resp.type)
                        + " args=" + juce::String(resp.argCount)
                        + " numArgs[2]=" + juce::String(resp.argCount >= 3 ? (int)resp.numArgs[2] : -1));
                }
            }
        }

        // Fallback: standard (PCOB) -- no colors/comments but has positions.
        // The .EXT can hold PCOB too: beat-link reads basic cues from the
        // .EXT when there is one ("it can contain both nxs2-style commented
        // cues and basic cues", CrateDigger.getCueList).
        if (!conn.isConnected()) return {};
        {
            uint32_t dmst = makeDMST(ourPlayer, 0x01, slot, trackType);
            uint32_t txId = conn.txId++;
            auto reqMsg = buildMessage(txId, kNxs2ExtRequest,
                                        { dmst, trackId, kMagicPCOB, kMagicTXE });
            if (conn.socket->write(reqMsg.getData(), (int)reqMsg.getSize()) == (int)reqMsg.getSize())
            {
                auto resp = readMessageOrInvalidate(conn, kReadTimeoutMs, txId);
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
                    {
                        DBG("DbServerClient: PCOB extractBlob failed");
                    }
                }
                else
                {
                    DBG("DbServerClient: PCOB response -- ok=" + juce::String((int)resp.ok)
                        + " type=0x" + juce::String::toHexString((int)resp.type)
                        + " args=" + juce::String(resp.argCount)
                        + " numArgs[2]=" + juce::String(resp.argCount >= 3 ? (int)resp.numArgs[2] : -1));
                }
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
    ///     [0x18] loop_time (u4be, ms; used only when type is 2)
    ///     [0x1C] color_id (u1)
    ///     [0x24] loop_numerator (u2), [0x26] loop_denominator (u2)
    ///     [0x28] len_comment (u4, byte count of UTF-16BE string)
    ///     [0x2C] comment (UTF-16BE, len_comment bytes)
    ///     [0x2C + len_comment] color_code (u1)
    ///     [0x2C + len_comment + 1] color_r, +2 color_g, +3 color_b
    static std::vector<TrackMetadata::RekordboxCue> parseCueListExtended(const juce::MemoryBlock& blob)
    {
        const uint8_t* d = static_cast<const uint8_t*>(blob.getData());
        const size_t size = blob.getSize();
        std::vector<TrackMetadata::RekordboxCue> result;

        // Search for all PCO2 sections (one for memory points, one for hot cues)
        for (size_t i = 0; i + 20 <= size; ++i)
        {
            if (d[i] != 'P' || d[i+1] != 'C' || d[i+2] != 'O' || d[i+3] != '2')
                continue;

            uint32_t lenHeader = readBE32(d + i + 4);
            // numCues at offset 16 from tag start (body[4-5] after 12-byte section header + 4-byte type)
            uint16_t numCues   = readBE16(d + i + 16);
            if (numCues == 0 || numCues > kMaxCuesPerList) continue;
            if (lenHeader > size - i) continue;   // entries would start past the blob

            DBG("DbServerClient: PCO2 section found at " + juce::String((int) i)
                + " lenHeader=" + juce::String(lenHeader)
                + " numCues=" + juce::String(numCues));

            size_t entryOff = i + lenHeader;

            for (uint16_t c = 0; c < numCues; ++c)
            {
                // Find next PCP2 tag
                if (entryOff + 12 > size) break;
                if (d[entryOff] != 'P' || d[entryOff+1] != 'C'
                    || d[entryOff+2] != 'P' || d[entryOff+3] != '2')
                {
                    bool found = false;
                    for (size_t scan = entryOff; scan + 12 <= size && scan < entryOff + 200; ++scan)
                    {
                        if (d[scan] == 'P' && d[scan+1] == 'C' && d[scan+2] == 'P' && d[scan+3] == '2')
                            { entryOff = scan; found = true; break; }
                    }
                    if (!found) break;
                }

                uint32_t entryLen = readBE32(d + entryOff + 8);  // total PCP2 entry size
                if (entryLen > kMaxPcp2EntryBytes || entryLen < 0x1D
                    || entryLen > size - entryOff)
                {
                    // Skip it by its own length when that is plausible, else by
                    // the 12-byte header; a skip past the blob ends the list.
                    entryOff += (entryLen >= 12 && entryLen <= size - entryOff) ? entryLen : 12;
                    continue;
                }

                const uint8_t* e = d + entryOff;  // points to "PCP2" magic
                TrackMetadata::RekordboxCue cue;

                uint32_t hotCue = readBE32(e + 0x0C);  // hot cue number
                uint8_t  ctype  = e[0x10];               // 1=cue, 2=loop
                uint32_t timeMs = readBE32(e + 0x14);    // position ms
                uint32_t loopMs = readBE32(e + 0x18);    // loop end ms

                if (ctype == 0) { entryOff += entryLen; continue; }

                cue.hotCueNumber = (uint8_t)hotCue;
                cue.positionMs   = timeMs;

                // A loop is type 2 (crate-digger anlz.adoc, beat-link CueList);
                // loop_time is meaningful only then (AUDIT DEBT-7).
                if (ctype == 2)
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
                    if (commentBytes > 0 && commentBytes < kMaxCueCommentBytes
                        && commentBytes <= entryLen - 0x2C)
                        cue.comment = decodeUtf16BE(e + 0x2C, commentBytes / 2);
                }

                // Color RGB (after comment), only when the comment length leaves
                // room for it inside the entry
                if ((uint64_t) commentBytes + 0x2C + 4 <= entryLen)
                {
                    const size_t colorOff = 0x2C + (size_t) commentBytes;
                    cue.colorCode = e[colorOff];
                    cue.colorR    = e[colorOff + 1];
                    cue.colorG    = e[colorOff + 2];
                    cue.colorB    = e[colorOff + 3];
                    cue.hasColor  = (cue.colorR != 0 || cue.colorG != 0 || cue.colorB != 0);
                }

                result.push_back(cue);
                entryOff += entryLen;
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
    ///   [0x24-0x27] loop_time (u4be, ms; used only when type is 2)
    static std::vector<TrackMetadata::RekordboxCue> parseCueListStandard(const juce::MemoryBlock& blob)
    {
        const uint8_t* d = static_cast<const uint8_t*>(blob.getData());
        const size_t size = blob.getSize();
        std::vector<TrackMetadata::RekordboxCue> result;

        for (size_t i = 0; i + 20 <= size; ++i)
        {
            if (d[i] != 'P' || d[i+1] != 'C' || d[i+2] != 'O' || d[i+3] != 'B')
                continue;

            uint32_t lenHeader = readBE32(d + i + 4);
            uint16_t numCues   = readBE16(d + i + 18);
            if (numCues == 0 || numCues > kMaxCuesPerList) continue;
            if (lenHeader > size - i) continue;   // entries would start past the blob

            size_t entryOff = i + lenHeader;
            static constexpr size_t kPcptSize = 0x38;

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

                // A loop is type 2 only (crate-digger anlz.adoc, beat-link
                // CueList; AUDIT DEBT-7).  Testing loop_time > 0 made a cue
                // point whose loop_time is 0xFFFFFFFF a loop ending at 49 days.
                if (ctype == 2)
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

    /// Worker thread.  Store what queryTrackMetadata found for key.  An
    /// entry already there has no title -- ensureEntry made it when the
    /// dbserver gave none -- but may hold what the NFS route or the disk
    /// cache found since: its beat grid, cues, phrases, waveforms and
    /// nfsAttempted are kept, and the dbserver's fields replace the rest,
    /// waveformQueried included (artwork and the preview are still to be
    /// fetched) (AUDIT META-7).  Replacing the whole entry dropped that data
    /// until a second NFS download brought it back.
    void cacheMetadata(const CacheKey& key, const TrackMetadata& meta)
    {
        if (!meta.isValid()) return;

        const juce::SpinLock::ScopedLockType lock(cacheLock);
        auto it = metadataCache.find(key);
        if (it == metadataCache.end())
        {
            evictOldestIfFullLocked();
            metadataCache[key] = meta;
            return;
        }
        TrackMetadata merged = meta;
        auto& old = it->second;
        merged.waveformData          = std::move(old.waveformData);
        merged.waveformEntryCount    = old.waveformEntryCount;
        merged.waveformBytesPerEntry = old.waveformBytesPerEntry;
        merged.detailData            = std::move(old.detailData);
        merged.detailEntryCount      = old.detailEntryCount;
        merged.detailBytesPerEntry   = old.detailBytesPerEntry;
        merged.beatGrid              = std::move(old.beatGrid);
        merged.phraseMood            = old.phraseMood;
        merged.songStructure         = std::move(old.songStructure);
        merged.cueList               = std::move(old.cueList);
        merged.nfsAttempted          = old.nfsAttempted;
        merged.cacheVersion          = old.cacheVersion + 1;
        old = std::move(merged);
    }

    /// Make sure an entry exists for key -- one with no metadata when the
    /// dbserver gave none -- so the NFS route has somewhere to put what it
    /// finds.  An existing entry is left as it is.
    void ensureEntry(const CacheKey& key)
    {
        const juce::SpinLock::ScopedLockType lock(cacheLock);
        if (metadataCache.find(key) != metadataCache.end())
            return;
        evictOldestIfFullLocked();
        TrackMetadata meta;
        meta.trackId = key.id;
        meta.cacheTime = juce::Time::getMillisecondCounterHiRes();
        metadataCache[key] = std::move(meta);
    }

    /// Caller holds cacheLock and is about to add a key: if the cache is
    /// full, evict the entry fetched longest ago (smallest cacheTime).
    void evictOldestIfFullLocked()
    {
        if ((int)metadataCache.size() < kMaxCacheEntries)
            return;
        auto oldest = metadataCache.end();
        for (auto it = metadataCache.begin(); it != metadataCache.end(); ++it)
            if (oldest == metadataCache.end() || it->second.cacheTime < oldest->second.cacheTime)
                oldest = it;
        if (oldest != metadataCache.end())
            metadataCache.erase(oldest);
    }

    void cacheArtwork(const CacheKey& key, const juce::Image& img)
    {
        if (key.id == 0 || !img.isValid()) return;

        const juce::SpinLock::ScopedLockType lock(artCacheLock);

        if ((int)artworkCache.size() >= kMaxArtCacheEntries
            && artworkCache.find(key) == artworkCache.end())
        {
            // Simple eviction: remove the first entry in hash order (arbitrary)
            artworkCache.erase(artworkCache.begin());
        }

        artworkCache[key] = img;
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

            // Process pending invalidations — close connections to addresses
            // ProDJLink reported lost or moved away from (forgetAddress). Safe
            // to do here because we're between TCP queries on this thread.
            {
                juce::StringArray ips;
                {
                    const juce::ScopedLock sl(pendingInvalidateLock);
                    ips.swapWith(pendingInvalidateIPs);
                }
                for (auto& ip : ips)
                {
                    for (auto& conn : connections)
                    {
                        if (conn.playerIP == ip)
                        {
                            DBG("DbServerClient: closing connection to " + ip + " (player lost or moved)");
                            // Player is off the network -- teardown has nowhere
                            // to go and waitUntilReady would block for 200ms
                            // waiting for a close signal that will never arrive.
                            conn.closeAbrupt();
                        }
                    }
                    failedAtMs.erase(ip.toStdString());   // a player that returns starts afresh
                    cooldownRetries.erase(std::remove_if(cooldownRetries.begin(), cooldownRetries.end(),
                                                         [&ip](const CooldownRetry& r) { return r.req.playerIP == ip; }),
                                          cooldownRetries.end());
                    nfsLostPlayers.addIfNotAlreadyThere(ip);
                }
            }

            // The NFS fetcher's state for lost players (mount handles, ports,
            // ANLZ paths from the PDB) is dropped once no download is running.
            if (!nfsLostPlayers.isEmpty() && nfsIdle())
                forgetLostPlayersInNfs();

            // Close idle connections after kIdleTimeoutMs.  See the comment
            // on kIdleTimeoutMs above for the rationale -- the previous 1 s
            // value caused excessive TCP churn against NXS2 dbservers.
            {
                double now = juce::Time::getMillisecondCounterHiRes();
                for (auto& conn : connections)
                {
                    if (conn.isConnected() && conn.lastActivityTime > 0.0
                        && (now - conn.lastActivityTime) > kIdleTimeoutMs)
                    {
                        DBG("DbServerClient: closing idle connection to "
                            + conn.playerIP + ":" + juce::String(conn.dbPort)
                            + " (idle " + juce::String((int)((now - conn.lastActivityTime) / 1000.0)) + "s)");
                        conn.close();
                    }
                }
            }

            if (threadShouldExit()) break;

            requeueCooldownRetries();

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
        auto* conn = getConnection(req.playerIP, req.ourPlayer);
        if (!conn)
        {
            // No dbserver session (refused, silent, or in its failure
            // cooldown).  The NFS route needs none -- it is stateless and
            // works whatever player number STC asks as -- so it runs now, in
            // phase 1 as in phase 2 (AUDIT META-7).  A title still missing
            // is asked for again once the cooldown has passed
            // (retryAfterCooldown, AUDIT META-8).
            errorCount.fetch_add(1, std::memory_order_relaxed);
            DBG("DbServerClient: no dbserver connection to " + req.playerIP
                + " (phase " + juce::String(req.phase) + ") -- trying NFS only");
            if (req.trackId != 0)
            {
                const CacheKey cacheKey = makeCacheKey(req.playerIP, req.slot, req.trackId);
                processNfsFallback(req, cacheKey);
                retryAfterCooldown(req, cacheKey);
            }
            return;
        }

        if (req.trackId != 0)
        {
            // Check if metadata text is already cached (partial cache --
            // metadata succeeded on a previous request but waveform or
            // artwork may still be missing).
            const CacheKey cacheKey = makeCacheKey(req.playerIP, req.slot, req.trackId);
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
                    cacheMetadata(cacheKey, meta);
                    DBG("DbServerClient: cached metadata for track "
                        + juce::String(req.trackId) + " -- \""
                        + meta.artist + " - " + meta.title + "\"");
                }
                else
                {
                    errorCount.fetch_add(1, std::memory_order_relaxed);
                    DBG("DbServerClient: metadata query failed for trackId="
                        + juce::String(req.trackId));
                    if (!conn->isConnected())
                        conn->close();

                    // Keep an entry with no metadata so that phase 2's NFS
                    // route has one to fill (it finds the ANLZ path in the
                    // PDB from the track ID).  cacheMetadata() keeps only
                    // entries with a title, so it cannot make this one.
                    ensureEntry(cacheKey);
                }
            }

            // Artwork and waveform queries -- only if we have valid metadata
            // (skip when dbserver connection failed to avoid hanging on dead socket)
            if (meta.isValid())
            {
                // Artwork: fetch if not yet cached
                if (req.wantArt && meta.artworkId != 0)
                {
                    const CacheKey artKey = makeCacheKey(req.playerIP, req.slot, meta.artworkId);
                    bool artCached;
                    {
                        const juce::SpinLock::ScopedLockType lock(artCacheLock);
                        artCached = artworkCache.count(artKey) > 0;
                    }
                    if (!artCached)
                    {
                        auto img = queryArtwork(*conn, req.slot, req.trackType,
                                                 meta.artworkId, req.ourPlayer);
                        if (img.isValid())
                            cacheArtwork(artKey, img);
                    }
                }

                // Waveform: fetch if not yet cached.
                //
                // For non-CDJ-3000 hardware the dbserver waveform path has
                // shown the following symptoms in field captures (issues #6
                // and #9):
                //   - In Joren2087's player1_link_error capture STC opened
                //     72 dbserver TCP sessions to the USB-holder NXS2 across
                //     ~120 s for the same loaded track, all of which the
                //     player accepted but none of which produced a usable
                //     waveform on the CDJ side (the user reported the CDJ
                //     hung with "metadata only, no waveform / no audio").
                //   - The same captures show zero NFS traffic from STC,
                //     because the existing NFS Fallback only runs when one
                //     of the has*() flags is false -- and the dbserver
                //     waveform query was filling at least one of them with
                //     something that was not actually usable downstream.
                // Beat Link's CrateDigger documents the same NXS2 dbserver
                // unreliability and is the de-facto reference for getting
                // around it ("the NFSv2 server is stateless, does not care
                // what player number we are using, and can be used no matter
                // how many players are on the network").  v1.9.10 therefore
                // skips the dbserver preview-waveform query for non-3000
                // hardware and lets the NFS pipeline (further below) fetch
                // the analysis files directly; for CDJ-3000-class hardware
                // the dbserver path is left exactly as it shipped in v1.9.9.
                const bool isCdj3000Class = req.playerModel.containsIgnoreCase("3000");
                if (req.wantWaveform && !meta.hasWaveform() && isCdj3000Class)
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
                // (.anlz) and, for an entry with none, the colour preview
                // (.wfc) from previous session BEFORE re-enqueueing phase 2.
                //
                // The disk key is artist|title|duration (TrackMapEntry::makeKey),
                // so two versions of a track that share all three share one
                // file, and once the files hold all four kinds of data and the
                // preview, phase 2 never runs NFS for the track: a later
                // rekordbox edit of its grid or cues is not picked up (AUDIT
                // META-9; when to refresh is not decided yet).  The .wfc is
                // what spares a player whose preview only NFS gives (any but a
                // CDJ-3000) an NFS download per track in every session (AUDIT
                // META-15).  Nothing stable identifies the audio file here --
                // track IDs are per export -- so the key stays.
                std::string diskKey;
                {
                    const juce::SpinLock::ScopedLockType lock(cacheLock);
                    auto it = metadataCache.find(cacheKey);
                    if (it != metadataCache.end()
                        && it->second.title.isNotEmpty())
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
                bool wantPreview = false;
                if (!diskKey.empty())
                {
                    const juce::SpinLock::ScopedLockType lock(cacheLock);
                    auto it = metadataCache.find(cacheKey);
                    wantPreview = it != metadataCache.end() && !it->second.hasWaveform();
                }
                if (wantPreview && WaveformCache::exists(diskKey))
                {
                    auto cached = WaveformCache::load(diskKey);
                    if (cached.valid)
                    {
                        const juce::SpinLock::ScopedLockType lock(cacheLock);
                        auto it = metadataCache.find(cacheKey);
                        if (it != metadataCache.end() && !it->second.hasWaveform())
                        {
                            it->second.waveformData          = std::move(cached.data);
                            it->second.waveformEntryCount    = cached.entryCount;
                            it->second.waveformBytesPerEntry = cached.bytesPerEntry;
                            ++it->second.cacheVersion;
                        }
                    }
                }

                // Re-enqueue as phase 2 for whatever is still missing: the
                // dbserver analysis queries (CDJ-3000 class) and the NFS route
                enqueueInternal(req.playerIP, req.playerModel, req.slot,
                                req.trackType, req.trackId, req.ourPlayer, 2);
                return;
            }

            // --- Phase 2: supplementary queries + NFS ---
            // This runs AFTER phase 1 has published. If a newer track request
            // arrived while we were waiting in queue, skip slow dbserver queries
            // and go straight to the disk save and the NFS route (which runs
            // only for data still missing, not as a refresh).

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

            // Phase-2 dbserver waveform / analysis queries.  Same reasoning
            // as the preview-waveform skip in phase 1 above: for non-3000
            // hardware we gate every dbserver-based query below on
            // phase2Cdj3000Class so that NFS gets a clean shot at the
            // analysis files (PQTZ / PCO2 / PCOB / PSSI / PWV4 / PWV5)
            // without our previous query having already filled the cache
            // with a partial / wrong-format dbserver answer that would
            // suppress the NFS fallback further down.  For CDJ-3000 every
            // query keeps running exactly as in v1.9.9.
            const bool phase2Cdj3000Class = req.playerModel.containsIgnoreCase("3000");

            // Beat grid: fetch if not yet cached
            if (req.wantWaveform && !meta.hasBeatGrid() && conn->isConnected() && phase2Cdj3000Class)
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
            if (req.wantWaveform && !meta.hasDetailWaveform() && conn->isConnected() && phase2Cdj3000Class)
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
                    + " connected=" + juce::String((int)conn->isConnected())
                    + " cdj3000=" + juce::String((int)phase2Cdj3000Class));
            }

            // Song structure (phrase analysis): fetch if not yet cached
            if (req.wantWaveform && !meta.hasSongStructure() && conn->isConnected() && phase2Cdj3000Class)
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
            if (req.wantWaveform && !meta.hasCueList() && conn->isConnected() && phase2Cdj3000Class)
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
            // Even if dbserver queries were skipped, save whatever we have
            // now (disk cache + dbserver).  The NFS thread saves its own
            // results when it finishes.
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
                        if (hasNewData && it->second.title.isNotEmpty())
                            saveDiskKey = TrackMapEntry::makeKey(
                                it->second.artist, it->second.title, it->second.durationSeconds);
                    }
                }
                if (!saveDiskKey.empty() && hasNewData)
                    saveAnlzToDisk(cacheKey, saveDiskKey);
            }

            // --- NFS ANLZ Fallback ---
            // If dbserver queries AND disk cache both failed to provide beat grid,
            // cues, song structure, detail or the colour preview (whose disk
            // cache is the .wfc phase 1 loads; AUDIT META-15), download via NFS
            // from CDJ USB/SD (once per entry: nfsAttempted).
            {
                bool needsNfs = false;
                uint32_t trackIdForNfs = 0;
                {
                    const juce::SpinLock::ScopedLockType lock(cacheLock);
                    auto it = metadataCache.find(cacheKey);
                    if (it != metadataCache.end()
                        && !it->second.nfsAttempted
                        && (!it->second.hasBeatGrid() || !it->second.hasCueList()
                            || !it->second.hasSongStructure()
                            || !it->second.hasDetailWaveform()
                            || !it->second.hasWaveform()))
                    {
                        needsNfs = true;
                        trackIdForNfs = req.trackId;
                    }
                }

                if (needsNfs)
                {
                    DBG("DbServerClient: NFS LAUNCH trackId=" + juce::String(trackIdForNfs));
                    {
                        const juce::SpinLock::ScopedLockType lock(cacheLock);
                        auto it = metadataCache.find(cacheKey);
                        if (it != metadataCache.end())
                            it->second.nfsAttempted = true;
                    }

                    juce::String nfsPlayerIP = req.playerIP;
                    uint8_t nfsSlot = req.slot;
                    launchNfsAsync(cacheKey, nfsPlayerIP, nfsSlot, trackIdForNfs);
                }
            }
        }
    }

    //==========================================================================
    // Member data
    //==========================================================================
    std::atomic<bool> isRunningFlag { false };

    // Request queue (MPSC: any thread produces via queueLock, DB thread consumes)
    juce::SpinLock queueProducerLock;  // serialises requestMetadata callers (and enqueueInternal)
    std::array<MetadataRequest, kRequestQueueSize> requestQueue;
    std::atomic<uint32_t> reqWritePos { 0 };
    std::atomic<uint32_t> reqReadPos  { 0 };
    juce::WaitableEvent requestSemaphore { false };  // auto-reset: prevents busy-loop

    // TCP connections (one per CDJ, max 6)
    std::array<PlayerConnection, kMaxConnections> connections;

    // When each player's last connection attempt or fresh session failed
    // (hiRes ms), keyed by IP.  Worker thread only.  AUDIT META-8.
    std::unordered_map<std::string, double> failedAtMs;

    // Requests that found their player in its cooldown with no title yet
    // (retryAfterCooldown).  Worker thread only.  AUDIT META-8.
    struct CooldownRetry
    {
        MetadataRequest req;
        int retriesLeft = 0;
    };
    std::vector<CooldownRetry> cooldownRetries;

    // Cache of discovered db ports per player IP. Once we know the port
    // (dysentery has only seen 1051 from CDJs), reusing it on reconnect avoids
    // the 12523 port-discovery TCP dance and saves one round-trip per
    // reconnect. Port doesn't change between reconnects to the same CDJ.
    std::unordered_map<std::string, int> knownDbPorts;
    juce::CriticalSection knownDbPortsLock;

    // Deferred invalidation: external threads (ProDJLink onPlayerLost and
    // onPlayerMoved, through forgetAddress) queue IPs here; the worker
    // thread closes the matching connections
    // between TCP queries. Avoids closing sockets while they are in use.
    juce::StringArray pendingInvalidateIPs;
    juce::CriticalSection pendingInvalidateLock;

    // Metadata cache (protected by SpinLock)
    mutable juce::SpinLock cacheLock;
    std::unordered_map<CacheKey, TrackMetadata, CacheKeyHash> metadataCache;

    // Media changes seen per player and slot (mediaChanged, AUDIT META-4),
    // under cacheLock.  An NFS download notes the count at launch and stores
    // its result only if it has not moved; otherwise a download from the old
    // media, still running when an entry for the same ID on the new media
    // was made, would put the old analysis into it.
    std::unordered_map<uint64_t, uint32_t> slotMediaGeneration;

    // Artwork cache (separate lock for independent access)
    mutable juce::SpinLock artCacheLock;
    std::unordered_map<CacheKey, juce::Image, CacheKeyHash> artworkCache;   // key.id = artwork ID

    // Stats
    std::atomic<uint32_t> dbPortInboundCount { 0 };  // TCP 12523 accepts (diagnostic)
    std::atomic<uint32_t> errorCount { 0 };

    // NFS ANLZ fetcher -- reads the media's export.pdb for the track's ANLZ
    // path, then downloads and parses its analysis files straight from the
    // player's USB/SD.  Runs, once per entry (nfsAttempted), whenever an
    // entry still lacks beat grid, cues, phrases, detail or colour preview
    // (AUDIT META-15): in phase 2, or at once when there is no dbserver
    // session (AUDIT META-7).  The only
    // route for non-3000 players (their dbserver analysis queries are
    // skipped), a fallback for CDJ-3000s.  Runs on its own thread
    // (nfsThread), one download at a time: before launching the next, the
    // worker waits for the previous download to finish, and handles no
    // other request meanwhile.
    NfsAnlzFetcher nfsAnlzFetcher;
    std::thread nfsThread;
    std::atomic<bool> nfsBusy { false };   // set by the worker at launch, cleared by the NFS thread when done
    juce::StringArray nfsLostPlayers;      // worker thread only: lost players the fetcher still has to forget

    // dbserver port-discovery listener (TCP 12523).
    //
    // CDJs that interpret a peer as running a Pioneer-style dbserver attempt
    // to discover that peer's database port by opening TCP to 12523 (see
    // discoverDbPortOnce() above for the protocol).  In the trickofdjequip
    // capture (issue #6) we observed a CDJ-2000NXS2 doing exactly this to
    // STC at a sustained ~25 SYN/s for the entire ~180 s capture: every
    // unanswered SYN got RST from the host TCP stack, the player retried
    // with the next source port (1055, 1056, 1057 ...), and the burst was
    // heavy enough to congest the link-local segment.  The trigger is the
    // 95 B keepalive, which STC still sends to every player by default (see
    // prodjlink95bMode in AppSettings: it can be limited to CDJ-3000s or
    // turned off), and other paths -- firmware revisions, link-load
    // handshakes, simple probing -- cannot be ruled out.
    //
    // So STC also answers the query: accept the TCP connection, read the
    // 19-byte "RemoteDBServer\0" query (best effort), write back a 2-byte
    // port of 0 -- see dbPortListenerLoop() for why 0 -- and close.
    //
    // If port 12523 is already bound by another DJ Link tool running on the
    // same machine (Beat Link Trigger, the official rekordbox bridge, etc.)
    // STC logs the failure and yields gracefully -- whatever is bound there
    // will take care of replying.
    std::unique_ptr<juce::StreamingSocket> dbPortListenSock;
    std::thread dbPortListenerThread;

    void dbPortListenerLoop()
    {
        if (!dbPortListenSock) return;
        while (isRunningFlag.load(std::memory_order_relaxed))
        {
            auto* client = dbPortListenSock->waitForNextConnection();
            if (!client) break;  // shutdown signalled or socket closed
            std::unique_ptr<juce::StreamingSocket> conn(client);

            // Diagnostic (v1.9.11-beta15): count inbound 12523 connections.
            // Each one is a CDJ probing us for a dbserver because a 95B
            // keepalive told it we are a Pioneer bridge.  With the 95B off
            // and the C0 identity this MUST stay at 0 (the reference bridge
            // capture shows zero TCP) -- a non-zero value under those
            // settings falsifies the hypothesis.  Shown in the PDL View.
            dbPortInboundCount.fetch_add(1, std::memory_order_relaxed);

            // Best-effort read of the protocol query (19 B), then reply.
            // Even if the read times out or fails we still send the 2-byte
            // "no service" response so the caller does not retry.
            uint8_t buf[32] = {};
            if (conn->waitUntilReady(true, 1000) == 1)
                (void) conn->read(buf, (int)sizeof(buf), false);

            // The reply is the TCP port of the dbserver, a 2-byte
            // big-endian integer (dysentery, "Track metadata").  STC has no
            // dbserver and replies port 0.  v1.9.10-beta replied 0xFFFF; in
            // the field (issue #6) the NXS2 read that as port 65535 and
            // retried against it, moving the flood instead of stopping it,
            // so v1.9.11 gives it no port to retry against.  Neither value
            // is documented by Deep Symmetry as "no dbserver" (beat-link only
            // logs a 65535 reply as "not yet ready?"), and CDJ-3000s do run a
            // dbserver -- STC queries theirs -- so port 0 rests on that field
            // report, not on the protocol analysis.
            const uint8_t noService[2] = { 0x00, 0x00 };
            (void) conn->write(noService, (int)sizeof(noService));
            // unique_ptr will close on destruct
        }
    }

    /// Run the NFS route only (used when there is no dbserver connection).
    /// Ensures a cache entry exists and launches the NFS download.
    void processNfsFallback(const MetadataRequest& req, const CacheKey& cacheKey)
    {
        // Ensure cache entry exists (may have been created by a previous failed attempt)
        ensureEntry(cacheKey);

        bool needsNfs = false;
        {
            const juce::SpinLock::ScopedLockType lock(cacheLock);
            auto it = metadataCache.find(cacheKey);
            if (it != metadataCache.end()
                && !it->second.nfsAttempted
                && (!it->second.hasBeatGrid() || !it->second.hasCueList()
                    || !it->second.hasSongStructure()
                    || !it->second.hasDetailWaveform()
                    || !it->second.hasWaveform()))
            {
                needsNfs = true;
                it->second.nfsAttempted = true;
            }
        }

        if (needsNfs)
        {
            DBG("DbServerClient: NFS LAUNCH (no conn) trackId=" + juce::String(req.trackId));
            launchNfsAsync(cacheKey, req.playerIP, req.slot, req.trackId);
        }
    }

    /// Launch the NFS download on its own thread.  One download at a time:
    /// waits for the previous one to finish first.  The result is saved to
    /// the disk cache under the title the entry has when the result lands:
    /// one launched while the entry had no title (no dbserver session, AUDIT
    /// META-7) is saved too if the title came during the download.
    void launchNfsAsync(const CacheKey& cacheKey, const juce::String& playerIP,
                        uint8_t slot, uint32_t trackId)
    {
        // Wait for the previous download in short slices rather than in
        // join(), so that stop() never waits on this thread behind a long
        // NFS download (AUDIT META-14).  Not launched once stopping.
        while (!nfsIdle())
        {
            if (threadShouldExit()) return;
            wait(50);
        }
        if (threadShouldExit()) return;
        forgetLostPlayersInNfs();   // before a download could use their stale handles

        // Taken here, before the thread exists: stop()'s cancel() ends this
        // download even if it lands before the thread begins (AUDIT WIRE-6).
        const uint32_t cancelToken = nfsAnlzFetcher.cancelToken();
        uint32_t mediaGeneration = 0;
        {
            const juce::SpinLock::ScopedLockType lock(cacheLock);
            mediaGeneration = slotMediaGenerationLocked(cacheKey.ip, cacheKey.slot);
        }
        nfsBusy.store(true, std::memory_order_release);
        nfsThread = std::thread([this, cacheKey, playerIP, slot, trackId, cancelToken, mediaGeneration]()
        {
            // The ANLZ path comes from the media's export.pdb (AUDIT META-1).
            DBG("DbServerClient: NFS async (PDB lookup) -- trackId=" + juce::String(trackId));
            NfsAnlzFetcher::AnlzResult anlz = nfsAnlzFetcher.fetchByTrackId(playerIP, slot, trackId, cancelToken);

            if (anlz.ok && isRunningFlag.load(std::memory_order_relaxed))
            {
                bool applied = false;
                std::string diskKey;
                TrackMetadata preview;   // the colour preview this download brought, if any
                {
                    const juce::SpinLock::ScopedLockType lock(cacheLock);
                    auto it = metadataCache.find(cacheKey);
                    // Not if the slot's media changed since the launch: the
                    // entry is then another medium's (AUDIT META-4).
                    if (it != metadataCache.end()
                        && slotMediaGenerationLocked(cacheKey.ip, cacheKey.slot) == mediaGeneration)
                    {
                        applied = true;
                        const bool hadPreview = it->second.hasWaveform();
                        // NFS data is authoritative (from USB) -- always overwrite
                        applyNfsAnlzResult(it->second, anlz, true);
                        if (!hadPreview && it->second.hasWaveform())
                        {
                            preview.waveformData          = it->second.waveformData;
                            preview.waveformEntryCount    = it->second.waveformEntryCount;
                            preview.waveformBytesPerEntry = it->second.waveformBytesPerEntry;
                            preview.durationSeconds       = it->second.durationSeconds;
                        }
                        ++it->second.cacheVersion;
                        DBG("DbServerClient: NFS ANLZ applied -- beats="
                            + juce::String((int)it->second.beatGrid.size())
                            + " cues=" + juce::String((int)it->second.cueList.size())
                            + " phrases=" + juce::String((int)it->second.songStructure.size())
                            + " detailEntries=" + juce::String(it->second.detailEntryCount)
                            + " detailBpe=" + juce::String(it->second.detailBytesPerEntry)
                            + " detailDataSz=" + juce::String((int)it->second.detailData.size()));
                        if (it->second.title.isNotEmpty())
                            diskKey = TrackMapEntry::makeKey(
                                it->second.artist, it->second.title, it->second.durationSeconds);
                    }
                }

                // Persist to disk cache for next session: the analysis
                // (.anlz), and the colour preview (.wfc) when this download
                // brought it and no session saved one -- phase 1 loads it
                // from there next time, so NFS does not run again just for
                // the preview (AUDIT META-15).
                if (applied && !diskKey.empty())
                {
                    saveAnlzToDisk(cacheKey, diskKey);
                    if (preview.hasWaveform() && !WaveformCache::exists(diskKey))
                        WaveformCache::save(diskKey, preview.waveformData,
                                            preview.waveformEntryCount, preview.waveformBytesPerEntry,
                                            preview.durationSeconds > 0 ? (uint32_t) preview.durationSeconds * 1000 : 0);
                }
            }
            nfsBusy.store(false, std::memory_order_release);   // last use of nfsAnlzFetcher
        });
    }

    /// Worker thread, with no NFS download running (nfsIdle()).  Tell the
    /// NFS fetcher to forget every player reported lost since the last call:
    /// NfsAnlzFetcher::removePlayer(ip) for each (AUDIT META-4).  A media
    /// change in one slot calls clearPdbCache(ip, slot) at once instead
    /// (mediaChanged): the fetcher's invalidations are thread-safe and only
    /// make a download running for that player or slot not store what it
    /// learnt.
    void forgetLostPlayersInNfs()
    {
        for (auto& ip : nfsLostPlayers)
            nfsAnlzFetcher.removePlayer(ip);
        nfsLostPlayers.clear();
    }

    /// Worker thread.  True when no NFS download is running; then joins the
    /// finished thread (at once), so nfsAnlzFetcher is free until the next
    /// launch.
    bool nfsIdle()
    {
        if (nfsBusy.load(std::memory_order_acquire))
            return false;
        if (nfsThread.joinable())
            nfsThread.join();
        return true;
    }

    /// Build a CachedAnlz from in-memory TrackMetadata and save to disk.
    void saveAnlzToDisk(const CacheKey& cacheKey, const std::string& diskKey)
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

        // Colour preview (PWV4, 6 bytes per column, from the .EXT), only when
        // the entry has none: the only preview a player other than the
        // CDJ-3000 gets, since its dbserver preview query is skipped; a
        // CDJ-3000's dbserver preview (PWV6, 3-band) is kept (AUDIT META-15).
        if (anlz.previewEntryCount > 0 && !anlz.previewData.empty() && !meta.hasWaveform())
        {
            meta.waveformData          = anlz.previewData;
            meta.waveformEntryCount    = anlz.previewEntryCount;
            meta.waveformBytesPerEntry = anlz.previewBytesPerEntry;
            meta.waveformQueried       = true;
        }
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DbServerClient)
};
