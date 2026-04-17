// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

#pragma once
#include <JuceHeader.h>
#include "TimecodeCore.h"
#include "TimecodeDisplay.h"
#include "TimecodeEngine.h"
#include "AppSettings.h"
#include "CustomLookAndFeel.h"
#include "LevelMeter.h"
#include "TrackMapEditor.h"
#include "MixerMapEditor.h"
#include "GeneratorPresetEditor.h"
#include "OscInputServer.h"
#include "NetworkUtils.h"
#include "UpdateChecker.h"
#include "MediaDisplay.h"
#include "ProDJLinkView.h"
#include "StageLinQView.h"
#include "StageLinQDbClient.h"
#include "TCNetOutput.h"
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
// Small circular LED that flashes on beat detection
class BeatLed : public juce::Component
{
public:
    BeatLed() { setOpaque(false); }

    void flash()
    {
        brightness = 1.0f;
        repaint();
    }

    void decay(float amount = 0.12f)
    {
        if (brightness > 0.0f)
        {
            brightness = juce::jmax(0.0f, brightness - amount);
            repaint();
        }
    }

    void setLedColour(juce::Colour c) { onColour = c; repaint(); }

    void paint(juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat().reduced(1.0f);
        float side = juce::jmin(area.getWidth(), area.getHeight());
        auto circle = juce::Rectangle<float>(side, side)
                        .withCentre(area.getCentre());

        auto c = onColour.withAlpha(0.15f + 0.85f * brightness);
        g.setColour(c);
        g.fillEllipse(circle);

        // Bright glow ring when active
        if (brightness > 0.1f)
        {
            g.setColour(onColour.withAlpha(0.3f * brightness));
            g.drawEllipse(circle.expanded(1.0f), 1.5f);
        }
    }

private:
    juce::Colour onColour { 0xFFFF9900 };
    float brightness = 0.0f;
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

    /// Add a section separator at the given Y position with optional label.
    void addSectionSeparator(int y, const juce::String& label = {})
    {
        sectionSeps.push_back({ y, label });
    }

    /// Clear all separators (call at start of resized before re-adding)
    void clearSectionSeparators() { sectionSeps.clear(); }

    void paint(juce::Graphics& g) override
    {
        for (auto& sep : sectionSeps)
        {
            if (sep.label.isNotEmpty())
            {
                // Labeled separator: draw label text only (acts as visual divider)
                g.setColour(juce::Colour(0xFF37474F));
                g.setFont(juce::Font(juce::FontOptions(getMonoFontName(), 9.0f, juce::Font::bold)));
                g.drawText(sep.label, 12, sep.y + 2, getWidth() - 24, 12,
                           juce::Justification::centredLeft);
            }
            else
            {
                // Unlabeled separator: subtle horizontal line
                g.setColour(juce::Colour(0xFF1E2028));
                g.drawHorizontalLine(sep.y, 12.0f, (float)(getWidth() - 12));
            }
        }
    }

private:
    struct SectionSep { int y; juce::String label; };
    std::vector<SectionSep> sectionSeps;
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
    bool keyPressed(const juce::KeyPress& key) override;

    void onAudioScanComplete(const juce::Array<AudioDeviceEntry>& inputs,
                             const juce::Array<AudioDeviceEntry>& outputs);

    /// Main window bounds persistence (called by MainWindow in Main.cpp)
    juce::String getSavedMainWindowBounds() const { return settings.mainWindowBounds; }
    bool isShowModeLocked() const { return settings.showModeLocked; }
    void saveMainWindowBounds(const juce::String& bounds)
    {
        settings.mainWindowBounds = bounds;
        settings.save();
    }

private:
    //==============================================================================
    // Background audio device scanner
    //==============================================================================
    class AudioScanThread : public juce::Thread
    {
    public:
        AudioScanThread(MainComponent* owner);
        void run() override;
        // Created on message thread before startThread() -- JUCE 8.x requires
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
    ProDJLinkInput sharedProDJLinkInput;  // shared across all engines
    StageLinQInput sharedStageLinQInput;  // shared across all engines
    StageLinQDbClient sharedStageLinQDb;  // database client for Denon metadata + artwork
    MixerMap sharedSlqMixerMap { MixerMapMode::Denon };  // Denon mixer map
    MixerMap       sharedMixerMap;        // shared DJM parameter mapping
    DbServerClient sharedDbClient;        // shared across all engines (Phase 2)
    TCNetOutput    sharedTcnetOutput;     // shared TCNet timecode broadcast
    juce::String   tcnetArtworkKey[TCNetOutput::kMaxLayers];  // track key per layer for artwork change detection

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
    double    beatFlashAccumMs    = 0.0;   // accumulates ms for BPM-based beat flash
    double    lastBeatFlashBpm    = 0.0;   // last BPM used for flash interval

    // --- Collapse state (per-view, not per-engine) ---
    bool inputConfigExpanded  = true;
    bool mtcOutExpanded       = true;
    bool artnetOutExpanded    = true;
    bool ltcOutExpanded       = true;
    bool thruOutExpanded      = true;

    // --- Input buttons ---
    juce::TextButton btnMtcIn    { "MTC" };
    juce::TextButton btnArtnetIn { "ART-NET" };
    juce::TextButton btnSysTime  { "GENERATOR" };

    // Generator controls (visible when input = Generator)
    juce::ToggleButton btnGenClock { "CLOCK" };  // system clock mode toggle
    juce::TextButton btnGenPlay   { "PLAY" };
    juce::TextButton btnGenPause  { "PAUSE" };
    juce::TextButton btnGenStop   { "STOP" };
    juce::TextEditor txtGenStartTC;
    juce::TextEditor txtGenStopTC;
    juce::Label      lblGenStartTC;
    juce::Label      lblGenStopTC;
    juce::ComboBox   cmbGenPreset;
    juce::Label      lblGenPreset;
    juce::TextButton btnGenPrev   { "<" };
    juce::TextButton btnGenNext   { ">" };
    juce::TextButton btnGenGo     { "GO" };
    juce::TextButton btnGenPresetEdit { "EDIT" };

    // OSC Input (global, controls generator from external OSC sources)
    juce::ToggleButton btnOscIn { "OSC IN" };
    juce::ComboBox     cmbOscInputInterface;
    juce::Label        lblOscInputInterface;
    juce::TextEditor   txtOscInPort;
    juce::Label        lblOscInPort;
    juce::Label        lblOscInStatus;
    OscInputServer     oscInputServer;
    juce::TextButton btnLtcIn    { "LTC" };
    juce::TextButton btnProDJLinkIn { "PRO DJ LINK" };
    juce::TextButton btnStageLinQIn { "STAGELINQ" };
    juce::TextButton btnHippoIn    { "HIPPONET" };

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
    juce::ComboBox cmbHippoInputInterface;  juce::Label lblHippoInputInterface;
    juce::ComboBox cmbHippoTcChannel;      juce::Label lblHippoTcChannel;
    // Pro DJ Link controls
    juce::ComboBox cmbProDJLinkInterface;    juce::Label lblProDJLinkInterface;
    juce::ComboBox cmbProDJLinkPlayer;       juce::Label lblProDJLinkPlayer;
    juce::ComboBox cmbStageLinQInterface;    juce::Label lblStageLinQInterface;

    // BPM Multiplier buttons (per-player, ProDJLink only)
    // Single click: session override (temporary, cleared on track change).
    // Double click: save to TrackMap (persistent, auto-loaded on track change).
    // 0=off, 1=x2, 2=x4, -1=/2, -2=/4.
    juce::TextButton btnBpmOff  { "1x" };
    juce::TextButton btnBpmX2   { "x2" };
    juce::TextButton btnBpmX4   { "x4" };
    juce::TextButton btnBpmD2   { "/2" };
    juce::TextButton btnBpmD4   { "/4" };
    void updateBpmMultButtonStates();
    void saveBpmMultToTrackMap(int clickedMult);
    juce::int64 lastBpmClickMs = 0;
    int lastBpmClickMult = -999;
    juce::Label lblProDJLinkTrackInfo;
    juce::Label lblProDJLinkMetadata;
    juce::Label lblNextCue;          // "NEXT: DROP in 0:12"
    juce::Label lblMixerStatus;  // DJM model + fader values
    ArtworkDisplay artworkDisplay;               // Phase 2c: album art from CDJ
    WaveformDisplay waveformDisplay;             // Phase 3: color waveform from CDJ
    uint32_t displayedWaveformTrackId = 0;       // currently displayed waveform track
    uint32_t displayedArtworkId = 0;             // currently displayed artwork ID

    // Features: TrackMap, MIDI Clock, OSC BPM, Ableton Link
    juce::ToggleButton btnTrackMap { "TRACK MAP" };
    juce::TextButton   btnTrackMapEdit { "Track Map" };
    juce::ToggleButton btnMidiClock { "MIDI CLOCK" };
    juce::ToggleButton btnOscFwdBpm     { "OSC BPM FWD" };
    juce::TextEditor   edOscFwdBpmAddr;
    juce::Label        lblOscFwdBpmAddr;
    juce::TextEditor   edOscFwdBpmCmd;
    juce::Label        lblOscFwdBpmCmd;
    juce::ToggleButton btnOscMixerFwd   { "OSC MIXER FWD" };
    juce::ToggleButton btnMidiMixerFwd  { "MIDI MIXER FWD" };
    juce::ToggleButton btnArtnetMixerFwd { "ARTNET MIXER FWD" };
    juce::ComboBox     cmbArtMixNet, cmbArtMixSub, cmbArtMixUni;
    juce::Label        lblArtMixAddr;
    juce::ComboBox     cmbMidiMixCCCh;
    juce::Label        lblMidiMixCCCh;
    juce::ComboBox     cmbMidiMixNoteCh;
    juce::Label        lblMidiMixNoteCh;
    juce::ToggleButton btnLink { "ABLETON LINK" };
    juce::Label        lblLinkStatus;

    // Audio BPM detection (for non-DJ sources)
    juce::ToggleButton btnAudioBpm { "AUDIO BPM" };
    juce::Label        lblBpmValue;       // "128.0 BPM" display
    BeatLed            ledBeat;           // beat flash indicator
    GainSlider         sldBpmSmoothing;           juce::Label lblBpmSmoothing;
    GainSlider         sldBpmInputGain;           juce::Label lblBpmInputGain;
    juce::ComboBox     cmbAudioBpmDevice;   juce::Label lblAudioBpmDevice;
    juce::ComboBox     cmbAudioBpmChannel;  juce::Label lblAudioBpmChannel;
    LevelMeter         mtrAudioBpm;

    juce::Component::SafePointer<juce::DocumentWindow> trackMapWindow;
    juce::Component::SafePointer<juce::DocumentWindow> genPresetWindow;
    std::unique_ptr<CuePointEditorWindow> cuePointWindow;
    std::string cuePointTrackKey;  // key of the entry being edited (for dangling ref safety)
    juce::TextButton btnMixerMapEdit { "Mixer Map" };
    juce::Component::SafePointer<juce::DocumentWindow> mixerMapWindow;
    juce::TextButton btnProDJLinkView { "PDL View" };
    juce::TextButton btnStageLinQView { "SLQ View" };
    juce::TextButton btnBackup  { "Backup" };
    juce::TextButton btnRestore { "Restore" };
    juce::TextButton btnShowLock { "SHOW LOCK" };
    juce::ToggleButton btnTcnetOut  { "TCNET OUT" };
    juce::ComboBox cmbTcnetInterface; juce::Label lblTcnetInterface;
    juce::ComboBox cmbTcnetLayer; juce::Label lblTcnetLayer;
    GainSlider sldTcnetOffset;        juce::Label lblTcnetOffset;

    // Hippotizer output
    juce::ToggleButton btnHippoOut { "HIPPONET OUT" };
    juce::TextEditor   txtHippoDestIp;    juce::Label lblHippoDestIp;
    juce::Label        lblHippoOutStatus;

    // On-air gate (Pro DJ Link only): require the CDJ to be flagged on-air
    // by the DJM before the engine produces active timecode. Lets the DJ
    // preview tracks on a deck without triggering output.
    juce::ToggleButton btnOnAirGate { "ON-AIR ONLY" };

    int showLockFlashCountdown = 0;  // ticks remaining for flash feedback

    /// Returns true if Show Lock is active and the action should be blocked.
    /// Flashes the lock button red briefly to give visual feedback.
    bool isShowLocked()
    {
        if (!settings.showModeLocked) return false;
        // Flash feedback: briefly brighten the lock button (reset in timerCallback)
        btnShowLock.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFFFF3333));
        showLockFlashCountdown = 18;  // ~300ms at 60Hz
        return true;
    }

    /// Same as isShowLocked(), but also reverts a ToggleButton's auto-flipped state.
    /// JUCE ToggleButtons flip their state BEFORE onClick fires, so we must undo it.
    bool isShowLockedToggle(juce::ToggleButton& btn)
    {
        if (!settings.showModeLocked) return false;
        btn.setToggleState(!btn.getToggleState(), juce::dontSendNotification);
        btnShowLock.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFFFF3333));
        showLockFlashCountdown = 18;
        return true;
    }

    /// Same as isShowLocked(), but also reverts the UI to match engine state.
    /// JUCE ComboBoxes and Sliders update their visual value BEFORE onChange
    /// fires, so we re-sync the entire UI from engine state to undo the change.
    bool isShowLockedRevert()
    {
        if (!settings.showModeLocked) return false;
        syncUIFromEngine();  // restores all combos/sliders/toggles to engine state
        btnShowLock.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFFFF3333));
        showLockFlashCountdown = 18;
        return true;
    }
    std::unique_ptr<juce::FileChooser> configFileChooser;
    juce::ScopedMessageBox importConfirmBox;
    std::unique_ptr<ProDJLinkViewWindow> proDJLinkViewWindow;
    std::unique_ptr<StageLinQViewWindow> stageLinQViewWindow;

    // Track change triggers
    juce::ToggleButton btnTriggerMidi { "MIDI Trigger" };
    juce::ComboBox cmbTriggerMidiDevice;
    juce::ToggleButton btnTriggerOsc { "OSC Trigger" };
    juce::TextEditor edOscIp;
    juce::TextEditor edOscPort;
    juce::ToggleButton btnArtnetTrigger { "ARTNET Trigger" };
    juce::ComboBox cmbArtTrigNet, cmbArtTrigSub, cmbArtTrigUni;
    juce::Label    lblArtTrigAddr;
    juce::ComboBox cmbArtnetDmxInterface;     juce::Label lblArtnetDmxInterface;
    juce::ComboBox cmbAudioInputDevice;      juce::Label lblAudioInputDevice;
    juce::ComboBox cmbAudioInputChannel;     juce::Label lblAudioInputChannel;
    GainSlider sldLtcInputGain;              juce::Label lblLtcInputGain;
    LevelMeter mtrLtcInput;
    juce::ComboBox cmbThruInputChannel;      juce::Label lblThruInputChannel;
    GainSlider sldThruInputGain;             juce::Label lblThruInputGain;
    LevelMeter mtrThruInput;
    juce::Label lblInputStatus;

    // --- Left panel (scrollable, like right) ---
    juce::Viewport leftViewport;
    PanelContent leftContent;

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
    void repopulateTcnetLayerCombo();
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
    void startCurrentProDJLinkInput();
    void startCurrentStageLinQInput();
    void startCurrentHippotizerInput();
    void openTrackMapEditor();
    void openCuePointEditor(TrackMapEntry* entry);
    void openMixerMapEditor();
    void openProDJLinkView();
    void openStageLinQView();
    void exportConfig();
    void importConfig();
    void applyTriggerSettings();
    void propagateGlobalSettings();
    void startCurrentThruOutput();
    void startCurrentMtcOutput();
    void startCurrentArtnetOutput();
    void startCurrentLtcOutput();
    void updateCurrentOutputStates();

    void populateAudioInputChannels();
    void populateAudioOutputChannels();
    void populateThruOutputChannels();
    void populateAudioBpmChannels();
    void restartAudioBpm();

    // Returns the index of another engine that has Link active, or -1 if none.
    int findLinkOwnerOtherThan(int engineIdx) const;
    int getChannelFromCombo(const juce::ComboBox& cmb) const;

    void updateInputButtonStates();
    void updateFpsButtonStates();
    void updateOutputFpsButtonStates();
    void updateDeviceSelectorVisibility();
    void updateStatusLabels();
    void updateNextCueLabel(TimecodeEngine& eng);
    static double parseTimecodeToMs(const juce::String& tc, FrameRate fps);
    static juce::String msToTimecodeString(double ms, FrameRate fps);
    void populateGenPresetCombo();
    void activateGenPreset(const juce::String& name);
    void loadGenPresetToFields(const juce::String& name);
    void openGeneratorPresetEditor();
    void setupOscInputServer();
    void startOscInput();
    void stopOscInput();

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

    // Art-Net port-address helpers (15-bit: Net[7] | Subnet[4] | Universe[4])
    static int packArtNetAddress(int net, int sub, int uni)
    {
        return ((net & 0x7F) << 8) | ((sub & 0x0F) << 4) | (uni & 0x0F);
    }
    static void unpackArtNetAddress(int addr, int& net, int& sub, int& uni)
    {
        net = (addr >> 8) & 0x7F;
        sub = (addr >> 4) & 0x0F;
        uni = addr & 0x0F;
    }
    void setupArtNetAddressCombos(juce::ComboBox& cmbNet, juce::ComboBox& cmbSub,
                                   juce::ComboBox& cmbUni, juce::Label& lbl,
                                   const juce::String& labelText,
                                   std::function<void()> onChange);
    void setArtNetCombosFromAddress(juce::ComboBox& cmbNet, juce::ComboBox& cmbSub,
                                     juce::ComboBox& cmbUni, int portAddress);
    int  getArtNetAddressFromCombos(const juce::ComboBox& cmbNet, const juce::ComboBox& cmbSub,
                                     const juce::ComboBox& cmbUni);

    // OpenGL context removed: see constructor comment for rationale.
    // Keeping the juce_opengl module in Projucer is harmless and allows
    // re-enabling GPU rendering in the future if the thread safety issues
    // are addressed.

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
