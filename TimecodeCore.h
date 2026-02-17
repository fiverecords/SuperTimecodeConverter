// Super Timecode Converter
// Copyright (c) 2026 Fiverecords — MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

#pragma once
#include <JuceHeader.h>
#include <atomic>

// Several protocol handlers use std::atomic<double> for cross-thread timing.
// Verify the platform provides lock-free atomics for double so we don't
// inadvertently introduce mutex contention on the audio or timer threads.
// This requires a 64-bit platform (x86_64, ARM64, etc.).
static_assert(std::atomic<double>::is_always_lock_free,
              "This project requires a 64-bit platform for lock-free atomic<double>");

enum class FrameRate
{
    FPS_2398  = 0,  // 23.976 (24000/1001) — cinema/digital workflows
    FPS_24    = 1,
    FPS_25    = 2,
    FPS_2997  = 3,
    FPS_30    = 4
};

// std::atomic<FrameRate> is used in several protocol handlers for cross-thread
// frame rate updates.  Verify it is lock-free (guaranteed on 64-bit platforms
// for any enum backed by a 4-byte int, but worth asserting explicitly).
static_assert(std::atomic<FrameRate>::is_always_lock_free,
              "This project requires lock-free atomic<FrameRate>");

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

    // SMPTE-standard display: uses ';' as frame separator for drop-frame,
    // ':' for non-drop-frame (broadcast convention per SMPTE ST 12-1)
    // Clamps values to valid SMPTE ranges to prevent garbled display from
    // corrupt or uninitialised data.
    juce::String toDisplayString(FrameRate /*fps*/) const
    {
        int h = juce::jlimit(0, 23, hours);
        int m = juce::jlimit(0, 59, minutes);
        int s = juce::jlimit(0, 59, seconds);
        int f = juce::jlimit(0, 29, frames);
        // Use '.' as the frame separator for all frame rates.
        // This visually distinguishes the frame count from the HH:MM:SS time
        // fields (which always use ':').
        // Note: SMPTE ST 12-1 recommends ';' for drop-frame, but this app
        // intentionally uses '.' for visual clarity in all modes.
        return juce::String::formatted("%02d:%02d:%02d.%02d", h, m, s, f);
    }
};

inline double frameRateToDouble(FrameRate fps)
{
    switch (fps)
    {
        case FrameRate::FPS_2398: return 24000.0 / 1001.0;   // exact 23.976023976... (not truncated 23.976)
        case FrameRate::FPS_24:   return 24.0;
        case FrameRate::FPS_25:   return 25.0;
        case FrameRate::FPS_2997: return 30000.0 / 1001.0;   // exact 29.970029970... (consistent with DF math)
        case FrameRate::FPS_30:   return 30.0;
        default:                  return 30.0;
    }
}

inline int frameRateToInt(FrameRate fps)
{
    switch (fps)
    {
        case FrameRate::FPS_2398: return 24;
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
        case FrameRate::FPS_2398: return "23.976";
        case FrameRate::FPS_24:   return "24";
        case FrameRate::FPS_25:   return "25";
        case FrameRate::FPS_2997: return "29.97";
        case FrameRate::FPS_30:   return "30";
        default:                  return "30";
    }
}

//==============================================================================
// Increment a timecode by one frame, wrapping at 24h.
// For 29.97 drop-frame: skips frames 0 and 1 at the start of each
// minute that is NOT a multiple of 10 (SMPTE 12M standard).
//==============================================================================
inline Timecode incrementFrame(const Timecode& tc, FrameRate fps)
{
    int maxFrames = frameRateToInt(fps);
    Timecode r = tc;
    r.frames++;
    if (r.frames >= maxFrames) { r.frames = 0; r.seconds++; }
    if (r.seconds >= 60)       { r.seconds = 0; r.minutes++; }
    if (r.minutes >= 60)       { r.minutes = 0; r.hours++; }
    if (r.hours >= 24)         { r.hours = 0; }

    // Drop-frame: skip frames 0 and 1 at the start of each minute
    // except every 10th minute (00, 10, 20, 30, 40, 50)
    if (fps == FrameRate::FPS_2997
        && r.frames == 0
        && r.seconds == 0
        && (r.minutes % 10) != 0)
    {
        r.frames = 2;
    }

    return r;
}

//==============================================================================
// Source activity timeout: if no data arrives within this window,
// the source is considered paused.  MTC at 24fps sends QF every ~10ms,
// Art-Net at 30fps sends a packet every ~33ms, LTC frames arrive every
// ~33-42ms.  150ms covers several missed frames with margin.
//==============================================================================
inline constexpr double kSourceTimeoutMs = 150.0;

//==============================================================================
// Atomic-safe pack/unpack -- fits H:M:S:F into a single uint64_t
//==============================================================================
inline uint64_t packTimecode(int h, int m, int s, int f)
{
    return ((uint64_t)(h & 0xFF) << 24)
         | ((uint64_t)(m & 0xFF) << 16)
         | ((uint64_t)(s & 0xFF) << 8)
         |  (uint64_t)(f & 0xFF);
}

inline Timecode unpackTimecode(uint64_t packed)
{
    Timecode tc;
    tc.hours   = (int)((packed >> 24) & 0xFF);
    tc.minutes = (int)((packed >> 16) & 0xFF);
    tc.seconds = (int)((packed >> 8)  & 0xFF);
    tc.frames  = (int)( packed        & 0xFF);
    return tc;
}

//==============================================================================
// Apply a frame offset (+/-) to a Timecode, wrapping at 24h.
// Note: this uses a linear frame-count model (maxFrames per second) rather
// than true SMPTE DF counting.  The DF correction at the end patches any
// landing on skipped frame numbers 0-1.  This is exact for small offsets
// (the ±30 frame range used by output offsets) because DF skips only occur
// at minute boundaries, which are always >30 frames apart.
//==============================================================================
inline Timecode offsetTimecode(const Timecode& tc, int offsetFrames, FrameRate fps)
{
    if (offsetFrames == 0) return tc;

    // The linear-frame arithmetic below is exact only for small offsets.
    // Drop-frame timecode has non-uniform frame distribution, so converting
    // linear→DF→linear for large offsets accumulates error. The UI sliders
    // are constrained to ±30 frames; assert here to catch any future misuse.
    jassert(std::abs(offsetFrames) <= 30);

    int maxFrames = frameRateToInt(fps);
    int64_t total = (int64_t)tc.hours * 3600 * maxFrames
                  + (int64_t)tc.minutes * 60 * maxFrames
                  + (int64_t)tc.seconds * maxFrames
                  + (int64_t)tc.frames
                  + offsetFrames;

    // Wrap around 24h
    int64_t dayFrames = (int64_t)24 * 3600 * maxFrames;
    total = ((total % dayFrames) + dayFrames) % dayFrames;

    Timecode result;
    result.frames  = (int)(total % maxFrames);
    result.seconds = (int)((total / maxFrames) % 60);
    result.minutes = (int)((total / (maxFrames * 60)) % 60);
    result.hours   = (int)((total / (maxFrames * 3600)) % 24);

    // Drop-frame: skip frames 0 and 1 at the start of each minute
    // except every 10th minute (00, 10, 20, 30, 40, 50)
    if (fps == FrameRate::FPS_2997
        && result.frames < 2
        && result.seconds == 0
        && (result.minutes % 10) != 0)
    {
        result.frames = 2;
    }

    return result;
}

//==============================================================================
// Convert wall-clock time (ms since midnight) to timecode.
// For 29.97fps, uses SMPTE drop-frame counting so that timecode stays
// synchronised with real time (drops frames 0 and 1 at the start of each
// minute, except every 10th minute).
//==============================================================================
inline Timecode wallClockToTimecode(double msSinceMidnight, FrameRate fps)
{
    if (fps == FrameRate::FPS_2997)
    {
        // Drop-frame: 29.97fps = 30000/1001 frames per second
        // Total frames elapsed = ms * 29.97 / 1000
        double exactFps = 30000.0 / 1001.0;
        int64_t totalFrames = (int64_t)(msSinceMidnight / 1000.0 * exactFps);

        // SMPTE drop-frame algorithm:
        // In DF counting, every minute (except every 10th) drops 2 frame numbers.
        // D = frames per 10-minute block = 17982 (10*60*30 - 9*2)
        // d = frames per 1-minute block  = 1798  (60*30 - 2)
        const int64_t framesPerTenMin = 17982;
        const int64_t framesPerMin    = 1798;

        int64_t tenMinBlocks = totalFrames / framesPerTenMin;
        int64_t remainder    = totalFrames % framesPerTenMin;

        // First minute of each 10-min block is NOT dropped (has 1800 frames)
        int64_t minutesSinceBlock;
        if (remainder < 1800)
            minutesSinceBlock = 0;
        else
            minutesSinceBlock = 1 + (remainder - 1800) / framesPerMin;

        // Convert back to a frame number in 30fps space
        int64_t frameNumber = totalFrames + 18 * tenMinBlocks + 2 * minutesSinceBlock;

        Timecode tc;
        tc.frames  = (int)(frameNumber % 30);
        tc.seconds = (int)((frameNumber / 30) % 60);
        tc.minutes = (int)((frameNumber / 1800) % 60);
        tc.hours   = (int)((frameNumber / 108000) % 24);
        return tc;
    }
    else
    {
        // Non-drop-frame: straightforward conversion
        double fpsVal = frameRateToDouble(fps);
        int maxFrames = frameRateToInt(fps);
        double secondsTotal = msSinceMidnight / 1000.0;

        Timecode tc;
        int64_t totalSeconds = (int64_t)secondsTotal;
        double fractional = secondsTotal - (double)totalSeconds;

        tc.hours   = (int)((totalSeconds / 3600) % 24);
        tc.minutes = (int)((totalSeconds / 60) % 60);
        tc.seconds = (int)(totalSeconds % 60);
        tc.frames  = (int)(fractional * fpsVal) % maxFrames;
        return tc;
    }
}

//==============================================================================
// Convert a Timecode back to milliseconds since midnight.
// Inverse of wallClockToTimecode().  For 29.97 drop-frame, converts
// the DF frame numbering back to a linear frame count before computing
// real elapsed time using the exact 30000/1001 rate.
//==============================================================================
inline double timecodeToMs(const Timecode& tc, FrameRate fps)
{
    if (fps == FrameRate::FPS_2997)
    {
        // Drop-frame: frame numbers 0 and 1 are skipped at the start
        // of each minute except every 10th minute.  To recover the true
        // linear frame count, subtract the total dropped frame numbers.
        int totalMinutes = tc.hours * 60 + tc.minutes;
        int tenMinBlocks = totalMinutes / 10;

        // Frame number in 30fps space (as written in the TC display)
        int64_t frameNumber = (int64_t)tc.hours   * 108000   // 30 * 3600
                            + (int64_t)tc.minutes  * 1800     // 30 * 60
                            + (int64_t)tc.seconds  * 30
                            + (int64_t)tc.frames;

        // Total dropped frame numbers up to this point:
        // 2 per minute, except every 10th minute (which has no drops)
        int64_t droppedFrames = 2 * (totalMinutes - tenMinBlocks);
        int64_t actualFrames  = frameNumber - droppedFrames;

        double exactFps = 30000.0 / 1001.0;
        return (double)actualFrames / exactFps * 1000.0;
    }
    else
    {
        double fpsVal = frameRateToDouble(fps);
        return (tc.hours * 3600.0 + tc.minutes * 60.0 + tc.seconds) * 1000.0
             + ((double)tc.frames / fpsVal) * 1000.0;
    }
}

//==============================================================================
// Convert a Timecode from one frame rate to another.
// Uses milliseconds as the intermediate representation so the same
// point in real time maps correctly between any pair of rates,
// including drop-frame ↔ non-drop-frame conversions.
//==============================================================================
inline Timecode convertTimecodeRate(const Timecode& tc, FrameRate fromFps, FrameRate toFps)
{
    if (fromFps == toFps) return tc;
    double ms = timecodeToMs(tc, fromFps);
    return wallClockToTimecode(ms, toFps);
}

//==============================================================================
// SMPTE rate code (shared by MTC and Art-Net)
//   0 = 24fps, 1 = 25fps, 2 = 29.97df, 3 = 30fps
//==============================================================================
inline int fpsToRateCode(FrameRate fps)
{
    switch (fps)
    {
        case FrameRate::FPS_2398: return 0;  // Transmitted as 24fps rate code (no dedicated SMPTE code)
        case FrameRate::FPS_24:   return 0;
        case FrameRate::FPS_25:   return 1;
        case FrameRate::FPS_2997: return 2;
        case FrameRate::FPS_30:   return 3;
        default:                  return 1;
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

        // WASAPI variants -- JUCE may use different parenthetical suffixes
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

        // Unknown type -- use full name
        return name;
    }

    static juce::String makeDisplayName(const juce::String& typeName, const juce::String& deviceName)
    {
        auto prefix = shortenTypeName(typeName);
        return prefix.isEmpty() ? deviceName : (prefix + ": " + deviceName);
    }
};
