// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

#pragma once
#include <JuceHeader.h>
#include "TimecodeCore.h"
#include "MtcInput.h"
#include "MtcOutput.h"
#include "ArtnetInput.h"
#include "ArtnetOutput.h"
#include "LtcInput.h"
#include "LtcOutput.h"
#include "ProDJLinkInput.h"
#include "StageLinQInput.h"
#include "HippotizerInput.h"
#include "HippotizerOutput.h"
#include "DbServerClient.h"
#include "TriggerOutput.h"
#include "LinkBridge.h"
#include "AudioThru.h"
#include "AudioBpmInput.h"
#include "AppSettings.h"
#include "MixerMap.h"
#include <memory>

//==============================================================================
// TimecodeEngine -- one independent routing pipeline
//
// Each engine owns: 1 input source -> N output destinations.
// AudioThru is only available on the primary engine (index 0).
//==============================================================================
// All public methods of TimecodeEngine are designed to be called exclusively
// from the JUCE message thread.  Protocol handler callbacks (MTC/ArtNet/LTC)
// communicate back via atomics only.  No additional synchronisation is needed.
inline constexpr int kPrimaryEngineIndex = 0;
inline constexpr int kMaxEngines = 8;

class TimecodeEngine
{
public:
    enum class InputSource { MTC, ArtNet, SystemTime, LTC, ProDJLink, StageLinQ, Hippotizer };

    //--------------------------------------------------------------------------
    explicit TimecodeEngine(int index, const juce::String& name = {})
        : engineIndex(index),
          engineName(name.isEmpty() ? ("ENGINE " + juce::String(index + 1)) : name)
    {
        std::fill(std::begin(lastSentMixer), std::end(lastSentMixer), -1); lastMixerPktCount = 0;

        // Only the primary engine (index 0) gets AudioThru
        if (index == kPrimaryEngineIndex)
            audioThru = std::make_unique<AudioThru>();
    }

    ~TimecodeEngine()
    {
        // Stop MIDI clock timer first (runs on HighResolutionTimer thread)
        setMidiClockEnabled(false);
        triggerOutput.stopMidi();
        triggerOutput.disconnectOsc();

        // Shutdown order: outputs first, then inputs
        stopMtcOutput();
        stopArtnetOutput();
        stopLtcOutput();
        stopHippotizerOutput();
        stopThruOutput();
        stopMtcInput();
        stopArtnetInput();
        stopLtcInput();
        stopHippotizerInput();
        stopAudioBpm();
        // ProDJLink is shared -- not stopped per-engine
        // StageLinQ is shared -- not stopped per-engine
    }

    //==========================================================================
    // Identity
    //==========================================================================
    int getIndex() const { return engineIndex; }
    juce::String getName() const { return engineName; }
    void setName(const juce::String& name) { engineName = name; }
    bool isPrimary() const { return engineIndex == kPrimaryEngineIndex; }

    // Called after engine deletion to fix indices so isPrimary() stays correct
    // and AudioThru is created for the new primary engine if needed.
    // NOTE: The caller (MainComponent::removeEngine) is responsible for
    // restarting AudioThru on the new primary engine after reindexing,
    // because that requires UI state (device combos) that the engine doesn't own.
    void reindex(int newIndex)
    {
        // If we were the primary engine and are being moved away, destroy AudioThru
        // to avoid a stale handler referencing a deleted LtcInput.
        if (engineIndex == kPrimaryEngineIndex && newIndex != kPrimaryEngineIndex)
        {
            stopThruOutput();
            audioThru.reset();
            outputThruEnabled = false;
        }

        engineIndex = newIndex;

        // Create AudioThru if we just became the primary engine
        if (newIndex == kPrimaryEngineIndex && !audioThru)
            audioThru = std::make_unique<AudioThru>();
    }

    //==========================================================================
    // Input source
    //==========================================================================
    InputSource getActiveInput() const { return activeInput; }
    FrameRate getCurrentFps() const { return currentFps; }
    Timecode getCurrentTimecode() const
    {
        const juce::SpinLock::ScopedLockType sl(timecodeLock);
        return currentTimecode;
    }
    bool isSourceActive() const { return sourceActive; }
    bool getUserOverrodeLtcFps() const { return userOverrodeLtcFps; }

    void setInputSource(InputSource source)
    {
        // Stop current input
        switch (activeInput)
        {
            case InputSource::MTC:    stopMtcInput();    break;
            case InputSource::ArtNet: stopArtnetInput(); break;
            case InputSource::LTC:    stopLtcInput();    break;
            case InputSource::ProDJLink: stopProDJLinkInput(); break;
            case InputSource::StageLinQ: stopStageLinQInput(); break;
            case InputSource::Hippotizer: stopHippotizerInput(); break;
            default: break;
        }

        userOverrodeLtcFps = false;
        activeInput = source;
        sourceActive = false;

        // Reset TrackMap cache when leaving ProDJLink / StageLinQ
        if (source != InputSource::ProDJLink && source != InputSource::StageLinQ)
        {
            trackMapped = false;
            cachedTrackId = 0;
            cachedOffH = cachedOffM = cachedOffS = cachedOffF = 0;
            cachedBpmMultiplier = 0;
            bpmPlayerOverride = kBpmNoOverride;
            cachedTrackArtist.clear();
            cachedTrackTitle.clear();
            cachedTrackDurationSec = 0;
            // Disable Link to avoid publishing stale tempo on the network
            // (unless audio BPM is active -- it will keep feeding Link)
            if (!audioBpmEnabled)
                linkBridge.setEnabled(false);
        }

        // Note: actual start is deferred to the caller (MainComponent),
        // which gathers device params from UI before calling startXxxInput().
        if (source == InputSource::SystemTime)
        {
            sourceActive = genClockMode;  // clock mode: always active; transport mode: wait for Play
            genLastTickTime = juce::Time::getMillisecondCounterHiRes();  // prevent time jump
        }
    }

    void setFrameRate(FrameRate fps)
    {
        currentFps = fps;
        FrameRate outRate = getEffectiveOutputFps();
        mtcOutput.setFrameRate(outRate);
        artnetOutput.setFrameRate(outRate);
        ltcOutput.setFrameRate(outRate);
        hippotizerOutput.setFrameRate(outRate);
    }

    void setUserOverrodeLtcFps(bool v) { userOverrodeLtcFps = v; }

    //==========================================================================
    // FPS conversion
    //==========================================================================
    bool isFpsConvertEnabled() const { return fpsConvertEnabled; }
    FrameRate getOutputFps() const { return outputFps; }
    Timecode getOutputTimecode() const { return outputTimecode; }

    /// Playhead in ms from CDJ/Denon (for UI cursor / position display).
    /// Reads directly from ProDJLinkInput or StageLinQInput.
    uint32_t getSmoothedPlayheadMs() const
    {
        if (activeInput == InputSource::StageLinQ && sharedStageLinQ != nullptr)
        {
            int ep = getEffectivePlayer();
            return (ep >= 1) ? sharedStageLinQ->getPlayheadMs(ep) : 0;
        }
        if (activeInput != InputSource::ProDJLink) return 0;  // non-DJ sources use tcToMs in TCNet
        if (sharedProDJLink == nullptr) return 0;
        int ep = getEffectivePlayer();
        if (ep < 1) return 0;
        return sharedProDJLink->getPlayheadMs(ep);
    }

    /// Play position as 0.0-1.0 ratio (for waveform cursor).
    float getSmoothedPlayPositionRatio() const
    {
        if (activeInput == InputSource::StageLinQ && sharedStageLinQ != nullptr)
        {
            int ep = getEffectivePlayer();
            return (ep >= 1) ? sharedStageLinQ->getPlayPositionRatio(ep) : 0.0f;
        }
        if (activeInput != InputSource::ProDJLink) return 0.0f;
        if (sharedProDJLink == nullptr) return 0.0f;
        int ep = getEffectivePlayer();
        if (ep < 1) return 0.0f;
        return sharedProDJLink->getPlayPositionRatio(ep);
    }

    FrameRate getEffectiveOutputFps() const
    {
        return fpsConvertEnabled ? outputFps : currentFps;
    }

    void setFpsConvertEnabled(bool enabled)
    {
        fpsConvertEnabled = enabled;
        if (!enabled)
        {
            outputFps = currentFps;
            setOutputFrameRate(currentFps);
        }
    }

    void setOutputFrameRate(FrameRate fps)
    {
        outputFps = fps;
        // ProDJLink has no inherent frame rate (CDJ sends ms, not frames).
        // The user's fps choice IS the current fps.
        if (activeInput == InputSource::ProDJLink)
            currentFps = fps;
        FrameRate outRate = getEffectiveOutputFps();
        mtcOutput.setFrameRate(outRate);
        artnetOutput.setFrameRate(outRate);
        ltcOutput.setFrameRate(outRate);
        hippotizerOutput.setFrameRate(outRate);
    }

    //==========================================================================
    // Output enables & offsets
    //==========================================================================
    bool isOutputMtcEnabled() const     { return outputMtcEnabled; }
    bool isOutputArtnetEnabled() const  { return outputArtnetEnabled; }
    bool isOutputLtcEnabled() const     { return outputLtcEnabled; }
    bool isOutputThruEnabled() const    { return outputThruEnabled; }

    void setOutputMtcEnabled(bool e)    { outputMtcEnabled = e; }
    void setOutputArtnetEnabled(bool e) { outputArtnetEnabled = e; }
    void setOutputLtcEnabled(bool e)    { outputLtcEnabled = e; }
    void setOutputThruEnabled(bool e)   { outputThruEnabled = e; }
    void setOutputTcnetEnabled(bool e)  { outputTcnetEnabled = e; }
    bool isOutputTcnetEnabled() const   { return outputTcnetEnabled; }
    void setOutputHippoEnabled(bool e)  { outputHippoEnabled = e; }
    bool isOutputHippoEnabled() const   { return outputHippoEnabled; }
    void setTcnetLayer(int l)           { tcnetLayer = juce::jlimit(0, 3, l); }
    int  getTcnetLayer() const          { return tcnetLayer; }

    int getMtcOutputOffset() const      { return mtcOutputOffset; }
    int getArtnetOutputOffset() const   { return artnetOutputOffset; }
    int getLtcOutputOffset() const      { return ltcOutputOffset; }

    void setMtcOutputOffset(int v)      { mtcOutputOffset = v; }
    void setArtnetOutputOffset(int v)   { artnetOutputOffset = v; }
    void setLtcOutputOffset(int v)      { ltcOutputOffset = v; }
    void setTcnetOutputOffsetMs(int v)  { tcnetOutputOffsetMs = juce::jlimit(-1000, 1000, v); }

    int getTcnetOutputOffsetMs() const  { return tcnetOutputOffsetMs; }

    // On-air gate (requires CDJ on-air flag from DJM for engine to be active)
    bool isOnAirGateEnabled() const     { return onAirGateEnabled; }
    void setOnAirGateEnabled(bool e)    { onAirGateEnabled = e; }

    /// Returns true if the on-air gate allows the engine to be active.
    /// If the gate is disabled, always returns true. Otherwise consults the
    /// CDJ's on-air flag -- this reflects the DJM mixer's full gating logic
    /// (channel fader + cross-fader + EQ kill + mute), computed by the mixer
    /// itself and transmitted to the CDJs. The DJ sees the same "ON AIR"
    /// indicator light on the CDJ display.
    bool isOnAirGateOpen() const
    {
        if (!onAirGateEnabled) return true;
        if (activeInput != InputSource::ProDJLink) return true;
        if (sharedProDJLink == nullptr) return true;
        // If no DJM is active on the network, the on-air flag is meaningless
        // (CDJs default to false without a DJM broadcast). Keep the gate open
        // so the engine isn't silenced just because the DJ unplugged the mixer.
        if (!sharedProDJLink->hasMixerFaderData()) return true;
        int ep = getEffectivePlayer();
        // If XF mode hasn't resolved yet (ep == 0) or is out of range,
        // don't block the engine.
        if (ep < 1 || ep > 4) return true;
        return sharedProDJLink->isPlayerOnAir(ep);
    }

    //==========================================================================
    // Protocol handlers -- direct access for device queries
    //==========================================================================
    MtcInput&     getMtcInput()     { return mtcInput; }
    MtcOutput&    getMtcOutput()    { return mtcOutput; }
    ArtnetInput&  getArtnetInput()  { return artnetInput; }
    ArtnetOutput& getArtnetOutput() { return artnetOutput; }
    LtcInput&     getLtcInput()     { return ltcInput; }
    LtcOutput&    getLtcOutput()    { return ltcOutput; }
    ProDJLinkInput& getProDJLinkInput()   { jassert(sharedProDJLink != nullptr); return *sharedProDJLink; }
    void setSharedProDJLinkInput(ProDJLinkInput* shared) { sharedProDJLink = shared; }
    StageLinQInput& getStageLinQInput()   { jassert(sharedStageLinQ != nullptr); return *sharedStageLinQ; }
    void setSharedStageLinQInput(StageLinQInput* shared) { sharedStageLinQ = shared; }
    void setDbServerClient(DbServerClient* client) { dbClient = client; }
    int  getProDJLinkPlayer() const     { return proDJLinkPlayer; }
    /// Returns the physical player (1-6) actually being followed.
    /// In fixed mode: same as proDJLinkPlayer. In XF mode: the resolved player.
    int  getEffectivePlayer() const
    {
        return isXfMode() ? resolvedXfPlayer : proDJLinkPlayer;
    }
    void setProDJLinkPlayer(int p)
    {
        proDJLinkPlayer = juce::jlimit(1, kPlayerXfB, p);
        resolvedXfPlayer = 0;  // force re-resolve on next tick
        resetProDJLinkCache();
        bpmPlayerOverride = kBpmNoOverride;
        lastSentClockBpm = -1.0f;
        lastSentOscBpm   = -1.0f;
    }

    AudioThru*    getAudioThru()    { return audioThru.get(); }

    //==========================================================================
    // Start / Stop input protocols
    //==========================================================================
    bool startMtcInput(int deviceIndex)
    {
        stopMtcInput();
        mtcInput.refreshDeviceList();
        if (deviceIndex < 0 && mtcInput.getDeviceCount() > 0) deviceIndex = 0;
        if (deviceIndex >= 0 && mtcInput.start(deviceIndex))
        {
            inputStatusText = "RX: " + mtcInput.getCurrentDeviceName();
            return true;
        }
        inputStatusText = (deviceIndex < 0) ? "NO MIDI DEVICE AVAILABLE" : "FAILED TO OPEN DEVICE";
        return false;
    }

    void stopMtcInput() { mtcInput.stop(); }

    bool startArtnetInput(int interfaceIndex)
    {
        stopArtnetInput();
        if (interfaceIndex < 0) interfaceIndex = 0;
        artnetInput.refreshNetworkInterfaces();
        if (artnetInput.start(interfaceIndex, 6454))
        {
            inputStatusText = "RX ON " + artnetInput.getBindInfo();
            if (artnetInput.didFallBackToAllInterfaces())
                inputStatusText += " [FALLBACK]";
            return true;
        }
        inputStatusText = "FAILED TO BIND PORT 6454";
        return false;
    }

    void stopArtnetInput() { artnetInput.stop(); }

    bool startLtcInput(const juce::String& typeName, const juce::String& devName,
                       int ltcChannel, int thruChannel = -1,
                       double sampleRate = 0, int bufferSize = 0)
    {
        stopLtcInput();
        if (devName.isEmpty()) { inputStatusText = "NO AUDIO DEVICE AVAILABLE"; return false; }

        if (ltcInput.start(typeName, devName, ltcChannel, thruChannel, sampleRate, bufferSize))
        {
            inputStatusText = "RX: " + ltcInput.getCurrentDeviceName()
                            + " Ch " + juce::String(ltcChannel + 1);
            return true;
        }
        inputStatusText = "FAILED TO OPEN AUDIO DEVICE";
        return false;
    }

    void stopLtcInput()
    {
        stopThruOutput();
        ltcInput.stop();
    }

    //==========================================================================
    // Audio BPM detection (for non-DJ input sources)
    //==========================================================================
    bool startAudioBpm(const juce::String& typeName, const juce::String& devName,
                       int channel, double sampleRate = 0, int bufferSize = 0)
    {
        stopAudioBpm();
        if (devName.isEmpty()) return false;
        if (audioBpmInput.start(typeName, devName, channel, sampleRate, bufferSize))
        {
            audioBpmEnabled = true;
            return true;
        }
        return false;
    }

    void stopAudioBpm()
    {
        audioBpmEnabled = false;
        audioBpmInput.stop();
        lastSentClockBpm = -1.0f;
        lastSentOscBpm   = -1.0f;
    }

    bool isAudioBpmRunning() const { return audioBpmEnabled && audioBpmInput.getIsRunning(); }
    double getAudioBpm() const { return audioBpmInput.getBpm(); }
    double getAudioBpmConfidence() const { return audioBpmInput.getConfidence(); }
    bool hasAudioBpm() const { return isAudioBpmRunning() && audioBpmInput.hasBpm(); }
    float getAudioBpmPeakLevel() const { return audioBpmInput.getPeakLevel(); }

    AudioBpmInput& getAudioBpmInput() { return audioBpmInput; }
    const AudioBpmInput& getAudioBpmInput() const { return audioBpmInput; }

    //==========================================================================
    // Pro DJ Link input (shared across engines)
    //==========================================================================
    void resetProDJLinkCache()
    {
        trackMapped = false;
        cachedTrackId = 0;
        lastSeenTrackVersion = 0;
        cachedOffH = cachedOffM = cachedOffS = cachedOffF = 0;
        cachedTrackArtist.clear();
        cachedTrackTitle.clear();
        cachedTrackDurationSec = 0;
        armedCues.clear();
        lastCueCheckMs = 0;
        pll.reset(); clearBeatGrid(); pdlTcFrozen = false; pdlLastPlayheadMs = 0; pdlLastAbsPosTs = 0.0;
        pdlSnapMs = 0.0; pdlSnapTime = 0.0; pdlSnapSpeed = 1.0;
        ltcOutput.setPitchMultiplier(1.0);
        std::memset(trigDmxBuffer, 0, sizeof(trigDmxBuffer));
        trigDmxHighWater = 0;
    }

    bool startProDJLinkInput(int player = 1)
    {
        resetProDJLinkCache();

        // Reset forward dedup
        lastSentClockBpm = -1.0f;
        lastSentOscBpm = -1.0f;

        // NOTE: do NOT overwrite currentFps here. The user's fps choice is
        // already set via setFrameRate() from settings load or the FPS buttons.
        // Previously this was "currentFps = outputFps" which would overwrite
        // a user-selected 25fps with outputFps=30 (the default) when loading
        // settings, because startProDJLinkInput runs after the initial load.

        // LTC direct mode is now toggled dynamically per tick
        // (direct in transient, auto-increment in stable)

        proDJLinkPlayer = juce::jlimit(1, kPlayerXfB, player);
        resolvedXfPlayer = 0;  // force resolve on first tick

        // NOTE: lastSeenTrackVersion stays at 0 (set by resetProDJLinkCache).
        // This forces the first tick to detect the current track as "changed"
        // and re-run metadata request + TrackMap lookup + cue point loading.
        // Without this, switching away from ProDJLink and back would leave
        // cachedTrackId=0 permanently because lastSeenTrackVersion already
        // matched the CDJ's current value, so "track changed" never fired.

        if (sharedProDJLink != nullptr && sharedProDJLink->getIsRunning())
        {
            inputStatusText = "LISTENING | " + sharedProDJLink->getBindInfo();
            return true;
        }
        inputStatusText = "WAITING FOR PRO DJ LINK...";
        return sharedProDJLink != nullptr;
    }

    void stopProDJLinkInput()
    {
        // Reset PLL and LTC encoder state so other sources start clean.
        pll.reset(); clearBeatGrid(); pdlTcFrozen = false; pdlLastPlayheadMs = 0; pdlLastAbsPosTs = 0.0;
        pdlSnapMs = 0.0; pdlSnapTime = 0.0; pdlSnapSpeed = 1.0;
        ltcOutput.setPitchMultiplier(1.0);
    }

    //==========================================================================
    // StageLinQ input (shared across engines)
    //==========================================================================
    bool startStageLinQInput(int player = 1)
    {
        resetProDJLinkCache();

        // Reset forward dedup
        lastSentClockBpm = -1.0f;
        lastSentOscBpm = -1.0f;
        std::fill(std::begin(lastSentSlqMixer), std::end(lastSentSlqMixer), -1);

        // Reset DMX buffer to avoid broadcasting stale Pioneer data
        std::memset(dmxBuffer, 0, sizeof(dmxBuffer));
        dmxHighWaterMark = 0;
        lastDmxSendTime = 0.0;

        // NOTE: do NOT overwrite currentFps here (see startProDJLinkInput).

        proDJLinkPlayer = juce::jlimit(1, kPlayerXfB, player);
        resolvedXfPlayer = 0;

        // NOTE: lastSeenTrackVersion stays at 0 (set by resetProDJLinkCache).
        // Same rationale as startProDJLinkInput -- forces track rediscovery
        // on the first tick after switching back to StageLinQ.

        if (sharedStageLinQ != nullptr && sharedStageLinQ->getIsRunning())
        {
            inputStatusText = "LISTENING | " + sharedStageLinQ->getBindInfo();
            return true;
        }
        inputStatusText = "WAITING FOR STAGELINQ...";
        return sharedStageLinQ != nullptr;
    }

    void stopStageLinQInput()
    {
        // Reset PLL and LTC encoder state so other sources start clean.
        pll.reset(); clearBeatGrid(); pdlTcFrozen = false; pdlLastPlayheadMs = 0; pdlLastAbsPosTs = 0.0;
        pdlSnapMs = 0.0; pdlSnapTime = 0.0; pdlSnapSpeed = 1.0;
        ltcOutput.setPitchMultiplier(1.0);
    }

    //==========================================================================
    // Hippotizer input (per-engine)
    //==========================================================================
    HippotizerInput& getHippotizerInput() { return hippotizerInput; }
    HippotizerOutput& getHippotizerOutput() { return hippotizerOutput; }

    bool startHippotizerInput(int interfaceIndex = 0, int port = 6091)
    {
        stopHippotizerInput();
        hippotizerInput.refreshNetworkInterfaces();
        if (interfaceIndex < 0) interfaceIndex = 0;
        if (hippotizerInput.start(interfaceIndex, port))
        {
            inputStatusText = "RX ON " + hippotizerInput.getBindInfo();
            if (hippotizerInput.didFallBackToAllInterfaces())
                inputStatusText += " [FALLBACK]";
            return true;
        }
        inputStatusText = "FAILED TO BIND PORT " + juce::String(port);
        return false;
    }

    void stopHippotizerInput() { hippotizerInput.stop(); }

    //==========================================================================
    // TrackMap -- track-to-timecode-offset mapping
    //==========================================================================

    /// Set the TrackMap pointer (owned by AppSettings, not by the engine).
    /// Must be called before enabling track mapping. Pass nullptr to disconnect.
    void setTrackMap(TrackMap* map)
    {
        trackMapPtr = map;
        if (map == nullptr)
        {
            // Clear pending flags to avoid stale operations on a null pointer
            trackMapDirty = false;
            trackMapAutoFilled = false;
        }
    }

    void setMixerMap(MixerMap* map)
    {
        mixerMapPtr = map;
        std::fill(std::begin(lastSentMixer), std::end(lastSentMixer), -1); lastMixerPktCount = 0;
    }

    void setSlqMixerMap(MixerMap* map)
    {
        slqMixerMapPtr = map;
        std::fill(std::begin(lastSentSlqMixer), std::end(lastSentSlqMixer), -1);
    }

    /// Enable or disable TrackMap offset application.
    /// Triggers fire independently of this setting.
    void setTrackMapEnabled(bool enabled)
    {
        trackMapEnabled = enabled;
        if (!enabled)
        {
            // Reset offset state only -- keep cachedTrackId for trigger use
            trackMapped = false;
            cachedOffH = cachedOffM = cachedOffS = cachedOffF = 0;
            cachedBpmMultiplier = 0;
            lastSentClockBpm = -1.0f;
            lastSentOscBpm   = -1.0f;
        }
        else if (trackMapPtr && activeInput == InputSource::ProDJLink
                 && sharedProDJLink != nullptr && sharedProDJLink->getIsRunning())
        {
            lastSeenTrackVersion = sharedProDJLink->getTrackVersion(getEffectivePlayer());

            uint32_t id = (cachedTrackId != 0) ? cachedTrackId : sharedProDJLink->getTrackID(getEffectivePlayer());
            if (id != 0)
            {
                cachedTrackId = id;
                auto tinfo = sharedProDJLink->getTrackInfo(getEffectivePlayer());
                cachedTrackArtist = tinfo.artist;
                cachedTrackTitle  = tinfo.title;
                cachedTrackDurationSec = (int)sharedProDJLink->getTrackLengthSec(getEffectivePlayer());

                // Phase 2: check dbClient cache or request if missing
                if (dbClient != nullptr && dbClient->getIsRunning())
                {
                    auto meta = dbClient->getCachedMetadataLightById(id);
                    if (meta.isValid())
                    {
                        if (meta.artist.isNotEmpty()) cachedTrackArtist = meta.artist;
                        if (meta.title.isNotEmpty())  cachedTrackTitle  = meta.title;
                        if (meta.durationSeconds > 0)
                        {
                            cachedTrackDurationSec = meta.durationSeconds;
                            if (sharedProDJLink != nullptr)
                                sharedProDJLink->setTrackLengthSec(getEffectivePlayer(),
                                    (uint32_t)meta.durationSeconds);
                        }
                    }
                    else
                    {
                        requestDbMetadata(id);
                    }
                }

                lookupTrackInMap();
            }
        }
    }

    bool isTrackMapEnabled() const { return trackMapEnabled; }

    /// Whether the currently playing track has a mapping in the TrackMap
    bool isTrackMapped() const { return trackMapped; }

    /// Get the Track ID currently being tracked (0 if none)
    uint32_t getActiveTrackId() const { return cachedTrackId; }

    /// Get the cached TrackMap entry info for the current track (for UI display).
    /// Returns empty strings and zero offset if no track is mapped.
    struct ActiveTrackInfo
    {
        uint32_t trackId = 0;
        juce::String artist;
        juce::String title;
        juce::String offset = "00:00:00:00";
        bool mapped = false;
        // Phase 2 -- extended metadata from dbserver (empty if not yet resolved)
        juce::String album;
        juce::String genre;
        juce::String key;
        int bpmTimes100 = 0;
        uint32_t artworkId = 0;
        int durationSec = 0;
    };

    ActiveTrackInfo getActiveTrackInfo() const
    {
        ActiveTrackInfo info;
        info.trackId = cachedTrackId;
        info.artist  = cachedTrackArtist;
        info.title   = cachedTrackTitle;
        info.durationSec = cachedTrackDurationSec;
        info.mapped  = trackMapped;
        if (trackMapped)
            info.offset = TrackMapEntry::formatTimecodeString(
                              cachedOffH, cachedOffM, cachedOffS, cachedOffF);

        // Phase 2: enrich with dbClient cache if available.
        // Uses the lightweight lookup to avoid copying waveform data (~3600B)
        // every frame at 60Hz.
        if (dbClient != nullptr && cachedTrackId != 0)
        {
            auto meta = dbClient->getCachedMetadataLightById(cachedTrackId);
            if (meta.isValid())
            {
                info.album      = meta.album;
                info.genre      = meta.genre;
                info.key        = meta.key;
                info.bpmTimes100 = meta.bpmTimes100;
                info.artworkId  = meta.artworkId;
                // Ensure artist/title are from dbClient if available
                if (info.artist.isEmpty() || info.title.startsWith("Track #"))
                {
                    if (meta.artist.isNotEmpty()) info.artist = meta.artist;
                    if (meta.title.isNotEmpty())  info.title  = meta.title;
                }
            }
        }
        return info;
    }

    /// Info about the next upcoming (unfired) cue point for UI display.
    struct NextCueInfo
    {
        bool     valid = false;         // true if a next cue exists
        juce::String name;             // cue name ("DROP", "LIGHTS ON", etc.)
        uint32_t positionMs = 0;       // absolute position in track (ms)
        int32_t  remainingMs = 0;      // ms until the cue fires (negative = overdue)
    };

    /// Returns info about the next unfired cue point relative to the current playhead.
    /// Called from UI thread (timerCallback) for countdown display.
    NextCueInfo getNextCueInfo() const
    {
        NextCueInfo info;
        if (armedCues.empty()) return info;

        uint32_t playhead = lastCueCheckMs;  // last known playhead position

        // armedCues is sorted by positionMs -- find the first unfired cue
        for (const auto& ac : armedCues)
        {
            if (!ac.fired)
            {
                info.valid       = true;
                info.name        = ac.cue.name;
                info.positionMs  = ac.cue.positionMs;
                info.remainingMs = (int32_t)ac.cue.positionMs - (int32_t)playhead;
                return info;
            }
        }
        return info;  // all cues fired
    }

    /// Force a re-lookup of the current track (e.g., after editing TrackMap)
    void refreshTrackMapLookup()
    {
        if (!trackMapPtr || cachedTrackTitle.isEmpty()) return;
        const auto* entry = lookupTrackInMap();
        // Reload cue points (user may have added/edited/deleted cues).
        // Preserve playhead position so cues behind the current playhead
        // are marked as already fired (don't re-trigger on edit).
        uint32_t savedPlayhead = lastCueCheckMs;
        loadCuePointsForTrack(entry);
        if (savedPlayhead > 0)
        {
            for (auto& ac : armedCues)
                ac.fired = (ac.cue.positionMs < savedPlayhead);
            lastCueCheckMs = savedPlayhead;
        }
    }

    /// Re-request metadata for the current track (used when waveform data
    /// hasn't arrived yet but metadata is already cached -- triggers the
    /// waveform-only retry path in DbServerClient).
    void retryWaveformRequest()
    {
        if (cachedTrackId != 0)
            requestDbMetadata(cachedTrackId);
    }

    /// Returns true (once) if auto-fill modified the TrackMap and it was saved.
    /// Used by MainComponent to refresh the TrackMapEditor window if open.
    bool consumeTrackMapAutoFilled()
    {
        if (trackMapAutoFilled) { trackMapAutoFilled = false; return true; }
        return false;
    }

    //==========================================================================
    // Track change triggers (MIDI / OSC)
    //==========================================================================
    TriggerOutput& getTriggerOutput() { return triggerOutput; }

    //==========================================================================
    // Start / Stop output protocols
    //==========================================================================
    bool startMtcOutput(int deviceIndex)
    {
        stopMtcOutput();
        mtcOutput.refreshDeviceList();
        if (deviceIndex < 0 && mtcOutput.getDeviceCount() > 0) deviceIndex = 0;
        if (deviceIndex >= 0 && mtcOutput.start(deviceIndex))
        {
            mtcOutput.setFrameRate(getEffectiveOutputFps());
            mtcOutStatusText = "TX: " + mtcOutput.getCurrentDeviceName();
            return true;
        }
        mtcOutStatusText = (deviceIndex < 0) ? "NO MIDI DEVICE" : "FAILED TO OPEN";
        return false;
    }

    void stopMtcOutput() { mtcOutput.stop(); mtcOutStatusText = ""; }

    bool startArtnetOutput(int interfaceIndex)
    {
        stopArtnetOutput();
        artnetOutput.refreshNetworkInterfaces();
        if (artnetOutput.start(interfaceIndex, 6454))
        {
            artnetOutput.setFrameRate(getEffectiveOutputFps());
            artnetOutStatusText = "TX: " + artnetOutput.getBroadcastIp() + ":6454";
            return true;
        }
        artnetOutStatusText = "FAILED TO BIND";
        return false;
    }

    void stopArtnetOutput() { artnetOutput.stop(); artnetOutStatusText = ""; }

    bool startLtcOutput(const juce::String& typeName, const juce::String& devName,
                        int channel, double sampleRate = 0, int bufferSize = 0)
    {
        stopLtcOutput();
        if (devName.isEmpty()) { ltcOutStatusText = "NO AUDIO DEVICE AVAILABLE"; return false; }

        // Check for AudioThru device conflict (primary engine only)
        if (audioThru && audioThru->getIsRunning()
            && audioThru->getCurrentDeviceName() == devName
            && audioThru->getCurrentTypeName() == typeName)
        {
            stopThruOutput();
            thruOutStatusText = "CONFLICT: same device as LTC OUT";
        }

        if (ltcOutput.start(typeName, devName, channel, sampleRate, bufferSize))
        {
            ltcOutput.setFrameRate(getEffectiveOutputFps());
            juce::String chName = (channel == -1) ? "Ch 1 + Ch 2" : ("Ch " + juce::String(channel + 1));
            ltcOutStatusText = "TX: " + ltcOutput.getCurrentDeviceName() + " " + chName;
            return true;
        }
        ltcOutStatusText = "FAILED TO OPEN AUDIO DEVICE";
        return false;
    }

    void stopLtcOutput() { ltcOutput.stop(); ltcOutStatusText = ""; }

    bool startHippotizerOutput(const juce::String& targetIp, int interfaceIndex = -1)
    {
        stopHippotizerOutput();
        hippotizerOutput.refreshNetworkInterfaces();
        if (hippotizerOutput.start(targetIp, interfaceIndex, 6091))
        {
            hippotizerOutput.setFrameRate(getEffectiveOutputFps());
            hippoOutStatusText = "TX: " + hippotizerOutput.getDestination();
            return true;
        }
        hippoOutStatusText = "FAILED TO BIND";
        return false;
    }

    void stopHippotizerOutput() { hippotizerOutput.stop(); hippoOutStatusText = ""; }

    bool startThruOutput(const juce::String& typeName, const juce::String& devName,
                         int channel, double sampleRate = 0, int bufferSize = 0)
    {
        stopThruOutput();
        if (!audioThru) return false;  // not primary engine
        if (!ltcInput.getIsRunning() || !ltcInput.hasPassthruChannel())
        {
            thruOutStatusText = "WAITING FOR LTC INPUT";
            return false;
        }

        ltcInput.resetPassthruCounters();
        ltcInput.syncPassthruReadPosition();

        if (devName.isEmpty()) { thruOutStatusText = "NO AUDIO DEVICE"; return false; }

        // Check for LTC output device conflict
        if (outputLtcEnabled && ltcOutput.getIsRunning()
            && ltcOutput.getCurrentDeviceName() == devName
            && ltcOutput.getCurrentTypeName() == typeName)
        {
            thruOutStatusText = "CONFLICT: same device as LTC OUT";
            return false;
        }

        if (audioThru->start(typeName, devName, channel, &ltcInput, sampleRate, bufferSize))
        {
            juce::String chName = (channel == -1) ? "Ch 1 + Ch 2" : ("Ch " + juce::String(channel + 1));
            thruOutStatusText = "THRU: " + audioThru->getCurrentDeviceName() + " " + chName;

            double inRate  = ltcInput.getActualSampleRate();
            double outRate = audioThru->getActualSampleRate();
            if (std::abs(inRate - outRate) > 1.0)
                thruOutStatusText += " [RATE MISMATCH: " + juce::String((int)inRate) + "/" + juce::String((int)outRate) + "]";

            return true;
        }
        thruOutStatusText = "FAILED TO OPEN";
        return false;
    }

    void stopThruOutput()
    {
        if (audioThru) audioThru->stop();
        thruOutStatusText = "";
    }

    //==========================================================================
    // tick() -- called from timerCallback each frame
    //==========================================================================
    void tick()
    {
        // Housekeeping: safely destroy MidiInput devices that were retired
        // by MtcInput::stop().  See MtcInput.h for why this is deferred.
        mtcInput.drainRetiredDevices();

        switch (activeInput)
        {
            case InputSource::SystemTime:
                updateGenerator();
                if (genClockMode)
                {
                    sourceActive = true;
                    if (statusTextVisible)
                        inputStatusText = "SYSTEM CLOCK";
                }
                else
                {
                    sourceActive = (genState == GeneratorState::Playing);
                    if (statusTextVisible)
                    {
                        switch (genState)
                        {
                            case GeneratorState::Playing: inputStatusText = "PLAYING"; break;
                            case GeneratorState::Paused:  inputStatusText = "PAUSED"; break;
                            case GeneratorState::Stopped: inputStatusText = "STOPPED"; break;
                        }
                    }
                }
                break;

            case InputSource::MTC:
                if (mtcInput.getIsRunning())
                {
                    currentTimecode = mtcInput.getCurrentTimecode();
                    bool rx = mtcInput.isReceiving();
                    if (rx)
                    {
                        auto d = mtcInput.getDetectedFrameRate();
                        if (d != currentFps) setFrameRate(d);
                        if (statusTextVisible)
                            inputStatusText = "RX: " + mtcInput.getCurrentDeviceName();
                    }
                    else if (statusTextVisible)
                        inputStatusText = "PAUSED - " + mtcInput.getCurrentDeviceName();
                    sourceActive = rx;
                }
                else { sourceActive = false; if (statusTextVisible) inputStatusText = "WAITING FOR DEVICE..."; }
                break;

            case InputSource::ArtNet:
                if (artnetInput.getIsRunning())
                {
                    currentTimecode = artnetInput.getCurrentTimecode();
                    bool rx = artnetInput.isReceiving();
                    if (rx)
                    {
                        auto d = artnetInput.getDetectedFrameRate();
                        if (d != currentFps) setFrameRate(d);
                        if (statusTextVisible)
                            inputStatusText = "RX ON " + artnetInput.getBindInfo();
                    }
                    else if (statusTextVisible)
                        inputStatusText = "PAUSED - " + artnetInput.getBindInfo();
                    sourceActive = rx;
                }
                else { sourceActive = false; if (statusTextVisible) inputStatusText = "NOT LISTENING"; }
                break;

            case InputSource::LTC:
                if (ltcInput.getIsRunning())
                {
                    currentTimecode = ltcInput.getCurrentTimecode();
                    bool rx = ltcInput.isReceiving();
                    if (rx)
                    {
                        auto d = ltcInput.getDetectedFrameRate();
                        bool ambiguousOverride = userOverrodeLtcFps
                            && ((currentFps == FrameRate::FPS_2398 && d == FrameRate::FPS_24)
                             || (currentFps == FrameRate::FPS_2997 && d == FrameRate::FPS_30));
                        if (d != currentFps && !ambiguousOverride)
                        {
                            if (d != FrameRate::FPS_24 && d != FrameRate::FPS_30)
                                userOverrodeLtcFps = false;
                            setFrameRate(d);
                        }
                        if (statusTextVisible)
                            inputStatusText = "RX: " + ltcInput.getCurrentDeviceName()
                                            + " Ch " + juce::String(ltcInput.getSelectedChannel() + 1);
                    }
                    else if (statusTextVisible)
                        inputStatusText = "PAUSED - " + ltcInput.getCurrentDeviceName();
                    sourceActive = rx;
                }
                else { sourceActive = false; if (statusTextVisible) inputStatusText = "WAITING FOR DEVICE..."; }
                break;

            case InputSource::ProDJLink:
                if (sharedProDJLink != nullptr && sharedProDJLink->getIsRunning())
                {
                    // --- XF-A/XF-B auto-follow: resolve physical player ---
                    if (isXfMode())
                        resolveXfPlayer();
                    const int ep = getEffectivePlayer();
                    if (ep < 1 || ep > ProDJLink::kMaxPlayers)
                    {
                        sourceActive = false;
                        if (statusTextVisible)
                        {
                            juce::String sideLabel = (proDJLinkPlayer == kPlayerXfA) ? "XF-A" : "XF-B";
                            inputStatusText = sideLabel + ": NO PLAYER ON SIDE";
                        }
                        break;
                    }

                    // PLL runs only for pitch calculation (LTC bit-rate scaling).
                    // It does NOT drive the displayed timecode -- that comes directly
                    // from the CDJ's playhead, which is clean and monotonic (~30Hz).
                    double cdjSpeed = sharedProDJLink->getActualSpeed(ep);
                    pll.tick(
                        sharedProDJLink->getPlayheadMs(ep),
                        sharedProDJLink->getAbsPositionTs(ep),
                        cdjSpeed,
                        sharedProDJLink->isPositionMoving(ep)
                    );

                    // Beat grid micro-correction: nudge PLL toward nearest beat
                    if (!pdlBeatGrid.empty())
                        pll.beatGridCorrect(pdlBeatGrid);

                    // Smooth timecode display using interpolation between CDJ packets.
                    uint32_t rawPlayheadMs = sharedProDJLink->getPlayheadMs(ep);
                    double now = juce::Time::getMillisecondCounterHiRes();
                    bool hasAbs = sharedProDJLink->playerHasAbsolutePosition(ep);

                    // Beat grid precision for NXS2: replace the rough position
                    // (beatCount x 60000/BPM) with the exact ms from the rekordbox
                    // beat grid.  This handles variable-BPM tracks and non-zero
                    // first-beat offsets that the simple formula misses.
                    // CDJ-3000 already provides precise ms via abspos packets.
                    if (!hasAbs && !pdlBeatGrid.empty())
                    {
                        uint32_t bc = sharedProDJLink->getBeatCount(ep);
                        if (bc > 0 && bc <= (uint32_t)pdlBeatGrid.size())
                            rawPlayheadMs = pdlBeatGrid[(size_t)(bc - 1)].timeMs;
                    }

                    // Detect new data from the CDJ.
                    // CDJ-3000: position changes with every abspos packet (~30Hz)
                    //   → simple position comparison works.
                    // NXS2 with beat grid: position only changes at beat boundaries
                    //   (~2Hz at 120BPM), but status packets arrive at 5Hz.
                    //   Use absPositionTs to detect all packets so the snap point
                    //   refreshes at 5Hz (prevents interpolation freeze between beats).
                    double latestTs = sharedProDJLink->getAbsPositionTs(ep);
                    bool isNewPacket;
                    if (hasAbs)
                    {
                        isNewPacket = (rawPlayheadMs != pdlLastPlayheadMs);
                    }
                    else
                    {
                        isNewPacket = (latestTs != pdlLastAbsPosTs);
                        pdlLastAbsPosTs = latestTs;
                    }

                    // ALWAYS start from raw playhead to prevent double-offset application.
                    // The offset is applied later and must start from a clean base.
                    currentTimecode = ProDJLink::playheadToTimecode(rawPlayheadMs, getEffectiveOutputFps());

                    if (isNewPacket)
                    {
                        // Detect if the grid-corrected position actually changed
                        // (new beat or seek) vs same beat with a new timestamp.
                        // Compare against pdlLastPlayheadMs (previous grid value),
                        // NOT pdlSnapMs (which drifts forward from interpolation
                        // advancement between beats).
                        bool positionChanged = (rawPlayheadMs != pdlLastPlayheadMs);
                        pdlLastPlayheadMs = rawPlayheadMs;

                        if (positionChanged)
                        {
                            // New beat or seek -- reset the interpolation anchor.
                            // NXS2 guard: if interpolation overshot the grid position
                            // by a small amount (<50ms), hold the interpolated position
                            // for this tick rather than snapping backward. The anchor
                            // resets to grid so future interpolation aligns naturally.
                            // Defense-in-depth alongside the encoder auto-increment
                            // (MtcOutput/ArtnetOutput).
                            // Large jumps (seek, track change) pass through normally.
                            // CDJ-3000 always snaps -- abspos IS the ground truth.
                            if (!hasAbs && pdlSnapMs > (double)rawPlayheadMs
                                && (pdlSnapMs - (double)rawPlayheadMs) < 50.0)
                            {
                                currentTimecode = ProDJLink::playheadToTimecode(
                                    (uint32_t)pdlSnapMs, getEffectiveOutputFps());
                            }
                            pdlSnapMs = (double)rawPlayheadMs;
                            pdlSnapTime = now;
                        }
                        else if (pdlSnapSpeed > 0.01)
                        {
                            // Same grid position (NXS2 same beat, new status
                            // packet).  Advance the anchor to the current
                            // interpolated position so elapsed stays small and
                            // doesn't hit the maxAdvance cap between beats.
                            double elapsed = now - pdlSnapTime;
                            if (elapsed > 0.0)
                                pdlSnapMs += elapsed * pdlSnapSpeed;
                            pdlSnapTime = now;
                            // Update currentTimecode to the advanced position --
                            // otherwise it stays at the grid value (line above)
                            // which is behind the interpolated position, causing
                            // a backward glitch at each 5Hz status packet.
                            currentTimecode = ProDJLink::playheadToTimecode(
                                (uint32_t)pdlSnapMs, getEffectiveOutputFps());
                        }
                        // Always refresh speed -- the DJ may have moved the
                        // pitch fader between beats, and the interpolation
                        // should use the latest velocity immediately.
                        pdlSnapSpeed = cdjSpeed;
                    }
                    else if (pdlSnapSpeed > 0.01
                            && (sharedProDJLink->isPlayerPlaying(ep)
                                || cdjSpeed > PlayheadPLL::kDeadZone))
                    {
                        // Between packets: interpolate forward using CDJ speed.
                        // Interpolate when PLAYING, or when the motor is still
                        // decelerating (pause ramp: playState=PAUSED but
                        // actualSpeed > 0).  Without this, NXS2 timecode stutters
                        // at 5Hz during the 4-5 second pause deceleration because
                        // only isNewPacket refreshes the timecode.
                        // CDJ-3000 (abspos ~30Hz): cap scales with speed -- limits how
                        //   far the interpolated position advances between updates.
                        // NXS2 (beat-derived ~5-7Hz): fixed 250ms real-time cap.
                        //   Status packets arrive at 5Hz regardless of playback speed,
                        //   so the gap is always ~200ms.  Speed-scaling would starve
                        //   the interpolation at low speeds (e.g. 0.5x -> 125ms < 200ms gap).
                        double elapsed = now - pdlSnapTime;
                        double interpMs = pdlSnapMs + elapsed * pdlSnapSpeed;
                        double maxAdvance = hasAbs ? (50.0 * pdlSnapSpeed) : 250.0;
                        if (elapsed <= maxAdvance)
                            currentTimecode = ProDJLink::playheadToTimecode((uint32_t)interpMs, getEffectiveOutputFps());
                    }

                    // Freeze timecode on end-of-track only.
                    // The CDJ freezes actualSpeed at the last playing value on END_TRACK,
                    // so the PLL keeps advancing while position correction fights back,
                    // causing frame flicker.  Snapshot the timecode and hold it stable.
                    // Pause is NOT frozen: actualSpeed ramps 0.88->0 in ~4-5s and the
                    // PLL follows that deceleration smoothly.
                    bool isEOT = sharedProDJLink->isEndOfTrack(ep);
                    if (!isEOT)
                    {
                        pdlTcFrozen = false;
                    }
                    else if (!pdlTcFrozen)
                    {
                        pdlFrozenTc = currentTimecode;
                        pdlTcFrozen = true;
                    }
                    if (pdlTcFrozen)
                        currentTimecode = pdlFrozenTc;

                    // Feed pitch to LTC output -- scales bit-rate so the audio
                    // timecode stream runs faster/slower matching the CDJ.
                    // MTC and ArtNet don't need pitch scaling -- they send at
                    // nominal rate and the timecode values advance naturally.
                    // When timecode is frozen (end-of-track), zero the pitch so
                    // the LTC encoder stops generating audio.
                    ltcOutput.setPitchMultiplier(pdlTcFrozen ? 0.0 : pll.pitch);

                    // Always auto-increment: encoder goes N->N+1->N+2 at actualSpeed.
                    // CDJ status packets (offset 152) report real motor speed including
                    // ramps, so the encoder rate naturally matches the CDJ. No direct
                    // mode needed -- auto-increment produces perfectly monotonic output
                    // during both acceleration and deceleration ramps.
                    // Resync only on seek (handled by packFrame's >1 frame check).

                    // --- Track change detection ---
                    uint32_t pdlTrackVer = sharedProDJLink->getTrackVersion(ep);
                    if (pdlTrackVer != lastSeenTrackVersion)
                    {
                        lastSeenTrackVersion = pdlTrackVer;
                        uint32_t newId = sharedProDJLink->getTrackID(ep);
                        if (newId != 0 && newId != cachedTrackId)
                        {
                            cachedTrackId = newId;
                            auto tinfo = sharedProDJLink->getTrackInfo(ep);
                            cachedTrackArtist = tinfo.artist;
                            cachedTrackTitle  = tinfo.title;
                            cachedTrackDurationSec = (int)sharedProDJLink->getTrackLengthSec(ep);

                            DBG("TimecodeEngine: track changed -- "
                                + cachedTrackArtist + " - " + cachedTrackTitle
                                + " (cdj_id=" + juce::String(newId) + ")"
                                + " player=" + juce::String(ep));

                            // Request metadata from dbserver (async) -- needed to resolve
                            // "Track #12345" into real artist/title for TrackMap lookup
                            requestDbMetadata(newId);

                            // Attempt TrackMap lookup (succeeds immediately if CDJ provides
                            // real artist/title; deferred until dbserver resolves if not)
                            const auto* entry = lookupTrackInMap();

                            // Reset session override on track change
                            bpmPlayerOverride = kBpmNoOverride;
                            lastSentClockBpm = -1.0f;
                            lastSentOscBpm   = -1.0f;
                            clearBeatGrid();
                            fireTrackTrigger(entry);
                            loadCuePointsForTrack(entry);
                        }
                    }

                    // Phase 2: poll dbClient cache for async metadata results.
                    // When "Track #12345" resolves to real artist/title, update cache
                    // and re-run TrackMap lookup (the first attempt on track change would
                    // have failed because we didn't have real metadata yet).
                    if (dbClient != nullptr && cachedTrackId != 0
                        && cachedTrackTitle.startsWith("Track #"))
                    {
                        auto meta = dbClient->getCachedMetadataLightById(cachedTrackId);
                        if (meta.isValid() && meta.title.isNotEmpty())
                        {
                            if (meta.artist.isNotEmpty()) cachedTrackArtist = meta.artist;
                            cachedTrackTitle  = meta.title;
                            if (meta.durationSeconds > 0)
                            {
                                cachedTrackDurationSec = meta.durationSeconds;
                                if (sharedProDJLink != nullptr)
                                    sharedProDJLink->setTrackLengthSec(ep,
                                        (uint32_t)meta.durationSeconds);
                            }

                            // NOW we have real artist+title -- do the TrackMap lookup
                            const auto* entry = lookupTrackInMap();
                            fireTrackTrigger(entry);
                            loadCuePointsForTrack(entry);
                        }
                    }

                    // --- Persist auto-filled metadata ---
                    // tick() runs on the message thread (timerCallback), so we can
                    // call save() directly.  The previous callAsync captured a raw
                    // pointer that could dangle if the app closed between post and
                    // execution.
                    if (trackMapDirty && trackMapPtr != nullptr)
                    {
                        trackMapDirty = false;
                        trackMapPtr->save();
                    }

                    // Deferred duration pickup: NXS2 doesn't report duration in
                    // protocol packets.  If we missed it at track-change time,
                    // check ProDJLinkInput (CDJ-3000 abspos) or dbClient cache.
                    if (cachedTrackDurationSec == 0 && trackMapPtr != nullptr)
                    {
                        int nowDur = (int)sharedProDJLink->getTrackLengthSec(ep);
                        if (nowDur == 0 && dbClient != nullptr && cachedTrackId != 0)
                        {
                            auto meta = dbClient->getCachedMetadataLightById(cachedTrackId);
                            if (meta.isValid() && meta.durationSeconds > 0)
                                nowDur = meta.durationSeconds;
                        }
                        if (nowDur > 0)
                        {
                            cachedTrackDurationSec = nowDur;
                            const auto* entry = lookupTrackInMap();
                            uint32_t savedPlayhead = lastCueCheckMs;
                            loadCuePointsForTrack(entry);
                            if (savedPlayhead > 0)
                            {
                                for (auto& ac : armedCues)
                                    ac.fired = (ac.cue.positionMs < savedPlayhead);
                                lastCueCheckMs = savedPlayhead;
                            }
                        }
                    }

                    // --- Apply timecode offset ---
                    if (trackMapEnabled && trackMapped)
                    {
                        currentTimecode = applyTimecodeOffset(
                            currentTimecode, currentFps,
                            cachedOffH, cachedOffM, cachedOffS, cachedOffF,
                            currentFps);
                    }

                    // --- Fire cue point triggers ---
                    // Only fire during actual playback.  Scrub/jog/cue-preview
                    // moves the playhead but should NOT trigger cue points --
                    // DJs preview constantly and spurious MIDI/OSC/ArtNet
                    // triggers during preparation would be disruptive.
                    if (sharedProDJLink->isPlayerPlaying(ep))
                        tickCuePoints(rawPlayheadMs);
                    else
                        lastCueCheckMs = rawPlayheadMs;  // track position so seek detection stays correct

                    bool pdlRx = sharedProDJLink->isReceiving();
                    if (statusTextVisible)
                    {
                        if (pdlRx)
                        {
                            // Note: no fps auto-detection for ProDJLink -- CDJ sends ms,
                            // not frames. The user's fps selection IS the frame rate.

                            bool pdlHasTC = sharedProDJLink->hasTimecodeData(ep);
                            if (pdlHasTC)
                            {
                                juce::String pdlModel = sharedProDJLink->getPlayerModel(ep);
                                inputStatusText = isXfMode()
                                    ? ((proDJLinkPlayer == kPlayerXfA ? "XF-A" : "XF-B")
                                       + juce::String(" P") + juce::String(ep))
                                    : ("RX P" + juce::String(ep));
                                if (pdlModel.isNotEmpty())
                                    inputStatusText += " " + pdlModel;

                                if (!sharedProDJLink->isPositionMoving(ep))
                                    inputStatusText += " " + sharedProDJLink->getPlayStateString(ep);

                                double pdlBpm = sharedProDJLink->getBPM(ep);
                                if (pdlBpm > 0.0)
                                    inputStatusText += " | " + juce::String(pdlBpm, 1) + " BPM";
                            }
                            else
                            {
                                inputStatusText = "P" + juce::String(ep)
                                    + " " + sharedProDJLink->getPlayStateString(ep)
                                    + " - NO POSITION DATA";
                            }

                            if (pdlHasTC && trackMapEnabled
                                && cachedTrackTitle.isNotEmpty())
                            {
                                if (trackMapped)
                                    inputStatusText += " | MAP: " + cachedTrackTitle;
                                else
                                    inputStatusText += " | NO MAP";
                            }

                            inputStatusText += " | " + sharedProDJLink->getBindInfo();
                        }
                        else
                        {
                            inputStatusText = "WAITING";
                            auto discP = sharedProDJLink->getDiscoveredPlayers();
                            if (discP.isEmpty())
                                inputStatusText += " | NO PLAYERS";
                            else
                                inputStatusText += " | " + juce::String(discP.size()) + " PLAYER(S)";
                            inputStatusText += " | " + sharedProDJLink->getBindInfo();
                        }
                    }

                    // --- MIDI Clock ---
                    if (pdlRx && midiClockEnabled && triggerOutput.isMidiClockRunning())
                    {
                        double pdlClkBpm = sharedProDJLink->getMasterBPM();
                        if (pdlClkBpm <= 0.0) pdlClkBpm = sharedProDJLink->getBPM(ep);
                        pdlClkBpm = applyBpmMultiplier(pdlClkBpm, getEffectiveBpmMultiplier());
                        if (pdlClkBpm > 0.0 && std::abs((float)pdlClkBpm - lastSentClockBpm) > 0.05f)
                        {
                            triggerOutput.updateMidiClockBpm(pdlClkBpm);
                            lastSentClockBpm = (float)pdlClkBpm;
                        }
                    }

                    // --- Ableton Link ---
                    if (pdlRx && linkBridge.isEnabled())
                    {
                        double pdlLnkBpm = sharedProDJLink->getMasterBPM();
                        if (pdlLnkBpm <= 0.0) pdlLnkBpm = sharedProDJLink->getBPM(ep);
                        pdlLnkBpm = applyBpmMultiplier(pdlLnkBpm, getEffectiveBpmMultiplier());
                        if (pdlLnkBpm > 0.0)
                            linkBridge.setTempo(pdlLnkBpm);
                    }

                    // --- OSC BPM Forward ---
                    if (pdlRx && oscForwardEnabled && triggerOutput.isOscConnected())
                    {
                        double pdlOscBpm = sharedProDJLink->getMasterBPM();
                        if (pdlOscBpm <= 0.0) pdlOscBpm = sharedProDJLink->getBPM(ep);
                        pdlOscBpm = applyBpmMultiplier(pdlOscBpm, getEffectiveBpmMultiplier());
                        sendBpmOsc(pdlOscBpm);
                    }

                    // --- Mixer Fader Forward (OSC + MIDI CC) ---
                    // Requires DJM type-0x39 packets (Pioneer bridge on network).
                    // OSC: normalised float 0.0-1.0 on /mixer/ch1..4, /mixer/crossfader, /mixer/master
                    // MIDI CC: ch1=CC1, ch2=CC2, ch3=CC3, ch4=CC4, crossfader=CC5, master=CC7
                    if (sharedProDJLink->hasMixerFaderData()
                        && mixerMapPtr != nullptr
                        && (oscMixerForwardEnabled || midiMixerForwardEnabled || artnetMixerForwardEnabled))
                    {
                        forwardMixerParams();
                    }

                    // sourceActive: determined directly from CDJ playState + actualSpeed.
                    //
                    //   PLAYING/LOOPING/CUE_PLAY: active while actualSpeed >= minimum
                    //     (includes acceleration ramp -- output starts as speed ramps up)
                    //   PAUSED: actualSpeed ramps target->0 in ~4-5s. Output naturally
                    //     stops when speed drops below kMinEncodingPitch. No special case.
                    //   END_TRACK: CDJ freezes actualSpeed (never ramps to 0).
                    //     playState == END_TRACK tells us directly -> force inactive.
                    //   NO_TRACK/LOADING/SEEKING: no useful timecode -> inactive.
                    bool pdlHasData = sharedProDJLink->hasTimecodeData(ep);
                    double speed = (pll.actualSpeed > pll.kDeadZone)
                                 ? pll.actualSpeed
                                 : std::abs(pll.smoothVelocity);

                    bool wasActive = sourceActive;
                    sourceActive = pdlRx && pdlHasData
                                && (speed >= PlayheadPLL::kMinEncodingPitch)
                                && !isEOT
                                && isOnAirGateOpen();

                    // Reseed LTC encoder on pause->active transition
                    // so it starts a fresh frame instead of continuing a stale one
                    if (sourceActive && !wasActive)
                        ltcOutput.reseed();
                }
                else { sourceActive = false; if (statusTextVisible) inputStatusText = "NOT CONNECTED"; }
                break;

            case InputSource::StageLinQ:
                if (sharedStageLinQ != nullptr && sharedStageLinQ->getIsRunning())
                {
                    // XF-A/XF-B auto-follow (same logic as ProDJLink)
                    if (isXfMode())
                        resolveXfPlayerStageLinQ();
                    const int ep = getEffectivePlayer();
                    if (ep < 1 || ep > StageLinQ::kMaxDecks)
                    {
                        sourceActive = false;
                        if (statusTextVisible)
                        {
                            if (isXfMode())
                                inputStatusText = juce::String(proDJLinkPlayer == kPlayerXfA ? "XF-A" : "XF-B")
                                                + ": NO DECK ON SIDE";
                            else
                                inputStatusText = "INVALID DECK";
                        }
                        break;
                    }

                    // Drive PLL from StageLinQ deck data
                    double slqSpeed = sharedStageLinQ->getActualSpeed(ep);
                    pll.tick(
                        sharedStageLinQ->getPlayheadMs(ep),
                        sharedStageLinQ->getAbsPositionTs(ep),
                        slqSpeed,
                        sharedStageLinQ->isPositionMoving(ep)
                    );

                    // Smooth timecode display using interpolation between StateMap/BeatInfo updates
                    uint32_t rawPlayheadMs = sharedStageLinQ->getPlayheadMs(ep);
                    double now = juce::Time::getMillisecondCounterHiRes();

                    bool isNewPacket = (rawPlayheadMs != pdlLastPlayheadMs);

                    // ALWAYS start from raw playhead to prevent double-offset application.
                    currentTimecode = StageLinQ::playheadToTimecode(rawPlayheadMs, getEffectiveOutputFps());

                    if (isNewPacket)
                    {
                        pdlLastPlayheadMs = rawPlayheadMs;
                        pdlSnapMs = (double)rawPlayheadMs;
                        pdlSnapTime = now;
                        pdlSnapSpeed = slqSpeed;
                    }
                    else if (pdlSnapSpeed > 0.01 && sharedStageLinQ->isPlayerPlaying(ep))
                    {
                        double elapsed = now - pdlSnapTime;
                        double interpMs = pdlSnapMs + elapsed * pdlSnapSpeed;
                        double maxAdvance = 50.0 * pdlSnapSpeed;
                        if (elapsed <= maxAdvance)
                            currentTimecode = StageLinQ::playheadToTimecode((uint32_t)interpMs, getEffectiveOutputFps());
                    }

                    // Feed pitch to LTC output.
                    // Use pll.pitch directly -- same as ProDJLink.  When the deck
                    // pauses, Speed ramps toward 0, the PLL follows, and LTC
                    // decelerates smoothly.  The previous code gated on isPlaying
                    // which cut LTC to zero the instant Play=false arrived -- before
                    // Speed had a chance to ramp down -- causing an audible click
                    // in the LTC audio stream.
                    // No end-of-track freeze needed: Denon does not have the CDJ
                    // frozen-actualSpeed quirk.  sourceActive below gates on
                    // speed >= kMinEncodingPitch, which handles the stopped state.
                    ltcOutput.setPitchMultiplier(pll.pitch);

                    // --- Track change detection ---
                    uint32_t slqTrackVer = sharedStageLinQ->getTrackVersion(ep);
                    if (slqTrackVer != lastSeenTrackVersion)
                    {
                        lastSeenTrackVersion = slqTrackVer;
                        auto tinfo = sharedStageLinQ->getTrackInfo(ep);
                        if (tinfo.artist.isNotEmpty() || tinfo.title.isNotEmpty())
                        {
                            cachedTrackArtist = tinfo.artist;
                            cachedTrackTitle  = tinfo.title;
                            cachedTrackId = slqTrackVer;  // StageLinQ has no numeric ID; use version
                            cachedTrackDurationSec = (int)sharedStageLinQ->getTrackLengthSec(ep);

                            DBG("TimecodeEngine: StageLinQ track changed -- "
                                + cachedTrackArtist + " - " + cachedTrackTitle
                                + " deck=" + juce::String(ep));

                            const auto* entry = lookupTrackInMap();
                            bpmPlayerOverride = kBpmNoOverride;
                            lastSentClockBpm = -1.0f;
                            lastSentOscBpm   = -1.0f;
                            fireTrackTrigger(entry);
                            loadCuePointsForTrack(entry);
                        }
                    }

                    // Persist auto-filled metadata
                    if (trackMapDirty && trackMapPtr != nullptr)
                    {
                        trackMapDirty = false;
                        trackMapPtr->save();
                    }

                    // Deferred duration pickup: TrackLength may arrive after
                    // SongName/SongLoaded in the StateMap stream.  If we missed
                    // it at track-change time, pick it up now and re-lookup.
                    if (cachedTrackDurationSec == 0 && trackMapPtr != nullptr)
                    {
                        int nowDur = (int)sharedStageLinQ->getTrackLengthSec(ep);
                        if (nowDur > 0)
                        {
                            cachedTrackDurationSec = nowDur;
                            const auto* entry = lookupTrackInMap();
                            uint32_t savedPlayhead = lastCueCheckMs;
                            loadCuePointsForTrack(entry);
                            if (savedPlayhead > 0)
                            {
                                for (auto& ac : armedCues)
                                    ac.fired = (ac.cue.positionMs < savedPlayhead);
                                lastCueCheckMs = savedPlayhead;
                            }
                        }
                    }

                    // Apply timecode offset from TrackMap
                    if (trackMapEnabled && trackMapped)
                    {
                        currentTimecode = applyTimecodeOffset(
                            currentTimecode, currentFps,
                            cachedOffH, cachedOffM, cachedOffS, cachedOffF,
                            currentFps);
                    }

                    // --- Fire cue point triggers ---
                    // Same guard as ProDJLink: only during playback.
                    if (sharedStageLinQ->isPlayerPlaying(ep))
                        tickCuePoints(rawPlayheadMs);
                    else
                        lastCueCheckMs = rawPlayheadMs;

                    bool slqRx = sharedStageLinQ->isReceiving();
                    if (statusTextVisible)
                    {
                        if (slqRx)
                        {
                            bool slqHasTC = sharedStageLinQ->hasTimecodeData(ep);
                            if (slqHasTC)
                            {
                                juce::String slqModel = sharedStageLinQ->getPlayerModel(ep);
                                inputStatusText = isXfMode()
                                    ? (juce::String(proDJLinkPlayer == kPlayerXfA ? "XF-A" : "XF-B")
                                       + " D" + juce::String(ep))
                                    : ("RX D" + juce::String(ep));
                                if (slqModel.isNotEmpty())
                                    inputStatusText += " " + slqModel;

                                if (!sharedStageLinQ->isPositionMoving(ep))
                                    inputStatusText += " " + sharedStageLinQ->getPlayStateString(ep);

                                double slqBpm = sharedStageLinQ->getBPM(ep);
                                if (slqBpm > 0.0)
                                    inputStatusText += " | " + juce::String(slqBpm, 1) + " BPM";
                            }
                            else
                            {
                                inputStatusText = "D" + juce::String(ep)
                                    + " " + sharedStageLinQ->getPlayStateString(ep)
                                    + " - NO POSITION DATA";
                            }

                            if (slqHasTC && trackMapEnabled
                                && cachedTrackTitle.isNotEmpty())
                            {
                                if (trackMapped)
                                    inputStatusText += " | MAP: " + cachedTrackTitle;
                                else
                                    inputStatusText += " | NO MAP";
                            }

                            inputStatusText += " | " + sharedStageLinQ->getBindInfo();
                        }
                        else
                        {
                            inputStatusText = "WAITING | " + sharedStageLinQ->getBindInfo();
                        }
                    }

                    // --- MIDI Clock ---
                    if (slqRx && midiClockEnabled && triggerOutput.isMidiClockRunning())
                    {
                        double slqClkBpm = sharedStageLinQ->getMasterBPM();
                        if (slqClkBpm <= 0.0) slqClkBpm = sharedStageLinQ->getBPM(ep);
                        slqClkBpm = applyBpmMultiplier(slqClkBpm, getEffectiveBpmMultiplier());
                        if (slqClkBpm > 0.0 && std::abs((float)slqClkBpm - lastSentClockBpm) > 0.05f)
                        {
                            triggerOutput.updateMidiClockBpm(slqClkBpm);
                            lastSentClockBpm = (float)slqClkBpm;
                        }
                    }

                    // --- Ableton Link ---
                    if (slqRx && linkBridge.isEnabled())
                    {
                        double slqLnkBpm = sharedStageLinQ->getMasterBPM();
                        if (slqLnkBpm <= 0.0) slqLnkBpm = sharedStageLinQ->getBPM(ep);
                        slqLnkBpm = applyBpmMultiplier(slqLnkBpm, getEffectiveBpmMultiplier());
                        if (slqLnkBpm > 0.0)
                            linkBridge.setTempo(slqLnkBpm);
                    }

                    // --- OSC BPM Forward ---
                    if (slqRx && oscForwardEnabled && triggerOutput.isOscConnected())
                    {
                        double slqOscBpm = sharedStageLinQ->getMasterBPM();
                        if (slqOscBpm <= 0.0) slqOscBpm = sharedStageLinQ->getBPM(ep);
                        slqOscBpm = applyBpmMultiplier(slqOscBpm, getEffectiveBpmMultiplier());
                        sendBpmOsc(slqOscBpm);
                    }

                    // --- Mixer fader forward (OSC/MIDI/ArtNet) ---
                    if (slqRx && (oscMixerForwardEnabled || midiMixerForwardEnabled || artnetMixerForwardEnabled))
                    {
                        forwardStageLinQMixer();
                    }

                    // sourceActive from direct play state
                    bool slqHasData = sharedStageLinQ->hasTimecodeData(ep);
                    double speed = (pll.actualSpeed > pll.kDeadZone)
                                 ? pll.actualSpeed
                                 : std::abs(pll.smoothVelocity);

                    bool wasActive = sourceActive;
                    sourceActive = slqRx && slqHasData
                                && (speed >= PlayheadPLL::kMinEncodingPitch);

                    if (sourceActive && !wasActive)
                        ltcOutput.reseed();
                }
                else { sourceActive = false; if (statusTextVisible) inputStatusText = "NOT CONNECTED"; }
                break;

            case InputSource::Hippotizer:
                if (hippotizerInput.getIsRunning())
                {
                    bool rx = hippotizerInput.isReceiving();
                    if (rx)
                    {
                        uint32_t ms = hippotizerInput.getMsSinceMidnight();
                        currentTimecode = wallClockToTimecode((double)ms, currentFps);
                    }

                    if (statusTextVisible)
                    {
                        // Build discovery suffix: "GOHIPPO (4.8.4.23374) @ 192.168.0.2"
                        juce::String discInfo;
                        if (hippotizerInput.isDiscovered())
                        {
                            discInfo = hippotizerInput.getDiscoveredName();
                            auto fw = hippotizerInput.getDiscoveredFirmware();
                            if (fw.isNotEmpty())
                                discInfo += " (" + fw + ")";
                            discInfo += " @ " + hippotizerInput.getDiscoveredIp();
                        }

                        if (rx)
                        {
                            inputStatusText = "RX ON " + hippotizerInput.getBindInfo();
                            if (discInfo.isNotEmpty())
                                inputStatusText += " | " + discInfo;
                        }
                        else if (discInfo.isNotEmpty())
                        {
                            inputStatusText = "WAITING | " + discInfo;
                        }
                        else
                        {
                            inputStatusText = "PAUSED - " + hippotizerInput.getBindInfo();
                        }
                    }
                    sourceActive = rx;
                }
                else { sourceActive = false; if (statusTextVisible) inputStatusText = "NOT LISTENING"; }
                break;
        }

        // --- Audio BPM forwarding (non-DJ sources only) ---
        // ProDJLink and StageLinQ have their own precise BPM from the CDJs/Denon;
        // audio detection is only useful for MTC, LTC, ArtNet, and SystemTime.
        if (audioBpmEnabled && audioBpmInput.getIsRunning()
            && activeInput != InputSource::ProDJLink
            && activeInput != InputSource::StageLinQ)
        {
            double abpm = audioBpmInput.getBpm();
            bool hasBpm = audioBpmInput.hasBpm();

            if (hasBpm)
            {
                double bpm = applyBpmMultiplier(abpm, getEffectiveBpmMultiplier());

                // MIDI Clock
                if (midiClockEnabled && triggerOutput.isMidiClockRunning() && bpm > 0.0)
                {
                    if (std::abs((float)bpm - lastSentClockBpm) > 0.05f)
                    {
                        triggerOutput.updateMidiClockBpm(bpm);
                        lastSentClockBpm = (float)bpm;
                    }
                }

                // Ableton Link
                if (linkBridge.isEnabled() && bpm > 0.0)
                    linkBridge.setTempo(bpm);

                // OSC BPM Forward
                sendBpmOsc(bpm);
            }

            // Append BPM to status text
            if (statusTextVisible && abpm > 0.0)
                inputStatusText += " | " + juce::String(abpm, 1) + " BPM";
        }

        routeTimecodeToOutputs();
        updateVuMeters();
    }

    //==========================================================================
    // Status text (read by MainComponent for UI)
    //==========================================================================
    juce::String getInputStatusText() const { return inputStatusText; }
    juce::String getMtcOutStatusText() const { return mtcOutStatusText; }
    juce::String getArtnetOutStatusText() const { return artnetOutStatusText; }
    juce::String getLtcOutStatusText() const { return ltcOutStatusText; }
    juce::String getThruOutStatusText() const { return thruOutStatusText; }
    juce::String getHippoOutStatusText() const { return hippoOutStatusText; }

    /// Only the currently displayed engine needs to build status text strings.
    /// Call with true for the selected engine, false for background engines.
    /// Avoids ~25 juce::String heap allocations per tick on non-visible engines.
    void setStatusTextVisible(bool visible) { statusTextVisible = visible; }

    //==========================================================================
    // Generator (internal timecode source, replaces old SystemTime)
    //==========================================================================
    enum class GeneratorState { Stopped, Playing, Paused };

    GeneratorState getGeneratorState() const { return genState; }

    void generatorPlay()
    {
        if (genState == GeneratorState::Playing) return;
        if (genState == GeneratorState::Stopped)
            genCurrentMs = genStartMs;  // reset to start TC
        genLastTickTime = juce::Time::getMillisecondCounterHiRes();
        genState = GeneratorState::Playing;
    }

    void generatorPause()
    {
        if (genState == GeneratorState::Playing)
            genState = GeneratorState::Paused;
    }

    void generatorStop()
    {
        genState = GeneratorState::Stopped;
        genCurrentMs = genStartMs;
        if (activeInput == InputSource::SystemTime)
            currentTimecode = wallClockToTimecode(genCurrentMs, currentFps);
    }

    /// Set start timecode in ms from midnight.
    void setGeneratorStartMs(double ms)
    {
        genStartMs = juce::jmax(0.0, ms);
        if (genState == GeneratorState::Stopped && activeInput == InputSource::SystemTime)
        {
            genCurrentMs = genStartMs;
            currentTimecode = wallClockToTimecode(genCurrentMs, currentFps);
        }
    }

    /// Set stop timecode in ms from midnight. 0 = no stop (freerun).
    void setGeneratorStopMs(double ms) { genStopMs = juce::jmax(0.0, ms); }

    double getGeneratorStartMs() const { return genStartMs; }
    double getGeneratorStopMs()  const { return genStopMs; }
    double getGeneratorCurrentMs() const { return genCurrentMs; }

    /// Clock mode: read wall clock (old SystemTime behavior) instead of generator transport.
    void setGeneratorClockMode(bool useSystemClock)
    {
        if (useSystemClock && !genClockMode)
            generatorStop();  // stop transport when switching to clock mode
        genClockMode = useSystemClock;
    }
    bool getGeneratorClockMode() const { return genClockMode; }

    //==========================================================================
    // VU meter smoothed levels
    //==========================================================================
    float getSmoothedLtcInLevel() const  { return sLtcIn; }
    float getSmoothedThruInLevel() const { return sThruIn; }
    float getSmoothedLtcOutLevel() const { return sLtcOut; }
    float getSmoothedThruOutLevel() const { return sThruOut; }

    //==========================================================================
    // Helper queries
    //==========================================================================
    bool isInputStarted() const
    {
        switch (activeInput)
        {
            case InputSource::SystemTime: return true;
            case InputSource::MTC:        return mtcInput.getIsRunning();
            case InputSource::ArtNet:     return artnetInput.getIsRunning();
            case InputSource::LTC:        return ltcInput.getIsRunning();
            case InputSource::ProDJLink:  return sharedProDJLink != nullptr && sharedProDJLink->getIsRunning();
            case InputSource::StageLinQ:  return sharedStageLinQ != nullptr && sharedStageLinQ->getIsRunning();
            case InputSource::Hippotizer: return hippotizerInput.getIsRunning();
            default:                      return false;
        }
    }

    static juce::String inputSourceToString(InputSource src)
    {
        switch (src)
        {
            case InputSource::MTC:        return "MTC";
            case InputSource::ArtNet:     return "ArtNet";
            case InputSource::SystemTime: return "Generator";
            case InputSource::LTC:        return "LTC";
            case InputSource::ProDJLink:  return "ProDJLink";
            case InputSource::StageLinQ:  return "StageLinQ";
            case InputSource::Hippotizer: return "HippoNet";
        }
        return "Generator";
    }

    static InputSource stringToInputSource(const juce::String& s)
    {
        if (s == "MTC") return InputSource::MTC;
        if (s == "ArtNet") return InputSource::ArtNet;
        if (s == "LTC") return InputSource::LTC;
        if (s == "ProDJLink") return InputSource::ProDJLink;
        if (s == "StageLinQ") return InputSource::StageLinQ;
        if (s == "HippoNet" || s == "Hippotizer") return InputSource::Hippotizer;
        if (s == "TCNet") return InputSource::ProDJLink;  // legacy migration
        if (s == "Generator" || s == "SystemTime") return InputSource::SystemTime;  // backward compat
        return InputSource::SystemTime;
    }

    static juce::String getInputName(InputSource s)
    {
        switch (s)
        {
            case InputSource::MTC:        return "MTC";
            case InputSource::ArtNet:     return "ART-NET";
            case InputSource::SystemTime: return "GENERATOR";
            case InputSource::LTC:        return "LTC";
            case InputSource::ProDJLink:  return "PRO DJ LINK";
            case InputSource::StageLinQ:  return "STAGELINQ";
            case InputSource::Hippotizer: return "HIPPONET";
            default:                      return "---";
        }
    }

    static int fpsToIndex(FrameRate fps)
    {
        switch (fps)
        {
            case FrameRate::FPS_2398: return 0;
            case FrameRate::FPS_24:   return 1;
            case FrameRate::FPS_25:   return 2;
            case FrameRate::FPS_2997: return 3;
            case FrameRate::FPS_30:   return 4;
        }
        return 4;
    }

    static FrameRate indexToFps(int index)
    {
        switch (index)
        {
            case 0: return FrameRate::FPS_2398;
            case 1: return FrameRate::FPS_24;
            case 2: return FrameRate::FPS_25;
            case 3: return FrameRate::FPS_2997;
            case 4: return FrameRate::FPS_30;
        }
        return FrameRate::FPS_30;
    }

private:
    //--------------------------------------------------------------------------
    int engineIndex;
    juce::String engineName;

    // Input state
    InputSource activeInput = InputSource::SystemTime;
    FrameRate currentFps = FrameRate::FPS_30;
    mutable juce::SpinLock timecodeLock;  // protects currentTimecode for getCurrentTimecode() thread safety
    Timecode currentTimecode;
    bool sourceActive = true;
    bool outputsWereActive = false;  // previous sourceActive state for transition detection
    bool userOverrodeLtcFps = false;

    // Generator state (internal timecode source)
    GeneratorState genState = GeneratorState::Stopped;
    bool   genClockMode = true;    // true = wall clock (old SystemTime), false = transport generator
    double genStartMs   = 0.0;     // start TC in ms from midnight
    double genStopMs    = 0.0;     // stop TC in ms (0 = freerun)
    double genCurrentMs = 0.0;     // current position in ms
    double genLastTickTime = 0.0;  // hiRes ms for delta calculation

    // FPS conversion
    bool fpsConvertEnabled = false;
    FrameRate outputFps = FrameRate::FPS_30;
    Timecode outputTimecode;

    // Output state
    bool outputMtcEnabled    = false;
    bool outputArtnetEnabled = false;
    bool outputLtcEnabled    = false;
    bool outputThruEnabled   = false;
    bool outputTcnetEnabled  = false;
    bool outputHippoEnabled  = false;
    int  tcnetLayer          = 0;      // TCNet layer index 0-3

    // On-air gate: when enabled, the engine only produces active timecode
    // when the current CDJ is flagged on-air by the DJM mixer. This uses the
    // mixer's own gating logic (fader + cross-fader + EQ + mute), so no
    // threshold or channel selection is needed.
    bool onAirGateEnabled = false;

    int mtcOutputOffset    = 0;
    int artnetOutputOffset = 0;
    int ltcOutputOffset    = 0;
    int tcnetOutputOffsetMs = 0;   // TCNet offset in milliseconds

    // Protocol handlers
    MtcInput     mtcInput;
    MtcOutput    mtcOutput;
    ArtnetInput  artnetInput;
    ArtnetOutput artnetOutput;
    HippotizerInput hippotizerInput;
    HippotizerOutput hippotizerOutput;
    LtcInput     ltcInput;
    LtcOutput    ltcOutput;
    ProDJLinkInput* sharedProDJLink = nullptr;  // shared across engines
    StageLinQInput* sharedStageLinQ = nullptr;  // shared across engines
    DbServerClient* dbClient       = nullptr;  // shared across engines (Phase 2)
    int             proDJLinkPlayer = 1;        // per-engine player selection (1..6, 7=XF-A, 8=XF-B)

    // Crossfader auto-follow (XF-A / XF-B mode)
    static constexpr int kPlayerXfA = 7;        // auto-follow crossfader side A
    static constexpr int kPlayerXfB = 8;        // auto-follow crossfader side B
    int  resolvedXfPlayer = 0;                  // physical player (1-4) currently followed in XF mode

    bool isXfMode() const { return proDJLinkPlayer >= kPlayerXfA; }

    /// Resolve which physical player to follow in XF-A/XF-B mode.
    /// Sticky: stays on current player while it has on-air flag.
    /// Falls back to another player on the same XF side when current loses on-air.
    void resolveXfPlayer()
    {
        if (!sharedProDJLink || !sharedProDJLink->hasMixerFaderData())
            return;

        uint8_t targetSide = (proDJLinkPlayer == kPlayerXfA) ? 1 : 2; // 1=A, 2=B

        // Sticky: keep current if still assigned to our side AND on-air
        if (resolvedXfPlayer >= 1 && resolvedXfPlayer <= 4)
        {
            uint8_t xf = sharedProDJLink->getChannelXfAssign(resolvedXfPlayer);
            bool onAir = sharedProDJLink->isPlayerOnAir(resolvedXfPlayer);
            if (xf == targetSide && onAir)
                return;  // still valid
        }

        // Current player lost on-air or wrong side -- find replacement
        // Prefer on-air players on our side
        for (int ch = 1; ch <= 4; ++ch)
        {
            uint8_t xf = sharedProDJLink->getChannelXfAssign(ch);
            if (xf == targetSide && sharedProDJLink->isPlayerOnAir(ch))
            {
                switchResolvedPlayer(ch);
                return;
            }
        }

        // No on-air player -- keep current if still on right side (just faded out)
        if (resolvedXfPlayer >= 1 && resolvedXfPlayer <= 4)
        {
            uint8_t xf = sharedProDJLink->getChannelXfAssign(resolvedXfPlayer);
            if (xf == targetSide)
                return;
        }

        // Find any player on this side (not on-air but assigned)
        for (int ch = 1; ch <= 4; ++ch)
        {
            uint8_t xf = sharedProDJLink->getChannelXfAssign(ch);
            if (xf == targetSide)
            {
                switchResolvedPlayer(ch);
                return;
            }
        }

        // No player assigned to this side at all
        if (resolvedXfPlayer != 0)
        {
            resolvedXfPlayer = 0;
            pll.reset(); clearBeatGrid(); pdlTcFrozen = false; pdlLastPlayheadMs = 0; pdlLastAbsPosTs = 0.0;
            pdlSnapMs = 0.0; pdlSnapTime = 0.0; pdlSnapSpeed = 1.0;
        }
    }

    /// Resolve XF-A/XF-B for StageLinQ.  Same sticky logic as ProDJLink.
    /// Uses derived on-air status (fader + crossfader + channel assignment).
    /// Channel assignment values assumed 0=THRU, 1=A, 2=B -- awaiting
    /// hardware confirmation.
    void resolveXfPlayerStageLinQ()
    {
        if (!sharedStageLinQ || !sharedStageLinQ->hasMixerData())
            return;

        int targetSide = (proDJLinkPlayer == kPlayerXfA) ? 1 : 2;

        // Helper: get crossfader assignment for a channel.
        // Values assumed: 0=THRU, 1=A, 2=B -- awaiting hardware confirmation.
        auto getAssign = [this](int ch) -> int
        {
            return sharedStageLinQ->getChannelAssignment(ch);
        };

        // Sticky: keep current if still assigned to our side AND on-air
        if (resolvedXfPlayer >= 1 && resolvedXfPlayer <= StageLinQ::kMaxDecks)
        {
            int assign = getAssign(resolvedXfPlayer);
            bool onAir = sharedStageLinQ->isDeckOnAir(resolvedXfPlayer);
            if (assign == targetSide && onAir)
                return;  // still valid
        }

        // Find on-air replacement on our side
        for (int ch = 1; ch <= StageLinQ::kMaxDecks; ++ch)
        {
            int assign = getAssign(ch);
            if (assign == targetSide && sharedStageLinQ->isDeckOnAir(ch))
            {
                switchResolvedPlayer(ch);
                return;
            }
        }

        // No on-air player -- keep current if still on right side
        if (resolvedXfPlayer >= 1 && resolvedXfPlayer <= StageLinQ::kMaxDecks)
        {
            int assign = getAssign(resolvedXfPlayer);
            if (assign == targetSide)
                return;
        }

        // Find any player on this side
        for (int ch = 1; ch <= StageLinQ::kMaxDecks; ++ch)
        {
            int assign = getAssign(ch);
            if (assign == targetSide)
            {
                switchResolvedPlayer(ch);
                return;
            }
        }

        // No player assigned to this side
        if (resolvedXfPlayer != 0)
        {
            resolvedXfPlayer = 0;
            pll.reset(); clearBeatGrid(); pdlTcFrozen = false; pdlLastPlayheadMs = 0; pdlLastAbsPosTs = 0.0;
            pdlSnapMs = 0.0; pdlSnapTime = 0.0; pdlSnapSpeed = 1.0;
        }
    }

    /// Switch the resolved player with a mini-reset (PLL + track cache)
    void switchResolvedPlayer(int newPlayer)
    {
        if (newPlayer == resolvedXfPlayer) return;
        DBG("TimecodeEngine[" + engineName + "]: XF resolved player "
            + juce::String(resolvedXfPlayer) + " -> " + juce::String(newPlayer));
        resolvedXfPlayer = newPlayer;
        // Reset PLL and track cache so we start clean on the new player
        pll.reset(); clearBeatGrid(); pdlTcFrozen = false; pdlLastPlayheadMs = 0; pdlLastAbsPosTs = 0.0;
        pdlSnapMs = 0.0; pdlSnapTime = 0.0; pdlSnapSpeed = 1.0;
        cachedTrackId = 0;
        cachedTrackArtist.clear();
        cachedTrackTitle.clear();
        cachedTrackDurationSec = 0;
        trackMapped = false;
        cachedOffH = cachedOffM = cachedOffS = cachedOffF = 0;
        cachedBpmMultiplier = 0;
        lastSeenTrackVersion = 0;
        armedCues.clear();
        lastCueCheckMs = 0;
        lastSentClockBpm = -1.0f;
        lastSentOscBpm   = -1.0f;
        bpmPlayerOverride = kBpmNoOverride;
        ltcOutput.setPitchMultiplier(1.0);
    }

    //==========================================================================
    // Playhead PLL -- smooth timecode generation from CDJ data.
    //
    // Instead of snapping to each CDJ packet (~30ms), we maintain a free-running
    // clock that advances at the CDJ's actual motor speed (offset 152). When a
    // new packet arrives, we gently correct the clock toward the CDJ's real position.
    //
    // CDJ actual speed (offset 152) is the real playback rate including:
    //   - Motor ramp on play (0 -> target over ~0.5s)
    //   - Motor ramp on pause (target -> 0 over ~4-5s)
    //   - Jog wheel adjustments
    //   - Pitch fader changes
    //
    // This means we DON'T need:
    //   - Dual transient/stable modes (actualSpeed already IS the correct rate)
    //   - Stability detection (no mode switching = no transition glitches)
    //   - dp/dt velocity estimation for CDJ-3000 (kept only as NXS2 fallback)
    //
    // Result: perfectly smooth timecode at any pitch, including during ramps.
    // Hard reset only on seek/track change (>500ms error).
    //==========================================================================
    struct PlayheadPLL
    {
        double positionMs     = 0.0;  // smoothed position (ms)
        double lastTickTime   = 0.0;  // timestamp of last tick
        double lastPacketTs   = 0.0;  // to detect new CDJ packets
        double lastCdjPos     = 0.0;  // previous CDJ position (for velocity calc)
        double pitch          = 0.0;  // effective speed for LTC encoding
        double smoothVelocity = 0.0;  // dp/dt fallback for non-CDJ-3000
        double actualSpeed    = 0.0;  // real playback speed (offset 152) -- includes motor ramp
        bool   playing        = false; // CDJ play state (PLAYING/LOOPING/CUE_PLAY)
        bool   initialized    = false;
        bool   seekDetected   = false; // set when position jumps >500ms (seek/hot cue/track load)

        // Below this, encoder bits become absurdly long -> glitches
        static constexpr double kMinEncodingPitch = 0.08;  // ~-92%

        // Velocity below this = stopped
        static constexpr double kDeadZone = 0.005;  // 0.5% speed

        void reset()
        {
            positionMs     = 0.0;
            lastTickTime   = 0.0;
            lastPacketTs   = 0.0;
            lastCdjPos     = 0.0;
            pitch          = 0.0;
            smoothVelocity = 0.0;
            actualSpeed    = 0.0;
            playing        = false;
            seekDetected   = false;
            initialized    = false;
        }

        // State-aware PLL driven by CDJ actual speed:
        //
        //   Position input:
        //     CDJ-3000:   absolute playhead at 30Hz (type 0x0b) -- ms precision
        //     NXS2/older: beat-derived at ~5Hz (beatCount x 60000/BPM from status)
        //
        //   Drive velocity: actualSpeed from CDJ status (offset 152).
        //     Includes motor ramp, jog, pitch changes -- everything.
        //     Fallback to dp/dt if actualSpeed is 0 (non-CDJ-3000 models).
        //
        //   CDJ state determines correction strategy:
        //     PLAYING:                 PLL interpolates, graduated correction (8-25%)
        //     PAUSED but decelerating: PLL still drives (respects motor ramp)
        //     PAUSED and stopped:      snap to CDJ position (zero oscillation)
        //     END_OF_TRACK:            handled externally by pdlTcFrozen
        //
        //   Hard reset on >500ms error (seek/track change).

        void tick(uint32_t cdjPlayheadMs, double cdjPacketTs, double cdjActualSpeed,
                  bool cdjPlaying)
        {
            double now = juce::Time::getMillisecondCounterHiRes();
            double cdjPos = double(cdjPlayheadMs);

            if (!initialized)
            {
                positionMs     = cdjPos;
                lastTickTime   = now;
                lastPacketTs   = cdjPacketTs;
                lastCdjPos     = cdjPos;
                actualSpeed    = cdjActualSpeed;
                pitch          = 0.0;
                smoothVelocity = 0.0;
                playing        = cdjPlaying;
                initialized    = true;
                return;
            }

            // --- Advance free-running clock ---
            double elapsed = now - lastTickTime;
            lastTickTime = now;
            actualSpeed = cdjActualSpeed;
            playing     = cdjPlaying;

            // When actualSpeed confirms the player is stopped, decay the dp/dt
            // fallback toward zero.  Without this, noise in the NXS2 beat-derived
            // position can leave smoothVelocity just above kDeadZone, causing a
            // slow position creep even though the CDJ reports 0 speed.
            if (actualSpeed <= kDeadZone && std::abs(smoothVelocity) > 0.0)
                smoothVelocity *= 0.8;  // decays to <kDeadZone in ~5 ticks

            // Drive at CDJ's actual motor speed. This already includes:
            //   - Acceleration ramp on play start
            //   - Deceleration ramp on pause
            //   - Jog wheel adjustments
            //   - Pitch fader changes applied through motor
            // Fallback to dp/dt for models that don't report actualSpeed.
            double driveVelocity = (actualSpeed > kDeadZone) ? actualSpeed : smoothVelocity;

            // When CDJ is not playing AND velocity is near zero, we're truly stopped.
            // Don't interpolate -- just hold position and snap on next packet.
            // This eliminates micro-oscillation from timer jitter (especially macOS).
            bool trulyStopped = !playing && std::abs(driveVelocity) <= kDeadZone;

            if (!trulyStopped && std::abs(driveVelocity) > kDeadZone
                && elapsed > 0.0 && elapsed < 200.0)
                positionMs += elapsed * driveVelocity;

            // Pitch for LTC encoder
            double absVel = std::abs(driveVelocity);
            pitch = (absVel > kDeadZone) ? std::max(absVel, kMinEncodingPitch) : 0.0;

            // --- Correct against CDJ when new packet arrives ---
            if (cdjPacketTs != lastPacketTs && cdjPacketTs > 0.0)
            {
                double packetDt = cdjPacketTs - lastPacketTs;
                double posDelta = cdjPos - lastCdjPos;
                lastPacketTs = cdjPacketTs;
                lastCdjPos = cdjPos;

                // dp/dt measurement: kept as fallback for NXS2 and older models
                // that may not report actualSpeed in status packets.
                if (packetDt > 0.0 && packetDt < 200.0)
                {
                    double instantVel = posDelta / packetDt;
                    constexpr double kAlpha = 0.6;
                    smoothVelocity = smoothVelocity * (1.0 - kAlpha) + instantVel * kAlpha;
                    if (std::abs(smoothVelocity) < kDeadZone)
                        smoothVelocity = 0.0;
                }
                else
                {
                    smoothVelocity = 0.0;
                }

                // --- Position correction ---
                double error = cdjPos - positionMs;
                double absErr = std::abs(error);
                if (absErr > 500.0)
                {
                    // Seek or track change: snap immediately
                    positionMs = cdjPos;
                    seekDetected = true;  // signal engine to resync outputs
                }
                else if (trulyStopped)
                {
                    // Stopped: snap directly to CDJ position.
                    positionMs = cdjPos;
                }
                else
                {
                    // Playing or decelerating: graduated correction.
                    //   |error| 50-500ms -> 25% per packet (fast convergence)
                    //   |error| < 50ms   -> 8% per packet  (gentle, avoids overshoot)
                    double gain = (absErr > 50.0) ? 0.25 : 0.08;
                    positionMs += error * gain;
                }
            }

            if (positionMs < 0.0) positionMs = 0.0;
        }

        /// Apply beat grid micro-correction.  Between CDJ abspos packets the
        /// PLL interpolates at constant velocity and accumulates small drift.
        /// When a beat grid is available, nudge the PLL position toward the
        /// nearest beat by a small fraction each tick.  This keeps the LTC
        /// output phase-locked to the musical grid without sudden jumps.
        void beatGridCorrect(const std::vector<TrackMetadata::BeatEntry>& grid)
        {
            if (grid.empty() || !playing || positionMs < 1.0) return;

            // Binary search: find nearest beat to current position
            uint32_t posMs = (uint32_t)positionMs;
            int lo = 0, hi = (int)grid.size() - 1, best = 0;
            while (lo <= hi)
            {
                int mid = (lo + hi) / 2;
                if (grid[(size_t)mid].timeMs <= posMs)
                    { best = mid; lo = mid + 1; }
                else
                    hi = mid - 1;
            }

            // Check both the beat before and after current position
            double nearestMs = (double)grid[(size_t)best].timeMs;
            if (best + 1 < (int)grid.size())
            {
                double nextMs = (double)grid[(size_t)(best + 1)].timeMs;
                if (std::abs(nextMs - positionMs) < std::abs(nearestMs - positionMs))
                    nearestMs = nextMs;
            }

            double beatErr = nearestMs - positionMs;
            double absBeatErr = std::abs(beatErr);

            // Only correct if within 15ms of a beat (avoids correcting during
            // transitions between beats).  Apply 3% per tick -- gentle enough
            // to not fight the CDJ correction, strong enough to converge in
            // ~10 ticks (~330ms at 30Hz).
            if (absBeatErr > 0.5 && absBeatErr < 15.0)
                positionMs += beatErr * 0.03;
        }

        uint32_t getPositionMs() const { return uint32_t(juce::jmax(0.0, positionMs)); }
    };

    PlayheadPLL pll;
    Timecode pdlFrozenTc {};          // frozen timecode on end-of-track (prevents flicker)
    bool pdlTcFrozen = false;         // true = outputting frozen timecode
    uint32_t pdlLastPlayheadMs = 0;   // last CDJ playhead for change detection
    double pdlLastAbsPosTs = 0.0;     // last absPositionTs for NXS2 new-packet detection
    double pdlSnapMs = 0.0;           // playhead ms at last CDJ packet (interpolation anchor)
    double pdlSnapTime = 0.0;         // hi-res timestamp of last CDJ packet
    double pdlSnapSpeed = 1.0;        // actualSpeed at last CDJ packet

    // Beat grid for PLL micro-correction (from rekordbox via DbServerClient).
    // Between CDJ abspos packets, the PLL interpolates at constant velocity.
    // Small timing errors accumulate.  When a beat grid is available, the PLL
    // applies a gentle nudge toward the nearest beat position, reducing drift.
    std::vector<TrackMetadata::BeatEntry> pdlBeatGrid;
    uint32_t pdlBeatGridTrackId = 0;  // track ID for which beat grid is loaded

    LinkBridge   linkBridge;
    std::unique_ptr<AudioThru> audioThru;  // Only for primary engine

    // Audio BPM detection (for non-DJ sources: MTC, LTC, ArtNet, SystemTime)
    AudioBpmInput audioBpmInput;
    bool audioBpmEnabled = false;

    // Status
    juce::String inputStatusText = "SYSTEM CLOCK";
    juce::String mtcOutStatusText, artnetOutStatusText, ltcOutStatusText, thruOutStatusText, hippoOutStatusText;
    bool statusTextVisible = true;  // only build inputStatusText when engine is displayed

    // VU meter smoothed state
    float sLtcIn = 0.0f, sThruIn = 0.0f, sLtcOut = 0.0f, sThruOut = 0.0f;

    // TrackMap state (track-to-offset mapping)
    TrackMap* trackMapPtr       = nullptr;
    MixerMap* mixerMapPtr       = nullptr;
    MixerMap* slqMixerMapPtr    = nullptr;  // Denon StageLinQ mixer map
    bool      trackMapEnabled   = false;
    bool      trackMapped       = false;    // current track has a mapping
    bool      trackMapDirty     = false;    // auto-fill modified map, needs save
    bool      trackMapAutoFilled = false;   // UI-consumable: editor needs refresh
    uint32_t  cachedTrackId     = 0;        // currently tracked Track ID
    uint32_t  lastSeenTrackVersion = 0;     // per-engine version counter for track change detection
    int       cachedOffH = 0, cachedOffM = 0, cachedOffS = 0, cachedOffF = 0;
    juce::String cachedTrackArtist, cachedTrackTitle;
    int cachedTrackDurationSec = 0;

    // Track change triggers
    TriggerOutput triggerOutput;

    // MIDI Clock (BPM), OSC Forward (BPM)
    bool midiClockEnabled = false;     // MIDI Clock (24ppqn) for BPM
    float lastSentClockBpm = -1.0f;    // dedup: last BPM sent to clock

    // BPM multiplier: session-level per-player override (set by UI buttons, not persisted)
    // 0=1x (passthrough override), 1=x2, 2=x4, -1=/2, -2=/4, kBpmNoOverride(-99)=no override
    int  bpmPlayerOverride   = -99;  // = kBpmNoOverride (literal avoids forward-ref with MSVC)
    // BPM multiplier: cached from TrackMap entry on track change
    int  cachedBpmMultiplier = 0;

    bool oscForwardEnabled    = false;
    bool oscMixerForwardEnabled  = false;
    bool midiMixerForwardEnabled = false;
    bool artnetMixerForwardEnabled = false;
    int  midiMixerCCChannel      = 1;   // 1-16 (for CC messages)
    int  midiMixerNoteChannel    = 1;   // 1-16 (for Note messages)
    int  artnetMixerUniverse     = 0;   // 0-32767
    uint8_t dmxBuffer[512] {};          // persistent DMX frame for Art-Net mixer forward
    int  dmxHighWaterMark = 0;          // highest DMX channel ever written (persistent)
    double lastDmxSendTime = 0.0;       // for periodic re-send (DMX timeout compliance)

    // Track trigger Art-Net DMX (one-shot on track change, separate from mixer forward)
    uint8_t trigDmxBuffer[512] {};      // persistent: previous trigger values stay until overwritten
    int  trigDmxHighWater = 0;
    int  artnetTriggerUniverse = 1;     // default universe 1 (separate from mixer universe 0)
    bool artnetTriggerEnabled = false;  // must be enabled for Art-Net DMX track triggers to fire

    // --- Cue point firing state ---
    // Armed cue points for the currently loaded track.  Loaded from TrackMap
    // on track change, sorted by positionMs.  Each has a `fired` flag that is
    // set when the playhead crosses the cue and reset on track change or seek.
    struct ArmedCue
    {
        CuePoint cue;           // copy of the cue point data (trigger config)
        bool     fired = false;
    };
    std::vector<ArmedCue> armedCues;
    uint32_t lastCueCheckMs = 0;   // last playhead position used for cue check (seek detection)
    juce::String oscFwdBpmAddr = "/composition/tempocontroller/tempo";
    juce::String oscFwdBpmCmd;  // e.g. "Master 3.x at %BPM%" -- if non-empty, sends string instead of float
    float lastSentOscBpm = -1.0f;      // dedup: last sent OSC value

    static constexpr float kOscBpmThreshold = 0.05f;   // 0.05 BPM

    // Mixer fader dedup (last sent values, -1 = never sent)
    // MixerMap-driven dedup: one slot per MixerMap entry (max 64)
    static constexpr int kMaxMixerEntries = 128;  // 6ch x 13 params + ~45 globals
    int lastSentMixer[kMaxMixerEntries];  // initialised to -1 in constructor/reset
    uint32_t lastMixerPktCount = 0;      // for dirty-flag skip in forwardMixerParams

    // StageLinQ mixer dedup: 4 faders + 1 crossfader (0-255 scaled)
    static constexpr int kSlqMixerSlots = 5;  // CH1..CH4 fader + crossfader
    int lastSentSlqMixer[kSlqMixerSlots] = { -1, -1, -1, -1, -1 };

public:
    void setMidiClockEnabled(bool enabled)
    {
        midiClockEnabled = enabled;
        if (enabled && triggerOutput.isMidiOpen())
        {
            float bpm = 0.0f;
            if (activeInput == InputSource::StageLinQ && sharedStageLinQ != nullptr)
                bpm = (float)sharedStageLinQ->getBPM(proDJLinkPlayer);
            else if (activeInput == InputSource::ProDJLink && sharedProDJLink != nullptr)
            {
                bpm = (float)sharedProDJLink->getMasterBPM();
                if (bpm <= 0.0f) bpm = (float)sharedProDJLink->getBPM(getEffectivePlayer());
            }
            // Fallback: audio BPM detection (non-DJ sources)
            if (bpm <= 0.0f && audioBpmEnabled && audioBpmInput.hasBpm())
                bpm = (float)audioBpmInput.getBpm();
            triggerOutput.startMidiClock(bpm > 0.0f ? (double)bpm : 120.0);
        }
        else
        {
            triggerOutput.stopMidiClock();
        }
        lastSentClockBpm = -1.0f;
    }
    bool isMidiClockEnabled() const { return midiClockEnabled; }

    // Sentinel value: "no session override active" (falls through to TrackMap).
    static constexpr int kBpmNoOverride = -99;

    // BPM multiplier -- session-level per-player override (not persisted).
    // Valid values: 0=1x (passthrough), 1=x2, 2=x4, -1=/2, -2=/4.
    // Use kBpmNoOverride to clear the override (falls through to TrackMap).
    // Session override takes priority over TrackMap value.
    void setBpmPlayerOverride(int mult)
    {
        if (mult == kBpmNoOverride)
        {
            bpmPlayerOverride = kBpmNoOverride;
        }
        else
        {
            static const int kValid[] = { -2, -1, 0, 1, 2 };
            bool ok = false;
            for (int v : kValid) if (v == mult) { ok = true; break; }
            bpmPlayerOverride = ok ? mult : kBpmNoOverride;
        }
        lastSentClockBpm = -1.0f;   // force resend
        lastSentOscBpm   = -1.0f;
    }
    int getBpmPlayerOverride() const { return bpmPlayerOverride; }

    // Allows external code (e.g. ProDJLinkView double-click save) to sync the
    // cached TrackMap value without waiting for the next lookupTrackInMap call.
    void setCachedBpmMultiplier(int mult)
    {
        cachedBpmMultiplier = mult;
        lastSentClockBpm = -1.0f;
        lastSentOscBpm   = -1.0f;
    }

    /// Set the beat grid for PLL micro-correction.  Called when rekordbox
    /// beat grid data is downloaded from the CDJ.  The grid is cleared
    /// automatically on track change.
    void setBeatGrid(const std::vector<TrackMetadata::BeatEntry>& grid, uint32_t trackId)
    {
        if (trackId == pdlBeatGridTrackId && !pdlBeatGrid.empty()) return;
        pdlBeatGrid = grid;
        pdlBeatGridTrackId = trackId;
    }

    void clearBeatGrid()
    {
        pdlBeatGrid.clear();
        pdlBeatGridTrackId = 0;
    }

    // Returns the effective multiplier: session override if set, else TrackMap value.
    int getEffectiveBpmMultiplier() const
    {
        return (bpmPlayerOverride != kBpmNoOverride) ? bpmPlayerOverride : cachedBpmMultiplier;
    }

    // Helper: apply effective multiplier to a raw BPM value
    static double applyBpmMultiplier(double bpm, int mult)
    {
        switch (mult)
        {
            case  1: return bpm * 2.0;
            case  2: return bpm * 4.0;
            case -1: return bpm * 0.5;
            case -2: return bpm * 0.25;
            default: return bpm;
        }
    }

    void setOscForward(bool enabled,
                       const juce::String& bpmAddr = "/composition/tempocontroller/tempo",
                       const juce::String& bpmCmd = {})
    {
        oscForwardEnabled = enabled;
        oscFwdBpmAddr = bpmAddr;
        oscFwdBpmCmd = bpmCmd;
        lastSentOscBpm = -1.0f;
    }
    bool isOscForwardEnabled() const { return oscForwardEnabled; }
    juce::String getOscFwdBpmAddr() const { return oscFwdBpmAddr; }
    juce::String getOscFwdBpmCmd() const  { return oscFwdBpmCmd; }

    // Send BPM via OSC: if command template is set, send as string; otherwise as float
    void sendBpmOsc(double bpm)
    {
        if (!oscForwardEnabled || !triggerOutput.isOscConnected() || bpm <= 0.0) return;
        if (std::abs((float)bpm - lastSentOscBpm) < kOscBpmThreshold) return;

        if (oscFwdBpmCmd.isNotEmpty())
        {
            // Template mode: replace %BPM% with actual value
            auto cmd = oscFwdBpmCmd.replace("%BPM%", juce::String(bpm, 1));
            triggerOutput.sendOscString(oscFwdBpmAddr, cmd);
        }
        else
        {
            triggerOutput.sendOscFloat(oscFwdBpmAddr, (float)bpm);
        }
        lastSentOscBpm = (float)bpm;
    }

    void setOscMixerForward(bool enabled)
    {
        oscMixerForwardEnabled = enabled;
        std::fill(std::begin(lastSentMixer), std::end(lastSentMixer), -1); lastMixerPktCount = 0;
    }
    bool isOscMixerForwardEnabled() const { return oscMixerForwardEnabled; }

    void setMidiMixerForward(bool enabled, int ccChannel = 1, int noteChannel = 1)
    {
        midiMixerForwardEnabled = enabled;
        midiMixerCCChannel = juce::jlimit(1, 16, ccChannel);
        midiMixerNoteChannel = juce::jlimit(1, 16, noteChannel);
        std::fill(std::begin(lastSentMixer), std::end(lastSentMixer), -1); lastMixerPktCount = 0;
    }
    bool isMidiMixerForwardEnabled() const { return midiMixerForwardEnabled; }
    int  getMidiMixerCCChannel()     const { return midiMixerCCChannel; }
    int  getMidiMixerNoteChannel()   const { return midiMixerNoteChannel; }

    void setArtnetMixerForward(bool enabled, int universe = 0)
    {
        artnetMixerForwardEnabled = enabled;
        artnetMixerUniverse = juce::jlimit(0, 32767, universe);
        std::fill(std::begin(lastSentMixer), std::end(lastSentMixer), -1); lastMixerPktCount = 0;
        std::memset(dmxBuffer, 0, sizeof(dmxBuffer));
        dmxHighWaterMark = 0;
        lastDmxSendTime = 0.0;
    }
    bool isArtnetMixerForwardEnabled() const { return artnetMixerForwardEnabled; }
    int  getArtnetMixerUniverse()      const { return artnetMixerUniverse; }

    void setArtnetTriggerUniverse(int universe) { artnetTriggerUniverse = juce::jlimit(0, 32767, universe); }
    int  getArtnetTriggerUniverse() const       { return artnetTriggerUniverse; }

    void setArtnetTriggerEnabled(bool enabled) { artnetTriggerEnabled = enabled; }
    bool isArtnetTriggerEnabled() const        { return artnetTriggerEnabled; }

    //----------------------------------------------------------------------
    // Ableton Link -- BPM sync to Link session
    //----------------------------------------------------------------------
    LinkBridge& getLinkBridge() { return linkBridge; }
    const LinkBridge& getLinkBridge() const { return linkBridge; }
private:

    /// Request metadata from dbserver for a track on the current ProDJLink player.
    /// Encapsulates source-player discovery, dbCtx collision avoidance, and model propagation.
    void requestDbMetadata(uint32_t trackId)
    {
        if (dbClient == nullptr || !dbClient->getIsRunning()) return;
        if (sharedProDJLink == nullptr || !sharedProDJLink->getIsRunning()) return;

        int effP = getEffectivePlayer();
        if (effP < 1) return;

        uint8_t srcPlayer = sharedProDJLink->getLoadedPlayer(effP);
        if (srcPlayer == 0) srcPlayer = (uint8_t)effP;
        juce::String srcIP = sharedProDJLink->getPlayerIP((int)srcPlayer);
        uint8_t slot = sharedProDJLink->getLoadedSlot(effP);

        if (srcIP.isEmpty() || slot == 0)
        {
            DBG("TimecodeEngine: SKIPPED metadata -- srcIP empty or slot=0");
            return;
        }

        // Choose dbserver query identity.
        // CDJ-3000: accepts player 5 (our VCDJ number) — use it directly.
        // NXS2/older: rejects player 5, requires 1-4 that's present on the
        // network.  Use suggestDbPlayerNumber() to find a valid candidate.
        int dbCtx;
        bool srcHasAbsPos = sharedProDJLink->playerHasAbsolutePosition((int)srcPlayer);
        if (srcHasAbsPos)
        {
            dbCtx = sharedProDJLink->getVCDJPlayerNumber();
            // Safety: if VCDJ coincidentally matches source, pick another
            if (dbCtx == (int)srcPlayer)
                dbCtx = (srcPlayer != 1) ? 1 : 2;
        }
        else
        {
            // NXS2: suggestDbPlayerNumber already picks a valid 1-4 excluding
            // srcPlayer.  If it returns srcPlayer (last resort: only player on
            // network), that's the best we can do — don't override to a
            // non-existent player.
            dbCtx = sharedProDJLink->suggestDbPlayerNumber((int)srcPlayer);
            if (dbCtx == 0)
            {
                DBG("TimecodeEngine: SKIPPED metadata -- no valid player 1-4 for NXS2 query");
                return;
            }
        }

        juce::String model = sharedProDJLink->getPlayerModel((int)srcPlayer);

        DBG("TimecodeEngine: metadata request -- srcPlayer=" + juce::String(srcPlayer)
            + " srcIP=" + srcIP + " slot=" + juce::String(slot)
            + " dbCtx=" + juce::String(dbCtx) + " model=" + model);

        dbClient->requestMetadata(srcIP, slot, 1, trackId, dbCtx, model);
    }

    /// Lookup a track in the TrackMap by artist+title and cache the offset values.
    /// Called when a track change is detected or metadata is resolved.
    /// Returns the matched entry pointer (or nullptr if not found/malformed).
    const TrackMapEntry* lookupTrackInMap()
    {
        if (!trackMapPtr || cachedTrackTitle.isEmpty())
        {
            trackMapped = false;
            return nullptr;
        }

        auto* entry = trackMapPtr->find(cachedTrackArtist, cachedTrackTitle,
                                        cachedTrackDurationSec);

        // Duration fallback: when duration resolves late (deferred pickup),
        // the key changes from "artist|title" to "artist|title|300".  If the
        // user's entry was saved without duration (legacy or manual add),
        // the duration-aware lookup misses.  Fall back to duration=0 so we
        // don't lose the TrackMap match (and its armed cue points).
        if (!entry && cachedTrackDurationSec > 0)
            entry = trackMapPtr->find(cachedTrackArtist, cachedTrackTitle, 0);

        // Last resort: ignore duration entirely.  Catches the case where the
        // entry was saved with a duration that differs from the engine's cached
        // duration (e.g. PDL View saved with CDJ-reported duration while the
        // engine's cachedTrackDurationSec was still 0 or stale from an earlier
        // enrichment pass).
        if (!entry)
            entry = trackMapPtr->findIgnoringDuration(cachedTrackArtist, cachedTrackTitle);
        if (entry)
        {
            int h, m, s, f;
            if (TrackMapEntry::parseTimecodeString(entry->timecodeOffset, h, m, s, f))
            {
                cachedOffH = h;
                cachedOffM = m;
                cachedOffS = s;
                cachedOffF = f;
                trackMapped = true;

                cachedBpmMultiplier = entry->bpmMultiplier;
                lastSentClockBpm = -1.0f;
                lastSentOscBpm   = -1.0f;
                return entry;
            }
            else
            {
                trackMapped = false;
                cachedOffH = cachedOffM = cachedOffS = cachedOffF = 0;
                cachedBpmMultiplier = 0;
                return nullptr;
            }
        }
        else
        {
            trackMapped = false;
            cachedOffH = cachedOffM = cachedOffS = cachedOffF = 0;
            cachedBpmMultiplier = 0;
            return nullptr;
        }
    }

    /// Fire track-change triggers (MIDI/OSC/Art-Net DMX) for a TrackMap entry.
    /// Null-safe: does nothing if entry is nullptr or has no triggers.
    /// Art-Net DMX channel is bounds-checked to [1,512] before buffer access.
    void fireTrackTrigger(const TrackMapEntry* entry)
    {
        if (!entry || !entry->hasAnyTrigger()) return;

        triggerOutput.fire(*entry);

        if (artnetTriggerEnabled && entry->hasArtnetTrigger() && artnetOutput.getIsRunning())
        {
            int ch = entry->artnetCh;
            if (ch > 0 && ch <= 512)
            {
                trigDmxBuffer[ch - 1] = uint8_t(entry->artnetVal);
                if (ch > trigDmxHighWater) trigDmxHighWater = ch;
                artnetOutput.sendDmxFrame(trigDmxBuffer, trigDmxHighWater,
                                          artnetTriggerUniverse);
            }
        }
    }

    //--------------------------------------------------------------------------
    // Cue point management
    //--------------------------------------------------------------------------

    /// Load cue points from the TrackMap entry for the current track.
    /// Called on track change after fireTrackTrigger.  Resets all fired flags.
    void loadCuePointsForTrack(const TrackMapEntry* entry)
    {
        armedCues.clear();
        lastCueCheckMs = 0;

        if (!entry || entry->cuePoints.empty()) return;

        armedCues.reserve(entry->cuePoints.size());
        for (auto& cp : entry->cuePoints)
        {
            ArmedCue ac;
            ac.cue = cp;       // copy trigger data
            ac.fired = false;
            armedCues.push_back(std::move(ac));
        }
        // Guarantee sorted by position
        std::sort(armedCues.begin(), armedCues.end(),
                  [](const ArmedCue& a, const ArmedCue& b) {
                      return a.cue.positionMs < b.cue.positionMs;
                  });
    }

    /// Check playhead against armed cue points and fire triggers.
    /// Called from tick() with the current playhead in ms.
    /// Handles forward playback, seek forward, and seek backward.
    void tickCuePoints(uint32_t playheadMs)
    {
        if (armedCues.empty()) return;

        // Detect seek: playhead jumped backward or jumped forward more than 500ms
        // beyond what normal playback would produce (60Hz tick = ~17ms advance)
        bool seekDetected = (playheadMs < lastCueCheckMs)
                         || (playheadMs > lastCueCheckMs + 500);

        if (seekDetected)
        {
            // Reset fired flags: un-fire cues that are ahead of new playhead,
            // mark cues behind new playhead as already fired (don't re-trigger
            // cues we've passed)
            for (auto& ac : armedCues)
                ac.fired = (ac.cue.positionMs < playheadMs);
        }

        // Fire cues whose position the playhead has crossed
        for (auto& ac : armedCues)
        {
            if (ac.fired) continue;
            if (ac.cue.positionMs > playheadMs) break;  // sorted: no more to check

            // Playhead has crossed this cue -- fire it
            ac.fired = true;

            triggerOutput.fireCuePoint(ac.cue);

            // Art-Net DMX trigger (same pattern as track change triggers)
            if (artnetTriggerEnabled && ac.cue.hasArtnetTrigger() && artnetOutput.getIsRunning())
            {
                int ch = ac.cue.artnetCh;
                if (ch > 0 && ch <= 512)
                {
                    trigDmxBuffer[ch - 1] = uint8_t(ac.cue.artnetVal);
                    if (ch > trigDmxHighWater) trigDmxHighWater = ch;
                    artnetOutput.sendDmxFrame(trigDmxBuffer, trigDmxHighWater,
                                              artnetTriggerUniverse);
                }
            }

            DBG("TimecodeEngine: Cue fired '" + ac.cue.name + "' at "
                + CuePoint::formatPositionMs(ac.cue.positionMs)
                + " (playhead=" + CuePoint::formatPositionMs(playheadMs) + ")");
        }

        lastCueCheckMs = playheadMs;
    }

    //--------------------------------------------------------------------------
    void updateGenerator()
    {
        // Clock mode: read wall clock directly (old SystemTime behavior)
        if (genClockMode)
        {
            auto now = juce::Time::getCurrentTime();
            double msSinceMidnight = (double)now.getHours() * 3600000.0
                                   + (double)now.getMinutes() * 60000.0
                                   + (double)now.getSeconds() * 1000.0
                                   + (double)now.getMilliseconds();
            currentTimecode = wallClockToTimecode(msSinceMidnight, currentFps);
            return;
        }

        // Generator mode: transport-controlled
        if (genState == GeneratorState::Playing)
        {
            double now = juce::Time::getMillisecondCounterHiRes();
            double delta = now - genLastTickTime;
            genLastTickTime = now;
            if (delta > 0.0)
                genCurrentMs += delta;

            // Auto-stop at stop TC (if set and not zero)
            if (genStopMs > 0.0 && genCurrentMs >= genStopMs)
            {
                genCurrentMs = genStopMs;
                genState = GeneratorState::Stopped;
            }
        }
        // Stopped and Paused: genCurrentMs stays where it is
        currentTimecode = wallClockToTimecode(genCurrentMs, currentFps);
    }

    //==========================================================================
    // MixerMap-driven parameter forwarding
    //
    // Reads all DJM mixer values, compares with lastSentMixer[], and sends
    // changed values via OSC and/or MIDI CC using the addresses and CC numbers
    // configured in the MixerMap.
    //==========================================================================
    void forwardMixerParams()
    {
        if (!sharedProDJLink || !mixerMapPtr) return;

        const bool doArtnet = artnetMixerForwardEnabled && artnetOutput.getIsRunning();
        bool dmxDirty = false;

        // Only iterate mixer values when a new 0x39 packet has arrived.
        // At 60Hz tick rate with ~5Hz mixer packets, this skips ~92% of iterations.
        uint32_t currentMixerPktCount = sharedProDJLink->getMixerPacketCount();
        if (currentMixerPktCount != lastMixerPktCount)
        {
            lastMixerPktCount = currentMixerPktCount;

            const int n = std::min(mixerMapPtr->size(), kMaxMixerEntries);
            const auto& entries = mixerMapPtr->getEntries();
            const bool doOsc    = oscMixerForwardEnabled && triggerOutput.isOscConnected();
            const bool doMidi   = midiMixerForwardEnabled && triggerOutput.isMidiOpen();
            const int ccCh      = midiMixerCCChannel;
            const int noteCh    = midiMixerNoteChannel;

            for (int i = 0; i < n; ++i)
            {
                const auto& e = entries[(size_t)i];
                if (!e.enabled) continue;

                int val = readMixerValue(i);
                if (val < 0 || val == lastSentMixer[i]) continue;

                // Map raw value according to parameter type:
                //   Continuous (faders, knobs):  OSC 0.0-1.0, CC 0-127, DMX 0-255
                //   Toggle     (on/off buttons): OSC 0.0/1.0, CC 0/127, DMX 0/255
                //   Discrete   (select, assign): OSC integer,  CC clamp, DMX raw
                float oscVal;
                int   midiVal;
                int   dmxVal;
                switch (e.paramType)
                {
                    case ParamType::Toggle:
                        oscVal  = val > 0 ? 1.0f : 0.0f;
                        midiVal = val > 0 ? 127 : 0;
                        dmxVal  = val > 0 ? 255 : 0;
                        break;
                    case ParamType::Discrete:
                        oscVal  = (float)val;          // integer as float (e.g. 3.0 for BeatFX #3)
                        midiVal = juce::jlimit(0, 127, val);
                        dmxVal  = val;
                        break;
                    default: // Continuous
                        oscVal  = val / 255.0f;
                        midiVal = val >> 1;            // 0-255 -> 0-127
                        dmxVal  = val;
                        break;
                }

                if (doOsc && e.oscAddress.isNotEmpty())
                    triggerOutput.sendOscFloat(e.oscAddress, oscVal);

                if (doMidi && e.midiCC >= 0)
                    triggerOutput.sendCC(ccCh, e.midiCC, midiVal);

                if (doMidi && e.midiNote >= 0)
                    triggerOutput.sendNote(noteCh, e.midiNote, midiVal);

                if (doArtnet && e.artnetCh > 0 && e.artnetCh <= 512)
                {
                    dmxBuffer[e.artnetCh - 1] = uint8_t(dmxVal);
                    dmxDirty = true;
                    if (e.artnetCh > dmxHighWaterMark)
                        dmxHighWaterMark = e.artnetCh;
                }

                lastSentMixer[i] = val;
            }
        }

        // Art-Net DMX re-send: runs even when no new mixer data to prevent
        // Art-Net node DMX timeout (some nodes blackout after 2-3s without data).
        if (doArtnet && dmxHighWaterMark > 0)
        {
            double now = juce::Time::getMillisecondCounterHiRes();
            if (dmxDirty || (now - lastDmxSendTime) >= 100.0)
            {
                artnetOutput.sendDmxFrame(dmxBuffer, dmxHighWaterMark, artnetMixerUniverse);
                lastDmxSendTime = now;
            }
        }
    }

    //==========================================================================
    // Forward StageLinQ mixer data (faders + crossfader) via OSC/MIDI/ArtNet.
    //
    // StageLinQ provides channel faders (0.0-1.0) and crossfader (0.0-1.0)
    // directly from StateMap. No MixerMap needed -- we use built-in addresses.
    //
    // OSC:    /mixer/ch1/fader .. /mixer/ch4/fader, /mixer/crossfader  (0.0-1.0)
    // MIDI:   CC 1-4 = faders, CC 5 = crossfader  (0-127)
    // ArtNet: DMX ch 1-4 = faders, ch 5 = crossfader  (0-255)
    //
    // When more mixer paths are discovered from hardware (EQ, effects, etc.),
    // they can be added here without requiring a MixerMap editor.
    //==========================================================================
    void forwardStageLinQMixer()
    {
        if (!sharedStageLinQ || !slqMixerMapPtr) return;

        const bool doOsc    = oscMixerForwardEnabled && triggerOutput.isOscConnected();
        const bool doMidi   = midiMixerForwardEnabled && triggerOutput.isMidiOpen();
        const bool doArtnet = artnetMixerForwardEnabled && artnetOutput.getIsRunning();
        if (!doOsc && !doMidi && !doArtnet) return;

        bool dmxDirty = false;
        const int ccCh   = midiMixerCCChannel;
        const int noteCh = midiMixerNoteChannel;
        const int n = juce::jmin(slqMixerMapPtr->size(), kSlqMixerSlots);
        const auto& entries = slqMixerMapPtr->getEntries();

        for (int i = 0; i < n; ++i)
        {
            const auto& e = entries[(size_t)i];
            if (!e.enabled) continue;

            int val = readSlqMixerValue(i);
            if (val < 0 || val == lastSentSlqMixer[i]) continue;

            if (doOsc && e.oscAddress.isNotEmpty())
                triggerOutput.sendOscFloat(e.oscAddress, val / 255.0f);

            if (doMidi && e.midiCC >= 0)
                triggerOutput.sendCC(ccCh, e.midiCC, val >> 1);

            if (doMidi && e.midiNote >= 0)
                triggerOutput.sendNote(noteCh, e.midiNote, val >> 1);

            if (doArtnet && e.artnetCh > 0 && e.artnetCh <= 512)
            {
                dmxBuffer[e.artnetCh - 1] = (uint8_t)val;
                dmxDirty = true;
                if (e.artnetCh > dmxHighWaterMark)
                    dmxHighWaterMark = e.artnetCh;
            }

            lastSentSlqMixer[i] = val;
        }

        // Art-Net DMX re-send (prevent node timeout)
        if (doArtnet && dmxHighWaterMark > 0)
        {
            double now = juce::Time::getMillisecondCounterHiRes();
            if (dmxDirty || (now - lastDmxSendTime) >= 100.0)
            {
                artnetOutput.sendDmxFrame(dmxBuffer, dmxHighWaterMark, artnetMixerUniverse);
                lastDmxSendTime = now;
            }
        }
    }

    /// Read a StageLinQ mixer value (0-255) by MixerMap entry index.
    /// Entry order matches buildDenonDefaults():
    ///   [0] CH1 fader, [1] CH2 fader, [2] CH3 fader, [3] CH4 fader, [4] crossfader
    int readSlqMixerValue(int entryIndex) const
    {
        if (!sharedStageLinQ) return -1;
        if (entryIndex >= 0 && entryIndex < 4)
            return juce::jlimit(0, 255, (int)(sharedStageLinQ->getFaderPosition(entryIndex + 1) * 255.0));
        if (entryIndex == 4)
            return juce::jlimit(0, 255, (int)(sharedStageLinQ->getCrossfaderPosition() * 255.0));
        return -1;
    }

    /// Read a DJM mixer parameter value (0-255) by MixerMap entry index.
    /// Returns -1 if the index is out of range.
    /// Entry order matches MixerMap::buildDefaults():
    ///   [0-12]  CH1: fader,trim,eq_hi,eq_mid,eq_lo,color,cue,input_src,xf_assign,
    ///                 comp,eq_lo_mid,send,cue_b
    ///   [13-25] CH2, [26-38] CH3, [39-51] CH4, [52-64] CH5, [65-77] CH6
    ///   [78+]   globals (crossfader, master, monitor, fx, sends, isolator, etc.)
    int readMixerValue(int entryIndex) const
    {
        if (!sharedProDJLink) return -1;
        auto& pdl = *sharedProDJLink;

        // Per-channel params (13 per channel, up to 6 channels)
        static constexpr int kParamsPerCh = 13;
        static constexpr int kChBlock = kParamsPerCh * ProDJLink::kMaxMixerChannels;  // 78

        if (entryIndex < kChBlock)
        {
            int ch = (entryIndex / kParamsPerCh) + 1;   // 1-6
            int p  =  entryIndex % kParamsPerCh;         // 0-12
            switch (p)
            {
                case 0: return pdl.getChannelFader(ch);
                case 1: return pdl.getChannelTrim(ch);
                case 2: return pdl.getChannelEqHi(ch);
                case 3: return pdl.getChannelEqMid(ch);
                case 4: return pdl.getChannelEqLo(ch);
                case 5: return pdl.getChannelColor(ch);
                case 6: return pdl.getChannelCue(ch);
                case 7: return pdl.getChannelInputSrc(ch);
                case 8: return pdl.getChannelXfAssign(ch);
                // V10 per-channel (0 on 900NXS2)
                case  9: return pdl.getChannelComp(ch);
                case 10: return pdl.getChannelEqLoMid(ch);
                case 11: return pdl.getChannelSend(ch);
                case 12: return pdl.getChannelCueB(ch);
                default: return -1;
            }
        }

        // Global params (indices kChBlock+)
        int gi = entryIndex - kChBlock;
        switch (gi)
        {
            case  0: return pdl.getCrossfader();
            case  1: return pdl.getMasterFader();
            case  2: return pdl.getMasterCue();
            case  3: return pdl.getFaderCurve();
            case  4: return pdl.getXfCurve();
            case  5: return pdl.getBoothLevel();
            case  6: return pdl.getHpCueLink();
            case  7: return pdl.getHpMixing();
            case  8: return pdl.getHpLevel();
            case  9: return pdl.getBeatFxSelect();
            case 10: return pdl.getBeatFxLevel();
            case 11: return pdl.getBeatFxOn();
            case 12: return pdl.getBeatFxAssign();
            case 13: return pdl.getFxFreqLo();
            case 14: return pdl.getFxFreqMid();
            case 15: return pdl.getFxFreqHi();
            case 16: return pdl.getSendReturnLevel();
            case 17: return pdl.getColorFxSelect();
            case 18: return pdl.getColorFxParam();
            case 19: return pdl.getColorFxAssign();
            case 20: return pdl.getMicEqHi();
            case 21: return pdl.getMicEqLo();
            // V10 globals (0 on 900NXS2)
            // A9+ globals (A9 and V10: dual CUE, HP B, booth EQ)
            case 22: return pdl.getMasterCueB();
            case 23: return pdl.getHpCueLinkB();
            case 24: return pdl.getHpMixingB();
            case 25: return pdl.getHpLevelB();
            case 26: return pdl.getBoothEqHi();
            case 27: return pdl.getBoothEqLo();
            case 28: return pdl.getBoothEq();
            // V10-only globals
            case 29: return pdl.getIsolatorOn();
            case 30: return pdl.getIsolatorHi();
            case 31: return pdl.getIsolatorMid();
            case 32: return pdl.getIsolatorLo();
            case 33: return pdl.getFilterLPF();
            case 34: return pdl.getFilterHPF();
            case 35: return pdl.getFilterResonance();
            case 36: return pdl.getSendExt1();
            case 37: return pdl.getSendExt2();
            case 38: return pdl.getMasterMixOn();
            case 39: return pdl.getMasterMixSize();
            case 40: return pdl.getMasterMixTime();
            case 41: return pdl.getMasterMixTone();
            case 42: return pdl.getMasterMixLevel();
            case 43: return pdl.getMultiIoSelect();
            case 44: return pdl.getMultiIoLevel();
        }
        return -1;
    }

    void routeTimecodeToOutputs()
    {
        FrameRate outRate = getEffectiveOutputFps();
        Timecode baseTc = fpsConvertEnabled
                        ? convertTimecodeRate(currentTimecode, currentFps, outRate)
                        : currentTimecode;
        outputTimecode = baseTc;

        bool wasActive = outputsWereActive;
        outputsWereActive = sourceActive;

        if (sourceActive)
        {
            if (outputMtcEnabled && mtcOutput.getIsRunning())
            {
                mtcOutput.setTimecode(offsetTimecode(baseTc, mtcOutputOffset, outRate));
                mtcOutput.setPaused(false);
            }
            if (outputArtnetEnabled && artnetOutput.getIsRunning())
            {
                artnetOutput.setTimecode(offsetTimecode(baseTc, artnetOutputOffset, outRate));
                artnetOutput.setPaused(false);
            }
            if (outputLtcEnabled && ltcOutput.getIsRunning())
            {
                ltcOutput.setTimecode(offsetTimecode(baseTc, ltcOutputOffset, outRate));
                ltcOutput.setPaused(false);
            }
            if (outputHippoEnabled && hippotizerOutput.getIsRunning())
            {
                hippotizerOutput.setTimecode(baseTc);
                hippotizerOutput.setPaused(false);
            }

            // --- Seek/Hot Cue resync ---
            // PLL detected a position jump >500ms (seek, hot cue, track load).
            // Force immediate resync on all digital outputs so receivers know
            // the new position instantly:
            //   MTC:    Full Frame message (instant vs 8 QFs = 2 frames)
            //   ArtNet: immediate frame (vs waiting for next timer tick)
            //   LTC:    reseed encoder (clean frame start at new position)
            if (pll.seekDetected)
            {
                pll.seekDetected = false;
                if (outputMtcEnabled && mtcOutput.getIsRunning())
                    mtcOutput.forceResync();
                if (outputArtnetEnabled && artnetOutput.getIsRunning())
                    artnetOutput.forceResync();
                if (outputLtcEnabled && ltcOutput.getIsRunning())
                    ltcOutput.reseed();
                if (outputHippoEnabled && hippotizerOutput.getIsRunning())
                    hippotizerOutput.forceResync();
            }
        }
        else
        {
            // --- Clean stop ---
            // On active->inactive transition, send one final frame at the
            // stopped position before pausing. This gives receivers the exact
            // stop point instead of leaving them with an incomplete QF cycle
            // or stale position.
            if (wasActive)
            {
                if (outputMtcEnabled && mtcOutput.getIsRunning())
                {
                    mtcOutput.setTimecode(offsetTimecode(baseTc, mtcOutputOffset, outRate));
                    mtcOutput.sendFullFrame();
                }
                if (outputArtnetEnabled && artnetOutput.getIsRunning())
                {
                    artnetOutput.setTimecode(offsetTimecode(baseTc, artnetOutputOffset, outRate));
                    artnetOutput.forceResync();
                }
                // LTC: set final timecode so encoder finishes current frame cleanly
                if (outputLtcEnabled && ltcOutput.getIsRunning())
                    ltcOutput.setTimecode(offsetTimecode(baseTc, ltcOutputOffset, outRate));
                if (outputHippoEnabled && hippotizerOutput.getIsRunning())
                {
                    hippotizerOutput.setTimecode(baseTc);
                    hippotizerOutput.forceResync();
                }
            }

            // Clear seek flag if it was set during transition to inactive
            pll.seekDetected = false;

            if (outputMtcEnabled && mtcOutput.getIsRunning()) mtcOutput.setPaused(true);
            if (outputArtnetEnabled && artnetOutput.getIsRunning()) artnetOutput.setPaused(true);
            if (outputLtcEnabled && ltcOutput.getIsRunning()) ltcOutput.setPaused(true);
            if (outputHippoEnabled && hippotizerOutput.getIsRunning()) hippotizerOutput.setPaused(true);
        }
    }

    void updateVuMeters()
    {
        auto decayLevel = [](float current, float target, float decay = 0.85f) {
            return target > current ? target : current * decay;
        };

        float ltcInLvl  = ltcInput.getIsRunning()  ? ltcInput.getLtcPeakLevel()  : 0.0f;
        float thruInLvl = ltcInput.getIsRunning()  ? ltcInput.getThruPeakLevel() : 0.0f;
        float ltcOutLvl = (ltcOutput.getIsRunning() && !ltcOutput.isPaused()) ? ltcOutput.getPeakLevel() : 0.0f;
        float thruOutLvl = (audioThru && audioThru->getIsRunning()) ? audioThru->getPeakLevel() : 0.0f;

        sLtcIn  = decayLevel(sLtcIn,  ltcInLvl);
        sThruIn = decayLevel(sThruIn, thruInLvl);
        sLtcOut = decayLevel(sLtcOut, ltcOutLvl);
        sThruOut = decayLevel(sThruOut, thruOutLvl);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TimecodeEngine)
};
