#include "MainComponent.h"

//==============================================================================
// AudioScanThread
//==============================================================================
MainComponent::AudioScanThread::AudioScanThread(MainComponent* owner)
    : Thread("AudioDeviceScan"), safeOwner(owner) {}

void MainComponent::AudioScanThread::run()
{
    juce::Array<AudioDeviceEntry> inputs, outputs;

    juce::AudioDeviceManager tempMgr;
    tempMgr.initialise(128, 128, nullptr, false);

    for (auto* type : tempMgr.getAvailableDeviceTypes())
    {
        if (threadShouldExit()) return;
        type->scanForDevices();
        auto typeName = type->getTypeName();
        for (auto& name : type->getDeviceNames(true))
            inputs.add({ typeName, name, AudioDeviceEntry::makeDisplayName(typeName, name) });
        for (auto& name : type->getDeviceNames(false))
            outputs.add({ typeName, name, AudioDeviceEntry::makeDisplayName(typeName, name) });
    }

    if (!threadShouldExit())
    {
        auto safe = safeOwner;
        juce::MessageManager::callAsync([safe, inputs, outputs]()
        {
            if (auto* mc = safe.getComponent())
                mc->onAudioScanComplete(inputs, outputs);
        });
    }
}

//==============================================================================
// CONSTRUCTOR / DESTRUCTOR
//==============================================================================
MainComponent::MainComponent()
{
    setSize(900, 700);
    setLookAndFeel(&customLookAndFeel);

    // --- Input buttons ---
    for (auto* btn : { &btnMtcIn, &btnArtnetIn, &btnSysTime, &btnLtcIn })
    { addAndMakeVisible(btn); btn->setClickingTogglesState(false); }

    // Input buttons: switch source OR toggle collapse if already active
    btnMtcIn.onClick = [this] {
        if (activeInput == InputSource::MTC) { inputConfigExpanded = !inputConfigExpanded; updateDeviceSelectorVisibility(); }
        else { inputConfigExpanded = true; setInputSource(InputSource::MTC); saveSettings(); }
    };
    btnArtnetIn.onClick = [this] {
        if (activeInput == InputSource::ArtNet) { inputConfigExpanded = !inputConfigExpanded; updateDeviceSelectorVisibility(); }
        else { inputConfigExpanded = true; setInputSource(InputSource::ArtNet); saveSettings(); }
    };
    btnSysTime.onClick = [this] {
        if (activeInput == InputSource::SystemTime) { inputConfigExpanded = !inputConfigExpanded; updateDeviceSelectorVisibility(); }
        else { inputConfigExpanded = true; setInputSource(InputSource::SystemTime); saveSettings(); }
    };
    btnLtcIn.onClick = [this] {
        if (activeInput == InputSource::LTC) { inputConfigExpanded = !inputConfigExpanded; updateDeviceSelectorVisibility(); }
        else { inputConfigExpanded = true; setInputSource(InputSource::LTC); saveSettings(); }
    };

    // --- Output toggles ---
    for (auto* btn : { &btnMtcOut, &btnArtnetOut, &btnLtcOut, &btnThruOut })
        addAndMakeVisible(btn);

    styleOutputToggle(btnMtcOut, accentRed);
    styleOutputToggle(btnArtnetOut, accentOrange);
    styleOutputToggle(btnLtcOut, accentPurple);
    styleOutputToggle(btnThruOut, accentCyan);

    auto outputToggleHandler = [this]
    {
        outputMtcEnabled    = btnMtcOut.getToggleState();
        outputArtnetEnabled = btnArtnetOut.getToggleState();
        outputLtcEnabled    = btnLtcOut.getToggleState();
        outputThruEnabled   = btnThruOut.getToggleState();
        updateOutputStates();
        updateDeviceSelectorVisibility();
        saveSettings();
    };
    btnMtcOut.onClick = btnArtnetOut.onClick = btnLtcOut.onClick = btnThruOut.onClick = outputToggleHandler;

    // --- Collapse toggle buttons for outputs ---
    for (auto* btn : { &btnCollapseMtcOut, &btnCollapseArtnetOut, &btnCollapseLtcOut, &btnCollapseThruOut })
    {
        addAndMakeVisible(btn);
        styleCollapseButton(*btn);
    }
    btnCollapseMtcOut.onClick    = [this] { mtcOutExpanded = !mtcOutExpanded;       updateCollapseButtonText(btnCollapseMtcOut, mtcOutExpanded);       updateDeviceSelectorVisibility(); };
    btnCollapseArtnetOut.onClick = [this] { artnetOutExpanded = !artnetOutExpanded; updateCollapseButtonText(btnCollapseArtnetOut, artnetOutExpanded); updateDeviceSelectorVisibility(); };
    btnCollapseLtcOut.onClick    = [this] { ltcOutExpanded = !ltcOutExpanded;       updateCollapseButtonText(btnCollapseLtcOut, ltcOutExpanded);       updateDeviceSelectorVisibility(); };
    btnCollapseThruOut.onClick   = [this] { thruOutExpanded = !thruOutExpanded;     updateCollapseButtonText(btnCollapseThruOut, thruOutExpanded);     updateDeviceSelectorVisibility(); };
    updateCollapseButtonText(btnCollapseMtcOut, mtcOutExpanded);
    updateCollapseButtonText(btnCollapseArtnetOut, artnetOutExpanded);
    updateCollapseButtonText(btnCollapseLtcOut, ltcOutExpanded);
    updateCollapseButtonText(btnCollapseThruOut, thruOutExpanded);

    // Input collapse button
    addAndMakeVisible(btnCollapseInput);
    styleCollapseButton(btnCollapseInput);
    btnCollapseInput.onClick = [this] { inputConfigExpanded = !inputConfigExpanded; updateCollapseButtonText(btnCollapseInput, inputConfigExpanded); updateDeviceSelectorVisibility(); };
    updateCollapseButtonText(btnCollapseInput, inputConfigExpanded);

    // --- FPS buttons ---
    for (auto* btn : { &btnFps24, &btnFps25, &btnFps2997, &btnFps30 })
    { addAndMakeVisible(btn); btn->setClickingTogglesState(false); }

    btnFps24.onClick   = [this] { setFrameRate(FrameRate::FPS_24); saveSettings(); };
    btnFps25.onClick   = [this] { setFrameRate(FrameRate::FPS_25); saveSettings(); };
    btnFps2997.onClick = [this] { setFrameRate(FrameRate::FPS_2997); saveSettings(); };
    btnFps30.onClick   = [this] { setFrameRate(FrameRate::FPS_30); saveSettings(); };

    addAndMakeVisible(timecodeDisplay);

    // =====================================================================
    // LEFT PANEL Ã¢â‚¬â€œ INPUT SELECTORS
    // =====================================================================
    auto addLabelAndCombo = [this](juce::Label& lbl, juce::ComboBox& cmb, const juce::String& text)
    {
        addAndMakeVisible(lbl); addAndMakeVisible(cmb);
        lbl.setText(text, juce::dontSendNotification);
        styleLabel(lbl); styleComboBox(cmb);
    };

    addLabelAndCombo(lblAudioInputTypeFilter, cmbAudioInputTypeFilter, "AUDIO DRIVER:");
    cmbAudioInputTypeFilter.onChange = [this]
    {
        populateFilteredInputDeviceCombo();
        if (activeInput == InputSource::LTC)
            restartLtcWithCurrentSettings();
        saveSettings();
    };

    addLabelAndCombo(lblSampleRate, cmbSampleRate, "SAMPLE RATE / BUFFER:");
    populateSampleRateCombo();
    cmbSampleRate.onChange = [this] { restartAllAudioDevices(); saveSettings(); };

    addLabelAndCombo(lblBufferSize, cmbBufferSize, "BUFFER SIZE:");
    populateBufferSizeCombo();
    cmbBufferSize.onChange = [this] { restartAllAudioDevices(); saveSettings(); };

    addLabelAndCombo(lblMidiInputDevice, cmbMidiInputDevice, "MIDI INPUT DEVICE:");
    cmbMidiInputDevice.onChange = [this]
    {
        int sel = cmbMidiInputDevice.getSelectedId() - 1;
        if (sel >= 0 && activeInput == InputSource::MTC)
        { stopMtcInput(); mtcInput.refreshDeviceList(); if (mtcInput.start(sel)) inputStatusText = "RX: " + mtcInput.getCurrentDeviceName(); else inputStatusText = "FAILED TO OPEN"; saveSettings(); }
    };

    addLabelAndCombo(lblArtnetInputInterface, cmbArtnetInputInterface, "ART-NET INPUT DEVICE:");
    cmbArtnetInputInterface.onChange = [this]
    {
        if (activeInput == InputSource::ArtNet)
        { int sel = cmbArtnetInputInterface.getSelectedId() - 1; stopArtnetInput(); artnetInput.refreshNetworkInterfaces(); if (artnetInput.start(sel, 6454)) inputStatusText = "RX ON " + artnetInput.getBindInfo(); else inputStatusText = "FAILED TO BIND"; saveSettings(); }
    };

    addLabelAndCombo(lblAudioInputDevice, cmbAudioInputDevice, "AUDIO INPUT DEVICE:");
    cmbAudioInputDevice.onChange = [this]
    {
        if (activeInput == InputSource::LTC && cmbAudioInputDevice.getSelectedId() > 0
            && cmbAudioInputDevice.getSelectedId() != 999)
        { restartLtcWithCurrentSettings(); populateAudioInputChannels(); }
    };

    addLabelAndCombo(lblAudioInputChannel, cmbAudioInputChannel, "LTC CHANNEL:");
    cmbAudioInputChannel.onChange = [this] { if (activeInput == InputSource::LTC) restartLtcWithCurrentSettings(); };

    addAndMakeVisible(sldLtcInputGain); styleGainSlider(sldLtcInputGain);
    addAndMakeVisible(lblLtcInputGain); lblLtcInputGain.setText("LTC INPUT GAIN:", juce::dontSendNotification); styleLabel(lblLtcInputGain);
    addAndMakeVisible(mtrLtcInput); mtrLtcInput.setColour(accentPurple);
    sldLtcInputGain.onValueChange = [this] { ltcInput.setInputGain((float)sldLtcInputGain.getValue() / 100.0f); saveSettings(); };

    addLabelAndCombo(lblThruInputChannel, cmbThruInputChannel, "AUDIO THRU CHANNEL:");
    cmbThruInputChannel.onChange = [this] { if (activeInput == InputSource::LTC) restartLtcWithCurrentSettings(); };

    addAndMakeVisible(sldThruInputGain); styleGainSlider(sldThruInputGain);
    addAndMakeVisible(lblThruInputGain); lblThruInputGain.setText("AUDIO THRU INPUT GAIN:", juce::dontSendNotification); styleLabel(lblThruInputGain);
    addAndMakeVisible(mtrThruInput); mtrThruInput.setColour(accentCyan);
    sldThruInputGain.onValueChange = [this] { ltcInput.setPassthruGain((float)sldThruInputGain.getValue() / 100.0f); saveSettings(); };

    addAndMakeVisible(lblInputStatus); styleLabel(lblInputStatus); lblInputStatus.setColour(juce::Label::textColourId, accentGreen);

    // =====================================================================
    // RIGHT PANEL Ã¢â‚¬â€œ OUTPUT SELECTORS
    // =====================================================================
    addLabelAndCombo(lblMidiOutputDevice, cmbMidiOutputDevice, "MIDI OUTPUT DEVICE:");
    cmbMidiOutputDevice.onChange = [this]
    {
        int sel = cmbMidiOutputDevice.getSelectedId() - 1;
        if (sel >= 0 && outputMtcEnabled)
        { stopMtcOutput(); mtcOutput.refreshDeviceList(); if (mtcOutput.start(sel)) { mtcOutput.setFrameRate(currentFps); mtcOutStatusText = "TX: " + mtcOutput.getCurrentDeviceName(); } else mtcOutStatusText = "FAILED TO OPEN"; saveSettings(); }
    };
    addAndMakeVisible(lblOutputMtcStatus); styleLabel(lblOutputMtcStatus); lblOutputMtcStatus.setColour(juce::Label::textColourId, accentRed);
    addAndMakeVisible(sldMtcOffset); styleOffsetSlider(sldMtcOffset);
    addAndMakeVisible(lblMtcOffset); lblMtcOffset.setText("MTC OFFSET:", juce::dontSendNotification); styleLabel(lblMtcOffset);
    sldMtcOffset.onValueChange = [this] { mtcOutputOffset = (int)sldMtcOffset.getValue(); saveSettings(); };

    addLabelAndCombo(lblArtnetOutputInterface, cmbArtnetOutputInterface, "ART-NET OUTPUT DEVICE:");
    cmbArtnetOutputInterface.onChange = [this]
    {
        if (outputArtnetEnabled)
        { int sel = cmbArtnetOutputInterface.getSelectedId() - 1; stopArtnetOutput(); artnetOutput.refreshNetworkInterfaces(); if (artnetOutput.start(sel, 6454)) { artnetOutput.setFrameRate(currentFps); artnetOutStatusText = "TX: " + artnetOutput.getBroadcastIp() + ":6454"; } else artnetOutStatusText = "FAILED TO BIND"; saveSettings(); }
    };
    addAndMakeVisible(lblOutputArtnetStatus); styleLabel(lblOutputArtnetStatus); lblOutputArtnetStatus.setColour(juce::Label::textColourId, accentOrange);
    addAndMakeVisible(sldArtnetOffset); styleOffsetSlider(sldArtnetOffset);
    addAndMakeVisible(lblArtnetOffset); lblArtnetOffset.setText("ART-NET OFFSET:", juce::dontSendNotification); styleLabel(lblArtnetOffset);
    sldArtnetOffset.onValueChange = [this] { artnetOutputOffset = (int)sldArtnetOffset.getValue(); saveSettings(); };

    addLabelAndCombo(lblAudioOutputTypeFilter, cmbAudioOutputTypeFilter, "AUDIO DRIVER:");
    cmbAudioOutputTypeFilter.onChange = [this]
    {
        populateFilteredOutputDeviceCombos();
        if (outputLtcEnabled) { stopLtcOutput(); startLtcOutput(); }
        if (outputThruEnabled) startThruOutput();
        saveSettings();
    };

    addLabelAndCombo(lblAudioOutputDevice, cmbAudioOutputDevice, "LTC OUTPUT DEVICE:");
    cmbAudioOutputDevice.onChange = [this]
    {
        if (cmbAudioOutputDevice.getSelectedId() > 0
            && cmbAudioOutputDevice.getSelectedId() != 999 && outputLtcEnabled)
        { stopLtcOutput(); startLtcOutput(); saveSettings(); }
    };

    addLabelAndCombo(lblAudioOutputChannel, cmbAudioOutputChannel, "LTC CHANNEL:");
    cmbAudioOutputChannel.onChange = [this]
    {
        if (outputLtcEnabled && cmbAudioOutputDevice.getSelectedId() > 0
            && cmbAudioOutputDevice.getSelectedId() != 999)
        { stopLtcOutput(); startLtcOutput(); saveSettings(); }
    };

    addAndMakeVisible(sldLtcOutputGain); styleGainSlider(sldLtcOutputGain);
    addAndMakeVisible(lblLtcOutputGain); lblLtcOutputGain.setText("LTC OUTPUT GAIN:", juce::dontSendNotification); styleLabel(lblLtcOutputGain);
    addAndMakeVisible(mtrLtcOutput); mtrLtcOutput.setColour(accentPurple);
    sldLtcOutputGain.onValueChange = [this] { ltcOutput.setOutputGain((float)sldLtcOutputGain.getValue() / 100.0f); saveSettings(); };

    addAndMakeVisible(lblOutputLtcStatus); styleLabel(lblOutputLtcStatus); lblOutputLtcStatus.setColour(juce::Label::textColourId, accentPurple);
    addAndMakeVisible(sldLtcOffset); styleOffsetSlider(sldLtcOffset);
    addAndMakeVisible(lblLtcOffset); lblLtcOffset.setText("LTC OFFSET:", juce::dontSendNotification); styleLabel(lblLtcOffset);
    sldLtcOffset.onValueChange = [this] { ltcOutputOffset = (int)sldLtcOffset.getValue(); saveSettings(); };

    addLabelAndCombo(lblThruOutputDevice, cmbThruOutputDevice, "AUDIO THRU OUTPUT DEVICE:");
    cmbThruOutputDevice.onChange = [this]
    {
        if (outputThruEnabled && cmbThruOutputDevice.getSelectedId() != 999)
        { startThruOutput(); saveSettings(); }
    };

    addLabelAndCombo(lblThruOutputChannel, cmbThruOutputChannel, "AUDIO THRU OUTPUT CHANNEL:");
    cmbThruOutputChannel.onChange = [this]
    {
        if (outputThruEnabled && cmbThruOutputDevice.getSelectedId() != 999)
        { startThruOutput(); saveSettings(); }
    };

    addAndMakeVisible(sldThruOutputGain); styleGainSlider(sldThruOutputGain);
    addAndMakeVisible(lblThruOutputGain); lblThruOutputGain.setText("AUDIO THRU OUTPUT GAIN:", juce::dontSendNotification); styleLabel(lblThruOutputGain);
    addAndMakeVisible(mtrThruOutput); mtrThruOutput.setColour(accentCyan);
    sldThruOutputGain.onValueChange = [this] { audioThru.setOutputGain((float)sldThruOutputGain.getValue() / 100.0f); saveSettings(); };

    addAndMakeVisible(lblOutputThruStatus); styleLabel(lblOutputThruStatus); lblOutputThruStatus.setColour(juce::Label::textColourId, accentCyan);

    addAndMakeVisible(btnRefreshDevices);
    btnRefreshDevices.onClick = [this] { populateMidiAndNetworkCombos(); startAudioDeviceScan(); };
    btnRefreshDevices.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF1A1D23));
    btnRefreshDevices.setColour(juce::TextButton::textColourOffId, textMid);

    addAndMakeVisible(btnGitHub);
    btnGitHub.setFont(juce::Font(juce::FontOptions("Consolas", 9.0f, juce::Font::plain)), false);
    btnGitHub.setColour(juce::HyperlinkButton::textColourId, juce::Colour(0xFF546E7A));

    // =====================================================================
    // STARTUP
    // =====================================================================
    populateMidiAndNetworkCombos();
    loadAndApplyNonAudioSettings();

    for (auto* cmb : { &cmbAudioInputDevice, &cmbAudioOutputDevice, &cmbThruOutputDevice })
        cmb->addItem("Scanning...", 999);

    startTimerHz(60);
    startAudioDeviceScan();
}

MainComponent::~MainComponent()
{
    setLookAndFeel(nullptr);
    saveSettings();
    stopTimer();

    if (scanThread && scanThread->isThreadRunning())
        scanThread->stopThread(2000);

    stopMtcInput();
    stopArtnetInput();
    stopThruOutput();
    stopLtcInput();
    stopMtcOutput();
    stopArtnetOutput();
    stopLtcOutput();
}

//==============================================================================
// BACKGROUND AUDIO SCAN
//==============================================================================
void MainComponent::startAudioDeviceScan()
{
    if (scanThread && scanThread->isThreadRunning())
        scanThread->stopThread(2000);
    scanThread = std::make_unique<AudioScanThread>(this);
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
// DRIVER TYPE FILTER HELPERS
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
    if (sel <= 1) return {};
    return cmbAudioInputTypeFilter.getText();
}

juce::String MainComponent::getOutputTypeFilter() const
{
    int sel = cmbAudioOutputTypeFilter.getSelectedId();
    if (sel <= 1) return {};
    return cmbAudioOutputTypeFilter.getText();
}

void MainComponent::populateTypeFilterCombos()
{
    auto inputTypes = getUniqueTypeNames(scannedAudioInputs);
    juce::String prevInputFilter = getInputTypeFilter();

    cmbAudioInputTypeFilter.clear(juce::dontSendNotification);
    cmbAudioInputTypeFilter.addItem("ALL", 1);
    for (int i = 0; i < inputTypes.size(); i++)
    {
        auto shortName = AudioDeviceEntry::shortenTypeName(inputTypes[i]);
        cmbAudioInputTypeFilter.addItem(shortName.isEmpty() ? inputTypes[i] : shortName, i + 2);
    }

    juce::String filterToApply = prevInputFilter.isNotEmpty() ? prevInputFilter : settings.audioInputTypeFilter;
    bool found = false;
    if (filterToApply.isNotEmpty())
    {
        for (int i = 0; i < inputTypes.size(); i++)
        {
            auto shortName = AudioDeviceEntry::shortenTypeName(inputTypes[i]);
            auto displayName = shortName.isEmpty() ? inputTypes[i] : shortName;
            if (displayName == filterToApply || inputTypes[i] == filterToApply)
            { cmbAudioInputTypeFilter.setSelectedId(i + 2, juce::dontSendNotification); found = true; break; }
        }
    }
    if (!found) cmbAudioInputTypeFilter.setSelectedId(1, juce::dontSendNotification);

    auto outputTypes = getUniqueTypeNames(scannedAudioOutputs);
    juce::String prevOutputFilter = getOutputTypeFilter();

    cmbAudioOutputTypeFilter.clear(juce::dontSendNotification);
    cmbAudioOutputTypeFilter.addItem("ALL", 1);
    for (int i = 0; i < outputTypes.size(); i++)
    {
        auto shortName = AudioDeviceEntry::shortenTypeName(outputTypes[i]);
        cmbAudioOutputTypeFilter.addItem(shortName.isEmpty() ? outputTypes[i] : shortName, i + 2);
    }

    filterToApply = prevOutputFilter.isNotEmpty() ? prevOutputFilter : settings.audioOutputTypeFilter;
    found = false;
    if (filterToApply.isNotEmpty())
    {
        for (int i = 0; i < outputTypes.size(); i++)
        {
            auto shortName = AudioDeviceEntry::shortenTypeName(outputTypes[i]);
            auto displayName = shortName.isEmpty() ? outputTypes[i] : shortName;
            if (displayName == filterToApply || outputTypes[i] == filterToApply)
            { cmbAudioOutputTypeFilter.setSelectedId(i + 2, juce::dontSendNotification); found = true; break; }
        }
    }
    if (!found) cmbAudioOutputTypeFilter.setSelectedId(1, juce::dontSendNotification);
}

void MainComponent::populateFilteredInputDeviceCombo()
{
    auto prevEntry = getSelectedAudioInput();

    juce::String filter;
    int filterId = cmbAudioInputTypeFilter.getSelectedId();
    if (filterId >= 2)
    {
        auto inputTypes = getUniqueTypeNames(scannedAudioInputs);
        int typeIdx = filterId - 2;
        if (typeIdx >= 0 && typeIdx < inputTypes.size()) filter = inputTypes[typeIdx];
    }

    filteredInputIndices.clear();
    cmbAudioInputDevice.clear(juce::dontSendNotification);

    for (int i = 0; i < scannedAudioInputs.size(); i++)
    {
        if (filter.isEmpty() || scannedAudioInputs[i].typeName == filter)
        {
            filteredInputIndices.add(i);
            juce::String displayName = filter.isNotEmpty()
                ? scannedAudioInputs[i].deviceName : scannedAudioInputs[i].displayName;
            cmbAudioInputDevice.addItem(displayName, filteredInputIndices.size());
        }
    }

    if (filteredInputIndices.isEmpty())
        cmbAudioInputDevice.addItem("(No audio devices)", 999);

    if (prevEntry.deviceName.isNotEmpty())
    {
        int idx = findFilteredIndex(filteredInputIndices, scannedAudioInputs, prevEntry.typeName, prevEntry.deviceName);
        if (idx >= 0) cmbAudioInputDevice.setSelectedId(idx + 1, juce::dontSendNotification);
    }
}

void MainComponent::populateFilteredOutputDeviceCombos()
{
    auto prevLtcEntry = getSelectedAudioOutput();
    auto prevThruEntry = getSelectedThruOutput();

    juce::String filter;
    int filterId = cmbAudioOutputTypeFilter.getSelectedId();
    if (filterId >= 2)
    {
        auto outputTypes = getUniqueTypeNames(scannedAudioOutputs);
        int typeIdx = filterId - 2;
        if (typeIdx >= 0 && typeIdx < outputTypes.size()) filter = outputTypes[typeIdx];
    }

    filteredOutputIndices.clear();
    cmbAudioOutputDevice.clear(juce::dontSendNotification);
    cmbThruOutputDevice.clear(juce::dontSendNotification);

    for (int i = 0; i < scannedAudioOutputs.size(); i++)
    {
        if (filter.isEmpty() || scannedAudioOutputs[i].typeName == filter)
        {
            filteredOutputIndices.add(i);
            juce::String displayName = filter.isNotEmpty()
                ? scannedAudioOutputs[i].deviceName : scannedAudioOutputs[i].displayName;
            int id = filteredOutputIndices.size();
            cmbAudioOutputDevice.addItem(displayName, id);
            cmbThruOutputDevice.addItem(displayName, id);
        }
    }

    if (filteredOutputIndices.isEmpty())
    {
        cmbAudioOutputDevice.addItem("(No audio devices)", 999);
        cmbThruOutputDevice.addItem("(No audio devices)", 999);
    }

    if (prevLtcEntry.deviceName.isNotEmpty())
    {
        int idx = findFilteredIndex(filteredOutputIndices, scannedAudioOutputs, prevLtcEntry.typeName, prevLtcEntry.deviceName);
        if (idx >= 0) cmbAudioOutputDevice.setSelectedId(idx + 1, juce::dontSendNotification);
    }

    if (prevThruEntry.deviceName.isNotEmpty())
    {
        int idx = findFilteredIndex(filteredOutputIndices, scannedAudioOutputs, prevThruEntry.typeName, prevThruEntry.deviceName);
        if (idx >= 0) cmbThruOutputDevice.setSelectedId(idx + 1, juce::dontSendNotification);
    }
}

int MainComponent::findFilteredIndex(const juce::Array<int>& filteredIndices,
                                      const juce::Array<AudioDeviceEntry>& entries,
                                      const juce::String& typeName, const juce::String& deviceName)
{
    if (deviceName.isEmpty()) return -1;
    if (typeName.isNotEmpty())
        for (int i = 0; i < filteredIndices.size(); i++)
        {
            int realIdx = filteredIndices[i];
            if (realIdx >= 0 && realIdx < entries.size()
                && entries[realIdx].typeName == typeName && entries[realIdx].deviceName == deviceName)
                return i;
        }
    for (int i = 0; i < filteredIndices.size(); i++)
    {
        int realIdx = filteredIndices[i];
        if (realIdx >= 0 && realIdx < entries.size() && entries[realIdx].deviceName == deviceName)
            return i;
    }
    return -1;
}

//==============================================================================
// SAMPLE RATE / BUFFER SIZE
//==============================================================================
void MainComponent::populateSampleRateCombo()
{
    cmbSampleRate.clear(juce::dontSendNotification);
    cmbSampleRate.addItem("Default", 1);
    cmbSampleRate.addItem("44100 Hz", 2);
    cmbSampleRate.addItem("48000 Hz", 3);
    cmbSampleRate.addItem("88200 Hz", 4);
    cmbSampleRate.addItem("96000 Hz", 5);
    cmbSampleRate.addItem("176400 Hz", 6);
    cmbSampleRate.addItem("192000 Hz", 7);
    cmbSampleRate.setSelectedId(1, juce::dontSendNotification);
}

void MainComponent::populateBufferSizeCombo()
{
    cmbBufferSize.clear(juce::dontSendNotification);
    cmbBufferSize.addItem("Default", 1);
    cmbBufferSize.addItem("32 smp", 2);
    cmbBufferSize.addItem("64 smp", 3);
    cmbBufferSize.addItem("128 smp", 4);
    cmbBufferSize.addItem("256 smp", 5);
    cmbBufferSize.addItem("512 smp", 6);
    cmbBufferSize.addItem("1024 smp", 7);
    cmbBufferSize.addItem("2048 smp", 8);
    cmbBufferSize.addItem("4096 smp", 9);
    cmbBufferSize.setSelectedId(1, juce::dontSendNotification);
}

double MainComponent::getPreferredSampleRate() const
{
    switch (cmbSampleRate.getSelectedId())
    {
        case 2: return 44100.0;  case 3: return 48000.0;
        case 4: return 88200.0;  case 5: return 96000.0;
        case 6: return 176400.0; case 7: return 192000.0;
        default: return 0.0;
    }
}

int MainComponent::getPreferredBufferSize() const
{
    switch (cmbBufferSize.getSelectedId())
    {
        case 2: return 32;   case 3: return 64;   case 4: return 128;
        case 5: return 256;  case 6: return 512;  case 7: return 1024;
        case 8: return 2048; case 9: return 4096;
        default: return 0;
    }
}

static int sampleRateToComboId(double sr)
{
    if (sr <= 0) return 1;
    if (std::abs(sr - 44100.0) < 1.0)  return 2;
    if (std::abs(sr - 48000.0) < 1.0)  return 3;
    if (std::abs(sr - 88200.0) < 1.0)  return 4;
    if (std::abs(sr - 96000.0) < 1.0)  return 5;
    if (std::abs(sr - 176400.0) < 1.0) return 6;
    if (std::abs(sr - 192000.0) < 1.0) return 7;
    return 1;
}

static int bufferSizeToComboId(int bs)
{
    switch (bs)
    {
        case 32: return 2; case 64: return 3; case 128: return 4;
        case 256: return 5; case 512: return 6; case 1024: return 7;
        case 2048: return 8; case 4096: return 9; default: return 1;
    }
}

void MainComponent::restartAllAudioDevices()
{
    bool ltcInWasRunning  = ltcInput.getIsRunning();
    bool ltcOutWasRunning = ltcOutput.getIsRunning();
    bool thruWasRunning   = audioThru.getIsRunning();

    if (thruWasRunning)   stopThruOutput();
    if (ltcInWasRunning)  ltcInput.stop();
    if (ltcOutWasRunning) stopLtcOutput();

    if (ltcInWasRunning && activeInput == InputSource::LTC) startLtcInput();
    if (ltcOutWasRunning && outputLtcEnabled) startLtcOutput();
    if (thruWasRunning && outputThruEnabled)  startThruOutput();
}

//==============================================================================
// DEVICE COMBO POPULATION
//==============================================================================
void MainComponent::populateMidiAndNetworkCombos()
{
    mtcInput.refreshDeviceList();
    cmbMidiInputDevice.clear(juce::dontSendNotification);
    auto inNames = mtcInput.getDeviceNames();
    for (int i = 0; i < inNames.size(); i++) cmbMidiInputDevice.addItem(inNames[i], i + 1);
    if (inNames.isEmpty()) cmbMidiInputDevice.addItem("(No MIDI devices)", 999);

    mtcOutput.refreshDeviceList();
    cmbMidiOutputDevice.clear(juce::dontSendNotification);
    auto outNames = mtcOutput.getDeviceNames();
    for (int i = 0; i < outNames.size(); i++) cmbMidiOutputDevice.addItem(outNames[i], i + 1);
    if (outNames.isEmpty()) cmbMidiOutputDevice.addItem("(No MIDI devices)", 999);

    artnetInput.refreshNetworkInterfaces();
    cmbArtnetInputInterface.clear(juce::dontSendNotification);
    auto inIfaces = artnetInput.getInterfaceNames();
    for (int i = 0; i < inIfaces.size(); i++) cmbArtnetInputInterface.addItem(inIfaces[i], i + 1);
    if (inIfaces.isEmpty()) cmbArtnetInputInterface.addItem("(No interfaces)", 999);

    artnetOutput.refreshNetworkInterfaces();
    cmbArtnetOutputInterface.clear(juce::dontSendNotification);
    auto outIfaces = artnetOutput.getInterfaceNames();
    for (int i = 0; i < outIfaces.size(); i++) cmbArtnetOutputInterface.addItem(outIfaces[i], i + 1);
    if (outIfaces.isEmpty()) cmbArtnetOutputInterface.addItem("(No interfaces)", 999);
}

void MainComponent::populateAudioCombos()
{
    populateTypeFilterCombos();
    populateFilteredInputDeviceCombo();
    populateFilteredOutputDeviceCombos();

    auto resetChannels = [](juce::ComboBox& cmb, int defId, int kStereo = -1)
    {
        if (cmb.getNumItems() == 0)
        {
            if (kStereo > 0) cmb.addItem("Ch 1 + Ch 2", kStereo);
            cmb.addItem("Ch 1", 1);
            cmb.addItem("Ch 2", 2);
            cmb.setSelectedId(defId, juce::dontSendNotification);
        }
    };

    resetChannels(cmbAudioInputChannel, 1);
    resetChannels(cmbThruInputChannel, 2);
    resetChannels(cmbAudioOutputChannel, kStereoItemId, kStereoItemId);
    resetChannels(cmbThruOutputChannel, kStereoItemId, kStereoItemId);
}

int MainComponent::findAudioDevice(const juce::Array<AudioDeviceEntry>& entries,
                                    const juce::String& typeName, const juce::String& deviceName)
{
    if (deviceName.isEmpty()) return -1;
    if (typeName.isNotEmpty())
        for (int i = 0; i < entries.size(); i++)
            if (entries[i].typeName == typeName && entries[i].deviceName == deviceName)
                return i;
    for (int i = 0; i < entries.size(); i++)
        if (entries[i].deviceName == deviceName) return i;
    return -1;
}

AudioDeviceEntry MainComponent::getSelectedAudioInput() const
{
    int filtIdx = cmbAudioInputDevice.getSelectedId() - 1;
    if (filtIdx >= 0 && filtIdx < filteredInputIndices.size())
    {
        int realIdx = filteredInputIndices[filtIdx];
        if (realIdx >= 0 && realIdx < scannedAudioInputs.size())
            return scannedAudioInputs[realIdx];
    }
    return {};
}

AudioDeviceEntry MainComponent::getSelectedAudioOutput() const
{
    int filtIdx = cmbAudioOutputDevice.getSelectedId() - 1;
    if (filtIdx >= 0 && filtIdx < filteredOutputIndices.size())
    {
        int realIdx = filteredOutputIndices[filtIdx];
        if (realIdx >= 0 && realIdx < scannedAudioOutputs.size())
            return scannedAudioOutputs[realIdx];
    }
    return {};
}

AudioDeviceEntry MainComponent::getSelectedThruOutput() const
{
    int filtIdx = cmbThruOutputDevice.getSelectedId() - 1;
    if (filtIdx >= 0 && filtIdx < filteredOutputIndices.size())
    {
        int realIdx = filteredOutputIndices[filtIdx];
        if (realIdx >= 0 && realIdx < scannedAudioOutputs.size())
            return scannedAudioOutputs[realIdx];
    }
    return {};
}

int MainComponent::getChannelFromCombo(const juce::ComboBox& cmb) const
{
    return (cmb.getSelectedId() == kStereoItemId) ? -1 : (cmb.getSelectedId() - 1);
}

//==============================================================================
// SETTINGS Ã¢â‚¬â€œ Phase 1
//==============================================================================
void MainComponent::loadAndApplyNonAudioSettings()
{
    if (!settings.load())
    {
        settingsLoaded = true;
        updateInputButtonStates();
        updateFpsButtonStates();
        updateDeviceSelectorVisibility();
        return;
    }

    FrameRate fpsValues[] = { FrameRate::FPS_24, FrameRate::FPS_25, FrameRate::FPS_2997, FrameRate::FPS_30 };
    currentFps = fpsValues[juce::jlimit(0, 3, settings.fpsSelection)];

    int midiInIdx = findDeviceByName(cmbMidiInputDevice, settings.midiInputDevice);
    if (midiInIdx >= 0) cmbMidiInputDevice.setSelectedId(midiInIdx + 1, juce::dontSendNotification);

    int midiOutIdx = findDeviceByName(cmbMidiOutputDevice, settings.midiOutputDevice);
    if (midiOutIdx >= 0) cmbMidiOutputDevice.setSelectedId(midiOutIdx + 1, juce::dontSendNotification);

    if (settings.artnetInputInterface >= 0 && settings.artnetInputInterface < cmbArtnetInputInterface.getNumItems())
        cmbArtnetInputInterface.setSelectedId(settings.artnetInputInterface + 1, juce::dontSendNotification);
    if (settings.artnetOutputInterface >= 0 && settings.artnetOutputInterface < cmbArtnetOutputInterface.getNumItems())
        cmbArtnetOutputInterface.setSelectedId(settings.artnetOutputInterface + 1, juce::dontSendNotification);

    sldLtcInputGain.setValue(settings.ltcInputGain, juce::dontSendNotification);
    sldThruInputGain.setValue(settings.thruInputGain, juce::dontSendNotification);
    sldLtcOutputGain.setValue(settings.ltcOutputGain, juce::dontSendNotification);
    sldThruOutputGain.setValue(settings.thruOutputGain, juce::dontSendNotification);

    cmbSampleRate.setSelectedId(sampleRateToComboId(settings.preferredSampleRate), juce::dontSendNotification);
    cmbBufferSize.setSelectedId(bufferSizeToComboId(settings.preferredBufferSize), juce::dontSendNotification);

    if (settings.audioInputChannel >= 0)
        cmbAudioInputChannel.setSelectedId(settings.audioInputChannel + 1, juce::dontSendNotification);
    if (settings.thruInputChannel >= 0)
        cmbThruInputChannel.setSelectedId(settings.thruInputChannel + 1, juce::dontSendNotification);
    if (settings.audioOutputStereo)
        cmbAudioOutputChannel.setSelectedId(kStereoItemId, juce::dontSendNotification);
    else if (settings.audioOutputChannel >= 0)
        cmbAudioOutputChannel.setSelectedId(settings.audioOutputChannel + 1, juce::dontSendNotification);
    if (settings.thruOutputStereo)
        cmbThruOutputChannel.setSelectedId(kStereoItemId, juce::dontSendNotification);
    else if (settings.thruOutputChannel >= 0)
        cmbThruOutputChannel.setSelectedId(settings.thruOutputChannel + 1, juce::dontSendNotification);

    outputMtcEnabled = settings.mtcOutEnabled;
    outputArtnetEnabled = settings.artnetOutEnabled;
    outputLtcEnabled = settings.ltcOutEnabled;
    outputThruEnabled = settings.thruOutEnabled;

    mtcOutputOffset = settings.mtcOutputOffset;
    artnetOutputOffset = settings.artnetOutputOffset;
    ltcOutputOffset = settings.ltcOutputOffset;
    sldMtcOffset.setValue(mtcOutputOffset, juce::dontSendNotification);
    sldArtnetOffset.setValue(artnetOutputOffset, juce::dontSendNotification);
    sldLtcOffset.setValue(ltcOutputOffset, juce::dontSendNotification);

    btnMtcOut.setToggleState(outputMtcEnabled, juce::dontSendNotification);
    btnArtnetOut.setToggleState(outputArtnetEnabled, juce::dontSendNotification);
    btnLtcOut.setToggleState(outputLtcEnabled, juce::dontSendNotification);
    btnThruOut.setToggleState(outputThruEnabled, juce::dontSendNotification);

    if (outputMtcEnabled) startMtcOutput();
    if (outputArtnetEnabled) startArtnetOutput();

    InputSource src = stringToInputSource(settings.inputSource);
    activeInput = src;
    sourceActive = false;

    switch (src)
    {
        case InputSource::MTC:        startMtcInput();     break;
        case InputSource::ArtNet:     startArtnetInput();  break;
        case InputSource::SystemTime: sourceActive = true; break;
        case InputSource::LTC:        break;
    }

    settingsLoaded = true;
    updateInputButtonStates();
    updateFpsButtonStates();
    updateDeviceSelectorVisibility();
}

//==============================================================================
// SETTINGS Ã¢â‚¬â€œ Phase 2
//==============================================================================
void MainComponent::applyAudioSettings()
{
    int audioInIdx = findFilteredIndex(filteredInputIndices, scannedAudioInputs,
                                        settings.audioInputType, settings.audioInputDevice);
    if (audioInIdx >= 0) cmbAudioInputDevice.setSelectedId(audioInIdx + 1, juce::dontSendNotification);

    int audioOutIdx = findFilteredIndex(filteredOutputIndices, scannedAudioOutputs,
                                         settings.audioOutputType, settings.audioOutputDevice);
    if (audioOutIdx >= 0) cmbAudioOutputDevice.setSelectedId(audioOutIdx + 1, juce::dontSendNotification);

    int thruOutIdx = findFilteredIndex(filteredOutputIndices, scannedAudioOutputs,
                                        settings.thruOutputType, settings.thruOutputDevice);
    if (thruOutIdx >= 0) cmbThruOutputDevice.setSelectedId(thruOutIdx + 1, juce::dontSendNotification);

    ltcInput.setInputGain((float)settings.ltcInputGain / 100.0f);
    ltcInput.setPassthruGain((float)settings.thruInputGain / 100.0f);
    ltcOutput.setOutputGain((float)settings.ltcOutputGain / 100.0f);
    audioThru.setOutputGain((float)settings.thruOutputGain / 100.0f);

    if (outputLtcEnabled) startLtcOutput();

    if (activeInput == InputSource::LTC) startLtcInput();

    if (outputThruEnabled && !audioThru.getIsRunning())
    {
        if (activeInput == InputSource::LTC && ltcInput.getIsRunning())
            startThruOutput();
    }

    updateDeviceSelectorVisibility();
}

//==============================================================================
// SAVE SETTINGS
//==============================================================================
void MainComponent::saveSettings()
{
    if (!settingsLoaded) return;

    settings.inputSource = inputSourceToString(activeInput);
    settings.fpsSelection = (currentFps == FrameRate::FPS_24) ? 0 : (currentFps == FrameRate::FPS_25) ? 1 : (currentFps == FrameRate::FPS_2997) ? 2 : 3;

    settings.midiInputDevice = cmbMidiInputDevice.getText();
    settings.midiOutputDevice = cmbMidiOutputDevice.getText();
    settings.artnetInputInterface = cmbArtnetInputInterface.getSelectedId() - 1;
    settings.artnetOutputInterface = cmbArtnetOutputInterface.getSelectedId() - 1;

    bool audioReady = !scannedAudioInputs.isEmpty() || !scannedAudioOutputs.isEmpty();
    if (audioReady)
    {
        auto inEntry = getSelectedAudioInput();
        if (inEntry.deviceName.isNotEmpty()) { settings.audioInputDevice = inEntry.deviceName; settings.audioInputType = inEntry.typeName; }
        settings.audioInputChannel = cmbAudioInputChannel.getSelectedId() - 1;

        auto outEntry = getSelectedAudioOutput();
        if (outEntry.deviceName.isNotEmpty()) { settings.audioOutputDevice = outEntry.deviceName; settings.audioOutputType = outEntry.typeName; }
        settings.audioOutputStereo = (cmbAudioOutputChannel.getSelectedId() == kStereoItemId);
        settings.audioOutputChannel = settings.audioOutputStereo ? 0 : (cmbAudioOutputChannel.getSelectedId() - 1);

        auto thruEntry = getSelectedThruOutput();
        if (thruEntry.deviceName.isNotEmpty()) { settings.thruOutputDevice = thruEntry.deviceName; settings.thruOutputType = thruEntry.typeName; }
        settings.thruOutputStereo = (cmbThruOutputChannel.getSelectedId() == kStereoItemId);
        settings.thruOutputChannel = settings.thruOutputStereo ? 0 : (cmbThruOutputChannel.getSelectedId() - 1);
        settings.thruInputChannel = cmbThruInputChannel.getSelectedId() - 1;
    }

    settings.mtcOutEnabled = outputMtcEnabled;
    settings.artnetOutEnabled = outputArtnetEnabled;
    settings.ltcOutEnabled = outputLtcEnabled;
    settings.thruOutEnabled = outputThruEnabled;

    settings.mtcOutputOffset = mtcOutputOffset;
    settings.artnetOutputOffset = artnetOutputOffset;
    settings.ltcOutputOffset = ltcOutputOffset;

    settings.ltcInputGain   = (int)sldLtcInputGain.getValue();
    settings.thruInputGain  = (int)sldThruInputGain.getValue();
    settings.ltcOutputGain  = (int)sldLtcOutputGain.getValue();
    settings.thruOutputGain = (int)sldThruOutputGain.getValue();

    settings.audioInputTypeFilter  = cmbAudioInputTypeFilter.getText();
    settings.audioOutputTypeFilter = cmbAudioOutputTypeFilter.getText();
    settings.preferredSampleRate = getPreferredSampleRate();
    settings.preferredBufferSize = getPreferredBufferSize();

    settings.save();
}

int MainComponent::findDeviceByName(const juce::ComboBox& cmb, const juce::String& name)
{
    if (name.isEmpty()) return -1;
    for (int i = 0; i < cmb.getNumItems(); i++)
        if (cmb.getItemText(i) == name) return i;
    return -1;
}

juce::String MainComponent::inputSourceToString(InputSource src) const
{
    switch (src) { case InputSource::MTC: return "MTC"; case InputSource::ArtNet: return "ArtNet"; case InputSource::SystemTime: return "SystemTime"; case InputSource::LTC: return "LTC"; }
    return "SystemTime";
}

MainComponent::InputSource MainComponent::stringToInputSource(const juce::String& s) const
{
    if (s == "MTC") return InputSource::MTC;
    if (s == "ArtNet") return InputSource::ArtNet;
    if (s == "LTC") return InputSource::LTC;
    return InputSource::SystemTime;
}

//==============================================================================
// INPUT/OUTPUT CONTROL (unchanged logic)
//==============================================================================
void MainComponent::startMtcInput()
{
    stopMtcInput();
    int sel = cmbMidiInputDevice.getSelectedId() - 1;
    if (sel < 0 && mtcInput.getDeviceCount() > 0) sel = 0;
    if (sel >= 0 && mtcInput.start(sel))
    { inputStatusText = "RX: " + mtcInput.getCurrentDeviceName(); cmbMidiInputDevice.setSelectedId(sel + 1, juce::dontSendNotification); }
    else inputStatusText = sel < 0 ? "NO MIDI DEVICE AVAILABLE" : "FAILED TO OPEN DEVICE";
}

void MainComponent::stopMtcInput() { mtcInput.stop(); }

void MainComponent::startArtnetInput()
{
    stopArtnetInput();
    int sel = cmbArtnetInputInterface.getSelectedId() - 1;
    if (sel < 0) sel = 0;
    artnetInput.refreshNetworkInterfaces();
    if (artnetInput.start(sel, 6454))
    { inputStatusText = "RX ON " + artnetInput.getBindInfo(); cmbArtnetInputInterface.setSelectedId(sel + 1, juce::dontSendNotification); }
    else inputStatusText = "FAILED TO BIND PORT 6454";
}

void MainComponent::stopArtnetInput() { artnetInput.stop(); }

void MainComponent::startLtcInput()
{
    stopLtcInput();
    auto entry = getSelectedAudioInput();
    if (entry.deviceName.isEmpty() && !filteredInputIndices.isEmpty())
    { cmbAudioInputDevice.setSelectedId(1, juce::dontSendNotification); entry = getSelectedAudioInput(); }

    if (entry.deviceName.isEmpty()) { inputStatusText = "NO AUDIO DEVICE AVAILABLE"; return; }

    int ltcCh = cmbAudioInputChannel.getSelectedId() - 1;
    if (ltcCh < 0) ltcCh = 0;

    int thruCh = -1;
    if (outputThruEnabled) { thruCh = cmbThruInputChannel.getSelectedId() - 1; if (thruCh < 0) thruCh = 1; }

    if (ltcInput.start(entry.typeName, entry.deviceName, ltcCh, thruCh,
                       getPreferredSampleRate(), getPreferredBufferSize()))
    {
        ltcInput.setInputGain((float)sldLtcInputGain.getValue() / 100.0f);
        ltcInput.setPassthruGain((float)sldThruInputGain.getValue() / 100.0f);
        inputStatusText = "RX: " + ltcInput.getCurrentDeviceName() + " Ch " + juce::String(ltcCh + 1);
        populateAudioInputChannels();
        if (outputThruEnabled) startThruOutput();
    }
    else inputStatusText = "FAILED TO OPEN AUDIO DEVICE";
}

void MainComponent::stopLtcInput() { stopThruOutput(); ltcInput.stop(); }
void MainComponent::restartLtcWithCurrentSettings() { startLtcInput(); saveSettings(); }

void MainComponent::startThruOutput()
{
    stopThruOutput();
    if (!ltcInput.getIsRunning() || !ltcInput.hasPassthruChannel()) { thruOutStatusText = "WAITING FOR LTC INPUT"; return; }

    auto entry = getSelectedThruOutput();
    if (entry.deviceName.isEmpty() && !filteredOutputIndices.isEmpty())
    { cmbThruOutputDevice.setSelectedId(1, juce::dontSendNotification); entry = getSelectedThruOutput(); }

    if (entry.deviceName.isEmpty()) { thruOutStatusText = "NO AUDIO DEVICE"; return; }

    if (outputLtcEnabled && ltcOutput.getIsRunning() && ltcOutput.getCurrentDeviceName() == entry.deviceName)
    { thruOutStatusText = "CONFLICT: same device as LTC OUT"; return; }

    int outCh = getChannelFromCombo(cmbThruOutputChannel);

    if (audioThru.start(entry.typeName, entry.deviceName, outCh, &ltcInput,
                        getPreferredSampleRate(), getPreferredBufferSize()))
    {
        audioThru.setOutputGain((float)sldThruOutputGain.getValue() / 100.0f);
        juce::String chName = (outCh == -1) ? "Ch 1 + Ch 2" : ("Ch " + juce::String(outCh + 1));
        thruOutStatusText = "THRU: " + audioThru.getCurrentDeviceName() + " " + chName;
        populateThruOutputChannels();
    }
    else thruOutStatusText = "FAILED TO OPEN";
}

void MainComponent::stopThruOutput() { audioThru.stop(); thruOutStatusText = ""; }

void MainComponent::startMtcOutput()
{
    stopMtcOutput();
    int sel = cmbMidiOutputDevice.getSelectedId() - 1;
    if (sel < 0 && mtcOutput.getDeviceCount() > 0) sel = 0;
    if (sel >= 0 && mtcOutput.start(sel))
    { mtcOutput.setFrameRate(currentFps); mtcOutStatusText = "TX: " + mtcOutput.getCurrentDeviceName(); cmbMidiOutputDevice.setSelectedId(sel + 1, juce::dontSendNotification); }
    else mtcOutStatusText = sel < 0 ? "NO MIDI DEVICE" : "FAILED TO OPEN";
}

void MainComponent::stopMtcOutput() { mtcOutput.stop(); mtcOutStatusText = ""; }

void MainComponent::startArtnetOutput()
{
    stopArtnetOutput();
    int sel = cmbArtnetOutputInterface.getSelectedId() - 1;
    artnetOutput.refreshNetworkInterfaces();
    if (artnetOutput.start(sel, 6454))
    { artnetOutput.setFrameRate(currentFps); artnetOutStatusText = "TX: " + artnetOutput.getBroadcastIp() + ":6454"; if (sel >= 0) cmbArtnetOutputInterface.setSelectedId(sel + 1, juce::dontSendNotification); }
    else artnetOutStatusText = "FAILED TO BIND";
}

void MainComponent::stopArtnetOutput() { artnetOutput.stop(); artnetOutStatusText = ""; }

void MainComponent::startLtcOutput()
{
    stopLtcOutput();
    auto entry = getSelectedAudioOutput();
    if (entry.deviceName.isEmpty() && !filteredOutputIndices.isEmpty())
    { cmbAudioOutputDevice.setSelectedId(1, juce::dontSendNotification); entry = getSelectedAudioOutput(); }

    if (entry.deviceName.isEmpty()) { ltcOutStatusText = "NO AUDIO DEVICE AVAILABLE"; return; }

    if (audioThru.getIsRunning() && audioThru.getCurrentDeviceName() == entry.deviceName)
    { stopThruOutput(); thruOutStatusText = "CONFLICT: same device as LTC OUT"; }

    int channel = getChannelFromCombo(cmbAudioOutputChannel);

    if (ltcOutput.start(entry.typeName, entry.deviceName, channel,
                        getPreferredSampleRate(), getPreferredBufferSize()))
    {
        ltcOutput.setFrameRate(currentFps);
        ltcOutput.setOutputGain((float)sldLtcOutputGain.getValue() / 100.0f);
        juce::String chName = (channel == -1) ? "Ch 1 + Ch 2" : ("Ch " + juce::String(channel + 1));
        ltcOutStatusText = "TX: " + ltcOutput.getCurrentDeviceName() + " " + chName;
        populateAudioOutputChannels();
    }
    else ltcOutStatusText = "FAILED TO OPEN AUDIO DEVICE";
}

void MainComponent::stopLtcOutput() { ltcOutput.stop(); ltcOutStatusText = ""; }

void MainComponent::populateAudioInputChannels()
{
    int prevLtcCh = cmbAudioInputChannel.getSelectedId();
    int prevThru = cmbThruInputChannel.getSelectedId();
    cmbAudioInputChannel.clear(juce::dontSendNotification);
    cmbThruInputChannel.clear(juce::dontSendNotification);
    int n = juce::jmax(2, ltcInput.getChannelCount());
    for (int i = 0; i < n; i++) { auto nm = "Ch " + juce::String(i+1); cmbAudioInputChannel.addItem(nm, i+1); cmbThruInputChannel.addItem(nm, i+1); }
    cmbAudioInputChannel.setSelectedId((prevLtcCh > 0 && prevLtcCh <= n) ? prevLtcCh : 1, juce::dontSendNotification);
    cmbThruInputChannel.setSelectedId((prevThru > 0 && prevThru <= n) ? prevThru : juce::jmin(2, n), juce::dontSendNotification);
}

void MainComponent::populateAudioOutputChannels()
{
    int prev = cmbAudioOutputChannel.getSelectedId();
    cmbAudioOutputChannel.clear(juce::dontSendNotification);
    int n = juce::jmax(2, ltcOutput.getChannelCount());
    if (n >= 2) cmbAudioOutputChannel.addItem("Ch 1 + Ch 2", kStereoItemId);
    for (int i = 0; i < n; i++) cmbAudioOutputChannel.addItem("Ch " + juce::String(i+1), i+1);
    if (prev == kStereoItemId && n >= 2) cmbAudioOutputChannel.setSelectedId(kStereoItemId, juce::dontSendNotification);
    else if (prev > 0 && prev <= n) cmbAudioOutputChannel.setSelectedId(prev, juce::dontSendNotification);
    else cmbAudioOutputChannel.setSelectedId(n >= 2 ? kStereoItemId : 1, juce::dontSendNotification);
}

void MainComponent::populateThruOutputChannels()
{
    int prev = cmbThruOutputChannel.getSelectedId();
    cmbThruOutputChannel.clear(juce::dontSendNotification);
    int n = juce::jmax(2, audioThru.getChannelCount());
    if (n >= 2) cmbThruOutputChannel.addItem("Ch 1 + Ch 2", kStereoItemId);
    for (int i = 0; i < n; i++) cmbThruOutputChannel.addItem("Ch " + juce::String(i+1), i+1);
    if (prev == kStereoItemId && n >= 2) cmbThruOutputChannel.setSelectedId(kStereoItemId, juce::dontSendNotification);
    else if (prev > 0 && prev <= n) cmbThruOutputChannel.setSelectedId(prev, juce::dontSendNotification);
    else cmbThruOutputChannel.setSelectedId(n >= 2 ? kStereoItemId : 1, juce::dontSendNotification);
}

void MainComponent::updateOutputStates()
{
    if (outputMtcEnabled && !mtcOutput.getIsRunning()) startMtcOutput();
    else if (!outputMtcEnabled && mtcOutput.getIsRunning()) stopMtcOutput();

    if (outputArtnetEnabled && !artnetOutput.getIsRunning()) startArtnetOutput();
    else if (!outputArtnetEnabled && artnetOutput.getIsRunning()) stopArtnetOutput();

    if (outputLtcEnabled && !ltcOutput.getIsRunning() && !scannedAudioOutputs.isEmpty()) startLtcOutput();
    else if (!outputLtcEnabled && ltcOutput.getIsRunning()) stopLtcOutput();

    if (outputThruEnabled && !audioThru.getIsRunning())
    { if (activeInput == InputSource::LTC) restartLtcWithCurrentSettings(); else startThruOutput(); }
    else if (!outputThruEnabled && audioThru.getIsRunning())
    { stopThruOutput(); if (activeInput == InputSource::LTC) restartLtcWithCurrentSettings(); }
}

void MainComponent::routeTimecodeToOutputs()
{
    if (sourceActive)
    {
        if (outputMtcEnabled && mtcOutput.getIsRunning())
        {
            mtcOutput.setTimecode(offsetTimecode(currentTimecode, mtcOutputOffset, currentFps));
            mtcOutput.setPaused(false);
        }
        if (outputArtnetEnabled && artnetOutput.getIsRunning())
        {
            artnetOutput.setTimecode(offsetTimecode(currentTimecode, artnetOutputOffset, currentFps));
            artnetOutput.setPaused(false);
        }
        if (outputLtcEnabled && ltcOutput.getIsRunning())
        {
            ltcOutput.setTimecode(offsetTimecode(currentTimecode, ltcOutputOffset, currentFps));
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

//==============================================================================
// VISIBILITY (updated for collapse state)
//==============================================================================
void MainComponent::updateDeviceSelectorVisibility()
{
    bool showMidiIn   = (activeInput == InputSource::MTC)    && inputConfigExpanded;
    bool showArtnetIn = (activeInput == InputSource::ArtNet)  && inputConfigExpanded;
    bool showLtcIn    = (activeInput == InputSource::LTC)     && inputConfigExpanded;
    bool showAudioOut = (outputLtcEnabled || outputThruEnabled);
    bool hasInputConfig = (activeInput != InputSource::SystemTime);

    // Input collapse button: visible when input has config
    btnCollapseInput.setVisible(hasInputConfig);
    updateCollapseButtonText(btnCollapseInput, inputConfigExpanded);

    cmbMidiInputDevice.setVisible(showMidiIn);       lblMidiInputDevice.setVisible(showMidiIn);
    cmbArtnetInputInterface.setVisible(showArtnetIn); lblArtnetInputInterface.setVisible(showArtnetIn);

    cmbAudioInputTypeFilter.setVisible(showLtcIn);    lblAudioInputTypeFilter.setVisible(showLtcIn);
    cmbSampleRate.setVisible(showLtcIn);
    lblSampleRate.setVisible(showLtcIn);
    cmbBufferSize.setVisible(showLtcIn);
    lblBufferSize.setVisible(showLtcIn);
    cmbAudioInputDevice.setVisible(showLtcIn);        lblAudioInputDevice.setVisible(showLtcIn);
    cmbAudioInputChannel.setVisible(showLtcIn);       lblAudioInputChannel.setVisible(showLtcIn);
    sldLtcInputGain.setVisible(showLtcIn);            lblLtcInputGain.setVisible(showLtcIn);
    mtrLtcInput.setVisible(showLtcIn);
    cmbThruInputChannel.setVisible(showLtcIn && outputThruEnabled);  lblThruInputChannel.setVisible(showLtcIn && outputThruEnabled);
    sldThruInputGain.setVisible(showLtcIn && outputThruEnabled);     lblThruInputGain.setVisible(showLtcIn && outputThruEnabled);
    mtrThruInput.setVisible(showLtcIn && outputThruEnabled);
    lblInputStatus.setVisible(true);

    // Output sections: visible = enabled, config visible = enabled && expanded
    bool showMtcConfig    = outputMtcEnabled    && mtcOutExpanded;
    bool showArtnetConfig = outputArtnetEnabled && artnetOutExpanded;
    bool showLtcConfig    = outputLtcEnabled    && ltcOutExpanded;
    bool showThruConfig   = outputThruEnabled   && thruOutExpanded;

    // Collapse buttons visible when output is enabled
    btnCollapseMtcOut.setVisible(outputMtcEnabled);
    btnCollapseArtnetOut.setVisible(outputArtnetEnabled);
    btnCollapseLtcOut.setVisible(outputLtcEnabled);
    btnCollapseThruOut.setVisible(outputThruEnabled);
    updateCollapseButtonText(btnCollapseMtcOut, mtcOutExpanded);
    updateCollapseButtonText(btnCollapseArtnetOut, artnetOutExpanded);
    updateCollapseButtonText(btnCollapseLtcOut, ltcOutExpanded);
    updateCollapseButtonText(btnCollapseThruOut, thruOutExpanded);

    cmbMidiOutputDevice.setVisible(showMtcConfig);       lblMidiOutputDevice.setVisible(showMtcConfig);
    sldMtcOffset.setVisible(showMtcConfig);              lblMtcOffset.setVisible(showMtcConfig);
    lblOutputMtcStatus.setVisible(outputMtcEnabled);

    cmbArtnetOutputInterface.setVisible(showArtnetConfig); lblArtnetOutputInterface.setVisible(showArtnetConfig);
    sldArtnetOffset.setVisible(showArtnetConfig);          lblArtnetOffset.setVisible(showArtnetConfig);
    lblOutputArtnetStatus.setVisible(outputArtnetEnabled);

    cmbAudioOutputTypeFilter.setVisible(showAudioOut && (showLtcConfig || showThruConfig));
    lblAudioOutputTypeFilter.setVisible(showAudioOut && (showLtcConfig || showThruConfig));

    cmbAudioOutputDevice.setVisible(showLtcConfig);  lblAudioOutputDevice.setVisible(showLtcConfig);
    cmbAudioOutputChannel.setVisible(showLtcConfig); lblAudioOutputChannel.setVisible(showLtcConfig);
    sldLtcOutputGain.setVisible(showLtcConfig);      lblLtcOutputGain.setVisible(showLtcConfig);
    mtrLtcOutput.setVisible(showLtcConfig);
    sldLtcOffset.setVisible(showLtcConfig);            lblLtcOffset.setVisible(showLtcConfig);
    lblOutputLtcStatus.setVisible(outputLtcEnabled);

    cmbThruOutputDevice.setVisible(showThruConfig);  lblThruOutputDevice.setVisible(showThruConfig);
    cmbThruOutputChannel.setVisible(showThruConfig); lblThruOutputChannel.setVisible(showThruConfig);
    sldThruOutputGain.setVisible(showThruConfig);    lblThruOutputGain.setVisible(showThruConfig);
    mtrThruOutput.setVisible(showThruConfig);
    lblOutputThruStatus.setVisible(outputThruEnabled);

    bool anyDevice = (activeInput != InputSource::SystemTime) || outputMtcEnabled || outputArtnetEnabled || outputLtcEnabled || outputThruEnabled;
    btnRefreshDevices.setVisible(anyDevice);

    resized();
}

void MainComponent::updateStatusLabels()
{
    lblInputStatus.setText(inputStatusText, juce::dontSendNotification);
    lblInputStatus.setColour(juce::Label::textColourId, sourceActive ? getInputColour(activeInput) : textDim);

    if (outputMtcEnabled && mtcOutput.getIsRunning()) lblOutputMtcStatus.setText(mtcOutput.isPaused() ? "PAUSED" : mtcOutStatusText, juce::dontSendNotification);
    else lblOutputMtcStatus.setText(mtcOutStatusText, juce::dontSendNotification);

    if (outputArtnetEnabled && artnetOutput.getIsRunning()) lblOutputArtnetStatus.setText(artnetOutput.isPaused() ? "PAUSED" : artnetOutStatusText, juce::dontSendNotification);
    else lblOutputArtnetStatus.setText(artnetOutStatusText, juce::dontSendNotification);

    if (outputLtcEnabled)
    { if (ltcOutput.getIsRunning()) lblOutputLtcStatus.setText(ltcOutput.isPaused() ? "PAUSED" : ltcOutStatusText, juce::dontSendNotification); else lblOutputLtcStatus.setText(ltcOutStatusText, juce::dontSendNotification); }

    if (outputThruEnabled) lblOutputThruStatus.setText(thruOutStatusText, juce::dontSendNotification);
}

//==============================================================================
// PAINT
//==============================================================================
void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(bgDark);
    auto bounds = getLocalBounds();
    int panelWidth = 240;

    // Left panel background
    auto leftPanel = bounds.removeFromLeft(panelWidth);
    g.setColour(bgPanel); g.fillRect(leftPanel);
    g.setColour(borderCol); g.drawLine((float)leftPanel.getRight(), 0.0f, (float)leftPanel.getRight(), (float)getHeight(), 1.0f);
    g.setColour(textDim); g.setFont(juce::Font(juce::FontOptions("Consolas", 10.0f, juce::Font::bold)));
    g.drawText(">> SOURCE INPUT", leftPanel.withHeight(40).reduced(16, 0), juce::Justification::centredLeft);

    // Right panel background
    auto rightPanel = bounds.removeFromRight(panelWidth);
    g.setColour(bgPanel); g.fillRect(rightPanel);
    g.setColour(borderCol); g.drawLine((float)rightPanel.getX(), 0.0f, (float)rightPanel.getX(), (float)getHeight(), 1.0f);
    g.setColour(textDim); g.drawText(">> OUTPUTS", rightPanel.withHeight(40).reduced(16, 0), juce::Justification::centredLeft);

    // Top bar
    g.setColour(bgDarker); g.fillRect(0, 0, getWidth(), 32);
    g.setColour(borderCol); g.drawLine(0.0f, 32.0f, (float)getWidth(), 32.0f, 1.0f);
    g.setColour(textDim); g.setFont(juce::Font(juce::FontOptions("Consolas", 11.0f, juce::Font::bold)));
    g.drawText("SUPER TIMECODE CONVERTER", juce::Rectangle<int>(0, 0, getWidth(), 32), juce::Justification::centred);

    // Bottom bar
    int bbH = 24;
    g.setColour(bgDarker); g.fillRect(0, getHeight() - bbH, getWidth(), bbH);
    g.setColour(borderCol); g.drawLine(0.0f, (float)(getHeight() - bbH), (float)getWidth(), (float)(getHeight() - bbH), 1.0f);
    g.setColour(textDim); g.setFont(juce::Font(juce::FontOptions("Consolas", 9.0f, juce::Font::plain)));
    g.drawText("STC v1.1  |  Fiverecords " + juce::String(juce::CharPointer_UTF8("\xc2\xa9")) + " 2026", juce::Rectangle<int>(10, getHeight() - bbH, 280, bbH), juce::Justification::centredLeft);

    juce::String statusText; juce::Colour statusColour;
    if (sourceActive) { statusText = getInputName(activeInput) + " ACTIVE"; statusColour = getInputColour(activeInput); }
    else if (isInputStarted()) { statusText = getInputName(activeInput) + " PAUSED"; statusColour = juce::Colour(0xFFFFAB00); }
    else { statusText = getInputName(activeInput) + " STOPPED"; statusColour = textDim; }
    g.setColour(statusColour); g.drawText(statusText, juce::Rectangle<int>(getWidth() - 180, getHeight() - bbH, 170, bbH), juce::Justification::centredRight);

    // Frame rate label
    g.setColour(textDim); g.setFont(juce::Font(juce::FontOptions("Consolas", 10.0f, juce::Font::bold)));
    auto centerArea = getLocalBounds().reduced(panelWidth, 0);
    g.drawText("FRAME RATE", centerArea.getX(), getHeight() - bbH - 80, centerArea.getWidth(), 14, juce::Justification::centred);
}

//==============================================================================
// RESIZED Ã¢â‚¬â€ Interleaved layout
//==============================================================================
void MainComponent::resized()
{
    auto bounds = getLocalBounds();
    int panelWidth = 240, topBar = 32, bottomBar = 24;

    // ===== LEFT PANEL =====
    auto leftPanel = bounds.removeFromLeft(panelWidth);
    leftPanel.removeFromTop(topBar + 40);
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
    struct IBI { juce::TextButton* btn; InputSource src; };
    IBI iBtns[] = { {&btnMtcIn,InputSource::MTC}, {&btnArtnetIn,InputSource::ArtNet}, {&btnSysTime,InputSource::SystemTime}, {&btnLtcIn,InputSource::LTC} };
    for (auto& ib : iBtns) { ib.btn->setBounds(leftPanel.removeFromTop(btnH)); leftPanel.removeFromTop(btnG); }

    // Collapse button for input config
    if (btnCollapseInput.isVisible())
    {
        leftPanel.removeFromTop(2);
        btnCollapseInput.setBounds(leftPanel.removeFromTop(18));
        leftPanel.removeFromTop(4);
    }

    // Input config (if expanded)
    if (cmbMidiInputDevice.isVisible()) layCombo(lblMidiInputDevice, cmbMidiInputDevice, leftPanel);
    if (cmbArtnetInputInterface.isVisible()) layCombo(lblArtnetInputInterface, cmbArtnetInputInterface, leftPanel);
    if (cmbAudioInputDevice.isVisible())
    {
        layCombo(lblAudioInputTypeFilter, cmbAudioInputTypeFilter, leftPanel);
        if (cmbSampleRate.isVisible())
        {
            lblSampleRate.setBounds(leftPanel.removeFromTop(14));
            leftPanel.removeFromTop(2);
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

    // ===== RIGHT PANEL Ã¢â‚¬â€ Interleaved: toggle Ã¢â€ â€™ config Ã¢â€ â€™ toggle Ã¢â€ â€™ config =====
    auto rightPanel = bounds.removeFromRight(panelWidth);
    rightPanel.removeFromTop(topBar + 40);
    rightPanel.removeFromBottom(bottomBar);
    rightPanel = rightPanel.reduced(12, 0);

    int colBtnW = 26;  // collapse button width

    // --- MTC OUT section ---
    {
        auto row = rightPanel.removeFromTop(btnH);
        if (btnCollapseMtcOut.isVisible())
        { btnCollapseMtcOut.setBounds(row.removeFromRight(colBtnW)); row.removeFromRight(3); }
        btnMtcOut.setBounds(row);
        rightPanel.removeFromTop(2);

        if (cmbMidiOutputDevice.isVisible()) layCombo(lblMidiOutputDevice, cmbMidiOutputDevice, rightPanel);
        if (sldMtcOffset.isVisible()) laySlider(lblMtcOffset, sldMtcOffset, rightPanel);
        if (lblOutputMtcStatus.isVisible()) layStatus(lblOutputMtcStatus, rightPanel);
        rightPanel.removeFromTop(2);
    }

    // --- ART-NET OUT section ---
    {
        auto row = rightPanel.removeFromTop(btnH);
        if (btnCollapseArtnetOut.isVisible())
        { btnCollapseArtnetOut.setBounds(row.removeFromRight(colBtnW)); row.removeFromRight(3); }
        btnArtnetOut.setBounds(row);
        rightPanel.removeFromTop(2);

        if (cmbArtnetOutputInterface.isVisible()) layCombo(lblArtnetOutputInterface, cmbArtnetOutputInterface, rightPanel);
        if (sldArtnetOffset.isVisible()) laySlider(lblArtnetOffset, sldArtnetOffset, rightPanel);
        if (lblOutputArtnetStatus.isVisible()) layStatus(lblOutputArtnetStatus, rightPanel);
        rightPanel.removeFromTop(2);
    }

    // --- LTC OUT section ---
    {
        auto row = rightPanel.removeFromTop(btnH);
        if (btnCollapseLtcOut.isVisible())
        { btnCollapseLtcOut.setBounds(row.removeFromRight(colBtnW)); row.removeFromRight(3); }
        btnLtcOut.setBounds(row);
        rightPanel.removeFromTop(2);

        // Shared audio driver filter Ã¢â‚¬â€ shown inside LTC/Thru config area
        if (cmbAudioOutputTypeFilter.isVisible())
            layCombo(lblAudioOutputTypeFilter, cmbAudioOutputTypeFilter, rightPanel);

        if (cmbAudioOutputDevice.isVisible())
        {
            layCombo(lblAudioOutputDevice, cmbAudioOutputDevice, rightPanel);
            layCombo(lblAudioOutputChannel, cmbAudioOutputChannel, rightPanel);
            laySlider(lblLtcOutputGain, sldLtcOutputGain, rightPanel);
            if (mtrLtcOutput.isVisible()) layMeter(mtrLtcOutput, rightPanel);
            if (sldLtcOffset.isVisible()) laySlider(lblLtcOffset, sldLtcOffset, rightPanel);
        }
        if (lblOutputLtcStatus.isVisible()) layStatus(lblOutputLtcStatus, rightPanel);
        rightPanel.removeFromTop(2);
    }

    // --- AUDIO THRU section ---
    {
        auto row = rightPanel.removeFromTop(btnH);
        if (btnCollapseThruOut.isVisible())
        { btnCollapseThruOut.setBounds(row.removeFromRight(colBtnW)); row.removeFromRight(3); }
        btnThruOut.setBounds(row);
        rightPanel.removeFromTop(2);

        if (cmbThruOutputDevice.isVisible())
        {
            layCombo(lblThruOutputDevice, cmbThruOutputDevice, rightPanel);
            layCombo(lblThruOutputChannel, cmbThruOutputChannel, rightPanel);
            laySlider(lblThruOutputGain, sldThruOutputGain, rightPanel);
            if (mtrThruOutput.isVisible()) layMeter(mtrThruOutput, rightPanel);
        }
        if (lblOutputThruStatus.isVisible()) layStatus(lblOutputThruStatus, rightPanel);
        rightPanel.removeFromTop(2);
    }

    // Refresh button at bottom
    if (btnRefreshDevices.isVisible())
        btnRefreshDevices.setBounds(rightPanel.removeFromBottom(26));

    // ===== CENTER Ã¢â‚¬â€ Timecode + FPS =====
    auto centerArea = bounds;
    centerArea.removeFromTop(topBar);
    centerArea.removeFromBottom(bottomBar);

    auto fpsArea = centerArea.removeFromBottom(90);
    fpsArea.removeFromTop(20);
    int fpsBW = 65, totalW = fpsBW * 4 + 8 * 3;
    int fpsX = fpsArea.getCentreX() - totalW / 2, fpsY = fpsArea.getY() + 10;
    juce::TextButton* fB[] = {&btnFps24,&btnFps25,&btnFps2997,&btnFps30};
    FrameRate fV[] = {FrameRate::FPS_24,FrameRate::FPS_25,FrameRate::FPS_2997,FrameRate::FPS_30};
    for (int i = 0; i < 4; i++) { fB[i]->setBounds(fpsX + i * (fpsBW + 8), fpsY, fpsBW, 32); styleFpsButton(*fB[i], currentFps == fV[i]); }

    timecodeDisplay.setBounds(centerArea);

    // GitHub link in bottom bar
    int bbH = 24;
    btnGitHub.setBounds(290, getHeight() - bbH, 320, bbH);
}

//==============================================================================
// TIMER
//==============================================================================
void MainComponent::timerCallback()
{
    switch (activeInput)
    {
        case InputSource::SystemTime:
            updateSystemTime(); sourceActive = true; inputStatusText = "SYSTEM CLOCK"; break;

        case InputSource::MTC:
            if (mtcInput.getIsRunning()) {
                currentTimecode = mtcInput.getCurrentTimecode(); bool rx = mtcInput.isReceiving();
                if (rx) { auto d = mtcInput.getDetectedFrameRate(); if (d != currentFps) setFrameRate(d); inputStatusText = "RX: " + mtcInput.getCurrentDeviceName(); }
                else inputStatusText = "PAUSED - " + mtcInput.getCurrentDeviceName();
                sourceActive = rx;
            } else { sourceActive = false; inputStatusText = "WAITING FOR DEVICE..."; } break;

        case InputSource::ArtNet:
            if (artnetInput.getIsRunning()) {
                currentTimecode = artnetInput.getCurrentTimecode(); bool rx = artnetInput.isReceiving();
                if (rx) { auto d = artnetInput.getDetectedFrameRate(); if (d != currentFps) setFrameRate(d); inputStatusText = "RX ON " + artnetInput.getBindInfo(); }
                else inputStatusText = "PAUSED - " + artnetInput.getBindInfo();
                sourceActive = rx;
            } else { sourceActive = false; inputStatusText = "NOT LISTENING"; } break;

        case InputSource::LTC:
            if (ltcInput.getIsRunning()) {
                currentTimecode = ltcInput.getCurrentTimecode(); bool rx = ltcInput.isReceiving();
                if (rx) { auto d = ltcInput.getDetectedFrameRate(); if (d != currentFps) setFrameRate(d); inputStatusText = "RX: " + ltcInput.getCurrentDeviceName() + " Ch " + juce::String(ltcInput.getSelectedChannel() + 1); }
                else inputStatusText = "PAUSED - " + ltcInput.getCurrentDeviceName();
                sourceActive = rx;
            } else { sourceActive = false; inputStatusText = "WAITING FOR DEVICE..."; } break;
    }

    routeTimecodeToOutputs(); updateStatusLabels();
    timecodeDisplay.setTimecode(currentTimecode); timecodeDisplay.setFrameRate(currentFps);
    timecodeDisplay.setRunning(sourceActive); timecodeDisplay.setSourceName(getInputName(activeInput));

    // Update level meters (with decay for smooth visual)
    auto decayLevel = [](float current, float target, float decay = 0.85f) {
        return target > current ? target : current * decay;
    };
    float ltcInLvl  = ltcInput.getIsRunning()  ? ltcInput.getLtcPeakLevel()  : 0.0f;
    float thruInLvl = ltcInput.getIsRunning()  ? ltcInput.getThruPeakLevel() : 0.0f;
    float ltcOutLvl = ltcOutput.getIsRunning() ? ltcOutput.getPeakLevel()    : 0.0f;
    float thruOutLvl = audioThru.getIsRunning() ? audioThru.getPeakLevel()   : 0.0f;

    static float sLtcIn = 0, sThruIn = 0, sLtcOut = 0, sThruOut = 0;
    sLtcIn  = decayLevel(sLtcIn,  ltcInLvl);
    sThruIn = decayLevel(sThruIn, thruInLvl);
    sLtcOut = decayLevel(sLtcOut, ltcOutLvl);
    sThruOut = decayLevel(sThruOut, thruOutLvl);

    mtrLtcInput.setLevel(sLtcIn);
    mtrThruInput.setLevel(sThruIn);
    mtrLtcOutput.setLevel(sLtcOut);
    mtrThruOutput.setLevel(sThruOut);

    repaint();
}

void MainComponent::updateSystemTime()
{
    auto now = juce::Time::getCurrentTime();
    currentTimecode.hours = now.getHours(); currentTimecode.minutes = now.getMinutes();
    currentTimecode.seconds = now.getSeconds();
    currentTimecode.frames = (int)(now.getMilliseconds() / 1000.0 * frameRateToDouble(currentFps)) % frameRateToInt(currentFps);
}

void MainComponent::setInputSource(InputSource source)
{
    switch (activeInput) { case InputSource::MTC: stopMtcInput(); break; case InputSource::ArtNet: stopArtnetInput(); break; case InputSource::LTC: stopLtcInput(); break; default: break; }
    activeInput = source; sourceActive = false;
    switch (source)
    {
        case InputSource::MTC:        startMtcInput();     break;
        case InputSource::ArtNet:     startArtnetInput();  break;
        case InputSource::SystemTime: sourceActive = true; break;
        case InputSource::LTC:
            if (!scannedAudioInputs.isEmpty()) startLtcInput();
            break;
    }
    updateInputButtonStates(); updateDeviceSelectorVisibility(); repaint();
}

void MainComponent::setFrameRate(FrameRate fps)
{
    currentFps = fps; mtcOutput.setFrameRate(fps); artnetOutput.setFrameRate(fps); ltcOutput.setFrameRate(fps);
    updateFpsButtonStates(); repaint();
}

//==============================================================================
// BUTTON STATES
//==============================================================================
void MainComponent::updateInputButtonStates()
{
    struct I { juce::TextButton* b; InputSource s; };
    I bs[] = { {&btnMtcIn,InputSource::MTC}, {&btnArtnetIn,InputSource::ArtNet}, {&btnSysTime,InputSource::SystemTime}, {&btnLtcIn,InputSource::LTC} };
    for (auto& i : bs) styleInputButton(*i.b, activeInput == i.s, getInputColour(i.s));
}

void MainComponent::updateFpsButtonStates()
{
    FrameRate v[] = {FrameRate::FPS_24,FrameRate::FPS_25,FrameRate::FPS_2997,FrameRate::FPS_30};
    juce::TextButton* b[] = {&btnFps24,&btnFps25,&btnFps2997,&btnFps30};
    for (int i = 0; i < 4; i++) styleFpsButton(*b[i], currentFps == v[i]);
}

juce::Colour MainComponent::getInputColour(InputSource s) const
{ switch (s) { case InputSource::MTC: return accentRed; case InputSource::ArtNet: return accentOrange; case InputSource::SystemTime: return accentGreen; case InputSource::LTC: return accentPurple; default: return textMid; } }

juce::String MainComponent::getInputName(InputSource s) const
{ switch (s) { case InputSource::MTC: return "MTC"; case InputSource::ArtNet: return "ART-NET"; case InputSource::SystemTime: return "SYSTEM"; case InputSource::LTC: return "LTC"; default: return "---"; } }

bool MainComponent::isInputStarted() const
{ switch (activeInput) { case InputSource::SystemTime: return true; case InputSource::MTC: return mtcInput.getIsRunning(); case InputSource::ArtNet: return artnetInput.getIsRunning(); case InputSource::LTC: return ltcInput.getIsRunning(); default: return false; } }

//==============================================================================
// STYLING
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
    auto c = getInputColour(activeInput);
    btn.setColour(juce::TextButton::buttonColourId, active ? c.withAlpha(0.15f) : juce::Colour(0xFF1A1D23));
    btn.setColour(juce::TextButton::textColourOffId, active ? juce::Colour(0xFFE0F7FA) : textMid);
}

void MainComponent::styleOutputToggle(juce::ToggleButton& btn, juce::Colour colour)
{ btn.setColour(juce::ToggleButton::textColourId, textBright); btn.setColour(juce::ToggleButton::tickColourId, colour); btn.setColour(juce::ToggleButton::tickDisabledColourId, textDim); }

void MainComponent::styleComboBox(juce::ComboBox& cmb)
{ cmb.setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xFF1A1D23)); cmb.setColour(juce::ComboBox::textColourId, textBright); cmb.setColour(juce::ComboBox::outlineColourId, borderCol); cmb.setColour(juce::ComboBox::arrowColourId, textMid); }

void MainComponent::styleLabel(juce::Label& lbl, float fs)
{ lbl.setFont(juce::Font(juce::FontOptions("Consolas", fs, juce::Font::plain))); lbl.setColour(juce::Label::textColourId, textDim); lbl.setJustificationType(juce::Justification::centredLeft); }

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
    btn.setButtonText(expanded ? juce::String::charToString(0x25BE)   // Ã¢â€“Â¾
                               : juce::String::charToString(0x25B8)); // Ã¢â€“Â¸
}
