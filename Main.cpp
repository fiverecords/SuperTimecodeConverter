#include <JuceHeader.h>
#include "MainComponent.h"

class SuperTimecodeConverterApplication : public juce::JUCEApplication
{
public:
    SuperTimecodeConverterApplication() {}

    const juce::String getApplicationName() override    { return "Super Timecode Converter"; }
    const juce::String getApplicationVersion() override { return "1.0"; }
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
            setUsingNativeTitleBar(true);
            setContentOwned(new MainComponent(), true);

            setResizable(true, true);
            setResizeLimits(800, 450, 1920, 1080);
            centreWithSize(getWidth(), getHeight());

            setVisible(true);
        }

        void closeButtonPressed() override
        {
            JUCEApplication::getInstance()->systemRequestedQuit();
        }

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainWindow)
    };

private:
    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION(SuperTimecodeConverterApplication)
