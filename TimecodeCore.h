// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
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
    FPS_2398  = 0,  // 23.976 (24000/1001) -- cinema/digital workflows
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
// NOTE: 23.976fps (FPS_2398) is always non-drop-frame.  There is no
// SMPTE-standard drop-frame variant for 23.976; the ~0.1% drift vs.
// wall-clock is accepted in cinema/digital workflows.
//==============================================================================
inline Timecode incrementFrame(const Timecode& tc, FrameRate fps)
{
    int maxFrames = frameRateToInt(fps);

    // Clamp input -- during FPS conversion frames may be out of range
    // for the target rate (e.g. frame 29 from 30fps -> 25fps output).
    Timecode r = tc;
    if (r.frames < 0)          r.frames = 0;
    if (r.frames >= maxFrames) r.frames = maxFrames - 1;
    if (r.seconds < 0 || r.seconds >= 60) r.seconds = 0;
    if (r.minutes < 0 || r.minutes >= 60) r.minutes = 0;
    if (r.hours < 0   || r.hours >= 24)   r.hours = 0;

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
// Frame index: the position of a timecode as a count of frames since
// 00:00:00:00, honouring drop-frame numbering.  At 29.97 the address space
// skips two numbers at the start of every minute that is not a multiple of
// ten, so a linear (h*3600 + m*60 + s) * 30 + f count is NOT a frame count:
// consecutive addresses across 59;29 -> 00;02 are one frame apart, not three.
// These helpers convert between the address and the true count, so any
// arithmetic (offsets, distances, interpolation) can be done on the count and
// converted back exactly.  For every other rate the index is the plain linear
// count.  23.976 uses 24 addresses per timecode second like 24 fps.
//==============================================================================
inline int64_t framesPerDay(FrameRate fps)
{
    // 29.97 DF: 144 ten-minute blocks of 17982 addresses (10*60*30 - 9*2).
    if (fps == FrameRate::FPS_2997) return (int64_t)144 * 17982;
    return (int64_t)24 * 3600 * frameRateToInt(fps);
}

inline int64_t timecodeToFrameIndex(const Timecode& tc, FrameRate fps)
{
    const int maxFrames = frameRateToInt(fps);
    int64_t idx = ((int64_t)tc.hours * 3600 + (int64_t)tc.minutes * 60 + (int64_t)tc.seconds) * maxFrames
                + (int64_t)tc.frames;
    if (fps == FrameRate::FPS_2997)
    {
        // Two addresses dropped per minute, except every tenth minute.
        const int totalMinutes = tc.hours * 60 + tc.minutes;
        idx -= 2 * (totalMinutes - totalMinutes / 10);
    }
    return idx;
}

inline Timecode frameIndexToTimecode(int64_t idx, FrameRate fps)
{
    const int maxFrames = frameRateToInt(fps);
    const int64_t day = framesPerDay(fps);
    idx = ((idx % day) + day) % day;   // wrap at 24h, negative-safe

    if (fps == FrameRate::FPS_2997)
    {
        // SMPTE drop-frame: within each 10-minute block the first minute has
        // 1800 addresses and the other nine have 1798.  Add back the dropped
        // numbers to get the address in 30 fps space.
        const int64_t framesPerTenMin = 17982;
        const int64_t framesPerMin    = 1798;
        const int64_t tenMinBlocks = idx / framesPerTenMin;
        const int64_t remainder    = idx % framesPerTenMin;
        const int64_t minutesSinceBlock = (remainder < 1800) ? 0 : 1 + (remainder - 1800) / framesPerMin;
        idx = idx + 18 * tenMinBlocks + 2 * minutesSinceBlock;
    }

    Timecode tc;
    tc.frames  = (int)(idx % maxFrames);
    tc.seconds = (int)((idx / maxFrames) % 60);
    tc.minutes = (int)((idx / (maxFrames * 60)) % 60);
    tc.hours   = (int)((idx / (maxFrames * 3600)) % 24);
    return tc;
}

/// Shortest signed distance a - b in frames on the 24h circle.
/// Positive when a is ahead of b.  Drop-frame aware.
inline int64_t frameDistance(const Timecode& a, const Timecode& b, FrameRate fps)
{
    const int64_t day = framesPerDay(fps);
    int64_t d = ((timecodeToFrameIndex(a, fps) - timecodeToFrameIndex(b, fps)) % day + day) % day;
    if (d > day / 2) d -= day;
    return d;
}

//==============================================================================
// Apply a frame offset (+/-) to a Timecode, wrapping at 24h.  Exact at every
// rate: the offset is added to the frame index, so a drop-frame minute
// boundary inside the offset window is counted correctly.  (The previous
// linear model patched addresses 00/01 to 02 after the fact and was up to
// two frames off for |offset| frames around every non-tenth minute.)
//==============================================================================
inline Timecode offsetTimecode(const Timecode& tc, int offsetFrames, FrameRate fps)
{
    if (offsetFrames == 0) return tc;
    return frameIndexToTimecode(timecodeToFrameIndex(tc, fps) + offsetFrames, fps);
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
        // Epsilon guards against FP truncation at frame boundaries
        // (e.g. 33.3667ms * 29.97/1000 = 0.99999... -> should be frame 1)
        int64_t totalFrames = (int64_t)(msSinceMidnight / 1000.0 * exactFps + 1e-9);
        // The frame count is the drop-frame index; the address comes from the
        // shared helper so the two never disagree.
        return frameIndexToTimecode(totalFrames, fps);
    }
    else
    {
        // Non-drop-frame: split into integer seconds + fractional frame.
        // This correctly handles 23.976fps where 24 frames span slightly
        // more than 1 wall-clock second (1001/1000 s).  Using a total
        // frame count with % maxFrames would drift vs second boundaries.
        //
        // Precision note: double has ~15 significant digits.  At 24h
        // (86400s), the fractional part retains ~10 digits of precision --
        // far more than needed for sub-frame accuracy at any supported rate.
        double fpsVal = frameRateToDouble(fps);
        int maxFrames = frameRateToInt(fps);
        double secondsTotal = msSinceMidnight / 1000.0;

        Timecode tc;
        int64_t totalSeconds = (int64_t)secondsTotal;
        double fractional = secondsTotal - (double)totalSeconds;

        tc.hours   = (int)((totalSeconds / 3600) % 24);
        tc.minutes = (int)((totalSeconds / 60) % 60);
        tc.seconds = (int)(totalSeconds % 60);
        // Guard against floating-point truncation at frame boundaries:
        // e.g. at 30fps, 33.333ms -> fractional*30 = 0.99999... truncates to 0
        // instead of 1.  An epsilon of 1e-9 (~1ns) fixes boundary rounding
        // without risk of pushing legitimate values past the next frame.
        tc.frames  = (int)(fractional * fpsVal + 1e-9) % maxFrames;
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
        // Drop-frame: recover the true frame count from the address (see
        // timecodeToFrameIndex), then scale by the exact 30000/1001 rate.
        const int64_t actualFrames = timecodeToFrameIndex(tc, fps);
        const double exactFps = 30000.0 / 1001.0;
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
// Apply a large timecode offset (for TrackMap -- no +/-30 frame limit).
// Adds offset HH:MM:SS:FF to the input timecode, wrapping at 24h.
// Uses milliseconds as intermediate representation for exact drop-frame
// arithmetic (same proven approach as convertTimecodeRate).
//
// tcFps     -- frame rate of the input timecode and of the result
// offsetFps -- frame rate used to interpret the offset fields
//             (may differ if the offset was authored at a different rate)
//==============================================================================
inline Timecode applyTimecodeOffset(const Timecode& tc, FrameRate tcFps,
                                    int offH, int offM, int offS, int offF,
                                    FrameRate offsetFps)
{
    double tcMs  = timecodeToMs(tc, tcFps);

    Timecode offTc { offH, offM, offS, offF };
    double offMs = timecodeToMs(offTc, offsetFps);

    double totalMs = tcMs + offMs;

    // Wrap at 24 hours
    constexpr double kMsPerDay = 24.0 * 3600.0 * 1000.0;
    totalMs = std::fmod(totalMs, kMsPerDay);
    if (totalMs < 0.0) totalMs += kMsPerDay;

    return wallClockToTimecode(totalMs, tcFps);
}

//==============================================================================
// Convert a Timecode from one frame rate to another.
// Uses milliseconds as the intermediate representation so the same
// point in real time maps correctly between any pair of rates,
// including drop-frame <-> non-drop-frame conversions.
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
