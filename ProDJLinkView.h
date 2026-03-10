// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter
//
// ProDJLinkView -- External window showing global Pro DJ Link network state.
//
// Displays all 4 CDJ decks with:
//   - Album artwork (from DbServerClient cache)
//   - Color waveform with playhead cursor
//   - Track info (artist, title, key)
//   - BPM, pitch %, play state
//   - Raw timecode from playhead
//   - TrackMap offset (if mapped)
//   - Engine assignment (which STC engine is monitoring this player)
//   - On-air / master / beat indicators
//   - DJM mixer fader + crossfader visualization
//
// Architecture: Owns its own 30Hz Timer, reads data from shared objects
// (ProDJLinkInput, DbServerClient, TrackMap, engines) via const getters.
// Actively requests metadata from DbServerClient for all discovered
// players (independently of engine assignments) so waveform, artwork,
// and track info load regardless of which players have engines.

#pragma once
#include <JuceHeader.h>
#include "ProDJLinkInput.h"
#include "DbServerClient.h"
#include "MediaDisplay.h"
#include "TimecodeEngine.h"
#include "AppSettings.h"
#include "CustomLookAndFeel.h"
#include <vector>
#include <memory>

//==============================================================================
// ProDJLinkViewComponent -- Main content: 4 deck panels + mixer strip
//==============================================================================
class ProDJLinkViewComponent : public juce::Component,
                                public juce::Timer
{
public:
    ProDJLinkViewComponent(ProDJLinkInput& pdl,
                           DbServerClient& db,
                           TrackMap& tmap,
                           std::vector<std::unique_ptr<TimecodeEngine>>& engs)
        : proDJLink(pdl), dbClient(db), trackMap(tmap), engines(engs)
    {
        setSize(900, 680);
        startTimerHz(30);
    }

    ~ProDJLinkViewComponent() override
    {
        stopTimer();
    }

    //==========================================================================
    void paint(juce::Graphics& g) override
    {
        g.fillAll(bgMain);
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(6);

        // Mixer strip at bottom (scales slightly with window height, min 44, max 64)
        int mixerH = juce::jlimit(44, 64, bounds.getHeight() / 12);
        auto mixerArea = bounds.removeFromBottom(mixerH);
        bounds.removeFromBottom(4); // gap

        // 2x2 grid: two rows, two columns, 4px gaps
        int gapH = 4, gapV = 4;
        int deckW = (bounds.getWidth() - gapH) / 2;
        int deckH = (bounds.getHeight() - gapV) / 2;

        auto topRow = bounds.removeFromTop(deckH);
        bounds.removeFromTop(gapV);
        auto bottomRow = bounds;

        deckBounds[0] = topRow.removeFromLeft(deckW);
        topRow.removeFromLeft(gapH);
        deckBounds[1] = topRow;

        deckBounds[2] = bottomRow.removeFromLeft(deckW);
        bottomRow.removeFromLeft(gapH);
        deckBounds[3] = bottomRow;

        mixerBounds = mixerArea;
    }

    //==========================================================================
    void timerCallback() override
    {
        for (int deck = 0; deck < 4; ++deck)
        {
            int pn = deck + 1; // 1-based player number
            auto& ds = deckState[deck];

            ds.discovered    = proDJLink.isPlayerDiscovered(pn);
            ds.model         = proDJLink.getPlayerModel(pn);
            ds.ip            = proDJLink.getPlayerIP(pn);
            ds.playState     = proDJLink.getPlayStateString(pn);
            ds.bpm           = proDJLink.getBPM(pn);
            ds.faderPitch    = proDJLink.getFaderPitch(pn);
            ds.isOnAir       = proDJLink.isPlayerOnAir(pn);
            ds.isMaster      = proDJLink.isPlayerMaster(pn);
            ds.isPlaying     = proDJLink.isPlayerPlaying(pn);
            ds.beatInBar     = proDJLink.getBeatInBar(pn);
            ds.playheadMs    = proDJLink.getPlayheadMs(pn);
            ds.trackLenSec   = proDJLink.getTrackLengthSec(pn);
            ds.trackId       = proDJLink.getTrackID(pn);
            ds.posSource     = proDJLink.getPositionSourceString(pn);

            // Timecode from playhead
            FrameRate fps = proDJLink.getDetectedFrameRate(pn);
            ds.timecode = ProDJLink::playheadToTimecode(ds.playheadMs, fps);
            ds.fpsStr = frameRateToString(fps);

            // Position ratio for waveform cursor
            ds.posRatio = proDJLink.getPlayPositionRatio(pn);

            // TrackMap lookup
            ds.trackMapped = false;
            ds.offset = "00:00:00:00";
            ds.offsetTimecode = {};
            if (ds.trackId != 0)
            {
                const auto* entry = trackMap.find(ds.trackId);
                if (entry != nullptr)
                {
                    ds.trackMapped = true;
                    ds.offset = entry->timecodeOffset;

                    // Parse offset and compute running timecode with offset applied
                    int oH, oM, oS, oF;
                    if (TrackMapEntry::parseTimecodeString(entry->timecodeOffset, oH, oM, oS, oF))
                    {
                        FrameRate offsetFps = TimecodeEngine::indexToFps(entry->frameRate);
                        ds.offsetTimecode = applyTimecodeOffset(
                            ds.timecode, fps, oH, oM, oS, oF, offsetFps);
                    }
                }
            }

            // Engine assignment: find which engines monitor this player
            ds.engineNames.clear();
            for (auto& eng : engines)
            {
                if (eng->getActiveInput() == TimecodeEngine::InputSource::ProDJLink
                    && eng->getProDJLinkPlayer() == pn)
                {
                    ds.engineNames.add(eng->getName());
                }
            }

            // --- Track changed? ---
            bool trackChanged = (ds.trackId != ds.prevTrackId);
            if (trackChanged)
            {
                ds.waveform.clearWaveform();
                ds.displayedWaveformTrackId = 0;
                ds.displayedArtworkId = 0;
                ds.cachedArtworkImg = {};
                ds.artist.clear();
                ds.title.clear();
                ds.key.clear();
                ds.artworkId = 0;
                ds.prevTrackId = ds.trackId;
                ds.metadataRequested = false;  // reset so we request for new track
            }

            // --- Request metadata independently of engines ---
            // ProDJLinkView requests metadata for ALL discovered players,
            // so waveform/artwork/info loads even without an engine assigned.
            if (ds.trackId != 0 && !ds.metadataRequested
                && !ds.ip.isEmpty() && dbClient.getIsRunning())
            {
                uint8_t srcPlayer = proDJLink.getLoadedPlayer(pn);
                if (srcPlayer == 0) srcPlayer = (uint8_t)pn;
                uint8_t slot = proDJLink.getLoadedSlot(pn);
                if (slot != 0)
                {
                    int dbCtx = proDJLink.getVCDJPlayerNumber();
                    juce::String model = proDJLink.getPlayerModel((int)srcPlayer);
                    dbClient.requestMetadata(
                        ds.ip, slot, 1, ds.trackId, dbCtx, model);
                    ds.metadataRequested = true;
                    ds.metadataRequestTick = 0;
                }
            }

            // Retry if metadata hasn't arrived after ~3s (90 ticks at 30Hz)
            if (ds.metadataRequested && ds.artist.isEmpty()
                && ds.displayedWaveformTrackId != ds.trackId)
            {
                if (++ds.metadataRequestTick > 90)
                    ds.metadataRequested = false;  // allow re-request
            }

            // --- Metadata from DbServerClient ---
            if (ds.trackId != 0)
            {
                // Only re-fetch metadata if we still need artist/title or waveform
                // (artwork image is fetched separately below, doesn't need metadata re-fetch)
                bool needMeta = ds.artist.isEmpty()
                             || ds.displayedWaveformTrackId != ds.trackId;

                if (needMeta && !ds.ip.isEmpty())
                {
                    auto meta = dbClient.getCachedMetadata(ds.ip, ds.trackId);
                    if (meta.isValid())
                    {
                        ds.artist    = meta.artist;
                        ds.title     = meta.title;
                        ds.key       = meta.key;
                        ds.artworkId = meta.artworkId;

                        if (meta.hasWaveform() && ds.displayedWaveformTrackId != ds.trackId)
                        {
                            ds.waveform.setColorWaveformData(meta.waveformData,
                                meta.waveformEntryCount, meta.waveformBytesPerEntry);
                            ds.displayedWaveformTrackId = ds.trackId;
                        }
                    }
                    else
                    {
                        // Fallback: try by trackId only (cached from another player)
                        auto metaById = dbClient.getCachedMetadataByTrackId(ds.trackId);
                        if (metaById.isValid())
                        {
                            ds.artist    = metaById.artist;
                            ds.title     = metaById.title;
                            ds.key       = metaById.key;
                            ds.artworkId = metaById.artworkId;

                            if (metaById.hasWaveform() && ds.displayedWaveformTrackId != ds.trackId)
                            {
                                ds.waveform.setColorWaveformData(metaById.waveformData,
                                    metaById.waveformEntryCount, metaById.waveformBytesPerEntry);
                                ds.displayedWaveformTrackId = ds.trackId;
                            }
                        }
                    }
                }

                // Fallback: if still no title, show "Track #ID" from protocol
                if (ds.title.isEmpty())
                    ds.title = "Track #" + juce::String(ds.trackId);

                // Artwork update (only when artworkId changes)
                if (ds.artworkId != 0 && ds.artworkId != ds.displayedArtworkId)
                {
                    auto artImg = dbClient.getCachedArtwork(ds.artworkId);
                    if (artImg.isValid())
                    {
                        ds.cachedArtworkImg = artImg;
                        ds.displayedArtworkId = ds.artworkId;
                    }
                }
            }
            else
            {
                // No track loaded: clear everything
                if (ds.displayedWaveformTrackId != 0)
                {
                    ds.waveform.clearWaveform();
                    ds.displayedWaveformTrackId = 0;
                }
                ds.displayedArtworkId = 0;
                ds.cachedArtworkImg = {};
                ds.artist.clear();
                ds.title.clear();
                ds.key.clear();
                ds.artworkId = 0;
                ds.metadataRequested = false;
            }

            // Waveform cursor
            if (ds.waveform.hasWaveformData())
            {
                ds.waveform.setPlayPosition(ds.posRatio);
                ds.waveform.setDebugPositionMs(ds.playheadMs, ds.trackLenSec * 1000);
            }
        }

        // --- Smooth VU meters ---
        if (proDJLink.hasVuMeterData())
        {
            for (int ch = 0; ch < 6; ++ch)
            {
                float target = proDJLink.getVuPeakNorm(ch);
                // Fast attack, slow decay (ballistic meter feel)
                if (target > vuSmoothed[ch])
                    vuSmoothed[ch] = target;
                else
                    vuSmoothed[ch] *= 0.88f;  // ~120ms decay at 30Hz
                if (vuSmoothed[ch] < 0.001f) vuSmoothed[ch] = 0.0f;
            }
        }
        else
        {
            for (int ch = 0; ch < 6; ++ch)
                vuSmoothed[ch] *= 0.9f;
        }

        repaint();
    }

    //==========================================================================
    // Custom painting -- all decks + mixer painted in one pass
    //==========================================================================
    void paintOverChildren(juce::Graphics& g) override
    {
        // Paint each deck
        for (int deck = 0; deck < 4; ++deck)
            paintDeck(g, deckBounds[deck], deck);

        // Paint mixer
        paintMixer(g, mixerBounds);
    }

private:
    //==========================================================================
    // Colors
    //==========================================================================
    juce::Colour bgMain       { 0xFF0D0E12 };
    juce::Colour bgDeck       { 0xFF12141A };
    juce::Colour borderCol    { 0xFF1E2028 };
    juce::Colour textBright   { 0xFFCFD8DC };
    juce::Colour textMid      { 0xFF78909C };
    juce::Colour textDim      { 0xFF37474F };
    juce::Colour accentCyan   { 0xFF00BCD4 };
    juce::Colour accentGreen  { 0xFF00C853 };
    juce::Colour accentAmber  { 0xFFFFB74D };
    juce::Colour accentRed    { 0xFFFF5252 };
    juce::Colour tcGlow       { 0xFF00AAFF };

    //==========================================================================
    // Per-deck state cache (updated every timer tick)
    //==========================================================================
    struct DeckState
    {
        bool discovered = false;
        juce::String model, ip, playState, posSource;
        juce::String artist, title, key, offset;
        double bpm = 0.0, faderPitch = 1.0;
        bool isOnAir = false, isMaster = false, isPlaying = false, trackMapped = false;
        uint8_t beatInBar = 0;
        uint32_t playheadMs = 0, trackLenSec = 0, trackId = 0;
        uint32_t artworkId = 0;
        float posRatio = 0.0f;
        juce::String fpsStr;
        Timecode timecode {};
        Timecode offsetTimecode {};    // timecode with TrackMap offset applied (running clock)
        juce::StringArray engineNames;

        // Per-deck waveform component (painted via translated Graphics context)
        WaveformDisplay waveform;
        juce::Image     cachedArtworkImg;  // cached from DbServerClient (avoid lock during paint)
        uint32_t displayedWaveformTrackId = 0;
        uint32_t displayedArtworkId = 0;
        uint32_t prevTrackId = 0;
        bool     metadataRequested = false;  // true once we've sent a dbserver request for this track
        int      metadataRequestTick = 0;   // tick counter for retry after ~3s
    };

    DeckState deckState[4];
    juce::Rectangle<int> deckBounds[4];
    juce::Rectangle<int> mixerBounds;

    // Smoothed VU meter levels (decay-based, updated per timer tick)
    // Indices: 0=CH1, 1=CH2, 2=CH3, 3=CH4, 4=MasterL, 5=MasterR
    float vuSmoothed[6] { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };

    // Shared data references (not owned)
    ProDJLinkInput& proDJLink;
    DbServerClient& dbClient;
    TrackMap&        trackMap;
    std::vector<std::unique_ptr<TimecodeEngine>>& engines;

    //==========================================================================
    // Paint one deck panel
    //==========================================================================
    void paintDeck(juce::Graphics& g, juce::Rectangle<int> area, int deckIndex)
    {
        if (area.isEmpty()) return;

        auto& ds = deckState[deckIndex];
        int pn = deckIndex + 1;
        auto af = area.toFloat();

        // Background panel
        g.setColour(bgDeck);
        g.fillRoundedRectangle(af, 4.0f);
        g.setColour(borderCol);
        g.drawRoundedRectangle(af, 4.0f, 1.0f);

        auto inner = area.reduced(6);
        int deckH = inner.getHeight();

        // --- Proportional layout ---
        // Fixed chrome: header(22) + gaps(~16) + map(16) + engine(16) = ~70px
        // Flexible: infoRow, waveform, timecode scale with available height
        constexpr int kFixedChrome = 70;
        int flexH = juce::jmax(0, deckH - kFixedChrome);

        // Distribute flexible space: info 35%, waveform 45%, timecode 20%
        int infoH   = juce::jmax(50, (int)(flexH * 0.35f));
        int wfH     = juce::jmax(30, (int)(flexH * 0.45f));
        int tcH     = juce::jmax(20, (int)(flexH * 0.20f));

        //----------------------------------------------------------------------
        // Header bar: Player number + model + status badges
        //----------------------------------------------------------------------
        auto headerRow = inner.removeFromTop(22);

        // Player number badge
        {
            auto badge = headerRow.removeFromLeft(28).toFloat();
            juce::Colour badgeCol = ds.discovered
                ? (ds.isOnAir ? accentGreen : accentCyan)
                : textDim;
            g.setColour(badgeCol.withAlpha(0.2f));
            g.fillRoundedRectangle(badge, 3.0f);
            g.setColour(badgeCol);
            g.drawRoundedRectangle(badge, 3.0f, 1.0f);
            g.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));
            g.drawText(juce::String(pn), badge, juce::Justification::centred);
        }
        headerRow.removeFromLeft(4);

        // Model name + IP
        {
            g.setFont(juce::Font(juce::FontOptions(10.0f)));
            if (ds.discovered)
            {
                g.setColour(textBright);
                juce::String headerStr = ds.model;
                g.drawText(headerStr, headerRow, juce::Justification::centredLeft);

                // Right-aligned badges: MASTER, ON AIR
                auto rightBadges = headerRow;
                if (ds.isMaster)
                {
                    auto mb = rightBadges.removeFromRight(40).toFloat().reduced(0, 2);
                    g.setColour(accentAmber.withAlpha(0.25f));
                    g.fillRoundedRectangle(mb, 2.0f);
                    g.setColour(accentAmber);
                    g.setFont(juce::Font(juce::FontOptions(8.0f, juce::Font::bold)));
                    g.drawText("MST", mb, juce::Justification::centred);
                    rightBadges.removeFromRight(2);
                }
                if (ds.isOnAir)
                {
                    auto ob = rightBadges.removeFromRight(40).toFloat().reduced(0, 2);
                    g.setColour(accentGreen.withAlpha(0.25f));
                    g.fillRoundedRectangle(ob, 2.0f);
                    g.setColour(accentGreen);
                    g.setFont(juce::Font(juce::FontOptions(8.0f, juce::Font::bold)));
                    g.drawText("AIR", ob, juce::Justification::centred);
                }
            }
            else
            {
                g.setColour(textDim);
                g.drawText("OFFLINE", headerRow, juce::Justification::centredLeft);
            }
        }

        inner.removeFromTop(4);

        if (!ds.discovered)
        {
            // Empty deck: draw placeholder
            g.setColour(textDim.withAlpha(0.3f));
            float placeholderSize = juce::jmin(24.0f, deckH * 0.15f);
            g.setFont(juce::Font(juce::FontOptions(placeholderSize)));
            g.drawText(juce::String(pn), inner, juce::Justification::centred);
            return;
        }

        //----------------------------------------------------------------------
        // Artwork + Track info row (proportional height)
        //----------------------------------------------------------------------
        auto infoRow = inner.removeFromTop(infoH);
        {
            // Artwork: square, left side, sized to row height
            int artSize = juce::jmin(infoH, infoRow.getWidth() / 3);
            auto artBounds = infoRow.removeFromLeft(artSize);
            paintArtwork(g, artBounds.toFloat(), deckIndex);
            infoRow.removeFromLeft(6);

            // Track info stacked to the right
            auto infoArea = infoRow;

            // Scale font sizes with info row height
            float titleFs   = juce::jlimit(10.0f, 16.0f, infoH * 0.18f);
            float artistFs  = juce::jlimit(9.0f,  14.0f, infoH * 0.15f);
            float detailFs  = juce::jlimit(8.0f,  12.0f, infoH * 0.13f);
            int   lineH     = juce::jmax(12, infoH / 6);

            // Title (bold, brighter)
            auto titleRow = infoArea.removeFromTop(lineH + 2);
            g.setFont(juce::Font(juce::FontOptions(titleFs, juce::Font::bold)));
            g.setColour(ds.title.isNotEmpty() ? textBright : textDim);
            g.drawText(ds.title.isNotEmpty() ? ds.title : "No track",
                       titleRow, juce::Justification::centredLeft, true);

            // Artist
            auto artistRow = infoArea.removeFromTop(lineH);
            g.setFont(juce::Font(juce::FontOptions(artistFs)));
            g.setColour(textMid);
            g.drawText(ds.artist.isNotEmpty() ? ds.artist : "",
                       artistRow, juce::Justification::centredLeft, true);

            // BPM + Key
            auto bpmRow = infoArea.removeFromTop(lineH);
            juce::String bpmStr;
            if (ds.bpm > 0.0) bpmStr += juce::String(ds.bpm, 1) + " BPM";
            if (ds.key.isNotEmpty()) bpmStr += "  " + ds.key;
            g.setFont(juce::Font(juce::FontOptions(detailFs)));
            g.setColour(accentCyan.brighter(0.3f));
            g.drawText(bpmStr, bpmRow, juce::Justification::centredLeft, true);

            // Pitch (fader position — this is what the DJ sets)
            auto pitchRow = infoArea.removeFromTop(lineH);
            {
                double pitchPct = (ds.faderPitch - 1.0) * 100.0;
                bool pitchNonZero = std::abs(pitchPct) > 0.005;
                juce::String pitchStr = "PITCH ";
                if (pitchNonZero)
                {
                    pitchStr += (pitchPct >= 0.0 ? "+" : "")
                              + juce::String(pitchPct, 2) + "%";
                }
                else
                    pitchStr += "0.00%";

                g.setFont(juce::Font(juce::FontOptions(detailFs, juce::Font::bold)));
                g.setColour(pitchNonZero ? accentAmber : textMid);
                g.drawText(pitchStr, pitchRow, juce::Justification::centredLeft);
            }

            // Play state + position source
            auto stateRow = infoArea.removeFromTop(lineH);
            g.setFont(juce::Font(juce::FontOptions(detailFs, juce::Font::bold)));
            juce::Colour stateCol = ds.isPlaying ? accentGreen : accentAmber;
            g.setColour(stateCol);
            g.drawText(ds.playState + "  [" + ds.posSource + "]",
                       stateRow, juce::Justification::centredLeft);

            // Beat in bar indicator
            if (infoArea.getHeight() >= 8)
            {
                auto beatRow = infoArea.removeFromTop(juce::jmin(12, infoArea.getHeight()));
                paintBeatIndicator(g, beatRow.removeFromLeft(60).toFloat(), ds.beatInBar);
            }
        }

        inner.removeFromTop(4);

        //----------------------------------------------------------------------
        // Waveform (proportional height)
        //----------------------------------------------------------------------
        auto wfBounds = inner.removeFromTop(wfH);
        paintWaveformArea(g, wfBounds, deckIndex);

        inner.removeFromTop(4);

        //----------------------------------------------------------------------
        // Timecode display (scales with deck size)
        //----------------------------------------------------------------------
        auto tcRow = inner.removeFromTop(tcH);
        {
            // FPS badge (right-aligned, reserve space first)
            auto fpsBadge = tcRow.removeFromRight(34);
            g.setFont(juce::Font(juce::FontOptions(9.0f)));
            g.setColour(textDim);
            g.drawText(ds.fpsStr, fpsBadge, juce::Justification::centred);

            // Timecode font scales with row height
            float tcFontSize = juce::jlimit(14.0f, 48.0f, tcH * 0.8f);
            juce::String tcStr = timecodeToString(ds.timecode);
            g.setFont(juce::Font(juce::FontOptions(getMonoFontName(), tcFontSize, juce::Font::bold)));
            g.setColour(ds.isPlaying ? tcGlow : textMid);
            g.drawText(tcStr, tcRow, juce::Justification::centred);
        }

        inner.removeFromTop(2);

        //----------------------------------------------------------------------
        // TrackMap offset row -- running timecode with offset applied
        //----------------------------------------------------------------------
        auto mapRow = inner.removeFromTop(16);
        {
            if (ds.trackMapped)
            {
                g.setColour(accentAmber.withAlpha(0.2f));
                g.fillRoundedRectangle(mapRow.toFloat(), 2.0f);

                // Running offset timecode (monospace, prominent)
                juce::String offsetTcStr = timecodeToString(ds.offsetTimecode);
                g.setColour(accentAmber);
                g.setFont(juce::Font(juce::FontOptions(getMonoFontName(), 11.0f, juce::Font::bold)));
                g.drawText(offsetTcStr, mapRow, juce::Justification::centred);

                // Small "MAP" label on the left
                g.setFont(juce::Font(juce::FontOptions(7.0f, juce::Font::bold)));
                g.setColour(accentAmber.withAlpha(0.6f));
                g.drawText("MAP", mapRow.withTrimmedLeft(4).withWidth(24),
                           juce::Justification::centredLeft);
            }
            else
            {
                g.setColour(textDim.withAlpha(0.5f));
                g.setFont(juce::Font(juce::FontOptions(9.0f)));
                g.drawText("NO MAP", mapRow, juce::Justification::centred);
            }
        }

        inner.removeFromTop(2);

        //----------------------------------------------------------------------
        // Engine assignment row
        //----------------------------------------------------------------------
        auto engRow = inner.removeFromTop(16);
        {
            g.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
            if (ds.engineNames.size() > 0)
            {
                juce::String engStr = ds.engineNames.joinIntoString(", ");
                g.setColour(accentCyan.withAlpha(0.2f));
                g.fillRoundedRectangle(engRow.toFloat(), 2.0f);
                g.setColour(accentCyan);
                g.drawText(engStr, engRow, juce::Justification::centred);
            }
            else
            {
                g.setColour(textDim.withAlpha(0.5f));
                g.drawText("--", engRow, juce::Justification::centred);
            }
        }
    }

    //==========================================================================
    // Artwork -- painted inline (not as a child component, to avoid layout)
    //==========================================================================
    void paintArtwork(juce::Graphics& g, juce::Rectangle<float> bounds, int deckIndex)
    {
        auto& ds = deckState[deckIndex];

        g.setColour(juce::Colour(0xFF0D1117));
        g.fillRoundedRectangle(bounds, 3.0f);

        if (ds.cachedArtworkImg.isValid())
        {
            float inset = 2.0f;
            auto db = bounds.reduced(inset);
            float imgW = (float)ds.cachedArtworkImg.getWidth();
            float imgH = (float)ds.cachedArtworkImg.getHeight();
            float scale = juce::jmin(db.getWidth() / imgW, db.getHeight() / imgH);
            float dw = imgW * scale, dh = imgH * scale;
            float dx = db.getX() + (db.getWidth() - dw) * 0.5f;
            float dy = db.getY() + (db.getHeight() - dh) * 0.5f;
            g.drawImage(ds.cachedArtworkImg, (int)dx, (int)dy, (int)dw, (int)dh,
                        0, 0, ds.cachedArtworkImg.getWidth(), ds.cachedArtworkImg.getHeight());
            return;
        }

        g.setColour(juce::Colour(0xFF4A5568));
        g.setFont(juce::Font(juce::FontOptions(10.0f)));
        g.drawText("ART", bounds, juce::Justification::centred);
    }

    //==========================================================================
    // Waveform -- delegate to the WaveformDisplay's paint, positioned inline
    //==========================================================================
    void paintWaveformArea(juce::Graphics& g, juce::Rectangle<int> bounds, int deckIndex)
    {
        auto& ds = deckState[deckIndex];

        if (ds.waveform.hasWaveformData())
        {
            // WaveformDisplay::paint() draws its own background, no need to pre-fill.
            // Translate the Graphics context so WaveformDisplay::paint() works
            // in its own local coordinate space but draws into our bounds.
            juce::Graphics::ScopedSaveState saveState(g);
            g.reduceClipRegion(bounds);
            g.setOrigin(bounds.getPosition());
            ds.waveform.setBounds(0, 0, bounds.getWidth(), bounds.getHeight());
            ds.waveform.paint(g);
        }
        else
        {
            auto bf = bounds.toFloat();
            g.setColour(juce::Colour(0xFF0D1117));
            g.fillRoundedRectangle(bf, 3.0f);
            g.setColour(textDim.withAlpha(0.3f));
            g.setFont(juce::Font(juce::FontOptions(9.0f)));
            g.drawText("WAVEFORM", bf, juce::Justification::centred);
        }
    }

    //==========================================================================
    // Beat indicator: 4 boxes, active one highlighted
    //==========================================================================
    void paintBeatIndicator(juce::Graphics& g, juce::Rectangle<float> area, uint8_t beat)
    {
        float boxW = 10.0f;
        float gap = 3.0f;
        float totalW = 4 * boxW + 3 * gap;
        float x = area.getX() + (area.getWidth() - totalW) * 0.5f;
        float y = area.getY() + 1.0f;
        float h = area.getHeight() - 2.0f;

        for (int i = 1; i <= 4; ++i)
        {
            auto box = juce::Rectangle<float>(x, y, boxW, h);
            bool active = (beat == i);
            if (active)
            {
                g.setColour(i == 1 ? accentRed : accentCyan);
                g.fillRoundedRectangle(box, 1.5f);
            }
            else
            {
                g.setColour(textDim.withAlpha(0.3f));
                g.fillRoundedRectangle(box, 1.5f);
            }
            x += boxW + gap;
        }
    }

    //==========================================================================
    // Mixer strip (DJM faders + crossfader)
    //==========================================================================
    void paintMixer(juce::Graphics& g, juce::Rectangle<int> area)
    {
        auto af = area.toFloat();
        g.setColour(bgDeck);
        g.fillRoundedRectangle(af, 4.0f);
        g.setColour(borderCol);
        g.drawRoundedRectangle(af, 4.0f, 1.0f);

        auto inner = area.reduced(8, 4);

        // DJM model label
        juce::String djmModel = proDJLink.getDJMModel();
        if (djmModel.isEmpty()) djmModel = "DJM";
        auto labelArea = inner.removeFromLeft(80);
        g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
        g.setColour(textMid);
        g.drawText(djmModel, labelArea, juce::Justification::centredLeft);

        if (!proDJLink.hasMixerFaderData())
        {
            g.setColour(textDim);
            g.setFont(juce::Font(juce::FontOptions(9.0f)));
            g.drawText("Waiting for bridge data...", inner, juce::Justification::centredLeft);
            return;
        }

        // Channel faders 1-4 with VU meters
        int faderW = 60;
        for (int ch = 1; ch <= 4; ++ch)
        {
            auto faderArea = inner.removeFromLeft(faderW);
            uint8_t val = proDJLink.getChannelFader(ch);
            float vu = vuSmoothed[ch - 1];  // VU index 0-3 = CH1-CH4
            paintFader(g, faderArea.toFloat(), val, "CH" + juce::String(ch),
                       proDJLink.isPlayerOnAir(ch) ? accentGreen : accentCyan, vu);
            inner.removeFromLeft(4);
        }

        // Crossfader
        inner.removeFromLeft(12);
        {
            auto xfArea = inner.removeFromLeft(80);
            uint8_t val = proDJLink.getCrossfader();
            paintCrossfader(g, xfArea.toFloat(), val);
            inner.removeFromLeft(12);
        }

        // Master fader with stereo VU (L+R)
        {
            auto mfArea = inner.removeFromLeft(faderW + 8);  // slightly wider for stereo VU
            uint8_t val = proDJLink.getMasterFader();
            float vuL = vuSmoothed[4];  // MasterL
            float vuR = vuSmoothed[5];  // MasterR
            paintFader(g, mfArea.toFloat(), val, "MST", accentAmber, vuL, vuR);
        }
    }

    //==========================================================================
    // Vertical fader with value bar + optional VU meter
    //==========================================================================
    void paintFader(juce::Graphics& g, juce::Rectangle<float> area,
                    uint8_t val, const juce::String& label, juce::Colour color,
                    float vuLevel = -1.0f, float vuLevel2 = -1.0f)
    {
        // Label on top
        auto labelArea = area.removeFromTop(12.0f);
        g.setFont(juce::Font(juce::FontOptions(8.0f, juce::Font::bold)));
        g.setColour(textMid);
        g.drawText(label, labelArea, juce::Justification::centred);

        // Value text on bottom (reserve space first)
        auto valueArea = area.removeFromBottom(9.0f);

        // VU meter(s) on the right side of the fader area
        bool hasVu  = (vuLevel  >= 0.0f);
        bool hasVu2 = (vuLevel2 >= 0.0f);  // stereo: second VU bar (Master R)

        float vuBarW = 3.0f;
        float vuGap  = 2.0f;

        juce::Rectangle<float> vuArea1, vuArea2;
        if (hasVu2)
        {
            // Stereo: two VU bars on the right
            auto vuStrip = area.removeFromRight(vuBarW * 2.0f + vuGap + 1.0f);
            vuArea2 = vuStrip.removeFromRight(vuBarW);
            vuStrip.removeFromRight(vuGap);
            vuArea1 = vuStrip.removeFromRight(vuBarW);
        }
        else if (hasVu)
        {
            // Mono: one VU bar on the right
            area.removeFromRight(1.0f);
            vuArea1 = area.removeFromRight(vuBarW);
        }

        // Fader track fills the remaining space
        float trackW = 8.0f;
        float trackX = area.getCentreX() - trackW * 0.5f;
        float trackH = area.getHeight();
        float trackY = area.getY();
        auto trackRect = juce::Rectangle<float>(trackX, trackY, trackW, trackH);

        g.setColour(juce::Colour(0xFF1A1D23));
        g.fillRoundedRectangle(trackRect, 2.0f);

        // Fill from bottom
        float fillH = (val / 255.0f) * trackH;
        auto fillRect = juce::Rectangle<float>(trackX, trackY + trackH - fillH, trackW, fillH);
        g.setColour(color.withAlpha(0.7f));
        g.fillRoundedRectangle(fillRect, 2.0f);

        // Draw VU bars
        if (hasVu)  paintVuBar(g, vuArea1, vuLevel);
        if (hasVu2) paintVuBar(g, vuArea2, vuLevel2);

        // Value text
        int pct = (int)std::round((val / 255.0f) * 100.0f);
        g.setFont(juce::Font(juce::FontOptions(7.0f)));
        g.setColour(textDim);
        g.drawText(juce::String(pct) + "%", valueArea, juce::Justification::centred);
    }

    //==========================================================================
    // Single VU meter bar (vertical, bottom-up, green-yellow-red gradient)
    //==========================================================================
    void paintVuBar(juce::Graphics& g, juce::Rectangle<float> area, float level)
    {
        float h = area.getHeight();
        float w = area.getWidth();
        float x = area.getX();
        float y = area.getY();

        // Background
        g.setColour(juce::Colour(0xFF0A0C10));
        g.fillRoundedRectangle(area, 1.0f);

        if (level <= 0.001f) return;

        // Fill from bottom with segmented green→yellow→red
        constexpr float kVuGain = 3.0f;
		float boosted = juce::jlimit(0.0f, 1.0f, level * kVuGain);
		float scaled  = (boosted > 0.0f)
			? juce::jlimit(0.0f, 1.0f, std::log10(1.0f + boosted * 9.0f))
			: 0.0f;
		float fillH = scaled * h;
        float segH = h / 15.0f;  // 15 visual segments matching DJM VU
		for (int seg = 0; seg < 15; ++seg)
        {
            float segBottom = y + h - (seg + 1) * segH;
            if (segBottom < y + h - fillH) continue;  // not reached yet

            // Color gradient: 0-9 green, 10-12 yellow, 13-14 red
            juce::Colour segCol;
            if (seg < 10)      segCol = accentGreen.withAlpha(0.75f);
            else if (seg < 13) segCol = accentAmber.withAlpha(0.80f);
            else               segCol = accentRed.withAlpha(0.85f);

            g.setColour(segCol);
            g.fillRect(x, segBottom, w, segH - 1.0f);
        }
    }

    //==========================================================================
    // Horizontal crossfader
    //==========================================================================
    void paintCrossfader(juce::Graphics& g, juce::Rectangle<float> area, uint8_t val)
    {
        auto labelArea = area.removeFromTop(12.0f);
        g.setFont(juce::Font(juce::FontOptions(8.0f, juce::Font::bold)));
        g.setColour(textMid);
        g.drawText("XFADER", labelArea, juce::Justification::centred);

        // Track (horizontal)
        float trackH = 8.0f;
        float trackY = area.getCentreY() - trackH * 0.5f;
        auto trackRect = juce::Rectangle<float>(area.getX(), trackY, area.getWidth(), trackH);

        g.setColour(juce::Colour(0xFF1A1D23));
        g.fillRoundedRectangle(trackRect, 2.0f);

        // Thumb position: 0=side-A (left), 255=side-B (right) — already corrected
        float thumbW = 12.0f;
        float thumbH = 14.0f;
        float range = area.getWidth() - thumbW;
        float normPos = val / 255.0f;  // 0(A)=left, 255(B)=right — direct mapping
        float thumbX = area.getX() + normPos * range;
        float thumbY = area.getCentreY() - thumbH * 0.5f;

        g.setColour(accentCyan.withAlpha(0.7f));
        g.fillRoundedRectangle(thumbX, thumbY, thumbW, thumbH, 2.0f);

        // A / B labels
        g.setFont(juce::Font(juce::FontOptions(8.0f)));
        g.setColour(textDim);
        g.drawText("A", juce::Rectangle<float>(area.getX(), area.getBottom() - 10, 12, 10),
                   juce::Justification::centred);
        g.drawText("B", juce::Rectangle<float>(area.getRight() - 12, area.getBottom() - 10, 12, 10),
                   juce::Justification::centred);
    }

    //==========================================================================
    // Utility: Timecode to string
    //==========================================================================
    static juce::String timecodeToString(const Timecode& tc)
    {
        return juce::String::formatted("%02d:%02d:%02d:%02d", tc.hours, tc.minutes, tc.seconds, tc.frames);
    }

    //==========================================================================
    // Utility: FrameRate to string
    //==========================================================================
    static juce::String frameRateToString(FrameRate fps)
    {
        switch (fps)
        {
            case FrameRate::FPS_2398: return "23.98";
            case FrameRate::FPS_24:   return "24";
            case FrameRate::FPS_25:   return "25";
            case FrameRate::FPS_2997: return "29.97";
            case FrameRate::FPS_30:   return "30";
            default:                  return "?";
        }
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ProDJLinkViewComponent)
};


//==============================================================================
// ProDJLinkViewWindow -- DocumentWindow wrapper
//==============================================================================
class ProDJLinkViewWindow : public juce::DocumentWindow
{
public:
    ProDJLinkViewWindow(ProDJLinkInput& pdl,
                        DbServerClient& db,
                        TrackMap& tmap,
                        std::vector<std::unique_ptr<TimecodeEngine>>& engines)
        : DocumentWindow("PRO DJ LINK VIEW",
                          juce::Colour(0xFF0D0E12),
                          DocumentWindow::allButtons)
    {
        setContentOwned(new ProDJLinkViewComponent(pdl, db, tmap, engines), true);
        setUsingNativeTitleBar(true);
        setResizable(true, true);
        centreWithSize(getWidth(), getHeight());
        setVisible(true);
        toFront(true);
    }

    void closeButtonPressed() override
    {
        // Stop the timer before hiding (avoids 30Hz CPU burn while invisible)
        if (auto* content = dynamic_cast<ProDJLinkViewComponent*>(getContentComponent()))
            content->stopTimer();
        setVisible(false);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ProDJLinkViewWindow)
};
