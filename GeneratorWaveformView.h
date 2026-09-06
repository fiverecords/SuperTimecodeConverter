// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

#pragma once
#include <JuceHeader.h>
#include <climits>
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

    /// Optional click handler called when the user clicks the waveform in
    /// Mini mode.  Allows the parent to react (e.g. open the full window).
    /// Has no effect in Full mode where click is already used for seeking.
    std::function<void()> onMiniClick;

    /// Optional check used in Full mode to refuse seeks while Show Lock (or
    /// any other operational lock) is engaged.  Returns true to block seeks.
    /// Empty / not set = always allow.
    std::function<bool()> isLockedFn;

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

        // --- Hover cursor (Full mode only, drawn under the playhead) ---
        // Shows where a click would land while the mouse is over the wave area.
        if (mode == Mode::Full && hoverX >= 0 && engine != nullptr)
        {
            const int hx = juce::jlimit(waveArea.getX(), waveArea.getRight() - 1, hoverX);

            // Vertical hover line (semi-transparent grey).
            g.setColour(hoverCursorCol);
            g.fillRect(hx, waveArea.getY(), 1, waveArea.getHeight());

            // Time tooltip: "M:SS  HH:MM:SS:FF"  next to the cursor.
            const double frac    = (double)(hx - waveArea.getX()) / (double) waveArea.getWidth();
            const double hoverSec = juce::jlimit(0.0, totalLen, frac * totalLen);
            const double hoverAbsMs = engine->getGeneratorStartMs() + hoverSec * 1000.0;
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

        // --- A/B loop range (drawn under cues but above waveform body) ---
        // Tinted overlay between In and Out + two distinctive vertical
        // markers.  Active when loop is enabled and Out > In; otherwise
        // markers are still drawn if individual points are placed, so the
        // operator can see where they would land before arming the loop.
        if (engine != nullptr)
        {
            const double startMs       = engine->getGeneratorStartMs();
            const double loopInMs      = engine->getGeneratorLoopInMs();
            const double loopOutMs     = engine->getGeneratorLoopOutMs();
            const bool   loopActive    = engine->isGeneratorLoopActive();

            auto msToX = [&](double absMs) -> int
            {
                const double posSec = (absMs - startMs) / 1000.0;
                if (posSec < 0.0 || posSec > totalLen) return INT_MIN;
                return waveArea.getX() + (int)((posSec / totalLen) * waveArea.getWidth());
            };

            const int inX  = (loopInMs  > 0.0) ? msToX(loopInMs)  : INT_MIN;
            const int outX = (loopOutMs > 0.0) ? msToX(loopOutMs) : INT_MIN;

            // Range fill -- amber tint between In and Out, lighter when
            // loop is armed, dimmer when only markers are set.
            if (inX != INT_MIN && outX != INT_MIN && outX > inX)
            {
                g.setColour(loopActive ? juce::Colour(0x33FFA500)   // amber 20%
                                       : juce::Colour(0x14FFA500)); // amber 8%
                g.fillRect(inX, waveArea.getY(), outX - inX, waveArea.getHeight());
            }

            // In marker -- amber line, slightly thicker than cues.
            if (inX != INT_MIN)
            {
                g.setColour(juce::Colour(0xFFFFAA00));
                g.fillRect(inX, waveArea.getY(), 2, waveArea.getHeight());
            }
            // Out marker -- same colour, distinguishable by being the right
            // edge of the tinted band.
            if (outX != INT_MIN)
            {
                g.setColour(juce::Colour(0xFFFFAA00));
                g.fillRect(outX - 1, waveArea.getY(), 2, waveArea.getHeight());
            }
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
                // a visual.
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

        // --- Cursor (drawn last so it stays visible over ticks) ---
        if (engine != nullptr)
        {
            const double startMs   = engine->getGeneratorStartMs();
            const double currentMs = engine->getGeneratorCurrentMs();
            const double curPosSec = juce::jlimit(0.0, totalLen, (currentMs - startMs) / 1000.0);

            int cursorX = waveArea.getX()
                        + (int)((curPosSec / totalLen) * waveArea.getWidth());
            // Keep the 1-px cursor inside the wave area at both edges.
            cursorX = juce::jlimit(waveArea.getX(), waveArea.getRight() - 1, cursorX);
            g.setColour(cursorCol);
            g.fillRect(cursorX, waveArea.getY(), 1, waveArea.getHeight());
        }
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

        const double startMs = engine->getGeneratorStartMs();
        const FrameRate fps  = engine->getCurrentFps();

        g.setFont(juce::Font(juce::FontOptions(9.5f)));

        // Top strip: file time (0:00, 1:00, ...)
        auto topStrip    = juce::Rectangle<int>(waveArea.getX(), waveArea.getY() - kMarkerAreaH,
                                                 waveArea.getWidth(), kMarkerAreaH);
        // Bottom strip: absolute TC
        auto bottomStrip = juce::Rectangle<int>(waveArea.getX(), waveArea.getBottom(),
                                                 waveArea.getWidth(), kMarkerAreaH);

        for (double t = 0.0; t <= totalLen + 0.0001; t += interval)
        {
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
            const Timecode tc = wallClockToTimecode(startMs + t * 1000.0, fps);
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

        const double newGenMs = engine->getGeneratorStartMs() + seekSec * 1000.0;
        engine->setGeneratorPosition(newGenMs);
        repaint();
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        // Treat drag as continuous click so the user can position precisely
        // without releasing.  Skipped in Mini mode and when locked.
        if (mode == Mode::Mini) return;
        if (isLockedFn && isLockedFn()) return;
        mouseDown(e);
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

    GeneratorAudioPlayer* player = nullptr;
    TimecodeEngine*       engine = nullptr;
    Mode                  mode   = Mode::Full;
    int                   hoverX = -1;     // -1 = mouse outside wave area

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GeneratorWaveformView)
};
