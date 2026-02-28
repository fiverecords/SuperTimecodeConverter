// Super Timecode Converter
// Copyright (c) 2026 Fiverecords — MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

#pragma once
#include <JuceHeader.h>
#include "TimecodeCore.h"
#include "CustomLookAndFeel.h"  // getMonoFontName(), measureStringWidth()

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

    // FPS conversion display state
    void setFpsConvertEnabled(bool enabled)
    {
        if (fpsConvertActive != enabled)
        {
            fpsConvertActive = enabled;
            repaint();
        }
    }

    void setOutputTimecode(const Timecode& tc)
    {
        if (outTimecode.hours   != tc.hours   ||
            outTimecode.minutes != tc.minutes  ||
            outTimecode.seconds != tc.seconds  ||
            outTimecode.frames  != tc.frames)
        {
            outTimecode = tc;
            if (fpsConvertActive) repaint();
        }
    }

    void setOutputFrameRate(FrameRate fps)
    {
        if (outFps != fps)
        {
            outFps = fps;
            if (fpsConvertActive) repaint();
        }
    }

    void paint(juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();

        // --- Status indicator colour ---
        auto statusColour = running ? juce::Colour(0xFF2E7D32) : juce::Colour(0xFF37474F);

        // --- Main timecode display ---
        juce::String tcText = currentTimecode.toDisplayString(currentFps);

        // Scale font to fit available width
        // When FPS convert is active, we need room for "/FF" suffix (~14 chars total)
        float maxFontSize = 72.0f;
        float availW = bounds.getWidth() - 20.0f;
        float totalChars = fpsConvertActive ? 14.0f : 11.0f;

        // Compute character width ratio dynamically from the actual font metrics
        // (avoids hardcoded values that only match a single platform's monospace font)
        auto measureFont = juce::Font(juce::FontOptions(getMonoFontName(), maxFontSize, juce::Font::bold));
        float charWidthRatio = measureStringWidth(measureFont, "0") / maxFontSize;
        if (charWidthRatio <= 0.0f) charWidthRatio = 0.6f;  // safe fallback

        float fittedSize = availW / (totalChars * charWidthRatio);
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

        g.setColour(running ? juce::Colour(0xFF66BB6A) : juce::Colour(0xFF546E7A));
        g.setFont(juce::Font(juce::FontOptions(getMonoFontName(), 11.0f, juce::Font::plain)));
        g.drawText(running ? "RUNNING" : "STOPPED",
                   juce::Rectangle<float>(bounds.getCentreX() - 30.0f, statusY - 2.0f, 80.0f, 14.0f),
                   juce::Justification::centredLeft);

        // --- Timecode text ---
        float tcY = statusY + statusH + gap1;
        juce::Font tcFont(juce::FontOptions(getMonoFontName(), fontSize, juce::Font::bold));
        g.setFont(tcFont);

        // Timecode colour: bright green when running, muted gray when stopped
        auto tcColour = running ? juce::Colour(0xFF00E676) : juce::Colour(0xFF546E7A);

        if (fpsConvertActive)
        {
            // Split rendering: main TC in running colour, "/FF" suffix in cyan/dimmed
            juce::String outFrameStr = juce::String::formatted("%02d",
                juce::jlimit(0, 29, outTimecode.frames));
            juce::String fullText = tcText + "/" + outFrameStr;

            // Measure widths to position both parts
            float fullW = measureStringWidth(tcFont, fullText);
            float mainW = measureStringWidth(tcFont, tcText);
            float suffixW = fullW - mainW;
            float textStartX = bounds.getCentreX() - fullW / 2.0f;

            // Draw main TC (input)
            g.setColour(tcColour);
            g.drawText(tcText,
                       juce::Rectangle<float>(textStartX, tcY, mainW, tcHeight),
                       juce::Justification::centredLeft);

            // Draw "/FF" suffix — cyan when running, dimmer when stopped
            g.setColour(running ? juce::Colour(0xFF00ACC1) : juce::Colour(0xFF37474F));
            g.drawText("/" + outFrameStr,
                       juce::Rectangle<float>(textStartX + mainW, tcY, suffixW, tcHeight),
                       juce::Justification::centredLeft);
        }
        else
        {
            // Standard display
            g.setColour(tcColour);
            g.drawText(tcText,
                       juce::Rectangle<float>(bounds.getX(), tcY, bounds.getWidth(), tcHeight),
                       juce::Justification::centred);
        }

        // --- Labels under each pair ---
        float labelY = tcY + tcHeight + gap2;
        g.setFont(juce::Font(juce::FontOptions(getMonoFontName(), juce::jmax(7.0f, labelSize), juce::Font::plain)));

        // Compute label positions based on the full displayed text width
        float centerX = bounds.getCentreX();
        float displayChars = fpsConvertActive ? 14.0f : 11.0f;
        float tcWidth = fontSize * displayChars * charWidthRatio;
        float startX = centerX - tcWidth / 2.0f;

        if (fpsConvertActive)
        {
            // Character layout: HH:MM:SS.FF/FF = 14 chars
            // Positions:         01 2 34 5 67 8 9A B CD
            // Centre of each group:
            //   HRS = chars 0-1  → centre at char 1.0
            //   MIN = chars 3-4  → centre at char 3.5
            //   SEC = chars 6-7  → centre at char 6.5
            //   FRM = chars 9-10 → centre at char 9.5
            //   OUT = chars 12-13→ centre at char 12.5
            float charW = tcWidth / displayChars;
            float posHrs = startX + 1.0f  * charW;
            float posMn  = startX + 3.5f  * charW;
            float posSec = startX + 6.5f  * charW;
            float posFrm = startX + 9.5f  * charW;
            float posOut = startX + 12.5f * charW;

            g.setColour(juce::Colour(0xFF546E7A));
            g.drawText("HRS", juce::Rectangle<float>(posHrs - 15.0f, labelY, 30.0f, 14.0f), juce::Justification::centred);
            g.drawText("MIN", juce::Rectangle<float>(posMn  - 15.0f, labelY, 30.0f, 14.0f), juce::Justification::centred);
            g.drawText("SEC", juce::Rectangle<float>(posSec - 15.0f, labelY, 30.0f, 14.0f), juce::Justification::centred);
            g.drawText("FRM", juce::Rectangle<float>(posFrm - 15.0f, labelY, 30.0f, 14.0f), juce::Justification::centred);

            // "OUT" label in cyan under the converted frame digits
            g.setColour(juce::Colour(0xFF00838F));
            g.drawText("OUT", juce::Rectangle<float>(posOut - 15.0f, labelY, 30.0f, 14.0f), juce::Justification::centred);
        }
        else
        {
            float segW = tcWidth / 4.0f;
            float positions[] = { startX + segW * 0.5f, startX + segW * 1.5f,
                                  startX + segW * 2.5f, startX + segW * 3.5f };
            const char* labels[] = { "HRS", "MIN", "SEC", "FRM" };

            g.setColour(juce::Colour(0xFF546E7A));
            for (int i = 0; i < 4; i++)
            {
                g.drawText(labels[i],
                           juce::Rectangle<float>(positions[i] - 15.0f, labelY, 60.0f, 14.0f),
                           juce::Justification::centred);
            }
        }

        // --- Source + FPS info ---
        float infoY = bounds.getBottom() - 40.0f;
        g.setFont(juce::Font(juce::FontOptions(getMonoFontName(), 10.0f, juce::Font::plain)));

        if (fpsConvertActive)
        {
            // Show both input and output rates with arrow
            juce::String inLabel  = "SOURCE: " + sourceName + "  |  "
                                  + frameRateToString(currentFps);
            juce::String arrow    = juce::String::fromUTF8(" \xe2\x86\x92 ");  // " → "
            juce::String outLabel = frameRateToString(outFps) + " FPS";

            // Measure to centre the full composite string
            juce::Font infoFont(juce::FontOptions(getMonoFontName(), 10.0f, juce::Font::plain));
            g.setFont(infoFont);
            float inW  = measureStringWidth(infoFont, inLabel);
            float arW  = measureStringWidth(infoFont, arrow);
            float outW = measureStringWidth(infoFont, outLabel);
            float totW = inW + arW + outW;
            float sx   = bounds.getCentreX() - totW / 2.0f;

            g.setColour(juce::Colour(0xFF37474F));
            g.drawText(inLabel, juce::Rectangle<float>(sx, infoY, inW, 14.0f),
                       juce::Justification::centredLeft);

            g.setColour(juce::Colour(0xFF546E7A));
            g.drawText(arrow, juce::Rectangle<float>(sx + inW, infoY, arW, 14.0f),
                       juce::Justification::centredLeft);

            g.setColour(juce::Colour(0xFF00ACC1));
            g.drawText(outLabel, juce::Rectangle<float>(sx + inW + arW, infoY, outW, 14.0f),
                       juce::Justification::centredLeft);
        }
        else
        {
            g.setColour(juce::Colour(0xFF37474F));
            juce::String infoText = "SOURCE: " + sourceName + "  |  "
                                  + frameRateToString(currentFps) + " FPS";
            g.drawText(infoText,
                       juce::Rectangle<float>(0.0f, infoY, bounds.getWidth(), 14.0f),
                       juce::Justification::centred);
        }
    }

private:
    Timecode currentTimecode;
    FrameRate currentFps = FrameRate::FPS_30;
    juce::String sourceName = "SYSTEM";
    bool running = false;

    // FPS conversion state
    bool fpsConvertActive = false;
    Timecode outTimecode;
    FrameRate outFps = FrameRate::FPS_30;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TimecodeDisplay)
};
