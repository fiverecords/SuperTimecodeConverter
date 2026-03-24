// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter
//
// LinkBridge -- Ableton Link integration for tempo synchronization.
//
// Reads BPM from ProDJLink (or any active input source) and proposes it
// as the session tempo on an Ableton Link network.  Any Link-enabled peer
// (Resolume, Ableton Live, Traktor, etc.) on the same LAN will automatically
// sync to the DJ's tempo.
//
// Build requirements:
//   1. Clone Ableton Link SDK:
//        git clone --recurse-submodules https://github.com/Ableton/link.git
//   2. Add include paths to your Projucer or CMakeLists.txt:
//        <link_repo>/include
//        <link_repo>/modules/asio-standalone/asio/include
//   3. Define the preprocessor macros:
//        STC_ENABLE_LINK=1
//        LINK_PLATFORM_WINDOWS=1   (Windows only)
//        _WIN32_WINNT=0x0602       (Windows only)
//        NOMINMAX=1                (Windows only)
//   4. On Windows, link against: ws2_32, iphlpapi, winmm
//      On macOS/Linux: no extra libraries needed
//
// When STC_ENABLE_LINK is not defined, this class becomes a no-op stub
// so the rest of the app compiles and works normally without Link.
//
// Architecture:
//   - Uses captureAppSessionState / commitAppSessionState (app thread)
//     NOT the audio thread variants -- we're not in an audio callback.
//   - Tempo is updated from TimecodeEngine::tick() (message thread, 60Hz).
//   - setTempo() only commits to Link when the BPM delta exceeds a threshold,
//     preventing Link from constantly re-proposing micro-fluctuations.
//   - Beat phase is NOT synchronized -- we only set tempo. Phase alignment
//     requires knowledge of the exact beat position which would need
//     sub-frame timing from the CDJ beat marker.
//
// Protocol reference:
//   - https://ableton.github.io/link/
//   - https://github.com/Ableton/link
//

#pragma once

//==============================================================================
// Compile-time switch
//==============================================================================
#ifdef STC_ENABLE_LINK

#include <ableton/Link.hpp>
#include <atomic>
#include <cmath>

class LinkBridge
{
public:
    /// Construct with a default tempo. Link session is NOT enabled by default.
    explicit LinkBridge(double initialTempo = 120.0)
        : link(initialTempo)
    {
        // Register peer count callback (thread-safe -- Link calls on its own thread)
        link.setNumPeersCallback([this](std::size_t numPeers)
        {
            peerCount.store((int)numPeers, std::memory_order_relaxed);
        });

        link.setTempoCallback([this](double bpm)
        {
            // A remote peer changed the tempo -- we note it but don't override.
            // Our next setTempo() call will re-propose the DJ's BPM anyway.
            currentTempo.store(bpm, std::memory_order_relaxed);
        });
    }

    ~LinkBridge()
    {
        link.enable(false);
    }

    //--------------------------------------------------------------------------
    // Enable / Disable
    //--------------------------------------------------------------------------

    /// Enable or disable Link. When enabled, STC joins the Link session
    /// and peers on the LAN can see it.
    void setEnabled(bool enabled)
    {
        link.enable(enabled);
        enabledFlag.store(enabled, std::memory_order_relaxed);
        if (enabled)
        {
            // Force the next setTempo() to commit immediately, even if the
            // BPM happens to match what we last sent.  Without this, re-enabling
            // Link after a disable leaves the session at 120 BPM (SDK default)
            // because lastCommittedBpm still holds the old value and the
            // hysteresis check suppresses the "identical" commit.
            lastCommittedBpm = 0.0;
        }
        else
        {
            peerCount.store(0, std::memory_order_relaxed);
            lastCommittedBpm = 0.0;
        }
    }

    bool isEnabled() const { return enabledFlag.load(std::memory_order_relaxed); }

    //--------------------------------------------------------------------------
    // Tempo control (call from message thread / timer tick)
    //--------------------------------------------------------------------------

    /// Propose a new tempo to the Link session.
    /// Only commits if the delta from the last committed value exceeds
    /// the threshold (default 0.1 BPM) to avoid constant micro-updates.
    void setTempo(double bpm)
    {
        if (!enabledFlag.load(std::memory_order_relaxed))
            return;
        if (bpm < 20.0 || bpm > 999.0)
            return;  // sanity check

        // Hysteresis: only commit if BPM changed meaningfully
        if (std::abs(bpm - lastCommittedBpm) < kTempoThreshold)
            return;

        auto state = link.captureAppSessionState();
        state.setTempo(bpm, link.clock().micros());
        link.commitAppSessionState(state);

        lastCommittedBpm = bpm;
        currentTempo.store(bpm, std::memory_order_relaxed);
    }

    /// Get current session tempo (may differ from what we proposed if
    /// another peer is also changing tempo).
    double getTempo() const
    {
        return currentTempo.load(std::memory_order_relaxed);
    }

    //--------------------------------------------------------------------------
    // Session info
    //--------------------------------------------------------------------------

    /// Number of peers currently connected in the Link session
    /// (not counting ourselves).
    int getNumPeers() const { return peerCount.load(std::memory_order_relaxed); }

    /// Get current beat position (useful for UI display)
    double getBeat(double quantum = 4.0) const
    {
        if (!enabledFlag.load(std::memory_order_relaxed))
            return 0.0;
        auto state = link.captureAppSessionState();
        return state.beatAtTime(link.clock().micros(), quantum);
    }

    /// Get current phase within quantum (0.0 to quantum)
    double getPhase(double quantum = 4.0) const
    {
        if (!enabledFlag.load(std::memory_order_relaxed))
            return 0.0;
        auto state = link.captureAppSessionState();
        return state.phaseAtTime(link.clock().micros(), quantum);
    }

private:
    // IMPORTANT: 'link' is declared LAST so it is destroyed FIRST.
    // Link callbacks reference the atomics above; destroying link first
    // guarantees callbacks have finished before atomics are torn down.
    std::atomic<bool>   enabledFlag { false };
    std::atomic<int>    peerCount   { 0 };
    std::atomic<double> currentTempo { 120.0 };

    double lastCommittedBpm = 0.0;

    ableton::Link link;

    // Only commit tempo changes > 0.1 BPM to avoid flooding the session
    static constexpr double kTempoThreshold = 0.1;
};

#else  // STC_ENABLE_LINK not defined -- no-op stub

//==============================================================================
// Stub implementation when Ableton Link is not available.
// All methods are no-ops so the rest of the code compiles without changes.
//==============================================================================

class LinkBridge
{
public:
    explicit LinkBridge(double = 120.0) {}

    void setEnabled(bool)     {}
    bool isEnabled() const    { return false; }
    void setTempo(double)     {}
    double getTempo() const   { return 0.0; }
    int  getNumPeers() const  { return 0; }
    double getBeat(double = 4.0) const  { return 0.0; }
    double getPhase(double = 4.0) const { return 0.0; }
};

#endif  // STC_ENABLE_LINK
