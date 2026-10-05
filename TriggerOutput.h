// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

#pragma once
#include <JuceHeader.h>
#include <atomic>
#include "OscSender.h"
#include "AppSettings.h"
#include "MidiOutputHub.h"

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

        // The same port object as the MTC output when both name one device
        // (MidiOutputHub, D32), so the two never race on it.
        ownPort = MidiOutputHub::get().acquire(midiDevices[deviceIndex]);
        if (ownPort)
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
        sharedPort = nullptr;      // drop the MTC output's port
        ownPort = nullptr;         // and our own; a port closes with its last sender
        currentMidiDeviceName.clear();
    }

    bool isMidiOpen() const { return getActiveMidi() != nullptr; }
    bool hasOwnMidiOpen() const { return ownPort != nullptr; }
    juce::String getCurrentMidiDeviceName() const { return currentMidiDeviceName; }

    //--------------------------------------------------------------------------
    // Shared MIDI output -- the trigger output uses the MTC output's port when
    // both target the same MIDI device.  Every send goes through
    // MidiOutputHub::Port::send, which serialises all threads on the port;
    // juce::MidiOutput::sendMessageNow on its own is NOT thread-safe (JUCE
    // 8.0.11 onwards, see MidiOutputHub.h).
    //--------------------------------------------------------------------------

    /// Use the MTC output's port instead of (or alongside) our own.  Pass
    /// nullptr to clear and fall back to our own device.  A running MIDI clock
    /// moves to a new shared port; when the shared port is cleared the clock
    /// keeps its own reference, so the port stays open under it until the
    /// clock is moved or stopped (it used to keep a raw pointer to an object
    /// the MTC output then deleted).
    void setSharedMidiOutput(MidiOutputHub::PortPtr shared)
    {
        sharedPort = shared;
        // If sharing and clock is running, redirect it to the shared output
        if (shared && clockTimer.isTimerRunning())
            clockTimer.updateOutput(std::move(shared));
    }

    /// Drop our OWN port (if open) without affecting the shared one.  Used
    /// before MtcOutput opens the same device -- a double open conflicted
    /// before D32; MidiOutputHub now hands both the same port.
    void releaseOwnMidi()
    {
        if (ownPort)
        {
            // If clock was using our own output, redirect to shared (if available)
            if (clockTimer.isTimerRunning())
            {
                if (sharedPort)
                    clockTimer.updateOutput(sharedPort);
                else
                    clockTimer.stop();
            }
            ownPort = nullptr;
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
    /// JUCE message thread (60Hz timer callback).  Port::send is synchronous
    /// and may briefly block (~microseconds on healthy drivers, or up to a few
    /// ms if another thread is sending a SysEx on the same port).  For Note On +
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

    /// Fire a cue point trigger (MIDI + OSC).
    /// Same dispatch as fire() but reads from CuePoint fields.
    void fireCuePoint(const CuePoint& cue)
    {
        if (midiEnabled && cue.hasMidiTrigger())
        {
            auto* midi = getActiveMidi();
            if (midi)
            {
                int ch = juce::jlimit(0, 15, cue.midiChannel) + 1;
                if (cue.midiNoteNum >= 0)
                {
                    int note = juce::jlimit(0, 127, cue.midiNoteNum);
                    int vel  = juce::jlimit(0, 127, cue.midiNoteVel);
                    sendTriggerNote(midi, ch, note, vel);
                }
                if (cue.midiCCNum >= 0)
                {
                    int cc  = juce::jlimit(0, 127, cue.midiCCNum);
                    int val = juce::jlimit(0, 127, cue.midiCCVal);
                    midi->send(juce::MidiMessage::controllerEvent(ch, cc, val));
                }
            }
        }
        if (oscEnabled && cue.hasOscTrigger() && oscSender.isConnected())
        {
            oscSender.send(cue.oscAddress, cue.oscArgs);
        }
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
        midi->send(msg);
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
        midi->send(msg);
    }

    /// Send a raw OSC message with a single float value.
    /// Uses the zero-allocation fast path -- no String creation, no tokenization.
    /// The connected check is inside sendFloatDirect (single lock acquisition).
    void sendOscFloat(const juce::String& address, float value)
    {
        oscSender.sendFloatDirect(address, value);
    }

    /// One string argument, sent as it is: a quote in the BPM command
    /// template no longer splits it (AUDIT NET-12).
    void sendOscString(const juce::String& address, const juce::String& value)
    {
        oscSender.sendString(address, value);
    }

    //--------------------------------------------------------------------------
    // MIDI Clock -- 24 pulses per quarter note at the current BPM.
    // Runs on a dedicated HighResolutionTimer thread (1ms tick).
    // Uses a fractional accumulator for drift-free pulse timing.
    //--------------------------------------------------------------------------

    void startMidiClock(double bpm)
    {
        auto port = sharedPort ? sharedPort : ownPort;
        if (!port) return;
        clockTimer.start(bpm, std::move(port));
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
    // The clock holds its own reference to its port, under portLock: the
    // timer callback sends while holding it, and start/stop/updateOutput swap
    // the port under it on the message thread and let the old one go outside
    // it -- so a port is never closed under a pulse, and the last reference is
    // always dropped on the message thread.  stopTimer() is never called with
    // portLock held (it waits for the callback, which takes it).
    class MidiClockTimer : public juce::HighResolutionTimer
    {
    public:
        MidiClockTimer() = default;
        ~MidiClockTimer() override { stopTimer(); }

        void start(double bpm, MidiOutputHub::PortPtr output)
        {
            stopTimer();   // a restart must not reset the accumulator under a running callback
            auto previous = swapPort(output);
            setBpm(bpm);
            resetClock();
            // Send MIDI Start (0xFA)
            if (output) output->send(juce::MidiMessage(0xFA));
            startTimer(1);
        }

        void stop()
        {
            stopTimer();
            // Send MIDI Stop (0xFC)
            auto previous = swapPort(nullptr);
            if (previous) previous->send(juce::MidiMessage(0xFC));
        }

        void setBpm(double bpm)
        {
            if (bpm >= 20.0 && bpm <= 999.0)
                pulsesPerMs.store(bpm * 24.0 / 60000.0, std::memory_order_relaxed);
        }

        /// Redirect clock output to a different port (e.g. when switching from
        /// own device to the shared MtcOutput port).  Message thread.
        void updateOutput(MidiOutputHub::PortPtr newOut) { swapPort(std::move(newOut)); }

        void hiResTimerCallback() override
        {
            double ppms = pulsesPerMs.load(std::memory_order_relaxed);
            const juce::ScopedLock sl(portLock);
            if (ppms <= 0.0 || port == nullptr) return;

            // Pulses owed = elapsed time x pulses per ms.  Counting callbacks
            // instead (one pulse-worth per call) made the tempo error equal
            // the timer's rate error and lost every missed callback; MTC and
            // Art-Net already use the elapsed-time accumulator.  A gap over
            // 100 ms (suspended thread) is dropped rather than caught up
            // with a burst of clocks.
            const double now = juce::Time::getMillisecondCounterHiRes();
            if (lastCallbackMs <= 0.0) lastCallbackMs = now;
            double elapsed = now - lastCallbackMs;
            lastCallbackMs = now;
            if (elapsed > 100.0) elapsed = 0.0;

            accumulator += elapsed * ppms;
            int burst = 0;
            while (accumulator >= 1.0 && burst < 4)
            {
                port->send(juce::MidiMessage((uint8_t)0xF8));
                accumulator -= 1.0;
                ++burst;
            }
        }

        void resetClock() { accumulator = 0.0; lastCallbackMs = 0.0; }

    private:
        /// Install `next`, return the port it replaces (to be released by the
        /// caller, outside the lock).
        MidiOutputHub::PortPtr swapPort(MidiOutputHub::PortPtr next)
        {
            const juce::ScopedLock sl(portLock);
            std::swap(port, next);
            return next;
        }

        juce::CriticalSection portLock;
        MidiOutputHub::PortPtr port;   // under portLock
        std::atomic<double> pulsesPerMs { 0.048 };  // default 120 BPM
        double accumulator = 0.0;
        double lastCallbackMs = 0.0;
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

        // Note On, Note Off after a short hold (see sendTriggerNote).
        if (entry.midiNoteNum >= 0)
        {
            int note = juce::jlimit(0, 127, entry.midiNoteNum);
            int vel  = juce::jlimit(0, 127, entry.midiNoteVel);
            sendTriggerNote(midi, ch, note, vel);
        }

        // Control Change
        if (entry.midiCCNum >= 0)
        {
            int cc  = juce::jlimit(0, 127, entry.midiCCNum);
            int val = juce::jlimit(0, 127, entry.midiCCVal);
            midi->send(juce::MidiMessage::controllerEvent(ch, cc, val));
        }
    }

    //--------------------------------------------------------------------------
    // OSC dispatch
    //--------------------------------------------------------------------------
    void fireOsc(const TrackMapEntry& entry)
    {
        if (!oscSender.isConnected() || !entry.hasOscTrigger()) return;

        // Built-in variables in oscArgs, each a token of its own:
        //   {artist}   -> artist string
        //   {title}    -> title string
        //   {offset}   -> timecode offset string
        // OscSender puts the value in as one string argument after it has
        // split the text, so nothing in a title can be read as syntax.  They
        // used to be pasted in as s:"<value>" before the split, and a quote
        // in a title -- Song (12" Mix) -- ended the string there and
        // swallowed the arguments after it (AUDIT NET-12).
        oscSender.send(entry.oscAddress, entry.oscArgs,
                       { { "{artist}", entry.artist },
                         { "{title}",  entry.title },
                         { "{offset}", entry.timecodeOffset } });
    }

    //--------------------------------------------------------------------------
    // Members
    //--------------------------------------------------------------------------
    // MIDI -- both message thread only (the clock keeps its own reference)
    MidiOutputHub::PortPtr ownPort;       // own device (when not sharing)
    MidiOutputHub::PortPtr sharedPort;    // the MtcOutput's port, when on the same device
    juce::Array<juce::MidiDeviceInfo> midiDevices;
    juce::String currentMidiDeviceName;
    bool midiEnabled = false;

    /// Returns the active port: shared if set, else own.  Message thread.
    MidiOutputHub::Port* getActiveMidi() const
    {
        return sharedPort ? sharedPort.get() : ownPort.get();
    }

    /// Trigger note: Note On now, Note Off after a short hold.  A zero-length
    /// note (On immediately followed by Off) is fine for consoles that act on
    /// the On, but a receiver that needs the note held -- a media server in
    /// piano mode, a DAW -- gets nothing usable from it, and a "flash" key
    /// mapped to a note gets a press it can see.  kTriggerNoteHoldMs is
    /// well above any MIDI interface's latency and well below a musical
    /// note.  Message thread only (triggers fire from tick()); the Off is
    /// posted through a WeakReference so a trigger fired just before the
    /// engine is destroyed does not reach a dead object.
    static constexpr int kTriggerNoteHoldMs = 100;

    void sendTriggerNote(MidiOutputHub::Port* midi, int channel1to16, int note, int velocity)
    {
        midi->send(juce::MidiMessage::noteOn(channel1to16, note, (uint8_t) velocity));
        juce::WeakReference<TriggerOutput> weak(this);
        juce::Timer::callAfterDelay(kTriggerNoteHoldMs, [weak, channel1to16, note]
        {
            if (auto* self = weak.get())
                if (auto* out = self->getActiveMidi())
                    out->send(juce::MidiMessage::noteOff(channel1to16, note));
        });
    }

    // OSC
    OscSender oscSender;
    juce::String oscIp = "127.0.0.1";
    int oscPort = 53000;
    bool oscEnabled = false;

    std::string lastFiredTrackKey;

    JUCE_DECLARE_WEAK_REFERENCEABLE(TriggerOutput)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TriggerOutput)
};
