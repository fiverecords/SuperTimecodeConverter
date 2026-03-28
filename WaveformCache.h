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

#pragma once
#include <JuceHeader.h>

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
        if ((int)data.size() < entryCount * bytesPerEntry) return false;

        auto file = getCacheFile(trackKey);
        if (!file.getParentDirectory().exists())
            file.getParentDirectory().createDirectory();

        juce::FileOutputStream fos(file);
        if (fos.failedToOpen()) return false;
        fos.setPosition(0);
        fos.truncate();

        // Header: 3 x uint32 LE
        writeU32LE(fos, (uint32_t)entryCount);
        writeU32LE(fos, (uint32_t)bytesPerEntry);
        writeU32LE(fos, durationMs);

        // Raw data
        int rawSize = entryCount * bytesPerEntry;
        fos.write(data.data(), (size_t)rawSize);

        fos.flush();
        return fos.getStatus().wasOk();
    }

    /// Load cached waveform for a track key.
    static CachedWaveform load(const std::string& trackKey)
    {
        CachedWaveform result;
        auto file = getCacheFile(trackKey);
        if (!file.existsAsFile()) return result;

        juce::FileInputStream fis(file);
        if (fis.failedToOpen() || fis.getTotalLength() < 12) return result;

        uint32_t entryCount    = readU32LE(fis);
        uint32_t bytesPerEntry = readU32LE(fis);
        uint32_t durationMs    = readU32LE(fis);

        // Sanity checks
        if (entryCount == 0 || entryCount > 100000) return result;
        if (bytesPerEntry != 3 && bytesPerEntry != 6) return result;

        int rawSize = (int)(entryCount * bytesPerEntry);
        int64_t remaining = fis.getTotalLength() - fis.getPosition();
        if (remaining < rawSize) return result;

        result.data.resize((size_t)rawSize);
        fis.read(result.data.data(), rawSize);

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

        auto file = getArtworkFile(trackKey);
        if (!file.getParentDirectory().exists())
            file.getParentDirectory().createDirectory();

        juce::FileOutputStream fos(file);
        if (fos.failedToOpen()) return false;
        fos.setPosition(0);
        fos.truncate();

        juce::PNGImageFormat png;
        return png.writeImageToStream(img, fos);
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

        auto file = getAnlzFile(trackKey);
        if (!file.getParentDirectory().exists())
            file.getParentDirectory().createDirectory();

        juce::FileOutputStream fos(file);
        if (fos.failedToOpen()) return false;
        fos.setPosition(0);
        fos.truncate();

        // Magic + version
        fos.write("ALC1", 4);

        // Counts
        writeU32LE(fos, (uint32_t)a.beatGrid.size());
        writeU32LE(fos, (uint32_t)a.cueList.size());
        writeU32LE(fos, (uint32_t)a.songStructure.size());
        writeU16LE(fos, a.phraseMood);
        writeU32LE(fos, (uint32_t)a.detailEntryCount);
        writeU32LE(fos, (uint32_t)a.detailBytesPerEntry);

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
        int detailSize = a.detailEntryCount * a.detailBytesPerEntry;
        if (detailSize > 0 && (int)a.detailData.size() >= detailSize)
            fos.write(a.detailData.data(), (size_t)detailSize);

        fos.flush();
        return fos.getStatus().wasOk();
    }

    static CachedAnlz loadAnlz(const std::string& trackKey)
    {
        CachedAnlz result;
        auto file = getAnlzFile(trackKey);
        if (!file.existsAsFile()) return result;

        juce::FileInputStream fis(file);
        if (fis.failedToOpen() || fis.getTotalLength() < 24) return result;

        // Magic check
        char magic[4];
        fis.read(magic, 4);
        if (magic[0] != 'A' || magic[1] != 'L' || magic[2] != 'C' || magic[3] != '1')
            return result;

        uint32_t numBeats   = readU32LE(fis);
        uint32_t numCues    = readU32LE(fis);
        uint32_t numPhrases = readU32LE(fis);
        uint16_t mood       = readU16LE(fis);
        uint32_t detailEC   = readU32LE(fis);
        uint32_t detailBPE  = readU32LE(fis);

        // Sanity
        if (numBeats > 200000 || numCues > 200 || numPhrases > 500) return result;
        if (detailBPE > 3 || detailEC > 2000000) return result;

        // Beats
        result.beatGrid.resize(numBeats);
        for (uint32_t i = 0; i < numBeats; i++)
        {
            result.beatGrid[i].beatNumber  = readU16LE(fis);
            result.beatGrid[i].bpmTimes100 = readU16LE(fis);
            result.beatGrid[i].timeMs      = readU32LE(fis);
        }

        // Cues
        result.cueList.resize(numCues);
        for (uint32_t i = 0; i < numCues; i++)
        {
            auto& c = result.cueList[i];
            c.type          = (uint8_t)fis.readByte();
            c.hotCueNumber  = (uint8_t)fis.readByte();
            c.positionMs    = readU32LE(fis);
            c.loopEndMs     = readU32LE(fis);
            c.colorR        = (uint8_t)fis.readByte();
            c.colorG        = (uint8_t)fis.readByte();
            c.colorB        = (uint8_t)fis.readByte();
            c.colorCode     = (uint8_t)fis.readByte();
            c.hasColor      = (fis.readByte() != 0);
            uint16_t cLen   = readU16LE(fis);
            if (cLen > 0 && cLen <= 500)
            {
                juce::HeapBlock<char> buf(cLen + 1);
                fis.read(buf, cLen);
                buf[cLen] = 0;
                c.comment = juce::String::fromUTF8(buf, (int)cLen);
            }
        }

        // Phrases
        result.songStructure.resize(numPhrases);
        for (uint32_t i = 0; i < numPhrases; i++)
        {
            auto& p = result.songStructure[i];
            p.index      = readU16LE(fis);
            p.beatNumber = readU16LE(fis);
            p.kind       = readU16LE(fis);
            p.fill       = (uint8_t)fis.readByte();
            p.beatCount  = readU16LE(fis);
            p.beatFill   = readU16LE(fis);
        }

        // Detail waveform
        int detailSize = (int)(detailEC * detailBPE);
        if (detailSize > 0)
        {
            int64_t remaining = fis.getTotalLength() - fis.getPosition();
            if (remaining >= detailSize)
            {
                result.detailData.resize((size_t)detailSize);
                fis.read(result.detailData.data(), detailSize);
                result.detailEntryCount = (int)detailEC;
                result.detailBytesPerEntry = (int)detailBPE;
            }
        }

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

    static void writeU32LE(juce::FileOutputStream& fos, uint32_t val)
    {
        uint8_t buf[4];
        buf[0] = (uint8_t)(val & 0xFF);
        buf[1] = (uint8_t)((val >> 8) & 0xFF);
        buf[2] = (uint8_t)((val >> 16) & 0xFF);
        buf[3] = (uint8_t)((val >> 24) & 0xFF);
        fos.write(buf, 4);
    }

    static uint32_t readU32LE(juce::FileInputStream& fis)
    {
        uint8_t buf[4] = {};
        fis.read(buf, 4);
        return (uint32_t)buf[0]
             | ((uint32_t)buf[1] << 8)
             | ((uint32_t)buf[2] << 16)
             | ((uint32_t)buf[3] << 24);
    }

    static void writeU16LE(juce::FileOutputStream& fos, uint16_t val)
    {
        uint8_t buf[2];
        buf[0] = (uint8_t)(val & 0xFF);
        buf[1] = (uint8_t)((val >> 8) & 0xFF);
        fos.write(buf, 2);
    }

    static uint16_t readU16LE(juce::FileInputStream& fis)
    {
        uint8_t buf[2] = {};
        fis.read(buf, 2);
        return (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    }
};
