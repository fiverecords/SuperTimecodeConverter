// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

#include <JuceHeader.h>
#include "MainComponent.h"

class SuperTimecodeConverterApplication : public juce::JUCEApplication
{
public:
    SuperTimecodeConverterApplication() {}

    const juce::String getApplicationName() override    { return "Super Timecode Converter"; }
    const juce::String getApplicationVersion() override { return "1.8.0"; }
    bool moreThanOneInstanceAllowed() override           { return false; }

    void initialise(const juce::String&) override
    {
        mainWindow.reset(new MainWindow(getApplicationName()));
    }

    void shutdown() override
    {
        mainWindow = nullptr;
    }

    void systemRequestedQuit() override
    {
        // Route through closeButtonPressed so the Show Lock guard applies
        // (covers macOS Cmd+Q, dock Quit, etc.)
        if (mainWindow != nullptr)
            mainWindow->closeButtonPressed();
        else
            quit();
    }

    class MainWindow : public juce::DocumentWindow
    {
    public:
        MainWindow(juce::String name)
            : DocumentWindow(name,
                             juce::Colour(0xFF12141A),  // Dark background
                             DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar(false);
            setTitleBarHeight(20);
            setColour(juce::DocumentWindow::textColourId, juce::Colour(0xFF546E7A));

            auto* mc = new MainComponent();
            setContentOwned(mc, true);

            setResizable(true, true);
            setResizeLimits(800, 550, 2560, 1440);
            centreWithSize(getWidth(), getHeight());

            // Restore saved window position/size
            auto saved = mc->getSavedMainWindowBounds();
            if (saved.isNotEmpty())
            {
                auto parts = juce::StringArray::fromTokens(saved, " ", "");
                if (parts.size() == 4)
                {
                    auto b = juce::Rectangle<int>(parts[0].getIntValue(), parts[1].getIntValue(),
                                                   parts[2].getIntValue(), parts[3].getIntValue());
                    if (b.getWidth() >= 800 && b.getHeight() >= 550)
                    {
                        auto c = b.getCentre();
                        for (auto& d : juce::Desktop::getInstance().getDisplays().displays)
                            if (d.totalArea.contains(c)) { setBounds(b); break; }
                    }
                }
            }

            setVisible(true);
        }

        void closeButtonPressed() override
        {
            auto* mc = dynamic_cast<MainComponent*>(getContentComponent());

            // If Show Lock is active, confirm before quitting
            if (mc != nullptr && mc->isShowModeLocked())
            {
                auto options = juce::MessageBoxOptions()
                    .withIconType(juce::MessageBoxIconType::WarningIcon)
                    .withTitle("Show Lock Active")
                    .withMessage("Show Lock is active. Closing the application "
                                 "will stop all timecode outputs.\n\n"
                                 "Are you sure you want to quit?")
                    .withButton("Quit")
                    .withButton("Cancel");
                juce::Component::SafePointer<MainWindow> safeThis(this);
                juce::AlertWindow::showAsync(options, [safeThis](int result)
                {
                    if (safeThis == nullptr) return;
                    if (result == 1)
                    {
                        auto* mc2 = dynamic_cast<MainComponent*>(safeThis->getContentComponent());
                        safeThis->saveWindowBoundsAndQuit(mc2);
                    }
                });
                return;
            }

            saveWindowBoundsAndQuit(mc);
        }

    private:
        void saveWindowBoundsAndQuit(MainComponent* mc)
        {
            // Save window bounds before quitting
            if (mc != nullptr)
            {
                auto b = getBounds();
                mc->saveMainWindowBounds(
                    juce::String(b.getX()) + " " + juce::String(b.getY()) + " "
                    + juce::String(b.getWidth()) + " " + juce::String(b.getHeight()));
            }
            JUCEApplication::getInstance()->quit();
        }
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainWindow)
    };

private:
    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION(SuperTimecodeConverterApplication)
