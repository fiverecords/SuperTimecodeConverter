// Super Timecode Converter
// Copyright (c) 2026 Fiverecords — MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

#pragma once
#include <JuceHeader.h>

struct AppSettings
{
    // Input
    juce::String inputSource = "SystemTime";
    juce::String midiInputDevice = "";
    int artnetInputInterface = 0;
    juce::String audioInputDevice = "";
    juce::String audioInputType = "";       // Audio device type (WASAPI, ASIO, etc.)
    int audioInputChannel = 0;

    // Output
    bool mtcOutEnabled = false;
    bool artnetOutEnabled = false;
    bool ltcOutEnabled = false;
    bool thruOutEnabled = false;
    juce::String midiOutputDevice = "";
    int artnetOutputInterface = -1;
    juce::String audioOutputDevice = "";
    juce::String audioOutputType = "";      // Audio device type
    int audioOutputChannel = 0;
    bool audioOutputStereo = true;
    juce::String thruOutputDevice = "";
    juce::String thruOutputType = "";       // Audio device type
    int thruOutputChannel = 1;
    bool thruOutputStereo = true;
    int thruInputChannel = 1;

    // Gain (percentage: 100 = unity)
    int ltcInputGain = 100;
    int thruInputGain = 100;
    int ltcOutputGain = 100;
    int thruOutputGain = 100;

    // Driver type filters (empty = show all)
    juce::String audioInputTypeFilter = "";
    juce::String audioOutputTypeFilter = "";

    // Audio engine (0 = device default)
    double preferredSampleRate = 0;
    int preferredBufferSize = 0;

    // FPS
    int fpsSelection = 3;

    // Output offsets (frames, -30 to +30)
    int mtcOutputOffset = 0;
    int artnetOutputOffset = 0;
    int ltcOutputOffset = 0;

    //==============================================================================
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

        obj->setProperty("audioInputTypeFilter", audioInputTypeFilter);
        obj->setProperty("audioOutputTypeFilter", audioOutputTypeFilter);

        obj->setProperty("preferredSampleRate", preferredSampleRate);
        obj->setProperty("preferredBufferSize", preferredBufferSize);

        obj->setProperty("fpsSelection", fpsSelection);

        obj->setProperty("mtcOutputOffset", mtcOutputOffset);
        obj->setProperty("artnetOutputOffset", artnetOutputOffset);
        obj->setProperty("ltcOutputOffset", ltcOutputOffset);

        juce::var jsonVar(obj.release());
        getSettingsFile().replaceWithText(juce::JSON::toString(jsonVar));
    }

    bool load()
    {
        auto file = getSettingsFile();
        if (!file.existsAsFile()) return false;

        auto parsed = juce::JSON::parse(file.loadFileAsString());
        if (auto* obj = parsed.getDynamicObject())
        {
            // Helpers for safe loading with defaults (handles missing keys from older versions)
            auto getBool = [&](const char* key, bool def) {
                auto v = obj->getProperty(key);
                return v.isVoid() ? def : (bool)v;
            };
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

            inputSource          = getString("inputSource", "SystemTime");
            midiInputDevice      = getString("midiInputDevice");
            artnetInputInterface = getInt("artnetInputInterface", 0);
            audioInputDevice     = getString("audioInputDevice");
            audioInputType       = getString("audioInputType");
            audioInputChannel    = getInt("audioInputChannel", 0);

            mtcOutEnabled        = getBool("mtcOutEnabled", false);
            artnetOutEnabled     = getBool("artnetOutEnabled", false);
            ltcOutEnabled        = getBool("ltcOutEnabled", false);
            thruOutEnabled       = getBool("thruOutEnabled", false);
            midiOutputDevice     = getString("midiOutputDevice");
            artnetOutputInterface = getInt("artnetOutputInterface", -1);
            audioOutputDevice    = getString("audioOutputDevice");
            audioOutputType      = getString("audioOutputType");
            audioOutputChannel   = getInt("audioOutputChannel", 0);
            audioOutputStereo    = getBool("audioOutputStereo", true);
            thruOutputDevice     = getString("thruOutputDevice");
            thruOutputType       = getString("thruOutputType");
            thruOutputChannel    = getInt("thruOutputChannel", 1);
            thruOutputStereo     = getBool("thruOutputStereo", true);
            thruInputChannel     = getInt("thruInputChannel", 1);

            auto clampGain = [](int v) { return (v < 0 || v > 200) ? 100 : v; };
            ltcInputGain   = clampGain(getInt("ltcInputGain", 100));
            thruInputGain  = clampGain(getInt("thruInputGain", 100));
            ltcOutputGain  = clampGain(getInt("ltcOutputGain", 100));
            thruOutputGain = clampGain(getInt("thruOutputGain", 100));

            audioInputTypeFilter  = getString("audioInputTypeFilter");
            audioOutputTypeFilter = getString("audioOutputTypeFilter");

            preferredSampleRate = getDouble("preferredSampleRate", 0.0);
            preferredBufferSize = getInt("preferredBufferSize", 0);

            fpsSelection   = juce::jlimit(0, 3, getInt("fpsSelection", 3));

            auto clampOffset = [](int v) { return juce::jlimit(-30, 30, v); };
            mtcOutputOffset    = clampOffset(getInt("mtcOutputOffset", 0));
            artnetOutputOffset = clampOffset(getInt("artnetOutputOffset", 0));
            ltcOutputOffset    = clampOffset(getInt("ltcOutputOffset", 0));

            return true;
        }
        return false;
    }
};
