// Super Timecode Converter
// Copyright (c) 2026 Fiverecords — MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

#pragma once
#include <JuceHeader.h>

class LevelMeter : public juce::Component
{
public:
    LevelMeter() { setOpaque(false); }  // false: parent repaints the 1px margin cleanly

    void setLevel(float newLevel)
    {
        // Allow levels above 1.0 so we can indicate clipping/hot signal
        newLevel = juce::jlimit(0.0f, 2.0f, newLevel);
        if (std::abs(currentLevel - newLevel) > 0.001f)
        {
            currentLevel = newLevel;
            repaint();
        }
    }

    void setMeterColour(juce::Colour c) { meterColour = c; }

    void paint(juce::Graphics& g) override
    {
        // Use full integer bounds for the background fill so there are no
        // uncleared pixels at the component edge (previously reduced(0.5f)
        // left a fractional-pixel strip that caused a flickering halo).
        auto intBounds = getLocalBounds();
        float cornerSize = 2.0f;

        // Background — fill the full component area first
        g.setColour(juce::Colour(0xFF0D0E12));
        g.fillRoundedRectangle(intBounds.toFloat(), cornerSize);

        // Inner drawing area inset by 1px so the border sits cleanly on top
        auto bounds = intBounds.toFloat().reduced(1.0f);

        // Filled level bar
        if (currentLevel > 0.001f)
        {
            // Clamp display width to bar bounds, but use real level for colour
            float displayLevel = juce::jmin(1.0f, currentLevel);
            float fillW = bounds.getWidth() * displayLevel;
            auto fillBounds = bounds.withWidth(fillW);

            // Green -> Yellow -> Red gradient based on actual level (not clamped)
            juce::Colour barColour;
            if (currentLevel < 0.6f)
                barColour = meterColour.withAlpha(0.7f);
            else if (currentLevel < 0.85f)
                barColour = juce::Colour(0xFFFFAB00).withAlpha(0.8f);
            else
                barColour = juce::Colour(0xFFC62828).withAlpha(0.9f);

            g.setColour(barColour);
            g.fillRoundedRectangle(fillBounds, cornerSize);

            // Subtle glow on top
            g.setColour(juce::Colours::white.withAlpha(0.08f));
            g.fillRoundedRectangle(fillBounds.withHeight(fillBounds.getHeight() * 0.4f), cornerSize);

            // Clipping indicator: flash full bar red when level > 1.0
            if (currentLevel > 1.0f)
            {
                g.setColour(juce::Colour(0xFFC62828).withAlpha(0.3f));
                g.fillRoundedRectangle(bounds, cornerSize);
            }
        }

        // Border
        g.setColour(juce::Colour(0xFF2A2D35));
        g.drawRoundedRectangle(bounds, cornerSize, 0.5f);

        // Tick marks at -12dB (~0.25), -6dB (~0.5), 0dB (~1.0)
        g.setColour(juce::Colour(0xFF2A2D35).withAlpha(0.6f));
        float tickPositions[] = { 0.25f, 0.5f, 0.75f };
        for (float tp : tickPositions)
        {
            float x = bounds.getX() + bounds.getWidth() * tp;
            g.drawLine(x, bounds.getY(), x, bounds.getBottom(), 0.5f);
        }
    }

private:
    float currentLevel = 0.0f;
    juce::Colour meterColour { 0xFF2E7D32 };  // Default green

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LevelMeter)
};
