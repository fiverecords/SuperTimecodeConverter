// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

#pragma once
#include <JuceHeader.h>
#include "AppSettings.h"
#include "GeneratorCuePointEditor.h"

//==============================================================================
// GeneratorPresetEditor -- Table editor for named timecode generator presets.
//
// Each preset has a Name, Start TC, Stop TC, an optional audio file, what
// happens at the end ("Loop audio only", or "Loop timecode", #24 -- one or
// neither), and cue points (edited in their own window, Cues...).  The
// editor mirrors the TrackMapEditor UX: table + form at the bottom for
// add/edit.
// Calls onChange() whenever the preset map is modified, and
// onPresetUpdated() after Save has updated a preset.
//==============================================================================
class GeneratorPresetEditor : public juce::Component,
                              public juce::TableListBoxModel
{
public:
    GeneratorPresetEditor(GeneratorPresetMap& map)
        : presetMap(map)
    {
        setSize(600, 440);   // room for the columns' default widths (560 px)
        rebuildRows();

        // --- Table ---
        addAndMakeVisible(table);
        table.setModel(this);
        table.setColour(juce::ListBox::backgroundColourId, bgDarker);
        table.setColour(juce::ListBox::outlineColourId, borderCol);
        table.setOutlineThickness(1);
        table.setRowHeight(24);
        table.setHeaderHeight(22);
        table.getHeader().setStretchToFitActive(true);

        // Stop TC is wider than Start TC: after the Stop TC, or after
        // "(audio length)", it shows "  (loop TC)" for a Loop timecode preset
        // (#24, paintCell), which needs about 120 px in all.
        auto& hdr = table.getHeader();
        hdr.addColumn("Name",     ColName,    140, 80, 300, juce::TableHeaderComponent::notSortable);
        hdr.addColumn("Start TC", ColStart,   100, 80, 180, juce::TableHeaderComponent::notSortable);
        hdr.addColumn("Stop TC",  ColStop,    140, 80, 220, juce::TableHeaderComponent::notSortable);
        hdr.addColumn("Audio",    ColAudio,   180,  0, 600, juce::TableHeaderComponent::notSortable);

        // --- Buttons ---
        auto addBtn = [this](juce::TextButton& btn, const juce::String& text)
        {
            addAndMakeVisible(btn);
            btn.setButtonText(text);
            btn.setColour(juce::TextButton::buttonColourId, bgPanel);
            btn.setColour(juce::TextButton::textColourOffId, textBright);
        };

        addBtn(btnAdd,      "Add");
        addBtn(btnSave,     "Save");
        addBtn(btnDelete,   "Delete");
        addBtn(btnCues,     "Cues...");
        addBtn(btnClearAll, "Clear All");
        addBtn(btnImport,   "Import");
        addBtn(btnExport,   "Export");
        addBtn(btnOscHelp,  "OSC ?");

        btnAdd.setTooltip("Create a NEW preset from the form fields; the selected preset is not "
                          "changed (to change it, use Save). "
                          "If a preset with the same name already exists, "
                          "the new one is auto-numbered (e.g. \"My preset (2)\").");
        // The live part of Save (#24): MainComponent::applyGenPresetLoopBoxesLive.
        btnSave.setTooltip("Update the selected preset with the current form fields. "
                           "You can rename it by editing the Name field. "
                           "Disabled until a preset is selected -- use Add to create a new one. "
                           "The two loop boxes take effect at once on an engine that has this "
                           "preset loaded, also while it plays, unless the name or the audio "
                           "file changes; everything else takes effect when the preset is next "
                           "applied (GO).");
        btnDelete.setTooltip("Delete the selected preset. Disabled until a preset is selected.");
        btnCues.setTooltip("Edit cue points for the selected preset. "
                           "Cue points fire MIDI / OSC / Art-Net triggers when the generated TC "
                           "reaches their position. Disabled until a preset is selected.");

        btnSave.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF22AA44).withAlpha(0.3f));
        btnSave.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFF22DD55));

        btnCues.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFFAA8800).withAlpha(0.3f));
        btnCues.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFFFCC44));

        btnImport.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF2244AA).withAlpha(0.3f));
        btnImport.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFF4488FF));
        btnExport.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF2244AA).withAlpha(0.3f));
        btnExport.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFF4488FF));
        btnOscHelp.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF555555));
        btnOscHelp.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFFFCC44));

        btnAdd.onClick    = [this] { addNewPreset(); };
        btnSave.onClick   = [this] { saveSelectedPreset(); };
        btnDelete.onClick = [this] { deleteSelected(); };
        btnCues.onClick   = [this] { openCueEditor(); };
        btnClearAll.onClick = [this] { clearAll(); };
        btnImport.onClick  = [this] { importPresets(); };
        btnExport.onClick  = [this] { exportPresets(); };
        btnOscHelp.onClick = [this] { showOscReference(); };

        // --- Form fields ---
        auto addField = [this](juce::Label& lbl, juce::TextEditor& ed,
                                const juce::String& labelText, const juce::String& defaultText)
        {
            addAndMakeVisible(lbl);
            lbl.setText(labelText, juce::dontSendNotification);
            lbl.setFont(juce::Font(juce::FontOptions(9.0f)));
            lbl.setColour(juce::Label::textColourId, textMid);
            lbl.setJustificationType(juce::Justification::centredRight);

            addAndMakeVisible(ed);
            ed.setFont(juce::Font(juce::FontOptions(11.0f)));
            ed.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xFF222222));
            ed.setColour(juce::TextEditor::textColourId, juce::Colours::white);
            ed.setText(defaultText, false);
        };

        addField(lblName,    edName,    "Name:",     "");
        addField(lblStartTC, edStartTC, "Start TC:", "00:00:00:00");
        addField(lblStopTC,  edStopTC,  "Stop TC:",  "00:00:00:00");
        addField(lblAudio,   edAudio,   "Audio:",    "");
        edAudio.setReadOnly(true);
        edAudio.setColour(juce::TextEditor::textColourId, juce::Colour(0xFFCCCCCC));

        addAndMakeVisible(btnBrowseAudio);
        btnBrowseAudio.setButtonText("...");
        btnBrowseAudio.setColour(juce::TextButton::buttonColourId, bgPanel);
        btnBrowseAudio.setColour(juce::TextButton::textColourOffId, textBright);
        btnBrowseAudio.onClick = [this] { browseForAudio(); };

        addAndMakeVisible(btnClearAudio);
        btnClearAudio.setButtonText("X");
        btnClearAudio.setColour(juce::TextButton::buttonColourId, bgPanel);
        btnClearAudio.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFFF6666));
        btnClearAudio.onClick = [this] { clearAudioField(); };

        // The two end options exclude each other: ticking one unticks the
        // other.  "Loop audio only" loops only the audio (the timecode
        // counts on past the file's end); "Loop timecode" (#24) takes the
        // timecode, the audio and the cues back to Start TC where the
        // generator would stop: at the end of the audio file, or at Stop TC
        // for a preset without one.  Save applies both at once to an engine
        // that has the preset loaded (onPresetUpdated), which each tooltip
        // says (kLoopBoxSaveNote).  1.9.15-beta2 called the first "Loop
        // audio file when it ends", which did not say that the timecode
        // goes on (#24, Joaky's field test).
        addAndMakeVisible(btnLoopAudio);
        btnLoopAudio.setButtonText("Loop audio only (timecode keeps counting)");
        btnLoopAudio.setColour(juce::ToggleButton::textColourId, textBright);
        btnLoopAudio.setTooltip(juce::String("Only the audio loops: at the end of the file it starts again "
                                             "from the top, while the timecode keeps counting (up to a Stop "
                                             "TC set after GO, or for ever) and the cue points fire on the "
                                             "first pass only. For a music bed under a longer timecode. To "
                                             "loop the timecode with the audio, tick Loop timecode below. ")
                                + kLoopBoxSaveNote);
        btnLoopAudio.onClick = [this]
        {
            if (btnLoopAudio.getToggleState())
                btnLoopTimecode.setToggleState(false, juce::dontSendNotification);
        };

        addAndMakeVisible(btnLoopTimecode);
        btnLoopTimecode.setButtonText("Loop timecode (back to Start TC at the end)");
        btnLoopTimecode.setColour(juce::ToggleButton::textColourId, textBright);
        btnLoopTimecode.setTooltip(juce::String("At Stop TC, or at the end of the audio file, the timecode "
                                                "and the audio go back to Start TC. ")
                                   + kLoopBoxSaveNote);
        btnLoopTimecode.onClick = [this]
        {
            if (btnLoopTimecode.getToggleState())
                btnLoopAudio.setToggleState(false, juce::dontSendNotification);
        };

        // Enter on any form field commits the form.  If a preset is
        // selected, Enter saves it (matches the natural reading of "I'm
        // editing this preset, finish editing"); otherwise Enter creates
        // a new preset (matches "I just typed a name, add it").  This
        // avoids the surprise where Enter silently does nothing when there
        // is no selection.
        auto commitForm = [this]
        {
            if (table.getSelectedRow() >= 0
                && table.getSelectedRow() < (int)rows.size())
                saveSelectedPreset();
            else
                addNewPreset();
        };
        edName.onReturnKey    = commitForm;
        edStartTC.onReturnKey = commitForm;
        edStopTC.onReturnKey  = commitForm;

        updateStopTCEnabledState();
        updateButtonStates();
    }

    ~GeneratorPresetEditor() override = default;

    std::function<void()> onChange;

    /// Called after Save (the Update of the selected preset) has stored it,
    /// after onChange, with the preset as stored.  MainComponent applies
    /// the preset's two loop boxes to every engine that has it loaded, at
    /// once (#24, Joaky 2026-10-12): an engine that names it and was given
    /// its file, so a renamed preset, or one given another file, is left to
    /// GO.  Not called by Add, which makes a new preset.  Message thread.
    std::function<void(const GeneratorPreset& stored)> onPresetUpdated;

    /// True when the form holds what Save (or Add) has not stored (#24,
    /// Joaky's field test: a box ticked and the window closed without Save
    /// was lost without a word).  With a preset selected, the form differs
    /// from it; with none, from the preset of the form's name if there is
    /// one, otherwise from an empty form: a new preset with content.  A
    /// Start or Stop TC compares as Save would store it (normalizeTC), and
    /// the loop boxes as selectedRowsChanged shows a preset.  Message thread.
    bool hasUnsavedChanges() const
    {
        const auto* stored = editingName.isNotEmpty() ? presetMap.find(editingName)
                                                      : presetMap.find(edName.getText().trim());
        if (stored == nullptr)
            return edName.getText().trim().isNotEmpty() || edAudio.getText().trim().isNotEmpty()
                || formTCDiffers(edStartTC, "00:00:00:00") || formTCDiffers(edStopTC, "00:00:00:00")
                || btnLoopAudio.getToggleState() || btnLoopTimecode.getToggleState();
        return edName.getText().trim() != stored->name
            || formTCDiffers(edStartTC, stored->startTC) || formTCDiffers(edStopTC, stored->stopTC)
            || edAudio.getText().trim() != stored->audioFilePath
            || btnLoopAudio.getToggleState() != (stored->audioLoop && ! stored->loopTimecode)
            || btnLoopTimecode.getToggleState() != stored->loopTimecode;
    }

    /// The window's close button with unsaved changes (hasUnsavedChanges):
    /// ask Save / Discard / Cancel.  Save is the form's commit, as Enter in
    /// a field: Save (the Update of the selected preset, whose loop boxes
    /// then reach an engine that has it loaded, onPresetUpdated) or, with
    /// none selected, Add; then `closeWindow` -- unless it stored nothing (an
    /// empty name, or a name another preset has: the form stays, as with
    /// the button).  Discard calls `closeWindow` and drops the form;
    /// Cancel, or Escape, keeps the window.  The question covers the editor
    /// (ClosePrompt) and is not modal: the other windows -- the waveform
    /// window's STOP and PLAY, the panel -- take clicks while it is up, as
    /// the music may be playing (the modal AlertWindow first used here
    /// swallowed them until answered, as the waveform window's [ and ] did
    /// before #24).  Asked again while it is up, it stays as it is.  It goes
    /// with the editor (closePrompt), and its answer then never comes.  The
    /// answer is acted on in the button's click; the prompt is deleted, and
    /// the window closed, a message later.  Show Lock's forced close does
    /// not ask (MainComponent).  Message thread.
    void askToSaveBeforeClose(std::function<void()> closeWindow)
    {
        if (closePrompt != nullptr)
        {
            closePrompt->grabKeyboardFocus();
            return;
        }
        const bool update = editingName.isNotEmpty();
        const juce::String message = update ? "The form has changes that are not saved in the preset \""
                                              + editingName + "\".  Save them before closing?"
                                            : juce::String("The form holds a new preset that is not saved.  "
                                                           "Save it before closing?");
        closePrompt = std::make_unique<ClosePrompt>(message,
            [this, update, closeWindow = std::move(closeWindow)](int result)
            {
                // Inside the button's click: the answer is acted on at once
                // (Save stores the preset, and its loop boxes reach the
                // engines, as the Save button does), while the prompt, whose
                // button this is, goes a message later, and the window with
                // it after Save or Discard.
                bool close = result == ClosePrompt::discard;
                if (result == ClosePrompt::save)
                    close = update ? saveSelectedPreset() : addNewPreset();
                juce::Component::SafePointer<GeneratorPresetEditor> self(this);
                juce::MessageManager::callAsync([self, close, closeWindow]
                {
                    if (auto* editor = self.getComponent())
                    {
                        editor->closePrompt.reset();
                        if (close)
                            closeWindow();
                    }
                });
            });
        addAndMakeVisible(*closePrompt);
        closePrompt->setBounds(getLocalBounds());
        closePrompt->grabKeyboardFocus();
    }

    /// The question askToSaveBeforeClose asks, over the whole editor: a
    /// dimmed backdrop that takes the editor's clicks, and a panel with the
    /// message and Save / Discard / Cancel.  Escape is Cancel.  `answer`
    /// gets the first button pressed (save, discard or cancel), once.  Not
    /// modal: windows other than the editor's are not blocked.  Message
    /// thread.
    class ClosePrompt : public juce::Component
    {
    public:
        enum { save = 1, discard = 2, cancel = 3 };

        ClosePrompt(const juce::String& message, std::function<void(int)> answer)
            : onAnswer(std::move(answer))
        {
            addAndMakeVisible(lblTitle);
            lblTitle.setText("Unsaved changes", juce::dontSendNotification);
            lblTitle.setFont(juce::Font(juce::FontOptions(15.0f).withStyle("Bold")));
            lblTitle.setColour(juce::Label::textColourId, juce::Colour(0xFFDDDDDD));
            addAndMakeVisible(lblMessage);
            lblMessage.setText(message, juce::dontSendNotification);
            lblMessage.setFont(juce::Font(juce::FontOptions(13.0f)));
            lblMessage.setColour(juce::Label::textColourId, juce::Colour(0xFFDDDDDD));
            lblMessage.setJustificationType(juce::Justification::topLeft);
            lblMessage.setMinimumHorizontalScale(1.0f);
            int result = save;
            for (auto* b : { &btnSave, &btnDiscard, &btnCancel })
            {
                addAndMakeVisible(b);
                b->setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF444444));
                b->setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFDDDDDD));
                b->onClick = [this, r = result++] { give(r); };
            }
            btnSave.setButtonText("Save");
            btnDiscard.setButtonText("Discard");
            btnCancel.setButtonText("Cancel");
            setWantsKeyboardFocus(true);
        }

        void paint(juce::Graphics& g) override
        {
            g.fillAll(juce::Colours::black.withAlpha(0.55f));
            g.setColour(juce::Colour(0xFF2A2A2A));
            g.fillRoundedRectangle(panel.toFloat(), 6.0f);
            g.setColour(juce::Colour(0xFFFFB74D));
            g.drawRoundedRectangle(panel.toFloat().reduced(0.5f), 6.0f, 1.0f);
        }

        void resized() override
        {
            panel = getLocalBounds().withSizeKeepingCentre(juce::jmin(420, getWidth() - 20), 150);
            auto area = panel.reduced(14, 10);
            lblTitle.setBounds(area.removeFromTop(22));
            auto buttons = area.removeFromBottom(26);
            area.removeFromBottom(8);
            lblMessage.setBounds(area);
            const int w = 80, gap = 6;
            btnCancel .setBounds(buttons.removeFromRight(w)); buttons.removeFromRight(gap);
            btnDiscard.setBounds(buttons.removeFromRight(w)); buttons.removeFromRight(gap);
            btnSave   .setBounds(buttons.removeFromRight(w));
        }

        bool keyPressed(const juce::KeyPress& key) override
        {
            if (key == juce::KeyPress::escapeKey)
            {
                give(cancel);
                return true;
            }
            return false;
        }

    private:
        void give(int result)
        {
            if (answered)
                return;
            answered = true;
            if (onAnswer)
                onAnswer(result);
        }

        std::function<void(int)> onAnswer;
        bool answered = false;
        juce::Rectangle<int> panel;
        juce::Label lblTitle, lblMessage;
        juce::TextButton btnSave, btnDiscard, btnCancel;
    };

    /// Set the selected engine's fps: the rate the Start/Stop TC fields are
    /// normalised at, and the one the cue editor converts cue TC to ms (and
    /// back) with.  Called by MainComponent when it opens the editor; a call
    /// while the editor is open (the selected engine, or its rate, changed)
    /// is forwarded to the cue window if one is open.  Message thread.
    void setCurrentFpsForCueEditor(double fpsValue)
    {
        if (fpsValue <= 0.0) return;
        currentFpsForCueEditor = fpsValue;
        if (cueWindow != nullptr)
            if (auto* ed = cueWindow->getEditor())
                ed->setFps(fpsValue);
    }

    /// Install a playhead getter that the cue editor (when opened) will
    /// poll at ~30Hz to drive the live playback cursor in the waveform
    /// strip.  Returns ms-from-audio-start (i.e. corrected for the
    /// preset's startTC) or a negative value when nothing is playing.
    /// Pass empty std::function to disable.
    void setPlayheadGetter(std::function<int64_t()> getter)
    {
        playheadGetter = std::move(getter);
        // If a cue editor is already open, push the new getter to it
        // immediately so toggling playback feedback doesn't require
        // closing and reopening the cue window.
        if (cueWindow != nullptr)
            if (auto* ed = cueWindow->getEditor())
                ed->setPlayheadGetter(playheadGetter);
    }

    //--------------------------------------------------------------------------
    // TableListBoxModel
    //--------------------------------------------------------------------------
    int getNumRows() override { return (int)rows.size(); }

    void paintRowBackground(juce::Graphics& g, int rowNumber, int, int,
                            bool rowIsSelected) override
    {
        g.fillAll(rowIsSelected ? accentCyan.withAlpha(0.15f)
                                : (rowNumber % 2 == 0 ? bgDarker : bgDarker.brighter(0.03f)));
    }

    void paintCell(juce::Graphics& g, int rowNumber, int columnId,
                   int width, int height, bool /*rowIsSelected*/) override
    {
        if (rowNumber < 0 || rowNumber >= (int)rows.size()) return;
        auto& p = rows[(size_t)rowNumber];

        // When the preset carries an audio file, the Stop TC field is
        // ignored at runtime in favour of the file's actual length.  Render
        // it muted in the list so the user sees that the stored value is
        // not the one being applied.  This goes by the path alone: GO
        // applies the Stop TC of a preset whose file is missing on disk
        // (MainComponent::applyGenPresetToEngine).
        const bool stopOverriddenByAudio = (columnId == ColStop) && p.audioFilePath.isNotEmpty();
        const juce::Font font(juce::FontOptions(11.0f));
        g.setColour(stopOverriddenByAudio ? textMid : textBright);
        g.setFont(font);
        juce::String text, suffix;
        switch (columnId)
        {
            case ColName:  text = p.name;    break;
            case ColStart: text = p.startTC; break;
            case ColStop:
                text = stopOverriddenByAudio ? juce::String("(audio length)") : p.stopTC;
                break;
            case ColAudio:
                if (p.audioFilePath.isNotEmpty())
                {
                    text = juce::File(p.audioFilePath).getFileName();
                    // A file with both flags runs "Loop timecode" (the engine's
                    // rule, TimecodeEngine::setGeneratorAudioFile).  "(loop
                    // audio)", not "(loop)" as in 1.9.15-beta2: with the
                    // timecode's "(loop TC)" and the A/B loop there are three
                    // loops, and this is the one the box and the waveform
                    // window's LOOP AUDIO name.
                    if (p.audioLoop && ! p.loopTimecode) suffix = "  (loop audio)";
                }
                break;
        }
        // The suffix is kept whole and the file name cut with an ellipsis
        // before it: "(loop audio)" is 6 characters longer than beta2's
        // "(loop)", and at the end of the text it was what the ellipsis cut
        // first, for file names of ordinary length at the editor's default
        // width.
        int textW = width - 8;
        if (suffix.isNotEmpty())
            textW = juce::jmax(0, textW - juce::roundToInt(juce::GlyphArrangement::getStringWidth(font, suffix)) - 1);
        g.drawText(text, 4, 0, textW, height, juce::Justification::centredLeft, true);
        if (suffix.isNotEmpty())
        {
            const int x = 4 + juce::jmin(textW, juce::roundToInt(juce::GlyphArrangement::getStringWidth(font, text)));
            g.drawText(suffix, x, 0, juce::jmax(0, width - 4 - x), height, juce::Justification::centredLeft, true);
        }

        // "Loop timecode" (#24) after the Stop TC: bright, or muted when a
        // preset without an audio file gives the loop nothing to act on --
        // no Stop TC, so no end to loop at and the generator runs on; or a
        // Stop TC less than kMinLoopTimecodeMs after the Start TC (at or
        // before it included), where the option does nothing and the
        // generator stops there as without it (TimecodeEngine::
        // loopTimecodeEnd).  With an audio file the end is the file's, whose
        // length the table does not know.
        if (columnId == ColStop && p.loopTimecode)
        {
            const FrameRate fps = frameRateFromDouble(currentFpsForCueEditor);
            const double stopMs = parseTimecodeTextToMs(p.stopTC, fps);
            const bool noEnd = p.audioFilePath.isEmpty()
                && (stopMs <= 0.0 || stopMs - parseTimecodeTextToMs(p.startTC, fps) < kMinLoopTimecodeMs);
            const int x = 4 + juce::roundToInt(juce::GlyphArrangement::getStringWidth(font, text));
            g.setColour(noEnd ? textMid : textBright);
            g.drawText("  (loop TC)", x, 0, juce::jmax(0, width - 4 - x), height,
                       juce::Justification::centredLeft, true);
        }
    }

    void selectedRowsChanged(int lastRowSelected) override
    {
        if (lastRowSelected >= 0 && lastRowSelected < (int)rows.size())
        {
            auto& p = rows[(size_t)lastRowSelected];
            editingName = p.name;
            edName.setText(p.name, false);
            edStartTC.setText(p.startTC, false);
            edStopTC.setText(p.stopTC, false);
            edAudio.setText(p.audioFilePath, false);
            // A preset file with both flags (only a hand edit makes one) is
            // shown, and saved back, as the engine runs it: Loop timecode.
            btnLoopAudio.setToggleState(p.audioLoop && ! p.loopTimecode, juce::dontSendNotification);
            btnLoopTimecode.setToggleState(p.loopTimecode, juce::dontSendNotification);
            updateStopTCEnabledState();
        }
        else
        {
            editingName.clear();
        }
        // If the cue editor window is open on a different preset, close it
        // to avoid showing cues that don't belong to the current selection.
        // Re-opening Cues from the toolbar will bind it to the new selection.
        closeCueEditorWindow();
        updateButtonStates();
    }

    /// SAVE / DELETE are only meaningful when a preset is selected -- without
    /// a selection there's nothing to update or delete.  Disabling them in
    /// that state makes the difference between ADD ("create new from form")
    /// and SAVE ("update the selected one") obvious without having to read
    /// any documentation.
    void updateButtonStates()
    {
        const bool hasSelection = table.getSelectedRow() >= 0
                                  && table.getSelectedRow() < (int)rows.size();
        btnSave.setEnabled(hasSelection);
        btnDelete.setEnabled(hasSelection);
        btnCues.setEnabled(hasSelection);
    }

    /// When an audio file is set, the audio's actual length is what the
    /// runtime uses -- the preset's Stop TC stored value is ignored.  Make
    /// that visually obvious in the form by disabling the field and
    /// dimming its text, and surface a tooltip explaining why.
    void updateStopTCEnabledState()
    {
        const bool hasAudio = edAudio.getText().trim().isNotEmpty();
        edStopTC.setEnabled(! hasAudio);
        edStopTC.setReadOnly(hasAudio);
        // setColour() only changes the colour for *new* text; the characters
        // already in the editor keep whatever colour they were drawn with.
        // applyColourToAllText repaints existing characters too, so when the
        // audio is cleared the previously-dimmed Stop TC text returns to the
        // bright colour immediately instead of staying greyed-out until the
        // user retypes it.
        const auto col = hasAudio ? textMid : textBright;
        edStopTC.setColour(juce::TextEditor::textColourId, col);
        edStopTC.applyColourToAllText(col, true);
        edStopTC.setTooltip(hasAudio
            ? "Stop TC is determined by the loaded audio file's length when one is set. "
              "Clear the audio field to edit Stop TC manually."
            : juce::String());
    }

    void cellDoubleClicked(int rowNumber, int, const juce::MouseEvent&) override
    {
        // Double-click = populate form for editing
        selectedRowsChanged(rowNumber);
    }

    //--------------------------------------------------------------------------
    // Layout
    //--------------------------------------------------------------------------
    void resized() override
    {
        auto area = getLocalBounds().reduced(8);

        // Bottom form: 4 rows of label+editor + 2 loop rows + 2 button rows
        auto formArea = area.removeFromBottom(210);
        area.removeFromBottom(6);

        // File operations row (Import / Export / OSC Help)
        auto fileRow = formArea.removeFromBottom(26);
        int fileBtnW = 80, gap = 4;
        btnImport.setBounds(fileRow.removeFromLeft(fileBtnW)); fileRow.removeFromLeft(gap);
        btnExport.setBounds(fileRow.removeFromLeft(fileBtnW)); fileRow.removeFromLeft(gap);
        btnOscHelp.setBounds(fileRow.removeFromRight(50));
        formArea.removeFromBottom(4);

        // Edit button row
        auto btnRow = formArea.removeFromBottom(26);
        int btnW = 65;
        btnAdd     .setBounds(btnRow.removeFromLeft(btnW)); btnRow.removeFromLeft(gap);
        btnSave    .setBounds(btnRow.removeFromLeft(btnW)); btnRow.removeFromLeft(gap);
        btnDelete  .setBounds(btnRow.removeFromLeft(btnW)); btnRow.removeFromLeft(gap);
        btnCues    .setBounds(btnRow.removeFromLeft(btnW)); btnRow.removeFromLeft(gap);
        btnClearAll.setBounds(btnRow.removeFromLeft(btnW));
        formArea.removeFromBottom(6);

        // Form fields
        auto layField = [](juce::Label& lbl, juce::TextEditor& ed, juce::Rectangle<int>& fa)
        {
            auto row = fa.removeFromTop(22);
            lbl.setBounds(row.removeFromLeft(60));
            row.removeFromLeft(4);
            ed.setBounds(row);
            fa.removeFromTop(3);
        };

        layField(lblName,    edName,    formArea);
        layField(lblStartTC, edStartTC, formArea);
        layField(lblStopTC,  edStopTC,  formArea);

        // Audio row: label + editor + Browse + Clear inline
        {
            auto row = formArea.removeFromTop(22);
            lblAudio.setBounds(row.removeFromLeft(60));
            row.removeFromLeft(4);
            btnClearAudio .setBounds(row.removeFromRight(24));
            row.removeFromRight(2);
            btnBrowseAudio.setBounds(row.removeFromRight(30));
            row.removeFromRight(4);
            edAudio.setBounds(row);
            formArea.removeFromTop(3);
        }

        // Loop checkbox rows (offset from labels for alignment)
        {
            auto row = formArea.removeFromTop(22);
            row.removeFromLeft(64);
            btnLoopAudio.setBounds(row);
            formArea.removeFromTop(3);
            row = formArea.removeFromTop(22);
            row.removeFromLeft(64);
            btnLoopTimecode.setBounds(row);
        }

        // Table fills the rest
        table.setBounds(area);

        if (closePrompt != nullptr)
            closePrompt->setBounds(getLocalBounds());
    }

private:
    enum { ColName = 1, ColStart, ColStop, ColAudio };

    GeneratorPresetMap& presetMap;
    juce::TableListBox table { "Presets", this };

    std::vector<GeneratorPreset> rows;

    // Name of the preset the form was loaded from (the map compares names
    // case-insensitively): set when a row is selected, cleared when the
    // selection is.  SAVE updates this preset.  The selected row is only
    // an index into `rows`, which an Import rebuilds without touching the
    // selection or the form, so the highlighted row could name another
    // preset (AUDIT SET-2; the same class as AUDIT SET-3).  Message thread.
    juce::String editingName;

    // Buttons
    juce::TextButton btnAdd, btnSave, btnDelete, btnCues, btnClearAll;
    juce::TextButton btnImport, btnExport, btnOscHelp;
    juce::TextButton btnBrowseAudio, btnClearAudio;
    juce::ToggleButton btnLoopAudio, btnLoopTimecode;   // never both ticked

    // Form
    juce::Label      lblName, lblStartTC, lblStopTC, lblAudio;
    juce::TextEditor edName, edStartTC, edStopTC, edAudio;

    // File chooser (must persist during async operation)
    std::unique_ptr<juce::FileChooser> fileChooser;

    // Cue editor window (lazy-created on first Cues click; reused after)
    std::unique_ptr<GeneratorCuePointEditorWindow> cueWindow;
    // Frame rate in fps the Start/Stop TC fields are normalised at, and the
    // one the cue editor uses when projecting cue TC onto the audio
    // waveform strip.  Pushed by MainComponent (the only thing that knows
    // the selected engine's current fps) via setCurrentFpsForCueEditor.
    // Default 30 if never set.
    double currentFpsForCueEditor = 30.0;

    // The sentence both loop boxes' tooltips end with (#24): when their
    // change reaches the engine (MainComponent::applyGenPresetLoopBoxesLive).
    static constexpr const char* kLoopBoxSaveNote =
        "It takes effect when you press Save, also on an engine that is playing this "
        "preset; if the name or the audio file changes, when the preset is next "
        "applied (GO).";

    // TimecodeEngine::kMinLoopTimecodeMs (#24), the shortest loop "Loop
    // timecode" makes, for the table's "(loop TC)"; repeated here so this
    // header does not include the engine's.
    static constexpr double kMinLoopTimecodeMs = 1000.0;

    // Playhead getter forwarded to the cue editor when opened.  Set by
    // MainComponent: the waveform strip's red cursor shows the selected
    // engine's generator position, measured from that engine's Start TC,
    // while its generator plays.  The getter does not check which preset
    // the engine has loaded, so the cursor also moves while it plays
    // another preset.
    std::function<int64_t()> playheadGetter;

    // Colors
    juce::Colour bgDarker   { 0xFF1A1A1A };
    juce::Colour bgPanel    { 0xFF333333 };
    juce::Colour borderCol  { 0xFF444444 };
    juce::Colour textBright { 0xFFDDDDDD };
    juce::Colour textMid    { 0xFF999999 };
    juce::Colour accentCyan { 0xFF00AAFF };

    void rebuildRows()
    {
        rows = presetMap.getAllSorted();
    }

    void notifyChange()
    {
        presetMap.save();
        rebuildRows();
        table.updateContent();
        reselectEditedPreset();
        table.repaint();
        updateButtonStates();
        if (onChange) onChange();
    }

    /// Row of the preset called `name` in `rows`, or -1.
    int rowOfPreset(const juce::String& name) const
    {
        for (int i = 0; i < (int)rows.size(); ++i)
            if (rows[(size_t)i].name.equalsIgnoreCase(name))
                return i;
        return -1;
    }

    /// After the rows were rebuilt, put the highlight back on the preset the
    /// form edits (editingName), without reloading the form.  When that
    /// preset is gone (deleted), nothing is selected and the form is no
    /// longer bound to a preset.
    void reselectEditedPreset()
    {
        const int row = editingName.isNotEmpty() ? rowOfPreset(editingName) : -1;
        juce::SparseSet<int> sel;
        if (row >= 0)
            sel.addRange({ row, row + 1 });
        else
            editingName.clear();
        table.setSelectedRows(sel, juce::dontSendNotification);
    }

    /// Normalise a typed Start/Stop TC through TimecodeCore's shared text
    /// parser (AUDIT C1, AUDIT SET-8) at currentFpsForCueEditor, the rate
    /// MainComponent last pushed: ':' '.' or ';' between fields, each field
    /// clamped to its range (frames to the rate), at 29.97 a drop-frame
    /// number that does not exist moved to the first that does.  The field
    /// then shows what an engine at that rate runs.  Presets are shared by
    /// every engine, and an engine parses the stored text again at its own
    /// rate when it loads one (MainComponent::loadGenPresetToFields), so an
    /// engine at another rate may clamp or move the frames once more.  The
    /// private copy this replaces lost the frames after a ';', carried
    /// frames at 30 whatever the rate, and wrapped hours past 23 to 0.
    juce::String normalizeTC(const juce::String& tc) const
    {
        return formatTimecodeText(parseTimecodeText(tc, frameRateFromDouble(currentFpsForCueEditor)));
    }

    /// Whether the Start/Stop TC field `ed` would store something other
    /// than `stored`, as readFormToPreset stores it (hasUnsavedChanges).
    bool formTCDiffers(const juce::TextEditor& ed, const juce::String& stored) const
    {
        const juce::String typed = ed.getText().trim();
        if (typed.isNotEmpty() && typed == stored)
            return false;
        return normalizeTC(typed.isEmpty() ? juce::String("00:00:00:00") : typed) != stored;
    }

    /// Put the form fields (name, Start/Stop TC, audio file, the two loop
    /// options) into `p` and return it; whatever the form does not show --
    /// the cue points -- is left as `p` has it.  SAVE passes the stored preset
    /// (AUDIT SET-2); ADD a blank one carrying only the Start/Stop TC of
    /// the preset the form was loaded from, if any (addNewPreset).
    /// A Start/Stop TC field that still holds `p`'s text is kept as it is;
    /// any other is normalised (normalizeTC), and the values are written
    /// back to the editors so what the user sees matches what gets saved.
    /// An untouched field is not normalised because presets are shared: at
    /// this window's rate it would re-clamp a value made for an engine at
    /// another one (a SAVE for the audio file, from a 25 fps engine, would
    /// move a 30 fps preset's Start TC from :29 to :24).
    GeneratorPreset readFormToPreset(const juce::String& name, GeneratorPreset p = {})
    {
        auto formTC = [this](const juce::TextEditor& ed, const juce::String& stored)
        {
            const juce::String typed = ed.getText().trim();
            if (typed.isNotEmpty() && typed == stored)
                return stored;
            return normalizeTC(typed.isEmpty() ? juce::String("00:00:00:00") : typed);
        };
        p.name    = name;
        p.startTC = formTC(edStartTC, p.startTC);
        p.stopTC  = formTC(edStopTC, p.stopTC);
        p.audioFilePath = edAudio.getText().trim();
        p.audioLoop     = btnLoopAudio.getToggleState();
        p.loopTimecode  = btnLoopTimecode.getToggleState();
        edStartTC.setText(p.startTC, false);
        edStopTC.setText(p.stopTC, false);
        return p;
    }

    /// Return `name` unchanged if no preset exists with that name; otherwise
    /// append " (2)", " (3)", ... until a free name is found.  Comparison
    /// is case-insensitive to match the underlying map's key handling.
    /// In the absurd edge case where 999 numbered duplicates already exist,
    /// fall back to a timestamp suffix rather than returning a name that
    /// would silently overwrite an existing preset.
    juce::String makeUniqueName(const juce::String& name) const
    {
        if (presetMap.find(name) == nullptr)
            return name;
        for (int i = 2; i < 1000; ++i)
        {
            auto candidate = name + " (" + juce::String(i) + ")";
            if (presetMap.find(candidate) == nullptr)
                return candidate;
        }
        // Pathological -- thousands of duplicates already exist.  Use a
        // timestamp so we still return a unique name; if even *that* exists
        // we just return it anyway, which is the best we can do without
        // throwing.
        return name + " (" + juce::String(juce::Time::currentTimeMillis()) + ")";
    }

    /// ADD always creates a new preset.  If the typed name is already in
    /// use, the new preset is auto-renamed "Name (2)", "Name (3)", etc.
    /// The form is repopulated so the user sees the actual name used.
    /// False, storing nothing, with no name.
    bool addNewPreset()
    {
        auto name = edName.getText().trim();
        if (name.isEmpty())
        {
            edName.grabKeyboardFocus();
            return false;
        }

        auto uniqueName = makeUniqueName(name);
        // ADD with a preset selected is how a copy is made ("Name (2)").
        // The Start/Stop TC it was loaded with go in as the stored text, so
        // fields the user did not touch are kept as they are, as SAVE keeps
        // them, instead of being re-clamped at this window's rate (a 30 fps
        // preset's :29 became :24 in a 25 fps window).  The cue points are
        // not copied: ADD creates a preset without cues, as before.
        GeneratorPreset seed;
        if (const auto* src = editingName.isNotEmpty() ? presetMap.find(editingName) : nullptr)
        {
            seed.startTC = src->startTC;
            seed.stopTC  = src->stopTC;
        }
        auto p = readFormToPreset(uniqueName, std::move(seed));
        // Reflect the actual stored name in the field whenever it differs
        // from what was typed -- either because we auto-numbered to avoid
        // a collision, or because the input had surrounding whitespace
        // that got trimmed.
        if (uniqueName != edName.getText())
            edName.setText(uniqueName, juce::dontSendNotification);

        // Close any open cue window.  Not for its reference: an
        // unordered_map rehash moves no element, so a new preset leaves it
        // valid.  The selection moves to the new preset below, and a cue
        // window left open would edit another one than the table shows.
        closeCueEditorWindow();
        presetMap.addOrUpdate(p);
        notifyChange();

        // Select the newly added row so SAVE / DELETE target it.
        for (int i = 0; i < (int)rows.size(); ++i)
        {
            if (rows[(size_t)i].name.equalsIgnoreCase(uniqueName))
            {
                table.selectRow(i);
                break;
            }
        }
        return true;
    }

    /// SAVE updates the preset the form was loaded from (editingName) in
    /// place.  The name field can be edited to rename the preset, but only
    /// if the new name is not already used by a *different* preset -- in
    /// that case we refuse and tell the user, so SAVE never silently
    /// overwrites someone else's preset.  When no row is selected, SAVE is
    /// a no-op (the button is disabled in that state, but we double-check
    /// defensively).  Stored, the preset goes to onPresetUpdated, so its
    /// two loop boxes reach an engine that has it loaded at once (#24).
    /// Returns whether it stored the preset.
    bool saveSelectedPreset()
    {
        auto name = edName.getText().trim();
        if (name.isEmpty())
        {
            edName.grabKeyboardFocus();
            return false;
        }

        // The preset the form shows, by name -- not rows[selected row],
        // which an Import can have moved -- and by value: notifyChange()
        // changes editingName.
        const juce::String oldName = editingName;
        if (oldName.isEmpty())
            return false;  // nothing selected; UI should already prevent this
        const bool nameChanged = ! oldName.equalsIgnoreCase(name);

        // If the user typed a name that belongs to a *different* preset,
        // refuse the rename rather than silently clobber it.  We're already
        // inside the nameChanged branch, where the canonical keys (lower-
        // cased + trimmed) of old and new differ -- so any non-null find()
        // is necessarily a different preset.  A case-only rename
        // ("Foo" -> "FOO") sets nameChanged to false via equalsIgnoreCase
        // and skips this branch entirely, hitting addOrUpdate with the same
        // key and updating the entry's display name in place.
        if (nameChanged && presetMap.find(name) != nullptr)
        {
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon,
                "Name in use",
                "A preset called \"" + name + "\" already exists. "
                "Choose a different name, or select that preset and Save to overwrite it.",
                "OK");
            edName.grabKeyboardFocus();
            return false;
        }

        // Start from the stored preset, copied before the map is touched:
        // the form holds name, Start/Stop TC and the audio file, and the
        // rest -- the cue points -- must survive SAVE and rename.  Built
        // from a blank preset, SAVE deleted every cue of the preset
        // without a word (AUDIT SET-2).
        GeneratorPreset stored;
        if (const auto* live = presetMap.find(oldName))
            stored = *live;

        // Close the cue window before the map changes: it edits the stored
        // preset through a reference, which a rename's remove() destroys
        // and SAVE's addOrUpdate() overwrites.  Re-opening Cues from the
        // table picks up the saved entry.
        closeCueEditorWindow();

        if (nameChanged)
            presetMap.remove(oldName);

        auto p = readFormToPreset(name, std::move(stored));
        // Mirror the trimmed name back if the user's input had surrounding
        // whitespace (matches what addNewPreset does).
        if (name != edName.getText())
            edName.setText(name, juce::dontSendNotification);
        presetMap.addOrUpdate(p);
        // The form now edits the preset under its (possibly new) name;
        // notifyChange() keeps it selected, so a subsequent SAVE works
        // without clicking the row again.  A rename can move it in the
        // name-sorted list: bring it into view.
        editingName = name;
        notifyChange();
        if (const int row = rowOfPreset(name); row >= 0)
            table.scrollToEnsureRowIsOnscreen(row);
        if (onPresetUpdated) onPresetUpdated(p);
        return true;
    }

    void deleteSelected()
    {
        int sel = table.getSelectedRow();
        if (sel < 0 || sel >= (int)rows.size()) return;

        // Close cue editor: it may be holding a reference into the very
        // entry we're about to remove from the map.
        closeCueEditorWindow();

        presetMap.remove(rows[(size_t)sel].name);
        notifyChange();   // the deleted preset was the form's: nothing stays selected

        // Clear form
        edName.clear();
        edStartTC.setText("00:00:00:00", false);
        edStopTC.setText("00:00:00:00", false);
        edAudio.clear();
        btnLoopAudio.setToggleState(false, juce::dontSendNotification);
        btnLoopTimecode.setToggleState(false, juce::dontSendNotification);
        updateButtonStates();
    }

    /// Close any open cue editor window.  Called before any operation
    /// that mutates the preset map (add, save/rename, delete, clearAll,
    /// import).  The window holds a reference into the map's
    /// unordered_map: remove() and clear() destroy the preset it points to
    /// (so does GeneratorPresetMap::load(), which AppSettings::load() runs
    /// on a configuration restore -- MainComponent must close this editor
    /// first), and addOrUpdate() of the same name replaces its contents.
    /// A rehash moves no element and leaves the reference valid.
    void closeCueEditorWindow()
    {
        if (cueWindow != nullptr)
        {
            cueWindow->setVisible(false);
            cueWindow.reset();
        }
    }

    /// Open the cue-point editor for the selected preset.
    /// The window is constructed against a *pointer* into the live
    /// GeneratorPresetMap (not against rows[], which is a sorted snapshot
    /// that gets rebuilt on every notifyChange).  That way edits made in
    /// the cue window are persisted on the actual preset object.
    void openCueEditor()
    {
        int sel = table.getSelectedRow();
        if (sel < 0 || sel >= (int)rows.size()) return;

        const auto presetName = rows[(size_t)sel].name;
        auto* livePreset = presetMap.find(presetName);
        if (livePreset == nullptr) return;  // shouldn't happen, defensive

        // Close any previous window first (it may be holding a reference
        // into a different preset, or even a stale one if the user just
        // renamed something).
        closeCueEditorWindow();

        cueWindow = std::make_unique<GeneratorCuePointEditorWindow>(
            *livePreset, currentFpsForCueEditor);
        if (auto* ed = cueWindow->getEditor())
        {
            ed->onChange = [this]
            {
                // Persist + refresh the preset table.  rebuildRows only
                // *reads* the map (copies entries into the rows snapshot),
                // it doesn't mutate the underlying unordered_map, so the
                // cue window's pointer into the map remains valid.
                presetMap.save();
                rebuildRows();
                table.updateContent();
                table.repaint();
                if (onChange) onChange();
            };
            // If the host installed a playhead getter (MainComponent did,
            // bound to its engine), forward the same getter to the cue
            // editor so it can drive the live playback cursor in the
            // waveform strip.  When no getter is set the cue editor just
            // doesn't show a playhead -- everything else still works.
            if (playheadGetter)
                ed->setPlayheadGetter(playheadGetter);
        }
        cueWindow->setVisible(true);
        cueWindow->toFront(true);
    }

    void clearAll()
    {
        if (rows.empty()) return;

        auto options = juce::MessageBoxOptions()
            .withIconType(juce::MessageBoxIconType::WarningIcon)
            .withTitle("Clear All Presets")
            .withMessage("Remove all generator presets? This cannot be undone.")
            .withButton("Clear All")
            .withButton("Cancel");

        confirmBox = juce::AlertWindow::showScopedAsync(options,
            [this](int result)
            {
                if (result == 1)
                {
                    closeCueEditorWindow();
                    presetMap.clear();
                    notifyChange();
                    edName.clear();
                    edStartTC.setText("00:00:00:00", false);
                    edStopTC.setText("00:00:00:00", false);
                    edAudio.clear();
                    btnLoopAudio.setToggleState(false, juce::dontSendNotification);
                    btnLoopTimecode.setToggleState(false, juce::dontSendNotification);
                }
            });
    }

    juce::ScopedMessageBox confirmBox;
    std::unique_ptr<ClosePrompt> closePrompt;   // askToSaveBeforeClose, while it asks

    void browseForAudio()
    {
        // Build a wildcard list of supported audio extensions.  We do not
        // construct an AudioFormatManager here just to query extensions; the
        // file chooser will accept anything matching this pattern and the
        // engine will validate when loading.
        juce::String wildcards = "*.wav;*.aif;*.aiff;*.flac;*.ogg";
       #if JUCE_USE_MP3AUDIOFORMAT
        wildcards += ";*.mp3";
       #endif

        // Pick a sensible starting directory: the existing file's folder if
        // it still exists, otherwise the user's Music folder.  Defends
        // against imported presets pointing to paths that no longer exist.
        juce::File initial;
        auto path = edAudio.getText().trim();
        if (path.isNotEmpty())
        {
            juce::File f(path);
            if (f.existsAsFile() || f.getParentDirectory().exists())
                initial = f.getParentDirectory();
        }
        if (initial == juce::File())
            initial = juce::File::getSpecialLocation(juce::File::userMusicDirectory);

        fileChooser = std::make_unique<juce::FileChooser>(
            "Select audio file for preset",
            initial,
            wildcards);

        fileChooser->launchAsync(
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [this](const juce::FileChooser& fc)
            {
                auto file = fc.getResult();
                if (file == juce::File() || ! file.existsAsFile()) return;
                edAudio.setText(file.getFullPathName(), false);
                updateStopTCEnabledState();
            });
    }

    /// Clearing the audio file unticks "Loop audio only", which needs one;
    /// "Loop timecode" stays as it is, as it also loops at a Stop TC
    /// without a file.
    void clearAudioField()
    {
        edAudio.clear();
        btnLoopAudio.setToggleState(false, juce::dontSendNotification);
        updateStopTCEnabledState();
    }

    void exportPresets()
    {
        if (rows.empty()) return;

        fileChooser = std::make_unique<juce::FileChooser>(
            "Export Generator Presets",
            juce::File::getSpecialLocation(juce::File::userDesktopDirectory)
                .getChildFile("generator_presets.json"),
            "*.json");

        fileChooser->launchAsync(
            juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
            [this](const juce::FileChooser& fc)
            {
                auto file = fc.getResult();
                if (file == juce::File()) return;

                auto* root = new juce::DynamicObject();
                root->setProperty("version", 1);
                juce::Array<juce::var> arr;
                for (auto& p : rows)
                    arr.add(p.toVar());
                root->setProperty("presets", arr);
                file.replaceWithText(juce::JSON::toString(juce::var(root)));
            });
    }

    void importPresets()
    {
        fileChooser = std::make_unique<juce::FileChooser>(
            "Import Generator Presets",
            juce::File::getSpecialLocation(juce::File::userDesktopDirectory),
            "*.json");

        fileChooser->launchAsync(
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [this](const juce::FileChooser& fc)
            {
                auto file = fc.getResult();
                if (file == juce::File() || !file.existsAsFile()) return;

                auto parsed = juce::JSON::parse(file.loadFileAsString());
                auto* obj = parsed.getDynamicObject();
                if (!obj || !obj->hasProperty("presets"))
                {
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::MessageBoxIconType::WarningIcon,
                        "Import Failed",
                        "This file is not a valid generator presets file.");
                    return;
                }

                auto* arr = obj->getProperty("presets").getArray();
                if (!arr) return;

                // Close any cue window: an imported preset with the name of
                // the one it edits replaces that preset's contents (a
                // rehash alone would leave its reference valid).
                closeCueEditorWindow();

                int imported = 0;
                for (auto& item : *arr)
                {
                    GeneratorPreset p;
                    p.fromVar(item);
                    if (p.hasValidKey())
                    {
                        presetMap.addOrUpdate(p);
                        ++imported;
                    }
                }

                if (imported > 0)
                    notifyChange();
            });
    }

    void showOscReference()
    {
        juce::String help;
        help << "OSC COMMAND REFERENCE\n";
        help << "==============================================\n\n";
        help << "Default port: 9800  (configurable in Generator panel)\n\n";
        help << "ENGINE TARGETING (N = engine number, required)\n\n";
        help << "TRANSPORT\n";
        help << "  /stc/N/gen/play\n";
        help << "  /stc/N/gen/pause\n";
        help << "  /stc/N/gen/stop\n";
        help << "  /stc/N/gen/jog (float)    move by seconds (- = back)\n\n";
        help << "MODE\n";
        help << "  /stc/N/gen/clock (int)    0=transport, 1=clock\n";
        help << "  /stc/N/gen/looptc (int)   0=off, 1=Loop timecode (until the\n";
        help << "                            next GO / preset load, or a Save of\n";
        help << "                            the preset here; off stops at the end)\n\n";
        help << "TIMECODE\n";
        help << "  /stc/N/gen/start (string)     \"HH:MM:SS:FF\"\n";
        help << "  /stc/N/gen/stoptime (string)  \"HH:MM:SS:FF\"\n\n";
        help << "PRESETS\n";
        help << "  /stc/N/gen/preset (string)    preset name (loads + plays)\n\n";
        help << "N = 1 to 8 (engine number)\n\n";
        help << "EXAMPLES\n\n";
        help << "QLab (Network Cue, type OSC):\n";
        help << "  Destination: <STC IP>:9800\n";
        help << "  /stc/1/gen/play\n";
        help << "  /stc/1/gen/stop\n";
        help << "  /stc/1/gen/start 01:30:00:00\n";
        help << "  /stc/1/gen/stoptime 02:00:00:00\n";
        help << "  /stc/1/gen/preset MyPreset\n\n";
        help << "Resolume / Companion / TouchOSC:\n";
        help << "  Host: <STC IP>, Port: 9800\n";
        help << "  Same address patterns as above\n\n";
        help << "TIMECODE FORMAT\n";
        help << "  String argument: \"HH:MM:SS:FF\"\n";
        help << "  00:00:00:00 = midnight\n";
        help << "  01:30:00:00 = 1 hour 30 minutes\n";
        help << "  00:05:30:15 = 5 min, 30 sec, frame 15";

        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::InfoIcon,
            "OSC Command Reference",
            help);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GeneratorPresetEditor)
};
