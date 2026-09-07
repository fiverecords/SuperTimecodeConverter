// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter
//
// GeneratorCuePointEditor -- editor for the cue points attached to a
// GeneratorPreset.  Modelled after CuePointEditor (used for TrackMap
// entries) but with two key differences:
//
//   1. Cue position is stored as an absolute SMPTE TC string (HH:MM:SS:FF)
//      rather than as ms-from-track-start, because the Generator runs in
//      TC space (Start TC + Stop TC) regardless of whether it has an audio
//      file attached.  A cue at "01:00:30:00" fires when the generated TC
//      reaches that mark.
//   2. There is no waveform strip / playhead capture.  The audio in a
//      preset is optional and the cue position is independent of the
//      audio anyway -- it's tied to the generated TC.
//
// The class is intentionally a separate copy rather than a refactor of
// CuePointEditor: the TrackMap one is field-tested, and reusing it would
// drag in the waveform strip and ms-based position semantics that don't
// apply here.
#pragma once
#include <JuceHeader.h>
#include "AppSettings.h"

//==============================================================================
// GeneratorAudioWaveformStrip -- horizontal mono waveform of the preset's
// attached audio file, with cue and edit-cursor overlays.  Built on top of
// JUCE's AudioThumbnail so we don't have to load the file ourselves; the
// editor reuses the thumbnail that GeneratorAudioPlayer already maintains
// for the engine's runtime use.
//
// Coordinate semantics:
//   - X axis is "ms from the start of the audio file" (0..audioLengthMs).
//   - A Generator cue's TC is absolute (HH:MM:SS:FF since midnight).  The
//     host editor converts cue.positionMs - preset.startTC.ms to get the
//     value to plot on this strip.  Cues that fall outside [0,audioLength]
//     are simply not drawn (they'd point off the strip), but they still
//     fire correctly at runtime -- this is just a visual.
//   - Click position returns ms-from-audio-start, which the editor then
//     converts back to a TC string for the position field.
//==============================================================================
class GeneratorAudioWaveformStrip : public juce::Component,
                                    private juce::ChangeListener
{
public:
    GeneratorAudioWaveformStrip()
    {
        setOpaque(false);
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
    }

    ~GeneratorAudioWaveformStrip() override
    {
        if (thumbnail != nullptr)
            thumbnail->removeChangeListener(this);
    }

    /// Bind to a thumbnail owned elsewhere (typically the preset's
    /// GeneratorAudioPlayer thumbnail).  Pass nullptr to detach.
    void setThumbnail(juce::AudioThumbnail* tn)
    {
        if (thumbnail == tn) return;
        if (thumbnail != nullptr)
            thumbnail->removeChangeListener(this);
        thumbnail = tn;
        if (thumbnail != nullptr)
            thumbnail->addChangeListener(this);
        repaint();
    }

    void setAudioLengthMs(uint32_t ms)   { audioLengthMs = ms; repaint(); }
    void setEditCursorMs(uint32_t ms)    { editCursorMs  = ms; editCursorSet = true; repaint(); }
    void clearEditCursor()                { editCursorSet = false; repaint(); }
    bool hasEditCursor() const           { return editCursorSet; }
    uint32_t getEditCursorMs() const     { return editCursorMs; }

    /// Set the live playback cursor position (ms-from-audio-start).
    /// Pass < 0 to hide the playhead (e.g. when not playing).  This is a
    /// separate visual from the edit cursor: edit cursor is yellow and
    /// only moves on click/drag; playhead is bright red and follows the
    /// engine while audio is playing.
    void setPlayheadMs(int64_t ms)
    {
        playheadMs = ms;
        repaint();
    }

    /// Cue positions in ms-from-audio-start.  Pass values clamped /
    /// pre-translated by the host; this strip doesn't know about TC.
    void setCuePositions(const std::vector<uint32_t>& positions)
    {
        cuePositions = positions;
        repaint();
    }

    void setSelectedCueIndex(int idx) { selectedCue = idx; repaint(); }

    /// Click handler: notifies the host with ms-from-audio-start.  The
    /// host converts that back to a TC string and writes it to the
    /// position editor.
    std::function<void(uint32_t ms)> onClickAtMs;

    void paint(juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        g.setColour(juce::Colour(0xFF0D1117));
        g.fillRoundedRectangle(b, 3.0f);

        if (thumbnail == nullptr || audioLengthMs == 0)
        {
            g.setColour(juce::Colour(0xFF666666));
            g.setFont(juce::Font(juce::FontOptions(11.0f)));
            g.drawText("No audio attached -- positions are TC only",
                       b.toNearestInt(), juce::Justification::centred, true);
            return;
        }

        auto inset = b.reduced(2.0f).toNearestInt();
        if (thumbnail->getNumChannels() > 0
            && thumbnail->getTotalLength() > 0.0)
        {
            // Mirror-symmetric paint, matching GeneratorWaveformView: top
            // half shows channel 0 (left or mono), bottom half shows
            // channel 1 inverted (right).  Trick: each drawChannel() call
            // is given the FULL waveArea as geometry so the bipolar peaks
            // are centred on the shared baseline; we then clip to that
            // channel's half so only "its side" of the bipolar shape is
            // visible.  Looks like a typical DAW waveform.
            g.setColour(juce::Colour(0xFF3A82C8));
            const double totalLen = thumbnail->getTotalLength();
            const int    centreY  = inset.getCentreY();
            const auto   topHalf  = inset.withBottom(centreY);

            if (thumbnail->getNumChannels() > 1)
            {
                const auto bottomHalf = inset.withTop(centreY);

                g.saveState();
                g.reduceClipRegion(topHalf);
                thumbnail->drawChannel(g, inset, 0.0, totalLen, 0, 1.0f);
                g.restoreState();

                g.saveState();
                g.reduceClipRegion(bottomHalf);
                thumbnail->drawChannel(g, inset, 0.0, totalLen, 1, 1.0f);
                g.restoreState();
            }
            else
            {
                // Mono: same channel mirrored top + bottom.  drawChannel
                // already produces a bipolar shape centred on the rect's
                // midline, so a single full-height draw gives the right
                // visual.
                thumbnail->drawChannel(g, inset, 0.0, totalLen, 0, 1.0f);
            }
        }
        else
        {
            g.setColour(juce::Colour(0xFF666666));
            g.setFont(juce::Font(juce::FontOptions(11.0f)));
            g.drawText("(loading waveform...)", inset,
                       juce::Justification::centred, true);
        }

        // Cue markers: vertical lines at each cue position.  Selected cue
        // is highlighted in amber, others in dim white.
        const float w = (float) inset.getWidth();
        const float h = (float) inset.getHeight();
        const float originX = (float) inset.getX();
        const float originY = (float) inset.getY();

        for (size_t i = 0; i < cuePositions.size(); ++i)
        {
            const uint32_t ms = cuePositions[i];
            if (ms > audioLengthMs) continue;
            float x = originX + ((float)ms / (float)audioLengthMs) * w;
            const bool isSelected = ((int)i == selectedCue);
            g.setColour(isSelected ? juce::Colour(0xFFFFCC44)
                                   : juce::Colour(0xCCCCCCCC));
            g.fillRect(x - 0.5f, originY, isSelected ? 2.0f : 1.0f, h);
            g.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
            g.drawText(juce::String((int)i + 1), juce::Rectangle<float>(x + 2.0f, originY + 1.0f, 14.0f, 12.0f),
                       juce::Justification::centredLeft, false);
        }

        // Edit cursor: thin yellow vertical line where the user last clicked.
        if (editCursorSet && editCursorMs <= audioLengthMs)
        {
            float x = originX + ((float)editCursorMs / (float)audioLengthMs) * w;
            g.setColour(juce::Colour(0xFFFFFF44));
            g.fillRect(x - 0.5f, originY, 1.0f, h);
        }

        // Live playback cursor: bright red, drawn on top of everything else
        // so it remains visible against cue markers and the edit cursor.
        // Suppressed when negative (engine not playing or no preset loaded).
        if (playheadMs >= 0 && (uint32_t) playheadMs <= audioLengthMs)
        {
            float x = originX + ((float)playheadMs / (float)audioLengthMs) * w;
            g.setColour(juce::Colour(0xFFFF3B30));
            g.fillRect(x - 1.0f, originY, 2.0f, h);
        }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        updateCursorFromMouseX((float) e.position.x);
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        // Drag = continuous fine-tuning of the cue position.  The strip
        // notifies the host on every drag step so the position field in
        // the detail panel updates live; pressing Add at the end commits
        // the cue at whatever the cursor ended on.
        updateCursorFromMouseX((float) e.position.x);
    }

private:
    /// Compute and apply the edit-cursor position for a given mouse X
    /// coordinate (in this component's local space).  Shared by mouseDown
    /// and mouseDrag so click-to-set and drag-to-fine-tune behave
    /// identically.
    void updateCursorFromMouseX(float mouseX)
    {
        if (audioLengthMs == 0) return;
        auto inset = getLocalBounds().toFloat().reduced(2.0f);
        const float w = inset.getWidth();
        if (w <= 0.0f) return;
        const float relX = mouseX - inset.getX();
        const float frac = juce::jlimit(0.0f, 1.0f, relX / w);
        editCursorMs = (uint32_t) ((double)audioLengthMs * frac);
        editCursorSet = true;
        repaint();
        if (onClickAtMs) onClickAtMs(editCursorMs);
    }

    void changeListenerCallback(juce::ChangeBroadcaster*) override
    {
        repaint();  // thumbnail finished loading another chunk
    }

    juce::AudioThumbnail* thumbnail = nullptr;
    uint32_t audioLengthMs = 0;
    std::vector<uint32_t> cuePositions;
    int  selectedCue = -1;
    uint32_t editCursorMs = 0;
    bool editCursorSet = false;
    int64_t playheadMs = -1;  // -1 = not playing / hide

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GeneratorAudioWaveformStrip)
};

class GeneratorCuePointEditor : public juce::Component,
                                public juce::TableListBoxModel,
                                private juce::Timer
{
public:
    /// Construct the editor.
    /// `fpsForTcConversion` is used to convert between TC strings and
    /// absolute ms when projecting cues onto the audio strip; the engine's
    /// current fps is the right value to pass here.  If the preset has an
    /// audio file attached, the editor loads its own AudioThumbnail from
    /// that file (independent of the engine's runtime player) so the
    /// waveform shown always matches the preset being edited, regardless
    /// of what the engine happens to be playing.
    GeneratorCuePointEditor(GeneratorPreset& presetRef,
                            double fpsForTcConversion = 30.0)
        : preset(presetRef),
          fps(fpsForTcConversion <= 0.0 ? 30.0 : fpsForTcConversion),
          ownThumbnailCache(8),
          ownThumbnail(512, ownFormatManager, ownThumbnailCache)
    {
        ownFormatManager.registerBasicFormats();
        loadOwnAudio();
        setSize(680, 540);  // taller than before to accommodate the strip

        bgPanel     = juce::Colour(0xFF1A1D23);
        bgRow       = juce::Colour(0xFF22252B);
        bgRowAlt    = juce::Colour(0xFF1E2128);
        textLight   = juce::Colour(0xFFE0E0E0);
        textDim     = juce::Colour(0xFF888888);
        accentBlue  = juce::Colour(0xFF00AAFF);
        accentGreen = juce::Colour(0xFF44CC44);
        accentRed   = juce::Colour(0xFFFF4444);

        // --- Header ---
        addAndMakeVisible(lblTitle);
        lblTitle.setText("Cue points -- " + preset.name, juce::dontSendNotification);
        lblTitle.setColour(juce::Label::textColourId, textLight);
        lblTitle.setFont(juce::Font(juce::FontOptions(14.0f).withStyle("Bold")));

        addAndMakeVisible(lblCount);
        lblCount.setColour(juce::Label::textColourId, textDim);
        lblCount.setFont(juce::Font(juce::FontOptions(11.0f)));

        // --- Waveform strip ---
        // Visible always; when there is no audio, the strip itself shows a
        // "No audio attached" message rather than disappearing -- that way
        // the editor layout doesn't jump when the user attaches / detaches
        // audio in the parent preset editor.
        addAndMakeVisible(strip);
        strip.setThumbnail(&ownThumbnail);
        strip.setAudioLengthMs(audioLengthMs);
        strip.onClickAtMs = [this](uint32_t msFromAudioStart)
        {
            // Convert ms-from-audio-start to absolute SMPTE TC by adding the
            // preset's startTC offset, then write that into the position
            // editor.  We don't auto-apply / auto-save the cue here because
            // the user might be in the middle of editing other fields; the
            // cursor click just sets a candidate position the user then
            // confirms by hitting Apply.
            const uint32_t absMs = preset.startTC.isNotEmpty()
                ? msFromAudioStart + (uint32_t) parseTcToMs(preset.startTC, fps)
                : msFromAudioStart;
            edPosition.setText(msToTc(absMs, fps), false);
        };
        refreshStripCues();

        // --- Table ---
        addAndMakeVisible(table);
        table.setModel(this);
        table.setMultipleSelectionEnabled(true);
        table.setColour(juce::ListBox::backgroundColourId, bgPanel);
        table.setColour(juce::ListBox::outlineColourId, juce::Colour(0xFF333333));
        table.setOutlineThickness(1);
        table.setRowHeight(26);
        table.getHeader().setStretchToFitActive(true);

        auto& hdr = table.getHeader();
        hdr.addColumn("#",        ColNum,       32,  28,  40, juce::TableHeaderComponent::notSortable);
        hdr.addColumn("Position", ColPosition, 110,  90, 140, juce::TableHeaderComponent::notSortable);
        hdr.addColumn("Name",     ColName,     140,  80, 300, juce::TableHeaderComponent::notSortable);
        hdr.addColumn("MIDI",     ColMidi,     100,  60, 160, juce::TableHeaderComponent::notSortable);
        hdr.addColumn("OSC",      ColOsc,      110,  60, 200, juce::TableHeaderComponent::notSortable);
        hdr.addColumn("DMX",      ColDmx,       60,  40,  90, juce::TableHeaderComponent::notSortable);

        // --- Buttons ---
        auto setupBtn = [&](juce::TextButton& btn, const juce::String& text,
                            juce::Colour bgCol, juce::Colour txtCol)
        {
            addAndMakeVisible(btn);
            btn.setButtonText(text);
            btn.setColour(juce::TextButton::buttonColourId, bgCol);
            btn.setColour(juce::TextButton::textColourOffId, txtCol);
        };

        setupBtn(btnAdd,    "Add",    accentGreen.withAlpha(0.2f), accentGreen);
        setupBtn(btnDelete, "Delete", accentRed.withAlpha(0.2f),   accentRed);

        btnAdd.setTooltip("Add a new cue point.  Defaults to the preset's Start TC.");
        btnDelete.setTooltip("Delete the selected cue point(s).");

        btnAdd.onClick = [this]
        {
            GeneratorCuePoint cp;
            // Position priority:
            //   1. If the user has clicked on the waveform strip, use that
            //      cursor position (translated from ms-from-audio-start to
            //      absolute TC by adding startTC).
            //   2. Otherwise default to startTC, so a brand-new cue still
            //      lands at a sensible spot.
            if (strip.hasEditCursor() && audioLengthMs > 0)
            {
                const double startMs = preset.startTC.isNotEmpty()
                    ? parseTcToMs(preset.startTC, fps) : 0.0;
                const double absMs = startMs + (double) strip.getEditCursorMs();
                cp.positionTC = msToTc(absMs, fps);
            }
            else
            {
                cp.positionTC = preset.startTC.isNotEmpty()
                    ? preset.startTC : juce::String("00:00:00:00");
            }
            cp.name = "CUE " + juce::String(preset.cuePoints.size() + 1);
            preset.cuePoints.push_back(std::move(cp));
            preset.sortCuePoints();
            table.updateContent();
            table.repaint();
            updateCount();
            refreshStripCues();
            // Clear the cursor after consuming it so the next ADD doesn't
            // silently reuse the same position; the user re-clicks if they
            // want another cue at a specific spot.
            strip.clearEditCursor();
            if (onChange) onChange();
        };

        btnDelete.onClick = [this]
        {
            auto selected = table.getSelectedRows();
            if (selected.isEmpty()) return;

            std::vector<int> indices;
            for (int i = 0; i < selected.size(); ++i)
                indices.push_back(selected[i]);
            std::sort(indices.rbegin(), indices.rend());

            for (int idx : indices)
            {
                if (idx >= 0 && idx < (int) preset.cuePoints.size())
                    preset.cuePoints.erase(preset.cuePoints.begin() + idx);
            }

            table.deselectAllRows();
            table.updateContent();
            table.repaint();
            updateCount();
            refreshStripCues();
            selectedCue = -1;
            strip.setSelectedCueIndex(-1);
            detailPanel.setVisible(false);
            resized();
            if (onChange) onChange();
        };

        // --- Detail panel ---
        addAndMakeVisible(detailPanel);
        detailPanel.setVisible(false);

        auto addLbl = [&](juce::Label& lbl, const juce::String& text)
        {
            detailPanel.addAndMakeVisible(lbl);
            lbl.setText(text, juce::dontSendNotification);
            lbl.setColour(juce::Label::textColourId, textDim);
            lbl.setFont(juce::Font(juce::FontOptions(10.0f)));
        };
        auto addEd = [&](juce::TextEditor& ed)
        {
            detailPanel.addAndMakeVisible(ed);
            ed.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xFF2A2A2A));
            ed.setColour(juce::TextEditor::textColourId, textLight);
            ed.setColour(juce::TextEditor::outlineColourId, juce::Colour(0xFF444444));
            ed.setFont(juce::Font(juce::FontOptions(11.0f)));
        };

        addLbl(lblPosition, "POSITION (HH:MM:SS:FF):");
        addEd(edPosition);
        addLbl(lblName, "NAME:");
        addEd(edName);

        addLbl(lblMidiCh,    "MIDI CH:");   addEd(edMidiCh);
        addLbl(lblMidiNote,  "NOTE:");      addEd(edMidiNote);
        addLbl(lblMidiVel,   "VEL:");       addEd(edMidiVel);
        addLbl(lblMidiCC,    "CC#:");       addEd(edMidiCC);
        addLbl(lblMidiCCVal, "CC VAL:");    addEd(edMidiCCVal);

        addLbl(lblOscAddr, "OSC ADDRESS:"); addEd(edOscAddr);
        addLbl(lblOscArgs, "OSC ARGS:");    addEd(edOscArgs);

        addLbl(lblDmxCh,  "DMX CH:");       addEd(edDmxCh);
        addLbl(lblDmxVal, "DMX VAL:");      addEd(edDmxVal);

        setupBtn(btnApply, "Apply", accentBlue.withAlpha(0.2f), accentBlue);
        detailPanel.addAndMakeVisible(btnApply);
        btnApply.onClick = [this] { applyDetailEdits(); };

        updateCount();
    }

    /// Called when cue points are modified (add/delete/edit) so the host
    /// can persist the preset map to disk.
    std::function<void()> onChange;

    /// Optional getter the host can install to drive the live playback
    /// cursor.  Returns the current generator playhead in
    /// ms-from-audio-start (i.e. already corrected for the preset's
    /// startTC), or a negative value when nothing is playing.  When set,
    /// the editor polls it at ~30Hz and forwards the value to the strip.
    void setPlayheadGetter(std::function<int64_t()> getter)
    {
        playheadGetter = std::move(getter);
        if (playheadGetter)
            startTimerHz(30);
        else
        {
            stopTimer();
            strip.setPlayheadMs(-1);
        }
    }

    //--------------------------------------------------------------------------
    // TableListBoxModel
    //--------------------------------------------------------------------------
    int getNumRows() override { return (int) preset.cuePoints.size(); }

    void paintRowBackground(juce::Graphics& g, int rowNumber,
                            int /*width*/, int /*height*/, bool isSelected) override
    {
        g.fillAll(isSelected ? accentBlue.withAlpha(0.15f)
                             : ((rowNumber % 2 == 0) ? bgRow : bgRowAlt));
    }

    void paintCell(juce::Graphics& g, int rowNumber, int columnId,
                   int width, int height, bool /*isSelected*/) override
    {
        if (rowNumber < 0 || rowNumber >= (int) preset.cuePoints.size()) return;
        auto& cue = preset.cuePoints[(size_t) rowNumber];

        g.setColour(textLight);
        g.setFont(juce::Font(juce::FontOptions(12.0f)));

        auto cellArea = juce::Rectangle<int>(4, 0, width - 8, height);

        switch (columnId)
        {
            case ColNum:
                g.drawText(juce::String(rowNumber + 1), cellArea,
                           juce::Justification::centredLeft, true);
                break;
            case ColPosition:
                g.drawText(cue.positionTC, cellArea,
                           juce::Justification::centredLeft, true);
                break;
            case ColName:
                g.drawText(cue.name, cellArea,
                           juce::Justification::centredLeft, true);
                break;
            case ColMidi:
                if (cue.hasMidiTrigger())
                {
                    juce::String txt;
                    if (cue.midiNoteNum >= 0)
                        txt = "N" + juce::String(cue.midiNoteNum) + "v" + juce::String(cue.midiNoteVel);
                    if (cue.midiCCNum >= 0)
                    {
                        if (txt.isNotEmpty()) txt += " ";
                        txt += "CC" + juce::String(cue.midiCCNum) + "=" + juce::String(cue.midiCCVal);
                    }
                    g.drawText(txt + " ch" + juce::String(cue.midiChannel + 1),
                               cellArea, juce::Justification::centredLeft, true);
                }
                else
                {
                    g.setColour(textDim);
                    g.drawText("--", cellArea, juce::Justification::centredLeft, true);
                }
                break;
            case ColOsc:
                if (cue.hasOscTrigger())
                    g.drawText(cue.oscAddress, cellArea,
                               juce::Justification::centredLeft, true);
                else
                {
                    g.setColour(textDim);
                    g.drawText("--", cellArea, juce::Justification::centredLeft, true);
                }
                break;
            case ColDmx:
                if (cue.hasArtnetTrigger())
                    g.drawText("ch" + juce::String(cue.artnetCh) + "=" + juce::String(cue.artnetVal),
                               cellArea, juce::Justification::centredLeft, true);
                else
                {
                    g.setColour(textDim);
                    g.drawText("--", cellArea, juce::Justification::centredLeft, true);
                }
                break;
            default:
                break;
        }
    }

    void selectedRowsChanged(int lastRowSelected) override
    {
        selectedCue = lastRowSelected;
        strip.setSelectedCueIndex(selectedCue);
        if (selectedCue < 0 || selectedCue >= (int) preset.cuePoints.size())
        {
            detailPanel.setVisible(false);
            resized();
            return;
        }
        loadDetailFromCue(preset.cuePoints[(size_t) selectedCue]);
        detailPanel.setVisible(true);
        resized();
    }

    //--------------------------------------------------------------------------
    // Layout
    //--------------------------------------------------------------------------
    void resized() override
    {
        auto area = getLocalBounds().reduced(8);

        // Header row
        auto headerRow = area.removeFromTop(20);
        lblTitle.setBounds(headerRow.removeFromLeft(headerRow.getWidth() - 120));
        lblCount.setBounds(headerRow);
        area.removeFromTop(6);

        // Waveform strip (fixed height; placed above the buttons so it's
        // always visible even when the detail panel is open at the bottom)
        strip.setBounds(area.removeFromTop(60));
        area.removeFromTop(6);

        // Buttons row
        auto buttonsRow = area.removeFromTop(28);
        btnAdd.setBounds(buttonsRow.removeFromLeft(70));
        buttonsRow.removeFromLeft(6);
        btnDelete.setBounds(buttonsRow.removeFromLeft(70));
        area.removeFromTop(6);

        // If detail panel visible, take its space from the bottom
        if (detailPanel.isVisible())
        {
            const int detailHeight = 220;
            detailPanel.setBounds(area.removeFromBottom(detailHeight));
            area.removeFromBottom(6);

            // Layout inside detail panel
            auto dp = detailPanel.getLocalBounds().reduced(8);
            auto row1 = dp.removeFromTop(22);
            lblPosition.setBounds(row1.removeFromLeft(160));
            edPosition.setBounds(row1.removeFromLeft(120));
            row1.removeFromLeft(10);
            lblName.setBounds(row1.removeFromLeft(60));
            edName.setBounds(row1);
            dp.removeFromTop(6);

            auto row2 = dp.removeFromTop(22);
            lblMidiCh.setBounds(row2.removeFromLeft(60));    edMidiCh.setBounds(row2.removeFromLeft(40));
            row2.removeFromLeft(10);
            lblMidiNote.setBounds(row2.removeFromLeft(50));  edMidiNote.setBounds(row2.removeFromLeft(40));
            row2.removeFromLeft(10);
            lblMidiVel.setBounds(row2.removeFromLeft(40));   edMidiVel.setBounds(row2.removeFromLeft(40));
            row2.removeFromLeft(10);
            lblMidiCC.setBounds(row2.removeFromLeft(40));    edMidiCC.setBounds(row2.removeFromLeft(40));
            row2.removeFromLeft(10);
            lblMidiCCVal.setBounds(row2.removeFromLeft(60)); edMidiCCVal.setBounds(row2.removeFromLeft(40));
            dp.removeFromTop(6);

            auto row3 = dp.removeFromTop(22);
            lblOscAddr.setBounds(row3.removeFromLeft(110)); edOscAddr.setBounds(row3.removeFromLeft(220));
            row3.removeFromLeft(10);
            lblOscArgs.setBounds(row3.removeFromLeft(80));  edOscArgs.setBounds(row3);
            dp.removeFromTop(6);

            auto row4 = dp.removeFromTop(22);
            lblDmxCh.setBounds(row4.removeFromLeft(60));   edDmxCh.setBounds(row4.removeFromLeft(50));
            row4.removeFromLeft(10);
            lblDmxVal.setBounds(row4.removeFromLeft(60));  edDmxVal.setBounds(row4.removeFromLeft(50));
            dp.removeFromTop(10);

            auto applyRow = dp.removeFromTop(28);
            btnApply.setBounds(applyRow.removeFromRight(80));
        }

        table.setBounds(area);
    }

private:
    enum ColumnId { ColNum = 1, ColPosition, ColName, ColMidi, ColOsc, ColDmx };

    void updateCount()
    {
        lblCount.setText(juce::String(preset.cuePoints.size()) + " cue points",
                         juce::dontSendNotification);
    }

    /// Normalise a TC string by carrying overflow.  Mirrors the helper in
    /// GeneratorPresetEditor so cues round-trip consistently.
    static juce::String normalizeTC(const juce::String& tc)
    {
        auto parts = juce::StringArray::fromTokens(tc, ":.", "");
        int h = 0, m = 0, s = 0, f = 0;
        if (parts.size() >= 1) h = juce::jmax(0, parts[0].getIntValue());
        if (parts.size() >= 2) m = juce::jmax(0, parts[1].getIntValue());
        if (parts.size() >= 3) s = juce::jmax(0, parts[2].getIntValue());
        if (parts.size() >= 4) f = juce::jmax(0, parts[3].getIntValue());
        s += f / 30; f %= 30;  // assume max 30fps for frame carry
        m += s / 60; s %= 60;
        h += m / 60; m %= 60;
        h %= 24;
        return juce::String(h).paddedLeft('0', 2) + ":"
             + juce::String(m).paddedLeft('0', 2) + ":"
             + juce::String(s).paddedLeft('0', 2) + ":"
             + juce::String(f).paddedLeft('0', 2);
    }

    /// Convert a SMPTE TC string to absolute milliseconds.  Frame fps
    /// rounding goes via the supplied fps so this and the engine's
    /// armedCues stay coherent.  Defensive against malformed input.
    ///
    /// Note: this uses non-drop-frame counting even at 29.97fps.  The
    /// engine's `GeneratorCuePoint::positionMs` does the same, so the
    /// editor's display and the engine's runtime arming agree on cue
    /// positions -- they're internally consistent, just non-DF-accurate
    /// for the 29.97 case.  If we ever switch to true DF math here, the
    /// engine helper has to switch in lockstep.
    /// Both delegate to TimecodeCore's shared text conversion (drop-frame
    /// aware, same arithmetic as the engine display).  fps is the engine's
    /// rate as a double, as the rest of this editor carries it.
    static double parseTcToMs(const juce::String& tc, double fps)
    {
        return parseTimecodeTextToMs(tc, frameRateFromDouble(fps));
    }

    static juce::String msToTc(double ms, double fps)
    {
        return msToTimecodeText(ms, frameRateFromDouble(fps));
    }

    /// Recompute the cue marker positions on the strip.  Each cue's
    /// absolute TC is projected onto the audio file's time axis by
    /// subtracting the preset's startTC offset; cues outside the audio
    /// range are still pushed (the strip clips them on draw).
    void refreshStripCues()
    {
        std::vector<uint32_t> positions;
        positions.reserve(preset.cuePoints.size());
        const double startMs = preset.startTC.isNotEmpty()
            ? parseTcToMs(preset.startTC, fps) : 0.0;
        for (auto& cp : preset.cuePoints)
        {
            const double absMs = (double) cp.positionMs(fps);
            const double rel   = absMs - startMs;
            // Clamp negatives to 0 so a cue placed before startTC at least
            // shows up at the left edge as a hint that it's not in audio.
            positions.push_back((uint32_t) juce::jmax(0.0, rel));
        }
        strip.setCuePositions(positions);
    }

    void loadDetailFromCue(const GeneratorCuePoint& cue)
    {
        edPosition.setText(cue.positionTC, false);
        edName.setText(cue.name, false);
        edMidiCh.setText(juce::String(cue.midiChannel + 1), false);
        edMidiNote.setText(cue.midiNoteNum >= 0 ? juce::String(cue.midiNoteNum) : "", false);
        edMidiVel.setText(juce::String(cue.midiNoteVel), false);
        edMidiCC.setText(cue.midiCCNum >= 0 ? juce::String(cue.midiCCNum) : "", false);
        edMidiCCVal.setText(juce::String(cue.midiCCVal), false);
        edOscAddr.setText(cue.oscAddress, false);
        edOscArgs.setText(cue.oscArgs, false);
        edDmxCh.setText(cue.artnetCh > 0 ? juce::String(cue.artnetCh) : "", false);
        edDmxVal.setText(juce::String(cue.artnetVal), false);
    }

    void applyDetailEdits()
    {
        if (selectedCue < 0 || selectedCue >= (int) preset.cuePoints.size()) return;
        auto& cue = preset.cuePoints[(size_t) selectedCue];

        cue.positionTC = normalizeTC(edPosition.getText().trim());
        edPosition.setText(cue.positionTC, false);

        cue.name = edName.getText().trim();

        cue.midiChannel = juce::jlimit(0, 15, edMidiCh.getText().getIntValue() - 1);
        {
            auto noteTxt = edMidiNote.getText().trim();
            cue.midiNoteNum = noteTxt.isEmpty() ? -1 : juce::jlimit(0, 127, noteTxt.getIntValue());
        }
        cue.midiNoteVel = juce::jlimit(0, 127, edMidiVel.getText().getIntValue());
        {
            auto ccTxt = edMidiCC.getText().trim();
            cue.midiCCNum = ccTxt.isEmpty() ? -1 : juce::jlimit(0, 127, ccTxt.getIntValue());
        }
        cue.midiCCVal = juce::jlimit(0, 127, edMidiCCVal.getText().getIntValue());

        cue.oscAddress = edOscAddr.getText().trim();
        cue.oscArgs    = edOscArgs.getText().trim();

        {
            auto chTxt = edDmxCh.getText().trim();
            cue.artnetCh = chTxt.isEmpty() ? 0 : juce::jlimit(1, 512, chTxt.getIntValue());
        }
        cue.artnetVal = juce::jlimit(0, 255, edDmxVal.getText().getIntValue());

        // Re-sort and re-locate the edited cue.  Same pattern as TrackMap:
        // snapshot the full cue value (the position is the dominant key but
        // duplicates can exist if the user has multiple cues with the same
        // TC) and walk the sorted vector to find the matching entry.
        GeneratorCuePoint editedSnapshot = cue;
        preset.sortCuePoints();

        for (int i = 0; i < (int) preset.cuePoints.size(); ++i)
        {
            auto& c = preset.cuePoints[(size_t) i];
            if (c.positionTC   == editedSnapshot.positionTC
                && c.name       == editedSnapshot.name
                && c.midiChannel == editedSnapshot.midiChannel
                && c.midiNoteNum == editedSnapshot.midiNoteNum
                && c.midiCCNum   == editedSnapshot.midiCCNum
                && c.oscAddress  == editedSnapshot.oscAddress
                && c.artnetCh    == editedSnapshot.artnetCh)
            {
                selectedCue = i;
                table.selectRow(i);
                break;
            }
        }

        table.updateContent();
        table.repaint();
        refreshStripCues();
        strip.setSelectedCueIndex(selectedCue);
        if (onChange) onChange();
    }

    GeneratorPreset& preset;
    uint32_t audioLengthMs = 0;
    double   fps = 30.0;

    // Live playback cursor: when set by the host, polled at ~30Hz to
    // drive the strip's red playhead.  Empty/null = no playhead drawn.
    std::function<int64_t()> playheadGetter;

    // Editor-owned audio thumbnail.  Independent from the engine's runtime
    // GeneratorAudioPlayer so the waveform shown always matches the preset
    // being edited, regardless of what (if anything) the engine happens to
    // be playing right now.  AudioThumbnailCache is a small fixed-size
    // pool (just one source here, but JUCE requires a cache).
    juce::AudioFormatManager   ownFormatManager;
    juce::AudioThumbnailCache  ownThumbnailCache;
    juce::AudioThumbnail       ownThumbnail;

    /// Read the preset's audio file (if any) into the local thumbnail and
    /// compute its length in ms.  Called once during construction; the
    /// preset's audio file is treated as immutable for the life of the
    /// editor window (the parent closes / reopens the window if the user
    /// changes the audio file in the preset form).
    void loadOwnAudio()
    {
        audioLengthMs = 0;
        if (preset.audioFilePath.isEmpty()) return;
        juce::File f(preset.audioFilePath);
        if (! f.existsAsFile()) return;
        std::unique_ptr<juce::AudioFormatReader> reader(
            ownFormatManager.createReaderFor(f));
        if (reader == nullptr) return;
        if (reader->sampleRate > 0.0)
            audioLengthMs = (uint32_t) std::llround(
                1000.0 * (double) reader->lengthInSamples / reader->sampleRate);
        // Hand the reader off to the thumbnail; AudioThumbnail::setReader
        // takes ownership and triggers asynchronous decoding that the strip
        // observes via its ChangeListener attachment.
        ownThumbnail.setReader(reader.release(), juce::Time::currentTimeMillis());
    }

    /// Timer tick (~30Hz when a playhead getter has been installed).
    /// Pushes the latest playhead value to the strip; negatives mean
    /// "not playing", which the strip renders by hiding the cursor.
    void timerCallback() override
    {
        if (playheadGetter)
            strip.setPlayheadMs(playheadGetter());
    }

    juce::Label       lblTitle, lblCount;
    juce::TableListBox table;

    GeneratorAudioWaveformStrip strip;

    juce::Component   detailPanel;
    juce::Label       lblPosition, lblName,
                      lblMidiCh, lblMidiNote, lblMidiVel, lblMidiCC, lblMidiCCVal,
                      lblOscAddr, lblOscArgs,
                      lblDmxCh, lblDmxVal;
    juce::TextEditor  edPosition, edName,
                      edMidiCh, edMidiNote, edMidiVel, edMidiCC, edMidiCCVal,
                      edOscAddr, edOscArgs,
                      edDmxCh, edDmxVal;

    juce::TextButton btnAdd, btnDelete, btnApply;

    juce::Colour bgPanel, bgRow, bgRowAlt, textLight, textDim,
                 accentBlue, accentGreen, accentRed;

    int selectedCue = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GeneratorCuePointEditor)
};

//==============================================================================
// GeneratorCuePointEditorWindow -- floating window that hosts the editor.
// Modelled after CuePointEditorWindow.
//==============================================================================
class GeneratorCuePointEditorWindow : public juce::DocumentWindow
{
public:
    GeneratorCuePointEditorWindow(GeneratorPreset& preset,
                                  double fps = 30.0)
        : juce::DocumentWindow("Cue Points",
                               juce::Colour(0xFF1A1D23),
                               juce::DocumentWindow::closeButton,
                               true)
    {
        // setContentOwned takes ownership of the raw pointer and deletes
        // it when this window is destroyed.  No need for a unique_ptr
        // member; that would just complicate things and risk double-free
        // semantics if anyone reads it as live ownership.
        setContentOwned(new GeneratorCuePointEditor(preset, fps), true);
        setUsingNativeTitleBar(true);
        setResizable(true, false);
        setSize(720, 560);
        centreWithSize(720, 560);
    }

    void closeButtonPressed() override { setVisible(false); }

    GeneratorCuePointEditor* getEditor()
    {
        return dynamic_cast<GeneratorCuePointEditor*>(getContentComponent());
    }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GeneratorCuePointEditorWindow)
};
