// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter
//
// CuePointEditor -- Table editor for cue points within a single track.
//
// Allows the user to create, edit, and delete cue points that fire
// MIDI/OSC/ArtNet triggers at specific playhead positions during
// playback.  Designed to be opened from the TrackMapEditor for a
// specific TrackMapEntry.
//
// LIFETIME CONTRACT: This class holds a mutable reference to a
// TrackMapEntry that lives inside TrackMap's unordered_map.  Any
// mutation of the TrackMap (add/remove/import) can rehash the map
// and invalidate this reference.  MainComponent MUST close this
// editor before or immediately after any TrackMap mutation.  The
// current architecture guarantees this because both TrackMapEditor
// and CuePointEditor callbacks run synchronously on the message
// thread, and TrackMapEditor::onChange closes this editor before
// proceeding.
//
// Key feature: "Capture" button reads the current playhead from the
// active engine and creates a cue at that position.  This is the
// primary workflow -- play the track on a CDJ/Denon, press Capture
// at each key moment (break, drop, intro lights, etc.)

#pragma once
#include <JuceHeader.h>
#include "AppSettings.h"
#include "CustomLookAndFeel.h"

//==============================================================================
// CueWaveformStrip -- Horizontal waveform display with playback + edit cursors
//==============================================================================
class CueWaveformStrip : public juce::Component
{
public:
    CueWaveformStrip() { setOpaque(false); }

    void setWaveformData(const std::vector<uint8_t>& data, int entryCount, int bytesPerEntry)
    {
        wfData = data;
        wfEntries = entryCount;
        wfBytesPerEntry = bytesPerEntry;
        hasData = (entryCount > 0 && (int)data.size() >= entryCount * bytesPerEntry);
        repaint();
    }

    void setDurationMs(uint32_t ms)   { durationMs = ms; repaint(); }
    void setPlayheadMs(uint32_t ms)   { playheadMs = ms; repaint(); }
    uint32_t getEditCursorMs() const  { return editCursorSet ? editCursorMs : 0; }
    bool hasEditCursor() const        { return editCursorSet; }
    void clearEditCursor()            { editCursorSet = false; repaint(); }

    void setCuePositions(const std::vector<uint32_t>& positions)
    {
        cuePositions = positions;
        repaint();
    }

    void setSelectedCueIndex(int idx)
    {
        selectedCue = idx;
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        g.setColour(juce::Colour(0xFF0D1117));
        g.fillRoundedRectangle(b, 3.0f);

        float inset = 2.0f;
        float drawW = b.getWidth() - inset * 2;
        float drawH = b.getHeight() - inset * 2;
        float midY = inset + drawH * 0.5f;

        // Draw waveform bars
        if (hasData && wfEntries > 0 && drawW > 0)
        {
            float barW = std::max(1.0f, drawW / (float)wfEntries);
            const uint8_t* raw = wfData.data();

            // Find peak for normalization
            uint8_t peak = 1;
            for (int i = 0; i < wfEntries; ++i)
            {
                int off = i * wfBytesPerEntry;
                for (int j = 0; j < std::min(wfBytesPerEntry, 3); ++j)
                    if (raw[off + j] > peak) peak = raw[off + j];
            }
            float norm = drawH * 0.45f / (float)peak;

            for (int i = 0; i < wfEntries; ++i)
            {
                int off = i * wfBytesPerEntry;
                float x = inset + (float)i / (float)wfEntries * drawW;

                juce::Colour col;
                float h;
                if (wfBytesPerEntry == 3)
                {
                    // ThreeBand: mid=blue, high=cyan, low=purple
                    uint8_t mid = raw[off], hi = raw[off+1], lo = raw[off+2];
                    h = std::max({(float)mid, (float)hi, (float)lo}) * norm;
                    float r = (float)lo * 0.6f / 255.0f;
                    float gr = (float)hi * 0.8f / 255.0f;
                    float bl = (float)mid / 255.0f;
                    col = juce::Colour::fromFloatRGBA(0.2f + r * 0.5f, 0.3f + gr * 0.5f, 0.5f + bl * 0.5f, 0.85f);
                }
                else if (wfBytesPerEntry >= 6)
                {
                    // ColorNxs2: d3=R, d4=G, d5=B, height from d0-d2
                    uint8_t d0 = raw[off], d1 = raw[off+1], d2 = raw[off+2];
                    h = std::max({(float)d0, (float)d1, (float)d2}) * norm;
                    col = juce::Colour(raw[off+3], raw[off+4], raw[off+5]).withAlpha(0.85f);
                }
                else
                {
                    continue;  // unknown format -- skip
                }

                h = std::max(h, 1.0f);
                g.setColour(col);
                g.fillRect(x, midY - h, barW, h * 2);
            }
        }
        else
        {
            g.setColour(juce::Colour(0xFF444444));
            g.drawText("No Waveform", b, juce::Justification::centred);
        }

        // Center line
        g.setColour(juce::Colour(0x30FFFFFF));
        g.drawHorizontalLine((int)midY, inset, b.getWidth() - inset);

        if (durationMs == 0) return;

        // Cue markers (yellow vertical lines, selected = white + thicker)
        for (int ci = 0; ci < (int)cuePositions.size(); ++ci)
        {
            float ratio = (float)cuePositions[(size_t)ci] / (float)durationMs;
            float cx = inset + ratio * drawW;
            if (ci == selectedCue)
            {
                g.setColour(juce::Colour(0xFFFFFFFF));
                g.fillRect(cx - 1.0f, 0.0f, 3.0f, b.getHeight());
            }
            else
            {
                g.setColour(juce::Colour(0xCCFFCC00));
                g.drawVerticalLine((int)cx, inset, b.getHeight() - inset);
            }
        }

        // Edit cursor (cyan, dashed look with wider line)
        if (editCursorSet)
        {
            float ratio = (float)editCursorMs / (float)durationMs;
            float cx = inset + ratio * drawW;
            g.setColour(juce::Colour(0xFF00DDFF));
            g.fillRect(cx - 1.0f, 0.0f, 3.0f, b.getHeight());
        }

        // Playhead cursor (red)
        {
            float ratio = (float)playheadMs / (float)durationMs;
            float cx = inset + ratio * drawW;
            g.setColour(juce::Colour(0xFFFF3333));
            g.fillRect(cx - 0.5f, 0.0f, 2.0f, b.getHeight());
        }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (durationMs == 0) return;
        float inset = 2.0f;
        float drawW = (float)getWidth() - inset * 2;
        float relX = ((float)e.getPosition().getX() - inset) / drawW;
        relX = juce::jlimit(0.0f, 1.0f, relX);
        editCursorMs = (uint32_t)(relX * (float)durationMs);
        editCursorSet = true;
        repaint();
        if (onEditCursorChanged) onEditCursorChanged(editCursorMs);
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (durationMs == 0) return;
        float inset = 2.0f;
        float drawW = (float)getWidth() - inset * 2;
        float relX = ((float)e.getPosition().getX() - inset) / drawW;
        relX = juce::jlimit(0.0f, 1.0f, relX);
        editCursorMs = (uint32_t)(relX * (float)durationMs);
        editCursorSet = true;
        repaint();
        if (onEditCursorChanged) onEditCursorChanged(editCursorMs);
    }

    std::function<void(uint32_t)> onEditCursorChanged;

private:
    std::vector<uint8_t> wfData;
    int wfEntries = 0, wfBytesPerEntry = 0;
    bool hasData = false;
    uint32_t durationMs = 0;
    uint32_t playheadMs = 0;
    uint32_t editCursorMs = 0;
    bool editCursorSet = false;
    std::vector<uint32_t> cuePositions;
    int selectedCue = -1;
};

//==============================================================================
class CuePointEditor : public juce::Component,
                       public juce::TableListBoxModel,
                       public juce::Timer
{
public:
    CuePointEditor(TrackMapEntry& entry)
        : trackEntry(entry)
    {
        setSize(680, 480);

        // Colours
        bgPanel = juce::Colour(0xFF1A1D23);
        bgRow   = juce::Colour(0xFF22252B);
        bgRowAlt = juce::Colour(0xFF1E2128);
        textLight = juce::Colour(0xFFE0E0E0);
        textDim   = juce::Colour(0xFF888888);
        accentBlue = juce::Colour(0xFF00AAFF);
        accentGreen = juce::Colour(0xFF44CC44);
        accentRed   = juce::Colour(0xFFFF4444);

        // --- Header ---
        addAndMakeVisible(lblTrackInfo);
        lblTrackInfo.setText(entry.artist + " - " + entry.title, juce::dontSendNotification);
        lblTrackInfo.setColour(juce::Label::textColourId, textLight);
        lblTrackInfo.setFont(juce::Font(juce::FontOptions(14.0f).withStyle("Bold")));

        addAndMakeVisible(lblCueCount);
        lblCueCount.setColour(juce::Label::textColourId, textDim);
        lblCueCount.setFont(juce::Font(juce::FontOptions(11.0f)));

        // --- Waveform strip ---
        addAndMakeVisible(waveformStrip);
        waveformStrip.onEditCursorChanged = [this](uint32_t ms)
        {
            // Update position display in status
            editCursorMs = ms;
            editCursorActive = true;
        };

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
        hdr.addColumn("#",          ColNum,       32,  28,  40, juce::TableHeaderComponent::notSortable);
        hdr.addColumn("Position",   ColPosition,  90,  70, 110, juce::TableHeaderComponent::notSortable);
        hdr.addColumn("Name",       ColName,     140,  80, 300, juce::TableHeaderComponent::notSortable);
        hdr.addColumn("MIDI",       ColMidi,     100,  60, 160, juce::TableHeaderComponent::notSortable);
        hdr.addColumn("OSC",        ColOsc,      110,  60, 200, juce::TableHeaderComponent::notSortable);
        hdr.addColumn("DMX",        ColDmx,       60,  40,  90, juce::TableHeaderComponent::notSortable);

        // --- Buttons ---
        auto setupBtn = [&](juce::TextButton& btn, const juce::String& text,
                            juce::Colour bgCol, juce::Colour txtCol)
        {
            addAndMakeVisible(btn);
            btn.setButtonText(text);
            btn.setColour(juce::TextButton::buttonColourId, bgCol);
            btn.setColour(juce::TextButton::textColourOffId, txtCol);
        };

        setupBtn(btnCapture, "Capture", accentGreen.withAlpha(0.2f), accentGreen);
        setupBtn(btnAdd,     "Add",     bgRow, textLight);
        setupBtn(btnDelete,  "Delete",  bgRow, accentRed);

        btnCapture.onClick = [this]
        {
            uint32_t ms = 0;

            // Use edit cursor if set (clicked on waveform), else live playhead
            if (editCursorActive && editCursorMs > 0)
            {
                ms = editCursorMs;
            }
            else if (onCapturePlayhead)
            {
                ms = onCapturePlayhead();
            }
            if (ms == 0) return;

            CuePoint cp;
            cp.positionMs = ms;
            cp.name = "CUE " + juce::String(trackEntry.cuePoints.size() + 1);
            trackEntry.cuePoints.push_back(std::move(cp));
            trackEntry.sortCuePoints();
            table.updateContent();
            table.repaint();
            updateCueCount();
            updateWaveformCueMarkers();
            // Clear edit cursor after capture
            editCursorActive = false;
            waveformStrip.clearEditCursor();
            if (onChange) onChange();
        };

        btnAdd.onClick = [this]
        {
            CuePoint cp;
            cp.positionMs = editCursorActive ? editCursorMs : 0;
            cp.name = "CUE " + juce::String(trackEntry.cuePoints.size() + 1);
            trackEntry.cuePoints.push_back(std::move(cp));
            trackEntry.sortCuePoints();
            table.updateContent();
            table.repaint();
            updateCueCount();
            updateWaveformCueMarkers();
            if (onChange) onChange();
        };

        btnDelete.onClick = [this]
        {
            auto selected = table.getSelectedRows();
            if (selected.isEmpty()) return;

            // Collect indices and erase in reverse order to avoid index shifting
            std::vector<int> indices;
            for (int i = 0; i < selected.size(); ++i)
                indices.push_back(selected[i]);
            std::sort(indices.rbegin(), indices.rend());

            for (int idx : indices)
            {
                if (idx >= 0 && idx < (int)trackEntry.cuePoints.size())
                    trackEntry.cuePoints.erase(trackEntry.cuePoints.begin() + idx);
            }

            table.deselectAllRows();
            table.updateContent();
            table.repaint();
            updateCueCount();
            updateWaveformCueMarkers();
            selectedCue = -1;
            waveformStrip.setSelectedCueIndex(-1);
            detailPanel.setVisible(false);
            resized();
            if (onChange) onChange();
        };

        // --- Detail panel (shows when a cue is selected) ---
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

        addLbl(lblPosition, "POSITION (MM:SS.mmm):");
        addEd(edPosition);
        addLbl(lblName, "NAME:");
        addEd(edName);

        addLbl(lblMidiCh, "MIDI CH:");    addEd(edMidiCh);
        addLbl(lblMidiNote, "NOTE:");      addEd(edMidiNote);
        addLbl(lblMidiVel, "VEL:");        addEd(edMidiVel);
        addLbl(lblMidiCC, "CC#:");         addEd(edMidiCC);
        addLbl(lblMidiCCVal, "CC VAL:");   addEd(edMidiCCVal);

        addLbl(lblOscAddr, "OSC ADDRESS:");  addEd(edOscAddr);
        addLbl(lblOscArgs, "OSC ARGS:");     addEd(edOscArgs);

        addLbl(lblDmxCh, "DMX CH:");        addEd(edDmxCh);
        addLbl(lblDmxVal, "DMX VAL:");       addEd(edDmxVal);

        // Apply button for detail edits
        setupBtn(btnApply, "Apply", accentBlue.withAlpha(0.2f), accentBlue);
        detailPanel.addAndMakeVisible(btnApply);
        btnApply.onClick = [this] { applyDetailEdits(); };

        updateCueCount();
        updateWaveformCueMarkers();
        startTimer(33);  // ~30Hz for playhead cursor updates
    }

    ~CuePointEditor() override { stopTimer(); }

    //--------------------------------------------------------------------------
    // Callbacks
    //--------------------------------------------------------------------------

    /// Set album artwork image (call after construction, before display)
    void setArtwork(const juce::Image& img)
    {
        artworkImage = img;
        repaint();
    }

    /// Set waveform data for the strip display
    void setWaveformData(const std::vector<uint8_t>& data, int entryCount, int bytesPerEntry)
    {
        waveformStrip.setWaveformData(data, entryCount, bytesPerEntry);
    }

    /// Set track duration for cursor positioning
    void setDurationMs(uint32_t ms)
    {
        trackDurationMs = ms;
        waveformStrip.setDurationMs(ms);
    }

    /// Called when cue points are modified (add/delete/edit).
    /// Caller should persist TrackMap and refresh engine cue state.
    std::function<void()> onChange;

    /// Called by Capture button to get current playhead in ms.
    /// Returns 0 if no playhead available.
    std::function<uint32_t()> onCapturePlayhead;

    /// Refresh table (e.g. after external changes)
    void refresh()
    {
        table.updateContent();
        table.repaint();
        updateCueCount();
        updateWaveformCueMarkers();
    }

    //--------------------------------------------------------------------------
    // Timer -- updates playhead cursor on waveform at ~30Hz
    //--------------------------------------------------------------------------
    void timerCallback() override
    {
        if (onCapturePlayhead && trackDurationMs > 0)
        {
            uint32_t ms = onCapturePlayhead();
            waveformStrip.setPlayheadMs(ms);

            // Any playhead movement (play, jog, scrub) clears the edit cursor
            // so Capture uses the live position instead of the stale click point.
            if (editCursorActive && ms != lastPlayheadMs)
            {
                editCursorActive = false;
                editCursorMs = 0;
                waveformStrip.clearEditCursor();
            }
            lastPlayheadMs = ms;
        }
    }

    //--------------------------------------------------------------------------
    // TableListBoxModel
    //--------------------------------------------------------------------------
    int getNumRows() override { return (int)trackEntry.cuePoints.size(); }

    void paintRowBackground(juce::Graphics& g, int rowNumber,
                            int /*width*/, int /*height*/, bool isSelected) override
    {
        g.fillAll(isSelected ? accentBlue.withAlpha(0.15f)
                             : ((rowNumber % 2 == 0) ? bgRow : bgRowAlt));
    }

    void paintCell(juce::Graphics& g, int rowNumber, int columnId,
                   int width, int height, bool /*isSelected*/) override
    {
        if (rowNumber < 0 || rowNumber >= (int)trackEntry.cuePoints.size()) return;
        auto& cue = trackEntry.cuePoints[(size_t)rowNumber];

        g.setColour(textLight);
        g.setFont(juce::Font(juce::FontOptions(11.0f)));
        int pad = 4;

        juce::String text;
        switch (columnId)
        {
            case ColNum:
                text = juce::String(rowNumber + 1);
                break;
            case ColPosition:
                text = CuePoint::formatPositionMs(cue.positionMs);
                break;
            case ColName:
                text = cue.name;
                break;
            case ColMidi:
            {
                juce::StringArray parts;
                if (cue.midiNoteNum >= 0)
                    parts.add("N" + juce::String(cue.midiNoteNum));
                if (cue.midiCCNum >= 0)
                    parts.add("CC" + juce::String(cue.midiCCNum));
                if (parts.isEmpty())
                    { g.setColour(textDim); text = "-"; }
                else
                    text = "Ch" + juce::String(cue.midiChannel + 1) + " " + parts.joinIntoString(" ");
                break;
            }
            case ColOsc:
                if (cue.oscAddress.isEmpty())
                    { g.setColour(textDim); text = "-"; }
                else
                    text = cue.oscAddress;
                break;
            case ColDmx:
                if (cue.artnetCh <= 0)
                    { g.setColour(textDim); text = "-"; }
                else
                    text = juce::String(cue.artnetCh) + "=" + juce::String(cue.artnetVal);
                break;
            default: break;
        }
        g.drawText(text, pad, 0, width - pad * 2, height, juce::Justification::centredLeft);
    }

    void selectedRowsChanged(int /*lastRowSelected*/) override
    {
        auto selected = table.getSelectedRows();

        // Show detail panel only when exactly one cue is selected
        if (selected.size() == 1)
        {
            int row = selected[0];
            if (row >= 0 && row < (int)trackEntry.cuePoints.size())
            {
                selectedCue = row;
                loadDetailFromCue(trackEntry.cuePoints[(size_t)selectedCue]);
                detailPanel.setVisible(true);
                waveformStrip.setSelectedCueIndex(row);
            }
            else
            {
                selectedCue = -1;
                detailPanel.setVisible(false);
                waveformStrip.setSelectedCueIndex(-1);
            }
        }
        else
        {
            selectedCue = -1;
            detailPanel.setVisible(false);
            waveformStrip.setSelectedCueIndex(-1);
        }
        resized();
    }

    //--------------------------------------------------------------------------
    // Layout
    //--------------------------------------------------------------------------
    void resized() override
    {
        auto b = getLocalBounds().reduced(8);

        // Header with artwork
        int artSize = artworkImage.isValid() ? 48 : 0;
        int headerH = juce::jmax(20, artSize);
        auto headerArea = b.removeFromTop(headerH);
        if (artworkImage.isValid())
        {
            artworkBounds = headerArea.removeFromLeft(artSize);
            headerArea.removeFromLeft(8);
        }
        else
        {
            artworkBounds = {};
        }
        lblTrackInfo.setBounds(headerArea.removeFromTop(headerH / 2));
        lblCueCount.setBounds(headerArea);
        b.removeFromTop(4);

        // Buttons
        auto btnArea = b.removeFromTop(30);
        int bw = 80, bg = 6;
        btnCapture.setBounds(btnArea.removeFromLeft(bw)); btnArea.removeFromLeft(bg);
        btnAdd.setBounds(btnArea.removeFromLeft(bw));     btnArea.removeFromLeft(bg);
        btnDelete.setBounds(btnArea.removeFromLeft(bw));
        b.removeFromTop(4);

        // Waveform strip
        waveformStrip.setBounds(b.removeFromTop(56));
        b.removeFromTop(4);

        // Detail panel (bottom, if visible)
        int detailH = detailPanel.isVisible() ? 150 : 0;
        auto detailArea = b.removeFromBottom(detailH);
        if (detailPanel.isVisible())
        {
            b.removeFromBottom(4);
            detailPanel.setBounds(detailArea);
            layoutDetailPanel(detailArea.withZeroOrigin());
        }

        // Table fills remaining space
        table.setBounds(b);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(bgPanel);

        if (artworkImage.isValid() && !artworkBounds.isEmpty())
        {
            g.drawImage(artworkImage, artworkBounds.toFloat(),
                        juce::RectanglePlacement::centred);
            g.setColour(juce::Colour(0xFF333333));
            g.drawRect(artworkBounds, 1);
        }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        // Click anywhere outside the waveform strip clears the edit cursor,
        // so Capture reverts to using the live playhead position.
        if (!waveformStrip.getBounds().contains(e.getPosition()))
        {
            if (editCursorActive)
            {
                editCursorActive = false;
                editCursorMs = 0;
                waveformStrip.clearEditCursor();
            }
        }
    }

private:
    TrackMapEntry& trackEntry;
    int selectedCue = -1;
    juce::Image artworkImage;
    juce::Rectangle<int> artworkBounds;

    // Waveform strip with dual cursors
    CueWaveformStrip waveformStrip;
    uint32_t trackDurationMs = 0;
    uint32_t editCursorMs = 0;
    bool editCursorActive = false;
    uint32_t lastPlayheadMs = 0;   // for detecting playhead movement (jog/scrub/play)

    // Table
    juce::TableListBox table { "CuePoints", this };
    enum { ColNum = 1, ColPosition, ColName, ColMidi, ColOsc, ColDmx };

    // Header
    juce::Label lblTrackInfo, lblCueCount;

    // Buttons
    juce::TextButton btnCapture, btnAdd, btnDelete;

    // Detail panel
    juce::Component detailPanel;
    juce::Label lblPosition, lblName;
    juce::Label lblMidiCh, lblMidiNote, lblMidiVel, lblMidiCC, lblMidiCCVal;
    juce::Label lblOscAddr, lblOscArgs;
    juce::Label lblDmxCh, lblDmxVal;

    juce::TextEditor edPosition, edName;
    juce::TextEditor edMidiCh, edMidiNote, edMidiVel, edMidiCC, edMidiCCVal;
    juce::TextEditor edOscAddr, edOscArgs;
    juce::TextEditor edDmxCh, edDmxVal;
    juce::TextButton btnApply;

    // Colours
    juce::Colour bgPanel, bgRow, bgRowAlt, textLight, textDim;
    juce::Colour accentBlue, accentGreen, accentRed;

    //--------------------------------------------------------------------------
    void updateCueCount()
    {
        lblCueCount.setText(juce::String(trackEntry.cuePoints.size()) + " cue points",
                            juce::dontSendNotification);
    }

    void updateWaveformCueMarkers()
    {
        std::vector<uint32_t> positions;
        positions.reserve(trackEntry.cuePoints.size());
        for (auto& cp : trackEntry.cuePoints)
            positions.push_back(cp.positionMs);
        waveformStrip.setCuePositions(positions);
    }

    void loadDetailFromCue(const CuePoint& cue)
    {
        edPosition.setText(CuePoint::formatPositionMs(cue.positionMs), false);
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
        if (selectedCue < 0 || selectedCue >= (int)trackEntry.cuePoints.size()) return;
        auto& cue = trackEntry.cuePoints[(size_t)selectedCue];

        // Parse position MM:SS.mmm
        {
            auto txt = edPosition.getText().trim();
            auto parts = juce::StringArray::fromTokens(txt, ":.", "");
            if (parts.size() >= 2)
            {
                int mins = juce::jmax(0, parts[0].getIntValue());
                int secs = juce::jmax(0, parts[1].getIntValue());
                int ms   = (parts.size() >= 3) ? juce::jmax(0, parts[2].getIntValue()) : 0;
                // Pad ms if user typed e.g. "1:23.5" meaning 500ms
                if (parts.size() >= 3 && parts[2].length() == 1) ms *= 100;
                else if (parts.size() >= 3 && parts[2].length() == 2) ms *= 10;
                cue.positionMs = (uint32_t)(mins * 60000 + secs * 1000 + ms);
            }
        }

        cue.name = edName.getText().trim();

        // MIDI
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

        // OSC
        cue.oscAddress = edOscAddr.getText().trim();
        cue.oscArgs    = edOscArgs.getText().trim();

        // ArtNet
        {
            auto chTxt = edDmxCh.getText().trim();
            cue.artnetCh  = chTxt.isEmpty() ? 0 : juce::jlimit(1, 512, chTxt.getIntValue());
        }
        cue.artnetVal = juce::jlimit(0, 255, edDmxVal.getText().getIntValue());

        // Re-sort in case position changed.
        // Snapshot full cue state so we can find it after sort even when
        // duplicates exist (position + name alone can be ambiguous).
        CuePoint editedSnapshot = cue;  // value copy after edits applied
        trackEntry.sortCuePoints();

        // Update selectedCue to follow the cue to its new position after sort.
        // Compare all trigger fields to disambiguate true duplicates.
        for (int i = 0; i < (int)trackEntry.cuePoints.size(); ++i)
        {
            auto& c = trackEntry.cuePoints[(size_t)i];
            if (c.positionMs   == editedSnapshot.positionMs
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
        updateCueCount();
        updateWaveformCueMarkers();
        if (onChange) onChange();
    }

    void layoutDetailPanel(juce::Rectangle<int> area)
    {
        area = area.reduced(6);
        int rowH = 22, gap = 4;

        // Row 1: Position + Name
        auto row = area.removeFromTop(rowH);
        lblPosition.setBounds(row.removeFromLeft(145)); row.removeFromLeft(gap);
        edPosition.setBounds(row.removeFromLeft(90)); row.removeFromLeft(gap + 10);
        lblName.setBounds(row.removeFromLeft(40)); row.removeFromLeft(gap);
        edName.setBounds(row);
        area.removeFromTop(gap);

        // Row 2: MIDI
        row = area.removeFromTop(rowH);
        lblMidiCh.setBounds(row.removeFromLeft(50)); row.removeFromLeft(2);
        edMidiCh.setBounds(row.removeFromLeft(35)); row.removeFromLeft(gap);
        lblMidiNote.setBounds(row.removeFromLeft(38)); row.removeFromLeft(2);
        edMidiNote.setBounds(row.removeFromLeft(35)); row.removeFromLeft(gap);
        lblMidiVel.setBounds(row.removeFromLeft(28)); row.removeFromLeft(2);
        edMidiVel.setBounds(row.removeFromLeft(35)); row.removeFromLeft(gap);
        lblMidiCC.setBounds(row.removeFromLeft(30)); row.removeFromLeft(2);
        edMidiCC.setBounds(row.removeFromLeft(35)); row.removeFromLeft(gap);
        lblMidiCCVal.setBounds(row.removeFromLeft(50)); row.removeFromLeft(2);
        edMidiCCVal.setBounds(row.removeFromLeft(35));
        area.removeFromTop(gap);

        // Row 3: OSC
        row = area.removeFromTop(rowH);
        lblOscAddr.setBounds(row.removeFromLeft(90)); row.removeFromLeft(2);
        edOscAddr.setBounds(row.removeFromLeft(200)); row.removeFromLeft(gap);
        lblOscArgs.setBounds(row.removeFromLeft(60)); row.removeFromLeft(2);
        edOscArgs.setBounds(row);
        area.removeFromTop(gap);

        // Row 4: ArtNet + Apply
        row = area.removeFromTop(rowH);
        lblDmxCh.setBounds(row.removeFromLeft(50)); row.removeFromLeft(2);
        edDmxCh.setBounds(row.removeFromLeft(40)); row.removeFromLeft(gap);
        lblDmxVal.setBounds(row.removeFromLeft(55)); row.removeFromLeft(2);
        edDmxVal.setBounds(row.removeFromLeft(40)); row.removeFromLeft(gap + 20);
        btnApply.setBounds(row.removeFromLeft(80));
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CuePointEditor)
};

//==============================================================================
// CuePointEditorWindow -- DocumentWindow wrapper
//==============================================================================
class CuePointEditorWindow : public juce::DocumentWindow
{
public:
    CuePointEditorWindow(TrackMapEntry& entry)
        : DocumentWindow("Cue Points: " + entry.artist + " - " + entry.title,
                          juce::Colour(0xFF12141A),
                          DocumentWindow::closeButton | DocumentWindow::maximiseButton)
    {
        setUsingNativeTitleBar(false);
        setTitleBarHeight(20);
        setContentOwned(new CuePointEditor(entry), true);
        setResizable(true, true);
        setResizeLimits(500, 350, 4096, 2160);
        centreWithSize(getWidth(), getHeight());
        setVisible(true);
    }

    void closeButtonPressed() override
    {
        if (onBoundsCapture) onBoundsCapture();
        setVisible(false);
    }

    CuePointEditor* getEditor()
    {
        return dynamic_cast<CuePointEditor*>(getContentComponent());
    }

    void setOnChange(std::function<void()> cb)
    {
        if (auto* ed = getEditor()) ed->onChange = std::move(cb);
    }

    void setOnCapturePlayhead(std::function<uint32_t()> cb)
    {
        if (auto* ed = getEditor()) ed->onCapturePlayhead = std::move(cb);
    }

    void setArtwork(const juce::Image& img)
    {
        if (auto* ed = getEditor()) ed->setArtwork(img);
    }

    void setWaveformData(const std::vector<uint8_t>& data, int entryCount, int bytesPerEntry)
    {
        if (auto* ed = getEditor()) ed->setWaveformData(data, entryCount, bytesPerEntry);
    }

    void setDurationMs(uint32_t ms)
    {
        if (auto* ed = getEditor()) ed->setDurationMs(ms);
    }

    std::function<void()> onBoundsCapture;

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CuePointEditorWindow)
};
