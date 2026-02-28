// Super Timecode Converter
// Copyright (c) 2026 Fiverecords — MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

#pragma once
#include <JuceHeader.h>
#include "TimecodeCore.h"
#include "TimecodeDisplay.h"
#include "TimecodeEngine.h"
#include "AppSettings.h"
#include "CustomLookAndFeel.h"
#include "LevelMeter.h"
#include "NetworkUtils.h"
#include "UpdateChecker.h"
#include <vector>
#include <memory>

//==============================================================================
class GainSlider : public juce::Slider
{
public:
    GainSlider()
    {
        setDoubleClickReturnValue(true, 100.0);
        setTooltip("Right-click or double-click to reset");
    }
    void mouseDown(const juce::MouseEvent& e) override
    {
        if (e.mods.isRightButtonDown() || e.mods.isPopupMenu())
        { setValue(getDoubleClickReturnValue(), juce::sendNotificationAsync); return; }
        juce::Slider::mouseDown(e);
    }
};

//==============================================================================
class PanelContent : public juce::Component
{
public:
    PanelContent() { setOpaque(false); }
    void setContentHeight(int h)
    {
        if (getHeight() != h) setSize(getWidth(), h);
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
        // Created on message thread before startThread() — JUCE 8.x requires
        // AudioDeviceManager construction on the message thread.
        std::unique_ptr<juce::AudioDeviceManager> tempManager;
    private:
        juce::Component::SafePointer<MainComponent> safeOwner;
    };

    std::unique_ptr<AudioScanThread> scanThread;
    bool settingsLoaded = false;

    juce::Array<AudioDeviceEntry> scannedAudioInputs;
    juce::Array<AudioDeviceEntry> scannedAudioOutputs;
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
    juce::Colour accentBlue  { 0xFF1565C0 };

    //==============================================================================
    // ENGINE MANAGEMENT
    //==============================================================================
    std::vector<std::unique_ptr<TimecodeEngine>> engines;
    int selectedEngine = 0;

    TimecodeEngine& currentEngine() { return *engines[(size_t)selectedEngine]; }
    const TimecodeEngine& currentEngine() const { return *engines[(size_t)selectedEngine]; }

    void addEngine();
    void removeEngine(int index);
    void selectEngine(int index);
    void renameEngine(int index);

    //==============================================================================
    // TAB BAR
    //==============================================================================
    class TabButton : public juce::TextButton
    {
    public:
        TabButton(const juce::String& name) : juce::TextButton(name) {}
        std::function<void()> onRightClick;
        void mouseDown(const juce::MouseEvent& e) override
        {
            if (e.mods.isRightButtonDown() || e.mods.isPopupMenu())
            { if (onRightClick) onRightClick(); return; }
            juce::TextButton::mouseDown(e);
        }
    };

    std::vector<std::unique_ptr<TabButton>> tabButtons;
    juce::TextButton btnAddEngine { "+" };
    static constexpr int kTabBarHeight = 28;
    static constexpr int kMiniStripRowH = 30;

    void rebuildTabButtons();
    void updateTabAppearance();
    void showTabContextMenu(int index);
    int getMiniStripHeight() const;
    juce::Rectangle<int> miniStripArea;  // cached from resized()
    void paintMiniStrip(juce::Graphics& g);
    void mouseDown(const juce::MouseEvent& e) override;

    //==============================================================================
    // SYNC UI <-> ENGINE
    //==============================================================================
    // Prevents onChange callbacks from firing during sync
    bool syncing = false;

    void syncUIFromEngine();      // Load engine state into UI controls
    void syncEngineFromUI();      // Save UI state into engine settings

    //==============================================================================
    // UI COMPONENTS (single set, bound to selected engine)
    //==============================================================================
    TimecodeDisplay timecodeDisplay;

    // Bottom bar repaint tracking
    juce::String lastBottomBarStatus;
    bool lastBottomBarActive = false;

    // FPS auto-detect change tracking (for button state updates)
    FrameRate lastDisplayedFps    = FrameRate::FPS_30;
    FrameRate lastDisplayedOutFps = FrameRate::FPS_30;

    // --- Collapse state (per-view, not per-engine) ---
    bool inputConfigExpanded  = true;
    bool mtcOutExpanded       = true;
    bool artnetOutExpanded    = true;
    bool ltcOutExpanded       = true;
    bool thruOutExpanded      = true;

    // --- Input buttons ---
    juce::TextButton btnMtcIn    { "MTC" };
    juce::TextButton btnArtnetIn { "ART-NET" };
    juce::TextButton btnSysTime  { "SYSTEM" };
    juce::TextButton btnLtcIn    { "LTC" };

    // --- Output toggles ---
    juce::ToggleButton btnMtcOut    { "MTC OUT" };
    juce::ToggleButton btnArtnetOut { "ART-NET OUT" };
    juce::ToggleButton btnLtcOut    { "LTC OUT" };
    juce::ToggleButton btnThruOut   { "AUDIO THRU" };

    // --- FPS buttons ---
    juce::TextButton btnFps2398 { "23.976" };
    juce::TextButton btnFps24   { "24" };
    juce::TextButton btnFps25   { "25" };
    juce::TextButton btnFps2997 { "29.97" };
    juce::TextButton btnFps30   { "30" };

    // --- FPS conversion ---
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

    // --- Left panel (input config) ---
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

    // --- Right panel (scrollable) ---
    juce::Viewport rightViewport;
    PanelContent rightContent;

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

    // --- Update checker ---
    UpdateChecker updateChecker;
    juce::HyperlinkButton btnUpdateAvailable { "", juce::URL() };
    juce::TextButton btnCheckUpdates { "Check for updates" };
    int updateCheckDelay = 0;               // ticks to wait before checking
    bool updateNotificationShown = false;    // true once UI is updated
    int updateResetCountdown = 0;            // ticks to reset button text

    AppSettings settings;
    static constexpr int kStereoItemId = 10000;
    static constexpr int kPlaceholderItemId = 10001;

    // --- Save debounce ---
    bool settingsDirty = false;
    int settingsSaveCountdown = 0;
    static constexpr int kSaveDelayTicks = 30;

    // --- Methods ---
    void startAudioDeviceScan();
    void populateMidiAndNetworkCombos();
    void populateAudioCombos();
    void populateTypeFilterCombos();
    void populateFilteredInputDeviceCombo();
    void populateFilteredOutputDeviceCombos();
    juce::String getDeviceInUseMarker(const juce::String& devName, const juce::String& typeName, bool isInput);
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

    // Engine-level start/stop (gathers params from UI, calls engine methods)
    void startCurrentMtcInput();
    void startCurrentArtnetInput();
    void startCurrentLtcInput();
    void startCurrentThruOutput();
    void startCurrentMtcOutput();
    void startCurrentArtnetOutput();
    void startCurrentLtcOutput();
    void updateCurrentOutputStates();

    void populateAudioInputChannels();
    void populateAudioOutputChannels();
    void populateThruOutputChannels();
    int getChannelFromCombo(const juce::ComboBox& cmb) const;

    void updateInputButtonStates();
    void updateFpsButtonStates();
    void updateOutputFpsButtonStates();
    void updateDeviceSelectorVisibility();
    void updateStatusLabels();

    void layoutLeftPanel();
    void layoutRightPanel();
    void saveSettings();
    void flushSettings();
    int findDeviceByName(const juce::ComboBox& cmb, const juce::String& name);

    juce::Colour getInputColour(TimecodeEngine::InputSource source) const;

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
