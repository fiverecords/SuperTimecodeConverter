// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

#pragma once
#include <JuceHeader.h>
#include "AppSettings.h"

//==============================================================================
// GeneratorPresetEditor -- Table editor for named timecode generator presets.
//
// Each preset has a Name, Start TC, and Stop TC.  The editor mirrors the
// TrackMapEditor UX: table + form at the bottom for add/edit.
// Calls onChange() whenever the preset map is modified.
//==============================================================================
class GeneratorPresetEditor : public juce::Component,
                              public juce::TableListBoxModel
{
public:
    GeneratorPresetEditor(GeneratorPresetMap& map)
        : presetMap(map)
    {
        setSize(480, 360);
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
        hdr.addColumn("Name",     ColName,    160, 80, 300, juce::TableHeaderComponent::notSortable);
        hdr.addColumn("Start TC", ColStart,   120, 80, 180, juce::TableHeaderComponent::notSortable);
        hdr.addColumn("Stop TC",  ColStop,    120, 80, 180, juce::TableHeaderComponent::notSortable);

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
        addBtn(btnClearAll, "Clear All");
        addBtn(btnImport,   "Import");
        addBtn(btnExport,   "Export");
        addBtn(btnOscHelp,  "OSC ?");

        btnSave.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF22AA44).withAlpha(0.3f));
        btnSave.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFF22DD55));

        btnImport.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF2244AA).withAlpha(0.3f));
        btnImport.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFF4488FF));
        btnExport.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF2244AA).withAlpha(0.3f));
        btnExport.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFF4488FF));
        btnOscHelp.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF555555));
        btnOscHelp.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFFFCC44));

        btnAdd.onClick    = [this] { addNewPreset(); };
        btnSave.onClick   = [this] { saveSelectedPreset(); };
        btnDelete.onClick = [this] { deleteSelected(); };
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

        edName.onReturnKey    = [this] { saveSelectedPreset(); };
        edStartTC.onReturnKey = [this] { saveSelectedPreset(); };
        edStopTC.onReturnKey  = [this] { saveSelectedPreset(); };
    }

    ~GeneratorPresetEditor() override = default;

    std::function<void()> onChange;

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

        g.setColour(textBright);
        g.setFont(juce::Font(juce::FontOptions(11.0f)));
        juce::String text;
        switch (columnId)
        {
            case ColName:  text = p.name;    break;
            case ColStart: text = p.startTC; break;
            case ColStop:  text = p.stopTC;  break;
        }
        g.drawText(text, 4, 0, width - 8, height, juce::Justification::centredLeft, true);
    }

    void selectedRowsChanged(int lastRowSelected) override
    {
        if (lastRowSelected >= 0 && lastRowSelected < (int)rows.size())
        {
            auto& p = rows[(size_t)lastRowSelected];
            edName.setText(p.name, false);
            edStartTC.setText(p.startTC, false);
            edStopTC.setText(p.stopTC, false);
        }
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

        // Bottom form: 3 rows of label+editor + 2 button rows
        auto formArea = area.removeFromBottom(140);
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
        btnClearAll.setBounds(btnRow.removeFromLeft(btnW));
        formArea.removeFromBottom(6);

        // Form fields
        auto layField = [](juce::Label& lbl, juce::TextEditor& ed, juce::Rectangle<int>& area)
        {
            auto row = area.removeFromTop(22);
            lbl.setBounds(row.removeFromLeft(60));
            row.removeFromLeft(4);
            ed.setBounds(row);
            area.removeFromTop(3);
        };

        layField(lblName,    edName,    formArea);
        layField(lblStartTC, edStartTC, formArea);
        layField(lblStopTC,  edStopTC,  formArea);

        // Table fills the rest
        table.setBounds(area);
    }

private:
    enum { ColName = 1, ColStart, ColStop };

    GeneratorPresetMap& presetMap;
    juce::TableListBox table { "Presets", this };

    std::vector<GeneratorPreset> rows;

    // Buttons
    juce::TextButton btnAdd, btnSave, btnDelete, btnClearAll;
    juce::TextButton btnImport, btnExport, btnOscHelp;

    // Form
    juce::Label      lblName, lblStartTC, lblStopTC;
    juce::TextEditor edName, edStartTC, edStopTC;

    // File chooser (must persist during async operation)
    std::unique_ptr<juce::FileChooser> fileChooser;

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
        table.repaint();
        if (onChange) onChange();
    }

    /// Normalize a timecode string (HH:MM:SS:FF) by carrying overflow.
    static juce::String normalizeTC(const juce::String& tc)
    {
        auto parts = juce::StringArray::fromTokens(tc, ":.", "");
        int h = 0, m = 0, s = 0, f = 0;
        if (parts.size() >= 1) h = parts[0].getIntValue();
        if (parts.size() >= 2) m = parts[1].getIntValue();
        if (parts.size() >= 3) s = parts[2].getIntValue();
        if (parts.size() >= 4) f = parts[3].getIntValue();
        if (f < 0) f = 0;
        if (s < 0) s = 0;
        if (m < 0) m = 0;
        if (h < 0) h = 0;
        // Carry overflow
        s += f / 30; f %= 30;  // assume max 30fps for frame carry
        m += s / 60; s %= 60;
        h += m / 60; m %= 60;
        h %= 24;
        return juce::String(h).paddedLeft('0', 2) + ":"
             + juce::String(m).paddedLeft('0', 2) + ":"
             + juce::String(s).paddedLeft('0', 2) + ":"
             + juce::String(f).paddedLeft('0', 2);
    }

    void addNewPreset()
    {
        auto name = edName.getText().trim();
        if (name.isEmpty())
        {
            edName.grabKeyboardFocus();
            return;
        }

        GeneratorPreset p;
        p.name    = name;
        p.startTC = edStartTC.getText().trim();
        p.stopTC  = edStopTC.getText().trim();
        if (p.startTC.isEmpty()) p.startTC = "00:00:00:00";
        if (p.stopTC.isEmpty())  p.stopTC  = "00:00:00:00";
        p.startTC = normalizeTC(p.startTC);
        p.stopTC  = normalizeTC(p.stopTC);
        edStartTC.setText(p.startTC, false);
        edStopTC.setText(p.stopTC, false);

        presetMap.addOrUpdate(p);
        notifyChange();

        // Select the newly added row
        for (int i = 0; i < (int)rows.size(); ++i)
        {
            if (rows[(size_t)i].name.equalsIgnoreCase(name))
            {
                table.selectRow(i);
                break;
            }
        }
    }

    void saveSelectedPreset()
    {
        auto name = edName.getText().trim();
        if (name.isEmpty()) return;

        // If a row is selected and the name matches, update it
        int sel = table.getSelectedRow();
        if (sel >= 0 && sel < (int)rows.size())
        {
            auto& existing = rows[(size_t)sel];
            // Remove old entry if name changed
            if (!existing.name.equalsIgnoreCase(name))
                presetMap.remove(existing.name);
        }

        GeneratorPreset p;
        p.name    = name;
        p.startTC = edStartTC.getText().trim();
        p.stopTC  = edStopTC.getText().trim();
        if (p.startTC.isEmpty()) p.startTC = "00:00:00:00";
        if (p.stopTC.isEmpty())  p.stopTC  = "00:00:00:00";
        p.startTC = normalizeTC(p.startTC);
        p.stopTC  = normalizeTC(p.stopTC);
        edStartTC.setText(p.startTC, false);
        edStopTC.setText(p.stopTC, false);

        presetMap.addOrUpdate(p);
        notifyChange();
    }

    void deleteSelected()
    {
        int sel = table.getSelectedRow();
        if (sel < 0 || sel >= (int)rows.size()) return;

        presetMap.remove(rows[(size_t)sel].name);
        notifyChange();

        // Clear form
        edName.clear();
        edStartTC.setText("00:00:00:00", false);
        edStopTC.setText("00:00:00:00", false);
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
                    presetMap.clear();
                    notifyChange();
                    edName.clear();
                    edStartTC.setText("00:00:00:00", false);
                    edStopTC.setText("00:00:00:00", false);
                }
            });
    }

    juce::ScopedMessageBox confirmBox;

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
        help << "  /stc/N/gen/stop\n\n";
        help << "MODE\n";
        help << "  /stc/N/gen/clock (int)    0=transport, 1=clock\n\n";
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
