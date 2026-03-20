// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

#include "MainComponent.h"

using InputSource = TimecodeEngine::InputSource;

//==============================================================================
// Remove " *" or " [Engine N]" markers from combo item text to get the
// underlying device name.  Used when saving device names to settings.
//==============================================================================
static juce::String stripComboMarker(const juce::String& text)
{
    static const juce::String dotSuffix = juce::String(" ") + juce::String::charToString(0x25CF);
    auto s = text;
    if (s.endsWith(dotSuffix))
        s = s.dropLastCharacters(dotSuffix.length());
    int bracket = s.lastIndexOf(" [");
    if (bracket >= 0 && s.endsWith("]"))
        s = s.substring(0, bracket);
    return s.trimEnd();
}

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
    engines[0]->setSharedProDJLinkInput(&sharedProDJLinkInput);
    engines[0]->setDbServerClient(&sharedDbClient);
    engines[0]->setTrackMap(&settings.trackMap);
    engines[0]->setMixerMap(&sharedMixerMap);

    setSize(900, 700);
    setWantsKeyboardFocus(true);  // enable Ctrl+D diagnostic shortcut

    // --- Tab bar ---
    addAndMakeVisible(btnAddEngine);
    btnAddEngine.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF1A1D23));
    btnAddEngine.setColour(juce::TextButton::textColourOffId, accentBlue);
    btnAddEngine.onClick = [this] { addEngine(); };
    rebuildTabButtons();

    // --- Left panel scrollable viewport ---
    addAndMakeVisible(leftViewport);
    leftViewport.setViewedComponent(&leftContent, false);
    leftViewport.setScrollBarsShown(true, false);

    // --- Right panel scrollable viewport ---
    addAndMakeVisible(rightViewport);
    rightViewport.setViewedComponent(&rightContent, false);
    rightViewport.setScrollBarsShown(true, false);

    // --- Input buttons ---
    for (auto* btn : { &btnMtcIn, &btnArtnetIn, &btnSysTime, &btnLtcIn, &btnProDJLinkIn })
    { leftContent.addAndMakeVisible(btn); btn->setClickingTogglesState(false); }

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

    btnProDJLinkIn.onClick = [this] {
        if (syncing) return;
        auto& eng = currentEngine();
        if (eng.getActiveInput() == InputSource::ProDJLink) { inputConfigExpanded = !inputConfigExpanded; updateDeviceSelectorVisibility(); }
        else { inputConfigExpanded = true; eng.setInputSource(InputSource::ProDJLink); startCurrentProDJLinkInput(); updateInputButtonStates(); updateDeviceSelectorVisibility(); saveSettings(); }
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
    leftContent.addAndMakeVisible(btnCollapseInput);
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
        leftContent.addAndMakeVisible(lbl); leftContent.addAndMakeVisible(cmb);
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
            populateMidiAndNetworkCombos();  // refresh markers (auto-restores selections)
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
            // If bind fell back, update combo to actual interface before repopulate
            int actualId = currentEngine().getArtnetInput().getSelectedInterface() + 1;
            cmbArtnetInputInterface.setSelectedId(actualId, juce::dontSendNotification);
            populateMidiAndNetworkCombos();  // refresh markers (auto-restores all selections)
            saveSettings();
        }
    };

    // --- Pro DJ Link controls ---
    addLabelAndCombo(lblProDJLinkInterface, cmbProDJLinkInterface, "PRO DJ LINK INTERFACE:");
    cmbProDJLinkInterface.onChange = [this]
    {
        if (syncing) return;
        if (currentEngine().getActiveInput() == InputSource::ProDJLink)
        {
            startCurrentProDJLinkInput();
            saveSettings();
        }
    };

    addLabelAndCombo(lblProDJLinkPlayer, cmbProDJLinkPlayer, "PLAYER:");
    for (int i = 1; i <= ProDJLink::kMaxPlayers; ++i)
        cmbProDJLinkPlayer.addItem("PLAYER " + juce::String(i), i);
    cmbProDJLinkPlayer.addItem("XF-A", 7);
    cmbProDJLinkPlayer.addItem("XF-B", 8);
    cmbProDJLinkPlayer.setSelectedId(1, juce::dontSendNotification);
    cmbProDJLinkPlayer.onChange = [this]
    {
        if (syncing) return;
        if (currentEngine().getActiveInput() == InputSource::ProDJLink)
        {
            int player = cmbProDJLinkPlayer.getSelectedId();
            if (player >= 1)
            {
                currentEngine().setProDJLinkPlayer(player);
                // Reset UI display so stale data from previous player doesn't linger
                displayedArtworkId = 0;
                artworkDisplay.clearImage();
                displayedWaveformTrackId = 0;
                waveformDisplay.clearWaveform();
                lblProDJLinkTrackInfo.setText("", juce::dontSendNotification);
                lblProDJLinkMetadata.setText("", juce::dontSendNotification);
            }
            saveSettings();
        }
    };

    leftContent.addAndMakeVisible(lblProDJLinkMetadata);
    styleLabel(lblProDJLinkMetadata, 9.0f);
    lblProDJLinkMetadata.setVisible(false);

    leftContent.addAndMakeVisible(lblMixerStatus);
    lblMixerStatus.setFont(juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 9.0f, juce::Font::plain)));
    lblMixerStatus.setColour(juce::Label::textColourId, juce::Colour(0xFF888888));
    lblMixerStatus.setVisible(false);

    leftContent.addAndMakeVisible(artworkDisplay);
    artworkDisplay.setVisible(false);
    leftContent.addAndMakeVisible(waveformDisplay);
    waveformDisplay.setVisible(false);

    leftContent.addAndMakeVisible(lblProDJLinkTrackInfo);
    styleLabel(lblProDJLinkTrackInfo, 8.0f);
    lblProDJLinkTrackInfo.setVisible(false);

    // --- ProDJLink features: TrackMap, MIDI Clock, OSC BPM, Ableton Link ---
    auto pdlAccent = juce::Colour(0xFF00AAFF);

    leftContent.addAndMakeVisible(btnTrackMap);
    btnTrackMap.setVisible(false);
    btnTrackMap.setColour(juce::ToggleButton::textColourId, textMid);
    btnTrackMap.setColour(juce::ToggleButton::tickColourId, pdlAccent);
    btnTrackMap.onClick = [this]
    {
        if (syncing) return;
        currentEngine().setTrackMapEnabled(btnTrackMap.getToggleState());
        updateDeviceSelectorVisibility();
        saveSettings();
    };

    leftContent.addAndMakeVisible(btnTrackMapEdit);
    btnTrackMapEdit.setVisible(false);
    btnTrackMapEdit.setColour(juce::TextButton::buttonColourId, pdlAccent.withAlpha(0.15f));
    btnTrackMapEdit.setColour(juce::TextButton::textColourOffId, pdlAccent.brighter(0.3f));
    btnTrackMapEdit.onClick = [this] { openTrackMapEditor(); };

    leftContent.addAndMakeVisible(btnProDJLinkView);
    btnProDJLinkView.setVisible(false);
    btnProDJLinkView.setColour(juce::TextButton::buttonColourId, pdlAccent.withAlpha(0.15f));
    btnProDJLinkView.setColour(juce::TextButton::textColourOffId, pdlAccent.brighter(0.3f));
    btnProDJLinkView.onClick = [this] { openProDJLinkView(); };

    leftContent.addAndMakeVisible(btnMixerMapEdit);
    btnMixerMapEdit.setVisible(false);
    btnMixerMapEdit.setColour(juce::TextButton::buttonColourId, pdlAccent.withAlpha(0.15f));
    btnMixerMapEdit.setColour(juce::TextButton::textColourOffId, pdlAccent.brighter(0.3f));
    btnMixerMapEdit.onClick = [this] { openMixerMapEditor(); };

    addAndMakeVisible(btnBackup);
    btnBackup.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF1A1D23));
    btnBackup.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFF66CC66));
    btnBackup.onClick = [this] { exportConfig(); };

    addAndMakeVisible(btnRestore);
    btnRestore.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF1A1D23));
    btnRestore.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFFF9966));
    btnRestore.onClick = [this] { importConfig(); };

    leftContent.addAndMakeVisible(btnMidiClock);
    btnMidiClock.setVisible(false);
    btnMidiClock.setColour(juce::ToggleButton::textColourId, textMid);
    btnMidiClock.setColour(juce::ToggleButton::tickColourId, pdlAccent);
    btnMidiClock.onClick = [this]
    {
        if (syncing) return;
        bool wantClock = btnMidiClock.getToggleState();
        currentEngine().setMidiClockEnabled(wantClock);  // set flag (clock may not start yet if device not open)
        applyTriggerSettings();  // ensure MIDI device is opened/closed based on needs
        if (wantClock)
            currentEngine().setMidiClockEnabled(true);  // re-try now that device is open
        propagateGlobalSettings();
        updateDeviceSelectorVisibility();
        saveSettings();
    };

    leftContent.addAndMakeVisible(btnOscFwdBpm);
    btnOscFwdBpm.setVisible(false);
    btnOscFwdBpm.setColour(juce::ToggleButton::textColourId, textMid);
    btnOscFwdBpm.setColour(juce::ToggleButton::tickColourId, pdlAccent);
    btnOscFwdBpm.onClick = [this]
    {
        if (syncing) return;
        currentEngine().setOscForward(btnOscFwdBpm.getToggleState(), edOscFwdBpmAddr.getText());
        applyTriggerSettings();  // ensure OSC connection is opened/closed
        propagateGlobalSettings();
        updateDeviceSelectorVisibility();
        saveSettings();
    };

    leftContent.addAndMakeVisible(lblOscFwdBpmAddr);
    lblOscFwdBpmAddr.setText("Addr:", juce::dontSendNotification);
    lblOscFwdBpmAddr.setFont(juce::Font(juce::FontOptions(10.0f)));
    lblOscFwdBpmAddr.setColour(juce::Label::textColourId, textDim);
    lblOscFwdBpmAddr.setVisible(false);

    leftContent.addAndMakeVisible(edOscFwdBpmAddr);
    edOscFwdBpmAddr.setVisible(false);
    edOscFwdBpmAddr.setText("/composition/tempocontroller/tempo");
    edOscFwdBpmAddr.setFont(juce::Font(juce::FontOptions(10.0f)));
    edOscFwdBpmAddr.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xFF2A2A2A));
    edOscFwdBpmAddr.setColour(juce::TextEditor::textColourId, textLight);
    edOscFwdBpmAddr.setColour(juce::TextEditor::outlineColourId, juce::Colour(0xFF444444));
    edOscFwdBpmAddr.onReturnKey = [this]
    {
        if (!syncing) { currentEngine().setOscForward(btnOscFwdBpm.getToggleState(), edOscFwdBpmAddr.getText()); propagateGlobalSettings(); saveSettings(); }
    };
    edOscFwdBpmAddr.onFocusLost = [this]
    {
        if (!syncing) { currentEngine().setOscForward(btnOscFwdBpm.getToggleState(), edOscFwdBpmAddr.getText()); propagateGlobalSettings(); saveSettings(); }
    };

    leftContent.addAndMakeVisible(btnOscMixerFwd);
    btnOscMixerFwd.setVisible(false);
    btnOscMixerFwd.setColour(juce::ToggleButton::textColourId, textMid);
    btnOscMixerFwd.setColour(juce::ToggleButton::tickColourId, pdlAccent);
    btnOscMixerFwd.onClick = [this]
    {
        currentEngine().setOscMixerForward(btnOscMixerFwd.getToggleState());
        applyTriggerSettings();
        propagateGlobalSettings();
        saveSettings();
    };

    leftContent.addAndMakeVisible(btnMidiMixerFwd);
    btnMidiMixerFwd.setVisible(false);
    btnMidiMixerFwd.setColour(juce::ToggleButton::textColourId, textMid);
    btnMidiMixerFwd.setColour(juce::ToggleButton::tickColourId, pdlAccent);
    btnMidiMixerFwd.onClick = [this]
    {
        currentEngine().setMidiMixerForward(btnMidiMixerFwd.getToggleState(),
                                             cmbMidiMixCCCh.getSelectedId(),
                                             cmbMidiMixNoteCh.getSelectedId());
        applyTriggerSettings();
        propagateGlobalSettings();
        updateDeviceSelectorVisibility();
        saveSettings();
    };

    auto setupMidiChCombo = [this](juce::ComboBox& cmb, juce::Label& lbl, const juce::String& labelText)
    {
        leftContent.addAndMakeVisible(lbl);
        lbl.setVisible(false);
        lbl.setText(labelText, juce::dontSendNotification);
        lbl.setFont(juce::Font(juce::FontOptions(10.0f)));
        lbl.setColour(juce::Label::textColourId, textDim);

        leftContent.addAndMakeVisible(cmb);
        cmb.setVisible(false);
        for (int ch = 1; ch <= 16; ++ch)
            cmb.addItem("Ch " + juce::String(ch), ch);
        cmb.setSelectedId(1, juce::dontSendNotification);
        styleComboBox(cmb);
        cmb.onChange = [this]
        {
            currentEngine().setMidiMixerForward(btnMidiMixerFwd.getToggleState(),
                                                 cmbMidiMixCCCh.getSelectedId(),
                                                 cmbMidiMixNoteCh.getSelectedId());
            propagateGlobalSettings();
            saveSettings();
        };
    };
    setupMidiChCombo(cmbMidiMixCCCh, lblMidiMixCCCh, "CC CH:");
    setupMidiChCombo(cmbMidiMixNoteCh, lblMidiMixNoteCh, "NOTE CH:");

    leftContent.addAndMakeVisible(btnArtnetMixerFwd);
    btnArtnetMixerFwd.setVisible(false);
    btnArtnetMixerFwd.setColour(juce::ToggleButton::textColourId, textMid);
    btnArtnetMixerFwd.setColour(juce::ToggleButton::tickColourId, pdlAccent);
    btnArtnetMixerFwd.onClick = [this]
    {
        currentEngine().setArtnetMixerForward(btnArtnetMixerFwd.getToggleState(),
                                               getArtNetAddressFromCombos(cmbArtMixNet, cmbArtMixSub, cmbArtMixUni));
        // Ensure ArtNet output is running -- DMX uses the same socket.
        // If ArtNet timecode out is already running, this is a no-op.
        if (btnArtnetMixerFwd.getToggleState() && !currentEngine().getArtnetOutput().getIsRunning())
        {
            int iface = cmbArtnetDmxInterface.getSelectedId() - 2;  // -1=All, 0+=NIC
            currentEngine().startArtnetOutput(iface);
        }
        propagateGlobalSettings();
        updateDeviceSelectorVisibility();
        saveSettings();
    };

    setupArtNetAddressCombos(cmbArtMixNet, cmbArtMixSub, cmbArtMixUni, lblArtMixAddr,
                             "MIXER:", [this]
    {
        currentEngine().setArtnetMixerForward(btnArtnetMixerFwd.getToggleState(),
                                               getArtNetAddressFromCombos(cmbArtMixNet, cmbArtMixSub, cmbArtMixUni));
        propagateGlobalSettings();
        saveSettings();
    });

    leftContent.addAndMakeVisible(btnLink);
    btnLink.setVisible(false);
    btnLink.setColour(juce::ToggleButton::textColourId, textMid);
    btnLink.setColour(juce::ToggleButton::tickColourId, pdlAccent);
    btnLink.onClick = [this]
    {
        if (syncing) return;
        currentEngine().getLinkBridge().setEnabled(btnLink.getToggleState());
        propagateGlobalSettings();
        updateDeviceSelectorVisibility();
        saveSettings();
    };

    leftContent.addAndMakeVisible(lblLinkStatus);
    lblLinkStatus.setVisible(false);
    lblLinkStatus.setFont(juce::Font(juce::FontOptions(10.0f)));
    lblLinkStatus.setColour(juce::Label::textColourId, textMid);
    lblLinkStatus.setJustificationType(juce::Justification::centredLeft);

    // --- BPM Multiplier buttons (per-player, ProDJLink only) ---
    // Single click: session override (temporary). Double click: save to TrackMap (persistent).
    {
        auto setupBpmBtn = [this, pdlAccent](juce::TextButton& btn, int mult)
        {
            leftContent.addAndMakeVisible(btn);
            btn.setVisible(false);
            btn.setColour(juce::TextButton::buttonColourId,   bgPanel);
            btn.setColour(juce::TextButton::textColourOffId,  textMid);
            btn.setColour(juce::TextButton::buttonOnColourId, pdlAccent.withAlpha(0.30f));
            btn.setColour(juce::TextButton::textColourOnId,   pdlAccent.brighter(0.3f));
            btn.setClickingTogglesState(false);
            btn.onClick = [this, mult]
            {
                if (syncing) return;
                auto now = (juce::int64)juce::Time::getMillisecondCounter();
                bool isDouble = (mult == lastBpmClickMult
                                 && (now - lastBpmClickMs) < 400);
                lastBpmClickMs   = now;
                lastBpmClickMult = mult;

                if (isDouble)
                {
                    // Double click: persist to TrackMap (toggle)
                    saveBpmMultToTrackMap(mult);
                }
                else
                {
                    // Single click: set session override (skip if already effective)
                    auto& eng = currentEngine();
                    if (eng.getEffectiveBpmMultiplier() != mult)
                        eng.setBpmPlayerOverride(mult);
                }
                updateBpmMultButtonStates();
            };
        };
        setupBpmBtn(btnBpmOff,  0);
        setupBpmBtn(btnBpmX2,  1);
        setupBpmBtn(btnBpmX4,  2);
        setupBpmBtn(btnBpmD2, -1);
        setupBpmBtn(btnBpmD4, -2);
    }

    // --- Track change trigger controls ---
    auto accentAmber = juce::Colour(0xFFFFAB00);

    leftContent.addAndMakeVisible(btnTriggerMidi);
    btnTriggerMidi.setVisible(false);
    btnTriggerMidi.setColour(juce::ToggleButton::textColourId, textMid);
    btnTriggerMidi.setColour(juce::ToggleButton::tickColourId, accentAmber);
    btnTriggerMidi.onClick = [this]
    {
        if (syncing) return;
        applyTriggerSettings();
        saveSettings();
    };

    leftContent.addAndMakeVisible(cmbTriggerMidiDevice);
    cmbTriggerMidiDevice.setVisible(false);
    styleComboBox(cmbTriggerMidiDevice);
    cmbTriggerMidiDevice.onChange = [this]
    {
        if (syncing) return;
        applyTriggerSettings();
        saveSettings();
    };

    leftContent.addAndMakeVisible(btnTriggerOsc);
    btnTriggerOsc.setVisible(false);
    btnTriggerOsc.setColour(juce::ToggleButton::textColourId, textMid);
    btnTriggerOsc.setColour(juce::ToggleButton::tickColourId, accentAmber);
    btnTriggerOsc.onClick = [this]
    {
        if (syncing) return;
        applyTriggerSettings();
        saveSettings();
    };

    leftContent.addAndMakeVisible(edOscIp);
    edOscIp.setVisible(false);
    edOscIp.setFont(juce::Font(juce::FontOptions(10.0f)));
    edOscIp.setColour(juce::TextEditor::backgroundColourId, bgDarker);
    edOscIp.setColour(juce::TextEditor::textColourId, textBright);
    edOscIp.setColour(juce::TextEditor::outlineColourId, borderCol);
    edOscIp.setTextToShowWhenEmpty("127.0.0.1", textDim);
    edOscIp.onFocusLost = [this] { applyTriggerSettings(); saveSettings(); };
    edOscIp.onReturnKey  = [this] { applyTriggerSettings(); saveSettings(); };

    leftContent.addAndMakeVisible(edOscPort);
    edOscPort.setVisible(false);
    edOscPort.setFont(juce::Font(juce::FontOptions(10.0f)));
    edOscPort.setColour(juce::TextEditor::backgroundColourId, bgDarker);
    edOscPort.setColour(juce::TextEditor::textColourId, textBright);
    edOscPort.setColour(juce::TextEditor::outlineColourId, borderCol);
    edOscPort.setTextToShowWhenEmpty("53000", textDim);
    edOscPort.setInputRestrictions(5, "0123456789");
    edOscPort.onFocusLost = [this] { applyTriggerSettings(); saveSettings(); };
    edOscPort.onReturnKey  = [this] { applyTriggerSettings(); saveSettings(); };

    leftContent.addAndMakeVisible(btnArtnetTrigger);
    btnArtnetTrigger.setVisible(false);
    btnArtnetTrigger.setColour(juce::ToggleButton::textColourId, textMid);
    btnArtnetTrigger.setColour(juce::ToggleButton::tickColourId, accentAmber);
    btnArtnetTrigger.onClick = [this]
    {
        if (syncing) return;
        currentEngine().setArtnetTriggerEnabled(btnArtnetTrigger.getToggleState());
        // Ensure ArtNet output is running if enabled
        if (btnArtnetTrigger.getToggleState() && !currentEngine().getArtnetOutput().getIsRunning())
        {
            int iface = cmbArtnetDmxInterface.getSelectedId() - 2;  // -1=All, 0+=NIC
            currentEngine().startArtnetOutput(iface);
        }
        updateDeviceSelectorVisibility();
        saveSettings();
    };

    setupArtNetAddressCombos(cmbArtTrigNet, cmbArtTrigSub, cmbArtTrigUni, lblArtTrigAddr,
                             "TRIGGER:", [this]
    {
        currentEngine().setArtnetTriggerUniverse(getArtNetAddressFromCombos(cmbArtTrigNet, cmbArtTrigSub, cmbArtTrigUni));
        saveSettings();
    });
    // Default universe 1 (Net=0, Sub=0, Uni=1)
    cmbArtTrigUni.setSelectedId(2, juce::dontSendNotification);

    // Art-Net DMX interface selector (for triggers and mixer forward)
    addLabelAndCombo(lblArtnetDmxInterface, cmbArtnetDmxInterface, "ART-NET DMX INTERFACE:");
    cmbArtnetDmxInterface.onChange = [this]
    {
        if (syncing) return;
        auto& eng = currentEngine();
        bool needsArtnet = eng.isArtnetMixerForwardEnabled() || eng.isArtnetTriggerEnabled();
        if (needsArtnet)
        {
            int sel = cmbArtnetDmxInterface.getSelectedId() - 2;  // -1=All, 0+=NIC
            // Restart ArtnetOutput on the new interface (only if timecode output isn't controlling it)
            if (!eng.isOutputArtnetEnabled() || !eng.getArtnetOutput().getIsRunning())
                eng.startArtnetOutput(sel);
        }
        saveSettings();
    };

    addLabelAndCombo(lblAudioInputDevice, cmbAudioInputDevice, "AUDIO INPUT DEVICE:");
    cmbAudioInputDevice.onChange = [this]
    {
        if (syncing) return;
        if (currentEngine().getActiveInput() == InputSource::LTC
            && cmbAudioInputDevice.getSelectedId() > 0
            && cmbAudioInputDevice.getSelectedId() != kPlaceholderItemId)
        {
            startCurrentLtcInput();
            populateFilteredInputDeviceCombo();  // refresh markers (auto-restores selection)
            populateAudioInputChannels();
            saveSettings();
        }
    };

    addLabelAndCombo(lblAudioInputChannel, cmbAudioInputChannel, "LTC CHANNEL:");
    cmbAudioInputChannel.onChange = [this] { if (!syncing && currentEngine().getActiveInput() == InputSource::LTC) { startCurrentLtcInput(); saveSettings(); } };

    leftContent.addAndMakeVisible(sldLtcInputGain); styleGainSlider(sldLtcInputGain);
    leftContent.addAndMakeVisible(lblLtcInputGain); lblLtcInputGain.setText("LTC INPUT GAIN:", juce::dontSendNotification); styleLabel(lblLtcInputGain);
    leftContent.addAndMakeVisible(mtrLtcInput); mtrLtcInput.setMeterColour(accentPurple);
    sldLtcInputGain.onValueChange = [this] { if (!syncing) { currentEngine().getLtcInput().setInputGain((float)sldLtcInputGain.getValue() / 100.0f); saveSettings(); } };

    addLabelAndCombo(lblThruInputChannel, cmbThruInputChannel, "AUDIO THRU CHANNEL:");
    cmbThruInputChannel.onChange = [this] { if (!syncing && currentEngine().getActiveInput() == InputSource::LTC) { startCurrentLtcInput(); saveSettings(); } };

    leftContent.addAndMakeVisible(sldThruInputGain); styleGainSlider(sldThruInputGain);
    leftContent.addAndMakeVisible(lblThruInputGain); lblThruInputGain.setText("AUDIO THRU INPUT GAIN:", juce::dontSendNotification); styleLabel(lblThruInputGain);
    leftContent.addAndMakeVisible(mtrThruInput); mtrThruInput.setMeterColour(accentCyan);
    sldThruInputGain.onValueChange = [this] { if (!syncing) { currentEngine().getLtcInput().setPassthruGain((float)sldThruInputGain.getValue() / 100.0f); saveSettings(); } };

    leftContent.addAndMakeVisible(lblInputStatus); styleLabel(lblInputStatus); lblInputStatus.setColour(juce::Label::textColourId, accentGreen);

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
            // Clear sharing before stopping old MTC device
            eng.getTriggerOutput().setSharedMidiOutput(nullptr);
            eng.stopMtcOutput();

            // Release trigger's own handle if it matches the new device
            eng.getMtcOutput().refreshDeviceList();
            auto mtcNames = eng.getMtcOutput().getDeviceNames();
            if (sel < mtcNames.size()
                && eng.getTriggerOutput().hasOwnMidiOpen()
                && eng.getTriggerOutput().getCurrentMidiDeviceName() == mtcNames[sel])
            {
                eng.getTriggerOutput().releaseOwnMidi();
            }

            eng.startMtcOutput(sel);

            // Re-establish sharing if devices match
            if (eng.getMtcOutput().getIsRunning())
            {
                auto trigDevName = stripComboMarker(cmbTriggerMidiDevice.getText());
                if (trigDevName == eng.getMtcOutput().getCurrentDeviceName())
                    eng.getTriggerOutput().setSharedMidiOutput(eng.getMtcOutput().getMidiOutputPtr());
                else
                    applyTriggerSettings();  // reopen trigger on its own device
            }
            populateMidiAndNetworkCombos();
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
            int sel = cmbArtnetOutputInterface.getSelectedId() - 2;
            eng.stopArtnetOutput();
            eng.startArtnetOutput(sel);
            // Update combo to actual interface before repopulate (handles fallback)
            int actualId = eng.getArtnetOutput().getSelectedInterface() + 2;
            cmbArtnetOutputInterface.setSelectedId(actualId, juce::dontSendNotification);
            populateMidiAndNetworkCombos();  // refresh markers (auto-restores all selections)
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
        {
            startCurrentLtcOutput();
            populateFilteredOutputDeviceCombos();  // refresh markers (auto-restores both combos)
            saveSettings();
        }
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
        {
            startCurrentThruOutput();
            populateFilteredOutputDeviceCombos();  // refresh markers (auto-restores both combos)
            saveSettings();
        }
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

    // GPU-accelerated rendering: DISABLED for thread safety.
    //
    // When glContext.attachTo(*this) is active, JUCE calls paint() for ALL
    // child components on the OpenGL thread -- not the message thread.
    // Meanwhile, timerCallback() writes juce::String members (artist, title,
    // playState, sourceName, etc.) on the message thread.  juce::String is
    // reference-counted: a concurrent read during write can corrupt the
    // refcount and crash.  This affects TimecodeDisplay, ProDJLinkView,
    // all juce::Label instances, and any component that reads String data
    // in paint().
    //
    // JUCE's OpenGL renderer paints into software images on the CPU and
    // only uses the GPU for the final texture upload + composite.  With
    // the waveform image cache, HiDPI deck image cache, and targeted
    // dirty-rect repainting already in place, the performance difference
    // is negligible.  Windows DWM already hardware-accelerates the native
    // GDI composite path.
    //
    // For a live performance application, eliminating the entire class of
    // GL-thread data races is worth more than the marginal compositing
    // speedup.  If GPU acceleration is ever re-enabled, all juce::String
    // members read in paint() must be replaced with atomic-safe types
    // (fixed char arrays, packed atomics, or a SpinLock-protected snapshot).
}

MainComponent::~MainComponent()
{
    // 1. Stop our UI timer first -- no more timerCallback() after this
    stopTimer();

    // 2. Detach LookAndFeel before destroying any child components
    setLookAndFeel(nullptr);

    // 3. Save settings while engines are still alive
    flushSettings();

    // 4. UpdateChecker thread stops in its own destructor (10s timeout)

    // 5. Stop AudioScanThread
    if (scanThread)
    {
        scanThread->signalThreadShouldExit();
        if (scanThread->isThreadRunning())
        {
            if (!scanThread->stopThread(2000))
            { DBG("WARNING: AudioScanThread did not stop within 2s timeout"); }
        }
        scanThread->tempManager = nullptr;  // release AudioDeviceManager early
        scanThread = nullptr;
    }

    // 6. Capture window bounds before closing (delete doesn't call closeButtonPressed)
    if (trackMapWindow != nullptr)
    {
        auto b = trackMapWindow->getBounds();
        settings.trackMapBounds = juce::String(b.getX()) + " " + juce::String(b.getY())
                                + " " + juce::String(b.getWidth()) + " " + juce::String(b.getHeight());
        delete trackMapWindow.getComponent();
    }

    if (mixerMapWindow != nullptr)
    {
        auto b = mixerMapWindow->getBounds();
        settings.mixerMapBounds = juce::String(b.getX()) + " " + juce::String(b.getY())
                                + " " + juce::String(b.getWidth()) + " " + juce::String(b.getHeight());
        delete mixerMapWindow.getComponent();
    }

    if (proDJLinkViewWindow != nullptr)
    {
        auto b = proDJLinkViewWindow->getBounds();
        settings.pdlViewBounds = juce::String(b.getX()) + " " + juce::String(b.getY())
                               + " " + juce::String(b.getWidth()) + " " + juce::String(b.getHeight());
        settings.pdlViewHorizontal = proDJLinkViewWindow->getLayoutHorizontal();
        settings.pdlViewShowMixer  = proDJLinkViewWindow->getShowMixer();
        proDJLinkViewWindow.reset();
    }

    settings.save();

    // 7. Stop metadata client (before ProDJLink -- it queries player IPs)
    sharedDbClient.stop();

    // 8. Stop ProDJLink receiver
    sharedProDJLinkInput.stop();

    // 9. Explicitly shut down each engine (timers, threads, sockets)
    //    BEFORE engines.clear() destroys the objects, so all HighResolutionTimer
    //    threads are stopped while the message manager is still alive.
    //    Also disconnect shared pointers (TrackMap, MixerMap, ProDJLink, DbServer)
    //    so no stale references survive into AppSettings destruction.
    for (auto& eng : engines)
    {
        eng->setMidiClockEnabled(false);
        eng->setTrackMap(nullptr);
        eng->setMixerMap(nullptr);
        eng->setSharedProDJLinkInput(nullptr);
        eng->setDbServerClient(nullptr);
        eng->stopMtcOutput();
        eng->stopArtnetOutput();
        eng->stopLtcOutput();
        eng->stopThruOutput();
        eng->stopMtcInput();
        eng->stopArtnetInput();
        eng->stopLtcInput();
    }

    // 10. Now safe to destroy engine objects
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
    engines.back()->setSharedProDJLinkInput(&sharedProDJLinkInput);
    engines.back()->setDbServerClient(&sharedDbClient);
    engines.back()->setTrackMap(&settings.trackMap);
    engines.back()->setMixerMap(&sharedMixerMap);
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
    // Disconnect shared pointers first to prevent stale access during stop.
    engines[(size_t)index]->setTrackMap(nullptr);
    engines[(size_t)index]->setMixerMap(nullptr);
    engines[(size_t)index]->setSharedProDJLinkInput(nullptr);
    engines[(size_t)index]->setDbServerClient(nullptr);
    engines[(size_t)index]->setMidiClockEnabled(false);
    engines[(size_t)index]->getTriggerOutput().setSharedMidiOutput(nullptr);
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
                    // Only update text -- don't rebuild buttons (avoids heap
                    // corruption from destroying components during modal callback)
                    safeThis->updateTabAppearance();
                    safeThis->resized();  // reposition in case text width changed
                    safeThis->saveSettings();
                }
            }
            // alertWindow destroyed when shared_ptr goes out of scope
        }), false);  // deleteWhenDismissed = false -- shared_ptr owns the window
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
    updateBpmMultButtonStates();

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

    // MIDI device selections -- find matching device in combo
    // First try running device, then saved setting, else deselect
    {
        int idx = findDeviceByName(cmbMidiInputDevice, eng.getMtcInput().getCurrentDeviceName());
        if (idx < 0 && selectedEngine < (int)settings.engines.size())
            idx = findDeviceByName(cmbMidiInputDevice, settings.engines[(size_t)selectedEngine].midiInputDevice);
        cmbMidiInputDevice.setSelectedId(idx >= 0 ? idx + 1 : 0, juce::dontSendNotification);
    }
    {
        int idx = findDeviceByName(cmbMidiOutputDevice, eng.getMtcOutput().getCurrentDeviceName());
        if (idx < 0 && selectedEngine < (int)settings.engines.size())
            idx = findDeviceByName(cmbMidiOutputDevice, settings.engines[(size_t)selectedEngine].midiOutputDevice);
        cmbMidiOutputDevice.setSelectedId(idx >= 0 ? idx + 1 : 0, juce::dontSendNotification);
    }

    // ArtNet interface selections -- restore from saved settings
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

        // TrackMap toggle
        btnTrackMap.setToggleState(eng.isTrackMapEnabled(), juce::dontSendNotification);

        // MIDI Clock toggle
        btnMidiClock.setToggleState(eng.isMidiClockEnabled(), juce::dontSendNotification);

        // OSC Forward toggle + address fields
        btnOscFwdBpm.setToggleState(eng.isOscForwardEnabled(), juce::dontSendNotification);
        btnOscMixerFwd.setToggleState(eng.isOscMixerForwardEnabled(), juce::dontSendNotification);
        btnMidiMixerFwd.setToggleState(eng.isMidiMixerForwardEnabled(), juce::dontSendNotification);
        cmbMidiMixCCCh.setSelectedId(eng.getMidiMixerCCChannel(), juce::dontSendNotification);
        cmbMidiMixNoteCh.setSelectedId(eng.getMidiMixerNoteChannel(), juce::dontSendNotification);
        btnArtnetMixerFwd.setToggleState(eng.isArtnetMixerForwardEnabled(), juce::dontSendNotification);
        setArtNetCombosFromAddress(cmbArtMixNet, cmbArtMixSub, cmbArtMixUni, eng.getArtnetMixerUniverse());
        setArtNetCombosFromAddress(cmbArtTrigNet, cmbArtTrigSub, cmbArtTrigUni, eng.getArtnetTriggerUniverse());
        edOscFwdBpmAddr.setText(eng.getOscFwdBpmAddr(), false);

        // Art-Net DMX interface (for trigger + mixer forward)
        {
            int artDmxId = es.artnetDmxInterface + 2;  // -1->1 (All), 0->2, 1->3...
            if (artDmxId < 1) artDmxId = 1;
            if (artDmxId <= cmbArtnetDmxInterface.getNumItems())
                cmbArtnetDmxInterface.setSelectedId(artDmxId, juce::dontSendNotification);
        }

        // Ableton Link toggle
        btnLink.setToggleState(eng.getLinkBridge().isEnabled(), juce::dontSendNotification);

        // Pro DJ Link (per-engine player)
        cmbProDJLinkPlayer.setSelectedId(juce::jlimit(1, 8, es.proDJLinkPlayer), juce::dontSendNotification);
        // ProDJLink interface (global)
        int pdlIfId = settings.proDJLinkInterface + 1;
        if (pdlIfId >= 1 && pdlIfId <= cmbProDJLinkInterface.getNumItems())
            cmbProDJLinkInterface.setSelectedId(pdlIfId, juce::dontSendNotification);

        // Trigger toggles + destination
        auto& trig = eng.getTriggerOutput();
        btnTriggerMidi.setToggleState(trig.isMidiEnabled(), juce::dontSendNotification);
        btnTriggerOsc.setToggleState(trig.isOscEnabled(), juce::dontSendNotification);
        btnArtnetTrigger.setToggleState(eng.isArtnetTriggerEnabled(), juce::dontSendNotification);

        // Populate trigger MIDI device combo
        trig.refreshMidiDevices();
        cmbTriggerMidiDevice.clear(juce::dontSendNotification);
        for (int d = 0; d < trig.getMidiDeviceCount(); ++d)
            cmbTriggerMidiDevice.addItem(trig.getMidiDeviceName(d), d + 1);
        if (trig.isMidiOpen())
        {
            int idx = trig.findMidiDeviceByName(trig.getCurrentMidiDeviceName());
            if (idx >= 0) cmbTriggerMidiDevice.setSelectedId(idx + 1, juce::dontSendNotification);
        }
        else if (es.triggerMidiDevice.isNotEmpty())
        {
            int idx = trig.findMidiDeviceByName(es.triggerMidiDevice);
            if (idx >= 0) cmbTriggerMidiDevice.setSelectedId(idx + 1, juce::dontSendNotification);
        }

        edOscIp.setText(es.oscDestIp.isNotEmpty() ? es.oscDestIp : "127.0.0.1", false);
        edOscPort.setText(juce::String(es.oscDestPort > 0 ? es.oscDestPort : 53000), false);
    }

    // Audio device selections -- find matching device in filtered list
    if (selectedEngine < (int)settings.engines.size())
    {
        auto& es = settings.engines[(size_t)selectedEngine];

        int audioInIdx = findFilteredIndex(filteredInputIndices, scannedAudioInputs,
                                            es.audioInputType, es.audioInputDevice);
        cmbAudioInputDevice.setSelectedId(audioInIdx >= 0 ? audioInIdx + 1 : 0, juce::dontSendNotification);

        int audioOutIdx = findFilteredIndex(filteredOutputIndices, scannedAudioOutputs,
                                             es.audioOutputType, es.audioOutputDevice);
        cmbAudioOutputDevice.setSelectedId(audioOutIdx >= 0 ? audioOutIdx + 1 : 0, juce::dontSendNotification);

        int thruOutIdx = findFilteredIndex(filteredOutputIndices, scannedAudioOutputs,
                                            es.thruOutputType, es.thruOutputDevice);
        cmbThruOutputDevice.setSelectedId(thruOutIdx >= 0 ? thruOutIdx + 1 : 0, juce::dontSendNotification);
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


void MainComponent::startCurrentProDJLinkInput()
{
    auto& eng = currentEngine();
    int iface = cmbProDJLinkInterface.getSelectedId() - 1;
    int player = cmbProDJLinkPlayer.getSelectedId();
    if (player < 1) player = 1;
    if (iface < 0) iface = 0;

    // If already running on a DIFFERENT interface, stop and restart.
    // This handles the case where the user changes the network interface
    // combo while ProDJLink is active. Without this, the new interface
    // selection is ignored because start() bails on getIsRunning()==true.
    if (sharedProDJLinkInput.getIsRunning()
        && sharedProDJLinkInput.getSelectedInterface() != iface)
    {
        DBG("MainComponent: ProDJLink interface changed from "
            << sharedProDJLinkInput.getSelectedInterface() << " to " << iface
            << " -- restarting");
        sharedProDJLinkInput.stop();
        // DbClient also needs restart since it depends on network connectivity
        sharedDbClient.stop();
    }

    // Start shared connection if not already running
    if (!sharedProDJLinkInput.getIsRunning())
    {
        sharedProDJLinkInput.refreshNetworkInterfaces();
        sharedProDJLinkInput.start(iface);
    }

    // Phase 2: start dbClient for metadata queries
    if (!sharedDbClient.getIsRunning())
    {
        DBG("MainComponent: starting DbServerClient for metadata queries");
        sharedDbClient.start();
    }
    else
    {
        DBG("MainComponent: DbServerClient already running");
    }

    // Set this engine's player
    eng.startProDJLinkInput(player);
    sharedProDJLinkInput.setOutputFrameRate(eng.getCurrentFps());
}

void MainComponent::applyTriggerSettings()
{
    auto& eng = currentEngine();
    auto& trig = eng.getTriggerOutput();

    // --- MIDI ---
    trig.setMidiEnabled(btnTriggerMidi.getToggleState());
    bool needsMidi = btnTriggerMidi.getToggleState() || eng.isMidiClockEnabled()
                  || eng.isMidiMixerForwardEnabled();
    if (needsMidi)
    {
        auto trigDevName = stripComboMarker(cmbTriggerMidiDevice.getText());

        // Share MtcOutput's handle if it's already open on the same device
        if (eng.getMtcOutput().getIsRunning()
            && trigDevName == eng.getMtcOutput().getCurrentDeviceName())
        {
            trig.releaseOwnMidi();
            trig.setSharedMidiOutput(eng.getMtcOutput().getMidiOutputPtr());
        }
        else
        {
            // Different device or MTC not running -- open our own handle
            trig.setSharedMidiOutput(nullptr);
            int sel = cmbTriggerMidiDevice.getSelectedId() - 1;
            if (sel >= 0)
            {
                if (!trig.hasOwnMidiOpen()
                    || trig.getCurrentMidiDeviceName() != trigDevName)
                {
                    trig.startMidi(sel);
                }
            }
        }
    }
    else
    {
        trig.setSharedMidiOutput(nullptr);
        trig.stopMidi();
    }

    // --- OSC ---
    trig.setOscEnabled(btnTriggerOsc.getToggleState());
    bool needsOsc = btnTriggerOsc.getToggleState() || eng.isOscForwardEnabled()
                 || eng.isOscMixerForwardEnabled();
    if (needsOsc)
    {
        auto ip = edOscIp.getText().trim();
        if (ip.isEmpty()) ip = "127.0.0.1";
        int port = edOscPort.getText().getIntValue();
        if (port <= 0 || port > 65535) port = 53000;

        if (!trig.isOscConnected())
            trig.connectOsc(ip, port);
        else
            trig.updateOscDestination(ip, port);
    }
    else
    {
        trig.disconnectOsc();
    }

    // Refresh visibility (MIDI device combo / OSC fields show/hide)
    updateDeviceSelectorVisibility();
    resized();
}

//==============================================================================
// Propagate global Pro DJ Link settings to all engines.
// BPM sources (MIDI Clock, OSC BPM, Ableton Link) and mixer forward
// are network-global: one master, one DJM. When enabled on any engine,
// all engines should share the same enabled state.
//==============================================================================
void MainComponent::propagateGlobalSettings()
{
    auto& cur = currentEngine();

    bool clockEn   = cur.isMidiClockEnabled();
    bool oscBpmEn  = cur.isOscForwardEnabled();
    auto  bpmAddr  = cur.getOscFwdBpmAddr();
    bool oscMixEn  = cur.isOscMixerForwardEnabled();
    bool midiMixEn = cur.isMidiMixerForwardEnabled();
    int  midiCCCh  = cur.getMidiMixerCCChannel();
    int  midiNotCh = cur.getMidiMixerNoteChannel();
    bool artMixEn  = cur.isArtnetMixerForwardEnabled();
    int  artMixUni = cur.getArtnetMixerUniverse();
    bool linkEn    = cur.getLinkBridge().isEnabled();

    for (auto& engPtr : engines)
    {
        if (engPtr.get() == &cur) continue;
        auto& eng = *engPtr;

        eng.setMidiClockEnabled(clockEn);
        eng.setOscForward(oscBpmEn, bpmAddr);
        eng.setOscMixerForward(oscMixEn);
        eng.setMidiMixerForward(midiMixEn, midiCCCh, midiNotCh);
        eng.setArtnetMixerForward(artMixEn, artMixUni);
        eng.getLinkBridge().setEnabled(linkEn);
    }
}

void MainComponent::openTrackMapEditor()
{
    // If already open, bring to front
    if (trackMapWindow != nullptr)
    {
        trackMapWindow->toFront(true);
        return;
    }

    auto& eng = currentEngine();

    auto* editor = new TrackMapEditor(settings.trackMap, &sharedProDJLinkInput);
    editor->setDbServerClient(&sharedDbClient);
    {
        auto info = eng.getActiveTrackInfo();
        editor->setActiveTrack(info.artist, info.title);
    }

    editor->onChange = [this]
    {
        settings.trackMap.save();
        // Refresh all engines' TrackMap lookups
        for (auto& e : engines)
            e->refreshTrackMapLookup();
    };

    // Non-modal window (self-deleting on close so SafePointer auto-nulls)
    struct FloatingWindow : juce::DocumentWindow
    {
        FloatingWindow(const juce::String& t, juce::Colour bg)
            : DocumentWindow(t, bg, DocumentWindow::closeButton) {}
        void closeButtonPressed() override { if (onClose) onClose(); delete this; }
        std::function<void()> onClose;
    };

    auto* win = new FloatingWindow("Track Map Editor", juce::Colour(0xFF12141A));
    win->setContentOwned(editor, true);
    win->setUsingNativeTitleBar(false);
    win->setTitleBarHeight(20);
    win->setColour(juce::DocumentWindow::textColourId, juce::Colour(0xFF546E7A));
    win->setResizable(true, true);
    win->centreWithSize(editor->getWidth(), editor->getHeight());
    if (settings.trackMapBounds.isNotEmpty())
    {
        auto parts = juce::StringArray::fromTokens(settings.trackMapBounds, " ", "");
        if (parts.size() == 4)
        {
            auto b = juce::Rectangle<int>(parts[0].getIntValue(), parts[1].getIntValue(),
                                           parts[2].getIntValue(), parts[3].getIntValue());
            if (b.getWidth() >= 200 && b.getHeight() >= 150)
            {
                auto c = b.getCentre();
                for (auto& d : juce::Desktop::getInstance().getDisplays().displays)
                    if (d.userArea.contains(c)) { win->setBounds(b); break; }
            }
        }
    }
    win->onClose = [this, win]()
    {
        auto b = win->getBounds();
        settings.trackMapBounds = juce::String(b.getX()) + " " + juce::String(b.getY())
                                + " " + juce::String(b.getWidth()) + " " + juce::String(b.getHeight());
        saveSettings();
    };
    win->setVisible(true);
    trackMapWindow = win;
}

void MainComponent::openMixerMapEditor()
{
    if (mixerMapWindow != nullptr)
    {
        mixerMapWindow->toFront(true);
        return;
    }

    auto* editor = new MixerMapEditor(sharedMixerMap,
                                      djmModelFromString(sharedProDJLinkInput.getDJMModel()));
    editor->onChange = [this]
    {
        sharedMixerMap.save();
    };

    // Non-modal window (self-deleting on close so SafePointer auto-nulls)
    struct FloatingWindow : juce::DocumentWindow
    {
        FloatingWindow(const juce::String& t, juce::Colour bg)
            : DocumentWindow(t, bg, DocumentWindow::closeButton) {}
        void closeButtonPressed() override { if (onClose) onClose(); delete this; }
        std::function<void()> onClose;
    };

    auto* win = new FloatingWindow("Mixer Map - DJM Parameter Routing", juce::Colour(0xFF12141A));
    win->setContentOwned(editor, true);
    win->setUsingNativeTitleBar(false);
    win->setTitleBarHeight(20);
    win->setColour(juce::DocumentWindow::textColourId, juce::Colour(0xFF546E7A));
    win->setResizable(true, true);
    win->centreWithSize(editor->getWidth(), editor->getHeight());
    if (settings.mixerMapBounds.isNotEmpty())
    {
        auto parts = juce::StringArray::fromTokens(settings.mixerMapBounds, " ", "");
        if (parts.size() == 4)
        {
            auto b = juce::Rectangle<int>(parts[0].getIntValue(), parts[1].getIntValue(),
                                           parts[2].getIntValue(), parts[3].getIntValue());
            if (b.getWidth() >= 200 && b.getHeight() >= 150)
            {
                auto c = b.getCentre();
                for (auto& d : juce::Desktop::getInstance().getDisplays().displays)
                    if (d.userArea.contains(c)) { win->setBounds(b); break; }
            }
        }
    }
    win->onClose = [this, win]()
    {
        auto b = win->getBounds();
        settings.mixerMapBounds = juce::String(b.getX()) + " " + juce::String(b.getY())
                                + " " + juce::String(b.getWidth()) + " " + juce::String(b.getHeight());
        saveSettings();
    };
    win->setVisible(true);
    mixerMapWindow = win;
}

void MainComponent::openProDJLinkView()
{
    // If already open and visible, bring to front
    if (proDJLinkViewWindow != nullptr && proDJLinkViewWindow->isVisible())
    {
        proDJLinkViewWindow->toFront(true);
        return;
    }

    // Recreate window (simpler than restarting timer + resetting state)
    proDJLinkViewWindow = std::make_unique<ProDJLinkViewWindow>(
        sharedProDJLinkInput, sharedDbClient, settings.trackMap, engines);

    // Restore layout state and window bounds
    proDJLinkViewWindow->setLayoutState(settings.pdlViewHorizontal, settings.pdlViewShowMixer);
    if (settings.pdlViewBounds.isNotEmpty())
        proDJLinkViewWindow->restoreFromBoundsString(settings.pdlViewBounds);

    proDJLinkViewWindow->setOnTrackMapChanged([this]
    {
        settings.trackMap.save();
        for (auto& eng : engines)
            eng->refreshTrackMapLookup();
        updateBpmMultButtonStates();

        if (trackMapWindow != nullptr)
        {
            if (auto* editor = dynamic_cast<TrackMapEditor*>(trackMapWindow->getContentComponent()))
                editor->refresh();
        }
    });
    proDJLinkViewWindow->setOnLayoutChanged([this]
    {
        if (proDJLinkViewWindow != nullptr)
        {
            settings.pdlViewHorizontal = proDJLinkViewWindow->getLayoutHorizontal();
            settings.pdlViewShowMixer  = proDJLinkViewWindow->getShowMixer();
            saveSettings();
        }
    });
    proDJLinkViewWindow->onBoundsCapture = [this]
    {
        if (proDJLinkViewWindow != nullptr)
        {
            auto b = proDJLinkViewWindow->getBounds();
            settings.pdlViewBounds = juce::String(b.getX()) + " " + juce::String(b.getY())
                                   + " " + juce::String(b.getWidth()) + " " + juce::String(b.getHeight());
            settings.pdlViewHorizontal = proDJLinkViewWindow->getLayoutHorizontal();
            settings.pdlViewShowMixer  = proDJLinkViewWindow->getShowMixer();
            saveSettings();
        }
    };
}

//==============================================================================
// CONFIGURATION BACKUP / RESTORE
//==============================================================================
void MainComponent::exportConfig()
{
    // Save current state to disk first so the export is up-to-date
    saveSettings();
    sharedMixerMap.save();

    configFileChooser = std::make_unique<juce::FileChooser>(
        "Export STC Configuration",
        juce::File::getSpecialLocation(juce::File::userDesktopDirectory)
            .getChildFile("stc_backup.json"),
        "*.json");

    configFileChooser->launchAsync(
        juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
        [this](const juce::FileChooser& fc)
        {
            auto file = fc.getResult();
            if (file == juce::File()) return;

            auto bundle = settings.buildExportBundle();
            file.replaceWithText(juce::JSON::toString(bundle));
        });
}

void MainComponent::importConfig()
{
    configFileChooser = std::make_unique<juce::FileChooser>(
        "Import STC Configuration",
        juce::File::getSpecialLocation(juce::File::userDesktopDirectory),
        "*.json");

    configFileChooser->launchAsync(
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this](const juce::FileChooser& fc)
        {
            auto file = fc.getResult();
            if (file == juce::File() || !file.existsAsFile()) return;

            auto parsed = juce::JSON::parse(file.loadFileAsString());
            auto* obj = parsed.getDynamicObject();
            if (!obj || !obj->hasProperty("stc_backup_version"))
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::MessageBoxIconType::WarningIcon,
                    "Import Failed",
                    "This file is not a valid STC backup.");
                return;
            }

            auto options = juce::MessageBoxOptions()
                .withIconType(juce::MessageBoxIconType::QuestionIcon)
                .withTitle("Import Configuration")
                .withMessage("This will replace ALL current settings, track maps, and mixer maps. "
                             "The application will need to restart to apply changes.\n\n"
                             "Continue?")
                .withButton("Import")
                .withButton("Cancel");

            importConfirmBox = juce::AlertWindow::showScopedAsync(options,
                [this, parsed](int result)
                {
                    if (result != 1) return;

                    if (settings.applyImportBundle(parsed))
                    {
                        // Reload settings into live state
                        settings.load();
                        sharedMixerMap.resetToDefaults();
                        sharedMixerMap.load();

                        juce::AlertWindow::showMessageBoxAsync(
                            juce::MessageBoxIconType::InfoIcon,
                            "Import Complete",
                            "Configuration restored successfully.\n\n"
                            "Please restart STC to fully apply all settings "
                            "(engine configuration, audio devices, etc.).");
                    }
                    else
                    {
                        juce::AlertWindow::showMessageBoxAsync(
                            juce::MessageBoxIconType::WarningIcon,
                            "Import Failed",
                            "Could not apply the backup file.");
                    }
                });
        });
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
    auto& trig = eng.getTriggerOutput();
    int sel = cmbMidiOutputDevice.getSelectedId() - 1;

    // If TriggerOutput has its OWN handle open on the same device,
    // release it before MtcOutput opens -- Windows forbids two handles.
    if (trig.hasOwnMidiOpen() && sel >= 0)
    {
        eng.getMtcOutput().refreshDeviceList();
        auto mtcNames = eng.getMtcOutput().getDeviceNames();
        if (sel < mtcNames.size()
            && trig.getCurrentMidiDeviceName() == mtcNames[sel])
        {
            trig.releaseOwnMidi();
        }
    }

    eng.startMtcOutput(sel);

    // Share MtcOutput's open handle with TriggerOutput if same device
    if (eng.getMtcOutput().getIsRunning())
    {
        auto trigDevName = stripComboMarker(cmbTriggerMidiDevice.getText());
        if (trigDevName == eng.getMtcOutput().getCurrentDeviceName())
            trig.setSharedMidiOutput(eng.getMtcOutput().getMidiOutputPtr());
        else
            trig.setSharedMidiOutput(nullptr);
    }
}

void MainComponent::startCurrentArtnetOutput()
{
    auto& eng = currentEngine();
    // ArtnetOutput convention: -1 = All Interfaces, 0 = first NIC
    // Combo IDs: 1 = All, 2 = first NIC -> subtract 2
    int sel = cmbArtnetOutputInterface.getSelectedId() - 2;
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
    else if (!eng.isOutputMtcEnabled() && eng.getMtcOutput().getIsRunning())
    {
        // Clear sharing before stopping -- TriggerOutput must not hold a dangling pointer
        eng.getTriggerOutput().setSharedMidiOutput(nullptr);
        eng.stopMtcOutput();
        // Re-open trigger's own device if it still needs MIDI
        applyTriggerSettings();
    }

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
        { DBG("WARNING: AudioScanThread did not stop within 2s timeout -- skipping new scan"); return; }
    }
    scanThread = std::make_unique<AudioScanThread>(this);
    // Create AudioDeviceManager on the message thread -- JUCE 8.x internally
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
    int savedId = cmbAudioInputDevice.getSelectedId();
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
    if (savedId > 0 && savedId <= cmbAudioInputDevice.getNumItems())
        cmbAudioInputDevice.setSelectedId(savedId, juce::dontSendNotification);
}

void MainComponent::populateFilteredOutputDeviceCombos()
{
    int savedOutId = cmbAudioOutputDevice.getSelectedId();
    int savedThruId = cmbThruOutputDevice.getSelectedId();
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
    if (savedOutId > 0 && savedOutId <= cmbAudioOutputDevice.getNumItems())
        cmbAudioOutputDevice.setSelectedId(savedOutId, juce::dontSendNotification);
    if (savedThruId > 0 && savedThruId <= cmbThruOutputDevice.getNumItems())
        cmbThruOutputDevice.setSelectedId(savedThruId, juce::dontSendNotification);
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
            // MIDI input (typeName empty -> MIDI check)
            if (typeName.isEmpty()
                && eng.getMtcInput().getIsRunning()
                && eng.getMtcInput().getCurrentDeviceName() == devName)
            {
                if (isCurrent) { result += juce::String::charToString(0x25CF); }   // "*"
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

    // "*" prefix for current engine's active device
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
    // Save current selections before clearing
    int savedMidiIn   = cmbMidiInputDevice.getSelectedId();
    int savedMidiOut  = cmbMidiOutputDevice.getSelectedId();
    int savedArtIn    = cmbArtnetInputInterface.getSelectedId();
    int savedArtOut   = cmbArtnetOutputInterface.getSelectedId();
    int savedArtDmx   = cmbArtnetDmxInterface.getSelectedId();

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
    cmbArtnetDmxInterface.clear(juce::dontSendNotification);

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
    cmbArtnetDmxInterface.addItem("All Interfaces (Broadcast)", 1);
    for (int i = 0; i < nets.size(); i++)
    {
        auto label = nets[i].name + " (" + nets[i].ip + ")";
        cmbArtnetInputInterface.addItem(label + getArtnetMarker(i + 2, true), i + 2);
        cmbArtnetOutputInterface.addItem(label + getArtnetMarker(i + 2, false), i + 2);
        cmbArtnetDmxInterface.addItem(label, i + 2);
    }

    // Pro DJ Link interfaces
    int savedProDJLinkIf = cmbProDJLinkInterface.getSelectedId();
    cmbProDJLinkInterface.clear(juce::dontSendNotification);
    for (int i = 0; i < nets.size(); i++)
        cmbProDJLinkInterface.addItem(nets[i].name + " (" + nets[i].ip + ")", i + 1);
    if (savedProDJLinkIf > 0 && savedProDJLinkIf <= cmbProDJLinkInterface.getNumItems())
        cmbProDJLinkInterface.setSelectedId(savedProDJLinkIf, juce::dontSendNotification);
    else if (cmbProDJLinkInterface.getNumItems() > 0)
        cmbProDJLinkInterface.setSelectedId(1, juce::dontSendNotification);

    // Restore all selections (IDs are stable across repopulate)
    if (savedMidiIn > 0 && savedMidiIn <= cmbMidiInputDevice.getNumItems())
        cmbMidiInputDevice.setSelectedId(savedMidiIn, juce::dontSendNotification);
    if (savedMidiOut > 0 && savedMidiOut <= cmbMidiOutputDevice.getNumItems())
        cmbMidiOutputDevice.setSelectedId(savedMidiOut, juce::dontSendNotification);
    if (savedArtIn > 0 && savedArtIn <= cmbArtnetInputInterface.getNumItems())
        cmbArtnetInputInterface.setSelectedId(savedArtIn, juce::dontSendNotification);
    if (savedArtOut > 0 && savedArtOut <= cmbArtnetOutputInterface.getNumItems())
        cmbArtnetOutputInterface.setSelectedId(savedArtOut, juce::dontSendNotification);
    if (savedArtDmx > 0 && savedArtDmx <= cmbArtnetDmxInterface.getNumItems())
        cmbArtnetDmxInterface.setSelectedId(savedArtDmx, juce::dontSendNotification);
    else if (cmbArtnetDmxInterface.getNumItems() > 0)
        cmbArtnetDmxInterface.setSelectedId(1, juce::dontSendNotification);
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
    {
        engines.push_back(std::make_unique<TimecodeEngine>((int)engines.size()));
        engines.back()->setSharedProDJLinkInput(&sharedProDJLinkInput);
        engines.back()->setMixerMap(&sharedMixerMap);
    }

    // Load mixer map (user-editable DJM param -> OSC/MIDI mapping)
    sharedMixerMap.load();

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
        else if (src == InputSource::ProDJLink)
        {
            if (!sharedProDJLinkInput.getIsRunning())
            {
                sharedProDJLinkInput.refreshNetworkInterfaces();
                sharedProDJLinkInput.start(settings.proDJLinkInterface);
            }
            // Phase 2: start dbClient for metadata queries
            if (!sharedDbClient.getIsRunning())
            {
                DBG("MainComponent: starting DbServerClient (settings restore)");
                sharedDbClient.start();
            }
            eng.startProDJLinkInput(es.proDJLinkPlayer);
        }

        // TrackMap -- wire pointer and restore enabled state
        eng.setTrackMap(&settings.trackMap);
        eng.setTrackMapEnabled(es.trackMapEnabled);

        // Track change triggers -- restore enable state and connect destinations
        eng.getTriggerOutput().setMidiEnabled(es.triggerMidiEnabled);
        eng.getTriggerOutput().setOscEnabled(es.triggerOscEnabled);

        // Open MIDI if any feature needs it (triggers, MIDI clock, or mixer forward)
        if ((es.triggerMidiEnabled || es.midiClockEnabled || es.midiMixerForward) && es.triggerMidiDevice.isNotEmpty())
            eng.getTriggerOutput().startMidiByName(es.triggerMidiDevice);

        // Enable MIDI Clock after device is open
        if (es.midiClockEnabled)
            eng.setMidiClockEnabled(true);

        // OSC BPM forward
        if (es.oscBpmForward)
            eng.setOscForward(true, es.oscBpmAddr);

        // Mixer fader forward
        if (es.oscMixerForward)
            eng.setOscMixerForward(true);
        if (es.midiMixerForward)
            eng.setMidiMixerForward(true, es.midiMixerCCChannel, es.midiMixerNoteChannel);
        if (es.artnetMixerForward)
            eng.setArtnetMixerForward(true, es.artnetMixerUniverse);
        eng.setArtnetTriggerUniverse(es.artnetTriggerUniverse);
        eng.setArtnetTriggerEnabled(es.artnetTriggerEnabled);

        // Ableton Link
        eng.getLinkBridge().setEnabled(es.linkEnabled);

        // Connect OSC if any feature needs it (triggers, OSC BPM forward, or mixer forward)
        if (es.triggerOscEnabled || es.oscBpmForward || es.oscMixerForward)
        {
            auto ip = es.oscDestIp.isNotEmpty() ? es.oscDestIp : juce::String("127.0.0.1");
            auto port = es.oscDestPort > 0 ? es.oscDestPort : 53000;
            eng.getTriggerOutput().connectOsc(ip, port);
        }

        // Start non-audio outputs
        if (es.mtcOutEnabled)
        {
            int idx = findDeviceByName(cmbMidiOutputDevice, es.midiOutputDevice);

            // Release TriggerOutput's own handle if it matches the MTC device --
            // Windows forbids two handles to the same MIDI port.
            if (eng.getTriggerOutput().hasOwnMidiOpen()
                && es.triggerMidiDevice == es.midiOutputDevice)
            {
                eng.getTriggerOutput().releaseOwnMidi();
            }

            eng.startMtcOutput(idx);

            // Share MtcOutput's handle with TriggerOutput if same device
            if (eng.getMtcOutput().getIsRunning()
                && es.triggerMidiDevice == eng.getMtcOutput().getCurrentDeviceName())
            {
                eng.getTriggerOutput().setSharedMidiOutput(
                    eng.getMtcOutput().getMidiOutputPtr());
            }
        }
        if (es.artnetOutEnabled)
            eng.startArtnetOutput(es.artnetOutputInterface - 1);  // saved as combo-1; ArtnetOutput needs -1=All, 0=firstNIC
        else if (es.artnetMixerForward && !eng.getArtnetOutput().getIsRunning())
            eng.startArtnetOutput(es.artnetDmxInterface);  // DMX mixer needs the socket even without timecode output
        else if (es.artnetTriggerEnabled && !eng.getArtnetOutput().getIsRunning())
            eng.startArtnetOutput(es.artnetDmxInterface);  // DMX triggers need the socket even without timecode output
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
            es.midiInputDevice = stripComboMarker(cmbMidiInputDevice.getText());
            es.midiOutputDevice = stripComboMarker(cmbMidiOutputDevice.getText());
            es.artnetInputInterface = cmbArtnetInputInterface.getSelectedId() - 1;
            es.artnetOutputInterface = cmbArtnetOutputInterface.getSelectedId() - 1;
            es.trackMapEnabled = eng.isTrackMapEnabled();
            es.midiClockEnabled = eng.isMidiClockEnabled();
            es.oscBpmForward    = eng.isOscForwardEnabled();
            es.oscBpmAddr       = eng.getOscFwdBpmAddr();
            es.oscMixerForward  = eng.isOscMixerForwardEnabled();
            es.midiMixerForward = eng.isMidiMixerForwardEnabled();
            es.midiMixerCCChannel   = eng.getMidiMixerCCChannel();
            es.midiMixerNoteChannel = eng.getMidiMixerNoteChannel();
            es.artnetMixerForward  = eng.isArtnetMixerForwardEnabled();
            es.artnetMixerUniverse = eng.getArtnetMixerUniverse();
            es.artnetTriggerUniverse = eng.getArtnetTriggerUniverse();
            es.artnetDmxInterface = cmbArtnetDmxInterface.getSelectedId() - 2;  // combo 1->-1(All), 2->0, 3->1...
            es.linkEnabled = eng.getLinkBridge().isEnabled();

            // Pro DJ Link
            es.proDJLinkPlayer = cmbProDJLinkPlayer.getSelectedId();
            if (es.proDJLinkPlayer < 1) es.proDJLinkPlayer = 1;

            // Track change triggers
            es.triggerMidiEnabled = eng.getTriggerOutput().isMidiEnabled();
            es.triggerOscEnabled  = eng.getTriggerOutput().isOscEnabled();
            es.artnetTriggerEnabled = eng.isArtnetTriggerEnabled();
            if (cmbTriggerMidiDevice.getSelectedId() > 0)
                es.triggerMidiDevice = cmbTriggerMidiDevice.getText();
            else if (eng.getTriggerOutput().isMidiOpen())
                es.triggerMidiDevice = eng.getTriggerOutput().getCurrentMidiDeviceName();
            auto oscIpText = edOscIp.getText().trim();
            es.oscDestIp   = oscIpText.isNotEmpty() ? oscIpText : "127.0.0.1";
            auto oscPortText = edOscPort.getText().trim();
            es.oscDestPort = oscPortText.isNotEmpty() ? juce::jlimit(1, 65535, oscPortText.getIntValue()) : 53000;

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
            es.trackMapEnabled = eng.isTrackMapEnabled();
            es.midiClockEnabled = eng.isMidiClockEnabled();
            es.oscBpmForward    = eng.isOscForwardEnabled();
            es.oscBpmAddr       = eng.getOscFwdBpmAddr();
            es.oscMixerForward  = eng.isOscMixerForwardEnabled();
            es.midiMixerForward = eng.isMidiMixerForwardEnabled();
            es.midiMixerCCChannel   = eng.getMidiMixerCCChannel();
            es.midiMixerNoteChannel = eng.getMidiMixerNoteChannel();
            es.artnetMixerForward  = eng.isArtnetMixerForwardEnabled();
            es.artnetMixerUniverse = eng.getArtnetMixerUniverse();
            es.artnetTriggerUniverse = eng.getArtnetTriggerUniverse();
            es.linkEnabled = eng.getLinkBridge().isEnabled();

            // Pro DJ Link
            es.proDJLinkPlayer = eng.getProDJLinkPlayer();

            // Track change triggers
            es.triggerMidiEnabled = eng.getTriggerOutput().isMidiEnabled();
            es.triggerOscEnabled  = eng.getTriggerOutput().isOscEnabled();
            es.artnetTriggerEnabled = eng.isArtnetTriggerEnabled();
            if (eng.getTriggerOutput().isMidiOpen())
                es.triggerMidiDevice = eng.getTriggerOutput().getCurrentMidiDeviceName();
        }
    }

    // Pro DJ Link global settings
    settings.proDJLinkInterface = cmbProDJLinkInterface.getSelectedId() - 1;
    if (settings.proDJLinkInterface < 0) settings.proDJLinkInterface = 0;

    // Capture window bounds (if windows are open and visible)
    if (proDJLinkViewWindow != nullptr && proDJLinkViewWindow->isVisible())
    {
        auto b = proDJLinkViewWindow->getBounds();
        settings.pdlViewBounds = juce::String(b.getX()) + " " + juce::String(b.getY())
                               + " " + juce::String(b.getWidth()) + " " + juce::String(b.getHeight());
        settings.pdlViewHorizontal = proDJLinkViewWindow->getLayoutHorizontal();
        settings.pdlViewShowMixer  = proDJLinkViewWindow->getShowMixer();
    }
    if (trackMapWindow != nullptr && trackMapWindow->isVisible())
    {
        auto b = trackMapWindow->getBounds();
        settings.trackMapBounds = juce::String(b.getX()) + " " + juce::String(b.getY())
                                + " " + juce::String(b.getWidth()) + " " + juce::String(b.getHeight());
    }
    if (mixerMapWindow != nullptr && mixerMapWindow->isVisible())
    {
        auto b = mixerMapWindow->getBounds();
        settings.mixerMapBounds = juce::String(b.getX()) + " " + juce::String(b.getY())
                                + " " + juce::String(b.getWidth()) + " " + juce::String(b.getHeight());
    }

    settings.save();
}

int MainComponent::findDeviceByName(const juce::ComboBox& cmb, const juce::String& name)
{
    if (name.isEmpty()) return -1;
    // Strip any markers that may have been saved in older settings files
    auto cleanName = stripComboMarker(name);
    if (cleanName.isEmpty()) return -1;

    static const juce::String dotSuffix = juce::String(" ") + juce::String::charToString(0x25CF);
    for (int i = 0; i < cmb.getNumItems(); i++)
    {
        auto text = cmb.getItemText(i);
        // Exact match, or match ignoring the " [ENGINE N]" or " *" marker suffix
        if (text == cleanName || text.startsWith(cleanName + " [") || text == cleanName + dotSuffix)
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
    bool showProDJLinkIn = (input == InputSource::ProDJLink) && inputConfigExpanded;
    bool showAudioOut = (eng.isOutputLtcEnabled() || (eng.isPrimary() && eng.isOutputThruEnabled()));
    bool hasInputConfig = (input != InputSource::SystemTime);

    btnCollapseInput.setVisible(hasInputConfig);
    updateCollapseButtonText(btnCollapseInput, inputConfigExpanded);

    cmbMidiInputDevice.setVisible(showMidiIn);       lblMidiInputDevice.setVisible(showMidiIn);
    cmbArtnetInputInterface.setVisible(showArtnetIn); lblArtnetInputInterface.setVisible(showArtnetIn);
    btnTrackMap.setVisible(showProDJLinkIn);
    btnTrackMapEdit.setVisible(showProDJLinkIn);
    btnProDJLinkView.setVisible(showProDJLinkIn);
    btnMixerMapEdit.setVisible(showProDJLinkIn);
    btnBpmOff.setVisible(showProDJLinkIn);
    btnBpmX2.setVisible(showProDJLinkIn);
    btnBpmX4.setVisible(showProDJLinkIn);
    btnBpmD2.setVisible(showProDJLinkIn);
    btnBpmD4.setVisible(showProDJLinkIn);
    btnMidiClock.setVisible(showProDJLinkIn);
    btnOscFwdBpm.setVisible(showProDJLinkIn);
    edOscFwdBpmAddr.setVisible(showProDJLinkIn && btnOscFwdBpm.getToggleState());
    lblOscFwdBpmAddr.setVisible(showProDJLinkIn && btnOscFwdBpm.getToggleState());
    btnOscMixerFwd.setVisible(showProDJLinkIn);
    btnMidiMixerFwd.setVisible(showProDJLinkIn);
    {
        bool showMidiMix = showProDJLinkIn && btnMidiMixerFwd.getToggleState();
        cmbMidiMixCCCh.setVisible(showMidiMix);
        lblMidiMixCCCh.setVisible(showMidiMix);
        cmbMidiMixNoteCh.setVisible(showMidiMix);
        lblMidiMixNoteCh.setVisible(showMidiMix);
    }
    btnArtnetMixerFwd.setVisible(showProDJLinkIn);
    {
        bool showArtMix = showProDJLinkIn && btnArtnetMixerFwd.getToggleState();
        cmbArtMixNet.setVisible(showArtMix);
        cmbArtMixSub.setVisible(showArtMix);
        cmbArtMixUni.setVisible(showArtMix);
        lblArtMixAddr.setVisible(showArtMix);
    }
    btnLink.setVisible(showProDJLinkIn);
    lblLinkStatus.setVisible(showProDJLinkIn && btnLink.getToggleState());

    btnTriggerMidi.setVisible(showProDJLinkIn);
    cmbTriggerMidiDevice.setVisible(showProDJLinkIn
        && (btnTriggerMidi.getToggleState() || btnMidiClock.getToggleState()
            || btnMidiMixerFwd.getToggleState()));
    btnTriggerOsc.setVisible(showProDJLinkIn);
    btnArtnetTrigger.setVisible(showProDJLinkIn);
    {
        bool showArtTrig = showProDJLinkIn && btnArtnetTrigger.getToggleState();
        cmbArtTrigNet.setVisible(showArtTrig);
        cmbArtTrigSub.setVisible(showArtTrig);
        cmbArtTrigUni.setVisible(showArtTrig);
        lblArtTrigAddr.setVisible(showArtTrig);
    }
    {
        bool showArtDmxIface = showProDJLinkIn
            && (btnArtnetMixerFwd.getToggleState() || btnArtnetTrigger.getToggleState());
        cmbArtnetDmxInterface.setVisible(showArtDmxIface);
        lblArtnetDmxInterface.setVisible(showArtDmxIface);
    }
    edOscIp.setVisible(showProDJLinkIn
        && (btnTriggerOsc.getToggleState() || btnOscFwdBpm.getToggleState()
            || btnOscMixerFwd.getToggleState()));
    edOscPort.setVisible(showProDJLinkIn
        && (btnTriggerOsc.getToggleState() || btnOscFwdBpm.getToggleState()
            || btnOscMixerFwd.getToggleState()));

    // Pro DJ Link
    cmbProDJLinkInterface.setVisible(showProDJLinkIn);  lblProDJLinkInterface.setVisible(showProDJLinkIn);
    cmbProDJLinkPlayer.setVisible(showProDJLinkIn);     lblProDJLinkPlayer.setVisible(showProDJLinkIn);
    lblProDJLinkMetadata.setVisible(showProDJLinkIn);
    lblProDJLinkTrackInfo.setVisible(showProDJLinkIn);
    lblMixerStatus.setVisible(showProDJLinkIn);
    artworkDisplay.setVisible(showProDJLinkIn);
    waveformDisplay.setVisible(showProDJLinkIn);

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

    // FPS Convert: not applicable for ProDJLink (user selects fps directly)
    bool showFpsConvert = (input != InputSource::ProDJLink);
    btnFpsConvert.setVisible(showFpsConvert);
    if (!showFpsConvert && eng.isFpsConvertEnabled())
    {
        eng.setFpsConvertEnabled(false);
        btnFpsConvert.setToggleState(false, juce::dontSendNotification);
    }

    // Close TrackMap / MixerMap editor windows when no engine uses ProDJLink
    // (these are shared windows -- keep open as long as any engine is on PDL)
    {
        bool anyPdl = false;
        for (auto& e : engines)
            if (e->getActiveInput() == InputSource::ProDJLink) { anyPdl = true; break; }
        if (!anyPdl)
        {
            if (trackMapWindow != nullptr)
                delete trackMapWindow.getComponent();
            if (mixerMapWindow != nullptr)
                delete mixerMapWindow.getComponent();
        }
    }

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
// MINI STRIP -- compact timecode monitors for non-selected engines
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

        // Timecode (big monospace) -- use '.' before frames to match main display
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
    int panelWidth = juce::jlimit(240, 290, 240 + (getWidth() - 800) / 8);

    // Left panel background
    auto leftPanel = bounds.removeFromLeft(panelWidth);
    g.setColour(bgPanel); g.fillRect(leftPanel);
    g.setColour(borderCol); g.drawLine((float)leftPanel.getRight(), 0.0f, (float)leftPanel.getRight(), (float)getHeight(), 1.0f);
    g.setColour(textDim); g.setFont(juce::Font(juce::FontOptions(getMonoFontName(), 10.0f, juce::Font::bold)));
    g.drawText(">> SOURCE INPUT", leftPanel.withHeight(40).translated(0, kTabBarHeight).reduced(16, 0), juce::Justification::centredLeft);

    // Right panel background
    auto rightPanel = bounds.removeFromRight(panelWidth);
    g.setColour(bgPanel); g.fillRect(rightPanel);
    g.setColour(borderCol); g.drawLine((float)rightPanel.getX(), 0.0f, (float)rightPanel.getX(), (float)getHeight(), 1.0f);
    g.setColour(textDim); g.drawText(">> OUTPUTS", rightPanel.withHeight(40).translated(0, kTabBarHeight).reduced(16, 0), juce::Justification::centredLeft);

    // Tab bar (top of content area)
    g.setColour(bgDarker); g.fillRect(0, 0, getWidth(), kTabBarHeight);
    g.setColour(borderCol); g.drawLine(0.0f, (float)kTabBarHeight, (float)getWidth(), (float)kTabBarHeight, 1.0f);

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

    g.setColour(statusColour); g.drawText(statusText, juce::Rectangle<int>(getWidth() - 420, getHeight() - bbH, 270, bbH), juce::Justification::centredRight);

    // Center area: display background + frame rate labels
    auto centerArea = getLocalBounds().reduced(panelWidth, 0);

    // Subtle background for the timecode display area
    {
        auto displayBg = centerArea;
        displayBg.removeFromTop(kTabBarHeight);
        displayBg.removeFromBottom(bbH);
        // Inner panel with very subtle rounded rect
        auto innerBg = displayBg.reduced(8, 4);
        g.setColour(juce::Colour(0xFF0F1116));
        g.fillRoundedRectangle(innerBg.toFloat(), 6.0f);
        // Very subtle border
        g.setColour(juce::Colour(0xFF1A1D24));
        g.drawRoundedRectangle(innerBg.toFloat(), 6.0f, 1.0f);
    }

    // Frame rate labels
    g.setColour(textDim);
    g.setFont(juce::Font(juce::FontOptions(getMonoFontName(), 10.0f, juce::Font::bold)));

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
    // Adaptive panel width: 240px at 800w, scales up to 290px at wider windows
    int panelWidth = juce::jlimit(240, 290, 240 + (getWidth() - 800) / 8);
    int topBar = 0, bottomBar = 24;
    int tabBar = kTabBarHeight;

    // ===== TAB BAR (full window width -- the bar sits above the side panels) =====
    {
        int numTabs = (int)tabButtons.size();
        int addBtnW = 30;
        int tabGap = 4;
        int pad = 8;

        // Backup/Restore buttons on the left edge
        int bkBtnW = juce::jlimit(40, 60, (getWidth() - 400) / 10 + 40);
        btnBackup.setBounds(pad, topBar + 2, bkBtnW, tabBar - 4);
        btnRestore.setBounds(pad + bkBtnW + 2, topBar + 2, bkBtnW, tabBar - 4);
        int leftUsed = pad + bkBtnW * 2 + 2 + tabGap * 2;

        int availableW = getWidth() - leftUsed - pad;
        int addBtnTotal = tabGap + addBtnW;

        int maxTabW = 120;
        int tabW = maxTabW;
        if (numTabs > 0)
        {
            int spaceForTabs = availableW - addBtnTotal - juce::jmax(0, numTabs - 1) * tabGap;
            tabW = juce::jmin(maxTabW, spaceForTabs / numTabs);
            tabW = juce::jmax(50, tabW);
        }

        int totalTabsW = numTabs * tabW + juce::jmax(0, numTabs - 1) * tabGap + addBtnTotal;
        int tabX = leftUsed + (availableW - totalTabsW) / 2;
        tabX = juce::jmax(leftUsed, tabX);

        for (int i = 0; i < numTabs; i++)
        {
            tabButtons[(size_t)i]->setBounds(tabX, topBar + 2, tabW, tabBar - 4);
            tabX += tabW + tabGap;
        }
        btnAddEngine.setBounds(tabX, topBar + 2, addBtnW, tabBar - 4);
    }

    // ===== LEFT PANEL (scrollable viewport) =====
    auto leftPanelBounds = bounds.removeFromLeft(panelWidth);
    leftPanelBounds.removeFromTop(topBar + tabBar + 40);
    leftPanelBounds.removeFromBottom(bottomBar);
    leftViewport.setBounds(leftPanelBounds);

    int leftContentW = leftPanelBounds.getWidth();
    int leftScrollW = leftViewport.getScrollBarThickness();
    int leftUsableW = leftContentW - leftScrollW;
    auto leftPanel = juce::Rectangle<int>(12, 0, leftUsableW - 24, 10000);

    leftContent.clearSectionSeparators();

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
    IBI iBtns[] = { {&btnMtcIn,InputSource::MTC}, {&btnArtnetIn,InputSource::ArtNet}, {&btnSysTime,InputSource::SystemTime}, {&btnLtcIn,InputSource::LTC}, {&btnProDJLinkIn,InputSource::ProDJLink} };
    for (auto& ib : iBtns) { ib.btn->setBounds(leftPanel.removeFromTop(btnH)); leftPanel.removeFromTop(btnG); }

    // Section separator after input source buttons
    leftContent.addSectionSeparator(leftPanel.getY());
    leftPanel.removeFromTop(4);

    if (btnCollapseInput.isVisible())
    { leftPanel.removeFromTop(2); btnCollapseInput.setBounds(leftPanel.removeFromTop(18)); leftPanel.removeFromTop(4); }

    if (cmbMidiInputDevice.isVisible()) layCombo(lblMidiInputDevice, cmbMidiInputDevice, leftPanel);
    if (cmbArtnetInputInterface.isVisible()) layCombo(lblArtnetInputInterface, cmbArtnetInputInterface, leftPanel);
    if (cmbProDJLinkInterface.isVisible())
    {
        layCombo(lblProDJLinkInterface, cmbProDJLinkInterface, leftPanel);
        layCombo(lblProDJLinkPlayer, cmbProDJLinkPlayer, leftPanel);

        // BPM Multiplier row (5 equal buttons: 1x x2 x4 /2 /4)
        if (btnBpmOff.isVisible())
        {
            leftContent.addSectionSeparator(leftPanel.getY(), "BPM MULT");
            leftPanel.removeFromTop(16);
            auto bpmRow = leftPanel.removeFromTop(22);
            int gap = 3;
            int bw = (bpmRow.getWidth() - gap * 4) / 5;
            auto placBtn = [&](juce::TextButton& btn)
            {
                btn.setBounds(bpmRow.removeFromLeft(bw));
                bpmRow.removeFromLeft(gap);
            };
            placBtn(btnBpmD4);
            placBtn(btnBpmD2);
            placBtn(btnBpmOff);
            placBtn(btnBpmX2);
            btnBpmX4.setBounds(bpmRow);  // last button takes remaining width
            leftPanel.removeFromTop(3);
            updateBpmMultButtonStates();
        }

        if (artworkDisplay.isVisible())
        {
            // Side-by-side: artwork (56x56) | track info + metadata stacked
            auto artRow = leftPanel.removeFromTop(56);
            artworkDisplay.setBounds(artRow.removeFromLeft(56));
            artRow.removeFromLeft(6);
            // Stack the two text labels in the remaining space
            auto textArea = artRow;
            lblProDJLinkTrackInfo.setBounds(textArea.removeFromTop(20));
            textArea.removeFromTop(2);
            lblProDJLinkMetadata.setBounds(textArea.removeFromTop(20));
            leftPanel.removeFromTop(3);
        }
        else
        {
            if (lblProDJLinkTrackInfo.isVisible())
            { lblProDJLinkTrackInfo.setBounds(leftPanel.removeFromTop(14)); leftPanel.removeFromTop(1); }
            if (lblProDJLinkMetadata.isVisible())
            { lblProDJLinkMetadata.setBounds(leftPanel.removeFromTop(14)); leftPanel.removeFromTop(3); }
        }

        // --- Waveform display ---
        if (waveformDisplay.isVisible())
        {
            auto wfBounds = leftPanel.removeFromTop(48);
            waveformDisplay.setBounds(wfBounds);
            leftPanel.removeFromTop(3);
        }

        // --- Mixer fader status ---
        if (lblMixerStatus.isVisible())
        {
            lblMixerStatus.setBounds(leftPanel.removeFromTop(13));
            leftPanel.removeFromTop(3);
        }

        // --- TrackMap ---
        // --- Action buttons + TrackMap toggle ---
        if (btnTrackMap.isVisible())
        {
            leftContent.addSectionSeparator(leftPanel.getY());
            leftPanel.removeFromTop(4);

            // Row 1: Action buttons (3 equal columns)
            auto btnRow = leftPanel.removeFromTop(22);
            int thirdW = (btnRow.getWidth() - 8) / 3;
            btnTrackMapEdit.setBounds(btnRow.removeFromLeft(thirdW));
            btnRow.removeFromLeft(4);
            btnMixerMapEdit.setBounds(btnRow.removeFromLeft(thirdW));
            btnRow.removeFromLeft(4);
            btnProDJLinkView.setBounds(btnRow);
            leftPanel.removeFromTop(3);

            // Row 2: TrackMap toggle (full width)
            btnTrackMap.setBounds(leftPanel.removeFromTop(22));
            leftPanel.removeFromTop(3);
        }

        // --- MIDI section ---
        if (cmbTriggerMidiDevice.isVisible() || btnMidiClock.isVisible() || btnTriggerMidi.isVisible())
        {
            leftContent.addSectionSeparator(leftPanel.getY(), "MIDI");
            leftPanel.removeFromTop(16);
        }
        if (btnMidiClock.isVisible())
        {
            btnMidiClock.setBounds(leftPanel.removeFromTop(22));
            leftPanel.removeFromTop(3);
        }
        if (btnMidiMixerFwd.isVisible())
        {
            btnMidiMixerFwd.setBounds(leftPanel.removeFromTop(22));
            leftPanel.removeFromTop(3);
            if (cmbMidiMixCCCh.isVisible())
            {
                auto chRow = leftPanel.removeFromTop(22);
                int halfW = (chRow.getWidth() - 4) / 2;
                lblMidiMixCCCh.setBounds(chRow.removeFromLeft(42));
                cmbMidiMixCCCh.setBounds(chRow.removeFromLeft(halfW - 42));
                chRow.removeFromLeft(4);
                lblMidiMixNoteCh.setBounds(chRow.removeFromLeft(52));
                cmbMidiMixNoteCh.setBounds(chRow);
                leftPanel.removeFromTop(3);
            }
        }
        if (btnTriggerMidi.isVisible())
        {
            btnTriggerMidi.setBounds(leftPanel.removeFromTop(22));
            leftPanel.removeFromTop(3);
        }
        if (cmbTriggerMidiDevice.isVisible())
        {
            cmbTriggerMidiDevice.setBounds(leftPanel.removeFromTop(22));
            leftPanel.removeFromTop(3);
        }

        // --- OSC section ---
        if (btnOscFwdBpm.isVisible() || btnTriggerOsc.isVisible())
        {
            leftContent.addSectionSeparator(leftPanel.getY(), "OSC");
            leftPanel.removeFromTop(16);
        }
        if (btnOscFwdBpm.isVisible())
        {
            btnOscFwdBpm.setBounds(leftPanel.removeFromTop(22));
            if (edOscFwdBpmAddr.isVisible())
            {
                leftPanel.removeFromTop(2);
                auto bpmRow = leftPanel.removeFromTop(22);
                lblOscFwdBpmAddr.setBounds(bpmRow.removeFromLeft(40));
                bpmRow.removeFromLeft(2);
                edOscFwdBpmAddr.setBounds(bpmRow);
            }
            leftPanel.removeFromTop(3);
        }
        if (btnOscMixerFwd.isVisible())
        {
            btnOscMixerFwd.setBounds(leftPanel.removeFromTop(22));
            leftPanel.removeFromTop(3);
        }
        if (btnTriggerOsc.isVisible())
        {
            btnTriggerOsc.setBounds(leftPanel.removeFromTop(22));
            leftPanel.removeFromTop(3);
        }
        if (edOscIp.isVisible())
        {
            auto oscConfRow = leftPanel.removeFromTop(22);
            edOscIp.setBounds(oscConfRow.removeFromLeft(oscConfRow.getWidth() - 58));
            oscConfRow.removeFromLeft(4);
            edOscPort.setBounds(oscConfRow);
            leftPanel.removeFromTop(3);
        }

        // --- ART-NET DMX section ---
        if (btnArtnetMixerFwd.isVisible() || btnArtnetTrigger.isVisible())
        {
            leftContent.addSectionSeparator(leftPanel.getY(), "ART-NET DMX");
            leftPanel.removeFromTop(16);
        }
        if (btnArtnetMixerFwd.isVisible())
        {
            btnArtnetMixerFwd.setBounds(leftPanel.removeFromTop(22));
            leftPanel.removeFromTop(3);
            if (cmbArtMixNet.isVisible())
            {
                auto addrRow = leftPanel.removeFromTop(22);
                lblArtMixAddr.setBounds(addrRow.removeFromLeft(48));
                addrRow.removeFromLeft(2);
                int cmbW = (addrRow.getWidth() - 4) / 3;
                cmbArtMixNet.setBounds(addrRow.removeFromLeft(cmbW));
                addrRow.removeFromLeft(2);
                cmbArtMixSub.setBounds(addrRow.removeFromLeft(cmbW));
                addrRow.removeFromLeft(2);
                cmbArtMixUni.setBounds(addrRow);
                leftPanel.removeFromTop(3);
            }
        }
        if (btnArtnetTrigger.isVisible())
        {
            btnArtnetTrigger.setBounds(leftPanel.removeFromTop(22));
            leftPanel.removeFromTop(3);
            if (cmbArtTrigNet.isVisible())
            {
                auto addrRow = leftPanel.removeFromTop(22);
                lblArtTrigAddr.setBounds(addrRow.removeFromLeft(48));
                addrRow.removeFromLeft(2);
                int cmbW = (addrRow.getWidth() - 4) / 3;
                cmbArtTrigNet.setBounds(addrRow.removeFromLeft(cmbW));
                addrRow.removeFromLeft(2);
                cmbArtTrigSub.setBounds(addrRow.removeFromLeft(cmbW));
                addrRow.removeFromLeft(2);
                cmbArtTrigUni.setBounds(addrRow);
                leftPanel.removeFromTop(3);
            }
        }
        if (cmbArtnetDmxInterface.isVisible())
            layCombo(lblArtnetDmxInterface, cmbArtnetDmxInterface, leftPanel);

        // --- Ableton Link ---
        if (btnLink.isVisible())
        {
            leftContent.addSectionSeparator(leftPanel.getY(), "ABLETON LINK");
            leftPanel.removeFromTop(16);
            btnLink.setBounds(leftPanel.removeFromTop(22));
            if (lblLinkStatus.isVisible())
            {
                leftPanel.removeFromTop(2);
                lblLinkStatus.setBounds(leftPanel.removeFromTop(13));
            }
            leftPanel.removeFromTop(3);
        }
    }

    if (cmbAudioInputDevice.isVisible())
    {
        // Section separator before audio settings
        leftContent.addSectionSeparator(leftPanel.getY(), "AUDIO SETTINGS");
        leftPanel.removeFromTop(16);

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

    // Set content height for left panel scrolling
    int leftUsedHeight = leftPanel.getY() + 8;
    leftContent.setSize(leftContentW, juce::jmax(leftPanelBounds.getHeight(), leftUsedHeight));

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

    rightContent.clearSectionSeparators();

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

    rightContent.addSectionSeparator(rp.getY());
    rp.removeFromTop(4);

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
    {
        rightContent.addSectionSeparator(rp.getY());
        rp.removeFromTop(4);
        layCombo(lblAudioOutputTypeFilter, cmbAudioOutputTypeFilter, rp);
    }
    else
    {
        rightContent.addSectionSeparator(rp.getY());
        rp.removeFromTop(4);
    }

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
        rightContent.addSectionSeparator(rp.getY());
        rp.removeFromTop(4);

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
    btnCheckUpdates.setBounds(getWidth() - 145, getHeight() - bottomBar, 140, bottomBar);

    // Update notification -- replaces check button in bottom bar
    if (btnUpdateAvailable.isVisible())
        btnUpdateAvailable.setBounds(getWidth() - 245, getHeight() - bottomBar, 240, bottomBar);
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
    // target timecode -- it does NOT limit output precision.  LTC output
    // uses its own audio-callback-driven auto-increment, so it's similarly
    // decoupled.  If UI stalls (resize, modal dialog), outputs continue
    // transmitting the last-known timecode until the next tick() updates it.
    for (int i = 0; i < (int)engines.size(); ++i)
    {
        engines[(size_t)i]->setStatusTextVisible(i == selectedEngine);
        engines[(size_t)i]->tick();
    }

    // Update UI for selected engine
    auto& eng = currentEngine();

    updateStatusLabels();

    if (eng.getActiveInput() == InputSource::ProDJLink && sharedProDJLinkInput.isReceiving())
    {
        int pdlPlayer = eng.getEffectivePlayer();

        // Guard: in XF mode, resolved player can be 0 (no player on this side)
        if (pdlPlayer < 1)
        {
            lblProDJLinkTrackInfo.setText("", juce::dontSendNotification);
            lblProDJLinkMetadata.setText("", juce::dontSendNotification);
            lblMixerStatus.setText("", juce::dontSendNotification);
        }
        else
        {

        // Phase 2: use engine's enriched track info (resolves "Track #12345" via dbClient)
        auto trackInfo = eng.getActiveTrackInfo();
        juce::String pdlTrackStr;
        if (trackInfo.artist.isNotEmpty() && trackInfo.title.isNotEmpty())
            pdlTrackStr = trackInfo.artist + " " + juce::String::charToString(0x2014) + " " + trackInfo.title;
        else if (trackInfo.title.isNotEmpty())
            pdlTrackStr = trackInfo.title;
        if (trackInfo.key.isNotEmpty())
            pdlTrackStr += "  [" + trackInfo.key + "]";
        lblProDJLinkTrackInfo.setText(pdlTrackStr, juce::dontSendNotification);
        lblProDJLinkTrackInfo.setColour(juce::Label::textColourId,
            pdlTrackStr.isNotEmpty() ? juce::Colour(0xFF00AAFF).brighter(0.5f) : textDim);

        double pdlBpm = sharedProDJLinkInput.getBPM(pdlPlayer);
        juce::String pdlMeta;
        if (pdlBpm > 0.0)
        {
            pdlMeta += juce::String(pdlBpm, 1) + " BPM";

            // Show effective (multiplied) BPM if multiplier is active
            int effMult = eng.getEffectiveBpmMultiplier();
            if (effMult != 0)
            {
                double multBpm = TimecodeEngine::applyBpmMultiplier(pdlBpm, effMult);
                juce::String multLabel;
                switch (effMult) {
                    case  1: multLabel = "x2"; break;
                    case  2: multLabel = "x4"; break;
                    case -1: multLabel = "/2"; break;
                    case -2: multLabel = "/4"; break;
                    default: break;
                }
                pdlMeta += "  " + juce::String::charToString(0x2192) + " "
                         + juce::String(multBpm, 1) + " (" + multLabel + ")";
            }
        }
        double pdlPitch = sharedProDJLinkInput.getActualSpeed(pdlPlayer);
        if (pdlPitch > 0.01)
        {
            double pitchPct = (pdlPitch - 1.0) * 100.0;
            pdlMeta += "  " + (pitchPct >= 0.0 ? juce::String("+") : juce::String(""))
                            + juce::String(pitchPct, 2) + "%";
        }
        juce::String pdlModel = sharedProDJLinkInput.getPlayerModel(pdlPlayer);
        if (pdlModel.isNotEmpty())
            pdlMeta += "  | " + pdlModel;

        lblProDJLinkMetadata.setText(pdlMeta, juce::dontSendNotification);
        lblProDJLinkMetadata.setColour(juce::Label::textColourId, juce::Colour(0xFF00AAFF).brighter(0.3f));

        // Mixer fader status line
        {
            // Pre-computed fader bar strings (avoids per-call charToString allocations)
            static const juce::String kBar3[4] = {
                juce::String::charToString(0x2591) + juce::String::charToString(0x2591) + juce::String::charToString(0x2591),
                juce::String::charToString(0x2588) + juce::String::charToString(0x2591) + juce::String::charToString(0x2591),
                juce::String::charToString(0x2588) + juce::String::charToString(0x2588) + juce::String::charToString(0x2591),
                juce::String::charToString(0x2588) + juce::String::charToString(0x2588) + juce::String::charToString(0x2588)
            };
            static const juce::String kBar2[3] = {
                juce::String::charToString(0x2591) + juce::String::charToString(0x2591),
                juce::String::charToString(0x2588) + juce::String::charToString(0x2591),
                juce::String::charToString(0x2588) + juce::String::charToString(0x2588)
            };
            static const juce::String kCrossSymbol = juce::String::charToString(0x2715);

            auto faderBar = [](uint8_t val, int bars) -> const juce::String&
            {
                int filled = (int)std::round((val / 255.0f) * bars);
                return (bars <= 2) ? kBar2[juce::jlimit(0, 2, filled)]
                                   : kBar3[juce::jlimit(0, 3, filled)];
            };
            if (sharedProDJLinkInput.hasMixerFaderData())
            {
                juce::String djmModel = sharedProDJLinkInput.getDJMModel();
                if (djmModel.isEmpty()) djmModel = "DJM";
                int numCh = sharedProDJLinkInput.getMixerChannelCount();
                // Cap model name and adjust bar width to fit panel
                if (djmModel.length() > 7) djmModel = djmModel.substring(0, 7);
                int bars = (numCh > 4) ? 2 : 3;  // narrower bars for 6-channel DJMs
                auto& pdl = sharedProDJLinkInput;
                juce::String mixStr = djmModel;
                for (int ch = 1; ch <= numCh; ++ch)
                    mixStr += " " + juce::String(ch) + ":" + faderBar(pdl.getChannelFader(ch), bars);
                mixStr += " " + kCrossSymbol + ":" + faderBar(pdl.getCrossfader(), bars)
                        + " M:" + faderBar(pdl.getMasterFader(), bars);
                lblMixerStatus.setText(mixStr, juce::dontSendNotification);
                lblMixerStatus.setColour(juce::Label::textColourId,
                    juce::Colour(0xFF00AAFF).withAlpha(0.7f));
            }
            else
            {
                lblMixerStatus.setText("DJM: waiting for bridge data...",
                    juce::dontSendNotification);
                lblMixerStatus.setColour(juce::Label::textColourId, textDim);
            }
        }

        // Phase 2c: update artwork from DbServerClient cache
        uint32_t artId = trackInfo.artworkId;
        if (artId != 0 && artId != displayedArtworkId)
        {
            auto artImg = sharedDbClient.getCachedArtwork(artId);
            if (artImg.isValid())
            {
                artworkDisplay.setImage(artImg);
                displayedArtworkId = artId;
            }
        }
        else if (artId == 0 && displayedArtworkId != 0)
        {
            artworkDisplay.clearImage();
            displayedArtworkId = 0;
        }

        // Phase 3: update color waveform from DbServerClient cache
        uint32_t wfTrackId = trackInfo.trackId;
        if (wfTrackId != 0 && wfTrackId != displayedWaveformTrackId)
        {
            // Track changed -- clear old waveform immediately (avoids stale cursor)
            waveformDisplay.clearWaveform();
            // Mark this track as "attempted" so we don't re-enter this block
            // every frame.  The retry path below uses hasWaveformData() to
            // detect when the async waveform query completes.
            displayedWaveformTrackId = wfTrackId;

            // Try to populate from cache (may not have waveform yet)
            juce::String pdlIP = sharedProDJLinkInput.getPlayerIP(pdlPlayer);
            auto meta = sharedDbClient.getCachedMetadata(pdlIP, wfTrackId);
            if (meta.hasWaveform())
            {
                waveformDisplay.setColorWaveformData(meta.waveformData,
                    meta.waveformEntryCount, meta.waveformBytesPerEntry);
            }
        }
        else if (wfTrackId != 0 && !waveformDisplay.hasWaveformData())
        {
            // Waveform not yet loaded -- retry from cache (async: arrives after metadata)
            juce::String pdlIP = sharedProDJLinkInput.getPlayerIP(pdlPlayer);
            auto meta = sharedDbClient.getCachedMetadata(pdlIP, wfTrackId);
            if (meta.hasWaveform())
            {
                waveformDisplay.setColorWaveformData(meta.waveformData,
                    meta.waveformEntryCount, meta.waveformBytesPerEntry);
            }
        }
        else if (wfTrackId == 0 && displayedWaveformTrackId != 0)
        {
            waveformDisplay.clearWaveform();
            displayedWaveformTrackId = 0;
        }

        // Update waveform cursor position from PLL-smoothed playhead.
        // Using the engine's PLL output instead of raw CDJ packets avoids
        // visible cursor jitter, especially on macOS where timer scheduling
        // has more variance than Windows.
        if (waveformDisplay.hasWaveformData())
        {
            float posRatio = eng.getSmoothedPlayPositionRatio();
            waveformDisplay.setPlayPosition(posRatio);
        }

        // Keep BPM multiplier buttons in sync (track load -> cached multiplier changes)
        updateBpmMultButtonStates();
        } // end pdlPlayer >= 1
    }
    else if (eng.getActiveInput() != InputSource::ProDJLink)
    {
        lblProDJLinkTrackInfo.setText("", juce::dontSendNotification);
        lblProDJLinkMetadata.setText("", juce::dontSendNotification);
        lblMixerStatus.setText("", juce::dontSendNotification);
        if (displayedArtworkId != 0)
        {
            artworkDisplay.clearImage();
            displayedArtworkId = 0;
        }
        if (displayedWaveformTrackId != 0)
        {
            waveformDisplay.clearWaveform();
            displayedWaveformTrackId = 0;
        }
    }

    // Ableton Link status label
    if (eng.getLinkBridge().isEnabled() && lblLinkStatus.isVisible())
    {
        int peers = eng.getLinkBridge().getNumPeers();
        double linkBpm = eng.getLinkBridge().getTempo();
        juce::String linkText = juce::String(linkBpm, 1) + " BPM";
        if (peers > 0)
            linkText += " | " + juce::String(peers) + (peers == 1 ? " peer" : " peers");
        else
            linkText += " | no peers";
        lblLinkStatus.setText(linkText, juce::dontSendNotification);
        lblLinkStatus.setColour(juce::Label::textColourId,
            peers > 0 ? juce::Colour(0xFF00D084) : juce::Colour(0xFFFF9800));
    }
    else
    {
        lblLinkStatus.setText("", juce::dontSendNotification);
    }

    // Update TrackMap editor window if open
    if (trackMapWindow != nullptr)
    {
        if (auto* editor = dynamic_cast<TrackMapEditor*>(trackMapWindow->getContentComponent()))
        {
            auto ati = eng.getActiveTrackInfo();
            editor->setActiveTrack(ati.artist, ati.title);

            if (eng.consumeTrackMapAutoFilled())
            {
                editor->refresh();
                updateBpmMultButtonStates();  // TrackMap entry may have a bpmMultiplier
            }
        }
    }

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

    // Repaint mini strip so non-selected engine timecodes update live.
    // Skip if no non-selected engine is running (nothing to animate).
    if (engines.size() > 1 && !miniStripArea.isEmpty())
    {
        bool anyOtherActive = false;
        for (int i = 0; i < (int)engines.size(); ++i)
        {
            if (i != selectedEngine && engines[(size_t)i]->isSourceActive())
                { anyOtherActive = true; break; }
        }
        if (anyOtherActive)
            repaint(miniStripArea);
    }

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
            btnUpdateAvailable.setBounds(getWidth() - 245, getHeight() - 24, 240, 24);
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
// KEYBOARD SHORTCUTS
//==============================================================================
bool MainComponent::keyPressed(const juce::KeyPress& key)
{
    // Ctrl+Shift+E (Cmd+Shift+E on Mac): export full configuration backup
    if (key.getModifiers().isCommandDown() && key.getModifiers().isShiftDown()
        && key.getKeyCode() == 'E')
    {
        exportConfig();
        return true;
    }

    // Ctrl+Shift+I (Cmd+Shift+I on Mac): import full configuration backup
    if (key.getModifiers().isCommandDown() && key.getModifiers().isShiftDown()
        && key.getKeyCode() == 'I')
    {
        importConfig();
        return true;
    }

    return false;
}

//==============================================================================
// BUTTON STATE UPDATES
//==============================================================================
void MainComponent::updateBpmMultButtonStates()
{
    auto& eng = currentEngine();
    int eff  = eng.getEffectiveBpmMultiplier();   // 0=1x, 1=x2, 2=x4, -1=/2, -2=/4

    // Get TrackMap value directly for secondary indicator
    auto trackInfo = eng.getActiveTrackInfo();
    int map = 0;
    if (trackInfo.artist.isNotEmpty() && trackInfo.title.isNotEmpty())
    {
        auto* entry = settings.trackMap.find(trackInfo.artist, trackInfo.title);
        if (entry != nullptr) map = entry->bpmMultiplier;
    }

    // Accent colours
    auto lit    = juce::Colour(0xFF00AAFF);   // pdlAccent (active)
    auto amber  = juce::Colour(0xFFFFAB00);   // saved TrackMap indicator
    auto dim    = bgPanel;

    // Helper: style a button with visual distinction for session vs TrackMap.
    // Sets both textColourOffId and textColourOnId (hover) to the same value
    // so the custom L&F doesn't override gold TrackMap text with blue on hover.
    auto styleBtn = [&](juce::TextButton& btn, int mult)
    {
        bool active  = (eff == mult);
        bool isSaved = (map != 0 && map == mult);  // has TrackMap value on this button

        juce::Colour bgCol = active ? lit.withAlpha(0.30f) : dim;
        btn.setColour(juce::TextButton::buttonColourId, bgCol);
        btn.setColour(juce::TextButton::buttonOnColourId, bgCol.brighter(0.15f));  // subtle hover bg

        // Golden text always wins for saved TrackMap value -- both normal and hover
        juce::Colour txtCol = isSaved ? amber
                            : (active ? lit.brighter(0.3f) : textMid);
        btn.setColour(juce::TextButton::textColourOffId, txtCol);
        btn.setColour(juce::TextButton::textColourOnId,  txtCol);
    };

    styleBtn(btnBpmOff,  0);
    styleBtn(btnBpmX2,   1);
    styleBtn(btnBpmX4,   2);
    styleBtn(btnBpmD2,  -1);
    styleBtn(btnBpmD4,  -2);
}

//==============================================================================
// Save BPM multiplier to TrackMap (double-click from engine panel buttons).
// Double-click on 1x clears saved value. Other values save without toggle.
// After saving, session override is cleared (TrackMap = source of truth).
//==============================================================================
void MainComponent::saveBpmMultToTrackMap(int clickedMult)
{
    auto& eng = currentEngine();
    auto info = eng.getActiveTrackInfo();
    if (info.artist.isEmpty() || info.title.isEmpty()) return;

    auto* entry = settings.trackMap.find(info.artist, info.title);
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
        // Create a new TrackMap entry with basic info + bpmMultiplier
        TrackMapEntry newEntry;
        newEntry.artist  = info.artist;
        newEntry.title   = info.title;
        newEntry.bpmMultiplier = newValue;
        settings.trackMap.addOrUpdate(newEntry);
    }
    else
    {
        return;  // no entry exists and clearing to 0 = nothing to do
    }

    // Clear session override; TrackMap is now the source of truth
    eng.setBpmPlayerOverride(TimecodeEngine::kBpmNoOverride);
    eng.setCachedBpmMultiplier(newValue);

    // Persist and refresh all engines (other engines on same track benefit too)
    settings.trackMap.save();
    for (auto& e : engines)
        e->refreshTrackMapLookup();
    updateBpmMultButtonStates();

    // Refresh TrackMapEditor if open (entry may have been created/modified)
    if (trackMapWindow != nullptr)
    {
        if (auto* editor = dynamic_cast<TrackMapEditor*>(trackMapWindow->getContentComponent()))
            editor->refresh();
    }
}

void MainComponent::updateInputButtonStates()
{
    auto& eng = currentEngine();
    auto active = eng.getActiveInput();
    struct I { juce::TextButton* b; InputSource s; };
    I bs[] = { {&btnMtcIn,InputSource::MTC}, {&btnArtnetIn,InputSource::ArtNet}, {&btnSysTime,InputSource::SystemTime}, {&btnLtcIn,InputSource::LTC}, {&btnProDJLinkIn,InputSource::ProDJLink} };
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
        case InputSource::ProDJLink:  return juce::Colour(0xFF00AAFF);  // bright blue
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

void MainComponent::setupArtNetAddressCombos(juce::ComboBox& cmbNet, juce::ComboBox& cmbSub,
                                              juce::ComboBox& cmbUni, juce::Label& lbl,
                                              const juce::String& labelText,
                                              std::function<void()> onChangeFunc)
{
    leftContent.addAndMakeVisible(lbl);
    lbl.setVisible(false);
    lbl.setText(labelText, juce::dontSendNotification);
    lbl.setFont(juce::Font(juce::FontOptions(10.0f)));
    lbl.setColour(juce::Label::textColourId, textDim);

    for (auto* cmb : { &cmbNet, &cmbSub, &cmbUni })
    {
        leftContent.addAndMakeVisible(*cmb);
        cmb->setVisible(false);
        styleComboBox(*cmb);
    }

    for (int n = 0; n < 128; ++n) cmbNet.addItem("N:" + juce::String(n), n + 1);
    for (int s = 0; s < 16; ++s)  cmbSub.addItem("S:" + juce::String(s), s + 1);
    for (int u = 0; u < 16; ++u)  cmbUni.addItem("U:" + juce::String(u), u + 1);

    cmbNet.setTooltip("Net (0-127)");
    cmbSub.setTooltip("Subnet (0-15)");
    cmbUni.setTooltip("Universe (0-15)");

    cmbNet.setSelectedId(1, juce::dontSendNotification);
    cmbSub.setSelectedId(1, juce::dontSendNotification);
    cmbUni.setSelectedId(1, juce::dontSendNotification);

    auto cb = [onChangeFunc]() { if (onChangeFunc) onChangeFunc(); };
    cmbNet.onChange = cb;
    cmbSub.onChange = cb;
    cmbUni.onChange = cb;
}

void MainComponent::setArtNetCombosFromAddress(juce::ComboBox& cmbNet, juce::ComboBox& cmbSub,
                                                juce::ComboBox& cmbUni, int portAddress)
{
    int net, sub, uni;
    unpackArtNetAddress(portAddress, net, sub, uni);
    cmbNet.setSelectedId(net + 1, juce::dontSendNotification);
    cmbSub.setSelectedId(sub + 1, juce::dontSendNotification);
    cmbUni.setSelectedId(uni + 1, juce::dontSendNotification);
}

int MainComponent::getArtNetAddressFromCombos(const juce::ComboBox& cmbNet, const juce::ComboBox& cmbSub,
                                               const juce::ComboBox& cmbUni)
{
    return packArtNetAddress(cmbNet.getSelectedId() - 1,
                             cmbSub.getSelectedId() - 1,
                             cmbUni.getSelectedId() - 1);
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
