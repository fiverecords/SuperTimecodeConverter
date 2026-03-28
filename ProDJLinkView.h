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
// Architecture: Owns its own 60Hz Timer, reads data from shared objects
// (ProDJLinkInput, DbServerClient, TrackMap, engines) via const getters.
// Actively requests metadata from DbServerClient for all discovered
// players (independently of engine assignments) so waveform, artwork,
// and track info load regardless of which players have engines.

#pragma once
#include <JuceHeader.h>
#include "ProDJLinkInput.h"
#include "DbServerClient.h"
#include "MediaDisplay.h"
#include "WaveformDetailDisplay.h"
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

        // --- Toolbar buttons ---
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

        addAndMakeVisible(btnShowMixer);
        btnShowMixer.setButtonText("DJM");
        btnShowMixer.setClickingTogglesState(true);
        btnShowMixer.setToggleState(true, juce::dontSendNotification);
        btnShowMixer.setColour(juce::ToggleButton::textColourId, textMid);
        btnShowMixer.setColour(juce::ToggleButton::tickColourId, accentCyan);
        btnShowMixer.onClick = [this]
        {
            showMixer = btnShowMixer.getToggleState();
            resized();
            repaint();
            if (onLayoutChanged) onLayoutChanged();
        };

        startTimerHz(60);
    }

    ~ProDJLinkViewComponent() override
    {
        stopTimer();
    }

    // Called when BPM multiplier is saved to TrackMap via double-click.
    // Wire this to save settings and refresh engine lookups.
    std::function<void()> onTrackMapChanged;

    /// Callback to check if Show Lock is active. If set and returns true,
    /// TrackMap writes (BPM mult double-click) are blocked.
    std::function<bool()> isShowLockedFn;

    // Called when layout or mixer visibility changes (for settings persistence)
    std::function<void()> onLayoutChanged;

    void setLayoutHorizontal(bool h)
    {
        layoutHorizontal = h;
        btnLayout.setButtonText(layoutHorizontal ? "2x2" : "4x1");
        resized(); repaint();
    }
    void setShowMixer(bool m)
    {
        showMixer = m;
        btnShowMixer.setToggleState(m, juce::dontSendNotification);
        resized(); repaint();
    }
    bool getLayoutHorizontal() const { return layoutHorizontal; }
    bool getShowMixer()        const { return showMixer; }

private:
    //==========================================================================
    // Per-deck state cache (updated every timer tick)
    //==========================================================================
    struct DeckState
    {
        bool discovered = false;
        juce::String model, ip, playState, posSource;
        juce::String artist, title, key, offset;
        double bpm = 0.0, faderPitch = 1.0;
        bool isOnAir = false, isMaster = false, isPlaying = false, isReverse = false, trackMapped = false;
        uint8_t beatInBar = 0;
        uint8_t xfAssign  = 0;   // 0=THRU, 1=A, 2=B (from DJM mixer data)
        uint32_t playheadMs = 0, trackLenSec = 0, trackId = 0;
        uint32_t artworkId = 0;
        int cueCount = 0;  // rekordbox cue/hot cue/loop count
        float posRatio = 0.0f;
        juce::String fpsStr;
        Timecode timecode {};
        Timecode offsetTimecode {};    // timecode with TrackMap offset applied (running clock)
        juce::StringArray engineNames;

        // Per-deck waveform component (painted LIVE by paintDeck).
        // NOT a child component -- paint() is called manually with translated Graphics.
        WaveformDisplay waveform;
        WaveformDetailDisplay detailWaveform;
        juce::Rectangle<int> wfLocalBounds;  // waveform area relative to deck (set during paintDeckStatic)
        juce::Rectangle<int> detailLocalBounds; // detail waveform area relative to deck
        uint32_t displayedDetailTrackId = 0;
        int detailRetryTicks = 0;  // countdown for supplementary data retry
        // Fed-tracking: avoid redundant full metadata copies during retry
        bool previewCuesFed = false, previewBeatGridFed = false;
        int  previewRetryTicks = 0;  // countdown for preview supplementary data
        bool detailCuesFed = false, detailBeatGridFed = false, detailSongStructFed = false;
        uint32_t lastMetaVersion = 0;  // tracks DbServerClient cache version to skip redundant copies
        uint32_t lastDetailVersion = 0;  // separate version tracker for detail waveform section
        juce::Rectangle<int> tcLocalBounds;  // timecode row relative to deck (set during paintDeckStatic)
        juce::Rectangle<int> mapLocalBounds; // offset timecode row relative to deck
        juce::Rectangle<int> timeLocalBounds; // track time row relative to deck (for click-to-toggle)
        bool showRemainingTime = false;      // false=elapsed, true=remaining (per deck)
        juce::Image     cachedArtworkImg;  // cached from DbServerClient (avoid lock during paint)
        uint32_t displayedWaveformTrackId = 0;
        uint32_t displayedArtworkId = 0;
        uint32_t prevTrackId = 0;
        bool     metadataRequested = false;  // true once we've sent a dbserver request for this track
        int      metadataRequestTick = 0;   // tick counter for retry after ~3s

        // Cached deck image -- Windows only (GDI benefits from caching).
        // On macOS, painting directly preserves CoreGraphics subpixel AA.
#if JUCE_WINDOWS
        juce::Image cachedDeckImg;
        int         cachedDeckW = 0, cachedDeckH = 0;
        float       cachedDeckScale = 0.0f;
        bool        deckImgValid = false;
        void invalidateDeckImg() { deckImgValid = false; }
#else
        void invalidateDeckImg() { }  // no-op on macOS (no cache)
#endif

        // BPM multiplier state (updated each tick from engine + TrackMap)
        int  bpmSessionOverride = TimecodeEngine::kBpmNoOverride;  // from engine: kBpmNoOverride=none, 0=1x, 1=x2...
        int  bpmTrackMapValue   = 0;  // from TrackMap entry: same encoding
        // Cached button hit-test bounds (set during paint, used in mouseDown)
        juce::Rectangle<int> bpmBtnBounds[5];  // order: /4 /2 1x x2 x4
        // Per-deck double-click timing for BPM multiplier buttons
        juce::int64 lastBpmClickMs   = 0;
        int         lastBpmClickMult = -999;

        // Snapshot of last-painted state -- used to skip repaint() when nothing changed
        struct Snapshot
        {
            bool discovered = false, isOnAir = false, isMaster = false, isPlaying = false, isReverse = false, trackMapped = false;
            uint8_t beatInBar = 0, xfAssign = 0;
            double bpm = -1.0, faderPitch = -1.0;
            uint32_t playheadMs = 0xFFFFFFFF, trackId = 0;
            int bpmSessionOverride = -999, bpmTrackMapValue = -999;
            juce::String playState, artist, title, posSource, fpsStr;
            Timecode timecode {}, offsetTimecode {};
            juce::StringArray engineNames;
        } snapshot;
    };

public:
    // BPM multiplier button click handler with timing-based double-click detection.
    // Single click: toggle session override (temporary, cleared on track change).
    // Double click: persist to TrackMap (creates entry if track not mapped).
    void mouseDown(const juce::MouseEvent& e) override
    {
        auto pos = e.getPosition();
        for (int deck = 0; deck < 4; ++deck)
        {
            if (!deckBounds[deck].contains(pos)) continue;
            auto& ds = deckState[deck];
            int pn = deck + 1;

            // Convert to deck-local coords (bpmBtnBounds are set in local space
            // because paintDeckStatic receives {0,0,w,h} local coordinates)
            auto localPos = pos - deckBounds[deck].getPosition();

            static const int kBpmValues[5] = { -2, -1, 0, 1, 2 };
            for (int bi = 0; bi < 5; ++bi)
            {
                if (!ds.bpmBtnBounds[bi].contains(localPos)) continue;
                int clickedMult = kBpmValues[bi];

                // Timing-based double-click detection (same approach as MainComponent)
                auto now = (juce::int64)juce::Time::getMillisecondCounter();
                bool isDouble = (clickedMult == ds.lastBpmClickMult
                                 && (now - ds.lastBpmClickMs) < 400);
                ds.lastBpmClickMs   = now;
                ds.lastBpmClickMult = clickedMult;

                if (isDouble)
                {
                    // Double click: persist to TrackMap (toggle)
                    // Blocked during Show Lock -- TrackMap is configuration.
                    if (!isShowLockedFn || !isShowLockedFn())
                        saveBpmToTrackMap(pn, ds, clickedMult);
                }
                else
                {
                    // Single click: set session override (skip if already effective)
                    for (auto& eng : engines)
                    {
                        if (eng->getActiveInput() == TimecodeEngine::InputSource::ProDJLink
                            && eng->getEffectivePlayer() == pn)
                        {
                            if (eng->getEffectiveBpmMultiplier() == clickedMult)
                                break;  // already active -- do nothing
                            eng->setBpmPlayerOverride(clickedMult);
                            break;
                        }
                    }
                }
                return;
            }

            // Detail waveform zoom buttons: click + / -
            if (ds.detailWaveform.hasDetailData()
                && !ds.detailLocalBounds.isEmpty() && ds.detailLocalBounds.contains(localPos))
            {
                auto detLocal = localPos - ds.detailLocalBounds.getPosition();
                int zoom = ds.detailWaveform.hitTestZoomButtons(detLocal);
                if (zoom != 0)
                {
                    int cur = ds.detailWaveform.getScale();
                    ds.detailWaveform.setScale(cur - zoom);  // +1 = zoom in (less entries/px)
                    return;
                }
            }

            // Track time area: click to toggle elapsed / remaining
            if (!ds.timeLocalBounds.isEmpty() && ds.timeLocalBounds.contains(localPos))
            {
                ds.showRemainingTime = !ds.showRemainingTime;
                ds.invalidateDeckImg();
                repaint(deckBounds[deck]);
                return;
            }
            break;
        }
    }

    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        auto pos = e.getPosition();
        for (int deck = 0; deck < 4; ++deck)
        {
            if (!deckBounds[deck].contains(pos)) continue;
            auto& ds = deckState[deck];
            auto localPos = pos - deckBounds[deck].getPosition();

            if (!ds.detailLocalBounds.isEmpty() && ds.detailLocalBounds.contains(localPos))
            {
                int cur = ds.detailWaveform.getScale();
                if (wheel.deltaY > 0.0f)
                    ds.detailWaveform.setScale(cur - 1);  // zoom in
                else if (wheel.deltaY < 0.0f)
                    ds.detailWaveform.setScale(cur + 1);  // zoom out
                return;
            }
            break;
        }
    }

    //--------------------------------------------------------------------------
    // Persist BPM multiplier to TrackMap.
    // Toggle logic: if TrackMap already has this value, clear it; else set it.
    // After saving, session override is cleared (TrackMap = source of truth).
    // Creates a new TrackMap entry if the track isn't mapped yet.
    //--------------------------------------------------------------------------
    void saveBpmToTrackMap(int playerNum, DeckState& ds, int clickedMult)
    {
        if (ds.artist.isEmpty() || ds.title.isEmpty()) return;

        int dur = (int)ds.trackLenSec;
        auto* entry = trackMap.find(ds.artist, ds.title, dur);
        int currentMapValue = (entry != nullptr) ? entry->bpmMultiplier : 0;

        // Double-click on 1x: clear saved value. Otherwise: save (no toggle).
        int newValue;
        if (clickedMult == 0)
            newValue = 0;                           // 1x = clear
        else if (clickedMult == currentMapValue)
            return;                                 // already saved, do nothing
        else
            newValue = clickedMult;                  // save new value

        if (entry != nullptr)
        {
            entry->bpmMultiplier = newValue;
        }
        else if (newValue != 0)
        {
            TrackMapEntry newEntry;
            newEntry.artist  = ds.artist;
            newEntry.title   = ds.title;
            newEntry.durationSec = dur;
            newEntry.bpmMultiplier = newValue;
            trackMap.addOrUpdate(newEntry);
        }
        else
        {
            return;  // no entry and clearing to 0 = nothing to do
        }

        // Clear session override: TrackMap is now the source of truth.
        // Update engine's cachedBpmMultiplier so it takes effect immediately.
        for (auto& eng : engines)
        {
            if (eng->getActiveInput() == TimecodeEngine::InputSource::ProDJLink
                && eng->getEffectivePlayer() == playerNum)
            {
                eng->setBpmPlayerOverride(TimecodeEngine::kBpmNoOverride);
                eng->setCachedBpmMultiplier(newValue);
                break;
            }
        }

        if (onTrackMapChanged) onTrackMapChanged();
    }

    //==========================================================================
    void paint(juce::Graphics& g) override
    {
        g.fillAll(bgMain);
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(6);
        if (bounds.getWidth() < 50 || bounds.getHeight() < 50) return;

        for (auto& ds : deckState)
            ds.invalidateDeckImg();

        // --- Toolbar (top) ---
        auto toolbar = bounds.removeFromTop(22);
        btnLayout.setBounds(toolbar.removeFromLeft(40));
        toolbar.removeFromLeft(6);
        btnShowMixer.setBounds(toolbar.removeFromLeft(60));
        bounds.removeFromTop(4);

        // --- Mixer panel (right side, conditional) ---
        if (showMixer && bounds.getWidth() > 300)
        {
            int mixerW = juce::jlimit(200, 280, bounds.getWidth() / 5);
            bounds.removeFromRight(4);
            mixerBounds = bounds.removeFromRight(mixerW);
        }
        else
        {
            mixerBounds = {};
        }

        // --- Deck layout ---
        int gapH = 4, gapV = 4;

        if (layoutHorizontal)
        {
            // 4x1: four decks side by side, evenly distributed
            int totalDeckW = juce::jmax(0, bounds.getWidth() - gapH * 3);

            for (int i = 0; i < 4; ++i)
            {
                int w = totalDeckW * (i + 1) / 4 - totalDeckW * i / 4;
                deckBounds[i] = bounds.removeFromLeft(w);
                if (i < 3) bounds.removeFromLeft(gapH);
            }
        }
        else
        {
            // 2x2 grid
            int deckW = juce::jmax(0, (bounds.getWidth() - gapH) / 2);
            int deckH = juce::jmax(0, (bounds.getHeight() - gapV) / 2);

            auto topRow = bounds.removeFromTop(deckH);
            bounds.removeFromTop(gapV);
            auto bottomRow = bounds;

            deckBounds[0] = topRow.removeFromLeft(deckW);
            topRow.removeFromLeft(gapH);
            deckBounds[1] = topRow;

            deckBounds[2] = bottomRow.removeFromLeft(deckW);
            bottomRow.removeFromLeft(gapH);
            deckBounds[3] = bottomRow;
        }
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
            ds.isReverse     = proDJLink.isPlayingReverse(pn);
            ds.beatInBar     = proDJLink.getBeatInBar(pn);
            ds.xfAssign      = proDJLink.hasMixerFaderData() ? proDJLink.getChannelXfAssign(pn) : 0;
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

            // TrackMap lookup (single find, reused for offset + BPM multiplier)
            const TrackMapEntry* tmEntry = nullptr;
            ds.trackMapped = false;
            ds.offset = "00:00:00:00";
            ds.offsetTimecode = {};
            if (ds.artist.isNotEmpty() && ds.title.isNotEmpty()
                && !ds.title.startsWith("Track #"))
            {
                tmEntry = trackMap.find(ds.artist, ds.title, (int)ds.trackLenSec);
                if (tmEntry != nullptr)
                {
                    ds.trackMapped = true;
                    ds.offset = tmEntry->timecodeOffset;

                    // Parse offset and compute running timecode with offset applied
                    int oH, oM, oS, oF;
                    if (TrackMapEntry::parseTimecodeString(tmEntry->timecodeOffset, oH, oM, oS, oF))
                    {
                        ds.offsetTimecode = applyTimecodeOffset(
                            ds.timecode, fps, oH, oM, oS, oF, fps);
                    }
                }
            }

            // Engine assignment: find which engines monitor this player
            ds.engineNames.clear();
            ds.bpmSessionOverride = TimecodeEngine::kBpmNoOverride;
            ds.bpmTrackMapValue   = tmEntry ? tmEntry->bpmMultiplier : 0;
            for (auto& eng : engines)
            {
                if (eng->getActiveInput() == TimecodeEngine::InputSource::ProDJLink
                    && eng->getEffectivePlayer() == pn)
                {
                    ds.engineNames.add(eng->getName());
                    ds.bpmSessionOverride = eng->getBpmPlayerOverride();
                }
            }

            // --- Track changed? ---
            bool trackChanged = (ds.trackId != ds.prevTrackId);
            if (trackChanged)
            {
                ds.waveform.clearWaveform();
                ds.detailWaveform.clear();
                ds.displayedWaveformTrackId = 0;
                ds.displayedDetailTrackId = 0;
                ds.detailRetryTicks = 0;
                ds.previewRetryTicks = 0;
                ds.previewCuesFed = false;
                ds.previewBeatGridFed = false;
                ds.detailCuesFed = false;
                ds.detailBeatGridFed = false;
                ds.detailSongStructFed = false;
                ds.lastMetaVersion = 0;
                ds.lastDetailVersion = 0;
                ds.displayedArtworkId = 0;
                ds.cachedArtworkImg = {};
                ds.artist.clear();
                ds.title.clear();
                ds.key.clear();
                ds.artworkId = 0;
                ds.cueCount = 0;
                ds.invalidateDeckImg();
                ds.prevTrackId = ds.trackId;
                ds.metadataRequested = false;  // reset so we request for new track
            }

            // --- Request metadata independently of engines ---
            // ProDJLinkView requests metadata for ALL discovered players,
            // so waveform/artwork/info loads even without an engine assigned.
            // IMPORTANT: resolve SOURCE player IP -- when CDJ 2 loads from
            // CDJ 1's USB via Link Export, the metadata is on CDJ 1.
            uint8_t srcPlayer = proDJLink.getLoadedPlayer(pn);
            if (srcPlayer == 0) srcPlayer = (uint8_t)pn;
            juce::String srcIP = proDJLink.getPlayerIP((int)srcPlayer);
            if (srcIP.isEmpty()) srcIP = ds.ip;

            if (ds.trackId != 0 && !ds.metadataRequested
                && !srcIP.isEmpty() && dbClient.getIsRunning())
            {
                uint8_t slot = proDJLink.getLoadedSlot(pn);
                if (slot != 0)
                {
                    int dbCtx = proDJLink.getVCDJPlayerNumber();
                    juce::String model = proDJLink.getPlayerModel((int)srcPlayer);
                    dbClient.requestMetadata(
                        srcIP, slot, 1, ds.trackId, dbCtx, model);
                    ds.metadataRequested = true;
                    ds.metadataRequestTick = 0;
                }
            }

            // Retry if metadata hasn't arrived after ~3s (180 ticks at 60Hz)
            if (ds.metadataRequested && ds.artist.isEmpty()
                && ds.displayedWaveformTrackId != ds.trackId)
            {
                if (++ds.metadataRequestTick > 180)
                    ds.metadataRequested = false;  // allow re-request
            }

            // --- Metadata from DbServerClient ---
            if (ds.trackId != 0)
            {
                // Only re-fetch metadata if we still need artist/title or waveform
                // (artwork image is fetched separately below, doesn't need metadata re-fetch)
                bool needMeta = ds.artist.isEmpty()
                             || ds.displayedWaveformTrackId != ds.trackId;

                if (needMeta && !srcIP.isEmpty())
                {
                    auto meta = dbClient.getCachedMetadata(srcIP, ds.trackId);
                    if (meta.isValid())
                    {
                        ds.lastMetaVersion = meta.cacheVersion;
                        ds.artist    = meta.artist;
                        ds.title     = meta.title;
                        ds.key       = meta.key;
                        ds.artworkId = meta.artworkId;
                        ds.cueCount  = (int)meta.cueList.size();

                        if (meta.hasWaveform() && ds.displayedWaveformTrackId != ds.trackId)
                        {
                            ds.waveform.setColorWaveformData(meta.waveformData,
                                meta.waveformEntryCount, meta.waveformBytesPerEntry);
                            if (meta.durationSeconds > 0)
                                ds.waveform.setDurationMs((uint32_t)meta.durationSeconds * 1000);
                            if (meta.hasCueList())
                                { ds.waveform.setRekordboxCues(meta.cueList); ds.previewCuesFed = true; }
                            if (meta.hasBeatGrid())
                                { ds.waveform.setBeatGrid(meta.beatGrid); ds.previewBeatGridFed = true; }
                            ds.displayedWaveformTrackId = ds.trackId;
                            ds.previewRetryTicks = (!ds.previewCuesFed || !ds.previewBeatGridFed) ? 180 : 0;
                            ds.invalidateDeckImg();
                            if (!deckBounds[pn - 1].isEmpty())
                                repaint(deckBounds[pn - 1]);

                            // Save waveform + artwork to disk cache for future sessions
                            if (meta.artist.isNotEmpty() && meta.title.isNotEmpty())
                            {
                                auto diskKey = TrackMapEntry::makeKey(
                                    meta.artist, meta.title, meta.durationSeconds);
                                if (!WaveformCache::exists(diskKey))
                                {
                                    uint32_t durMs = (meta.durationSeconds > 0)
                                        ? (uint32_t)meta.durationSeconds * 1000 : 0;
                                    WaveformCache::save(diskKey, meta.waveformData,
                                        meta.waveformEntryCount, meta.waveformBytesPerEntry, durMs);
                                }
                            }
                        }
                        else if (!meta.hasWaveform() && ds.displayedWaveformTrackId != ds.trackId
                                 && ds.artist.isNotEmpty() && ds.title.isNotEmpty())
                        {
                            // Dbserver hasn't returned waveform yet -- try disk cache
                            // Use meta.durationSeconds (same source as save path),
                            // fall back to ds.trackLenSec if dbserver didn't provide it.
                            int durForKey = (meta.durationSeconds > 0)
                                ? meta.durationSeconds : (int)ds.trackLenSec;
                            auto diskKey = TrackMapEntry::makeKey(
                                ds.artist, ds.title, durForKey);
                            if (WaveformCache::exists(diskKey))
                            {
                                auto cached = WaveformCache::load(diskKey);
                                if (cached.valid)
                                {
                                    ds.waveform.setColorWaveformData(cached.data,
                                        cached.entryCount, cached.bytesPerEntry);
                                    if (cached.durationMs > 0)
                                        ds.waveform.setDurationMs(cached.durationMs);
                                    ds.displayedWaveformTrackId = ds.trackId;
                                    ds.invalidateDeckImg();
                                    if (!deckBounds[pn - 1].isEmpty())
                                        repaint(deckBounds[pn - 1]);
                                }
                            }
                        }
                    }
                    else
                    {
                        // Fallback: try by trackId only (cached from another player)
                        auto metaById = dbClient.getCachedMetadataByTrackId(ds.trackId);
                        if (metaById.isValid())
                        {
                            ds.lastMetaVersion = metaById.cacheVersion;
                            ds.artist    = metaById.artist;
                            ds.title     = metaById.title;
                            ds.key       = metaById.key;
                            ds.artworkId = metaById.artworkId;
                            ds.cueCount  = (int)metaById.cueList.size();

                            if (metaById.hasWaveform() && ds.displayedWaveformTrackId != ds.trackId)
                            {
                                ds.waveform.setColorWaveformData(metaById.waveformData,
                                    metaById.waveformEntryCount, metaById.waveformBytesPerEntry);
                                if (metaById.durationSeconds > 0)
                                    ds.waveform.setDurationMs((uint32_t)metaById.durationSeconds * 1000);
                                if (metaById.hasCueList())
                                    { ds.waveform.setRekordboxCues(metaById.cueList); ds.previewCuesFed = true; }
                                if (metaById.hasBeatGrid())
                                    { ds.waveform.setBeatGrid(metaById.beatGrid); ds.previewBeatGridFed = true; }
                                ds.displayedWaveformTrackId = ds.trackId;
                                ds.previewRetryTicks = (!ds.previewCuesFed || !ds.previewBeatGridFed) ? 180 : 0;
                                ds.invalidateDeckImg();
                                if (!deckBounds[pn - 1].isEmpty())
                                    repaint(deckBounds[pn - 1]);
                            }
                        }
                    }
                }

                // Preview supplementary retry: cues / beat grid may arrive
                // after the preview waveform.  Only fetch when the background
                // thread has actually updated the cache (version changed).
                if (ds.displayedWaveformTrackId == ds.trackId
                    && ds.previewRetryTicks > 0
                    && (!ds.previewCuesFed || !ds.previewBeatGridFed))
                {
                    uint32_t ver = dbClient.getMetadataVersion(srcIP, ds.trackId);
                    if (ver == 0) ver = dbClient.getMetadataVersionByTrackId(ds.trackId);

                    if (ver != ds.lastMetaVersion && !srcIP.isEmpty())
                    {
                        ds.lastMetaVersion = ver;
                        auto pm = dbClient.getCachedMetadata(srcIP, ds.trackId);
                        if (!pm.isValid())
                            pm = dbClient.getCachedMetadataByTrackId(ds.trackId);
                        if (pm.isValid())
                        {
                            bool changed = false;
                            if (!ds.previewCuesFed && pm.hasCueList())
                            {
                                ds.waveform.setRekordboxCues(pm.cueList);
                                ds.previewCuesFed = true;
                                ds.cueCount = (int)pm.cueList.size();
                                changed = true;
                            }
                            if (!ds.previewBeatGridFed && pm.hasBeatGrid())
                            {
                                ds.waveform.setBeatGrid(pm.beatGrid);
                                ds.previewBeatGridFed = true;
                                changed = true;
                            }
                            if (changed)
                            {
                                ds.invalidateDeckImg();
                                if (!deckBounds[pn - 1].isEmpty())
                                    repaint(deckBounds[pn - 1]);
                                DBG("PDL Preview RETRY P" + juce::String(pn)
                                    + ": cuesFed=" + juce::String((int)ds.previewCuesFed)
                                    + " beatsFed=" + juce::String((int)ds.previewBeatGridFed)
                                    + " ver=" + juce::String(ver));
                            }
                        }
                    }
                    if (ds.previewCuesFed && ds.previewBeatGridFed)
                        ds.previewRetryTicks = 0;
                    else
                        ds.previewRetryTicks--;
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
                        ds.invalidateDeckImg();
                        if (!deckBounds[pn - 1].isEmpty())
                            repaint(deckBounds[pn - 1]);

                        // Save to disk for future sessions
                        if (ds.artist.isNotEmpty() && ds.title.isNotEmpty())
                        {
                            auto diskKey = TrackMapEntry::makeKey(
                                ds.artist, ds.title, (int)ds.trackLenSec);
                            if (!WaveformCache::artworkExists(diskKey))
                                WaveformCache::saveArtwork(diskKey, artImg);
                        }
                    }
                }

                // Artwork fallback from disk cache
                if (!ds.cachedArtworkImg.isValid() && ds.artist.isNotEmpty()
                    && ds.title.isNotEmpty())
                {
                    auto diskKey = TrackMapEntry::makeKey(
                        ds.artist, ds.title, (int)ds.trackLenSec);
                    if (WaveformCache::artworkExists(diskKey))
                    {
                        auto cachedArt = WaveformCache::loadArtwork(diskKey);
                        if (cachedArt.isValid())
                        {
                            ds.cachedArtworkImg = cachedArt;
                            ds.displayedArtworkId = ds.artworkId;
                            ds.invalidateDeckImg();
                            if (!deckBounds[pn - 1].isEmpty())
                                repaint(deckBounds[pn - 1]);
                        }
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
                if (ds.displayedDetailTrackId != 0)
                {
                    ds.detailWaveform.clear();
                    ds.displayedDetailTrackId = 0;
                    ds.detailRetryTicks = 0;
                }
                ds.previewRetryTicks = 0;
                ds.previewCuesFed = false;
                ds.previewBeatGridFed = false;
                ds.detailCuesFed = false;
                ds.detailBeatGridFed = false;
                ds.detailSongStructFed = false;
                ds.lastMetaVersion = 0;
                ds.lastDetailVersion = 0;
                ds.displayedArtworkId = 0;
                ds.cachedArtworkImg = {};
                ds.artist.clear();
                ds.title.clear();
                ds.key.clear();
                ds.artworkId = 0;
                ds.cueCount = 0;
                ds.metadataRequested = false;
            }

            // Waveform cursor
            if (ds.waveform.hasWaveformData())
                ds.waveform.setPlayPosition(ds.posRatio);

            // Detail waveform: feed data, beat grid, song structure, cues.
            // Data arrives asynchronously from DbServerClient (phase 2 + NFS thread).
            // Poll periodically until detail data arrives.
            if (ds.trackId != 0 && !srcIP.isEmpty())
            {
                bool needsInitial = (ds.displayedDetailTrackId != ds.trackId);
                bool needsSupplementary = (ds.displayedDetailTrackId == ds.trackId
                                           && (!ds.detailCuesFed || !ds.detailBeatGridFed
                                               || !ds.detailSongStructFed
                                               || ds.detailWaveform.getDetailBytesPerEntry() < 3));

                bool doFetch = false;
                if (needsInitial)
                {
                    bool hasDetail = dbClient.hasDetailWaveformCached(srcIP, ds.trackId);
                    if (hasDetail)
                        doFetch = true;
                    else
                    {
                        ds.detailRetryTicks++;
                        if (ds.detailRetryTicks % 60 == 1)
                            doFetch = true;
                    }

                    // Log once per second to track progress
                    if (ds.detailRetryTicks % 60 == 0)
                    {
                        DBG("PDL Detail POLL P" + juce::String(pn)
                            + ": needsInitial tick=" + juce::String(ds.detailRetryTicks)
                            + " hasDetail=" + juce::String((int)hasDetail)
                            + " doFetch=" + juce::String((int)doFetch)
                            + " trackId=" + juce::String(ds.trackId)
                            + " srcIP=" + srcIP);
                    }
                }
                else if (needsSupplementary)
                {
                    uint32_t curVer = dbClient.getMetadataVersion(srcIP, ds.trackId);
                    if (curVer == 0) curVer = dbClient.getMetadataVersionByTrackId(ds.trackId);
                    if (curVer != 0 && curVer != ds.lastDetailVersion)
                        doFetch = true;
                }

                if (doFetch)
                {
                    auto meta = dbClient.getCachedMetadata(srcIP, ds.trackId);
                    if (!meta.isValid())
                        meta = dbClient.getCachedMetadataByTrackId(ds.trackId);
                    ds.lastDetailVersion = meta.cacheVersion;

                    DBG("PDL Detail FETCH P" + juce::String(pn)
                        + ": valid=" + juce::String((int)meta.isValid())
                        + " hasDetail=" + juce::String((int)meta.hasDetailWaveform())
                        + " detailEntries=" + juce::String(meta.detailEntryCount)
                        + " detailBpe=" + juce::String(meta.detailBytesPerEntry)
                        + " detailDataSz=" + juce::String((int)meta.detailData.size())
                        + " beats=" + juce::String((int)meta.beatGrid.size())
                        + " cues=" + juce::String((int)meta.cueList.size())
                        + " phrases=" + juce::String((int)meta.songStructure.size())
                        + " nfsAttempted=" + juce::String((int)meta.nfsAttempted)
                        + " ver=" + juce::String(meta.cacheVersion));

                    if (needsInitial)
                    {
                        if (meta.hasDetailWaveform())
                        {
                            uint32_t durMs = (meta.durationSeconds > 0)
                                ? (uint32_t)meta.durationSeconds * 1000 : 0;
                            ds.detailWaveform.setDetailData(meta.detailData,
                                meta.detailEntryCount, meta.detailBytesPerEntry, durMs);
                            ds.displayedDetailTrackId = ds.trackId;
                            ds.detailRetryTicks = 0;  // stop polling
                            ds.invalidateDeckImg();
                            if (!deckBounds[pn - 1].isEmpty())
                                repaint(deckBounds[pn - 1]);

                            // Feed any supplementary data available in this same fetch
                            // (avoids waiting for the next version change poll)
                            if (meta.hasBeatGrid())
                            {
                                ds.detailWaveform.setBeatGrid(meta.beatGrid);
                                ds.detailBeatGridFed = true;
                                for (auto& eng : engines)
                                {
                                    if (eng->getActiveInput() == TimecodeEngine::InputSource::ProDJLink
                                        && eng->getEffectivePlayer() == pn)
                                        eng->setBeatGrid(meta.beatGrid, ds.trackId);
                                }
                            }
                            if (meta.hasSongStructure())
                            {
                                ds.detailWaveform.setSongStructure(meta.songStructure, meta.phraseMood);
                                ds.detailSongStructFed = true;
                            }
                            if (meta.hasCueList())
                            {
                                ds.detailWaveform.setRekordboxCues(meta.cueList);
                                ds.detailCuesFed = true;
                                if (ds.cueCount == 0)
                                    ds.cueCount = (int)meta.cueList.size();
                            }

                            // Persist ANLZ to disk (detail waveform + whatever else arrived)
                            saveAnlzFromMeta(meta);
                        }
                    }

                    // Feed supplementary data (may arrive after detail waveform)
                    if (ds.displayedDetailTrackId == ds.trackId)
                    {
                        bool supplementaryFed = false;

                        // Upgrade detail waveform if better quality arrived
                        // (e.g. dbserver provided PWV7 3-band after NFS provided PWV5 2-byte)
                        if (meta.hasDetailWaveform()
                            && meta.detailBytesPerEntry > ds.detailWaveform.getDetailBytesPerEntry())
                        {
                            uint32_t durMs = (meta.durationSeconds > 0)
                                ? (uint32_t)meta.durationSeconds * 1000 : 0;
                            ds.detailWaveform.setDetailData(meta.detailData,
                                meta.detailEntryCount, meta.detailBytesPerEntry, durMs);
                            supplementaryFed = true;
                            DBG("PDL Detail UPGRADE P" + juce::String(pn)
                                + ": bpe " + juce::String(ds.detailWaveform.getDetailBytesPerEntry())
                                + " -> " + juce::String(meta.detailBytesPerEntry));
                        }

                        if (!ds.detailBeatGridFed && meta.hasBeatGrid())
                        {
                            ds.detailWaveform.setBeatGrid(meta.beatGrid);
                            ds.detailBeatGridFed = true;
                            supplementaryFed = true;
                            // Feed to engines for PLL micro-correction
                            for (auto& eng : engines)
                            {
                                if (eng->getActiveInput() == TimecodeEngine::InputSource::ProDJLink
                                    && eng->getEffectivePlayer() == pn)
                                    eng->setBeatGrid(meta.beatGrid, ds.trackId);
                            }
                        }
                        if (!ds.detailSongStructFed && meta.hasSongStructure())
                        {
                            ds.detailWaveform.setSongStructure(meta.songStructure, meta.phraseMood);
                            ds.detailSongStructFed = true;
                            supplementaryFed = true;
                        }
                        if (!ds.detailCuesFed && meta.hasCueList())
                        {
                            ds.detailWaveform.setRekordboxCues(meta.cueList);
                            ds.detailCuesFed = true;
                            supplementaryFed = true;
                            if (ds.cueCount == 0)
                            {
                                ds.cueCount = (int)meta.cueList.size();
                                ds.invalidateDeckImg();
                            }

                            // Auto-populate TrackMap cue points from rekordbox
                            // hot cues, but only if the entry exists and has NO
                            // manually-configured cue points.
                            // Blocked during Show Lock -- TrackMap is configuration.
                            if (ds.artist.isNotEmpty() && ds.title.isNotEmpty()
                                && (!isShowLockedFn || !isShowLockedFn()))
                            {
                                int dur = (int)ds.trackLenSec;
                                auto* mutableEntry = trackMap.find(ds.artist, ds.title, dur);
                                if (mutableEntry != nullptr && mutableEntry->cuePoints.empty())
                                {
                                    for (auto& rc : meta.cueList)
                                    {
                                        if (rc.positionMs == 0) continue;  // skip track start
                                        CuePoint cp;
                                        cp.positionMs = rc.positionMs;
                                        auto letter = rc.hotCueLetter();
                                        if (letter.isNotEmpty())
                                            cp.name = letter;
                                        if (rc.comment.isNotEmpty())
                                            cp.name += cp.name.isNotEmpty()
                                                ? " " + rc.comment : rc.comment;
                                        if (cp.name.isEmpty())
                                        {
                                            if (rc.type == TrackMetadata::RekordboxCue::MemoryPoint)
                                                cp.name = "MEM";
                                            else if (rc.type == TrackMetadata::RekordboxCue::Loop)
                                                cp.name = "LOOP";
                                        }
                                        mutableEntry->cuePoints.push_back(std::move(cp));
                                    }
                                    mutableEntry->sortCuePoints();
                                }
                            }
                        }
                        else if (!ds.detailCuesFed)
                        {
                            // Cues not yet from rekordbox -- fallback to TrackMap
                            int dur = (int)ds.trackLenSec;
                            auto* tmFallback = trackMap.find(ds.artist, ds.title, dur);
                            if (tmFallback && !tmFallback->cuePoints.empty())
                                ds.detailWaveform.setCuePoints(tmFallback->cuePoints);
                        }

                        // Force deck repaint when new data arrives for the detail waveform
                        if (supplementaryFed)
                        {
                            ds.invalidateDeckImg();
                            if (!deckBounds[pn - 1].isEmpty())
                                repaint(deckBounds[pn - 1]);

                            // Re-persist ANLZ to disk with updated supplementary data
                            saveAnlzFromMeta(meta);
                        }

                        // If all sources exhausted (NFS done), mark missing data as "fed"
                        // to stop polling for tracks that genuinely don't have cues/beats/phrases.
                        if (meta.nfsAttempted)
                        {
                            if (!ds.detailBeatGridFed && !meta.hasBeatGrid())
                                ds.detailBeatGridFed = true;
                            if (!ds.detailSongStructFed && !meta.hasSongStructure())
                                ds.detailSongStructFed = true;
                            if (!ds.detailCuesFed && !meta.hasCueList())
                                ds.detailCuesFed = true;
                        }
                    }
                }

            }


            // Detail waveform: update playhead and active loop every frame
            if (ds.detailWaveform.hasDetailData())
            {
                ds.detailWaveform.setPlayheadMs(ds.playheadMs, ds.isPlaying, ds.faderPitch);
                ds.detailWaveform.setActiveLoop(
                    proDJLink.getLoopStartMs(pn),
                    proDJLink.getLoopEndMs(pn));

                // Force repaint of detail area every frame when playing or decelerating.
                // The detail waveform is not a real child component (painted manually
                // in paintDeck), so its repaint() doesn't reach us. We must drive it.
                bool needsDetailRepaint = ds.isPlaying
                    || ds.detailWaveform.isAnimating();
                if (needsDetailRepaint && !ds.detailLocalBounds.isEmpty()
                    && !deckBounds[pn - 1].isEmpty())
                {
                    auto detAbsolute = ds.detailLocalBounds + deckBounds[pn - 1].getPosition();
                    repaint(detAbsolute);
                }
            }
        }

        // --- Smooth VU meters ---
        if (proDJLink.hasVuMeterData())
        {
            for (int ch = 0; ch < ProDJLink::kVuSlots; ++ch)
            {
                float target = proDJLink.getVuPeakNorm(ch);
                // Fast attack, slow decay (ballistic meter feel)
                if (target > vuSmoothed[ch])
                    vuSmoothed[ch] = target;
                else
                    vuSmoothed[ch] *= 0.94f;  // ~120ms decay at 60Hz
                if (vuSmoothed[ch] < 0.001f) vuSmoothed[ch] = 0.0f;
            }
        }
        else
        {
            for (int ch = 0; ch < ProDJLink::kVuSlots; ++ch)
                vuSmoothed[ch] *= 0.95f;  // decay when no VU data at 60Hz
        }

        // Two levels of dirty:
        // - staticDirty: anything except timecode.frames changed -> invalidate cached deck image
        // - frameDirty:  only timecode.frames changed -> repaint (timecode + waveform cursor overlay)
        for (int deck = 0; deck < 4; ++deck)
        {
            auto& ds = deckState[deck];
            auto& snap = ds.snapshot;

            bool staticDirty = (ds.discovered        != snap.discovered
                             || ds.isOnAir            != snap.isOnAir
                             || ds.isMaster           != snap.isMaster
                             || ds.isPlaying          != snap.isPlaying
                             || ds.isReverse          != snap.isReverse
                             || ds.trackMapped        != snap.trackMapped
                             || ds.beatInBar          != snap.beatInBar
                             || ds.xfAssign           != snap.xfAssign
                             || ds.trackId            != snap.trackId
                             || ds.bpmSessionOverride != snap.bpmSessionOverride
                             || ds.bpmTrackMapValue   != snap.bpmTrackMapValue
                             || std::abs(ds.bpm        - snap.bpm)        > 0.005
                             || std::abs(ds.faderPitch - snap.faderPitch) > 0.0001
                             || ds.playState          != snap.playState
                             || ds.artist             != snap.artist
                             || ds.title              != snap.title
                             || ds.posSource          != snap.posSource
                             || ds.fpsStr             != snap.fpsStr
                             || ds.timecode.hours     != snap.timecode.hours
                             || ds.timecode.minutes   != snap.timecode.minutes
                             || ds.timecode.seconds   != snap.timecode.seconds
                             || ds.offsetTimecode.frames != snap.offsetTimecode.frames
                             || ds.engineNames        != snap.engineNames);

            bool frameDirty = (ds.timecode.frames != snap.timecode.frames);

            if (staticDirty || frameDirty)
            {
                snap.discovered         = ds.discovered;
                snap.isOnAir            = ds.isOnAir;
                snap.isMaster           = ds.isMaster;
                snap.isPlaying          = ds.isPlaying;
                snap.isReverse          = ds.isReverse;
                snap.trackMapped        = ds.trackMapped;
                snap.beatInBar          = ds.beatInBar;
                snap.xfAssign           = ds.xfAssign;
                snap.trackId            = ds.trackId;
                snap.playheadMs         = ds.playheadMs;
                snap.bpmSessionOverride = ds.bpmSessionOverride;
                snap.bpmTrackMapValue   = ds.bpmTrackMapValue;
                snap.bpm                = ds.bpm;
                snap.faderPitch         = ds.faderPitch;
                snap.playState          = ds.playState;
                snap.artist             = ds.artist;
                snap.title              = ds.title;
                snap.posSource          = ds.posSource;
                snap.fpsStr             = ds.fpsStr;
                snap.timecode           = ds.timecode;
                snap.offsetTimecode     = ds.offsetTimecode;
                snap.engineNames        = ds.engineNames;

                if (staticDirty)
                {
                    ds.invalidateDeckImg();  // Windows: regenerate cached image; macOS: no-op
                    if (!deckBounds[deck].isEmpty())
                        repaint(deckBounds[deck]);  // full deck repaint
                }
                else if (frameDirty && !deckBounds[deck].isEmpty())
                {
                    // Only timecode frame changed -- repaint just the live overlay
                    // strips (TC digits + waveform cursor + detail waveform + offset TC).
                    // This avoids compositing the full HiDPI deck image (~3MB on
                    // Retina) when only a few rows of pixels actually changed.
                    auto origin = deckBounds[deck].getPosition();
                    juce::Rectangle<int> liveArea;
                    if (!ds.tcLocalBounds.isEmpty())
                        liveArea = ds.tcLocalBounds + origin;
                    if (!ds.wfLocalBounds.isEmpty())
                    {
                        auto wfAbs = ds.wfLocalBounds + origin;
                        liveArea = liveArea.isEmpty() ? wfAbs : liveArea.getUnion(wfAbs);
                    }
                    if (!ds.detailLocalBounds.isEmpty())
                        liveArea = liveArea.isEmpty()
                            ? (ds.detailLocalBounds + origin)
                            : liveArea.getUnion(ds.detailLocalBounds + origin);
                    if (!ds.mapLocalBounds.isEmpty())
                        liveArea = liveArea.getUnion(ds.mapLocalBounds + origin);
                    if (!liveArea.isEmpty())
                        repaint(liveArea);
                }
            }
        }

        // Mixer panel: repaint when fader data arrives or VU changes.
        if (showMixer && !mixerBounds.isEmpty())
        {
            bool hasFaderData = proDJLink.hasMixerFaderData();

            // Detect fader/knob changes via DJM packet counter.
            // Each 0x39 packet increments the counter -- if it changed,
            // the DJ moved something (fader, EQ, crossfader, etc.).
            bool faderDirty = false;
            if (hasFaderData)
            {
                uint32_t pktCount = proDJLink.getMixerPacketCount();
                if (pktCount != lastMixerPktCount)
                {
                    lastMixerPktCount = pktCount;
                    faderDirty = true;
                }
            }

            // First time fader data arrives or DJM disconnects
            if (hasFaderData && !lastHadMixerFaderData)
            {
                lastHadMixerFaderData = true;
                faderDirty = true;
            }
            else if (!hasFaderData && lastHadMixerFaderData)
            {
                lastHadMixerFaderData = false;
                repaint(mixerBounds);  // clear mixer on disconnect
            }

            // VU meters: repaint on visible change
            if (hasFaderData)
            {
                bool vuDirty = false;
                for (int ch = 0; ch < ProDJLink::kVuSlots; ++ch)
                    if (std::abs(vuSmoothed[ch] - vuSnapshotted[ch]) > 0.005f)
                        { vuDirty = true; break; }
                if (vuDirty)
                {
                    for (int ch = 0; ch < ProDJLink::kVuSlots; ++ch)
                        vuSnapshotted[ch] = vuSmoothed[ch];
                    faderDirty = true;
                }
            }

            if (faderDirty)
                repaint(mixerBounds);
        }
    }

    //==========================================================================
    // Custom painting -- all decks + mixer painted in one pass
    //==========================================================================
    void paintOverChildren(juce::Graphics& g) override
    {
        // Paint each deck (skip if bounds are too small)
        for (int deck = 0; deck < 4; ++deck)
            if (!deckBounds[deck].isEmpty())
                paintDeck(g, deckBounds[deck], deck);

        // Paint mixer (only if visible)
        if (showMixer && !mixerBounds.isEmpty())
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

    DeckState deckState[4];
    juce::Rectangle<int> deckBounds[4];
    juce::Rectangle<int> mixerBounds;

    // Layout state
    bool layoutHorizontal = false;   // false = 2x2 grid, true = 4x1 horizontal
    bool showMixer        = true;    // show DJM mixer strip
    juce::TextButton   btnLayout;
    juce::ToggleButton btnShowMixer;

    // Smoothed VU meter levels (decay-based, updated per timer tick)
    // Indices: 0-5=CH1-CH6, kVuMasterL=MasterL, kVuMasterR=MasterR
    float vuSmoothed   [ProDJLink::kVuSlots] {};
    float vuSnapshotted[ProDJLink::kVuSlots] {};  // last-painted VU values for dirty check
    bool     lastHadMixerFaderData = false;        // tracks hasMixerFaderData() transitions
    uint32_t lastMixerPktCount     = 0;            // tracks getMixerPacketCount() for fader dirty

    // Shared data references (not owned)
    ProDJLinkInput& proDJLink;
    DbServerClient& dbClient;
    TrackMap&        trackMap;
    std::vector<std::unique_ptr<TimecodeEngine>>& engines;

    //==========================================================================
    // Paint one deck panel
    //==========================================================================

#if JUCE_WINDOWS
    /// Detect the display scale factor for HiDPI rendering (Windows deck cache).
    float getDisplayScale() const
    {
        if (auto* tlc = getTopLevelComponent())
            if (auto* peer = tlc->getPeer())
                return (float)peer->getPlatformScaleFactor();
        auto& displays = juce::Desktop::getInstance().getDisplays();
        if (auto* primary = displays.getPrimaryDisplay())
            return (float)primary->scale;
        return 1.0f;
    }
#endif

    void paintDeck(juce::Graphics& g, juce::Rectangle<int> area, int deckIndex)
    {
        if (area.isEmpty()) return;

        auto& ds = deckState[deckIndex];
        int w = area.getWidth(), h = area.getHeight();

#if JUCE_WINDOWS
        // Windows: cached HiDPI deck image (avoids full repaint every frame)
        float scale = getDisplayScale();
        if (scale < 1.0f) scale = 1.0f;

        if (!ds.deckImgValid || ds.cachedDeckW != w || ds.cachedDeckH != h
            || ds.cachedDeckScale != scale)
        {
            int imgW = (int)std::ceil(w * scale);
            int imgH = (int)std::ceil(h * scale);
            ds.cachedDeckImg = juce::Image(juce::Image::ARGB, imgW, imgH, true);
            juce::Graphics ig(ds.cachedDeckImg);
            ig.addTransform(juce::AffineTransform::scale(scale));
            paintDeckStatic(ig, juce::Rectangle<int>(0, 0, w, h), deckIndex);
            ds.cachedDeckW     = w;
            ds.cachedDeckH     = h;
            ds.cachedDeckScale = scale;
            ds.deckImgValid    = true;
        }
        g.drawImage(ds.cachedDeckImg, area.toFloat());
#else
        // macOS: paint directly into the CoreGraphics backing store.
        // Text rendered into a juce::Image loses CoreGraphics subpixel
        // antialiasing (only works on opaque native backing stores), so
        // caching always produces blurry text on macOS.  The targeted
        // dirty rects keep per-frame work minimal: frameDirty only
        // repaints the TC + waveform + offset TC strips, not the full deck.
        {
            juce::Graphics::ScopedSaveState sss(g);
            g.reduceClipRegion(area);
            g.setOrigin(area.getPosition());
            paintDeckStatic(g, juce::Rectangle<int>(0, 0, w, h), deckIndex);
        }
#endif

        // Waveform -- painted LIVE from WaveformDisplay's own cached image.
        if (!ds.wfLocalBounds.isEmpty())
        {
            auto wfAbsolute = ds.wfLocalBounds + area.getPosition();
            juce::Graphics::ScopedSaveState sss(g);
            g.reduceClipRegion(wfAbsolute);
            g.setOrigin(wfAbsolute.getPosition());
            ds.waveform.setBounds(0, 0, wfAbsolute.getWidth(), wfAbsolute.getHeight());
            ds.waveform.paint(g);
        }

        // Detail waveform -- painted LIVE (scrolls with playhead every frame).
        if (!ds.detailLocalBounds.isEmpty() && ds.detailWaveform.hasDetailData())
        {
            auto detAbsolute = ds.detailLocalBounds + area.getPosition();
            juce::Graphics::ScopedSaveState sss(g);
            g.reduceClipRegion(detAbsolute);
            g.setOrigin(detAbsolute.getPosition());
            ds.detailWaveform.setBounds(0, 0, detAbsolute.getWidth(), detAbsolute.getHeight());
            ds.detailWaveform.paint(g);
        }

        // Draw timecode live on top
        paintDeckTimecode(g, area, deckIndex);
    }

    // Paints everything except the timecode digits into g (local coords).
    void paintDeckStatic(juce::Graphics& g, juce::Rectangle<int> area, int deckIndex)
    {
        auto& ds = deckState[deckIndex];
        int pn = deckIndex + 1;
        auto af = area.toFloat();

        // Clear waveform/timecode bounds -- will be set below if layout succeeds.
        // This prevents stale bounds from a previous larger layout being
        // used when the deck is too small to lay out (early return below).
        ds.wfLocalBounds = {};
        ds.detailLocalBounds = {};
        ds.tcLocalBounds = {};
        ds.mapLocalBounds = {};
        ds.timeLocalBounds = {};

        // Background panel
        g.setColour(bgDeck);
        g.fillRoundedRectangle(af, 4.0f);
        g.setColour(borderCol);
        g.drawRoundedRectangle(af, 4.0f, 1.0f);

        auto inner = area.reduced(6);
        if (inner.getWidth() < 30 || inner.getHeight() < 30) return;
        int deckH = inner.getHeight();

        // --- Proportional layout ---
        // Fixed chrome: header(22) + gaps(~20) + map(16) + engine(16) + bpmRow(18) = ~92px
        // Flexible: infoRow, preview waveform, detail waveform, timecode
        constexpr int kFixedChrome = 92;
        int flexH = juce::jmax(0, deckH - kFixedChrome);

        // Distribute flexible space: info 28%, preview 18%, detail 34%, timecode 20%
        int infoH   = juce::jmax(50, (int)(flexH * 0.28f));
        int wfH     = juce::jmax(24, (int)(flexH * 0.18f));
        int detH    = juce::jmax(30, (int)(flexH * 0.34f));
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

        // Crossfader assign badge (A/B from DJM, only when not THRU)
        if (ds.xfAssign == 1 || ds.xfAssign == 2)
        {
            auto xfBadge = headerRow.removeFromLeft(16).toFloat().reduced(0, 3);
            juce::Colour xfCol = (ds.xfAssign == 1) ? accentCyan : accentAmber;
            g.setColour(xfCol.withAlpha(0.25f));
            g.fillRoundedRectangle(xfBadge, 2.0f);
            g.setColour(xfCol);
            g.setFont(juce::Font(juce::FontOptions(8.0f, juce::Font::bold)));
            g.drawText(ds.xfAssign == 1 ? "A" : "B", xfBadge, juce::Justification::centred);
            headerRow.removeFromLeft(2);
        }

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
                    rightBadges.removeFromRight(2);
                }
                if (ds.isReverse)
                {
                    auto rb = rightBadges.removeFromRight(40).toFloat().reduced(0, 2);
                    g.setColour(juce::Colour(0xFFCC3030).withAlpha(0.25f));
                    g.fillRoundedRectangle(rb, 2.0f);
                    g.setColour(juce::Colour(0xFFCC3030));
                    g.setFont(juce::Font(juce::FontOptions(8.0f, juce::Font::bold)));
                    g.drawText("REV", rb, juce::Justification::centred);
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

            // BPM + Key + multiplied BPM if active
            auto bpmRow = infoArea.removeFromTop(lineH);
            juce::String bpmStr;
            if (ds.bpm > 0.0)
            {
                bpmStr += juce::String(ds.bpm, 1) + " BPM";

                // Show effective (multiplied) BPM if multiplier is active
                int sess = ds.bpmSessionOverride;
                int map  = ds.bpmTrackMapValue;
                int eff  = (sess != TimecodeEngine::kBpmNoOverride) ? sess : map;
                if (eff != 0)
                {
                    double multBpm = TimecodeEngine::applyBpmMultiplier(ds.bpm, eff);
                    juce::String multLabel;
                    switch (eff) {
                        case  1: multLabel = "x2"; break;
                        case  2: multLabel = "x4"; break;
                        case -1: multLabel = "/2"; break;
                        case -2: multLabel = "/4"; break;
                        default: break;
                    }
                    bpmStr += "  " + juce::String::charToString(0x2192) + " "
                            + juce::String(multBpm, 1) + " (" + multLabel + ")";
                }
            }
            if (ds.key.isNotEmpty()) bpmStr += "  " + ds.key;
            if (ds.cueCount > 0) bpmStr += "  | " + juce::String(ds.cueCount) + " cues";
            g.setFont(juce::Font(juce::FontOptions(detailFs)));
            g.setColour(accentCyan.brighter(0.3f));
            g.drawText(bpmStr, bpmRow, juce::Justification::centredLeft, true);

            // Pitch (fader position -- this is what the DJ sets)
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

            // Track time: elapsed or remaining (click to toggle)
            if (infoArea.getHeight() >= 10 && ds.trackLenSec > 0)
            {
                auto timeRow = infoArea.removeFromTop(juce::jmin(lineH, infoArea.getHeight()));
                ds.timeLocalBounds = timeRow;

                uint32_t elapsedSec = ds.playheadMs / 1000;
                juce::String timeStr;

                if (ds.showRemainingTime)
                {
                    uint32_t totalSec = ds.trackLenSec;
                    uint32_t remainSec = (elapsedSec < totalSec) ? (totalSec - elapsedSec) : 0;
                    uint32_t rm = remainSec / 60;
                    uint32_t rs = remainSec % 60;
                    timeStr = "-" + juce::String(rm).paddedLeft('0', 2) + ":"
                            + juce::String(rs).paddedLeft('0', 2);
                }
                else
                {
                    uint32_t em = elapsedSec / 60;
                    uint32_t es = elapsedSec % 60;
                    timeStr = juce::String(em).paddedLeft('0', 2) + ":"
                            + juce::String(es).paddedLeft('0', 2);
                }

                // Total duration
                uint32_t tm = ds.trackLenSec / 60;
                uint32_t ts = ds.trackLenSec % 60;
                timeStr += " / " + juce::String(tm).paddedLeft('0', 2) + ":"
                         + juce::String(ts).paddedLeft('0', 2);

                g.setFont(juce::Font(juce::FontOptions(getMonoFontName(), detailFs, juce::Font::bold)));
                g.setColour(ds.showRemainingTime ? accentAmber : textMid);
                g.drawText(timeStr, timeRow, juce::Justification::centredLeft);
            }
        }

        inner.removeFromTop(4);

        //----------------------------------------------------------------------
        // Waveform area -- NOT painted here.  The waveform is painted LIVE
        // in paintDeck() so the cursor updates every frame.
        // We just store the local bounds and fill the background.
        //----------------------------------------------------------------------
        auto wfBounds = inner.removeFromTop(wfH);
        ds.wfLocalBounds = wfBounds;
        g.setColour(juce::Colour(0xFF0D1117));
        g.fillRoundedRectangle(wfBounds.toFloat(), 3.0f);

        inner.removeFromTop(2);

        //----------------------------------------------------------------------
        // Detail waveform area -- painted LIVE in paintDeck(), same as preview.
        // Reserve space and fill background.
        //----------------------------------------------------------------------
        auto detBounds = inner.removeFromTop(detH);
        ds.detailLocalBounds = detBounds;
        g.setColour(juce::Colour(0xFF0A0E14));
        g.fillRoundedRectangle(detBounds.toFloat(), 3.0f);

        inner.removeFromTop(4);

        //----------------------------------------------------------------------
        // Timecode display -- drawn LIVE by paintDeckTimecode, not here.
        // Reserve the space and store bounds for targeted repaints.
        //----------------------------------------------------------------------
        ds.tcLocalBounds = inner.removeFromTop(tcH);

        inner.removeFromTop(2);

        //----------------------------------------------------------------------
        // TrackMap offset row -- static background only (text drawn live)
        //----------------------------------------------------------------------
        auto mapRow = inner.removeFromTop(16);
        ds.mapLocalBounds = mapRow;
        {
            if (ds.trackMapped)
            {
                g.setColour(accentAmber.withAlpha(0.2f));
                g.fillRoundedRectangle(mapRow.toFloat(), 2.0f);
                // Small "MAP" label only (offset TC text drawn live)
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

        inner.removeFromTop(2);

        //----------------------------------------------------------------------
        // BPM Multiplier row -- 5 buttons: /4  /2  1x  x2  x4
        // Session override takes priority; TrackMap value shown dimmer when
        // no session override is active.
        //----------------------------------------------------------------------
        auto bpmRow = inner.removeFromTop(16);
        {
            // Effective multiplier: session takes priority over TrackMap
            int sess = ds.bpmSessionOverride;
            int map  = ds.bpmTrackMapValue;
            int eff  = (sess != TimecodeEngine::kBpmNoOverride) ? sess : map;

            // Button definitions: label, multiplier value
            struct BpmBtn { const char* label; int mult; };
            static const BpmBtn kBtns[5] = {
                { "/4", -2 }, { "/2", -1 }, { "1x", 0 }, { "x2", 1 }, { "x4", 2 }
            };

            int gap = 2;
            int btnW = (bpmRow.getWidth() - gap * 4) / 5;
            auto rowCopy = bpmRow;

            for (int bi = 0; bi < 5; ++bi)
            {
                auto btnRect = (bi < 4) ? rowCopy.removeFromLeft(btnW) : rowCopy;
                if (bi < 4) rowCopy.removeFromLeft(gap);

                // Store for hit-testing in mouseDown
                ds.bpmBtnBounds[bi] = btnRect;

                int mult = kBtns[bi].mult;
                bool active   = (eff == mult);
                bool isSaved  = (map != 0 && map == mult);  // has TrackMap value on this button

                // Background: active gets blue glow, inactive gets dim
                juce::Colour bg, fg;
                if (active)
                {
                    bg = accentCyan.withAlpha(0.30f);
                    fg = accentCyan.brighter(0.3f);
                }
                else
                {
                    bg = bgDeck.brighter(0.05f);
                    fg = textDim;
                }

                // Golden text always wins for saved TrackMap value
                if (isSaved)
                    fg = accentAmber;

                auto bf = btnRect.toFloat();
                g.setColour(bg);
                g.fillRoundedRectangle(bf, 2.0f);
                g.setColour(active ? accentCyan.withAlpha(0.6f) : borderCol);
                g.drawRoundedRectangle(bf, 2.0f, 0.5f);

                g.setFont(juce::Font(juce::FontOptions(8.0f, juce::Font::bold)));
                g.setColour(fg);
                g.drawText(kBtns[bi].label, btnRect, juce::Justification::centred);
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
    // Live timecode overlay -- painted on top of the deck static content.
    // Runs every repaint; the static elements are also direct-painted.
    //==========================================================================
    void paintDeckTimecode(juce::Graphics& g, juce::Rectangle<int> area, int deckIndex)
    {
        if (area.isEmpty()) return;
        auto& ds = deckState[deckIndex];

        // No timecode overlay when deck is not connected
        if (!ds.discovered) return;

        // Use pre-computed bounds from paintDeckStatic (deck-local -> absolute)
        auto offset = area.getPosition();

        if (!ds.tcLocalBounds.isEmpty())
        {
            auto tcRow = ds.tcLocalBounds + offset;
            int tcH = tcRow.getHeight();

            // Fill TC area background
            g.setColour(bgDeck);
            g.fillRect(tcRow);

            // Draw timecode
            float tcFontSize = juce::jlimit(14.0f, 48.0f, tcH * 0.8f);
            juce::String tcStr = timecodeToString(ds.timecode);
            auto tcFont = juce::Font(juce::FontOptions(getMonoFontName(), tcFontSize, juce::Font::bold));
            float availW = (float)tcRow.getWidth();
            while (tcFontSize > 8.0f && measureStringWidth(tcFont, tcStr) > availW)
            {
                tcFontSize -= 1.0f;
                tcFont = juce::Font(juce::FontOptions(getMonoFontName(), tcFontSize, juce::Font::bold));
            }
            g.setFont(tcFont);
            g.setColour(ds.isPlaying ? tcGlow : textMid);
            g.drawText(tcStr, tcRow, juce::Justification::centred);
        }

        // Draw offset timecode text
        if (ds.trackMapped && !ds.mapLocalBounds.isEmpty())
        {
            auto mapRow = ds.mapLocalBounds + offset;
            juce::String offsetTcStr = timecodeToString(ds.offsetTimecode);
            g.setColour(accentAmber);
            g.setFont(juce::Font(juce::FontOptions(getMonoFontName(), 11.0f, juce::Font::bold)));
            g.drawText(offsetTcStr, mapRow, juce::Justification::centred);
        }
    }

    //==========================================================================
    // Mixer panel (DJM -- vertical side panel)
    //==========================================================================
    void paintMixer(juce::Graphics& g, juce::Rectangle<int> area)
    {
        if (area.getWidth() < 20 || area.getHeight() < 40) return;

        auto af = area.toFloat();
        g.setColour(bgDeck);
        g.fillRoundedRectangle(af, 4.0f);
        g.setColour(borderCol);
        g.drawRoundedRectangle(af, 4.0f, 1.0f);

        auto inner = area.reduced(6, 4);

        // --- Header: DJM model ---
        auto headerRow = inner.removeFromTop(16);
        juce::String djmModel = proDJLink.getDJMModel();
        if (djmModel.isEmpty()) djmModel = "DJM";
        g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
        g.setColour(textMid);
        g.drawText(djmModel, headerRow, juce::Justification::centred);
        inner.removeFromTop(4);

        if (!proDJLink.hasMixerFaderData())
        {
            g.setColour(textDim);
            g.setFont(juce::Font(juce::FontOptions(9.0f)));
            g.drawText("Waiting for\nbridge data...", inner,
                        juce::Justification::centred);
            return;
        }

        // --- Bottom section: crossfader + master (reserve space) ---
        auto bottomSection = inner.removeFromBottom(juce::jmin(90, inner.getHeight() / 3));
        inner.removeFromBottom(juce::jmin(4, inner.getHeight()));

        // --- Channel strips: columns filling the remaining height ---
        int numCh = proDJLink.getMixerChannelCount();
        int gap = 3;
        int totalW = inner.getWidth() - gap * (numCh - 1);

        for (int ch = 1; ch <= numCh; ++ch)
        {
            int colW = totalW * ch / numCh - totalW * (ch - 1) / numCh;
            auto col = inner.removeFromLeft(colW);
            if (ch < numCh) inner.removeFromLeft(gap);

            paintChannelStrip(g, col, ch);
        }

        // --- Crossfader ---
        if (bottomSection.getHeight() > 30)
        {
            auto xfRow = bottomSection.removeFromTop(28);
            paintCrossfader(g, xfRow.toFloat(), proDJLink.getCrossfader());
            bottomSection.removeFromTop(4);
        }

        // --- Master fader (horizontal) + stereo VU ---
        if (bottomSection.getHeight() > 10)
        {
            auto mstRow = bottomSection;
            auto labelArea = mstRow.removeFromTop(10);
            g.setFont(juce::Font(juce::FontOptions(8.0f, juce::Font::bold)));
            g.setColour(accentAmber);
            g.drawText("MASTER", labelArea, juce::Justification::centred);

            // Horizontal master bar
            auto barArea = mstRow.removeFromTop(8).reduced(4, 0);
            g.setColour(juce::Colour(0xFF1A1D23));
            g.fillRoundedRectangle(barArea.toFloat(), 2.0f);
            float mstPct = proDJLink.getMasterFader() / 255.0f;
            auto fillBar = barArea.toFloat().withWidth(barArea.getWidth() * mstPct);
            g.setColour(accentAmber.withAlpha(0.7f));
            g.fillRoundedRectangle(fillBar, 2.0f);

            mstRow.removeFromTop(3);

            // Stereo VU bars (horizontal)
            auto vuRow = mstRow.removeFromTop(6).reduced(4, 0);
            paintHorizontalVu(g, vuRow.removeFromTop(3), vuSmoothed[ProDJLink::kVuMasterL]);
            paintHorizontalVu(g, vuRow, vuSmoothed[ProDJLink::kVuMasterR]);

            mstRow.removeFromTop(3);

            // Booth + Headphone info
            auto infoRow = mstRow.removeFromTop(10);
            g.setFont(juce::Font(juce::FontOptions(7.0f)));
            g.setColour(textDim);
            int boothPct = (int)std::round(proDJLink.getBoothLevel() / 255.0f * 100.0f);
            int hpPct    = (int)std::round(proDJLink.getHpLevel() / 255.0f * 100.0f);
            g.drawText("BOOTH " + juce::String(boothPct) + "%  HP " + juce::String(hpPct) + "%",
                        infoRow, juce::Justification::centred);

            // Beat FX info
            auto fxRow = mstRow.removeFromTop(10);
            static const char* fxNames900[] = {
                "DELAY","ECHO","PING PONG","SPIRAL","REVERB","TRANS",
                "FILTER","FLANGER","PHASER","PITCH","SLIP ROLL","ROLL",
                "V.BRAKE","HELIX"
            };
            static const char* fxNamesA9[] = {
                "DELAY","ECHO","PING PONG","SPIRAL","HELIX","REVERB",
                "FLANGER","PHASER","FILTER","TRIP FLT","TRANS","ROLL",
                "TRIP ROLL","MOBIUS"
            };
            static const char* fxNamesV10[] = {
                "DELAY","ECHO","PING PONG","SPIRAL","HELIX","REVERB",
                "SHIMMER","FLANGER","PHASER","FILTER","TRANS","ROLL",
                "PITCH","V.BRAKE"
            };
            DjmModel djmG = djmModelFromString(proDJLink.getDJMModel());
            const char** fxNames = (djmG == DjmModel::V10Only) ? fxNamesV10
                                 : (djmG == DjmModel::A9Plus)  ? fxNamesA9
                                                                : fxNames900;
            int fxSel = proDJLink.getBeatFxSelect();
            bool fxOn = proDJLink.getBeatFxOn() != 0;
            juce::String fxStr = (fxSel >= 0 && fxSel < 14)
                ? juce::String(fxNames[fxSel]) : "FX";
            g.setColour(fxOn ? accentCyan : textDim);
            g.setFont(juce::Font(juce::FontOptions(7.0f, fxOn ? juce::Font::bold : juce::Font::plain)));
            g.drawText("FX: " + fxStr + (fxOn ? " ON" : ""), fxRow, juce::Justification::centred);
        }
    }

    //==========================================================================
    // Single channel strip (vertical: label, trim, EQ, color, CUE, fader+VU, XF)
    //==========================================================================
    void paintChannelStrip(juce::Graphics& g, juce::Rectangle<int> area, int ch)
    {
        bool onAir = proDJLink.isPlayerOnAir(ch);
        DjmModel djm = djmModelFromString(proDJLink.getDJMModel());
        bool isV10 = (djm == DjmModel::V10Only);
        bool hasDualCue = (djm >= DjmModel::A9Plus);  // A9 and V10 have CUE A/B
        juce::Colour chCol = onAir ? accentGreen : accentCyan;

        // --- Channel label + on-air ---
        auto labelRow = area.removeFromTop(12);
        g.setFont(juce::Font(juce::FontOptions(8.0f, juce::Font::bold)));
        g.setColour(onAir ? accentGreen : textMid);
        g.drawText(juce::String(ch), labelRow, juce::Justification::centred);
        area.removeFromTop(2);

        // --- Trim (small horizontal bar) ---
        paintParamBar(g, area.removeFromTop(6), proDJLink.getChannelTrim(ch), textMid, "T");
        area.removeFromTop(2);

        // --- Compressor (V10 only) ---
        if (isV10)
        {
            paintParamBar(g, area.removeFromTop(6), proDJLink.getChannelComp(ch), accentCyan, "K");
            area.removeFromTop(1);
        }

        // --- EQ HI / MID / LO (center-referenced bars) ---
        // V10 has 4-band EQ: Hi, Hi-Mid, Lo-Mid, Lo
        paintEqBar(g, area.removeFromTop(6), proDJLink.getChannelEqHi(ch), accentAmber, "H");
        area.removeFromTop(1);
        paintEqBar(g, area.removeFromTop(6), proDJLink.getChannelEqMid(ch), accentAmber, isV10 ? "h" : "M");
        area.removeFromTop(1);
        if (isV10)
        {
            paintEqBar(g, area.removeFromTop(6), proDJLink.getChannelEqLoMid(ch), accentAmber, "l");
            area.removeFromTop(1);
        }
        paintEqBar(g, area.removeFromTop(6), proDJLink.getChannelEqLo(ch), accentAmber, "L");
        area.removeFromTop(2);

        // --- Color knob (horizontal bar) ---
        paintEqBar(g, area.removeFromTop(6), proDJLink.getChannelColor(ch), accentCyan, "C");
        area.removeFromTop(2);

        // --- Send knob (V10 only) ---
        if (isV10)
        {
            paintParamBar(g, area.removeFromTop(6), proDJLink.getChannelSend(ch), accentGreen, "S");
            area.removeFromTop(2);
        }

        // --- CUE button(s) ---
        {
            bool cue = proDJLink.getChannelCue(ch) != 0;
            bool cueB = hasDualCue && proDJLink.getChannelCueB(ch) != 0;
            auto cueRow = area.removeFromTop(10);

            if (hasDualCue)
            {
                // Two CUE buttons side by side
                int btnW = juce::jmin(20, (cueRow.getWidth() - 2) / 2);
                int totalW = btnW * 2 + 2;
                int xOff = (cueRow.getWidth() - totalW) / 2;
                auto cueA = cueRow.withX(cueRow.getX() + xOff).withWidth(btnW).withHeight(10);
                auto cueBx = cueA.translated(btnW + 2, 0);

                g.setColour(cue ? juce::Colour(0xFFFF6D00) : juce::Colour(0xFF1A1D23));
                g.fillRoundedRectangle(cueA.toFloat(), 2.0f);
                g.setFont(juce::Font(juce::FontOptions(6.0f, juce::Font::bold)));
                g.setColour(cue ? juce::Colours::black : textDim);
                g.drawText("A", cueA, juce::Justification::centred);

                g.setColour(cueB ? juce::Colour(0xFFFF6D00) : juce::Colour(0xFF1A1D23));
                g.fillRoundedRectangle(cueBx.toFloat(), 2.0f);
                g.setColour(cueB ? juce::Colours::black : textDim);
                g.drawText("B", cueBx, juce::Justification::centred);
            }
            else
            {
                auto cueBox = cueRow.reduced(juce::jmax(0, (cueRow.getWidth() - 20) / 2), 0)
                                     .withHeight(10);
                g.setColour(cue ? juce::Colour(0xFFFF6D00) : juce::Colour(0xFF1A1D23));
                g.fillRoundedRectangle(cueBox.toFloat(), 2.0f);
                g.setFont(juce::Font(juce::FontOptions(7.0f, juce::Font::bold)));
                g.setColour(cue ? juce::Colours::black : textDim);
                g.drawText("CUE", cueBox, juce::Justification::centred);
            }
        }
        area.removeFromTop(3);

        // --- XF assign at bottom (skip if no space) ---
        if (area.getHeight() > 24)
        {
            auto xfRow = area.removeFromBottom(10);
            uint8_t xfAssign = proDJLink.getChannelXfAssign(ch);
            {
                juce::String xfLabel = (xfAssign == 1) ? "A" : (xfAssign == 2) ? "B" : "-";
                juce::Colour xfCol = (xfAssign == 1) ? accentCyan
                                   : (xfAssign == 2) ? accentAmber : textDim;
                g.setFont(juce::Font(juce::FontOptions(8.0f, juce::Font::bold)));
                g.setColour(xfCol);
                g.drawText(xfLabel, xfRow, juce::Justification::centred);
            }
            area.removeFromBottom(2);
        }

        // --- Fader + VU (fills remaining height, skip if too small) ---
        if (area.getHeight() > 4)
        {
            paintFader(g, area.toFloat(), proDJLink.getChannelFader(ch),
                       "", chCol, vuSmoothed[ch - 1]);
        }
    }

    //==========================================================================
    // Horizontal parameter bar (0-255 range, left to right fill)
    //==========================================================================
    void paintParamBar(juce::Graphics& g, juce::Rectangle<int> area,
                       uint8_t val, juce::Colour color, const juce::String& label)
    {
        // Tiny label on left
        if (label.isNotEmpty() && area.getWidth() > 20)
        {
            auto lbl = area.removeFromLeft(8);
            g.setFont(juce::Font(juce::FontOptions(6.0f)));
            g.setColour(textDim);
            g.drawText(label, lbl, juce::Justification::centredRight);
            area.removeFromLeft(1);
        }
        auto bar = area.toFloat();
        g.setColour(juce::Colour(0xFF1A1D23));
        g.fillRoundedRectangle(bar, 1.5f);
        float pct = val / 255.0f;
        g.setColour(color.withAlpha(0.6f));
        g.fillRoundedRectangle(bar.withWidth(bar.getWidth() * pct), 1.5f);
    }

    //==========================================================================
    // Horizontal EQ bar (center-referenced: 128=center, < left, > right)
    //==========================================================================
    void paintEqBar(juce::Graphics& g, juce::Rectangle<int> area,
                    uint8_t val, juce::Colour color, const juce::String& label)
    {
        if (label.isNotEmpty() && area.getWidth() > 20)
        {
            auto lbl = area.removeFromLeft(8);
            g.setFont(juce::Font(juce::FontOptions(6.0f)));
            g.setColour(textDim);
            g.drawText(label, lbl, juce::Justification::centredRight);
            area.removeFromLeft(1);
        }
        auto bar = area.toFloat();
        g.setColour(juce::Colour(0xFF1A1D23));
        g.fillRoundedRectangle(bar, 1.5f);

        float center = bar.getX() + bar.getWidth() * 0.5f;
        float pct = (val - 128) / 128.0f;  // -1..+1

        // Draw center tick
        g.setColour(textDim.withAlpha(0.3f));
        g.fillRect(center - 0.5f, bar.getY(), 1.0f, bar.getHeight());

        // Draw deviation bar from center
        float devW = std::abs(pct) * bar.getWidth() * 0.5f;
        float devX = (pct >= 0) ? center : center - devW;
        g.setColour(color.withAlpha(0.6f));
        g.fillRoundedRectangle(devX, bar.getY(), devW, bar.getHeight(), 1.0f);
    }

    //==========================================================================
    // Horizontal VU bar (for master section)
    //==========================================================================
    void paintHorizontalVu(juce::Graphics& g, juce::Rectangle<int> area, float level)
    {
        auto bar = area.toFloat();
        g.setColour(juce::Colour(0xFF0A0C10));
        g.fillRoundedRectangle(bar, 1.0f);
        if (level <= 0.001f) return;

        constexpr float kVuGain = 3.0f;
        float boosted = juce::jlimit(0.0f, 1.0f, level * kVuGain);
        float scaled  = (boosted > 0.0f)
            ? juce::jlimit(0.0f, 1.0f, std::log10(1.0f + boosted * 9.0f))
            : 0.0f;

        // Fill with green-yellow-red gradient
        float fillW = scaled * bar.getWidth();
        for (float x = 0; x < fillW; x += 2.0f)
        {
            float t = x / bar.getWidth();
            juce::Colour col = (t < 0.65f) ? accentGreen.withAlpha(0.75f)
                             : (t < 0.85f) ? accentAmber.withAlpha(0.80f)
                             : accentRed.withAlpha(0.85f);
            g.setColour(col);
            g.fillRect(bar.getX() + x, bar.getY(), 1.5f, bar.getHeight());
        }
    }

    //==========================================================================
    // Vertical fader with value bar + optional VU meter
    //==========================================================================
    void paintFader(juce::Graphics& g, juce::Rectangle<float> area,
                    uint8_t val, const juce::String& label, juce::Colour color,
                    float vuLevel = -1.0f)
    {
        if (label.isNotEmpty())
        {
            auto labelArea = area.removeFromTop(12.0f);
            g.setFont(juce::Font(juce::FontOptions(8.0f, juce::Font::bold)));
            g.setColour(textMid);
            g.drawText(label, labelArea, juce::Justification::centred);
        }

        bool hasVu = (vuLevel >= 0.0f);
        float vuBarW = 3.0f;
        juce::Rectangle<float> vuArea1;
        if (hasVu)
        {
            area.removeFromRight(1.0f);
            vuArea1 = area.removeFromRight(vuBarW);
        }

        float trackW = juce::jmin(8.0f, area.getWidth() * 0.6f);
        float trackX = area.getCentreX() - trackW * 0.5f;
        float trackH = area.getHeight();
        float trackY = area.getY();
        auto trackRect = juce::Rectangle<float>(trackX, trackY, trackW, trackH);

        g.setColour(juce::Colour(0xFF1A1D23));
        g.fillRoundedRectangle(trackRect, 2.0f);

        float fillH = (val / 255.0f) * trackH;
        auto fillRect = juce::Rectangle<float>(trackX, trackY + trackH - fillH, trackW, fillH);
        g.setColour(color.withAlpha(0.7f));
        g.fillRoundedRectangle(fillRect, 2.0f);

        if (hasVu) paintVuBar(g, vuArea1, vuLevel);
    }

    //==========================================================================
    // Vertical VU meter bar (bottom-up, green-yellow-red segmented)
    //==========================================================================
    void paintVuBar(juce::Graphics& g, juce::Rectangle<float> area, float level)
    {
        float h = area.getHeight();
        float w = area.getWidth();
        float x = area.getX();
        float y = area.getY();

        g.setColour(juce::Colour(0xFF0A0C10));
        g.fillRoundedRectangle(area, 1.0f);
        if (level <= 0.001f) return;

        constexpr float kVuGain = 3.0f;
        float boosted = juce::jlimit(0.0f, 1.0f, level * kVuGain);
        float scaled  = (boosted > 0.0f)
            ? juce::jlimit(0.0f, 1.0f, std::log10(1.0f + boosted * 9.0f))
            : 0.0f;
        float fillH = scaled * h;
        float segH = h / 15.0f;
        for (int seg = 0; seg < 15; ++seg)
        {
            float segBottom = y + h - (seg + 1) * segH;
            if (segBottom < y + h - fillH) continue;

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

        // Track (horizontal) -- inset to leave room for A/B labels at edges
        float labelW = 10.0f;
        float trackH = 8.0f;
        float trackY = area.getCentreY() - trackH * 0.5f;
        auto trackArea = area.reduced(labelW + 2.0f, 0.0f);
        auto trackRect = juce::Rectangle<float>(trackArea.getX(), trackY, trackArea.getWidth(), trackH);

        g.setColour(juce::Colour(0xFF1A1D23));
        g.fillRoundedRectangle(trackRect, 2.0f);

        // Thumb position: 0=side-A (left), 255=side-B (right) -- already corrected
        float thumbW = 12.0f;
        float thumbH = 14.0f;
        float range = trackArea.getWidth() - thumbW;
        float normPos = val / 255.0f;  // 0(A)=left, 255(B)=right -- direct mapping
        float thumbX = trackArea.getX() + normPos * range;
        float thumbY = area.getCentreY() - thumbH * 0.5f;

        g.setColour(accentCyan.withAlpha(0.7f));
        g.fillRoundedRectangle(thumbX, thumbY, thumbW, thumbH, 2.0f);

        // A / B labels -- vertically centred with the thumb, at bar edges
        g.setFont(juce::Font(juce::FontOptions(8.0f)));
        g.setColour(textDim);
        g.drawText("A", juce::Rectangle<float>(area.getX(), thumbY, labelW, thumbH),
                   juce::Justification::centred);
        g.drawText("B", juce::Rectangle<float>(area.getRight() - labelW, thumbY, labelW, thumbH),
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

    //==========================================================================
    // Utility: persist ANLZ data to disk from a TrackMetadata snapshot.
    // Called from the display-feed path so that detail waveform, beat grid,
    // cues and phrases are cached regardless of which source provided them
    // (dbserver phase 2, NFS async, or disk cache reload).
    //==========================================================================
    static void saveAnlzFromMeta(const TrackMetadata& meta)
    {
        if (meta.artist.isEmpty() || meta.title.isEmpty()) return;

        bool hasData = meta.hasDetailWaveform() || meta.hasBeatGrid()
                    || meta.hasCueList()        || meta.hasSongStructure();
        if (!hasData) return;

        auto diskKey = TrackMapEntry::makeKey(meta.artist, meta.title, meta.durationSeconds);
        if (diskKey.empty()) return;

        WaveformCache::CachedAnlz ca;

        // Start from existing disk data (if any) so we never lose components
        // that meta doesn't have yet.  For example: NFS wrote detail waveform,
        // then supplementaryFed fires with cues but no detail in meta yet --
        // without this merge, detail would be overwritten with 0 bytes.
        if (WaveformCache::anlzExists(diskKey))
        {
            auto existing = WaveformCache::loadAnlz(diskKey);
            if (existing.valid)
                ca = std::move(existing);
        }

        // Overwrite each component only when meta has equal or better data
        bool merged = false;

        if (!meta.beatGrid.empty()
            && meta.beatGrid.size() >= ca.beatGrid.size())
        {
            ca.beatGrid.clear();
            for (auto& b : meta.beatGrid)
                ca.beatGrid.push_back({ b.beatNumber, b.bpmTimes100, b.timeMs });
            merged = true;
        }

        if (!meta.cueList.empty()
            && meta.cueList.size() >= ca.cueList.size())
        {
            ca.cueList.clear();
            for (auto& c : meta.cueList)
            {
                WaveformCache::CachedAnlz::Cue cc;
                cc.type          = (uint8_t)c.type;
                cc.hotCueNumber  = c.hotCueNumber;
                cc.positionMs    = c.positionMs;
                cc.loopEndMs     = c.loopEndMs;
                cc.colorR        = c.colorR;
                cc.colorG        = c.colorG;
                cc.colorB        = c.colorB;
                cc.colorCode     = c.colorCode;
                cc.hasColor      = c.hasColor;
                cc.comment       = c.comment;
                ca.cueList.push_back(std::move(cc));
            }
            merged = true;
        }

        if (!meta.songStructure.empty()
            && meta.songStructure.size() >= ca.songStructure.size())
        {
            ca.songStructure.clear();
            for (auto& p : meta.songStructure)
                ca.songStructure.push_back({ p.index, p.beatNumber, p.kind, p.fill,
                                             p.beatCount, p.beatFill });
            ca.phraseMood = meta.phraseMood;
            merged = true;
        }

        if (meta.detailEntryCount > 0
            && (meta.detailBytesPerEntry > ca.detailBytesPerEntry
                || (meta.detailBytesPerEntry == ca.detailBytesPerEntry
                    && meta.detailEntryCount >= ca.detailEntryCount)))
        {
            ca.detailData          = meta.detailData;
            ca.detailEntryCount    = meta.detailEntryCount;
            ca.detailBytesPerEntry = meta.detailBytesPerEntry;
            merged = true;
        }

        if (!merged) return;  // disk already had equal or better data

        ca.valid = true;

        WaveformCache::saveAnlz(diskKey, ca);
        DBG("PDL saveAnlzFromMeta: saved beats=" + juce::String((int)ca.beatGrid.size())
            + " cues=" + juce::String((int)ca.cueList.size())
            + " phrases=" + juce::String((int)ca.songStructure.size())
            + " detail=" + juce::String(ca.detailEntryCount)
            + "x" + juce::String(ca.detailBytesPerEntry)
            + " key=" + juce::String(diskKey.substr(0, 40)));
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
        : DocumentWindow("PDL VIEW",
                          juce::Colour(0xFF0D0E12),
                          DocumentWindow::closeButton | DocumentWindow::maximiseButton)
    {
        setContentOwned(new ProDJLinkViewComponent(pdl, db, tmap, engines), true);
        setUsingNativeTitleBar(false);
        setTitleBarHeight(20);
        setColour(juce::DocumentWindow::textColourId, juce::Colour(0xFF546E7A));
        setResizable(true, true);
        if (auto* c = getConstrainer())
            c->setMinimumSize(400, 300);
        centreWithSize(getWidth(), getHeight());
        setVisible(true);
        toFront(true);

        // OpenGL context removed: see MainComponent constructor for rationale.
        // With timerCallback() writing juce::String members (artist, title,
        // playState, key...) on the message thread and paint() reading them on
        // the GL thread, the concurrent access corrupts String refcounts.
    }

    ~ProDJLinkViewWindow() override = default;

    void closeButtonPressed() override
    {
        // Save bounds before hiding
        if (onBoundsCapture) onBoundsCapture();
        if (auto* content = dynamic_cast<ProDJLinkViewComponent*>(getContentComponent()))
            content->stopTimer();
        setVisible(false);
    }

    std::function<void()> onBoundsCapture;

    void setOnTrackMapChanged(std::function<void()> cb)
    {
        if (auto* content = dynamic_cast<ProDJLinkViewComponent*>(getContentComponent()))
            content->onTrackMapChanged = std::move(cb);
    }

    void setOnLayoutChanged(std::function<void()> cb)
    {
        if (auto* content = dynamic_cast<ProDJLinkViewComponent*>(getContentComponent()))
            content->onLayoutChanged = std::move(cb);
    }

    void setIsShowLockedFn(std::function<bool()> cb)
    {
        if (auto* content = dynamic_cast<ProDJLinkViewComponent*>(getContentComponent()))
            content->isShowLockedFn = std::move(cb);
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
                // Check the centre point is on a valid display (guard against off-screen restore)
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
    void setLayoutState(bool horizontal, bool mixer)
    {
        if (auto* c = dynamic_cast<ProDJLinkViewComponent*>(getContentComponent()))
        {
            c->setLayoutHorizontal(horizontal);
            c->setShowMixer(mixer);
        }
    }

    bool getLayoutHorizontal() const
    {
        if (auto* c = dynamic_cast<const ProDJLinkViewComponent*>(getContentComponent()))
            return c->getLayoutHorizontal();
        return false;
    }

    bool getShowMixer() const
    {
        if (auto* c = dynamic_cast<const ProDJLinkViewComponent*>(getContentComponent()))
            return c->getShowMixer();
        return true;
    }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ProDJLinkViewWindow)
};
