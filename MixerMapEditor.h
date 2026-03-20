// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

#pragma once
#include <JuceHeader.h>
#include "MixerMap.h"
#include "CustomLookAndFeel.h"

//==============================================================================
// MixerMapEditor -- Table editor for DJM mixer parameter -> MIDI CC / OSC mapping.
//
// Designed to be shown in a DialogWindow from MainComponent.
// Receives a MixerMap& (owned by AppSettings or MainComponent) and allows
// editing OSC addresses, MIDI CCs, and enable/disable per parameter.
// Calls onChange() whenever the map is modified.
//==============================================================================
class MixerMapEditor : public juce::Component,
                       public juce::TableListBoxModel
{
public:
    MixerMapEditor(MixerMap& map, DjmModel model = DjmModel::V10Only)
        : mixerMap(map), selectedModel(model)
    {
        setSize(750, 620);

        // Build filtered row indices (hide params above selected model level)
        rebuildFilter(selectedModel);

        // --- Table ---
        addAndMakeVisible(table);
        table.setModel(this);
        table.setColour(juce::ListBox::backgroundColourId, bgDarker);
        table.setColour(juce::ListBox::outlineColourId, borderCol);
        table.setOutlineThickness(1);
        table.setRowHeight(22);
        table.setHeaderHeight(22);
        table.getHeader().setStretchToFitActive(true);

        auto& hdr = table.getHeader();
        hdr.addColumn("",          ColEnabled,  32,  28,  36, juce::TableHeaderComponent::notSortable);
        hdr.addColumn("Group",     ColGroup,    90,  60, 130, juce::TableHeaderComponent::notSortable);
        hdr.addColumn("Parameter", ColParam,   110,  80, 200, juce::TableHeaderComponent::notSortable);
        hdr.addColumn("OSC Address", ColOsc,   170, 100, 300, juce::TableHeaderComponent::notSortable);
        hdr.addColumn("MIDI CC",   ColMidiCC,   55,  40,  80, juce::TableHeaderComponent::notSortable);
        hdr.addColumn("Note",      ColMidiNote, 50,  40,  80, juce::TableHeaderComponent::notSortable);
        hdr.addColumn("DMX Ch",    ColDmxCh,    55,  40,  80, juce::TableHeaderComponent::notSortable);

        // --- Buttons ---
        addAndMakeVisible(btnResetDefaults);
        btnResetDefaults.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF3A1E1E));
        btnResetDefaults.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFFF6666));
        btnResetDefaults.onClick = [this]
        {
            auto options = juce::MessageBoxOptions()
                .withIconType(juce::MessageBoxIconType::WarningIcon)
                .withTitle("Reset Mixer Map")
                .withMessage("Reset all parameters to factory defaults?")
                .withButton("Reset")
                .withButton("Cancel");

            resetConfirmBox = juce::AlertWindow::showScopedAsync(options,
                [this](int result)
                {
                    if (result == 1)
                    {
                        mixerMap.resetToDefaults();
                        rebuildFilter(selectedModel);
                        table.updateContent();
                        table.repaint();
                        if (onChange) onChange();
                    }
                });
        };

        addAndMakeVisible(btnEnableAll);
        btnEnableAll.setColour(juce::TextButton::buttonColourId, accentCol.withAlpha(0.15f));
        btnEnableAll.setColour(juce::TextButton::textColourOffId, accentCol.brighter(0.3f));
        btnEnableAll.onClick = [this] { setAllEnabled(true); };

        addAndMakeVisible(btnDisableAll);
        btnDisableAll.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF2A2A2A));
        btnDisableAll.setColour(juce::TextButton::textColourOffId, textDim);
        btnDisableAll.onClick = [this] { setAllEnabled(false); };

        addAndMakeVisible(btnToggleModel);
        updateModelButton();
        btnToggleModel.onClick = [this]
        {
            // Cycle: 900NXS2 -> A9 -> V10 -> 900NXS2
            if (selectedModel == DjmModel::All)         selectedModel = DjmModel::A9Plus;
            else if (selectedModel == DjmModel::A9Plus) selectedModel = DjmModel::V10Only;
            else                                        selectedModel = DjmModel::All;
            rebuildFilter(selectedModel);
            updateModelButton();
            table.updateContent();
            table.repaint();
        };

        addAndMakeVisible(btnExport);
        btnExport.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF1E2A1E));
        btnExport.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFF66CC66));
        btnExport.onClick = [this]
        {
            fileChooser = std::make_unique<juce::FileChooser>(
                "Export Mixer Map", juce::File::getSpecialLocation(juce::File::userDesktopDirectory)
                    .getChildFile("mixermap.json"), "*.json");
            fileChooser->launchAsync(juce::FileBrowserComponent::saveMode
                                   | juce::FileBrowserComponent::canSelectFiles,
                [this](const juce::FileChooser& fc)
                {
                    auto file = fc.getResult();
                    if (file == juce::File()) return;
                    // Build JSON from current entries
                    auto* root = new juce::DynamicObject();
                    juce::Array<juce::var> arr;
                    for (auto& e : mixerMap.getEntries())
                        arr.add(e.toVar());
                    root->setProperty("params", arr);
                    file.replaceWithText(juce::JSON::toString(juce::var(root)));
                });
        };

        addAndMakeVisible(btnImport);
        btnImport.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF1E1E2A));
        btnImport.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFF6699FF));
        btnImport.onClick = [this]
        {
            fileChooser = std::make_unique<juce::FileChooser>(
                "Import Mixer Map", juce::File::getSpecialLocation(juce::File::userDesktopDirectory),
                "*.json");
            fileChooser->launchAsync(juce::FileBrowserComponent::openMode
                                   | juce::FileBrowserComponent::canSelectFiles,
                [this](const juce::FileChooser& fc)
                {
                    auto file = fc.getResult();
                    if (file == juce::File() || !file.existsAsFile()) return;
                    auto parsed = juce::JSON::parse(file.loadFileAsString());
                    auto* obj = parsed.getDynamicObject();
                    if (!obj) return;
                    auto* arr = obj->getProperty("params").getArray();
                    if (!arr) return;
                    for (auto& item : *arr)
                    {
                        MixerMapEntry saved;
                        saved.fromVar(item);
                        for (auto& e : mixerMap.getEntries())
                        {
                            if (e.paramId == saved.paramId)
                            {
                                e.oscAddress = saved.oscAddress;
                                e.midiCC     = saved.midiCC;
                                e.midiNote   = saved.midiNote;
                                e.artnetCh   = saved.artnetCh;
                                e.enabled    = saved.enabled;
                                break;
                            }
                        }
                    }
                    table.updateContent();
                    table.repaint();
                    if (onChange) onChange();
                });
        };
    }

    /// Called whenever a parameter mapping is changed
    std::function<void()> onChange;

    // --- Component ---
    void resized() override
    {
        auto area = getLocalBounds().reduced(8);

        auto btnRow = area.removeFromBottom(32);
        btnResetDefaults.setBounds(btnRow.removeFromRight(120));
        btnRow.removeFromRight(6);
        btnDisableAll.setBounds(btnRow.removeFromRight(90));
        btnRow.removeFromRight(6);
        btnEnableAll.setBounds(btnRow.removeFromRight(90));
        btnToggleModel.setBounds(btnRow.removeFromLeft(110));
        btnRow.removeFromLeft(6);
        btnExport.setBounds(btnRow.removeFromLeft(70));
        btnRow.removeFromLeft(6);
        btnImport.setBounds(btnRow.removeFromLeft(70));

        area.removeFromBottom(6);
        table.setBounds(area);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(bgDark);
    }

    // --- TableListBoxModel ---
    int getNumRows() override { return (int)filteredIndices.size(); }

    void paintRowBackground(juce::Graphics& g, int rowNumber,
                            int /*w*/, int /*h*/, bool selected) override
    {
        if (selected)
            g.fillAll(accentCol.withAlpha(0.2f));
        else if (rowNumber % 2 == 0)
            g.fillAll(bgDarker);
        else
            g.fillAll(bgDark);

        // Draw group separator: thin line when group changes
        if (rowNumber > 0 && rowNumber < (int)filteredIndices.size())
        {
            if (mixerMap[mapRow(rowNumber)].group != mixerMap[mapRow(rowNumber - 1)].group)
            {
                g.setColour(borderCol);
                g.drawLine(0.0f, 0.5f, (float)getWidth(), 0.5f, 1.0f);
            }
        }
    }

    void paintCell(juce::Graphics& g, int rowNumber, int columnId,
                   int width, int height, bool /*selected*/) override
    {
        if (rowNumber < 0 || rowNumber >= (int)filteredIndices.size()) return;
        const auto& e = mixerMap[mapRow(rowNumber)];

        g.setFont(juce::Font(juce::FontOptions(11.0f)));
        auto textCol = e.enabled ? textNormal : textDim;
        g.setColour(textCol);

        juce::String text;
        switch (columnId)
        {
            case ColGroup:  text = e.group; break;
            case ColParam:  text = e.displayName; break;
            case ColOsc:    text = e.oscAddress; break;
            case ColMidiCC:   text = (e.midiCC >= 0) ? juce::String(e.midiCC) : "--"; break;
            case ColMidiNote: text = (e.midiNote >= 0) ? juce::String(e.midiNote) : "--"; break;
            case ColDmxCh:    text = (e.artnetCh > 0) ? juce::String(e.artnetCh) : "--"; break;
            default: break;
        }

        if (text.isNotEmpty())
            g.drawText(text, 4, 0, width - 8, height, juce::Justification::centredLeft);
    }

    juce::Component* refreshComponentForCell(int rowNumber, int columnId,
                                              bool /*selected*/,
                                              juce::Component* existing) override
    {
        if (rowNumber < 0 || rowNumber >= (int)filteredIndices.size())
        {
            delete existing;
            return nullptr;
        }

        // --- Enabled checkbox ---
        if (columnId == ColEnabled)
        {
            auto* cb = dynamic_cast<EnabledToggle*>(existing);
            if (!cb) { delete existing; cb = new EnabledToggle(); }
            cb->setRow(rowNumber, mixerMap[mapRow(rowNumber)].enabled);
            cb->onToggle = [this](int row, bool val)
            {
                if (row >= 0 && row < (int)filteredIndices.size())
                {
                    mixerMap[mapRow(row)].enabled = val;
                    table.repaintRow(row);
                    if (onChange) onChange();
                }
            };
            return cb;
        }

        // --- Editable OSC address ---
        if (columnId == ColOsc)
        {
            auto* ed = dynamic_cast<EditableCell*>(existing);
            if (!ed) { delete existing; ed = new EditableCell(); }
            ed->setRow(rowNumber, mixerMap[mapRow(rowNumber)].oscAddress);
            ed->onEdit = [this](int row, const juce::String& val)
            {
                if (row >= 0 && row < (int)filteredIndices.size())
                {
                    mixerMap[mapRow(row)].oscAddress = val.trim();
                    if (onChange) onChange();
                }
            };
            return ed;
        }

        // --- Editable MIDI CC ---
        if (columnId == ColMidiCC)
        {
            auto* ed = dynamic_cast<EditableCell*>(existing);
            if (!ed) { delete existing; ed = new EditableCell(); }
            juce::String ccText = (mixerMap[mapRow(rowNumber)].midiCC >= 0)
                                ? juce::String(mixerMap[mapRow(rowNumber)].midiCC) : "";
            ed->setRow(rowNumber, ccText);
            ed->onEdit = [this](int row, const juce::String& val)
            {
                if (row >= 0 && row < (int)filteredIndices.size())
                {
                    auto trimmed = val.trim();
                    if (trimmed.isEmpty() || trimmed == "--" || trimmed == "-1")
                        mixerMap[mapRow(row)].midiCC = -1;
                    else
                        mixerMap[mapRow(row)].midiCC = juce::jlimit(-1, 127, trimmed.getIntValue());
                    table.repaintRow(row);
                    if (onChange) onChange();
                }
            };
            return ed;
        }

        // --- Editable MIDI Note ---
        if (columnId == ColMidiNote)
        {
            auto* ed = dynamic_cast<EditableCell*>(existing);
            if (!ed) { delete existing; ed = new EditableCell(); }
            juce::String noteText = (mixerMap[mapRow(rowNumber)].midiNote >= 0)
                                  ? juce::String(mixerMap[mapRow(rowNumber)].midiNote) : "";
            ed->setRow(rowNumber, noteText);
            ed->onEdit = [this](int row, const juce::String& val)
            {
                if (row >= 0 && row < (int)filteredIndices.size())
                {
                    auto trimmed = val.trim();
                    if (trimmed.isEmpty() || trimmed == "--" || trimmed == "-1")
                        mixerMap[mapRow(row)].midiNote = -1;
                    else
                        mixerMap[mapRow(row)].midiNote = juce::jlimit(-1, 127, trimmed.getIntValue());
                    table.repaintRow(row);
                    if (onChange) onChange();
                }
            };
            return ed;
        }

        // --- Editable DMX Channel ---
        if (columnId == ColDmxCh)
        {
            auto* ed = dynamic_cast<EditableCell*>(existing);
            if (!ed) { delete existing; ed = new EditableCell(); }
            juce::String dmxText = (mixerMap[mapRow(rowNumber)].artnetCh > 0)
                                 ? juce::String(mixerMap[mapRow(rowNumber)].artnetCh) : "";
            ed->setRow(rowNumber, dmxText);
            ed->onEdit = [this](int row, const juce::String& val)
            {
                if (row >= 0 && row < (int)filteredIndices.size())
                {
                    auto trimmed = val.trim();
                    if (trimmed.isEmpty() || trimmed == "--" || trimmed == "0")
                        mixerMap[mapRow(row)].artnetCh = 0;
                    else
                        mixerMap[mapRow(row)].artnetCh = juce::jlimit(0, 512, trimmed.getIntValue());
                    table.repaintRow(row);
                    if (onChange) onChange();
                }
            };
            return ed;
        }

        delete existing;
        return nullptr;
    }

    void cellClicked(int rowNumber, int columnId, const juce::MouseEvent&) override
    {
        // Double-click is handled by EditableCell's own TextEditor
        (void)rowNumber; (void)columnId;
    }

private:
    enum ColumnIds { ColEnabled = 1, ColGroup, ColParam, ColOsc, ColMidiCC, ColMidiNote, ColDmxCh };

    MixerMap& mixerMap;
    DjmModel selectedModel = DjmModel::V10Only;

    // --- Model filtering ---
    std::vector<int> filteredIndices;

    int mapRow(int visibleRow) const
    {
        if (visibleRow >= 0 && visibleRow < (int)filteredIndices.size())
            return filteredIndices[(size_t)visibleRow];
        return 0;
    }

    void rebuildFilter(DjmModel model)
    {
        filteredIndices.clear();
        for (int i = 0; i < mixerMap.size(); ++i)
        {
            // Show entry if its minimum model requirement is met by selected model
            if ((int)mixerMap[i].minModel > (int)model)
                continue;
            filteredIndices.push_back(i);
        }
    }

    // Colours (matching MainComponent / STC theme)
    juce::Colour bgDark    { 0xFF16181E };
    juce::Colour bgDarker  { 0xFF12141A };
    juce::Colour borderCol { 0xFF2A2C34 };
    juce::Colour textNormal{ 0xFFB0B4BE };
    juce::Colour textDim   { 0xFF5A5E68 };
    juce::Colour accentCol { 0xFF4A9EFF };

    juce::TableListBox table { "MixerMapTable", this };

    juce::TextButton btnResetDefaults { "Reset Defaults" };
    juce::TextButton btnEnableAll     { "Enable All" };
    juce::TextButton btnDisableAll    { "Disable All" };
    juce::TextButton btnToggleModel   { "DJM-V10" };
    juce::TextButton btnExport        { "Export" };
    juce::TextButton btnImport        { "Import" };

    std::unique_ptr<juce::FileChooser> fileChooser;

    void updateModelButton()
    {
        juce::String label;
        juce::Colour bgCol, txtCol;
        switch (selectedModel)
        {
            case DjmModel::All:
                label = "DJM-900NXS2";
                bgCol = juce::Colour(0xFF2A2A2A);
                txtCol = textDim;
                break;
            case DjmModel::A9Plus:
                label = "DJM-A9";
                bgCol = juce::Colour(0xFF2A3A1E);
                txtCol = juce::Colour(0xFF88CC44);
                break;
            case DjmModel::V10Only:
                label = "DJM-V10";
                bgCol = accentCol.withAlpha(0.25f);
                txtCol = accentCol.brighter(0.3f);
                break;
        }
        btnToggleModel.setButtonText(label);
        btnToggleModel.setColour(juce::TextButton::buttonColourId, bgCol);
        btnToggleModel.setColour(juce::TextButton::textColourOffId, txtCol);
    }
    juce::ScopedMessageBox resetConfirmBox;

    void setAllEnabled(bool val)
    {
        for (int idx : filteredIndices)
            mixerMap[idx].enabled = val;
        table.updateContent();
        table.repaint();
        if (onChange) onChange();
    }

    //==========================================================================
    // Inline toggle for Enabled column
    //==========================================================================
    class EnabledToggle : public juce::Component
    {
    public:
        EnabledToggle()
        {
            addAndMakeVisible(toggle);
            toggle.setClickingTogglesState(true);
            toggle.onClick = [this]
            {
                if (onToggle) onToggle(row, toggle.getToggleState());
            };
        }

        void setRow(int r, bool val)
        {
            row = r;
            toggle.setToggleState(val, juce::dontSendNotification);
        }

        void resized() override
        {
            toggle.setBounds(getLocalBounds().reduced(2));
        }

        std::function<void(int, bool)> onToggle;

    private:
        int row = 0;
        juce::ToggleButton toggle;
    };

    //==========================================================================
    // Inline editable text cell (for OSC address and MIDI CC)
    //==========================================================================
    class EditableCell : public juce::Label
    {
    public:
        EditableCell()
        {
            setEditable(false, true, false);  // single-click=no, double-click=yes
            setColour(juce::Label::textColourId, juce::Colour(0xFFB0B4BE));
            setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
            setColour(juce::Label::outlineWhenEditingColourId, juce::Colour(0xFF4A9EFF));
            setFont(juce::Font(juce::FontOptions(11.0f)));
        }

        void setRow(int r, const juce::String& val)
        {
            row = r;
            setText(val, juce::dontSendNotification);
        }

        void textWasEdited() override
        {
            if (onEdit) onEdit(row, getText());
        }

        std::function<void(int, const juce::String&)> onEdit;

    private:
        int row = 0;
    };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MixerMapEditor)
};
