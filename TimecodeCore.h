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
// count.  23.976 uses 24 addresses per timecode second like 24 fps; its
// timecode second is 1.001 real seconds (see wallClockToTimecode).
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
// Free-running sender tracking policy.
//
// Every sender (LTC, MTC, Art-Net, LA-Net) advances its own value once per
// frame on its own clock and compares it with the value the engine last
// published.  The published value lags the truth by up to one engine tick,
// so a distance of 0 or -1 is "aligned".  When the source runs at a pitch
// the sender cannot follow (MTC and Art-Net keep their nominal frame rate by
// design, and any clock drifts) the distance walks away one frame at a
// time; the policy corrects it one frame at a time too -- a repeated frame
// when the sender is ahead, a skipped frame when it is behind -- which is
// what a varispeed source looks like on a fixed-rate protocol and the
// smallest discontinuity a receiver can see.  Only a distance beyond
// kTrackingHardResync (a seek the engine did not announce) snaps to the
// published value.
//
// Before this, the senders snapped whenever the distance exceeded one (two
// for MTC): at -8 % pitch Art-Net went backwards two frames every 0.8 s,
// MTC three frames every 1.6 s, and a console chasing them treated every
// snap as a locate.
//
// Returns the value to emit given the sender's next nominal value (already
// advanced by `nominalAdvance` frames from the last emitted one) and the
// engine's published value.
//==============================================================================
static constexpr int64_t kTrackingHardResync = 4;

inline Timecode trackPublishedValue(const Timecode& nominalNext, const Timecode& published,
                                    int nominalAdvance, FrameRate fps)
{
    const int64_t d = frameDistance(published, nominalNext, fps);   // published - next
    if (d > kTrackingHardResync || d < -kTrackingHardResync)
        return published;                                            // seek: snap
    if (d >= 1)
        return frameIndexToTimecode(timecodeToFrameIndex(nominalNext, fps) + 1, fps);   // behind: skip one
    if (d <= -2 && nominalAdvance > 0)
        return frameIndexToTimecode(timecodeToFrameIndex(nominalNext, fps) - 1, fps);   // ahead: repeat one
    return nominalNext;
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
    else if (fps == FrameRate::FPS_2398)
    {
        // 24/1.001, normative: the time address counts 24 frames per
        // timecode second and there is no drop-frame variant, so it runs
        // 0.1 % slow against real time (ST 12-1) -- 3.6 s per hour, like
        // every 23.976 generator.  (Until 2026-09 STC kept HH:MM:SS on the
        // wall clock and fitted 24 numbers into each real second, which put
        // one number too many on the 23.976 carrier every 41.7 s and made a
        // video server counting clip frames drift 3.6 s per hour from the
        // audio.  D20.)
        const double  exactFps    = 24000.0 / 1001.0;
        const int64_t totalFrames = (int64_t)(msSinceMidnight / 1000.0 * exactFps + 1e-9);
        return frameIndexToTimecode(totalFrames, fps);
    }
    else
    {
        // Integer rates: split into integer seconds + fractional frame.
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
    if (fps == FrameRate::FPS_2997 || fps == FrameRate::FPS_2398)
    {
        // Fractional rates: the frame index (drop-frame aware at 29.97,
        // linear at 23.976) scaled by the exact 1001-based rate.  This is
        // the inverse of wallClockToTimecode for both.
        const int64_t frames = timecodeToFrameIndex(tc, fps);
        return (double)frames / frameRateToDouble(fps) * 1000.0;
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
// Operator-typed timecode text.  One parser and one formatter for every text
// field that holds a timecode (generator start/stop, generator cue points,
// presets), so what the operator types, what the field shows back, and what
// the engine displays are the same number.  Before this, four private copies
// of the arithmetic (wall-clock seconds plus frames/fps, no drop-frame, no
// epsilon) lived in MainComponent, the generator cue editor, the cue point
// struct and TCNet; at 29.97 they disagreed with the display by up to two
// frames.
//
// Accepted forms: HH:MM:SS:FF, HH:MM:SS.FF, HH:MM:SS;FF, with fewer fields
// allowed (missing ones read as 0).  Fields are clamped to range.  At 29.97
// an address in the two numbers a drop-frame minute skips (00 or 01 at second
// 00 of a non-tenth minute) is moved to 02, the first address that exists.
//==============================================================================
inline FrameRate frameRateFromDouble(double fps)
{
    if (fps < 23.99) return FrameRate::FPS_2398;
    if (fps < 24.5)  return FrameRate::FPS_24;
    if (fps < 27.0)  return FrameRate::FPS_25;
    if (fps < 29.99) return FrameRate::FPS_2997;
    return FrameRate::FPS_30;
}

inline Timecode normaliseDropFrame(Timecode tc, FrameRate fps)
{
    if (fps == FrameRate::FPS_2997 && tc.seconds == 0 && tc.frames < 2 && (tc.minutes % 10) != 0)
        tc.frames = 2;
    return tc;
}

inline Timecode parseTimecodeText(const juce::String& text, FrameRate fps)
{
    auto parts = juce::StringArray::fromTokens(text, ":.;", "");
    int v[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < 4 && i < parts.size(); ++i)
        v[i] = parts[i].getIntValue();

    Timecode tc;
    tc.hours   = juce::jlimit(0, 23, v[0]);
    tc.minutes = juce::jlimit(0, 59, v[1]);
    tc.seconds = juce::jlimit(0, 59, v[2]);
    tc.frames  = juce::jlimit(0, frameRateToInt(fps) - 1, v[3]);
    return normaliseDropFrame(tc, fps);
}

inline double parseTimecodeTextToMs(const juce::String& text, FrameRate fps)
{
    return timecodeToMs(parseTimecodeText(text, fps), fps);
}

inline juce::String formatTimecodeText(const Timecode& tc, juce::juce_wchar frameSeparator = ':')
{
    return juce::String(tc.hours).paddedLeft('0', 2) + ":"
         + juce::String(tc.minutes).paddedLeft('0', 2) + ":"
         + juce::String(tc.seconds).paddedLeft('0', 2) + juce::String::charToString(frameSeparator)
         + juce::String(tc.frames).paddedLeft('0', 2);
}

inline juce::String msToTimecodeText(double ms, FrameRate fps, juce::juce_wchar frameSeparator = ':')
{
    if (ms < 0.0) ms = 0.0;
    return formatTimecodeText(wallClockToTimecode(ms, fps), frameSeparator);
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

//==============================================================================
// Time of day, followed rather than jumped to (D31)
//
// The Generator's clock mode shows the time of day.  The wall clock is the
// truth for WHAT time it is, but it moves in system-timer steps (up to
// 15.6 ms on Windows) and would put that granularity into the frame phase;
// the high-resolution counter is smooth but is a different oscillator, and
// the two drift apart by tens of ppm.  The previous design ran on the counter
// and re-anchored to the wall clock whenever they disagreed by 50 ms -- a
// 50 ms jump, more than a frame at 25 fps, every time the two clocks drifted
// that far apart: @mungewell's skip every 46 minutes, on every output.
//
// This advances on the counter and pulls gently towards the wall clock every
// tick, so the drift is followed continuously instead of in steps.  The pull
// also averages the wall clock's quantisation away.  Only a disagreement of
// more than a second -- the clock being set, a resume from sleep, midnight --
// is treated as a real jump and followed at once.
//
// Pure and header-only so the harness can drive the same code the engine
// runs (tools/audit/ltc_clockmode_sim.cpp).
//==============================================================================
struct WallClockFollower
{
    // Fraction of the disagreement taken out per engine tick.  At 60 Hz that
    // is a time constant of about 3.3 s: a 15 ppm drift leaves a standing lag
    // of 0.05 ms, and the wall clock's 15.6 ms steps come through as about
    // 0.2 ms of noise, which the LTC phase lock filters anyway.
    static constexpr double kPullPerTick = 0.005;
    static constexpr double kJumpMs      = 1000.0;

    double update(double wallMs, double hiResMs)
    {
        if (lastHiResMs < 0.0)
        {
            posMs = wallMs;
            lastHiResMs = hiResMs;
            return posMs;
        }
        posMs += hiResMs - lastHiResMs;
        lastHiResMs = hiResMs;

        const double err = wallMs - posMs;
        if (std::abs(err) > kJumpMs) posMs = wallMs;      // set, resume, midnight
        else                         posMs += err * kPullPerTick;
        return posMs;
    }

    void reset() { lastHiResMs = -1.0; }

private:
    double posMs       = 0.0;
    double lastHiResMs = -1.0;
};
