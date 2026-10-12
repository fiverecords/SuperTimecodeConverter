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
//   - Header: filename + file-time / total-time, and LOOP TC while the
//     preset's "Loop timecode" (#24) acts (loopTimecodeShown)
//   - Big absolute TC display (HH:MM:SS:FF)
//   - GeneratorWaveformView in Full mode (markers + click-to-seek)
//   - Transport buttons (PLAY / PAUSE / STOP)
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

        // The timecode loop's indicator.  This window's own loop controls
        // (A/B LOOP, [ ], L) are the A/B loop's, so the header says when the
        // other loop, the preset's, is the one running.
        addChildComponent(lblLoopTc);
        lblLoopTc.setText("LOOP TC", juce::dontSendNotification);
        lblLoopTc.setFont(juce::Font(juce::FontOptions(13.0f).withStyle("Bold")));
        lblLoopTc.setColour(juce::Label::textColourId, accentCyan);
        lblLoopTc.setJustificationType(juce::Justification::centredRight);
        lblLoopTc.setTooltip("Loop timecode is on for this preset (set in Generator Presets, "
                             "or with the OSC command looptc): at its end the timecode and "
                             "the audio go back to Start TC.  With an audio file, the cyan "
                             "line on the waveform marks that end.  A/B LOOP is a separate "
                             "rehearsal loop.");

        // --- Big absolute TC ---
        addAndMakeVisible(lblAbsoluteTC);
        lblAbsoluteTC.setFont(juce::Font(juce::FontOptions(28.0f).withStyle("Bold")));
        lblAbsoluteTC.setColour(juce::Label::textColourId, accentAmber);
        lblAbsoluteTC.setJustificationType(juce::Justification::centredLeft);
        lblAbsoluteTC.setText("--:--:--:--", juce::dontSendNotification);

        // --- Waveform ---
        addAndMakeVisible(waveform);
        waveform.setMode(GeneratorWaveformView::Mode::Full);

        // --- Transport ---
        for (auto* btn : { &btnPlay, &btnPause, &btnStop, &btnPrev, &btnNext, &btnEdit })
        {
            addAndMakeVisible(btn);
            btn->setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF444444));
            btn->setColour(juce::TextButton::textColourOffId, textBright);
        }
        btnPlay.setButtonText("PLAY");
        btnPause.setButtonText("PAUSE");
        btnStop.setButtonText("STOP");
        btnPrev.setButtonText("<");
        btnNext.setButtonText(">");
        btnEdit.setButtonText("EDIT");

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
        btnLoopIn .setTooltip("A/B loop: set In at the playhead (keyboard: [, in this window or the main window)");
        btnLoopOut.setTooltip("A/B loop: set Out at the playhead (keyboard: ], in this window or the main window)");

        // "A/B LOOP", not "LOOP": the preset's Loop timecode is the other
        // loop (#24).
        addAndMakeVisible(btnLoopOn);
        btnLoopOn.setButtonText("A/B LOOP");
        btnLoopOn.setColour(juce::ToggleButton::textColourId, textBright);
        btnLoopOn.setTooltip("A/B loop on / off (keyboard: L, in this window or the main window).  When on, "
                             "playback goes back from Out to In, and PLAY from STOP starts at "
                             "In.  A rehearsal loop, separate from the preset's Loop timecode "
                             "(LOOP TC), which ignores an A/B not inside its own loop (drawn "
                             "dashed).");

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
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::InfoIcon, "Loop In",
                    "In point must be earlier than Out.  Move the playhead before "
                    "the current Out, or clear the Out first.");
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
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::InfoIcon, "Loop Out",
                    "Out point must be after In.  Move the playhead after the "
                    "current In, or set the In first.");
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
    /// mark uses too.  What this window's LOOP TC and the panel's STOP TC
    /// (LOOP) label show.  Message thread.
    static bool loopTimecodeShown(const TimecodeEngine& e)
    {
        return GeneratorWaveformView::loopTimecodeShown(e);
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

        // Header row: filename left, file-time right, LOOP TC before it.
        auto headerRow = area.removeFromTop(20);
        lblFileTime.setBounds(headerRow.removeFromRight(180));
        lblLoopTc.setBounds(headerRow.removeFromRight(80));
        lblFilename.setBounds(headerRow);
        area.removeFromTop(2);

        // Big absolute TC.
        lblAbsoluteTC.setBounds(area.removeFromTop(34));
        area.removeFromTop(8);

        // Transport row at bottom.
        // Layout: [PREV]  [PLAY] [PAUSE] [STOP]  [NEXT] | VOL [slider]  ...gap... [EDIT]
        auto transportRow = area.removeFromBottom(28);
        const int btnW = 90, gap = 4, navW = 40, editW = 60;
        const int volLabelW = 28, volSliderMinW = 140;

        btnPrev .setBounds(transportRow.removeFromLeft(navW)); transportRow.removeFromLeft(gap);
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
        const bool loopTc = engine != nullptr && loopTimecodeShown(*engine);
        if (lblLoopTc.isVisible() != loopTc)
            lblLoopTc.setVisible(loopTc);
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

    GeneratorAudioPlayer* player = nullptr;
    TimecodeEngine*       engine = nullptr;

    GeneratorWaveformView waveform;
    juce::Label   lblFilename, lblFileTime, lblAbsoluteTC, lblVolume;
    juce::Label   lblLoopTc;   // "LOOP TC", shown while loopTimecodeShown()
    juce::TextButton btnPlay, btnPause, btnStop;
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
