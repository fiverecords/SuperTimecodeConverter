// Super Timecode Converter
// Copyright (c) 2026 Fiverecords — MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

#include "MainComponent.h"

using InputSource = TimecodeEngine::InputSource;

//==============================================================================
// BACKGROUND AUDIO DEVICE SCANNER
//==============================================================================
MainComponent::AudioScanThread::AudioScanThread(MainComponent* owner)
    : juce::Thread("AudioScanThread"), safeOwner(owner) {}

void MainComponent::AudioScanThread::run()
{
    juce::Array<AudioDeviceEntry> inputs, outputs;

    if (!tempManager)
        return;

    for (auto* type : tempManager->getAvailableDeviceTypes())
    {
        if (threadShouldExit()) return;
        auto typeName = type->getTypeName();
        type->scanForDevices();
        for (auto& name : type->getDeviceNames(true))
            inputs.add({ typeName, name, AudioDeviceEntry::makeDisplayName(typeName, name) });
        for (auto& name : type->getDeviceNames(false))
            outputs.add({ typeName, name, AudioDeviceEntry::makeDisplayName(typeName, name) });
    }

    juce::MessageManager::callAsync([safeOwner = this->safeOwner, inputs, outputs]()
    {
        if (auto* comp = safeOwner.getComponent())
            comp->onAudioScanComplete(inputs, outputs);
    });
}

//==============================================================================
// SAMPLE RATE / BUFFER SIZE helpers (same as v1.3)
//==============================================================================
static int sampleRateToComboId(double sr)
{
    if (sr <= 0) return 1;
    if (std::abs(sr - 44100) < 1) return 2;
    if (std::abs(sr - 48000) < 1) return 3;
    if (std::abs(sr - 88200) < 1) return 4;
    if (std::abs(sr - 96000) < 1) return 5;
    return 1;
}

static int bufferSizeToComboId(int bs)
{
    if (bs <= 0)    return 1;
    if (bs <= 32)   return 2;
    if (bs <= 64)   return 3;
    if (bs <= 128)  return 4;
    if (bs <= 256)  return 5;
    if (bs <= 512)  return 6;
    if (bs <= 1024) return 7;
    return 8;
}

//==============================================================================
// CONSTRUCTOR / DESTRUCTOR
//==============================================================================
MainComponent::MainComponent()
{
    setLookAndFeel(&customLookAndFeel);

    // --- Create initial engine BEFORE setSize, because setSize triggers
    //     resized() which calls currentEngine() ---
    engines.push_back(std::make_unique<TimecodeEngine>(0));

    setSize(900, 700);

    // --- Tab bar ---
    addAndMakeVisible(btnAddEngine);
    btnAddEngine.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF1A1D23));
    btnAddEngine.setColour(juce::TextButton::textColourOffId, accentBlue);
    btnAddEngine.onClick = [this] { addEngine(); };
    rebuildTabButtons();

    // --- Right panel scrollable viewport ---
    addAndMakeVisible(rightViewport);
    rightViewport.setViewedComponent(&rightContent, false);
    rightViewport.setScrollBarsShown(true, false);

    // --- Input buttons ---
    for (auto* btn : { &btnMtcIn, &btnArtnetIn, &btnSysTime, &btnLtcIn })
    { addAndMakeVisible(btn); btn->setClickingTogglesState(false); }

    btnMtcIn.onClick = [this] {
        if (syncing) return;
        auto& eng = currentEngine();
        if (eng.getActiveInput() == InputSource::MTC) { inputConfigExpanded = !inputConfigExpanded; updateDeviceSelectorVisibility(); }
        else { inputConfigExpanded = true; eng.setInputSource(InputSource::MTC); startCurrentMtcInput(); updateInputButtonStates(); updateDeviceSelectorVisibility(); saveSettings(); }
    };
    btnArtnetIn.onClick = [this] {
        if (syncing) return;
        auto& eng = currentEngine();
        if (eng.getActiveInput() == InputSource::ArtNet) { inputConfigExpanded = !inputConfigExpanded; updateDeviceSelectorVisibility(); }
        else { inputConfigExpanded = true; eng.setInputSource(InputSource::ArtNet); startCurrentArtnetInput(); updateInputButtonStates(); updateDeviceSelectorVisibility(); saveSettings(); }
    };
    btnSysTime.onClick = [this] {
        if (syncing) return;
        auto& eng = currentEngine();
        if (eng.getActiveInput() == InputSource::SystemTime) { inputConfigExpanded = !inputConfigExpanded; updateDeviceSelectorVisibility(); }
        else { inputConfigExpanded = true; eng.setInputSource(InputSource::SystemTime); updateInputButtonStates(); updateDeviceSelectorVisibility(); saveSettings(); }
    };
    btnLtcIn.onClick = [this] {
        if (syncing) return;
        auto& eng = currentEngine();
        if (eng.getActiveInput() == InputSource::LTC) { inputConfigExpanded = !inputConfigExpanded; updateDeviceSelectorVisibility(); }
        else { inputConfigExpanded = true; eng.setInputSource(InputSource::LTC); if (!scannedAudioInputs.isEmpty()) startCurrentLtcInput(); updateInputButtonStates(); updateDeviceSelectorVisibility(); saveSettings(); }
    };

    // --- Output toggles ---
    for (auto* btn : { &btnMtcOut, &btnArtnetOut, &btnLtcOut, &btnThruOut })
        rightContent.addAndMakeVisible(btn);

    styleOutputToggle(btnMtcOut, accentRed);
    styleOutputToggle(btnArtnetOut, accentOrange);
    styleOutputToggle(btnLtcOut, accentPurple);
    styleOutputToggle(btnThruOut, accentCyan);

    auto outputToggleHandler = [this]
    {
        if (syncing) return;
        auto& eng = currentEngine();
        eng.setOutputMtcEnabled(btnMtcOut.getToggleState());
        eng.setOutputArtnetEnabled(btnArtnetOut.getToggleState());
        eng.setOutputLtcEnabled(btnLtcOut.getToggleState());
        eng.setOutputThruEnabled(btnThruOut.getToggleState());
        updateCurrentOutputStates();
        updateDeviceSelectorVisibility();
        saveSettings();
    };
    btnMtcOut.onClick = btnArtnetOut.onClick = btnLtcOut.onClick = btnThruOut.onClick = outputToggleHandler;

    // --- Collapse toggle buttons for outputs ---
    for (auto* btn : { &btnCollapseMtcOut, &btnCollapseArtnetOut, &btnCollapseLtcOut, &btnCollapseThruOut })
    {
        rightContent.addAndMakeVisible(btn);
        styleCollapseButton(*btn);
    }
    auto makeCollapseHandler = [this](bool& expandedFlag, juce::TextButton& collapseBtn) {
        return [this, &expandedFlag, &collapseBtn] {
            expandedFlag = !expandedFlag;
            updateCollapseButtonText(collapseBtn, expandedFlag);
            updateDeviceSelectorVisibility();
        };
    };
    btnCollapseMtcOut.onClick    = makeCollapseHandler(mtcOutExpanded,    btnCollapseMtcOut);
    btnCollapseArtnetOut.onClick = makeCollapseHandler(artnetOutExpanded, btnCollapseArtnetOut);
    btnCollapseLtcOut.onClick    = makeCollapseHandler(ltcOutExpanded,    btnCollapseLtcOut);
    btnCollapseThruOut.onClick   = makeCollapseHandler(thruOutExpanded,   btnCollapseThruOut);
    updateCollapseButtonText(btnCollapseMtcOut, mtcOutExpanded);
    updateCollapseButtonText(btnCollapseArtnetOut, artnetOutExpanded);
    updateCollapseButtonText(btnCollapseLtcOut, ltcOutExpanded);
    updateCollapseButtonText(btnCollapseThruOut, thruOutExpanded);

    // Input collapse button
    addAndMakeVisible(btnCollapseInput);
    styleCollapseButton(btnCollapseInput);
    btnCollapseInput.onClick = [this] {
        inputConfigExpanded = !inputConfigExpanded;
        updateCollapseButtonText(btnCollapseInput, inputConfigExpanded);
        updateDeviceSelectorVisibility();
    };
    updateCollapseButtonText(btnCollapseInput, inputConfigExpanded);

    // --- FPS buttons ---
    for (auto* btn : { &btnFps2398, &btnFps24, &btnFps25, &btnFps2997, &btnFps30 })
    { addAndMakeVisible(btn); btn->setClickingTogglesState(false); }

    btnFps2398.onClick = [this] {
        if (syncing) return;
        auto& eng = currentEngine();
        if (eng.getActiveInput() == InputSource::LTC) eng.setUserOverrodeLtcFps(true);
        eng.setFrameRate(FrameRate::FPS_2398); updateFpsButtonStates(); saveSettings();
    };
    btnFps24.onClick = [this] {
        if (syncing) return;
        currentEngine().setUserOverrodeLtcFps(false);
        currentEngine().setFrameRate(FrameRate::FPS_24); updateFpsButtonStates(); saveSettings();
    };
    btnFps25.onClick = [this] {
        if (syncing) return;
        currentEngine().setUserOverrodeLtcFps(false);
        currentEngine().setFrameRate(FrameRate::FPS_25); updateFpsButtonStates(); saveSettings();
    };
    btnFps2997.onClick = [this] {
        if (syncing) return;
        auto& eng = currentEngine();
        if (eng.getActiveInput() == InputSource::LTC) eng.setUserOverrodeLtcFps(true);
        eng.setFrameRate(FrameRate::FPS_2997); updateFpsButtonStates(); saveSettings();
    };
    btnFps30.onClick = [this] {
        if (syncing) return;
        currentEngine().setUserOverrodeLtcFps(false);
        currentEngine().setFrameRate(FrameRate::FPS_30); updateFpsButtonStates(); saveSettings();
    };

    // --- FPS Conversion ---
    addAndMakeVisible(btnFpsConvert);
    styleOutputToggle(btnFpsConvert, accentGreen);
    btnFpsConvert.onClick = [this]
    {
        if (syncing) return;
        auto& eng = currentEngine();
        eng.setFpsConvertEnabled(btnFpsConvert.getToggleState());
        updateOutputFpsButtonStates();
        resized(); repaint();
        saveSettings();
    };

    for (auto* btn : { &btnOutFps2398, &btnOutFps24, &btnOutFps25, &btnOutFps2997, &btnOutFps30 })
    { addAndMakeVisible(btn); btn->setClickingTogglesState(false); }

    btnOutFps2398.onClick = [this] { if (!syncing) { currentEngine().setOutputFrameRate(FrameRate::FPS_2398); updateOutputFpsButtonStates(); saveSettings(); } };
    btnOutFps24.onClick   = [this] { if (!syncing) { currentEngine().setOutputFrameRate(FrameRate::FPS_24);   updateOutputFpsButtonStates(); saveSettings(); } };
    btnOutFps25.onClick   = [this] { if (!syncing) { currentEngine().setOutputFrameRate(FrameRate::FPS_25);   updateOutputFpsButtonStates(); saveSettings(); } };
    btnOutFps2997.onClick = [this] { if (!syncing) { currentEngine().setOutputFrameRate(FrameRate::FPS_2997); updateOutputFpsButtonStates(); saveSettings(); } };
    btnOutFps30.onClick   = [this] { if (!syncing) { currentEngine().setOutputFrameRate(FrameRate::FPS_30);   updateOutputFpsButtonStates(); saveSettings(); } };

    addAndMakeVisible(timecodeDisplay);

    // =====================================================================
    // LEFT PANEL -- INPUT SELECTORS
    // =====================================================================
    auto addLabelAndCombo = [this](juce::Label& lbl, juce::ComboBox& cmb, const juce::String& text)
    {
        addAndMakeVisible(lbl); addAndMakeVisible(cmb);
        lbl.setText(text, juce::dontSendNotification);
        styleLabel(lbl); styleComboBox(cmb);
    };

    auto addRightLabelAndCombo = [this](juce::Label& lbl, juce::ComboBox& cmb, const juce::String& text)
    {
        rightContent.addAndMakeVisible(lbl); rightContent.addAndMakeVisible(cmb);
        lbl.setText(text, juce::dontSendNotification);
        styleLabel(lbl); styleComboBox(cmb);
    };

    addLabelAndCombo(lblAudioInputTypeFilter, cmbAudioInputTypeFilter, "AUDIO DRIVER:");
    cmbAudioInputTypeFilter.onChange = [this]
    {
        if (syncing) return;
        populateFilteredInputDeviceCombo();
        if (currentEngine().getActiveInput() == InputSource::LTC)
            startCurrentLtcInput();
        saveSettings();
    };

    addLabelAndCombo(lblSampleRate, cmbSampleRate, "SAMPLE RATE / BUFFER:");
    populateSampleRateCombo();
    cmbSampleRate.onChange = [this] { if (!syncing) { restartAllAudioDevices(); saveSettings(); } };

    addLabelAndCombo(lblBufferSize, cmbBufferSize, "BUFFER SIZE:");
    populateBufferSizeCombo();
    cmbBufferSize.onChange = [this] { if (!syncing) { restartAllAudioDevices(); saveSettings(); } };

    addLabelAndCombo(lblMidiInputDevice, cmbMidiInputDevice, "MIDI INPUT DEVICE:");
    cmbMidiInputDevice.onChange = [this]
    {
        if (syncing) return;
        int sel = cmbMidiInputDevice.getSelectedId() - 1;
        if (sel >= 0 && currentEngine().getActiveInput() == InputSource::MTC)
        {
            currentEngine().stopMtcInput();
            currentEngine().getMtcInput().refreshDeviceList();
            currentEngine().startMtcInput(sel);
            saveSettings();
        }
    };

    addLabelAndCombo(lblArtnetInputInterface, cmbArtnetInputInterface, "ART-NET INPUT DEVICE:");
    cmbArtnetInputInterface.onChange = [this]
    {
        if (syncing) return;
        if (currentEngine().getActiveInput() == InputSource::ArtNet)
        {
            int sel = cmbArtnetInputInterface.getSelectedId() - 1;
            currentEngine().stopArtnetInput();
            currentEngine().startArtnetInput(sel);
            saveSettings();
        }
    };

    addLabelAndCombo(lblAudioInputDevice, cmbAudioInputDevice, "AUDIO INPUT DEVICE:");
    cmbAudioInputDevice.onChange = [this]
    {
        if (syncing) return;
        if (currentEngine().getActiveInput() == InputSource::LTC
            && cmbAudioInputDevice.getSelectedId() > 0
            && cmbAudioInputDevice.getSelectedId() != kPlaceholderItemId)
        { startCurrentLtcInput(); populateAudioInputChannels(); }
    };

    addLabelAndCombo(lblAudioInputChannel, cmbAudioInputChannel, "LTC CHANNEL:");
    cmbAudioInputChannel.onChange = [this] { if (!syncing && currentEngine().getActiveInput() == InputSource::LTC) { startCurrentLtcInput(); saveSettings(); } };

    addAndMakeVisible(sldLtcInputGain); styleGainSlider(sldLtcInputGain);
    addAndMakeVisible(lblLtcInputGain); lblLtcInputGain.setText("LTC INPUT GAIN:", juce::dontSendNotification); styleLabel(lblLtcInputGain);
    addAndMakeVisible(mtrLtcInput); mtrLtcInput.setMeterColour(accentPurple);
    sldLtcInputGain.onValueChange = [this] { if (!syncing) { currentEngine().getLtcInput().setInputGain((float)sldLtcInputGain.getValue() / 100.0f); saveSettings(); } };

    addLabelAndCombo(lblThruInputChannel, cmbThruInputChannel, "AUDIO THRU CHANNEL:");
    cmbThruInputChannel.onChange = [this] { if (!syncing && currentEngine().getActiveInput() == InputSource::LTC) { startCurrentLtcInput(); saveSettings(); } };

    addAndMakeVisible(sldThruInputGain); styleGainSlider(sldThruInputGain);
    addAndMakeVisible(lblThruInputGain); lblThruInputGain.setText("AUDIO THRU INPUT GAIN:", juce::dontSendNotification); styleLabel(lblThruInputGain);
    addAndMakeVisible(mtrThruInput); mtrThruInput.setMeterColour(accentCyan);
    sldThruInputGain.onValueChange = [this] { if (!syncing) { currentEngine().getLtcInput().setPassthruGain((float)sldThruInputGain.getValue() / 100.0f); saveSettings(); } };

    addAndMakeVisible(lblInputStatus); styleLabel(lblInputStatus); lblInputStatus.setColour(juce::Label::textColourId, accentGreen);

    // =====================================================================
    // RIGHT PANEL -- OUTPUT SELECTORS
    // =====================================================================
    addRightLabelAndCombo(lblMidiOutputDevice, cmbMidiOutputDevice, "MIDI OUTPUT DEVICE:");
    cmbMidiOutputDevice.onChange = [this]
    {
        if (syncing) return;
        int sel = cmbMidiOutputDevice.getSelectedId() - 1;
        auto& eng = currentEngine();
        if (sel >= 0 && eng.isOutputMtcEnabled())
        {
            eng.stopMtcOutput();
            eng.getMtcOutput().refreshDeviceList();
            eng.startMtcOutput(sel);
            saveSettings();
        }
    };
    rightContent.addAndMakeVisible(lblOutputMtcStatus); styleLabel(lblOutputMtcStatus); lblOutputMtcStatus.setColour(juce::Label::textColourId, accentRed);
    rightContent.addAndMakeVisible(sldMtcOffset); styleOffsetSlider(sldMtcOffset);
    rightContent.addAndMakeVisible(lblMtcOffset); lblMtcOffset.setText("MTC OFFSET:", juce::dontSendNotification); styleLabel(lblMtcOffset);
    sldMtcOffset.onValueChange = [this] { if (!syncing) { currentEngine().setMtcOutputOffset((int)sldMtcOffset.getValue()); saveSettings(); } };

    addRightLabelAndCombo(lblArtnetOutputInterface, cmbArtnetOutputInterface, "ART-NET OUTPUT DEVICE:");
    cmbArtnetOutputInterface.onChange = [this]
    {
        if (syncing) return;
        auto& eng = currentEngine();
        if (eng.isOutputArtnetEnabled())
        {
            int sel = cmbArtnetOutputInterface.getSelectedId() - 1;
            eng.stopArtnetOutput();
            eng.startArtnetOutput(sel);
            saveSettings();
        }
    };
    rightContent.addAndMakeVisible(lblOutputArtnetStatus); styleLabel(lblOutputArtnetStatus); lblOutputArtnetStatus.setColour(juce::Label::textColourId, accentOrange);
    rightContent.addAndMakeVisible(sldArtnetOffset); styleOffsetSlider(sldArtnetOffset);
    rightContent.addAndMakeVisible(lblArtnetOffset); lblArtnetOffset.setText("ART-NET OFFSET:", juce::dontSendNotification); styleLabel(lblArtnetOffset);
    sldArtnetOffset.onValueChange = [this] { if (!syncing) { currentEngine().setArtnetOutputOffset((int)sldArtnetOffset.getValue()); saveSettings(); } };

    addRightLabelAndCombo(lblAudioOutputTypeFilter, cmbAudioOutputTypeFilter, "AUDIO DRIVER:");
    cmbAudioOutputTypeFilter.onChange = [this]
    {
        if (syncing) return;
        populateFilteredOutputDeviceCombos();
        auto& eng = currentEngine();
        if (eng.isOutputLtcEnabled()) startCurrentLtcOutput();
        if (eng.isOutputThruEnabled()) startCurrentThruOutput();
        saveSettings();
    };

    addRightLabelAndCombo(lblAudioOutputDevice, cmbAudioOutputDevice, "LTC OUTPUT DEVICE:");
    cmbAudioOutputDevice.onChange = [this]
    {
        if (syncing) return;
        if (cmbAudioOutputDevice.getSelectedId() > 0
            && cmbAudioOutputDevice.getSelectedId() != kPlaceholderItemId && currentEngine().isOutputLtcEnabled())
        { startCurrentLtcOutput(); saveSettings(); }
    };

    addRightLabelAndCombo(lblAudioOutputChannel, cmbAudioOutputChannel, "LTC CHANNEL:");
    cmbAudioOutputChannel.onChange = [this]
    {
        if (syncing) return;
        if (currentEngine().isOutputLtcEnabled() && cmbAudioOutputDevice.getSelectedId() > 0
            && cmbAudioOutputDevice.getSelectedId() != kPlaceholderItemId)
        { startCurrentLtcOutput(); saveSettings(); }
    };

    rightContent.addAndMakeVisible(sldLtcOutputGain); styleGainSlider(sldLtcOutputGain);
    rightContent.addAndMakeVisible(lblLtcOutputGain); lblLtcOutputGain.setText("LTC OUTPUT GAIN:", juce::dontSendNotification); styleLabel(lblLtcOutputGain);
    rightContent.addAndMakeVisible(mtrLtcOutput); mtrLtcOutput.setMeterColour(accentPurple);
    sldLtcOutputGain.onValueChange = [this] { if (!syncing) { currentEngine().getLtcOutput().setOutputGain((float)sldLtcOutputGain.getValue() / 100.0f); saveSettings(); } };

    rightContent.addAndMakeVisible(lblOutputLtcStatus); styleLabel(lblOutputLtcStatus); lblOutputLtcStatus.setColour(juce::Label::textColourId, accentPurple);
    rightContent.addAndMakeVisible(sldLtcOffset); styleOffsetSlider(sldLtcOffset);
    rightContent.addAndMakeVisible(lblLtcOffset); lblLtcOffset.setText("LTC OFFSET:", juce::dontSendNotification); styleLabel(lblLtcOffset);
    sldLtcOffset.onValueChange = [this] { if (!syncing) { currentEngine().setLtcOutputOffset((int)sldLtcOffset.getValue()); saveSettings(); } };

    // AudioThru controls visible for all engines in the panel but only functional for engine 0
    addRightLabelAndCombo(lblThruOutputDevice, cmbThruOutputDevice, "AUDIO THRU OUTPUT DEVICE:");
    cmbThruOutputDevice.onChange = [this]
    {
        if (syncing) return;
        if (currentEngine().isOutputThruEnabled() && cmbThruOutputDevice.getSelectedId() != kPlaceholderItemId)
        { startCurrentThruOutput(); saveSettings(); }
    };

    addRightLabelAndCombo(lblThruOutputChannel, cmbThruOutputChannel, "AUDIO THRU OUTPUT CHANNEL:");
    cmbThruOutputChannel.onChange = [this]
    {
        if (syncing) return;
        if (currentEngine().isOutputThruEnabled() && cmbThruOutputDevice.getSelectedId() != kPlaceholderItemId)
        { startCurrentThruOutput(); saveSettings(); }
    };

    rightContent.addAndMakeVisible(sldThruOutputGain); styleGainSlider(sldThruOutputGain);
    rightContent.addAndMakeVisible(lblThruOutputGain); lblThruOutputGain.setText("AUDIO THRU OUTPUT GAIN:", juce::dontSendNotification); styleLabel(lblThruOutputGain);
    rightContent.addAndMakeVisible(mtrThruOutput); mtrThruOutput.setMeterColour(accentCyan);
    sldThruOutputGain.onValueChange = [this] {
        if (!syncing && currentEngine().getAudioThru())
        { currentEngine().getAudioThru()->setOutputGain((float)sldThruOutputGain.getValue() / 100.0f); saveSettings(); }
    };

    rightContent.addAndMakeVisible(lblOutputThruStatus); styleLabel(lblOutputThruStatus); lblOutputThruStatus.setColour(juce::Label::textColourId, accentCyan);

    rightContent.addAndMakeVisible(btnRefreshDevices);
    btnRefreshDevices.onClick = [this] { populateMidiAndNetworkCombos(); startAudioDeviceScan(); };
    btnRefreshDevices.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF1A1D23));
    btnRefreshDevices.setColour(juce::TextButton::textColourOffId, textMid);

    addAndMakeVisible(btnGitHub);
    btnGitHub.setFont(juce::Font(juce::FontOptions(getMonoFontName(), 9.0f, juce::Font::plain)), false);
    btnGitHub.setColour(juce::HyperlinkButton::textColourId, juce::Colour(0xFF546E7A));

    // --- Update checker button (hidden until update found) ---
    addChildComponent(btnUpdateAvailable);   // hidden by default
    btnUpdateAvailable.setFont(juce::Font(juce::FontOptions(getMonoFontName(), 10.0f, juce::Font::bold)), false);
    btnUpdateAvailable.setColour(juce::HyperlinkButton::textColourId, juce::Colour(0xFF4FC3F7));  // cyan

    addAndMakeVisible(btnCheckUpdates);
    btnCheckUpdates.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    btnCheckUpdates.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFF546E7A));
    btnCheckUpdates.onClick = [this]
    {
        auto appVer = juce::JUCEApplication::getInstance()->getApplicationVersion();
        updateNotificationShown = false;
        updateCheckDelay = 0;
        btnUpdateAvailable.setVisible(false);
        btnCheckUpdates.setButtonText("Checking...");
        btnCheckUpdates.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFF78909C));
        updateChecker.checkAsync(appVer);
    };
    updateCheckDelay = 180;  // ~3 seconds at 60Hz before first check

    // =====================================================================
    // STARTUP
    // =====================================================================
    populateMidiAndNetworkCombos();
    loadAndApplyNonAudioSettings();

    for (auto* cmb : { &cmbAudioInputDevice, &cmbAudioOutputDevice, &cmbThruOutputDevice })
        cmb->addItem("Scanning...", kPlaceholderItemId);

    startTimerHz(60);
    startAudioDeviceScan();
}

MainComponent::~MainComponent()
{
    setLookAndFeel(nullptr);
    flushSettings();
    stopTimer();

    if (scanThread && scanThread->isThreadRunning())
    {
        if (!scanThread->stopThread(2000))
        { DBG("WARNING: AudioScanThread did not stop within 2s timeout (destructor)"); }
    }

    // NOTE: Any pending callAsync from AudioScanThread::run() is safe here because
    // callAsync dispatches on the message thread, and the destructor also runs on the
    // message thread, so there is no interleaving between engines.clear() and the
    // async callback.  The SafePointer guard would also catch a fully-deleted component.
    engines.clear();
}

//==============================================================================
// ENGINE MANAGEMENT
//==============================================================================
void MainComponent::addEngine()
{
    if ((int)engines.size() >= kMaxEngines)
        return;

    // Generate a unique name
    int nameNum = (int)engines.size() + 1;
    auto nameExists = [this](const juce::String& name) {
        for (auto& e : engines)
            if (e->getName() == name) return true;
        return false;
    };
    juce::String newName;
    do { newName = "ENGINE " + juce::String(nameNum++); } while (nameExists(newName));

    int newIndex = (int)engines.size();
    engines.push_back(std::make_unique<TimecodeEngine>(newIndex, newName));
    rebuildTabButtons();
    selectEngine(newIndex);
    saveSettings();
}

void MainComponent::removeEngine(int index)
{
    if (engines.size() <= 1 || index < 0 || index >= (int)engines.size())
        return;

    // Flush pending settings before modifying arrays
    if (settingsDirty)
        flushSettings();

    // Remember if the deleted engine was the primary and had thru enabled,
    // so we can restart AudioThru on the new primary after reindexing.
    bool deletedWasPrimary = engines[(size_t)index]->isPrimary();
    bool deletedHadThru    = deletedWasPrimary
                           && engines[(size_t)index]->isOutputThruEnabled();

    // Explicitly stop all protocols on the engine being deleted BEFORE
    // erasing it, so destructors don't race with any pending callbacks.
    engines[(size_t)index]->stopMtcOutput();
    engines[(size_t)index]->stopArtnetOutput();
    engines[(size_t)index]->stopLtcOutput();
    engines[(size_t)index]->stopThruOutput();
    engines[(size_t)index]->stopMtcInput();
    engines[(size_t)index]->stopArtnetInput();
    engines[(size_t)index]->stopLtcInput();

    engines.erase(engines.begin() + index);

    // Re-index remaining engines so isPrimary() and getIndex() stay correct.
    // This also creates AudioThru on the new primary engine if engine 0 was deleted.
    for (int i = 0; i < (int)engines.size(); i++)
        engines[(size_t)i]->reindex(i);

    // Keep settings.engines in sync with engines vector
    if (index < (int)settings.engines.size())
        settings.engines.erase(settings.engines.begin() + index);

    // Fix selectedEngine to track the same engine after shift
    if (index < selectedEngine)
        selectedEngine--;               // engine we were on shifted left
    else if (selectedEngine >= (int)engines.size())
        selectedEngine = (int)engines.size() - 1;  // we deleted the one we were on (or last)

    rebuildTabButtons();
    syncUIFromEngine();

    // If the deleted engine was the primary with AudioThru active,
    // the new primary engine (index 0) got a fresh AudioThru instance
    // from reindex() but it's not started.  Attempt to restart it if
    // the new primary's LTC input is running and thru settings exist.
    if (deletedHadThru && !engines.empty())
    {
        auto& newPrimary = *engines[0];
        if (newPrimary.getActiveInput() == InputSource::LTC
            && newPrimary.getLtcInput().getIsRunning()
            && !settings.engines.empty()
            && settings.engines[0].thruOutEnabled)
        {
            newPrimary.setOutputThruEnabled(true);
            // If engine 0 is currently selected, use UI combos; otherwise use saved settings
            if (selectedEngine == 0)
            {
                startCurrentThruOutput();
            }
            else if (!settings.engines[0].thruOutputDevice.isEmpty())
            {
                int ch = settings.engines[0].thruOutputStereo ? -1 : settings.engines[0].thruOutputChannel;
                newPrimary.startThruOutput(settings.engines[0].thruOutputType,
                                           settings.engines[0].thruOutputDevice, ch,
                                           getPreferredSampleRate(), getPreferredBufferSize());
            }
        }
    }

    saveSettings();
}

void MainComponent::selectEngine(int index)
{
    if (index < 0 || index >= (int)engines.size() || index == selectedEngine)
        return;

    // Flush any pending settings from the current engine before switching
    if (settingsDirty)
        flushSettings();

    selectedEngine = index;
    inputConfigExpanded = true;
    mtcOutExpanded = artnetOutExpanded = ltcOutExpanded = thruOutExpanded = true;

    // Reset FPS tracking so buttons update immediately for new engine
    lastDisplayedFps    = engines[(size_t)index]->getCurrentFps();
    lastDisplayedOutFps = engines[(size_t)index]->getEffectiveOutputFps();

    syncUIFromEngine();
    updateTabAppearance();
    repaint();
}

void MainComponent::renameEngine(int index)
{
    if (index < 0 || index >= (int)engines.size()) return;

    auto alertWindow = std::make_shared<juce::AlertWindow>("Rename Engine",
                                                "Enter a name for this engine:",
                                                juce::MessageBoxIconType::NoIcon, this);
    alertWindow->addTextEditor("name", engines[(size_t)index]->getName());
    alertWindow->addButton("OK", 1);
    alertWindow->addButton("Cancel", 0);

    juce::Component::SafePointer<MainComponent> safeThis(this);
    alertWindow->enterModalState(true, juce::ModalCallbackFunction::create(
        [safeThis, index, alertWindow](int result)
        {
            if (result == 1 && safeThis != nullptr)
            {
                auto newName = alertWindow->getTextEditorContents("name").trim();
                if (newName.isNotEmpty() && index < (int)safeThis->engines.size())
                {
                    safeThis->engines[(size_t)index]->setName(newName);
                    // Only update text — don't rebuild buttons (avoids heap
                    // corruption from destroying components during modal callback)
                    safeThis->updateTabAppearance();
                    safeThis->resized();  // reposition in case text width changed
                    safeThis->saveSettings();
                }
            }
            // alertWindow is destroyed automatically when shared_ptr goes out of scope
        }), true);
}

//==============================================================================
// TAB BAR
//==============================================================================
void MainComponent::rebuildTabButtons()
{
    for (auto& tb : tabButtons)
        removeChildComponent(tb.get());
    tabButtons.clear();

    for (int i = 0; i < (int)engines.size(); i++)
    {
        auto btn = std::make_unique<TabButton>(engines[(size_t)i]->getName());
        btn->setClickingTogglesState(false);
        int idx = i;
        btn->onClick = [this, idx] { selectEngine(idx); };
        btn->onRightClick = [this, idx] { showTabContextMenu(idx); };
        addAndMakeVisible(btn.get());
        tabButtons.push_back(std::move(btn));
    }

    updateTabAppearance();
    resized();

    // Disable "+" button when at max engines
    btnAddEngine.setEnabled((int)engines.size() < kMaxEngines);
    btnAddEngine.setAlpha((int)engines.size() < kMaxEngines ? 1.0f : 0.3f);
}

void MainComponent::updateTabAppearance()
{
    for (int i = 0; i < (int)tabButtons.size(); i++)
    {
        bool active = (i == selectedEngine);
        auto& btn = *tabButtons[(size_t)i];
        btn.setButtonText(engines[(size_t)i]->getName());
        btn.setColour(juce::TextButton::buttonColourId, active ? accentBlue.withAlpha(0.2f) : juce::Colour(0xFF1A1D23));
        btn.setColour(juce::TextButton::textColourOffId, active ? textBright : textMid);
    }
}

void MainComponent::showTabContextMenu(int index)
{
    juce::PopupMenu menu;
    menu.addItem(1, "Rename");
    if (engines.size() > 1)
        menu.addItem(2, "Delete");

    juce::Component::SafePointer<MainComponent> safeThis(this);
    menu.showMenuAsync(juce::PopupMenu::Options(), [safeThis, index](int result) {
        if (safeThis == nullptr) return;
        if (result == 1) safeThis->renameEngine(index);
        else if (result == 2) safeThis->removeEngine(index);
    });
}

//==============================================================================
// SYNC UI <-> ENGINE
//==============================================================================
void MainComponent::syncUIFromEngine()
{
    syncing = true;

    auto& eng = currentEngine();

    // Input buttons
    updateInputButtonStates();

    // FPS
    updateFpsButtonStates();
    btnFpsConvert.setToggleState(eng.isFpsConvertEnabled(), juce::dontSendNotification);
    updateOutputFpsButtonStates();

    // Output toggles
    btnMtcOut.setToggleState(eng.isOutputMtcEnabled(), juce::dontSendNotification);
    btnArtnetOut.setToggleState(eng.isOutputArtnetEnabled(), juce::dontSendNotification);
    btnLtcOut.setToggleState(eng.isOutputLtcEnabled(), juce::dontSendNotification);

    // AudioThru toggle: only show for primary engine
    btnThruOut.setToggleState(eng.isOutputThruEnabled(), juce::dontSendNotification);
    btnThruOut.setVisible(eng.isPrimary());

    // Offsets
    sldMtcOffset.setValue(eng.getMtcOutputOffset(), juce::dontSendNotification);
    sldArtnetOffset.setValue(eng.getArtnetOutputOffset(), juce::dontSendNotification);
    sldLtcOffset.setValue(eng.getLtcOutputOffset(), juce::dontSendNotification);

    // Gains
    sldLtcInputGain.setValue(eng.getLtcInput().getInputGain() * 100.0f, juce::dontSendNotification);
    sldThruInputGain.setValue(eng.getLtcInput().getPassthruGain() * 100.0f, juce::dontSendNotification);
    sldLtcOutputGain.setValue(eng.getLtcOutput().getOutputGain() * 100.0f, juce::dontSendNotification);
    if (eng.getAudioThru())
        sldThruOutputGain.setValue(eng.getAudioThru()->getOutputGain() * 100.0f, juce::dontSendNotification);

    // Repopulate device combos so in-use markers reflect the new engine context
    populateMidiAndNetworkCombos();
    populateFilteredInputDeviceCombo();
    populateFilteredOutputDeviceCombos();

    // MIDI device selections — find matching device in combo
    {
        int idx = findDeviceByName(cmbMidiInputDevice, eng.getMtcInput().getCurrentDeviceName());
        if (idx >= 0) cmbMidiInputDevice.setSelectedId(idx + 1, juce::dontSendNotification);
    }
    {
        int idx = findDeviceByName(cmbMidiOutputDevice, eng.getMtcOutput().getCurrentDeviceName());
        if (idx >= 0) cmbMidiOutputDevice.setSelectedId(idx + 1, juce::dontSendNotification);
    }

    // ArtNet interface selections — restore from saved settings
    if (selectedEngine < (int)settings.engines.size())
    {
        auto& es = settings.engines[(size_t)selectedEngine];
        int artInId = es.artnetInputInterface + 1;   // combo id is 1-based (1 = All Interfaces)
        if (artInId < 1) artInId = 1;                // handle legacy -1 default
        if (artInId <= cmbArtnetInputInterface.getNumItems())
            cmbArtnetInputInterface.setSelectedId(artInId, juce::dontSendNotification);
        int artOutId = es.artnetOutputInterface + 1;
        if (artOutId < 1) artOutId = 1;              // handle legacy -1 default
        if (artOutId <= cmbArtnetOutputInterface.getNumItems())
            cmbArtnetOutputInterface.setSelectedId(artOutId, juce::dontSendNotification);
    }

    // Audio device selections — find matching device in filtered list
    if (selectedEngine < (int)settings.engines.size())
    {
        auto& es = settings.engines[(size_t)selectedEngine];

        int audioInIdx = findFilteredIndex(filteredInputIndices, scannedAudioInputs,
                                            es.audioInputType, es.audioInputDevice);
        if (audioInIdx >= 0)
            cmbAudioInputDevice.setSelectedId(audioInIdx + 1, juce::dontSendNotification);

        int audioOutIdx = findFilteredIndex(filteredOutputIndices, scannedAudioOutputs,
                                             es.audioOutputType, es.audioOutputDevice);
        if (audioOutIdx >= 0)
            cmbAudioOutputDevice.setSelectedId(audioOutIdx + 1, juce::dontSendNotification);

        int thruOutIdx = findFilteredIndex(filteredOutputIndices, scannedAudioOutputs,
                                            es.thruOutputType, es.thruOutputDevice);
        if (thruOutIdx >= 0)
            cmbThruOutputDevice.setSelectedId(thruOutIdx + 1, juce::dontSendNotification);
    }

    // Audio channel selections based on running engines
    if (eng.getLtcInput().getIsRunning())
    {
        int ch = eng.getLtcInput().getSelectedChannel();
        if (ch >= 0) cmbAudioInputChannel.setSelectedId(ch + 1, juce::dontSendNotification);

        if (eng.getLtcInput().hasPassthruChannel())
        {
            int thruCh = eng.getLtcInput().getPassthruChannel();
            if (thruCh >= 0) cmbThruInputChannel.setSelectedId(thruCh + 1, juce::dontSendNotification);
        }
    }
    else if (selectedEngine < (int)settings.engines.size())
    {
        auto& es = settings.engines[(size_t)selectedEngine];
        cmbAudioInputChannel.setSelectedId(es.audioInputChannel + 1, juce::dontSendNotification);
        cmbThruInputChannel.setSelectedId(es.thruInputChannel + 1, juce::dontSendNotification);
    }

    if (eng.getLtcOutput().getIsRunning())
    {
        int ch = eng.getLtcOutput().getSelectedChannel();
        if (ch == -1)
            cmbAudioOutputChannel.setSelectedId(kStereoItemId, juce::dontSendNotification);
        else if (ch >= 0)
            cmbAudioOutputChannel.setSelectedId(ch + 1, juce::dontSendNotification);
    }
    else if (selectedEngine < (int)settings.engines.size())
    {
        auto& es = settings.engines[(size_t)selectedEngine];
        if (es.audioOutputStereo)
            cmbAudioOutputChannel.setSelectedId(kStereoItemId, juce::dontSendNotification);
        else
            cmbAudioOutputChannel.setSelectedId(es.audioOutputChannel + 1, juce::dontSendNotification);
    }

    if (eng.getAudioThru() && eng.getAudioThru()->getIsRunning())
    {
        int ch = eng.getAudioThru()->getSelectedChannel();
        if (ch == -1)
            cmbThruOutputChannel.setSelectedId(kStereoItemId, juce::dontSendNotification);
        else if (ch >= 0)
            cmbThruOutputChannel.setSelectedId(ch + 1, juce::dontSendNotification);
    }
    else if (selectedEngine < (int)settings.engines.size())
    {
        auto& es = settings.engines[(size_t)selectedEngine];
        if (es.thruOutputStereo)
            cmbThruOutputChannel.setSelectedId(kStereoItemId, juce::dontSendNotification);
        else
            cmbThruOutputChannel.setSelectedId(es.thruOutputChannel + 1, juce::dontSendNotification);
    }

    updateDeviceSelectorVisibility();
    resized();
    repaint();

    syncing = false;
}

//==============================================================================
// ENGINE-LEVEL START/STOP (gathers params from UI)
//==============================================================================
void MainComponent::startCurrentMtcInput()
{
    auto& eng = currentEngine();
    int sel = cmbMidiInputDevice.getSelectedId() - 1;
    eng.startMtcInput(sel);
}

void MainComponent::startCurrentArtnetInput()
{
    auto& eng = currentEngine();
    int sel = cmbArtnetInputInterface.getSelectedId() - 1;
    eng.startArtnetInput(sel);
}

void MainComponent::startCurrentLtcInput()
{
    auto& eng = currentEngine();
    auto entry = getSelectedAudioInput();
    if (entry.deviceName.isEmpty() && !filteredInputIndices.isEmpty())
    { cmbAudioInputDevice.setSelectedId(1, juce::dontSendNotification); entry = getSelectedAudioInput(); }

    int ltcCh = cmbAudioInputChannel.getSelectedId() - 1;
    if (ltcCh < 0) ltcCh = 0;

    int thruCh = -1;
    if (eng.isPrimary() && eng.isOutputThruEnabled())
    { thruCh = cmbThruInputChannel.getSelectedId() - 1; if (thruCh < 0) thruCh = 1; }

    if (eng.startLtcInput(entry.typeName, entry.deviceName, ltcCh, thruCh,
                           getPreferredSampleRate(), getPreferredBufferSize()))
    {
        eng.getLtcInput().setInputGain((float)sldLtcInputGain.getValue() / 100.0f);
        eng.getLtcInput().setPassthruGain((float)sldThruInputGain.getValue() / 100.0f);
        populateAudioInputChannels();
        if (eng.isPrimary() && eng.isOutputThruEnabled())
            startCurrentThruOutput();
    }
    saveSettings();
}

void MainComponent::startCurrentThruOutput()
{
    auto& eng = currentEngine();
    if (!eng.isPrimary()) return;

    auto entry = getSelectedThruOutput();
    if (entry.deviceName.isEmpty() && !filteredOutputIndices.isEmpty())
    { cmbThruOutputDevice.setSelectedId(1, juce::dontSendNotification); entry = getSelectedThruOutput(); }

    int outCh = getChannelFromCombo(cmbThruOutputChannel);
    eng.startThruOutput(entry.typeName, entry.deviceName, outCh,
                         getPreferredSampleRate(), getPreferredBufferSize());

    if (eng.getAudioThru() && eng.getAudioThru()->getIsRunning())
    {
        eng.getAudioThru()->setOutputGain((float)sldThruOutputGain.getValue() / 100.0f);
        populateThruOutputChannels();
    }
}

void MainComponent::startCurrentMtcOutput()
{
    auto& eng = currentEngine();
    int sel = cmbMidiOutputDevice.getSelectedId() - 1;
    eng.startMtcOutput(sel);
}

void MainComponent::startCurrentArtnetOutput()
{
    auto& eng = currentEngine();
    int sel = cmbArtnetOutputInterface.getSelectedId() - 1;
    eng.startArtnetOutput(sel);
}

void MainComponent::startCurrentLtcOutput()
{
    auto& eng = currentEngine();
    eng.stopLtcOutput();

    auto entry = getSelectedAudioOutput();
    if (entry.deviceName.isEmpty() && !filteredOutputIndices.isEmpty())
    { cmbAudioOutputDevice.setSelectedId(1, juce::dontSendNotification); entry = getSelectedAudioOutput(); }

    int channel = getChannelFromCombo(cmbAudioOutputChannel);

    if (eng.startLtcOutput(entry.typeName, entry.deviceName, channel,
                            getPreferredSampleRate(), getPreferredBufferSize()))
    {
        eng.getLtcOutput().setOutputGain((float)sldLtcOutputGain.getValue() / 100.0f);
        populateAudioOutputChannels();

        // Restart thru if it was stopped due to a previous device conflict
        if (eng.isPrimary() && eng.isOutputThruEnabled()
            && eng.getAudioThru() && !eng.getAudioThru()->getIsRunning())
        {
            auto thruEntry = getSelectedThruOutput();
            if (thruEntry.deviceName.isNotEmpty()
                && !(thruEntry.deviceName == entry.deviceName && thruEntry.typeName == entry.typeName))
                startCurrentThruOutput();
        }
    }
}

void MainComponent::updateCurrentOutputStates()
{
    auto& eng = currentEngine();

    if (eng.isOutputMtcEnabled() && !eng.getMtcOutput().getIsRunning()) startCurrentMtcOutput();
    else if (!eng.isOutputMtcEnabled() && eng.getMtcOutput().getIsRunning()) eng.stopMtcOutput();

    if (eng.isOutputArtnetEnabled() && !eng.getArtnetOutput().getIsRunning()) startCurrentArtnetOutput();
    else if (!eng.isOutputArtnetEnabled() && eng.getArtnetOutput().getIsRunning()) eng.stopArtnetOutput();

    if (eng.isOutputLtcEnabled() && !eng.getLtcOutput().getIsRunning() && !scannedAudioOutputs.isEmpty()) startCurrentLtcOutput();
    else if (!eng.isOutputLtcEnabled() && eng.getLtcOutput().getIsRunning()) eng.stopLtcOutput();

    if (eng.isPrimary())
    {
        if (eng.isOutputThruEnabled() && eng.getAudioThru() && !eng.getAudioThru()->getIsRunning())
        {
            if (eng.getActiveInput() == InputSource::LTC)
                startCurrentLtcInput();
            else
                startCurrentThruOutput();
        }
        else if (!eng.isOutputThruEnabled() && eng.getAudioThru() && eng.getAudioThru()->getIsRunning())
        {
            eng.stopThruOutput();
        }
    }
}

//==============================================================================
// BACKGROUND AUDIO SCAN
//==============================================================================
void MainComponent::startAudioDeviceScan()
{
    if (scanThread && scanThread->isThreadRunning())
    {
        if (!scanThread->stopThread(2000))
        { DBG("WARNING: AudioScanThread did not stop within 2s timeout — skipping new scan"); return; }
    }
    scanThread = std::make_unique<AudioScanThread>(this);
    // Create AudioDeviceManager on the message thread — JUCE 8.x internally
    // registers a MIDI device-change listener that requires JUCE_ASSERT_MESSAGE_THREAD.
    scanThread->tempManager = std::make_unique<juce::AudioDeviceManager>();
    scanThread->tempManager->initialise(128, 128, nullptr, false);
    scanThread->startThread();
}

void MainComponent::onAudioScanComplete(const juce::Array<AudioDeviceEntry>& inputs,
                                         const juce::Array<AudioDeviceEntry>& outputs)
{
    scannedAudioInputs  = inputs;
    scannedAudioOutputs = outputs;
    populateAudioCombos();
    applyAudioSettings();
}

//==============================================================================
// DRIVER TYPE FILTER HELPERS (same as v1.3)
//==============================================================================
juce::StringArray MainComponent::getUniqueTypeNames(const juce::Array<AudioDeviceEntry>& entries) const
{
    juce::StringArray types;
    for (auto& e : entries)
        if (!types.contains(e.typeName))
            types.add(e.typeName);
    return types;
}

juce::String MainComponent::getInputTypeFilter() const
{
    int sel = cmbAudioInputTypeFilter.getSelectedId();
    return (sel <= 1) ? juce::String() : cmbAudioInputTypeFilter.getText();
}

juce::String MainComponent::getOutputTypeFilter() const
{
    int sel = cmbAudioOutputTypeFilter.getSelectedId();
    return (sel <= 1) ? juce::String() : cmbAudioOutputTypeFilter.getText();
}

void MainComponent::populateTypeFilterCombos()
{
    auto inputTypes = getUniqueTypeNames(scannedAudioInputs);
    auto outputTypes = getUniqueTypeNames(scannedAudioOutputs);

    cmbAudioInputTypeFilter.clear(juce::dontSendNotification);
    cmbAudioInputTypeFilter.addItem("All Drivers", 1);
    for (int i = 0; i < inputTypes.size(); i++)
    {
        auto shortName = AudioDeviceEntry::shortenTypeName(inputTypes[i]);
        cmbAudioInputTypeFilter.addItem(shortName.isEmpty() ? inputTypes[i] : shortName, i + 2);
    }

    // Restore saved filter
    if (settings.audioInputTypeFilter.isNotEmpty())
    {
        for (int i = 0; i < inputTypes.size(); i++)
        {
            auto shortName = AudioDeviceEntry::shortenTypeName(inputTypes[i]);
            if (shortName == settings.audioInputTypeFilter || inputTypes[i] == settings.audioInputTypeFilter)
            { cmbAudioInputTypeFilter.setSelectedId(i + 2, juce::dontSendNotification); break; }
        }
    }
    if (cmbAudioInputTypeFilter.getSelectedId() == 0)
        cmbAudioInputTypeFilter.setSelectedId(1, juce::dontSendNotification);

    cmbAudioOutputTypeFilter.clear(juce::dontSendNotification);
    cmbAudioOutputTypeFilter.addItem("All Drivers", 1);
    for (int i = 0; i < outputTypes.size(); i++)
    {
        auto shortName = AudioDeviceEntry::shortenTypeName(outputTypes[i]);
        cmbAudioOutputTypeFilter.addItem(shortName.isEmpty() ? outputTypes[i] : shortName, i + 2);
    }

    if (settings.audioOutputTypeFilter.isNotEmpty())
    {
        for (int i = 0; i < outputTypes.size(); i++)
        {
            auto shortName = AudioDeviceEntry::shortenTypeName(outputTypes[i]);
            if (shortName == settings.audioOutputTypeFilter || outputTypes[i] == settings.audioOutputTypeFilter)
            { cmbAudioOutputTypeFilter.setSelectedId(i + 2, juce::dontSendNotification); break; }
        }
    }
    if (cmbAudioOutputTypeFilter.getSelectedId() == 0)
        cmbAudioOutputTypeFilter.setSelectedId(1, juce::dontSendNotification);
}

void MainComponent::populateFilteredInputDeviceCombo()
{
    auto filter = getInputTypeFilter();
    filteredInputIndices.clear();
    cmbAudioInputDevice.clear(juce::dontSendNotification);

    for (int i = 0; i < scannedAudioInputs.size(); i++)
    {
        if (filter.isEmpty() || AudioDeviceEntry::shortenTypeName(scannedAudioInputs[i].typeName) == filter
            || scannedAudioInputs[i].typeName == filter)
        {
            filteredInputIndices.add(i);
            auto marker = getDeviceInUseMarker(scannedAudioInputs[i].deviceName,
                                                scannedAudioInputs[i].typeName, true);
            cmbAudioInputDevice.addItem(scannedAudioInputs[i].displayName + marker,
                                         filteredInputIndices.size());
        }
    }
}

void MainComponent::populateFilteredOutputDeviceCombos()
{
    auto filter = getOutputTypeFilter();
    filteredOutputIndices.clear();
    cmbAudioOutputDevice.clear(juce::dontSendNotification);
    cmbThruOutputDevice.clear(juce::dontSendNotification);

    for (int i = 0; i < scannedAudioOutputs.size(); i++)
    {
        if (filter.isEmpty() || AudioDeviceEntry::shortenTypeName(scannedAudioOutputs[i].typeName) == filter
            || scannedAudioOutputs[i].typeName == filter)
        {
            filteredOutputIndices.add(i);
            int id = filteredOutputIndices.size();
            auto marker = getDeviceInUseMarker(scannedAudioOutputs[i].deviceName,
                                                scannedAudioOutputs[i].typeName, false);
            cmbAudioOutputDevice.addItem(scannedAudioOutputs[i].displayName + marker, id);
            cmbThruOutputDevice.addItem(scannedAudioOutputs[i].displayName + marker, id);
        }
    }
}

//==============================================================================
// Returns " [ENGINE N]" if the device is in use by another engine, "" otherwise.
// typeName is checked for audio devices; for MIDI pass "".
//==============================================================================
juce::String MainComponent::getDeviceInUseMarker(const juce::String& devName,
                                                  const juce::String& typeName,
                                                  bool isInput)
{
    if (devName.isEmpty()) return {};

    juce::String result;

    for (int i = 0; i < (int)engines.size(); i++)
    {
        auto& eng = *engines[(size_t)i];
        bool isCurrent = (i == selectedEngine);

        if (isInput)
        {
            // MIDI input (typeName empty → MIDI check)
            if (typeName.isEmpty()
                && eng.getMtcInput().getIsRunning()
                && eng.getMtcInput().getCurrentDeviceName() == devName)
            {
                if (isCurrent) { result += juce::String::charToString(0x25CF); }   // "●"
                else           { return " [" + eng.getName() + "]"; }
            }

            // Audio input (LTC)
            if (typeName.isNotEmpty()
                && eng.getLtcInput().getIsRunning()
                && eng.getLtcInput().getCurrentDeviceName() == devName
                && eng.getLtcInput().getCurrentTypeName() == typeName)
            {
                if (isCurrent) { result += juce::String::charToString(0x25CF); }
                else           { return " [" + eng.getName() + "]"; }
            }
        }
        else
        {
            // MIDI output
            if (typeName.isEmpty()
                && eng.getMtcOutput().getIsRunning()
                && eng.getMtcOutput().getCurrentDeviceName() == devName)
            {
                if (isCurrent) { result += juce::String::charToString(0x25CF); }
                else           { return " [" + eng.getName() + "]"; }
            }

            // Audio output (LTC)
            if (typeName.isNotEmpty()
                && eng.getLtcOutput().getIsRunning()
                && eng.getLtcOutput().getCurrentDeviceName() == devName
                && eng.getLtcOutput().getCurrentTypeName() == typeName)
            {
                if (isCurrent) { result += juce::String::charToString(0x25CF); }
                else           { return " [" + eng.getName() + "]"; }
            }

            // Audio output (Thru)
            if (typeName.isNotEmpty()
                && eng.getAudioThru() && eng.getAudioThru()->getIsRunning()
                && eng.getAudioThru()->getCurrentDeviceName() == devName
                && eng.getAudioThru()->getCurrentTypeName() == typeName)
            {
                if (isCurrent) { result += juce::String::charToString(0x25CF); }
                else           { return " [" + eng.getName() + " THRU]"; }
            }
        }
    }

    // "●" prefix for current engine's active device
    if (result.isNotEmpty())
        return " " + result;

    return {};
}

void MainComponent::populateSampleRateCombo()
{
    cmbSampleRate.clear(juce::dontSendNotification);
    cmbSampleRate.addItem("Default", 1);
    cmbSampleRate.addItem("44100", 2);
    cmbSampleRate.addItem("48000", 3);
    cmbSampleRate.addItem("88200", 4);
    cmbSampleRate.addItem("96000", 5);
    cmbSampleRate.setSelectedId(1, juce::dontSendNotification);
}

void MainComponent::populateBufferSizeCombo()
{
    cmbBufferSize.clear(juce::dontSendNotification);
    cmbBufferSize.addItem("Default", 1);
    cmbBufferSize.addItem("32", 2);
    cmbBufferSize.addItem("64", 3);
    cmbBufferSize.addItem("128", 4);
    cmbBufferSize.addItem("256", 5);
    cmbBufferSize.addItem("512", 6);
    cmbBufferSize.addItem("1024", 7);
    cmbBufferSize.addItem("2048", 8);
    cmbBufferSize.setSelectedId(1, juce::dontSendNotification);
}

double MainComponent::getPreferredSampleRate() const
{
    switch (cmbSampleRate.getSelectedId())
    {
        case 2: return 44100; case 3: return 48000;
        case 4: return 88200; case 5: return 96000;
        default: return 0;
    }
}

int MainComponent::getPreferredBufferSize() const
{
    switch (cmbBufferSize.getSelectedId())
    {
        case 2: return 32; case 3: return 64; case 4: return 128;
        case 5: return 256; case 6: return 512; case 7: return 1024; case 8: return 2048;
        default: return 0;
    }
}

void MainComponent::restartAllAudioDevices()
{
    double sr = getPreferredSampleRate();
    int bs = getPreferredBufferSize();

    // Restart LTC input/output for ALL engines
    for (int i = 0; i < (int)engines.size(); i++)
    {
        auto& eng = *engines[(size_t)i];

        if (i == selectedEngine)
        {
            // Selected engine: restart via UI combos
            if (eng.getActiveInput() == InputSource::LTC && eng.getLtcInput().getIsRunning())
                startCurrentLtcInput();
            if (eng.isOutputLtcEnabled() && eng.getLtcOutput().getIsRunning())
                startCurrentLtcOutput();
        }
        else
        {
            // Non-selected engines: restart using their current device settings
            if (eng.getActiveInput() == InputSource::LTC && eng.getLtcInput().getIsRunning())
            {
                auto devName = eng.getLtcInput().getCurrentDeviceName();
                auto typeName = eng.getLtcInput().getCurrentTypeName();
                int ltcCh = eng.getLtcInput().getSelectedChannel();
                int thruCh = eng.getLtcInput().hasPassthruChannel() ? eng.getLtcInput().getPassthruChannel() : -1;
                eng.startLtcInput(typeName, devName, ltcCh, thruCh, sr, bs);
            }
            if (eng.isOutputLtcEnabled() && eng.getLtcOutput().getIsRunning())
            {
                auto devName = eng.getLtcOutput().getCurrentDeviceName();
                auto typeName = eng.getLtcOutput().getCurrentTypeName();
                int ch = eng.getLtcOutput().getSelectedChannel();
                eng.startLtcOutput(typeName, devName, ch, sr, bs);
            }
            if (eng.isPrimary() && eng.getAudioThru() && eng.getAudioThru()->getIsRunning())
            {
                auto devName = eng.getAudioThru()->getCurrentDeviceName();
                auto typeName = eng.getAudioThru()->getCurrentTypeName();
                int ch = eng.getAudioThru()->getSelectedChannel();
                eng.startThruOutput(typeName, devName, ch, sr, bs);
            }
        }
    }
    saveSettings();
}

void MainComponent::populateMidiAndNetworkCombos()
{
    // MIDI Input
    auto midiIns = juce::MidiInput::getAvailableDevices();
    cmbMidiInputDevice.clear(juce::dontSendNotification);
    for (int i = 0; i < midiIns.size(); i++)
    {
        auto marker = getDeviceInUseMarker(midiIns[i].name, "", true);
        cmbMidiInputDevice.addItem(midiIns[i].name + marker, i + 1);
    }

    // MIDI Output
    auto midiOuts = juce::MidiOutput::getAvailableDevices();
    cmbMidiOutputDevice.clear(juce::dontSendNotification);
    for (int i = 0; i < midiOuts.size(); i++)
    {
        auto marker = getDeviceInUseMarker(midiOuts[i].name, "", false);
        cmbMidiOutputDevice.addItem(midiOuts[i].name + marker, i + 1);
    }

    // Art-Net interfaces
    auto nets = getNetworkInterfaces();
    cmbArtnetInputInterface.clear(juce::dontSendNotification);
    cmbArtnetOutputInterface.clear(juce::dontSendNotification);

    // Helper: check if an Art-Net interface combo ID is in use by another engine
    auto getArtnetMarker = [&](int comboId, bool isInput) -> juce::String
    {
        juce::String currentDot;

        for (int i = 0; i < (int)engines.size(); i++)
        {
            auto& eng = *engines[(size_t)i];
            bool isCurrent = (i == selectedEngine);

            if (isInput && eng.getArtnetInput().getIsRunning())
            {
                int inUseComboId = eng.getArtnetInput().getSelectedInterface() + 1;
                if (inUseComboId == comboId)
                {
                    if (isCurrent) currentDot = juce::String(" ") + juce::String::charToString(0x25CF);
                    else           return " [" + eng.getName() + "]";
                }
            }
            if (!isInput && eng.getArtnetOutput().getIsRunning())
            {
                int inUseComboId = eng.getArtnetOutput().getSelectedInterface() + 2;
                if (inUseComboId == comboId)
                {
                    if (isCurrent) currentDot = juce::String(" ") + juce::String::charToString(0x25CF);
                    else           return " [" + eng.getName() + "]";
                }
            }
        }
        return currentDot;
    };

    cmbArtnetInputInterface.addItem("All Interfaces" + getArtnetMarker(1, true), 1);
    cmbArtnetOutputInterface.addItem("All Interfaces (Broadcast)" + getArtnetMarker(1, false), 1);
    for (int i = 0; i < nets.size(); i++)
    {
        auto label = nets[i].name + " (" + nets[i].ip + ")";
        cmbArtnetInputInterface.addItem(label + getArtnetMarker(i + 2, true), i + 2);
        cmbArtnetOutputInterface.addItem(label + getArtnetMarker(i + 2, false), i + 2);
    }
}

void MainComponent::populateAudioCombos()
{
    populateTypeFilterCombos();
    populateFilteredInputDeviceCombo();
    populateFilteredOutputDeviceCombos();
}

//==============================================================================
// SETTINGS LOAD / SAVE
//==============================================================================
void MainComponent::loadAndApplyNonAudioSettings()
{
    if (!settings.load())
    {
        settingsLoaded = true;
        syncUIFromEngine();
        return;
    }

    // Ensure we have enough engines
    while (engines.size() < settings.engines.size())
        engines.push_back(std::make_unique<TimecodeEngine>((int)engines.size()));

    // Apply per-engine settings
    for (int i = 0; i < (int)settings.engines.size() && i < (int)engines.size(); i++)
    {
        auto& es = settings.engines[(size_t)i];
        auto& eng = *engines[(size_t)i];

        if (es.engineName.isNotEmpty())
            eng.setName(es.engineName);

        eng.setFrameRate(TimecodeEngine::indexToFps(es.fpsSelection));
        eng.setFpsConvertEnabled(es.fpsConvertEnabled);
        eng.setOutputFrameRate(TimecodeEngine::indexToFps(es.outputFpsSelection));
        eng.setUserOverrodeLtcFps(es.ltcFpsUserOverride);

        eng.setOutputMtcEnabled(es.mtcOutEnabled);
        eng.setOutputArtnetEnabled(es.artnetOutEnabled);
        eng.setOutputLtcEnabled(es.ltcOutEnabled);
        eng.setOutputThruEnabled(es.thruOutEnabled);

        eng.setMtcOutputOffset(es.mtcOutputOffset);
        eng.setArtnetOutputOffset(es.artnetOutputOffset);
        eng.setLtcOutputOffset(es.ltcOutputOffset);

        eng.getLtcInput().setInputGain((float)es.ltcInputGain / 100.0f);
        eng.getLtcInput().setPassthruGain((float)es.thruInputGain / 100.0f);
        eng.getLtcOutput().setOutputGain((float)es.ltcOutputGain / 100.0f);
        if (eng.getAudioThru())
            eng.getAudioThru()->setOutputGain((float)es.thruOutputGain / 100.0f);

        // Set input source (defer actual device start)
        auto src = TimecodeEngine::stringToInputSource(es.inputSource);
        eng.setInputSource(src);

        // Start non-audio protocols
        if (src == InputSource::MTC)
        {
            int idx = findDeviceByName(cmbMidiInputDevice, es.midiInputDevice);
            eng.startMtcInput(idx);
        }
        else if (src == InputSource::ArtNet)
        {
            eng.startArtnetInput(es.artnetInputInterface);
        }

        // Start non-audio outputs
        if (es.mtcOutEnabled)
        {
            int idx = findDeviceByName(cmbMidiOutputDevice, es.midiOutputDevice);
            eng.startMtcOutput(idx);
        }
        if (es.artnetOutEnabled)
            eng.startArtnetOutput(es.artnetOutputInterface);
    }

    // Global settings
    cmbSampleRate.setSelectedId(sampleRateToComboId(settings.preferredSampleRate), juce::dontSendNotification);
    cmbBufferSize.setSelectedId(bufferSizeToComboId(settings.preferredBufferSize), juce::dontSendNotification);

    selectedEngine = juce::jlimit(0, (int)engines.size() - 1, settings.selectedEngine);
    rebuildTabButtons();

    settingsLoaded = true;
    syncUIFromEngine();
}

void MainComponent::applyAudioSettings()
{
    // Apply audio device settings for all engines
    for (int i = 0; i < (int)settings.engines.size() && i < (int)engines.size(); i++)
    {
        auto& es = settings.engines[(size_t)i];
        auto& eng = *engines[(size_t)i];

        // Audio inputs (LTC)
        if (eng.getActiveInput() == InputSource::LTC)
        {
            // Find the device in our scanned list
            int audioInIdx = findFilteredIndex(filteredInputIndices, scannedAudioInputs,
                                                es.audioInputType, es.audioInputDevice);

            if (i == selectedEngine)
            {
                // For selected engine, use combo box path
                if (audioInIdx >= 0) cmbAudioInputDevice.setSelectedId(audioInIdx + 1, juce::dontSendNotification);
                startCurrentLtcInput();
            }
            else if (!es.audioInputDevice.isEmpty())
            {
                // For non-selected engines, start directly
                int thruCh = (eng.isPrimary() && eng.isOutputThruEnabled()) ? es.thruInputChannel : -1;
                eng.startLtcInput(es.audioInputType, es.audioInputDevice,
                                   es.audioInputChannel, thruCh,
                                   getPreferredSampleRate(), getPreferredBufferSize());
            }
        }

        // Audio outputs (LTC)
        if (eng.isOutputLtcEnabled())
        {
            if (i == selectedEngine)
            {
                int audioOutIdx = findFilteredIndex(filteredOutputIndices, scannedAudioOutputs,
                                                     es.audioOutputType, es.audioOutputDevice);
                if (audioOutIdx >= 0) cmbAudioOutputDevice.setSelectedId(audioOutIdx + 1, juce::dontSendNotification);
                startCurrentLtcOutput();
            }
            else if (!es.audioOutputDevice.isEmpty())
            {
                int ch = es.audioOutputStereo ? -1 : es.audioOutputChannel;
                eng.startLtcOutput(es.audioOutputType, es.audioOutputDevice, ch,
                                    getPreferredSampleRate(), getPreferredBufferSize());
            }
        }

        // AudioThru (only engine 0)
        if (eng.isPrimary() && eng.isOutputThruEnabled() && eng.getAudioThru() && !eng.getAudioThru()->getIsRunning())
        {
            if (eng.getLtcInput().getIsRunning())
            {
                if (i == selectedEngine)
                {
                    int thruOutIdx = findFilteredIndex(filteredOutputIndices, scannedAudioOutputs,
                                                        es.thruOutputType, es.thruOutputDevice);
                    if (thruOutIdx >= 0) cmbThruOutputDevice.setSelectedId(thruOutIdx + 1, juce::dontSendNotification);
                    startCurrentThruOutput();
                }
                else if (!es.thruOutputDevice.isEmpty())
                {
                    int ch = es.thruOutputStereo ? -1 : es.thruOutputChannel;
                    eng.startThruOutput(es.thruOutputType, es.thruOutputDevice, ch,
                                         getPreferredSampleRate(), getPreferredBufferSize());
                }
            }
        }
    }

    updateDeviceSelectorVisibility();
}

void MainComponent::saveSettings()
{
    if (!settingsLoaded) return;
    settingsDirty = true;
    settingsSaveCountdown = kSaveDelayTicks;
}

void MainComponent::flushSettings()
{
    if (!settingsLoaded) return;
    settingsDirty = false;

    settings.selectedEngine = selectedEngine;
    settings.audioInputTypeFilter  = (cmbAudioInputTypeFilter.getSelectedId() <= 1) ? "" : cmbAudioInputTypeFilter.getText();
    settings.audioOutputTypeFilter = (cmbAudioOutputTypeFilter.getSelectedId() <= 1) ? "" : cmbAudioOutputTypeFilter.getText();
    settings.preferredSampleRate = getPreferredSampleRate();
    settings.preferredBufferSize = getPreferredBufferSize();

    settings.engines.resize(engines.size());

    bool audioReady = !scannedAudioInputs.isEmpty() || !scannedAudioOutputs.isEmpty();

    for (int i = 0; i < (int)engines.size(); i++)
    {
        auto& eng = *engines[(size_t)i];
        auto& es = settings.engines[(size_t)i];

        es.engineName = eng.getName();
        es.inputSource = TimecodeEngine::inputSourceToString(eng.getActiveInput());
        es.fpsSelection = TimecodeEngine::fpsToIndex(eng.getCurrentFps());
        es.fpsConvertEnabled = eng.isFpsConvertEnabled();
        es.outputFpsSelection = TimecodeEngine::fpsToIndex(eng.getOutputFps());
        es.ltcFpsUserOverride = eng.getUserOverrodeLtcFps();

        es.mtcOutEnabled = eng.isOutputMtcEnabled();
        es.artnetOutEnabled = eng.isOutputArtnetEnabled();
        es.ltcOutEnabled = eng.isOutputLtcEnabled();
        es.thruOutEnabled = eng.isOutputThruEnabled();

        es.mtcOutputOffset = eng.getMtcOutputOffset();
        es.artnetOutputOffset = eng.getArtnetOutputOffset();
        es.ltcOutputOffset = eng.getLtcOutputOffset();

        es.ltcInputGain = (int)(eng.getLtcInput().getInputGain() * 100.0f);
        es.thruInputGain = (int)(eng.getLtcInput().getPassthruGain() * 100.0f);
        es.ltcOutputGain = (int)(eng.getLtcOutput().getOutputGain() * 100.0f);
        if (eng.getAudioThru())
            es.thruOutputGain = (int)(eng.getAudioThru()->getOutputGain() * 100.0f);

        // For the selected engine, read from combos (may have been changed)
        if (i == selectedEngine)
        {
            es.midiInputDevice = cmbMidiInputDevice.getText();
            es.midiOutputDevice = cmbMidiOutputDevice.getText();
            es.artnetInputInterface = cmbArtnetInputInterface.getSelectedId() - 1;
            es.artnetOutputInterface = cmbArtnetOutputInterface.getSelectedId() - 1;

            if (audioReady)
            {
                auto inEntry = getSelectedAudioInput();
                if (inEntry.deviceName.isNotEmpty()) { es.audioInputDevice = inEntry.deviceName; es.audioInputType = inEntry.typeName; }
                es.audioInputChannel = cmbAudioInputChannel.getSelectedId() - 1;

                auto outEntry = getSelectedAudioOutput();
                if (outEntry.deviceName.isNotEmpty()) { es.audioOutputDevice = outEntry.deviceName; es.audioOutputType = outEntry.typeName; }
                es.audioOutputStereo = (cmbAudioOutputChannel.getSelectedId() == kStereoItemId);
                es.audioOutputChannel = es.audioOutputStereo ? 0 : (cmbAudioOutputChannel.getSelectedId() - 1);

                auto thruEntry = getSelectedThruOutput();
                if (thruEntry.deviceName.isNotEmpty()) { es.thruOutputDevice = thruEntry.deviceName; es.thruOutputType = thruEntry.typeName; }
                es.thruOutputStereo = (cmbThruOutputChannel.getSelectedId() == kStereoItemId);
                es.thruOutputChannel = es.thruOutputStereo ? 0 : (cmbThruOutputChannel.getSelectedId() - 1);
                es.thruInputChannel = cmbThruInputChannel.getSelectedId() - 1;
            }
        }
        else
        {
            // For non-selected engines, read device names from the running handler
            // state to keep settings in sync even if devices were reconnected or
            // changed programmatically (e.g., via restartAllAudioDevices).
            if (eng.getMtcInput().getIsRunning())
                es.midiInputDevice = eng.getMtcInput().getCurrentDeviceName();
            if (eng.getMtcOutput().getIsRunning())
                es.midiOutputDevice = eng.getMtcOutput().getCurrentDeviceName();
            if (eng.getLtcInput().getIsRunning())
            {
                es.audioInputDevice  = eng.getLtcInput().getCurrentDeviceName();
                es.audioInputType    = eng.getLtcInput().getCurrentTypeName();
                es.audioInputChannel = eng.getLtcInput().getSelectedChannel();
                if (eng.getLtcInput().hasPassthruChannel())
                    es.thruInputChannel = eng.getLtcInput().getPassthruChannel();
            }
            if (eng.getLtcOutput().getIsRunning())
            {
                es.audioOutputDevice  = eng.getLtcOutput().getCurrentDeviceName();
                es.audioOutputType    = eng.getLtcOutput().getCurrentTypeName();
                int ch = eng.getLtcOutput().getSelectedChannel();
                es.audioOutputStereo  = (ch == -1);
                es.audioOutputChannel = (ch == -1) ? 0 : ch;
            }
            if (eng.getAudioThru() && eng.getAudioThru()->getIsRunning())
            {
                es.thruOutputDevice = eng.getAudioThru()->getCurrentDeviceName();
                es.thruOutputType   = eng.getAudioThru()->getCurrentTypeName();
                int ch = eng.getAudioThru()->getSelectedChannel();
                es.thruOutputStereo  = (ch == -1);
                es.thruOutputChannel = (ch == -1) ? 0 : ch;
            }
            // ArtNet interfaces preserved from last save when engine was selected
        }
    }

    settings.save();
}

int MainComponent::findDeviceByName(const juce::ComboBox& cmb, const juce::String& name)
{
    if (name.isEmpty()) return -1;
    static const juce::String dotSuffix = juce::String(" ") + juce::String::charToString(0x25CF);
    for (int i = 0; i < cmb.getNumItems(); i++)
    {
        auto text = cmb.getItemText(i);
        // Exact match, or match ignoring the " [ENGINE N]" or " ●" marker suffix
        if (text == name || text.startsWith(name + " [") || text == name + dotSuffix)
            return i;
    }
    return -1;
}

int MainComponent::findFilteredIndex(const juce::Array<int>& filteredIndices,
                                      const juce::Array<AudioDeviceEntry>& entries,
                                      const juce::String& typeName, const juce::String& deviceName)
{
    for (int f = 0; f < filteredIndices.size(); f++)
    {
        int realIdx = filteredIndices[f];
        if (realIdx < entries.size()
            && entries[realIdx].deviceName == deviceName
            && (typeName.isEmpty() || entries[realIdx].typeName == typeName))
            return f;
    }
    return -1;
}

AudioDeviceEntry MainComponent::getSelectedAudioInput() const
{
    int sel = cmbAudioInputDevice.getSelectedId() - 1;
    if (sel >= 0 && sel < filteredInputIndices.size())
    { int realIdx = filteredInputIndices[sel]; if (realIdx < scannedAudioInputs.size()) return scannedAudioInputs[realIdx]; }
    return {};
}

AudioDeviceEntry MainComponent::getSelectedAudioOutput() const
{
    int sel = cmbAudioOutputDevice.getSelectedId() - 1;
    if (sel >= 0 && sel < filteredOutputIndices.size())
    { int realIdx = filteredOutputIndices[sel]; if (realIdx < scannedAudioOutputs.size()) return scannedAudioOutputs[realIdx]; }
    return {};
}

AudioDeviceEntry MainComponent::getSelectedThruOutput() const
{
    int sel = cmbThruOutputDevice.getSelectedId() - 1;
    if (sel >= 0 && sel < filteredOutputIndices.size())
    { int realIdx = filteredOutputIndices[sel]; if (realIdx < scannedAudioOutputs.size()) return scannedAudioOutputs[realIdx]; }
    return {};
}

//==============================================================================
// CHANNEL HELPERS
//==============================================================================
void MainComponent::populateAudioInputChannels()
{
    auto& eng = currentEngine();
    int prevLtcCh = cmbAudioInputChannel.getSelectedId();
    int prevThru = cmbThruInputChannel.getSelectedId();
    cmbAudioInputChannel.clear(juce::dontSendNotification);
    cmbThruInputChannel.clear(juce::dontSendNotification);

    int n = juce::jmax(2, eng.getLtcInput().getChannelCount());
    for (int i = 0; i < n; i++)
    {
        auto nm = "Ch " + juce::String(i + 1);
        cmbAudioInputChannel.addItem(nm, i + 1);
        cmbThruInputChannel.addItem(nm, i + 1);
    }

    cmbAudioInputChannel.setSelectedId(
        (prevLtcCh > 0 && prevLtcCh <= n) ? prevLtcCh : 1, juce::dontSendNotification);
    cmbThruInputChannel.setSelectedId(
        (prevThru > 0 && prevThru <= n) ? prevThru : juce::jmin(2, n), juce::dontSendNotification);
}

void MainComponent::populateAudioOutputChannels()
{
    auto& eng = currentEngine();
    int prev = cmbAudioOutputChannel.getSelectedId();
    cmbAudioOutputChannel.clear(juce::dontSendNotification);
    int n = juce::jmax(2, eng.getLtcOutput().getChannelCount());
    if (n >= 2) cmbAudioOutputChannel.addItem("Ch 1 + Ch 2", kStereoItemId);
    for (int i = 0; i < n; i++) cmbAudioOutputChannel.addItem("Ch " + juce::String(i+1), i+1);
    if (prev == kStereoItemId && n >= 2) cmbAudioOutputChannel.setSelectedId(kStereoItemId, juce::dontSendNotification);
    else if (prev > 0 && prev <= n) cmbAudioOutputChannel.setSelectedId(prev, juce::dontSendNotification);
    else cmbAudioOutputChannel.setSelectedId(n >= 2 ? kStereoItemId : 1, juce::dontSendNotification);
}

void MainComponent::populateThruOutputChannels()
{
    auto& eng = currentEngine();
    if (!eng.getAudioThru()) return;
    int prev = cmbThruOutputChannel.getSelectedId();
    cmbThruOutputChannel.clear(juce::dontSendNotification);
    int n = juce::jmax(2, eng.getAudioThru()->getChannelCount());
    if (n >= 2) cmbThruOutputChannel.addItem("Ch 1 + Ch 2", kStereoItemId);
    for (int i = 0; i < n; i++) cmbThruOutputChannel.addItem("Ch " + juce::String(i+1), i+1);
    if (prev == kStereoItemId && n >= 2) cmbThruOutputChannel.setSelectedId(kStereoItemId, juce::dontSendNotification);
    else if (prev > 0 && prev <= n) cmbThruOutputChannel.setSelectedId(prev, juce::dontSendNotification);
    else cmbThruOutputChannel.setSelectedId(n >= 2 ? kStereoItemId : 1, juce::dontSendNotification);
}

int MainComponent::getChannelFromCombo(const juce::ComboBox& cmb) const
{
    int id = cmb.getSelectedId();
    if (id == kStereoItemId) return -1;
    return id - 1;
}

//==============================================================================
// VISIBILITY (updated for collapse + engine)
//==============================================================================
void MainComponent::updateDeviceSelectorVisibility()
{
    auto& eng = currentEngine();
    auto input = eng.getActiveInput();
    bool showMidiIn   = (input == InputSource::MTC)    && inputConfigExpanded;
    bool showArtnetIn = (input == InputSource::ArtNet)  && inputConfigExpanded;
    bool showLtcIn    = (input == InputSource::LTC)     && inputConfigExpanded;
    bool showAudioOut = (eng.isOutputLtcEnabled() || (eng.isPrimary() && eng.isOutputThruEnabled()));
    bool hasInputConfig = (input != InputSource::SystemTime);

    btnCollapseInput.setVisible(hasInputConfig);
    updateCollapseButtonText(btnCollapseInput, inputConfigExpanded);

    cmbMidiInputDevice.setVisible(showMidiIn);       lblMidiInputDevice.setVisible(showMidiIn);
    cmbArtnetInputInterface.setVisible(showArtnetIn); lblArtnetInputInterface.setVisible(showArtnetIn);

    cmbAudioInputTypeFilter.setVisible(showLtcIn);    lblAudioInputTypeFilter.setVisible(showLtcIn);
    cmbSampleRate.setVisible(showLtcIn);              lblSampleRate.setVisible(showLtcIn);
    cmbBufferSize.setVisible(showLtcIn);              lblBufferSize.setVisible(showLtcIn);
    cmbAudioInputDevice.setVisible(showLtcIn);        lblAudioInputDevice.setVisible(showLtcIn);
    cmbAudioInputChannel.setVisible(showLtcIn);       lblAudioInputChannel.setVisible(showLtcIn);
    sldLtcInputGain.setVisible(showLtcIn);            lblLtcInputGain.setVisible(showLtcIn);
    mtrLtcInput.setVisible(showLtcIn);

    bool showThruInput = showLtcIn && eng.isPrimary() && eng.isOutputThruEnabled();
    cmbThruInputChannel.setVisible(showThruInput);  lblThruInputChannel.setVisible(showThruInput);
    sldThruInputGain.setVisible(showThruInput);     lblThruInputGain.setVisible(showThruInput);
    mtrThruInput.setVisible(showThruInput);
    lblInputStatus.setVisible(true);

    // Output sections
    bool showMtcConfig    = eng.isOutputMtcEnabled()    && mtcOutExpanded;
    bool showArtnetConfig = eng.isOutputArtnetEnabled() && artnetOutExpanded;
    bool showLtcConfig    = eng.isOutputLtcEnabled()    && ltcOutExpanded;
    bool showThruConfig   = eng.isPrimary() && eng.isOutputThruEnabled() && thruOutExpanded;

    btnCollapseMtcOut.setVisible(eng.isOutputMtcEnabled());
    btnCollapseArtnetOut.setVisible(eng.isOutputArtnetEnabled());
    btnCollapseLtcOut.setVisible(eng.isOutputLtcEnabled());
    btnCollapseThruOut.setVisible(eng.isPrimary() && eng.isOutputThruEnabled());
    updateCollapseButtonText(btnCollapseMtcOut, mtcOutExpanded);
    updateCollapseButtonText(btnCollapseArtnetOut, artnetOutExpanded);
    updateCollapseButtonText(btnCollapseLtcOut, ltcOutExpanded);
    updateCollapseButtonText(btnCollapseThruOut, thruOutExpanded);

    cmbMidiOutputDevice.setVisible(showMtcConfig);       lblMidiOutputDevice.setVisible(showMtcConfig);
    sldMtcOffset.setVisible(showMtcConfig);              lblMtcOffset.setVisible(showMtcConfig);
    lblOutputMtcStatus.setVisible(eng.isOutputMtcEnabled());

    cmbArtnetOutputInterface.setVisible(showArtnetConfig); lblArtnetOutputInterface.setVisible(showArtnetConfig);
    sldArtnetOffset.setVisible(showArtnetConfig);          lblArtnetOffset.setVisible(showArtnetConfig);
    lblOutputArtnetStatus.setVisible(eng.isOutputArtnetEnabled());

    cmbAudioOutputTypeFilter.setVisible(showAudioOut && (showLtcConfig || showThruConfig));
    lblAudioOutputTypeFilter.setVisible(showAudioOut && (showLtcConfig || showThruConfig));

    cmbAudioOutputDevice.setVisible(showLtcConfig);  lblAudioOutputDevice.setVisible(showLtcConfig);
    cmbAudioOutputChannel.setVisible(showLtcConfig); lblAudioOutputChannel.setVisible(showLtcConfig);
    sldLtcOutputGain.setVisible(showLtcConfig);      lblLtcOutputGain.setVisible(showLtcConfig);
    mtrLtcOutput.setVisible(showLtcConfig);
    sldLtcOffset.setVisible(showLtcConfig);            lblLtcOffset.setVisible(showLtcConfig);
    lblOutputLtcStatus.setVisible(eng.isOutputLtcEnabled());

    // AudioThru controls: only visible for primary engine
    bool showThruSection = eng.isPrimary();
    btnThruOut.setVisible(showThruSection);
    cmbThruOutputDevice.setVisible(showThruConfig);  lblThruOutputDevice.setVisible(showThruConfig);
    cmbThruOutputChannel.setVisible(showThruConfig); lblThruOutputChannel.setVisible(showThruConfig);
    sldThruOutputGain.setVisible(showThruConfig);    lblThruOutputGain.setVisible(showThruConfig);
    mtrThruOutput.setVisible(showThruConfig);
    lblOutputThruStatus.setVisible(eng.isPrimary() && eng.isOutputThruEnabled());

    bool anyDevice = (input != InputSource::SystemTime) || eng.isOutputMtcEnabled() || eng.isOutputArtnetEnabled() || eng.isOutputLtcEnabled() || (eng.isPrimary() && eng.isOutputThruEnabled());
    btnRefreshDevices.setVisible(anyDevice);

    resized();
}

void MainComponent::updateStatusLabels()
{
    auto& eng = currentEngine();

    lblInputStatus.setText(eng.getInputStatusText(), juce::dontSendNotification);
    lblInputStatus.setColour(juce::Label::textColourId,
                             eng.isSourceActive() ? getInputColour(eng.getActiveInput()) : textDim);

    if (eng.isOutputMtcEnabled() && eng.getMtcOutput().getIsRunning())
        lblOutputMtcStatus.setText(eng.getMtcOutput().isPaused() ? "PAUSED" : eng.getMtcOutStatusText(), juce::dontSendNotification);
    else
        lblOutputMtcStatus.setText(eng.getMtcOutStatusText(), juce::dontSendNotification);

    if (eng.isOutputArtnetEnabled() && eng.getArtnetOutput().getIsRunning())
        lblOutputArtnetStatus.setText(eng.getArtnetOutput().isPaused() ? "PAUSED" : eng.getArtnetOutStatusText(), juce::dontSendNotification);
    else
        lblOutputArtnetStatus.setText(eng.getArtnetOutStatusText(), juce::dontSendNotification);

    if (eng.isOutputLtcEnabled())
    {
        if (eng.getLtcOutput().getIsRunning())
            lblOutputLtcStatus.setText(eng.getLtcOutput().isPaused() ? "PAUSED" : eng.getLtcOutStatusText(), juce::dontSendNotification);
        else
            lblOutputLtcStatus.setText(eng.getLtcOutStatusText(), juce::dontSendNotification);
    }

    if (eng.isPrimary() && eng.isOutputThruEnabled())
    {
        juce::String thruStatus = eng.getThruOutStatusText();
        if (eng.getAudioThru() && eng.getAudioThru()->getIsRunning() && eng.getLtcInput().getIsRunning())
        {
            uint32_t under = eng.getLtcInput().getPassthruUnderruns();
            uint32_t over  = eng.getLtcInput().getPassthruOverruns();
            if (under > 0 || over > 0)
                thruStatus += " [XRUNS: " + juce::String(under + over) + "]";

            double inBuf  = (double)eng.getLtcInput().getActualBufferSize();
            double outBuf = (double)eng.getAudioThru()->getActualBufferSize();
            double sr     = eng.getLtcInput().getActualSampleRate();
            if (sr > 0.0)
            {
                double latencyMs = (inBuf + outBuf) / sr * 1000.0;
                if (latencyMs > 10.0)
                    thruStatus += " [LAT: " + juce::String(latencyMs, 1) + "ms]";
            }
        }
        lblOutputThruStatus.setText(thruStatus, juce::dontSendNotification);
    }
}

//==============================================================================
// MINI STRIP — compact timecode monitors for non-selected engines
//==============================================================================
int MainComponent::getMiniStripHeight() const
{
    if (engines.size() <= 1) return 0;
    return (int)(engines.size() - 1) * kMiniStripRowH + 8;
}

void MainComponent::paintMiniStrip(juce::Graphics& g)
{
    if (engines.size() <= 1) return;

    int rowY = miniStripArea.getY() + 4;
    int centerX = miniStripArea.getCentreX();
    int counterW = juce::jmin(320, miniStripArea.getWidth() - 20);

    for (int i = 0; i < (int)engines.size(); i++)
    {
        if (i == selectedEngine) continue;

        auto& eng = *engines[(size_t)i];
        auto src = eng.getActiveInput();
        auto tc = eng.getCurrentTimecode();
        bool active = eng.isSourceActive();
        juce::Colour srcColour = getInputColour(src);

        int x = centerX - counterW / 2;
        auto rowRect = juce::Rectangle<int>(x, rowY, counterW, kMiniStripRowH - 4);

        // Background
        g.setColour(juce::Colour(0xFF0D0E12).withAlpha(0.8f));
        g.fillRoundedRectangle(rowRect.toFloat(), 4.0f);

        // Border (subtle, source-colored when active)
        g.setColour(active ? srcColour.withAlpha(0.3f) : borderCol);
        g.drawRoundedRectangle(rowRect.toFloat(), 4.0f, 1.0f);

        int innerX = rowRect.getX() + 8;
        int innerY = rowRect.getY();
        int innerH = rowRect.getHeight();

        // Status dot
        juce::Colour dotCol = active ? srcColour : textDim;
        g.setColour(dotCol);
        g.fillEllipse((float)innerX, (float)(innerY + innerH / 2 - 3), 6.0f, 6.0f);
        innerX += 12;

        // Engine name
        g.setFont(juce::Font(juce::FontOptions(getMonoFontName(), 9.0f, juce::Font::bold)));
        g.setColour(active ? textBright : textMid);
        g.drawText(eng.getName(), innerX, innerY, 80, innerH, juce::Justification::centredLeft);
        innerX += 82;

        // Timecode (big monospace) — use '.' before frames to match main display
        juce::String tcStr = juce::String(tc.hours).paddedLeft('0', 2) + ":"
                           + juce::String(tc.minutes).paddedLeft('0', 2) + ":"
                           + juce::String(tc.seconds).paddedLeft('0', 2) + "."
                           + juce::String(tc.frames).paddedLeft('0', 2);

        g.setFont(juce::Font(juce::FontOptions(getMonoFontName(), 13.0f, juce::Font::bold)));
        g.setColour(active ? juce::Colour(0xFF00E676) : textDim);
        g.drawText(tcStr, innerX, innerY, 120, innerH, juce::Justification::centredLeft);
        innerX += 122;

        // Source type label
        g.setFont(juce::Font(juce::FontOptions(getMonoFontName(), 8.0f, juce::Font::plain)));
        g.setColour(active ? srcColour.withAlpha(0.7f) : textDim.withAlpha(0.5f));
        g.drawText(TimecodeEngine::getInputName(src), innerX, innerY, 50, innerH, juce::Justification::centredLeft);

        rowY += kMiniStripRowH;
    }
}

void MainComponent::mouseDown(const juce::MouseEvent& e)
{
    if (engines.size() <= 1) return;
    if (!miniStripArea.contains(e.getPosition())) return;

    int rowY = miniStripArea.getY() + 4;

    for (int i = 0; i < (int)engines.size(); i++)
    {
        if (i == selectedEngine) continue;

        auto rowRect = juce::Rectangle<int>(miniStripArea.getX(), rowY,
                                             miniStripArea.getWidth(), kMiniStripRowH);
        if (rowRect.contains(e.getPosition()))
        {
            selectEngine(i);
            return;
        }
        rowY += kMiniStripRowH;
    }
}

//==============================================================================
// PAINT
//==============================================================================
void MainComponent::paint(juce::Graphics& g)
{
    if (engines.empty()) return;  // guard: same as resized()

    g.fillAll(bgDark);
    auto bounds = getLocalBounds();
    int panelWidth = 240;

    // Left panel background
    auto leftPanel = bounds.removeFromLeft(panelWidth);
    g.setColour(bgPanel); g.fillRect(leftPanel);
    g.setColour(borderCol); g.drawLine((float)leftPanel.getRight(), 0.0f, (float)leftPanel.getRight(), (float)getHeight(), 1.0f);
    g.setColour(textDim); g.setFont(juce::Font(juce::FontOptions(getMonoFontName(), 10.0f, juce::Font::bold)));
    g.drawText(">> SOURCE INPUT", leftPanel.withHeight(40).translated(0, 32).reduced(16, 0), juce::Justification::centredLeft);

    // Right panel background
    auto rightPanel = bounds.removeFromRight(panelWidth);
    g.setColour(bgPanel); g.fillRect(rightPanel);
    g.setColour(borderCol); g.drawLine((float)rightPanel.getX(), 0.0f, (float)rightPanel.getX(), (float)getHeight(), 1.0f);
    g.setColour(textDim); g.drawText(">> OUTPUTS", rightPanel.withHeight(40).translated(0, 32).reduced(16, 0), juce::Justification::centredLeft);

    // Top bar
    g.setColour(bgDarker); g.fillRect(0, 0, getWidth(), 32);
    g.setColour(borderCol); g.drawLine(0.0f, 32.0f, (float)getWidth(), 32.0f, 1.0f);
    g.setColour(textDim); g.setFont(juce::Font(juce::FontOptions(getMonoFontName(), 11.0f, juce::Font::bold)));
    g.drawText("SUPER TIMECODE CONVERTER", juce::Rectangle<int>(0, 0, getWidth(), 32), juce::Justification::centred);

    // Tab bar (below top bar)
    g.setColour(bgDarker); g.fillRect(0, 32, getWidth(), kTabBarHeight);
    g.setColour(borderCol); g.drawLine(0.0f, (float)(32 + kTabBarHeight), (float)getWidth(), (float)(32 + kTabBarHeight), 1.0f);

    // Bottom bar
    int bbH = 24;
    g.setColour(bgDarker); g.fillRect(0, getHeight() - bbH, getWidth(), bbH);
    g.setColour(borderCol); g.drawLine(0.0f, (float)(getHeight() - bbH), (float)getWidth(), (float)(getHeight() - bbH), 1.0f);
    g.setColour(textDim); g.setFont(juce::Font(juce::FontOptions(getMonoFontName(), 9.0f, juce::Font::plain)));
    juce::String verStr = "STC v" + juce::JUCEApplication::getInstance()->getApplicationVersion()
                        + "  |  Fiverecords " + juce::String::charToString(0x00A9) + " 2026";
    g.drawText(verStr,
               juce::Rectangle<int>(10, getHeight() - bbH, 280, bbH), juce::Justification::centredLeft);

    auto& eng = currentEngine();
    juce::String statusText; juce::Colour statusColour;
    if (eng.isSourceActive()) { statusText = TimecodeEngine::getInputName(eng.getActiveInput()) + " ACTIVE"; statusColour = getInputColour(eng.getActiveInput()); }
    else if (eng.isInputStarted()) { statusText = TimecodeEngine::getInputName(eng.getActiveInput()) + " PAUSED"; statusColour = juce::Colour(0xFFFFAB00); }
    else { statusText = TimecodeEngine::getInputName(eng.getActiveInput()) + " STOPPED"; statusColour = textDim; }

    // Show engine name in status if multiple engines
    if (engines.size() > 1)
        statusText = "[" + eng.getName() + "] " + statusText;

    g.setColour(statusColour); g.drawText(statusText, juce::Rectangle<int>(getWidth() - 280, getHeight() - bbH, 270, bbH), juce::Justification::centredRight);

    // Frame rate labels
    g.setColour(textDim); g.setFont(juce::Font(juce::FontOptions(getMonoFontName(), 10.0f, juce::Font::bold)));
    auto centerArea = getLocalBounds().reduced(panelWidth, 0);
    int fpsLabelBase = getHeight() - bbH - (eng.isFpsConvertEnabled() ? 172 : 90);
    g.drawText(eng.isFpsConvertEnabled() ? "INPUT FPS" : "FRAME RATE", centerArea.getX(), fpsLabelBase, centerArea.getWidth(), 14, juce::Justification::centred);

    if (eng.isFpsConvertEnabled())
    {
        g.setColour(accentCyan);
        g.drawText("OUTPUT FPS", centerArea.getX(), fpsLabelBase + 84, centerArea.getWidth(), 14, juce::Justification::centred);
    }

    // Mini timecode strip for non-selected engines
    paintMiniStrip(g);
}

//==============================================================================
// RESIZED
//==============================================================================
void MainComponent::resized()
{
    if (engines.empty()) return;  // guard: resized() can fire before engines are created

    auto bounds = getLocalBounds();
    int panelWidth = 240, topBar = 32, bottomBar = 24;
    int tabBar = kTabBarHeight;

    // ===== TAB BAR (full window width — the bar sits above the side panels) =====
    {
        int numTabs = (int)tabButtons.size();
        int addBtnW = 30;
        int tabGap = 4;
        int pad = 16;

        int availableW = getWidth() - pad * 2;
        int addBtnTotal = tabGap + addBtnW;

        int maxTabW = 120;
        int tabW = maxTabW;
        if (numTabs > 0)
        {
            int spaceForTabs = availableW - addBtnTotal - juce::jmax(0, numTabs - 1) * tabGap;
            tabW = juce::jmin(maxTabW, spaceForTabs / numTabs);
            tabW = juce::jmax(50, tabW);
        }

        int totalW = numTabs * tabW + juce::jmax(0, numTabs - 1) * tabGap + addBtnTotal;
        int tabX = (getWidth() - totalW) / 2;
        tabX = juce::jmax(pad, tabX);

        for (int i = 0; i < numTabs; i++)
        {
            tabButtons[(size_t)i]->setBounds(tabX, topBar + 2, tabW, tabBar - 4);
            tabX += tabW + tabGap;
        }
        btnAddEngine.setBounds(tabX, topBar + 2, addBtnW, tabBar - 4);
    }

    // ===== LEFT PANEL =====
    auto leftPanel = bounds.removeFromLeft(panelWidth);
    leftPanel.removeFromTop(topBar + tabBar + 40);
    leftPanel.removeFromBottom(bottomBar);
    leftPanel = leftPanel.reduced(12, 0);

    int btnH = 36, btnG = 4;

    auto layCombo = [](juce::Label& l, juce::ComboBox& c, juce::Rectangle<int>& panel) {
        l.setBounds(panel.removeFromTop(14)); panel.removeFromTop(2);
        c.setBounds(panel.removeFromTop(24)); panel.removeFromTop(3);
    };
    auto laySlider = [](juce::Label& l, GainSlider& s, juce::Rectangle<int>& panel) {
        l.setBounds(panel.removeFromTop(14)); panel.removeFromTop(2);
        s.setBounds(panel.removeFromTop(20)); panel.removeFromTop(3);
    };
    auto layMeter = [](LevelMeter& m, juce::Rectangle<int>& panel) {
        m.setBounds(panel.removeFromTop(6)); panel.removeFromTop(3);
    };
    auto layStatus = [](juce::Label& l, juce::Rectangle<int>& panel) {
        l.setBounds(panel.removeFromTop(13)); panel.removeFromTop(4);
    };

    // Input buttons
    auto& eng = currentEngine();
    struct IBI { juce::TextButton* btn; InputSource src; };
    IBI iBtns[] = { {&btnMtcIn,InputSource::MTC}, {&btnArtnetIn,InputSource::ArtNet}, {&btnSysTime,InputSource::SystemTime}, {&btnLtcIn,InputSource::LTC} };
    for (auto& ib : iBtns) { ib.btn->setBounds(leftPanel.removeFromTop(btnH)); leftPanel.removeFromTop(btnG); }

    if (btnCollapseInput.isVisible())
    { leftPanel.removeFromTop(2); btnCollapseInput.setBounds(leftPanel.removeFromTop(18)); leftPanel.removeFromTop(4); }

    if (cmbMidiInputDevice.isVisible()) layCombo(lblMidiInputDevice, cmbMidiInputDevice, leftPanel);
    if (cmbArtnetInputInterface.isVisible()) layCombo(lblArtnetInputInterface, cmbArtnetInputInterface, leftPanel);
    if (cmbAudioInputDevice.isVisible())
    {
        layCombo(lblAudioInputTypeFilter, cmbAudioInputTypeFilter, leftPanel);
        if (cmbSampleRate.isVisible())
        {
            lblSampleRate.setBounds(leftPanel.removeFromTop(14)); leftPanel.removeFromTop(2);
            auto srRow = leftPanel.removeFromTop(24);
            cmbSampleRate.setBounds(srRow.removeFromLeft(srRow.getWidth() / 2 - 2));
            srRow.removeFromLeft(4);
            cmbBufferSize.setBounds(srRow);
            leftPanel.removeFromTop(3);
            lblBufferSize.setBounds(0, 0, 0, 0);
        }
        layCombo(lblAudioInputDevice, cmbAudioInputDevice, leftPanel);
        layCombo(lblAudioInputChannel, cmbAudioInputChannel, leftPanel);
        laySlider(lblLtcInputGain, sldLtcInputGain, leftPanel);
        if (mtrLtcInput.isVisible()) layMeter(mtrLtcInput, leftPanel);
        if (cmbThruInputChannel.isVisible())
        {
            layCombo(lblThruInputChannel, cmbThruInputChannel, leftPanel);
            laySlider(lblThruInputGain, sldThruInputGain, leftPanel);
            if (mtrThruInput.isVisible()) layMeter(mtrThruInput, leftPanel);
        }
    }

    lblInputStatus.setBounds(leftPanel.removeFromTop(14));

    // ===== RIGHT PANEL =====
    auto rightPanelBounds = bounds.removeFromRight(panelWidth);
    rightPanelBounds.removeFromTop(topBar + tabBar + 40);
    rightPanelBounds.removeFromBottom(bottomBar);
    rightViewport.setBounds(rightPanelBounds);

    int contentW = rightPanelBounds.getWidth();
    int scrollW = rightViewport.getScrollBarThickness();
    int usableW = contentW - scrollW;
    auto rp = juce::Rectangle<int>(12, 0, usableW - 24, 10000);
    int colBtnW = 26;

    // MTC OUT
    {
        auto row = rp.removeFromTop(btnH);
        if (btnCollapseMtcOut.isVisible()) { btnCollapseMtcOut.setBounds(row.removeFromRight(colBtnW)); row.removeFromRight(3); }
        btnMtcOut.setBounds(row); rp.removeFromTop(2);
        if (cmbMidiOutputDevice.isVisible()) layCombo(lblMidiOutputDevice, cmbMidiOutputDevice, rp);
        if (sldMtcOffset.isVisible()) laySlider(lblMtcOffset, sldMtcOffset, rp);
        if (lblOutputMtcStatus.isVisible()) layStatus(lblOutputMtcStatus, rp);
        rp.removeFromTop(2);
    }

    // ART-NET OUT
    {
        auto row = rp.removeFromTop(btnH);
        if (btnCollapseArtnetOut.isVisible()) { btnCollapseArtnetOut.setBounds(row.removeFromRight(colBtnW)); row.removeFromRight(3); }
        btnArtnetOut.setBounds(row); rp.removeFromTop(2);
        if (cmbArtnetOutputInterface.isVisible()) layCombo(lblArtnetOutputInterface, cmbArtnetOutputInterface, rp);
        if (sldArtnetOffset.isVisible()) laySlider(lblArtnetOffset, sldArtnetOffset, rp);
        if (lblOutputArtnetStatus.isVisible()) layStatus(lblOutputArtnetStatus, rp);
        rp.removeFromTop(2);
    }

    // Shared audio driver filter
    if (cmbAudioOutputTypeFilter.isVisible())
        layCombo(lblAudioOutputTypeFilter, cmbAudioOutputTypeFilter, rp);

    // LTC OUT
    {
        auto row = rp.removeFromTop(btnH);
        if (btnCollapseLtcOut.isVisible()) { btnCollapseLtcOut.setBounds(row.removeFromRight(colBtnW)); row.removeFromRight(3); }
        btnLtcOut.setBounds(row); rp.removeFromTop(2);
        if (cmbAudioOutputDevice.isVisible())
        {
            layCombo(lblAudioOutputDevice, cmbAudioOutputDevice, rp);
            layCombo(lblAudioOutputChannel, cmbAudioOutputChannel, rp);
            laySlider(lblLtcOutputGain, sldLtcOutputGain, rp);
            if (mtrLtcOutput.isVisible()) layMeter(mtrLtcOutput, rp);
            if (sldLtcOffset.isVisible()) laySlider(lblLtcOffset, sldLtcOffset, rp);
        }
        if (lblOutputLtcStatus.isVisible()) layStatus(lblOutputLtcStatus, rp);
        rp.removeFromTop(2);
    }

    // AUDIO THRU (primary engine only)
    if (btnThruOut.isVisible())
    {
        auto row = rp.removeFromTop(btnH);
        if (btnCollapseThruOut.isVisible()) { btnCollapseThruOut.setBounds(row.removeFromRight(colBtnW)); row.removeFromRight(3); }
        btnThruOut.setBounds(row); rp.removeFromTop(2);
        if (cmbThruOutputDevice.isVisible())
        {
            layCombo(lblThruOutputDevice, cmbThruOutputDevice, rp);
            layCombo(lblThruOutputChannel, cmbThruOutputChannel, rp);
            laySlider(lblThruOutputGain, sldThruOutputGain, rp);
            if (mtrThruOutput.isVisible()) layMeter(mtrThruOutput, rp);
        }
        if (lblOutputThruStatus.isVisible()) layStatus(lblOutputThruStatus, rp);
        rp.removeFromTop(2);
    }

    if (btnRefreshDevices.isVisible())
    { rp.removeFromTop(4); btnRefreshDevices.setBounds(rp.removeFromTop(26)); }

    int usedHeight = rp.getY() + 8;
    rightContent.setSize(contentW, juce::jmax(rightPanelBounds.getHeight(), usedHeight));

    // ===== CENTER =====
    auto centerArea = bounds;
    centerArea.removeFromTop(topBar + tabBar);
    centerArea.removeFromBottom(bottomBar);

    auto fpsArea = centerArea.removeFromBottom(eng.isFpsConvertEnabled() ? 186 : 100);

    // Mini strip for non-selected engines (between display and FPS)
    int miniH = getMiniStripHeight();
    if (miniH > 0)
    {
        miniStripArea = juce::Rectangle<int>(centerArea.getX(), centerArea.getBottom() - miniH,
                                              centerArea.getWidth(), miniH);
        centerArea.removeFromBottom(miniH);
    }
    else
    {
        miniStripArea = {};
    }
    fpsArea.removeFromTop(20);
    int fpsBW = 58, fpsGap = 6, totalW = fpsBW * 5 + fpsGap * 4;
    int fpsX = fpsArea.getCentreX() - totalW / 2, fpsY = fpsArea.getY() + 10;
    juce::TextButton* fB[] = {&btnFps2398,&btnFps24,&btnFps25,&btnFps2997,&btnFps30};
    FrameRate fV[] = {FrameRate::FPS_2398,FrameRate::FPS_24,FrameRate::FPS_25,FrameRate::FPS_2997,FrameRate::FPS_30};
    for (int i = 0; i < 5; i++) { fB[i]->setBounds(fpsX + i * (fpsBW + fpsGap), fpsY, fpsBW, 32); styleFpsButton(*fB[i], eng.getCurrentFps() == fV[i]); }

    int toggleW = 140, toggleH = 24;
    int toggleY = fpsY + 32 + 6;
    btnFpsConvert.setBounds(fpsArea.getCentreX() - toggleW / 2, toggleY, toggleW, toggleH);

    juce::TextButton* oB[] = {&btnOutFps2398,&btnOutFps24,&btnOutFps25,&btnOutFps2997,&btnOutFps30};
    FrameRate effectiveOut = eng.getEffectiveOutputFps();
    if (eng.isFpsConvertEnabled())
    {
        int outLabelY = toggleY + toggleH + 6;
        int outFpsY   = outLabelY + 14 + 6;
        for (int i = 0; i < 5; i++) { oB[i]->setBounds(fpsX + i * (fpsBW + fpsGap), outFpsY, fpsBW, 32); styleFpsButton(*oB[i], effectiveOut == fV[i]); oB[i]->setVisible(true); }
    }
    else
    {
        for (int i = 0; i < 5; i++) oB[i]->setVisible(false);
    }

    timecodeDisplay.setBounds(centerArea);
    btnGitHub.setBounds(getWidth() / 2 - 160, getHeight() - bottomBar, 320, bottomBar);
    btnCheckUpdates.setBounds(getWidth() - 170, 2, 150, 28);

    // Update notification — in top bar, replacing check button
    if (btnUpdateAvailable.isVisible())
        btnUpdateAvailable.setBounds(getWidth() - 220, 2, 200, 28);
}

//==============================================================================
// TIMER
//==============================================================================
void MainComponent::timerCallback()
{
    if (engines.empty()) return;  // guard

    // Tick ALL engines (not just selected).
    // NOTE: tick() feeds timecode values to the output protocol handlers.
    // MTC and ArtNet outputs use their own HighResolutionTimers (1ms) for
    // actual transmission timing, so the 60Hz UI timer only updates the
    // target timecode — it does NOT limit output precision.  LTC output
    // uses its own audio-callback-driven auto-increment, so it's similarly
    // decoupled.  If UI stalls (resize, modal dialog), outputs continue
    // transmitting the last-known timecode until the next tick() updates it.
    for (auto& eng : engines)
        eng->tick();

    // Update UI for selected engine
    auto& eng = currentEngine();

    updateStatusLabels();

    timecodeDisplay.setTimecode(eng.getCurrentTimecode());
    timecodeDisplay.setFrameRate(eng.getCurrentFps());
    timecodeDisplay.setRunning(eng.isSourceActive());
    timecodeDisplay.setSourceName(TimecodeEngine::getInputName(eng.getActiveInput()));
    timecodeDisplay.setFpsConvertEnabled(eng.isFpsConvertEnabled());
    timecodeDisplay.setOutputTimecode(eng.getOutputTimecode());
    timecodeDisplay.setOutputFrameRate(eng.getEffectiveOutputFps());

    // Update VU meters for selected engine
    mtrLtcInput.setLevel(eng.getSmoothedLtcInLevel());
    mtrThruInput.setLevel(eng.getSmoothedThruInLevel());
    mtrLtcOutput.setLevel(eng.getSmoothedLtcOutLevel());
    mtrThruOutput.setLevel(eng.getSmoothedThruOutLevel());

    // Auto-update FPS button states when the frame rate changes
    // (e.g. from protocol auto-detection in MTC/ArtNet/LTC inputs)
    {
        FrameRate curFps = eng.getCurrentFps();
        FrameRate curOutFps = eng.getEffectiveOutputFps();
        if (curFps != lastDisplayedFps)
        {
            lastDisplayedFps = curFps;
            updateFpsButtonStates();
        }
        if (curOutFps != lastDisplayedOutFps)
        {
            lastDisplayedOutFps = curOutFps;
            updateOutputFpsButtonStates();
        }
    }

    // Repaint mini strip so non-selected engine timecodes update live
    if (engines.size() > 1 && !miniStripArea.isEmpty())
        repaint(miniStripArea);

    // Debounced settings save
    if (settingsDirty)
    {
        if (--settingsSaveCountdown <= 0)
            flushSettings();
    }

    // --- Update checker ---
    if (updateCheckDelay > 0)
    {
        if (--updateCheckDelay == 0)
        {
            auto appVer = juce::JUCEApplication::getInstance()->getApplicationVersion();
            updateChecker.checkAsync(appVer);
        }
    }
    else if (!updateNotificationShown && updateChecker.hasResult())
    {
        updateNotificationShown = true;
        if (updateChecker.isUpdateAvailable())
        {
            juce::String label = "Update available: v" + updateChecker.getLatestVersion();
            btnUpdateAvailable.setButtonText(label);
            btnUpdateAvailable.setURL(juce::URL(updateChecker.getReleaseUrl()));
            btnUpdateAvailable.setVisible(true);
            btnCheckUpdates.setVisible(false);
            // Position directly in top bar
            btnUpdateAvailable.setBounds(getWidth() - 220, 2, 200, 28);
        }
        else if (updateChecker.didCheckFail())
        {
            btnCheckUpdates.setButtonText("Check failed - retry?");
            btnCheckUpdates.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFFF8A65));  // orange
            btnCheckUpdates.setVisible(true);
            // Reset text after ~4 seconds (240 ticks at 60Hz)
            updateResetCountdown = 240;
        }
        else
        {
            btnCheckUpdates.setButtonText("Up to date " + juce::String::charToString(0x2713));
            btnCheckUpdates.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFF66BB6A));  // green
            btnCheckUpdates.setVisible(true);
            // Reset text after ~4 seconds
            updateResetCountdown = 240;
        }
    }

    // Reset "check for updates" button text after countdown
    if (updateResetCountdown > 0)
    {
        if (--updateResetCountdown == 0)
        {
            btnCheckUpdates.setButtonText("Check for updates");
            btnCheckUpdates.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFF546E7A));
        }
    }

    // Bottom bar repaint when state changes
    juce::String currentBottomStatus = TimecodeEngine::getInputName(eng.getActiveInput());
    if (currentBottomStatus != lastBottomBarStatus || eng.isSourceActive() != lastBottomBarActive)
    {
        lastBottomBarStatus = currentBottomStatus;
        lastBottomBarActive = eng.isSourceActive();
        repaint(0, getHeight() - 24, getWidth(), 24);
    }
}

//==============================================================================
// BUTTON STATE UPDATES
//==============================================================================
void MainComponent::updateInputButtonStates()
{
    auto& eng = currentEngine();
    auto active = eng.getActiveInput();
    struct I { juce::TextButton* b; InputSource s; };
    I bs[] = { {&btnMtcIn,InputSource::MTC}, {&btnArtnetIn,InputSource::ArtNet}, {&btnSysTime,InputSource::SystemTime}, {&btnLtcIn,InputSource::LTC} };
    for (auto& i : bs) styleInputButton(*i.b, active == i.s, getInputColour(i.s));
}

void MainComponent::updateFpsButtonStates()
{
    auto& eng = currentEngine();
    FrameRate v[] = {FrameRate::FPS_2398,FrameRate::FPS_24,FrameRate::FPS_25,FrameRate::FPS_2997,FrameRate::FPS_30};
    juce::TextButton* b[] = {&btnFps2398,&btnFps24,&btnFps25,&btnFps2997,&btnFps30};
    for (int i = 0; i < 5; i++) styleFpsButton(*b[i], eng.getCurrentFps() == v[i]);
}

void MainComponent::updateOutputFpsButtonStates()
{
    auto& eng = currentEngine();
    FrameRate v[] = {FrameRate::FPS_2398, FrameRate::FPS_24, FrameRate::FPS_25, FrameRate::FPS_2997, FrameRate::FPS_30};
    juce::TextButton* b[] = {&btnOutFps2398, &btnOutFps24, &btnOutFps25, &btnOutFps2997, &btnOutFps30};
    FrameRate effective = eng.getEffectiveOutputFps();
    for (int i = 0; i < 5; i++)
    {
        styleFpsButton(*b[i], effective == v[i]);
        b[i]->setEnabled(eng.isFpsConvertEnabled());
        b[i]->setAlpha(eng.isFpsConvertEnabled() ? 1.0f : 0.4f);
    }
}

juce::Colour MainComponent::getInputColour(InputSource s) const
{
    switch (s)
    {
        case InputSource::MTC:        return accentRed;
        case InputSource::ArtNet:     return accentOrange;
        case InputSource::SystemTime: return accentGreen;
        case InputSource::LTC:        return accentPurple;
        default:                      return textMid;
    }
}

//==============================================================================
// STYLING (same as v1.3)
//==============================================================================
void MainComponent::styleInputButton(juce::TextButton& btn, bool active, juce::Colour colour)
{
    btn.setColour(juce::TextButton::buttonColourId, active ? colour.withAlpha(0.15f) : juce::Colour(0xFF1A1D23));
    btn.setColour(juce::TextButton::buttonOnColourId, colour.withAlpha(0.2f));
    btn.setColour(juce::TextButton::textColourOffId, active ? textBright : textMid);
    btn.setColour(juce::TextButton::textColourOnId, textBright);
}

void MainComponent::styleFpsButton(juce::TextButton& btn, bool active)
{
    auto c = getInputColour(currentEngine().getActiveInput());
    btn.setColour(juce::TextButton::buttonColourId, active ? c.withAlpha(0.15f) : juce::Colour(0xFF1A1D23));
    btn.setColour(juce::TextButton::textColourOffId, active ? juce::Colour(0xFFE0F7FA) : textMid);
}

void MainComponent::styleOutputToggle(juce::ToggleButton& btn, juce::Colour colour)
{
    btn.setColour(juce::ToggleButton::textColourId, textBright);
    btn.setColour(juce::ToggleButton::tickColourId, colour);
    btn.setColour(juce::ToggleButton::tickDisabledColourId, textDim);
}

void MainComponent::styleComboBox(juce::ComboBox& cmb)
{
    cmb.setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xFF1A1D23));
    cmb.setColour(juce::ComboBox::textColourId, textBright);
    cmb.setColour(juce::ComboBox::outlineColourId, borderCol);
    cmb.setColour(juce::ComboBox::arrowColourId, textMid);
}

void MainComponent::styleLabel(juce::Label& lbl, float fs)
{
    lbl.setFont(juce::Font(juce::FontOptions(getMonoFontName(), fs, juce::Font::plain)));
    lbl.setColour(juce::Label::textColourId, textDim);
    lbl.setJustificationType(juce::Justification::centredLeft);
}

void MainComponent::styleGainSlider(GainSlider& sld)
{
    sld.setSliderStyle(juce::Slider::LinearHorizontal);
    sld.setTextBoxStyle(juce::Slider::TextBoxRight, false, 44, 20);
    sld.setRange(0.0, 200.0, 1.0); sld.setValue(100.0, juce::dontSendNotification);
    sld.setTextValueSuffix("%"); sld.setDoubleClickReturnValue(true, 100.0);
    sld.setColour(juce::Slider::backgroundColourId, juce::Colour(0xFF1A1D23));
    sld.setColour(juce::Slider::trackColourId, juce::Colour(0xFF37474F));
    sld.setColour(juce::Slider::thumbColourId, juce::Colour(0xFF78909C));
    sld.setColour(juce::Slider::textBoxTextColourId, textBright);
    sld.setColour(juce::Slider::textBoxBackgroundColourId, juce::Colour(0xFF1A1D23));
    sld.setColour(juce::Slider::textBoxOutlineColourId, borderCol);
}

void MainComponent::styleOffsetSlider(GainSlider& sld)
{
    sld.setSliderStyle(juce::Slider::LinearHorizontal);
    sld.setTextBoxStyle(juce::Slider::TextBoxRight, false, 44, 20);
    sld.setRange(-30.0, 30.0, 1.0); sld.setValue(0.0, juce::dontSendNotification);
    sld.setTextValueSuffix(" f"); sld.setDoubleClickReturnValue(true, 0.0);
    sld.setColour(juce::Slider::backgroundColourId, juce::Colour(0xFF1A1D23));
    sld.setColour(juce::Slider::trackColourId, juce::Colour(0xFF37474F));
    sld.setColour(juce::Slider::thumbColourId, juce::Colour(0xFF78909C));
    sld.setColour(juce::Slider::textBoxTextColourId, textBright);
    sld.setColour(juce::Slider::textBoxBackgroundColourId, juce::Colour(0xFF1A1D23));
    sld.setColour(juce::Slider::textBoxOutlineColourId, borderCol);
}

void MainComponent::styleCollapseButton(juce::TextButton& btn)
{
    btn.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    btn.setColour(juce::TextButton::buttonOnColourId, juce::Colours::transparentBlack);
    btn.setColour(juce::TextButton::textColourOffId, textMid);
    btn.setColour(juce::TextButton::textColourOnId, textMid);
}

void MainComponent::updateCollapseButtonText(juce::TextButton& btn, bool expanded)
{
    btn.setButtonText(expanded ? juce::String::charToString(0x25BE)
                               : juce::String::charToString(0x25B8));
}
