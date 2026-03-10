// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

#pragma once
#include <JuceHeader.h>
#include <unordered_map>

//==============================================================================
// TrackMap -- maps Track IDs to timecode offsets
//==============================================================================

/// A single entry mapping a track (identified by Track ID) to a
/// timecode offset that will be applied when that track is detected as playing.
struct TrackMapEntry
{
    uint32_t     trackId         = 0;
    juce::String artist;
    juce::String title;
    juce::String timecodeOffset  = "00:00:00:00";   // HH:MM:SS:FF
    int          frameRate       = 4;                // 0=23.976, 1=24, 2=25, 3=29.97, 4=30
    juce::String notes;

    // MIDI triggers (independent -- any combination can fire simultaneously)
    int          midiChannel     = 0;       // 0-15 (displayed as 1-16), shared across all MIDI types
    int          midiNoteNum     = -1;      // Note On: note number (-1 = disabled, 0-127)
    int          midiNoteVel     = 127;     // Note On: velocity (0-127)
    int          midiCCNum       = -1;      // CC: controller number (-1 = disabled, 0-127)
    int          midiCCVal       = 127;     // CC: value (0-127)
    int          midiPC          = -1;      // Program Change number (-1 = disabled, 0-127)

    // OSC trigger (per-track: what to send when this track becomes active)
    juce::String oscAddress;                // e.g. "/cue/1/go", empty = no OSC trigger
    juce::String oscArgs;                   // typed args, e.g. "i:42 s:hello f:3.14"

    // Art-Net DMX trigger (per-track: one-shot DMX value on track change)
    int          artnetCh        = 0;       // DMX channel (0 = disabled, 1-512)
    int          artnetVal       = 255;     // DMX value (0-255)

    //------------------------------------------------------------------
    // Trigger queries
    //------------------------------------------------------------------
    bool hasMidiTrigger()   const { return midiNoteNum >= 0 || midiCCNum >= 0 || midiPC >= 0; }
    bool hasOscTrigger()    const { return oscAddress.isNotEmpty(); }
    bool hasArtnetTrigger() const { return artnetCh > 0; }
    bool hasAnyTrigger()    const { return hasMidiTrigger() || hasOscTrigger() || hasArtnetTrigger(); }

    //------------------------------------------------------------------
    juce::var toVar() const
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty("trackId",        (int64_t)trackId);
        obj->setProperty("artist",         artist);
        obj->setProperty("title",          title);
        obj->setProperty("timecodeOffset", timecodeOffset);
        obj->setProperty("frameRate",      frameRate);
        obj->setProperty("notes",          notes);

        // MIDI triggers (independent)
        obj->setProperty("midiChannel",  midiChannel);
        obj->setProperty("midiNoteNum",  midiNoteNum);
        obj->setProperty("midiNoteVel",  midiNoteVel);
        obj->setProperty("midiCCNum",    midiCCNum);
        obj->setProperty("midiCCVal",    midiCCVal);
        obj->setProperty("midiPC",       midiPC);

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

        return juce::var(obj);
    }

    void fromVar(const juce::var& v)
    {
        auto* obj = v.getDynamicObject();
        if (!obj) return;

        // Track ID stored as int64 in JSON to avoid uint32 overflow in juce::var
        auto idVal = obj->getProperty("trackId");
        trackId = idVal.isVoid() ? 0u : (uint32_t)(int64_t)idVal;

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
        notes          = getString("notes");

        // Validate timecodeOffset: must parse as valid HH:MM:SS:FF
        {
            juce::String rawOffset = getString("timecodeOffset", "00:00:00:00");
            int h, m, s, f;
            if (parseTimecodeString(rawOffset, h, m, s, f))
                timecodeOffset = rawOffset;
            else
                timecodeOffset = "00:00:00:00";  // reset malformed offsets
        }

        auto frVal = obj->getProperty("frameRate");
        frameRate = frVal.isVoid() ? 4 : juce::jlimit(0, 4, (int)frVal);

        // MIDI triggers (independent fields, v1.5+)
        auto newNoteField = obj->getProperty("midiNoteNum");
        if (!newNoteField.isVoid())
        {
            // New format: independent fields
            midiChannel = juce::jlimit(0, 15, getInt("midiChannel", 0));
            midiNoteNum = juce::jlimit(-1, 127, getInt("midiNoteNum", -1));
            midiNoteVel = juce::jlimit(0, 127, getInt("midiNoteVel", 127));
            midiCCNum   = juce::jlimit(-1, 127, getInt("midiCCNum", -1));
            midiCCVal   = juce::jlimit(0, 127, getInt("midiCCVal", 127));
            midiPC      = juce::jlimit(-1, 127, getInt("midiPC", -1));
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
            midiPC = -1;
            switch (legacyType)
            {
                case 1: midiNoteNum = v1; midiNoteVel = v2; break;  // NoteOn
                case 2: midiPC = v1; break;                          // ProgramChange
                case 3: midiCCNum = v1; midiCCVal = v2; break;      // ControlChange
                default: break;                                       // None
            }
        }

        // OSC trigger
        oscAddress = getString("oscAddress");
        oscArgs    = getString("oscArgs");

        // Art-Net DMX trigger (new in v1.5, absent in legacy files → defaults to disabled)
        artnetCh  = juce::jlimit(0, 512, getInt("artnetCh", 0));
        artnetVal = juce::jlimit(0, 255, getInt("artnetVal", 255));
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
// TrackMap -- O(1) lookup by Track ID, persisted as separate JSON file
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
        root->setProperty("version", 1);

        juce::Array<juce::var> arr;
        for (auto& [id, entry] : entries)
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
                if (e.trackId != 0)
                    entries[e.trackId] = std::move(e);
            }
        }
        ++generation;
        return true;
    }

    //------------------------------------------------------------------
    // Lookup
    //------------------------------------------------------------------

    /// Find entry by Track ID -- returns nullptr if not found
    const TrackMapEntry* find(uint32_t trackId) const
    {
        auto it = entries.find(trackId);
        return (it != entries.end()) ? &it->second : nullptr;
    }

    /// Mutable find (for editing in-place)
    TrackMapEntry* find(uint32_t trackId)
    {
        auto it = entries.find(trackId);
        return (it != entries.end()) ? &it->second : nullptr;
    }

    /// Check if a track ID exists in the map
    bool contains(uint32_t trackId) const
    {
        return entries.count(trackId) > 0;
    }

    //------------------------------------------------------------------
    // Mutation
    //------------------------------------------------------------------

    /// Add or update an entry (key = entry.trackId)
    void addOrUpdate(const TrackMapEntry& entry)
    {
        if (entry.trackId != 0)
        {
            entries[entry.trackId] = entry;
            ++generation;
        }
    }

    /// Remove by Track ID -- returns true if found and removed
    bool remove(uint32_t trackId)
    {
        bool erased = entries.erase(trackId) > 0;
        if (erased) ++generation;
        return erased;
    }

    /// Clear all entries
    void clear() { entries.clear(); ++generation; }

    //------------------------------------------------------------------
    // Iteration & info
    //------------------------------------------------------------------
    size_t size() const { return entries.size(); }
    bool   empty() const { return entries.empty(); }

    /// Get all entries as a sorted vector (by Track ID) for UI display
    std::vector<TrackMapEntry> getAllSorted() const
    {
        std::vector<TrackMapEntry> result;
        result.reserve(entries.size());
        for (auto& [id, entry] : entries)
            result.push_back(entry);

        std::sort(result.begin(), result.end(),
                  [](const TrackMapEntry& a, const TrackMapEntry& b) {
                      return a.trackId < b.trackId;
                  });
        return result;
    }

    /// Lightweight variant returning const pointers -- avoids copying strings.
    /// IMPORTANT: Pointers are invalidated by ANY mutation of the TrackMap
    /// (addOrUpdate, remove, clear, importFromFile). Callers MUST call
    /// rebuildRows() / refresh() after any mutation before accessing these pointers.
    std::vector<const TrackMapEntry*> getAllSortedPtrs() const
    {
        std::vector<const TrackMapEntry*> result;
        result.reserve(entries.size());
        for (auto& [id, entry] : entries)
            result.push_back(&entry);

        std::sort(result.begin(), result.end(),
                  [](const TrackMapEntry* a, const TrackMapEntry* b) {
                      return a->trackId < b->trackId;
                  });
        return result;
    }

    //------------------------------------------------------------------
    // Import / Export (for Step 3d UI, but API ready now)
    //------------------------------------------------------------------

    /// Export to a user-chosen file
    bool exportToFile(const juce::File& file) const
    {
        auto* root = new juce::DynamicObject();
        root->setProperty("version", 1);

        juce::Array<juce::var> arr;
        for (auto& [id, entry] : entries)
            arr.add(entry.toVar());

        root->setProperty("tracks", arr);

        juce::var jsonVar(root);
        return file.replaceWithText(juce::JSON::toString(jsonVar));
    }

    /// Import from a user-chosen file -- merges with existing entries.
    /// Returns number of entries imported.
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
            if (e.trackId != 0)
            {
                entries[e.trackId] = std::move(e);
                ++count;
            }
        }
        if (count > 0) ++generation;
        return count;
    }

    //------------------------------------------------------------------
    // Direct access to the map (for advanced iteration)
    //------------------------------------------------------------------
    const std::unordered_map<uint32_t, TrackMapEntry>& getEntries() const { return entries; }

    /// Mutation generation counter -- incremented on every structural change.
    /// Used by TrackMapEditor to detect stale pointer vectors in debug builds.
    uint64_t getGeneration() const { return generation; }

private:
    std::unordered_map<uint32_t, TrackMapEntry> entries;
    uint64_t generation = 0;   // bumped by every mutation that could invalidate pointers
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
    // Pro DJ Link
    int proDJLinkPlayer = 1;
    bool trackMapEnabled = false;
    bool midiClockEnabled = false;
    juce::String oscBpmAddr = "/composition/tempocontroller/tempo";
    bool oscBpmForward    = false;
    bool oscMixerForward  = false;
    bool midiMixerForward = false;
    int  midiMixerCCChannel = 1;    // 1-16 (CC messages)
    int  midiMixerNoteChannel = 1;  // 1-16 (Note messages)
    bool artnetMixerForward = false;
    int  artnetMixerUniverse = 0;   // 0-32767
    int  artnetTriggerUniverse = 1; // 0-32767 (separate from mixer, default 1)
    bool linkEnabled = false;
    juce::String audioInputDevice = "";
    juce::String audioInputType = "";
    int audioInputChannel = 0;

    // Output
    bool mtcOutEnabled = false;
    bool artnetOutEnabled = false;
    bool ltcOutEnabled = false;
    bool thruOutEnabled = false;       // only meaningful for engine 0
    juce::String midiOutputDevice = "";
    int artnetOutputInterface = 0;
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
    int thruOutputGain = 100;

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
    int ltcOutputOffset = 0;

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
        obj->setProperty("proDJLinkPlayer", proDJLinkPlayer);
        obj->setProperty("trackMapEnabled", trackMapEnabled);
        obj->setProperty("midiClockEnabled", midiClockEnabled);
        obj->setProperty("oscBpmAddr", oscBpmAddr);
        obj->setProperty("oscBpmForward",    oscBpmForward);
        obj->setProperty("oscMixerForward",  oscMixerForward);
        obj->setProperty("midiMixerForward", midiMixerForward);
        obj->setProperty("midiMixerCCChannel", midiMixerCCChannel);
        obj->setProperty("midiMixerNoteChannel", midiMixerNoteChannel);
        obj->setProperty("artnetMixerForward",  artnetMixerForward);
        obj->setProperty("artnetMixerUniverse", artnetMixerUniverse);
        obj->setProperty("artnetTriggerUniverse", artnetTriggerUniverse);
        obj->setProperty("linkEnabled", linkEnabled);
        obj->setProperty("audioInputDevice", audioInputDevice);
        obj->setProperty("audioInputType", audioInputType);
        obj->setProperty("audioInputChannel", audioInputChannel);

        obj->setProperty("mtcOutEnabled", mtcOutEnabled);
        obj->setProperty("artnetOutEnabled", artnetOutEnabled);
        obj->setProperty("ltcOutEnabled", ltcOutEnabled);
        obj->setProperty("thruOutEnabled", thruOutEnabled);
        obj->setProperty("midiOutputDevice", midiOutputDevice);
        obj->setProperty("artnetOutputInterface", artnetOutputInterface);
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
        obj->setProperty("thruOutputGain", thruOutputGain);

        obj->setProperty("fpsSelection", fpsSelection);
        obj->setProperty("fpsConvertEnabled", fpsConvertEnabled);
        obj->setProperty("outputFpsSelection", outputFpsSelection);
        obj->setProperty("ltcFpsUserOverride", ltcFpsUserOverride);

        obj->setProperty("mtcOutputOffset", mtcOutputOffset);
        obj->setProperty("artnetOutputOffset", artnetOutputOffset);
        obj->setProperty("ltcOutputOffset", ltcOutputOffset);

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
        auto getString = [&](const char* key, const juce::String& def = {}) {
            auto val = obj->getProperty(key);
            return val.isVoid() ? def : val.toString();
        };

        engineName           = getString("engineName");
        inputSource          = getString("inputSource", "SystemTime");
        midiInputDevice      = getString("midiInputDevice");
        artnetInputInterface = getInt("artnetInputInterface", 0);
        proDJLinkPlayer      = juce::jlimit(1, 6, getInt("proDJLinkPlayer", 1));
        trackMapEnabled      = getBool("trackMapEnabled", getBool("tcnetTrackMapEnabled", false));
        midiClockEnabled     = getBool("midiClockEnabled", getBool("tcnetMidiClock", false));
        oscBpmAddr           = getString("oscBpmAddr", getString("tcnetOscBpmAddr", "/composition/tempocontroller/tempo"));
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
        linkEnabled          = getBool("linkEnabled", getBool("tcnetLinkEnabled", false));
        audioInputDevice     = getString("audioInputDevice");
        audioInputType       = getString("audioInputType");
        audioInputChannel    = juce::jlimit(0, 127, getInt("audioInputChannel", 0));

        mtcOutEnabled        = getBool("mtcOutEnabled", false);
        artnetOutEnabled     = getBool("artnetOutEnabled", false);
        ltcOutEnabled        = getBool("ltcOutEnabled", false);
        thruOutEnabled       = getBool("thruOutEnabled", false);
        midiOutputDevice     = getString("midiOutputDevice");
        artnetOutputInterface = getInt("artnetOutputInterface", 0);
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
        thruOutputGain = clampGain(getInt("thruOutputGain", 100));

        fpsSelection       = juce::jlimit(0, 4, getInt("fpsSelection", 4));
        fpsConvertEnabled  = getBool("fpsConvertEnabled", false);
        outputFpsSelection = juce::jlimit(0, 4, getInt("outputFpsSelection", 4));
        ltcFpsUserOverride = getBool("ltcFpsUserOverride", false);

        auto clampOffset = [](int val) { return juce::jlimit(-30, 30, val); };
        mtcOutputOffset    = clampOffset(getInt("mtcOutputOffset", 0));
        artnetOutputOffset = clampOffset(getInt("artnetOutputOffset", 0));
        ltcOutputOffset    = clampOffset(getInt("ltcOutputOffset", 0));

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

    // Per-engine settings
    std::vector<EngineSettings> engines;

    // Which engine tab was selected
    int selectedEngine = 0;

    // Track map (Track ID -> timecode offset mapping)
    TrackMap trackMap;

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
        obj->setProperty("proDJLinkInterface", proDJLinkInterface);

        juce::Array<juce::var> engineArray;
        for (auto& eng : engines)
            engineArray.add(eng.toVar());
        obj->setProperty("engines", engineArray);

        juce::var jsonVar(obj.release());
        getSettingsFile().replaceWithText(juce::JSON::toString(jsonVar));

        // TrackMap is saved to its own file (trackmap.json)
        trackMap.save();
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
            proDJLinkInterface    = getInt("proDJLinkInterface", 0);

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
            return true;
        }
        else
        {
            bool ok = migrateFromV1(obj);
            trackMap.load();  // load track map even from v1 migration
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
        es.audioInputDevice     = getString("audioInputDevice");
        es.audioInputType       = getString("audioInputType");
        es.audioInputChannel    = juce::jlimit(0, 127, getInt("audioInputChannel", 0));

        es.mtcOutEnabled        = getBool("mtcOutEnabled", false);
        es.artnetOutEnabled     = getBool("artnetOutEnabled", false);
        es.ltcOutEnabled        = getBool("ltcOutEnabled", false);
        es.thruOutEnabled       = getBool("thruOutEnabled", false);
        es.midiOutputDevice     = getString("midiOutputDevice");
        es.artnetOutputInterface = getInt("artnetOutputInterface", 0);
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
        es.thruOutputGain = clampGain(getInt("thruOutputGain", 100));

        es.fpsSelection       = juce::jlimit(0, 4, getInt("fpsSelection", 4));
        es.fpsConvertEnabled  = getBool("fpsConvertEnabled", false);
        es.outputFpsSelection = juce::jlimit(0, 4, getInt("outputFpsSelection", 4));
        es.ltcFpsUserOverride = getBool("ltcFpsUserOverride", false);

        auto clampOffset = [](int v) { return juce::jlimit(-30, 30, v); };
        es.mtcOutputOffset    = clampOffset(getInt("mtcOutputOffset", 0));
        es.artnetOutputOffset = clampOffset(getInt("artnetOutputOffset", 0));
        es.ltcOutputOffset    = clampOffset(getInt("ltcOutputOffset", 0));

        engines.clear();
        engines.push_back(es);
        return true;
    }
};
