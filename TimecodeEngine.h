// Super Timecode Converter
// Copyright (c) 2026 Fiverecords — MIT License
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
#include "AudioThru.h"
#include <memory>

//==============================================================================
// TimecodeEngine — one independent routing pipeline
//
// Each engine owns: 1 input source → N output destinations.
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
    enum class InputSource { MTC, ArtNet, SystemTime, LTC };

    //--------------------------------------------------------------------------
    explicit TimecodeEngine(int index, const juce::String& name = {})
        : engineIndex(index),
          engineName(name.isEmpty() ? ("ENGINE " + juce::String(index + 1)) : name)
    {
        // Only the primary engine (index 0) gets AudioThru
        if (index == kPrimaryEngineIndex)
            audioThru = std::make_unique<AudioThru>();
    }

    ~TimecodeEngine()
    {
        // Shutdown order: outputs first, then inputs
        stopMtcOutput();
        stopArtnetOutput();
        stopLtcOutput();
        stopThruOutput();
        stopMtcInput();
        stopArtnetInput();
        stopLtcInput();
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
            default: break;
        }

        userOverrodeLtcFps = false;
        activeInput = source;
        sourceActive = false;

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
    // Protocol handlers — direct access for device queries
    //==========================================================================
    MtcInput&     getMtcInput()     { return mtcInput; }
    MtcOutput&    getMtcOutput()    { return mtcOutput; }
    ArtnetInput&  getArtnetInput()  { return artnetInput; }
    ArtnetOutput& getArtnetOutput() { return artnetOutput; }
    LtcInput&     getLtcInput()     { return ltcInput; }
    LtcOutput&    getLtcOutput()    { return ltcOutput; }
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
    // tick() — called from timerCallback each frame
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
        }
        return "SystemTime";
    }

    static InputSource stringToInputSource(const juce::String& s)
    {
        if (s == "MTC") return InputSource::MTC;
        if (s == "ArtNet") return InputSource::ArtNet;
        if (s == "LTC") return InputSource::LTC;
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
    std::unique_ptr<AudioThru> audioThru;  // Only for primary engine

    // Status
    juce::String inputStatusText = "SYSTEM CLOCK";
    juce::String mtcOutStatusText, artnetOutStatusText, ltcOutStatusText, thruOutStatusText;

    // VU meter smoothed state
    float sLtcIn = 0.0f, sThruIn = 0.0f, sLtcOut = 0.0f, sThruOut = 0.0f;

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

    void routeTimecodeToOutputs()
    {
        FrameRate outRate = getEffectiveOutputFps();
        Timecode baseTc = fpsConvertEnabled
                        ? convertTimecodeRate(currentTimecode, currentFps, outRate)
                        : currentTimecode;
        outputTimecode = baseTc;

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
        }
        else
        {
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
