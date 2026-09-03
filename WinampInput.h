// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

#pragma once
#include <JuceHeader.h>
#include <atomic>

//==============================================================================
// WinampInput
//
// Reads playback position, state, and current-track metadata from a running
// Winamp / WACUP / "Winamp Origins" instance via the Winamp IPC API
// (SendMessage with WM_USER + IPC_* selectors against the "Winamp v1.x"
// window class).
//
// The class is Windows-only by definition.  On macOS the entire body is
// replaced by a stub that always reports "not connected", so TimecodeEngine
// can hold a `WinampInput` member and emit a `InputSource::Winamp` case
// unconditionally; the engine and UI hide the source on non-Windows builds.
//
// Cross-process notes
// -------------------
// SendMessage works cross-process for IPC selectors that return scalar LRESULT
// values (playback state, position in ms, track length, playlist position).
// IPC selectors that return pointer values (e.g. IPC_GETPLAYLISTTITLE,
// IPC_GETPLAYLISTFILE) return pointers into Winamp's address space which are
// NOT dereferenceable from STC's process.  This implementation therefore
// reads the artist/title text from Winamp's main-window title bar
// (`GetWindowTextW`), which is the same approach used by every cross-process
// Winamp scraper I am aware of (including the Python `hugovk/winamp` library
// linked from the original feature request).
//
// Polling cadence
// ---------------
// Winamp's IPC handler runs on its own UI thread; cheap selectors return in
// well under 1 ms when Winamp is responsive.  We poll at 20 Hz which is
// frequent enough for the engine PLL to keep MTC / LTC / Art-Net output frame-
// accurate without putting visible load on Winamp.
//
// Other players that emulate the Winamp IPC API
// ---------------------------------------------
// AIMP, MusicBee and a few other Windows players ship a Winamp-plug-in
// compatibility mode that creates a hidden "Winamp v1.x" window and answers
// the same IPC selectors.  We support those out of the box: the IPC calls
// return correct scalar values (state, position, length, list pos) and the
// title bar of the host window is the canonical source of the current
// artist / title.  Two small differences from Winamp itself are worth
// flagging:
//   * Title-bar suffix.  AIMP appends "- AIMP", MusicBee appends
//     "- MusicBee", etc.  The parser strips a recognised set of suffixes
//     before splitting the artist / title; new players are easy to add by
//     extending `kPlayerSuffixes`.
//   * Playlist position semantics.  Winamp updates IPC_GETLISTPOS on every
//     track change.  AIMP does so when the user advances through the
//     playlist but not when, for example, the user double-clicks a file
//     outside the playlist or replays the same playlist entry.  Detecting
//     track change purely from the playlist index would miss those cases
//     (the symptom is "track changed but STC still shows the previous
//     track's metadata"), so the run loop diffs the title bar text on
//     every poll instead and refreshes whenever it changes.  Cheap: one
//     GetWindowTextW per 50 ms.
//
// Caveat: scrolling title bar
// ---------------------------
// If the user has Winamp's / WACUP's / AIMP's "scrolling title in the
// window title bar" preference on, the host rewrites the title character-
// by-character on a short timer and the parser will see fragments rather
// than the canonical "N. Artist - Title - Winamp" form.  With diff-based
// detection above, every rotation looks like a track change to STC and
// TrackMap matching flips constantly.  The mitigation is to leave that
// preference off (it is off by default on all forks we have looked at).
// Documented behaviour, not a bug.
//==============================================================================

#if JUCE_WINDOWS

#include <windows.h>

class WinampInput : public juce::Thread
{
public:
    enum class State { Stopped, Playing, Paused };

    WinampInput() : juce::Thread("Winamp Input") {}
    ~WinampInput() override { stop(); }

    //==========================================================================
    bool start()
    {
        stop();
        isRunningFlag.store(true, std::memory_order_relaxed);
        startThread();
        return true;
    }

    void stop()
    {
        isRunningFlag.store(false, std::memory_order_relaxed);
        if (isThreadRunning())
            stopThread(500);

        // Reset all observable state so a stopped + re-started input
        // does not briefly report stale data.
        winampConnected.store(false,    std::memory_order_relaxed);
        state.store(State::Stopped,     std::memory_order_relaxed);
        durationSec.store(0,            std::memory_order_relaxed);
        playlistIndex.store(-1,         std::memory_order_relaxed);
        lastPollTime.store(0.0,         std::memory_order_relaxed);
        {
            const juce::SpinLock::ScopedLockType lock(anchorLock);
            positionAnchorMs   = 0;
            positionAnchorTime = 0.0;
        }

        const juce::SpinLock::ScopedLockType lock(metadataLock);
        artist = {};
        title  = {};
        lastSeenTitleBar = {};
    }

    bool getIsRunning() const { return isRunningFlag.load(std::memory_order_relaxed); }

    //==========================================================================
    // True if a Winamp window was found in the most recent poll.  Independent
    // of playback state -- Winamp can be connected and stopped at the same time.
    bool isConnected() const { return winampConnected.load(std::memory_order_relaxed); }

    // True if the input loop polled Winamp within kStaleTimeoutMs ago.
    // The engine uses this the same way it uses isReceiving() on the
    // MTC / ArtNet / LTC inputs: as the gate that promotes the source
    // to "active" and lets its data drive the output.
    bool isReceiving() const
    {
        if (!winampConnected.load(std::memory_order_relaxed))
            return false;
        double lpt = lastPollTime.load(std::memory_order_relaxed);
        if (lpt == 0.0)
            return false;
        double now = juce::Time::getMillisecondCounterHiRes();
        return (now - lpt) < kStaleTimeoutMs;
    }

    State    getState()         const { return state.load(std::memory_order_relaxed); }
    int32_t  getDurationSec()   const { return durationSec.load(std::memory_order_relaxed); }
    int      getPlaylistIndex() const { return playlistIndex.load(std::memory_order_relaxed); }

    //==========================================================================
    // Position in milliseconds.
    //
    // Not a plain load of the last polled value -- the engine ticks the
    // output handlers at 60 Hz while we poll Winamp at 20 Hz, and any one
    // IPC call can stall for up to 200 ms when the player's message thread
    // is busy (the classic case is AIMP / Winamp finishing the VBR scan a
    // second or so into a freshly loaded MP3; the player's audio keeps
    // playing in its own thread but IPC_GETOUTPUTTIME does not return
    // until the scan finishes).  If we just handed the engine the latest
    // stored value, the output timecode would freeze on the receiver for
    // those 200 ms even though the audio kept playing.
    //
    // Instead we anchor on a (position, wall-clock) pair updated by the
    // poll thread via a PLL-style filter (see run() for the slew logic)
    // and extrapolate forward to "now" using the local high-resolution
    // clock.  The PLL filter absorbs the per-poll quantisation noise that
    // would otherwise show up as ±1-2 frames of jitter on the receiver
    // when used for frame-accurate video sync (Winamp / WACUP / AIMP all
    // round their reported position to an audio buffer boundary -- 1024
    // samples = ~23 ms at 44.1 kHz -- so consecutive polls can disagree
    // by up to half a buffer even though the audio plays smoothly).
    //
    // While paused or stopped we return the anchor verbatim so timecode
    // freezes correctly, mirroring what the player itself does.
    //
    // The SpinLock guarantees the (pos, time) pair is read atomically as
    // a unit -- without it, the engine could read a freshly-updated
    // anchorPos paired with a stale anchorTime (or vice versa) in the
    // microsecond window between the two writes, computing an
    // interpolated value that's off by tens of ms for one tick.
    int32_t getPositionMs() const
    {
        int32_t anchorP;
        double  anchorT;
        {
            const juce::SpinLock::ScopedLockType lock(anchorLock);
            anchorP = positionAnchorMs;
            anchorT = positionAnchorTime;
        }

        State s = state.load(std::memory_order_relaxed);
        if (s != State::Playing || anchorT <= 0.0)
            return anchorP;

        double now = juce::Time::getMillisecondCounterHiRes();
        double dt  = now - anchorT;
        if (dt < 0.0) dt = 0.0;          // monotonic clock guard
        return anchorP + (int32_t)dt;
    }

    juce::String getArtist() const
    {
        const juce::SpinLock::ScopedLockType lock(metadataLock);
        return artist;
    }

    juce::String getTitle() const
    {
        const juce::SpinLock::ScopedLockType lock(metadataLock);
        return title;
    }

private:
    //--------------------------------------------------------------------------
    // Winamp IPC selectors -- see SDK/IPC.h.  Subset that returns scalar
    // LRESULT values and is therefore safe to call cross-process.
    static constexpr UINT WM_WA_IPC = WM_USER;

    static constexpr int IPC_ISPLAYING     = 104;  // 0 = stopped, 1 = playing, 3 = paused
    static constexpr int IPC_GETOUTPUTTIME = 105;  // wparam=0 -> ms position, wparam=1 -> seconds length
    static constexpr int IPC_GETLISTPOS    = 125;  // 0-based playlist index of current track

    static constexpr int kPollIntervalMs   = 50;     // 20 Hz
    static constexpr double kStaleTimeoutMs = 500.0;  // 10 missed polls -> "not receiving"

    //--------------------------------------------------------------------------
    // Cross-process IPC send with a hung-process timeout.
    //
    // SendMessage is synchronous: it does not return until the target thread
    // has processed the message.  A Winamp instance that is busy decoding,
    // hung in a plugin, or modally blocked (right-click menu open, file dialog
    // showing) can stall our poll thread for many seconds, and run() cannot
    // observe `threadShouldExit()` while it is blocked inside the system call,
    // so a stop() at exit can race past the 500 ms stopThread() grace and
    // leak the thread.
    //
    // SendMessageTimeoutA with SMTO_ABORTIFHUNG bounds every IPC call to a
    // small, fixed budget; if the target is unresponsive we get a zero return
    // and skip that field this cycle.  All Winamp IPC selectors we use return
    // -1 when "no answer / not playing", so reporting -1 here matches what
    // the rest of the run() loop already treats as "skip".
    //
    // The 200 ms timeout is generous -- responsive Winamp / WACUP serves these
    // selectors in well under 1 ms.
    LRESULT ipcSend(HWND hwnd, WPARAM wparam, LPARAM lparam) const
    {
        DWORD_PTR result = 0;
        LRESULT ok = ::SendMessageTimeoutA(hwnd, WM_WA_IPC, wparam, lparam,
                                            SMTO_ABORTIFHUNG, 200, &result);
        return ok != 0 ? (LRESULT)result : (LRESULT)-1;
    }

    //--------------------------------------------------------------------------
    void run() override
    {
        while (!threadShouldExit() && isRunningFlag.load(std::memory_order_relaxed))
        {
            // Find any Winamp-compatible window.  WACUP and Winamp Origins both
            // keep the original "Winamp v1.x" window class for plugin and IPC
            // compatibility, so a single FindWindow call covers all current
            // forks people actually run.
            HWND hwnd = ::FindWindowA("Winamp v1.x", nullptr);

            if (hwnd == nullptr)
            {
                winampConnected.store(false, std::memory_order_relaxed);
                state.store(State::Stopped, std::memory_order_relaxed);
                juce::Thread::sleep(kPollIntervalMs);
                continue;
            }

            winampConnected.store(true, std::memory_order_relaxed);

            // -- Playback state ----------------------------------------------
            // IPC_ISPLAYING returns 0 (stopped) / 1 (playing) / 3 (paused), all
            // non-negative.  ipcSend() returns -1 on SendMessageTimeout
            // failure (the player's message thread is busy -- typically the
            // VBR duration scan in the first ~1 s after a freshly loaded MP3),
            // and we MUST NOT collapse that to "stopped" because:
            //
            //   * The getter `getPositionMs()` only interpolates while the
            //     state is Playing; treating an IPC timeout as a stop would
            //     freeze the engine's output timecode for as long as the
            //     timeouts persist, which is the exact freeze the
            //     interpolation rewrite is supposed to prevent.
            //   * The next poll usually succeeds (the message thread is busy
            //     for hundreds of milliseconds, not seconds), so flipping
            //     Playing->Stopped->Playing back and forth across poll cycles
            //     would also alternately freeze and resume the interpolation,
            //     producing the visible "TC fluctuates for ~1 s after a track
            //     change" symptom.
            //
            // On IPC failure (-1) we leave `state` at the last known good
            // value; the next successful poll updates it.
            LRESULT ps = ipcSend(hwnd, 0, IPC_ISPLAYING);
            if (ps >= 0)
            {
                State st = (ps == 1) ? State::Playing
                         : (ps == 3) ? State::Paused
                                     : State::Stopped;
                state.store(st, std::memory_order_relaxed);
            }

            // -- Position in ms (wparam=0 returns ms, -1 if not playing) -----
            //
            // The anchor is filtered through a single-pole IIR low-pass with
            // alpha = 1/20 (one part in twenty of the difference applied per
            // poll), plus a hard-snap escape hatch for genuine transport
            // events.  Why a continuous filter instead of a tiered slew /
            // dead-zone scheme:
            //
            // Winamp / WACUP / AIMP all round their reported position to an
            // audio-buffer boundary (1024 samples = ~23 ms at 44.1 kHz,
            // larger for higher buffer settings) -- consecutive polls can
            // therefore disagree with the true playback position by up to
            // half a buffer even during steady playback.  A naive snap to
            // each report propagates that ±10-30 ms quantisation noise
            // straight through to the receiver as ±1-2 frames of timecode
            // jitter, which is exactly what breaks frame-accurate video
            // sync.  A continuous IIR with the time constant set to ~1 s
            // (alpha = poll_period / tau = 50 ms / 1000 ms = 1/20) reduces
            // that noise by a factor of sqrt(alpha / (2 - alpha)) ≈ 0.16,
            // so a ±30 ms input becomes ±5 ms at the output -- well below
            // a single frame at every standard frame rate, and without the
            // boundary discontinuities a tiered filter would introduce
            // around the dead-zone / slew threshold.
            //
            // The hard snap (|drift| >= 500 ms) keeps the filter responsive
            // to genuine transport events -- loop points, hot cues, manual
            // seeks, track-change re-anchor.  500 ms is far larger than any
            // realistic quantisation or clock-drift can produce on a single
            // poll, so the threshold never trips during steady playback.
            LRESULT pos = ipcSend(hwnd, 0, IPC_GETOUTPUTTIME);
            if (pos >= 0)
            {
                double  now         = juce::Time::getMillisecondCounterHiRes();
                int32_t reportedPos = (int32_t)pos;

                int32_t oldAnchorP;
                double  oldAnchorT;
                {
                    const juce::SpinLock::ScopedLockType lock(anchorLock);
                    oldAnchorP = positionAnchorMs;
                    oldAnchorT = positionAnchorTime;
                }

                // What the local interpolation thinks the position is at
                // "now" (this is what the engine would read from
                // getPositionMs() right now if it were called) -- the IIR
                // operates on the difference between that and the report,
                // not between the raw anchor and the report, otherwise the
                // time-since-anchor delay would look like extra drift.
                State   s = state.load(std::memory_order_relaxed);
                int32_t curInterpolated;
                if (s == State::Playing && oldAnchorT > 0.0)
                    curInterpolated = oldAnchorP + (int32_t)(now - oldAnchorT);
                else
                    curInterpolated = oldAnchorP;

                int32_t drift    = reportedPos - curInterpolated;
                int32_t absDrift = drift < 0 ? -drift : drift;

                int32_t newAnchorP;
                if (oldAnchorT <= 0.0 || absDrift >= 500)
                    newAnchorP = reportedPos;                    // snap (first lock / seek)
                else
                    newAnchorP = curInterpolated + drift / 20;   // IIR, tau = 1 s

                {
                    const juce::SpinLock::ScopedLockType lock(anchorLock);
                    positionAnchorMs   = newAnchorP;
                    positionAnchorTime = now;
                }
            }

            // -- Duration in seconds (wparam=1, -1 if not playing) -----------
            LRESULT len = ipcSend(hwnd, 1, IPC_GETOUTPUTTIME);
            if (len >= 0)
                durationSec.store((int32_t)len, std::memory_order_relaxed);

            // -- Playlist position (informational, kept for callers that want
            //    to display it but no longer the trigger for refresh -- see
            //    the title-bar diff logic below).  Same guard as state: an
            //    IPC failure (-1) leaves the previously reported index in
            //    place instead of pretending we're back at position -1.
            LRESULT idx = ipcSend(hwnd, 0, IPC_GETLISTPOS);
            if (idx >= 0)
                playlistIndex.store((int)idx, std::memory_order_relaxed);

            // -- Track-change detection by title-bar diff -------------------
            // Reading IPC_GETLISTPOS used to be how we decided to refresh
            // artist + title, but AIMP (and to a lesser extent MusicBee)
            // sometimes leave the playlist index unchanged across what is
            // visibly a track change to the user -- playing the same file
            // twice, opening a file outside the playlist, switching to a
            // queued track, etc.  Diffing the title bar string instead
            // catches every visible change because the title is the same
            // surface the user sees in the player window.  One Win32 call
            // per poll, no allocation in the common "no change" path.
            juce::String currentTitleBar = fetchTitleBar(hwnd);
            if (currentTitleBar.isNotEmpty() && currentTitleBar != lastSeenTitleBar)
            {
                lastSeenTitleBar = currentTitleBar;
                parseTitleBarIntoMetadata(currentTitleBar);

                // Force-reset position and duration on track change.
                //
                // Winamp / WACUP / AIMP have a brief window after a track
                // transition where IPC_GETOUTPUTTIME (both position and
                // length) returns -1 -- our step-2 / step-3 fetches above
                // explicitly do not overwrite the cached value when the
                // IPC returns a negative result, so without this reset
                // the engine would read the previous track's last
                // position (and previous track's duration) for the first
                // tick or so after the title bar already changed, sending
                // garbage timecode to the receivers for one frame and
                // mis-matching the TrackMap key (which uses duration as a
                // tiebreaker between same-titled tracks).
                //
                // Setting both to 0 here means "new track, starting from
                // the beginning, duration not yet known".  The very next
                // IPC poll will refine both values as soon as Winamp has
                // decoded enough of the file to know them.  We also
                // re-anchor the interpolation clock so getPositionMs()
                // starts counting from "now" instead of carrying the
                // previous track's anchor forward.
                {
                    const juce::SpinLock::ScopedLockType lock(anchorLock);
                    positionAnchorMs   = 0;
                    positionAnchorTime = juce::Time::getMillisecondCounterHiRes();
                }
                durationSec.store(0, std::memory_order_relaxed);
            }

            lastPollTime.store(juce::Time::getMillisecondCounterHiRes(),
                               std::memory_order_relaxed);

            juce::Thread::sleep(kPollIntervalMs);
        }
    }

    //--------------------------------------------------------------------------
    // Title-bar metadata parsing.
    //
    // Winamp's main window title is "N. Artist Name - Track Title - Winamp"
    // (the trailing "- Winamp" can be "- WACUP" or other names depending on
    // the fork).  We strip the leading "N. " playlist-position prefix and
    // the trailing "- <player name>", then split the remainder on the LAST
    // " - " separator into artist + title.  If no separator is present we
    // treat the whole remainder as the title and leave the artist empty,
    // which matches what STC's TrackMap already does for tracks that have
    // only a title.
    //
    // Reads via GetWindowTextW so non-ASCII tags survive.
    //--------------------------------------------------------------------------
    /// Parse a title-bar string into artist + title and update the cache.
    ///
    /// Caller is responsible for having already read the window title (the
    /// run loop does this once per poll cycle and reuses the result for
    /// both the change-detection diff and this parse, so we never do two
    /// `GetWindowTextW` calls back to back on the same poll).
    void parseTitleBarIntoMetadata(const juce::String& titleBarText)
    {
        if (titleBarText.isEmpty())
        {
            // Defensive: never clear the cache from an empty string -- a
            // transient empty read on an otherwise-running player would
            // cause the engine's track-change detector to fire spurious
            // "track changed to <empty>" events and wipe the TrackMap
            // lookup for the current track.  Leave the previous metadata
            // in place; the next non-empty read will refresh it.
            return;
        }

        juce::String full = titleBarText;

        // Strip a trailing " - <player name>".  The player names we want to
        // recognise are the well-known forks ("Winamp", "WACUP") plus the
        // Winamp-IPC-emulating players (AIMP, MusicBee) that some users run
        // through this same input.  We are conservative and only strip the
        // recognised names so a track legitimately titled "Foo - Bar -
        // Winamp" does not get misparsed.  Adding a new player means
        // adding one line here and (optionally) verifying its IPC subset
        // matches Winamp's; the rest of the parser is player-agnostic.
        static const char* const kPlayerSuffixes[] =
            { " - Winamp", " - WACUP", " - winamp.com", " - AIMP", " - MusicBee" };
        for (auto* suffix : kPlayerSuffixes)
        {
            if (full.endsWith(suffix))
            {
                full = full.dropLastCharacters((int)juce::String(suffix).length());
                break;
            }
        }
        full = full.trim();

        // Strip leading "N. " playlist-position prefix where N is one or more
        // digits.  juce's containsAnyOf + regex would be overkill; we walk
        // the first characters manually.
        int dotIdx = -1;
        for (int i = 0; i < full.length() && i < 8; ++i)
        {
            auto c = full[i];
            if (c == '.') { dotIdx = i; break; }
            if (c < '0' || c > '9') break;
        }
        if (dotIdx > 0 && dotIdx + 1 < full.length() && full[dotIdx + 1] == ' ')
            full = full.substring(dotIdx + 2);

        // Split on the LAST " - " in case the title or artist itself contains
        // " - " (Winamp's own title rendering always uses " - " between
        // artist and title).
        int sep = full.lastIndexOf(" - ");
        juce::String newArtist, newTitle;
        if (sep > 0)
        {
            newArtist = full.substring(0, sep).trim();
            newTitle  = full.substring(sep + 3).trim();
        }
        else
        {
            newTitle = full.trim();
        }

        const juce::SpinLock::ScopedLockType lock(metadataLock);
        artist = newArtist;
        title  = newTitle;
    }

    /// Read the current title bar text into a juce::String.
    ///
    /// Cheap Win32 call; safe to do on every poll cycle.  Empty result on
    /// failure (caller should not interpret that as "no track" -- see the
    /// guard at the top of parseTitleBarIntoMetadata).
    juce::String fetchTitleBar(HWND hwnd) const
    {
        wchar_t buf[512];
        int n = ::GetWindowTextW(hwnd, buf, (int)(sizeof(buf) / sizeof(buf[0])));
        if (n <= 0) return {};
        return juce::String(juce::CharPointer_UTF16(
                                (const juce::CharPointer_UTF16::CharType*)buf));
    }

    //--------------------------------------------------------------------------
    std::atomic<bool>    isRunningFlag    { false };
    std::atomic<bool>    winampConnected  { false };
    std::atomic<State>   state            { State::Stopped };
    std::atomic<int32_t> durationSec      { 0 };
    std::atomic<int>     playlistIndex    { -1 };
    std::atomic<double>  lastPollTime     { 0.0 };

    // Position anchor: a (position, wall-clock-time) pair updated by the
    // poll thread with PLL-style filtering and read by the engine via
    // getPositionMs() with interpolation.  Held together under a SpinLock
    // so the engine cannot observe a partially-updated pair (the two
    // fields must move together for the interpolation maths to stay
    // correct -- see getPositionMs).  Mutable so the const getter can
    // take the lock.
    mutable juce::SpinLock anchorLock;
    int32_t                positionAnchorMs   { 0 };
    double                 positionAnchorTime { 0.0 };

    juce::SpinLock metadataLock;
    juce::String   artist;
    juce::String   title;

    // Thread-local to the polling thread (run() is the only reader/writer);
    // used to detect track changes by diffing the host window's title bar
    // text on every poll.  See the run loop for the rationale -- playlist
    // index is not reliable on every Winamp-IPC-emulating player.
    juce::String   lastSeenTitleBar;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WinampInput)
};

#else  // ===================================================================
       // Non-Windows stub.  Always reports "not connected" so the engine and
       // UI can compile and link the same way on macOS without scattering
       // #ifdefs through every call site.
       // ===================================================================

class WinampInput
{
public:
    enum class State { Stopped, Playing, Paused };

    bool         start()             { return false; }
    void         stop()              {}
    bool         getIsRunning()  const { return false; }
    bool         isConnected()   const { return false; }
    bool         isReceiving()   const { return false; }
    State        getState()      const { return State::Stopped; }
    int32_t      getPositionMs() const { return 0; }
    int32_t      getDurationSec()const { return 0; }
    int          getPlaylistIndex() const { return -1; }
    juce::String getArtist()     const { return {}; }
    juce::String getTitle()      const { return {}; }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WinampInput)
};

#endif  // JUCE_WINDOWS
