// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

#include "MainComponent.h"
#include "WaveformCache.h"

using SrcType = TimecodeEngine::InputSource;

//==============================================================================
// Remove " *" or " [Engine N]" markers from combo item text to get the
// underlying device name.  Used when saving device names to settings.
//==============================================================================
static juce::String stripComboMarker(const juce::String& text)
{
    static const juce::String dotChar = juce::String::charToString(0x25CF);
    static const juce::String dotWithSpace = juce::String(" ") + dotChar;
    auto s = text;
    if (s.endsWith(dotWithSpace))
        s = s.dropLastCharacters(dotWithSpace.length());
    else if (s.endsWith(dotChar))
        s = s.dropLastCharacters(dotChar.length());
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
    engines[0]->setSharedStageLinQInput(&sharedStageLinQInput);
    sharedStageLinQInput.onMetadataRequest = [this](const juce::String& path)
    {
        sharedStageLinQDb.requestMetadata(path);
    };
    sharedStageLinQInput.onFileTransferAvailable = [this](const juce::String& ip, uint16_t port, const uint8_t* token)
    {
        sharedStageLinQDb.start(ip, port, token);
    };
    sharedProDJLinkInput.onPlayerLost = [this](const juce::String& playerIP)
    {
        sharedDbClient.invalidatePlayer(playerIP);
    };
    engines[0]->setDbServerClient(&sharedDbClient);
    engines[0]->setTrackMap(&settings.trackMap);
    engines[0]->setMixerMap(&sharedMixerMap);
    engines[0]->setSlqMixerMap(&sharedSlqMixerMap);

    setSize(900, 700);
    setWantsKeyboardFocus(true);  // enable Ctrl+D diagnostic shortcut

    // --- Tab bar ---
    addAndMakeVisible(btnAddEngine);
    btnAddEngine.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF1A1D23));
    btnAddEngine.setColour(juce::TextButton::textColourOffId, accentBlue);
    btnAddEngine.onClick = [this] { if (!isShowLocked()) addEngine(); };
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
    for (auto* btn : { &btnMtcIn, &btnArtnetIn, &btnSysTime, &btnLtcIn, &btnProDJLinkIn, &btnStageLinQIn })
    { leftContent.addAndMakeVisible(btn); btn->setClickingTogglesState(false); }
    // HippoNet input hidden pending hardware validation (code preserved in HippotizerInput.h)
    btnHippoIn.setVisible(false);

    btnMtcIn.onClick = [this] {
        if (syncing || isShowLocked()) return;
        auto& eng = currentEngine();
        if (eng.getActiveInput() == SrcType::MTC) { inputConfigExpanded = !inputConfigExpanded; updateDeviceSelectorVisibility(); resized(); }
        else { inputConfigExpanded = true; eng.setInputSource(SrcType::MTC); startCurrentMtcInput(); updateInputButtonStates(); updateDeviceSelectorVisibility(); resized(); saveSettings(); }
    };
    btnArtnetIn.onClick = [this] {
        if (syncing || isShowLocked()) return;
        auto& eng = currentEngine();
        if (eng.getActiveInput() == SrcType::ArtNet) { inputConfigExpanded = !inputConfigExpanded; updateDeviceSelectorVisibility(); resized(); }
        else { inputConfigExpanded = true; eng.setInputSource(SrcType::ArtNet); startCurrentArtnetInput(); updateInputButtonStates(); updateDeviceSelectorVisibility(); resized(); saveSettings(); }
    };
    btnSysTime.onClick = [this] {
        if (syncing || isShowLocked()) return;
        auto& eng = currentEngine();
        if (eng.getActiveInput() == SrcType::SystemTime) { inputConfigExpanded = !inputConfigExpanded; updateDeviceSelectorVisibility(); resized(); }
        else { inputConfigExpanded = true; eng.setInputSource(SrcType::SystemTime); updateInputButtonStates(); updateDeviceSelectorVisibility(); resized(); saveSettings(); }
    };
    btnLtcIn.onClick = [this] {
        if (syncing || isShowLocked()) return;
        auto& eng = currentEngine();
        if (eng.getActiveInput() == SrcType::LTC) { inputConfigExpanded = !inputConfigExpanded; updateDeviceSelectorVisibility(); resized(); }
        else { inputConfigExpanded = true; eng.setInputSource(SrcType::LTC); if (!scannedAudioInputs.isEmpty()) startCurrentLtcInput(); updateInputButtonStates(); updateDeviceSelectorVisibility(); resized(); saveSettings(); }
    };

    btnProDJLinkIn.onClick = [this] {
        if (syncing || isShowLocked()) return;
        auto& eng = currentEngine();
        if (eng.getActiveInput() == SrcType::ProDJLink) { inputConfigExpanded = !inputConfigExpanded; updateDeviceSelectorVisibility(); resized(); }
        else { inputConfigExpanded = true; eng.setInputSource(SrcType::ProDJLink); startCurrentProDJLinkInput(); updateInputButtonStates(); updateDeviceSelectorVisibility(); resized(); saveSettings(); }
    };

    btnStageLinQIn.onClick = [this] {
        if (syncing || isShowLocked()) return;
        auto& eng = currentEngine();
        if (eng.getActiveInput() == SrcType::StageLinQ) { inputConfigExpanded = !inputConfigExpanded; updateDeviceSelectorVisibility(); resized(); }
        else { inputConfigExpanded = true; eng.setInputSource(SrcType::StageLinQ); startCurrentStageLinQInput(); updateInputButtonStates(); updateDeviceSelectorVisibility(); resized(); saveSettings(); }
    };

    btnHippoIn.onClick = [this] {
        if (syncing || isShowLocked()) return;
        auto& eng = currentEngine();
        if (eng.getActiveInput() == SrcType::Hippotizer) { inputConfigExpanded = !inputConfigExpanded; updateDeviceSelectorVisibility(); resized(); }
        else { inputConfigExpanded = true; eng.setInputSource(SrcType::Hippotizer); startCurrentHippotizerInput(); updateInputButtonStates(); updateDeviceSelectorVisibility(); resized(); saveSettings(); }
    };

    // --- Output toggles ---
    for (auto* btn : { &btnMtcOut, &btnArtnetOut, &btnLtcOut, &btnThruOut, &btnTcnetOut })
        rightContent.addAndMakeVisible(btn);

    styleOutputToggle(btnMtcOut, accentRed);
    styleOutputToggle(btnArtnetOut, accentOrange);
    styleOutputToggle(btnLtcOut, accentPurple);
    styleOutputToggle(btnThruOut, accentCyan);
    styleOutputToggle(btnTcnetOut, juce::Colour(0xFF00CC66));

    auto outputToggleHandler = [this]
    {
        if (syncing) return;
        if (isShowLockedRevert()) return;
        auto& eng = currentEngine();
        eng.setOutputMtcEnabled(btnMtcOut.getToggleState());
        eng.setOutputArtnetEnabled(btnArtnetOut.getToggleState());
        eng.setOutputLtcEnabled(btnLtcOut.getToggleState());
        eng.setOutputThruEnabled(btnThruOut.getToggleState());
        eng.setOutputTcnetEnabled(btnTcnetOut.getToggleState());

        // Auto-start/stop shared TCNet output based on any engine needing it
        bool anyTcnet = false;
        for (auto& e : engines)
            if (e->isOutputTcnetEnabled()) { anyTcnet = true; break; }
        if (anyTcnet && !sharedTcnetOutput.getIsRunning())
        {
            sharedTcnetOutput.refreshNetworkInterfaces();
            sharedTcnetOutput.start(settings.tcnetInterface);
        }
        else if (!anyTcnet && sharedTcnetOutput.getIsRunning())
        {
            sharedTcnetOutput.stop();
        }

        updateCurrentOutputStates();
        updateDeviceSelectorVisibility();
        resized();
        saveSettings();
    };
    btnMtcOut.onClick = btnArtnetOut.onClick = btnLtcOut.onClick = btnThruOut.onClick = btnTcnetOut.onClick = outputToggleHandler;

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
            resized();
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
        resized();
    };
    updateCollapseButtonText(btnCollapseInput, inputConfigExpanded);

    // --- FPS buttons ---
    for (auto* btn : { &btnFps2398, &btnFps24, &btnFps25, &btnFps2997, &btnFps30 })
    { addAndMakeVisible(btn); btn->setClickingTogglesState(false); }

    btnFps2398.onClick = [this] {
        if (syncing || isShowLocked()) return;
        auto& eng = currentEngine();
        if (eng.getActiveInput() == SrcType::LTC) eng.setUserOverrodeLtcFps(true);
        eng.setFrameRate(FrameRate::FPS_2398); updateFpsButtonStates(); saveSettings();
    };
    btnFps24.onClick = [this] {
        if (syncing || isShowLocked()) return;
        currentEngine().setUserOverrodeLtcFps(false);
        currentEngine().setFrameRate(FrameRate::FPS_24); updateFpsButtonStates(); saveSettings();
    };
    btnFps25.onClick = [this] {
        if (syncing || isShowLocked()) return;
        currentEngine().setUserOverrodeLtcFps(false);
        currentEngine().setFrameRate(FrameRate::FPS_25); updateFpsButtonStates(); saveSettings();
    };
    btnFps2997.onClick = [this] {
        if (syncing || isShowLocked()) return;
        auto& eng = currentEngine();
        if (eng.getActiveInput() == SrcType::LTC) eng.setUserOverrodeLtcFps(true);
        eng.setFrameRate(FrameRate::FPS_2997); updateFpsButtonStates(); saveSettings();
    };
    btnFps30.onClick = [this] {
        if (syncing || isShowLocked()) return;
        currentEngine().setUserOverrodeLtcFps(false);
        currentEngine().setFrameRate(FrameRate::FPS_30); updateFpsButtonStates(); saveSettings();
    };

    // --- FPS Conversion ---
    addAndMakeVisible(btnFpsConvert);
    styleOutputToggle(btnFpsConvert, accentGreen);
    btnFpsConvert.onClick = [this]
    {
        if (syncing) return;
        if (isShowLockedToggle(btnFpsConvert)) return;
        auto& eng = currentEngine();
        eng.setFpsConvertEnabled(btnFpsConvert.getToggleState());
        updateOutputFpsButtonStates();
        resized(); repaint();
        saveSettings();
    };

    for (auto* btn : { &btnOutFps2398, &btnOutFps24, &btnOutFps25, &btnOutFps2997, &btnOutFps30 })
    { addAndMakeVisible(btn); btn->setClickingTogglesState(false); }

    btnOutFps2398.onClick = [this] { if (!syncing && !isShowLocked()) { currentEngine().setOutputFrameRate(FrameRate::FPS_2398); updateOutputFpsButtonStates(); saveSettings(); } };
    btnOutFps24.onClick   = [this] { if (!syncing && !isShowLocked()) { currentEngine().setOutputFrameRate(FrameRate::FPS_24);   updateOutputFpsButtonStates(); saveSettings(); } };
    btnOutFps25.onClick   = [this] { if (!syncing && !isShowLocked()) { currentEngine().setOutputFrameRate(FrameRate::FPS_25);   updateOutputFpsButtonStates(); saveSettings(); } };
    btnOutFps2997.onClick = [this] { if (!syncing && !isShowLocked()) { currentEngine().setOutputFrameRate(FrameRate::FPS_2997); updateOutputFpsButtonStates(); saveSettings(); } };
    btnOutFps30.onClick   = [this] { if (!syncing && !isShowLocked()) { currentEngine().setOutputFrameRate(FrameRate::FPS_30);   updateOutputFpsButtonStates(); saveSettings(); } };

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
        if (isShowLockedRevert()) return;
        populateFilteredInputDeviceCombo();
        if (currentEngine().getActiveInput() == SrcType::LTC)
            startCurrentLtcInput();
        saveSettings();
    };

    addLabelAndCombo(lblSampleRate, cmbSampleRate, "SAMPLE RATE / BUFFER:");
    populateSampleRateCombo();
    cmbSampleRate.onChange = [this] { if (!syncing && !isShowLockedRevert()) { restartAllAudioDevices(); saveSettings(); } };

    addLabelAndCombo(lblBufferSize, cmbBufferSize, "BUFFER SIZE:");
    populateBufferSizeCombo();
    cmbBufferSize.onChange = [this] { if (!syncing && !isShowLockedRevert()) { restartAllAudioDevices(); saveSettings(); } };

    addLabelAndCombo(lblMidiInputDevice, cmbMidiInputDevice, "MIDI INPUT DEVICE:");
    cmbMidiInputDevice.onChange = [this]
    {
        if (syncing) return;
        if (isShowLockedRevert()) return;
        int sel = cmbMidiInputDevice.getSelectedId() - 1;
        if (sel >= 0 && currentEngine().getActiveInput() == SrcType::MTC)
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
        if (isShowLockedRevert()) return;
        if (currentEngine().getActiveInput() == SrcType::ArtNet)
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

    addLabelAndCombo(lblHippoInputInterface, cmbHippoInputInterface, "HIPPONET INPUT DEVICE:");
    cmbHippoInputInterface.onChange = [this]
    {
        if (syncing) return;
        if (isShowLockedRevert()) return;
        if (currentEngine().getActiveInput() == SrcType::Hippotizer)
        {
            int sel = cmbHippoInputInterface.getSelectedId() - 1;
            currentEngine().stopHippotizerInput();
            currentEngine().startHippotizerInput(sel);
            int actualId = currentEngine().getHippotizerInput().getSelectedInterface() + 1;
            cmbHippoInputInterface.setSelectedId(actualId, juce::dontSendNotification);
            populateMidiAndNetworkCombos();
            saveSettings();
        }
    };

    addLabelAndCombo(lblHippoTcChannel, cmbHippoTcChannel, "HIPPONET TC CHANNEL:");
    cmbHippoTcChannel.addItem("TC 1", 1);
    cmbHippoTcChannel.addItem("TC 2", 2);
    cmbHippoTcChannel.setSelectedId(1, juce::dontSendNotification);
    cmbHippoTcChannel.onChange = [this]
    {
        if (syncing) return;
        int sel = cmbHippoTcChannel.getSelectedId() - 1;  // 0-based
        currentEngine().getHippotizerInput().setSelectedTcIndex(sel);
        saveSettings();
    };

    // --- Generator controls (play/pause/stop + start/stop TC) ---
    {
        leftContent.addAndMakeVisible(btnGenClock);
        btnGenClock.setVisible(false);
        btnGenClock.setColour(juce::ToggleButton::textColourId, juce::Colour(0xFFAAAAAA));
        btnGenClock.onClick = [this]
        {
            if (isShowLocked()) { btnGenClock.setToggleState(!btnGenClock.getToggleState(), juce::dontSendNotification); return; }
            auto& eng = currentEngine();
            eng.setGeneratorClockMode(btnGenClock.getToggleState());
            updateDeviceSelectorVisibility();
            resized();
            saveSettings();
        };

        auto genBtnColor = juce::Colour(0xFF444444);
        for (auto* btn : { &btnGenPlay, &btnGenPause, &btnGenStop })
        {
            leftContent.addAndMakeVisible(btn);
            btn->setColour(juce::TextButton::buttonColourId, genBtnColor);
            btn->setVisible(false);
        }
        btnGenPlay.onClick = [this] {
            auto& eng = currentEngine();
            eng.generatorPlay();
        };
        btnGenPause.onClick = [this] {
            auto& eng = currentEngine();
            eng.generatorPause();
        };
        btnGenStop.onClick = [this] {
            auto& eng = currentEngine();
            eng.generatorStop();
        };

        auto setupTcEditor = [&](juce::TextEditor& ed, juce::Label& lbl,
                                  const juce::String& labelText)
        {
            leftContent.addAndMakeVisible(ed);
            ed.setFont(juce::Font(juce::FontOptions(11.0f)));
            ed.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xFF222222));
            ed.setColour(juce::TextEditor::textColourId, juce::Colours::white);
            ed.setJustification(juce::Justification::centred);
            ed.setVisible(false);
            ed.setText("00:00:00:00", juce::dontSendNotification);

            leftContent.addAndMakeVisible(lbl);
            styleLabel(lbl, 8.0f);
            lbl.setText(labelText, juce::dontSendNotification);
            lbl.setVisible(false);
        };

        setupTcEditor(txtGenStartTC, lblGenStartTC, "START TC:");
        setupTcEditor(txtGenStopTC,  lblGenStopTC,  "STOP TC (0=FREE):");

        // Callbacks use member pointers directly (no dangling reference)
        auto applyStartTC = [this] {
            if (isShowLocked()) return;
            auto& eng = currentEngine();
            eng.setGeneratorStartMs(parseTimecodeToMs(txtGenStartTC.getText(), eng.getCurrentFps()));
            txtGenStartTC.setText(msToTimecodeString(eng.getGeneratorStartMs(), eng.getCurrentFps()), false);
            saveSettings();
        };
        txtGenStartTC.onReturnKey = applyStartTC;
        txtGenStartTC.onFocusLost = applyStartTC;

        auto applyStopTC = [this] {
            if (isShowLocked()) return;
            auto& eng = currentEngine();
            eng.setGeneratorStopMs(parseTimecodeToMs(txtGenStopTC.getText(), eng.getCurrentFps()));
            txtGenStopTC.setText(msToTimecodeString(eng.getGeneratorStopMs(), eng.getCurrentFps()), false);
            saveSettings();
        };
        txtGenStopTC.onReturnKey = applyStopTC;
        txtGenStopTC.onFocusLost = applyStopTC;

        // Preset selector
        leftContent.addAndMakeVisible(lblGenPreset);
        styleLabel(lblGenPreset, 8.0f);
        lblGenPreset.setText("PRESET:", juce::dontSendNotification);
        lblGenPreset.setVisible(false);

        leftContent.addAndMakeVisible(cmbGenPreset);
        cmbGenPreset.setVisible(false);
        cmbGenPreset.setTextWhenNothingSelected("(select preset)");
        cmbGenPreset.onChange = [this] {
            int sel = cmbGenPreset.getSelectedId();
            if (sel > 0)
                loadGenPresetToFields(cmbGenPreset.getText());
        };

        leftContent.addAndMakeVisible(btnGenPrev);
        btnGenPrev.setVisible(false);
        btnGenPrev.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF444444));
        btnGenPrev.onClick = [this] {
            int sel = cmbGenPreset.getSelectedId();
            int num = cmbGenPreset.getNumItems();
            if (num == 0) return;
            int newSel = (sel <= 1) ? num : sel - 1;
            cmbGenPreset.setSelectedId(newSel, juce::sendNotificationSync);
        };

        leftContent.addAndMakeVisible(btnGenNext);
        btnGenNext.setVisible(false);
        btnGenNext.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF444444));
        btnGenNext.onClick = [this] {
            int sel = cmbGenPreset.getSelectedId();
            int num = cmbGenPreset.getNumItems();
            if (num == 0) return;
            int newSel = (sel >= num) ? 1 : sel + 1;
            cmbGenPreset.setSelectedId(newSel, juce::sendNotificationSync);
        };

        leftContent.addAndMakeVisible(btnGenGo);
        btnGenGo.setVisible(false);
        btnGenGo.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF22AA44));
        btnGenGo.onClick = [this] {
            int sel = cmbGenPreset.getSelectedId();
            if (sel > 0)
                activateGenPreset(cmbGenPreset.getText());
        };

        leftContent.addAndMakeVisible(btnGenPresetEdit);
        btnGenPresetEdit.setVisible(false);
        btnGenPresetEdit.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF444444));
        btnGenPresetEdit.onClick = [this] {
            if (isShowLocked()) return;
            openGeneratorPresetEditor();
        };
    }

    // --- OSC Input (generator remote control) ---
    leftContent.addAndMakeVisible(btnOscIn);
    styleOutputToggle(btnOscIn, juce::Colour(0xFF8888CC));
    btnOscIn.setVisible(false);
    btnOscIn.onClick = [this]
    {
        if (isShowLocked()) { btnOscIn.setToggleState(!btnOscIn.getToggleState(), juce::dontSendNotification); return; }
        if (btnOscIn.getToggleState())
            startOscInput();
        else
            stopOscInput();
        updateDeviceSelectorVisibility();
        resized();
        saveSettings();
    };

    addLabelAndCombo(lblOscInputInterface, cmbOscInputInterface, "OSC NETWORK DEVICE:");
    cmbOscInputInterface.onChange = [this]
    {
        if (isShowLocked()) return;
        settings.oscInputInterface = cmbOscInputInterface.getSelectedId() - 1;
        if (btnOscIn.getToggleState())
        {
            stopOscInput();
            startOscInput();
        }
        saveSettings();
    };

    leftContent.addAndMakeVisible(lblOscInPort);
    styleLabel(lblOscInPort, 8.0f);
    lblOscInPort.setText("OSC PORT:", juce::dontSendNotification);
    lblOscInPort.setVisible(false);

    leftContent.addAndMakeVisible(txtOscInPort);
    txtOscInPort.setFont(juce::Font(juce::FontOptions(11.0f)));
    txtOscInPort.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xFF222222));
    txtOscInPort.setColour(juce::TextEditor::textColourId, juce::Colours::white);
    txtOscInPort.setJustification(juce::Justification::centred);
    txtOscInPort.setText("9800", juce::dontSendNotification);
    txtOscInPort.setVisible(false);
    auto applyOscPort = [this]
    {
        if (isShowLocked()) return;
        int port = juce::jlimit(1, 65535, txtOscInPort.getText().getIntValue());
        txtOscInPort.setText(juce::String(port), false);
        settings.oscInputPort = port;
        if (btnOscIn.getToggleState())
        {
            stopOscInput();
            startOscInput();
        }
        saveSettings();
    };
    txtOscInPort.onReturnKey = applyOscPort;
    txtOscInPort.onFocusLost = applyOscPort;

    leftContent.addAndMakeVisible(lblOscInStatus);
    styleLabel(lblOscInStatus, 8.0f);
    lblOscInStatus.setVisible(false);

    setupOscInputServer();

    // --- Pro DJ Link controls ---
    addLabelAndCombo(lblProDJLinkInterface, cmbProDJLinkInterface, "PRO DJ LINK INTERFACE:");
    cmbProDJLinkInterface.onChange = [this]
    {
        if (syncing) return;
        if (isShowLockedRevert()) return;
        if (currentEngine().getActiveInput() == SrcType::ProDJLink)
        {
            startCurrentProDJLinkInput();
            populateMidiAndNetworkCombos();
            saveSettings();
        }
    };

    addLabelAndCombo(lblStageLinQInterface, cmbStageLinQInterface, "STAGELINQ INTERFACE:");
    cmbStageLinQInterface.onChange = [this]
    {
        if (syncing) return;
        if (isShowLockedRevert()) return;
        if (currentEngine().getActiveInput() == SrcType::StageLinQ)
        {
            startCurrentStageLinQInput();
            populateMidiAndNetworkCombos();
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
        if (isShowLockedRevert()) return;
        auto activeIn = currentEngine().getActiveInput();
        if (activeIn == SrcType::ProDJLink || activeIn == SrcType::StageLinQ)
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
                lblNextCue.setText("", juce::dontSendNotification);
            }
            saveSettings();
        }
    };

    leftContent.addAndMakeVisible(lblProDJLinkMetadata);
    styleLabel(lblProDJLinkMetadata, 9.0f);
    lblProDJLinkMetadata.setVisible(false);

    leftContent.addAndMakeVisible(lblNextCue);
    lblNextCue.setFont(juce::Font(juce::FontOptions(10.0f)));
    lblNextCue.setColour(juce::Label::textColourId, juce::Colour(0xFFFFAA00));  // amber
    lblNextCue.setVisible(false);

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
        if (isShowLockedToggle(btnTrackMap)) return;
        bool enabled = btnTrackMap.getToggleState();
        for (auto& eng : engines)
            eng->setTrackMapEnabled(enabled);
        updateDeviceSelectorVisibility();
        saveSettings();
    };

    leftContent.addAndMakeVisible(btnTrackMapEdit);
    btnTrackMapEdit.setVisible(false);
    btnTrackMapEdit.setColour(juce::TextButton::buttonColourId, pdlAccent.withAlpha(0.15f));
    btnTrackMapEdit.setColour(juce::TextButton::textColourOffId, pdlAccent.brighter(0.3f));
    btnTrackMapEdit.onClick = [this] { if (!isShowLocked()) openTrackMapEditor(); };

    leftContent.addAndMakeVisible(btnProDJLinkView);
    btnProDJLinkView.setVisible(false);
    btnProDJLinkView.setColour(juce::TextButton::buttonColourId, pdlAccent.withAlpha(0.15f));
    btnProDJLinkView.setColour(juce::TextButton::textColourOffId, pdlAccent.brighter(0.3f));
    btnProDJLinkView.onClick = [this] { openProDJLinkView(); };

    auto slqAccent = juce::Colour(0xFF00CC66);
    leftContent.addAndMakeVisible(btnStageLinQView);
    btnStageLinQView.setVisible(false);
    btnStageLinQView.setColour(juce::TextButton::buttonColourId, slqAccent.withAlpha(0.15f));
    btnStageLinQView.setColour(juce::TextButton::textColourOffId, slqAccent.brighter(0.3f));
    btnStageLinQView.onClick = [this] { openStageLinQView(); };

    leftContent.addAndMakeVisible(btnMixerMapEdit);
    btnMixerMapEdit.setVisible(false);
    btnMixerMapEdit.setColour(juce::TextButton::buttonColourId, pdlAccent.withAlpha(0.15f));
    btnMixerMapEdit.setColour(juce::TextButton::textColourOffId, pdlAccent.brighter(0.3f));
    btnMixerMapEdit.onClick = [this] { openMixerMapEditor(); };

    addAndMakeVisible(btnBackup);
    btnBackup.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF1A1D23));
    btnBackup.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFF66CC66));
    btnBackup.onClick = [this] { if (!isShowLocked()) exportConfig(); };

    addAndMakeVisible(btnRestore);
    btnRestore.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF1A1D23));
    btnRestore.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFFF9966));
    btnRestore.onClick = [this] { if (!isShowLocked()) importConfig(); };

    // --- Show Lock button ---
    addAndMakeVisible(btnShowLock);
    auto updateShowLockVisuals = [this]()
    {
        if (settings.showModeLocked)
        {
            btnShowLock.setButtonText("LOCKED");
            btnShowLock.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFFCC2222));
            btnShowLock.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
        }
        else
        {
            btnShowLock.setButtonText("SHOW LOCK");
            btnShowLock.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF1A1D23));
            btnShowLock.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFF888888));
        }
    };
    updateShowLockVisuals();

    btnShowLock.onClick = [this, updateShowLockVisuals]
    {
        if (settings.showModeLocked)
        {
            // Unlock: confirm dialog to prevent accidental unlock
            auto options = juce::MessageBoxOptions()
                .withIconType(juce::MessageBoxIconType::WarningIcon)
                .withTitle("Unlock Show Mode?")
                .withMessage("Show Lock is active. Unlocking allows changes to "
                             "input sources, devices, frame rates, and engine "
                             "configuration.\n\nUnlock now?")
                .withButton("Unlock")
                .withButton("Cancel");
            juce::Component::SafePointer<MainComponent> safeThis(this);
            juce::AlertWindow::showAsync(options, [safeThis, updateShowLockVisuals](int result)
            {
                if (safeThis == nullptr) return;
                if (result == 1)
                {
                    safeThis->settings.showModeLocked = false;
                    safeThis->showLockFlashCountdown = 0;  // cancel pending flash
                    updateShowLockVisuals();
                    safeThis->saveSettings();
                }
            });
        }
        else
        {
            settings.showModeLocked = true;
            // Close cue editor (config window -- shouldn't be open during show)
            if (cuePointWindow != nullptr) { cuePointWindow.reset(); cuePointTrackKey.clear(); }
            updateShowLockVisuals();
            saveSettings();
        }
    };

    leftContent.addAndMakeVisible(btnMidiClock);

    btnMidiClock.setVisible(false);
    btnMidiClock.setColour(juce::ToggleButton::textColourId, textMid);
    btnMidiClock.setColour(juce::ToggleButton::tickColourId, pdlAccent);
    btnMidiClock.onClick = [this]
    {
        if (syncing) return;
        if (isShowLockedToggle(btnMidiClock)) return;
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
        if (isShowLockedToggle(btnOscFwdBpm)) return;
        currentEngine().setOscForward(btnOscFwdBpm.getToggleState(), edOscFwdBpmAddr.getText(), edOscFwdBpmCmd.getText());
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
    auto applyOscBpmSettings = [this]
    {
        if (!syncing && !isShowLockedRevert())
        {
            currentEngine().setOscForward(btnOscFwdBpm.getToggleState(),
                                          edOscFwdBpmAddr.getText(),
                                          edOscFwdBpmCmd.getText());
            propagateGlobalSettings();
            saveSettings();
        }
    };
    edOscFwdBpmAddr.onReturnKey = applyOscBpmSettings;
    edOscFwdBpmAddr.onFocusLost = applyOscBpmSettings;

    leftContent.addAndMakeVisible(lblOscFwdBpmCmd);
    lblOscFwdBpmCmd.setVisible(false);
    lblOscFwdBpmCmd.setText("CMD:", juce::dontSendNotification);
    lblOscFwdBpmCmd.setFont(juce::Font(juce::FontOptions(9.0f)));
    lblOscFwdBpmCmd.setColour(juce::Label::textColourId, textMid);
    lblOscFwdBpmCmd.setJustificationType(juce::Justification::centredRight);

    leftContent.addAndMakeVisible(edOscFwdBpmCmd);
    edOscFwdBpmCmd.setVisible(false);
    edOscFwdBpmCmd.setFont(juce::Font(juce::FontOptions(10.0f)));
    edOscFwdBpmCmd.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xFF2A2A2A));
    edOscFwdBpmCmd.setColour(juce::TextEditor::textColourId, textLight);
    edOscFwdBpmCmd.setColour(juce::TextEditor::outlineColourId, juce::Colour(0xFF444444));
    edOscFwdBpmCmd.setTextToShowWhenEmpty("float (empty=float, %BPM%=string)", juce::Colour(0xFF555555));
    edOscFwdBpmCmd.onReturnKey = applyOscBpmSettings;
    edOscFwdBpmCmd.onFocusLost = applyOscBpmSettings;

    leftContent.addAndMakeVisible(btnOscMixerFwd);
    btnOscMixerFwd.setVisible(false);
    btnOscMixerFwd.setColour(juce::ToggleButton::textColourId, textMid);
    btnOscMixerFwd.setColour(juce::ToggleButton::tickColourId, pdlAccent);
    btnOscMixerFwd.onClick = [this]
    {
        if (syncing) return;
        if (isShowLockedToggle(btnOscMixerFwd)) return;
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
        if (syncing) return;
        if (isShowLockedToggle(btnMidiMixerFwd)) return;
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
            if (syncing) return;
            if (isShowLockedRevert()) return;
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
        if (syncing) return;
        if (isShowLockedToggle(btnArtnetMixerFwd)) return;
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
        if (syncing) return;
        if (isShowLockedRevert()) return;
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
        if (isShowLockedToggle(btnLink)) return;

        if (btnLink.getToggleState())
        {
            // Check if another engine already has Link active
            int owner = findLinkOwnerOtherThan(selectedEngine);
            if (owner >= 0)
            {
                // Block: revert toggle and show which engine has it
                btnLink.setToggleState(false, juce::dontSendNotification);
                lblLinkStatus.setText("Link active on " + engines[(size_t)owner]->getName(),
                                     juce::dontSendNotification);
                lblLinkStatus.setColour(juce::Label::textColourId, juce::Colour(0xFFFF5555));
                lblLinkStatus.setVisible(true);
                resized();
                return;
            }
        }

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

    // --- Audio BPM detection (for non-DJ sources: MTC, LTC, ArtNet, SystemTime) ---
    {
        juce::Colour bpmAccent(0xFFFF9900);  // orange for audio BPM
        leftContent.addAndMakeVisible(btnAudioBpm);
        btnAudioBpm.setVisible(false);
        btnAudioBpm.setColour(juce::ToggleButton::textColourId, textMid);
        btnAudioBpm.setColour(juce::ToggleButton::tickColourId, bpmAccent);
        btnAudioBpm.onClick = [this]
        {
            if (syncing) return;
            if (isShowLockedToggle(btnAudioBpm)) return;
            if (btnAudioBpm.getToggleState())
            {
                // When LTC is active, populate the separate BPM device combo
                if (currentEngine().getActiveInput() == SrcType::LTC)
                {
                    cmbAudioBpmDevice.clear(juce::dontSendNotification);
                    for (int i = 0; i < scannedAudioInputs.size(); i++)
                    {
                        auto marker = getDeviceInUseMarker(scannedAudioInputs[i].deviceName,
                                                            scannedAudioInputs[i].typeName, true);
                        cmbAudioBpmDevice.addItem(scannedAudioInputs[i].displayName + marker, i + 1);
                    }
                    if (cmbAudioBpmDevice.getNumItems() > 0)
                        cmbAudioBpmDevice.setSelectedId(1, juce::dontSendNotification);
                    populateAudioBpmChannels();
                }
                restartAudioBpm();
            }
            else
                currentEngine().stopAudioBpm();
            applyTriggerSettings();
            updateDeviceSelectorVisibility();
            resized();
            saveSettings();
        };

        leftContent.addAndMakeVisible(lblBpmValue);
        lblBpmValue.setVisible(false);
        lblBpmValue.setFont(juce::Font(juce::FontOptions(18.0f, juce::Font::bold)));
        lblBpmValue.setColour(juce::Label::textColourId, juce::Colour(0xFF666666));
        lblBpmValue.setJustificationType(juce::Justification::centredLeft);
        lblBpmValue.setText("--- BPM", juce::dontSendNotification);

        leftContent.addAndMakeVisible(ledBeat);
        ledBeat.setVisible(false);
        ledBeat.setLedColour(bpmAccent);

        leftContent.addAndMakeVisible(sldBpmSmoothing); styleGainSlider(sldBpmSmoothing);
        leftContent.addAndMakeVisible(lblBpmSmoothing); lblBpmSmoothing.setText("BPM SMOOTHING:", juce::dontSendNotification); styleLabel(lblBpmSmoothing);
        sldBpmSmoothing.setRange(0.0, 100.0, 1.0);
        sldBpmSmoothing.setValue(50.0, juce::dontSendNotification);  // default 50% = 0.5
        sldBpmSmoothing.setVisible(false);
        sldBpmSmoothing.onValueChange = [this]
        {
            if (syncing) return;
            float s = (float)sldBpmSmoothing.getValue() / 100.0f;
            currentEngine().getAudioBpmInput().setSmoothing(s);
            saveSettings();
        };

        addLabelAndCombo(lblAudioBpmDevice, cmbAudioBpmDevice, "BPM DEVICE:");
        cmbAudioBpmDevice.onChange = [this]
        {
            if (syncing || isShowLockedRevert()) return;
            populateAudioBpmChannels();
            if (btnAudioBpm.getToggleState()) restartAudioBpm();
            populateFilteredInputDeviceCombo();  // refresh LTC combo markers
            saveSettings();
        };

        addLabelAndCombo(lblAudioBpmChannel, cmbAudioBpmChannel, "BPM CHANNEL:");
        cmbAudioBpmChannel.onChange = [this]
        {
            if (syncing || isShowLockedRevert()) return;
            if (btnAudioBpm.getToggleState()) restartAudioBpm();
            saveSettings();
        };

        leftContent.addAndMakeVisible(sldBpmInputGain); styleGainSlider(sldBpmInputGain);
        leftContent.addAndMakeVisible(lblBpmInputGain); lblBpmInputGain.setText("BPM INPUT GAIN:", juce::dontSendNotification); styleLabel(lblBpmInputGain);
        sldBpmInputGain.setVisible(false);
        sldBpmInputGain.onValueChange = [this]
        {
            if (syncing) return;
            float gain = (float)sldBpmInputGain.getValue() / 100.0f;
            currentEngine().getAudioBpmInput().setInputGain(gain);
            saveSettings();
        };

        leftContent.addAndMakeVisible(mtrAudioBpm);
        mtrAudioBpm.setMeterColour(bpmAccent);
        mtrAudioBpm.setVisible(false);
    }

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
                    if (!isShowLocked())  // TrackMap is config -- blocked in Show Lock
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
        if (isShowLockedToggle(btnTriggerMidi)) return;
        applyTriggerSettings();
        saveSettings();
    };

    leftContent.addAndMakeVisible(cmbTriggerMidiDevice);
    cmbTriggerMidiDevice.setVisible(false);
    styleComboBox(cmbTriggerMidiDevice);
    cmbTriggerMidiDevice.onChange = [this]
    {
        if (syncing) return;
        if (isShowLockedRevert()) return;
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
        if (isShowLockedToggle(btnTriggerOsc)) return;
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
        if (isShowLockedToggle(btnArtnetTrigger)) return;
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
        if (syncing) return;
        if (isShowLockedRevert()) return;
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
        if (isShowLockedRevert()) return;
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
        if (isShowLockedRevert()) return;
        auto& eng = currentEngine();
        if (eng.getActiveInput() == SrcType::LTC
            && cmbAudioInputDevice.getSelectedId() > 0
            && cmbAudioInputDevice.getSelectedId() != kPlaceholderItemId)
        {
            startCurrentLtcInput();
            populateFilteredInputDeviceCombo();  // refresh markers (auto-restores selection)
            populateAudioInputChannels();
            saveSettings();
        }
        else if (btnAudioBpm.getToggleState()
                 && cmbAudioInputDevice.getSelectedId() > 0
                 && cmbAudioInputDevice.getSelectedId() != kPlaceholderItemId)
        {
            populateAudioInputChannels();
            restartAudioBpm();
            saveSettings();
        }
    };

    addLabelAndCombo(lblAudioInputChannel, cmbAudioInputChannel, "LTC CHANNEL:");
    cmbAudioInputChannel.onChange = [this]
    {
        if (syncing || isShowLockedRevert()) return;
        if (currentEngine().getActiveInput() == SrcType::LTC) { startCurrentLtcInput(); saveSettings(); }
        else if (btnAudioBpm.getToggleState()) { restartAudioBpm(); saveSettings(); }
    };

    leftContent.addAndMakeVisible(sldLtcInputGain); styleGainSlider(sldLtcInputGain);
    leftContent.addAndMakeVisible(lblLtcInputGain); lblLtcInputGain.setText("LTC INPUT GAIN:", juce::dontSendNotification); styleLabel(lblLtcInputGain);
    leftContent.addAndMakeVisible(mtrLtcInput); mtrLtcInput.setMeterColour(accentPurple);
    sldLtcInputGain.onValueChange = [this]
    {
        if (syncing) return;
        float gain = (float)sldLtcInputGain.getValue() / 100.0f;
        auto& eng = currentEngine();
        if (eng.getActiveInput() == SrcType::LTC)
            eng.getLtcInput().setInputGain(gain);
        if (eng.isAudioBpmRunning())
            eng.getAudioBpmInput().setInputGain(gain);
        saveSettings();
    };

    addLabelAndCombo(lblThruInputChannel, cmbThruInputChannel, "AUDIO THRU CHANNEL:");
    cmbThruInputChannel.onChange = [this] { if (!syncing && !isShowLockedRevert() && currentEngine().getActiveInput() == SrcType::LTC) { startCurrentLtcInput(); saveSettings(); } };

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
        if (isShowLockedRevert()) return;
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
    sldMtcOffset.onValueChange = [this] { if (!syncing && !isShowLockedRevert()) { currentEngine().setMtcOutputOffset((int)sldMtcOffset.getValue()); saveSettings(); } };

    addRightLabelAndCombo(lblArtnetOutputInterface, cmbArtnetOutputInterface, "ART-NET OUTPUT DEVICE:");
    cmbArtnetOutputInterface.onChange = [this]
    {
        if (syncing) return;
        if (isShowLockedRevert()) return;
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
    sldArtnetOffset.onValueChange = [this] { if (!syncing && !isShowLockedRevert()) { currentEngine().setArtnetOutputOffset((int)sldArtnetOffset.getValue()); saveSettings(); } };

    // TCNet interface combo (shown when TCNET OUT is enabled)
    addRightLabelAndCombo(lblTcnetInterface, cmbTcnetInterface, "TCNET INTERFACE:");
    cmbTcnetInterface.onChange = [this]
    {
        if (syncing) return;
        if (isShowLockedRevert()) return;
        settings.tcnetInterface = cmbTcnetInterface.getSelectedId() - 2;  // 1=All(-1), 2+=NIC index
        if (sharedTcnetOutput.getIsRunning())
        {
            sharedTcnetOutput.stop();
            sharedTcnetOutput.refreshNetworkInterfaces();
            sharedTcnetOutput.start(settings.tcnetInterface);
        }
        saveSettings();
    };

    // TCNet layer selector (per-engine, selects which TCNet layer 1-4 this engine announces)
    addRightLabelAndCombo(lblTcnetLayer, cmbTcnetLayer, "TCNET LAYER:");
    cmbTcnetLayer.onChange = [this]
    {
        if (syncing) return;
        if (isShowLockedRevert()) return;
        int sel = cmbTcnetLayer.getSelectedId() - 1;  // 1-4 -> 0-3
        currentEngine().setTcnetLayer(sel);
        repopulateTcnetLayerCombo();
        saveSettings();
    };

    rightContent.addAndMakeVisible(sldTcnetOffset);
    sldTcnetOffset.setSliderStyle(juce::Slider::LinearHorizontal);
    sldTcnetOffset.setTextBoxStyle(juce::Slider::TextBoxRight, false, 52, 20);
    sldTcnetOffset.setRange(-1000.0, 1000.0, 1.0);
    sldTcnetOffset.setValue(0.0, juce::dontSendNotification);
    sldTcnetOffset.setTextValueSuffix(" ms");
    sldTcnetOffset.setDoubleClickReturnValue(true, 0.0);
    sldTcnetOffset.setColour(juce::Slider::backgroundColourId, juce::Colour(0xFF1A1D23));
    sldTcnetOffset.setColour(juce::Slider::trackColourId, juce::Colour(0xFF37474F));
    sldTcnetOffset.setColour(juce::Slider::thumbColourId, juce::Colour(0xFF78909C));
    sldTcnetOffset.setColour(juce::Slider::textBoxTextColourId, textBright);
    sldTcnetOffset.setColour(juce::Slider::textBoxBackgroundColourId, juce::Colour(0xFF1A1D23));
    sldTcnetOffset.setColour(juce::Slider::textBoxOutlineColourId, borderCol);
    rightContent.addAndMakeVisible(lblTcnetOffset);
    lblTcnetOffset.setText("TCNET OFFSET:", juce::dontSendNotification);
    styleLabel(lblTcnetOffset);
    sldTcnetOffset.onValueChange = [this] { if (!syncing && !isShowLockedRevert()) { currentEngine().setTcnetOutputOffsetMs((int)sldTcnetOffset.getValue()); saveSettings(); } };

    // --- Hippotizer output controls ---
    rightContent.addAndMakeVisible(lblHippoDestIp);
    lblHippoDestIp.setText("HIPPONET DEST IP:", juce::dontSendNotification);
    styleLabel(lblHippoDestIp);
    rightContent.addAndMakeVisible(txtHippoDestIp);
    txtHippoDestIp.setText("255.255.255.255", false);
    txtHippoDestIp.setFont(juce::Font(juce::FontOptions(13.0f)));
    txtHippoDestIp.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xFF1A1D23));
    txtHippoDestIp.setColour(juce::TextEditor::textColourId, textBright);
    txtHippoDestIp.setColour(juce::TextEditor::outlineColourId, borderCol);
    auto hippoIpChanged = [this]
    {
        if (syncing) return;
        if (isShowLockedRevert()) return;
        auto& eng = currentEngine();
        if (eng.isOutputHippoEnabled())
        {
            juce::String ip = txtHippoDestIp.getText().trim();
            if (ip.isEmpty()) ip = "255.255.255.255";
            eng.stopHippotizerOutput();
            eng.startHippotizerOutput(ip);
        }
        saveSettings();
    };
    txtHippoDestIp.onReturnKey = hippoIpChanged;
    txtHippoDestIp.onFocusLost = hippoIpChanged;
    rightContent.addAndMakeVisible(lblHippoOutStatus);
    lblHippoOutStatus.setFont(juce::Font(juce::FontOptions(11.0f)));
    lblHippoOutStatus.setColour(juce::Label::textColourId, textMid);

    addRightLabelAndCombo(lblAudioOutputTypeFilter, cmbAudioOutputTypeFilter, "AUDIO DRIVER:");
    cmbAudioOutputTypeFilter.onChange = [this]
    {
        if (syncing) return;
        if (isShowLockedRevert()) return;
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
        if (isShowLockedRevert()) return;
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
        if (isShowLockedRevert()) return;
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
    sldLtcOffset.onValueChange = [this] { if (!syncing && !isShowLockedRevert()) { currentEngine().setLtcOutputOffset((int)sldLtcOffset.getValue()); saveSettings(); } };

    // AudioThru controls visible for all engines in the panel but only functional for engine 0
    addRightLabelAndCombo(lblThruOutputDevice, cmbThruOutputDevice, "AUDIO THRU OUTPUT DEVICE:");
    cmbThruOutputDevice.onChange = [this]
    {
        if (syncing) return;
        if (isShowLockedRevert()) return;
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
        if (isShowLockedRevert()) return;
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

    if (genPresetWindow != nullptr)
    {
        auto b = genPresetWindow->getBounds();
        settings.genPresetBounds = juce::String(b.getX()) + " " + juce::String(b.getY())
                                 + " " + juce::String(b.getWidth()) + " " + juce::String(b.getHeight());
        delete genPresetWindow.getComponent();
    }

    oscInputServer.stop();

    if (proDJLinkViewWindow != nullptr)
    {
        auto b = proDJLinkViewWindow->getBounds();
        settings.pdlViewBounds = juce::String(b.getX()) + " " + juce::String(b.getY())
                               + " " + juce::String(b.getWidth()) + " " + juce::String(b.getHeight());
        settings.pdlViewHorizontal = proDJLinkViewWindow->getLayoutHorizontal();
        settings.pdlViewShowMixer  = proDJLinkViewWindow->getShowMixer();
        proDJLinkViewWindow.reset();
    }

    if (stageLinQViewWindow != nullptr)
    {
        auto b = stageLinQViewWindow->getBounds();
        settings.slqViewBounds = juce::String(b.getX()) + " " + juce::String(b.getY())
                               + " " + juce::String(b.getWidth()) + " " + juce::String(b.getHeight());
        settings.slqViewHorizontal = stageLinQViewWindow->getLayoutHorizontal();
        stageLinQViewWindow.reset();
    }

    settings.save();

    // 7. Stop metadata client (before ProDJLink -- it queries player IPs)
    sharedDbClient.stop();

    // 8. Stop ProDJLink receiver
    sharedProDJLinkInput.stop();

    // 9. Stop StageLinQ database client (before receiver)
    sharedStageLinQDb.stop();

    // 10. Stop StageLinQ receiver
    sharedStageLinQInput.stop();

    // 10. Explicitly shut down each engine (timers, threads, sockets)
    //    BEFORE engines.clear() destroys the objects, so all HighResolutionTimer
    //    threads are stopped while the message manager is still alive.
    //    Also disconnect shared pointers (TrackMap, MixerMap, ProDJLink, DbServer)
    //    so no stale references survive into AppSettings destruction.
    for (auto& eng : engines)
    {
        eng->setMidiClockEnabled(false);
        eng->setTrackMap(nullptr);
        eng->setMixerMap(nullptr);
        eng->setSlqMixerMap(nullptr);
        eng->setSharedProDJLinkInput(nullptr);
        eng->setSharedStageLinQInput(nullptr);
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
    engines.back()->setSharedStageLinQInput(&sharedStageLinQInput);
    engines.back()->setDbServerClient(&sharedDbClient);
    engines.back()->setTrackMap(&settings.trackMap);
    engines.back()->setMixerMap(&sharedMixerMap);
    engines.back()->setSlqMixerMap(&sharedSlqMixerMap);
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
    engines[(size_t)index]->setSlqMixerMap(nullptr);
    engines[(size_t)index]->setSharedProDJLinkInput(nullptr);
    engines[(size_t)index]->setSharedStageLinQInput(nullptr);
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
        if (newPrimary.getActiveInput() == SrcType::LTC
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
    if (isShowLocked()) return;  // no engine changes during show

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
    btnTcnetOut.setToggleState(eng.isOutputTcnetEnabled(), juce::dontSendNotification);
    repopulateTcnetLayerCombo();
    btnHippoOut.setToggleState(eng.isOutputHippoEnabled(), juce::dontSendNotification);
    if (selectedEngine < (int)settings.engines.size())
        txtHippoDestIp.setText(settings.engines[(size_t)selectedEngine].hippotizerDestIp, false);

    // Generator TC fields
    btnGenClock.setToggleState(eng.getGeneratorClockMode(), juce::dontSendNotification);
    txtGenStartTC.setText(msToTimecodeString(eng.getGeneratorStartMs(), eng.getCurrentFps()), false);
    txtGenStopTC.setText(msToTimecodeString(eng.getGeneratorStopMs(), eng.getCurrentFps()), false);
    populateGenPresetCombo();

    // OSC Input (global)
    btnOscIn.setToggleState(oscInputServer.getIsRunning(), juce::dontSendNotification);
    txtOscInPort.setText(juce::String(settings.oscInputPort), false);
    cmbOscInputInterface.setSelectedId(settings.oscInputInterface + 1, juce::dontSendNotification);

    // Offsets
    sldMtcOffset.setValue(eng.getMtcOutputOffset(), juce::dontSendNotification);
    sldArtnetOffset.setValue(eng.getArtnetOutputOffset(), juce::dontSendNotification);
    sldLtcOffset.setValue(eng.getLtcOutputOffset(), juce::dontSendNotification);
    sldTcnetOffset.setValue(eng.getTcnetOutputOffsetMs(), juce::dontSendNotification);

    // Gains
    if (eng.isAudioBpmRunning() && eng.getActiveInput() != SrcType::LTC)
        sldLtcInputGain.setValue(eng.getAudioBpmInput().getInputGain() * 100.0f, juce::dontSendNotification);
    else
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

        int hippoInId = es.hippotizerInputInterface + 1;
        if (hippoInId < 1) hippoInId = 1;
        if (hippoInId <= cmbHippoInputInterface.getNumItems())
            cmbHippoInputInterface.setSelectedId(hippoInId, juce::dontSendNotification);
        cmbHippoTcChannel.setSelectedId(es.hippotizerTcChannel + 1, juce::dontSendNotification);
        eng.getHippotizerInput().setSelectedTcIndex(es.hippotizerTcChannel);

        int artOutId = es.artnetOutputInterface + 1;
        if (artOutId < 1) artOutId = 1;              // handle legacy -1 default
        if (artOutId <= cmbArtnetOutputInterface.getNumItems())
            cmbArtnetOutputInterface.setSelectedId(artOutId, juce::dontSendNotification);

        // TCNet interface combo (global setting, not per-engine)
        int tcnetIfId = settings.tcnetInterface + 2;  // -1->1(All), 0->2, 1->3...
        if (tcnetIfId < 1) tcnetIfId = 1;
        if (tcnetIfId <= cmbTcnetInterface.getNumItems())
            cmbTcnetInterface.setSelectedId(tcnetIfId, juce::dontSendNotification);

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
        edOscFwdBpmCmd.setText(eng.getOscFwdBpmCmd(), false);

        // Art-Net DMX interface (for trigger + mixer forward)
        {
            int artDmxId = es.artnetDmxInterface + 2;  // -1->1 (All), 0->2, 1->3...
            if (artDmxId < 1) artDmxId = 1;
            if (artDmxId <= cmbArtnetDmxInterface.getNumItems())
                cmbArtnetDmxInterface.setSelectedId(artDmxId, juce::dontSendNotification);
        }

        // Ableton Link toggle
        btnLink.setToggleState(eng.getLinkBridge().isEnabled(), juce::dontSendNotification);

        // Audio BPM toggle + smoothing
        btnAudioBpm.setToggleState(eng.isAudioBpmRunning(), juce::dontSendNotification);
        sldBpmSmoothing.setValue(eng.getAudioBpmInput().getSmoothing() * 100.0f, juce::dontSendNotification);

        // When LTC is active, Audio BPM uses separate device/channel combos
        if (eng.getActiveInput() == SrcType::LTC && eng.isAudioBpmRunning())
        {
            cmbAudioBpmDevice.clear(juce::dontSendNotification);
            for (int i = 0; i < scannedAudioInputs.size(); i++)
            {
                auto marker = getDeviceInUseMarker(scannedAudioInputs[i].deviceName,
                                                    scannedAudioInputs[i].typeName, true);
                cmbAudioBpmDevice.addItem(scannedAudioInputs[i].displayName + marker, i + 1);
            }
            // Select saved device
            for (int i = 0; i < scannedAudioInputs.size(); i++)
                if (scannedAudioInputs[i].deviceName == es.audioBpmDevice
                    && scannedAudioInputs[i].typeName == es.audioBpmType)
                { cmbAudioBpmDevice.setSelectedId(i + 1, juce::dontSendNotification); break; }
            populateAudioBpmChannels();
            int abChId = es.audioBpmChannel + 2;
            if (abChId >= 1 && abChId <= cmbAudioBpmChannel.getNumItems())
                cmbAudioBpmChannel.setSelectedId(abChId, juce::dontSendNotification);
            sldBpmInputGain.setValue(eng.getAudioBpmInput().getInputGain() * 100.0f, juce::dontSendNotification);
        }

        // Pro DJ Link (per-engine player)
        cmbProDJLinkPlayer.setSelectedId(juce::jlimit(1, 8, es.proDJLinkPlayer), juce::dontSendNotification);
        // ProDJLink interface (global)
        int pdlIfId = settings.proDJLinkInterface + 1;
        if (pdlIfId >= 1 && pdlIfId <= cmbProDJLinkInterface.getNumItems())
            cmbProDJLinkInterface.setSelectedId(pdlIfId, juce::dontSendNotification);

        // StageLinQ interface (global, independent from ProDJLink)
        int slqIfId = settings.stageLinQInterface + 1;
        if (slqIfId >= 1 && slqIfId <= cmbStageLinQInterface.getNumItems())
            cmbStageLinQInterface.setSelectedId(slqIfId, juce::dontSendNotification);

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

        // Audio input device: use Audio BPM device when BPM is active on a non-LTC source
        bool bpmUsesSharedAudio = eng.isAudioBpmRunning() && eng.getActiveInput() != SrcType::LTC;
        juce::String inType  = bpmUsesSharedAudio ? es.audioBpmType   : es.audioInputType;
        juce::String inDev   = bpmUsesSharedAudio ? es.audioBpmDevice : es.audioInputDevice;
        int audioInIdx = findFilteredIndex(filteredInputIndices, scannedAudioInputs, inType, inDev);
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
    else if (eng.isAudioBpmRunning())
    {
        int ch = eng.getAudioBpmInput().getSelectedChannel();
        if (ch >= 0) cmbAudioInputChannel.setSelectedId(ch + 1, juce::dontSendNotification);
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

void MainComponent::startCurrentStageLinQInput()
{
    auto& eng = currentEngine();
    int iface = cmbStageLinQInterface.getSelectedId() - 1;
    int player = cmbProDJLinkPlayer.getSelectedId();
    if (player < 1) player = 1;
    if (iface < 0) iface = 0;

    // If already running on a DIFFERENT interface, stop and restart.
    if (sharedStageLinQInput.getIsRunning()
        && sharedStageLinQInput.getSelectedInterface() != iface)
    {
        DBG("MainComponent: StageLinQ interface changed from "
            << sharedStageLinQInput.getSelectedInterface() << " to " << iface
            << " -- restarting");
        sharedStageLinQInput.stop();
        sharedStageLinQDb.stop();
    }

    // Start shared StageLinQ connection if not already running
    if (!sharedStageLinQInput.getIsRunning())
    {
        sharedStageLinQInput.refreshNetworkInterfaces();
        sharedStageLinQInput.start(iface);
    }

    // Set this engine's deck (supports XF-A/XF-B)
    eng.startStageLinQInput(player);
}

void MainComponent::startCurrentHippotizerInput()
{
    auto& eng = currentEngine();
    int sel = cmbHippoInputInterface.getSelectedId() - 1;
    eng.startHippotizerInput(sel);
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
    auto  bpmCmd   = cur.getOscFwdBpmCmd();
    bool oscMixEn  = cur.isOscMixerForwardEnabled();
    bool midiMixEn = cur.isMidiMixerForwardEnabled();
    int  midiCCCh  = cur.getMidiMixerCCChannel();
    int  midiNotCh = cur.getMidiMixerNoteChannel();
    bool artMixEn  = cur.isArtnetMixerForwardEnabled();
    int  artMixUni = cur.getArtnetMixerUniverse();

    for (auto& engPtr : engines)
    {
        if (engPtr.get() == &cur) continue;
        auto& eng = *engPtr;

        eng.setMidiClockEnabled(clockEn);
        eng.setOscForward(oscBpmEn, bpmAddr, bpmCmd);
        eng.setOscMixerForward(oscMixEn);
        eng.setMidiMixerForward(midiMixEn, midiCCCh, midiNotCh);
        eng.setArtnetMixerForward(artMixEn, artMixUni);
        // Link is NOT propagated -- it's exclusive (only one engine at a time)
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
        // Close CuePointEditor FIRST, before save/refresh.
        // TrackMapEditor::onFormSave calls addOrUpdate/remove which can
        // rehash the unordered_map, invalidating the TrackMapEntry&
        // reference held by CuePointEditor.  The mutation has already
        // happened by the time this callback fires (synchronous message
        // thread), so close the editor before any code that might
        // indirectly touch the dangling reference.
        if (cuePointWindow != nullptr)
        {
            cuePointWindow.reset();
            cuePointTrackKey.clear();
        }

        settings.trackMap.save();
        for (auto& e : engines)
            e->refreshTrackMapLookup();
    };

    editor->onOpenCueEditor = [this](TrackMapEntry* entry)
    {
        if (entry && !settings.showModeLocked) openCuePointEditor(entry);
    };

    editor->onLearnTrackInfo = [this]() -> TrackMapEditor::LearnTrackInfo
    {
        auto info = currentEngine().getActiveTrackInfo();
        return { info.artist, info.title, info.durationSec };
    };

    // Non-modal window (self-deleting on close so SafePointer auto-nulls)
    struct FloatingWindow : juce::DocumentWindow
    {
        FloatingWindow(const juce::String& t, juce::Colour bg)
            : DocumentWindow(t, bg, DocumentWindow::closeButton | DocumentWindow::maximiseButton) {}
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
                    if (d.totalArea.contains(c)) { win->setBounds(b); break; }
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

void MainComponent::openCuePointEditor(TrackMapEntry* entry)
{
    if (!entry) return;

    // If already open for a different track, close it first
    if (cuePointWindow != nullptr)
    {
        cuePointWindow.reset();
    }

    cuePointWindow = std::make_unique<CuePointEditorWindow>(*entry);
    cuePointTrackKey = entry->key();

    // Try to find artwork, waveform, and duration for this track
    bool foundMeta = false;
    bool foundWaveform = false;
    bool foundArtwork = false;
    for (auto& eng : engines)
    {
        auto info = eng->getActiveTrackInfo();
        if (info.title.isNotEmpty()
            && TrackMapEntry::makeKey(info.artist, info.title, info.durationSec)
                == entry->key())
        {
            if (info.trackId != 0)
            {
                auto meta = sharedDbClient.getCachedMetadataByTrackId(info.trackId);

                // Artwork
                if (meta.artworkId != 0)
                {
                    auto art = sharedDbClient.getCachedArtwork(meta.artworkId);
                    if (art.isValid())
                    {
                        cuePointWindow->setArtwork(art);
                        foundArtwork = true;
                        if (!WaveformCache::artworkExists(entry->key()))
                            WaveformCache::saveArtwork(entry->key(), art);
                    }
                }

                // Waveform
                if (meta.hasWaveform())
                {
                    cuePointWindow->setWaveformData(meta.waveformData,
                        meta.waveformEntryCount, meta.waveformBytesPerEntry);
                    foundWaveform = true;

                    // Cache waveform to disk for offline use
                    if (!WaveformCache::exists(entry->key()))
                    {
                        uint32_t durMs = (meta.durationSeconds > 0)
                            ? (uint32_t)meta.durationSeconds * 1000
                            : (info.durationSec > 0 ? (uint32_t)info.durationSec * 1000 : 0);
                        WaveformCache::save(entry->key(), meta.waveformData,
                            meta.waveformEntryCount, meta.waveformBytesPerEntry, durMs);
                    }
                }

                // Duration
                if (meta.durationSeconds > 0)
                {
                    cuePointWindow->setDurationMs((uint32_t)meta.durationSeconds * 1000);
                    foundMeta = true;
                }
            }

            // Fallback duration: from engine (StageLinQ tracks have no dbClient metadata)
            if (!foundMeta && info.durationSec > 0)
                cuePointWindow->setDurationMs((uint32_t)info.durationSec * 1000);

            // StageLinQ: try waveform + artwork from StageLinQDbClient
            if (!foundWaveform
                && eng->getActiveInput() == SrcType::StageLinQ)
            {
                int deck = eng->getEffectivePlayer();
                auto netPath = sharedStageLinQInput.getTrackNetworkPath(deck);
                if (netPath.isNotEmpty())
                {
                    auto wf = sharedStageLinQDb.getWaveformForTrack(netPath);
                    if (wf.valid && !wf.data.empty())
                    {
                        cuePointWindow->setWaveformData(wf.data, wf.entryCount, 3);
                        foundWaveform = true;

                        if (!WaveformCache::exists(entry->key()))
                        {
                            uint32_t durMs = (info.durationSec > 0)
                                ? (uint32_t)info.durationSec * 1000 : 0;
                            WaveformCache::save(entry->key(), wf.data,
                                wf.entryCount, 3, durMs);
                        }
                    }

                    if (!foundArtwork)
                    {
                        auto art = sharedStageLinQDb.getArtworkForTrack(netPath);
                        if (art.isValid())
                        {
                            cuePointWindow->setArtwork(art);
                            foundArtwork = true;
                            if (!WaveformCache::artworkExists(entry->key()))
                                WaveformCache::saveArtwork(entry->key(), art);
                        }
                    }
                }
            }

            break;
        }
    }

    // If no engine has this track active, search the dbClient cache directly.
    // This covers tracks loaded on a player that no engine follows (e.g. Link Export
    // from another CDJ, or a player the user hasn't assigned to any engine).
    if (!foundWaveform || !foundArtwork)
    {
        auto entryKey = entry->key();
        for (int pn = 1; pn <= 6; ++pn)
        {
            if (!sharedProDJLinkInput.isPlayerDiscovered(pn)) continue;
            uint32_t tid = sharedProDJLinkInput.getTrackID(pn);
            if (tid == 0) continue;

            auto meta = sharedDbClient.getCachedMetadataByTrackId(tid);
            if (!meta.isValid()) continue;

            auto metaKey = TrackMapEntry::makeKey(meta.artist, meta.title, meta.durationSeconds);
            if (metaKey != entryKey) continue;

            if (!foundArtwork && meta.artworkId != 0)
            {
                auto art = sharedDbClient.getCachedArtwork(meta.artworkId);
                if (art.isValid())
                {
                    cuePointWindow->setArtwork(art);
                    foundArtwork = true;
                    if (!WaveformCache::artworkExists(entry->key()))
                        WaveformCache::saveArtwork(entry->key(), art);
                }
            }
            if (!foundWaveform && meta.hasWaveform())
            {
                cuePointWindow->setWaveformData(meta.waveformData,
                    meta.waveformEntryCount, meta.waveformBytesPerEntry);
                foundWaveform = true;
                if (!WaveformCache::exists(entry->key()))
                {
                    uint32_t durMs = (meta.durationSeconds > 0)
                        ? (uint32_t)meta.durationSeconds * 1000 : 0;
                    WaveformCache::save(entry->key(), meta.waveformData,
                        meta.waveformEntryCount, meta.waveformBytesPerEntry, durMs);
                }
            }
            if (!foundMeta && meta.durationSeconds > 0)
            {
                cuePointWindow->setDurationMs((uint32_t)meta.durationSeconds * 1000);
                foundMeta = true;
            }
            break;
        }
    }

    // If no live waveform was found, try loading from cache
    if (!foundWaveform)
    {
        auto cached = WaveformCache::load(entry->key());
        if (cached.valid)
        {
            cuePointWindow->setWaveformData(cached.data,
                cached.entryCount, cached.bytesPerEntry);
            // Use cached duration if we didn't get one from live sources
            if (!foundMeta && cached.durationMs > 0)
                cuePointWindow->setDurationMs(cached.durationMs);
        }
    }

    // If no live artwork was found, try loading from cache
    if (!foundArtwork)
    {
        auto cachedArt = WaveformCache::loadArtwork(entry->key());
        if (cachedArt.isValid())
            cuePointWindow->setArtwork(cachedArt);
    }

    // Last resort: use duration from the TrackMap entry itself
    if (!foundMeta && entry->durationSec > 0)
        cuePointWindow->setDurationMs((uint32_t)entry->durationSec * 1000);

    cuePointWindow->setOnChange([this]
    {
        settings.trackMap.save();
        for (auto& eng : engines)
            eng->refreshTrackMapLookup();

        // Refresh TrackMap editor table if open (cue count column)
        if (trackMapWindow != nullptr)
        {
            if (auto* editor = dynamic_cast<TrackMapEditor*>(trackMapWindow->getContentComponent()))
                editor->refresh();
        }
    });

    cuePointWindow->setOnCapturePlayhead([this]() -> uint32_t
    {
        // Only return live playhead if the currently playing track matches
        // the track being edited.  Otherwise return sentinel so the editor
        // hides the red cursor and doesn't interfere with mouse editing.
        auto& eng = currentEngine();
        auto info = eng.getActiveTrackInfo();
        std::string playingKey;
        if (info.title.isNotEmpty())
            playingKey = TrackMapEntry::makeKey(info.artist, info.title, info.durationSec);

        if (playingKey.empty() || playingKey != cuePointTrackKey)
            return UINT32_MAX;  // sentinel: "no matching track"

        return eng.getSmoothedPlayheadMs();
    });

    // Restore saved window position
    if (settings.cuePointBounds.isNotEmpty())
    {
        auto parts = juce::StringArray::fromTokens(settings.cuePointBounds, " ", "");
        if (parts.size() == 4)
        {
            auto b = juce::Rectangle<int>(parts[0].getIntValue(), parts[1].getIntValue(),
                                           parts[2].getIntValue(), parts[3].getIntValue());
            if (b.getWidth() >= 400 && b.getHeight() >= 300)
            {
                auto c = b.getCentre();
                for (auto& d : juce::Desktop::getInstance().getDisplays().displays)
                    if (d.totalArea.contains(c)) { cuePointWindow->setBounds(b); break; }
            }
        }
    }

    // Save window position on close
    cuePointWindow->onBoundsCapture = [this]
    {
        if (cuePointWindow != nullptr)
        {
            auto b = cuePointWindow->getBounds();
            settings.cuePointBounds = juce::String(b.getX()) + " " + juce::String(b.getY())
                                    + " " + juce::String(b.getWidth()) + " " + juce::String(b.getHeight());
            saveSettings();
        }
    };
}

void MainComponent::openMixerMapEditor()
{
    if (mixerMapWindow != nullptr)
    {
        mixerMapWindow->toFront(true);
        return;
    }

    // Choose mixer map based on active input
    bool isSlq = currentEngine().getActiveInput() == SrcType::StageLinQ;
    MixerMap& activeMap = isSlq ? sharedSlqMixerMap : sharedMixerMap;

    auto* editor = new MixerMapEditor(activeMap,
                                      isSlq ? DjmModel::All : djmModelFromString(sharedProDJLinkInput.getDJMModel()));
    editor->onChange = [this, isSlq]
    {
        if (isSlq)
            sharedSlqMixerMap.save();
        else
            sharedMixerMap.save();
    };

    juce::String title = isSlq ? "Mixer Map - Denon StageLinQ" : "Mixer Map - DJM Parameter Routing";

    // Non-modal window (self-deleting on close so SafePointer auto-nulls)
    struct FloatingWindow : juce::DocumentWindow
    {
        FloatingWindow(const juce::String& t, juce::Colour bg)
            : DocumentWindow(t, bg, DocumentWindow::closeButton | DocumentWindow::maximiseButton) {}
        void closeButtonPressed() override { if (onClose) onClose(); delete this; }
        std::function<void()> onClose;
    };

    auto* win = new FloatingWindow(title, juce::Colour(0xFF12141A));
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
                    if (d.totalArea.contains(c)) { win->setBounds(b); break; }
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
    proDJLinkViewWindow->setIsShowLockedFn([this] { return settings.showModeLocked; });
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

void MainComponent::openStageLinQView()
{
    // If already open and visible, bring to front
    if (stageLinQViewWindow != nullptr && stageLinQViewWindow->isVisible())
    {
        stageLinQViewWindow->toFront(true);
        return;
    }

    stageLinQViewWindow = std::make_unique<StageLinQViewWindow>(
        sharedStageLinQInput, sharedStageLinQDb, settings.trackMap, engines);

    // Restore layout state and bounds
    if (settings.slqViewBounds.isNotEmpty())
        stageLinQViewWindow->restoreFromBoundsString(settings.slqViewBounds);

    stageLinQViewWindow->setOnLayoutChanged([this]
    {
        if (stageLinQViewWindow != nullptr)
        {
            settings.slqViewHorizontal = stageLinQViewWindow->getLayoutHorizontal();
            saveSettings();
        }
    });
    stageLinQViewWindow->onBoundsCapture = [this]
    {
        if (stageLinQViewWindow != nullptr)
        {
            auto b = stageLinQViewWindow->getBounds();
            settings.slqViewBounds = juce::String(b.getX()) + " " + juce::String(b.getY())
                                   + " " + juce::String(b.getWidth()) + " " + juce::String(b.getHeight());
            settings.slqViewHorizontal = stageLinQViewWindow->getLayoutHorizontal();
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
    sharedSlqMixerMap.save();

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
                        sharedSlqMixerMap.resetToDefaults();
                        sharedSlqMixerMap.load();

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
            if (eng.getActiveInput() == SrcType::LTC)
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
    syncUIFromEngine();  // Re-populate Audio BPM device combo now that scan data is available
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

            // Audio input (BPM detection)
            if (typeName.isNotEmpty()
                && eng.isAudioBpmRunning()
                && eng.getAudioBpmInput().getCurrentDeviceName() == devName
                && eng.getAudioBpmInput().getCurrentTypeName() == typeName)
            {
                if (isCurrent) { result += juce::String::charToString(0x25CF); }
                else           { return " [" + eng.getName() + " BPM]"; }
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
            if (eng.getActiveInput() == SrcType::LTC && eng.getLtcInput().getIsRunning())
                startCurrentLtcInput();
            if (eng.isOutputLtcEnabled() && eng.getLtcOutput().getIsRunning())
                startCurrentLtcOutput();
        }
        else
        {
            // Non-selected engines: restart using their current device settings
            if (eng.getActiveInput() == SrcType::LTC && eng.getLtcInput().getIsRunning())
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

        // Restart Audio BPM on all engines (independent from LTC input)
        if (eng.isAudioBpmRunning())
        {
            auto devName = eng.getAudioBpmInput().getCurrentDeviceName();
            auto typeName = eng.getAudioBpmInput().getCurrentTypeName();
            int ch = eng.getAudioBpmInput().getSelectedChannel();
            eng.startAudioBpm(typeName, devName, ch, sr, bs);
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
    int savedHippoIn  = cmbHippoInputInterface.getSelectedId();
    int savedArtOut   = cmbArtnetOutputInterface.getSelectedId();
    int savedArtDmx   = cmbArtnetDmxInterface.getSelectedId();
    int savedTcnetIf  = cmbTcnetInterface.getSelectedId();

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
    cmbTcnetInterface.clear(juce::dontSendNotification);
    cmbTcnetInterface.addItem("All Interfaces (Broadcast)", 1);
    for (int i = 0; i < nets.size(); i++)
    {
        auto label = nets[i].name + " (" + nets[i].ip + ")";
        cmbArtnetInputInterface.addItem(label + getArtnetMarker(i + 2, true), i + 2);
        cmbArtnetOutputInterface.addItem(label + getArtnetMarker(i + 2, false), i + 2);
        cmbArtnetDmxInterface.addItem(label, i + 2);
        cmbTcnetInterface.addItem(label, i + 2);
    }

    // Pro DJ Link interfaces
    int savedProDJLinkIf = cmbProDJLinkInterface.getSelectedId();
    cmbProDJLinkInterface.clear(juce::dontSendNotification);
    {
        // Determine which IP is currently active for ProDJLink
        juce::String activeIp;
        if (sharedProDJLinkInput.getIsRunning())
            activeIp = sharedProDJLinkInput.getBindInfo();

        static const juce::String dot = juce::String(" ") + juce::String::charToString(0x25CF);
        for (int i = 0; i < nets.size(); i++)
        {
            juce::String label = nets[i].name + " (" + nets[i].ip + ")";
            if (activeIp.isNotEmpty() && nets[i].ip == activeIp)
                label += dot;
            cmbProDJLinkInterface.addItem(label, i + 1);
        }
    }
    if (savedProDJLinkIf > 0 && savedProDJLinkIf <= cmbProDJLinkInterface.getNumItems())
        cmbProDJLinkInterface.setSelectedId(savedProDJLinkIf, juce::dontSendNotification);
    else if (cmbProDJLinkInterface.getNumItems() > 0)
        cmbProDJLinkInterface.setSelectedId(1, juce::dontSendNotification);

    // StageLinQ interfaces (independent combo)
    int savedStageLinQIf = cmbStageLinQInterface.getSelectedId();
    cmbStageLinQInterface.clear(juce::dontSendNotification);
    {
        juce::String slqActiveIp;
        if (sharedStageLinQInput.getIsRunning())
            slqActiveIp = sharedStageLinQInput.getBindInfo();

        static const juce::String dot = juce::String(" ") + juce::String::charToString(0x25CF);
        for (int i = 0; i < nets.size(); i++)
        {
            juce::String label = nets[i].name + " (" + nets[i].ip + ")";
            if (slqActiveIp.isNotEmpty() && nets[i].ip == slqActiveIp)
                label += dot;
            cmbStageLinQInterface.addItem(label, i + 1);
        }
    }
    if (savedStageLinQIf > 0 && savedStageLinQIf <= cmbStageLinQInterface.getNumItems())
        cmbStageLinQInterface.setSelectedId(savedStageLinQIf, juce::dontSendNotification);
    else if (cmbStageLinQInterface.getNumItems() > 0)
        cmbStageLinQInterface.setSelectedId(1, juce::dontSendNotification);

    // Hippotizer interfaces (same NIC list as Art-Net, with All Interfaces)
    cmbHippoInputInterface.clear(juce::dontSendNotification);
    cmbHippoInputInterface.addItem("All Interfaces", 1);
    for (int i = 0; i < nets.size(); i++)
        cmbHippoInputInterface.addItem(nets[i].name + " (" + nets[i].ip + ")", i + 2);

    // Restore all selections (IDs are stable across repopulate)
    if (savedMidiIn > 0 && savedMidiIn <= cmbMidiInputDevice.getNumItems())
        cmbMidiInputDevice.setSelectedId(savedMidiIn, juce::dontSendNotification);
    if (savedMidiOut > 0 && savedMidiOut <= cmbMidiOutputDevice.getNumItems())
        cmbMidiOutputDevice.setSelectedId(savedMidiOut, juce::dontSendNotification);
    if (savedArtIn > 0 && savedArtIn <= cmbArtnetInputInterface.getNumItems())
        cmbArtnetInputInterface.setSelectedId(savedArtIn, juce::dontSendNotification);
    if (savedHippoIn > 0 && savedHippoIn <= cmbHippoInputInterface.getNumItems())
        cmbHippoInputInterface.setSelectedId(savedHippoIn, juce::dontSendNotification);
    else if (cmbHippoInputInterface.getNumItems() > 0)
        cmbHippoInputInterface.setSelectedId(1, juce::dontSendNotification);
    if (savedArtOut > 0 && savedArtOut <= cmbArtnetOutputInterface.getNumItems())
        cmbArtnetOutputInterface.setSelectedId(savedArtOut, juce::dontSendNotification);
    if (savedArtDmx > 0 && savedArtDmx <= cmbArtnetDmxInterface.getNumItems())
        cmbArtnetDmxInterface.setSelectedId(savedArtDmx, juce::dontSendNotification);
    else if (cmbArtnetDmxInterface.getNumItems() > 0)
        cmbArtnetDmxInterface.setSelectedId(1, juce::dontSendNotification);
    if (savedTcnetIf > 0 && savedTcnetIf <= cmbTcnetInterface.getNumItems())
        cmbTcnetInterface.setSelectedId(savedTcnetIf, juce::dontSendNotification);
    else if (cmbTcnetInterface.getNumItems() > 0)
        cmbTcnetInterface.setSelectedId(1, juce::dontSendNotification);  // default: All Interfaces
}

void MainComponent::populateAudioCombos()
{
    populateTypeFilterCombos();
    populateFilteredInputDeviceCombo();
    populateFilteredOutputDeviceCombos();
}

void MainComponent::repopulateTcnetLayerCombo()
{
    cmbTcnetLayer.clear(juce::dontSendNotification);

    static const char* names[] = { "Layer 1", "Layer 2", "Layer 3", "Layer 4" };

    for (int layer = 0; layer < TCNetOutput::kMaxLayers; ++layer)
    {
        juce::String label(names[layer]);

        // Check if another engine is using this layer
        for (int e = 0; e < (int)engines.size(); ++e)
        {
            if (e == selectedEngine) continue;
            auto& eng = *engines[(size_t)e];
            if (eng.isOutputTcnetEnabled() && eng.getTcnetLayer() == layer)
            {
                label += " [" + eng.getName() + "]";
                break;
            }
        }

        cmbTcnetLayer.addItem(label, layer + 1);
    }

    cmbTcnetLayer.setSelectedId(currentEngine().getTcnetLayer() + 1, juce::dontSendNotification);
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
        engines.back()->setSharedStageLinQInput(&sharedStageLinQInput);
        engines.back()->setDbServerClient(&sharedDbClient);
        engines.back()->setMixerMap(&sharedMixerMap);
        engines.back()->setSlqMixerMap(&sharedSlqMixerMap);
    }

    // Load mixer maps (user-editable param -> OSC/MIDI mapping)
    sharedMixerMap.load();
    sharedSlqMixerMap.load();

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
        eng.setOutputTcnetEnabled(es.tcnetOutEnabled);
        eng.setTcnetLayer(es.tcnetLayer);
        eng.setOutputHippoEnabled(es.hippoOutEnabled);

        eng.setMtcOutputOffset(es.mtcOutputOffset);
        eng.setArtnetOutputOffset(es.artnetOutputOffset);
        eng.setLtcOutputOffset(es.ltcOutputOffset);
        eng.setTcnetOutputOffsetMs(es.tcnetOutputOffsetMs);

        eng.getLtcInput().setInputGain((float)es.ltcInputGain / 100.0f);
        eng.getLtcInput().setPassthruGain((float)es.thruInputGain / 100.0f);
        eng.getLtcOutput().setOutputGain((float)es.ltcOutputGain / 100.0f);
        if (eng.getAudioThru())
            eng.getAudioThru()->setOutputGain((float)es.thruOutputGain / 100.0f);

        // Set input source (defer actual device start)
        auto src = TimecodeEngine::stringToInputSource(es.inputSource);
        eng.setInputSource(src);

        // Start non-audio protocols
        if (src == SrcType::MTC)
        {
            int idx = findDeviceByName(cmbMidiInputDevice, es.midiInputDevice);
            eng.startMtcInput(idx);
        }
        else if (src == SrcType::ArtNet)
        {
            eng.startArtnetInput(es.artnetInputInterface);
        }
        else if (src == SrcType::ProDJLink)
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
        else if (src == SrcType::StageLinQ)
        {
            if (!sharedStageLinQInput.getIsRunning())
            {
                sharedStageLinQInput.refreshNetworkInterfaces();
                sharedStageLinQInput.start(settings.stageLinQInterface);
            }
            eng.startStageLinQInput(es.proDJLinkPlayer);
        }
        else if (src == SrcType::Hippotizer)
        {
            // HippoNet input disabled pending hardware validation -- fall back to Generator
            DBG("MainComponent: HippoNet input disabled, falling back to Generator");
            eng.setInputSource(SrcType::SystemTime);
        }

        // Generator start/stop TC (applies regardless of current source)
        eng.setGeneratorClockMode(es.generatorClockMode);
        eng.setGeneratorStartMs(es.generatorStartMs);
        eng.setGeneratorStopMs(es.generatorStopMs);

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
            eng.setOscForward(true, es.oscBpmAddr, es.oscBpmCmd);

        // Mixer fader forward
        if (es.oscMixerForward)
            eng.setOscMixerForward(true);
        if (es.midiMixerForward)
            eng.setMidiMixerForward(true, es.midiMixerCCChannel, es.midiMixerNoteChannel);
        if (es.artnetMixerForward)
            eng.setArtnetMixerForward(true, es.artnetMixerUniverse);
        eng.setArtnetTriggerUniverse(es.artnetTriggerUniverse);
        eng.setArtnetTriggerEnabled(es.artnetTriggerEnabled);

        // Ableton Link (exclusive: only one engine can have it)
        if (es.linkEnabled)
        {
            bool alreadyOwned = false;
            for (int j = 0; j < i; j++)
                if (engines[(size_t)j]->getLinkBridge().isEnabled())
                { alreadyOwned = true; break; }
            if (!alreadyOwned)
                eng.getLinkBridge().setEnabled(true);
        }

        // Audio BPM detection
        if (es.audioBpmEnabled && !es.audioBpmDevice.isEmpty())
            eng.startAudioBpm(es.audioBpmType, es.audioBpmDevice, es.audioBpmChannel);
        eng.getAudioBpmInput().setSmoothing(es.audioBpmSmoothing);
        eng.getAudioBpmInput().setInputGain((float)es.audioBpmGain / 100.0f);

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
        // HippoNet output disabled in this version (pending hardware validation)
        // if (es.hippoOutEnabled)
        //     eng.startHippotizerOutput(es.hippotizerDestIp);
    }

    // Start TCNet output if any engine has it enabled
    {
        bool anyTcnet = false;
        for (auto& e : engines)
            if (e->isOutputTcnetEnabled()) { anyTcnet = true; break; }
        if (anyTcnet)
        {
            sharedTcnetOutput.refreshNetworkInterfaces();
            sharedTcnetOutput.start(settings.tcnetInterface);
        }
    }

    // OSC Input (global generator remote control)
    txtOscInPort.setText(juce::String(settings.oscInputPort), false);
    cmbOscInputInterface.setSelectedId(settings.oscInputInterface + 1, juce::dontSendNotification);
    if (settings.oscInputEnabled)
        startOscInput();

    // Global settings
    cmbSampleRate.setSelectedId(sampleRateToComboId(settings.preferredSampleRate), juce::dontSendNotification);
    cmbBufferSize.setSelectedId(bufferSizeToComboId(settings.preferredBufferSize), juce::dontSendNotification);

    selectedEngine = juce::jlimit(0, (int)engines.size() - 1, settings.selectedEngine);
    rebuildTabButtons();

    // Restore Show Lock visual state
    if (settings.showModeLocked)
    {
        btnShowLock.setButtonText("LOCKED");
        btnShowLock.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFFCC2222));
        btnShowLock.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    }
    else
    {
        btnShowLock.setButtonText("SHOW LOCK");
        btnShowLock.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF1A1D23));
        btnShowLock.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFF888888));
    }

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
        if (eng.getActiveInput() == SrcType::LTC)
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
        es.tcnetOutEnabled = eng.isOutputTcnetEnabled();
        es.tcnetLayer = eng.getTcnetLayer();
        es.hippoOutEnabled = eng.isOutputHippoEnabled();
        es.generatorClockMode = eng.getGeneratorClockMode();
        es.generatorStartMs = eng.getGeneratorStartMs();
        es.generatorStopMs  = eng.getGeneratorStopMs();

        es.mtcOutputOffset = eng.getMtcOutputOffset();
        es.artnetOutputOffset = eng.getArtnetOutputOffset();
        es.ltcOutputOffset = eng.getLtcOutputOffset();
        es.tcnetOutputOffsetMs = eng.getTcnetOutputOffsetMs();

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
            es.hippotizerInputInterface = cmbHippoInputInterface.getSelectedId() - 1;
            es.hippotizerTcChannel = cmbHippoTcChannel.getSelectedId() - 1;
            es.hippotizerDestIp = txtHippoDestIp.getText().trim();
            if (es.hippotizerDestIp.isEmpty()) es.hippotizerDestIp = "255.255.255.255";
            es.generatorClockMode = eng.getGeneratorClockMode();
            es.generatorStartMs = eng.getGeneratorStartMs();
            es.generatorStopMs  = eng.getGeneratorStopMs();
            es.artnetOutputInterface = cmbArtnetOutputInterface.getSelectedId() - 1;
            es.trackMapEnabled = eng.isTrackMapEnabled();
            es.midiClockEnabled = eng.isMidiClockEnabled();
            es.oscBpmForward    = eng.isOscForwardEnabled();
            es.oscBpmAddr       = eng.getOscFwdBpmAddr();
            es.oscBpmCmd        = eng.getOscFwdBpmCmd();
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

                // Audio BPM device/channel
                es.audioBpmEnabled = eng.isAudioBpmRunning();
                if (eng.isAudioBpmRunning())
                {
                    if (eng.getActiveInput() == SrcType::LTC)
                    {
                        // LTC active: read from separate BPM combos
                        int abSel = cmbAudioBpmDevice.getSelectedId() - 1;
                        if (abSel >= 0 && abSel < scannedAudioInputs.size())
                        {
                            es.audioBpmDevice = scannedAudioInputs[abSel].deviceName;
                            es.audioBpmType   = scannedAudioInputs[abSel].typeName;
                        }
                        es.audioBpmChannel = cmbAudioBpmChannel.getSelectedId() - 2;
                    }
                    else
                    {
                        // Non-LTC: read from shared AUDIO SETTINGS combos
                        auto bpmEntry = getSelectedAudioInput();
                        if (bpmEntry.deviceName.isNotEmpty())
                        {
                            es.audioBpmDevice = bpmEntry.deviceName;
                            es.audioBpmType   = bpmEntry.typeName;
                        }
                        es.audioBpmChannel = cmbAudioInputChannel.getSelectedId() - 1;
                    }
                }
                es.audioBpmSmoothing = (float)sldBpmSmoothing.getValue() / 100.0f;
                // Gain: from separate slider in LTC mode, from shared slider otherwise
                if (eng.getActiveInput() == SrcType::LTC)
                    es.audioBpmGain = (int)sldBpmInputGain.getValue();
                else if (eng.isAudioBpmRunning())
                    es.audioBpmGain = (int)sldLtcInputGain.getValue();
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
            es.oscBpmCmd        = eng.getOscFwdBpmCmd();
            es.oscMixerForward  = eng.isOscMixerForwardEnabled();
            es.midiMixerForward = eng.isMidiMixerForwardEnabled();
            es.midiMixerCCChannel   = eng.getMidiMixerCCChannel();
            es.midiMixerNoteChannel = eng.getMidiMixerNoteChannel();
            es.artnetMixerForward  = eng.isArtnetMixerForwardEnabled();
            es.artnetMixerUniverse = eng.getArtnetMixerUniverse();
            es.artnetTriggerUniverse = eng.getArtnetTriggerUniverse();
            es.linkEnabled = eng.getLinkBridge().isEnabled();

            // Audio BPM
            es.audioBpmEnabled = eng.isAudioBpmRunning();
            if (eng.getAudioBpmInput().getIsRunning())
            {
                es.audioBpmDevice = eng.getAudioBpmInput().getCurrentDeviceName();
                es.audioBpmType   = eng.getAudioBpmInput().getCurrentTypeName();
                es.audioBpmChannel = eng.getAudioBpmInput().getSelectedChannel();
            }
            es.audioBpmSmoothing = eng.getAudioBpmInput().getSmoothing();
            es.audioBpmGain = (int)(eng.getAudioBpmInput().getInputGain() * 100.0f);

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

    // StageLinQ global settings
    settings.stageLinQInterface = cmbStageLinQInterface.getSelectedId() - 1;
    if (settings.stageLinQInterface < 0) settings.stageLinQInterface = 0;

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
    if (cuePointWindow != nullptr && cuePointWindow->isVisible())
    {
        auto b = cuePointWindow->getBounds();
        settings.cuePointBounds = juce::String(b.getX()) + " " + juce::String(b.getY())
                                + " " + juce::String(b.getWidth()) + " " + juce::String(b.getHeight());
    }
    if (genPresetWindow != nullptr && genPresetWindow->isVisible())
    {
        auto b = genPresetWindow->getBounds();
        settings.genPresetBounds = juce::String(b.getX()) + " " + juce::String(b.getY())
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

    // Channel count: use whichever audio source is active
    int n = 2;
    if (eng.getLtcInput().getIsRunning())
        n = juce::jmax(2, eng.getLtcInput().getChannelCount());
    else if (eng.isAudioBpmRunning())
        n = juce::jmax(2, eng.getAudioBpmInput().getChannelCount());

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

void MainComponent::populateAudioBpmChannels()
{
    int prev = cmbAudioBpmChannel.getSelectedId();
    cmbAudioBpmChannel.clear(juce::dontSendNotification);

    // Use running device channel count if available, else default 2
    auto& eng = currentEngine();
    int numCh = eng.getAudioBpmInput().getIsRunning()
              ? juce::jmax(2, eng.getAudioBpmInput().getChannelCount())
              : 2;

    if (numCh >= 2)
        cmbAudioBpmChannel.addItem("Stereo Mix", 1);  // id 1 -> channel -1
    for (int i = 0; i < numCh; i++)
        cmbAudioBpmChannel.addItem("Ch " + juce::String(i + 1), i + 2);  // id 2..N+1 -> channel 0..N-1

    if (prev >= 1 && prev <= cmbAudioBpmChannel.getNumItems())
        cmbAudioBpmChannel.setSelectedId(prev, juce::dontSendNotification);
    else
        cmbAudioBpmChannel.setSelectedId(numCh >= 2 ? 1 : 2, juce::dontSendNotification);
}

void MainComponent::restartAudioBpm()
{
    auto& eng = currentEngine();
    bool ltcActive = (eng.getActiveInput() == SrcType::LTC);

    juce::String typeName, deviceName;
    int channel = 0;

    if (ltcActive)
    {
        // LTC owns shared AUDIO SETTINGS — read from separate BPM combos
        int devIdx = cmbAudioBpmDevice.getSelectedId() - 1;
        if (devIdx < 0 || devIdx >= scannedAudioInputs.size())
        {
            eng.stopAudioBpm();
            return;
        }
        typeName   = scannedAudioInputs[devIdx].typeName;
        deviceName = scannedAudioInputs[devIdx].deviceName;
        channel = cmbAudioBpmChannel.getSelectedId() - 2;  // id 1->-1(stereo), 2->0, 3->1...
        if (channel < -1) channel = -1;
    }
    else
    {
        // Non-LTC — read from shared AUDIO SETTINGS combos
        auto entry = getSelectedAudioInput();
        if (entry.deviceName.isEmpty() && !filteredInputIndices.isEmpty())
        { cmbAudioInputDevice.setSelectedId(1, juce::dontSendNotification); entry = getSelectedAudioInput(); }
        if (entry.deviceName.isEmpty())
        {
            eng.stopAudioBpm();
            return;
        }
        typeName   = entry.typeName;
        deviceName = entry.deviceName;
        channel = cmbAudioInputChannel.getSelectedId() - 1;
        if (channel < 0) channel = 0;
    }

    double sr = getPreferredSampleRate();
    int bs = getPreferredBufferSize();
    eng.startAudioBpm(typeName, deviceName, channel, sr, bs);
    float gain = ltcActive ? (float)sldBpmInputGain.getValue() / 100.0f
                           : (float)sldLtcInputGain.getValue() / 100.0f;
    eng.getAudioBpmInput().setInputGain(gain);
    if (!ltcActive)
        populateAudioInputChannels();
    else
        populateAudioBpmChannels();
}

int MainComponent::findLinkOwnerOtherThan(int engineIdx) const
{
    for (int i = 0; i < (int)engines.size(); i++)
    {
        if (i == engineIdx) continue;
        if (engines[(size_t)i]->getLinkBridge().isEnabled())
            return i;
    }
    return -1;
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
    bool showMidiIn   = (input == SrcType::MTC)    && inputConfigExpanded;
    bool showArtnetIn = (input == SrcType::ArtNet)  && inputConfigExpanded;
    bool showHippoIn  = (input == SrcType::Hippotizer) && inputConfigExpanded;
    bool showLtcIn    = (input == SrcType::LTC)     && inputConfigExpanded;
    bool showGenerator = (input == SrcType::SystemTime) && inputConfigExpanded;
    bool showProDJLinkIn = (input == SrcType::ProDJLink || input == SrcType::StageLinQ) && inputConfigExpanded;
    bool showAudioOut = (eng.isOutputLtcEnabled() || (eng.isPrimary() && eng.isOutputThruEnabled()));
    bool hasInputConfig = true;  // all sources have expandable config now

    btnCollapseInput.setVisible(hasInputConfig);
    updateCollapseButtonText(btnCollapseInput, inputConfigExpanded);

    cmbMidiInputDevice.setVisible(showMidiIn);       lblMidiInputDevice.setVisible(showMidiIn);
    cmbArtnetInputInterface.setVisible(showArtnetIn); lblArtnetInputInterface.setVisible(showArtnetIn);
    cmbHippoInputInterface.setVisible(showHippoIn);   lblHippoInputInterface.setVisible(showHippoIn);
    cmbHippoTcChannel.setVisible(showHippoIn);        lblHippoTcChannel.setVisible(showHippoIn);
    btnGenClock.setVisible(showGenerator);
    bool showGenTransport = showGenerator && !eng.getGeneratorClockMode();
    btnGenPlay.setVisible(showGenTransport);   btnGenPause.setVisible(showGenTransport);  btnGenStop.setVisible(showGenTransport);
    txtGenStartTC.setVisible(showGenTransport); lblGenStartTC.setVisible(showGenTransport);
    txtGenStopTC.setVisible(showGenTransport);  lblGenStopTC.setVisible(showGenTransport);
    cmbGenPreset.setVisible(showGenTransport);  lblGenPreset.setVisible(showGenTransport);
    btnGenPrev.setVisible(showGenTransport);    btnGenNext.setVisible(showGenTransport);
    btnGenGo.setVisible(showGenTransport);      btnGenPresetEdit.setVisible(showGenTransport);
    btnOscIn.setVisible(showGenerator);
    bool showOscInConfig = showGenerator;
    cmbOscInputInterface.setVisible(showOscInConfig);  lblOscInputInterface.setVisible(showOscInConfig);
    lblOscInPort.setVisible(showOscInConfig);           txtOscInPort.setVisible(showOscInConfig);
    lblOscInStatus.setVisible(showOscInConfig && lblOscInStatus.getText().isNotEmpty());
    btnTrackMap.setVisible(showProDJLinkIn);
    btnTrackMapEdit.setVisible(showProDJLinkIn);
    btnProDJLinkView.setVisible(showProDJLinkIn && input == SrcType::ProDJLink);
    btnStageLinQView.setVisible(showProDJLinkIn && input == SrcType::StageLinQ);
    btnMixerMapEdit.setVisible(showProDJLinkIn);
    if (input == SrcType::StageLinQ)
    {
        juce::Colour slqAccent(0xFF00CC66);
        btnMixerMapEdit.setColour(juce::TextButton::buttonColourId, slqAccent.withAlpha(0.15f));
        btnMixerMapEdit.setColour(juce::TextButton::textColourOffId, slqAccent.brighter(0.3f));
    }
    else
    {
        juce::Colour pdl(0xFF00AAFF);
        btnMixerMapEdit.setColour(juce::TextButton::buttonColourId, pdl.withAlpha(0.15f));
        btnMixerMapEdit.setColour(juce::TextButton::textColourOffId, pdl.brighter(0.3f));
    }
    btnBpmOff.setVisible(showProDJLinkIn);
    btnBpmX2.setVisible(showProDJLinkIn);
    btnBpmX4.setVisible(showProDJLinkIn);
    btnBpmD2.setVisible(showProDJLinkIn);
    btnBpmD4.setVisible(showProDJLinkIn);

    // Audio BPM: available for non-DJ sources (MTC, LTC, ArtNet, SystemTime)
    // For SystemTime there is no collapse button, so show unconditionally.
    bool isNonDjSource = (input != SrcType::ProDJLink && input != SrcType::StageLinQ);
    bool showAudioBpmToggle = isNonDjSource
        && (input == SrcType::SystemTime || inputConfigExpanded);
    bool showAudioBpmActive = showAudioBpmToggle && btnAudioBpm.getToggleState();
    btnAudioBpm.setVisible(showAudioBpmToggle);
    lblBpmValue.setVisible(showAudioBpmActive);
    ledBeat.setVisible(showAudioBpmActive);
    sldBpmSmoothing.setVisible(showAudioBpmActive);  lblBpmSmoothing.setVisible(showAudioBpmActive);

    // When LTC is active, Audio BPM uses its OWN separate device/channel combos
    // (the shared AUDIO SETTINGS section belongs to LTC).
    // When NOT LTC, Audio BPM shares the AUDIO SETTINGS section.
    bool bpmUsesSeparateAudio = showAudioBpmActive && showLtcIn;
    bool bpmUsesSharedAudio   = showAudioBpmActive && !showLtcIn;
    cmbAudioBpmDevice.setVisible(bpmUsesSeparateAudio);  lblAudioBpmDevice.setVisible(bpmUsesSeparateAudio);
    cmbAudioBpmChannel.setVisible(bpmUsesSeparateAudio); lblAudioBpmChannel.setVisible(bpmUsesSeparateAudio);
    sldBpmInputGain.setVisible(bpmUsesSeparateAudio);    lblBpmInputGain.setVisible(bpmUsesSeparateAudio);
    mtrAudioBpm.setVisible(bpmUsesSeparateAudio);

    // BPM output controls: visible for DJ sources OR when audio BPM is active
    bool showBpmOutputs = showProDJLinkIn || showAudioBpmActive;
    btnMidiClock.setVisible(showBpmOutputs);
    btnOscFwdBpm.setVisible(showBpmOutputs);
    edOscFwdBpmAddr.setVisible(showBpmOutputs && btnOscFwdBpm.getToggleState());
    lblOscFwdBpmAddr.setVisible(showBpmOutputs && btnOscFwdBpm.getToggleState());
    edOscFwdBpmCmd.setVisible(showBpmOutputs && btnOscFwdBpm.getToggleState());
    lblOscFwdBpmCmd.setVisible(showBpmOutputs && btnOscFwdBpm.getToggleState());
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
    btnLink.setVisible(showBpmOutputs);
    bool linkOwnedElsewhere = findLinkOwnerOtherThan(selectedEngine) >= 0;
    lblLinkStatus.setVisible(showBpmOutputs && (btnLink.getToggleState() || linkOwnedElsewhere));

    btnTriggerMidi.setVisible(showProDJLinkIn);
    cmbTriggerMidiDevice.setVisible(showProDJLinkIn
        && (btnTriggerMidi.getToggleState() || btnMidiClock.getToggleState()
            || btnMidiMixerFwd.getToggleState())
        || (showAudioBpmActive && btnMidiClock.getToggleState()));
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
    bool showOscConfig = (showProDJLinkIn || showAudioBpmActive)
        && (btnOscFwdBpm.getToggleState() || btnOscMixerFwd.getToggleState());
    bool showOscConfigFull = showOscConfig
        || (showProDJLinkIn && btnTriggerOsc.getToggleState());
    edOscIp.setVisible(showOscConfigFull);
    edOscPort.setVisible(showOscConfigFull);

    // Pro DJ Link / StageLinQ -- separate interface combos
    bool showPdlIface = showProDJLinkIn && input == SrcType::ProDJLink;
    bool showSlqIface = showProDJLinkIn && input == SrcType::StageLinQ;
    cmbProDJLinkInterface.setVisible(showPdlIface);  lblProDJLinkInterface.setVisible(showPdlIface);
    cmbStageLinQInterface.setVisible(showSlqIface);   lblStageLinQInterface.setVisible(showSlqIface);
    cmbProDJLinkPlayer.setVisible(showProDJLinkIn);     lblProDJLinkPlayer.setVisible(showProDJLinkIn);

    // Repopulate player combo based on input source:
    // ProDJLink: players 1-6 + XF-A + XF-B
    // StageLinQ: decks 1-4 + XF-A + XF-B
    {
        int prevId = cmbProDJLinkPlayer.getSelectedId();
        cmbProDJLinkPlayer.clear(juce::dontSendNotification);
        if (input == SrcType::StageLinQ)
        {
            for (int i = 1; i <= StageLinQ::kMaxDecks; ++i)
                cmbProDJLinkPlayer.addItem("DECK " + juce::String(i), i);
            cmbProDJLinkPlayer.addItem("XF-A", 7);
            cmbProDJLinkPlayer.addItem("XF-B", 8);
            if (prevId < 1 || prevId > 8) prevId = 1;
        }
        else
        {
            for (int i = 1; i <= ProDJLink::kMaxPlayers; ++i)
                cmbProDJLinkPlayer.addItem("PLAYER " + juce::String(i), i);
            cmbProDJLinkPlayer.addItem("XF-A", 7);
            cmbProDJLinkPlayer.addItem("XF-B", 8);
            if (prevId < 1 || prevId > 8) prevId = 1;
        }
        cmbProDJLinkPlayer.setSelectedId(prevId, juce::dontSendNotification);
    }
    lblProDJLinkMetadata.setVisible(showProDJLinkIn);
    lblNextCue.setVisible(showProDJLinkIn);
    lblProDJLinkTrackInfo.setVisible(showProDJLinkIn);
    lblMixerStatus.setVisible(showProDJLinkIn);
    artworkDisplay.setVisible(showProDJLinkIn);
    waveformDisplay.setVisible(showProDJLinkIn);

    bool showAudioInput = showLtcIn || bpmUsesSharedAudio;
    cmbAudioInputTypeFilter.setVisible(showAudioInput);  lblAudioInputTypeFilter.setVisible(showAudioInput);
    cmbSampleRate.setVisible(showAudioInput);             lblSampleRate.setVisible(showAudioInput);
    cmbBufferSize.setVisible(showAudioInput);             lblBufferSize.setVisible(showAudioInput);
    cmbAudioInputDevice.setVisible(showAudioInput);       lblAudioInputDevice.setVisible(showAudioInput);
    cmbAudioInputChannel.setVisible(showAudioInput);      lblAudioInputChannel.setVisible(showAudioInput);
    sldLtcInputGain.setVisible(showAudioInput);           lblLtcInputGain.setVisible(showAudioInput);
    mtrLtcInput.setVisible(showAudioInput);

    // Dynamic labels: adapt to LTC vs Audio BPM context
    if (bpmUsesSharedAudio)
    {
        lblAudioInputChannel.setText("BPM CHANNEL:", juce::dontSendNotification);
        lblLtcInputGain.setText("BPM INPUT GAIN:", juce::dontSendNotification);
    }
    else
    {
        lblAudioInputChannel.setText("LTC CHANNEL:", juce::dontSendNotification);
        lblLtcInputGain.setText("LTC INPUT GAIN:", juce::dontSendNotification);
    }

    bool showThruInput = showLtcIn && eng.isPrimary() && eng.isOutputThruEnabled();
    cmbThruInputChannel.setVisible(showThruInput);  lblThruInputChannel.setVisible(showThruInput);
    sldThruInputGain.setVisible(showThruInput);     lblThruInputGain.setVisible(showThruInput);
    mtrThruInput.setVisible(showThruInput);
    lblInputStatus.setVisible(true);

    // FPS Convert: not applicable for ProDJLink/StageLinQ (user selects fps directly)
    bool showFpsConvert = (input != SrcType::ProDJLink && input != SrcType::StageLinQ);
    btnFpsConvert.setVisible(showFpsConvert);
    if (!showFpsConvert && eng.isFpsConvertEnabled())
    {
        eng.setFpsConvertEnabled(false);
        btnFpsConvert.setToggleState(false, juce::dontSendNotification);
    }

    // Close TrackMap / MixerMap editor windows when no engine uses ProDJLink/StageLinQ
    // (these are shared windows -- keep open as long as any engine is on PDL/SLQ)
    {
        bool anyPdl = false;
        for (auto& e : engines)
            if (e->getActiveInput() == SrcType::ProDJLink || e->getActiveInput() == SrcType::StageLinQ) { anyPdl = true; break; }
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

    // TCNet interface combo: visible when TCNET OUT is enabled
    bool showTcnetConfig = eng.isOutputTcnetEnabled();
    cmbTcnetInterface.setVisible(showTcnetConfig);  lblTcnetInterface.setVisible(showTcnetConfig);
    cmbTcnetLayer.setVisible(showTcnetConfig);      lblTcnetLayer.setVisible(showTcnetConfig);
    sldTcnetOffset.setVisible(showTcnetConfig);     lblTcnetOffset.setVisible(showTcnetConfig);
    if (showTcnetConfig) repopulateTcnetLayerCombo();

    // Hippotizer output: disabled in this version (pending hardware validation)
    btnHippoOut.setVisible(false);
    txtHippoDestIp.setVisible(false);      lblHippoDestIp.setVisible(false);
    lblHippoOutStatus.setVisible(false);

    bool anyDevice = (input != SrcType::SystemTime) || eng.isOutputMtcEnabled() || eng.isOutputArtnetEnabled() || eng.isOutputLtcEnabled() || (eng.isPrimary() && eng.isOutputThruEnabled());
    btnRefreshDevices.setVisible(anyDevice);

    resized();
    leftContent.repaint();
    rightContent.repaint();

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

    // Hippotizer output status
    if (eng.isOutputHippoEnabled())
    {
        juce::String hippoStatus;
        if (eng.getHippotizerOutput().getIsRunning())
            hippoStatus = eng.getHippotizerOutput().isPaused() ? "PAUSED" : eng.getHippoOutStatusText();
        else
            hippoStatus = eng.getHippoOutStatusText();

        // Append discovery hint if input detected a Hippotizer on the network
        if (eng.getHippotizerInput().isDiscovered())
        {
            auto discName = eng.getHippotizerInput().getDiscoveredName();
            auto discIp   = eng.getHippotizerInput().getDiscoveredIp();
            if (discName.isNotEmpty())
            {
                if (hippoStatus.isNotEmpty()) hippoStatus += " | ";
                hippoStatus += discName + " @ " + discIp;
            }
        }

        lblHippoOutStatus.setText(hippoStatus, juce::dontSendNotification);
    }
    else
    {
        lblHippoOutStatus.setText("", juce::dontSendNotification);
    }

    // Generator transport button highlighting
    if (btnGenPlay.isVisible())
    {
        auto state = eng.getGeneratorState();
        auto activeCol  = juce::Colour(0xFF22AA44);  // green
        auto pauseCol   = juce::Colour(0xFFCC8800);  // amber
        auto defaultCol = juce::Colour(0xFF444444);

        btnGenPlay .setColour(juce::TextButton::buttonColourId,
            state == TimecodeEngine::GeneratorState::Playing ? activeCol : defaultCol);
        btnGenPause.setColour(juce::TextButton::buttonColourId,
            state == TimecodeEngine::GeneratorState::Paused ? pauseCol : defaultCol);
        btnGenStop .setColour(juce::TextButton::buttonColourId,
            state == TimecodeEngine::GeneratorState::Stopped ? juce::Colour(0xFF666666) : defaultCol);
    }
}

void MainComponent::updateNextCueLabel(TimecodeEngine& eng)
{
    auto info = eng.getNextCueInfo();
    if (!info.valid || info.remainingMs < 0)
    {
        lblNextCue.setText("", juce::dontSendNotification);
        return;
    }

    // Format remaining time as M:SS
    int totalSec = info.remainingMs / 1000;
    int mins = totalSec / 60;
    int secs = totalSec % 60;

    juce::String cueName = info.name.isNotEmpty() ? info.name : "CUE";
    juce::String countdown = juce::String(mins) + ":"
                           + juce::String(secs).paddedLeft('0', 2);

    juce::String text = juce::String::charToString(0x25B6) + " NEXT: "
                      + cueName + " in " + countdown;

    lblNextCue.setText(text, juce::dontSendNotification);

    // Color: amber normally, red when close (< 10s), flash bright on < 3s
    if (totalSec < 3)
        lblNextCue.setColour(juce::Label::textColourId, juce::Colour(0xFFFF4444));
    else if (totalSec < 10)
        lblNextCue.setColour(juce::Label::textColourId, juce::Colour(0xFFFF6600));
    else
        lblNextCue.setColour(juce::Label::textColourId, juce::Colour(0xFFFFAA00));
}

double MainComponent::parseTimecodeToMs(const juce::String& tc, FrameRate fps)
{
    // Parse "HH:MM:SS:FF" or "HH:MM:SS.FF" → ms from midnight
    auto parts = juce::StringArray::fromTokens(tc, ":.", "");
    int h = 0, m = 0, s = 0, f = 0;
    if (parts.size() >= 1) h = parts[0].getIntValue();
    if (parts.size() >= 2) m = parts[1].getIntValue();
    if (parts.size() >= 3) s = parts[2].getIntValue();
    if (parts.size() >= 4) f = parts[3].getIntValue();

    double fpsVal = 30.0;
    switch (fps)
    {
        case FrameRate::FPS_2398: fpsVal = 24000.0 / 1001.0; break;
        case FrameRate::FPS_24:   fpsVal = 24.0; break;
        case FrameRate::FPS_25:   fpsVal = 25.0; break;
        case FrameRate::FPS_2997: fpsVal = 30000.0 / 1001.0; break;
        case FrameRate::FPS_30:   fpsVal = 30.0; break;
    }

    double ms = (double)h * 3600000.0
              + (double)m * 60000.0
              + (double)s * 1000.0
              + (double)f * (1000.0 / fpsVal);
    return ms;
}

juce::String MainComponent::msToTimecodeString(double ms, FrameRate fps)
{
    if (ms < 0.0) ms = 0.0;

    double fpsVal = 30.0;
    switch (fps)
    {
        case FrameRate::FPS_2398: fpsVal = 24000.0 / 1001.0; break;
        case FrameRate::FPS_24:   fpsVal = 24.0; break;
        case FrameRate::FPS_25:   fpsVal = 25.0; break;
        case FrameRate::FPS_2997: fpsVal = 30000.0 / 1001.0; break;
        case FrameRate::FPS_30:   fpsVal = 30.0; break;
    }

    int totalSec = (int)(ms / 1000.0);
    double remainder = ms - (double)totalSec * 1000.0;
    int h = totalSec / 3600;
    int m = (totalSec % 3600) / 60;
    int s = totalSec % 60;
    int f = (int)(remainder / (1000.0 / fpsVal));

    return juce::String(h).paddedLeft('0', 2) + ":"
         + juce::String(m).paddedLeft('0', 2) + ":"
         + juce::String(s).paddedLeft('0', 2) + ":"
         + juce::String(f).paddedLeft('0', 2);
}

void MainComponent::populateGenPresetCombo()
{
    cmbGenPreset.clear(juce::dontSendNotification);
    auto presets = settings.generatorPresets.getAllSorted();
    int id = 1;
    for (auto& p : presets)
        cmbGenPreset.addItem(p.name, id++);
}

void MainComponent::activateGenPreset(const juce::String& name)
{
    loadGenPresetToFields(name);

    auto& eng = currentEngine();

    // Switch to transport mode if in clock mode
    if (eng.getGeneratorClockMode())
    {
        eng.setGeneratorClockMode(false);
        btnGenClock.setToggleState(false, juce::dontSendNotification);
        updateDeviceSelectorVisibility();
        resized();
    }

    auto fps = eng.getCurrentFps();
    double startMs = parseTimecodeToMs(txtGenStartTC.getText(), fps);
    double stopMs  = parseTimecodeToMs(txtGenStopTC.getText(), fps);

    eng.generatorStop();
    eng.setGeneratorStartMs(startMs);
    eng.setGeneratorStopMs(stopMs);

    eng.generatorPlay();
    saveSettings();
}

void MainComponent::loadGenPresetToFields(const juce::String& name)
{
    auto* preset = settings.generatorPresets.find(name);
    if (!preset) return;

    auto& eng = currentEngine();
    auto fps = eng.getCurrentFps();

    // Fill TC editors with normalized values
    juce::String startNorm = msToTimecodeString(parseTimecodeToMs(preset->startTC, fps), fps);
    juce::String stopNorm  = msToTimecodeString(parseTimecodeToMs(preset->stopTC, fps), fps);
    txtGenStartTC.setText(startNorm, false);
    txtGenStopTC.setText(stopNorm, false);

    // Apply to engine (without starting playback)
    eng.setGeneratorStartMs(parseTimecodeToMs(preset->startTC, fps));
    eng.setGeneratorStopMs(parseTimecodeToMs(preset->stopTC, fps));
    saveSettings();
}

void MainComponent::setupOscInputServer()
{
    // Populate network interface combo
    oscInputServer.refreshNetworkInterfaces();
    cmbOscInputInterface.clear(juce::dontSendNotification);
    auto names = oscInputServer.getInterfaceNames();
    for (int i = 0; i < names.size(); ++i)
        cmbOscInputInterface.addItem(names[i], i + 1);
    cmbOscInputInterface.setSelectedId(settings.oscInputInterface + 1, juce::dontSendNotification);

    oscInputServer.onMessage = [this](const OscInputServer::Message& msg)
    {
        // Dispatch on message thread (callback runs on listener thread)
        juce::MessageManager::callAsync([this, msg]()
        {
            auto addr = msg.address.trimEnd();

            // Parse engine number: /stc/N/gen/... (N = 1-8, required)
            int targetEngine = -1;
            juce::String cmd;

            if (addr.startsWith("/stc/") && addr.length() > 6)
            {
                auto afterStc = addr.substring(5);  // "N/gen/play"
                int slashPos = afterStc.indexOf("/");
                if (slashPos > 0)
                {
                    auto numPart = afterStc.substring(0, slashPos);
                    int n = numPart.getIntValue();
                    if (n >= 1 && n <= (int)engines.size())
                    {
                        targetEngine = n - 1;
                        cmd = "/stc/" + afterStc.substring(slashPos + 1);  // "/stc/gen/play"
                    }
                }
            }

            if (targetEngine < 0 || targetEngine >= (int)engines.size()) return;
            auto& eng = *engines[(size_t)targetEngine];

            // --- Generator commands ---
            if (cmd == "/stc/gen/play")
            {
                eng.generatorPlay();
            }
            else if (cmd == "/stc/gen/pause")
            {
                eng.generatorPause();
            }
            else if (cmd == "/stc/gen/stop")
            {
                eng.generatorStop();
            }
            else if (cmd == "/stc/gen/clock")
            {
                int val = msg.getInt(0, -1);
                if (val >= 0)
                {
                    eng.setGeneratorClockMode(val != 0);
                    if (targetEngine == selectedEngine)
                    {
                        btnGenClock.setToggleState(eng.getGeneratorClockMode(), juce::dontSendNotification);
                        updateDeviceSelectorVisibility();
                        resized();
                    }
                }
            }
            else if (cmd == "/stc/gen/start")
            {
                auto tc = msg.getString(0);
                if (tc.isNotEmpty())
                {
                    double ms = parseTimecodeToMs(tc, eng.getCurrentFps());
                    eng.setGeneratorStartMs(ms);
                    if (targetEngine == selectedEngine)
                        txtGenStartTC.setText(tc, false);
                    saveSettings();
                }
            }
            else if (cmd == "/stc/gen/stoptime")
            {
                auto tc = msg.getString(0);
                if (tc.isNotEmpty())
                {
                    double ms = parseTimecodeToMs(tc, eng.getCurrentFps());
                    eng.setGeneratorStopMs(ms);
                    if (targetEngine == selectedEngine)
                        txtGenStopTC.setText(tc, false);
                    saveSettings();
                }
            }
            else if (cmd == "/stc/gen/preset")
            {
                auto name = msg.getString(0);
                if (name.isNotEmpty())
                {
                    auto* preset = settings.generatorPresets.find(name);
                    if (preset)
                    {
                        auto fps = eng.getCurrentFps();
                        if (eng.getGeneratorClockMode())
                            eng.setGeneratorClockMode(false);
                        eng.generatorStop();
                        eng.setGeneratorStartMs(parseTimecodeToMs(preset->startTC, fps));
                        eng.setGeneratorStopMs(parseTimecodeToMs(preset->stopTC, fps));
                        eng.generatorPlay();

                        // Update UI if this is the selected engine
                        if (targetEngine == selectedEngine)
                        {
                            btnGenClock.setToggleState(false, juce::dontSendNotification);
                            txtGenStartTC.setText(preset->startTC, false);
                            txtGenStopTC.setText(preset->stopTC, false);
                            updateDeviceSelectorVisibility();
                            resized();
                        }
                        saveSettings();
                    }
                }
            }
        });
    };
}

void MainComponent::startOscInput()
{
    int port = settings.oscInputPort;
    int iface = settings.oscInputInterface;
    if (oscInputServer.start(port, iface))
    {
        settings.oscInputEnabled = true;
        btnOscIn.setToggleState(true, juce::dontSendNotification);
        int actualId = oscInputServer.getSelectedInterface() + 1;
        cmbOscInputInterface.setSelectedId(actualId, juce::dontSendNotification);
        lblOscInStatus.setText("LISTENING ON " + oscInputServer.getBindInfo(), juce::dontSendNotification);
        lblOscInStatus.setColour(juce::Label::textColourId, juce::Colour(0xFF44CC44));
    }
    else
    {
        btnOscIn.setToggleState(false, juce::dontSendNotification);
        settings.oscInputEnabled = false;
        lblOscInStatus.setText("BIND FAILED - PORT " + juce::String(port), juce::dontSendNotification);
        lblOscInStatus.setColour(juce::Label::textColourId, juce::Colour(0xFFFF4444));
    }
    updateDeviceSelectorVisibility();
    resized();
}

void MainComponent::stopOscInput()
{
    oscInputServer.stop();
    settings.oscInputEnabled = false;
    btnOscIn.setToggleState(false, juce::dontSendNotification);
    lblOscInStatus.setText("", juce::dontSendNotification);
    updateDeviceSelectorVisibility();
    resized();
}

void MainComponent::openGeneratorPresetEditor()
{
    if (genPresetWindow != nullptr)
    {
        genPresetWindow->toFront(true);
        return;
    }

    auto* editor = new GeneratorPresetEditor(settings.generatorPresets);

    editor->onChange = [this]
    {
        populateGenPresetCombo();
    };

    struct FloatingWindow : juce::DocumentWindow
    {
        FloatingWindow(const juce::String& t, juce::Colour bg)
            : DocumentWindow(t, bg, DocumentWindow::closeButton | DocumentWindow::maximiseButton) {}
        void closeButtonPressed() override { if (onClose) onClose(); delete this; }
        std::function<void()> onClose;
    };

    auto* win = new FloatingWindow("Generator Presets", juce::Colour(0xFF12141A));
    win->setContentOwned(editor, true);
    win->setUsingNativeTitleBar(false);
    win->setTitleBarHeight(20);
    win->setColour(juce::DocumentWindow::textColourId, juce::Colour(0xFF546E7A));
    win->setResizable(true, true);
    win->centreWithSize(editor->getWidth(), editor->getHeight());
    if (settings.genPresetBounds.isNotEmpty())
    {
        auto parts = juce::StringArray::fromTokens(settings.genPresetBounds, " ", "");
        if (parts.size() == 4)
        {
            auto b = juce::Rectangle<int>(parts[0].getIntValue(), parts[1].getIntValue(),
                                           parts[2].getIntValue(), parts[3].getIntValue());
            if (b.getWidth() >= 200 && b.getHeight() >= 150)
            {
                auto c = b.getCentre();
                for (auto& d : juce::Desktop::getInstance().getDisplays().displays)
                    if (d.totalArea.contains(c)) { win->setBounds(b); break; }
            }
        }
    }
    win->onClose = [this, win]()
    {
        auto b = win->getBounds();
        settings.genPresetBounds = juce::String(b.getX()) + " " + juce::String(b.getY())
                                 + " " + juce::String(b.getWidth()) + " " + juce::String(b.getHeight());
        saveSettings();
    };
    win->setVisible(true);
    genPresetWindow = win;
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

        // Show Lock button on the right edge -- reserve space
        int lockBtnW = juce::jlimit(70, 90, (getWidth() - 400) / 10 + 70);
        int rightUsed = pad + lockBtnW + tabGap;

        int availableW = getWidth() - leftUsed - rightUsed;
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

        // Center tabs relative to the full window width (not just remaining space)
        int tabX = (getWidth() - totalTabsW) / 2;
        // Clamp so tabs don't overlap backup/restore or show lock buttons
        tabX = juce::jmax(leftUsed, tabX);
        int maxTabRight = getWidth() - rightUsed;
        if (tabX + totalTabsW > maxTabRight)
            tabX = juce::jmax(leftUsed, maxTabRight - totalTabsW);

        for (int i = 0; i < numTabs; i++)
        {
            tabButtons[(size_t)i]->setBounds(tabX, topBar + 2, tabW, tabBar - 4);
            tabX += tabW + tabGap;
        }
        btnAddEngine.setBounds(tabX, topBar + 2, addBtnW, tabBar - 4);

        // Show Lock button on the right edge of the tab bar
        btnShowLock.setBounds(getWidth() - pad - lockBtnW, topBar + 2, lockBtnW, tabBar - 4);
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
    struct IBI { juce::TextButton* btn; SrcType src; };
    IBI iBtns[] = { {&btnMtcIn,SrcType::MTC}, {&btnArtnetIn,SrcType::ArtNet}, {&btnHippoIn,SrcType::Hippotizer}, {&btnSysTime,SrcType::SystemTime}, {&btnLtcIn,SrcType::LTC}, {&btnProDJLinkIn,SrcType::ProDJLink}, {&btnStageLinQIn,SrcType::StageLinQ} };
    for (auto& ib : iBtns) { if (!ib.btn->isVisible()) continue; ib.btn->setBounds(leftPanel.removeFromTop(btnH)); leftPanel.removeFromTop(btnG); }

    // Section separator after input source buttons
    leftContent.addSectionSeparator(leftPanel.getY());
    leftPanel.removeFromTop(4);

    if (btnCollapseInput.isVisible())
    { leftPanel.removeFromTop(2); btnCollapseInput.setBounds(leftPanel.removeFromTop(18)); leftPanel.removeFromTop(4); }

    if (cmbMidiInputDevice.isVisible()) layCombo(lblMidiInputDevice, cmbMidiInputDevice, leftPanel);
    if (cmbArtnetInputInterface.isVisible()) layCombo(lblArtnetInputInterface, cmbArtnetInputInterface, leftPanel);
    if (cmbHippoInputInterface.isVisible()) layCombo(lblHippoInputInterface, cmbHippoInputInterface, leftPanel);
    if (cmbHippoTcChannel.isVisible()) layCombo(lblHippoTcChannel, cmbHippoTcChannel, leftPanel);

    // Generator clock toggle + transport + start/stop TC
    if (btnGenClock.isVisible())
    {
        leftPanel.removeFromTop(3);
        btnGenClock.setBounds(leftPanel.removeFromTop(22));
    }
    if (btnGenPlay.isVisible())
    {
        leftPanel.removeFromTop(3);
        auto transportRow = leftPanel.removeFromTop(24);
        int btnW = (transportRow.getWidth() - 6) / 3;
        btnGenPlay .setBounds(transportRow.removeFromLeft(btnW));
        transportRow.removeFromLeft(3);
        btnGenPause.setBounds(transportRow.removeFromLeft(btnW));
        transportRow.removeFromLeft(3);
        btnGenStop .setBounds(transportRow);

        leftPanel.removeFromTop(4);
        auto startRow = leftPanel.removeFromTop(20);
        lblGenStartTC.setBounds(startRow.removeFromLeft(80));
        txtGenStartTC.setBounds(startRow);

        leftPanel.removeFromTop(3);
        auto stopRow = leftPanel.removeFromTop(20);
        lblGenStopTC.setBounds(stopRow.removeFromLeft(80));
        txtGenStopTC.setBounds(stopRow);

        leftPanel.removeFromTop(4);
        auto presetRow = leftPanel.removeFromTop(22);
        lblGenPreset.setBounds(presetRow.removeFromLeft(50));
        btnGenPresetEdit.setBounds(presetRow.removeFromRight(36));
        presetRow.removeFromRight(3);
        btnGenGo.setBounds(presetRow.removeFromRight(32));
        presetRow.removeFromRight(3);
        btnGenNext.setBounds(presetRow.removeFromRight(22));
        presetRow.removeFromRight(2);
        btnGenPrev.setBounds(presetRow.removeFromRight(22));
        presetRow.removeFromRight(2);
        cmbGenPreset.setBounds(presetRow);
        leftPanel.removeFromTop(3);
    }

    // OSC Input toggle + config
    if (btnOscIn.isVisible())
    {
        leftPanel.removeFromTop(2);
        btnOscIn.setBounds(leftPanel.removeFromTop(22));
        leftPanel.removeFromTop(3);
    }
    if (cmbOscInputInterface.isVisible())
    {
        layCombo(lblOscInputInterface, cmbOscInputInterface, leftPanel);
        auto portRow = leftPanel.removeFromTop(20);
        lblOscInPort.setBounds(portRow.removeFromLeft(70));
        txtOscInPort.setBounds(portRow.removeFromLeft(60));
        leftPanel.removeFromTop(2);
    }
    if (lblOscInStatus.isVisible())
    {
        lblOscInStatus.setBounds(leftPanel.removeFromTop(13));
        leftPanel.removeFromTop(3);
    }

    if (cmbProDJLinkInterface.isVisible())
    {
        layCombo(lblProDJLinkInterface, cmbProDJLinkInterface, leftPanel);
    }
    if (cmbStageLinQInterface.isVisible())
    {
        layCombo(lblStageLinQInterface, cmbStageLinQInterface, leftPanel);
    }
    if (cmbProDJLinkPlayer.isVisible())
    {
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

        // --- Next cue countdown ---
        if (lblNextCue.isVisible())
        {
            lblNextCue.setBounds(leftPanel.removeFromTop(14));
            leftPanel.removeFromTop(2);
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
            btnStageLinQView.setBounds(btnRow);  // same slot, only one is visible
            leftPanel.removeFromTop(3);

            // Row 2: TrackMap toggle (full width)
            btnTrackMap.setBounds(leftPanel.removeFromTop(22));
            leftPanel.removeFromTop(3);
        }
    }

    // === Feature sections (shared: work for ProDJLink AND Audio BPM) ===

    // --- Audio BPM toggle (non-DJ sources only) ---
    bool bpmDriven = btnAudioBpm.getToggleState() && eng.getActiveInput() != SrcType::LTC;
    bool bpmInline = btnAudioBpm.isVisible() && btnAudioBpm.getToggleState();  // BPM outputs go inline

    if (btnAudioBpm.isVisible())
    {
        leftContent.addSectionSeparator(leftPanel.getY(), "AUDIO BPM");
        leftPanel.removeFromTop(16);
        btnAudioBpm.setBounds(leftPanel.removeFromTop(22));
        leftPanel.removeFromTop(3);

        // BPM value display + beat LED (same row)
        if (lblBpmValue.isVisible())
        {
            auto bpmRow = leftPanel.removeFromTop(24);
            ledBeat.setBounds(bpmRow.removeFromLeft(20).reduced(2));
            bpmRow.removeFromLeft(2);
            lblBpmValue.setBounds(bpmRow);
            leftPanel.removeFromTop(3);
        }

        if (sldBpmSmoothing.isVisible())
            laySlider(lblBpmSmoothing, sldBpmSmoothing, leftPanel);
        // When LTC is the source, Audio BPM has its own separate device/channel controls
        if (cmbAudioBpmDevice.isVisible())
        {
            layCombo(lblAudioBpmDevice, cmbAudioBpmDevice, leftPanel);
            layCombo(lblAudioBpmChannel, cmbAudioBpmChannel, leftPanel);
            if (sldBpmInputGain.isVisible())
                laySlider(lblBpmInputGain, sldBpmInputGain, leftPanel);
            if (mtrAudioBpm.isVisible()) layMeter(mtrAudioBpm, leftPanel);
        }
    }

    // Shared audio device layout block (used by both LTC and Audio BPM)
    auto layAudioInputSettings = [&]()
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
    };

    // When Audio BPM is active (non-LTC), audio settings go right here after the toggle
    if (bpmDriven && cmbAudioInputDevice.isVisible())
        layAudioInputSettings();

    // BPM output toggles inline (MIDI Clock, OSC Tempo, Link) — after audio settings
    if (bpmInline)
    {
        if (btnMidiClock.isVisible())
        {
            btnMidiClock.setBounds(leftPanel.removeFromTop(22));
            leftPanel.removeFromTop(3);
        }
        if (btnOscFwdBpm.isVisible())
        {
            btnOscFwdBpm.setBounds(leftPanel.removeFromTop(22));
            if (edOscFwdBpmAddr.isVisible())
            {
                leftPanel.removeFromTop(2);
                auto bpmAddrRow = leftPanel.removeFromTop(22);
                lblOscFwdBpmAddr.setBounds(bpmAddrRow.removeFromLeft(40));
                bpmAddrRow.removeFromLeft(2);
                edOscFwdBpmAddr.setBounds(bpmAddrRow);

                leftPanel.removeFromTop(2);
                auto cmdRow = leftPanel.removeFromTop(22);
                lblOscFwdBpmCmd.setBounds(cmdRow.removeFromLeft(40));
                cmdRow.removeFromLeft(2);
                edOscFwdBpmCmd.setBounds(cmdRow);
            }
            if (edOscIp.isVisible())
            {
                leftPanel.removeFromTop(2);
                auto oscConfRow = leftPanel.removeFromTop(22);
                edOscIp.setBounds(oscConfRow.removeFromLeft(oscConfRow.getWidth() - 58));
                oscConfRow.removeFromLeft(4);
                edOscPort.setBounds(oscConfRow);
            }
            leftPanel.removeFromTop(3);
        }
        if (btnLink.isVisible())
        {
            btnLink.setBounds(leftPanel.removeFromTop(22));
            if (lblLinkStatus.isVisible())
            {
                leftPanel.removeFromTop(2);
                lblLinkStatus.setBounds(leftPanel.removeFromTop(13));
            }
            leftPanel.removeFromTop(3);
        }
    }

    // --- MIDI section (skip BPM-only toggles already placed inline) ---
    bool hasMidiSection = (!bpmInline && btnMidiClock.isVisible())
                       || btnMidiMixerFwd.isVisible()
                       || btnTriggerMidi.isVisible()
                       || cmbTriggerMidiDevice.isVisible();
    if (hasMidiSection)
    {
        leftContent.addSectionSeparator(leftPanel.getY(), "MIDI");
        leftPanel.removeFromTop(16);
    }
    if (!bpmInline && btnMidiClock.isVisible())
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

    // --- OSC section (skip BPM toggle already placed inline) ---
    bool hasOscSection = (!bpmInline && btnOscFwdBpm.isVisible())
                      || btnOscMixerFwd.isVisible()
                      || btnTriggerOsc.isVisible();
    if (hasOscSection)
    {
        leftContent.addSectionSeparator(leftPanel.getY(), "OSC");
        leftPanel.removeFromTop(16);
    }
    if (!bpmInline && btnOscFwdBpm.isVisible())
    {
        btnOscFwdBpm.setBounds(leftPanel.removeFromTop(22));
        if (edOscFwdBpmAddr.isVisible())
        {
            leftPanel.removeFromTop(2);
            auto bpmRow = leftPanel.removeFromTop(22);
            lblOscFwdBpmAddr.setBounds(bpmRow.removeFromLeft(40));
            bpmRow.removeFromLeft(2);
            edOscFwdBpmAddr.setBounds(bpmRow);

            leftPanel.removeFromTop(2);
            auto cmdRow = leftPanel.removeFromTop(22);
            lblOscFwdBpmCmd.setBounds(cmdRow.removeFromLeft(40));
            cmdRow.removeFromLeft(2);
            edOscFwdBpmCmd.setBounds(cmdRow);
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
    if (!bpmInline && edOscIp.isVisible())
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

    // --- Ableton Link (skip when already placed inline in Audio BPM section) ---
    if (!bpmInline && btnLink.isVisible())
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

    // AUDIO SETTINGS: only rendered here for LTC (Audio BPM renders it inline above)
    if (!bpmDriven && cmbAudioInputDevice.isVisible())
    {
        leftContent.addSectionSeparator(leftPanel.getY(), "AUDIO SETTINGS");
        leftPanel.removeFromTop(16);
        layAudioInputSettings();
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

    // TCNET OUT
    {
        rightContent.addSectionSeparator(rp.getY());
        rp.removeFromTop(4);
        auto row = rp.removeFromTop(btnH);
        btnTcnetOut.setBounds(row); rp.removeFromTop(2);
        if (cmbTcnetInterface.isVisible()) layCombo(lblTcnetInterface, cmbTcnetInterface, rp);
        if (cmbTcnetLayer.isVisible()) layCombo(lblTcnetLayer, cmbTcnetLayer, rp);
        if (sldTcnetOffset.isVisible()) laySlider(lblTcnetOffset, sldTcnetOffset, rp);
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

    // Show Lock flash feedback: reset button color after countdown
    if (showLockFlashCountdown > 0)
    {
        if (--showLockFlashCountdown == 0)
        {
            btnShowLock.setColour(juce::TextButton::buttonColourId,
                                  juce::Colour(0xFFCC2222));  // back to normal locked red
        }
    }

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

    // Feed TCNet output layers from engines.
    // Each engine selects its target layer (1-4) via tcnetLayer.
    // Clear all layers first, then fill from enabled engines (last-write-wins if
    // two engines target the same layer).
    if (sharedTcnetOutput.getIsRunning())
    {
        // Track which layers are assigned by enabled engines
        bool layerUsed[TCNetOutput::kMaxLayers] = {};

        for (int i = 0; i < (int)engines.size(); ++i)
        {
            auto& eng = *engines[(size_t)i];
            if (!eng.isOutputTcnetEnabled()) continue;

            int layer = eng.getTcnetLayer();  // 0-3
            layerUsed[layer] = true;

            auto src = eng.getActiveInput();
            int ep = eng.getEffectivePlayer();
            // Fader for Resolume opacity: real DJM/Denon fader value, 255 for non-DJ sources
            uint8_t onAirFader = 255;
            uint8_t beatInBar = 0;
            uint32_t durationMs = 0;
            uint32_t bpm100 = 0;

            if (src == SrcType::ProDJLink && sharedProDJLinkInput.getIsRunning())
            {
                if (sharedProDJLinkInput.hasMixerFaderData())
                    onAirFader = sharedProDJLinkInput.getChannelFader(ep);
                beatInBar = sharedProDJLinkInput.getBeatInBar(ep);
                bpm100 = (uint32_t)(sharedProDJLinkInput.getBPM(ep) * 100.0);
            }
            else if (src == SrcType::StageLinQ && sharedStageLinQInput.getIsRunning())
            {
                if (sharedStageLinQInput.hasMixerData())
                {
                    double faderPos = sharedStageLinQInput.getFaderPosition(ep);
                    onAirFader = (uint8_t)juce::jlimit(0, 255, (int)(faderPos * 255.0));
                }
                beatInBar = sharedStageLinQInput.getBeatInBar(ep);
                bpm100 = (uint32_t)(sharedStageLinQInput.getBPM(ep) * 100.0);
            }

            auto info = eng.getActiveTrackInfo();
            durationMs = (info.durationSec > 0) ? (uint32_t)info.durationSec * 1000 : 0;

            sharedTcnetOutput.setLayerFromEngine(
                layer,
                eng.getOutputTimecode(),
                eng.getEffectiveOutputFps(),
                eng.getSmoothedPlayheadMs(),
                durationMs,
                eng.isSourceActive(),
                onAirFader,
                beatInBar,
                bpm100,
                eng.getTcnetOutputOffsetMs());

            // Feed track metadata for Resolume unicast.
            // DJ sources: real artist + title from CDJ/Denon.
            // Non-DJ sources: input name as artist, "STC" as title.
            juce::String artist, title;
            if (info.artist.isNotEmpty() || info.title.isNotEmpty())
            {
                artist = info.artist;
                title  = info.title;
            }
            else
            {
                switch (src)
                {
                    case SrcType::MTC:        artist = "MTC Input";        break;
                    case SrcType::ArtNet:      artist = "Art-Net Input";    break;
                    case SrcType::LTC:         artist = "LTC Input";        break;
                    case SrcType::SystemTime:  artist = "System Time";      break;
                    case SrcType::ProDJLink:   artist = "Pro DJ Link";      break;
                    case SrcType::StageLinQ:   artist = "StageLinQ";        break;
                    case SrcType::Hippotizer:  artist = "HippoNet";       break;
                    default:                   artist = "STC";              break;
                }
                title = eng.getName();
            }
            sharedTcnetOutput.setLayerMetadata(layer, artist, title);

            // Feed artwork: live source -> disk cache -> STC logo
            if ((src == SrcType::ProDJLink || src == SrcType::StageLinQ) && info.title.isNotEmpty())
            {
                // Build cache key matching TrackMap format
                juce::String artKey = info.artist + "|" + info.title;
                if (info.durationSec > 0)
                    artKey += "|" + juce::String(info.durationSec);
                if (tcnetArtworkKey[layer] != artKey)
                {
                    tcnetArtworkKey[layer] = artKey;
                    juce::Image artImg;

                    // 1. Try live source (already in memory from metadata fetch)
                    if (src == SrcType::ProDJLink && info.artworkId != 0)
                        artImg = sharedDbClient.getCachedArtwork(info.artworkId);
                    else if (src == SrcType::StageLinQ)
                    {
                        auto netPath = sharedStageLinQInput.getTrackNetworkPath(ep);
                        if (netPath.isNotEmpty() && sharedStageLinQDb.isDatabaseReady())
                            artImg = sharedStageLinQDb.getArtworkForTrack(netPath);
                    }

                    // 2. Try disk cache (saved by CuePointEditor)
                    if (!artImg.isValid())
                        artImg = WaveformCache::loadArtwork(artKey.toStdString());
                    if (!artImg.isValid() && info.durationSec > 0)
                        artImg = WaveformCache::loadArtwork((info.artist + "|" + info.title).toStdString());

                    // 3. Convert to JPEG and send, or fall back to STC logo
                    if (artImg.isValid())
                    {
                        juce::MemoryOutputStream mos;
                        juce::JPEGImageFormat fmt;
                        fmt.setQuality(0.7f);
                        if (fmt.writeImageToStream(artImg, mos))
                            sharedTcnetOutput.setLayerArtwork(layer, mos.getData(), mos.getDataSize());
                        else
                            sharedTcnetOutput.setLayerArtwork(layer, nullptr, 0);
                    }
                    else
                        sharedTcnetOutput.setLayerArtwork(layer, nullptr, 0);
                }
            }
            else
            {
                if (tcnetArtworkKey[layer].isNotEmpty())
                {
                    tcnetArtworkKey[layer] = {};
                    sharedTcnetOutput.setLayerArtwork(layer, nullptr, 0);  // STC logo
                }
            }
        }

        // Clear layers not assigned to any engine
        for (int i = 0; i < TCNetOutput::kMaxLayers; ++i)
            if (!layerUsed[i])
                sharedTcnetOutput.clearLayer(i);
    }

    // Update UI for selected engine
    auto& eng = currentEngine();

    updateStatusLabels();

    if (eng.getActiveInput() == SrcType::ProDJLink && sharedProDJLinkInput.isReceiving())
    {
        int pdlPlayer = eng.getEffectivePlayer();

        // Guard: in XF mode, resolved player can be 0 (no player on this side)
        if (pdlPlayer < 1)
        {
            lblProDJLinkTrackInfo.setText("", juce::dontSendNotification);
            lblProDJLinkMetadata.setText("", juce::dontSendNotification);
            lblNextCue.setText("", juce::dontSendNotification);
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

        // Next cue countdown
        updateNextCueLabel(eng);

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
        //
        // IMPORTANT: Use the SOURCE player's IP (the CDJ that owns the media),
        // not the deck's own IP.  When CDJ 2 loads a track from CDJ 1's USB
        // via Link export, the cache entry is keyed by CDJ 1's IP (set by
        // TimecodeEngine::requestDbMetadata).  Looking up with CDJ 2's IP
        // would miss the cache entry and the waveform would never load.
        uint32_t wfTrackId = trackInfo.trackId;
        uint8_t wfSrcPlayer = sharedProDJLinkInput.getLoadedPlayer(pdlPlayer);
        if (wfSrcPlayer == 0) wfSrcPlayer = (uint8_t)pdlPlayer;
        juce::String srcIP = sharedProDJLinkInput.getPlayerIP((int)wfSrcPlayer);
        if (srcIP.isEmpty()) srcIP = sharedProDJLinkInput.getPlayerIP(pdlPlayer);

        if (wfTrackId != 0 && wfTrackId != displayedWaveformTrackId)
        {
            // Track changed -- clear old waveform immediately (avoids stale cursor)
            waveformDisplay.clearWaveform();
            // Mark this track as "attempted" so we don't re-enter this block
            // every frame.  The retry path below uses hasWaveformData() to
            // detect when the async waveform query completes.
            displayedWaveformTrackId = wfTrackId;

            // Try to populate from cache (may not have waveform yet)
            auto meta = sharedDbClient.getCachedMetadata(srcIP, wfTrackId);
            if (meta.hasWaveform())
            {
                waveformDisplay.setColorWaveformData(meta.waveformData,
                    meta.waveformEntryCount, meta.waveformBytesPerEntry);
                if (meta.durationSeconds > 0)
                    waveformDisplay.setDurationMs((uint32_t)meta.durationSeconds * 1000);
                if (meta.hasCueList())
                    waveformDisplay.setRekordboxCues(meta.cueList);
                if (meta.hasBeatGrid())
                    waveformDisplay.setBeatGrid(meta.beatGrid);
                // Feed beat grid to engine for PLL micro-correction
                if (meta.hasBeatGrid())
                    eng.setBeatGrid(meta.beatGrid, wfTrackId);
            }
        }
        else if (wfTrackId != 0 && !waveformDisplay.hasWaveformData())
        {
            // Waveform not yet loaded -- retry from cache (async: arrives after metadata)
            auto meta = sharedDbClient.getCachedMetadata(srcIP, wfTrackId);
            if (meta.hasWaveform())
            {
                waveformDisplay.setColorWaveformData(meta.waveformData,
                    meta.waveformEntryCount, meta.waveformBytesPerEntry);
                if (meta.durationSeconds > 0)
                    waveformDisplay.setDurationMs((uint32_t)meta.durationSeconds * 1000);
                if (meta.hasCueList())
                    waveformDisplay.setRekordboxCues(meta.cueList);
                if (meta.hasBeatGrid())
                    waveformDisplay.setBeatGrid(meta.beatGrid);
                if (meta.hasBeatGrid())
                    eng.setBeatGrid(meta.beatGrid, wfTrackId);
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
    else if (eng.getActiveInput() == SrcType::StageLinQ && sharedStageLinQInput.isReceiving())
    {
        int slqDeck = eng.getEffectivePlayer();
        if (slqDeck >= 1 && slqDeck <= StageLinQ::kMaxDecks)
        {
            // Track info from StageLinQ StateMap
            auto trackInfo = eng.getActiveTrackInfo();
            juce::String slqTrackStr;
            if (trackInfo.artist.isNotEmpty() && trackInfo.title.isNotEmpty())
                slqTrackStr = trackInfo.artist + " -- " + trackInfo.title;
            else if (trackInfo.title.isNotEmpty())
                slqTrackStr = trackInfo.title;
            lblProDJLinkTrackInfo.setText(slqTrackStr, juce::dontSendNotification);
            juce::Colour slqAccent(0xFF00CC66);  // Denon green
            lblProDJLinkTrackInfo.setColour(juce::Label::textColourId,
                slqTrackStr.isNotEmpty() ? slqAccent.brighter(0.5f) : textDim);

            double slqBpm = sharedStageLinQInput.getBPM(slqDeck);
            juce::String slqMeta;
            if (slqBpm > 0.0)
            {
                slqMeta += juce::String(slqBpm, 1) + " BPM";
                int effMult = eng.getEffectiveBpmMultiplier();
                if (effMult != 0)
                {
                    double multBpm = TimecodeEngine::applyBpmMultiplier(slqBpm, effMult);
                    juce::String multLabel;
                    switch (effMult) {
                        case  1: multLabel = "x2"; break;
                        case  2: multLabel = "x4"; break;
                        case -1: multLabel = "/2"; break;
                        case -2: multLabel = "/4"; break;
                        default: break;
                    }
                    slqMeta += "  -> " + juce::String(multBpm, 1) + " (" + multLabel + ")";
                }
            }
            double slqSpeed = sharedStageLinQInput.getActualSpeed(slqDeck);
            if (slqSpeed > 0.01)
            {
                double pitchPct = (slqSpeed - 1.0) * 100.0;
                slqMeta += "  " + (pitchPct >= 0.0 ? juce::String("+") : juce::String(""))
                                + juce::String(pitchPct, 2) + "%";
            }
            juce::String slqModel = sharedStageLinQInput.getPlayerModel(slqDeck);
            if (slqModel.isNotEmpty())
                slqMeta += "  | " + slqModel;

            lblProDJLinkMetadata.setText(slqMeta, juce::dontSendNotification);
            lblProDJLinkMetadata.setColour(juce::Label::textColourId, slqAccent.brighter(0.3f));

            // Next cue countdown
            updateNextCueLabel(eng);

            // Artwork + waveform from StageLinQ database
            if (sharedStageLinQDb.isDatabaseReady())
            {
                auto netPath = sharedStageLinQInput.getTrackNetworkPath(slqDeck);
                uint32_t slqTrackVer = sharedStageLinQInput.getTrackVersion(slqDeck);

                // Only update displays on track change (avoid 60Hz cache thrashing)
                if (netPath.isNotEmpty() && slqTrackVer != displayedWaveformTrackId)
                {
                    auto dbMeta = sharedStageLinQDb.getTrackByNetworkPath(netPath);
                    if (dbMeta.valid)
                    {
                        displayedWaveformTrackId = slqTrackVer;

                        auto artImg = sharedStageLinQDb.getArtworkForTrack(netPath);
                        if (artImg.isValid())
                            artworkDisplay.setImage(artImg);
                        else
                            artworkDisplay.clearImage();

                        auto wf = sharedStageLinQDb.getWaveformForTrack(netPath);
                        if (wf.valid && wf.entryCount > 0)
                            waveformDisplay.setColorWaveformData(wf.data, wf.entryCount, 3);
                        else
                            waveformDisplay.clearWaveform();

                        // Set track duration so minute markers and cue markers render
                        uint32_t trackLenMs = sharedStageLinQInput.getTrackLengthSec(slqDeck) * 1000;
                        if (trackLenMs == 0 && dbMeta.length > 0.0)
                            trackLenMs = (uint32_t)(dbMeta.length * 1000.0);
                        if (trackLenMs > 0)
                            waveformDisplay.setDurationMs(trackLenMs);

                        // Fetch performance data (quick cues, loops, beat grid)
                        auto perf = sharedStageLinQDb.getPerformanceData(netPath);
                        if (perf.valid && perf.sampleRate > 0.0 && !perf.quickCues.empty())
                        {
                            // Convert DenonQuickCues → RekordboxCue for waveform display
                            std::vector<TrackMetadata::RekordboxCue> wfCues;
                            for (int ci = 0; ci < (int)perf.quickCues.size(); ++ci)
                            {
                                auto& qc = perf.quickCues[(size_t)ci];
                                if (!qc.isSet()) continue;

                                TrackMetadata::RekordboxCue rc;
                                rc.type = TrackMetadata::RekordboxCue::HotCue;
                                rc.hotCueNumber = (uint8_t)(ci + 1);
                                rc.positionMs = (uint32_t)((qc.sampleOffset / perf.sampleRate) * 1000.0);
                                rc.colorR = qc.r;
                                rc.colorG = qc.g;
                                rc.colorB = qc.b;
                                rc.hasColor = true;
                                rc.comment = qc.label;
                                wfCues.push_back(rc);
                            }
                            if (!wfCues.empty())
                                waveformDisplay.setRekordboxCues(wfCues);

                            // Auto-populate TrackMap cue points (same as ProDJLink path)
                            auto cueTrackInfo = eng.getActiveTrackInfo();
                            if (cueTrackInfo.title.isNotEmpty())
                            {
                                int dur = (int)sharedStageLinQInput.getTrackLengthSec(slqDeck);
                                auto* tmEntry = settings.trackMap.find(cueTrackInfo.artist, cueTrackInfo.title, dur);
                                if (!tmEntry && dur > 0)
                                    tmEntry = settings.trackMap.find(cueTrackInfo.artist, cueTrackInfo.title, 0);
                                if (!tmEntry)
                                    tmEntry = settings.trackMap.findIgnoringDuration(cueTrackInfo.artist, cueTrackInfo.title);
                                if (tmEntry != nullptr && tmEntry->cuePoints.empty())
                                {
                                    for (int ci = 0; ci < (int)perf.quickCues.size(); ++ci)
                                    {
                                        auto& qc = perf.quickCues[(size_t)ci];
                                        if (!qc.isSet()) continue;
                                        uint32_t posMs = (uint32_t)((qc.sampleOffset / perf.sampleRate) * 1000.0);
                                        if (posMs == 0) continue;

                                        CuePoint cp;
                                        cp.positionMs = posMs;
                                        // Name: letter + label (e.g. "A DROP")
                                        if (ci < 8)
                                            cp.name = juce::String::charToString((juce::juce_wchar)('A' + ci));
                                        if (qc.label.isNotEmpty())
                                            cp.name += cp.name.isNotEmpty() ? " " + qc.label : qc.label;
                                        if (cp.name.isEmpty())
                                            cp.name = "CUE " + juce::String(ci + 1);

                                        tmEntry->cuePoints.push_back(std::move(cp));
                                    }
                                    tmEntry->sortCuePoints();
                                    eng.refreshTrackMapLookup();
                                }
                            }
                        }
                    }
                }
                else if (netPath.isEmpty() && displayedWaveformTrackId != 0)
                {
                    artworkDisplay.clearImage();
                    waveformDisplay.clearWaveform();
                    displayedWaveformTrackId = 0;
                }
            }

            // Update waveform cursor position every frame
            if (waveformDisplay.hasWaveformData())
            {
                float posRatio = eng.getSmoothedPlayPositionRatio();
                waveformDisplay.setPlayPosition(posRatio);
            }

            // Mixer fader bars (same visual style as ProDJLink DJM display)
            {
                static const juce::String kBar3[] = {
                    juce::String::charToString(0x2581),   // low
                    juce::String::charToString(0x2584),   // mid-low
                    juce::String::charToString(0x2586),   // mid-high
                    juce::String::charToString(0x2588)    // full
                };
                static const juce::String kCrossSymbol = juce::String::charToString(0x2715);

                auto faderBar3 = [&](double val01) -> const juce::String&
                {
                    int filled = (int)std::round(val01 * 3.0);
                    return kBar3[juce::jlimit(0, 3, filled)];
                };

                juce::String mixStr = "MIX";
                for (int ch = 1; ch <= 4; ++ch)
                    mixStr += " " + juce::String(ch) + ":" + faderBar3(sharedStageLinQInput.getFaderPosition(ch));
                mixStr += " " + kCrossSymbol + ":" + faderBar3(sharedStageLinQInput.getCrossfaderPosition());

                lblMixerStatus.setText(mixStr, juce::dontSendNotification);
                lblMixerStatus.setColour(juce::Label::textColourId, slqAccent.withAlpha(0.7f));
            }

            updateBpmMultButtonStates();
        }
        else
        {
            lblProDJLinkTrackInfo.setText("", juce::dontSendNotification);
            lblProDJLinkMetadata.setText("", juce::dontSendNotification);
            lblNextCue.setText("", juce::dontSendNotification);
            lblMixerStatus.setText("", juce::dontSendNotification);
        }
    }
    else if (eng.getActiveInput() != SrcType::ProDJLink && eng.getActiveInput() != SrcType::StageLinQ)
    {
        lblProDJLinkTrackInfo.setText("", juce::dontSendNotification);
        lblProDJLinkMetadata.setText("", juce::dontSendNotification);
        lblNextCue.setText("", juce::dontSendNotification);
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
    else if (lblLinkStatus.isVisible())
    {
        // Show which engine owns Link (if any)
        int owner = findLinkOwnerOtherThan(selectedEngine);
        if (owner >= 0)
        {
            lblLinkStatus.setText("Active on " + engines[(size_t)owner]->getName(),
                                 juce::dontSendNotification);
            lblLinkStatus.setColour(juce::Label::textColourId, juce::Colour(0xFF888888));
        }
        else
        {
            lblLinkStatus.setText("", juce::dontSendNotification);
        }
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
    if (eng.isAudioBpmRunning() && eng.getActiveInput() != SrcType::LTC)
        mtrLtcInput.setLevel(eng.getAudioBpmPeakLevel());
    else
        mtrLtcInput.setLevel(eng.getSmoothedLtcInLevel());
    // Separate Audio BPM meter (visible when LTC owns shared meter)
    if (mtrAudioBpm.isVisible())
        mtrAudioBpm.setLevel(eng.getAudioBpmPeakLevel());

    // Audio BPM value display + beat LED
    if (lblBpmValue.isVisible())
    {
        double bpm = eng.getAudioBpm();
        double conf = eng.getAudioBpmConfidence();
        if (bpm > 0.0 && conf > 0.15)
        {
            lblBpmValue.setText(juce::String(bpm, 1) + " BPM", juce::dontSendNotification);
            // Color: dim orange at low confidence → bright orange → green at high confidence
            juce::Colour bpmCol;
            if (conf > 0.6)
                bpmCol = juce::Colour(0xFF00DD77);  // green = locked
            else if (conf > 0.3)
                bpmCol = juce::Colour(0xFFFF9900);  // orange = tracking
            else
                bpmCol = juce::Colour(0xFFCC6600);  // dim orange = uncertain
            lblBpmValue.setColour(juce::Label::textColourId, bpmCol);
        }
        else
        {
            lblBpmValue.setText("--- BPM", juce::dontSendNotification);
            lblBpmValue.setColour(juce::Label::textColourId, juce::Colour(0xFF555555));
        }
    }
    if (ledBeat.isVisible())
    {
        double bpm = eng.getAudioBpm();
        if (bpm > 20.0 && eng.isAudioBpmRunning())
        {
            double intervalMs = 60000.0 / bpm;
            beatFlashAccumMs += 1000.0 / 60.0;  // ~16.67ms per tick at 60Hz timer
            if (beatFlashAccumMs >= intervalMs)
            {
                beatFlashAccumMs -= intervalMs;
                // Prevent runaway if BPM changed drastically
                if (beatFlashAccumMs > intervalMs)
                    beatFlashAccumMs = 0.0;
                ledBeat.flash();
            }
            else
            {
                ledBeat.decay(0.15f);
            }
        }
        else
        {
            beatFlashAccumMs = 0.0;
            ledBeat.decay(0.15f);
        }
    }

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
    if (trackInfo.title.isNotEmpty())
    {
        auto* entry = settings.trackMap.find(trackInfo.artist, trackInfo.title,
                                              trackInfo.durationSec);
        if (!entry && trackInfo.durationSec > 0)
            entry = settings.trackMap.find(trackInfo.artist, trackInfo.title, 0);
        if (!entry)
            entry = settings.trackMap.findIgnoringDuration(trackInfo.artist, trackInfo.title);
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
    if (info.title.isEmpty()) return;

    auto* entry = settings.trackMap.find(info.artist, info.title, info.durationSec);
    if (!entry && info.durationSec > 0)
        entry = settings.trackMap.find(info.artist, info.title, 0);
    if (!entry)
        entry = settings.trackMap.findIgnoringDuration(info.artist, info.title);
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
        newEntry.durationSec = info.durationSec;
        newEntry.bpmMultiplier = newValue;

        // Auto-populate cue points from rekordbox if available
        if (info.trackId != 0)
        {
            auto meta = sharedDbClient.getCachedMetadataByTrackId(info.trackId);
            if (meta.isValid() && !meta.cueList.empty())
            {
                for (auto& rc : meta.cueList)
                {
                    if (rc.positionMs == 0) continue;
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
                    newEntry.cuePoints.push_back(std::move(cp));
                }
                newEntry.sortCuePoints();
            }
        }

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
    struct I { juce::TextButton* b; SrcType s; };
    I bs[] = { {&btnMtcIn,SrcType::MTC}, {&btnArtnetIn,SrcType::ArtNet}, {&btnHippoIn,SrcType::Hippotizer}, {&btnSysTime,SrcType::SystemTime}, {&btnLtcIn,SrcType::LTC}, {&btnProDJLinkIn,SrcType::ProDJLink}, {&btnStageLinQIn,SrcType::StageLinQ} };
    for (auto& i : bs) styleInputButton(*i.b, active == i.s, getInputColour(i.s));

    // Stop shared network inputs when no engine uses them.
    // Without this, StageLinQ keeps broadcasting discovery (~1 pps) and
    // ProDJLink keeps sending keepalives even when all engines switched away.
    bool anySlq = false, anyPdl = false;
    for (auto& e : engines)
    {
        auto src = e->getActiveInput();
        if (src == SrcType::StageLinQ)  anySlq = true;
        if (src == SrcType::ProDJLink)  anyPdl = true;
    }
    if (!anySlq && sharedStageLinQInput.getIsRunning())
    {
        DBG("MainComponent: no engine uses StageLinQ — stopping shared input");
        sharedStageLinQDb.stop();
        sharedStageLinQInput.stop();
    }
    if (!anyPdl && sharedProDJLinkInput.getIsRunning())
    {
        DBG("MainComponent: no engine uses ProDJLink — stopping shared input");
        sharedDbClient.stop();
        sharedProDJLinkInput.stop();
    }
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

juce::Colour MainComponent::getInputColour(SrcType s) const
{
    switch (s)
    {
        case SrcType::MTC:        return accentRed;
        case SrcType::ArtNet:     return accentOrange;
        case SrcType::SystemTime: return accentGreen;
        case SrcType::LTC:        return accentPurple;
        case SrcType::ProDJLink:  return juce::Colour(0xFF00AAFF);  // bright blue
        case SrcType::StageLinQ:  return juce::Colour(0xFF00CC66);  // Denon green
        case SrcType::Hippotizer: return juce::Colour(0xFF66BBAA);  // Green Hippo teal
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
