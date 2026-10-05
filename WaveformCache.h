// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter
//
// WaveformCache -- Persist CDJ color waveform preview data to disk.
//
// Saves waveform data (ThreeBand or ColorNxs2) keyed by TrackMapEntry key
// (artist|title|duration).  Files are stored in the app data directory under
// waveform_cache/ with MD5-hashed filenames to avoid special characters.
//
// Format: 12-byte header + raw waveform bytes
//   [0..3]  uint32 LE  entryCount
//   [4..7]  uint32 LE  bytesPerEntry (3=ThreeBand, 6=ColorNxs2)
//   [8..11] uint32 LE  durationMs
//   [12..]  raw data   (entryCount * bytesPerEntry bytes)
//
// The same directory holds the artwork (.art.png) and the ANLZ data (.anlz:
// beat grid, cues, phrases, detail waveform) under the same key.
//
// Writes come from the message thread, the dbserver worker and the NFS
// thread, sometimes for the same key.  Each file is built in memory, written
// to a temporary file of its own in the same directory and moved over the
// target in one step, one move at a time (AUDIT META-10): a reader sees the
// old file or the new one, never half of one.  Of two saves of one key, the
// one moved last wins.  Loads read the whole file and reject one that
// is shorter than its header says, so a truncated file (an older version
// interrupted mid-write) is ignored instead of loading as zeros.  The cache
// has no size bound and no expiry.

#pragma once
#include <JuceHeader.h>
#include <cstring>
#include <memory>
#include <mutex>

class WaveformCache
{
public:
    struct CachedWaveform
    {
        std::vector<uint8_t> data;
        int entryCount = 0;
        int bytesPerEntry = 0;
        uint32_t durationMs = 0;
        bool valid = false;
    };

    /// Save waveform data for a track key.
    static bool save(const std::string& trackKey,
                     const std::vector<uint8_t>& data,
                     int entryCount, int bytesPerEntry,
                     uint32_t durationMs)
    {
        if (data.empty() || entryCount <= 0 || bytesPerEntry <= 0) return false;
        const size_t rawSize = (size_t)entryCount * (size_t)bytesPerEntry;
        if (data.size() < rawSize) return false;

        juce::MemoryOutputStream fos;

        // Header: 3 x uint32 LE
        writeU32LE(fos, (uint32_t)entryCount);
        writeU32LE(fos, (uint32_t)bytesPerEntry);
        writeU32LE(fos, durationMs);

        // Raw data
        fos.write(data.data(), rawSize);

        return writeAtomically(getCacheFile(trackKey), fos);
    }

    /// Load cached waveform for a track key.
    static CachedWaveform load(const std::string& trackKey)
    {
        CachedWaveform result;
        juce::MemoryBlock file;
        if (!readWholeFile(getCacheFile(trackKey), file)) return result;
        Reader fis(file);

        uint32_t entryCount    = fis.u32();
        uint32_t bytesPerEntry = fis.u32();
        uint32_t durationMs    = fis.u32();
        if (!fis.ok) return result;

        // Sanity checks
        if (entryCount == 0 || entryCount > 100000) return result;
        if (bytesPerEntry != 3 && bytesPerEntry != 6) return result;

        const size_t rawSize = (size_t)entryCount * bytesPerEntry;
        const uint8_t* raw = fis.bytes(rawSize);
        if (raw == nullptr) return result;   // shorter than the header says

        result.data.assign(raw, raw + rawSize);

        result.entryCount = (int)entryCount;
        result.bytesPerEntry = (int)bytesPerEntry;
        result.durationMs = durationMs;
        result.valid = true;
        return result;
    }

    /// Check if a cached waveform exists for a track key.
    static bool exists(const std::string& trackKey)
    {
        return getCacheFile(trackKey).existsAsFile();
    }

    //------------------------------------------------------------------
    // Artwork cache -- saves album art as PNG alongside waveform data
    //------------------------------------------------------------------

    /// Save artwork for a track key.
    static bool saveArtwork(const std::string& trackKey, const juce::Image& img)
    {
        if (!img.isValid()) return false;

        juce::MemoryOutputStream fos;
        juce::PNGImageFormat png;
        if (!png.writeImageToStream(img, fos)) return false;
        return writeAtomically(getArtworkFile(trackKey), fos);
    }

    /// Load cached artwork for a track key.
    static juce::Image loadArtwork(const std::string& trackKey)
    {
        auto file = getArtworkFile(trackKey);
        if (!file.existsAsFile()) return {};

        juce::FileInputStream fis(file);
        if (fis.failedToOpen()) return {};

        juce::PNGImageFormat png;
        return png.decodeImage(fis);
    }

    /// Check if cached artwork exists for a track key.
    static bool artworkExists(const std::string& trackKey)
    {
        return getArtworkFile(trackKey).existsAsFile();
    }

    //------------------------------------------------------------------
    // ANLZ cache -- persists beat grid, cues, phrases, detail waveform
    //------------------------------------------------------------------

    struct CachedAnlz
    {
        std::vector<uint8_t> detailData;
        int detailEntryCount = 0;
        int detailBytesPerEntry = 0;

        struct Beat { uint16_t beatNumber; uint16_t bpmTimes100; uint32_t timeMs; };
        std::vector<Beat> beatGrid;

        struct Cue
        {
            uint8_t  type = 0;       // 0=mem, 1=hot, 2=loop
            uint8_t  hotCueNumber = 0;
            uint32_t positionMs = 0;
            uint32_t loopEndMs = 0;
            uint8_t  colorR = 0, colorG = 0, colorB = 0, colorCode = 0;
            bool     hasColor = false;
            juce::String comment;
        };
        std::vector<Cue> cueList;

        struct Phrase { uint16_t index; uint16_t beatNumber; uint16_t kind; uint8_t fill; uint16_t beatCount; uint16_t beatFill; };
        std::vector<Phrase> songStructure;
        uint16_t phraseMood = 0;

        bool valid = false;
    };

    static bool saveAnlz(const std::string& trackKey, const CachedAnlz& a)
    {
        if (!a.valid) return false;

        // The detail counts are written only when the bytes behind them are,
        // so the file never claims more than it holds.
        const size_t detailSize = (a.detailEntryCount > 0 && a.detailBytesPerEntry > 0)
            ? (size_t)a.detailEntryCount * (size_t)a.detailBytesPerEntry : 0;
        const bool hasDetail = detailSize > 0 && a.detailData.size() >= detailSize;

        juce::MemoryOutputStream fos;

        // Magic + version
        fos.write("ALC1", 4);

        // Counts
        writeU32LE(fos, (uint32_t)a.beatGrid.size());
        writeU32LE(fos, (uint32_t)a.cueList.size());
        writeU32LE(fos, (uint32_t)a.songStructure.size());
        writeU16LE(fos, a.phraseMood);
        writeU32LE(fos, hasDetail ? (uint32_t)a.detailEntryCount : 0u);
        writeU32LE(fos, hasDetail ? (uint32_t)a.detailBytesPerEntry : 0u);

        // Beat grid: 8 bytes each (u16 beatNum, u16 bpm100, u32 timeMs)
        for (auto& b : a.beatGrid)
        {
            writeU16LE(fos, b.beatNumber);
            writeU16LE(fos, b.bpmTimes100);
            writeU32LE(fos, b.timeMs);
        }

        // Cues: variable length
        for (auto& c : a.cueList)
        {
            fos.writeByte((char)c.type);
            fos.writeByte((char)c.hotCueNumber);
            writeU32LE(fos, c.positionMs);
            writeU32LE(fos, c.loopEndMs);
            fos.writeByte((char)c.colorR);
            fos.writeByte((char)c.colorG);
            fos.writeByte((char)c.colorB);
            fos.writeByte((char)c.colorCode);
            fos.writeByte(c.hasColor ? 1 : 0);
            auto utf8 = c.comment.toUTF8();
            uint16_t commentLen = (uint16_t)juce::jmin((int)strlen(utf8), 500);
            writeU16LE(fos, commentLen);
            if (commentLen > 0)
                fos.write(utf8, commentLen);
        }

        // Phrases: 11 bytes each (u16+u16+u16+u8+u16+u16)
        for (auto& p : a.songStructure)
        {
            writeU16LE(fos, p.index);
            writeU16LE(fos, p.beatNumber);
            writeU16LE(fos, p.kind);
            fos.writeByte((char)p.fill);
            writeU16LE(fos, p.beatCount);
            writeU16LE(fos, p.beatFill);
        }

        // Detail waveform raw data
        if (hasDetail)
            fos.write(a.detailData.data(), detailSize);

        return writeAtomically(getAnlzFile(trackKey), fos);
    }

    static CachedAnlz loadAnlz(const std::string& trackKey)
    {
        CachedAnlz result;
        juce::MemoryBlock file;
        if (!readWholeFile(getAnlzFile(trackKey), file)) return result;
        Reader fis(file);

        // Magic check
        const uint8_t* magic = fis.bytes(4);
        if (magic == nullptr || std::memcmp(magic, "ALC1", 4) != 0)
            return result;

        uint32_t numBeats   = fis.u32();
        uint32_t numCues    = fis.u32();
        uint32_t numPhrases = fis.u32();
        uint16_t mood       = fis.u16();
        uint32_t detailEC   = fis.u32();
        uint32_t detailBPE  = fis.u32();
        if (!fis.ok) return result;

        // Sanity
        if (numBeats > 200000 || numCues > 200 || numPhrases > 500) return result;
        if (detailBPE > 3 || detailEC > 2000000) return result;

        // The fixed-size parts must all be in the file before anything is
        // allocated (8 bytes per beat, at least 17 per cue, 11 per phrase,
        // then the detail bytes).
        const uint64_t fixedBytes = (uint64_t)numBeats * 8 + (uint64_t)numCues * 17
                                  + (uint64_t)numPhrases * 11 + (uint64_t)detailEC * detailBPE;
        if (fixedBytes > fis.remaining()) return result;

        // Beats
        result.beatGrid.resize(numBeats);
        for (uint32_t i = 0; i < numBeats; i++)
        {
            result.beatGrid[i].beatNumber  = fis.u16();
            result.beatGrid[i].bpmTimes100 = fis.u16();
            result.beatGrid[i].timeMs      = fis.u32();
        }

        // Cues
        result.cueList.resize(numCues);
        for (uint32_t i = 0; i < numCues; i++)
        {
            auto& c = result.cueList[i];
            c.type          = fis.u8();
            c.hotCueNumber  = fis.u8();
            c.positionMs    = fis.u32();
            c.loopEndMs     = fis.u32();
            c.colorR        = fis.u8();
            c.colorG        = fis.u8();
            c.colorB        = fis.u8();
            c.colorCode     = fis.u8();
            c.hasColor      = (fis.u8() != 0);
            uint16_t cLen   = fis.u16();
            if (cLen > 500) return {};   // saveAnlz never writes more: corrupt
            if (cLen > 0)
            {
                const uint8_t* text = fis.bytes(cLen);
                if (text == nullptr) return {};
                c.comment = juce::String::fromUTF8(reinterpret_cast<const char*>(text), (int)cLen);
            }
        }

        // Phrases
        result.songStructure.resize(numPhrases);
        for (uint32_t i = 0; i < numPhrases; i++)
        {
            auto& p = result.songStructure[i];
            p.index      = fis.u16();
            p.beatNumber = fis.u16();
            p.kind       = fis.u16();
            p.fill       = fis.u8();
            p.beatCount  = fis.u16();
            p.beatFill   = fis.u16();
        }

        // Detail waveform
        const size_t detailSize = (size_t)detailEC * detailBPE;
        if (detailSize > 0)
        {
            const uint8_t* raw = fis.bytes(detailSize);
            if (raw == nullptr) return {};
            result.detailData.assign(raw, raw + detailSize);
            result.detailEntryCount = (int)detailEC;
            result.detailBytesPerEntry = (int)detailBPE;
        }

        // Any read past the end above means the file is shorter than its
        // header says: reject it whole rather than keep zeros.
        if (!fis.ok) return {};

        result.phraseMood = mood;
        result.valid = (!result.beatGrid.empty() || !result.cueList.empty()
                        || !result.songStructure.empty() || result.detailEntryCount > 0);
        return result;
    }

    static bool anlzExists(const std::string& trackKey)
    {
        return getAnlzFile(trackKey).existsAsFile();
    }

    /// Get the cache directory.
    static juce::File getCacheDir()
    {
        auto dir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                       .getChildFile("SuperTimecodeConverter")
                       .getChildFile("waveform_cache");
        return dir;
    }

private:
    /// Map a track key to a cache file path using MD5 hash.
    static juce::File getCacheFile(const std::string& trackKey)
    {
        auto hash = juce::MD5(juce::MemoryBlock(trackKey.data(), trackKey.size())).toHexString();
        return getCacheDir().getChildFile(hash + ".wfc");
    }

    /// Map a track key to an artwork cache file path using MD5 hash.
    static juce::File getArtworkFile(const std::string& trackKey)
    {
        auto hash = juce::MD5(juce::MemoryBlock(trackKey.data(), trackKey.size())).toHexString();
        return getCacheDir().getChildFile(hash + ".art.png");
    }

    /// Map a track key to an ANLZ cache file path using MD5 hash.
    static juce::File getAnlzFile(const std::string& trackKey)
    {
        auto hash = juce::MD5(juce::MemoryBlock(trackKey.data(), trackKey.size())).toHexString();
        return getCacheDir().getChildFile(hash + ".anlz");
    }

    /// Largest cache file a load will read (the largest .anlz saveAnlz can
    /// write within loadAnlz's limits is about 7.7 MB).
    static constexpr juce::int64 kMaxFileBytes = 16 * 1024 * 1024;

    /// Serialises the creation of the cache directory and the moves of
    /// finished files into place (any thread).  Not held while a file is
    /// written: every save writes its own temporary file.
    static std::mutex& writeLock()
    {
        static std::mutex m;
        return m;
    }

    /// Write `content` to a temporary file of its own next to `target`, then
    /// move it over `target` (rename(2) on POSIX, ReplaceFile on Windows,
    /// retried by JUCE for up to ~0.5 s while a reader holds the file open
    /// there).  Only the move is serialised, so a save on the message thread
    /// never waits for another thread's write of a multi-MB .anlz -- at most
    /// for its move, which on Windows includes those retries.
    static bool writeAtomically(const juce::File& target, const juce::MemoryOutputStream& content)
    {
        std::unique_ptr<juce::TemporaryFile> temp;
        {
            const std::lock_guard<std::mutex> lock(writeLock());
            auto dir = target.getParentDirectory();
            if (!dir.isDirectory() && !dir.createDirectory().wasOk())
                return false;
            // The temporary name comes from juce::Random::getSystemRandom(),
            // which is not thread-safe, so it is drawn here, where two saves
            // cannot draw at the same moment (and get the same name).
            temp = std::make_unique<juce::TemporaryFile>(target, juce::TemporaryFile::useHiddenFile);
        }
        {
            juce::FileOutputStream out(temp->getFile());
            if (out.failedToOpen())
                return false;
            if (!out.write(content.getData(), content.getDataSize()))
                return false;
            out.flush();
            if (!out.getStatus().wasOk())
                return false;
        }
        const std::lock_guard<std::mutex> lock(writeLock());
        return temp->overwriteTargetFileWithTemporary();   // the temp file is deleted on failure
    }

    /// Read a whole cache file into memory (false if missing, unreadable,
    /// or larger than any file this class writes).
    static bool readWholeFile(const juce::File& file, juce::MemoryBlock& out)
    {
        if (!file.existsAsFile() || file.getSize() > kMaxFileBytes)
            return false;
        return file.loadFileAsData(out);
    }

    /// Little-endian reader over a loaded file.  A read past the end returns
    /// zero and clears `ok`, so a caller checks once after a group of reads.
    struct Reader
    {
        explicit Reader(const juce::MemoryBlock& b)
            : p(static_cast<const uint8_t*>(b.getData())), n(b.getSize()) {}

        size_t remaining() const { return n - pos; }

        const uint8_t* bytes(size_t count)
        {
            if (!ok || count > n - pos) { ok = false; return nullptr; }
            const uint8_t* r = p + pos;
            pos += count;
            return r;
        }
        uint8_t u8()
        {
            const uint8_t* b = bytes(1);
            return b != nullptr ? b[0] : 0;
        }
        uint16_t u16()
        {
            const uint8_t* b = bytes(2);
            return b != nullptr ? (uint16_t)(b[0] | (b[1] << 8)) : 0;
        }
        uint32_t u32()
        {
            const uint8_t* b = bytes(4);
            return b != nullptr ? ((uint32_t)b[0] | ((uint32_t)b[1] << 8)
                                   | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24)) : 0;
        }

        const uint8_t* p;
        size_t n;
        size_t pos = 0;
        bool ok = true;
    };

    static void writeU32LE(juce::OutputStream& fos, uint32_t val)
    {
        uint8_t buf[4];
        buf[0] = (uint8_t)(val & 0xFF);
        buf[1] = (uint8_t)((val >> 8) & 0xFF);
        buf[2] = (uint8_t)((val >> 16) & 0xFF);
        buf[3] = (uint8_t)((val >> 24) & 0xFF);
        fos.write(buf, 4);
    }

    static void writeU16LE(juce::OutputStream& fos, uint16_t val)
    {
        uint8_t buf[2];
        buf[0] = (uint8_t)(val & 0xFF);
        buf[1] = (uint8_t)((val >> 8) & 0xFF);
        fos.write(buf, 2);
    }
};
