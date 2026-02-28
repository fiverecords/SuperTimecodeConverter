// Super Timecode Converter
// Copyright (c) 2026 Fiverecords — MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

#pragma once
#include <JuceHeader.h>

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

    //----------------------------------------------------------------------
    juce::var toVar() const
    {
        auto obj = new juce::DynamicObject();

        obj->setProperty("engineName", engineName);
        obj->setProperty("inputSource", inputSource);
        obj->setProperty("midiInputDevice", midiInputDevice);
        obj->setProperty("artnetInputInterface", artnetInputInterface);
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

    // Per-engine settings
    std::vector<EngineSettings> engines;

    // Which engine tab was selected
    int selectedEngine = 0;

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

        juce::Array<juce::var> engineArray;
        for (auto& eng : engines)
            engineArray.add(eng.toVar());
        obj->setProperty("engines", engineArray);

        juce::var jsonVar(obj.release());
        getSettingsFile().replaceWithText(juce::JSON::toString(jsonVar));
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
            return true;
        }
        else
        {
            return migrateFromV1(obj);
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
