// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

#pragma once
#include <JuceHeader.h>
#include <vector>
#include <cstring>

//==============================================================================
// MixerMap -- Configurable mapping from DJM mixer parameters to MIDI CC, OSC, and Art-Net DMX.
//
// Shared across all engines (like TrackMap). Each parameter has:
//   - enabled:    whether to forward this param (default: true for faders/EQ, false for misc)
//   - oscAddress: OSC path (editable, e.g. "/mixer/ch1/fader")
//   - midiCC:     MIDI CC number (-1 = disabled)
//   - artnetCh:   Art-Net DMX channel (0 = disabled, 1-512)
//   - category/display name for the editor UI
//
// Persisted to JSON in the user app data folder alongside trackmap.json.
//==============================================================================

struct MixerMapEntry
{
    juce::String paramId;       // unique key, e.g. "ch1_fader", "crossfader"
    juce::String displayName;   // human label, e.g. "CH1 Fader"
    juce::String group;         // grouping: "Channel 1", "Master", "Beat FX", etc.
    juce::String oscAddress;    // OSC path, e.g. "/mixer/ch1"
    int          midiCC = -1;   // MIDI CC number, -1 = disabled
    int          midiNote = -1; // MIDI Note number, -1 = disabled (velocity = value)
                                // grandMA2/MA3 use Note On for executor faders:
                                // note = executor, velocity = fader position (0-127)
    int          artnetCh = 0;  // Art-Net DMX channel, 0 = disabled, 1-512
    bool         enabled = true;

    juce::var toVar() const
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty("paramId",    paramId);
        obj->setProperty("oscAddress", oscAddress);
        obj->setProperty("midiCC",     midiCC);
        obj->setProperty("midiNote",   midiNote);
        obj->setProperty("artnetCh",   artnetCh);
        obj->setProperty("enabled",    enabled);
        return juce::var(obj);
    }

    void fromVar(const juce::var& v)
    {
        if (auto* obj = v.getDynamicObject())
        {
            paramId    = obj->getProperty("paramId").toString();
            oscAddress = obj->getProperty("oscAddress").toString();
            midiCC     = (int)obj->getProperty("midiCC");
            artnetCh   = (int)obj->getProperty("artnetCh");
            enabled    = (bool)obj->getProperty("enabled");
            // midiNote: default -1 for backward compat with files saved before v1.5
            auto noteVal = obj->getProperty("midiNote");
            midiNote = noteVal.isVoid() ? -1 : (int)noteVal;
        }
    }
};

class MixerMap
{
public:
    //------------------------------------------------------------------
    // Build default parameter list with hardcoded defaults
    //------------------------------------------------------------------
    MixerMap()
    {
        buildDefaults();
    }

    //------------------------------------------------------------------
    // File location
    //------------------------------------------------------------------
    static juce::File getMixerMapFile()
    {
        auto dir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                       .getChildFile("SuperTimecodeConverter");
        dir.createDirectory();
        return dir.getChildFile("mixermap.json");
    }

    //------------------------------------------------------------------
    // Persistence
    //------------------------------------------------------------------
    void save() const
    {
        auto* root = new juce::DynamicObject();
        root->setProperty("version", 1);

        juce::Array<juce::var> arr;
        for (auto& e : entries)
            arr.add(e.toVar());

        root->setProperty("params", arr);

        juce::var jsonVar(root);
        getMixerMapFile().replaceWithText(juce::JSON::toString(jsonVar));
    }

    bool load()
    {
        auto file = getMixerMapFile();
        if (!file.existsAsFile()) return false;

        auto parsed = juce::JSON::parse(file.loadFileAsString());
        auto* obj = parsed.getDynamicObject();
        if (!obj) return false;

        // Load saved entries and overlay onto defaults
        auto* arr = obj->getProperty("params").getArray();
        if (arr)
        {
            for (auto& item : *arr)
            {
                MixerMapEntry saved;
                saved.fromVar(item);
                // Find matching default entry and update user-editable fields
                for (auto& e : entries)
                {
                    if (e.paramId == saved.paramId)
                    {
                        e.oscAddress = saved.oscAddress;
                        e.midiCC     = saved.midiCC;
                        e.midiNote   = saved.midiNote;
                        e.artnetCh   = saved.artnetCh;
                        e.enabled    = saved.enabled;
                        break;
                    }
                }
            }
        }
        return true;
    }

    //------------------------------------------------------------------
    // Access
    //------------------------------------------------------------------
    int size() const { return (int)entries.size(); }

    MixerMapEntry&       operator[](int i)       { return entries[(size_t)i]; }
    const MixerMapEntry& operator[](int i) const { return entries[(size_t)i]; }

    const MixerMapEntry* findById(const juce::String& paramId) const
    {
        for (auto& e : entries)
            if (e.paramId == paramId) return &e;
        return nullptr;
    }

    MixerMapEntry* findById(const juce::String& paramId)
    {
        for (auto& e : entries)
            if (e.paramId == paramId) return &e;
        return nullptr;
    }

    /// Reset all entries to factory defaults
    void resetToDefaults()
    {
        entries.clear();
        buildDefaults();
    }

    std::vector<MixerMapEntry>& getEntries() { return entries; }
    const std::vector<MixerMapEntry>& getEntries() const { return entries; }

private:
    std::vector<MixerMapEntry> entries;

    void buildDefaults()
    {
        entries.clear();
        entries.reserve(60);

        // --- Per-channel parameters (x4) ---
        // Default DMX layout: CH1 at DMX 1-6, CH2 at 7-12, CH3 at 13-18, CH4 at 19-24
        for (int ch = 1; ch <= 4; ++ch)
        {
            juce::String grp = "Channel " + juce::String(ch);
            juce::String pfx = "ch" + juce::String(ch);
            juce::String osc = "/mixer/ch" + juce::String(ch);
            int ccBase = (ch - 1);  // ch1=0, ch2=1, ch3=2, ch4=3
            int dmxBase = (ch - 1) * 6;  // ch1=0, ch2=6, ch3=12, ch4=18

            addEntry(pfx + "_fader",     "Fader",       grp, osc,             1 + ccBase,  dmxBase + 1, true);
            addEntry(pfx + "_trim",      "Trim",        grp, osc + "/trim",   11 + ccBase, dmxBase + 2, true);
            addEntry(pfx + "_eq_hi",     "EQ High",     grp, osc + "/eq_hi",  15 + ccBase, dmxBase + 3, true);
            addEntry(pfx + "_eq_mid",    "EQ Mid",      grp, osc + "/eq_mid", 19 + ccBase, dmxBase + 4, true);
            addEntry(pfx + "_eq_lo",     "EQ Low",      grp, osc + "/eq_lo",  23 + ccBase, dmxBase + 5, true);
            addEntry(pfx + "_color",     "Color",       grp, osc + "/color",  27 + ccBase, dmxBase + 6, true);
            addEntry(pfx + "_cue",       "CUE Button",  grp, osc + "/cue",    -1, 0, false);
            addEntry(pfx + "_input_src", "Input Source", grp, osc + "/input",  -1, 0, false);
            addEntry(pfx + "_xf_assign", "XF Assign",   grp, osc + "/xf_assign", -1, 0, false);
        }

        // --- Master / Global faders --- (DMX 25-29)
        addEntry("crossfader",    "Crossfader",    "Master", "/mixer/crossfader",  5,  25, true);
        addEntry("master_fader",  "Master Fader",  "Master", "/mixer/master",      7,  26, true);
        addEntry("master_cue",    "Master CUE",    "Master", "/mixer/master_cue",  43, 0,  false);
        addEntry("fader_curve",   "Fader Curve",   "Master", "/mixer/fader_curve", -1, 0,  false);
        addEntry("xf_curve",      "XF Curve",      "Master", "/mixer/xf_curve",   -1, 0,  false);

        // --- Monitoring --- (DMX 30-32)
        addEntry("booth",         "Booth Level",   "Monitor", "/mixer/booth",       31, 30, false);
        addEntry("hp_cue_link",   "HP Cue Link",   "Monitor", "/mixer/hp_cue_link", -1, 0,  false);
        addEntry("hp_mixing",     "HP Mixing",     "Monitor", "/mixer/hp_mixing",   33, 31, false);
        addEntry("hp_level",      "HP Level",      "Monitor", "/mixer/hp_level",    34, 32, false);

        // --- Beat FX ---
        addEntry("beatfx_select", "Beat FX Select", "Beat FX", "/mixer/beatfx_select", 40, 0, false);
        addEntry("beatfx_level",  "Beat FX Level",  "Beat FX", "/mixer/beatfx_level",  35, 0, false);
        addEntry("beatfx_on",     "Beat FX On/Off", "Beat FX", "/mixer/beatfx_on",     41, 0, false);
        addEntry("beatfx_assign", "Beat FX Assign", "Beat FX", "/mixer/beatfx_assign", -1, 0, false);
        addEntry("fx_freq_lo",    "FX Freq Lo",     "Beat FX", "/mixer/fx_freq_lo",    -1, 0, false);
        addEntry("fx_freq_mid",   "FX Freq Mid",    "Beat FX", "/mixer/fx_freq_mid",   -1, 0, false);
        addEntry("fx_freq_hi",    "FX Freq Hi",     "Beat FX", "/mixer/fx_freq_hi",    -1, 0, false);
        addEntry("send_return",   "Send/Return",    "Beat FX", "/mixer/send_return",   37, 0, false);

        // --- Color FX ---
        addEntry("colorfx_select", "Color FX Select", "Color FX", "/mixer/colorfx_select", 42, 0, false);
        addEntry("colorfx_param",  "Color FX Param",  "Color FX", "/mixer/colorfx_param",  36, 0, false);
        addEntry("colorfx_assign", "Color FX Assign", "Color FX", "/mixer/colorfx_assign", -1, 0, false);

        // --- Mic ---
        addEntry("mic_eq_hi",  "Mic EQ High", "Mic", "/mixer/mic_eq_hi", 38, 0, false);
        addEntry("mic_eq_lo",  "Mic EQ Low",  "Mic", "/mixer/mic_eq_lo", 39, 0, false);
    }

    void addEntry(const juce::String& id, const juce::String& name, const juce::String& group,
                  const juce::String& osc, int cc, int dmxCh, bool enabled)
    {
        MixerMapEntry e;
        e.paramId     = id;
        e.displayName = name;
        e.group       = group;
        e.oscAddress  = osc;
        e.midiCC      = cc;
        e.artnetCh    = dmxCh;
        e.enabled     = enabled;
        entries.push_back(std::move(e));
    }
};
