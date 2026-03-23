// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter
//
// StageLinQView -- External window showing Denon StageLinQ network state.
//
// Displays up to 4 decks with:
//   - Track info (artist, title)
//   - BPM, pitch %, play state
//   - Beat-in-bar indicator
//   - Raw timecode from playhead
//   - TrackMap offset (if mapped)
//   - Engine assignment (which STC engine is monitoring this deck)
//   - Channel fader + crossfader visualization
//
// Architecture: Owns its own 30Hz Timer, reads data from shared objects
// (StageLinQInput, TrackMap, engines) via const getters.
// No artwork or waveform (requires FileTransfer service, planned for future).

#pragma once
#include <JuceHeader.h>
#include "StageLinQInput.h"
#include "StageLinQDbClient.h"
#include "TimecodeEngine.h"
#include "AppSettings.h"
#include "CustomLookAndFeel.h"
#include <vector>
#include <memory>

//==============================================================================
// StageLinQViewComponent -- Main content: 4 deck panels + mixer strip
//==============================================================================
class StageLinQViewComponent : public juce::Component,
                                public juce::Timer
{
public:
    StageLinQViewComponent(StageLinQInput& slq,
                           StageLinQDbClient& slqDb,
                           TrackMap& tmap,
                           std::vector<std::unique_ptr<TimecodeEngine>>& engs)
        : stageLinQ(slq), dbClient(slqDb), trackMap(tmap), engines(engs)
    {
        setSize(800, 560);

        // --- Toolbar: layout toggle ---
        addAndMakeVisible(btnLayout);
        btnLayout.setButtonText("4x1");
        btnLayout.setColour(juce::TextButton::buttonColourId, bgDeck);
        btnLayout.setColour(juce::TextButton::textColourOffId, textMid);
        btnLayout.setClickingTogglesState(false);
        btnLayout.onClick = [this]
        {
            layoutHorizontal = !layoutHorizontal;
            btnLayout.setButtonText(layoutHorizontal ? "2x2" : "4x1");
            resized();
            repaint();
            if (onLayoutChanged) onLayoutChanged();
        };

        startTimerHz(30);
    }

    ~StageLinQViewComponent() override { stopTimer(); }

    // --- Layout state ---
    bool getLayoutHorizontal() const { return layoutHorizontal; }
    void setLayoutHorizontal(bool h)
    {
        layoutHorizontal = h;
        btnLayout.setButtonText(layoutHorizontal ? "2x2" : "4x1");
        resized(); repaint();
    }

    // --- Callbacks ---
    std::function<void()> onLayoutChanged;
    std::function<void()> onTrackMapChanged;

private:
    StageLinQInput& stageLinQ;
    StageLinQDbClient& dbClient;
    TrackMap& trackMap;
    std::vector<std::unique_ptr<TimecodeEngine>>& engines;

    bool layoutHorizontal = false;

    juce::TextButton btnLayout;

    // Colors (Denon green accent)
    const juce::Colour bgMain    { 0xFF0D0E12 };
    const juce::Colour bgDeck    { 0xFF141519 };
    const juce::Colour borderCol { 0xFF1E2028 };
    const juce::Colour textBright{ 0xFFE0E0E0 };
    const juce::Colour textMid   { 0xFF90A4AE };
    const juce::Colour textDim   { 0xFF546E7A };
    const juce::Colour accentGreen { 0xFF00CC66 };   // Denon brand green
    const juce::Colour accentCyan  { 0xFF00BCD4 };
    const juce::Colour accentAmber { 0xFFFFA000 };
    const juce::Colour accentRed   { 0xFFFF5252 };

    //--------------------------------------------------------------------------
    // Per-deck cached state (read in timerCallback, used in paint)
    //--------------------------------------------------------------------------
    struct DeckState
    {
        bool active = false;
        juce::String model;
        juce::String playState;
        double bpm = 0.0;
        double speed = 0.0;           // pitch multiplier (1.0 = 0%)
        bool isPlaying = false;
        uint8_t beatInBar = 1;
        uint32_t playheadMs = 0;
        uint32_t trackLenSec = 0;
        float posRatio = 0.0f;
        juce::String artist;
        juce::String title;
        double faderPos = 0.0;        // channel fader 0-1
        Timecode timecode;
        juce::String fpsStr;

        // TrackMap
        bool trackMapped = false;
        juce::String offset;
        Timecode offsetTimecode;

        // Engine assignment
        juce::StringArray engineNames;
        int bpmSessionOverride = TimecodeEngine::kBpmNoOverride;
        int bpmTrackMapValue = 0;

        // Artwork (from StageLinQDbClient)
        juce::Image artwork;
        juce::String lastNetworkPath;  // to detect changes

        // Waveform (from StageLinQDbClient)
        DenonWaveformData waveform;
        bool waveformLoaded = false;

        // Performance data (from StageLinQDbClient)
        DenonPerformanceData perfData;
        juce::String keyStr;   // musical key string (DB or live)

        // Live state from StateMap (real-time, not from DB)
        bool loopEnabled = false;
        double loopInSamples = -1.0;
        double loopOutSamples = -1.0;
        double loopSizeBeats = 0.0;
        double sampleRate = 44100.0;
        bool keyLock = false;
        bool bleepMode = false;
        bool jogTouch = false;
        int syncMode = 0;
    };

    std::array<DeckState, StageLinQ::kMaxDecks> deckState;
    double crossfaderPos = 0.0;

    //==========================================================================
    // Timer: fetch data from StageLinQInput at 30Hz
    //==========================================================================
    void timerCallback() override
    {
        for (int deck = 0; deck < StageLinQ::kMaxDecks; ++deck)
        {
            int dn = deck + 1;
            auto& ds = deckState[deck];

            ds.active      = stageLinQ.isDeckActive(dn);
            ds.model       = stageLinQ.getPlayerModel(dn);
            ds.playState   = stageLinQ.getPlayStateString(dn);
            ds.bpm         = stageLinQ.getBPM(dn);
            ds.speed       = stageLinQ.getActualSpeed(dn);
            ds.isPlaying   = stageLinQ.isPlayerPlaying(dn);
            ds.beatInBar   = stageLinQ.getBeatInBar(dn);
            ds.playheadMs  = stageLinQ.getPlayheadMs(dn);
            ds.trackLenSec = stageLinQ.getTrackLengthSec(dn);
            ds.posRatio    = stageLinQ.getPlayPositionRatio(dn);
            ds.faderPos    = stageLinQ.getFaderPosition(dn);

            auto tinfo     = stageLinQ.getTrackInfo(dn);
            ds.artist      = tinfo.artist;
            ds.title       = tinfo.title;

            // Artwork + waveform from database (if available)
            if (dbClient.isDatabaseReady())
            {
                auto netPath = stageLinQ.getTrackNetworkPath(dn);
                if (netPath.isNotEmpty() && netPath != ds.lastNetworkPath)
                {
                    // Try to fetch from cache -- DB thread may not have
                    // processed requestMetadata() yet, so data may be empty.
                    auto art = dbClient.getArtworkForTrack(netPath);
                    auto wf  = dbClient.getWaveformForTrack(netPath);
                    auto perf = dbClient.getPerformanceData(netPath);
                    auto dbMeta = dbClient.getTrackByNetworkPath(netPath);

                    // Only commit path once we have at least metadata cached
                    // (artwork/waveform may be NULL for some tracks).
                    // If nothing is cached yet, we'll retry next timer tick.
                    if (dbMeta.valid)
                    {
                        ds.lastNetworkPath = netPath;
                        ds.artwork = art;
                        ds.waveform = wf;
                        ds.waveformLoaded = wf.valid;
                        ds.perfData = perf;
                        ds.keyStr = dbMeta.key;
                    }
                }
            }

            // Live key from StateMap (overrides DB key when available -- updates with key shift)
            auto liveKey = stageLinQ.getCurrentKeyString(dn);
            if (liveKey.isNotEmpty())
                ds.keyStr = liveKey;

            // Live loop state from StateMap (real-time active loop, not stored in DB)
            ds.loopEnabled    = stageLinQ.isLoopEnabled(dn);
            ds.loopInSamples  = stageLinQ.getLoopInPosition(dn);
            ds.loopOutSamples = stageLinQ.getLoopOutPosition(dn);
            ds.loopSizeBeats  = stageLinQ.getLoopSizeInBeats(dn);
            ds.sampleRate     = stageLinQ.getSampleRate(dn);

            // Live deck state badges
            ds.keyLock   = stageLinQ.getKeyLock(dn);
            ds.bleepMode = stageLinQ.isBleep(dn);
            ds.jogTouch  = stageLinQ.isScratchWheelTouched(dn);
            ds.syncMode  = stageLinQ.getSyncMode(dn);

            // Timecode from playhead
            FrameRate fps = FrameRate::FPS_30;  // default -- StageLinQ has no fps detection
            ds.timecode = StageLinQ::playheadToTimecode(ds.playheadMs, fps);
            ds.fpsStr = frameRateToString(fps);

            // TrackMap lookup
            const TrackMapEntry* tmEntry = nullptr;
            ds.trackMapped = false;
            ds.offset = "00:00:00:00";
            ds.offsetTimecode = {};
            if (ds.artist.isNotEmpty() && ds.title.isNotEmpty())
            {
                tmEntry = trackMap.find(ds.artist, ds.title, (int)ds.trackLenSec);
                if (tmEntry != nullptr)
                {
                    ds.trackMapped = true;
                    ds.offset = tmEntry->timecodeOffset;
                    int oH, oM, oS, oF;
                    if (TrackMapEntry::parseTimecodeString(tmEntry->timecodeOffset, oH, oM, oS, oF))
                    {
                        ds.offsetTimecode = applyTimecodeOffset(
                            ds.timecode, fps, oH, oM, oS, oF, fps);
                    }
                }
            }

            // Engine assignment
            ds.engineNames.clear();
            ds.bpmSessionOverride = TimecodeEngine::kBpmNoOverride;
            ds.bpmTrackMapValue   = tmEntry ? tmEntry->bpmMultiplier : 0;
            for (auto& eng : engines)
            {
                if (eng->getActiveInput() == TimecodeEngine::InputSource::StageLinQ
                    && eng->getEffectivePlayer() == dn)
                {
                    ds.engineNames.add(eng->getName());
                    ds.bpmSessionOverride = eng->getBpmPlayerOverride();
                }
            }
        }

        crossfaderPos = stageLinQ.getCrossfaderPosition();

        repaint();
    }

    //==========================================================================
    // Layout
    //==========================================================================
    void paint(juce::Graphics& g) override
    {
        g.fillAll(bgMain);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(6);

        // Toolbar
        auto toolbar = area.removeFromTop(24);
        btnLayout.setBounds(toolbar.removeFromLeft(40));
        area.removeFromTop(4);

        // Crossfader bar at bottom
        auto xfArea = area.removeFromBottom(20);
        area.removeFromBottom(4);

        // Deck panels
        if (layoutHorizontal)
        {
            // 4x1 horizontal row (for docking below another program)
            int deckW = area.getWidth() / StageLinQ::kMaxDecks;
            for (int i = 0; i < StageLinQ::kMaxDecks; ++i)
                deckBounds[i] = juce::Rectangle<int>(
                    area.getX() + i * deckW + (i > 0 ? 2 : 0),
                    area.getY(),
                    deckW - 2, area.getHeight());
        }
        else
        {
            // 2x2 grid
            int halfW = area.getWidth() / 2;
            int halfH = area.getHeight() / 2;
            for (int i = 0; i < StageLinQ::kMaxDecks; ++i)
                deckBounds[i] = juce::Rectangle<int>(
                    area.getX() + (i % 2) * halfW + (i % 2 ? 2 : 0),
                    area.getY() + (i / 2) * halfH + (i / 2 ? 2 : 0),
                    halfW - 2, halfH - 2);
        }

        xfBounds = xfArea;
    }

    void paintOverChildren(juce::Graphics& g) override
    {
        // Paint decks
        for (int i = 0; i < StageLinQ::kMaxDecks; ++i)
            paintDeck(g, deckBounds[i], i);

        // Paint crossfader
        paintCrossfader(g, xfBounds);
    }

    std::array<juce::Rectangle<int>, StageLinQ::kMaxDecks> deckBounds;
    juce::Rectangle<int> xfBounds;

    //==========================================================================
    // Deck painting
    //==========================================================================
    void paintDeck(juce::Graphics& g, juce::Rectangle<int> area, int deckIndex)
    {
        auto& ds = deckState[deckIndex];
        int dn = deckIndex + 1;
        auto af = area.toFloat();

        // Background
        g.setColour(bgDeck);
        g.fillRoundedRectangle(af, 4.0f);
        g.setColour(borderCol);
        g.drawRoundedRectangle(af, 4.0f, 1.0f);

        auto inner = area.reduced(6);
        if (inner.getWidth() < 30 || inner.getHeight() < 30) return;

        //----------------------------------------------------------------------
        // Header: deck number badge + model + play state
        //----------------------------------------------------------------------
        auto headerRow = inner.removeFromTop(20);

        // Deck number badge
        {
            auto badge = headerRow.removeFromLeft(24).toFloat();
            juce::Colour badgeCol = ds.active ? accentGreen : textDim;
            g.setColour(badgeCol.withAlpha(0.2f));
            g.fillRoundedRectangle(badge, 3.0f);
            g.setColour(badgeCol);
            g.drawRoundedRectangle(badge, 3.0f, 1.0f);
            g.setFont(juce::Font(juce::FontOptions(12.0f, juce::Font::bold)));
            g.drawText(juce::String(dn), badge, juce::Justification::centred);
        }
        headerRow.removeFromLeft(4);

        // Model + status
        g.setFont(juce::Font(juce::FontOptions(10.0f)));
        if (ds.active)
        {
            g.setColour(textBright);
            g.drawText(ds.model, headerRow, juce::Justification::centredLeft);

            // Play state badge (right-aligned)
            juce::Colour stateCol = ds.isPlaying ? accentGreen : accentAmber;
            auto stateBadge = headerRow.removeFromRight(60).toFloat().reduced(0, 2);
            g.setColour(stateCol.withAlpha(0.2f));
            g.fillRoundedRectangle(stateBadge, 2.0f);
            g.setColour(stateCol);
            g.setFont(juce::Font(juce::FontOptions(8.0f, juce::Font::bold)));
            g.drawText(ds.playState, stateBadge, juce::Justification::centred);
        }
        else
        {
            g.setColour(textDim);
            g.drawText("OFFLINE", headerRow, juce::Justification::centredLeft);
            return;
        }

        inner.removeFromTop(4);

        //----------------------------------------------------------------------
        // Artwork + Track info row
        //----------------------------------------------------------------------
        // Reserve artwork square (same height as info+bpm rows combined)
        int artSize = 0;
        juce::Rectangle<float> artBounds;
        if (ds.artwork.isValid() && inner.getHeight() >= 40)
        {
            artSize = juce::jmin(50, inner.getHeight() / 3);
            artBounds = inner.removeFromLeft(artSize).toFloat().withHeight((float)artSize);
            inner.removeFromLeft(4);
        }

        // Track info: artist -- title + key
        auto infoRow = inner.removeFromTop(16);
        {
            // Reserve space for key on the right
            juce::Rectangle<int> keyArea;
            if (ds.keyStr.isNotEmpty())
                keyArea = infoRow.removeFromRight(60);

            juce::String trackStr;
            if (ds.artist.isNotEmpty() && ds.title.isNotEmpty())
                trackStr = ds.artist + " -- " + ds.title;
            else if (ds.title.isNotEmpty())
                trackStr = ds.title;
            else
                trackStr = "No track loaded";

            g.setFont(juce::Font(juce::FontOptions(11.0f, ds.title.isNotEmpty() ? juce::Font::bold : juce::Font::italic)));
            g.setColour(ds.title.isNotEmpty() ? accentGreen.brighter(0.4f) : textDim);
            g.drawText(trackStr, infoRow, juce::Justification::centredLeft, true);

            // Musical key (right-aligned, reserved area) + key lock badge
            if (ds.keyStr.isNotEmpty())
            {
                g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
                g.setColour(accentCyan);
                juce::String keyDisplay = ds.keyStr;
                if (ds.keyLock) keyDisplay += " [KL]";
                g.drawText(keyDisplay, keyArea, juce::Justification::centredRight);
            }
        }

        // Draw artwork square (if reserved above)
        if (ds.artwork.isValid() && artSize > 0)
        {
            g.drawImage(ds.artwork, artBounds,
                        juce::RectanglePlacement::centred | juce::RectanglePlacement::onlyReduceInSize);
            g.setColour(borderCol);
            g.drawRoundedRectangle(artBounds, 2.0f, 1.0f);
        }

        inner.removeFromTop(2);

        //----------------------------------------------------------------------
        // BPM + pitch + beat indicator
        //----------------------------------------------------------------------
        auto bpmRow = inner.removeFromTop(18);
        {
            g.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));
            if (ds.bpm > 0.0)
            {
                g.setColour(textBright);
                juce::String bpmStr = juce::String(ds.bpm, 1) + " BPM";

                // Effective BPM multiplier
                int effMult = (ds.bpmSessionOverride != TimecodeEngine::kBpmNoOverride)
                    ? ds.bpmSessionOverride : ds.bpmTrackMapValue;
                if (effMult != 0)
                {
                    double multBpm = TimecodeEngine::applyBpmMultiplier(ds.bpm, effMult);
                    juce::String multLabel;
                    switch (effMult) {
                        case  1: multLabel = "x2"; break;
                        case  2: multLabel = "x4"; break;
                        case -1: multLabel = "/2"; break;
                        case -2: multLabel = "/4"; break;
                        default: break;
                    }
                    bpmStr += "  -> " + juce::String(multBpm, 1) + " (" + multLabel + ")";
                }

                g.drawText(bpmStr, bpmRow.removeFromLeft(bpmRow.getWidth() - 50),
                           juce::Justification::centredLeft);
            }
            else
            {
                g.setColour(textDim);
                g.drawText("--- BPM", bpmRow.removeFromLeft(bpmRow.getWidth() - 50),
                           juce::Justification::centredLeft);
            }

            // Pitch
            if (ds.speed > 0.01)
            {
                double pitchPct = (ds.speed - 1.0) * 100.0;
                g.setFont(juce::Font(juce::FontOptions(10.0f)));
                g.setColour(std::abs(pitchPct) > 0.05 ? accentCyan : textMid);
                juce::String pitchStr = (pitchPct >= 0.0 ? "+" : "") + juce::String(pitchPct, 2) + "%";
                g.drawText(pitchStr, bpmRow.removeFromRight(60), juce::Justification::centredRight);
            }

            // Beat indicator (right side, 4 dots)
            paintBeatIndicator(g, bpmRow.removeFromRight(40).toFloat(), ds.beatInBar);

            // Status badges (small, between BPM and pitch, right-to-left)
            g.setFont(juce::Font(juce::FontOptions(7.0f, juce::Font::bold)));
            if (ds.loopEnabled)
            {
                auto badge = bpmRow.removeFromRight(28).toFloat().reduced(1, 3);
                g.setColour(accentGreen.withAlpha(0.3f));
                g.fillRoundedRectangle(badge, 2.0f);
                g.setColour(accentGreen);
                juce::String loopTxt = "LOOP";
                if (ds.loopSizeBeats > 0.0)
                    loopTxt = juce::String(ds.loopSizeBeats, ds.loopSizeBeats < 1.0 ? 2 : 0);
                g.drawText(loopTxt, badge, juce::Justification::centred);
            }
            if (ds.bleepMode)
            {
                auto badge = bpmRow.removeFromRight(22).toFloat().reduced(1, 3);
                g.setColour(accentAmber.withAlpha(0.3f));
                g.fillRoundedRectangle(badge, 2.0f);
                g.setColour(accentAmber);
                g.drawText("REV", badge, juce::Justification::centred);
            }
            if (ds.jogTouch)
            {
                auto badge = bpmRow.removeFromRight(22).toFloat().reduced(1, 3);
                g.setColour(accentCyan.withAlpha(0.3f));
                g.fillRoundedRectangle(badge, 2.0f);
                g.setColour(accentCyan);
                g.drawText("JOG", badge, juce::Justification::centred);
            }
        }

        inner.removeFromTop(2);

        //----------------------------------------------------------------------
        // Timecode display
        //----------------------------------------------------------------------
        auto tcRow = inner.removeFromTop(22);
        {
            g.setFont(juce::Font(juce::FontOptions(getMonoFontName(), 16.0f, juce::Font::bold)));
            juce::String tcStr = timecodeToString(ds.timecode);
            g.setColour(ds.isPlaying ? accentGreen : textMid);
            g.drawText(tcStr, tcRow.removeFromLeft(140), juce::Justification::centredLeft);

            // Offset timecode (if mapped)
            if (ds.trackMapped)
            {
                g.setFont(juce::Font(juce::FontOptions(getMonoFontName(), 14.0f, juce::Font::plain)));
                g.setColour(accentAmber);
                juce::String offTcStr = "MAP " + timecodeToString(ds.offsetTimecode);
                g.drawText(offTcStr, tcRow, juce::Justification::centredLeft);
            }
        }

        inner.removeFromTop(2);

        //----------------------------------------------------------------------
        // Waveform / Position bar
        //----------------------------------------------------------------------
        if (ds.trackLenSec > 0 && inner.getHeight() >= 12)
        {
            int wfHeight = ds.waveformLoaded ? juce::jmin(40, inner.getHeight() / 3) : 8;
            auto wfArea = inner.removeFromTop(wfHeight);
            float ratio = juce::jlimit(0.0f, 1.0f, ds.posRatio);

            // Background
            g.setColour(bgMain);
            g.fillRoundedRectangle(wfArea.toFloat(), 2.0f);

            if (ds.waveformLoaded && ds.waveform.entryCount > 0)
            {
                // Paint 3-band waveform (same rendering as WaveformDisplay::renderThreeBandBars)
                paintWaveform(g, wfArea.toFloat(), ds.waveform, ratio);

                // Paint cue markers and loop regions on top of waveform
                if (ds.perfData.valid && ds.perfData.totalSamples > 0.0)
                    paintCueAndLoopOverlay(g, wfArea.toFloat(), ds.perfData);
            }
            else
            {
                // Simple progress bar fallback
                if (ratio > 0.0f)
                {
                    auto filled = wfArea.toFloat().withWidth(wfArea.getWidth() * ratio);
                    g.setColour(accentGreen.withAlpha(0.6f));
                    g.fillRoundedRectangle(filled, 2.0f);
                }
            }

            // Paint live active loop from StateMap (green, on top of everything)
            // Uses perfData.totalSamples (from beatData BLOB) for consistency
            // with DB cue/loop markers; falls back to sampleRate * trackLen.
            if (ds.loopEnabled && ds.loopInSamples >= 0.0 && ds.loopOutSamples > ds.loopInSamples)
            {
                double totalSamples = (ds.perfData.valid && ds.perfData.totalSamples > 0.0)
                    ? ds.perfData.totalSamples
                    : ds.sampleRate * ds.trackLenSec;
                if (totalSamples > 0.0)
                {
                    float startX = wfArea.toFloat().getX() + (float)(ds.loopInSamples / totalSamples) * wfArea.getWidth();
                    float endX   = wfArea.toFloat().getX() + (float)(ds.loopOutSamples / totalSamples) * wfArea.getWidth();
                    g.setColour(accentGreen.withAlpha(0.25f));
                    g.fillRect(startX, (float)wfArea.getY(), endX - startX, (float)wfArea.getHeight());
                    g.setColour(accentGreen.withAlpha(0.8f));
                    g.fillRect(startX, (float)wfArea.getY(), 1.5f, (float)wfArea.getHeight());
                    g.fillRect(endX - 1.5f, (float)wfArea.getY(), 1.5f, (float)wfArea.getHeight());
                }
            }

            // Playhead cursor
            float cursorX = wfArea.getX() + wfArea.getWidth() * ratio;
            g.setColour(juce::Colours::white);
            g.fillRect(cursorX - 0.5f, (float)wfArea.getY(), 1.0f, (float)wfArea.getHeight());

            inner.removeFromTop(2);
        }

        //----------------------------------------------------------------------
        // Channel fader bar
        //----------------------------------------------------------------------
        if (inner.getHeight() >= 10)
        {
            auto faderRow = inner.removeFromTop(8);
            float fVal = juce::jlimit(0.0f, 1.0f, (float)ds.faderPos);
            g.setColour(bgMain);
            g.fillRoundedRectangle(faderRow.toFloat(), 2.0f);
            if (fVal > 0.0f)
            {
                auto filled = faderRow.toFloat().withWidth(faderRow.getWidth() * fVal);
                g.setColour(accentCyan.withAlpha(0.5f));
                g.fillRoundedRectangle(filled, 2.0f);
            }
            // Label
            g.setFont(juce::Font(juce::FontOptions(7.0f)));
            g.setColour(textDim);
            g.drawText("CH" + juce::String(dn), faderRow.reduced(2, 0), juce::Justification::centredLeft);

            inner.removeFromTop(2);
        }

        //----------------------------------------------------------------------
        // Engine assignment + TrackMap status
        //----------------------------------------------------------------------
        if (inner.getHeight() >= 12)
        {
            auto bottomRow = inner.removeFromTop(12);
            g.setFont(juce::Font(juce::FontOptions(8.0f)));

            // Engine names
            if (!ds.engineNames.isEmpty())
            {
                g.setColour(accentCyan.withAlpha(0.8f));
                g.drawText(ds.engineNames.joinIntoString(", "),
                           bottomRow.removeFromLeft(bottomRow.getWidth() / 2),
                           juce::Justification::centredLeft);
            }

            // TrackMap status
            if (ds.trackMapped)
            {
                g.setColour(accentAmber);
                g.drawText("MAP: " + ds.offset, bottomRow, juce::Justification::centredRight);
            }
            else if (ds.artist.isNotEmpty())
            {
                g.setColour(textDim.withAlpha(0.5f));
                g.drawText("NO MAP", bottomRow, juce::Justification::centredRight);
            }
        }
    }

    //==========================================================================
    // Waveform painting (3-band overview, same colors as Pioneer ThreeBand)
    //==========================================================================
    void paintWaveform(juce::Graphics& g, juce::Rectangle<float> area,
                       const DenonWaveformData& wf, float playRatio)
    {
        float w = area.getWidth();
        float h = area.getHeight();
        float midY = area.getY() + h * 0.5f;
        float halfH = h * 0.5f;
        int numEntries = wf.entryCount;
        if (numEntries <= 0 || wf.data.size() < (size_t)(numEntries * 3)) return;

        const uint8_t* data = wf.data.data();
        float entriesPerPx = (float)numEntries / w;

        // Find global peak for normalization
        uint8_t globalPeak = 1;
        for (int i = 0; i < numEntries; ++i)
        {
            int off = i * 3;
            globalPeak = std::max({ globalPeak, data[off], data[off + 1], data[off + 2] });
        }
        float hScale = halfH / (float)globalPeak;

        // Colors: low=red, mid=green, high=blue (standard 3-band)
        const juce::Colour colLow  { 0xFFCC3333 };
        const juce::Colour colMid  { 0xFF33CC33 };
        const juce::Colour colHigh { 0xFF3366CC };

        for (int px = 0; px < (int)w; ++px)
        {
            int eStart = juce::jlimit(0, numEntries - 1, (int)(px * entriesPerPx));
            int eEnd   = juce::jlimit(0, numEntries - 1, (int)((px + 1) * entriesPerPx));
            if (eEnd < eStart) eEnd = eStart;

            float sumMid = 0, sumHigh = 0, sumLow = 0;
            int count = 0;
            for (int e = eStart; e <= eEnd; ++e)
            {
                int off = e * 3;
                sumMid  += data[off];      // mid (reordered at decode)
                sumHigh += data[off + 1];  // high
                sumLow  += data[off + 2];  // low
                count++;
            }
            if (count == 0) continue;
            float avgMid  = sumMid / count;
            float avgHigh = sumHigh / count;
            float avgLow  = sumLow / count;

            float x = area.getX() + px;

            // Draw stacked bars mirrored around midline
            // Low (bottom), Mid (middle), High (top)
            float lowH  = avgLow * hScale;
            float midH  = avgMid * hScale;
            float highH = avgHigh * hScale;

            // Dim bars behind playhead, bright ahead
            float alpha = (px < (int)(w * playRatio)) ? 0.4f : 0.85f;

            // Upper half: mid then high stacked
            g.setColour(colMid.withAlpha(alpha));
            g.fillRect(x, midY - midH, 1.0f, midH);
            g.setColour(colHigh.withAlpha(alpha));
            g.fillRect(x, midY - midH - highH, 1.0f, highH);

            // Lower half: mid then low stacked (mirrored)
            g.setColour(colMid.withAlpha(alpha));
            g.fillRect(x, midY, 1.0f, midH);
            g.setColour(colLow.withAlpha(alpha));
            g.fillRect(x, midY + midH, 1.0f, lowH);
        }
    }

    //==========================================================================
    // Cue markers + loop regions overlay on waveform
    //==========================================================================
    void paintCueAndLoopOverlay(juce::Graphics& g, juce::Rectangle<float> area,
                                const DenonPerformanceData& perf)
    {
        float w = area.getWidth();
        double totalSamples = perf.totalSamples;
        if (totalSamples <= 0.0) return;

        // Loop regions (drawn first, behind cue markers)
        for (auto& loop : perf.loops)
        {
            if (!loop.startSet || !loop.endSet) continue;
            float startX = area.getX() + (float)(loop.startSampleOffset / totalSamples) * w;
            float endX   = area.getX() + (float)(loop.endSampleOffset / totalSamples) * w;
            if (endX <= startX) continue;

            juce::Colour loopCol(loop.r, loop.g, loop.b);
            if (loopCol.isTransparent() || (loop.r == 0 && loop.g == 0 && loop.b == 0))
                loopCol = accentAmber;
            g.setColour(loopCol.withAlpha(0.15f));
            g.fillRect(startX, area.getY(), endX - startX, area.getHeight());

            // Loop boundaries
            g.setColour(loopCol.withAlpha(0.6f));
            g.fillRect(startX, area.getY(), 1.0f, area.getHeight());
            g.fillRect(endX - 1.0f, area.getY(), 1.0f, area.getHeight());
        }

        // Main cue marker
        if (perf.mainCueSampleOffset > 0.0)
        {
            float cueX = area.getX() + (float)(perf.mainCueSampleOffset / totalSamples) * w;
            g.setColour(juce::Colour(0xFFFF3333));
            float triH = juce::jmin(6.0f, area.getHeight() * 0.3f);
            juce::Path tri;
            tri.addTriangle(cueX, area.getY(), cueX + triH, area.getY(), cueX, area.getY() + triH);
            g.fillPath(tri);
        }

        // Quick cue markers (small colored triangles at top)
        for (int i = 0; i < (int)perf.quickCues.size(); ++i)
        {
            auto& cue = perf.quickCues[(size_t)i];
            if (!cue.isSet()) continue;

            float cueX = area.getX() + (float)(cue.sampleOffset / totalSamples) * w;
            juce::Colour cueCol(cue.r, cue.g, cue.b);
            if (cueCol.isTransparent() || (cue.r == 0 && cue.g == 0 && cue.b == 0))
                cueCol = accentCyan;
            g.setColour(cueCol);

            float triH = juce::jmin(5.0f, area.getHeight() * 0.25f);
            juce::Path tri;
            tri.addTriangle(cueX, area.getY(), cueX + triH, area.getY(), cueX, area.getY() + triH);
            g.fillPath(tri);
        }
    }

    //==========================================================================
    // Beat indicator: 4 dots, current beat highlighted
    //==========================================================================
    void paintBeatIndicator(juce::Graphics& g, juce::Rectangle<float> area, uint8_t beat)
    {
        float dotSize = juce::jmin(6.0f, area.getHeight() * 0.7f);
        float spacing = dotSize + 3.0f;
        float startX = area.getCentreX() - (spacing * 4.0f - 3.0f) * 0.5f;
        float cy = area.getCentreY();

        for (int i = 1; i <= 4; ++i)
        {
            float x = startX + (i - 1) * spacing;
            bool active = (i == beat);
            if (active)
            {
                g.setColour(i == 1 ? accentRed : accentGreen);
                g.fillEllipse(x, cy - dotSize * 0.5f, dotSize, dotSize);
            }
            else
            {
                g.setColour(textDim.withAlpha(0.3f));
                g.drawEllipse(x, cy - dotSize * 0.5f, dotSize, dotSize, 1.0f);
            }
        }
    }

    //==========================================================================
    // Crossfader visualization
    //==========================================================================
    void paintCrossfader(juce::Graphics& g, juce::Rectangle<int> area)
    {
        auto af = area.toFloat();

        // Track
        g.setColour(bgDeck);
        g.fillRoundedRectangle(af, 3.0f);
        g.setColour(borderCol);
        g.drawRoundedRectangle(af, 3.0f, 1.0f);

        // Center line
        float cx = af.getCentreX();
        g.setColour(textDim.withAlpha(0.3f));
        g.drawVerticalLine((int)cx, af.getY() + 2, af.getBottom() - 2);

        // Thumb
        float thumbW = juce::jmin(20.0f, af.getWidth() * 0.05f);
        float pos = juce::jlimit(0.0f, 1.0f, (float)crossfaderPos);
        float thumbX = af.getX() + (af.getWidth() - thumbW) * pos;
        auto thumbRect = juce::Rectangle<float>(thumbX, af.getY() + 1, thumbW, af.getHeight() - 2);
        g.setColour(accentGreen.withAlpha(0.8f));
        g.fillRoundedRectangle(thumbRect, 2.0f);

        // Label
        g.setFont(juce::Font(juce::FontOptions(8.0f)));
        g.setColour(textDim);
        g.drawText("A", af.withWidth(20).translated(4, 0), juce::Justification::centredLeft);
        g.drawText("B", af.withWidth(20).withX(af.getRight() - 24), juce::Justification::centredRight);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StageLinQViewComponent)

    //==========================================================================
    // Utility: Timecode to string (same as ProDJLinkViewComponent)
    //==========================================================================
    static juce::String timecodeToString(const Timecode& tc)
    {
        return juce::String::formatted("%02d:%02d:%02d:%02d", tc.hours, tc.minutes, tc.seconds, tc.frames);
    }
};

//==============================================================================
// StageLinQViewWindow -- DocumentWindow wrapper
//==============================================================================
class StageLinQViewWindow : public juce::DocumentWindow
{
public:
    StageLinQViewWindow(StageLinQInput& slq,
                        StageLinQDbClient& slqDb,
                        TrackMap& tmap,
                        std::vector<std::unique_ptr<TimecodeEngine>>& engines)
        : DocumentWindow("SLQ VIEW",
                          juce::Colour(0xFF0D0E12),
                          DocumentWindow::closeButton | DocumentWindow::maximiseButton)
    {
        setContentOwned(new StageLinQViewComponent(slq, slqDb, tmap, engines), true);
        setUsingNativeTitleBar(false);
        setTitleBarHeight(20);
        setColour(juce::DocumentWindow::textColourId, juce::Colour(0xFF00CC66));
        setResizable(true, true);
        if (auto* c = getConstrainer())
            c->setMinimumSize(400, 300);
        centreWithSize(getWidth(), getHeight());
        setVisible(true);
        toFront(true);
    }

    ~StageLinQViewWindow() override = default;

    void closeButtonPressed() override
    {
        if (onBoundsCapture) onBoundsCapture();
        if (auto* content = dynamic_cast<StageLinQViewComponent*>(getContentComponent()))
            content->stopTimer();
        setVisible(false);
    }

    std::function<void()> onBoundsCapture;

    void setOnLayoutChanged(std::function<void()> cb)
    {
        if (auto* content = dynamic_cast<StageLinQViewComponent*>(getContentComponent()))
            content->onLayoutChanged = std::move(cb);
    }

    // --- Bounds persistence ---
    juce::String getBoundsString() const
    {
        auto b = getBounds();
        return juce::String(b.getX()) + " " + juce::String(b.getY()) + " "
             + juce::String(b.getWidth()) + " " + juce::String(b.getHeight());
    }

    void restoreFromBoundsString(const juce::String& s)
    {
        auto parts = juce::StringArray::fromTokens(s, " ", "");
        if (parts.size() == 4)
        {
            auto b = juce::Rectangle<int>(parts[0].getIntValue(), parts[1].getIntValue(),
                                           parts[2].getIntValue(), parts[3].getIntValue());
            if (b.getWidth() >= 200 && b.getHeight() >= 150)
            {
                auto centre = b.getCentre();
                bool onScreen = false;
                for (auto& disp : juce::Desktop::getInstance().getDisplays().displays)
                    if (disp.totalArea.contains(centre)) { onScreen = true; break; }
                if (onScreen)
                    setBounds(b);
            }
        }
    }

    // --- Layout state ---
    bool getLayoutHorizontal() const
    {
        if (auto* c = dynamic_cast<const StageLinQViewComponent*>(getContentComponent()))
            return c->getLayoutHorizontal();
        return false;
    }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StageLinQViewWindow)
};
