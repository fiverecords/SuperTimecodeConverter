// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

#include <JuceHeader.h>
#include "MainComponent.h"
#include <csignal>

class SuperTimecodeConverterApplication : public juce::JUCEApplication
{
public:
    SuperTimecodeConverterApplication() {}

    const juce::String getApplicationName() override    { return "Super Timecode Converter"; }
    const juce::String getApplicationVersion() override { return "1.9.15-beta1"; }
    bool moreThanOneInstanceAllowed() override           { return false; }

    void initialise(const juce::String&) override
    {
       #if JUCE_LINUX || JUCE_MAC
        // A write to a TCP peer that has reset the connection raises
        // SIGPIPE, whose default action ends the process: JUCE 9.0.3's
        // StreamingSocket::write is send() without MSG_NOSIGNAL.  A
        // StageLinQ unit that closes its connection while STC still writes
        // to it does that (simulated with a fake device, AUDIT SLQ-5; not
        // seen on hardware).  Ignored, the write fails with EPIPE and the
        // connection's own error handling runs.
        // Set before any thread starts; it applies to the whole process.
        std::signal(SIGPIPE, SIG_IGN);
       #endif
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
            // No maximum size (JUCE's own default maximum): a 2560x1440 cap
            // kept the window from filling a 4K display (AUDIT UI-12).
            setResizeLimits(800, 550, 0x3fffffff, 0x3fffffff);
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
                        // Restored only where its title bar -- what the window
                        // is dragged by -- lies in a display's user area (off
                        // the taskbar and menu bar) for its full height and
                        // at least 100 px of its width.  The old test, the
                        // window's centre on some display, let a window saved
                        // on a monitor since removed or rearranged come back
                        // with its title bar out of reach (AUDIT UI-12).
                        // Otherwise it stays centred as above.
                        const auto titleBar = b.withHeight(getTitleBarHeight());
                        for (auto& d : juce::Desktop::getInstance().getDisplays().displays)
                        {
                            const auto onDisplay = titleBar.getIntersection(d.userBounds.toNearestInt());
                            if (onDisplay.getHeight() == titleBar.getHeight() && onDisplay.getWidth() >= 100)
                            {
                                setBounds(b);
                                break;
                            }
                        }
                    }
                }
            }

            setVisible(true);
        }

        void closeButtonPressed() override
        {
            auto* mc = dynamic_cast<MainComponent*>(getContentComponent());

            // If Show Lock is active, confirm before quitting.  One
            // confirmation at a time: a second close or Cmd+Q while it is up
            // does nothing, where it used to stack another (AUDIT UI-12).
            if (mc != nullptr && mc->isShowModeLocked())
            {
                if (quitConfirmShowing)
                    return;
                quitConfirmShowing = true;
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
                    safeThis->quitConfirmShowing = false;
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
        bool quitConfirmShowing = false;   // the Show Lock quit confirmation is up

        void saveWindowBoundsAndQuit(MainComponent* mc)
        {
            // Hand the window bounds to the settings before quitting; the
            // MainComponent destructor, which quit() reaches through
            // shutdown(), writes them with everything else in one save.
            if (mc != nullptr)
            {
                auto b = getBounds();
                mc->setMainWindowBounds(
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
