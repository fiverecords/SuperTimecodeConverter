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
};
