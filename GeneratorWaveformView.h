// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

#pragma once
#include <JuceHeader.h>
#include <climits>
#include <cmath>
#include <optional>
#include "GeneratorAudioPlayer.h"
#include "TimecodeEngine.h"
#include "TimecodeCore.h"

//==============================================================================
// GeneratorWaveformView -- Renders the waveform of the audio file currently
// loaded in the Generator's audio player, draws a playback cursor at the
// generator's position, and (in full mode) optionally allows click-to-seek
// and time-axis markers.
//
// Two display modes:
//   Mini:  compact overview, no markers, no interaction (useful as a thumbnail
//          embedded in the Generator panel).
//   Full:  large display with file-time markers above and absolute-TC markers
//          below the waveform, click-to-seek enabled.
//
// The view holds non-owning pointers to a GeneratorAudioPlayer (for the
// thumbnail) and a TimecodeEngine (for the cursor position and seek).  The
// parent is responsible for resetting these to nullptr before either is
// destroyed.
//
// Repaint cadence:
//   - Cursor is repainted at ~30Hz via a Timer.
//   - Waveform peaks repaint on AudioThumbnail change notifications (JUCE
//     emits these as the background peak generation progresses).
//
// The view shows the file once.  With "Loop audio file when it ends" the
// timecode counts on past the file's end while the player loops the file,
// so the cursor and the A/B points are folded by the file's length
// (cursorFileSeconds, fileSecondsAt), and a point of the view stands for the
// pass of the file the generator is in (pointMs): click-to-seek, drag, the
// hover readout and the TC ruler (#24).  Both come from one split of the
// timecode into a pass and a place in it (placeInFile), so they agree at a
// pass boundary.  Under "Loop timecode" a point stands for the first pass
// (currentPass).
//==============================================================================
class GeneratorWaveformView : public juce::Component,
                              private juce::ChangeListener,
                              private juce::Timer
{
public:
    enum class Mode { Mini, Full };

    GeneratorWaveformView()
    {
        startTimerHz(30);
    }

    ~GeneratorWaveformView() override
    {
        stopTimer();
        if (player != nullptr)
            player->getThumbnail().removeChangeListener(this);
    }

    //==========================================================================
    // Configuration
    //==========================================================================
    void setMode(Mode m)
    {
        mode = m;
        repaint();
    }

    void setAudioPlayer(GeneratorAudioPlayer* p)
    {
        if (player == p) return;
        if (player != nullptr)
            player->getThumbnail().removeChangeListener(this);
        player = p;
        if (player != nullptr)
            player->getThumbnail().addChangeListener(this);
        repaint();
    }

    /// The engine drives both cursor position (via getGeneratorCurrentMs) and
    /// the seek action on click (via setGeneratorPosition).
    void setEngine(TimecodeEngine* e) { engine = e; repaint(); }

    /// Where a point `sec` seconds after Start TC falls in a file `len`
    /// seconds long (> 0), with `looping` the player looping the file
    /// ("Loop audio file when it ends", or the timecode loop's gapless
    /// path; #24): the pass of the file (0 = the first) and the seconds
    /// into that pass.  While the player loops, both come from one floor,
    /// so they never disagree at a pass boundary: a point within
    /// kPassEdgeSec of one belongs to the pass it begins.  Otherwise pass 0
    /// and `sec` as it is (past the file's end, or below 0 before Start
    /// TC).  What this view's cursor and A/B points use, the pass its seeks
    /// take under "Loop audio" (currentPass), and GeneratorWaveformWindow's
    /// file-time label (the cursor and the label through
    /// cursorFileSeconds).  Pure (any thread).
    struct FilePlace { double pass; double sec; };
    static FilePlace placeInFile(double sec, double len, bool looping)
    {
        if (! looping || sec < len - kPassEdgeSec)
            return { 0.0, sec };
        double pass   = std::floor(sec / len);
        double inPass = sec - pass * len;
        if (inPass < 0.0)                 { pass -= 1.0; inPass += len; }
        if (inPass >= len - kPassEdgeSec) { pass += 1.0; inPass = 0.0; }
        return { pass, inPass };
    }

    /// How near a pass boundary a point counts as on it: 1 us.  A click at
    /// the waveform's left edge in a later pass puts the timecode on Start
    /// TC + pass * the file's length, which (timecode - Start TC) / 1000
    /// gives back a few ulps off either way (under 1e-10 s at any time of
    /// day); 1 us is far above that and far below a sample (21 us at
    /// 48 kHz) or a frame.
    static constexpr double kPassEdgeSec = 1.0e-6;

    /// Where the cursor of `e`'s generator goes, in seconds into a file
    /// `len` seconds long (> 0), with `looping` the player looping the
    /// file: placeInFile's place, except for a position at or past Stop TC
    /// on a pass boundary from the second pass on -- a stop at Stop TC a
    /// whole number of file lengths after Start TC under "Loop audio file
    /// when it ends".  That is drawn at the end of the pass before, the
    /// right edge, as a stop at a file's end is, and not at the start of a
    /// pass the generator does not play (PLAY there stops at once; a click
    /// takes the pass before, pointMs).  Not while the timecode loop acts:
    /// it goes round instead of stopping.  What this view's cursor and
    /// GeneratorWaveformWindow's file-time label use.  Message thread.
    static double cursorFileSeconds(const TimecodeEngine& e, double len, bool looping)
    {
        const double curMs  = e.getGeneratorCurrentMs();
        const double stopMs = e.getGeneratorStopMs();
        const auto   place  = placeInFile((curMs - e.getGeneratorStartMs()) / 1000.0, len, looping);
        if (place.pass > 0.0 && place.sec < kPassEdgeSec && stopMs > 0.0
            && curMs >= stopMs - kPassEdgeSec * 1000.0 && ! e.isGeneratorLoopTimecodeActive())
            return len;
        return place.sec;
    }

    /// Optional click handler called when the user clicks the waveform in
    /// Mini mode.  Allows the parent to react (e.g. open the full window).
    /// Has no effect in Full mode where click is already used for seeking.
    std::function<void()> onMiniClick;

    /// Optional check used in Full mode to refuse seeks while Show Lock (or
    /// any other operational lock) is engaged.  Returns true to block seeks.
    /// Empty / not set = always allow.
    std::function<bool()> isLockedFn;

    /// True when `e`'s generator runs the preset's timecode loop (#24): in
    /// transport mode (not Clock mode, which never loops), with "Loop
    /// timecode" acting (TimecodeEngine::isGeneratorLoopTimecodeActive: the
    /// flag on and an end at least 1 s after Start TC; with no Stop TC it
    /// reads false while the file is still loading).  What the waveform
    /// window's LOOP TC, the panel's STOP TC (LOOP) label and this view's
    /// end mark show (GeneratorWaveformWindow::loopTimecodeShown).
    /// Message thread.
    static bool loopTimecodeShown(const TimecodeEngine& e)
    {
        return e.getActiveInput() == TimecodeEngine::InputSource::SystemTime
            && ! e.getGeneratorClockMode()
            && e.isGeneratorLoopTimecodeActive();
    }

private:
    //==========================================================================
    void changeListenerCallback(juce::ChangeBroadcaster*) override
    {
        // AudioThumbnail emits change notifications on the message thread as
        // peak generation progresses.
        repaint();
    }

    void timerCallback() override
    {
        // Cheap: JUCE skips paint() for hidden components.  When playing, we
        // repaint to advance the cursor.  When paused/stopped the cursor is
        // static but we still repaint occasionally so external state changes
        // (preset load, Start TC change) get reflected without an explicit
        // repaint() call from the parent.
        repaint();
    }

    //==========================================================================
    juce::Rectangle<int> getWaveArea() const
    {
        auto area = getLocalBounds();
        if (mode == Mode::Full)
        {
            area.removeFromTop(kMarkerAreaH);
            area.removeFromBottom(kMarkerAreaH);
        }
        return area;
    }

    //==========================================================================
    void paint(juce::Graphics& g) override
    {
        auto fullArea = getLocalBounds();
        g.fillAll(bgCol);

        if (player == nullptr)
        {
            paintEmpty(g, fullArea, "(no audio player)");
            return;
        }

        auto& thumb = player->getThumbnail();
        if (! player->hasFileLoaded())
        {
            // Distinguish "no file configured" from "file configured but
            // failed to load" (path missing, format unsupported, etc.).
            const auto err = player->getLoadError();
            paintEmpty(g, fullArea, err.isNotEmpty() ? err : juce::String("(no audio file)"));
            return;
        }

        const double totalLen = thumb.getTotalLength();   // seconds
        if (totalLen <= 0.0)
        {
            paintEmpty(g, fullArea, "loading\u2026");      // loading…
            return;
        }

        const auto waveArea = getWaveArea();   // const: not mutated below
        if (waveArea.isEmpty()) return;

        // --- Waveform ---
        g.setColour(waveformCol);
        if (thumb.getNumChannels() >= 2)
        {
            // Stereo: L on top, R on bottom, both sharing a single centre
            // line (waveArea.getCentreY()) — matches the look of Ableton /
            // Logic / most DAWs.  Trick: draw each channel using the FULL
            // waveArea as the geometry rect so the bipolar peaks are
            // centred on the shared baseline, then clip to that channel's
            // half so only "its side" of the bipolar shape is visible.
            const int  centreY    = waveArea.getCentreY();
            const auto topHalf    = waveArea.withBottom(centreY);
            const auto bottomHalf = waveArea.withTop(centreY);

            g.saveState();
            g.reduceClipRegion(topHalf);
            thumb.drawChannel(g, waveArea, 0.0, totalLen, 0, 1.0f);
            g.restoreState();

            g.saveState();
            g.reduceClipRegion(bottomHalf);
            thumb.drawChannel(g, waveArea, 0.0, totalLen, 1, 1.0f);
            g.restoreState();
        }
        else
        {
            thumb.drawChannel(g, waveArea, 0.0, totalLen, 0, 1.0f);
        }

        // --- Tick lines + bottom/top label strips (Full mode only) ---
        // Drawn BEFORE the cursor so the cursor always sits on top of ticks.
        if (mode == Mode::Full)
            paintTimeMarkers(g, waveArea, totalLen);

        // --- Hover cursor (Full mode only): the line under the A/B, the cues
        // and the playhead, its readout over everything (drawn last) ---
        // Shows where a click would land while the mouse is over the wave area.
        const bool hovering = mode == Mode::Full && hoverX >= 0 && engine != nullptr;
        const int  hx       = juce::jlimit(waveArea.getX(), waveArea.getRight() - 1, hoverX);
        if (hovering)
        {
            // Vertical hover line (semi-transparent grey).
            g.setColour(hoverCursorCol);
            g.fillRect(hx, waveArea.getY(), 1, waveArea.getHeight());
        }

        // --- Where the timecode loop ends (#24), in the cyan of the
        // window's LOOP TC, while that shows: the file's end (the right
        // edge) on the gapless path, Stop TC on the seek path.  A 2-px line
        // ending at the end's pixel, under the A/B, the cues and the cursor.
        // Not folded (fileSecondsAt): on the gapless path the end is the
        // file's length, which the player's loop would fold to 0.
        if (engine != nullptr && loopTimecodeShown(*engine))
        {
            const double endSec = (engine->getGeneratorLoopTimecodeEndMs()
                                   - engine->getGeneratorStartMs()) / 1000.0;
            if (endSec > 0.0)
            {
                const int endX = waveArea.getX()
                               + (int)((juce::jmin(endSec, totalLen) / totalLen) * waveArea.getWidth());
                g.setColour(loopTcEndCol);
                g.fillRect(juce::jlimit(waveArea.getX(), waveArea.getRight() - 2, endX - 1),
                           waveArea.getY(), 2, waveArea.getHeight());
            }
        }

        // --- A/B loop range (drawn under cues but above waveform body) ---
        // Tinted overlay between In and Out + two distinctive vertical
        // markers.  Drawn armed -- the brighter tint -- only while the
        // generator follows the loop (isGeneratorAbLoopFollowed); otherwise
        // markers are still drawn if individual points are placed, so the
        // operator can see where they would land before arming the loop.
        // An A/B armed but ignored by the preset's timecode loop (#24: not
        // inside it) gets the dim tint and dashed markers, while the toggle
        // stays on as the operator set it.  Each point is drawn where it
        // falls in the file (fileSecondsAt), so under "Loop audio" one set
        // in a later pass shows too.
        if (engine != nullptr)
        {
            const double loopInMs      = engine->getGeneratorLoopInMs();
            const double loopOutMs     = engine->getGeneratorLoopOutMs();
            const bool   loopFollowed  = engine->isGeneratorAbLoopFollowed();
            const bool   loopIgnored   = engine->isGeneratorLoopActive() && ! loopFollowed;

            auto secToX = [&](double posSec) -> int
            {
                if (posSec < 0.0 || posSec > totalLen) return INT_MIN;
                return waveArea.getX() + (int)((posSec / totalLen) * waveArea.getWidth());
            };

            // Out is the loop's exclusive end: on a pass boundary it ends the
            // band at the right edge (fileEndSecondsAt).
            const int inX  = (loopInMs  > 0.0) ? secToX(fileSecondsAt(loopInMs, totalLen))     : INT_MIN;
            const int outX = (loopOutMs > 0.0) ? secToX(fileEndSecondsAt(loopOutMs, totalLen)) : INT_MIN;

            // Range fill -- amber tint between In and Out, lighter when
            // the loop is followed, dimmer otherwise.  Under "Loop audio"
            // an A/B over the file's end (Out folded at or before In,
            // compared in seconds, not pixels, so one less than a pixel
            // short of the file's length still shows) is drawn in two parts,
            // In to the right edge and the left edge to Out, and one a
            // file's length or longer (within kPassEdgeSec, so rounding at
            // large timecodes does not lose it) covers the whole file.
            if (inX != INT_MIN && outX != INT_MIN)
            {
                g.setColour(loopFollowed ? juce::Colour(0x33FFA500)   // amber 20%
                                         : juce::Colour(0x14FFA500)); // amber 8%
                const int bandY = waveArea.getY(), bandH = waveArea.getHeight();
                if (player->isLooping() && loopOutMs - loopInMs >= (totalLen - kPassEdgeSec) * 1000.0)
                    g.fillRect(waveArea);
                else if (player->isLooping() && loopOutMs > loopInMs
                         && fileEndSecondsAt(loopOutMs, totalLen) <= fileSecondsAt(loopInMs, totalLen))
                {
                    g.fillRect(inX, bandY, waveArea.getRight() - inX, bandH);
                    g.fillRect(waveArea.getX(), bandY, outX - waveArea.getX(), bandH);
                }
                else if (outX > inX)
                    g.fillRect(inX, bandY, outX - inX, bandH);
            }

            // A marker: a 2-px amber line, slightly thicker than cues;
            // dashed (4 px on, 4 off) while the loop is ignored.
            auto marker = [&](int x)
            {
                g.setColour(juce::Colour(0xFFFFAA00));
                if (! loopIgnored)
                {
                    g.fillRect(x, waveArea.getY(), 2, waveArea.getHeight());
                    return;
                }
                for (int y = waveArea.getY(); y < waveArea.getBottom(); y += 8)
                    g.fillRect(x, y, 2, juce::jmin(4, waveArea.getBottom() - y));
            };
            if (inX != INT_MIN)
                marker(inX);
            // Out marker -- same colour, distinguishable by being the right
            // edge of the tinted band.
            if (outX != INT_MIN)
                marker(outX - 1);
        }

        // --- Cue markers (drawn before the cursor so the cursor stays
        // visible on top when it crosses a cue) ---
        if (engine != nullptr)
        {
            const auto& cues   = engine->getRawGeneratorCues();
            const double startMs = engine->getGeneratorStartMs();
            const double fps    = frameRateToDouble(engine->getCurrentFps());
            for (const auto& gc : cues)
            {
                // Project the cue's absolute TC onto the audio file
                // timeline.  Cues that fall outside [startMs, startMs +
                // audioLength*1000] just don't have a visible marker --
                // they still fire correctly at runtime; this is purely
                // a visual.  Not folded by the file's length: under "Loop
                // audio file when it ends" cues fire on the first pass
                // only.
                const double cuePosMs = (double) gc.positionMs(fps);
                const double posSec   = (cuePosMs - startMs) / 1000.0;
                if (posSec < 0.0 || posSec > totalLen) continue;

                const int cueX = waveArea.getX()
                               + (int)((posSec / totalLen) * waveArea.getWidth());
                // Subtle white-ish vertical line so it's visible against
                // both the blue waveform and the dark background, but
                // dimmer than the playback cursor (which uses cursorCol).
                g.setColour(juce::Colour(0xCCCCCCCC));
                g.fillRect(cueX, waveArea.getY(), 1, waveArea.getHeight());
            }
        }

        // --- Cursor (drawn after the ticks, the A/B and the cues so it stays
        // visible over them; only the hover readout goes over it) ---
        if (engine != nullptr)
        {
            const double curPosSec = juce::jlimit(0.0, totalLen,
                                                  cursorFileSeconds(*engine, totalLen, player->isLooping()));

            int cursorX = waveArea.getX()
                        + (int)((curPosSec / totalLen) * waveArea.getWidth());
            // Keep the 1-px cursor inside the wave area at both edges.
            cursorX = juce::jlimit(waveArea.getX(), waveArea.getRight() - 1, cursorX);
            g.setColour(cursorCol);
            g.fillRect(cursorX, waveArea.getY(), 1, waveArea.getHeight());
        }

        // --- Hover readout, last: over the timecode loop's end, the A/B, the
        // cues and the cursor, so none of them crosses its text ---
        if (hovering)
        {
            // Time tooltip: "M:SS  HH:MM:SS:FF"  next to the cursor: the
            // TC a click there gives, in the pass the generator is in.
            const double frac    = (double)(hx - waveArea.getX()) / (double) waveArea.getWidth();
            const double hoverSec = juce::jlimit(0.0, totalLen, frac * totalLen);
            const double hoverAbsMs = pointMs(hoverSec, currentPass(totalLen), totalLen);
            const Timecode hoverTc  = wallClockToTimecode(hoverAbsMs, engine->getCurrentFps());
            const juce::String label = formatFileTime(hoverSec) + "  " + hoverTc.toString();

            const int   labelW    = 140;
            const int   labelH    = 18;
            const int   margin    = 6;
            // Place to the right of the line; flip to the left at the right edge.
            int labelX = hx + margin;
            if (labelX + labelW > waveArea.getRight())
                labelX = hx - margin - labelW;
            const int labelY = waveArea.getY() + 2;

            g.setColour(juce::Colour(0xCC000000));
            g.fillRect(labelX, labelY, labelW, labelH);
            g.setColour(hoverTooltipBorderCol);
            g.drawRect(labelX, labelY, labelW, labelH, 1);
            g.setColour(hoverTooltipTextCol);
            g.setFont(juce::Font(juce::FontOptions(10.5f)));
            g.drawText(label, labelX + 4, labelY, labelW - 8, labelH,
                       juce::Justification::centredLeft, false);
        }
    }

    //==========================================================================
    // Passes of the file (#24).  With "Loop audio file when it ends" the
    // player loops the file and the timecode counts on past its end, so
    // timecode - Start TC (the relation every seek uses) passes the file's
    // length from the second pass on: the cursor sat at the right edge, and
    // a click took the timecode back to the first pass.  The cursor, the
    // A/B points and the window's label are folded whenever the player
    // loops the file (cursorFileSeconds, fileSecondsAt), so also on the
    // timecode loop's gapless path.  There the timecode itself is folded
    // and stays in the file while it plays, but two things can still lie
    // past the file's end, and are drawn folded where before #24 they were
    // drawn at the right edge or not at all: an A/B point set under "Loop
    // audio", and the timecode left past the loop's end while paused
    // (Start TC moved back, an OSC jog), the cursor then drawn where PLAY
    // folds the timecode to.
    // Passes (currentPass) are for "Loop audio" only: with "Loop timecode"
    // on, a click, a drag, the hover readout and the ruler take the first
    // pass, inside the timecode loop, as before #24.  Without a loop the
    // player does not loop and nothing changes.  totalLen is the file's
    // length in seconds (> 0); engine and player are set.  Message thread
    // (paint, the mouse callbacks; the engine's tick writes the timecode
    // there too); isLooping is an atomic.

    /// Where absMs (ms from midnight, the generator's timeline) falls in the
    /// file, in seconds: absMs - Start TC, folded by the file's length while
    /// the player loops the file (placeInFile).  Below 0: before the file.
    double fileSecondsAt(double absMs, double totalLen) const
    {
        return placeInFile((absMs - engine->getGeneratorStartMs()) / 1000.0,
                           totalLen, player->isLooping()).sec;
    }

    /// The same for an exclusive end, the A/B's Out: one on a pass
    /// boundary (within kPassEdgeSec) ends the pass before, at the file's
    /// length (the right edge), where fileSecondsAt begins the next at 0.
    double fileEndSecondsAt(double absMs, double totalLen) const
    {
        const auto place = placeInFile((absMs - engine->getGeneratorStartMs()) / 1000.0,
                                       totalLen, player->isLooping());
        return (place.pass > 0.0 && place.sec < kPassEdgeSec) ? totalLen : place.sec;
    }

    /// The pass of the file the generator is in under "Loop audio file
    /// when it ends" (placeInFile; 0 = the first).  Always 0 while the
    /// player does not loop the file, and with "Loop timecode" on: the
    /// engine then keeps "Loop audio" off and the player loops only on the
    /// timecode loop's gapless path, where a pass past the first is only
    /// ever a timecode left past the loop's end while paused, and a click
    /// there would leave the loop.
    double currentPass(double totalLen) const
    {
        if (engine->getGeneratorLoopTimecode())
            return 0.0;
        return placeInFile((engine->getGeneratorCurrentMs() - engine->getGeneratorStartMs()) / 1000.0,
                           totalLen, player->isLooping()).pass;
    }

    /// The timecode (ms from midnight) that the point fileSec seconds into
    /// the file stands for in pass `pass`: Start TC + (pass * the file's
    /// length + fileSec) * 1000.  From the second pass on, with a Stop TC
    /// (the panel or OSC /stoptime; "Loop audio" stops there), a point at
    /// or past Stop TC stands for the latest earlier pass in which it is
    /// before it: the generator never gets past Stop TC in a later pass,
    /// and from Stop TC or a timecode past it PLAY stops at once.  The
    /// file's end (the ruler's last tick) is the end of its pass, not the
    /// start of the next, and may be Stop TC itself.  In the first pass a
    /// point past Stop TC is taken as it is, as before #24.
    double pointMs(double fileSec, double pass, double totalLen) const
    {
        const double startMs = engine->getGeneratorStartMs();
        const double stopMs  = engine->getGeneratorStopMs();
        // A point PLAY would start from must lie more than kPassEdgeSec
        // before Stop TC; the file's end, which ends its pass, may be Stop
        // TC itself (within kPassEdgeSec).
        const double edgeSec = fileSec >= totalLen - kPassEdgeSec ? kPassEdgeSec : -kPassEdgeSec;
        if (pass > 0.0 && stopMs > 0.0
            && startMs + (pass * totalLen + fileSec) * 1000.0 > stopMs + edgeSec * 1000.0)
        {
            const double latest = std::floor(((stopMs - startMs) / 1000.0 - fileSec + edgeSec) / totalLen);
            pass = juce::jlimit(0.0, pass - 1.0, latest);
        }
        return startMs + (pass * totalLen + fileSec) * 1000.0;
    }

    //==========================================================================
    void paintEmpty(juce::Graphics& g, juce::Rectangle<int> area, const juce::String& text)
    {
        g.setColour(emptyTextCol);
        g.setFont(juce::Font(juce::FontOptions(11.0f)));
        g.drawText(text, area, juce::Justification::centred);
    }

    //==========================================================================
    void paintTimeMarkers(juce::Graphics& g, juce::Rectangle<int> waveArea, double totalLen)
    {
        if (engine == nullptr) return;

        // Choose tick interval based on track length: aim for ~5-10 ticks
        // visible at all times so labels do not overlap.
        const double ticksTarget = 8.0;
        double ideal = totalLen / ticksTarget;
        const double candidates[] = { 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1200, 1800, 3600 };
        double interval = candidates[0];
        for (double c : candidates)
        {
            interval = c;
            if (c >= ideal) break;
        }

        const double pass   = currentPass(totalLen);
        const FrameRate fps = engine->getCurrentFps();

        g.setFont(juce::Font(juce::FontOptions(9.5f)));

        // Top strip: file time (0:00, 1:00, ...)
        auto topStrip    = juce::Rectangle<int>(waveArea.getX(), waveArea.getY() - kMarkerAreaH,
                                                 waveArea.getWidth(), kMarkerAreaH);
        // Bottom strip: absolute TC, in the pass the generator is in (the TC
        // a click there gives, pointMs)
        auto bottomStrip = juce::Rectangle<int>(waveArea.getX(), waveArea.getBottom(),
                                                 waveArea.getWidth(), kMarkerAreaH);

        // Integer tick counter: t is computed per tick, not accumulated.
        const int lastTick = (int) std::floor((totalLen + 0.0001) / interval);
        for (int tick = 0; tick <= lastTick; ++tick)
        {
            const double t    = (double) tick * interval;
            const double frac = t / totalLen;
            const int x = waveArea.getX() + (int)(frac * waveArea.getWidth());

            // Tick line through the wave area
            g.setColour(tickCol);
            g.fillRect(x, waveArea.getY(), 1, waveArea.getHeight());

            // File time label (top)
            g.setColour(markerTextCol);
            const juce::String fileLabel = formatFileTime(t);
            g.drawText(fileLabel, x - 30, topStrip.getY() + 1, 60, kMarkerAreaH - 2,
                       juce::Justification::centredTop, false);

            // Absolute TC label (bottom)
            const Timecode tc = wallClockToTimecode(pointMs(t, pass, totalLen), fps);
            const juce::String tcLabel = tc.toString();
            g.drawText(tcLabel, x - 40, bottomStrip.getY() + 1, 80, kMarkerAreaH - 2,
                       juce::Justification::centredTop, false);
        }
    }

    //==========================================================================
    static juce::String formatFileTime(double seconds)
    {
        const int total = (int) seconds;
        const int hours = total / 3600;
        const int mins  = (total % 3600) / 60;
        const int secs  = total % 60;
        if (hours > 0)
            return juce::String::formatted("%d:%02d:%02d", hours, mins, secs);
        return juce::String::formatted("%d:%02d", mins, secs);
    }

    //==========================================================================
    void mouseDown(const juce::MouseEvent& e) override
    {
        if (mode == Mode::Mini)
        {
            // Mini view click opens the floating window regardless of whether
            // a file is currently loaded -- the window itself shows the state.
            if (onMiniClick) onMiniClick();
            return;
        }

        gesturePass.reset();   // a new gesture: its pass is taken at its first seek
        seekTo(e);
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        // Treat drag as continuous click so the user can position precisely
        // without releasing.  Skipped in Mini mode and when locked.
        if (mode == Mode::Mini) return;
        if (isLockedFn && isLockedFn()) return;
        seekTo(e);
    }

    /// Full mode: move the generator to the point of the file under the
    /// mouse, in the pass of the file the gesture began in (pointMs; the
    /// pass is taken at its first seek and kept for the rest of a drag, so
    /// a drag while the generator plays does not jump a whole file length
    /// when the timecode enters the next pass).
    void seekTo(const juce::MouseEvent& e)
    {
        // Full mode below requires player + engine + a loaded file to seek.
        if (player == nullptr || engine == nullptr) return;
        if (! player->hasFileLoaded()) return;

        // Refuse if the parent is locked (Show Lock).
        if (isLockedFn && isLockedFn()) return;

        const auto waveArea = getWaveArea();
        if (! waveArea.contains(e.getPosition())) return;

        const double totalLen = player->getThumbnail().getTotalLength();
        if (totalLen <= 0.0) return;

        const double frac = juce::jlimit(0.0, 1.0,
                                          (double)(e.x - waveArea.getX()) / (double) waveArea.getWidth());
        const double seekSec = frac * totalLen;

        if (! gesturePass.has_value())
            gesturePass = currentPass(totalLen);
        const double newGenMs = pointMs(seekSec, *gesturePass, totalLen);
        engine->setGeneratorPosition(newGenMs);
        repaint();
    }

    void mouseMove(const juce::MouseEvent& e) override
    {
        if (mode == Mode::Mini) return;
        if (player == nullptr || ! player->hasFileLoaded()) { hoverX = -1; return; }

        const auto waveArea = getWaveArea();
        if (waveArea.contains(e.getPosition()))
            hoverX = e.x;
        else
            hoverX = -1;
        repaint();
    }

    void mouseEnter(const juce::MouseEvent& e) override
    {
        mouseMove(e);
    }

    void mouseExit(const juce::MouseEvent&) override
    {
        if (hoverX != -1) { hoverX = -1; repaint(); }
    }

    //==========================================================================
    static constexpr int kMarkerAreaH = 14;

    juce::Colour bgCol                  { 0xFF1A1A1A };
    juce::Colour waveformCol            { 0xFFCC8844 };  // amber, matches AUDIO PLAYBACK toggle
    juce::Colour cursorCol              { 0xFFFFFFFF };
    juce::Colour hoverCursorCol         { 0x88AAAAAA };  // semi-transparent grey
    juce::Colour hoverTooltipBorderCol  { 0xFF555555 };
    juce::Colour hoverTooltipTextCol    { 0xFFDDDDDD };
    juce::Colour tickCol                { 0xFF333333 };
    juce::Colour markerTextCol          { 0xFF999999 };
    juce::Colour emptyTextCol           { 0xFF666666 };
    juce::Colour loopTcEndCol           { 0xFF00AAFF };  // the window's LOOP TC (accentCyan)

    GeneratorAudioPlayer* player = nullptr;
    TimecodeEngine*       engine = nullptr;
    Mode                  mode   = Mode::Full;
    int                   hoverX = -1;     // -1 = mouse outside wave area
    std::optional<double> gesturePass;     // the pass a click / drag seeks in (seekTo)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GeneratorWaveformView)
};
