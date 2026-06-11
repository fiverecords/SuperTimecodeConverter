// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

#pragma once
#include <JuceHeader.h>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <string>

//==============================================================================
// TrackMap -- maps tracks (by artist|title) to timecode offsets and triggers
//==============================================================================

/// A single entry mapping a track (identified by artist + title) to a
/// timecode offset that will be applied when that track is detected as playing.
//==============================================================================
// CuePoint -- trigger at a specific playhead position within a track
//==============================================================================
struct CuePoint
{
    uint32_t positionMs = 0;        // playhead position in ms from track start
    juce::String name;              // user label ("BREAK", "DROP", "LIGHTS ON", etc.)

    // Trigger config (same structure as TrackMapEntry track-change triggers)
    int  midiChannel  = 0;          // 0-15 (displayed as 1-16)
    int  midiNoteNum  = -1;         // -1 = disabled
    int  midiNoteVel  = 127;
    int  midiCCNum    = -1;         // -1 = disabled
    int  midiCCVal    = 127;

    juce::String oscAddress;        // empty = no OSC
    juce::String oscArgs;

    int  artnetCh     = 0;          // 0 = disabled, 1-512
    int  artnetVal    = 255;

    // --- Query helpers ---
    bool hasMidiTrigger()   const { return midiNoteNum >= 0 || midiCCNum >= 0; }
    bool hasOscTrigger()    const { return oscAddress.isNotEmpty(); }
    bool hasArtnetTrigger() const { return artnetCh > 0; }
    bool hasAnyTrigger()    const { return hasMidiTrigger() || hasOscTrigger() || hasArtnetTrigger(); }

    // --- Serialization ---
    juce::var toVar() const
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty("positionMs", (int)positionMs);
        if (name.isNotEmpty())
            obj->setProperty("name", name);

        if (midiNoteNum >= 0 || midiCCNum >= 0)
        {
            obj->setProperty("midiChannel", midiChannel);
            obj->setProperty("midiNoteNum", midiNoteNum);
            obj->setProperty("midiNoteVel", midiNoteVel);
            obj->setProperty("midiCCNum",   midiCCNum);
            obj->setProperty("midiCCVal",   midiCCVal);
        }
        if (oscAddress.isNotEmpty())
        {
            obj->setProperty("oscAddress", oscAddress);
            if (oscArgs.isNotEmpty())
                obj->setProperty("oscArgs", oscArgs);
        }
        if (artnetCh > 0)
        {
            obj->setProperty("artnetCh",  artnetCh);
            obj->setProperty("artnetVal", artnetVal);
        }
        return juce::var(obj);
    }

    void fromVar(const juce::var& v)
    {
        auto* obj = v.getDynamicObject();
        if (!obj) return;

        auto getInt = [&](const char* key, int def) {
            auto val = obj->getProperty(key);
            return val.isVoid() ? def : (int)val;
        };
        auto getString = [&](const char* key, const juce::String& def = {}) {
            auto val = obj->getProperty(key);
            return val.isVoid() ? def : val.toString();
        };

        positionMs  = (uint32_t)juce::jmax(0, getInt("positionMs", 0));
        name        = getString("name");
        midiChannel = juce::jlimit(0, 15, getInt("midiChannel", 0));
        midiNoteNum = juce::jlimit(-1, 127, getInt("midiNoteNum", -1));
        midiNoteVel = juce::jlimit(0, 127, getInt("midiNoteVel", 127));
        midiCCNum   = juce::jlimit(-1, 127, getInt("midiCCNum", -1));
        midiCCVal   = juce::jlimit(0, 127, getInt("midiCCVal", 127));
        oscAddress  = getString("oscAddress");
        oscArgs     = getString("oscArgs");
        artnetCh    = juce::jlimit(0, 512, getInt("artnetCh", 0));
        artnetVal   = juce::jlimit(0, 255, getInt("artnetVal", 255));
    }

    /// Format positionMs as "MM:SS.mmm" for display
    static juce::String formatPositionMs(uint32_t ms)
    {
        int totalSec = (int)(ms / 1000);
        int mins = totalSec / 60;
        int secs = totalSec % 60;
        int millis = (int)(ms % 1000);
        return juce::String::formatted("%02d:%02d.%03d", mins, secs, millis);
    }
};

//==============================================================================
// GeneratorCuePoint -- a cue inside a Generator preset.  Same trigger
// payload as CuePoint (MIDI/OSC/Art-Net) but the trigger position is
// expressed as an absolute SMPTE timecode (HH:MM:SS:FF) rather than as
// milliseconds from track start.  This matches the rest of the Generator
// preset editor (which already speaks in TC for Start / Stop) and means a
// cue at "01:00:30:00" works identically whether the preset has an audio
// file (TC follows the audio playhead) or runs in pure TC-generation mode.
//==============================================================================
struct GeneratorCuePoint
{
    juce::String positionTC = "00:00:00:00";  // HH:MM:SS:FF -- when to fire
    juce::String name;                          // user label

    // Trigger config -- mirrors CuePoint exactly so the same dispatch
    // helpers (firing MIDI / OSC / DMX) can be reused.
    int  midiChannel  = 0;          // 0-15 (displayed as 1-16)
    int  midiNoteNum  = -1;         // -1 = disabled
    int  midiNoteVel  = 127;
    int  midiCCNum    = -1;         // -1 = disabled
    int  midiCCVal    = 127;

    juce::String oscAddress;
    juce::String oscArgs;

    int  artnetCh     = 0;          // 0 = disabled, 1-512
    int  artnetVal    = 255;

    bool hasMidiTrigger()   const { return midiNoteNum >= 0 || midiCCNum >= 0; }
    bool hasOscTrigger()    const { return oscAddress.isNotEmpty(); }
    bool hasArtnetTrigger() const { return artnetCh > 0; }
    bool hasAnyTrigger()    const { return hasMidiTrigger() || hasOscTrigger() || hasArtnetTrigger(); }

    /// Convert positionTC to total milliseconds (frame -> ms uses the
    /// supplied frame rate; default 30 matches the rest of the codebase
    /// when the caller doesn't know the engine's effective fps).  This is
    /// the form the engine uses to compare against its current TC and
    /// decide whether a cue should fire.
    uint32_t positionMs(double fps = 30.0) const
    {
        auto parts = juce::StringArray::fromTokens(positionTC, ":.", "");
        int h = 0, m = 0, s = 0, f = 0;
        if (parts.size() >= 1) h = parts[0].getIntValue();
        if (parts.size() >= 2) m = parts[1].getIntValue();
        if (parts.size() >= 3) s = parts[2].getIntValue();
        if (parts.size() >= 4) f = parts[3].getIntValue();
        if (fps <= 0.0) fps = 30.0;
        double totalSec = h * 3600.0 + m * 60.0 + s + (double) f / fps;
        return (uint32_t) juce::jmax(0.0, totalSec * 1000.0);
    }

    juce::var toVar() const
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty("positionTC", positionTC);
        if (name.isNotEmpty())
            obj->setProperty("name", name);

        if (midiNoteNum >= 0 || midiCCNum >= 0)
        {
            obj->setProperty("midiChannel", midiChannel);
            obj->setProperty("midiNoteNum", midiNoteNum);
            obj->setProperty("midiNoteVel", midiNoteVel);
            obj->setProperty("midiCCNum",   midiCCNum);
            obj->setProperty("midiCCVal",   midiCCVal);
        }
        if (oscAddress.isNotEmpty())
        {
            obj->setProperty("oscAddress", oscAddress);
            if (oscArgs.isNotEmpty())
                obj->setProperty("oscArgs", oscArgs);
        }
        if (artnetCh > 0)
        {
            obj->setProperty("artnetCh",  artnetCh);
            obj->setProperty("artnetVal", artnetVal);
        }
        return juce::var(obj);
    }

    void fromVar(const juce::var& v)
    {
        auto* obj = v.getDynamicObject();
        if (!obj) return;

        auto getInt = [&](const char* key, int def) {
            auto val = obj->getProperty(key);
            return val.isVoid() ? def : (int)val;
        };
        auto getString = [&](const char* key, const juce::String& def = {}) {
            auto val = obj->getProperty(key);
            return val.isVoid() ? def : val.toString();
        };

        positionTC  = getString("positionTC", "00:00:00:00");
        if (positionTC.isEmpty()) positionTC = "00:00:00:00";
        name        = getString("name");
        midiChannel = juce::jlimit(0, 15, getInt("midiChannel", 0));
        midiNoteNum = juce::jlimit(-1, 127, getInt("midiNoteNum", -1));
        midiNoteVel = juce::jlimit(0, 127, getInt("midiNoteVel", 127));
        midiCCNum   = juce::jlimit(-1, 127, getInt("midiCCNum", -1));
        midiCCVal   = juce::jlimit(0, 127, getInt("midiCCVal", 127));
        oscAddress  = getString("oscAddress");
        oscArgs     = getString("oscArgs");
        artnetCh    = juce::jlimit(0, 512, getInt("artnetCh", 0));
        artnetVal   = juce::jlimit(0, 255, getInt("artnetVal", 255));
    }
};

//==============================================================================
// TrackMapEntry -- per-track config: offset, triggers, cue points
//==============================================================================
struct TrackMapEntry
{
    juce::String artist;
    juce::String title;
    int          durationSec     = 0;       // track duration in seconds (0 = unknown/legacy)
    juce::String timecodeOffset  = "00:00:00:00";   // HH:MM:SS:FF
    juce::String notes;

    // Sort order (0 = default/alphabetical, >0 = explicit position from playlist import)
    int          sortOrder       = 0;

    // MIDI triggers (independent -- any combination can fire simultaneously)
    int          midiChannel     = 0;       // 0-15 (displayed as 1-16), shared across all MIDI types
    int          midiNoteNum     = -1;      // Note On: note number (-1 = disabled, 0-127)
    int          midiNoteVel     = 127;     // Note On: velocity (0-127)
    int          midiCCNum       = -1;      // CC: controller number (-1 = disabled, 0-127)
    int          midiCCVal       = 127;     // CC: value (0-127)

    // OSC trigger (per-track: what to send when this track becomes active)
    juce::String oscAddress;                // e.g. "/cue/1/go", empty = no OSC trigger
    juce::String oscArgs;                   // typed args, e.g. "i:42 s:hello f:3.14"

    // Art-Net DMX trigger (per-track: one-shot DMX value on track change)
    int          artnetCh        = 0;       // DMX channel (0 = disabled, 1-512)
    int          artnetVal       = 255;     // DMX value (0-255)

    // BPM multiplier (per-track: applied to MIDI Clock, Ableton Link, OSC BPM forward)
    // 0 = off (pass-through), 1 = x2, 2 = x4, -1 = /2, -2 = /4
    int          bpmMultiplier   = 0;

    // Cue points -- triggers at specific playhead positions within the track.
    // Sorted by positionMs ascending for efficient linear scan during playback.
    std::vector<CuePoint> cuePoints;

    //------------------------------------------------------------------
    // Key generation -- case-insensitive artist|title[|duration]
    //------------------------------------------------------------------
    static std::string makeKey(const juce::String& a, const juce::String& t, int dur = 0)
    {
        auto base = (a.toLowerCase().trim() + "|" + t.toLowerCase().trim()).toStdString();
        if (dur > 0)
            return base + "|" + std::to_string(dur);
        return base;
    }
    std::string key() const { return makeKey(artist, title, durationSec); }
    bool hasValidKey() const { return title.isNotEmpty(); }

    //------------------------------------------------------------------
    // Trigger queries
    //------------------------------------------------------------------
    bool hasMidiTrigger()   const { return midiNoteNum >= 0 || midiCCNum >= 0; }
    bool hasOscTrigger()    const { return oscAddress.isNotEmpty(); }
    bool hasArtnetTrigger() const { return artnetCh > 0; }
    bool hasAnyTrigger()    const { return hasMidiTrigger() || hasOscTrigger() || hasArtnetTrigger(); }
    bool hasCuePoints()     const { return !cuePoints.empty(); }

    /// Sort cue points by position (call after adding/editing cues)
    void sortCuePoints()
    {
        std::sort(cuePoints.begin(), cuePoints.end(),
                  [](const CuePoint& a, const CuePoint& b) { return a.positionMs < b.positionMs; });
    }

    //------------------------------------------------------------------
    juce::var toVar() const
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty("artist",         artist);
        obj->setProperty("title",          title);
        if (durationSec > 0)
            obj->setProperty("durationSec", durationSec);
        obj->setProperty("timecodeOffset", timecodeOffset);
        obj->setProperty("notes",          notes);
        if (sortOrder > 0)
            obj->setProperty("sortOrder",  sortOrder);

        // MIDI triggers (independent)
        obj->setProperty("midiChannel",  midiChannel);
        obj->setProperty("midiNoteNum",  midiNoteNum);
        obj->setProperty("midiNoteVel",  midiNoteVel);
        obj->setProperty("midiCCNum",    midiCCNum);
        obj->setProperty("midiCCVal",    midiCCVal);

        // OSC trigger
        if (oscAddress.isNotEmpty())
        {
            obj->setProperty("oscAddress", oscAddress);
            if (oscArgs.isNotEmpty())
                obj->setProperty("oscArgs", oscArgs);
        }

        // Art-Net DMX trigger
        if (artnetCh > 0)
        {
            obj->setProperty("artnetCh",  artnetCh);
            obj->setProperty("artnetVal", artnetVal);
        }

        // BPM multiplier (0 = off, only write if set)
        if (bpmMultiplier != 0)
            obj->setProperty("bpmMultiplier", bpmMultiplier);

        // Cue points (only write if present)
        if (!cuePoints.empty())
        {
            juce::Array<juce::var> cueArr;
            for (auto& cue : cuePoints)
                cueArr.add(cue.toVar());
            obj->setProperty("cuePoints", cueArr);
        }

        return juce::var(obj);
    }

    void fromVar(const juce::var& v)
    {
        auto* obj = v.getDynamicObject();
        if (!obj) return;

        auto getString = [&](const char* key, const juce::String& def = {}) {
            auto val = obj->getProperty(key);
            return val.isVoid() ? def : val.toString();
        };
        auto getInt = [&](const char* key, int def) {
            auto val = obj->getProperty(key);
            return val.isVoid() ? def : (int)val;
        };

        artist         = getString("artist");
        title          = getString("title");
        durationSec    = juce::jmax(0, getInt("durationSec", 0));
        notes          = getString("notes");
        sortOrder      = juce::jmax(0, getInt("sortOrder", 0));

        // Legacy migration: if entry has trackId but no title, generate a placeholder
        // so imported v1.5 entries don't vanish (user can edit them later).
        if (title.isEmpty())
        {
            auto idVal = obj->getProperty("trackId");
            if (!idVal.isVoid() && (juce::int64)idVal != 0)
                title = "Track #" + juce::String((juce::int64)idVal);
        }

        // Validate timecodeOffset: must parse as valid HH:MM:SS:FF
        {
            juce::String rawOffset = getString("timecodeOffset", "00:00:00:00");
            int h, m, s, f;
            if (parseTimecodeString(rawOffset, h, m, s, f))
                timecodeOffset = rawOffset;
            else
                timecodeOffset = "00:00:00:00";  // reset malformed offsets
        }

        // MIDI triggers (independent fields, v1.5+)
        auto newNoteField = obj->getProperty("midiNoteNum");
        if (!newNoteField.isVoid())
        {
            midiChannel = juce::jlimit(0, 15, getInt("midiChannel", 0));
            midiNoteNum = juce::jlimit(-1, 127, getInt("midiNoteNum", -1));
            midiNoteVel = juce::jlimit(0, 127, getInt("midiNoteVel", 127));
            midiCCNum   = juce::jlimit(-1, 127, getInt("midiCCNum", -1));
            midiCCVal   = juce::jlimit(0, 127, getInt("midiCCVal", 127));
        }
        else
        {
            // Legacy migration from midiMsgType (v1.4 and earlier)
            int legacyType = juce::jlimit(0, 3, getInt("midiMsgType", 0));
            midiChannel = juce::jlimit(0, 15, getInt("midiChannel", 0));
            int v1 = juce::jlimit(0, 127, getInt("midiValue1", 0));
            int v2 = juce::jlimit(0, 127, getInt("midiValue2", 127));
            midiNoteNum = -1;  midiNoteVel = 127;
            midiCCNum = -1;    midiCCVal = 127;
            switch (legacyType)
            {
                case 1: midiNoteNum = v1; midiNoteVel = v2; break;
                case 3: midiCCNum = v1; midiCCVal = v2; break;
                default: break;
            }
        }

        // OSC trigger
        oscAddress = getString("oscAddress");
        oscArgs    = getString("oscArgs");

        // Art-Net DMX trigger
        artnetCh  = juce::jlimit(0, 512, getInt("artnetCh", 0));
        artnetVal = juce::jlimit(0, 255, getInt("artnetVal", 255));

        // BPM multiplier
        {
            int raw = getInt("bpmMultiplier", 0);
            bpmMultiplier = (raw == 1 || raw == 2 || raw == -1 || raw == -2) ? raw : 0;
        }

        // Cue points
        cuePoints.clear();
        auto* cueArr = obj->getProperty("cuePoints").getArray();
        if (cueArr)
        {
            for (auto& item : *cueArr)
            {
                CuePoint cp;
                cp.fromVar(item);
                cuePoints.push_back(std::move(cp));
            }
            // Ensure sorted by position
            std::sort(cuePoints.begin(), cuePoints.end(),
                      [](const CuePoint& a, const CuePoint& b) { return a.positionMs < b.positionMs; });
        }
    }

    //------------------------------------------------------------------
    // Timecode offset parsing/formatting utilities
    //------------------------------------------------------------------

    /// Parse "HH:MM:SS:FF" or "HH:MM:SS.FF" -> individual fields.
    /// Returns false if format is invalid.
    static bool parseTimecodeString(const juce::String& s,
                                    int& h, int& m, int& sec, int& f)
    {
        // Accept both ':' and '.' as separators for the frame field
        auto normalized = s.replace(".", ":");
        auto parts = juce::StringArray::fromTokens(normalized, ":", "");
        if (parts.size() != 4) return false;

        h   = parts[0].getIntValue();
        m   = parts[1].getIntValue();
        sec = parts[2].getIntValue();
        f   = parts[3].getIntValue();

        return h >= 0 && h <= 23
            && m >= 0 && m <= 59
            && sec >= 0 && sec <= 59
            && f >= 0 && f <= 29;
    }

    /// Format fields -> "HH:MM:SS:FF"
    static juce::String formatTimecodeString(int h, int m, int s, int f)
    {
        return juce::String::formatted("%02d:%02d:%02d:%02d",
                                       juce::jlimit(0, 23, h),
                                       juce::jlimit(0, 59, m),
                                       juce::jlimit(0, 59, s),
                                       juce::jlimit(0, 29, f));
    }
};

//==============================================================================
// TrackMap -- O(1) lookup by artist|title, persisted as separate JSON file
//==============================================================================
class TrackMap
{
public:
    //------------------------------------------------------------------
    // File location
    //------------------------------------------------------------------
    static juce::File getTrackMapFile()
    {
        auto dir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                       .getChildFile("SuperTimecodeConverter");
        dir.createDirectory();
        return dir.getChildFile("trackmap.json");
    }

    //------------------------------------------------------------------
    // Persistence
    //------------------------------------------------------------------
    void save() const
    {
        auto* root = new juce::DynamicObject();
        root->setProperty("version", 2);  // v2 = artist|title keyed

        juce::Array<juce::var> arr;
        for (auto& [k, entry] : entries)
            arr.add(entry.toVar());

        root->setProperty("tracks", arr);

        juce::var jsonVar(root);
        getTrackMapFile().replaceWithText(juce::JSON::toString(jsonVar));
    }

    bool load()
    {
        auto file = getTrackMapFile();
        if (!file.existsAsFile()) return false;

        auto parsed = juce::JSON::parse(file.loadFileAsString());
        auto* obj = parsed.getDynamicObject();
        if (!obj) return false;

        entries.clear();

        auto* arr = obj->getProperty("tracks").getArray();
        if (arr)
        {
            for (auto& item : *arr)
            {
                TrackMapEntry e;
                e.fromVar(item);
                if (e.hasValidKey())
                    entries[e.key()] = std::move(e);
            }
        }
        ++generation;
        return true;
    }

    //------------------------------------------------------------------
    // Lookup -- single hash lookup by artist|title[|duration]
    //------------------------------------------------------------------

    /// Find entry by artist + title + optional duration.
    const TrackMapEntry* find(const juce::String& artist, const juce::String& title,
                              int dur = 0) const
    {
        auto it = entries.find(TrackMapEntry::makeKey(artist, title, dur));
        return (it != entries.end()) ? &it->second : nullptr;
    }

    /// Mutable find (for editing in-place)
    TrackMapEntry* find(const juce::String& artist, const juce::String& title,
                        int dur = 0)
    {
        auto it = entries.find(TrackMapEntry::makeKey(artist, title, dur));
        return (it != entries.end()) ? &it->second : nullptr;
    }

    /// Check if an artist+title[+duration] exists in the map
    bool contains(const juce::String& artist, const juce::String& title,
                  int dur = 0) const
    {
        return entries.count(TrackMapEntry::makeKey(artist, title, dur)) > 0;
    }

    /// Find by artist+title, ignoring duration.  Used as a last-resort fallback
    /// when the caller's duration doesn't match the entry's saved duration.
    const TrackMapEntry* findIgnoringDuration(const juce::String& artist,
                                              const juce::String& title) const
    {
        auto base = TrackMapEntry::makeKey(artist, title, 0);  // key without duration
        // Exact match (entry saved without duration)
        auto it = entries.find(base);
        if (it != entries.end()) return &it->second;
        // Prefix match (entry saved with some duration: "base|NNN")
        auto prefix = base + "|";
        for (auto& [k, v] : entries)
            if (k.size() > prefix.size() && k.substr(0, prefix.size()) == prefix)
                return &v;
        return nullptr;
    }

    /// Mutable version of findIgnoringDuration
    TrackMapEntry* findIgnoringDuration(const juce::String& artist,
                                        const juce::String& title)
    {
        auto base = TrackMapEntry::makeKey(artist, title, 0);
        auto it = entries.find(base);
        if (it != entries.end()) return &it->second;
        auto prefix = base + "|";
        for (auto& [k, v] : entries)
            if (k.size() > prefix.size() && k.substr(0, prefix.size()) == prefix)
                return &v;
        return nullptr;
    }

    //------------------------------------------------------------------
    // Mutation
    //------------------------------------------------------------------

    /// Add or update an entry (key = artist|title[|duration])
    void addOrUpdate(const TrackMapEntry& entry)
    {
        if (entry.hasValidKey())
        {
            entries[entry.key()] = entry;
            ++generation;
        }
    }

    /// Remove by artist+title+optional duration
    bool remove(const juce::String& artist, const juce::String& title,
                int dur = 0)
    {
        bool erased = entries.erase(TrackMapEntry::makeKey(artist, title, dur)) > 0;
        if (erased) ++generation;
        return erased;
    }

    /// Clear all entries
    void clear() { entries.clear(); ++generation; }

    /// Apply playlist order: reorder existing tracks, add missing ones.
    /// Does NOT touch cues, triggers, offsets, or notes of existing entries.
    /// Tracks not in the playlist have their sortOrder reset to 0 (appear after playlist).
    void applyPlaylistOrder(const std::vector<TrackMapEntry>& playlist)
    {
        // Reset all existing sortOrders
        for (auto& [k, entry] : entries)
            entry.sortOrder = 0;

        // Apply playlist positions: update sortOrder on existing, add new
        int pos = 1;
        for (auto& pe : playlist)
        {
            if (!pe.hasValidKey()) continue;

            // Try exact key (artist|title|duration) first, then fallback
            // to artist|title only — duration from XML may differ from CDJ
            auto key = pe.key();
            auto it = entries.find(key);
            if (it != entries.end())
            {
                it->second.sortOrder = pos;
            }
            else if (auto* existing = findIgnoringDuration(pe.artist, pe.title))
            {
                existing->sortOrder = pos;
            }
            else
            {
                // New entry — add with playlist position
                TrackMapEntry newEntry = pe;
                newEntry.sortOrder = pos;
                entries[key] = std::move(newEntry);
            }
            ++pos;
        }
        ++generation;
    }

    //------------------------------------------------------------------
    // Iteration & info
    //------------------------------------------------------------------
    size_t size() const { return entries.size(); }
    bool   empty() const { return entries.empty(); }

    /// Get all entries as a sorted vector (by artist then title) for UI display
    std::vector<TrackMapEntry> getAllSorted() const
    {
        std::vector<TrackMapEntry> result;
        result.reserve(entries.size());
        for (auto& [k, entry] : entries)
            result.push_back(entry);

        std::sort(result.begin(), result.end(),
                  [](const TrackMapEntry& a, const TrackMapEntry& b) {
                      int cmp = a.artist.compareIgnoreCase(b.artist);
                      return cmp != 0 ? cmp < 0 : a.title.compareIgnoreCase(b.title) < 0;
                  });
        return result;
    }

    /// Lightweight variant returning const pointers -- avoids copying strings.
    /// IMPORTANT: Pointers are invalidated by ANY mutation of the TrackMap
    std::vector<const TrackMapEntry*> getAllSortedPtrs() const
    {
        std::vector<const TrackMapEntry*> result;
        result.reserve(entries.size());
        for (auto& [k, entry] : entries)
            result.push_back(&entry);

        // Sort by sortOrder first (0 = unordered, sorts after explicit positions).
        // Within the same sortOrder (or both 0), sort alphabetically by artist/title.
        std::sort(result.begin(), result.end(),
                  [](const TrackMapEntry* a, const TrackMapEntry* b) {
                      // Both have explicit order → compare by order
                      if (a->sortOrder > 0 && b->sortOrder > 0)
                          return a->sortOrder < b->sortOrder;
                      // Only one has explicit order → it comes first
                      if (a->sortOrder > 0) return true;
                      if (b->sortOrder > 0) return false;
                      // Neither has order → alphabetical
                      int cmp = a->artist.compareIgnoreCase(b->artist);
                      return cmp != 0 ? cmp < 0 : a->title.compareIgnoreCase(b->title) < 0;
                  });
        return result;
    }

    //------------------------------------------------------------------
    // Import / Export
    //------------------------------------------------------------------

    bool exportToFile(const juce::File& file) const
    {
        auto* root = new juce::DynamicObject();
        root->setProperty("version", 2);

        juce::Array<juce::var> arr;
        for (auto& [k, entry] : entries)
            arr.add(entry.toVar());

        root->setProperty("tracks", arr);

        juce::var jsonVar(root);
        return file.replaceWithText(juce::JSON::toString(jsonVar));
    }

    /// Import from a user-chosen file -- merges with existing entries.
    int importFromFile(const juce::File& file)
    {
        if (!file.existsAsFile()) return 0;

        auto parsed = juce::JSON::parse(file.loadFileAsString());
        auto* obj = parsed.getDynamicObject();
        if (!obj) return 0;

        auto* arr = obj->getProperty("tracks").getArray();
        if (!arr) return 0;

        int count = 0;
        for (auto& item : *arr)
        {
            TrackMapEntry e;
            e.fromVar(item);
            if (e.hasValidKey())
            {
                entries[e.key()] = std::move(e);
                ++count;
            }
        }
        if (count > 0) ++generation;
        return count;
    }

    //------------------------------------------------------------------
    // Direct access to the map (for advanced iteration)
    //------------------------------------------------------------------
    const std::unordered_map<std::string, TrackMapEntry>& getEntries() const { return entries; }

    uint64_t getGeneration() const { return generation; }

    //------------------------------------------------------------------
    // rekordbox XML import -- parse <DJ_PLAYLISTS> into TrackMapEntry list.
    // Returns entries with artist, title, durationSec populated.
    // Offsets and triggers are left at defaults (user configures later).
    // Artwork and waveform will be fetched from the CDJ on first play.
    //------------------------------------------------------------------
    static std::vector<TrackMapEntry> parseRekordboxXml(const juce::File& file)
    {
        return parseRekordboxXml(file, "");
    }

    /// Parse rekordbox XML export.  If playlistName is non-empty, only return
    /// tracks from that playlist in playlist order.  Otherwise return all tracks.
    static std::vector<TrackMapEntry> parseRekordboxXml(const juce::File& file,
                                                        const juce::String& playlistName)
    {
        std::vector<TrackMapEntry> result;
        auto xml = juce::XmlDocument::parse(file);
        if (!xml || xml->getTagName() != "DJ_PLAYLISTS") return result;

        auto* collection = xml->getChildByName("COLLECTION");
        if (!collection) return result;

        // Build TrackID → entry map from COLLECTION
        std::unordered_map<int, TrackMapEntry> trackById;
        for (auto* track = collection->getChildByName("TRACK");
             track != nullptr;
             track = track->getNextElementWithTagName("TRACK"))
        {
            juce::String title  = track->getStringAttribute("Name").trim();
            juce::String artist = track->getStringAttribute("Artist").trim();
            int duration        = track->getIntAttribute("TotalTime", 0);
            int trackId         = track->getIntAttribute("TrackID", 0);

            if (title.isEmpty() || trackId <= 0) continue;

            TrackMapEntry e;
            e.title       = title;
            e.artist      = artist;
            e.durationSec = duration;
            trackById[trackId] = std::move(e);
        }

        // If a playlist is requested, filter and order by playlist
        if (playlistName.isNotEmpty())
        {
            auto* playlists = xml->getChildByName("PLAYLISTS");
            if (!playlists) return result;

            // Traverse by full path (e.g. "Shows / Saturday") to handle
            // duplicate playlist names in different folders.
            // Path segments skip ROOT (same as listRekordboxPlaylists).
            juce::XmlElement* playlistNode = nullptr;
            {
                // Split path into segments on the " / " SUBSTRING.
                // (Do NOT use addTokens — it treats the second arg as a set
                //  of break CHARACTERS, which would split on every space.)
                juce::StringArray segments;
                juce::String remaining = playlistName;
                int pos = remaining.indexOf(" / ");
                while (pos >= 0)
                {
                    segments.add(remaining.substring(0, pos));
                    remaining = remaining.substring(pos + 3);
                    pos = remaining.indexOf(" / ");
                }
                segments.add(remaining);
                // Drop any empty segments (shouldn't happen but defensive)
                for (int i = segments.size() - 1; i >= 0; --i)
                    if (segments[i].isEmpty()) segments.remove(i);

                // Start from ROOT node (first Type=0 child of PLAYLISTS)
                juce::XmlElement* current = nullptr;
                for (auto* child = playlists->getFirstChildElement();
                     child != nullptr; child = child->getNextElement())
                {
                    if (child->getTagName() == "NODE"
                        && child->getIntAttribute("Type", -1) == 0)
                    { current = child; break; }
                }

                // Walk path segments: folders first, last segment is the playlist
                for (int si = 0; current != nullptr && si < segments.size(); ++si)
                {
                    bool isLast = (si == segments.size() - 1);
                    int wantType = isLast ? 1 : 0;  // 1=playlist, 0=folder
                    juce::XmlElement* found = nullptr;
                    for (auto* child = current->getFirstChildElement();
                         child != nullptr; child = child->getNextElement())
                    {
                        if (child->getTagName() == "NODE"
                            && child->getStringAttribute("Name") == segments[si]
                            && child->getIntAttribute("Type", -1) == wantType)
                        { found = child; break; }
                    }
                    current = found;
                }
                playlistNode = current;
            }
            if (!playlistNode) return result;

            // Collect tracks in playlist order
            std::unordered_set<std::string> seen;
            for (auto* tr = playlistNode->getChildByName("TRACK");
                 tr != nullptr;
                 tr = tr->getNextElementWithTagName("TRACK"))
            {
                int key = tr->getIntAttribute("Key", 0);
                auto it = trackById.find(key);
                if (it != trackById.end())
                {
                    auto k = it->second.key();
                    if (!seen.count(k))
                    {
                        seen.insert(k);
                        result.push_back(it->second);
                    }
                }
            }
            return result;
        }

        // No playlist specified — return all tracks from COLLECTION
        std::unordered_set<std::string> seen;
        for (auto& [id, entry] : trackById)
        {
            auto k = entry.key();
            if (seen.count(k)) continue;
            seen.insert(k);
            result.push_back(std::move(entry));
        }
        return result;
    }

    /// List available playlist names in a rekordbox XML file
    static juce::StringArray listRekordboxPlaylists(const juce::File& file)
    {
        juce::StringArray result;
        auto xml = juce::XmlDocument::parse(file);
        if (!xml || xml->getTagName() != "DJ_PLAYLISTS") return result;

        auto* playlists = xml->getChildByName("PLAYLISTS");
        if (!playlists) return result;

        // Recursive scan for Type=1 (playlist) nodes with entries.
        // ROOT is always the top-level folder in rekordbox — skip it in the path.
        std::function<void(juce::XmlElement*, const juce::String&)> scan =
            [&](juce::XmlElement* node, const juce::String& path)
        {
            for (auto* child = node->getFirstChildElement();
                 child != nullptr; child = child->getNextElement())
            {
                if (child->getTagName() != "NODE") continue;
                juce::String name = child->getStringAttribute("Name");
                int type = child->getIntAttribute("Type", -1);

                if (type == 1)  // playlist
                {
                    int entries = child->getIntAttribute("Entries", 0);
                    if (entries > 0)
                        result.add(path.isEmpty() ? name : path + " / " + name);
                }
                else if (type == 0)  // folder
                {
                    // Skip ROOT — it's always the top-level node in rekordbox XML
                    juce::String childPath = name.equalsIgnoreCase("ROOT") ? path
                        : (path.isEmpty() ? name : path + " / " + name);
                    scan(child, childPath);
                }
            }
        };
        scan(playlists, "");
        return result;
    }

private:
    std::unordered_map<std::string, TrackMapEntry> entries;
    uint64_t generation = 0;
};

//==============================================================================
// Generator preset -- named timecode range for the internal generator
//==============================================================================
struct GeneratorPreset
{
    juce::String name;                          // unique key, e.g. "INTRO"
    juce::String startTC = "00:00:00:00";       // HH:MM:SS:FF
    juce::String stopTC  = "00:00:00:00";       // HH:MM:SS:FF (0 = freerun)
    juce::String audioFilePath;                 // empty = no audio playback
    bool         audioLoop = false;             // loop file when reaching its end

    // Cue points -- triggers that fire when the generated TC reaches each
    // cue's positionTC.  Sorted by positionTC (compared as ms) so the
    // engine can do a forward linear scan during playback.
    std::vector<GeneratorCuePoint> cuePoints;

    std::string key() const { return name.toLowerCase().trim().toStdString(); }
    bool hasValidKey() const { return name.trim().isNotEmpty(); }

    bool hasCuePoints() const { return ! cuePoints.empty(); }

    /// Sort cue points by position (ms), with frame-rate cancellation: any
    /// reasonable fps gives the same ordering since positionTC ordering is
    /// dominated by H/M/S, frames only matter in tie-breaking.
    void sortCuePoints()
    {
        std::sort(cuePoints.begin(), cuePoints.end(),
                  [](const GeneratorCuePoint& a, const GeneratorCuePoint& b)
                  { return a.positionMs() < b.positionMs(); });
    }

    juce::var toVar() const
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty("name",          name);
        obj->setProperty("startTC",       startTC);
        obj->setProperty("stopTC",        stopTC);
        obj->setProperty("audioFilePath", audioFilePath);
        obj->setProperty("audioLoop",     audioLoop);

        if (! cuePoints.empty())
        {
            juce::Array<juce::var> arr;
            for (auto& cp : cuePoints)
                arr.add(cp.toVar());
            obj->setProperty("cuePoints", arr);
        }
        return juce::var(obj);
    }

    void fromVar(const juce::var& v)
    {
        auto* obj = v.getDynamicObject();
        if (!obj) return;
        name          = obj->getProperty("name").toString();
        startTC       = obj->getProperty("startTC").toString();
        stopTC        = obj->getProperty("stopTC").toString();
        audioFilePath = obj->getProperty("audioFilePath").toString();
        audioLoop     = (bool) obj->getProperty("audioLoop");
        if (startTC.isEmpty()) startTC = "00:00:00:00";
        if (stopTC.isEmpty())  stopTC  = "00:00:00:00";

        cuePoints.clear();
        auto cuesVar = obj->getProperty("cuePoints");
        if (auto* arr = cuesVar.getArray())
        {
            cuePoints.reserve((size_t) arr->size());
            for (auto& item : *arr)
            {
                GeneratorCuePoint cp;
                cp.fromVar(item);
                cuePoints.push_back(std::move(cp));
            }
        }
    }
};

//==============================================================================
// Generator preset map -- persistent collection of named presets
//==============================================================================
class GeneratorPresetMap
{
public:
    static juce::File getPresetFile()
    {
        auto dir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                       .getChildFile("SuperTimecodeConverter");
        dir.createDirectory();
        return dir.getChildFile("generator_presets.json");
    }

    void save() const
    {
        auto* root = new juce::DynamicObject();
        root->setProperty("version", 1);

        juce::Array<juce::var> arr;
        for (auto& [k, preset] : entries)
            arr.add(preset.toVar());

        root->setProperty("presets", arr);
        juce::var jsonVar(root);
        getPresetFile().replaceWithText(juce::JSON::toString(jsonVar));
    }

    bool load()
    {
        auto file = getPresetFile();
        if (!file.existsAsFile()) return false;

        auto parsed = juce::JSON::parse(file.loadFileAsString());
        auto* obj = parsed.getDynamicObject();
        if (!obj) return false;

        entries.clear();
        auto* arr = obj->getProperty("presets").getArray();
        if (arr)
        {
            for (auto& item : *arr)
            {
                GeneratorPreset p;
                p.fromVar(item);
                if (p.hasValidKey())
                    entries[p.key()] = std::move(p);
            }
        }
        return true;
    }

    const GeneratorPreset* find(const juce::String& name) const
    {
        auto it = entries.find(name.toLowerCase().trim().toStdString());
        return (it != entries.end()) ? &it->second : nullptr;
    }

    GeneratorPreset* find(const juce::String& name)
    {
        auto it = entries.find(name.toLowerCase().trim().toStdString());
        return (it != entries.end()) ? &it->second : nullptr;
    }

    void addOrUpdate(const GeneratorPreset& preset)
    {
        if (preset.hasValidKey())
            entries[preset.key()] = preset;
    }

    bool remove(const juce::String& name)
    {
        return entries.erase(name.toLowerCase().trim().toStdString()) > 0;
    }

    void clear() { entries.clear(); }

    size_t size() const { return entries.size(); }
    bool   empty() const { return entries.empty(); }

    std::vector<GeneratorPreset> getAllSorted() const
    {
        std::vector<GeneratorPreset> result;
        result.reserve(entries.size());
        for (auto& [k, p] : entries)
            result.push_back(p);
        std::sort(result.begin(), result.end(),
                  [](const GeneratorPreset& a, const GeneratorPreset& b) {
                      return a.name.compareIgnoreCase(b.name) < 0;
                  });
        return result;
    }

private:
    std::unordered_map<std::string, GeneratorPreset> entries;
};

//==============================================================================
// Per-engine settings
//==============================================================================
struct EngineSettings
{
    juce::String engineName = "";   // empty = default "ENGINE N"

    // Input
    juce::String inputSource = "SystemTime";
    juce::String midiInputDevice = "";
    int artnetInputInterface = 0;
    int hippotizerInputInterface = 0;
    int hippotizerTcChannel = 0;  // 0=TC1, 1=TC2
    int laNetTCInputInterface = 0;
    // Generator (internal timecode source)
    bool   generatorClockMode = true;  // true = wall clock, false = transport
    double generatorStartMs = 0.0;    // start TC in ms from midnight
    double generatorStopMs  = 0.0;    // stop TC in ms (0 = freerun)
    // Generator A/B loop (in/out points and on/off toggle, programming aid).
    // When loopEnabled and loopOutMs > loopInMs, the Generator wraps to loopInMs
    // whenever playback crosses loopOutMs.  Pressing Play with loop enabled
    // starts at loopInMs regardless of current playhead position (standard
    // DAW / Pioneer CDJ loop semantics).  Both values are absolute TC ms,
    // same coordinate system as generatorStartMs / generatorStopMs.
    double generatorLoopInMs  = 0.0;
    double generatorLoopOutMs = 0.0;
    bool   generatorLoopEnabled = false;
    // Generator audio playback (per-engine output device for preset audio files)
    juce::String generatorAudioDevice = "";
    juce::String generatorAudioType   = "";
    int          generatorAudioChannel = 0;
    bool         generatorAudioStereo  = true;
    bool         generatorAudioEnabled = false;
    float        generatorAudioVolume  = 1.0f;   // 0..1.5, 1=unity
    double       generatorAudioSampleRate = 0.0; // 0 = use global preferred
    int          generatorAudioBufferSize = 0;   // 0 = use global preferred
    int          generatorAudioFileChannelMode = 0; // 0=Stereo, 1=L only, 2=R only
    // Pro DJ Link
    int proDJLinkPlayer = 1;
    bool trackMapEnabled = false;
    bool midiClockEnabled = false;
    juce::String oscBpmAddr = "/composition/tempocontroller/tempo";
    juce::String oscBpmCmd;   // e.g. "Master 3.x at %BPM%" — if set, sends string instead of float
    bool oscBpmForward    = false;
    bool oscMixerForward  = false;
    bool midiMixerForward = false;
    int  midiMixerCCChannel = 1;    // 1-16 (CC messages)
    int  midiMixerNoteChannel = 1;  // 1-16 (Note messages)
    bool artnetMixerForward = false;
    int  artnetMixerUniverse = 0;   // 0-32767
    int  artnetTriggerUniverse = 1; // 0-32767 (separate from mixer, default 1)
    int  artnetDmxInterface = -1;  // -1 = All Interfaces (Broadcast), 0+ = specific NIC
    bool linkEnabled = false;
    juce::String audioInputDevice = "";
    juce::String audioInputType = "";
    int audioInputChannel = 0;

    // Output
    bool mtcOutEnabled = false;
    bool artnetOutEnabled = false;
    bool laNetTCOutEnabled = false;
    bool ltcOutEnabled = false;
    bool thruOutEnabled = false;       // only meaningful for engine 0
    bool tcnetOutEnabled = false;      // TCNet timecode layer output
    int  tcnetLayer = 0;               // TCNet layer index 0-3 (Layer 1-4)
    bool hippoOutEnabled = false;      // Hippotizer timecode output
    juce::String hippotizerDestIp = "255.255.255.255";  // Hippotizer destination IP

    // On-air gate: engine only active when CDJ is flagged on-air by the DJM
    bool onAirGateEnabled = false;
    juce::String midiOutputDevice = "";
    int artnetOutputInterface = 0;
    int laNetTCOutputInterface = 0;
    juce::String audioOutputDevice = "";
    juce::String audioOutputType = "";
    int audioOutputChannel = 0;
    bool audioOutputStereo = true;
    juce::String thruOutputDevice = "";
    juce::String thruOutputType = "";
    int thruOutputChannel = 1;
    bool thruOutputStereo = true;
    int thruInputChannel = 1;

    // Gain (percentage: 100 = unity)
    int ltcInputGain = 100;
    int thruInputGain = 100;
    int ltcOutputGain = 100;
    bool ltcHoldOnPause = false;
    int thruOutputGain = 100;

    // Audio BPM detection (for non-DJ sources)
    bool audioBpmEnabled = false;
    juce::String audioBpmDevice = "";
    juce::String audioBpmType = "";
    int audioBpmChannel = -1;   // -1 = stereo mix
    float audioBpmSmoothing = 0.5f;  // 0.0=fast, 1.0=stable
    int audioBpmGain = 100;  // 0-400 (percentage, same as LTC gain)

    // FPS  (0=23.976, 1=24, 2=25, 3=29.97, 4=30)
    int fpsSelection = 4;

    // FPS conversion
    bool fpsConvertEnabled = false;
    int outputFpsSelection = 4;

    // LTC user override
    bool ltcFpsUserOverride = false;

    // Output offsets (frames, -30 to +30)
    int mtcOutputOffset = 0;
    int artnetOutputOffset = 0;
    int laNetTCOutputOffset = 0;
    int ltcOutputOffset = 0;
    // LTC user bits (SMPTE 12M binary groups), stored as up-to-8-digit hex
    // text so leading zeros and the operator's exact entry are preserved.
    // Empty or unparseable -> 0 (all-zero user bits, the prior behaviour).
    juce::String ltcUserBitsHex;
    // LTC user-bits source: 0 = manual value, 1 = passthrough from LTC in,
    // 2 = system date (BCD YYYYMMDD).
    int ltcUserBitsMode = 0;
    int tcnetOutputOffsetMs = 0;   // TCNet offset in milliseconds, -1000 to +1000

    // Track change triggers -- destinations
    bool triggerMidiEnabled = false;
    juce::String triggerMidiDevice = "";    // MIDI output device for triggers (independent from MTC)
    bool triggerOscEnabled = false;
    bool artnetTriggerEnabled = false;
    juce::String oscDestIp = "127.0.0.1";
    int oscDestPort = 53000;               // QLab default

    //----------------------------------------------------------------------
    juce::var toVar() const
    {
        auto obj = new juce::DynamicObject();

        obj->setProperty("engineName", engineName);
        obj->setProperty("inputSource", inputSource);
        obj->setProperty("midiInputDevice", midiInputDevice);
        obj->setProperty("artnetInputInterface", artnetInputInterface);
        obj->setProperty("hippotizerInputInterface", hippotizerInputInterface);
        obj->setProperty("hippotizerTcChannel", hippotizerTcChannel);
        obj->setProperty("laNetTCInputInterface", laNetTCInputInterface);
        obj->setProperty("generatorClockMode", generatorClockMode);
        obj->setProperty("generatorStartMs", generatorStartMs);
        obj->setProperty("generatorStopMs", generatorStopMs);
        obj->setProperty("generatorLoopInMs",  generatorLoopInMs);
        obj->setProperty("generatorLoopOutMs", generatorLoopOutMs);
        obj->setProperty("generatorLoopEnabled", generatorLoopEnabled);
        obj->setProperty("generatorAudioDevice",  generatorAudioDevice);
        obj->setProperty("generatorAudioType",    generatorAudioType);
        obj->setProperty("generatorAudioChannel", generatorAudioChannel);
        obj->setProperty("generatorAudioStereo",  generatorAudioStereo);
        obj->setProperty("generatorAudioEnabled", generatorAudioEnabled);
        obj->setProperty("generatorAudioVolume",  (double) generatorAudioVolume);
        obj->setProperty("generatorAudioSampleRate", generatorAudioSampleRate);
        obj->setProperty("generatorAudioBufferSize", generatorAudioBufferSize);
        obj->setProperty("generatorAudioFileChannelMode", generatorAudioFileChannelMode);
        obj->setProperty("proDJLinkPlayer", proDJLinkPlayer);
        obj->setProperty("trackMapEnabled", trackMapEnabled);
        obj->setProperty("midiClockEnabled", midiClockEnabled);
        obj->setProperty("oscBpmAddr", oscBpmAddr);
        obj->setProperty("oscBpmCmd", oscBpmCmd);
        obj->setProperty("oscBpmForward",    oscBpmForward);
        obj->setProperty("oscMixerForward",  oscMixerForward);
        obj->setProperty("midiMixerForward", midiMixerForward);
        obj->setProperty("midiMixerCCChannel", midiMixerCCChannel);
        obj->setProperty("midiMixerNoteChannel", midiMixerNoteChannel);
        obj->setProperty("artnetMixerForward",  artnetMixerForward);
        obj->setProperty("artnetMixerUniverse", artnetMixerUniverse);
        obj->setProperty("artnetTriggerUniverse", artnetTriggerUniverse);
        obj->setProperty("artnetDmxInterface", artnetDmxInterface);
        obj->setProperty("linkEnabled", linkEnabled);
        obj->setProperty("audioInputDevice", audioInputDevice);
        obj->setProperty("audioInputType", audioInputType);
        obj->setProperty("audioInputChannel", audioInputChannel);

        obj->setProperty("mtcOutEnabled", mtcOutEnabled);
        obj->setProperty("artnetOutEnabled", artnetOutEnabled);
        obj->setProperty("laNetTCOutEnabled", laNetTCOutEnabled);
        obj->setProperty("ltcOutEnabled", ltcOutEnabled);
        obj->setProperty("thruOutEnabled", thruOutEnabled);
        obj->setProperty("tcnetOutEnabled", tcnetOutEnabled);
        obj->setProperty("tcnetLayer", tcnetLayer);
        if (onAirGateEnabled)
            obj->setProperty("onAirGateEnabled", onAirGateEnabled);
        obj->setProperty("hippoOutEnabled", hippoOutEnabled);
        obj->setProperty("hippotizerDestIp", hippotizerDestIp);
        obj->setProperty("midiOutputDevice", midiOutputDevice);
        obj->setProperty("artnetOutputInterface", artnetOutputInterface);
        obj->setProperty("laNetTCOutputInterface", laNetTCOutputInterface);
        obj->setProperty("audioOutputDevice", audioOutputDevice);
        obj->setProperty("audioOutputType", audioOutputType);
        obj->setProperty("audioOutputChannel", audioOutputChannel);
        obj->setProperty("audioOutputStereo", audioOutputStereo);
        obj->setProperty("thruOutputDevice", thruOutputDevice);
        obj->setProperty("thruOutputType", thruOutputType);
        obj->setProperty("thruOutputChannel", thruOutputChannel);
        obj->setProperty("thruOutputStereo", thruOutputStereo);
        obj->setProperty("thruInputChannel", thruInputChannel);

        obj->setProperty("ltcInputGain", ltcInputGain);
        obj->setProperty("thruInputGain", thruInputGain);
        obj->setProperty("ltcOutputGain", ltcOutputGain);
        obj->setProperty("ltcHoldOnPause", ltcHoldOnPause);
        obj->setProperty("thruOutputGain", thruOutputGain);

        obj->setProperty("audioBpmEnabled", audioBpmEnabled);
        obj->setProperty("audioBpmDevice", audioBpmDevice);
        obj->setProperty("audioBpmType", audioBpmType);
        obj->setProperty("audioBpmChannel", audioBpmChannel);
        obj->setProperty("audioBpmSmoothing", (double)audioBpmSmoothing);
        obj->setProperty("audioBpmGain", audioBpmGain);

        obj->setProperty("fpsSelection", fpsSelection);
        obj->setProperty("fpsConvertEnabled", fpsConvertEnabled);
        obj->setProperty("outputFpsSelection", outputFpsSelection);
        obj->setProperty("ltcFpsUserOverride", ltcFpsUserOverride);

        obj->setProperty("mtcOutputOffset", mtcOutputOffset);
        obj->setProperty("artnetOutputOffset", artnetOutputOffset);
        obj->setProperty("laNetTCOutputOffset", laNetTCOutputOffset);
        obj->setProperty("ltcOutputOffset", ltcOutputOffset);
        if (ltcUserBitsHex.isNotEmpty())
            obj->setProperty("ltcUserBitsHex", ltcUserBitsHex);
        if (ltcUserBitsMode != 0)
            obj->setProperty("ltcUserBitsMode", ltcUserBitsMode);
        obj->setProperty("tcnetOutputOffsetMs", tcnetOutputOffsetMs);

        // Track change triggers
        obj->setProperty("triggerMidiEnabled", triggerMidiEnabled);
        obj->setProperty("triggerMidiDevice", triggerMidiDevice);
        obj->setProperty("triggerOscEnabled", triggerOscEnabled);
        obj->setProperty("artnetTriggerEnabled", artnetTriggerEnabled);
        obj->setProperty("oscDestIp", oscDestIp);
        obj->setProperty("oscDestPort", oscDestPort);

        return juce::var(obj);
    }

    void fromVar(const juce::var& v)
    {
        auto* obj = v.getDynamicObject();
        if (!obj) return;

        auto getBool = [&](const char* key, bool def) {
            auto val = obj->getProperty(key);
            return val.isVoid() ? def : (bool)val;
        };
        auto getInt = [&](const char* key, int def) {
            auto val = obj->getProperty(key);
            return val.isVoid() ? def : (int)val;
        };
        auto getDouble = [&](const char* key, double def) {
            auto val = obj->getProperty(key);
            return val.isVoid() ? def : (double)val;
        };
        auto getString = [&](const char* key, const juce::String& def = {}) {
            auto val = obj->getProperty(key);
            return val.isVoid() ? def : val.toString();
        };

        engineName           = getString("engineName");
        inputSource          = getString("inputSource", "SystemTime");
        midiInputDevice      = getString("midiInputDevice");
        artnetInputInterface = getInt("artnetInputInterface", 0);
        hippotizerInputInterface = getInt("hippotizerInputInterface", 0);
        hippotizerTcChannel      = getInt("hippotizerTcChannel", 0);
        laNetTCInputInterface    = getInt("laNetTCInputInterface", 0);
        generatorClockMode       = getBool("generatorClockMode", true);
        generatorStartMs         = (double)getInt("generatorStartMs", 0);
        generatorStopMs          = (double)getInt("generatorStopMs", 0);
        generatorLoopInMs        = (double) obj->getProperty("generatorLoopInMs");
        generatorLoopOutMs       = (double) obj->getProperty("generatorLoopOutMs");
        generatorLoopEnabled     = getBool("generatorLoopEnabled", false);
        generatorAudioDevice     = getString("generatorAudioDevice");
        generatorAudioType       = getString("generatorAudioType");
        generatorAudioChannel    = getInt("generatorAudioChannel", 0);
        generatorAudioStereo     = getBool("generatorAudioStereo", true);
        generatorAudioEnabled    = getBool("generatorAudioEnabled", false);
        generatorAudioVolume     = (float) getDouble("generatorAudioVolume", 1.0);
        generatorAudioSampleRate = getDouble("generatorAudioSampleRate", 0.0);
        generatorAudioBufferSize = getInt("generatorAudioBufferSize", 0);
        generatorAudioFileChannelMode = getInt("generatorAudioFileChannelMode", 0);
        // proDJLinkPlayer ids: 1-6 = players, 7 = XF-A, 8 = XF-B, 9 = MASTER
        proDJLinkPlayer      = juce::jlimit(1, 9, getInt("proDJLinkPlayer", 1));
        trackMapEnabled      = getBool("trackMapEnabled", getBool("tcnetTrackMapEnabled", false));
        midiClockEnabled     = getBool("midiClockEnabled", getBool("tcnetMidiClock", false));
        oscBpmAddr           = getString("oscBpmAddr", getString("tcnetOscBpmAddr", "/composition/tempocontroller/tempo"));
        oscBpmCmd            = getString("oscBpmCmd");
        oscBpmForward        = getBool("oscBpmForward", getBool("tcnetOscForward", false));
        oscMixerForward      = getBool("oscMixerForward", false);
        midiMixerForward     = getBool("midiMixerForward", false);
        // Dual MIDI channels (v1.5+); legacy single channel maps to both
        {
            auto ccVal = obj->getProperty("midiMixerCCChannel");
            if (!ccVal.isVoid())
            {
                midiMixerCCChannel   = juce::jlimit(1, 16, (int)ccVal);
                midiMixerNoteChannel = juce::jlimit(1, 16, getInt("midiMixerNoteChannel", 1));
            }
            else
            {
                int legacy = juce::jlimit(1, 16, getInt("midiMixerChannel", 1));
                midiMixerCCChannel   = legacy;
                midiMixerNoteChannel = legacy;
            }
        }
        artnetMixerForward   = getBool("artnetMixerForward", false);
        artnetMixerUniverse  = juce::jlimit(0, 32767, getInt("artnetMixerUniverse", 0));
        artnetTriggerUniverse = juce::jlimit(0, 32767, getInt("artnetTriggerUniverse", 1));
        artnetDmxInterface   = getInt("artnetDmxInterface", -1);  // -1 = All Interfaces
        linkEnabled          = getBool("linkEnabled", getBool("tcnetLinkEnabled", false));
        audioInputDevice     = getString("audioInputDevice");
        audioInputType       = getString("audioInputType");
        audioInputChannel    = juce::jlimit(0, 127, getInt("audioInputChannel", 0));

        mtcOutEnabled        = getBool("mtcOutEnabled", false);
        artnetOutEnabled     = getBool("artnetOutEnabled", false);
        laNetTCOutEnabled    = getBool("laNetTCOutEnabled", false);
        ltcOutEnabled        = getBool("ltcOutEnabled", false);
        thruOutEnabled       = getBool("thruOutEnabled", false);
        tcnetOutEnabled      = getBool("tcnetOutEnabled", false);
        tcnetLayer           = juce::jlimit(0, 3, getInt("tcnetLayer", 0));
        onAirGateEnabled     = getBool("onAirGateEnabled", false);
        hippoOutEnabled      = getBool("hippoOutEnabled", false);
        hippotizerDestIp     = getString("hippotizerDestIp", "255.255.255.255");
        midiOutputDevice     = getString("midiOutputDevice");
        artnetOutputInterface = getInt("artnetOutputInterface", 0);
        laNetTCOutputInterface = getInt("laNetTCOutputInterface", 0);
        audioOutputDevice    = getString("audioOutputDevice");
        audioOutputType      = getString("audioOutputType");
        audioOutputChannel   = juce::jlimit(0, 127, getInt("audioOutputChannel", 0));
        audioOutputStereo    = getBool("audioOutputStereo", true);
        thruOutputDevice     = getString("thruOutputDevice");
        thruOutputType       = getString("thruOutputType");
        thruOutputChannel    = juce::jlimit(0, 127, getInt("thruOutputChannel", 1));
        thruOutputStereo     = getBool("thruOutputStereo", true);
        thruInputChannel     = juce::jlimit(0, 127, getInt("thruInputChannel", 1));

        auto clampGain = [](int val) { return (val < 0 || val > 200) ? 100 : val; };
        ltcInputGain   = clampGain(getInt("ltcInputGain", 100));
        thruInputGain  = clampGain(getInt("thruInputGain", 100));
        ltcOutputGain  = clampGain(getInt("ltcOutputGain", 100));
        ltcHoldOnPause = getBool("ltcHoldOnPause", false);
        thruOutputGain = clampGain(getInt("thruOutputGain", 100));

        audioBpmEnabled  = getBool("audioBpmEnabled", false);
        audioBpmDevice   = getString("audioBpmDevice");
        audioBpmType     = getString("audioBpmType");
        audioBpmChannel  = juce::jlimit(-1, 127, getInt("audioBpmChannel", -1));
        audioBpmSmoothing = (float)juce::jlimit(0.0, 1.0,
            obj->getProperty("audioBpmSmoothing").isVoid() ? 0.5 : (double)obj->getProperty("audioBpmSmoothing"));
        audioBpmGain = juce::jlimit(0, 400, getInt("audioBpmGain", 100));

        fpsSelection       = juce::jlimit(0, 4, getInt("fpsSelection", 4));
        fpsConvertEnabled  = getBool("fpsConvertEnabled", false);
        outputFpsSelection = juce::jlimit(0, 4, getInt("outputFpsSelection", 4));
        ltcFpsUserOverride = getBool("ltcFpsUserOverride", false);

        auto clampOffset = [](int val) { return juce::jlimit(-30, 30, val); };
        mtcOutputOffset    = clampOffset(getInt("mtcOutputOffset", 0));
        artnetOutputOffset = clampOffset(getInt("artnetOutputOffset", 0));
        laNetTCOutputOffset  = clampOffset(getInt("laNetTCOutputOffset", 0));
        ltcOutputOffset    = clampOffset(getInt("ltcOutputOffset", 0));
        ltcUserBitsHex     = getString("ltcUserBitsHex");
        ltcUserBitsMode    = juce::jlimit(0, 2, getInt("ltcUserBitsMode", 0));
        tcnetOutputOffsetMs = juce::jlimit(-1000, 1000, getInt("tcnetOutputOffsetMs", 0));

        // Track change triggers
        triggerMidiEnabled = getBool("triggerMidiEnabled", false);
        triggerMidiDevice  = getString("triggerMidiDevice");
        triggerOscEnabled  = getBool("triggerOscEnabled", false);
        artnetTriggerEnabled = getBool("artnetTriggerEnabled", false);
        oscDestIp          = getString("oscDestIp", "127.0.0.1");
        oscDestPort        = juce::jlimit(1, 65535, getInt("oscDestPort", 53000));
    }
};

//==============================================================================
// Application settings (global + per-engine array)
//==============================================================================
struct AppSettings
{
    // Global settings
    juce::String audioInputTypeFilter = "";
    juce::String audioOutputTypeFilter = "";
    double preferredSampleRate = 0;
    int preferredBufferSize = 0;

    // Global Pro DJ Link settings (shared connection, not per-engine)
    int  proDJLinkInterface = 0;
    // Bridge identity profile for the 54B keepalive (beta15, extended beta16).
    // Three captures of real bridges show three different byte pairs -- the
    // value is dynamic per session (STC_PRODJLINK_AUDIT.md
    // sections 3, 7 and 12), so it ships as a user setting for A/B testing:
    //   0 = F9 profile (0xF9/0x04, Bridge_Original.pcapng / A9 rig) -- default
    //   1 = C0 profile (0xC0/0x03, 2000nxs2_PDL_bridge.pcapng / NXS2 rig)
    //   2 = E4 profile (0xE4/0x05, Prueba_prodjlink_bridge.pcapng / A9+3x3000)
    //   3 = AUTO (0xE4 / b30 = max(network mark, count incl. self) -- experimental)
    int prodjlinkBridgeIdentity = 0;
    // 95B dbserver-keepalive scope (v1.9.11-beta15):
    //   0 = all discovered players (beta14 behaviour) -- default
    //   1 = CDJ-3000 models only  (v1.9.10 behaviour)
    //   2 = off                   (reference-bridge behaviour)
    // The 95B is what triggers the NXS2 TCP-12523 probe storm; the reference
    // bridge never sends it.  Must only be turned off together with (or
    // after) the C0 identity, or the v1.9.10 zero-data regression returns.
    int prodjlink95bMode = 0;
    // NOTE: the old key "prodjlinkDualIdentity" (beta5-beta14) is no longer
    // read; the dual CDJ identity was removed in beta15 (see audit sec. 6).

    // Global StageLinQ settings (independent interface from ProDJLink)
    int  stageLinQInterface = 0;

    // TCNet output (global network interface, enable is per-engine in EngineSettings)
    int  tcnetInterface = -1;    // -1 = all interfaces (broadcast 255.255.255.255)
    // Venue-wide latency compensation applied to every TCNet layer in
    // addition to each engine's per-layer offset.  Use case: HDMI / NDI
    // capture pipelines added between the laptop and the house system
    // introduce the same delay to every output engine, so dialing it in
    // once at soundcheck saves having to touch every engine's offset.
    // Sums with the per-engine TCNet offset just before SMPTE encoding,
    // so this value of 0 reproduces v1.9.11-beta6 behaviour exactly.
    // Range -2000..+2000 ms; wider than the per-engine ±1000 ms because
    // full capture/encode chains can run several hundred ms by themselves.
    int  tcnetGlobalOffsetMs = 0;

    // OSC Input (global listener for generator remote control)
    bool oscInputEnabled = false;
    int  oscInputPort = 9800;
    int  oscInputInterface = 0;

    // Window positions/sizes (persisted as "x y w h" strings, empty = default)
    juce::String mainWindowBounds;
    juce::String pdlViewBounds;
    juce::String slqViewBounds;
    juce::String trackMapBounds;
    juce::String mixerMapBounds;
    juce::String cuePointBounds;
    juce::String genPresetBounds;
    juce::String genWaveformBounds;

    // PDL View layout state
    bool pdlViewHorizontal  = false;
    bool pdlViewAlternating = false;  // 1x4 vertical stack, full-width waveforms
    bool pdlViewShowMixer   = true;

    // SLQ View layout state
    bool slqViewHorizontal  = false;

    // Per-engine settings
    std::vector<EngineSettings> engines;

    // Which engine tab was selected
    int selectedEngine = 0;

    // Show Mode lock -- prevents accidental changes during live shows
    bool showModeLocked = false;

    // Track map (Track ID -> timecode offset mapping)
    TrackMap trackMap;

    // Generator presets (named timecode ranges for the internal generator)
    GeneratorPresetMap generatorPresets;

    //==================================================================
    static juce::File getSettingsFile()
    {
        auto dir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                       .getChildFile("SuperTimecodeConverter");
        dir.createDirectory();
        return dir.getChildFile("settings.json");
    }

    void save() const
    {
        auto obj = std::make_unique<juce::DynamicObject>();

        obj->setProperty("version", 2);

        obj->setProperty("audioInputTypeFilter", audioInputTypeFilter);
        obj->setProperty("audioOutputTypeFilter", audioOutputTypeFilter);
        obj->setProperty("preferredSampleRate", preferredSampleRate);
        obj->setProperty("preferredBufferSize", preferredBufferSize);
        obj->setProperty("selectedEngine", selectedEngine);
        obj->setProperty("showModeLocked", showModeLocked);
        obj->setProperty("proDJLinkInterface", proDJLinkInterface);
        obj->setProperty("prodjlinkBridgeIdentity", prodjlinkBridgeIdentity);
        obj->setProperty("prodjlink95bMode", prodjlink95bMode);
        obj->setProperty("stageLinQInterface", stageLinQInterface);
        obj->setProperty("tcnetInterface", tcnetInterface);
        obj->setProperty("tcnetGlobalOffsetMs", tcnetGlobalOffsetMs);
        obj->setProperty("oscInputEnabled", oscInputEnabled);
        obj->setProperty("oscInputPort", oscInputPort);
        obj->setProperty("oscInputInterface", oscInputInterface);

        if (mainWindowBounds.isNotEmpty()) obj->setProperty("mainWindowBounds", mainWindowBounds);
        if (pdlViewBounds.isNotEmpty())   obj->setProperty("pdlViewBounds",   pdlViewBounds);
        if (slqViewBounds.isNotEmpty())   obj->setProperty("slqViewBounds",   slqViewBounds);
        if (trackMapBounds.isNotEmpty())   obj->setProperty("trackMapBounds",  trackMapBounds);
        if (mixerMapBounds.isNotEmpty())   obj->setProperty("mixerMapBounds",  mixerMapBounds);
        if (cuePointBounds.isNotEmpty())  obj->setProperty("cuePointBounds", cuePointBounds);
        if (genPresetBounds.isNotEmpty()) obj->setProperty("genPresetBounds", genPresetBounds);
        if (genWaveformBounds.isNotEmpty()) obj->setProperty("genWaveformBounds", genWaveformBounds);
        obj->setProperty("pdlViewHorizontal", pdlViewHorizontal);
        obj->setProperty("pdlViewAlternating", pdlViewAlternating);
        obj->setProperty("pdlViewShowMixer",  pdlViewShowMixer);
        obj->setProperty("slqViewHorizontal", slqViewHorizontal);

        juce::Array<juce::var> engineArray;
        for (auto& eng : engines)
            engineArray.add(eng.toVar());
        obj->setProperty("engines", engineArray);

        juce::var jsonVar(obj.release());
        getSettingsFile().replaceWithText(juce::JSON::toString(jsonVar));

        // TrackMap is saved to its own file (trackmap.json)
        trackMap.save();

        // Generator presets saved to generator_presets.json
        generatorPresets.save();
    }

    bool load()
    {
        auto file = getSettingsFile();
        if (!file.existsAsFile()) return false;

        auto parsed = juce::JSON::parse(file.loadFileAsString());
        auto* obj = parsed.getDynamicObject();
        if (!obj) return false;

        auto getInt = [&](const char* key, int def) {
            auto v = obj->getProperty(key);
            return v.isVoid() ? def : (int)v;
        };
        auto getDouble = [&](const char* key, double def) {
            auto v = obj->getProperty(key);
            return v.isVoid() ? def : (double)v;
        };
        auto getString = [&](const char* key, const juce::String& def = {}) {
            auto v = obj->getProperty(key);
            return v.isVoid() ? def : v.toString();
        };

        int version = getInt("version", 1);

        if (version >= 2)
        {
            audioInputTypeFilter  = getString("audioInputTypeFilter");
            audioOutputTypeFilter = getString("audioOutputTypeFilter");
            preferredSampleRate   = getDouble("preferredSampleRate", 0.0);
            preferredBufferSize   = getInt("preferredBufferSize", 0);
            selectedEngine        = getInt("selectedEngine", 0);
            {
                auto v = obj->getProperty("showModeLocked");
                showModeLocked = v.isVoid() ? false : (bool)v;
            }
            proDJLinkInterface    = getInt("proDJLinkInterface", 0);
            prodjlinkBridgeIdentity = juce::jlimit(0, 3, getInt("prodjlinkBridgeIdentity", 0));
            prodjlink95bMode        = juce::jlimit(0, 2, getInt("prodjlink95bMode", 0));
            stageLinQInterface    = getInt("stageLinQInterface", 0);
            tcnetInterface        = getInt("tcnetInterface", -1);
            tcnetGlobalOffsetMs   = juce::jlimit(-2000, 2000, getInt("tcnetGlobalOffsetMs", 0));
            oscInputEnabled       = getInt("oscInputEnabled", 0) != 0;
            oscInputPort          = juce::jlimit(1, 65535, getInt("oscInputPort", 9800));
            oscInputInterface     = getInt("oscInputInterface", 0);

            mainWindowBounds = getString("mainWindowBounds");
            pdlViewBounds   = getString("pdlViewBounds");
            slqViewBounds   = getString("slqViewBounds");
            trackMapBounds  = getString("trackMapBounds");
            mixerMapBounds  = getString("mixerMapBounds");
            cuePointBounds  = getString("cuePointBounds");
            genPresetBounds = getString("genPresetBounds");
            genWaveformBounds = getString("genWaveformBounds");
            pdlViewHorizontal  = getInt("pdlViewHorizontal", 0) != 0;
            pdlViewAlternating = getInt("pdlViewAlternating", 0) != 0;
            pdlViewShowMixer   = getInt("pdlViewShowMixer", 1) != 0;
            slqViewHorizontal  = getInt("slqViewHorizontal", 0) != 0;

            engines.clear();
            auto* engArray = obj->getProperty("engines").getArray();
            if (engArray)
            {
                for (auto& item : *engArray)
                {
                    EngineSettings es;
                    es.fromVar(item);
                    engines.push_back(es);
                }
            }

            if (engines.empty())
                engines.push_back({});

            selectedEngine = juce::jlimit(0, (int)engines.size() - 1, selectedEngine);

            // TrackMap is loaded from its own file (trackmap.json)
            trackMap.load();
            generatorPresets.load();
            return true;
        }
        else
        {
            bool ok = migrateFromV1(obj);
            trackMap.load();  // load track map even from v1 migration
            generatorPresets.load();
            return ok;
        }
    }

private:
    bool migrateFromV1(juce::DynamicObject* obj)
    {
        auto getBool = [&](const char* key, bool def) {
            auto v = obj->getProperty(key); return v.isVoid() ? def : (bool)v;
        };
        auto getInt = [&](const char* key, int def) {
            auto v = obj->getProperty(key); return v.isVoid() ? def : (int)v;
        };
        auto getDouble = [&](const char* key, double def) {
            auto v = obj->getProperty(key); return v.isVoid() ? def : (double)v;
        };
        auto getString = [&](const char* key, const juce::String& def = {}) {
            auto v = obj->getProperty(key); return v.isVoid() ? def : v.toString();
        };

        audioInputTypeFilter  = getString("audioInputTypeFilter");
        audioOutputTypeFilter = getString("audioOutputTypeFilter");
        preferredSampleRate   = getDouble("preferredSampleRate", 0.0);
        preferredBufferSize   = getInt("preferredBufferSize", 0);
        selectedEngine        = 0;

        EngineSettings es;
        es.inputSource          = getString("inputSource", "SystemTime");
        es.midiInputDevice      = getString("midiInputDevice");
        es.artnetInputInterface = getInt("artnetInputInterface", 0);
        es.hippotizerInputInterface = getInt("hippotizerInputInterface", 0);
        es.hippotizerTcChannel      = getInt("hippotizerTcChannel", 0);
        es.laNetTCInputInterface = getInt("laNetTCInputInterface", 0);
        es.generatorClockMode   = getBool("generatorClockMode", true);
        es.generatorStartMs     = (double)getInt("generatorStartMs", 0);
        es.generatorStopMs      = (double)getInt("generatorStopMs", 0);
        es.generatorLoopInMs    = (double) obj->getProperty("generatorLoopInMs");
        es.generatorLoopOutMs   = (double) obj->getProperty("generatorLoopOutMs");
        es.generatorLoopEnabled = getBool("generatorLoopEnabled", false);
        es.audioInputDevice     = getString("audioInputDevice");
        es.audioInputType       = getString("audioInputType");
        es.audioInputChannel    = juce::jlimit(0, 127, getInt("audioInputChannel", 0));

        es.mtcOutEnabled        = getBool("mtcOutEnabled", false);
        es.artnetOutEnabled     = getBool("artnetOutEnabled", false);
        es.laNetTCOutEnabled    = getBool("laNetTCOutEnabled", false);
        es.ltcOutEnabled        = getBool("ltcOutEnabled", false);
        es.thruOutEnabled       = getBool("thruOutEnabled", false);
        es.midiOutputDevice     = getString("midiOutputDevice");
        es.artnetOutputInterface = getInt("artnetOutputInterface", 0);
        es.laNetTCOutputInterface = getInt("laNetTCOutputInterface", 0);
        es.audioOutputDevice    = getString("audioOutputDevice");
        es.audioOutputType      = getString("audioOutputType");
        es.audioOutputChannel   = juce::jlimit(0, 127, getInt("audioOutputChannel", 0));
        es.audioOutputStereo    = getBool("audioOutputStereo", true);
        es.thruOutputDevice     = getString("thruOutputDevice");
        es.thruOutputType       = getString("thruOutputType");
        es.thruOutputChannel    = juce::jlimit(0, 127, getInt("thruOutputChannel", 1));
        es.thruOutputStereo     = getBool("thruOutputStereo", true);
        es.thruInputChannel     = juce::jlimit(0, 127, getInt("thruInputChannel", 1));

        auto clampGain = [](int v) { return (v < 0 || v > 200) ? 100 : v; };
        es.ltcInputGain   = clampGain(getInt("ltcInputGain", 100));
        es.thruInputGain  = clampGain(getInt("thruInputGain", 100));
        es.ltcOutputGain  = clampGain(getInt("ltcOutputGain", 100));
        es.ltcHoldOnPause = getBool("ltcHoldOnPause", false);
        es.thruOutputGain = clampGain(getInt("thruOutputGain", 100));

        es.audioBpmEnabled  = getBool("audioBpmEnabled", false);
        es.audioBpmDevice   = getString("audioBpmDevice");
        es.audioBpmType     = getString("audioBpmType");
        es.audioBpmChannel  = juce::jlimit(-1, 127, getInt("audioBpmChannel", -1));
        es.audioBpmSmoothing = (float)juce::jlimit(0.0, 1.0, getDouble("audioBpmSmoothing", 0.5));
        es.audioBpmGain = juce::jlimit(0, 400, getInt("audioBpmGain", 100));

        es.fpsSelection       = juce::jlimit(0, 4, getInt("fpsSelection", 4));
        es.fpsConvertEnabled  = getBool("fpsConvertEnabled", false);
        es.outputFpsSelection = juce::jlimit(0, 4, getInt("outputFpsSelection", 4));
        es.ltcFpsUserOverride = getBool("ltcFpsUserOverride", false);

        auto clampOffset = [](int v) { return juce::jlimit(-30, 30, v); };
        es.mtcOutputOffset    = clampOffset(getInt("mtcOutputOffset", 0));
        es.artnetOutputOffset = clampOffset(getInt("artnetOutputOffset", 0));
        es.laNetTCOutputOffset = clampOffset(getInt("laNetTCOutputOffset", 0));
        es.ltcOutputOffset    = clampOffset(getInt("ltcOutputOffset", 0));
        es.ltcUserBitsHex     = getString("ltcUserBitsHex");
        es.ltcUserBitsMode    = juce::jlimit(0, 2, getInt("ltcUserBitsMode", 0));
        es.tcnetOutputOffsetMs = juce::jlimit(-1000, 1000, getInt("tcnetOutputOffsetMs", 0));

        engines.clear();
        engines.push_back(es);
        return true;
    }

public:
    //------------------------------------------------------------------
    // Full configuration export/import (backup/restore)
    //
    // Bundles settings.json + trackmap.json + mixermap.json into a
    // single JSON file.  Import overwrites all three and reloads.
    //------------------------------------------------------------------
    static juce::var readJsonFile(const juce::File& f)
    {
        if (!f.existsAsFile()) return {};
        return juce::JSON::parse(f.loadFileAsString());
    }

    juce::var buildExportBundle() const
    {
        auto dir = getSettingsFile().getParentDirectory();
        auto* root = new juce::DynamicObject();
        root->setProperty("stc_backup_version", 1);
        root->setProperty("settings", readJsonFile(getSettingsFile()));
        root->setProperty("trackmap", readJsonFile(dir.getChildFile("trackmap.json")));
        root->setProperty("mixermap", readJsonFile(dir.getChildFile("mixermap.json")));
        root->setProperty("generator_presets", readJsonFile(dir.getChildFile("generator_presets.json")));
        return juce::var(root);
    }

    bool applyImportBundle(const juce::var& bundle)
    {
        auto* obj = bundle.getDynamicObject();
        if (!obj) return false;

        auto dir = getSettingsFile().getParentDirectory();
        auto settingsVar = obj->getProperty("settings");
        auto trackmapVar = obj->getProperty("trackmap");
        auto mixermapVar = obj->getProperty("mixermap");
        auto presetsVar  = obj->getProperty("generator_presets");

        // Write each section back to its file (only if present in bundle)
        if (!settingsVar.isVoid())
            getSettingsFile().replaceWithText(juce::JSON::toString(settingsVar));
        if (!trackmapVar.isVoid())
            dir.getChildFile("trackmap.json").replaceWithText(juce::JSON::toString(trackmapVar));
        if (!mixermapVar.isVoid())
            dir.getChildFile("mixermap.json").replaceWithText(juce::JSON::toString(mixermapVar));
        if (!presetsVar.isVoid())
            dir.getChildFile("generator_presets.json").replaceWithText(juce::JSON::toString(presetsVar));

        return true;
    }
};
