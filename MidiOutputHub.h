// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

#pragma once
#include <JuceHeader.h>
#include <algorithm>
#include <functional>
#include <memory>
#include <vector>

//==============================================================================
// MidiOutputHub -- one open MIDI output per physical port, shared by every
// sender in the application, with every message to a port sent under that
// port's lock.  (DESIGN D32, issue #23)
//
// Three problems, one cause: nothing owned a MIDI port.
//
//  * Since JUCE 8.0.11 juce::MidiOutput::sendMessageNow is not safe to call
//    from two threads.  It clears and refills a member packet buffer and
//    hands its iterators to the device, with no lock; and on Windows every
//    MidiOutput opened on one port shares one device whose converter has no
//    lock either.  STC sends on one port from the MTC timer thread (quarter
//    frames), the message thread (Full Frame on every stop, seek, hot cue and
//    track load; triggers; mixer forward) and the MIDI clock thread.  When
//    two collide, JUCE's packet iterator can step past its end, and a release
//    build then reads the heap as MIDI until it faults.  Shown with JUCE's own
//    classes in tools/audit/midi_send_race_sim.cpp, and by ThreadSanitizer.
//  * The trigger output borrowed the MTC output's raw MidiOutput pointer and
//    the MIDI clock thread kept a copy of it, so turning MTC OUT off (or
//    changing its device) deleted the object under the clock.
//  * The code assumed "Windows forbids two handles to the same MIDI port".
//    Between processes it does; inside one, JUCE shares the port between
//    MidiOutput objects, so two engines could both stream MTC into one cable.
//
// A sender acquires a Port by device and keeps the shared_ptr for as long as
// it sends; the port closes when the last holder lets go.  Every message goes
// through Port::send, under the port's lock, so one port carries one message
// at a time whatever thread it comes from.  The lock is per port: a Full
// Frame holding one cable for the milliseconds a SysEx takes (3.2 ms of wire
// time on DIN, JUCE waits for the driver to finish) does not delay another.
//
// MTC is exclusive per port.  MTC has no channel, so two MTC streams in one
// cable cannot be received; claimMtc() records which sender streams MTC on a
// port and refuses a second one, naming the first.  Triggers, MIDI clock and
// mixer forward share a port with MTC and with each other, as before.
//
// Threads: acquire, claimMtc, releaseMtc and dropping the last reference to a
// port happen on the message thread (the port closes its device there).
// send() is for any thread.  The hub keeps no strong reference, so nothing is
// left to close at static teardown once the engines are gone.
//==============================================================================
class MidiOutputHub
{
public:
    class Port
    {
    public:
        ~Port()
        {
            JUCE_ASSERT_MESSAGE_THREAD
            const juce::ScopedLock sl(sendLock);
            output.reset();
        }

        /// Send one message.  Any thread; serialised with every other send on
        /// this port.  A long message (SysEx) holds the port until the driver
        /// is done with it, which is what the wire does anyway.
        void send(const juce::MidiMessage& message)
        {
            const juce::ScopedLock sl(sendLock);
            if (output != nullptr)
                output->sendMessageNow(message);
        }

        const juce::String& getName() const       { return device.name; }
        const juce::String& getIdentifier() const { return device.identifier; }

        /// Claim this port for an MTC stream.  Message thread.  Returns false
        /// if another sender already streams MTC here; `heldBy` then says who.
        /// Claiming again with the same owner is a no-op that succeeds.
        bool claimMtc(const void* owner, std::function<juce::String()> ownerName, juce::String& heldBy)
        {
            JUCE_ASSERT_MESSAGE_THREAD
            heldBy.clear();
            if (mtcOwner != nullptr && mtcOwner != owner)
            {
                heldBy = mtcOwnerName ? mtcOwnerName() : juce::String("ANOTHER ENGINE");
                return false;
            }
            mtcOwner = owner;
            mtcOwnerName = std::move(ownerName);
            return true;
        }

        /// Give the MTC claim back.  Message thread.  Only the owner can.
        void releaseMtc(const void* owner)
        {
            JUCE_ASSERT_MESSAGE_THREAD
            if (mtcOwner == owner)
            {
                mtcOwner = nullptr;
                mtcOwnerName = nullptr;
            }
        }

        bool hasMtcOwner() const { return mtcOwner != nullptr; }

    private:
        friend class MidiOutputHub;
        Port(const juce::MidiDeviceInfo& d, std::unique_ptr<juce::MidiOutput> o)
            : device(d), output(std::move(o)) {}

        juce::MidiDeviceInfo device;
        std::unique_ptr<juce::MidiOutput> output;
        juce::CriticalSection sendLock;

        const void* mtcOwner = nullptr;                 // message thread
        std::function<juce::String()> mtcOwnerName;     // message thread

        JUCE_DECLARE_NON_COPYABLE(Port)
    };

    using PortPtr = std::shared_ptr<Port>;

    static MidiOutputHub& get()
    {
        static MidiOutputHub hub;
        return hub;
    }

    /// Open `device`, or share it if a sender already has it open.  Message
    /// thread.  nullptr if the device cannot be opened (unplugged, or held by
    /// another application).
    PortPtr acquire(const juce::MidiDeviceInfo& device)
    {
        JUCE_ASSERT_MESSAGE_THREAD
        if (device.identifier.isEmpty())
            return nullptr;

        ports.erase(std::remove_if(ports.begin(), ports.end(),
                                   [] (const std::weak_ptr<Port>& w) { return w.expired(); }),
                    ports.end());

        for (auto& w : ports)
            if (auto p = w.lock())
                if (p->getIdentifier() == device.identifier)
                    return p;

        auto output = juce::MidiOutput::openDevice(device.identifier);
        if (output == nullptr)
            return nullptr;

        PortPtr port(new Port(device, std::move(output)));
        ports.push_back(port);
        return port;
    }

private:
    MidiOutputHub() = default;
    ~MidiOutputHub() = default;

    std::vector<std::weak_ptr<Port>> ports;   // message thread

    JUCE_DECLARE_NON_COPYABLE(MidiOutputHub)
};
