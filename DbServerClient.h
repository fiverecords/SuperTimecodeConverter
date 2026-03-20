// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter
//
// DbServerClient -- TCP client for Pioneer dbserver protocol (port 12523).
// Queries CDJ internal databases to retrieve track metadata (title, artist,
// album, key, genre, artwork) for rekordbox tracks loaded on the decks.
//
// Protocol reference: Deep Symmetry "DJ Link Ecosystem Analysis"
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
    double cacheTime = 0.0;        // when this entry was cached (for LRU)

    // Color preview waveform data from CDJ
    //   ThreeBand (PWV6): 3 bytes/entry = {mid, high, low} frequency heights
    //   ColorNxs2 (PWV4): 6 bytes/entry = {d0, d1, d2, d3(R), d4(G), d5(B)}
    std::vector<uint8_t> waveformData;
    int waveformEntryCount = 0;
    int waveformBytesPerEntry = 0;  // 3 = ThreeBand, 6 = ColorNxs2

    bool isValid() const { return trackId != 0 && title.isNotEmpty(); }
    bool hasWaveform() const { return waveformEntryCount > 0 && !waveformData.empty(); }
    bool isThreeBandWaveform() const { return waveformBytesPerEntry == 3; }
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

        // Check cache first -- avoid unnecessary requests
        uint64_t cacheKey = makeCacheKey(playerIP, trackId);
        {
            const juce::SpinLock::ScopedLockType lock(cacheLock);
            auto it = metadataCache.find(cacheKey);
            if (it != metadataCache.end() && it->second.isValid())
                return;  // already cached
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
                default:
                    // Color (0x0013-0x001B) or unknown -- skip
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
    /// Per Deep Symmetry: blob is in arg[3] (index 3).
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
            errorCount.fetch_add(1, std::memory_order_relaxed);
            DBG("DbServerClient: failed to get connection to " + req.playerIP);
            return;
        }

        if (req.trackId != 0)
        {
            // Metadata request
            auto meta = queryTrackMetadata(*conn, req.slot, req.trackType,
                                            req.trackId, req.ourPlayer);
            if (meta.isValid())
            {
                cacheMetadata(req.playerIP, meta);
                DBG("DbServerClient: cached metadata for track "
                    + juce::String(req.trackId) + " -- \""
                    + meta.artist + " - " + meta.title + "\"");

                // Request artwork if available
                if (req.wantArt && meta.artworkId != 0)
                {
                    auto img = queryArtwork(*conn, req.slot, req.trackType,
                                             meta.artworkId, req.ourPlayer);
                    if (img.isValid())
                        cacheArtwork(meta.artworkId, img);
                }

                // Request preview waveform (PWV6 for CDJ-3000, PWV4 for NXS2)
                if (req.wantWaveform)
                {
                    auto wfResult = queryPreviewWaveform(
                        *conn, req.slot, req.trackType, req.trackId,
                        req.ourPlayer, req.playerModel);
                    if (wfResult.entryCount > 0)
                    {
                        uint64_t key = makeCacheKey(req.playerIP, req.trackId);
                        const juce::SpinLock::ScopedLockType lock(cacheLock);
                        auto it = metadataCache.find(key);
                        if (it != metadataCache.end())
                        {
                            it->second.waveformData = std::move(wfResult.data);
                            it->second.waveformEntryCount = wfResult.entryCount;
                            it->second.waveformBytesPerEntry =
                                (wfResult.format == WaveformFormat::ThreeBand) ? 3 : 6;
                        }
                    }
                }
            }
            else
            {
                errorCount.fetch_add(1, std::memory_order_relaxed);
                DBG("DbServerClient: metadata query failed for trackId="
                    + juce::String(req.trackId));
                // Connection may be stale -- close so next request reconnects
                if (!conn->isConnected())
                    conn->close();
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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DbServerClient)
};
