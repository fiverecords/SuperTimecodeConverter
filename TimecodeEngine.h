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
#include "DbServerClient.h"
#include "TriggerOutput.h"
#include "LinkBridge.h"
#include "AudioThru.h"
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
    enum class InputSource { MTC, ArtNet, SystemTime, LTC, ProDJLink };

    //--------------------------------------------------------------------------
    explicit TimecodeEngine(int index, const juce::String& name = {})
        : engineIndex(index),
          engineName(name.isEmpty() ? ("ENGINE " + juce::String(index + 1)) : name)
    {
        std::fill(std::begin(lastSentMixer), std::end(lastSentMixer), -1);

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
        stopThruOutput();
        stopMtcInput();
        stopArtnetInput();
        stopLtcInput();
        // ProDJLink is shared -- not stopped per-engine
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
    Timecode getCurrentTimecode() const { return currentTimecode; }
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
            default: break;
        }

        userOverrodeLtcFps = false;
        activeInput = source;
        sourceActive = false;

        // Reset TrackMap cache when leaving ProDJLink
        if (source != InputSource::ProDJLink)
        {
            trackMapped = false;
            cachedTrackId = 0;
            cachedOffH = cachedOffM = cachedOffS = cachedOffF = 0;
            cachedTrackArtist.clear();
            cachedTrackTitle.clear();
            // Disable Link to avoid publishing stale tempo on the network
            linkBridge.setEnabled(false);
        }

        // Note: actual start is deferred to the caller (MainComponent),
        // which gathers device params from UI before calling startXxxInput().
        if (source == InputSource::SystemTime)
            sourceActive = true;
    }

    void setFrameRate(FrameRate fps)
    {
        currentFps = fps;
        FrameRate outRate = getEffectiveOutputFps();
        mtcOutput.setFrameRate(outRate);
        artnetOutput.setFrameRate(outRate);
        ltcOutput.setFrameRate(outRate);
    }

    void setUserOverrodeLtcFps(bool v) { userOverrodeLtcFps = v; }

    //==========================================================================
    // FPS conversion
    //==========================================================================
    bool isFpsConvertEnabled() const { return fpsConvertEnabled; }
    FrameRate getOutputFps() const { return outputFps; }
    Timecode getOutputTimecode() const { return outputTimecode; }

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

    int getMtcOutputOffset() const      { return mtcOutputOffset; }
    int getArtnetOutputOffset() const   { return artnetOutputOffset; }
    int getLtcOutputOffset() const      { return ltcOutputOffset; }

    void setMtcOutputOffset(int v)      { mtcOutputOffset = v; }
    void setArtnetOutputOffset(int v)   { artnetOutputOffset = v; }
    void setLtcOutputOffset(int v)      { ltcOutputOffset = v; }

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
    void setDbServerClient(DbServerClient* client) { dbClient = client; }
    int  getProDJLinkPlayer() const     { return proDJLinkPlayer; }
    void setProDJLinkPlayer(int p)      { proDJLinkPlayer = juce::jlimit(1, ProDJLink::kMaxPlayers, p); pll.reset(); pdlTcFrozen = false; }

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
    // Pro DJ Link input (shared across engines)
    //==========================================================================
    void resetProDJLinkCache()
    {
        trackMapped = false;
        cachedTrackId = 0;
        lastSeenTrackVersion = 0;
        pdlTickDiagDone = false;        cachedOffH = cachedOffM = cachedOffS = cachedOffF = 0;
        cachedTrackArtist.clear();
        cachedTrackTitle.clear();
        pll.reset(); pdlTcFrozen = false;
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

        // ProDJLink has no fps auto-detection -- use configured output fps
        currentFps = outputFps;

        // LTC direct mode is now toggled dynamically per tick
        // (direct in transient, auto-increment in stable)

        proDJLinkPlayer = juce::jlimit(1, ProDJLink::kMaxPlayers, player);

        if (sharedProDJLink != nullptr)
            lastSeenTrackVersion = sharedProDJLink->getTrackVersion(proDJLinkPlayer);

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
        pll.reset(); pdlTcFrozen = false;
        ltcOutput.setPitchMultiplier(1.0);
    }

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
        std::fill(std::begin(lastSentMixer), std::end(lastSentMixer), -1);
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
        }
        else if (trackMapPtr && activeInput == InputSource::ProDJLink
                 && sharedProDJLink != nullptr && sharedProDJLink->getIsRunning())
        {
            lastSeenTrackVersion = sharedProDJLink->getTrackVersion(proDJLinkPlayer);

            uint32_t id = (cachedTrackId != 0) ? cachedTrackId : sharedProDJLink->getTrackID(proDJLinkPlayer);
            if (id != 0)
            {
                cachedTrackId = id;
                auto tinfo = sharedProDJLink->getTrackInfo(proDJLinkPlayer);
                cachedTrackArtist = tinfo.artist;
                cachedTrackTitle  = tinfo.title;

                // Phase 2: check dbClient cache or request if missing
                if (dbClient != nullptr && dbClient->getIsRunning())
                {
                    auto meta = dbClient->getCachedMetadataByTrackId(id);
                    if (meta.isValid())
                    {
                        cachedTrackArtist = meta.artist;
                        cachedTrackTitle  = meta.title;
                        // Propagate track duration to player state (NXS2 needs this
                        // since it doesn't report duration in protocol packets)
                        if (meta.durationSeconds > 0 && sharedProDJLink != nullptr)
                            sharedProDJLink->setTrackLengthSec(proDJLinkPlayer,
                                (uint32_t)meta.durationSeconds);
                    }
                    else
                    {
                        requestDbMetadata(id);
                    }
                }

                lookupTrackInMap(id);
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
    };

    ActiveTrackInfo getActiveTrackInfo() const
    {
        ActiveTrackInfo info;
        info.trackId = cachedTrackId;
        info.artist  = cachedTrackArtist;
        info.title   = cachedTrackTitle;
        info.mapped  = trackMapped;
        if (trackMapped)
            info.offset = TrackMapEntry::formatTimecodeString(
                              cachedOffH, cachedOffM, cachedOffS, cachedOffF);

        // Phase 2: enrich with dbClient cache if available
        if (dbClient != nullptr && cachedTrackId != 0)
        {
            auto meta = dbClient->getCachedMetadataByTrackId(cachedTrackId);
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
                    info.artist = meta.artist;
                    info.title  = meta.title;
                }
            }
        }
        return info;
    }

    /// Force a re-lookup of the current track (e.g., after editing TrackMap)
    void refreshTrackMapLookup()
    {
        if (!trackMapPtr || cachedTrackId == 0) return;
        lookupTrackInMap(cachedTrackId);
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
        switch (activeInput)
        {
            case InputSource::SystemTime:
                updateSystemTime();
                sourceActive = true;
                inputStatusText = "SYSTEM CLOCK";
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
                        inputStatusText = "RX: " + mtcInput.getCurrentDeviceName();
                    }
                    else
                        inputStatusText = "PAUSED - " + mtcInput.getCurrentDeviceName();
                    sourceActive = rx;
                }
                else { sourceActive = false; inputStatusText = "WAITING FOR DEVICE..."; }
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
                        inputStatusText = "RX ON " + artnetInput.getBindInfo();
                    }
                    else
                        inputStatusText = "PAUSED - " + artnetInput.getBindInfo();
                    sourceActive = rx;
                }
                else { sourceActive = false; inputStatusText = "NOT LISTENING"; }
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
                        inputStatusText = "RX: " + ltcInput.getCurrentDeviceName()
                                        + " Ch " + juce::String(ltcInput.getSelectedChannel() + 1);
                    }
                    else
                        inputStatusText = "PAUSED - " + ltcInput.getCurrentDeviceName();
                    sourceActive = rx;
                }
                else { sourceActive = false; inputStatusText = "WAITING FOR DEVICE..."; }
                break;

            case InputSource::ProDJLink:
                if (sharedProDJLink != nullptr && sharedProDJLink->getIsRunning())
                {
                    // One-shot diagnostic
                    if (!pdlTickDiagDone)
                    {
                        pdlTickDiagDone = true;
                        DBG("TimecodeEngine[" + engineName + "]: ProDJLink tick active"
                            " -- monitoring player " + juce::String(proDJLinkPlayer)
                            + " dbClient=" + juce::String(dbClient != nullptr ? "valid" : "null")
                            + " dbRunning=" + juce::String(dbClient != nullptr && dbClient->getIsRunning() ? "yes" : "no"));
                    }
                    // Feed PLL with CDJ actual speed (offset 152).
                    // No faderPitch or isMoving needed — actualSpeed already
                    // includes motor ramp, jog, and all speed changes.
                    pll.tick(
                        sharedProDJLink->getPlayheadMs(proDJLinkPlayer),
                        sharedProDJLink->getAbsPositionTs(proDJLinkPlayer),
                        sharedProDJLink->getActualSpeed(proDJLinkPlayer)
                    );

                    // Use engine's output fps (user-configurable), not ProDJLinkInput's static default
                    currentTimecode = ProDJLink::playheadToTimecode(pll.getPositionMs(), getEffectiveOutputFps());

                    // Freeze timecode on end-of-track only.
                    // The CDJ freezes actualSpeed at the last playing value on END_TRACK,
                    // so the PLL keeps advancing while position correction fights back,
                    // causing frame flicker.  Snapshot the timecode and hold it stable.
                    // Pause is NOT frozen: actualSpeed ramps 0.88→0 in ~4-5s and the
                    // PLL follows that deceleration smoothly.
                    bool isEOT = sharedProDJLink->isEndOfTrack(proDJLinkPlayer);
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
                    uint32_t pdlTrackVer = sharedProDJLink->getTrackVersion(proDJLinkPlayer);
                    if (pdlTrackVer != lastSeenTrackVersion)
                    {
                        lastSeenTrackVersion = pdlTrackVer;
                        uint32_t newId = sharedProDJLink->getTrackID(proDJLinkPlayer);
                        if (newId != 0 && newId != cachedTrackId)
                        {
                            cachedTrackId = newId;
                            auto tinfo = sharedProDJLink->getTrackInfo(proDJLinkPlayer);
                            cachedTrackArtist = tinfo.artist;
                            cachedTrackTitle  = tinfo.title;

                            DBG("TimecodeEngine: track changed to id=" + juce::String(newId)
                                + " title=\"" + cachedTrackTitle + "\""
                                + " player=" + juce::String(proDJLinkPlayer));

                            // Phase 2: request metadata from dbserver (async)
                            requestDbMetadata(newId);

                            const auto* entry = lookupTrackInMap(newId);
                            if (entry != nullptr && entry->hasAnyTrigger())
                            {
                                triggerOutput.fire(*entry);

                                // Art-Net DMX trigger: set channel value in persistent
                                // buffer and send frame. Previous trigger values persist
                                // until overwritten by another track's trigger.
                                if (artnetTriggerEnabled && entry->hasArtnetTrigger() && artnetOutput.getIsRunning())
                                {
                                    int ch = entry->artnetCh;
                                    trigDmxBuffer[ch - 1] = uint8_t(entry->artnetVal);
                                    if (ch > trigDmxHighWater) trigDmxHighWater = ch;
                                    artnetOutput.sendDmxFrame(trigDmxBuffer, trigDmxHighWater,
                                                              artnetTriggerUniverse);
                                }
                            }
                        }
                    }

                    // Phase 2: poll dbClient cache for async metadata results.
                    // Once the background thread resolves "Track #12345" -> real artist/title,
                    // update the cached values so UI and TrackMap auto-fill get the real names.
                    if (dbClient != nullptr && cachedTrackId != 0
                        && cachedTrackTitle.startsWith("Track #"))
                    {
                        auto meta = dbClient->getCachedMetadataByTrackId(cachedTrackId);
                        if (meta.isValid())
                        {
                            cachedTrackArtist = meta.artist;
                            cachedTrackTitle  = meta.title;
                            // Propagate track duration to player state (NXS2 needs this)
                            if (meta.durationSeconds > 0 && sharedProDJLink != nullptr)
                                sharedProDJLink->setTrackLengthSec(proDJLinkPlayer,
                                    (uint32_t)meta.durationSeconds);
                            // Re-run auto-fill on the TrackMap entry now that we have real names
                            if (trackMapPtr && trackMapped)
                            {
                                auto* entry = trackMapPtr->find(cachedTrackId);
                                if (entry != nullptr && entry->artist.isEmpty()
                                    && cachedTrackArtist.isNotEmpty())
                                {
                                    entry->artist = cachedTrackArtist;
                                    entry->title  = cachedTrackTitle;
                                    trackMapDirty = true;
                                    trackMapAutoFilled = true;
                                }
                            }
                        }
                    }

                    // --- Persist auto-filled metadata ---
                    if (trackMapDirty && trackMapPtr != nullptr)
                    {
                        trackMapDirty = false;
                        TrackMap* mapRef = trackMapPtr;
                        juce::MessageManager::callAsync([mapRef]() { mapRef->save(); });
                    }

                    // --- Apply timecode offset ---
                    if (trackMapEnabled && trackMapped)
                    {
                        currentTimecode = applyTimecodeOffset(
                            currentTimecode, currentFps,
                            cachedOffH, cachedOffM, cachedOffS, cachedOffF,
                            cachedOffFps);
                    }

                    bool pdlRx = sharedProDJLink->isReceiving();
                    if (pdlRx)
                    {
                        // Note: no fps auto-detection for ProDJLink -- CDJ sends ms,
                        // not frames. The user's fps selection IS the frame rate.

                        bool pdlHasTC = sharedProDJLink->hasTimecodeData(proDJLinkPlayer);
                        if (pdlHasTC)
                        {
                            juce::String pdlModel = sharedProDJLink->getPlayerModel(proDJLinkPlayer);
                            inputStatusText = "RX P" + juce::String(proDJLinkPlayer);
                            if (pdlModel.isNotEmpty())
                                inputStatusText += " " + pdlModel;

                            // Show position source: ABS (CDJ-3000) or BEAT (NXS2)
                            inputStatusText += " [" + sharedProDJLink->getPositionSourceString(proDJLinkPlayer) + "]";

                            if (!sharedProDJLink->isPositionMoving(proDJLinkPlayer))
                                inputStatusText += " " + sharedProDJLink->getPlayStateString(proDJLinkPlayer);

                            double pdlBpm = sharedProDJLink->getBPM(proDJLinkPlayer);
                            if (pdlBpm > 0.0)
                                inputStatusText += " | " + juce::String(pdlBpm, 1) + " BPM";
                        }
                        else
                        {
                            inputStatusText = "P" + juce::String(proDJLinkPlayer)
                                + " " + sharedProDJLink->getPlayStateString(proDJLinkPlayer)
                                + " - NO POSITION DATA";
                        }

                        if (pdlHasTC && trackMapEnabled && cachedTrackId != 0)
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

                    // --- MIDI Clock ---
                    if (pdlRx && midiClockEnabled && triggerOutput.isMidiClockRunning())
                    {
                        float pdlClkBpm = (float)sharedProDJLink->getMasterBPM();
                        if (pdlClkBpm > 0.0f && std::abs(pdlClkBpm - lastSentClockBpm) > 0.05f)
                        {
                            triggerOutput.updateMidiClockBpm((double)pdlClkBpm);
                            lastSentClockBpm = pdlClkBpm;
                        }
                    }

                    // --- Ableton Link ---
                    if (pdlRx && linkBridge.isEnabled())
                    {
                        float pdlLnkBpm = (float)sharedProDJLink->getMasterBPM();
                        if (pdlLnkBpm > 0.0f)
                            linkBridge.setTempo((double)pdlLnkBpm);
                    }

                    // --- OSC BPM Forward ---
                    if (pdlRx && oscForwardEnabled && triggerOutput.isOscConnected())
                    {
                        float pdlOscBpm = (float)sharedProDJLink->getMasterBPM();
                        if (pdlOscBpm > 0.0f && std::abs(pdlOscBpm - lastSentOscBpm) > kOscBpmThreshold)
                        {
                            triggerOutput.sendOscFloat(oscFwdBpmAddr, pdlOscBpm);
                            lastSentOscBpm = pdlOscBpm;
                        }
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
                    //     (includes acceleration ramp — output starts as speed ramps up)
                    //   PAUSED: actualSpeed ramps target→0 in ~4-5s. Output naturally
                    //     stops when speed drops below kMinEncodingPitch. No special case.
                    //   END_TRACK: CDJ freezes actualSpeed (never ramps to 0).
                    //     playState == END_TRACK tells us directly → force inactive.
                    //   NO_TRACK/LOADING/SEEKING: no useful timecode → inactive.
                    bool pdlHasData = sharedProDJLink->hasTimecodeData(proDJLinkPlayer);
                    double speed = (pll.actualSpeed > pll.kDeadZone)
                                 ? pll.actualSpeed
                                 : std::abs(pll.smoothVelocity);

                    bool wasActive = sourceActive;
                    sourceActive = pdlRx && pdlHasData
                                && (speed >= PlayheadPLL::kMinEncodingPitch)
                                && !isEOT;

                    // Reseed LTC encoder on pause->active transition
                    // so it starts a fresh frame instead of continuing a stale one
                    if (sourceActive && !wasActive)
                        ltcOutput.reseed();
                }
                else { sourceActive = false; inputStatusText = "NOT CONNECTED"; }
                break;
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
            default:                      return false;
        }
    }

    static juce::String inputSourceToString(InputSource src)
    {
        switch (src)
        {
            case InputSource::MTC:        return "MTC";
            case InputSource::ArtNet:     return "ArtNet";
            case InputSource::SystemTime: return "SystemTime";
            case InputSource::LTC:        return "LTC";
            case InputSource::ProDJLink:  return "ProDJLink";
        }
        return "SystemTime";
    }

    static InputSource stringToInputSource(const juce::String& s)
    {
        if (s == "MTC") return InputSource::MTC;
        if (s == "ArtNet") return InputSource::ArtNet;
        if (s == "LTC") return InputSource::LTC;
        if (s == "ProDJLink") return InputSource::ProDJLink;
        if (s == "TCNet") return InputSource::ProDJLink;  // legacy migration
        return InputSource::SystemTime;
    }

    static juce::String getInputName(InputSource s)
    {
        switch (s)
        {
            case InputSource::MTC:        return "MTC";
            case InputSource::ArtNet:     return "ART-NET";
            case InputSource::SystemTime: return "SYSTEM";
            case InputSource::LTC:        return "LTC";
            case InputSource::ProDJLink:  return "PRO DJ LINK";
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

    static FrameRate indexToFps(int i)
    {
        constexpr FrameRate v[] = { FrameRate::FPS_2398, FrameRate::FPS_24,
                                    FrameRate::FPS_25, FrameRate::FPS_2997, FrameRate::FPS_30 };
        return v[juce::jlimit(0, 4, i)];
    }

private:
    //--------------------------------------------------------------------------
    int engineIndex;
    juce::String engineName;

    // Input state
    InputSource activeInput = InputSource::SystemTime;
    FrameRate currentFps = FrameRate::FPS_30;
    Timecode currentTimecode;
    bool sourceActive = true;
    bool outputsWereActive = false;  // previous sourceActive state for transition detection
    bool userOverrodeLtcFps = false;

    // FPS conversion
    bool fpsConvertEnabled = false;
    FrameRate outputFps = FrameRate::FPS_30;
    Timecode outputTimecode;

    // Output state
    bool outputMtcEnabled    = false;
    bool outputArtnetEnabled = false;
    bool outputLtcEnabled    = false;
    bool outputThruEnabled   = false;

    int mtcOutputOffset    = 0;
    int artnetOutputOffset = 0;
    int ltcOutputOffset    = 0;

    // Protocol handlers
    MtcInput     mtcInput;
    MtcOutput    mtcOutput;
    ArtnetInput  artnetInput;
    ArtnetOutput artnetOutput;
    LtcInput     ltcInput;
    LtcOutput    ltcOutput;
    ProDJLinkInput* sharedProDJLink = nullptr;  // shared across engines
    DbServerClient* dbClient       = nullptr;  // shared across engines (Phase 2)
    int             proDJLinkPlayer = 1;        // per-engine player selection (1..6)

    //==========================================================================
    // Playhead PLL -- smooth timecode generation from CDJ data.
    //
    // Instead of snapping to each CDJ packet (~30ms), we maintain a free-running
    // clock that advances at the CDJ's actual motor speed (offset 152). When a
    // new packet arrives, we gently correct the clock toward the CDJ's real position.
    //
    // CDJ actual speed (offset 152) is the real playback rate including:
    //   - Motor ramp on play (0 → target over ~0.5s)
    //   - Motor ramp on pause (target → 0 over ~4-5s)
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
            seekDetected   = false;
            initialized    = false;
        }

        // Simple PLL driven by CDJ actual speed:
        //
        //   Position input:
        //     CDJ-3000:   absolute playhead at 30Hz (type 0x0b) -- ms precision
        //     NXS2/older: beat-derived at ~5Hz (beatCount × 60000/BPM from status)
        //
        //   Drive velocity: actualSpeed from CDJ status (offset 152).
        //     Includes motor ramp, jog, pitch changes — everything.
        //     Fallback to dp/dt if actualSpeed is 0 (non-CDJ-3000 models).
        //
        //   Position correction: 25% of error per packet → converges in ~3 packets.
        //     Hard reset on >500ms error (seek/track change).

        void tick(uint32_t cdjPlayheadMs, double cdjPacketTs, double cdjActualSpeed)
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
                initialized    = true;
                return;
            }

            // --- Advance free-running clock ---
            double elapsed = now - lastTickTime;
            lastTickTime = now;
            actualSpeed = cdjActualSpeed;

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

            if (std::abs(driveVelocity) > kDeadZone && elapsed > 0.0 && elapsed < 200.0)
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
                if (std::abs(error) > 500.0)
                {
                    // Seek or track change: snap immediately
                    positionMs = cdjPos;
                    seekDetected = true;  // signal engine to resync outputs
                }
                else
                {
                    // Gentle correction: 25% per packet
                    // CDJ-3000 at 30Hz: converges in ~3 packets (~100ms)
                    // NXS2 at 5Hz: converges in ~3 packets (~600ms)
                    positionMs += error * 0.25;
                }
            }

            if (positionMs < 0.0) positionMs = 0.0;
        }

        uint32_t getPositionMs() const { return uint32_t(positionMs); }
    };

    PlayheadPLL pll;
    Timecode pdlFrozenTc {};          // frozen timecode on end-of-track (prevents flicker)
    bool pdlTcFrozen = false;         // true = outputting frozen timecode
    LinkBridge   linkBridge;
    std::unique_ptr<AudioThru> audioThru;  // Only for primary engine

    // Status
    juce::String inputStatusText = "SYSTEM CLOCK";
    juce::String mtcOutStatusText, artnetOutStatusText, ltcOutStatusText, thruOutStatusText;

    // VU meter smoothed state
    float sLtcIn = 0.0f, sThruIn = 0.0f, sLtcOut = 0.0f, sThruOut = 0.0f;

    // TrackMap state (track-to-offset mapping)
    TrackMap* trackMapPtr       = nullptr;
    MixerMap* mixerMapPtr       = nullptr;
    bool      trackMapEnabled   = false;
    bool      trackMapped       = false;    // current track has a mapping
    bool      trackMapDirty     = false;    // auto-fill modified map, needs save
    bool      trackMapAutoFilled = false;   // UI-consumable: editor needs refresh
    uint32_t  cachedTrackId     = 0;        // currently tracked Track ID
    uint32_t  lastSeenTrackVersion = 0;     // per-engine version counter for track change detection
    bool      pdlTickDiagDone = false;        // one-shot diagnostic flag
    int       cachedOffH = 0, cachedOffM = 0, cachedOffS = 0, cachedOffF = 0;
    FrameRate cachedOffFps = FrameRate::FPS_30;   // frame rate the offset was authored at
    juce::String cachedTrackArtist, cachedTrackTitle;

    // Track change triggers
    TriggerOutput triggerOutput;

    // MIDI Clock (BPM), OSC Forward (BPM)
    bool midiClockEnabled = false;     // MIDI Clock (24ppqn) for BPM
    float lastSentClockBpm = -1.0f;    // dedup: last BPM sent to clock

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
    juce::String oscFwdBpmAddr = "/composition/tempocontroller/tempo";
    float lastSentOscBpm = -1.0f;      // dedup: last sent OSC value

    static constexpr float kOscBpmThreshold = 0.05f;   // 0.05 BPM

    // Mixer fader dedup (last sent values, -1 = never sent)
    // MixerMap-driven dedup: one slot per MixerMap entry (max 64)
    static constexpr int kMaxMixerEntries = 64;
    int lastSentMixer[kMaxMixerEntries];  // initialised to -1 in constructor/reset

public:
    void setMidiClockEnabled(bool enabled)
    {
        midiClockEnabled = enabled;
        if (enabled && triggerOutput.isMidiOpen())
        {
            float bpm = (sharedProDJLink != nullptr) ? (float)sharedProDJLink->getMasterBPM() : 0.0f;
            triggerOutput.startMidiClock(bpm > 0.0f ? (double)bpm : 120.0);
        }
        else
        {
            triggerOutput.stopMidiClock();
        }
        lastSentClockBpm = -1.0f;
    }
    bool isMidiClockEnabled() const { return midiClockEnabled; }

    void setOscForward(bool enabled,
                       const juce::String& bpmAddr = "/composition/tempocontroller/tempo")
    {
        oscForwardEnabled = enabled;
        oscFwdBpmAddr = bpmAddr;
        lastSentOscBpm = -1.0f;
    }
    bool isOscForwardEnabled() const { return oscForwardEnabled; }
    juce::String getOscFwdBpmAddr() const { return oscFwdBpmAddr; }

    void setOscMixerForward(bool enabled)
    {
        oscMixerForwardEnabled = enabled;
        std::fill(std::begin(lastSentMixer), std::end(lastSentMixer), -1);
    }
    bool isOscMixerForwardEnabled() const { return oscMixerForwardEnabled; }

    void setMidiMixerForward(bool enabled, int ccChannel = 1, int noteChannel = 1)
    {
        midiMixerForwardEnabled = enabled;
        midiMixerCCChannel = juce::jlimit(1, 16, ccChannel);
        midiMixerNoteChannel = juce::jlimit(1, 16, noteChannel);
        std::fill(std::begin(lastSentMixer), std::end(lastSentMixer), -1);
    }
    bool isMidiMixerForwardEnabled() const { return midiMixerForwardEnabled; }
    int  getMidiMixerCCChannel()     const { return midiMixerCCChannel; }
    int  getMidiMixerNoteChannel()   const { return midiMixerNoteChannel; }

    void setArtnetMixerForward(bool enabled, int universe = 0)
    {
        artnetMixerForwardEnabled = enabled;
        artnetMixerUniverse = juce::jlimit(0, 32767, universe);
        std::fill(std::begin(lastSentMixer), std::end(lastSentMixer), -1);
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

        uint8_t srcPlayer = sharedProDJLink->getLoadedPlayer(proDJLinkPlayer);
        if (srcPlayer == 0) srcPlayer = (uint8_t)proDJLinkPlayer;
        juce::String srcIP = sharedProDJLink->getPlayerIP((int)srcPlayer);
        uint8_t slot = sharedProDJLink->getLoadedSlot(proDJLinkPlayer);

        if (srcIP.isEmpty() || slot == 0)
        {
            DBG("TimecodeEngine: SKIPPED metadata -- srcIP empty or slot=0");
            return;
        }

        // Use our VCDJ player number for the dbserver query context.
        // The CDJ only accepts queries from player numbers 1-4 on the network.
        int dbCtx = sharedProDJLink->getVCDJPlayerNumber();
        // Safety: if VCDJ is same as source, use the "other" low number
        if (dbCtx == (int)srcPlayer)
            dbCtx = (srcPlayer != 1) ? 1 : 2;

        juce::String model = sharedProDJLink->getPlayerModel((int)srcPlayer);

        DBG("TimecodeEngine: metadata request -- srcPlayer=" + juce::String(srcPlayer)
            + " srcIP=" + srcIP + " slot=" + juce::String(slot)
            + " dbCtx=" + juce::String(dbCtx) + " model=" + model);

        dbClient->requestMetadata(srcIP, slot, 1, trackId, dbCtx, model);
    }

    /// Lookup a Track ID in the TrackMap and cache the offset values.
    /// Called when a track change is detected.
    /// Returns the matched entry pointer (or nullptr if not found/malformed)
    /// so callers can use it directly without a redundant hash lookup.
    const TrackMapEntry* lookupTrackInMap(uint32_t trackId)
    {
        if (!trackMapPtr)
        {
            trackMapped = false;
            return nullptr;
        }

        auto* entry = trackMapPtr->find(trackId);
        if (entry)
        {
            // Parse the offset string once and cache the fields
            int h, m, s, f;
            if (TrackMapEntry::parseTimecodeString(entry->timecodeOffset, h, m, s, f))
            {
                cachedOffH = h;
                cachedOffM = m;
                cachedOffS = s;
                cachedOffF = f;
                cachedOffFps = indexToFps(entry->frameRate);
                trackMapped = true;

                // Auto-fill artist/title from player if the map entry is blank.
                // Sets dirty flag so the caller can persist the change.
                if (entry->artist.isEmpty() && cachedTrackArtist.isNotEmpty())
                {
                    entry->artist = cachedTrackArtist;
                    entry->title  = cachedTrackTitle;
                    trackMapDirty = true;
                    trackMapAutoFilled = true;
                }

                return entry;
            }
            else
            {
                // Malformed offset string -- treat as unmapped
                trackMapped = false;
                cachedOffH = cachedOffM = cachedOffS = cachedOffF = 0;
                return nullptr;
            }
        }
        else
        {
            // Track not in map -- pass through raw TC (no offset)
            trackMapped = false;
            cachedOffH = cachedOffM = cachedOffS = cachedOffF = 0;
            return nullptr;
        }
    }

    //--------------------------------------------------------------------------
    void updateSystemTime()
    {
        auto now = juce::Time::getCurrentTime();
        double msSinceMidnight = (double)now.getHours() * 3600000.0
                               + (double)now.getMinutes() * 60000.0
                               + (double)now.getSeconds() * 1000.0
                               + (double)now.getMilliseconds();
        currentTimecode = wallClockToTimecode(msSinceMidnight, currentFps);
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

        const int n = std::min(mixerMapPtr->size(), kMaxMixerEntries);
        const auto& entries = mixerMapPtr->getEntries();
        const bool doOsc    = oscMixerForwardEnabled && triggerOutput.isOscConnected();
        const bool doMidi   = midiMixerForwardEnabled && triggerOutput.isMidiOpen();
        const bool doArtnet = artnetMixerForwardEnabled && artnetOutput.getIsRunning();
        const int ccCh      = midiMixerCCChannel;    // 1-based, matches sendCC()
        const int noteCh    = midiMixerNoteChannel;   // 1-based, matches sendNote()
        bool dmxDirty       = false;

        for (int i = 0; i < n; ++i)
        {
            const auto& e = entries[(size_t)i];
            if (!e.enabled) continue;

            int val = readMixerValue(i);
            if (val < 0 || val == lastSentMixer[i]) continue;

            if (doOsc && e.oscAddress.isNotEmpty())
                triggerOutput.sendOscFloat(e.oscAddress, val / 255.0f);

            if (doMidi && e.midiCC >= 0)
                triggerOutput.sendCC(ccCh, e.midiCC, val >> 1);

            if (doMidi && e.midiNote >= 0)
                triggerOutput.sendNote(noteCh, e.midiNote, val >> 1);

            if (doArtnet && e.artnetCh > 0 && e.artnetCh <= 512)
            {
                dmxBuffer[e.artnetCh - 1] = uint8_t(val);
                dmxDirty = true;
                if (e.artnetCh > dmxHighWaterMark)
                    dmxHighWaterMark = e.artnetCh;
            }

            lastSentMixer[i] = val;
        }

        // Send Art-Net DMX frame.
        // Always use the persistent high-water mark for frame length — not just
        // the channels changed this tick. Otherwise channels beyond the current
        // dirty set get dropped by the receiver.
        //
        // Also re-send periodically (~10Hz) even when nothing changed to prevent
        // Art-Net node DMX timeout (some nodes drop to blackout after 2-3s without
        // fresh data). Standard DMX is 44Hz; 10Hz is a reasonable compromise.
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

    /// Read a DJM mixer parameter value (0-255) by MixerMap entry index.
    /// Returns -1 if the index is out of range.
    /// Entry order matches MixerMap::buildDefaults():
    ///   [0-8]   CH1: fader,trim,eq_hi,eq_mid,eq_lo,color,cue,input_src,xf_assign
    ///   [9-17]  CH2, [18-26] CH3, [27-35] CH4  (same 9-param pattern)
    ///   [36-40] crossfader, master_fader, master_cue, fader_curve, xf_curve
    ///   [41-44] booth, hp_cue_link, hp_mixing, hp_level
    ///   [45-52] beatfx_select..send_return
    ///   [53-55] colorfx_select, colorfx_param, colorfx_assign
    ///   [56-57] mic_eq_hi, mic_eq_lo
    int readMixerValue(int entryIndex) const
    {
        if (!sharedProDJLink) return -1;
        auto& pdl = *sharedProDJLink;

        // Per-channel params (indices 0-35, 9 per channel)
        if (entryIndex < 36)
        {
            int ch = (entryIndex / 9) + 1;   // 1-4
            int p  =  entryIndex % 9;         // 0-8
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
                default: return -1;  // unreachable, but silences compiler warning
            }
        }

        // Global params (indices 36+)
        switch (entryIndex)
        {
            case 36: return pdl.getCrossfader();
            case 37: return pdl.getMasterFader();
            case 38: return pdl.getMasterCue();
            case 39: return pdl.getFaderCurve();
            case 40: return pdl.getXfCurve();
            case 41: return pdl.getBoothLevel();
            case 42: return pdl.getHpCueLink();
            case 43: return pdl.getHpMixing();
            case 44: return pdl.getHpLevel();
            case 45: return pdl.getBeatFxSelect();
            case 46: return pdl.getBeatFxLevel();
            case 47: return pdl.getBeatFxOn();
            case 48: return pdl.getBeatFxAssign();
            case 49: return pdl.getFxFreqLo();
            case 50: return pdl.getFxFreqMid();
            case 51: return pdl.getFxFreqHi();
            case 52: return pdl.getSendReturnLevel();
            case 53: return pdl.getColorFxSelect();
            case 54: return pdl.getColorFxParam();
            case 55: return pdl.getColorFxAssign();
            case 56: return pdl.getMicEqHi();
            case 57: return pdl.getMicEqLo();
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
            }
        }
        else
        {
            // --- Clean stop ---
            // On active→inactive transition, send one final frame at the
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
            }

            // Clear seek flag if it was set during transition to inactive
            pll.seekDetected = false;

            if (outputMtcEnabled && mtcOutput.getIsRunning()) mtcOutput.setPaused(true);
            if (outputArtnetEnabled && artnetOutput.getIsRunning()) artnetOutput.setPaused(true);
            if (outputLtcEnabled && ltcOutput.getIsRunning()) ltcOutput.setPaused(true);
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
