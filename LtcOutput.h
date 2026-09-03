// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

#pragma once
#include <JuceHeader.h>
#include "TimecodeCore.h"
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>

class LtcOutput : private juce::AudioIODeviceCallback
{
public:
    LtcOutput() = default;
    ~LtcOutput() { stop(); }

    //==============================================================================
    // channel: 0+ = specific channel, -1 = Ch 1 + Ch 2
    // sampleRate: preferred sample rate (0 = device default)
    // bufferSize: preferred buffer size (0 = device default)
    //==============================================================================
    bool start(const juce::String& typeName, const juce::String& devName,
               int channel = 0, double sampleRate = 0, int bufferSize = 0)
    {
        stop();
        selectedChannel.store(channel, std::memory_order_relaxed);
        currentDeviceName = devName;
        currentTypeName = typeName;

        deviceManager.closeAudioDevice();

        // Register all device types without opening anything
        deviceManager.initialise(0, 128, nullptr, false);

        // Switch to requested type WITHOUT auto-opening a device (false)
        if (typeName.isNotEmpty())
            deviceManager.setCurrentAudioDeviceType(typeName, false);

        // Scan so the device name is recognised
        if (auto* type = deviceManager.getCurrentDeviceTypeObject())
            type->scanForDevices();

        // Open the specific device -- this is the ONLY open call
        auto setup = deviceManager.getAudioDeviceSetup();
        setup.outputDeviceName  = devName;
        setup.inputDeviceName   = "";
        setup.useDefaultInputChannels  = false;
        setup.useDefaultOutputChannels = true;
        if (sampleRate > 0)  setup.sampleRate = sampleRate;
        if (bufferSize > 0)  setup.bufferSize = bufferSize;
        auto err = deviceManager.setAudioDeviceSetup(setup, true);
        if (err.isNotEmpty())
            return false;

        auto* device = deviceManager.getCurrentAudioDevice();
        if (!device)
            return false;

        numChannelsAvailable = device->getActiveOutputChannels().countNumberOfSetBits();
        {
            int ch = selectedChannel.load(std::memory_order_relaxed);
            if (ch >= 0 && ch >= numChannelsAvailable)
                ch = 0;
            if (ch == -1 && numChannelsAvailable < 2)
                ch = 0;
            selectedChannel.store(ch, std::memory_order_relaxed);
        }

        currentSampleRate = device->getCurrentSampleRate();
        currentBufferSize = device->getCurrentBufferSizeSamples();

        resetEncoder();
        peakLevel.store(0.0f, std::memory_order_relaxed);
        deviceManager.addAudioCallback(this);
        isRunningFlag.store(true, std::memory_order_relaxed);
        return true;
    }

    void stop()
    {
        if (isRunningFlag.load(std::memory_order_relaxed))
        {
            deviceManager.removeAudioCallback(this);
            deviceManager.closeAudioDevice();
            isRunningFlag.store(false, std::memory_order_relaxed);
        }
    }

    bool getIsRunning() const { return isRunningFlag.load(std::memory_order_relaxed); }
    juce::String getCurrentDeviceName() const { return currentDeviceName; }
    juce::String getCurrentTypeName() const { return currentTypeName; }
    int getSelectedChannel() const { return selectedChannel.load(std::memory_order_relaxed); }
    int getChannelCount() const { return numChannelsAvailable; }
    bool isStereoMode() const { return selectedChannel.load(std::memory_order_relaxed) == -1; }
    double getActualSampleRate() const { return currentSampleRate; }
    int getActualBufferSize() const { return currentBufferSize; }

    void setTimecode(const Timecode& tc)
    {
        packedPendingTc.store(packTimecode(tc.hours, tc.minutes, tc.seconds, tc.frames),
                              std::memory_order_relaxed);
    }

    void setFrameRate(FrameRate fps)  { pendingFps.store(fps, std::memory_order_relaxed); }
    void setPitchMultiplier(double p) { pitchMultiplier.store(p, std::memory_order_relaxed); }

    /// Force encoder to re-seed from current pendingTc on next audio callback.
    /// Call when resuming from pause to avoid continuing a stale half-encoded frame.
    void reseed()
    {
        needNewFrame.store(true, std::memory_order_relaxed);
        encoderSeeded.store(false, std::memory_order_relaxed);
    }

    void setPaused(bool p)
    {
        paused.store(p, std::memory_order_relaxed);
        if (p)
            peakLevel.store(0.0f, std::memory_order_relaxed);  // meter drops immediately
    }
    bool isPaused() const             { return paused.load(std::memory_order_relaxed); }

    void setOutputGain(float gain)    { outputGain.store(juce::jlimit(0.0f, 2.0f, gain), std::memory_order_relaxed); }
    float getOutputGain() const       { return outputGain.load(std::memory_order_relaxed); }

    float getPeakLevel() const        { return peakLevel.load(std::memory_order_relaxed); }

    /// LTC user bits (32-bit "binary groups").  Displayed/entered as an
    /// 8-digit hex value; the most significant digit occupies user group 1.
    /// Takes effect on the next encoded frame; no reseed needed.
    void setUserBits(uint32_t bits)   { userBits.store(bits, std::memory_order_relaxed); }
    /// Binary group flags (bit0=BGF0, bit1=BGF1, bit2=BGF2); see SMPTE
    /// 12M-1-2008 Table 1.  0 = user-defined data / unspecified clock.
    void setBinaryGroupFlags(uint8_t f) { binaryGroupFlags.store(f & 0x7, std::memory_order_relaxed); }
    uint8_t getBinaryGroupFlags() const { return binaryGroupFlags.load(std::memory_order_relaxed); }
    uint32_t getUserBits() const      { return userBits.load(std::memory_order_relaxed); }

private:
    juce::AudioDeviceManager deviceManager;
    juce::String currentDeviceName;
    juce::String currentTypeName;
    std::atomic<bool> isRunningFlag { false };
    // selectedChannel is written in start() (UI thread) and read in
    // audioDeviceIOCallbackWithContext() (audio thread).  JUCE's
    // addAudioCallback provides a happens-before, but atomic makes
    // the cross-thread contract explicit.
    std::atomic<int> selectedChannel { 0 };
    int numChannelsAvailable = 0;
    double currentSampleRate = 48000.0;
    int currentBufferSize = 512;

    std::atomic<uint64_t> packedPendingTc { 0 };
    std::atomic<FrameRate> pendingFps { FrameRate::FPS_25 };
    std::atomic<bool> paused { false };
    std::atomic<float> outputGain { 1.0f };
    std::atomic<float> peakLevel { 0.0f };
    std::atomic<double> pitchMultiplier { 1.0 };
    // LTC user bits (SMPTE 12M "binary groups"): 32 bits = eight 4-bit
    // groups carried in every frame alongside the timecode.  Free for the
    // operator to use however they like (reel/scene/take, date, or -- as
    // requested in issue #13 -- a value read by other equipment).  Written
    // from the message thread, read once per frame in packFrame(); the
    // whole word is one atomic so a frame never carries a torn value.
    // Default 0 reproduces the previous all-zero user bits exactly.
    std::atomic<uint32_t> userBits { 0 };
    // Binary group flags, packed as bit0=BGF0, bit1=BGF1, bit2=BGF2.
    // Default 0 = user-defined data, unspecified clock (12M-1 sec. 8.4.1).
    std::atomic<uint8_t> binaryGroupFlags { 0 };

    // LTC encoder state -- mostly audio-callback-thread-only.
    // EXCEPTION: needNewFrame and encoderSeeded are also written by reseed()
    // from the message thread, so they must be atomic to avoid data races
    // (especially on ARM / Apple Silicon where non-atomic cross-thread writes
    // can produce torn reads).
    static constexpr int LTC_FRAME_BITS = 80;
    uint8_t frameBits[LTC_FRAME_BITS] = {};
    int currentBitIndex = 0;
    int halfCellIndex = 0;
    double samplePositionInHalfBit = 0.0;
    double samplesPerHalfBit = 0.0;
    float currentLevel = 1.0f;
    std::atomic<bool> needNewFrame { true };
    static constexpr float baseAmplitude = 0.8f;

    // Auto-increment state: the encoder maintains its own running timecode
    // to avoid repeating frames when the UI thread lags behind the audio clock
    Timecode encoderTc;
    std::atomic<bool> encoderSeeded { false };

    void resetEncoder()
    {
        currentBitIndex = 0;
        halfCellIndex = 0;
        samplePositionInHalfBit = 0.0;
        currentLevel = 1.0f;
        needNewFrame.store(true, std::memory_order_relaxed);
        encoderSeeded.store(false, std::memory_order_relaxed);
        encoderTc = Timecode();
        updateSamplesPerBit();
    }

    void updateSamplesPerBit()
    {
        double fps = frameRateToDouble(pendingFps.load(std::memory_order_relaxed));
        double pitch = pitchMultiplier.load(std::memory_order_relaxed);
        if (pitch <= 0.0) pitch = 1.0;
        // Scale bit duration by pitch: faster pitch -> shorter bits -> more frames/sec
        samplesPerHalfBit = currentSampleRate / (fps * pitch * LTC_FRAME_BITS * 2.0);
    }

    void packFrame()
    {
        FrameRate fps = pendingFps.load(std::memory_order_relaxed);
        Timecode pendingTc = unpackTimecode(packedPendingTc.load(std::memory_order_relaxed));

        if (!encoderSeeded.load(std::memory_order_relaxed))
        {
            encoderTc = pendingTc;
            encoderSeeded.store(true, std::memory_order_relaxed);
        }
        else
        {
            // Auto-increment from the last encoded frame
            encoderTc = incrementFrame(encoderTc, fps);

            // If the UI-provided timecode differs significantly (>1 frame),
            // re-sync to the UI value (handles seeks, source switches, jumps)
            int maxFrames = frameRateToInt(fps);
            auto toTotal = [maxFrames](const Timecode& t) -> int64_t {
                return (int64_t)t.hours * 3600 * maxFrames
                     + (int64_t)t.minutes * 60 * maxFrames
                     + (int64_t)t.seconds * maxFrames
                     + (int64_t)t.frames;
            };
            int64_t dayFrames = (int64_t)24 * 3600 * maxFrames;
            int64_t rawDiff = toTotal(pendingTc) - toTotal(encoderTc);
            // Modular distance: shortest path around the 24h wheel
            int64_t diff = ((rawDiff % dayFrames) + dayFrames) % dayFrames;
            if (diff > dayFrames / 2) diff = dayFrames - diff;
            if (diff > 1)
                encoderTc = pendingTc;
        }

        int frames  = encoderTc.frames;
        int seconds = encoderTc.seconds;
        int minutes = encoderTc.minutes;
        int hours   = encoderTc.hours;

        int frameUnits = frames % 10,  frameTens = frames / 10;
        int secUnits   = seconds % 10, secTens   = seconds / 10;
        int minUnits   = minutes % 10, minTens   = minutes / 10;
        int hourUnits  = hours % 10,   hourTens  = hours / 10;
        bool dropFrame = (fps == FrameRate::FPS_2997);

        std::memset(frameBits, 0, LTC_FRAME_BITS);

        // --- User bits (SMPTE 12M binary groups) ---
        // 32 bits laid into eight 4-bit groups.  Within each group the bits
        // are LSB-first (same convention as the BCD digits above).  The
        // groups are ordered so that the operator's hex value reads
        // left-to-right: the most significant hex digit lands in binary
        // group 1 (frame bits 4-7), matching how reel/date user bits are
        // conventionally displayed and read back.  So userBits 0x12345678
        // shows as "12345678", digit '1' in the first group.
        const uint32_t ub = userBits.load(std::memory_order_relaxed);
        static constexpr int kUserGroupStart[8] =
            { 4, 12, 20, 28, 36, 44, 52, 60 };
        for (int g = 0; g < 8; ++g)
        {
            const int shift  = (7 - g) * 4;           // group 0 = top nibble
            const int nibble = (ub >> shift) & 0xF;
            const int base   = kUserGroupStart[g];
            frameBits[base + 0] = (nibble >> 0) & 1;
            frameBits[base + 1] = (nibble >> 1) & 1;
            frameBits[base + 2] = (nibble >> 2) & 1;
            frameBits[base + 3] = (nibble >> 3) & 1;
        }

        frameBits[0] = (frameUnits >> 0) & 1;
        frameBits[1] = (frameUnits >> 1) & 1;
        frameBits[2] = (frameUnits >> 2) & 1;
        frameBits[3] = (frameUnits >> 3) & 1;

        frameBits[8] = (frameTens >> 0) & 1;
        frameBits[9] = (frameTens >> 1) & 1;

        frameBits[10] = dropFrame ? 1 : 0;

        frameBits[16] = (secUnits >> 0) & 1;
        frameBits[17] = (secUnits >> 1) & 1;
        frameBits[18] = (secUnits >> 2) & 1;
        frameBits[19] = (secUnits >> 3) & 1;

        frameBits[24] = (secTens >> 0) & 1;
        frameBits[25] = (secTens >> 1) & 1;
        frameBits[26] = (secTens >> 2) & 1;

        frameBits[32] = (minUnits >> 0) & 1;
        frameBits[33] = (minUnits >> 1) & 1;
        frameBits[34] = (minUnits >> 2) & 1;
        frameBits[35] = (minUnits >> 3) & 1;

        frameBits[40] = (minTens >> 0) & 1;
        frameBits[41] = (minTens >> 1) & 1;
        frameBits[42] = (minTens >> 2) & 1;

        frameBits[48] = (hourUnits >> 0) & 1;
        frameBits[49] = (hourUnits >> 1) & 1;
        frameBits[50] = (hourUnits >> 2) & 1;
        frameBits[51] = (hourUnits >> 3) & 1;

        frameBits[56] = (hourTens >> 0) & 1;
        frameBits[57] = (hourTens >> 1) & 1;

        frameBits[64] = 0; frameBits[65] = 0; frameBits[66] = 1; frameBits[67] = 1;
        frameBits[68] = 1; frameBits[69] = 1; frameBits[70] = 1; frameBits[71] = 1;
        frameBits[72] = 1; frameBits[73] = 1; frameBits[74] = 1; frameBits[75] = 1;
        frameBits[76] = 1; frameBits[77] = 1; frameBits[78] = 0; frameBits[79] = 1;

        // --- Binary group flags and biphase polarity correction ---
        // Per SMPTE 12M-1-2008 Table 3, three of these bit positions MOVE
        // with the frame rate:
        //
        //            30/24-frame     25-frame
        //   polarity     27              59
        //   BGF0         43              27
        //   BGF1         58              58
        //   BGF2         59              43
        //
        // BUG FIXED HERE (v1.9.11-beta22): STC previously wrote a computed
        // half-word parity into BOTH bit 27 and bit 59.  At 30/24 fps bit 59
        // is not a parity bit at all -- it is BGF2 -- so roughly half of all
        // emitted frames carried BGF2=1, BGF1=0, BGF0=0, which per Table 1
        // declares "the binary groups contain date and time zone data
        // encoded as described in SMPTE 309M".  STC was therefore telling
        // 309M-aware receivers to decode its user bits as a date, with the
        // claim flickering frame by frame according to the timecode value.
        // At 25 fps the same happened to BGF0 (declaring ISO 8-bit character
        // data).  Timecode decoding was unaffected -- biphase mark is
        // polarity insensitive and most gear ignores the BGFs -- but the
        // signalling was wrong, and it becomes actively harmful now that the
        // binary groups carry meaningful user data.
        const bool is25 = (fps == FrameRate::FPS_25);
        const int bitPolarity = is25 ? 59 : 27;
        const int bitBGF0     = is25 ? 27 : 43;
        const int bitBGF1     = 58;
        const int bitBGF2     = is25 ? 43 : 59;

        // Binary group flags.  Default 0/0/0 = "character set not specified
        // and unspecified clock time" (12M-1 sec. 8.4.1), under which the 32
        // user bits "may be assigned in any manner without restriction" --
        // exactly what STC's user-bits feature provides.  The setter exists
        // so future work (e.g. real SMPTE ST 309 date encoding, which
        // requires BGF2=1) can signal correctly.
        const uint8_t bgf = binaryGroupFlags.load(std::memory_order_relaxed);
        frameBits[bitBGF0] = (bgf >> 0) & 1;
        frameBits[bitBGF1] = (bgf >> 1) & 1;
        frameBits[bitBGF2] = (bgf >> 2) & 1;

        // Biphase mark polarity correction (12M-1 sec. 9.2.3): the bit is set
        // so the whole 80-bit codeword contains an even number of logical
        // zeros.  Normative rule: if the number of logical zeros in bits
        // 0-63, excluding the correction bit itself, is odd, set it to 1.
        // Must run last -- every other bit in 0..63 has to be final.
        frameBits[bitPolarity] = 0;
        int zeros = 0;
        for (int i = 0; i <= 63; ++i)
            if (i != bitPolarity && frameBits[i] == 0)
                ++zeros;
        frameBits[bitPolarity] = (zeros & 1) ? 1 : 0;
    }

    //==============================================================================
    void audioDeviceIOCallbackWithContext(const float* const*, int,
                                          float* const* outputChannelData,
                                          int numOutputChannels, int numSamples,
                                          const juce::AudioIODeviceCallbackContext&) override
    {
        for (int ch = 0; ch < numOutputChannels; ch++)
            if (outputChannelData[ch])
                std::memset(outputChannelData[ch], 0, sizeof(float) * (size_t)numSamples);

        if (paused.load(std::memory_order_relaxed))
            return;

        int selCh = selectedChannel.load(std::memory_order_relaxed);
        bool stereoMode = (selCh == -1);
        int primaryCh = stereoMode ? 0 : selCh;
        if (primaryCh >= numOutputChannels || !outputChannelData[primaryCh])
            return;

        float* output  = outputChannelData[primaryCh];
        float* output2 = (stereoMode && numOutputChannels >= 2 && outputChannelData[1])
                            ? outputChannelData[1] : nullptr;
        const float amplitude = baseAmplitude * outputGain.load(std::memory_order_relaxed);

        float peak = 0.0f;
        for (int i = 0; i < numSamples; ++i)
        {
            if (needNewFrame.load(std::memory_order_relaxed))
            {
                updateSamplesPerBit();
                packFrame();
                currentBitIndex = 0;
                halfCellIndex = 0;
                samplePositionInHalfBit = 0.0;
                needNewFrame.store(false, std::memory_order_relaxed);
                // Do NOT invert currentLevel here -- the mandatory start-of-bit
                // transition for bit 0 was already applied when the previous
                // frame's last bit completed (halfCellIndex 1 -> 0 branch).
                // An extra inversion here would cancel it out, creating a
                // biphase parity error.
            }

            float sample = currentLevel * amplitude;
            output[i] = sample;
            if (output2) output2[i] = sample;
            float a = std::abs(sample);
            if (a > peak) peak = a;
            samplePositionInHalfBit += 1.0;

            if (samplePositionInHalfBit >= samplesPerHalfBit)
            {
                samplePositionInHalfBit -= samplesPerHalfBit;

                if (halfCellIndex == 0)
                {
                    halfCellIndex = 1;
                    if (frameBits[currentBitIndex] == 1)
                        currentLevel = -currentLevel;
                }
                else
                {
                    halfCellIndex = 0;
                    currentBitIndex++;

                    // Mandatory biphase-mark transition at start of every
                    // bit cell -- including bit 0 of the next frame.
                    currentLevel = -currentLevel;

                    if (currentBitIndex >= LTC_FRAME_BITS)
                        needNewFrame.store(true, std::memory_order_relaxed);
                }
            }
        }
        peakLevel.store(peak, std::memory_order_relaxed);
    }

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override
    {
        if (device)
        {
            currentSampleRate = device->getCurrentSampleRate();
            currentBufferSize = device->getCurrentBufferSizeSamples();
            numChannelsAvailable = device->getActiveOutputChannels().countNumberOfSetBits();
        }
        resetEncoder();
    }

    void audioDeviceStopped() override
    {
        peakLevel.store(0.0f, std::memory_order_relaxed);
    }
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LtcOutput)
};
