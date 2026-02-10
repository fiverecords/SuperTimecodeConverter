#pragma once
#include <JuceHeader.h>

class LevelMeter : public juce::Component
{
public:
    LevelMeter() { setOpaque(false); }

    void setLevel(float newLevel)
    {
        newLevel = juce::jlimit(0.0f, 1.0f, newLevel);
        if (std::abs(currentLevel - newLevel) > 0.001f)
        {
            currentLevel = newLevel;
            repaint();
        }
    }

    void setColour(juce::Colour c) { meterColour = c; }

    void paint(juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat().reduced(0.5f);
        float cornerSize = 2.0f;

        // Background
        g.setColour(juce::Colour(0xFF0D0E12));
        g.fillRoundedRectangle(bounds, cornerSize);

        // Filled level bar
        if (currentLevel > 0.001f)
        {
            float fillW = bounds.getWidth() * currentLevel;
            auto fillBounds = bounds.withWidth(fillW);

            // Green -> Yellow -> Red gradient based on level
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
