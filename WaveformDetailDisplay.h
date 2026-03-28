// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter
//
// WaveformDetailDisplay -- Scrolling detail waveform view (like top of CDJ screen).
//
// Renders the high-resolution waveform (PWV5/PWV7) centered on the current
// playback position, with beat grid ticks, cue point markers, active loop
// overlay, and song structure phrase bars.
//
// Data sources:
//   - Detail waveform: TrackMetadata::detailData (150 entries/sec)
//   - Beat grid: TrackMetadata::beatGrid (PQTZ)
//   - Song structure: TrackMetadata::songStructure (PSSI)
//   - Cue points: RekordboxCue list (hot cues, memory points, loops with colors)
//                 or CuePoint list from TrackMapEntry (fallback)
//   - Active loop: ProDJLinkInput::getLoopStartMs/getLoopEndMs
//
// Thread model: all setters called from message thread (tick loop).

#pragma once
#include <JuceHeader.h>
#include "DbServerClient.h"
#include "AppSettings.h"
#include <vector>
#include <cmath>
#include <algorithm>

class WaveformDetailDisplay : public juce::Component
{
public:
    WaveformDetailDisplay() { setOpaque(false); }

    //==========================================================================
    // Data setters (call from message thread)
    //==========================================================================

    /// Set detail waveform data from TrackMetadata.
    void setDetailData(const std::vector<uint8_t>& data, int entryCount,
                       int bytesPerEntry, uint32_t trackDurationMs)
    {
        detailData = data;
        detailEntryCount = entryCount;
        detailBytesPerEntry = bytesPerEntry;
        durationMs = trackDurationMs;
        hasData = (entryCount > 0 && bytesPerEntry > 0
                   && (int)data.size() >= entryCount * bytesPerEntry);

        // Compute global peak once for stable normalization (avoids "breathing"
        // effect from recalculating peak per visible window every frame).
        globalPeak = 1;
        if (hasData)
        {
            const uint8_t* d = data.data();
            int totalBytes = (int)data.size();
            for (int e = 0; e < entryCount; ++e)
            {
                int off = e * bytesPerEntry;
                if (off + bytesPerEntry > totalBytes) break;
                for (int b = 0; b < bytesPerEntry && b < 3; ++b)
                    globalPeak = std::max(globalPeak, d[off + b]);
            }
        }
        invalidateStaticCache();
        repaint();
    }

    /// Set beat grid from TrackMetadata.
    void setBeatGrid(const std::vector<TrackMetadata::BeatEntry>& grid)
    {
        if (grid.size() == beatGrid.size() && !beatGrid.empty()) return;
        beatGrid = grid;
        invalidateStaticCache();
        repaint();
    }

    /// Set song structure (phrase analysis) from TrackMetadata.
    void setSongStructure(const std::vector<TrackMetadata::PhraseEntry>& phrases,
                          uint16_t mood)
    {
        if (phrases.size() == songStructure.size() && !songStructure.empty()) return;
        songStructure = phrases;
        phraseMood = mood;
        invalidateStaticCache();
        repaint();
    }

    /// Set rekordbox cue list (hot cues + memory points + loops with colors).
    void setRekordboxCues(const std::vector<TrackMetadata::RekordboxCue>& cues)
    {
        if (cues.size() == rekordboxCues.size() && !rekordboxCues.empty()) return;
        rekordboxCues = cues;
        invalidateStaticCache();
        repaint();
    }

    /// Set cue points from TrackMapEntry (fallback if no rekordbox cues).
    void setCuePoints(const std::vector<CuePoint>& cues)
    {
        trackMapCues = cues;
        invalidateStaticCache();
        repaint();
    }

    /// Set active loop range (from CDJ-3000 status packet). Pass 0,0 for no loop.
    void setActiveLoop(uint32_t startMs, uint32_t endMs)
    {
        if (loopStartMs != startMs || loopEndMs != endMs)
        {
            loopStartMs = startMs;
            loopEndMs = endMs;
            invalidateStaticCache();
        }
    }

    /// Set current playback position in ms. This drives the scroll.
    /// Called at 60Hz from timer. CDJ updates arrive at ~5Hz.
    /// @param ms        Current position from CDJ
    /// @param playing   True if the CDJ is actively playing
    /// @param pitchRatio  Playback speed ratio from CDJ fader (1.0 = normal, 1.05 = +5%)
    void setPlayheadMs(uint32_t ms, bool playing, double pitchRatio = 1.0)
    {
        double now = juce::Time::getMillisecondCounterHiRes();
        double frameDelta = now - lastFrameTime;
        lastFrameTime = now;

        if (frameDelta < 1.0 || frameDelta > 200.0)
            frameDelta = 16.67;

        if (ms != targetPlayheadMs)
        {
            // New position from CDJ -- update speed estimate
            if (targetPlayheadMs > 0 && ms > targetPlayheadMs && playing)
            {
                double dt = now - lastCdjUpdateTime;
                if (dt > 5.0 && dt < 1000.0)
                {
                    double dMs = (double)(ms - targetPlayheadMs);
                    if (dMs < 2000.0)
                    {
                        double newSpeed = dMs / dt;
                        playheadSpeed = playheadSpeed * 0.6 + newSpeed * 0.4;
                    }
                }
            }
            else if (ms < targetPlayheadMs || (ms - targetPlayheadMs) > 2000)
            {
                // Seek or track change -- snap immediately
                smoothPlayheadMs = (double)ms;
                playheadSpeed = 0.0;
            }

            // Correct drift toward actual CDJ position
            double drift = (double)ms - smoothPlayheadMs;
            if (std::abs(drift) > 500.0)
                smoothPlayheadMs = (double)ms;
            else
                smoothPlayheadMs += drift * 0.2;

            targetPlayheadMs = ms;
            lastCdjUpdateTime = now;
        }

        if (playing)
        {
            // Use pitch-adjusted speed if available, else PLL estimate
            double speed = playheadSpeed;
            if (speed < 0.01 && pitchRatio > 0.5)
                speed = pitchRatio;
            else if (speed < 0.01)
                speed = 1.0;

            smoothPlayheadMs += speed * frameDelta;
        }
        else
        {
            // Not playing -- track CDJ position directly but smoothly
            playheadSpeed = 0.0;
            double target = (double)ms;
            double diff = target - smoothPlayheadMs;
            if (std::abs(diff) > 200.0)
                smoothPlayheadMs = target;  // seek: snap
            else if (std::abs(diff) > 0.5)
                smoothPlayheadMs += diff * 0.3;  // gentle slide to final position
            // else: already at target, don't change
        }

        uint32_t displayMs = (uint32_t)juce::jmax(0.0, smoothPlayheadMs);
        playheadMs = displayMs;
    }

    /// Set zoom scale (entries per pixel). Default=4.
    /// 1 = full resolution, higher = more zoomed out.
    void setScale(int s)
    {
        scale = juce::jlimit(1, 32, s);
        invalidateStaticCache();
        repaint();
    }
    int getScale() const { return scale; }

    /// Clear all data (track change).
    void clear()
    {
        detailData.clear();
        hasData = false;
        detailEntryCount = 0;
        detailBytesPerEntry = 0;
        globalPeak = 1;
        beatGrid.clear();
        songStructure.clear();
        rekordboxCues.clear();
        trackMapCues.clear();
        loopStartMs = 0;
        loopEndMs = 0;
        playheadMs = 0;
        targetPlayheadMs = 0;
        smoothPlayheadMs = 0.0;
        lastCdjUpdateTime = 0.0;
        lastFrameTime = 0.0;
        playheadSpeed = 0.0;
        durationMs = 0;
        diagLogged = false;
        invalidateStaticCache();
        repaint();
    }

    bool hasDetailData() const { return hasData; }
    int getDetailBytesPerEntry() const { return detailBytesPerEntry; }

    /// Returns true if the smooth position is still converging (deceleration/blend).
    bool isAnimating() const
    {
        return std::abs(smoothPlayheadMs - (double)targetPlayheadMs) > 1.0;
    }

    //==========================================================================
    // Paint
    //==========================================================================

    void paint(juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();
        float w = bounds.getWidth();
        float h = bounds.getHeight();

        // Background
        g.setColour(juce::Colour(0xFF0A0E14));
        g.fillRoundedRectangle(bounds, 3.0f);

        if (!hasData || w < 10.0f || h < 10.0f)
        {
            g.setColour(juce::Colour(0xFF4A5568));
            g.setFont(10.0f);
            g.drawText("No Detail Waveform", bounds, juce::Justification::centred);
            return;
        }

        float inset = 2.0f;
        float drawW = w - inset * 2.0f;
        float midX = inset + drawW * 0.5f;
        float waveTop = inset + 12.0f;    // space for phrase bar (10px + 2px gap)
        float waveBot = h - inset - 2.0f;
        float waveH = waveBot - waveTop;

        float waveMidY = waveTop + waveH * 0.5f;
        float halfH = waveH * 0.5f;

        // Calculate visible range in entries (use float for sub-pixel precision)
        double centerEntryF = smoothPlayheadMs * TrackMetadata::kDetailEntriesPerSecond / 1000.0;
        int centerEntry = (int)centerEntryF;
        float subPixelOffset = (float)(centerEntryF - centerEntry) / (float)scale;  // fractional pixel
        int halfVisible = (int)(drawW * 0.5f) * scale;
        int startEntry = centerEntry - halfVisible;
        int endEntry   = centerEntry + halfVisible;

#if JUCE_DEBUG
        if (!diagLogged && hasData)
        {
            diagLogged = true;
            DBG("WaveformDetail paint: entries=" + juce::String(detailEntryCount)
                + " bpe=" + juce::String(detailBytesPerEntry)
                + " peak=" + juce::String((int)globalPeak)
                + " beats=" + juce::String((int)beatGrid.size())
                + " phrases=" + juce::String((int)songStructure.size())
                + " rbCues=" + juce::String((int)rekordboxCues.size())
                + " tmCues=" + juce::String((int)trackMapCues.size())
                + " dur=" + juce::String(durationMs)
                + " w=" + juce::String((int)w) + " h=" + juce::String((int)h)
                + " scale=" + juce::String(scale));
        }
#endif

        // --- Cached bitmap rendering ---
        // The static elements (waveform, beats, cues, phrases) are rendered
        // to an offscreen image 3x wider than visible. On each frame we just
        // blit the cached image at the correct scroll offset.
        // Re-render only when the playhead scrolls beyond the cached range,
        // or when data/zoom changes (invalidateStaticCache).
        // HiDPI: render at physical pixel size to avoid blurry scaling.

        // Detect HiDPI scale for sharp cache rendering.
        // WaveformDetailDisplay is painted manually (not in JUCE component tree),
        // so use the global display scale as best estimate.
        float dpiScale = 1.0f;
        {
            auto& displays = juce::Desktop::getInstance().getDisplays();
            for (auto& d : displays.displays)
                dpiScale = juce::jmax(dpiScale, (float)d.scale);
        }

        int logicalCacheW = (int)drawW * 3;
        int logicalCacheH = (int)h;
        int physCacheW = (int)(logicalCacheW * dpiScale);
        int physCacheH = (int)(logicalCacheH * dpiScale);
        int cacheHalfVisible = (int)(logicalCacheW * 0.5f) * scale;
        int cacheCenterEntry = centerEntry;

        // Check if we need to re-render the cache
        bool needsRender = !staticCache.isValid()
            || staticCache.getWidth() != physCacheW
            || staticCache.getHeight() != physCacheH
            || startEntry < cacheStartEntry
            || endEntry > cacheEndEntry;

        if (needsRender)
        {
            cacheCenterEntry = centerEntry;
            cacheStartEntry = cacheCenterEntry - cacheHalfVisible;
            cacheEndEntry   = cacheCenterEntry + cacheHalfVisible;

            staticCache = juce::Image(juce::Image::ARGB, physCacheW, physCacheH, true);
            juce::Graphics cg(staticCache);
            cg.addTransform(juce::AffineTransform::scale(dpiScale));

            // Render all static elements at cache coordinates (logical pixels)
            float cacheDrawW = (float)logicalCacheW;

            // Phrase bar
            paintPhraseBar(cg, 0.0f, inset, cacheDrawW, 10.0f, cacheStartEntry, cacheEndEntry);

            // Waveform bars
            paintWaveform(cg, 0.0f, cacheDrawW, waveMidY, halfH, cacheStartEntry);

            // Beat grid ticks
            paintBeatTicks(cg, 0.0f, waveTop, cacheDrawW, waveH, cacheStartEntry, cacheEndEntry);

            // Loop overlay
            paintLoop(cg, 0.0f, waveTop, cacheDrawW, waveH, cacheStartEntry, cacheEndEntry);

            // Cue markers
            paintCueMarkers(cg, 0.0f, waveTop, cacheDrawW, waveH, cacheStartEntry, cacheEndEntry);
        }

        // Blit the cached image at the correct scroll offset (sub-pixel precision).
        // Use clip + drawImageAt with fractional x to avoid integer pixel snapping.
        float cachePixelOffset = (float)(startEntry - cacheStartEntry) / (float)scale + subPixelOffset;
        {
            juce::Graphics::ScopedSaveState sss(g);
            g.reduceClipRegion((int)inset, 0, (int)drawW, logicalCacheH);
            // Draw the full cache image shifted left by the fractional offset.
            // The 1/dpiScale factor converts physical-pixel image to logical coords.
            float destX = inset - cachePixelOffset;
            float destScale = 1.0f / dpiScale;
            g.drawImageTransformed(staticCache,
                juce::AffineTransform::scale(destScale).translated(destX, 0.0f));
        }

        // --- Dynamic overlays (drawn every frame, cheap) ---

        // Playhead cursor (center line)
        g.setColour(juce::Colour(0xCCFFFFFF));
        g.fillRect(midX - 0.5f, waveTop, 1.5f, waveH);
        juce::Path tri;
        tri.addTriangle(midX - 3.0f, waveTop, midX + 3.0f, waveTop, midX, waveTop + 4.0f);
        g.fillPath(tri);

        // Zoom buttons (top-right of waveform area, below phrase bar)
        float btnSize = 14.0f;
        float btnY = waveTop + 1.0f;
        float btnGap = 2.0f;
        float minusX = w - inset - btnSize;
        float plusX  = minusX - btnSize - btnGap;

        zoomInBounds  = juce::Rectangle<float>(plusX, btnY, btnSize, btnSize);
        zoomOutBounds = juce::Rectangle<float>(minusX, btnY, btnSize, btnSize);

        auto paintZoomBtn = [&](float bx, float by, const juce::String& label)
        {
            auto r = juce::Rectangle<float>(bx, by, btnSize, btnSize);
            g.setColour(juce::Colour(0x60000000));
            g.fillRoundedRectangle(r, 2.0f);
            g.setColour(juce::Colour(0x80FFFFFF));
            g.drawRoundedRectangle(r, 2.0f, 0.5f);
            g.setColour(juce::Colour(0xCCFFFFFF));
            g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
            g.drawText(label, r, juce::Justification::centred);
        };
        paintZoomBtn(plusX, btnY, "+");
        paintZoomBtn(minusX, btnY, "-");
    }

    /// Hit-test a click in local coordinates. Returns +1 for zoom in, -1 for zoom out, 0 for miss.
    int hitTestZoomButtons(juce::Point<int> localPos) const
    {
        auto pt = localPos.toFloat();
        if (zoomInBounds.contains(pt))  return 1;
        if (zoomOutBounds.contains(pt)) return -1;
        return 0;
    }

private:
    //==========================================================================
    // Waveform rendering
    //==========================================================================

    void paintWaveform(juce::Graphics& g, float x0,
                       float drawW, float waveMidY, float halfH,
                       int startEntry)
    {
        if (detailData.empty()) return;

        const uint8_t* data = detailData.data();
        int totalBytes = (int)detailData.size();
        int bpe = detailBytesPerEntry;

        // Use pre-computed global peak for stable normalization
        float hScale = halfH / (float)globalPeak;

        // Draw one column per pixel
        for (int px = 0; px < (int)drawW; ++px)
        {
            int eStart = startEntry + px * scale;
            int eEnd   = eStart + scale - 1;

            // Average entries in this pixel
            float sums[3] = {};
            int count = 0;
            for (int e = eStart; e <= eEnd; ++e)
            {
                if (e < 0 || e >= detailEntryCount) continue;
                int off = e * bpe;
                if (off + bpe > totalBytes) break;
                for (int b = 0; b < bpe && b < 3; ++b)
                    sums[b] += data[off + b];
                count++;
            }
            if (count == 0) continue;

            float amplitude = sums[0] / (float)count;
            if (bpe == 3)
            {
                // PWV7: mid + high + low
                float mid  = sums[0] / (float)count;
                float high = sums[1] / (float)count;
                float low  = sums[2] / (float)count;
                amplitude = mid;  // primary amplitude
                if (amplitude < 0.5f) continue;

                float barH = amplitude * hScale;
                float xp = x0 + (float)px;

                // Color from frequency distribution
                float total = low + mid + high + 0.001f;
                float highRatio = high / total;
                float r = highRatio;
                float gn = 0.45f + highRatio * 0.55f;
                float blu = 1.0f;

                g.setColour(juce::Colour::fromFloatRGBA(r, gn, blu, 0.9f));
                g.fillRect(xp, waveMidY - barH, 1.0f, barH * 2.0f);
            }
            else
            {
                // PWV5: 2 bytes -- height + color
                float height = sums[0] / (float)count;
                if (height < 0.5f) continue;
                float barH = height * hScale;
                float xp = x0 + (float)px;

                // Blue with brightness from second byte
                float bright = (bpe >= 2) ? sums[1] / ((float)count * 255.0f) : 0.5f;
                g.setColour(juce::Colour::fromFloatRGBA(bright * 0.5f, bright * 0.7f, 1.0f, 0.9f));
                g.fillRect(xp, waveMidY - barH, 1.0f, barH * 2.0f);
            }
        }
    }

    //==========================================================================
    // Beat grid ticks
    //==========================================================================

    void paintBeatTicks(juce::Graphics& g, float x0, float waveTop,
                        float drawW, float waveH,
                        int startEntry, int endEntry)
    {
        if (beatGrid.empty()) return;

        // Binary search: find first beat with entry >= startEntry
        uint32_t startMs = (uint32_t)((uint64_t)juce::jmax(0, startEntry) * 1000 / TrackMetadata::kDetailEntriesPerSecond);
        uint32_t endMs   = (uint32_t)((uint64_t)juce::jmax(0, endEntry) * 1000 / TrackMetadata::kDetailEntriesPerSecond);
        int lo = 0, hi = (int)beatGrid.size() - 1, firstBeat = (int)beatGrid.size();
        while (lo <= hi)
        {
            int mid = (lo + hi) / 2;
            if (beatGrid[(size_t)mid].timeMs < startMs)
                lo = mid + 1;
            else
                { firstBeat = mid; hi = mid - 1; }
        }

        float waveBot = waveTop + waveH;

        for (int bi = firstBeat; bi < (int)beatGrid.size(); ++bi)
        {
            auto& beat = beatGrid[(size_t)bi];
            if (beat.timeMs > endMs) break;

            int entry = msToEntry(beat.timeMs);
            float xp = x0 + entryToPixel(entry, startEntry);
            if (xp < x0 || xp > x0 + drawW) continue;

            // CDJ-3000 style beat ticks:
            // beatNumber is position within bar: 1=downbeat, 2,3,4=other beats
            bool isDownbeat = (beat.beatNumber == 1);

            if (isDownbeat)
            {
                // Downbeat: red line like CDJ-3000
                g.setColour(juce::Colour(0x40FF0000));
                g.fillRect(xp, waveTop, 1.0f, waveH);  // subtle full-height guide
                g.setColour(juce::Colour(0xCCFF2020));
                g.fillRect(xp, waveTop, 1.0f, 5.0f);         // top tick
                g.fillRect(xp, waveBot - 5.0f, 1.0f, 5.0f);  // bottom tick
            }
            else
            {
                // Other beats: short subtle ticks from top and bottom only
                g.setColour(juce::Colour(0x30FFFFFF));
                g.fillRect(xp, waveTop, 1.0f, 3.0f);         // top tick
                g.fillRect(xp, waveBot - 3.0f, 1.0f, 3.0f);  // bottom tick
            }
        }
    }

    //==========================================================================
    // Song structure phrase bar (top strip)
    //==========================================================================

    void paintPhraseBar(juce::Graphics& g, float x0, float y0,
                        float /*drawW*/, float barH,
                        int startEntry, int endEntry)
    {
        if (songStructure.empty() || beatGrid.empty()) return;

        g.setFont(juce::Font(juce::FontOptions(barH - 1.0f, juce::Font::bold)));

        for (size_t pi = 0; pi < songStructure.size(); pi++)
        {
            auto& phrase = songStructure[pi];
            uint32_t startMs = beatToMs(phrase.beatNumber);
            uint32_t endMs   = (phrase.beatCount > 0)
                ? beatToMs(phrase.beatNumber + phrase.beatCount)
                : durationMs;

            int eStart = msToEntry(startMs);
            int eEnd   = msToEntry(endMs);

            if (eEnd < startEntry || eStart > endEntry) continue;

            float px0 = x0 + entryToPixel(juce::jmax(eStart, startEntry), startEntry);
            float px1 = x0 + entryToPixel(juce::jmin(eEnd, endEntry), startEntry);
            if (px1 <= px0) continue;

            auto col = phraseColor(phrase.kind, phraseMood);

            // Filled bar
            g.setColour(col);
            g.fillRect(px0, y0, px1 - px0, barH);

            // Phrase label text (centered in visible portion)
            juce::String label = phraseName(phrase.kind, phraseMood);
            float textW = px1 - px0;
            if (textW > 18.0f && label.isNotEmpty())
            {
                g.setColour(juce::Colours::white.withAlpha(0.9f));
                g.drawText(label, (int)(px0 + 2.0f), (int)y0,
                           (int)(textW - 4.0f), (int)barH,
                           juce::Justification::centredLeft, false);
            }

            // Left edge separator (thin dark line between phrases)
            if (pi > 0 && px0 > x0 + 1.0f)
            {
                g.setColour(juce::Colour(0x80000000));
                g.fillRect(px0, y0, 1.0f, barH);
            }
        }
    }

    /// Map phrase kind to display name based on mood.
    static juce::String phraseName(uint16_t kind, uint16_t mood)
    {
        if (mood == 1) // High
        {
            switch (kind)
            {
                case 1: return "Intro";
                case 2: return "Up";
                case 3: return "Down";
                case 5: return "Chorus";
                case 6: return "Outro";
                default: return {};
            }
        }
        else if (mood == 3) // Low
        {
            switch (kind)
            {
                case 1:  return "Intro";
                case 2: case 3: case 4: return "Verse 1";
                case 5: case 6: case 7: return "Verse 2";
                case 8:  return "Bridge";
                case 9:  return "Chorus";
                case 10: return "Outro";
                default: return {};
            }
        }
        else // Mid (default)
        {
            switch (kind)
            {
                case 1:  return "Intro";
                case 2:  return "Verse 1";
                case 3:  return "Verse 2";
                case 4:  return "Verse 3";
                case 5:  return "Verse 4";
                case 6:  return "Verse 5";
                case 7:  return "Verse 6";
                case 8:  return "Bridge";
                case 9:  return "Chorus";
                case 10: return "Outro";
                default: return {};
            }
        }
    }

    /// Map phrase kind to a color (matching rekordbox conventions per mood).
    static juce::Colour phraseColor(uint16_t kind, uint16_t mood)
    {
        if (mood == 1) // High
        {
            switch (kind)
            {
                case 1: return juce::Colour(0xDD2C5FE0);  // Intro -- blue
                case 2: return juce::Colour(0xDD32BE5A);  // Up -- green
                case 3: return juce::Colour(0xDDCC8844);  // Down -- amber
                case 5: return juce::Colour(0xDDE04080);  // Chorus -- magenta/pink
                case 6: return juce::Colour(0xDD2C5FE0);  // Outro -- blue
                default: return juce::Colour(0xAA555555);
            }
        }
        else if (mood == 3) // Low
        {
            switch (kind)
            {
                case 1:                 return juce::Colour(0xDD2C5FE0);  // Intro
                case 2: case 3: case 4: return juce::Colour(0xDD32BE5A);  // Verse 1 (a/b/c)
                case 5: case 6: case 7: return juce::Colour(0xDD30A8A0);  // Verse 2 (a/b/c)
                case 8:                 return juce::Colour(0xDD8844CC);  // Bridge
                case 9:                 return juce::Colour(0xDDE04080);  // Chorus
                case 10:                return juce::Colour(0xDD2C5FE0);  // Outro
                default: return juce::Colour(0xAA555555);
            }
        }
        else // Mid (default, mood==2)
        {
            switch (kind)
            {
                case 1:  return juce::Colour(0xDD2C5FE0);  // Intro -- blue
                case 2:  return juce::Colour(0xDD32BE5A);  // Verse 1 -- green
                case 3:  return juce::Colour(0xDD30A8A0);  // Verse 2 -- teal
                case 4:  return juce::Colour(0xDD50B848);  // Verse 3 -- lime-green
                case 5:  return juce::Colour(0xDD28C0A0);  // Verse 4 -- sea
                case 6:  return juce::Colour(0xDD40B870);  // Verse 5 -- emerald
                case 7:  return juce::Colour(0xDD58B040);  // Verse 6 -- olive
                case 8:  return juce::Colour(0xDD8844CC);  // Bridge -- purple
                case 9:  return juce::Colour(0xDDE04080);  // Chorus -- magenta/pink
                case 10: return juce::Colour(0xDD2C5FE0);  // Outro -- blue
                default: return juce::Colour(0xAA555555);
            }
        }
    }

    //==========================================================================
    // Loop overlay
    //==========================================================================

    void paintLoop(juce::Graphics& g, float x0, float waveTop,
                   float /*drawW*/, float waveH,
                   int startEntry, int endEntry)
    {
        if (loopStartMs == 0 || loopEndMs == 0) return;

        int eStart = msToEntry(loopStartMs);
        int eEnd   = msToEntry(loopEndMs);
        if (eEnd < startEntry || eStart > endEntry) return;

        float px0 = x0 + entryToPixel(juce::jmax(eStart, startEntry), startEntry);
        float px1 = x0 + entryToPixel(juce::jmin(eEnd, endEntry), startEntry);
        if (px1 <= px0) return;

        // Semi-transparent orange overlay
        g.setColour(juce::Colour(0x25FF8800));
        g.fillRect(px0, waveTop, px1 - px0, waveH);

        // Vertical bars at loop boundaries
        g.setColour(juce::Colour(0xAAFF8800));
        if (eStart >= startEntry && eStart <= endEntry)
        {
            float lx = x0 + entryToPixel(eStart, startEntry);
            g.fillRect(lx, waveTop, 1.5f, waveH);
        }
        if (eEnd >= startEntry && eEnd <= endEntry)
        {
            float lx = x0 + entryToPixel(eEnd, startEntry);
            g.fillRect(lx, waveTop, 1.5f, waveH);
        }
    }

    //==========================================================================
    // Cue point markers
    //==========================================================================

    void paintCueMarkers(juce::Graphics& g, float x0, float waveTop,
                         float drawW, float waveH,
                         int startEntry, int endEntry)
    {
        g.setFont(9.0f);

        // Prefer rekordbox cues (have colors + types). Fall back to TrackMap.
        if (!rekordboxCues.empty())
        {
            for (auto& cue : rekordboxCues)
            {
                int entry = msToEntry(cue.positionMs);
                if (entry < startEntry || entry > endEntry) continue;
                float xp = x0 + entryToPixel(entry, startEntry);
                if (xp < x0 || xp > x0 + drawW) continue;

                juce::Colour cueCol = cue.hasColor
                    ? cue.getColour()
                    : defaultCueColor(cue.type);

                // Loop body overlay
                if (cue.type == TrackMetadata::RekordboxCue::Loop && cue.loopEndMs > 0)
                {
                    int endE = msToEntry(cue.loopEndMs);
                    float xEnd = x0 + entryToPixel(juce::jmin(endE, endEntry), startEntry);
                    if (xEnd > xp)
                    {
                        g.setColour(cueCol.withAlpha(0.12f));
                        g.fillRect(xp, waveTop, xEnd - xp, waveH);
                    }
                }

                // Vertical line
                g.setColour(cueCol.withAlpha(0.6f));
                g.fillRect(xp, waveTop, 1.5f, waveH);

                // Marker shape: triangle for hot cues, diamond for memory points
                if (cue.type == TrackMetadata::RekordboxCue::HotCue)
                {
                    juce::Path tri;
                    tri.addTriangle(xp - 4.0f, waveTop, xp + 4.0f, waveTop, xp, waveTop + 5.0f);
                    g.setColour(cueCol);
                    g.fillPath(tri);
                }
                else if (cue.type == TrackMetadata::RekordboxCue::MemoryPoint)
                {
                    juce::Path dia;
                    dia.addTriangle(xp - 3.0f, waveTop + 3.0f, xp + 3.0f, waveTop + 3.0f, xp, waveTop);
                    g.setColour(cueCol);
                    g.fillPath(dia);
                }
                else // Loop
                {
                    juce::Path tri;
                    tri.addTriangle(xp - 4.0f, waveTop, xp + 4.0f, waveTop, xp, waveTop + 5.0f);
                    g.setColour(cueCol);
                    g.fillPath(tri);
                }

                // Label: hot cue letter + comment
                juce::String label;
                auto letter = cue.hotCueLetter();
                if (letter.isNotEmpty()) label = letter;
                if (cue.comment.isNotEmpty())
                    label += label.isNotEmpty() ? " " + cue.comment : cue.comment;

                if (label.isNotEmpty())
                {
                    g.setColour(cueCol);
                    g.drawText(label, (int)(xp + 3.0f), (int)waveTop,
                               80, 12, juce::Justification::centredLeft, false);
                }
            }
        }
        else if (!trackMapCues.empty())
        {
            // Fallback: TrackMap cue points (all green, no types)
            for (auto& cue : trackMapCues)
            {
                int entry = msToEntry(cue.positionMs);
                if (entry < startEntry || entry > endEntry) continue;
                float xp = x0 + entryToPixel(entry, startEntry);
                if (xp < x0 || xp > x0 + drawW) continue;

                juce::Colour cueCol(0xFF00DD44);
                g.setColour(cueCol.withAlpha(0.6f));
                g.fillRect(xp, waveTop, 1.5f, waveH);

                juce::Path tri;
                tri.addTriangle(xp - 4.0f, waveTop, xp + 4.0f, waveTop, xp, waveTop + 5.0f);
                g.setColour(cueCol);
                g.fillPath(tri);

                if (cue.name.isNotEmpty())
                {
                    g.setColour(cueCol);
                    g.drawText(cue.name, (int)(xp + 3.0f), (int)waveTop,
                               60, 12, juce::Justification::centredLeft, false);
                }
            }
        }
    }

    static juce::Colour defaultCueColor(TrackMetadata::RekordboxCue::Type type)
    {
        switch (type)
        {
            case TrackMetadata::RekordboxCue::HotCue:      return juce::Colour(0xFF1ECC3C);  // green
            case TrackMetadata::RekordboxCue::MemoryPoint:  return juce::Colour(0xFFCC2020);  // red
            case TrackMetadata::RekordboxCue::Loop:         return juce::Colour(0xFFFF8800);  // orange
            default:                                        return juce::Colour(0xFF1ECC3C);
        }
    }

    //==========================================================================
    // Coordinate helpers
    //==========================================================================

    int msToEntry(uint32_t ms) const
    {
        return (int)((uint64_t)ms * TrackMetadata::kDetailEntriesPerSecond / 1000);
    }

    /// Convert entry index to pixel X offset from the left of the draw area.
    float entryToPixel(int entry, int startEntry) const
    {
        return (float)(entry - startEntry) / (float)scale;
    }

    /// Find ms for a beat number using the beat grid.
    uint32_t beatToMs(uint16_t beatNum) const
    {
        if (beatGrid.empty() || beatNum == 0) return 0;
        // Beat grid entries are in chronological order.
        // beatNum is the 1-based absolute beat index from PSSI phrases.
        // beatGrid[i].beatNumber is the position within the bar (1-4), NOT absolute.
        // So we simply index: absolute beat N = beatGrid[N-1].
        int idx = (int)beatNum - 1;
        if (idx < 0) idx = 0;
        if (idx >= (int)beatGrid.size()) idx = (int)beatGrid.size() - 1;
        return beatGrid[(size_t)idx].timeMs;
    }

    //==========================================================================
    // Data
    //==========================================================================
    std::vector<uint8_t> detailData;
    int detailEntryCount = 0;
    int detailBytesPerEntry = 0;
    uint32_t durationMs = 0;
    uint8_t globalPeak = 1;  // pre-computed peak for stable normalization
    bool hasData = false;

    std::vector<TrackMetadata::BeatEntry> beatGrid;
    std::vector<TrackMetadata::PhraseEntry> songStructure;
    uint16_t phraseMood = 0;
    std::vector<TrackMetadata::RekordboxCue> rekordboxCues;
    std::vector<CuePoint> trackMapCues;

    uint32_t loopStartMs = 0;
    uint32_t loopEndMs = 0;
    uint32_t playheadMs = 0;

    // Smooth scrolling state (PLL model)
    uint32_t targetPlayheadMs = 0;
    double smoothPlayheadMs = 0.0;
    double lastCdjUpdateTime = 0.0;
    double lastFrameTime = 0.0;
    double playheadSpeed = 0.0;  // estimated ms-per-ms (1.0 = normal speed)

    int scale = 4;  // entries per pixel (zoom)
    juce::Rectangle<float> zoomInBounds, zoomOutBounds;  // set during paint
    bool diagLogged = false;  // one-shot diagnostic flag

    // Cached bitmap for static waveform elements (3x wider than visible)
    juce::Image staticCache;
    int cacheStartEntry = 0;
    int cacheEndEntry = 0;

    void invalidateStaticCache()
    {
        staticCache = juce::Image();  // release the cached image
        cacheStartEntry = 0;
        cacheEndEntry = 0;
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WaveformDetailDisplay)
};
