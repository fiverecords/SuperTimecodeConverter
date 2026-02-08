#pragma once
#include <JuceHeader.h>
#include "TimecodeCore.h"

class TimecodeDisplay : public juce::Component
{
public:
    TimecodeDisplay()
    {
        setOpaque(false);
    }

    void setTimecode(const Timecode& tc)
    {
        if (currentTimecode.hours   != tc.hours   ||
            currentTimecode.minutes != tc.minutes  ||
            currentTimecode.seconds != tc.seconds  ||
            currentTimecode.frames  != tc.frames)
        {
            currentTimecode = tc;
            repaint();
        }
    }

    void setFrameRate(FrameRate fps)
    {
        if (currentFps != fps)
        {
            currentFps = fps;
            repaint();
        }
    }

    void setSourceName(const juce::String& name)
    {
        if (sourceName != name)
        {
            sourceName = name;
            repaint();
        }
    }

    void setRunning(bool isRunning)
    {
        if (running != isRunning)
        {
            running = isRunning;
            repaint();
        }
    }

    void paint(juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();

        // --- Status indicator colour ---
        auto statusColour = running ? juce::Colour(0xFF2E7D32) : juce::Colour(0xFF37474F);

        // --- Main timecode display ---
        juce::String tcText = currentTimecode.toString();

        // Scale font to fit available width (11 chars at ~0.6 em width in Consolas)
        float maxFontSize = 72.0f;
        float availW = bounds.getWidth() - 20.0f;
        float charWidthRatio = 0.62f;  // Consolas character width / font size
        float fittedSize = availW / (11.0f * charWidthRatio);
        float fontSize = juce::jmin(maxFontSize, juce::jmax(24.0f, fittedSize));

        float tcHeight = fontSize * 1.25f;
        float labelSize = juce::jmin(9.0f, fontSize * 0.14f);
        float labelH = 14.0f;
        float statusH = 14.0f;
        float gap1 = 8.0f;   // status -> tc
        float gap2 = 4.0f;   // tc -> labels

        // Total content block height
        float contentH = statusH + gap1 + tcHeight + gap2 + labelH;

        // Center vertically, but leave room for bottom info
        float usableH = bounds.getHeight() - 50.0f;  // reserve 50px for source/fps info
        float contentY = bounds.getY() + (usableH - contentH) / 2.0f;
        contentY = juce::jmax(bounds.getY() + 10.0f, contentY);  // don't go above top

        // --- Status indicator (centered in block) ---
        float statusY = contentY;

        g.setColour(statusColour);
        g.fillEllipse(bounds.getCentreX() - 40.0f, statusY, 6.0f, 6.0f);

        g.setColour(juce::Colour(0xFF546E7A));
        g.setFont(juce::Font(juce::FontOptions("Consolas", 11.0f, juce::Font::plain)));
        g.drawText(running ? "RUNNING" : "STOPPED",
                   juce::Rectangle<float>(bounds.getCentreX() - 30.0f, statusY - 2.0f, 80.0f, 14.0f),
                   juce::Justification::centredLeft);

        // --- Timecode text ---
        float tcY = statusY + statusH + gap1;

        g.setFont(juce::Font(juce::FontOptions("Consolas", fontSize, juce::Font::bold)));
        g.setColour(juce::Colour(0xFFE0F7FA));
        g.drawText(tcText,
                   juce::Rectangle<float>(bounds.getX(), tcY, bounds.getWidth(), tcHeight),
                   juce::Justification::centred);

        // --- Labels under each pair ---
        float labelY = tcY + tcHeight + gap2;
        g.setFont(juce::Font(juce::FontOptions("Consolas", juce::jmax(7.0f, labelSize), juce::Font::plain)));
        g.setColour(juce::Colour(0xFF546E7A));

        float centerX = bounds.getCentreX();
        float tcWidth = fontSize * 11.0f * charWidthRatio;
        float startX = centerX - tcWidth / 2.0f;
        float segW = tcWidth / 4.0f;

        float positions[] = { startX + segW * 0.5f, startX + segW * 1.5f,
                              startX + segW * 2.5f, startX + segW * 3.5f };
        const char* labels[] = { "HRS", "MIN", "SEC", "FRM" };

        for (int i = 0; i < 4; i++)
        {
            g.drawText(labels[i],
                       juce::Rectangle<float>(positions[i] - 15.0f, labelY, 60.0f, 14.0f),
                       juce::Justification::centred);
        }

        // --- Source + FPS info ---
        float infoY = bounds.getBottom() - 40.0f;
        g.setFont(juce::Font(juce::FontOptions("Consolas", 10.0f, juce::Font::plain)));
        g.setColour(juce::Colour(0xFF37474F));

        juce::String infoText = "SOURCE: " + sourceName + "  |  " + frameRateToString(currentFps) + " FPS";
        g.drawText(infoText,
                   juce::Rectangle<float>(0.0f, infoY, bounds.getWidth(), 14.0f),
                   juce::Justification::centred);
    }

private:
    Timecode currentTimecode;
    FrameRate currentFps = FrameRate::FPS_30;
    juce::String sourceName = "SYSTEM";
    bool running = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TimecodeDisplay)
};
