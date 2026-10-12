// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

#pragma once
#include <JuceHeader.h>
#include "GeneratorWaveformView.h"
#include "GeneratorAudioPlayer.h"
#include "TimecodeEngine.h"
#include "TimecodeCore.h"

//==============================================================================
// GeneratorWaveformWindow -- Content component for the floating waveform
// window.  Combines:
//   - Header: filename + file-time / total-time, and the loop in charge
//     (#24, loopInCharge): LOOP A/B, LOOP TC or LOOP AUDIO
//   - Big absolute TC display (HH:MM:SS:FF), and on its right the preset
//     GO starts (#24, PRESET: the main window's preset list selection)
//   - GeneratorWaveformView in Full mode (markers + click-to-seek)
//   - Transport buttons (GO / PLAY / PAUSE / STOP)
//
// The actual juce::DocumentWindow is created and owned by MainComponent
// (consistent with the other floating windows in STC).  This Component is
// what setContentOwned() wraps.
//==============================================================================
class GeneratorWaveformWindow : public juce::Component,
                                private juce::Timer
{
public:
    GeneratorWaveformWindow()
    {
        // --- Header ---
        addAndMakeVisible(lblFilename);
        lblFilename.setFont(juce::Font(juce::FontOptions(13.0f).withStyle("Bold")));
        lblFilename.setColour(juce::Label::textColourId, textBright);
        lblFilename.setJustificationType(juce::Justification::centredLeft);

        addAndMakeVisible(lblFileTime);
        lblFileTime.setFont(juce::Font(juce::FontOptions(13.0f)));
        lblFileTime.setColour(juce::Label::textColourId, textMid);
        lblFileTime.setJustificationType(juce::Justification::centredRight);

        // Which loop is in charge (#24, loopInCharge), one text at a time:
        // LOOP A/B, LOOP TC or LOOP AUDIO, each with a tooltip that says
        // what it means (updateLoopState).  This window's own loop controls
        // (A/B LOOP, [ ], L) are the A/B loop's; the other two are the
        // preset's.
        addChildComponent(lblLoopState);
        lblLoopState.setFont(juce::Font(juce::FontOptions(13.0f).withStyle("Bold")));
        lblLoopState.setJustificationType(juce::Justification::centredRight);

        // --- Big absolute TC ---
        addAndMakeVisible(lblAbsoluteTC);
        lblAbsoluteTC.setFont(juce::Font(juce::FontOptions(28.0f).withStyle("Bold")));
        lblAbsoluteTC.setColour(juce::Label::textColourId, accentAmber);
        lblAbsoluteTC.setJustificationType(juce::Justification::centredLeft);
        lblAbsoluteTC.setText("--:--:--:--", juce::dontSendNotification);

        // The preset GO starts (#24): the one selected in the main window's
        // preset list, which this window does not show otherwise -- two
        // presets with the same file look alike here, and the list is
        // hidden while the main window's input section is collapsed
        // (updatePresetForGo).
        addAndMakeVisible(lblPreset);
        lblPreset.setFont(juce::Font(juce::FontOptions(13.0f)));
        lblPreset.setColour(juce::Label::textColourId, textMid);
        lblPreset.setJustificationType(juce::Justification::centredRight);
        lblPreset.setText(kNoPresetText, juce::dontSendNotification);
        lblPreset.setTooltip("The preset selected in the main window's preset list: the one GO starts.");

        // --- Waveform ---
        addAndMakeVisible(waveform);
        waveform.setMode(GeneratorWaveformView::Mode::Full);

        // --- Transport ---
        for (auto* btn : { &btnGo, &btnPlay, &btnPause, &btnStop, &btnPrev, &btnNext, &btnEdit })
        {
            addAndMakeVisible(btn);
            btn->setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF444444));
            btn->setColour(juce::TextButton::textColourOffId, textBright);
        }
        // GO as the panel's, in its green (#24): MainComponent's onGo is the
        // panel's GO for this window's engine, the selected one, and it is
        // greyed out with another input or in Clock mode (goAvailable,
        // timerCallback).
        btnGo.setButtonText("GO");
        btnGo.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF22AA44));
        btnGo.setTooltip("GO: start the preset selected in the main window's preset list (PRESET, "
                         "above on the right), with its audio file and loop options, as the panel's "
                         "GO does: from its Start TC, or, with its file already loaded, from the In "
                         "of an A/B loop it follows.  Greyed out while no preset is selected, and, as "
                         "the panel's GO is hidden, while the input is not the generator or the "
                         "generator is in Clock mode.");
        btnPlay.setButtonText("PLAY");
        btnPause.setButtonText("PAUSE");
        btnStop.setButtonText("STOP");
        btnPrev.setButtonText("<");
        btnNext.setButtonText(">");
        btnEdit.setButtonText("EDIT");

        btnGo.onClick    = [this] { if (onGo) onGo(); };
        btnPlay.onClick  = [this] { if (engine) engine->generatorPlay();  };
        btnPause.onClick = [this] { if (engine) engine->generatorPause(); };
        btnStop.onClick  = [this] { if (engine) engine->generatorStop();  };
        btnPrev.onClick  = [this] { if (onPrev) onPrev(); };
        btnNext.onClick  = [this] { if (onNext) onNext(); };
        btnEdit.onClick  = [this] { if (onEdit) onEdit(); };

        //----------------------------------------------------------------------
        // A/B loop transport (programming aid).  Lives here next to the
        // waveform so the operator sets In / Out / toggle in the same visual
        // context where the loop markers are drawn.  Standard DAW / Pioneer
        // CDJ semantics: PLAY from STOP starts at In when armed; tick wraps
        // Out -> In; a seek moves the playhead (from STOP to PAUSE) and PLAY
        // resumes there.  The preset's timecode loop ignores an A/B not
        // inside it (#24, TimecodeEngine::isGeneratorAbLoopFollowed).
        //----------------------------------------------------------------------
        for (auto* btn : { &btnLoopIn, &btnLoopOut })
        {
            addAndMakeVisible(btn);
            btn->setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF444444));
            btn->setColour(juce::TextButton::textColourOffId, textBright);
        }
        btnLoopIn .setButtonText("[");
        btnLoopOut.setButtonText("]");
        // The keys work in this window too: the frame MainComponent puts
        // this content in hands [ ] L to the main window's handler
        // (MainComponent::handleGeneratorLoopKey).
        btnLoopIn .setTooltip("A/B loop: set In at the playhead (keyboard: [, in this window or the main window).  "
                              "At or past Out it is refused: the button flashes red.");
        btnLoopOut.setTooltip("A/B loop: set Out at the playhead (keyboard: ], in this window or the main window).  "
                              "At or before In it is refused: the button flashes red.");

        // "A/B LOOP", not "LOOP": the preset's Loop timecode is the other
        // loop (#24).
        addAndMakeVisible(btnLoopOn);
        btnLoopOn.setButtonText("A/B LOOP");
        btnLoopOn.setColour(juce::ToggleButton::textColourId, textBright);
        btnLoopOn.setTooltip("A/B loop on / off (keyboard: L, in this window or the main window).  When on, "
                             "playback goes back from Out to In, and PLAY from STOP starts at "
                             "In.  A rehearsal loop, separate from the preset's Loop timecode "
                             "(LOOP TC), which ignores an A/B not inside its own loop.  Without "
                             "that loop, an A/B past the end of a file that does not loop is "
                             "ignored too.  An ignored A/B is drawn dashed, at the right edge when "
                             "it lies past the end of the file (one set in a later pass of Loop "
                             "audio only, which runs only under Loop audio).");

        // An In at or past Out, or an Out at or before In, is refused as
        // the main window's keys refuse it (MainComponent::
        // handleGeneratorLoopKey), with the button flashing red for 0.4 s
        // (flashRefused).  Until #24 it opened a modal alert, and until its
        // OK the other windows took no click, STOP and PLAY included
        // (Joaky's field test: a ] pressed just after the timecode loop's
        // jump back, before the In).
        btnLoopIn.onClick = [this] {
            if (engine == nullptr) return;
            // Loop In is a timestamp-setting gesture, gated by Show Lock to
            // match Stop TC editing and the waveform click-to-seek.  Silent
            // refusal -- the locked UI already telegraphs unavailability.
            if (isLockedFn && isLockedFn()) return;
            const double here = engine->getGeneratorCurrentMs();
            const double existingOut = engine->getGeneratorLoopOutMs();
            if (existingOut > 0.0 && here >= existingOut)
            {
                flashRefused(btnLoopIn, loopInFlashTicks);
                return;
            }
            engine->setGeneratorLoopInMs(here);
            waveform.repaint();
            if (onLoopChanged) onLoopChanged();
        };
        btnLoopOut.onClick = [this] {
            if (engine == nullptr) return;
            // Symmetric with btnLoopIn: Loop Out is a timestamp-setting
            // gesture, gated by Show Lock for consistency.
            if (isLockedFn && isLockedFn()) return;
            const double here = engine->getGeneratorCurrentMs();
            const double existingIn = engine->getGeneratorLoopInMs();
            if (here <= existingIn)
            {
                flashRefused(btnLoopOut, loopOutFlashTicks);
                return;
            }
            engine->setGeneratorLoopOutMs(here);
            waveform.repaint();
            if (onLoopChanged) onLoopChanged();
        };
        btnLoopOn.onClick = [this] {
            if (engine == nullptr) return;
            engine->setGeneratorLoopEnabled(btnLoopOn.getToggleState());
            waveform.repaint();
            if (onLoopChanged) onLoopChanged();
        };

        // Volume slider: linear 0..1.5 (1=unity).  Lives next to the
        // transport buttons so it's always at hand.
        addAndMakeVisible(lblVolume);
        lblVolume.setText("VOL", juce::dontSendNotification);
        lblVolume.setFont(juce::Font(juce::FontOptions(10.0f)));
        lblVolume.setColour(juce::Label::textColourId, textMid);
        lblVolume.setJustificationType(juce::Justification::centredRight);

        addAndMakeVisible(sldVolume);
        sldVolume.setSliderStyle(juce::Slider::LinearHorizontal);
        sldVolume.setRange(0.0, 1.5, 0.0);
        sldVolume.setValue(1.0, juce::dontSendNotification);
        sldVolume.setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 18);
        sldVolume.setNumDecimalPlacesToDisplay(2);
        sldVolume.onValueChange = [this] {
            if (engine && ! ignoreVolume) engine->setGeneratorAudioVolume((float) sldVolume.getValue());
            if (onVolumeChanged && ! ignoreVolume) onVolumeChanged((float) sldVolume.getValue());
        };

        startTimerHz(30);
        setSize(900, 280);
    }

    ~GeneratorWaveformWindow() override
    {
        stopTimer();
    }

    /// True when `e`'s generator runs the preset's timecode loop (#24); see
    /// GeneratorWaveformView::loopTimecodeShown, which the waveform's end
    /// mark uses.  Message thread.
    static bool loopTimecodeShown(const TimecodeEngine& e)
    {
        return GeneratorWaveformView::loopTimecodeShown(e);
    }

    /// Whether GO is available for `e` (#24): with the generator as the
    /// input, in transport mode -- where the main window shows the panel's
    /// GO while its input section is expanded
    /// (MainComponent::updateDeviceSelectorVisibility).  GO leaves Clock
    /// mode, a switch the panel's CLOCK toggle refuses under Show Lock.
    /// The window's GO is greyed out otherwise, and MainComponent::
    /// goPanelGenPreset refuses it.  With the input section collapsed the
    /// panel also hides its GO and the preset list, and the window's GO
    /// still starts the preset selected there.  Message thread.
    static bool goAvailable(const TimecodeEngine& e)
    {
        return e.getActiveInput() == TimecodeEngine::InputSource::SystemTime && ! e.getGeneratorClockMode();
    }

    /// The loop in charge of `e`'s generator (#24): what this window's
    /// header and the panel's STOP TC label show; see
    /// GeneratorWaveformView::loopInCharge for the order.  Message thread.
    using LoopInCharge = GeneratorWaveformView::LoopInCharge;
    static LoopInCharge loopInCharge(const TimecodeEngine& e)
    {
        return GeneratorWaveformView::loopInCharge(e);
    }

    //==========================================================================
    void setAudioPlayer(GeneratorAudioPlayer* p)
    {
        player = p;
        waveform.setAudioPlayer(p);
        repaint();
    }

    void setEngine(TimecodeEngine* e)
    {
        engine = e;
        waveform.setEngine(e);
        if (engine != nullptr)
            setVolumeFromOutside(engine->getGeneratorAudioVolume());
        repaint();
    }

    /// Forward show-lock predicate to the waveform's seek handler.
    /// We also keep a copy at the window level so the loop In/Out buttons
    /// can refuse timestamp-setting gestures during Show Lock (mirrors the
    /// behaviour of the waveform's click-to-seek and the panel's Stop TC
    /// editor; the loop ENABLE toggle stays ungated because toggling an
    /// already-armed loop is an operational gesture, not config).
    void setLockedFn(std::function<bool()> fn)
    {
        isLockedFn = fn;
        waveform.isLockedFn = std::move(fn);
    }

    //==========================================================================
    void paint(juce::Graphics& g) override
    {
        g.fillAll(bgWindow);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(10);

        // Header row: filename left, file-time right, the loop in charge
        // before it.  110 px: "LOOP AUDIO" is 73 px in this 13-px bold font
        // in Liberation Sans (Arial's widths) and 78 px in DejaVu Sans (close
        // to Verdana, which LookAndFeel_V4 uses on Windows), within the
        // label's 5-px borders.
        auto headerRow = area.removeFromTop(20);
        lblFileTime.setBounds(headerRow.removeFromRight(180));
        lblLoopState.setBounds(headerRow.removeFromRight(110));
        lblFilename.setBounds(headerRow);
        area.removeFromTop(2);

        // Big absolute TC, the preset GO starts on its right (#24): at the
        // window's 640-px minimum the TC keeps 360 px.
        auto tcRow = area.removeFromTop(34);
        lblPreset.setBounds(tcRow.removeFromRight(juce::jmin(260, tcRow.getWidth() / 2)));
        lblAbsoluteTC.setBounds(tcRow);
        area.removeFromTop(8);

        // Transport row at bottom.
        // Layout: [PREV] [GO] [PLAY] [PAUSE] [STOP] [NEXT] | VOL [slider]  ...gap... [EDIT]
        // GO (#24) is paid for by PLAY / PAUSE / STOP, 76 px instead of 90,
        // so the row still fits the window's 640-px minimum
        // (MainComponent::openGeneratorWaveformWindow).  JUCE's
        // LookAndFeel_V4 draws a button's text 16 px high within 9-px
        // indents on these 28-px buttons: PAUSE, the widest, is 47.5 px in
        // Liberation Sans (Arial's widths) and 44.3 in DejaVu Sans, GO 22.3
        // and 21.5, so 58 and 26 px of text room are enough, and a font
        // wider than those is squeezed, not cut.
        auto transportRow = area.removeFromBottom(28);
        const int btnW = 76, gap = 4, navW = 40, goW = 44, editW = 60;
        const int volLabelW = 28, volSliderMinW = 140;

        btnPrev .setBounds(transportRow.removeFromLeft(navW)); transportRow.removeFromLeft(gap);
        btnGo   .setBounds(transportRow.removeFromLeft(goW));  transportRow.removeFromLeft(gap);
        btnPlay .setBounds(transportRow.removeFromLeft(btnW)); transportRow.removeFromLeft(gap);
        btnPause.setBounds(transportRow.removeFromLeft(btnW)); transportRow.removeFromLeft(gap);
        btnStop .setBounds(transportRow.removeFromLeft(btnW)); transportRow.removeFromLeft(gap);
        btnNext .setBounds(transportRow.removeFromLeft(navW));

        // Loop group, immediately after NEXT with a small visual break.
        // The toggle leaves "A/B LOOP" a 72 px text area: this window draws
        // with LookAndFeel_V4 (15 px text after a 28 px tick box and gaps),
        // with room for a font wider than Arial (Windows draws it in Verdana).
        transportRow.removeFromLeft(gap * 3);
        const int loopBtnW = 28, loopTogW = 100;
        btnLoopIn .setBounds(transportRow.removeFromLeft(loopBtnW)); transportRow.removeFromLeft(gap);
        btnLoopOut.setBounds(transportRow.removeFromLeft(loopBtnW)); transportRow.removeFromLeft(gap);
        btnLoopOn .setBounds(transportRow.removeFromLeft(loopTogW));

        // EDIT pinned to the right edge so it cannot be confused with transport.
        btnEdit .setBounds(transportRow.removeFromRight(editW));
        transportRow.removeFromRight(gap);

        // Volume label + slider take the remaining centre area.
        if (transportRow.getWidth() > volLabelW + volSliderMinW)
        {
            transportRow.removeFromLeft(gap * 2);   // visual breathing room from NEXT
            lblVolume.setBounds(transportRow.removeFromLeft(volLabelW));
            sldVolume.setBounds(transportRow);
        }
        else
        {
            // Window too narrow: hide volume to keep transport readable.
            lblVolume.setBounds({});
            sldVolume.setBounds({});
        }

        area.removeFromBottom(8);

        // Waveform fills the rest.
        waveform.setBounds(area);
    }

private:
    //==========================================================================
    void timerCallback() override
    {
        updateLabels();
        endFlash(btnLoopIn, loopInFlashTicks);
        endFlash(btnLoopOut, loopOutFlashTicks);
        // Keep the loop toggle visually consistent with engine state.  The
        // user can change the loop via keyboard shortcut even when this
        // window does not have focus, so polling 30 Hz here is the simplest
        // way to keep the button reflecting reality without wiring change
        // listeners into the engine.
        if (engine != nullptr)
        {
            const bool wanted = engine->getGeneratorLoopEnabled();
            if (btnLoopOn.getToggleState() != wanted)
                btnLoopOn.setToggleState(wanted, juce::dontSendNotification);
        }
        // GO with the generator as the input, in transport mode (#24,
        // goAvailable), and a preset selected to start.
        const bool presetSelected = updatePresetForGo();
        const bool go = engine != nullptr && goAvailable(*engine) && presetSelected;
        if (btnGo.isEnabled() != go)
            btnGo.setEnabled(go);
        updateLoopState(engine != nullptr ? loopInCharge(*engine) : LoopInCharge::none);
    }

    /// The PRESET label: the preset selected in the main window's list
    /// (selectedPresetName), which GO starts, changed only when it changes.
    /// Returns whether one is selected.
    bool updatePresetForGo()
    {
        const juce::String name = selectedPresetName ? selectedPresetName() : juce::String();
        const juce::String text = name.isNotEmpty() ? "PRESET: " + name : juce::String(kNoPresetText);
        if (lblPreset.getText() != text)
            lblPreset.setText(text, juce::dontSendNotification);
        return name.isNotEmpty();
    }

    /// The header's loop label for `state`, changed only when it changes.
    /// Colours: the A/B markers' amber, the cyan of the timecode loop's end
    /// mark, the waveform's amber.
    void updateLoopState(LoopInCharge state)
    {
        if (state == shownLoopState)
            return;
        shownLoopState = state;
        switch (state)
        {
            case LoopInCharge::abLoop:
                lblLoopState.setText("LOOP A/B", juce::dontSendNotification);
                lblLoopState.setColour(juce::Label::textColourId, abMarkerAmber);
                lblLoopState.setTooltip("The A/B loop is what runs: at Out the timecode and the audio go "
                                        "back to In.  It takes over from the preset's Loop timecode when "
                                        "it lies inside that loop, and from Loop audio.  It stays armed "
                                        "through GO on the same audio file (another file, or none where the "
                                        "engine had one, clears it); turn A/B LOOP off (or press L) to hand "
                                        "back to the preset.");
                break;
            case LoopInCharge::timecodeLoop:
                lblLoopState.setText("LOOP TC", juce::dontSendNotification);
                lblLoopState.setColour(juce::Label::textColourId, accentCyan);
                lblLoopState.setTooltip("Loop timecode is on for this preset (set in Generator Presets, "
                                        "or with the OSC command looptc): at its end the timecode and "
                                        "the audio go back to Start TC.  With an audio file, the cyan "
                                        "line on the waveform marks that end.  A/B LOOP is a separate "
                                        "rehearsal loop: armed inside this loop it takes over, and this "
                                        "reads LOOP A/B.");
                break;
            case LoopInCharge::audioLoop:
                lblLoopState.setText("LOOP AUDIO", juce::dontSendNotification);
                lblLoopState.setColour(juce::Label::textColourId, accentAmber);
                lblLoopState.setTooltip("Loop audio only is on for this preset (set in Generator Presets): "
                                        "at the end of the file the audio starts again from the top while "
                                        "the timecode keeps counting, up to a Stop TC or for ever; the cue "
                                        "points fire on the first pass only.");
                break;
            case LoopInCharge::none:
                break;
        }
        lblLoopState.setVisible(state != LoopInCharge::none);
    }

    void updateLabels()
    {
        if (player == nullptr || engine == nullptr)
        {
            lblFilename.setText("(no engine)", juce::dontSendNotification);
            lblFileTime.setText({}, juce::dontSendNotification);
            lblAbsoluteTC.setText("--:--:--:--", juce::dontSendNotification);
            return;
        }

        // Filename
        const auto file = player->getCurrentFile();
        if (file == juce::File() || ! player->hasFileLoaded())
            lblFilename.setText("(no audio file)", juce::dontSendNotification);
        else
            lblFilename.setText(file.getFileName(), juce::dontSendNotification);

        // File time / total time: where the cursor is in the file, folded by
        // the file's length while the player loops it, as the waveform's
        // cursor is (GeneratorWaveformView::cursorFileSeconds, #24).
        const double curMs      = engine->getGeneratorCurrentMs();
        const double totalSec   = player->getFileLengthSeconds();

        if (totalSec > 0.0)
        {
            const double curFileSec = GeneratorWaveformView::cursorFileSeconds(*engine, totalSec,
                                                                               player->isLooping());
            const double clamped = juce::jlimit(0.0, totalSec, curFileSec);
            lblFileTime.setText(formatTime(clamped) + " / " + formatTime(totalSec),
                                 juce::dontSendNotification);
        }
        else
        {
            lblFileTime.setText("--:-- / --:--", juce::dontSendNotification);
        }

        // Absolute TC
        const auto fps = engine->getCurrentFps();
        const auto tc  = wallClockToTimecode(curMs, fps);
        lblAbsoluteTC.setText(tc.toString(), juce::dontSendNotification);
    }

    /// A refused [ or ] (#24): the button red for kRefusedFlashTicks of the
    /// 30 Hz timer, then its own colour again (endFlash).  Not modal, and no
    /// focus or keyboard change.
    void flashRefused(juce::TextButton& b, int& ticks)
    {
        b.setColour(juce::TextButton::buttonColourId, refusedRed);
        ticks = kRefusedFlashTicks;
    }
    void endFlash(juce::TextButton& b, int& ticks)
    {
        if (ticks > 0 && --ticks == 0)
            b.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF444444));
    }

    static juce::String formatTime(double seconds)
    {
        const int total = (int) seconds;
        const int hours = total / 3600;
        const int mins  = (total % 3600) / 60;
        const int secs  = total % 60;
        if (hours > 0)
            return juce::String::formatted("%d:%02d:%02d", hours, mins, secs);
        return juce::String::formatted("%d:%02d", mins, secs);
    }

    //==========================================================================
    juce::Colour bgWindow    { 0xFF12141A };
    juce::Colour textBright  { 0xFFDDDDDD };
    juce::Colour textMid     { 0xFF999999 };
    juce::Colour accentAmber { 0xFFCC8844 };
    juce::Colour accentCyan  { 0xFF00AAFF };
    juce::Colour abMarkerAmber { 0xFFFFAA00 };   // GeneratorWaveformView's A/B markers
    juce::Colour refusedRed  { 0xFFAA3333 };   // a refused [ or ] (flashRefused)
    static constexpr int kRefusedFlashTicks = 12;   // 0.4 s of the 30 Hz timer
    int loopInFlashTicks = 0, loopOutFlashTicks = 0;

    GeneratorAudioPlayer* player = nullptr;
    TimecodeEngine*       engine = nullptr;

    GeneratorWaveformView waveform;
    juce::Label   lblFilename, lblFileTime, lblAbsoluteTC, lblVolume;
    juce::Label   lblLoopState;   // the loop in charge (updateLoopState)
    juce::Label   lblPreset;      // the preset GO starts (updatePresetForGo)
    static constexpr const char* kNoPresetText = "PRESET: none selected";
    LoopInCharge  shownLoopState = LoopInCharge::none;
    juce::TextButton btnGo, btnPlay, btnPause, btnStop;
    juce::TextButton btnPrev, btnNext, btnEdit;
    juce::TextButton   btnLoopIn, btnLoopOut;
    juce::ToggleButton btnLoopOn;
    juce::Slider     sldVolume;
    bool             ignoreVolume = false;  // set when programmatically syncing the slider
    std::function<bool()> isLockedFn;       // returns true when Show Lock is active;
                                            // consumed by loop In/Out buttons to refuse
                                            // timestamp-setting gestures during a locked show

public:
    /// Set by MainComponent so the PREV/NEXT/EDIT buttons in the floating
    /// window route through the same logic as the panel's preset combo
    /// (loading the new preset's TC range and waveform when idle, etc.).
    /// MainComponent is responsible for capturing only safe references in
    /// these lambdas.
    std::function<void()> onPrev;
    std::function<void()> onNext;
    std::function<void()> onEdit;
    /// GO (#24): MainComponent runs the panel's GO, which is this window's
    /// engine's (the window follows the selected engine), the same under
    /// Show Lock, and only with the generator as the input, in transport
    /// mode (goAvailable), and a preset selected.
    std::function<void()> onGo;
    /// The name of the preset selected in the main window's preset list,
    /// empty with none (#24): what GO starts, shown as PRESET; GO is greyed
    /// out without one.  Polled by the 30 Hz timer.  Message thread.
    std::function<juce::String()> selectedPresetName;

    /// Called when the user moves the volume slider.  MainComponent uses
    /// this to update the panel's slider so both stay in sync.
    std::function<void(float)> onVolumeChanged;

    /// Called whenever the user changes the loop In/Out/enabled via the
    /// buttons here, so MainComponent can flush settings to disk (and any
    /// future UI replicas can resync).
    std::function<void()> onLoopChanged;

    /// Sync the window's slider to a value pushed in from the outside (e.g.
    /// the panel's slider was moved).  Sends no notification, so does not
    /// re-fire onVolumeChanged.
    void setVolumeFromOutside(float v)
    {
        ignoreVolume = true;
        sldVolume.setValue(v, juce::dontSendNotification);
        ignoreVolume = false;
    }

private:

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GeneratorWaveformWindow)
};
