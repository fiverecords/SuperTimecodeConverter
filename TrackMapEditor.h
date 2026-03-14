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
        hdr.addColumn("Artist",    ColArtist,  140, 80, 300, juce::TableHeaderComponent::notSortable);
        hdr.addColumn("Title",     ColTitle,   130, 80, 300, juce::TableHeaderComponent::notSortable);
        hdr.addColumn("Offset",    ColOffset,  100, 80, 130, juce::TableHeaderComponent::notSortable);
        hdr.addColumn("BPM",       ColBpm,      46, 40,  70, juce::TableHeaderComponent::notSortable);
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

        addField(lblFormArtist,  edFormArtist,  "ARTIST:");
        addField(lblFormTitle,   edFormTitle,   "TITLE:");
        addField(lblFormOffset,  edFormOffset,  "OFFSET (HH:MM:SS:FF):");
        addField(lblFormNotes,   edFormNotes,   "NOTES:");

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

        // --- Trigger section header ---
        formPanel.addAndMakeVisible(lblTriggerSection);
        lblTriggerSection.setText("TRIGGERS ON TRACK LOAD", juce::dontSendNotification);
        lblTriggerSection.setFont(juce::Font(juce::FontOptions(getMonoFontName(), 10.0f, juce::Font::bold)));
        lblTriggerSection.setColour(juce::Label::textColourId, accentCyan);
        lblTriggerSection.setJustificationType(juce::Justification::centredLeft);

        // --- OSC trigger fields ---
        addField(lblFormOscAddr, edFormOscAddr, "OSC TRIGGER ADDRESS:");
        edFormOscAddr.setTextToShowWhenEmpty("/trigger/track", textDim);
        addField(lblFormOscArgs, edFormOscArgs, "OSC TRIGGER ARGS:");
        edFormOscArgs.setTextToShowWhenEmpty("i:1 s:\"text\"  |  {artist} {title}", textDim);

        // --- Art-Net DMX trigger fields ---
        addField(lblFormDmxCh, edFormDmxCh, "DMX CH:");
        edFormDmxCh.setInputRestrictions(3, "0123456789");
        edFormDmxCh.setTextToShowWhenEmpty("--", textDim);

        addField(lblFormDmxVal, edFormDmxVal, "DMX VAL:");
        edFormDmxVal.setInputRestrictions(3, "0123456789");

        // BPM multiplier combo
        formPanel.addAndMakeVisible(lblFormBpmMult);
        lblFormBpmMult.setText("BPM MULT:", juce::dontSendNotification);
        lblFormBpmMult.setFont(juce::Font(juce::FontOptions(getMonoFontName(), 9.0f, juce::Font::plain)));
        lblFormBpmMult.setColour(juce::Label::textColourId, textDim);
        lblFormBpmMult.setJustificationType(juce::Justification::centredLeft);

        formPanel.addAndMakeVisible(cmbFormBpmMult);
        cmbFormBpmMult.addItem("--  (off)",   1);   // bpmMultiplier = 0
        cmbFormBpmMult.addItem("x2  (double)",2);   // bpmMultiplier = 1
        cmbFormBpmMult.addItem("x4  (quad)",  3);   // bpmMultiplier = 2
        cmbFormBpmMult.addItem("/2  (half)",  4);   // bpmMultiplier = -1
        cmbFormBpmMult.addItem("/4  (quarter)",5);  // bpmMultiplier = -2
        cmbFormBpmMult.setSelectedId(1, juce::dontSendNotification);
        cmbFormBpmMult.setColour(juce::ComboBox::backgroundColourId, bgDarker);
        cmbFormBpmMult.setColour(juce::ComboBox::textColourId, textBright);
        cmbFormBpmMult.setColour(juce::ComboBox::outlineColourId, borderCol);

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
    void setActiveTrack(const juce::String& artist, const juce::String& title)
    {
        auto newKey = TrackMapEntry::makeKey(artist, title);
        if (activeTrackKey != newKey) { activeTrackKey = newKey; table.repaint(); }
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

        bool isActive = (!activeTrackKey.empty() && rows[(size_t)rowNumber]->key() == activeTrackKey);

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

        bool isActive = (!activeTrackKey.empty() && entry.key() == activeTrackKey);
        g.setColour(isActive ? accentGreen.brighter(0.3f) : textBright);
        g.setFont(juce::Font(juce::FontOptions(getMonoFontName(), 11.0f, juce::Font::plain)));

        juce::String text;
        switch (columnId)
        {
            case ColArtist:  text = entry.artist; break;
            case ColTitle:   text = entry.title; break;
            case ColOffset:  text = entry.timecodeOffset; break;
            case ColBpm:
            {
                // Show compact BPM multiplier indicator
                switch (entry.bpmMultiplier)
                {
                    case  1: text = "x2";  g.setColour(accentCyan.brighter(0.3f)); break;
                    case  2: text = "x4";  g.setColour(accentCyan.brighter(0.3f)); break;
                    case -1: text = "/2";  g.setColour(accentCyan.brighter(0.3f)); break;
                    case -2: text = "/4";  g.setColour(accentCyan.brighter(0.3f)); break;
                    default: text = "--";  break;
                }
                break;
            }
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
                    g.drawText(t, 4, 0, width - 8, height, juce::Justification::centredLeft, true);
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
    std::string activeTrackKey;  // artist|title key of currently playing track
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
    enum { ColArtist = 1, ColTitle, ColOffset, ColBpm, ColTrig, ColNotes };

    //--------------------------------------------------------------------------
    // UI components
    //--------------------------------------------------------------------------
    juce::TableListBox table { "TrackMapTable", this };

    juce::TextButton btnLearn, btnAdd, btnDelete, btnImport, btnExport;
    juce::Label lblLearnLayer;
    juce::ComboBox cmbLearnLayer;

    // Edit form
    juce::Component formPanel;
    static constexpr int kFormHeight = 300;   // track info + trigger section + buttons

    juce::Label      lblFormArtist, lblFormTitle, lblFormOffset, lblFormNotes;
    juce::TextEditor edFormArtist, edFormTitle, edFormOffset, edFormNotes;

    // MIDI trigger (Note, CC)
    juce::Label      lblFormMidiCh, lblFormMidiNote, lblFormMidiNoteVel;
    juce::Label      lblFormMidiCC, lblFormMidiCCVal;
    juce::TextEditor edFormMidiCh, edFormMidiNote, edFormMidiNoteVel;
    juce::TextEditor edFormMidiCC, edFormMidiCCVal;

    // Trigger section header
    juce::Label      lblTriggerSection;

    // OSC trigger
    juce::Label      lblFormOscAddr, lblFormOscArgs;
    juce::TextEditor edFormOscAddr, edFormOscArgs;

    // Art-Net DMX trigger
    juce::Label      lblFormDmxCh, lblFormDmxVal;
    juce::TextEditor edFormDmxCh, edFormDmxVal;

    // BPM multiplier (per-track)
    juce::Label    lblFormBpmMult;
    juce::ComboBox cmbFormBpmMult;

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
        if (!activeTrackKey.empty())
        {
            // Find the entry to show artist - title
            for (auto& [k, entry] : trackMap.getEntries())
            {
                if (k == activeTrackKey)
                {
                    s += " | Active: " + entry.artist + " - " + entry.title + " (MAPPED)";
                    break;
                }
            }
            if (!s.contains("MAPPED"))
                s += " | Active track (UNMAPPED)";
        }
        lblStatus.setText(s, juce::dontSendNotification);
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

        // Row 1: Artist, Title
        auto row1 = area.removeFromTop(rowH);
        {
            int halfW = (row1.getWidth() - 8) / 2;
            auto seg = row1.removeFromLeft(halfW);
            lblFormArtist.setBounds(seg.removeFromTop(labelH));
            edFormArtist.setBounds(seg);
            row1.removeFromLeft(8);

            lblFormTitle.setBounds(row1.removeFromTop(labelH));
            edFormTitle.setBounds(row1);
        }

        area.removeFromTop(4);

        // Row 2: Offset, Notes
        auto row2 = area.removeFromTop(rowH);
        {
            auto seg = row2.removeFromLeft(160);
            lblFormOffset.setBounds(seg.removeFromTop(labelH));
            edFormOffset.setBounds(seg);
            row2.removeFromLeft(8);

            lblFormNotes.setBounds(row2.removeFromTop(labelH));
            edFormNotes.setBounds(row2);
        }

        area.removeFromTop(6);

        // ---- Trigger section header ----
        lblTriggerSection.setBounds(area.removeFromTop(16));

        area.removeFromTop(4);

        // Row 4: MIDI Trigger: Ch, Note, Vel, CC#, CCVal
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

        // Row 6: Art-Net DMX trigger + BPM Multiplier
        auto row6 = area.removeFromTop(rowH);
        {
            auto seg = row6.removeFromLeft(80);
            lblFormDmxCh.setBounds(seg.removeFromTop(labelH));
            edFormDmxCh.setBounds(seg);
            row6.removeFromLeft(8);

            seg = row6.removeFromLeft(80);
            lblFormDmxVal.setBounds(seg.removeFromTop(labelH));
            edFormDmxVal.setBounds(seg);
            row6.removeFromLeft(16);

            seg = row6.removeFromLeft(180);
            lblFormBpmMult.setBounds(seg.removeFromTop(labelH));
            cmbFormBpmMult.setBounds(seg);
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

        edFormArtist.setText(entry.artist, false);
        edFormArtist.setReadOnly(true);
        edFormTitle.setText(entry.title, false);
        edFormTitle.setReadOnly(true);
        edFormOffset.setText(entry.timecodeOffset, false);
        edFormNotes.setText(entry.notes, false);

        // MIDI trigger (independent fields)
        edFormMidiCh.setText(juce::String(entry.midiChannel + 1), false);   // display 1-16
        edFormMidiNote.setText(entry.midiNoteNum >= 0 ? juce::String(entry.midiNoteNum) : "", false);
        edFormMidiNoteVel.setText(juce::String(entry.midiNoteVel), false);
        edFormMidiCC.setText(entry.midiCCNum >= 0 ? juce::String(entry.midiCCNum) : "", false);
        edFormMidiCCVal.setText(juce::String(entry.midiCCVal), false);

        // OSC trigger
        edFormOscAddr.setText(entry.oscAddress, false);
        edFormOscArgs.setText(entry.oscArgs, false);

        // Art-Net DMX trigger
        edFormDmxCh.setText(entry.artnetCh > 0 ? juce::String(entry.artnetCh) : "", false);
        edFormDmxVal.setText(juce::String(entry.artnetVal), false);

        // BPM multiplier
        {
            int comboId = 1; // default: off
            if      (entry.bpmMultiplier ==  1) comboId = 2;
            else if (entry.bpmMultiplier ==  2) comboId = 3;
            else if (entry.bpmMultiplier == -1) comboId = 4;
            else if (entry.bpmMultiplier == -2) comboId = 5;
            cmbFormBpmMult.setSelectedId(comboId, juce::dontSendNotification);
        }

        formPanel.setVisible(true);
        resized();
        edFormOffset.grabKeyboardFocus();
    }

    void openFormForNew(const juce::String& artist = {},
                        const juce::String& title = {})
    {
        editingRow = -1;

        edFormArtist.setText(artist, false);
        edFormArtist.setReadOnly(false);
        edFormTitle.setText(title, false);
        edFormTitle.setReadOnly(false);
        edFormOffset.setText("00:00:00:00", false);
        edFormNotes.setText("", false);

        // MIDI trigger defaults (all disabled)
        edFormMidiCh.setText("1", false);
        edFormMidiNote.setText("", false);
        edFormMidiNoteVel.setText("127", false);
        edFormMidiCC.setText("", false);
        edFormMidiCCVal.setText("127", false);

        // OSC trigger defaults
        edFormOscAddr.setText("", false);
        edFormOscArgs.setText("", false);

        // Art-Net DMX trigger defaults
        edFormDmxCh.setText("", false);
        edFormDmxVal.setText("255", false);

        // BPM multiplier default (off)
        cmbFormBpmMult.setSelectedId(1, juce::dontSendNotification);

        formPanel.setVisible(true);
        resized();

        if (artist.isNotEmpty())
            edFormOffset.grabKeyboardFocus();
        else
            edFormArtist.grabKeyboardFocus();
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
        // Validate artist + title (required as composite key)
        juce::String formArtist = edFormArtist.getText().trim();
        juce::String formTitle  = edFormTitle.getText().trim();
        if (formArtist.isEmpty() || formTitle.isEmpty())
        {
            if (formArtist.isEmpty()) edFormArtist.grabKeyboardFocus();
            else                      edFormTitle.grabKeyboardFocus();
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
        offset = TrackMapEntry::formatTimecodeString(h, m, s, f);

        // If editing an existing row whose artist/title changed, remove the old entry
        if (editingRow >= 0 && editingRow < (int)rows.size())
        {
            auto& oldEntry = *rows[(size_t)editingRow];
            if (TrackMapEntry::makeKey(formArtist, formTitle) != oldEntry.key())
                trackMap.remove(oldEntry.artist, oldEntry.title);
        }

        TrackMapEntry entry;
        entry.artist         = formArtist;
        entry.title          = formTitle;
        entry.timecodeOffset = offset;
        entry.notes          = edFormNotes.getText().trim();

        // MIDI trigger
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

        // BPM multiplier
        {
            int comboId = cmbFormBpmMult.getSelectedId();
            if      (comboId == 2) entry.bpmMultiplier =  1;
            else if (comboId == 3) entry.bpmMultiplier =  2;
            else if (comboId == 4) entry.bpmMultiplier = -1;
            else if (comboId == 5) entry.bpmMultiplier = -2;
            else                   entry.bpmMultiplier =  0;
        }

        auto savedKey = entry.key();
        trackMap.addOrUpdate(entry);
        closeForm();
        notifyChanged();

        // Select the saved row
        for (int i = 0; i < (int)rows.size(); ++i)
        {
            if (rows[(size_t)i]->key() == savedKey)
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

        int player = cmbLearnLayer.getSelectedId();
        uint32_t cdjId = proDJLinkInput->getTrackID(player);
        if (cdjId == 0) return;

        auto tinfo = proDJLinkInput->getTrackInfo(player);

        // Enrich with dbClient cache if available
        if (dbClient != nullptr)
        {
            auto meta = dbClient->getCachedMetadataByTrackId(cdjId);
            if (meta.isValid())
            {
                tinfo.artist = meta.artist;
                tinfo.title  = meta.title;
            }
        }

        if (tinfo.artist.isEmpty() || tinfo.title.isEmpty()
            || tinfo.title.startsWith("Track #"))
            return;  // can't learn without real metadata

        // If entry already exists, open it for editing
        if (trackMap.contains(tinfo.artist, tinfo.title))
        {
            rebuildRows();
            table.updateContent();
            auto targetKey = TrackMapEntry::makeKey(tinfo.artist, tinfo.title);
            for (int i = 0; i < (int)rows.size(); ++i)
            {
                if (rows[(size_t)i]->key() == targetKey)
                {
                    table.selectRow(i);
                    openFormForRow(i);
                    break;
                }
            }
        }
        else
        {
            openFormForNew(tinfo.artist, tinfo.title);
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
        jassert(rowsGeneration == trackMap.getGeneration());

        const auto& entry = *rows[(size_t)selected];
        juce::String deleteArtist = entry.artist;
        juce::String deleteTitle  = entry.title;
        juce::String msg = "Delete \"" + deleteArtist + " - " + deleteTitle + "\"?";

        auto options = juce::MessageBoxOptions()
            .withIconType(juce::MessageBoxIconType::QuestionIcon)
            .withTitle("Delete Track")
            .withMessage(msg)
            .withButton("Delete")
            .withButton("Cancel");

        deleteConfirmBox = juce::AlertWindow::showScopedAsync(options,
            [this, deleteArtist, deleteTitle](int result)
            {
                if (result == 1)
                {
                    trackMap.remove(deleteArtist, deleteTitle);
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
                if (e.hasValidKey())
                    entries.push_back(std::move(e));
            }
            if (entries.empty()) return;

            // Sort by artist then title
            std::sort(entries.begin(), entries.end(),
                [](const TrackMapEntry& a, const TrackMapEntry& b) {
                    int cmp = a.artist.compareIgnoreCase(b.artist);
                    return cmp != 0 ? cmp < 0 : a.title.compareIgnoreCase(b.title) < 0;
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
                isDuplicate.push_back(existingMap.contains(items[i].artist, items[i].title));

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
            hdr.addColumn("Artist",    2, 160, 60, 250, juce::TableHeaderComponent::notSortable);
            hdr.addColumn("Title",     3, 170, 60, 300, juce::TableHeaderComponent::notSortable);
            hdr.addColumn("Offset",    4,  90, 70, 110, juce::TableHeaderComponent::notSortable);
            hdr.addColumn("Status",    5,  80, 60, 100, juce::TableHeaderComponent::notSortable);

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
                    g.drawText(e.artist, 4, 0, w - 8, h, juce::Justification::centredLeft, true);
                    break;
                case 3:
                    g.setColour(sel ? juce::Colour(0xFFCFD8DC) : juce::Colour(0xFF546E7A));
                    g.drawText(e.title, 4, 0, w - 8, h, juce::Justification::centredLeft, true);
                    break;
                case 4:
                    g.setColour(sel ? juce::Colour(0xFFCFD8DC) : juce::Colour(0xFF546E7A));
                    g.drawText(e.timecodeOffset, 4, 0, w - 8, h, juce::Justification::centredLeft);
                    break;
                case 5:
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
        opts.useNativeTitleBar = false;
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
