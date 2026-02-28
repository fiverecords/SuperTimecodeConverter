// Super Timecode Converter
// Copyright (c) 2026 Fiverecords — MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

#pragma once
#include <JuceHeader.h>

//==============================================================================
// Cross-platform monospace font name
// Consolas is Windows-only; macOS uses Menlo, Linux uses monospace fallback
//==============================================================================
inline juce::String getMonoFontName()
{
#if JUCE_MAC
    return "Menlo";
#elif JUCE_WINDOWS
    return "Consolas";
#else
    // On Linux, JUCE's getDefaultMonospacedFontName() can return a generic
    // name not present on all distros. Try common monospace fonts first.
    // Result is cached after the first call — findAllTypefaceNames() scans
    // the system font database and is too expensive to call from paint().
    static juce::String cached;
    if (cached.isNotEmpty())
        return cached;

    const juce::StringArray candidates { "DejaVu Sans Mono", "Liberation Mono",
                                         "Noto Mono", "Courier New" };
    const auto sysFonts = juce::Font::findAllTypefaceNames();
    for (const auto& candidate : candidates)
    {
        if (sysFonts.contains(candidate))
        {
            cached = candidate;
            return cached;
        }
    }
    cached = juce::Font::getDefaultMonospacedFontName();
    return cached;
#endif
}

// Replacement for the deprecated juce::Font::getStringWidthFloat().
// JUCE 8.x recommends using GlyphArrangement to measure text layouts.
inline float measureStringWidth(const juce::Font& font, const juce::String& text)
{
    juce::GlyphArrangement ga;
    ga.addLineOfText(font, text, 0.0f, 0.0f);
    return ga.getBoundingBox(0, -1, true).getWidth();
}

class CustomLookAndFeel : public juce::LookAndFeel_V4
{
public:
    CustomLookAndFeel()
    {
        // Pre-cache the monospace font name so the first paint() call
        // doesn't trigger an expensive findAllTypefaceNames() scan.
        (void)getMonoFontName();

        setColour(juce::PopupMenu::backgroundColourId, bg);
        setColour(juce::PopupMenu::highlightedBackgroundColourId, bgHover);
        setColour(juce::PopupMenu::textColourId, textBright);
        setColour(juce::PopupMenu::highlightedTextColourId, textBright);
        setColour(juce::ComboBox::textColourId, textBright);
        setColour(juce::TextEditor::backgroundColourId, bg);
        setColour(juce::TextEditor::textColourId, textBright);
        setColour(juce::TextEditor::outlineColourId, border);
        setColour(juce::ScrollBar::thumbColourId, juce::Colour(0xFF37474F));
    }

    //==============================================================================
    // BUTTONS
    //==============================================================================
    void drawButtonBackground(juce::Graphics& g, juce::Button& button,
                              const juce::Colour& backgroundColour,
                              bool isHighlighted, bool isDown) override
    {
        auto bounds = button.getLocalBounds().toFloat().reduced(0.5f);
        float cornerSize = 5.0f;

        auto baseCol = backgroundColour;
        if (isDown)
            baseCol = baseCol.brighter(0.1f);
        else if (isHighlighted)
            baseCol = baseCol.brighter(0.05f);

        // Subtle gradient
        g.setGradientFill(juce::ColourGradient(
            baseCol.brighter(0.03f), 0, bounds.getY(),
            baseCol.darker(0.03f), 0, bounds.getBottom(), false));
        g.fillRoundedRectangle(bounds, cornerSize);

        // Border
        auto borderAlpha = isHighlighted ? 0.3f : 0.15f;
        g.setColour(juce::Colours::white.withAlpha(borderAlpha));
        g.drawRoundedRectangle(bounds, cornerSize, 1.0f);
    }

    void drawButtonText(juce::Graphics& g, juce::TextButton& button,
                        bool isHighlighted, bool /*isDown*/) override
    {
        auto font = juce::Font(juce::FontOptions(getMonoFontName(), 11.0f, juce::Font::bold));
        g.setFont(font);
        g.setColour(button.findColour(isHighlighted ? juce::TextButton::textColourOnId
                                                    : juce::TextButton::textColourOffId));

        auto bounds = button.getLocalBounds();
        g.drawText(button.getButtonText(), bounds, juce::Justification::centred, false);
    }

    juce::Font getTextButtonFont(juce::TextButton&, int) override
    {
        return juce::Font(juce::FontOptions(getMonoFontName(), 11.0f, juce::Font::bold));
    }

    //==============================================================================
    // TOGGLE BUTTONS
    //==============================================================================
    void drawToggleButton(juce::Graphics& g, juce::ToggleButton& button,
                          bool isHighlighted, bool /*isDown*/) override
    {
        auto bounds = button.getLocalBounds().toFloat();
        float cornerSize = 5.0f;
        bool isOn = button.getToggleState();

        auto tickColour = button.findColour(juce::ToggleButton::tickColourId);

        // Background -- strong accent tint when ON
        auto bgCol = isOn ? tickColour.withAlpha(0.18f) : bg;
        if (isHighlighted) bgCol = bgCol.brighter(0.04f);
        g.setColour(bgCol);
        g.fillRoundedRectangle(bounds.reduced(0.5f), cornerSize);

        // Border -- accent colour when ON
        g.setColour(isOn ? tickColour.withAlpha(0.5f) : border);
        g.drawRoundedRectangle(bounds.reduced(0.5f), cornerSize, isOn ? 1.5f : 1.0f);

        // Left accent bar when ON
        if (isOn)
        {
            g.setColour(tickColour);
            g.fillRoundedRectangle(bounds.getX() + 1.5f, bounds.getY() + 4.0f,
                                   3.0f, bounds.getHeight() - 8.0f, 1.5f);
        }

        // Toggle indicator (circle style)
        float indicatorSize = 14.0f;
        float indicatorX = bounds.getX() + 12.0f;
        float indicatorY = bounds.getCentreY() - indicatorSize / 2.0f;
        auto indicatorBounds = juce::Rectangle<float>(indicatorX, indicatorY, indicatorSize, indicatorSize);

        // Outer ring
        g.setColour(isOn ? tickColour : juce::Colour(0xFF2A2D35));
        g.fillEllipse(indicatorBounds);
        g.setColour(isOn ? tickColour.brighter(0.2f) : border);
        g.drawEllipse(indicatorBounds, 1.0f);

        // Inner filled circle when ON
        if (isOn)
        {
            g.setColour(tickColour.brighter(0.15f));
            g.fillEllipse(indicatorBounds.reduced(3.5f));
        }

        // Text -- brighter when ON, with accent tint
        auto textBounds = bounds.withTrimmedLeft(indicatorX + indicatorSize + 8.0f);
        g.setFont(juce::Font(juce::FontOptions(getMonoFontName(), 11.0f, juce::Font::bold)));
        g.setColour(isOn ? textBright : textMid);
        g.drawText(button.getButtonText(), textBounds.toNearestInt(),
                   juce::Justification::centredLeft, false);

        // Status dot -- pulsing accent
        if (isOn)
        {
            float dotSize = 6.0f;
            float dotX = bounds.getRight() - dotSize - 10.0f;
            float dotY = bounds.getCentreY() - dotSize / 2.0f;
            g.setColour(tickColour);
            g.fillEllipse(dotX, dotY, dotSize, dotSize);
            // Glow
            g.setColour(tickColour.withAlpha(0.15f));
            g.fillEllipse(dotX - 2.0f, dotY - 2.0f, dotSize + 4.0f, dotSize + 4.0f);
        }
    }

    //==============================================================================
    // COMBO BOXES
    //==============================================================================
    void drawComboBox(juce::Graphics& g, int width, int height, bool isDown,
                      int, int, int, int, juce::ComboBox& box) override
    {
        auto bounds = juce::Rectangle<float>(0, 0, (float)width, (float)height);
        float cornerSize = 4.0f;

        auto bgCol = box.findColour(juce::ComboBox::backgroundColourId);
        if (isDown) bgCol = bgCol.brighter(0.05f);
        g.setColour(bgCol);
        g.fillRoundedRectangle(bounds.reduced(0.5f), cornerSize);

        g.setColour(box.findColour(juce::ComboBox::outlineColourId));
        g.drawRoundedRectangle(bounds.reduced(0.5f), cornerSize, 1.0f);

        // Arrow
        float arrowSize = 6.0f;
        float arrowX = (float)width - 16.0f;
        float arrowY = (float)height / 2.0f;

        juce::Path arrow;
        arrow.addTriangle(arrowX - arrowSize, arrowY - arrowSize * 0.4f,
                          arrowX + arrowSize, arrowY - arrowSize * 0.4f,
                          arrowX, arrowY + arrowSize * 0.6f);
        g.setColour(box.findColour(juce::ComboBox::arrowColourId));
        g.fillPath(arrow);
    }

    juce::Font getComboBoxFont(juce::ComboBox&) override
    {
        return juce::Font(juce::FontOptions(getMonoFontName(), 11.0f, juce::Font::plain));
    }

    void positionComboBoxText(juce::ComboBox& box, juce::Label& label) override
    {
        label.setBounds(6, 0, box.getWidth() - 28, box.getHeight());
        label.setFont(getComboBoxFont(box));
    }

    //==============================================================================
    // POPUP MENU
    //==============================================================================
    void drawPopupMenuBackground(juce::Graphics& g, int width, int height) override
    {
        auto bounds = juce::Rectangle<float>(0, 0, (float)width, (float)height);
        g.setColour(juce::Colour(0xFF1A1D23));
        g.fillRoundedRectangle(bounds, 4.0f);
        g.setColour(border);
        g.drawRoundedRectangle(bounds.reduced(0.5f), 4.0f, 1.0f);
    }

    void drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& area,
                           bool isSeparator, bool isActive, bool isHighlighted,
                           bool isTicked, bool /*hasSubMenu*/,
                           const juce::String& text, const juce::String&,
                           const juce::Drawable*, const juce::Colour*) override
    {
        if (isSeparator)
        {
            auto sepArea = area.reduced(8, 0);
            g.setColour(border);
            g.fillRect(sepArea.getX(), sepArea.getCentreY(), sepArea.getWidth(), 1);
            return;
        }

        // Detect marker patterns:
        //   " ●"          → current engine active device (cyan dot)
        //   " [ENGINE N]" → other engine using this device (amber tag)
        static const juce::String dotChar = juce::String::charToString(0x25CF);

        bool hasActiveDot = text.endsWith(dotChar);
        int markerStart = -1;

        if (!hasActiveDot)
        {
            int bracketClose = text.lastIndexOf("]");
            int bracketOpen  = text.lastIndexOf(" [");
            if (bracketClose == text.length() - 1 && bracketOpen > 0)
                markerStart = bracketOpen;
        }

        bool hasMarker = hasActiveDot || (markerStart >= 0);

        // Background — tinted for in-use items
        if (isHighlighted && isActive)
        {
            g.setColour(bgHover);
            g.fillRoundedRectangle(area.toFloat().reduced(2, 1), 3.0f);
        }
        else if (hasMarker && isActive)
        {
            // Subtle background tint for marked items (even when not hovered)
            auto tintColour = hasActiveDot ? juce::Colour(0xFF00BCD4).withAlpha(0.06f)   // cyan tint
                                           : juce::Colour(0xFFFFB74D).withAlpha(0.06f);  // amber tint
            g.setColour(tintColour);
            g.fillRoundedRectangle(area.toFloat().reduced(2, 1), 3.0f);
        }

        // Left accent bar for marked items
        if (hasMarker && isActive)
        {
            auto barColour = hasActiveDot ? juce::Colour(0xFF00BCD4)    // cyan
                                          : juce::Colour(0xFFFFB74D);   // amber
            g.setColour(barColour);
            g.fillRoundedRectangle(area.toFloat().getX() + 2.0f,
                                   area.toFloat().getY() + 3.0f,
                                   3.0f,
                                   area.toFloat().getHeight() - 6.0f, 1.5f);
        }

        auto font = juce::Font(juce::FontOptions(getMonoFontName(), 11.0f, juce::Font::plain));
        g.setFont(font);

        auto textArea = area.reduced(hasMarker ? 16 : 12, 0);  // extra indent for bar

        if (hasActiveDot && isActive)
        {
            // Current engine active: draw device name + cyan dot
            auto basePart = text.substring(0, text.length() - (dotChar.length() + 1)); // strip " ●"
            g.setColour(textBright);
            g.drawText(basePart, textArea, juce::Justification::centredLeft, false);

            // Draw the dot in cyan
            float baseWidth = measureStringWidth(font, basePart + " ");
            auto dotArea = textArea.withTrimmedLeft((int)baseWidth);
            g.setColour(juce::Colour(0xFF00BCD4));  // cyan
            g.setFont(juce::Font(juce::FontOptions(getMonoFontName(), 13.0f, juce::Font::bold)));
            g.drawText(dotChar, dotArea, juce::Justification::centredLeft, false);
        }
        else if (markerStart >= 0 && isActive)
        {
            // Other engine marker: draw base text + amber tag
            auto basePart   = text.substring(0, markerStart);
            auto markerPart = text.substring(markerStart);

            g.setColour(textBright);
            g.drawText(basePart, textArea, juce::Justification::centredLeft, false);

            float baseWidth = measureStringWidth(font, basePart);
            auto markerArea = textArea.withTrimmedLeft((int)baseWidth);
            g.setColour(juce::Colour(0xFFFFB74D)); // amber
            g.setFont(juce::Font(juce::FontOptions(getMonoFontName(), 11.0f, juce::Font::bold)));
            g.drawText(markerPart, markerArea, juce::Justification::centredLeft, false);
        }
        else
        {
            g.setColour(isActive ? textBright : textDim);
            g.drawText(text, textArea, juce::Justification::centredLeft);
        }

        if (isTicked)
        {
            g.setColour(textBright);
            auto tickArea = area.withTrimmedLeft(area.getWidth() - 24).reduced(6);
            g.drawText(juce::String::charToString(0x2713), tickArea, juce::Justification::centred);
        }
    }

    //==============================================================================
    // SLIDERS
    //==============================================================================
    void drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height,
                          float sliderPos, float, float,
                          juce::Slider::SliderStyle, juce::Slider& slider) override
    {
        float trackH = 4.0f;
        float trackY = y + height / 2.0f - trackH / 2.0f;
        auto trackBounds = juce::Rectangle<float>((float)x, trackY, (float)width, trackH);

        // Track background
        g.setColour(slider.findColour(juce::Slider::backgroundColourId));
        g.fillRoundedRectangle(trackBounds, trackH / 2.0f);

        // Filled portion
        auto fillBounds = trackBounds.withWidth(sliderPos - (float)x);
        g.setColour(slider.findColour(juce::Slider::trackColourId));
        g.fillRoundedRectangle(fillBounds, trackH / 2.0f);

        // Thumb
        float thumbW = 10.0f, thumbH = 16.0f;
        float thumbX = sliderPos - thumbW / 2.0f;
        float thumbY = y + height / 2.0f - thumbH / 2.0f;
        auto thumbBounds = juce::Rectangle<float>(thumbX, thumbY, thumbW, thumbH);

        g.setColour(slider.findColour(juce::Slider::thumbColourId));
        g.fillRoundedRectangle(thumbBounds, 3.0f);

        // Thumb highlight line
        g.setColour(juce::Colours::white.withAlpha(0.2f));
        g.fillRoundedRectangle(thumbBounds.reduced(2.0f, 4.0f).withWidth(2.0f)
                                .withX(thumbBounds.getCentreX() - 1.0f), 1.0f);
    }

    juce::Label* createSliderTextBox(juce::Slider& slider) override
    {
        auto* label = LookAndFeel_V4::createSliderTextBox(slider);
        label->setFont(juce::Font(juce::FontOptions(getMonoFontName(), 10.0f, juce::Font::plain)));
        return label;
    }

    //==============================================================================
    // SCROLL BAR
    //==============================================================================
    void drawScrollbar(juce::Graphics& g, juce::ScrollBar&, int x, int y,
                       int width, int height, bool isVertical, int thumbStartPos,
                       int thumbSize, bool isMouseOver, bool isMouseDown) override
    {
        auto thumbColour = juce::Colour(0xFF37474F);
        if (isMouseOver || isMouseDown)
            thumbColour = thumbColour.brighter(0.15f);

        if (isVertical)
        {
            g.setColour(thumbColour);
            g.fillRoundedRectangle((float)x + 1.0f, (float)thumbStartPos,
                                   (float)width - 2.0f, (float)thumbSize, 3.0f);
        }
        else
        {
            g.setColour(thumbColour);
            g.fillRoundedRectangle((float)thumbStartPos, (float)y + 1.0f,
                                   (float)thumbSize, (float)height - 2.0f, 3.0f);
        }
    }

    int getDefaultScrollbarWidth() override { return 8; }

private:
    juce::Colour bg        { 0xFF1A1D23 };
    juce::Colour bgHover   { 0xFF252830 };
    juce::Colour border    { 0xFF2A2D35 };
    juce::Colour textDim   { 0xFF546E7A };
    juce::Colour textMid   { 0xFF78909C };
    juce::Colour textBright{ 0xFFCFD8DC };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CustomLookAndFeel)
};
