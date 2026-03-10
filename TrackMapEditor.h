// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

#pragma once
#include <JuceHeader.h>
#include "AppSettings.h"
#include "ProDJLinkInput.h"
#include "DbServerClient.h"
#include "CustomLookAndFeel.h"

//==============================================================================
// TrackMapEditor -- Table editor for Track ID -> Timecode Offset mapping.
//
// Designed to be shown in a DialogWindow from MainComponent.
// Receives a TrackMap* (owned by AppSettings) and a ProDJLinkInput* (for Learn).
// Calls onChange() whenever the map is modified so the caller can persist
// settings and refresh engine lookups.
//==============================================================================
class TrackMapEditor : public juce::Component,
                       public juce::TableListBoxModel
{
public:
    //--------------------------------------------------------------------------
    // Construction
    //--------------------------------------------------------------------------
    TrackMapEditor(TrackMap& map, ProDJLinkInput* proDJLink = nullptr)
        : trackMap(map), proDJLinkInput(proDJLink)
    {
        setSize(780, 560);
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

        auto& hdr = table.getHeader();
        hdr.addColumn("Track ID",  ColTrackId,  80, 60, 120, juce::TableHeaderComponent::notSortable);
        hdr.addColumn("Artist",    ColArtist,  130, 80, 300, juce::TableHeaderComponent::notSortable);
        hdr.addColumn("Title",     ColTitle,   130, 80, 300, juce::TableHeaderComponent::notSortable);
        hdr.addColumn("Offset",    ColOffset,  100, 80, 130, juce::TableHeaderComponent::notSortable);
        hdr.addColumn("FPS",       ColFps,      50, 45,  70, juce::TableHeaderComponent::notSortable);
        hdr.addColumn("Trig",      ColTrig,     40, 30,  60, juce::TableHeaderComponent::notSortable);
        hdr.addColumn("Notes",     ColNotes,   100, 60, 400, juce::TableHeaderComponent::notSortable);

        // --- Buttons ---
        auto addBtn = [this](juce::TextButton& btn, const juce::String& text)
        {
            addAndMakeVisible(btn);
            btn.setButtonText(text);
            btn.setColour(juce::TextButton::buttonColourId, bgPanel);
            btn.setColour(juce::TextButton::buttonOnColourId, bgPanel.brighter(0.1f));
            btn.setColour(juce::TextButton::textColourOffId, textBright);
        };

        addBtn(btnLearn,    "Learn");
        addBtn(btnAdd,      "Add");
        addBtn(btnDelete,   "Delete");
        addBtn(btnImport,   "Import");
        addBtn(btnExport,   "Export");

        btnLearn.setColour(juce::TextButton::buttonColourId, accentCyan.withAlpha(0.25f));
        btnLearn.setColour(juce::TextButton::textColourOffId, accentCyan.brighter(0.4f));
        btnLearn.setEnabled(proDJLinkInput != nullptr);

        // Player selector for Learn -- lets the user capture tracks from any CDJ player
        addAndMakeVisible(lblLearnLayer);
        lblLearnLayer.setText("Player:", juce::dontSendNotification);
        lblLearnLayer.setFont(juce::Font(juce::FontOptions(9.0f)));
        lblLearnLayer.setColour(juce::Label::textColourId, textMid);
        lblLearnLayer.setJustificationType(juce::Justification::centredRight);

        addAndMakeVisible(cmbLearnLayer);
        for (int i = 1; i <= ProDJLink::kMaxPlayers; ++i)
            cmbLearnLayer.addItem(juce::String("Player ") + juce::String(i), i);
        cmbLearnLayer.setSelectedId(1, juce::dontSendNotification);  // default Player 1
        cmbLearnLayer.setColour(juce::ComboBox::backgroundColourId, bgPanel);
        cmbLearnLayer.setColour(juce::ComboBox::textColourId, accentCyan.brighter(0.4f));
        cmbLearnLayer.setColour(juce::ComboBox::outlineColourId, borderCol);
        cmbLearnLayer.setEnabled(proDJLinkInput != nullptr);

        btnLearn.onClick  = [this] { onLearn(); };
        btnAdd.onClick    = [this] { onAdd(); };
        btnDelete.onClick = [this] { onDeleteSelected(); };
        btnImport.onClick = [this] { onImport(); };
        btnExport.onClick = [this] { onExport(); };

        // --- Edit form ---
        addAndMakeVisible(formPanel);
        formPanel.setVisible(false);

        auto addField = [this](juce::Label& lbl, juce::TextEditor& ed,
                               const juce::String& labelText, bool readOnly = false)
        {
            formPanel.addAndMakeVisible(lbl);
            lbl.setText(labelText, juce::dontSendNotification);
            lbl.setFont(juce::Font(juce::FontOptions(getMonoFontName(), 9.0f, juce::Font::plain)));
            lbl.setColour(juce::Label::textColourId, textDim);
            lbl.setJustificationType(juce::Justification::centredLeft);

            formPanel.addAndMakeVisible(ed);
            ed.setFont(juce::Font(juce::FontOptions(getMonoFontName(), 11.0f, juce::Font::plain)));
            ed.setColour(juce::TextEditor::backgroundColourId, bgDarker);
            ed.setColour(juce::TextEditor::textColourId, textBright);
            ed.setColour(juce::TextEditor::outlineColourId, borderCol);
            ed.setColour(juce::TextEditor::focusedOutlineColourId, accentCyan);
            ed.setReadOnly(readOnly);
        };

        addField(lblFormTrackId, edFormTrackId, "TRACK ID:", true);
        addField(lblFormArtist,  edFormArtist,  "ARTIST:");
        addField(lblFormTitle,   edFormTitle,   "TITLE:");
        addField(lblFormOffset,  edFormOffset,  "OFFSET (HH:MM:SS:FF):");
        addField(lblFormNotes,   edFormNotes,   "NOTES:");

        formPanel.addAndMakeVisible(lblFormFps);
        lblFormFps.setText("FPS:", juce::dontSendNotification);
        lblFormFps.setFont(juce::Font(juce::FontOptions(getMonoFontName(), 9.0f, juce::Font::plain)));
        lblFormFps.setColour(juce::Label::textColourId, textDim);

        formPanel.addAndMakeVisible(cmbFormFps);
        cmbFormFps.addItem("23.976", 1);
        cmbFormFps.addItem("24", 2);
        cmbFormFps.addItem("25", 3);
        cmbFormFps.addItem("29.97", 4);
        cmbFormFps.addItem("30", 5);
        cmbFormFps.setSelectedId(5, juce::dontSendNotification);

        // --- MIDI trigger fields (independent: Note, CC, PC can coexist) ---
        addField(lblFormMidiCh, edFormMidiCh, "MIDI CH:");
        edFormMidiCh.setInputRestrictions(2, "0123456789");

        addField(lblFormMidiNote, edFormMidiNote, "NOTE:");
        edFormMidiNote.setInputRestrictions(3, "-0123456789");
        edFormMidiNote.setTextToShowWhenEmpty("--", textDim);

        addField(lblFormMidiNoteVel, edFormMidiNoteVel, "VEL:");
        edFormMidiNoteVel.setInputRestrictions(3, "0123456789");

        addField(lblFormMidiCC, edFormMidiCC, "CC#:");
        edFormMidiCC.setInputRestrictions(3, "-0123456789");
        edFormMidiCC.setTextToShowWhenEmpty("--", textDim);

        addField(lblFormMidiCCVal, edFormMidiCCVal, "VAL:");
        edFormMidiCCVal.setInputRestrictions(3, "0123456789");

        addField(lblFormMidiPC, edFormMidiPC, "PC#:");
        edFormMidiPC.setInputRestrictions(3, "-0123456789");
        edFormMidiPC.setTextToShowWhenEmpty("--", textDim);

        // --- OSC trigger fields ---
        addField(lblFormOscAddr, edFormOscAddr, "OSC TRIGGER ADDRESS:");
        edFormOscAddr.setTextToShowWhenEmpty("/trigger/track", textDim);
        addField(lblFormOscArgs, edFormOscArgs, "OSC TRIGGER ARGS:");
        edFormOscArgs.setTextToShowWhenEmpty("i:1 s:\"text\"  |  {trackId} {artist} {title}", textDim);

        // --- Art-Net DMX trigger fields ---
        addField(lblFormDmxCh, edFormDmxCh, "DMX CH:");
        edFormDmxCh.setInputRestrictions(3, "0123456789");
        edFormDmxCh.setTextToShowWhenEmpty("--", textDim);

        addField(lblFormDmxVal, edFormDmxVal, "DMX VAL:");
        edFormDmxVal.setInputRestrictions(3, "0123456789");

        auto addFormBtn = [this](juce::TextButton& btn, const juce::String& text)
        {
            formPanel.addAndMakeVisible(btn);
            btn.setButtonText(text);
            btn.setColour(juce::TextButton::buttonColourId, bgPanel);
            btn.setColour(juce::TextButton::textColourOffId, textBright);
        };

        addFormBtn(btnFormSave,   "Save");
        addFormBtn(btnFormCancel, "Cancel");
        btnFormSave.setColour(juce::TextButton::buttonColourId, accentGreen.withAlpha(0.3f));
        btnFormSave.onClick   = [this] { onFormSave(); };
        btnFormCancel.onClick = [this] { onFormCancel(); };

        // --- Status bar ---
        addAndMakeVisible(lblStatus);
        lblStatus.setFont(juce::Font(juce::FontOptions(getMonoFontName(), 9.0f, juce::Font::plain)));
        lblStatus.setColour(juce::Label::textColourId, textMid);
        updateStatusText();
    }

    ~TrackMapEditor() override
    {
        if (importPreviewWindow != nullptr)
            delete importPreviewWindow.getComponent();
    }

    //--------------------------------------------------------------------------
    // Callback when TrackMap is modified (wire to save + refresh)
    //--------------------------------------------------------------------------
    std::function<void()> onChange;

    //--------------------------------------------------------------------------
    // Set the active track ID (for visual highlighting in the table)
    //--------------------------------------------------------------------------
    void setActiveTrackId(uint32_t id)
    {
        if (activeTrackId != id) { activeTrackId = id; table.repaint(); }
    }

    /// Set the Learn player combo to match the engine's current player.
    void setLearnPlayer(int player)
    {
        cmbLearnLayer.setSelectedId(juce::jlimit(1, ProDJLink::kMaxPlayers, player),
                                     juce::dontSendNotification);
    }

    /// Phase 2: set metadata client for enriched Learn data
    void setDbServerClient(DbServerClient* client) { dbClient = client; }

    //--------------------------------------------------------------------------
    // Refresh after external changes
    //--------------------------------------------------------------------------
    void refresh()
    {
        rebuildRows();
        table.updateContent();
        table.repaint();
        updateStatusText();
    }

    //--------------------------------------------------------------------------
    // Component overrides
    //--------------------------------------------------------------------------
    void paint(juce::Graphics& g) override
    {
        g.fillAll(bgDark);

        // Draw form panel border when visible
        if (formPanel.isVisible())
        {
            g.setColour(borderCol.brighter(0.1f));
            g.drawRect(formPanel.getBounds().expanded(1), 1);
            g.setColour(bgDarker.brighter(0.03f));
            g.fillRect(formPanel.getBounds());
        }
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(8);

        // Button row
        auto btnRow = area.removeFromTop(28);
        int bw = 64;
        btnLearn.setBounds(btnRow.removeFromLeft(bw));   btnRow.removeFromLeft(4);
        lblLearnLayer.setBounds(btnRow.removeFromLeft(38));
        cmbLearnLayer.setBounds(btnRow.removeFromLeft(100));  btnRow.removeFromLeft(8);
        btnAdd.setBounds(btnRow.removeFromLeft(bw));      btnRow.removeFromLeft(4);
        btnDelete.setBounds(btnRow.removeFromLeft(bw));

        btnExport.setBounds(btnRow.removeFromRight(bw));  btnRow.removeFromRight(4);
        btnImport.setBounds(btnRow.removeFromRight(bw));

        area.removeFromTop(6);

        // Status bar
        auto statusRow = area.removeFromBottom(20);
        lblStatus.setBounds(statusRow);

        // Edit form (fixed height at bottom, above status)
        if (formPanel.isVisible())
        {
            area.removeFromBottom(4);
            auto formArea = area.removeFromBottom(kFormHeight);
            formPanel.setBounds(formArea);
            layoutForm(formArea.withZeroOrigin());
            area.removeFromBottom(4);
        }

        // Table takes remaining space
        table.setBounds(area);
    }

    //==========================================================================
    // TableListBoxModel
    //==========================================================================
    int getNumRows() override { return (int)rows.size(); }

    void paintRowBackground(juce::Graphics& g, int rowNumber,
                            int /*w*/, int /*h*/, bool isSelected) override
    {
        if (rowNumber < 0 || rowNumber >= (int)rows.size()) return;

        bool isActive = (rows[(size_t)rowNumber]->trackId == activeTrackId && activeTrackId != 0);

        if (isSelected)
            g.fillAll(accentCyan.withAlpha(0.15f));
        else if (isActive)
            g.fillAll(accentGreen.withAlpha(0.08f));
        else
            g.fillAll((rowNumber % 2 == 0) ? bgDarker : bgDarker.brighter(0.02f));
    }

    void paintCell(juce::Graphics& g, int rowNumber, int columnId,
                   int width, int height, bool /*isSelected*/) override
    {
        if (rowNumber < 0 || rowNumber >= (int)rows.size()) return;
        jassert(rowsGeneration == trackMap.getGeneration());  // stale pointers!
        const auto& entry = *rows[(size_t)rowNumber];

        bool isActive = (entry.trackId == activeTrackId && activeTrackId != 0);
        g.setColour(isActive ? accentGreen.brighter(0.3f) : textBright);
        g.setFont(juce::Font(juce::FontOptions(getMonoFontName(), 11.0f, juce::Font::plain)));

        juce::String text;
        switch (columnId)
        {
            case ColTrackId: text = juce::String(entry.trackId); break;
            case ColArtist:  text = entry.artist; break;
            case ColTitle:   text = entry.title; break;
            case ColOffset:  text = entry.timecodeOffset; break;
            case ColFps:     text = fpsIndexToString(entry.frameRate); break;
            case ColTrig:
            {
                // Show compact trigger indicators: M=MIDI, O=OSC, D=DMX
                juce::String t;
                if (entry.hasMidiTrigger())   t += "M";
                if (entry.hasOscTrigger())    t += "O";
                if (entry.hasArtnetTrigger()) t += "D";
                if (t.isNotEmpty())
                {
                    g.setColour(juce::Colour(0xFFFFAB00));  // amber accent
                    g.drawText(t, 4, 0, width - 8, height, juce::Justification::centred, true);
                }
                return;
            }
            case ColNotes:   text = entry.notes; break;
        }

        g.drawText(text, 4, 0, width - 8, height, juce::Justification::centredLeft, true);
    }

    void cellDoubleClicked(int rowNumber, int /*columnId*/,
                           const juce::MouseEvent&) override
    {
        if (rowNumber >= 0 && rowNumber < (int)rows.size())
            openFormForRow(rowNumber);
    }

    void selectedRowsChanged(int lastRowSelected) override
    {
        (void)lastRowSelected;
        // Don't auto-open form on single click -- double-click opens it
    }

private:
    //--------------------------------------------------------------------------
    // Data
    //--------------------------------------------------------------------------
    TrackMap& trackMap;
    ProDJLinkInput* proDJLinkInput;
    DbServerClient* dbClient = nullptr;  // Phase 2: metadata cache for Learn

    std::vector<const TrackMapEntry*> rows;   // sorted snapshot (ptrs into trackMap) for display
    uint64_t rowsGeneration = 0;               // generation at which rows were built
    uint32_t activeTrackId = 0;
    int editingRow = -1;               // -1 = adding new, >= 0 = editing existing

    //--------------------------------------------------------------------------
    // Colours (matching MainComponent)
    //--------------------------------------------------------------------------
    juce::Colour bgDark       { 0xFF12141A };
    juce::Colour bgPanel      { 0xFF14161C };
    juce::Colour bgDarker     { 0xFF0D0E12 };
    juce::Colour borderCol    { 0xFF1E2028 };
    juce::Colour textDim      { 0xFF37474F };
    juce::Colour textMid      { 0xFF546E7A };
    juce::Colour textBright   { 0xFFCFD8DC };
    juce::Colour accentCyan   { 0xFF00838F };
    juce::Colour accentGreen  { 0xFF2E7D32 };

    //--------------------------------------------------------------------------
    // Column IDs
    //--------------------------------------------------------------------------
    enum { ColTrackId = 1, ColArtist, ColTitle, ColOffset, ColFps, ColTrig, ColNotes };

    //--------------------------------------------------------------------------
    // UI components
    //--------------------------------------------------------------------------
    juce::TableListBox table { "TrackMapTable", this };

    juce::TextButton btnLearn, btnAdd, btnDelete, btnImport, btnExport;
    juce::Label lblLearnLayer;
    juce::ComboBox cmbLearnLayer;

    // Edit form
    juce::Component formPanel;
    static constexpr int kFormHeight = 283;   // 7 rows + spacing

    juce::Label      lblFormTrackId, lblFormArtist, lblFormTitle, lblFormOffset, lblFormFps, lblFormNotes;
    juce::TextEditor edFormTrackId, edFormArtist, edFormTitle, edFormOffset, edFormNotes;
    juce::ComboBox   cmbFormFps;

    // MIDI trigger (independent: Note, CC, PC can coexist)
    juce::Label      lblFormMidiCh, lblFormMidiNote, lblFormMidiNoteVel;
    juce::Label      lblFormMidiCC, lblFormMidiCCVal, lblFormMidiPC;
    juce::TextEditor edFormMidiCh, edFormMidiNote, edFormMidiNoteVel;
    juce::TextEditor edFormMidiCC, edFormMidiCCVal, edFormMidiPC;

    // OSC trigger
    juce::Label      lblFormOscAddr, lblFormOscArgs;
    juce::TextEditor edFormOscAddr, edFormOscArgs;

    // Art-Net DMX trigger
    juce::Label      lblFormDmxCh, lblFormDmxVal;
    juce::TextEditor edFormDmxCh, edFormDmxVal;

    juce::TextButton btnFormSave, btnFormCancel;

    juce::Label lblStatus;

    //--------------------------------------------------------------------------
    // Helpers
    //--------------------------------------------------------------------------
    void rebuildRows()
    {
        rows = trackMap.getAllSortedPtrs();
        rowsGeneration = trackMap.getGeneration();
    }

    void notifyChanged()
    {
        rebuildRows();
        table.updateContent();
        table.repaint();
        updateStatusText();
        if (onChange) onChange();
    }

    void updateStatusText()
    {
        juce::String s = "Tracks: " + juce::String(trackMap.size());
        if (activeTrackId != 0)
        {
            const auto* entry = trackMap.find(activeTrackId);
            s += " | Active: #" + juce::String(activeTrackId);
            s += entry ? " (MAPPED)" : " (UNMAPPED)";
        }
        lblStatus.setText(s, juce::dontSendNotification);
    }

    static juce::String fpsIndexToString(int idx)
    {
        switch (idx)
        {
            case 0: return "23.976";
            case 1: return "24";
            case 2: return "25";
            case 3: return "29.97";
            case 4: return "30";
        }
        return "30";
    }

    //--------------------------------------------------------------------------
    // Form layout
    //--------------------------------------------------------------------------
    void layoutForm(juce::Rectangle<int> area)
    {
        area = area.reduced(6, 4);
        const int labelH = 13;
        const int fieldH = 22;
        const int rowH   = labelH + fieldH;

        // Row 1: Track ID, Offset, FPS
        auto row1 = area.removeFromTop(rowH);
        {
            auto seg = row1.removeFromLeft(140);
            lblFormTrackId.setBounds(seg.removeFromTop(labelH));
            edFormTrackId.setBounds(seg);
            row1.removeFromLeft(8);

            seg = row1.removeFromLeft(160);
            lblFormOffset.setBounds(seg.removeFromTop(labelH));
            edFormOffset.setBounds(seg);
            row1.removeFromLeft(8);

            seg = row1.removeFromLeft(80);
            lblFormFps.setBounds(seg.removeFromTop(labelH));
            cmbFormFps.setBounds(seg);
        }

        area.removeFromTop(4);

        // Row 2: Artist, Title
        auto row2 = area.removeFromTop(rowH);
        {
            int halfW = (row2.getWidth() - 8) / 2;
            auto seg = row2.removeFromLeft(halfW);
            lblFormArtist.setBounds(seg.removeFromTop(labelH));
            edFormArtist.setBounds(seg);
            row2.removeFromLeft(8);

            lblFormTitle.setBounds(row2.removeFromTop(labelH));
            edFormTitle.setBounds(row2);
        }

        area.removeFromTop(4);

        // Row 3: Notes (full width)
        auto row3 = area.removeFromTop(rowH);
        {
            lblFormNotes.setBounds(row3.removeFromTop(labelH));
            edFormNotes.setBounds(row3);
        }

        area.removeFromTop(4);

        // Row 4: MIDI Trigger: Ch, Note, Vel, CC#, CCVal, PC# (all independent)
        auto row4 = area.removeFromTop(rowH);
        {
            auto seg = row4.removeFromLeft(52);
            lblFormMidiCh.setBounds(seg.removeFromTop(labelH));
            edFormMidiCh.setBounds(seg);
            row4.removeFromLeft(6);

            seg = row4.removeFromLeft(52);
            lblFormMidiNote.setBounds(seg.removeFromTop(labelH));
            edFormMidiNote.setBounds(seg);
            row4.removeFromLeft(6);

            seg = row4.removeFromLeft(52);
            lblFormMidiNoteVel.setBounds(seg.removeFromTop(labelH));
            edFormMidiNoteVel.setBounds(seg);
            row4.removeFromLeft(12);

            seg = row4.removeFromLeft(52);
            lblFormMidiCC.setBounds(seg.removeFromTop(labelH));
            edFormMidiCC.setBounds(seg);
            row4.removeFromLeft(6);

            seg = row4.removeFromLeft(52);
            lblFormMidiCCVal.setBounds(seg.removeFromTop(labelH));
            edFormMidiCCVal.setBounds(seg);
            row4.removeFromLeft(12);

            seg = row4.removeFromLeft(52);
            lblFormMidiPC.setBounds(seg.removeFromTop(labelH));
            edFormMidiPC.setBounds(seg);
        }

        area.removeFromTop(4);

        // Row 5: OSC Trigger Address + Args
        auto row5 = area.removeFromTop(rowH);
        {
            int halfOsc = (row5.getWidth() - 8) / 2;

            auto seg = row5.removeFromLeft(halfOsc);
            lblFormOscAddr.setBounds(seg.removeFromTop(labelH));
            edFormOscAddr.setBounds(seg);
            row5.removeFromLeft(8);

            lblFormOscArgs.setBounds(row5.removeFromTop(labelH));
            edFormOscArgs.setBounds(row5);
        }

        area.removeFromTop(4);

        // Row 6: Art-Net DMX trigger
        auto row6 = area.removeFromTop(rowH);
        {
            auto seg = row6.removeFromLeft(80);
            lblFormDmxCh.setBounds(seg.removeFromTop(labelH));
            edFormDmxCh.setBounds(seg);
            row6.removeFromLeft(8);

            seg = row6.removeFromLeft(80);
            lblFormDmxVal.setBounds(seg.removeFromTop(labelH));
            edFormDmxVal.setBounds(seg);
        }

        area.removeFromTop(6);

        // Row 7: Save / Cancel buttons (right-aligned)
        auto row7 = area.removeFromTop(24);
        {
            btnFormCancel.setBounds(row7.removeFromRight(64));
            row7.removeFromRight(4);
            btnFormSave.setBounds(row7.removeFromRight(64));
        }
    }

    //--------------------------------------------------------------------------
    // Form open / close
    //--------------------------------------------------------------------------
    void openFormForRow(int rowIndex)
    {
        if (rowIndex < 0 || rowIndex >= (int)rows.size()) return;
        jassert(rowsGeneration == trackMap.getGeneration());  // stale pointers!
        const auto& entry = *rows[(size_t)rowIndex];
        editingRow = rowIndex;

        edFormTrackId.setReadOnly(true);
        edFormTrackId.setText(juce::String(entry.trackId), false);
        edFormArtist.setText(entry.artist, false);
        edFormTitle.setText(entry.title, false);
        edFormOffset.setText(entry.timecodeOffset, false);
        edFormNotes.setText(entry.notes, false);
        cmbFormFps.setSelectedId(entry.frameRate + 1, juce::dontSendNotification);

        // MIDI trigger (independent fields)
        edFormMidiCh.setText(juce::String(entry.midiChannel + 1), false);   // display 1-16
        edFormMidiNote.setText(entry.midiNoteNum >= 0 ? juce::String(entry.midiNoteNum) : "", false);
        edFormMidiNoteVel.setText(juce::String(entry.midiNoteVel), false);
        edFormMidiCC.setText(entry.midiCCNum >= 0 ? juce::String(entry.midiCCNum) : "", false);
        edFormMidiCCVal.setText(juce::String(entry.midiCCVal), false);
        edFormMidiPC.setText(entry.midiPC >= 0 ? juce::String(entry.midiPC) : "", false);

        // OSC trigger
        edFormOscAddr.setText(entry.oscAddress, false);
        edFormOscArgs.setText(entry.oscArgs, false);

        // Art-Net DMX trigger
        edFormDmxCh.setText(entry.artnetCh > 0 ? juce::String(entry.artnetCh) : "", false);
        edFormDmxVal.setText(juce::String(entry.artnetVal), false);

        formPanel.setVisible(true);
        resized();
        edFormOffset.grabKeyboardFocus();
    }

    void openFormForNew(uint32_t trackId = 0,
                        const juce::String& artist = {},
                        const juce::String& title = {})
    {
        editingRow = -1;

        edFormTrackId.setText(trackId != 0 ? juce::String(trackId) : "", false);
        edFormTrackId.setReadOnly(trackId != 0);
        edFormArtist.setText(artist, false);
        edFormTitle.setText(title, false);
        edFormOffset.setText("00:00:00:00", false);
        edFormNotes.setText("", false);
        cmbFormFps.setSelectedId(5, juce::dontSendNotification);  // default 30fps

        // MIDI trigger defaults (all disabled)
        edFormMidiCh.setText("1", false);
        edFormMidiNote.setText("", false);
        edFormMidiNoteVel.setText("127", false);
        edFormMidiCC.setText("", false);
        edFormMidiCCVal.setText("127", false);
        edFormMidiPC.setText("", false);

        // OSC trigger defaults
        edFormOscAddr.setText("", false);
        edFormOscArgs.setText("", false);

        // Art-Net DMX trigger defaults
        edFormDmxCh.setText("", false);
        edFormDmxVal.setText("255", false);

        formPanel.setVisible(true);
        resized();

        if (trackId != 0)
            edFormOffset.grabKeyboardFocus();
        else
            edFormTrackId.grabKeyboardFocus();
    }

    void closeForm()
    {
        formPanel.setVisible(false);
        editingRow = -1;
        resized();
    }

    //--------------------------------------------------------------------------
    // Form actions
    //--------------------------------------------------------------------------
    void onFormSave()
    {
        // Parse track ID
        uint32_t trackId = 0;
        if (editingRow >= 0 && editingRow < (int)rows.size())
        {
            trackId = rows[(size_t)editingRow]->trackId;
        }
        else
        {
            auto idText = edFormTrackId.getText().trim();
            trackId = (uint32_t)idText.getLargeIntValue();
        }

        if (trackId == 0)
        {
            edFormTrackId.grabKeyboardFocus();
            return;
        }

        // Validate offset
        juce::String offset = edFormOffset.getText().trim();
        int h, m, s, f;
        if (!TrackMapEntry::parseTimecodeString(offset, h, m, s, f))
        {
            edFormOffset.grabKeyboardFocus();
            edFormOffset.setHighlightedRegion(juce::Range<int>(0, offset.length()));
            return;
        }
        // Normalize the offset string
        offset = TrackMapEntry::formatTimecodeString(h, m, s, f);

        TrackMapEntry entry;
        entry.trackId        = trackId;
        entry.artist         = edFormArtist.getText().trim();
        entry.title          = edFormTitle.getText().trim();
        entry.timecodeOffset = offset;
        entry.frameRate      = juce::jlimit(0, 4, cmbFormFps.getSelectedId() - 1);
        entry.notes          = edFormNotes.getText().trim();

        // MIDI trigger (independent fields)
        entry.midiChannel = juce::jlimit(0, 15, edFormMidiCh.getText().getIntValue() - 1);
        {
            auto noteText = edFormMidiNote.getText().trim();
            entry.midiNoteNum = (noteText.isEmpty() || noteText == "--") ? -1
                              : juce::jlimit(-1, 127, noteText.getIntValue());
        }
        entry.midiNoteVel = juce::jlimit(0, 127, edFormMidiNoteVel.getText().getIntValue());
        {
            auto ccText = edFormMidiCC.getText().trim();
            entry.midiCCNum = (ccText.isEmpty() || ccText == "--") ? -1
                            : juce::jlimit(-1, 127, ccText.getIntValue());
        }
        entry.midiCCVal = juce::jlimit(0, 127, edFormMidiCCVal.getText().getIntValue());
        {
            auto pcText = edFormMidiPC.getText().trim();
            entry.midiPC = (pcText.isEmpty() || pcText == "--") ? -1
                         : juce::jlimit(-1, 127, pcText.getIntValue());
        }

        // OSC trigger
        entry.oscAddress = edFormOscAddr.getText().trim();
        entry.oscArgs    = edFormOscArgs.getText().trim();

        // Art-Net DMX trigger
        {
            auto dmxText = edFormDmxCh.getText().trim();
            entry.artnetCh = (dmxText.isEmpty() || dmxText == "--" || dmxText == "0") ? 0
                           : juce::jlimit(0, 512, dmxText.getIntValue());
        }
        entry.artnetVal = juce::jlimit(0, 255, edFormDmxVal.getText().getIntValue());

        trackMap.addOrUpdate(entry);
        closeForm();
        notifyChanged();

        // Select the saved row
        for (int i = 0; i < (int)rows.size(); ++i)
        {
            if (rows[(size_t)i]->trackId == trackId)
            {
                table.selectRow(i);
                break;
            }
        }
    }

    void onFormCancel()
    {
        closeForm();
    }

    //--------------------------------------------------------------------------
    // Button actions
    //--------------------------------------------------------------------------
    void onLearn()
    {
        if (!proDJLinkInput || !proDJLinkInput->getIsRunning()) return;

        int player = cmbLearnLayer.getSelectedId();  // combo ID == player number (1-6)
        uint32_t trackId = proDJLinkInput->getTrackID(player);
        if (trackId == 0) return;

        auto tinfo = proDJLinkInput->getTrackInfo(player);

        // Phase 2: enrich with dbClient cache if available
        if (dbClient != nullptr)
        {
            auto meta = dbClient->getCachedMetadataByTrackId(trackId);
            if (meta.isValid())
            {
                tinfo.artist = meta.artist;
                tinfo.title  = meta.title;
            }
        }

        // If entry already exists, open it for editing
        if (trackMap.contains(trackId))
        {
            rebuildRows();
            table.updateContent();
            for (int i = 0; i < (int)rows.size(); ++i)
            {
                if (rows[(size_t)i]->trackId == trackId)
                {
                    table.selectRow(i);
                    openFormForRow(i);
                    break;
                }
            }
        }
        else
        {
            // Create new entry pre-filled with ProDJLink metadata
            openFormForNew(trackId, tinfo.artist, tinfo.title);
        }
    }

    void onAdd()
    {
        openFormForNew();
    }

    void onDeleteSelected()
    {
        auto selected = table.getSelectedRow();
        if (selected < 0 || selected >= (int)rows.size()) return;
        jassert(rowsGeneration == trackMap.getGeneration());  // stale pointers!

        // Capture entry data by value before the async dialog.
        const auto& entry = *rows[(size_t)selected];
        uint32_t deleteTrackId = entry.trackId;
        juce::String msg = "Delete track #" + juce::String(deleteTrackId);
        if (entry.artist.isNotEmpty())
            msg += " (" + entry.artist + " - " + entry.title + ")";
        msg += "?";

        auto options = juce::MessageBoxOptions()
            .withIconType(juce::MessageBoxIconType::QuestionIcon)
            .withTitle("Delete Track")
            .withMessage(msg)
            .withButton("Delete")
            .withButton("Cancel");

        // showScopedAsync returns a ScopedMessageBox that must be kept alive
        // until the callback fires.  Storing it as a member ensures this.
        deleteConfirmBox = juce::AlertWindow::showScopedAsync(options,
            [this, deleteTrackId](int result)
            {
                if (result == 1)  // first button = Delete
                {
                    trackMap.remove(deleteTrackId);
                    closeForm();
                    notifyChanged();
                }
            });
    }

    void onImport()
    {
        fileChooser = std::make_unique<juce::FileChooser>(
            "Import Track Map", juce::File(), "*.json");

        fileChooser->launchAsync(juce::FileBrowserComponent::openMode
                                | juce::FileBrowserComponent::canSelectFiles,
            [this](const juce::FileChooser& fc)
        {
            auto file = fc.getResult();
            if (file == juce::File()) return;

            // Parse file into a temporary list
            auto parsed = juce::JSON::parse(file.loadFileAsString());
            auto* obj = parsed.getDynamicObject();
            if (!obj) return;

            auto* arr = obj->getProperty("tracks").getArray();
            if (!arr || arr->isEmpty()) return;

            std::vector<TrackMapEntry> entries;
            for (auto& item : *arr)
            {
                TrackMapEntry e;
                e.fromVar(item);
                if (e.trackId != 0)
                    entries.push_back(std::move(e));
            }
            if (entries.empty()) return;

            // Sort by Track ID
            std::sort(entries.begin(), entries.end(),
                [](const TrackMapEntry& a, const TrackMapEntry& b) {
                    return a.trackId < b.trackId;
                });

            showImportPreview(std::move(entries));
        });
    }

    //--------------------------------------------------------------------------
    // Import Preview -- select which tracks to import from a file
    //--------------------------------------------------------------------------
    class ImportPreview : public juce::Component,
                         public juce::TableListBoxModel
    {
    public:
        ImportPreview(std::vector<TrackMapEntry> entries,
                      const TrackMap& existingMap)
            : items(std::move(entries))
        {
            selected.resize(items.size(), true);

            // Mark duplicates
            for (size_t i = 0; i < items.size(); ++i)
                isDuplicate.push_back(existingMap.contains(items[i].trackId));

            setSize(600, 400);

            addAndMakeVisible(table);
            table.setModel(this);
            table.setColour(juce::ListBox::backgroundColourId, juce::Colour(0xFF0D0E12));
            table.setColour(juce::ListBox::outlineColourId, juce::Colour(0xFF1E2028));
            table.setOutlineThickness(1);
            table.setRowHeight(22);
            table.setHeaderHeight(20);
            table.getHeader().setStretchToFitActive(true);

            auto& hdr = table.getHeader();
            hdr.addColumn("",          1,  28, 28,  28, juce::TableHeaderComponent::notSortable);
            hdr.addColumn("Track ID",  2,  70, 50,  90, juce::TableHeaderComponent::notSortable);
            hdr.addColumn("Artist",    3, 140, 60, 250, juce::TableHeaderComponent::notSortable);
            hdr.addColumn("Title",     4, 150, 60, 300, juce::TableHeaderComponent::notSortable);
            hdr.addColumn("Offset",    5,  90, 70, 110, juce::TableHeaderComponent::notSortable);
            hdr.addColumn("Status",    6,  80, 60, 100, juce::TableHeaderComponent::notSortable);

            addAndMakeVisible(btnSelectAll);
            btnSelectAll.setButtonText("Select All");
            btnSelectAll.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF14161C));
            btnSelectAll.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFCFD8DC));
            btnSelectAll.onClick = [this] { std::fill(selected.begin(), selected.end(), true); table.repaint(); updateInfoLabel(); };

            addAndMakeVisible(btnSelectNone);
            btnSelectNone.setButtonText("Select None");
            btnSelectNone.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF14161C));
            btnSelectNone.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFCFD8DC));
            btnSelectNone.onClick = [this] { std::fill(selected.begin(), selected.end(), false); table.repaint(); updateInfoLabel(); };

            addAndMakeVisible(btnImport);
            btnImport.setButtonText("Import Selected");
            btnImport.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF2E7D32).withAlpha(0.3f));
            btnImport.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFCFD8DC));
            btnImport.onClick = [this] { if (onImportSelected) onImportSelected(); };

            addAndMakeVisible(btnCancel);
            btnCancel.setButtonText("Cancel");
            btnCancel.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF14161C));
            btnCancel.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFCFD8DC));
            btnCancel.onClick = [this] { if (onCancel) onCancel(); };

            addAndMakeVisible(lblInfo);
            lblInfo.setFont(juce::Font(juce::FontOptions(getMonoFontName(), 9.0f, juce::Font::plain)));
            lblInfo.setColour(juce::Label::textColourId, juce::Colour(0xFF546E7A));
            updateInfoLabel();
        }

        std::function<void()> onImportSelected;
        std::function<void()> onCancel;

        /// Get entries the user chose to import
        std::vector<TrackMapEntry> getSelectedEntries() const
        {
            std::vector<TrackMapEntry> result;
            for (size_t i = 0; i < items.size(); ++i)
                if (selected[i])
                    result.push_back(items[i]);
            return result;
        }

        void paint(juce::Graphics& g) override { g.fillAll(juce::Colour(0xFF12141A)); }

        void resized() override
        {
            auto area = getLocalBounds().reduced(8);
            auto btnRow = area.removeFromBottom(28);
            btnCancel.setBounds(btnRow.removeFromRight(70));
            btnRow.removeFromRight(4);
            btnImport.setBounds(btnRow.removeFromRight(110));
            btnRow.removeFromRight(12);
            btnSelectNone.setBounds(btnRow.removeFromRight(85));
            btnRow.removeFromRight(4);
            btnSelectAll.setBounds(btnRow.removeFromRight(75));

            auto infoRow = area.removeFromBottom(18);
            area.removeFromBottom(4);
            lblInfo.setBounds(infoRow);

            table.setBounds(area);
        }

        // TableListBoxModel
        int getNumRows() override { return (int)items.size(); }

        void paintRowBackground(juce::Graphics& g, int row, int, int, bool) override
        {
            if (row < 0 || row >= (int)items.size()) return;
            g.fillAll((row % 2 == 0) ? juce::Colour(0xFF0D0E12) : juce::Colour(0xFF0D0E12).brighter(0.02f));
        }

        void paintCell(juce::Graphics& g, int row, int col, int w, int h, bool) override
        {
            if (row < 0 || row >= (int)items.size()) return;
            auto& e = items[(size_t)row];
            bool sel = selected[(size_t)row];
            bool dup = isDuplicate[(size_t)row];

            g.setFont(juce::Font(juce::FontOptions(getMonoFontName(), 10.0f, juce::Font::plain)));

            switch (col)
            {
                case 1:  // Checkbox
                {
                    auto box = juce::Rectangle<float>(6, (h - 12) / 2.0f, 12, 12);
                    g.setColour(juce::Colour(0xFF1E2028));
                    g.fillRect(box);
                    g.setColour(juce::Colour(0xFF37474F));
                    g.drawRect(box, 1.0f);
                    if (sel)
                    {
                        g.setColour(juce::Colour(0xFF00838F));
                        g.drawText("X", box.toNearestInt(),
                                   juce::Justification::centred);
                    }
                    break;
                }
                case 2:
                    g.setColour(sel ? juce::Colour(0xFFCFD8DC) : juce::Colour(0xFF546E7A));
                    g.drawText(juce::String(e.trackId), 4, 0, w - 8, h, juce::Justification::centredLeft);
                    break;
                case 3:
                    g.setColour(sel ? juce::Colour(0xFFCFD8DC) : juce::Colour(0xFF546E7A));
                    g.drawText(e.artist, 4, 0, w - 8, h, juce::Justification::centredLeft, true);
                    break;
                case 4:
                    g.setColour(sel ? juce::Colour(0xFFCFD8DC) : juce::Colour(0xFF546E7A));
                    g.drawText(e.title, 4, 0, w - 8, h, juce::Justification::centredLeft, true);
                    break;
                case 5:
                    g.setColour(sel ? juce::Colour(0xFFCFD8DC) : juce::Colour(0xFF546E7A));
                    g.drawText(e.timecodeOffset, 4, 0, w - 8, h, juce::Justification::centredLeft);
                    break;
                case 6:
                    g.setColour(dup ? juce::Colour(0xFFE65100) : juce::Colour(0xFF2E7D32));
                    g.drawText(dup ? "overwrite" : "new", 4, 0, w - 8, h, juce::Justification::centredLeft);
                    break;
            }
        }

        void cellClicked(int row, int col, const juce::MouseEvent&) override
        {
            (void)col;
            if (row >= 0 && row < (int)selected.size())
            {
                selected[(size_t)row] = !selected[(size_t)row];
                table.repaint();
                updateInfoLabel();
            }
        }

    private:
        std::vector<TrackMapEntry> items;
        std::vector<bool> selected;
        std::vector<bool> isDuplicate;

        juce::TableListBox table { "ImportPreview", this };
        juce::TextButton btnSelectAll, btnSelectNone, btnImport, btnCancel;
        juce::Label lblInfo;

        void updateInfoLabel()
        {
            int sel = 0, dup = 0;
            for (size_t i = 0; i < items.size(); ++i)
            {
                if (selected[i]) { ++sel; if (isDuplicate[i]) ++dup; }
            }
            juce::String s = juce::String(sel) + " of " + juce::String(items.size()) + " selected";
            if (dup > 0) s += " (" + juce::String(dup) + " will overwrite existing)";
            lblInfo.setText(s, juce::dontSendNotification);
        }
    };

    juce::Component::SafePointer<juce::DialogWindow> importPreviewWindow;

    void showImportPreview(std::vector<TrackMapEntry> entries)
    {
        // Close any existing preview
        if (importPreviewWindow != nullptr)
            delete importPreviewWindow.getComponent();

        auto* preview = new ImportPreview(std::move(entries), trackMap);

        preview->onImportSelected = [this, preview]
        {
            auto selected = preview->getSelectedEntries();
            for (auto& e : selected)
                trackMap.addOrUpdate(e);

            int count = (int)selected.size();

            // Defer destruction -- we're inside a callback of a child component
            juce::Component::SafePointer<TrackMapEditor> safeThis(this);
            juce::MessageManager::callAsync([safeThis, count]
            {
                if (auto* self = safeThis.getComponent())
                {
                    if (self->importPreviewWindow != nullptr)
                        delete self->importPreviewWindow.getComponent();

                    self->notifyChanged();

                    if (count > 0)
                    {
                        juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                            .withIconType(juce::MessageBoxIconType::InfoIcon)
                            .withTitle("Import Complete")
                            .withMessage(juce::String(count) + " track(s) imported.")
                            .withButton("OK"),
                            nullptr);
                    }
                }
            });
        };

        preview->onCancel = [this]
        {
            juce::Component::SafePointer<TrackMapEditor> safeThis(this);
            juce::MessageManager::callAsync([safeThis]
            {
                if (auto* self = safeThis.getComponent())
                {
                    if (self->importPreviewWindow != nullptr)
                        delete self->importPreviewWindow.getComponent();
                }
            });
        };

        auto opts = juce::DialogWindow::LaunchOptions();
        opts.dialogTitle = "Import -- Select Tracks";
        opts.dialogBackgroundColour = juce::Colour(0xFF12141A);
        opts.content.setOwned(preview);
        opts.useNativeTitleBar = true;
        opts.resizable = true;

        importPreviewWindow = opts.launchAsync();
    }

    void onExport()
    {
        fileChooser = std::make_unique<juce::FileChooser>(
            "Export Track Map",
            TrackMap::getTrackMapFile().getParentDirectory().getChildFile("trackmap_export.json"),
            "*.json");

        fileChooser->launchAsync(juce::FileBrowserComponent::saveMode
                                | juce::FileBrowserComponent::canSelectFiles,
            [this](const juce::FileChooser& fc)
        {
            auto file = fc.getResult();
            if (file == juce::File()) return;

            if (trackMap.exportToFile(file))
            {
                juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                    .withIconType(juce::MessageBoxIconType::InfoIcon)
                    .withTitle("Export Complete")
                    .withMessage(juce::String(trackMap.size()) + " track(s) exported.")
                    .withButton("OK"),
                    nullptr);
            }
        });
    }

    // Async file chooser (must stay alive until callback)
    std::unique_ptr<juce::FileChooser> fileChooser;

    // Async delete confirmation (must stay alive until callback fires)
    juce::ScopedMessageBox deleteConfirmBox;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TrackMapEditor)
};
