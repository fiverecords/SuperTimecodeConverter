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

/// DJM model compatibility level for mixer parameters.
/// Parameters are visible in the MixerMap editor when the detected (or selected)
/// DJM model meets or exceeds the minimum level.
///   All     = present on all DJMs (900NXS2, A9, V10)
///   A9Plus  = A9 and V10 only (dual CUE, HP B, HP Pre EQ)
///   V10Only = V10 only (6ch, 4-band EQ, compressor, send, isolator, filter, master mix, multi I/O)
enum class DjmModel : int { All = 0, A9Plus = 1, V10Only = 2 };

/// Convert a DJM model name string (from ProDJLinkInput::getDJMModel()) to a DjmModel tier.
/// Used by MixerMapEditor, ProDJLinkView, and MainComponent for model-aware filtering.
inline DjmModel djmModelFromString(const juce::String& modelName)
{
    if (modelName.containsIgnoreCase("V10")) return DjmModel::V10Only;
    if (modelName.containsIgnoreCase("A9"))  return DjmModel::A9Plus;
    return DjmModel::All;  // 900NXS2 and unknown models
}

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
    DjmModel     minModel = DjmModel::All;  // minimum DJM model required (not serialized; set by buildDefaults)

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
        entries.reserve(128);

        // Shorthand aliases
        constexpr auto A  = DjmModel::All;
        constexpr auto A9 = DjmModel::A9Plus;
        constexpr auto V  = DjmModel::V10Only;

        // --- Per-channel parameters (up to 6 for V10 support) ---
        // Default DMX layout: 6 params per channel, packed sequentially.
        // CC layout: channels 1-4 use CCs 1-30, channels 5-6 use CCs 50-61.
        //
        // Model compatibility:
        //   CH1-4 base params (fader..xf_assign): All DJMs
        //   CH1-4 comp, eq_lo_mid, send:          V10 only (A9 is 3-band, no comp/send)
        //   CH1-4 cue_b:                          A9+ (A9 and V10 have dual CUE)
        //   CH5-6 all params:                     V10 only (A9 has 4 channels)
        static constexpr int kChCount = 6;  // max mixer channels (V10)
        for (int ch = 1; ch <= kChCount; ++ch)
        {
            bool isV10Ch = (ch > 4);  // CH5, CH6 are V10-only
            juce::String grp = isV10Ch ? ("Channel " + juce::String(ch) + " (V10)")
                                       : ("Channel " + juce::String(ch));
            juce::String pfx = "ch" + juce::String(ch);
            juce::String osc = "/mixer/ch" + juce::String(ch);
            int ccBase = (ch <= 4) ? (ch - 1) : (50 + (ch - 5) * 6);
            int dmxBase = (ch - 1) * 6;

            int ccFader = (ch <= 4) ? (1 + (ch - 1))  : ccBase;
            int ccTrim  = (ch <= 4) ? (11 + (ch - 1)) : ccBase + 1;
            int ccEqHi  = (ch <= 4) ? (15 + (ch - 1)) : ccBase + 2;
            int ccEqMid = (ch <= 4) ? (19 + (ch - 1)) : ccBase + 3;
            int ccEqLo  = (ch <= 4) ? (23 + (ch - 1)) : ccBase + 4;
            int ccColor = (ch <= 4) ? (27 + (ch - 1)) : ccBase + 5;

            DjmModel chBase = isV10Ch ? V : A;   // base params: All for CH1-4, V10 for CH5-6
            addEntry(pfx + "_fader",     "Fader",       grp, osc,             ccFader,  dmxBase + 1, !isV10Ch, chBase);
            addEntry(pfx + "_trim",      "Trim",        grp, osc + "/trim",   ccTrim,   dmxBase + 2, !isV10Ch, chBase);
            addEntry(pfx + "_eq_hi",     "EQ High",     grp, osc + "/eq_hi",  ccEqHi,   dmxBase + 3, !isV10Ch, chBase);
            addEntry(pfx + "_eq_mid",    "EQ Mid",      grp, osc + "/eq_mid", ccEqMid,  dmxBase + 4, !isV10Ch, chBase);
            addEntry(pfx + "_eq_lo",     "EQ Low",      grp, osc + "/eq_lo",  ccEqLo,   dmxBase + 5, !isV10Ch, chBase);
            addEntry(pfx + "_color",     "Color",       grp, osc + "/color",  ccColor,  dmxBase + 6, !isV10Ch, chBase);
            addEntry(pfx + "_cue",       "CUE Button",  grp, osc + "/cue",    -1, 0, false, chBase);
            addEntry(pfx + "_input_src", "Input Source", grp, osc + "/input",  -1, 0, false, chBase);
            addEntry(pfx + "_xf_assign", "XF Assign",   grp, osc + "/xf_assign", -1, 0, false, chBase);
            // V10 per-channel: compressor, 4-band EQ lo-mid, per-channel send (0 on 900NXS2 and A9)
            addEntry(pfx + "_comp",      "Compressor",  grp, osc + "/comp",       -1, 0, false, V);
            addEntry(pfx + "_eq_lo_mid", "EQ Lo Mid",   grp, osc + "/eq_lo_mid",  -1, 0, false, V);
            addEntry(pfx + "_send",      "Send",        grp, osc + "/send",       -1, 0, false, V);
            // Dual CUE: A9 and V10 (0 on 900NXS2)
            addEntry(pfx + "_cue_b",     "CUE B (A9/V10)", grp, osc + "/cue_b",      -1, 0, false, isV10Ch ? V : A9);
        }

        // --- Master / Global faders --- (DMX after all channels)
        int masterDmxBase = kChCount * 6 + 1;  // 37
        addEntry("crossfader",    "Crossfader",    "Master", "/mixer/crossfader",  5,  masterDmxBase,     true);
        addEntry("master_fader",  "Master Fader",  "Master", "/mixer/master",      7,  masterDmxBase + 1, true);
        addEntry("master_cue",    "Master CUE",    "Master", "/mixer/master_cue",  43, 0,  false);
        addEntry("fader_curve",   "Fader Curve",   "Master", "/mixer/fader_curve", -1, 0,  false);
        addEntry("xf_curve",      "XF Curve",      "Master", "/mixer/xf_curve",   -1, 0,  false);

        // --- Monitoring ---
        addEntry("booth",         "Booth Level",   "Monitor", "/mixer/booth",       31, masterDmxBase + 5, false);
        addEntry("hp_cue_link",   "HP Cue Link",   "Monitor", "/mixer/hp_cue_link", -1, 0,  false);
        addEntry("hp_mixing",     "HP Mixing",     "Monitor", "/mixer/hp_mixing",   33, masterDmxBase + 6, false);
        addEntry("hp_level",      "HP Level",      "Monitor", "/mixer/hp_level",    34, masterDmxBase + 7, false);

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

        // --- A9+ globals (A9 and V10: dual CUE, headphone B, booth EQ) ---
        addEntry("master_cue_b",     "Master CUE B",       "Master (A9/V10)",  "/mixer/master_cue_b",      -1, 0, false, A9);
        addEntry("hp_b_cue_link",    "HP B Cue Link",      "Monitor (A9/V10)", "/mixer/hp_b_cue_link",     -1, 0, false, A9);
        addEntry("hp_b_mixing",      "HP B Mixing",        "Monitor (A9/V10)", "/mixer/hp_b_mixing",       -1, 0, false, A9);
        addEntry("hp_b_level",       "HP B Level",         "Monitor (A9/V10)", "/mixer/hp_b_level",        -1, 0, false, A9);
        addEntry("booth_eq_hi",      "Booth EQ Hi",        "Monitor (A9/V10)", "/mixer/booth_eq_hi",       -1, 0, false, A9);
        addEntry("booth_eq_lo",      "Booth EQ Lo",        "Monitor (A9/V10)", "/mixer/booth_eq_lo",       -1, 0, false, A9);
        addEntry("booth_eq",     "Booth EQ",       "Monitor (A9/V10)", "/mixer/booth_eq",      -1, 0, false, A9);

        // --- V10-only globals (0 on 900NXS2 and A9) ---
        addEntry("isolator_on",      "Isolator On",        "Isolator (V10)", "/mixer/isolator_on",       -1, 0, false, V);
        addEntry("isolator_hi",      "Isolator Hi",        "Isolator (V10)", "/mixer/isolator_hi",       -1, 0, false, V);
        addEntry("isolator_mid",     "Isolator Mid",       "Isolator (V10)", "/mixer/isolator_mid",      -1, 0, false, V);
        addEntry("isolator_lo",      "Isolator Lo",        "Isolator (V10)", "/mixer/isolator_lo",       -1, 0, false, V);
        addEntry("filter_lpf",       "Filter LPF",         "Filter (V10)",   "/mixer/filter_lpf",        -1, 0, false, V);
        addEntry("filter_hpf",       "Filter HPF",         "Filter (V10)",   "/mixer/filter_hpf",        -1, 0, false, V);
        addEntry("filter_resonance", "Filter Resonance",   "Filter (V10)",   "/mixer/filter_resonance",  -1, 0, false, V);
        addEntry("send_ext1",        "Send Ext1",          "Sends (V10)",    "/mixer/send_ext1",         -1, 0, false, V);
        addEntry("send_ext2",        "Send Ext2",          "Sends (V10)",    "/mixer/send_ext2",         -1, 0, false, V);
        addEntry("master_mix_on",    "Master Mix On",      "Sends (V10)",    "/mixer/master_mix_on",     -1, 0, false, V);
        addEntry("master_mix_size",  "Master Mix Size",    "Sends (V10)",    "/mixer/master_mix_size",   -1, 0, false, V);
        addEntry("master_mix_time",  "Master Mix Time",    "Sends (V10)",    "/mixer/master_mix_time",   -1, 0, false, V);
        addEntry("master_mix_tone",  "Master Mix Tone",    "Sends (V10)",    "/mixer/master_mix_tone",   -1, 0, false, V);
        addEntry("master_mix_level", "Master Mix Level",   "Sends (V10)",    "/mixer/master_mix_level",  -1, 0, false, V);
        addEntry("multi_io_select",  "Multi I/O Select",   "Multi I/O (A9/V10)","/mixer/multi_io_select",   -1, 0, false, A9);
        addEntry("multi_io_level",   "Multi I/O Level",    "Multi I/O (A9/V10)","/mixer/multi_io_level",    -1, 0, false, A9);
    }

    void addEntry(const juce::String& id, const juce::String& name, const juce::String& group,
                  const juce::String& osc, int cc, int dmxCh, bool enabled,
                  DjmModel model = DjmModel::All)
    {
        MixerMapEntry e;
        e.paramId     = id;
        e.displayName = name;
        e.group       = group;
        e.oscAddress  = osc;
        e.midiCC      = cc;
        e.artnetCh    = dmxCh;
        e.enabled     = enabled;
        e.minModel    = model;
        entries.push_back(std::move(e));
    }
};
