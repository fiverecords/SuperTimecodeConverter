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
    const juce::String getApplicationVersion() override { return "1.5.3"; }
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
                            if (d.userArea.contains(c)) { setBounds(b); break; }
                    }
                }
            }

            setVisible(true);
        }

        void closeButtonPressed() override
        {
            // Save window bounds before quitting
            if (auto* mc = dynamic_cast<MainComponent*>(getContentComponent()))
            {
                auto b = getBounds();
                mc->saveMainWindowBounds(
                    juce::String(b.getX()) + " " + juce::String(b.getY()) + " "
                    + juce::String(b.getWidth()) + " " + juce::String(b.getHeight()));
            }
            JUCEApplication::getInstance()->systemRequestedQuit();
        }

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainWindow)
    };

private:
    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION(SuperTimecodeConverterApplication)
