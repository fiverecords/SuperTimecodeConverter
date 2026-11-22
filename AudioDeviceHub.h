// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <memory>
#include <vector>

//==============================================================================
// AudioDeviceHub -- one open audio device per physical interface, shared by
// every component that uses it.
//
// Until 2026-09 each audio component (LTC in, LTC out, Audio Thru, Audio BPM,
// the generator player, per engine) owned its own juce::AudioDeviceManager and
// opened the interface for itself.  Two consequences: an ASIO driver, which
// has one sample rate and one buffer for the whole interface, was opened
// several times by clients that each believed their own settings; and
// whether that worked at all depended on the driver tolerating several
// clients, which many do not.
//
// The hub opens each interface once.  A component acquires the device for
// the direction it needs (input or output), gets registered on a fan-out
// callback, and releases it when it stops; the device closes when the last
// client leaves.  Sample rate and buffer size are properties of the device
// (DESIGN D23): the request that opens it sets them; a client that joins an
// open device adapts to what is running, whatever it asked for; the global
// setting reaches open devices through reconfigureAll(), and adding a
// direction (ASIO) reopens the device -- both seen by every client as
// audioDeviceStopped / audioDeviceAboutToStart, like any device restart.
// The actual values the driver settled on are what every client is told.
//
// Identity: (device type, device name).  For ASIO the same name is the
// interface's input and its output, so a device opened for one direction is
// upgraded in place when the other is requested.  For the other APIs input
// and output devices have different names and stay separate, which is fine:
// those drivers are multi-client and have no per-interface rate to disagree
// about.
//
// Fan-out: the hub registers ONE callback on each manager and calls the
// clients itself.  Input-only clients are called with no output channels;
// output clients each write into a scratch buffer that is summed into the
// device buffers, so several outputs (an LTC per engine on different channels
// of one interface, the generator's music on others) coexist by construction
// and a client that writes only its own channel cannot leak into another.
//
// The client list is protected by a CriticalSection, the way
// juce::AudioDeviceManager protects its own callback list -- which means it
// is NOT held briefly on the audio side: the audio callback holds it for the
// whole fan-out, every client's callback in turn.  The message thread takes
// it when a client is added, removed or looked up (acquire, release,
// reconfigureAll) and while it announces a start or stop to the clients.
// The trade-off, kept until the list is made lock-free (AUDIT LTC-15,
// deferred): a message-thread call can wait for one fan-out; an audio
// callback that arrives while the message thread holds the lock waits for
// it, against CONTRIBUTING's "no locks in the audio callback" -- a priority
// inversion, short because the message thread does little under the lock,
// but not bounded; and the clients of one device share its period, so one
// slow client (the generator reading from disk, AUDIT LTC-3) delays every
// other client on that device.
//
// A lost device (AUD11 TC-IO-2).  An interface can stop delivering audio
// under its clients without any of them being told: JUCE closes it when it
// leaves the system's list (USB unplugged), a reopen can fail, and a stream
// that the system has ended (WASAPI: another program taking the output in
// exclusive mode) may simply stop calling back -- JUCE's WASAPI device
// reports no error and may never call audioDeviceStopped.  The clients stay
// registered and keep their running flag; what changes is that the hub can
// say so: isDeviceLost() is true for a client whose entry has no device, or
// whose device has made no callback for a second, or delivered under a
// quarter or over four times the samples of its rate in the last second
// (a dead stream that JUCE's device thread keeps polling).  The owner shows
// it in the status line.  An entry with no device, or one whose device has
// gone silent (no callback for a second, or under a quarter of its
// samples), is opened again with its stored names and format by the next
// acquire() on it -- any client switched off and on, or Refresh Devices,
// which restarts them all.  A device that still calls back, however fast,
// is only reported: a client joining it is no reason to restart it under
// the others.  An entry with no device is also reopened by the hub itself
// when the system's device list changes and lists it again, at most three
// times until an open succeeds; a device that is still there is never
// reopened without the operator.
//
// Message thread only for acquire/release; the callbacks run on the audio
// thread.
//==============================================================================
class AudioDeviceHub : private juce::AudioIODeviceType::Listener
{
public:
    static AudioDeviceHub& get()
    {
        static AudioDeviceHub hub;
        return hub;
    }

    /// Open (or share) `deviceName` of `typeName` for `client` in one
    /// direction.  Returns the live device on success (valid until the next
    /// reconfiguration: read what you need in audioDeviceAboutToStart, which
    /// the client is given before this returns), or nullptr with `error`
    /// set.  A client already registered is moved.
    /// `followsGlobalFormat`: whether this client's device should take part
    /// in reconfigureAll() (the global SAMPLE RATE / BUFFER SIZE setting); a
    /// device follows it if any of its clients does.  Every client takes the
    /// default (true) since the generator lost its own format combos
    /// (DESIGN D23, 2026-09-13); no client passes false today.
    juce::AudioIODevice* acquire(juce::AudioIODeviceCallback* client,
                                 const juce::String& typeName, const juce::String& deviceName,
                                 bool asInput, double sampleRate, int bufferSize,
                                 juce::String& error, bool followsGlobalFormat = true)
    {
        JUCE_ASSERT_MESSAGE_THREAD
        error.clear();
        if (client == nullptr || deviceName.isEmpty()) { error = "no device"; return nullptr; }

        release(client);

        Device* dev = findDevice(typeName, deviceName, asInput);
        if (dev == nullptr)
            dev = findDevice(typeName, deviceName, !asInput);   // same name in the other direction: upgrade
        if (dev == nullptr)
        {
            devices.push_back(std::make_unique<Device>());
            dev = devices.back().get();
            dev->typeName = typeName;
        }

        const bool needsInput  = asInput  || dev->inputName.isNotEmpty();
        const bool needsOutput = !asInput || dev->outputName.isNotEmpty();
        const juce::String wantIn  = needsInput  ? deviceName : juce::String();
        const juce::String wantOut = needsOutput ? deviceName : juce::String();

        // Rate and buffer belong to the device.  The client that opens it
        // sets them; a client joining an open device adapts to what is
        // running (its audioDeviceAboutToStart tells it), even if it asked
        // for something else -- otherwise two components with different
        // preferences on one interface would restart it at each other on
        // every start.  A global change goes through reconfigureAll().
        // Adding a direction (ASIO input to an output-only device) does
        // reconfigure, as it must.
        const bool alone = dev->fanout.empty();
        const double sr = alone ? sampleRate : dev->requestedSampleRate;
        const int    bs = alone ? bufferSize : dev->requestedBufferSize;
        // An entry whose manager has no device any more counts as not open
        // (AUD11 TC-IO-1).  JUCE closes the device itself when the interface
        // leaves the system's list (AudioDeviceManager::audioDeviceListChanged)
        // and the entry is left with open == true and nothing behind it.  A
        // client joining it used to skip the reopen, was put on the fan-out
        // with no device and got nullptr back: its start() failed, nothing
        // ever released it, and while such a ghost stayed the entry could
        // neither empty nor reopen -- every off/on failed until the last
        // real client left, and a later reconfigureAll() started the ghosts,
        // or called one whose engine had been removed.
        // The same goes for a device that is there but has gone silent
        // (AUD11 TC-IO-2, isSilent): joining it would start a client on a
        // dead stream, and with two clients nothing would ever reopen it.
        const bool wantsReconfigure = ! dev->open
                                   || isSilent(*dev)
                                   || dev->inputName != wantIn || dev->outputName != wantOut
                                   || (alone && (dev->requestedSampleRate != sr || dev->requestedBufferSize != bs));

        if (wantsReconfigure)
        {
            // Remember what the other clients had, so a request the driver
            // refuses (a rate it cannot do, a direction it cannot add) costs
            // only the requester: the device goes back to its previous
            // configuration for everyone else.
            const bool         wasOpen = dev->open;
            const juce::String prevIn = dev->inputName, prevOut = dev->outputName;
            const double       prevSr = dev->requestedSampleRate;
            const int          prevBs = dev->requestedBufferSize;

            if (! open(*dev, wantIn, wantOut, sr, bs, error))
            {
                // Nothing to go back to when the request was the previous
                // configuration itself -- a client joining a device that is
                // lost and not back yet: the same open would fail twice.
                const bool sameAsBefore = wantIn == prevIn && wantOut == prevOut && sr == prevSr && bs == prevBs;
                if (wasOpen && ! dev->fanout.empty() && ! sameAsBefore)
                {
                    juce::String restoreError;
                    open(*dev, prevIn, prevOut, prevSr, prevBs, restoreError);
                }
                if (dev->fanout.empty())
                    closeAndForget(dev);
                return nullptr;
            }
        }

        // Register on the running device: announced (audioDeviceAboutToStart)
        // before it can receive a callback, as JUCE's addAudioCallback does.
        // Registering after the open also means a re-opened device announces
        // itself to the existing clients only, and this one never sees a
        // stop it was not started for.
        // Only ever on a device that is there: a client that is given
        // nullptr is not registered, so a failed start() leaves nothing
        // behind for a later reopen to call (AUD11 TC-IO-1).
        auto* running = dev->manager->getCurrentAudioDevice();
        if (running == nullptr)
        {
            error = "device did not open";
            if (dev->fanout.empty())
                closeAndForget(dev);
            return nullptr;
        }
        dev->fanout.add(client, asInput, !asInput, followsGlobalFormat, running);
        return running;
    }

    /// Apply a new preferred rate and buffer to every open device, once
    /// each (every client sees a stop/start).  This is how the global
    /// SAMPLE RATE / BUFFER SIZE setting reaches shared devices; a device
    /// that cannot do the request keeps whatever the driver settled on,
    /// which the status lines show.
    void reconfigureAll(double sampleRate, int bufferSize)
    {
        JUCE_ASSERT_MESSAGE_THREAD
        for (auto& d : devices)
        {
            if (! d->open || ! d->fanout.anyFollowsGlobalFormat()) continue;
            if (d->requestedSampleRate == sampleRate && d->requestedBufferSize == bufferSize) continue;
            juce::String err;
            if (! open(*d, d->inputName, d->outputName, sampleRate, bufferSize, err))
            {
                juce::String restoreError;
                open(*d, d->inputName, d->outputName, d->requestedSampleRate, d->requestedBufferSize, restoreError);
            }
        }
    }

    /// Unregister `client`; the device closes when nobody is left.
    void release(juce::AudioIODeviceCallback* client)
    {
        JUCE_ASSERT_MESSAGE_THREAD
        for (auto& d : devices)
            if (d->fanout.contains(client))
            {
                d->fanout.remove(client, d->open ? d->manager->getCurrentAudioDevice() : nullptr);
                if (d->fanout.empty())
                    closeAndForget(d.get());
                return;
            }
    }

    /// Actual values the driver settled on for the device `client` is on
    /// (0 when the client is not registered or the device is not open).
    double getActualSampleRate(juce::AudioIODeviceCallback* client) const
    {
        for (auto& d : devices)
            if (d->fanout.contains(client) && d->open)
                if (auto* dev = d->manager->getCurrentAudioDevice()) return dev->getCurrentSampleRate();
        return 0.0;
    }

    int getActualBufferSize(juce::AudioIODeviceCallback* client) const
    {
        for (auto& d : devices)
            if (d->fanout.contains(client) && d->open)
                if (auto* dev = d->manager->getCurrentAudioDevice()) return dev->getCurrentBufferSizeSamples();
        return 0;
    }

    /// Whether the device `client` is registered on delivers no audio
    /// (AUD11 TC-IO-2, see "A lost device" above): closed under it, not
    /// reopened, silent for a second, or far off its sample rate.  False
    /// for a client that is not registered.  Cheap enough to ask every
    /// tick: no lock, no device call.  Message thread.
    bool isDeviceLost(const juce::AudioIODeviceCallback* client) const
    {
        JUCE_ASSERT_MESSAGE_THREAD
        for (auto& d : devices)
            if (d->fanout.containsNoLock(client))
                return isLost(*d);
        return false;
    }

    /// Close everything and drop the managers while JUCE is still alive.
    /// Call at the end of the main component's destructor, after every
    /// client has stopped; the static instance then destroys nothing at exit.
    static void shutdown()
    {
        auto& hub = get();
        for (auto& d : hub.devices)
            if (d->open) { d->manager->removeAudioCallback(&d->fanout); d->manager->closeAudioDevice(); d->open = false; }
        hub.devices.clear();
    }

private:
    AudioDeviceHub() = default;
    ~AudioDeviceHub() = default;

    //==========================================================================
    // Fan-out: the one callback registered on a manager
    //==========================================================================
    struct Fanout : public juce::AudioIODeviceCallback
    {
        struct Entry { juce::AudioIODeviceCallback* cb; bool input; bool output; bool globalFormat; };

        void add(juce::AudioIODeviceCallback* cb, bool input, bool output, bool globalFormat,
                 juce::AudioIODevice* runningDevice)
        {
            if (runningDevice != nullptr)
                cb->audioDeviceAboutToStart(runningDevice);
            const juce::ScopedLock sl(lock);
            entries.push_back({ cb, input, output, globalFormat });
        }

        bool anyFollowsGlobalFormat() const
        {
            const juce::ScopedLock sl(lock);
            for (auto& e : entries) if (e.globalFormat) return true;
            return false;
        }

        void remove(juce::AudioIODeviceCallback* cb, juce::AudioIODevice* runningDevice)
        {
            bool found = false;
            {
                const juce::ScopedLock sl(lock);
                for (size_t i = 0; i < entries.size(); ++i)
                    if (entries[i].cb == cb) { entries.erase(entries.begin() + (long) i); found = true; break; }
            }
            if (found && runningDevice != nullptr)
                cb->audioDeviceStopped();
        }

        bool contains(juce::AudioIODeviceCallback* cb) const
        {
            const juce::ScopedLock sl(lock);
            for (auto& e : entries) if (e.cb == cb) return true;
            return false;
        }

        // For the message thread's status poll (AUD11 TC-IO-2), which asks
        // every tick: without the lock.  The list is written on the message
        // thread only (add, remove), so reading it there needs none, and a
        // poll that took it would make the audio callback wait for the
        // message thread sixty times a second.
        bool containsNoLock(const juce::AudioIODeviceCallback* cb) const
        {
            for (auto& e : entries) if (e.cb == cb) return true;
            return false;
        }

        bool empty() const { const juce::ScopedLock sl(lock); return entries.empty(); }

        void audioDeviceAboutToStart(juce::AudioIODevice* device) override
        {
            // Sized once, off the audio thread, and generously: four times
            // the announced buffer and at least 16384 samples.  A driver
            // block longer than the announced buffer does not get here in
            // one piece: JUCE 9's AudioDeviceManager cuts it into blocks of
            // at most getCurrentBufferSizeSamples() before calling us
            // (CallbackMaxSizeEnforcer, juce_AudioDeviceManager.cpp).  The
            // margin, and the chunks below for a call bigger than this, are
            // a second guard that costs nothing.
            scratch.setSize(juce::jmax(1, device->getActiveOutputChannels().countNumberOfSetBits()),
                            juce::jmax(16384, 4 * device->getCurrentBufferSizeSamples()), false, false, true);
            // The health figures start with the stream (AUD11 TC-IO-2).  A
            // device that is slow to make its first callback is given
            // kStartGraceMs more before it counts as silent, and its rate is
            // measured from that first callback on.
            healthSampleRate    = device->getCurrentSampleRate();
            healthWindowSamples = 0;
            healthWindowStartMs = 0.0;
            lastCallbackMs.store(juce::Time::getMillisecondCounterHiRes() + kStartGraceMs, std::memory_order_relaxed);
            starved.store(false, std::memory_order_relaxed);
            racing.store(false, std::memory_order_relaxed);
            const juce::ScopedLock sl(lock);
            for (auto& e : entries) e.cb->audioDeviceAboutToStart(device);
        }

        void audioDeviceStopped() override
        {
            const juce::ScopedLock sl(lock);
            for (auto& e : entries) e.cb->audioDeviceStopped();
        }

        void audioDeviceError(const juce::String& message) override
        {
            const juce::ScopedLock sl(lock);
            for (auto& e : entries) e.cb->audioDeviceError(message);
        }

        void audioDeviceIOCallbackWithContext(const float* const* inputChannelData, int numInputChannels,
                                              float* const* outputChannelData, int numOutputChannels,
                                              int numSamples,
                                              const juce::AudioIODeviceCallbackContext& context) override
        {
            for (int ch = 0; ch < numOutputChannels; ++ch)
                if (outputChannelData[ch] != nullptr)
                    juce::FloatVectorOperations::clear(outputChannelData[ch], numSamples);

            // Health, for isLost() on the message thread (AUD11 TC-IO-2):
            // when the last callback came, and once a second whether the
            // samples delivered since the last check are anywhere near what
            // the sample rate makes in that time.  One clock reading and
            // atomic stores, nothing else.
            {
                const double nowMs = juce::Time::getMillisecondCounterHiRes();
                lastCallbackMs.store(nowMs, std::memory_order_relaxed);
                if (healthWindowStartMs <= 0.0)
                {
                    healthWindowStartMs = nowMs;   // the first callback opens the first window
                }
                else
                {
                    healthWindowSamples += numSamples;
                    const double windowMs = nowMs - healthWindowStartMs;
                    if (windowMs >= kHealthWindowMs)
                    {
                        const double expected = healthSampleRate * windowMs / 1000.0;
                        const double got      = (double) healthWindowSamples;
                        starved.store(expected > 0.0 && got < expected * 0.25, std::memory_order_relaxed);
                        racing.store (expected > 0.0 && got > expected * 4.0,  std::memory_order_relaxed);
                        healthWindowStartMs = nowMs;
                        healthWindowSamples = 0;
                    }
                }
            }

            const juce::ScopedLock sl(lock);

            // Output clients render into the scratch buffer, sized in
            // audioDeviceAboutToStart (no allocation here).  A call longer
            // than the scratch -- which AudioDeviceManager, cutting driver
            // blocks to the announced buffer, does not make -- is served in
            // scratch-sized chunks, in order, within this callback, so every
            // output client is asked for every sample the device plays.
            // (Clamping to the scratch length left the rest of the buffer
            // silent.)  Whether it renders a call longer than the buffer the
            // device announced is up to the client: LTC out and Audio Thru
            // do, but the generator's player (GeneratorAudioPlayer) sizes its
            // own scratch to the announced buffer and plays silence for the
            // whole of any longer call.  The chunks reach the client back to
            // back, with the device's context for the whole buffer, so a
            // client that dates its calls by the clock dates every chunk at
            // the callback's start.  LTC out does (measured with the real
            // LtcOutput behind this fan-out): clean for any callback up to
            // the scratch length, but past it the later chunks are dated a
            // whole scratch early, its value tracking snaps, and frames are
            // skipped or repeated every period.  Hence the generous scratch.
            const int scratchChans = juce::jmin(numOutputChannels, scratch.getNumChannels());
            const int chunkMax     = scratch.getNumSamples();

            for (auto& e : entries)
            {
                const float* const* ins = e.input ? inputChannelData : nullptr;
                const int numIns = e.input ? numInputChannels : 0;

                if (! e.output)
                {
                    e.cb->audioDeviceIOCallbackWithContext(ins, numIns, nullptr, 0, numSamples, context);
                    continue;
                }

                // An entry is input or output, never both (acquire registers
                // one direction), so an output client is given no input.
                for (int offset = 0; offset < numSamples && chunkMax > 0; offset += chunkMax)
                {
                    const int len = juce::jmin(chunkMax, numSamples - offset);
                    scratch.clear(0, len);   // only the samples in use: the buffer is deliberately oversized
                    // After clear(), every time: a full-length clear() marks the
                    // buffer clear and only this unmarks it (else the next
                    // clear() is skipped and old samples are summed again).
                    float* const* outs = scratch.getArrayOfWritePointers();
                    e.cb->audioDeviceIOCallbackWithContext(nullptr, 0, outs, scratchChans, len, context);
                    for (int ch = 0; ch < scratchChans; ++ch)
                        if (outputChannelData[ch] != nullptr)
                            juce::FloatVectorOperations::add(outputChannelData[ch] + offset, outs[ch], len);
                }
            }
        }

        juce::CriticalSection lock;
        std::vector<Entry> entries;
        juce::AudioBuffer<float> scratch;

        // Health (AUD11 TC-IO-2).  The atomics are read on the message
        // thread; the rest belongs to the audio thread, set before the
        // stream starts like the scratch buffer above.
        static constexpr double kHealthWindowMs = 1000.0;
        static constexpr double kStartGraceMs   = 2000.0;
        std::atomic<double> lastCallbackMs { 0.0 };
        std::atomic<bool>   starved { false };   // the last whole second: under a quarter of its samples
        std::atomic<bool>   racing  { false };   // ... over four times as many
        double       healthSampleRate    = 0.0;
        double       healthWindowStartMs = 0.0;
        juce::int64  healthWindowSamples = 0;
    };

    struct Device
    {
        juce::String typeName, inputName, outputName;
        double requestedSampleRate = 0.0;
        int    requestedBufferSize = 0;
        bool   open = false;
        int    autoReopenTries = 0;   // by audioDeviceListChanged() since the last open that worked
        std::unique_ptr<juce::AudioDeviceManager> manager;
        Fanout fanout;
    };

    std::vector<std::unique_ptr<Device>> devices;
    bool opening = false;             // inside open(): a list change announced from in there is ignored

    // AUD11 TC-IO-2.  Silent: no device behind the entry, or a device that
    // delivers (almost) nothing -- no callback for kSilentMs (the longest
    // period the buffer sizes on offer make is 186 ms), or under a quarter
    // of its samples in the last whole second.  Lost, for the status line,
    // is that or a device calling back at over four times its rate.
    static constexpr double kSilentMs = 1000.0;
    static constexpr int    kMaxAutoReopens = 3;

    bool hasNoDevice(const Device& d) const
    {
        return ! d.open || d.manager == nullptr || d.manager->getCurrentAudioDevice() == nullptr;
    }

    bool isSilent(const Device& d) const
    {
        if (hasNoDevice(d)) return true;
        if (d.fanout.starved.load(std::memory_order_relaxed)) return true;
        return juce::Time::getMillisecondCounterHiRes()
                 - d.fanout.lastCallbackMs.load(std::memory_order_relaxed) > kSilentMs;
    }

    bool isLost(const Device& d) const
    {
        return isSilent(d) || d.fanout.racing.load(std::memory_order_relaxed);
    }

    // The system's device list changed (juce::AudioIODeviceType::Listener;
    // JUCE's own AudioDeviceManager, registered before the hub, has by now
    // closed a device that left the list).  An entry that has clients and no
    // device is opened again with its stored names and format once the list
    // carries its names again: the interface was plugged back in, and
    // every client on it is told, as after any restart.  The open runs here,
    // on the message thread, like the one an off/on would make.  Bounded: a
    // device that is listed but will not open is tried kMaxAutoReopens times
    // and then left to the operator -- some backends announce a "change"
    // for much less than a new device (CoreAudio: any property), and a
    // failing open repeated on each would stall the message thread again
    // and again.  A device that is there is not touched, whatever it does.
    void audioDeviceListChanged() override
    {
        if (opening || ! juce::MessageManager::existsAndIsCurrentThread()) return;
        for (auto& d : devices)   // open() neither adds nor removes an entry
        {
            if (d->fanout.empty() || d->manager == nullptr || ! hasNoDevice(*d)) continue;
            if (d->autoReopenTries >= kMaxAutoReopens) continue;
            // The entry's own type, by name: after losing its device the
            // manager may have made another type the current one.
            juce::AudioIODeviceType* type = nullptr;
            for (auto* t : d->manager->getAvailableDeviceTypes())
                if (t->getTypeName() == d->typeName) { type = t; break; }
            if (type == nullptr) continue;
            if ((d->inputName.isNotEmpty()  && ! type->getDeviceNames(true).contains(d->inputName))
             || (d->outputName.isNotEmpty() && ! type->getDeviceNames(false).contains(d->outputName)))
                continue;
            ++d->autoReopenTries;
            juce::String err;
            open(*d, d->inputName, d->outputName, d->requestedSampleRate, d->requestedBufferSize, err);
        }
    }

    Device* findDevice(const juce::String& typeName, const juce::String& deviceName, bool asInput) const
    {
        for (auto& d : devices)
            if (d->typeName == typeName && (asInput ? d->inputName : d->outputName) == deviceName)
                return d.get();
        return nullptr;
    }

    bool open(Device& dev, const juce::String& inputName, const juce::String& outputName,
              double sampleRate, int bufferSize, juce::String& error)
    {
        const juce::ScopedValueSetter<bool> busy(opening, true);
        if (dev.open)
        {
            dev.manager->removeAudioCallback(&dev.fanout);
            dev.manager->closeAudioDevice();
            dev.open = false;
        }
        if (dev.manager == nullptr)
            dev.manager = std::make_unique<juce::AudioDeviceManager>();

        // Register every device type, switch to the requested one and scan so
        // the name is recognised.  With zero channels "needed" JUCE fills in
        // no default device names, so nothing is opened here -- the
        // components used to ask for 128 channels at this point, which made
        // initialise() and the type switch each open the type's default
        // device for a moment before the real one.  The channels are enabled
        // explicitly below instead.
        dev.manager->initialise(0, 0, nullptr, false);
        if (dev.typeName.isNotEmpty())
            dev.manager->setCurrentAudioDeviceType(dev.typeName, false);
        if (auto* type = dev.manager->getCurrentDeviceTypeObject())
        {
            type->addListener(this);   // audioDeviceListChanged(), AUD11 TC-IO-2; adding again is a no-op
            type->scanForDevices();
        }

        auto setup = dev.manager->getAudioDeviceSetup();
        setup.inputDeviceName  = inputName;
        setup.outputDeviceName = outputName;
        setup.useDefaultInputChannels  = false;
        setup.useDefaultOutputChannels = false;
        setup.inputChannels.clear();
        setup.outputChannels.clear();
        if (inputName.isNotEmpty())  setup.inputChannels.setRange(0, 128, true);   // masked to what the device has
        if (outputName.isNotEmpty()) setup.outputChannels.setRange(0, 128, true);
        // 0 is "device default" and has to be written as such (AUD11
        // TC-IO-7): the setup read above still carries the rate and buffer
        // of the device this manager had open before, and leaving them in
        // made SAMPLE RATE / BUFFER SIZE put back to Default keep the
        // previous explicit values on a running device -- while the same
        // setting gave the device's own after an off/on or a restart.  With
        // 0 JUCE chooses as it does for a manager's first open: the
        // device's current rate and its default buffer size.
        setup.sampleRate = sampleRate > 0 ? sampleRate : 0.0;
        setup.bufferSize = bufferSize > 0 ? bufferSize : 0;

        // Not as "the chosen device" (second argument; AUD11 TC-IO-2).  A
        // setup stored as the user's choice is what JUCE's manager goes
        // back to when its device leaves the system's list: it fails to
        // open the lost names and then opens the driver type's default
        // device instead, with this fan-out still registered.  Every client
        // on a lost interface was moved to another one that way -- LTC out
        // of the computer's own output, LTC IN listening to its microphone
        // -- under a status line that still named the lost device, and it
        // stayed there when the interface came back.  With nothing stored
        // the manager only retries the same names, fails, and is left with
        // no device, which is the lost state described at the top.
        const auto err = dev.manager->setAudioDeviceSetup(setup, false);
        if (err.isNotEmpty() || dev.manager->getCurrentAudioDevice() == nullptr)
        {
            error = err.isNotEmpty() ? err : juce::String("device did not open");
            dev.manager->closeAudioDevice();
            return false;
        }

        dev.inputName  = inputName;
        dev.outputName = outputName;
        dev.requestedSampleRate = sampleRate;
        dev.requestedBufferSize = bufferSize;
        dev.open = true;
        dev.autoReopenTries = 0;
        dev.manager->addAudioCallback(&dev.fanout);   // announces the device to every registered client
        return true;
    }

    void closeAndForget(Device* dev)
    {
        if (dev->open)
        {
            dev->manager->removeAudioCallback(&dev->fanout);
            dev->manager->closeAudioDevice();
            dev->open = false;
        }
        for (size_t i = 0; i < devices.size(); ++i)
            if (devices[i].get() == dev) { devices.erase(devices.begin() + (long) i); return; }
    }

    JUCE_DECLARE_NON_COPYABLE(AudioDeviceHub)
};
