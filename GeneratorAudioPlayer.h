// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <cstring>

//==============================================================================
// GeneratorAudioPlayer -- Plays a single audio file, transport-controlled,
// to a dedicated audio output device.  Used by the internal Generator to
// optionally play a song file synchronised with the generated timecode.
//
// Owns its own AudioDeviceManager so the programme audio does not have to
// share a device with the LTC bitstream output.  Channel routing matches the
// LtcOutput convention: -1 = stereo on channels 0+1, >=0 = mono mix on the
// named channel.
//
// Pipeline:
//   AudioFormatReader -> AudioFormatReaderSource -> AudioTransportSource
//     -> our audio callback -> selected device output(s)
//
// Threading:
//  - openDevice / closeDevice / loadFile / unloadFile / play / pause /
//    stopAndReset / seekSeconds are intended for the message thread (UI,
//    OSC handler dispatched via MessageManager::callAsync, engine tick).
//  - setSource() on the transport is serialised through transportLock so
//    loadFile and the audio callback never race over the source pointer.
//  - JUCE's AudioTransportSource handles its own internal locking for
//    play / stop / setPosition, so those calls are safe from any thread.
//==============================================================================
class GeneratorAudioPlayer : private juce::AudioIODeviceCallback
{
public:
    GeneratorAudioPlayer()
    {
        // Registers WAV, AIFF, FLAC, OGG, and -- when the Projucer flag
        // JUCE_USE_MP3AUDIOFORMAT is set -- MP3.  Do NOT re-register MP3
        // explicitly afterwards; JUCE_DEBUG asserts on duplicate format
        // registration (jassertfalse in AudioFormatManager::registerFormat).
        formatManager.registerBasicFormats();
    }

    ~GeneratorAudioPlayer() override
    {
        // Order matters here.  Members are destroyed in reverse declaration
        // order, which means the std::atomic<> flags (deviceOpen, userPaused,
        // shouldPlay, fileLoadedAtomic, ...) are torn down BEFORE the
        // deviceManager.  If the audio device had any callback still in
        // flight when ~AudioDeviceManager() runs, that callback would read
        // already-destroyed atomics -- the std::atomic load crash on shutdown
        // we hunted in v1.9.7/v1.9.8.
        //
        // Step 1: latch shuttingDown so any in-flight or imminent audio
        // callback returns immediately without touching other atomics.
        // The acquire/release pairing on shuttingDown ensures the callback
        // sees the flag if it observes the store.
        shuttingDown.store(true, std::memory_order_release);

        // Step 2: closeDevice() calls removeAudioCallback, which JUCE
        // documents as blocking until any in-flight callback returns.  We
        // call it explicitly here (rather than relying on the deviceManager
        // destructor) so the audio thread is provably idle BEFORE we leave
        // the user-defined destructor body and member destruction begins.
        // The loader thread is stopped first because its tick can call back
        // into transport / reader objects that closeDevice will tear down.
        loaderThread.stop();
        closeDevice();
        unloadFile();
        // Belt-and-braces: ensure the AudioDeviceManager is fully closed
        // and has no callbacks attached.  closeDevice already did this for
        // the case where a device was open, but this also covers the path
        // where ~GeneratorAudioPlayer runs without ever having opened one.
        deviceManager.removeAudioCallback(this);
        deviceManager.closeAudioDevice();
    }

    //==========================================================================
    // Device management
    //--------------------------------------------------------------------------
    // typeName: audio device type (e.g. "Windows Audio", "ASIO"). Empty
    //           leaves the manager's current type.
    // devName:  raw device name as reported by the type's getDeviceNames().
    // channel:  -1 = stereo (file L -> ch0, R -> ch1)
    //          >=0 = mono mix to that channel index.
    //==========================================================================
    bool openDevice(const juce::String& typeName,
                    const juce::String& devName,
                    int channel = -1,
                    double sampleRate = 0,
                    int bufferSize = 0)
    {
        closeDevice();

        currentDeviceName = devName;
        currentTypeName   = typeName;
        selectedChannel.store(channel, std::memory_order_relaxed);

        deviceManager.closeAudioDevice();
        deviceManager.initialise(0, 128, nullptr, false);

        if (typeName.isNotEmpty())
            deviceManager.setCurrentAudioDeviceType(typeName, false);

        if (auto* type = deviceManager.getCurrentDeviceTypeObject())
            type->scanForDevices();

        auto setup = deviceManager.getAudioDeviceSetup();
        setup.outputDeviceName  = devName;
        setup.inputDeviceName   = "";
        setup.useDefaultInputChannels  = false;
        setup.useDefaultOutputChannels = true;
        if (sampleRate > 0)  setup.sampleRate = sampleRate;
        if (bufferSize > 0)  setup.bufferSize = bufferSize;

        auto err = deviceManager.setAudioDeviceSetup(setup, true);
        if (err.isNotEmpty()) return false;

        auto* device = deviceManager.getCurrentAudioDevice();
        if (!device)
        {
            // Setup reported success but the manager has no active device
            // (rare but observed on some drivers). Close to avoid leaving an
            // orphan device open with no callback registered.
            deviceManager.closeAudioDevice();
            return false;
        }

        currentSampleRate    = device->getCurrentSampleRate();
        currentBufferSize    = device->getCurrentBufferSizeSamples();
        numChannelsAvailable = device->getActiveOutputChannels().countNumberOfSetBits();

        if (! backgroundThread.isThreadRunning())
            backgroundThread.startThread();

        // If a file was previously loaded, attach it now that the device SR is known.
        attachReaderToTransport();

        deviceManager.addAudioCallback(this);
        deviceOpen.store(true, std::memory_order_relaxed);
        return true;
    }

    void closeDevice()
    {
        if (deviceOpen.load(std::memory_order_relaxed))
        {
            transport.stop();
            {
                const juce::ScopedLock sl(transportLock);
                transport.setSource(nullptr);
            }
            deviceManager.removeAudioCallback(this);
            deviceManager.closeAudioDevice();
            deviceOpen.store(false, std::memory_order_relaxed);
        }
        if (backgroundThread.isThreadRunning())
            backgroundThread.stopThread(2000);
    }

    bool         isDeviceOpen()        const { return deviceOpen.load(std::memory_order_relaxed); }
    juce::String getCurrentDeviceName() const { return currentDeviceName; }
    juce::String getCurrentTypeName()   const { return currentTypeName; }
    int          getSelectedChannel()  const { return selectedChannel.load(std::memory_order_relaxed); }
    int          getChannelCount()     const { return numChannelsAvailable; }
    double       getActualSampleRate() const { return currentSampleRate; }
    int          getActualBufferSize() const { return currentBufferSize; }

    //==========================================================================
    // File management
    //==========================================================================
    /// Load an audio file synchronously.  Replaces any previously loaded
    /// file.  Returns true on success.  On failure, getLoadError() returns
    /// a human-readable description of why.
    bool loadFile(const juce::File& file)
    {
        loadError.clear();

        if (file == juce::File() || ! file.existsAsFile())
        {
            if (file != juce::File()) loadError = "FILE NOT FOUND";
            unloadFile();
            return false;
        }

        // Skip reload if same file is already loaded.
        {
            const juce::ScopedLock sl(transportLock);
            if (currentFile == file && currentReaderSource != nullptr)
                return true;
        }

        std::unique_ptr<juce::AudioFormatReader> newReader(formatManager.createReaderFor(file));
        if (newReader == nullptr)
        {
            // Most likely cause: the file's format is not registered with
            // the FormatManager.  For .mp3 specifically, this means the
            // Projucer flag JUCE_USE_MP3AUDIOFORMAT was not set when the
            // juce_audio_formats module was compiled.
            loadError = "UNSUPPORTED FORMAT: " + file.getFileExtension().removeCharacters(".").toUpperCase();
            unloadFile();
            return false;
        }

        const double  readerSR     = newReader->sampleRate;
        const int64_t totalSamples = newReader->lengthInSamples;

        if (readerSR <= 0.0 || totalSamples <= 0)
        {
            loadError = "EMPTY OR CORRUPT FILE";
            unloadFile();
            return false;
        }

        // AudioFormatReaderSource takes ownership of the reader.
        std::unique_ptr<juce::AudioFormatReaderSource> newSource(
            new juce::AudioFormatReaderSource(newReader.release(), true));
        newSource->setLooping(loopFlag.load(std::memory_order_relaxed));

        // The next two calls can briefly block on the audio reader's
        // background thread (especially with MP3 mid-decode).  Crucially we
        // do them OUTSIDE the transportLock so the message thread's UI
        // accessors and any concurrent callbacks never wait on us.  JUCE's
        // AudioTransportSource has its own internal locking against
        // getNextAudioBlock(), so these are safe to call without our lock.
        transport.stop();
        transport.setSource(nullptr);

        // Now swap our state under the lock.  This is fast; only blocks
        // anything for the time it takes to move-assign a unique_ptr and
        // copy a couple of doubles / a juce::File.  After this point the
        // old reader source is destroyed -- but the transport no longer
        // points to it (setSource(nullptr) above guarantees that).
        std::unique_ptr<juce::AudioFormatReaderSource> oldSource;
        {
            const juce::ScopedLock sl(transportLock);
            oldSource            = std::move(currentReaderSource);
            currentReaderSource  = std::move(newSource);
            currentFile          = file;
            sourceFileSampleRate = readerSR;
            fileLengthSeconds    = (readerSR > 0.0) ? (double) totalSamples / readerSR : 0.0;
        }
        // oldSource destroyed here, outside the lock.

        // Update lock-free mirrors AFTER the state is consistent.
        fileLengthAtomic.store(fileLengthSeconds, std::memory_order_release);
        fileLoadedAtomic.store(true, std::memory_order_release);

        // Note: the AudioThumbnail is updated separately by requestLoad() on
        // the caller's thread (typically the message thread) BEFORE this
        // function runs.  Touching the thumbnail here would force this
        // background thread to wait for the thumbnail's own internal decode
        // job to finish, which can take hundreds of ms with MP3 if the
        // previous file was still being processed.

        if (deviceOpen.load(std::memory_order_relaxed))
            attachReaderToTransport();

        return true;
    }

    /// Last load failure description, or empty if the last call succeeded
    /// (or no file has been loaded yet).
    juce::String getLoadError() const { return loadError; }

    void unloadFile()
    {
        // Same pattern as loadFile: do the blocking transport ops outside
        // the lock so UI accessors don't wait.
        transport.stop();
        transport.setSource(nullptr);

        std::unique_ptr<juce::AudioFormatReaderSource> oldSource;
        {
            const juce::ScopedLock sl(transportLock);
            oldSource            = std::move(currentReaderSource);
            currentFile          = juce::File();
            sourceFileSampleRate = 0.0;
            fileLengthSeconds    = 0.0;
        }
        // oldSource destroyed here, outside the lock.

        fileLengthAtomic.store(0.0, std::memory_order_release);
        fileLoadedAtomic.store(false, std::memory_order_release);
        // thumbnail is managed separately by requestLoad().
    }

    bool hasFileLoaded() const
    {
        // Lock-free; reads the atomic mirror updated in loadFile/unloadFile.
        return fileLoadedAtomic.load(std::memory_order_acquire);
    }

    juce::File getCurrentFile() const
    {
        const juce::ScopedLock sl(transportLock);
        return currentFile;
    }

    double getFileLengthSeconds() const
    {
        // Lock-free; reads the atomic mirror updated in loadFile/unloadFile.
        return fileLengthAtomic.load(std::memory_order_acquire);
    }

    //==========================================================================
    // Transport
    //==========================================================================
    // Logical state model (relevant when load is asynchronous):
    //   shouldPlay  -- caller intent ("the user wants this to be playing").
    //                  Set by play(), cleared by stopAndReset().  Pause does
    //                  NOT clear it because pause is a temporary state from
    //                  which a play() resumes.
    //   userPaused  -- temporary mute that the audio callback honours.
    //
    // play() during a pending async load: shouldPlay is set; when the loader
    // thread finishes attaching the new source, it consults shouldPlay and
    // calls transport.start() if appropriate.  This avoids the race where
    // transport.start() runs before any source exists.
    void play()
    {
        shouldPlay.store(true,  std::memory_order_release);
        userPaused.store(false, std::memory_order_release);
        if (! hasFileLoaded()) return;             // load still pending; loader will start
        // If a load is in flight, defer transport.start() to the
        // onLoadCompleted callback so the audio is started AT the engine's
        // current playhead instead of from position 0.  Without this guard
        // play() called from generatorPlay()/activateGenPreset during a
        // hot-swap would start the previously loaded file from 0, audible
        // for the load duration, and then the new file would also start
        // from 0 when attachReaderToTransport runs -- the persistent
        // cursor/audio desync the operator reported.
        if (pendingLoad.load(std::memory_order_acquire)) return;
        if (! deviceOpen.load(std::memory_order_relaxed)) return;
        transport.start();
    }

    /// Pauses playback while preserving the current position.
    /// Sets a flag that the audio callback checks before pulling samples,
    /// so the message thread returns INSTANTLY without waiting for the
    /// background reader thread to sync.  This matters with MP3 because
    /// AudioTransportSource::stop() can briefly block on the buffering
    /// reader, freezing the UI for tens of ms while the decoder finishes
    /// its current block.  The transport keeps "playing" internally (no
    /// audio is produced because the callback skips it), so resume is
    /// instantaneous when play() flips the flag back.
    void pause()
    {
        userPaused.store(true, std::memory_order_release);
    }

    /// Stops and rewinds to the start of the file.
    void stopAndReset()
    {
        shouldPlay.store(false, std::memory_order_release);
        userPaused.store(true,  std::memory_order_release);
        transport.setPosition(0.0);
    }

    /// Seek to position in seconds, relative to the start of the audio file.
    /// When looping, positions beyond file length are folded back via fmod.
    /// If the player was supposed to be playing (shouldPlay set, not paused)
    /// but the transport had auto-stopped (typically because it reached EOF
    /// before this seek), the transport is re-engaged so audio resumes from
    /// the new position.  start() is a no-op when the transport is already
    /// playing, so the in-flight seek-while-playing case is unaffected.
    void seekSeconds(double seconds)
    {
        if (! hasFileLoaded()) return;

        seconds = juce::jmax(0.0, seconds);

        const double len = fileLengthSeconds;
        if (loopFlag.load(std::memory_order_relaxed) && len > 0.0)
        {
            seconds = std::fmod(seconds, len);
        }
        else if (len > 0.0 && seconds > len)
        {
            seconds = len;
        }

        transport.setPosition(seconds);

        if (shouldPlay.load(std::memory_order_acquire)
            && ! userPaused.load(std::memory_order_acquire)
            && deviceOpen.load(std::memory_order_relaxed))
        {
            transport.start();
        }
    }

    double getCurrentPositionSeconds() const { return transport.getCurrentPosition(); }
    bool   isPlaying()                 const
    {
        return transport.isPlaying() && ! userPaused.load(std::memory_order_acquire);
    }

    //==========================================================================
    // Loop
    //==========================================================================
    void setLooping(bool shouldLoop)
    {
        loopFlag.store(shouldLoop, std::memory_order_relaxed);
        const juce::ScopedLock sl(transportLock);
        if (currentReaderSource)
            currentReaderSource->setLooping(shouldLoop);
    }

    bool isLooping() const { return loopFlag.load(std::memory_order_relaxed); }

    //==========================================================================
    // File channel mode: which channel(s) of the loaded audio file to use.
    // Useful for industry-standard files that carry programme audio on the
    // left channel and LTC on the right (or vice versa); selecting Left or
    // Right effectively treats the file as mono and silences the other side.
    //==========================================================================
    enum FileChannelMode { Stereo = 0, LeftOnly = 1, RightOnly = 2 };

    void setFileChannelMode(FileChannelMode m) { fileChannelMode.store((int) m, std::memory_order_release); }
    FileChannelMode getFileChannelMode() const
    {
        return (FileChannelMode) fileChannelMode.load(std::memory_order_acquire);
    }

    //==========================================================================
    // Output volume (linear gain, 0 = silence, 1 = unity).  Applied in the
    // audio transport so a volume change does not invalidate the read-ahead
    // buffer (unlike a seek would).  Thread-safe to call from any thread.
    //==========================================================================
    void  setOutputVolume(float linearGain) { transport.setGain(juce::jlimit(0.0f, 2.0f, linearGain)); }
    float getOutputVolume() const           { return transport.getGain(); }

    //==========================================================================
    // Waveform thumbnail (for UI display).  The thumbnail is generated in a
    // background thread by JUCE; UI components should listen as ChangeListener
    // for repaint notifications as more peak data becomes available.
    //==========================================================================
    // Asynchronous file load
    //==========================================================================
    // Schedule a load (or unload, when file == File()).  Returns immediately;
    // the actual loadFile() / unloadFile() runs on a dedicated thread so the
    // UI never waits on the audio reader's background thread to settle.
    // Multiple consecutive requests are coalesced -- only the most recent
    // file/loop pair is processed.  Use this in preference to the synchronous
    // loadFile() for any UI-driven path (preset switching, OSC, etc.).
    //
    // The thumbnail source is updated SYNCHRONOUSLY here (typically on the
    // message thread).  This is intentional: AudioThumbnail manages its own
    // peak-generation thread, and updating its source from the caller's
    // thread means the AUDIO load (running on LoaderThread) does not have
    // to wait on the thumbnail's previous decode job to finish.  With a
    // 32-entry thumbnail cache, navigating recently-seen presets does not
    // re-decode peaks at all -- the cached version is reused immediately.
    void requestLoad(const juce::File& file, bool shouldLoop)
    {
        if (file == juce::File() || ! file.existsAsFile())
            thumbnail.setSource(nullptr);
        else
            thumbnail.setSource(new juce::FileInputSource(file));

        // Mark the player as having a load in flight so subsequent play()
        // calls (and attachReaderToTransport's auto-start logic) defer
        // their transport.start() to the onLoadCompleted callback.  This
        // is what keeps the audio in sync after a hot-swap: starting the
        // transport eagerly (either via play() before the LoaderThread
        // runs, or via attachReaderToTransport's auto-start when the load
        // completes) would always start at position 0, producing the
        // load-duration desync that pause+play used to fix.  Cleared by
        // the LoaderThread itself after loadFile / unloadFile returns,
        // so it covers every load path including the same-file early
        // return inside loadFile().
        pendingLoad.store(true, std::memory_order_release);
        loaderThread.request(file, shouldLoop);
    }

    //==========================================================================
    juce::AudioThumbnail&       getThumbnail()       { return thumbnail; }
    const juce::AudioThumbnail& getThumbnail() const { return thumbnail; }

    /// Optional callback invoked on the message thread after each successful
    /// async file load completes.  Set by the owner (TimecodeEngine) once
    /// during construction; the lambda is expected to capture a
    /// juce::WeakReference so that a load in flight when the engine is
    /// destroyed does not produce a use-after-free.  See loadFile() for
    /// the firing site and the engine's constructor for the wiring.
    std::function<void()> onLoadCompleted;

private:
    //==========================================================================
    // LoaderThread -- dedicated I/O thread for asynchronous loadFile.
    //
    // Why a thread is needed:
    //   AudioTransportSource::setSource() (called from loadFile) tears down
    //   the previous BufferingAudioSource, which synchronises with its
    //   background reader.  With MP3 files, that reader can be in the middle
    //   of decoding a frame and the sync may take tens of ms.  Doing the
    //   load on the message thread freezes the UI.
    //
    // Why a single coalescing thread is enough:
    //   Multiple rapid preset changes (e.g. holding NEXT) only matter for the
    //   final destination -- intermediate loads are discarded.  Single-thread
    //   model also means we never have two concurrent loads racing on the
    //   transport.
    //
    // Lifetime:
    //   Stopped explicitly in the GeneratorAudioPlayer destructor before any
    //   member is destroyed.  Declared LAST below (after all members it may
    //   access) so even if the explicit stop is bypassed, RAII destruction
    //   order processes the thread first.
    //==========================================================================
    class LoaderThread : public juce::Thread
    {
    public:
        explicit LoaderThread(GeneratorAudioPlayer& o)
            : juce::Thread("STC Generator Loader"), owner(o)
        {
            startThread();
        }

        ~LoaderThread() override
        {
            stop();
        }

        void stop()
        {
            signalThreadShouldExit();
            notify();
            stopThread(3000);
        }

        void request(const juce::File& file, bool loop)
        {
            {
                const juce::ScopedLock sl(stateLock);
                pendingFile = file;
                pendingLoop = loop;
                hasPending  = true;
            }
            notify();
        }

    private:
        void run() override
        {
            while (! threadShouldExit())
            {
                wait(-1);
                if (threadShouldExit()) return;

                for (;;)
                {
                    juce::File file;
                    bool       loop = false;
                    {
                        const juce::ScopedLock sl(stateLock);
                        if (! hasPending) break;
                        file       = pendingFile;
                        loop       = pendingLoop;
                        hasPending = false;
                    }

                    // Apply loop first so any subsequent file load picks it up.
                    owner.setLooping(loop);

                    if (file == juce::File())
                        owner.unloadFile();
                    else
                        owner.loadFile(file);

                    // Single chokepoint for "load completed (or unloaded)".
                    // Doing it here, AFTER loadFile / unloadFile returns,
                    // covers every exit path inside loadFile -- success,
                    // same-file early return (line 187), unsupported
                    // format, empty / corrupt file -- so subsequent play()
                    // / seekSeconds calls behave consistently and the
                    // onLoadCompleted listener (TimecodeEngine's catch-up
                    // seek) gets to run regardless of which branch the
                    // load took inside.  Cleared BEFORE the callAsync so
                    // play() running inside the callback (via seekSeconds)
                    // sees pendingLoad == false and is allowed to start
                    // the transport.
                    owner.pendingLoad.store(false, std::memory_order_release);
                    if (owner.onLoadCompleted)
                        juce::MessageManager::callAsync(owner.onLoadCompleted);
                }
            }
        }

        GeneratorAudioPlayer& owner;
        juce::CriticalSection stateLock;
        juce::File            pendingFile;
        bool                  pendingLoop = false;
        bool                  hasPending  = false;
    };
    //==========================================================================
    // Internal: (re)wire the current reader source to the transport using
    // the file's native SR for resampling correction.  Caller must already
    // hold a reference to the reader (we do via currentReaderSource).
    //==========================================================================
    void attachReaderToTransport()
    {
        // Read the source pointer + sample rate under the lock, then call
        // transport.setSource() OUTSIDE the lock.  setSource() can briefly
        // block while it sets up the BufferingAudioSource and starts its
        // reader thread; doing it under the lock would block the UI.
        juce::AudioFormatReaderSource* src = nullptr;
        double sr = 0.0;
        {
            const juce::ScopedLock sl(transportLock);
            src = currentReaderSource.get();
            sr  = sourceFileSampleRate;
        }
        if (src == nullptr) return;

        // 32k sample read-ahead (~0.7s @ 48k) keeps file I/O off the audio thread.
        transport.setSource(src, 32768, &backgroundThread, sr, 2);  // max stereo
        // Always start a freshly-attached source from position 0; the
        // transport may otherwise carry over the position from the previous
        // source.  Subsequent seekSeconds() calls (e.g. from setGeneratorPosition)
        // can move it as needed.
        transport.setPosition(0.0);

        // Honour any pending "play" intent that was issued while the load
        // was still in-flight (race: play() called before the LoaderThread
        // finished attaching).  Without this, transport.start() from play()
        // would have been a no-op (no source yet) and the user's intent
        // would be lost.
        //
        // EXCEPT when an onLoadCompleted listener is wired: in that case
        // the listener (TimecodeEngine's catch-up seek) is responsible
        // for starting the transport AT the engine's current playhead,
        // so auto-starting here at position 0 would re-introduce the
        // hot-swap desync.  See requestLoad / play() / the LoaderThread's
        // callAsync for the full sequence.
        if (shouldPlay.load(std::memory_order_acquire)
            && ! userPaused.load(std::memory_order_acquire)
            && deviceOpen.load(std::memory_order_relaxed)
            && ! onLoadCompleted)
        {
            transport.start();
        }
    }

    //==========================================================================
    // AudioIODeviceCallback
    //==========================================================================
    void audioDeviceIOCallbackWithContext(const float* const*,
                                          int,
                                          float* const* outputChannelData,
                                          int numOutputChannels,
                                          int numSamples,
                                          const juce::AudioIODeviceCallbackContext&) override
    {
        // Always clear all outputs first; we only fill the routed ones.
        for (int ch = 0; ch < numOutputChannels; ++ch)
            if (outputChannelData[ch])
                std::memset(outputChannelData[ch], 0, sizeof(float) * (size_t) numSamples);

        // Shutting-down guard: if our destructor has latched this flag, we
        // return immediately without touching any other atomic / object.
        // This makes the callback's lifetime trivially safe even if some
        // exotic driver path delivers a final tick after removeAudioCallback
        // has been issued (which JUCE itself protects against on most
        // backends, but a single atomic load is cheap insurance).
        if (shuttingDown.load(std::memory_order_acquire)) return;

        if (userPaused.load(std::memory_order_acquire)) return;
        if (! transport.isPlaying()) return;
        if (! fileLoadedAtomic.load(std::memory_order_acquire)) return;

        // Pull stereo audio from transport into our scratch buffer.  The
        // buffer was pre-allocated in audioDeviceAboutToStart; if a driver
        // ever delivers a larger block we skip rather than allocate on the
        // audio thread.
        if (numSamples > scratchBuffer.getNumSamples()) return;
        scratchBuffer.clear(0, numSamples);
        juce::AudioSourceChannelInfo info(&scratchBuffer, 0, numSamples);

        // No transportLock here: JUCE's AudioTransportSource synchronises
        // setSource() against getNextAudioBlock() internally.  Holding our
        // own lock would compete with the LoaderThread (which now does
        // setSource() outside the lock too), and there's no extra safety
        // to gain since the reader source only gets destroyed after
        // setSource(nullptr) has detached it from the transport.
        transport.getNextAudioBlock(info);

        const int    selCh   = selectedChannel.load(std::memory_order_relaxed);
        const int    fcmRaw  = fileChannelMode.load(std::memory_order_relaxed);
        const float* left    = scratchBuffer.getReadPointer(0);
        const float* right   = scratchBuffer.getReadPointer(1);

        // File channel selection: in LeftOnly / RightOnly modes the file is
        // treated as mono (the chosen channel duplicated to both output sides
        // when in stereo mode) so the user doesn't hear LTC, click tracks, or
        // any other content that lives on the unselected channel.
        const bool monoFromFile = (fcmRaw == LeftOnly || fcmRaw == RightOnly);
        const float* monoSrc    = (fcmRaw == LeftOnly) ? left
                                : (fcmRaw == RightOnly) ? right
                                                        : nullptr;

        if (selCh < 0)
        {
            // Stereo output mode.
            if (numOutputChannels >= 2)
            {
                if (monoFromFile)
                {
                    // Duplicate mono source to both output channels so it sits
                    // centred in the stereo image.
                    if (outputChannelData[0])
                        std::memcpy(outputChannelData[0], monoSrc, sizeof(float) * (size_t) numSamples);
                    if (outputChannelData[1])
                        std::memcpy(outputChannelData[1], monoSrc, sizeof(float) * (size_t) numSamples);
                }
                else
                {
                    // True stereo: L -> ch 0, R -> ch 1.
                    if (outputChannelData[0])
                        std::memcpy(outputChannelData[0], left,  sizeof(float) * (size_t) numSamples);
                    if (outputChannelData[1])
                        std::memcpy(outputChannelData[1], right, sizeof(float) * (size_t) numSamples);
                }
            }
            else if (numOutputChannels == 1 && outputChannelData[0])
            {
                // Single-output device: mono down-mix.  In file-mono mode use
                // only the chosen channel; otherwise blend L+R as before.
                float* out = outputChannelData[0];
                if (monoFromFile)
                    std::memcpy(out, monoSrc, sizeof(float) * (size_t) numSamples);
                else
                    for (int i = 0; i < numSamples; ++i)
                        out[i] = (left[i] + right[i]) * 0.5f;
            }
        }
        else if (selCh < numOutputChannels && outputChannelData[selCh])
        {
            // Mono routing to a specific output channel.  In file-mono mode
            // send only the chosen file channel; otherwise blend L+R.
            float* out = outputChannelData[selCh];
            if (monoFromFile)
                std::memcpy(out, monoSrc, sizeof(float) * (size_t) numSamples);
            else
                for (int i = 0; i < numSamples; ++i)
                    out[i] = (left[i] + right[i]) * 0.5f;
        }
    }

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override
    {
        if (shuttingDown.load(std::memory_order_acquire)) return;

        if (device)
        {
            currentSampleRate    = device->getCurrentSampleRate();
            currentBufferSize    = device->getCurrentBufferSizeSamples();
            numChannelsAvailable = device->getActiveOutputChannels().countNumberOfSetBits();

            // Pre-allocate scratch so the audio thread never reallocates.
            scratchBuffer.setSize(2, currentBufferSize, false, false, false);
        }

        const juce::ScopedLock sl(transportLock);
        transport.prepareToPlay(currentBufferSize, currentSampleRate);
    }

    void audioDeviceStopped() override
    {
        if (shuttingDown.load(std::memory_order_acquire)) return;

        const juce::ScopedLock sl(transportLock);
        transport.releaseResources();
    }

    //==========================================================================
    juce::AudioFormatManager  formatManager;
    juce::AudioThumbnailCache thumbnailCache { 32 };  // cache the last 32 files' peaks so navigating presets back-and-forth does not re-decode each time
    juce::AudioThumbnail      thumbnail      { 512, formatManager, thumbnailCache };
    juce::TimeSliceThread     backgroundThread { "STC Generator Audio Reader" };
    juce::AudioDeviceManager  deviceManager;

    juce::CriticalSection                          transportLock;
    std::unique_ptr<juce::AudioFormatReaderSource> currentReaderSource;
    juce::AudioTransportSource                     transport;
    juce::File   currentFile;
    double       sourceFileSampleRate = 0.0;
    double       fileLengthSeconds    = 0.0;

    juce::AudioBuffer<float> scratchBuffer;

    std::atomic<bool> deviceOpen      { false };
    std::atomic<bool> shuttingDown    { false };  // set in dtor before closeDevice; audio callback returns immediately if true
    std::atomic<int>  selectedChannel { -1 };
    std::atomic<bool> loopFlag        { false };
    std::atomic<bool> userPaused      { false };  // logical pause state -- see pause() comment
    std::atomic<bool> shouldPlay      { false };  // caller intent across async loads
    std::atomic<int>  fileChannelMode { 0 };      // 0=Stereo, 1=LeftOnly, 2=RightOnly

    // Lock-free mirrors for hot UI accessors.  The waveform view repaints at
    // 30 Hz and queries hasFileLoaded() / getFileLengthSeconds() each time;
    // routing those through transportLock would freeze the UI whenever the
    // LoaderThread is mid-load (transport.stop() / setSource() can block on
    // the audio reader thread for tens to hundreds of ms with MP3).
    std::atomic<bool>   fileLoadedAtomic    { false };
    std::atomic<double> fileLengthAtomic    { 0.0 };

    // Set by requestLoad, cleared by LoaderThread after loadFile/unloadFile
    // returns.  Read by play() and attachReaderToTransport to defer
    // transport.start() while a load is in flight -- the onLoadCompleted
    // callback handles the start with the engine's current playhead so the
    // audio does not race ahead of the cursor during a hot-swap.
    std::atomic<bool>   pendingLoad         { false };

    juce::String currentDeviceName, currentTypeName;
    int    numChannelsAvailable = 0;
    double currentSampleRate    = 0.0;
    int    currentBufferSize    = 0;

    juce::String loadError;          // human-readable last-load-error message

    // Must be the LAST member declared.  Its destructor stops the thread
    // before any other member is torn down, so the thread cannot race
    // against the destructor of transport / currentReaderSource / thumbnail.
    LoaderThread loaderThread { *this };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GeneratorAudioPlayer)
};
