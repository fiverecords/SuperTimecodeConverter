// Super Timecode Converter
// Copyright (c) 2026 Fiverecords — MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

#pragma once
#include <JuceHeader.h>
#include "TimecodeCore.h"
#include "TimecodeDisplay.h"
#include "MtcInput.h"
#include "MtcOutput.h"
#include "ArtnetInput.h"
#include "ArtnetOutput.h"
#include "LtcInput.h"
#include "LtcOutput.h"
#include "AudioThru.h"
#include "AppSettings.h"
#include "CustomLookAndFeel.h"
#include "LevelMeter.h"

//==============================================================================
class GainSlider : public juce::Slider
{
public:
    GainSlider() { setDoubleClickReturnValue(true, 100.0); }
    void mouseDown(const juce::MouseEvent& e) override
    {
        if (e.mods.isRightButtonDown() || e.mods.isPopupMenu())
        { setValue(getDoubleClickReturnValue(), juce::sendNotificationAsync); return; }
        juce::Slider::mouseDown(e);
    }
};

//==============================================================================
// Content component for scrollable right panel
//==============================================================================
class PanelContent : public juce::Component
{
public:
    PanelContent() { setOpaque(false); }
    void setContentHeight(int h)
    {
        if (getHeight() != h)
            setSize(getWidth(), h);
    }
};

//==============================================================================
class MainComponent : public juce::Component,
                      public juce::Timer
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void timerCallback() override;

    // Called by background scan thread when complete
    void onAudioScanComplete(const juce::Array<AudioDeviceEntry>& inputs,
                             const juce::Array<AudioDeviceEntry>& outputs);

private:
    //==============================================================================
    // Background audio device scanner
    //==============================================================================
    class AudioScanThread : public juce::Thread
    {
    public:
        AudioScanThread(MainComponent* owner);
        void run() override;
    private:
        juce::Component::SafePointer<MainComponent> safeOwner;
    };

    std::unique_ptr<AudioScanThread> scanThread;
    bool settingsLoaded = false;

    // Scanned audio device entries
    juce::Array<AudioDeviceEntry> scannedAudioInputs;
    juce::Array<AudioDeviceEntry> scannedAudioOutputs;

    // Filtered index arrays
    juce::Array<int> filteredInputIndices;
    juce::Array<int> filteredOutputIndices;

    // --- Custom Look & Feel ---
    CustomLookAndFeel customLookAndFeel;

    // --- Colours ---
    juce::Colour bgDark      { 0xFF12141A };
    juce::Colour bgPanel     { 0xFF14161C };
    juce::Colour bgDarker    { 0xFF0D0E12 };
    juce::Colour borderCol   { 0xFF1E2028 };
    juce::Colour textDim     { 0xFF37474F };
    juce::Colour textMid     { 0xFF546E7A };
    juce::Colour textLight   { 0xFF78909C };
    juce::Colour textBright  { 0xFFCFD8DC };
    juce::Colour accentRed   { 0xFFC62828 };
    juce::Colour accentOrange{ 0xFFE65100 };
    juce::Colour accentGreen { 0xFF2E7D32 };
    juce::Colour accentPurple{ 0xFF6A1B9A };
    juce::Colour accentCyan  { 0xFF00838F };

    // --- State ---
    enum class InputSource { MTC, ArtNet, SystemTime, LTC };
    InputSource activeInput = InputSource::SystemTime;
    FrameRate currentFps = FrameRate::FPS_30;
    Timecode currentTimecode;
    bool sourceActive = true;

    // When the user manually selects a frame rate while LTC is the active input,
    // we suppress auto-detection for ambiguous pairs (24↔23.976, 30↔29.97)
    // because LTC cannot carry that distinction in its bitstream.
    bool userOverrodeLtcFps = false;

    // FPS conversion
    bool fpsConvertEnabled = false;
    FrameRate outputFps = FrameRate::FPS_30;
    Timecode outputTimecode;

    bool outputMtcEnabled    = false;
    bool outputArtnetEnabled = false;
    bool outputLtcEnabled    = false;
    bool outputThruEnabled   = false;

    int mtcOutputOffset      = 0;
    int artnetOutputOffset   = 0;
    int ltcOutputOffset      = 0;

    // VU meter decay state
    float sLtcIn = 0.0f, sThruIn = 0.0f, sLtcOut = 0.0f, sThruOut = 0.0f;

    // Bottom bar repaint tracking
    juce::String lastBottomBarStatus;
    bool lastBottomBarActive = false;

    // --- Collapse state ---
    bool inputConfigExpanded  = true;
    bool mtcOutExpanded       = true;
    bool artnetOutExpanded    = true;
    bool ltcOutExpanded       = true;
    bool thruOutExpanded      = true;

    // --- Protocol engines ---
    MtcInput mtcInput;
    MtcOutput mtcOutput;
    ArtnetInput artnetInput;
    ArtnetOutput artnetOutput;
    LtcInput ltcInput;
    LtcOutput ltcOutput;
    AudioThru audioThru;

    // --- Components ---
    TimecodeDisplay timecodeDisplay;

    juce::TextButton btnMtcIn    { "MTC" };
    juce::TextButton btnArtnetIn { "ART-NET" };
    juce::TextButton btnSysTime  { "SYSTEM" };
    juce::TextButton btnLtcIn    { "LTC" };

    juce::ToggleButton btnMtcOut    { "MTC OUT" };
    juce::ToggleButton btnArtnetOut { "ART-NET OUT" };
    juce::ToggleButton btnLtcOut    { "LTC OUT" };
    juce::ToggleButton btnThruOut   { "AUDIO THRU" };

    juce::TextButton btnFps2398 { "23.976" };
    juce::TextButton btnFps24   { "24" };
    juce::TextButton btnFps25   { "25" };
    juce::TextButton btnFps2997 { "29.97" };
    juce::TextButton btnFps30   { "30" };

    // FPS conversion controls
    juce::ToggleButton btnFpsConvert { "FPS CONVERT" };
    juce::TextButton btnOutFps2398 { "23.976" };
    juce::TextButton btnOutFps24   { "24" };
    juce::TextButton btnOutFps25   { "25" };
    juce::TextButton btnOutFps2997 { "29.97" };
    juce::TextButton btnOutFps30   { "30" };

    // --- Collapse toggle buttons ---
    juce::TextButton btnCollapseInput     { "SETTINGS" };
    juce::TextButton btnCollapseMtcOut    { "" };
    juce::TextButton btnCollapseArtnetOut { "" };
    juce::TextButton btnCollapseLtcOut    { "" };
    juce::TextButton btnCollapseThruOut   { "" };

    // Left panel
    juce::ComboBox cmbAudioInputTypeFilter;  juce::Label lblAudioInputTypeFilter;
    juce::ComboBox cmbSampleRate;            juce::Label lblSampleRate;
    juce::ComboBox cmbBufferSize;            juce::Label lblBufferSize;
    juce::ComboBox cmbMidiInputDevice;       juce::Label lblMidiInputDevice;
    juce::ComboBox cmbArtnetInputInterface;  juce::Label lblArtnetInputInterface;
    juce::ComboBox cmbAudioInputDevice;      juce::Label lblAudioInputDevice;
    juce::ComboBox cmbAudioInputChannel;     juce::Label lblAudioInputChannel;
    GainSlider sldLtcInputGain;              juce::Label lblLtcInputGain;
    LevelMeter mtrLtcInput;
    juce::ComboBox cmbThruInputChannel;      juce::Label lblThruInputChannel;
    GainSlider sldThruInputGain;             juce::Label lblThruInputGain;
    LevelMeter mtrThruInput;
    juce::Label lblInputStatus;

    // Right panel - scrollable
    juce::Viewport rightViewport;
    PanelContent rightContent;

    // Right panel controls
    juce::ComboBox cmbAudioOutputTypeFilter; juce::Label lblAudioOutputTypeFilter;
    juce::ComboBox cmbMidiOutputDevice;      juce::Label lblMidiOutputDevice;
    juce::ComboBox cmbArtnetOutputInterface; juce::Label lblArtnetOutputInterface;
    juce::ComboBox cmbAudioOutputDevice;     juce::Label lblAudioOutputDevice;
    juce::ComboBox cmbAudioOutputChannel;    juce::Label lblAudioOutputChannel;
    GainSlider sldLtcOutputGain;             juce::Label lblLtcOutputGain;
    LevelMeter mtrLtcOutput;
    juce::ComboBox cmbThruOutputDevice;      juce::Label lblThruOutputDevice;
    juce::ComboBox cmbThruOutputChannel;     juce::Label lblThruOutputChannel;
    GainSlider sldThruOutputGain;            juce::Label lblThruOutputGain;
    LevelMeter mtrThruOutput;
    juce::Label lblOutputMtcStatus;
    GainSlider sldMtcOffset;                 juce::Label lblMtcOffset;
    juce::Label lblOutputArtnetStatus;
    GainSlider sldArtnetOffset;              juce::Label lblArtnetOffset;
    juce::Label lblOutputLtcStatus;
    GainSlider sldLtcOffset;                 juce::Label lblLtcOffset;
    juce::Label lblOutputThruStatus;

    juce::TextButton btnRefreshDevices { "Refresh Devices" };
    juce::HyperlinkButton btnGitHub { "github.com/fiverecords/SuperTimecodeConverter",
                                       juce::URL("https://github.com/fiverecords/SuperTimecodeConverter") };

    juce::String inputStatusText = "SYSTEM CLOCK";
    juce::String mtcOutStatusText, artnetOutStatusText, ltcOutStatusText, thruOutStatusText;

    AppSettings settings;
    static constexpr int kStereoItemId = 10000;
    static constexpr int kPlaceholderItemId = 10001;  // "Scanning..." / "No devices" placeholder

    // --- Save debounce ---
    bool settingsDirty = false;
    int settingsSaveCountdown = 0;
    static constexpr int kSaveDelayTicks = 30;  // ~500ms at 60fps

    // --- Methods ---
    void startAudioDeviceScan();
    void populateMidiAndNetworkCombos();
    void populateAudioCombos();
    void populateTypeFilterCombos();
    void populateFilteredInputDeviceCombo();
    void populateFilteredOutputDeviceCombos();
    juce::StringArray getUniqueTypeNames(const juce::Array<AudioDeviceEntry>& entries) const;
    juce::String getInputTypeFilter() const;
    juce::String getOutputTypeFilter() const;

    void loadAndApplyNonAudioSettings();
    void applyAudioSettings();

    void populateSampleRateCombo();
    void populateBufferSizeCombo();
    double getPreferredSampleRate() const;
    int getPreferredBufferSize() const;
    void restartAllAudioDevices();

    int findFilteredIndex(const juce::Array<int>& filteredIndices,
                          const juce::Array<AudioDeviceEntry>& entries,
                          const juce::String& typeName, const juce::String& deviceName);
    AudioDeviceEntry getSelectedAudioInput() const;
    AudioDeviceEntry getSelectedAudioOutput() const;
    AudioDeviceEntry getSelectedThruOutput() const;

    void updateInputButtonStates();
    void updateFpsButtonStates();
    void updateSystemTime();
    void setInputSource(InputSource source);
    void setFrameRate(FrameRate fps);
    void setOutputFrameRate(FrameRate fps);
    void updateOutputFpsButtonStates();
    FrameRate getEffectiveOutputFps() const;
    void routeTimecodeToOutputs();

    void startMtcInput();    void stopMtcInput();
    void startArtnetInput(); void stopArtnetInput();
    void startLtcInput();    void stopLtcInput();
    void restartLtcWithCurrentSettings();
    void startMtcOutput();   void stopMtcOutput();
    void startArtnetOutput(); void stopArtnetOutput();
    void startLtcOutput();   void stopLtcOutput();
    void startThruOutput();  void stopThruOutput();
    void updateOutputStates();
    void populateAudioInputChannels();
    void populateAudioOutputChannels();
    void populateThruOutputChannels();
    int getChannelFromCombo(const juce::ComboBox& cmb) const;

    void updateDeviceSelectorVisibility();
    void updateStatusLabels();
    void layoutLeftPanel();
    void layoutRightPanel();
    void saveSettings();
    void flushSettings();
    int findDeviceByName(const juce::ComboBox& cmb, const juce::String& name);

    juce::Colour getInputColour(InputSource source) const;
    juce::String getInputName(InputSource source) const;
    bool isInputStarted() const;
    juce::String inputSourceToString(InputSource src) const;
    InputSource stringToInputSource(const juce::String& s) const;

    void styleInputButton(juce::TextButton& btn, bool active, juce::Colour colour);
    void styleFpsButton(juce::TextButton& btn, bool active);
    void styleOutputToggle(juce::ToggleButton& btn, juce::Colour colour);
    void styleComboBox(juce::ComboBox& cmb);
    void styleLabel(juce::Label& lbl, float fontSize = 10.0f);
    void styleGainSlider(GainSlider& sld);
    void styleOffsetSlider(GainSlider& sld);
    void styleCollapseButton(juce::TextButton& btn);
    void updateCollapseButtonText(juce::TextButton& btn, bool expanded);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
