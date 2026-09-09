// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

#pragma once
#include <JuceHeader.h>
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
// client leaves.  Sample rate and buffer size are properties of the device:
// the first request opens it with them, a later request with different
// values reconfigures it (which every client sees as audioDeviceStopped /
// audioDeviceAboutToStart, like any device restart), and the actual values
// the driver settled on are what every client is told.
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
// The client list is protected by a CriticalSection held briefly on both
// sides, as juce::AudioDeviceManager does for its own list.
//
// Message thread only for acquire/release; the callbacks run on the audio
// thread.
//==============================================================================
class AudioDeviceHub
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
    /// is called before this returns when the device is already running), or
    /// nullptr with `error` set.  A client already registered is moved.
    /// `followsGlobalFormat`: whether this client's device should take part
    /// in reconfigureAll() (the global SAMPLE RATE / BUFFER SIZE setting).
    /// The generator player has its own per-engine format combos and passes
    /// false; a device it shares with a client that follows the global
    /// setting still follows it.
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
        const bool wantsReconfigure = ! dev->open
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
                if (wasOpen && ! dev->fanout.empty())
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
        dev->fanout.add(client, asInput, !asInput, followsGlobalFormat, dev->manager->getCurrentAudioDevice());
        return dev->manager->getCurrentAudioDevice();
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

        bool empty() const { const juce::ScopedLock sl(lock); return entries.empty(); }

        void audioDeviceAboutToStart(juce::AudioIODevice* device) override
        {
            // Sized once, off the audio thread.  Generous in samples: a driver
            // that hands over more than its announced buffer (some WASAPI
            // configurations do) must not be clamped to a short callback.
            scratch.setSize(juce::jmax(1, device->getActiveOutputChannels().countNumberOfSetBits()),
                            juce::jmax(4096, device->getCurrentBufferSizeSamples()), false, false, true);
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

            const juce::ScopedLock sl(lock);

            // Scratch is sized in audioDeviceAboutToStart; a device that hands
            // us more than it announced gets the size it announced (no
            // allocation on the audio thread).
            const int scratchChans = juce::jmin(numOutputChannels, scratch.getNumChannels());
            const int scratchLen   = juce::jmin(numSamples, scratch.getNumSamples());

            for (auto& e : entries)
            {
                const float* const* ins = e.input ? inputChannelData : nullptr;
                const int numIns = e.input ? numInputChannels : 0;

                if (! e.output)
                {
                    e.cb->audioDeviceIOCallbackWithContext(ins, numIns, nullptr, 0, numSamples, context);
                    continue;
                }

                scratch.clear();
                float* const* outs = scratch.getArrayOfWritePointers();
                e.cb->audioDeviceIOCallbackWithContext(ins, numIns, outs, scratchChans, scratchLen, context);
                for (int ch = 0; ch < scratchChans; ++ch)
                    if (outputChannelData[ch] != nullptr)
                        juce::FloatVectorOperations::add(outputChannelData[ch], outs[ch], scratchLen);
            }
        }

        juce::CriticalSection lock;
        std::vector<Entry> entries;
        juce::AudioBuffer<float> scratch;
    };

    struct Device
    {
        juce::String typeName, inputName, outputName;
        double requestedSampleRate = 0.0;
        int    requestedBufferSize = 0;
        bool   open = false;
        std::unique_ptr<juce::AudioDeviceManager> manager;
        Fanout fanout;
    };

    std::vector<std::unique_ptr<Device>> devices;

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
            type->scanForDevices();

        auto setup = dev.manager->getAudioDeviceSetup();
        setup.inputDeviceName  = inputName;
        setup.outputDeviceName = outputName;
        setup.useDefaultInputChannels  = false;
        setup.useDefaultOutputChannels = false;
        setup.inputChannels.clear();
        setup.outputChannels.clear();
        if (inputName.isNotEmpty())  setup.inputChannels.setRange(0, 128, true);   // masked to what the device has
        if (outputName.isNotEmpty()) setup.outputChannels.setRange(0, 128, true);
        if (sampleRate > 0) setup.sampleRate = sampleRate;
        if (bufferSize > 0) setup.bufferSize = bufferSize;

        const auto err = dev.manager->setAudioDeviceSetup(setup, true);
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
