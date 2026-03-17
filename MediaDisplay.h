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
        invalidateCache();
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
        invalidateCache();
        repaint();
    }

    bool hasWaveformData() const { return hasColorData; }

    /// Set the play cursor position (0.0 = start, 1.0 = end).
    /// Two modes based on whether this component is a real child or an orphan:
    ///
    /// Real child (engine mini-player): Sub-pixel moves accumulate until
    /// the delta crosses 0.5px, then position updates and a partial repaint
    /// fires.  This avoids scheduling repaints for invisible movement.
    ///
    /// Orphan (PDL View -- painted manually by parent): Position is stored
    /// immediately every call; the parent controls when to repaint.
    void setPlayPosition(float ratio)
    {
        float clamped = juce::jlimit(0.0f, 1.0f, ratio);
        if (clamped == playPosition) return;

        // Orphan component (PDL View paints us manually) -- always store.
        // The parent's dirty check drives repaints, not ours.
        if (getParentComponent() == nullptr)
        {
            playPosition = clamped;
            return;
        }

        float w = (float)getWidth();
        if (w <= 0.0f) return;     // not yet laid out

        float pxDelta = std::abs(clamped - playPosition) * w;
        if (pxDelta < 0.5f && !(clamped == 0.0f && playPosition != 0.0f))
            return;

        float inset   = 2.0f;
        float drawW   = w - inset * 2.0f;
        float oldX    = inset + playPosition * drawW;
        float newX    = inset + clamped      * drawW;
        playPosition  = clamped;

        // Repaint the union of old and new cursor strips plus margin for
        // anti-aliased float-positioned edges (2px extra each side).
        float dirtyX  = std::min(oldX, newX) - 5.0f;
        float dirtyW  = std::abs(newX - oldX) + 11.0f;
        repaint((int)dirtyX, 0, (int)std::ceil(dirtyW), getHeight());
    }


    void paint(juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();

        if (hasColorData && !colorWaveformData.empty() && colorEntryCount > 0)
        {
            ensureCachedImage();
            if (cachedWaveformImg.isValid())
            {
                // Draw the cached high-res image scaled to logical bounds.
                // On HiDPI/Retina, the image is at physical resolution so
                // JUCE composites it 1:1 with the framebuffer -- no upscaling blur.
                g.drawImage(cachedWaveformImg, bounds);
                paintOverlay(g, bounds, colorEntryCount);
                return;
            }
        }

        // No waveform -- plain background + placeholder text
        g.setColour(juce::Colour(0xFF0D1117));
        g.fillRoundedRectangle(bounds, 3.0f);
        g.setColour(juce::Colour(0xFF4A5568));
        g.setFont(10.0f);
        g.drawText("No Waveform", bounds, juce::Justification::centred);
    }

private:
    //----------------------------------------------------------------------
    // Cached waveform image -- rendered at physical pixel resolution to
    // avoid HiDPI upscaling blur.  Regenerated only when data, size, or
    // display scale changes.
    //----------------------------------------------------------------------
    juce::Image cachedWaveformImg;
    int cachedW = 0, cachedH = 0;
    float cachedScale = 0.0f;
    bool cacheValid = false;

    void invalidateCache() { cacheValid = false; }

    /// Detect the display scale factor for this component (or its parent).
    float getDisplayScale() const
    {
        // Try the component's own top-level peer first
        if (auto* tlc = getTopLevelComponent())
            if (auto* peer = tlc->getPeer())
                return (float)peer->getPlatformScaleFactor();

        // Fallback: main display scale (covers orphan components in PDL View)
        auto& displays = juce::Desktop::getInstance().getDisplays();
        if (auto* primary = displays.getPrimaryDisplay())
            return (float)primary->scale;

        return 1.0f;
    }

    void ensureCachedImage()
    {
        int w = getWidth();
        int h = getHeight();
        if (w <= 0 || h <= 0) return;

        float scale = getDisplayScale();
        if (scale < 1.0f) scale = 1.0f;

        // Regenerate if size, scale, or data changed
        if (cacheValid && cachedW == w && cachedH == h && cachedScale == scale)
            return;

        // Create image at physical pixel resolution
        int imgW = (int)std::ceil(w * scale);
        int imgH = (int)std::ceil(h * scale);
        cachedWaveformImg = juce::Image(juce::Image::ARGB, imgW, imgH, true);
        juce::Graphics ig(cachedWaveformImg);

        // Scale the Graphics context so all drawing uses logical coordinates
        // but renders at physical pixel density
        ig.addTransform(juce::AffineTransform::scale(scale));

        // Background
        auto bounds = juce::Rectangle<float>(0.0f, 0.0f, (float)w, (float)h);
        ig.setColour(juce::Colour(0xFF0D1117));
        ig.fillRoundedRectangle(bounds, 3.0f);

        if (colorBytesPerEntry == 3)
            renderThreeBandBars(ig, bounds);
        else
            renderColorBars(ig, bounds);

        // Center line (static, part of cached image)
        float inset = 2.0f;
        float drawH = bounds.getHeight() - inset * 2;
        float midY = inset + drawH * 0.5f;
        ig.setColour(juce::Colour(0x40FFFFFF));
        ig.drawHorizontalLine((int)midY, inset, bounds.getWidth() - inset);

        cachedW = w;
        cachedH = h;
        cachedScale = scale;
        cacheValid = true;
    }

    /// Render CDJ-3000 3-band waveform bars into a Graphics context (cached).
    void renderThreeBandBars(juce::Graphics& g, juce::Rectangle<float> bounds)
    {
        float w = bounds.getWidth();
        float inset = 2.0f;
        float drawW = w - inset * 2;
        float drawH = bounds.getHeight() - inset * 2;
        float midY = inset + drawH * 0.5f;
        float halfH = drawH * 0.5f;
        float entriesPerPx = (float)colorEntryCount / drawW;
        float barW = std::max(1.0f, drawW / (float)colorEntryCount);
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

            float amplitude = avgMid;
            if (amplitude < 1.0f) continue;

            float barH = amplitude * hScale;
            float x = inset + (float)px;

            float total = avgLow + avgMid + avgHigh + 0.001f;
            float highRatio = avgHigh / total;
            float blueR = 0.0f  + highRatio * 1.0f;
            float blueG = 0.45f + highRatio * 0.55f;
            float blueB = 1.0f;

            g.setColour(juce::Colour::fromFloatRGBA(blueR, blueG, blueB, 1.0f));
            g.fillRect(x, midY - barH, barW, barH);
            g.fillRect(x, midY, barW, barH);
        }
    }

    /// Render CDJ NXS2+ color waveform bars into a Graphics context (cached).
    void renderColorBars(juce::Graphics& g, juce::Rectangle<float> bounds)
    {
        float w = bounds.getWidth();
        float inset = 2.0f;
        float drawW = w - inset * 2;
        float drawH = bounds.getHeight() - inset * 2;
        float midY = inset + drawH * 0.5f;
        float halfH = drawH * 0.5f;
        float entriesPerPx = (float)colorEntryCount / drawW;
        float barW = std::max(1.0f, drawW / (float)colorEntryCount);
        const uint8_t* data = colorWaveformData.data();
        int totalBytes = (int)colorWaveformData.size();

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

            float amplitude = avgD5;
            if (amplitude < 1.0f) continue;

            float barH = amplitude * hScale;
            float x = inset + (float)px;

            float total = avgD3 + avgD4 + avgD5 + 0.001f;
            float highRatio = avgD3 / total;
            float blueR = 0.0f  + highRatio * 1.0f;
            float blueG = 0.45f + highRatio * 0.55f;
            float blueB = 1.0f;

            g.setColour(juce::Colour::fromFloatRGBA(blueR, blueG, blueB, 1.0f));
            g.fillRect(x, midY - barH, barW, barH);
            g.fillRect(x, midY, barW, barH);
        }
    }

    /// Paint cursor and labels (lightweight -- called every frame over cached image)
    void paintOverlay(juce::Graphics& g, juce::Rectangle<float> bounds, int barCount)
    {
        float w = bounds.getWidth();
        float inset = 2.0f;
        float drawW = w - inset * 2;
        float drawH = bounds.getHeight() - inset * 2;

        // Play cursor -- draw directly (2 fillRect calls, cheap)
        if (playPosition >= 0.0f && playPosition <= 1.0f && hasWaveformData())
        {
            float cursorX = inset + playPosition * drawW;
            g.setColour(juce::Colour(0x4000D4FF));
            g.fillRect(cursorX - 3.0f, inset, 7.0f, drawH);
            g.setColour(juce::Colour(0xFFFFFFFF));
            g.fillRect(cursorX - 0.5f, inset, 2.0f, drawH);
        }

        // Resolution label -- drawn directly (one drawText call, negligible cost)
        juce::String label = juce::String(barCount)
                           + (colorBytesPerEntry == 3 ? " (3-band)" : " (color)");
        g.setColour(juce::Colour(0x60FFFFFF));
        g.setFont(9.0f);
        g.drawText(label, bounds.reduced(4.0f, 1.0f), juce::Justification::topRight);
    }

    std::vector<uint8_t> colorWaveformData;
    int colorEntryCount = 0;
    int colorBytesPerEntry = 0;  // 3=ThreeBand(CDJ-3000), 6=ColorNxs2
    bool hasColorData = false;
    float playPosition = 0.0f;   // 0.0 = start, 1.0 = end

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
