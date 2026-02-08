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
            inputSource          = obj->getProperty("inputSource").toString();
            midiInputDevice      = obj->getProperty("midiInputDevice").toString();
            artnetInputInterface = (int)obj->getProperty("artnetInputInterface");
            audioInputDevice     = obj->getProperty("audioInputDevice").toString();
            audioInputType       = obj->getProperty("audioInputType").toString();
            audioInputChannel    = (int)obj->getProperty("audioInputChannel");

            mtcOutEnabled        = (bool)obj->getProperty("mtcOutEnabled");
            artnetOutEnabled     = (bool)obj->getProperty("artnetOutEnabled");
            ltcOutEnabled        = (bool)obj->getProperty("ltcOutEnabled");
            thruOutEnabled       = (bool)obj->getProperty("thruOutEnabled");
            midiOutputDevice     = obj->getProperty("midiOutputDevice").toString();
            artnetOutputInterface = (int)obj->getProperty("artnetOutputInterface");
            audioOutputDevice    = obj->getProperty("audioOutputDevice").toString();
            audioOutputType      = obj->getProperty("audioOutputType").toString();
            audioOutputChannel   = (int)obj->getProperty("audioOutputChannel");
            audioOutputStereo    = (bool)obj->getProperty("audioOutputStereo");
            thruOutputDevice     = obj->getProperty("thruOutputDevice").toString();
            thruOutputType       = obj->getProperty("thruOutputType").toString();
            thruOutputChannel    = (int)obj->getProperty("thruOutputChannel");
            thruOutputStereo     = (bool)obj->getProperty("thruOutputStereo");
            thruInputChannel     = (int)obj->getProperty("thruInputChannel");

            auto clampGain = [](int v) { return (v <= 0 || v > 400) ? 100 : v; };
            ltcInputGain   = clampGain((int)obj->getProperty("ltcInputGain"));
            thruInputGain  = clampGain((int)obj->getProperty("thruInputGain"));
            ltcOutputGain  = clampGain((int)obj->getProperty("ltcOutputGain"));
            thruOutputGain = clampGain((int)obj->getProperty("thruOutputGain"));

            audioInputTypeFilter  = obj->getProperty("audioInputTypeFilter").toString();
            audioOutputTypeFilter = obj->getProperty("audioOutputTypeFilter").toString();

            preferredSampleRate = (double)obj->getProperty("preferredSampleRate");
            preferredBufferSize = (int)obj->getProperty("preferredBufferSize");

            fpsSelection   = (int)obj->getProperty("fpsSelection");
            return true;
        }
        return false;
    }
};
