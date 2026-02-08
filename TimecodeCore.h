#pragma once
#include <JuceHeader.h>

struct Timecode
{
    int hours   = 0;    // 0-23
    int minutes = 0;    // 0-59
    int seconds = 0;    // 0-59
    int frames  = 0;    // 0-29 depending on fps

    juce::String toString() const
    {
        return juce::String::formatted("%02d:%02d:%02d.%02d", hours, minutes, seconds, frames);
    }
};

enum class FrameRate
{
    FPS_24    = 0,
    FPS_25    = 1,
    FPS_2997  = 2,
    FPS_30    = 3
};

inline double frameRateToDouble(FrameRate fps)
{
    switch (fps)
    {
        case FrameRate::FPS_24:   return 24.0;
        case FrameRate::FPS_25:   return 25.0;
        case FrameRate::FPS_2997: return 29.97;
        case FrameRate::FPS_30:   return 30.0;
        default:                  return 30.0;
    }
}

inline int frameRateToInt(FrameRate fps)
{
    switch (fps)
    {
        case FrameRate::FPS_24:   return 24;
        case FrameRate::FPS_25:   return 25;
        case FrameRate::FPS_2997: return 30;
        case FrameRate::FPS_30:   return 30;
        default:                  return 30;
    }
}

inline juce::String frameRateToString(FrameRate fps)
{
    switch (fps)
    {
        case FrameRate::FPS_24:   return "24";
        case FrameRate::FPS_25:   return "25";
        case FrameRate::FPS_2997: return "29.97";
        case FrameRate::FPS_30:   return "30";
        default:                  return "30";
    }
}

//==============================================================================
// Audio device entry with device type information
//==============================================================================
struct AudioDeviceEntry
{
    juce::String typeName;      // JUCE type name ("Windows Audio", "ASIO", etc.)
    juce::String deviceName;    // Raw device name
    juce::String displayName;   // "WASAPI: Device Name" for UI

    // Convert JUCE internal type name to short UI-friendly prefix
    static juce::String shortenTypeName(const juce::String& name)
    {
        // Exact matches first
        if (name == "Windows Audio")    return "WASAPI";
        if (name == "ASIO")             return "ASIO";
        if (name == "DirectSound")      return "DirectSound";
        if (name == "CoreAudio")        return "";

        // WASAPI variants â€” JUCE may use different parenthetical suffixes
        // e.g. "Windows Audio (Exclusive Mode)", "Windows Audio (Exclusive)",
        //      "Windows Audio (Low Latency)"
        if (name.startsWith("Windows Audio"))
        {
            if (name.contains("("))
            {
                auto paren = name.fromFirstOccurrenceOf("(", false, false)
                                 .upToFirstOccurrenceOf(")", false, false).trim();
                if (paren.containsIgnoreCase("Exclusive"))  return "WAS.Excl";
                if (paren.containsIgnoreCase("Low"))        return "WAS.LowLat";
                return "WAS." + paren;
            }
            return "WASAPI";
        }

        // Unknown type â€” use full name
        return name;
    }

    static juce::String makeDisplayName(const juce::String& typeName, const juce::String& deviceName)
    {
        auto prefix = shortenTypeName(typeName);
        return prefix.isEmpty() ? deviceName : (prefix + ": " + deviceName);
    }
};
