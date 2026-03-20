// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

#pragma once
#include <JuceHeader.h>
#include <atomic>
#include "OscSender.h"
#include "AppSettings.h"

//==============================================================================
// TriggerOutput -- Dispatches MIDI and OSC messages on track changes.
//
// Owned by TimecodeEngine, one per engine instance.
// When a track change is detected and the track has triggers configured,
// fires the appropriate MIDI and/or OSC messages.
//
// MIDI output device is independent from MTC output.
//==============================================================================
class TriggerOutput
{
public:
    TriggerOutput() = default;
    ~TriggerOutput() { stopMidi(); }

    //--------------------------------------------------------------------------
    // MIDI output device management
    //--------------------------------------------------------------------------
    void refreshMidiDevices()
    {
        midiDevices = juce::MidiOutput::getAvailableDevices();
    }

    int getMidiDeviceCount() const { return (int)midiDevices.size(); }

    juce::String getMidiDeviceName(int index) const
    {
        if (index >= 0 && index < (int)midiDevices.size())
            return midiDevices[index].name;
        return {};
    }

    int findMidiDeviceByName(const juce::String& name) const
    {
        for (int i = 0; i < (int)midiDevices.size(); ++i)
            if (midiDevices[i].name == name)
                return i;
        return -1;
    }

    bool startMidi(int deviceIndex)
    {
        stopMidi();
        if (deviceIndex < 0 || deviceIndex >= (int)midiDevices.size())
            return false;

        midiOutput = juce::MidiOutput::openDevice(midiDevices[deviceIndex].identifier);
        if (midiOutput)
        {
            currentMidiDeviceName = midiDevices[deviceIndex].name;
            return true;
        }
        return false;
    }

    bool startMidiByName(const juce::String& name)
    {
        refreshMidiDevices();
        int idx = findMidiDeviceByName(name);
        return idx >= 0 && startMidi(idx);
    }

    void stopMidi()
    {
        clockTimer.stop();
        sharedMidiOut = nullptr;   // release borrowed pointer (not owned)
        midiOutput.reset();        // close own device if open
        currentMidiDeviceName.clear();
    }

    bool isMidiOpen() const { return getActiveMidi() != nullptr; }
    bool hasOwnMidiOpen() const { return midiOutput != nullptr; }
    juce::String getCurrentMidiDeviceName() const { return currentMidiDeviceName; }

    //--------------------------------------------------------------------------
    // Shared MIDI output -- allows TriggerOutput to piggyback on MtcOutput's
    // open device handle when both target the same MIDI port.
    // juce::MidiOutput::sendMessageNow() is thread-safe (internal CriticalSection).
    //--------------------------------------------------------------------------

    /// Set an external MidiOutput to use instead of (or alongside) our own.
    /// Pass nullptr to clear and fall back to own device.
    void setSharedMidiOutput(juce::MidiOutput* shared)
    {
        sharedMidiOut = shared;
        // If sharing and clock is running, redirect it to the shared output
        if (shared && clockTimer.isTimerRunning())
            clockTimer.updateOutput(shared);
    }

    /// Close our OWN MidiOutput handle (if open) without affecting the shared pointer.
    /// Used before MtcOutput opens the same device to avoid double-open conflict.
    void releaseOwnMidi()
    {
        if (midiOutput)
        {
            // If clock was using our own output, redirect to shared (if available)
            if (clockTimer.isTimerRunning())
            {
                if (sharedMidiOut)
                    clockTimer.updateOutput(sharedMidiOut);
                else
                    clockTimer.stop();
            }
            midiOutput.reset();
        }
    }

    //--------------------------------------------------------------------------
    // OSC destination management
    //--------------------------------------------------------------------------
    bool connectOsc(const juce::String& ip, int port)
    {
        oscIp = ip;
        oscPort = port;
        return oscSender.connect(ip, port);
    }

    void disconnectOsc() { oscSender.disconnect(); }
    bool isOscConnected() const { return oscSender.isConnected(); }

    void updateOscDestination(const juce::String& ip, int port)
    {
        if (ip != oscIp || port != oscPort)
        {
            oscIp = ip;
            oscPort = port;
            if (oscSender.isConnected())
                oscSender.connect(ip, port);    // reconnect to new dest
        }
    }

    juce::String getOscDestination() const
    {
        return oscIp + ":" + juce::String(oscPort);
    }

    //--------------------------------------------------------------------------
    // Enable flags
    //--------------------------------------------------------------------------
    void setMidiEnabled(bool enabled) { midiEnabled = enabled; }
    void setOscEnabled(bool enabled) { oscEnabled = enabled; }
    bool isMidiEnabled() const { return midiEnabled; }
    bool isOscEnabled() const { return oscEnabled; }

    //--------------------------------------------------------------------------
    // Fire trigger for a track change
    //--------------------------------------------------------------------------

    /// Call this when a track change is detected and the entry is found in TrackMap.
    /// Sends MIDI and/or OSC based on the entry's per-track config and the
    /// global enable flags.
    ///
    /// NOTE: This method is called from TimecodeEngine::tick() which runs on the
    /// JUCE message thread (60Hz timer callback).  sendMessageNow() is synchronous
    /// and may briefly block (~microseconds on healthy drivers).  For Note On +
    /// Note Off back-to-back, two synchronous calls are made.  This is acceptable
    /// for show control trigger use cases but could cause a UI stutter if a MIDI
    /// driver is exceptionally slow.
    void fire(const TrackMapEntry& entry)
    {
        if (midiEnabled)
            fireMidi(entry);
        if (oscEnabled)
            fireOsc(entry);

        lastFiredTrackKey = entry.key();
    }

    std::string getLastFiredTrackKey() const { return lastFiredTrackKey; }

    //--------------------------------------------------------------------------
    // Continuous control forwarding (crossfader, BPM)
    // Called from TimecodeEngine tick -- sends MIDI CC and/or OSC
    //--------------------------------------------------------------------------

    /// Send a MIDI CC message. Channel is 1-based (1-16).
    /// Only sends if MIDI output is open (ignores midiEnabled flag --
    /// CC forward has its own enable).
    void sendCC(int channel, int cc, int value)
    {
        auto* midi = getActiveMidi();
        if (!midi) return;
        auto msg = juce::MidiMessage::controllerEvent(
            juce::jlimit(1, 16, channel),
            juce::jlimit(0, 127, cc),
            juce::jlimit(0, 127, value));
        midi->sendMessageNow(msg);
    }

    /// Send a MIDI Note On message for continuous fader control.
    /// Channel is 1-based (1-16), note 0-127, velocity 0-127.
    /// Used by grandMA2/MA3: note = executor number, velocity = fader position.
    /// No Note Off is sent -- velocity 0 is the "fader closed" state and
    /// sending Note Off would reset the receiver to an undefined state.
    void sendNote(int channel, int note, int velocity)
    {
        auto* midi = getActiveMidi();
        if (!midi) return;
        auto msg = juce::MidiMessage::noteOn(
            juce::jlimit(1, 16, channel),
            juce::jlimit(0, 127, note),
            (uint8_t)juce::jlimit(0, 127, velocity));
        midi->sendMessageNow(msg);
    }

    /// Send a raw OSC message with a single float value.
    /// Uses the zero-allocation fast path -- no String creation, no tokenization.
    /// The connected check is inside sendFloatDirect (single lock acquisition).
    void sendOscFloat(const juce::String& address, float value)
    {
        oscSender.sendFloatDirect(address, value);
    }

    //--------------------------------------------------------------------------
    // MIDI Clock -- 24 pulses per quarter note at the current BPM.
    // Runs on a dedicated HighResolutionTimer thread (1ms tick).
    // Uses a fractional accumulator for drift-free pulse timing.
    //--------------------------------------------------------------------------

    void startMidiClock(double bpm)
    {
        auto* midi = getActiveMidi();
        if (!midi) return;
        clockTimer.start(bpm, midi);
    }

    void stopMidiClock()
    {
        clockTimer.stop();
    }

    void updateMidiClockBpm(double bpm)
    {
        clockTimer.setBpm(bpm);
    }

    bool isMidiClockRunning() const { return clockTimer.isTimerRunning(); }

private:
    //--------------------------------------------------------------------------
    // MIDI Clock timer -- 1ms resolution, fractional accumulator
    //--------------------------------------------------------------------------
    class MidiClockTimer : public juce::HighResolutionTimer
    {
    public:
        MidiClockTimer() = default;
        ~MidiClockTimer() override { stopTimer(); }

        void start(double bpm, juce::MidiOutput* output)
        {
            midiOut.store(output, std::memory_order_relaxed);
            setBpm(bpm);
            accumulator = 0.0;
            // Send MIDI Start (0xFA)
            if (output) output->sendMessageNow(juce::MidiMessage(0xFA));
            startTimer(1);
        }

        void stop()
        {
            stopTimer();
            // Send MIDI Stop (0xFC)
            auto* out = midiOut.load(std::memory_order_relaxed);
            if (out) out->sendMessageNow(juce::MidiMessage(0xFC));
            midiOut.store(nullptr, std::memory_order_relaxed);
        }

        void setBpm(double bpm)
        {
            if (bpm >= 20.0 && bpm <= 999.0)
                pulsesPerMs.store(bpm * 24.0 / 60000.0, std::memory_order_relaxed);
        }

        /// Redirect clock output to a different MidiOutput (e.g. when switching
        /// from own device to shared MtcOutput device).  Now truly thread-safe
        /// via atomic store -- timer thread reads via atomic load.
        void updateOutput(juce::MidiOutput* newOut) { midiOut.store(newOut, std::memory_order_relaxed); }

        void hiResTimerCallback() override
        {
            double ppms = pulsesPerMs.load(std::memory_order_relaxed);
            auto* out = midiOut.load(std::memory_order_relaxed);
            if (ppms <= 0.0 || !out) return;

            accumulator += ppms;
            while (accumulator >= 1.0)
            {
                out->sendMessageNow(juce::MidiMessage((uint8_t)0xF8));
                accumulator -= 1.0;
            }
        }

    private:
        std::atomic<juce::MidiOutput*> midiOut { nullptr };
        std::atomic<double> pulsesPerMs { 0.048 };  // default 120 BPM
        double accumulator = 0.0;
    };

    MidiClockTimer clockTimer;
    //--------------------------------------------------------------------------
    // MIDI dispatch
    //--------------------------------------------------------------------------
    void fireMidi(const TrackMapEntry& entry)
    {
        auto* midi = getActiveMidi();
        if (!midi || !entry.hasMidiTrigger()) return;

        int ch = juce::jlimit(0, 15, entry.midiChannel) + 1;  // 1-based for JUCE

        // Note On: fire-and-forget trigger (Note On + immediate Note Off).
        // Zero-duration notes are standard for lighting/show control triggers.
        if (entry.midiNoteNum >= 0)
        {
            int note = juce::jlimit(0, 127, entry.midiNoteNum);
            int vel  = juce::jlimit(0, 127, entry.midiNoteVel);
            midi->sendMessageNow(juce::MidiMessage::noteOn(ch, note, (uint8_t)vel));
            midi->sendMessageNow(juce::MidiMessage::noteOff(ch, note));
        }

        // Control Change
        if (entry.midiCCNum >= 0)
        {
            int cc  = juce::jlimit(0, 127, entry.midiCCNum);
            int val = juce::jlimit(0, 127, entry.midiCCVal);
            midi->sendMessageNow(juce::MidiMessage::controllerEvent(ch, cc, val));
        }
    }

    //--------------------------------------------------------------------------
    // OSC dispatch
    //--------------------------------------------------------------------------
    void fireOsc(const TrackMapEntry& entry)
    {
        if (!oscSender.isConnected() || !entry.hasOscTrigger()) return;

        // Expand built-in variables in oscArgs:
        //   {artist}   -> artist string (quoted for space safety)
        //   {title}    -> title string (quoted for space safety)
        //   {offset}   -> timecode offset string
        juce::String args = entry.oscArgs;
        args = args.replace("{artist}",  "s:\"" + entry.artist + "\"");
        args = args.replace("{title}",   "s:\"" + entry.title + "\"");
        args = args.replace("{offset}",  "s:\"" + entry.timecodeOffset + "\"");

        oscSender.send(entry.oscAddress, args);
    }

    //--------------------------------------------------------------------------
    // Members
    //--------------------------------------------------------------------------
    // MIDI
    std::unique_ptr<juce::MidiOutput> midiOutput;     // own device (when not sharing)
    juce::MidiOutput* sharedMidiOut = nullptr;         // borrowed from MtcOutput (not owned)
    juce::Array<juce::MidiDeviceInfo> midiDevices;
    juce::String currentMidiDeviceName;
    bool midiEnabled = false;

    /// Returns the active MidiOutput: shared if set, else own.
    juce::MidiOutput* getActiveMidi() const
    {
        return sharedMidiOut ? sharedMidiOut : midiOutput.get();
    }

    // OSC
    OscSender oscSender;
    juce::String oscIp = "127.0.0.1";
    int oscPort = 53000;
    bool oscEnabled = false;

    std::string lastFiredTrackKey;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TriggerOutput)
};
