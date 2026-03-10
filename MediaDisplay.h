// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter
//
// MediaDisplay -- Visual display components for waveform and artwork data.
//
// WaveformDisplay: Renders CDJ waveform preview in two formats:
//   - CDJ-3000 3-band (PWV6): 1200 entries x 3 bytes = {mid, high, low} heights
//   - NXS2 Color (PWV4): 1200 entries x 6 bytes = {d0, d1, d2, d3(R), d4(G), d5(B)}
//
// ArtworkDisplay: Renders a decoded JPEG album art image.

#pragma once
#include <JuceHeader.h>
#include <algorithm>
#include <cmath>
#include <vector>

//==============================================================================
// WaveformDisplay -- Paints CDJ waveform preview bars
//
// Supports CDJ-3000 3-band (PWV6) and NXS2 color (PWV4) formats,
// sourced from dbserver ANLZ queries via DbServerClient.
//==============================================================================

class WaveformDisplay : public juce::Component
{
public:
    WaveformDisplay()
    {
        setOpaque(false);
    }

    /// Set color preview waveform data (from CDJ dbserver ANLZ response).
    /// bytesPerEntry: 3 = CDJ-3000 3-band (mid,high,low), 6 = NXS2 color (d0-d5)
    void setColorWaveformData(const std::vector<uint8_t>& data, int entryCount, int bytesPerEntry)
    {
        colorWaveformData = data;
        colorEntryCount = entryCount;
        colorBytesPerEntry = bytesPerEntry;
        hasColorData = (entryCount > 0 && (int)data.size() >= entryCount * bytesPerEntry);
        repaint();
    }

    /// Clear all waveform data (e.g. on track change before new data arrives)
    void clearWaveform()
    {
        colorWaveformData.clear();
        hasColorData = false;
        colorEntryCount = 0;
        colorBytesPerEntry = 0;
        playPosition = 0.0f;
        repaint();
    }

    bool hasWaveformData() const { return hasColorData; }

    /// Set the play cursor position (0.0 = start, 1.0 = end). Triggers repaint.
    void setPlayPosition(float ratio)
    {
        float clamped = juce::jlimit(0.0f, 1.0f, ratio);
        if (clamped != playPosition)
        {
            // Pixel-based threshold: skip repaint only if cursor moved < 0.5px.
            // Using component width avoids the old bug where a ratio-based
            // threshold (0.002) meant the cursor froze for seconds on long tracks.
            float w = (float)getWidth();
            float pxDelta = std::abs(clamped - playPosition) * w;
            if (pxDelta >= 0.5f || (clamped == 0.0f && playPosition != 0.0f))
            {
                playPosition = clamped;
                repaint();
            }
        }
    }

    /// Set debug position info for overlay display.
    void setDebugPositionMs(uint32_t posMs, uint32_t totalMs)
    {
        debugPosMs = posMs;
        debugTotalMs = totalMs;
    }

    void paint(juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();

        // Background
        g.setColour(juce::Colour(0xFF0D1117));
        g.fillRoundedRectangle(bounds, 3.0f);

        if (hasColorData && !colorWaveformData.empty() && colorEntryCount > 0)
        {
            if (colorBytesPerEntry == 3)
                paintThreeBandWaveform(g, bounds);
            else
                paintColorWaveform(g, bounds);
            return;
        }

        // "No waveform" placeholder
        g.setColour(juce::Colour(0xFF4A5568));
        g.setFont(10.0f);
        g.drawText("No Waveform", bounds, juce::Justification::centred);
    }

private:
    /// Render CDJ-3000 3-band waveform (PWV6: 3 bytes per entry = mid, high, low)
    /// Per beat-link: byte[0]=mid, byte[1]=high, byte[2]=low
    /// Rendered in classic CDJ blue/white style (no multicolor, no peak bars).
    void paintThreeBandWaveform(juce::Graphics& g, juce::Rectangle<float> bounds)
    {
        float w = bounds.getWidth();
        float inset = 2.0f;
        float drawW = w - inset * 2;
        float drawH = bounds.getHeight() - inset * 2;
        float midY = inset + drawH * 0.5f;
        float halfH = drawH * 0.5f;
        float entriesPerPx = (float)colorEntryCount / drawW;
        const uint8_t* data = colorWaveformData.data();
        int totalBytes = (int)colorWaveformData.size();

        // Find global peak amplitude for height normalization
        uint8_t globalPeak = 1;
        for (int i = 0; i < colorEntryCount; ++i)
        {
            int off = i * 3;
            if (off + 2 >= totalBytes) break;
            globalPeak = std::max({ globalPeak, data[off], data[off + 1], data[off + 2] });
        }
        float hScale = halfH / (float)globalPeak;

        for (int px = 0; px < (int)drawW; ++px)
        {
            int eStart = juce::jlimit(0, colorEntryCount - 1, (int)(px * entriesPerPx));
            int eEnd   = juce::jlimit(0, colorEntryCount - 1, (int)((px + 1) * entriesPerPx));
            if (eEnd < eStart) eEnd = eStart;

            float sumMid = 0, sumHigh = 0, sumLow = 0;
            int count = 0;
            for (int e = eStart; e <= eEnd; ++e)
            {
                int off = e * 3;
                if (off + 2 >= totalBytes) break;
                sumMid  += data[off];
                sumHigh += data[off + 1];
                sumLow  += data[off + 2];
                count++;
            }
            if (count == 0) continue;

            float avgMid  = sumMid  / (float)count;
            float avgHigh = sumHigh / (float)count;
            float avgLow  = sumLow  / (float)count;

            // Use only the mid band as the main amplitude (no peak layer)
            float amplitude = avgMid;
            if (amplitude < 1.0f) continue;

            float barH = amplitude * hScale;
            float x = inset + (float)px;

            // Blend blue -> white based on relative high-frequency content
            float total = avgLow + avgMid + avgHigh + 0.001f;
            float highRatio = avgHigh / total;  // 0 = pure blue, 1 = white
            float blueR = 0.0f  + highRatio * 1.0f;
            float blueG = 0.45f + highRatio * 0.55f;
            float blueB = 1.0f;

            g.setColour(juce::Colour::fromFloatRGBA(blueR, blueG, blueB, 1.0f));
            g.drawVerticalLine((int)x, midY - barH, midY);
            g.drawVerticalLine((int)x, midY, midY + barH);
        }

        paintOverlay(g, bounds, colorEntryCount, true);
    }

    /// Render CDJ NXS2+ color waveform (PWV4: 6 bytes per entry)
    /// Per Deep Symmetry / jan2000: d3=red, d4=green, d5=blue+frontHeight
    /// Rendered in classic CDJ blue/white style (no multicolor, no peak bars).
    void paintColorWaveform(juce::Graphics& g, juce::Rectangle<float> bounds)
    {
        float w = bounds.getWidth();
        float inset = 2.0f;
        float drawW = w - inset * 2;
        float drawH = bounds.getHeight() - inset * 2;
        float midY = inset + drawH * 0.5f;
        float halfH = drawH * 0.5f;
        float entriesPerPx = (float)colorEntryCount / drawW;
        const uint8_t* data = colorWaveformData.data();
        int totalBytes = (int)colorWaveformData.size();

        // Find global peak for height normalization (use d5 = blue/front as main height)
        uint8_t globalPeak = 1;
        for (int i = 0; i < colorEntryCount; ++i)
        {
            int off = i * 6;
            if (off + 5 >= totalBytes) break;
            globalPeak = std::max(globalPeak, data[off + 5]);
        }
        float hScale = halfH / (float)globalPeak;

        for (int px = 0; px < (int)drawW; ++px)
        {
            int eStart = juce::jlimit(0, colorEntryCount - 1, (int)(px * entriesPerPx));
            int eEnd   = juce::jlimit(0, colorEntryCount - 1, (int)((px + 1) * entriesPerPx));
            if (eEnd < eStart) eEnd = eStart;

            float sumD3 = 0, sumD4 = 0, sumD5 = 0;
            int count = 0;
            for (int e = eStart; e <= eEnd; ++e)
            {
                int off = e * 6;
                if (off + 5 >= totalBytes) break;
                sumD3 += data[off + 3];
                sumD4 += data[off + 4];
                sumD5 += data[off + 5];
                count++;
            }
            if (count == 0) continue;

            float avgD3 = sumD3 / (float)count;
            float avgD4 = sumD4 / (float)count;
            float avgD5 = sumD5 / (float)count;

            // Use d5 (blue/front height) as amplitude — no peak overlay
            float amplitude = avgD5;
            if (amplitude < 1.0f) continue;

            float barH = amplitude * hScale;
            float x = inset + (float)px;

            // Blend blue -> white based on relative d3 (treble/brightness) content
            float total = avgD3 + avgD4 + avgD5 + 0.001f;
            float highRatio = avgD3 / total;  // 0 = pure blue, 1 = white
            float blueR = 0.0f  + highRatio * 1.0f;
            float blueG = 0.45f + highRatio * 0.55f;
            float blueB = 1.0f;

            g.setColour(juce::Colour::fromFloatRGBA(blueR, blueG, blueB, 1.0f));
            g.drawVerticalLine((int)x, midY - barH, midY);
            g.drawVerticalLine((int)x, midY, midY + barH);
        }

        paintOverlay(g, bounds, colorEntryCount, true);
    }

    /// Paint cursor, center line, labels (shared by both render paths)
    void paintOverlay(juce::Graphics& g, juce::Rectangle<float> bounds,
                      int barCount, bool /*isColor*/)
    {
        float w = bounds.getWidth();
        float inset = 2.0f;
        float drawW = w - inset * 2;
        float drawH = bounds.getHeight() - inset * 2;
        float midY = inset + drawH * 0.5f;

        // Center line
        g.setColour(juce::Colour(0x40FFFFFF));
        g.drawHorizontalLine((int)midY, inset, w - inset);

        // Play cursor
        if (playPosition >= 0.0f && playPosition <= 1.0f && hasWaveformData())
        {
            float cursorX = inset + playPosition * drawW;
            g.setColour(juce::Colour(0x4000D4FF));
            g.fillRect(cursorX - 3.0f, inset, 7.0f, drawH);
            g.setColour(juce::Colour(0xFFFFFFFF));
            g.fillRect(cursorX - 0.5f, inset, 2.0f, drawH);
        }

        // Resolution label
        juce::String label = juce::String(barCount)
                           + (colorBytesPerEntry == 3 ? " (3-band)" : " (color)");
        g.setColour(juce::Colour(0x60FFFFFF));
        g.setFont(9.0f);
        g.drawText(label, bounds.reduced(4.0f, 1.0f), juce::Justification::topRight);

        // Position info
        if (debugPosMs > 0 || debugTotalMs > 0)
        {
            auto fmtMs = [](uint32_t ms) -> juce::String {
                int s = (int)(ms / 1000);
                return juce::String(s / 60) + ":" + juce::String(s % 60).paddedLeft('0', 2);
            };
            g.setColour(juce::Colour(0x60FFFFFF));
            g.drawText(fmtMs(debugPosMs) + "/" + fmtMs(debugTotalMs),
                        bounds.reduced(4.0f, 1.0f), juce::Justification::topLeft);
        }
    }

    std::vector<uint8_t> colorWaveformData;
    int colorEntryCount = 0;
    int colorBytesPerEntry = 0;  // 3=ThreeBand(CDJ-3000), 6=ColorNxs2
    bool hasColorData = false;
    float playPosition = 0.0f;   // 0.0 = start, 1.0 = end
    uint32_t debugPosMs = 0;
    uint32_t debugTotalMs = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WaveformDisplay)
};


//==============================================================================
// ArtworkDisplay -- Paints a decoded JPEG album art image
//==============================================================================

class ArtworkDisplay : public juce::Component
{
public:
    ArtworkDisplay()
    {
        setOpaque(false);
    }

    /// Set the artwork image (call from message thread).
    void setImage(const juce::Image& img)
    {
        artwork = img;
        repaint();
    }

    /// Clear artwork (e.g. on track change)
    void clearImage()
    {
        artwork = {};
        repaint();
    }

    bool hasImage() const { return artwork.isValid(); }

    void paint(juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();

        // Background
        g.setColour(juce::Colour(0xFF0D1117));
        g.fillRoundedRectangle(bounds, 3.0f);

        if (!artwork.isValid())
        {
            // Placeholder icon
            g.setColour(juce::Colour(0xFF4A5568));
            g.setFont(10.0f);
            g.drawText("ART",
                        bounds, juce::Justification::centred);
            return;
        }

        // Draw image scaled to fit, centered, with small inset
        float inset = 2.0f;
        auto drawBounds = bounds.reduced(inset);

        // Maintain aspect ratio
        float imgW = (float)artwork.getWidth();
        float imgH = (float)artwork.getHeight();
        float scale = juce::jmin(drawBounds.getWidth() / imgW,
                                  drawBounds.getHeight() / imgH);
        float dw = imgW * scale;
        float dh = imgH * scale;
        float dx = drawBounds.getX() + (drawBounds.getWidth() - dw) * 0.5f;
        float dy = drawBounds.getY() + (drawBounds.getHeight() - dh) * 0.5f;

        g.drawImage(artwork, (int)dx, (int)dy, (int)dw, (int)dh,
                     0, 0, artwork.getWidth(), artwork.getHeight());
    }

private:
    juce::Image artwork;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ArtworkDisplay)
};
