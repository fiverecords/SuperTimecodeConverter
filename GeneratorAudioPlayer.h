// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

#pragma once
#include <JuceHeader.h>
#include "AudioDeviceHub.h"
#include <atomic>
#include <cstring>
#include <system_error>
#include <thread>

//==============================================================================
// GeneratorAudioPlayer -- Plays a single audio file, transport-controlled,
// to an audio output device.  Used by the internal Generator to optionally
// play a song file synchronised with the generated timecode.
//
// The output device is opened through AudioDeviceHub (DESIGN D23), so it may
// be shared with LTC out, Audio Thru and other engines' players on the same
// interface; each writes only its own channels.  Channel routing matches the
// LtcOutput convention: -1 = stereo on channels 0+1, >=0 = mono mix on the
// named channel.
//
// Pipeline:
//   AudioFormatReader -> AudioFormatReaderSource -> AudioTransportSource
//     (BufferingAudioSource read-ahead on backgroundThread, then
//      ResamplingAudioSource) -> our audio callback -> selected output(s)
//
// Threading:
//  - Message thread (UI, OSC handlers dispatched via callAsync, the engine
//    tick): openDevice / closeDevice / requestLoad / play / pause /
//    stopAndReset / seekSeconds / setLoopingWhilePlaying and the getters.
//    It never asks the disk about a preset's file (AUD11 GENERATOR-4):
//    requestLoad only queues the request, and the waveform thumbnail is
//    handed its reader, already open, by the LoaderThread (postWaveform).
//  - LoaderThread ("STC Generator Loader"): loadFile / unloadFile /
//    setLooping, as requestLoad schedules them.  What a load needs of the
//    disk -- whether the file exists, opening it and reading its header,
//    for the transport and for the thumbnail -- runs on a short-lived
//    thread per load that the LoaderThread waits for (DiskJob), so that a
//    player destroyed while a disk does not answer can leave it behind.
//    setLooping also runs on the message thread, inside
//    setLoopingWhilePlaying, only while no load is pending.
//  - Audio thread: audioDeviceIOCallbackWithContext, called by the hub's
//    fan-out.  audioDeviceAboutToStart / audioDeviceStopped run when the hub
//    adds or removes us or the device restarts (message thread, or the
//    driver's thread for a restart it starts itself).  A restart is told
//    to the owner on the message thread (onDeviceRestarted).
//  - sourceLock serialises everything that builds, replaces, prepares,
//    repositions or starts the transport's source chain (setSource,
//    prepareToPlay, releaseResources, setPosition, start) between those
//    threads: AudioTransportSource::setSource is not re-entrant (two calls
//    at once delete the same BufferingAudioSource), and setPosition and
//    start read the chain pointers setSource replaces, without a lock.  It
//    is held for as long as a load's setSource takes: the old chain's
//    read-ahead thread finishing the chunk it is reading, and the new one
//    prefilling its buffer from disk -- about 10 ms for a WAV on a local
//    disk, as long as the read takes on a slow or sleeping one.  It is
//    never taken on the audio thread.  play(), seekSeconds() and
//    stopAndReset() do not touch the transport while a load is pending
//    (pendingLoad), so the message thread waits on a load only in
//    openDevice() / closeDevice() and in a device restart
//    (audioDeviceAboutToStart / audioDeviceStopped, run by the hub).
//  - transportLock guards our own state (reader source, file, tags, length,
//    load error) and is only ever held for a few assignments, so UI
//    accessors never wait on a load.  Order: sourceLock, then transportLock.
//  - The audio callback takes neither: AudioTransportSource takes its own
//    callback lock in getNextAudioBlock, which setSource also takes to swap
//    the chain.  What that costs the audio thread is described there
//    (AUDIT LTC-3).
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
        // The same formats for the loads' disk jobs, which can outlive the
        // player (DiskJob, AUD11 GENERATOR-9) and so share their own.
        diskFormats->registerBasicFormats();
    }

    ~GeneratorAudioPlayer() override
    {
        // Order matters here.  The audio device is shared through the hub,
        // which may keep calling us until we release; an in-flight callback
        // reading already-destroyed atomics was the std::atomic load crash
        // on shutdown we hunted in v1.9.7/v1.9.8.
        //
        // Step 1: latch shuttingDown so any in-flight or imminent audio
        // callback returns immediately without touching other atomics.
        // The acquire/release pairing on shuttingDown ensures the callback
        // sees the flag if it observes the store.
        shuttingDown.store(true, std::memory_order_release);
        // A waveform hand-over still in the message queue (postWaveform)
        // finds this set and leaves the thumbnail alone.
        destroyed->store(true, std::memory_order_release);

        // Step 2: closeDevice() releases us from the hub, which blocks until
        // any in-flight callback returns, so the audio thread is provably
        // done with us BEFORE we leave the user-defined destructor body and
        // member destruction begins.
        // The loader thread is stopped first because its tick can call back
        // into transport / reader objects that closeDevice will tear down.
        // It is waited for, however long, and never killed (AUD11
        // GENERATOR-9): a load still asking the disk about its file is
        // abandoned at once (DiskJob), one already swapping the transport's
        // source finishes the swap first -- at worst the read in flight on
        // the read-ahead thread, which reads no more now that shuttingDown
        // is set (ReaderSource).
        loaderThread.stop();
        closeDevice();
        unloadFile();
        // Belt-and-braces: make sure the hub no longer references us, also
        // on the path where ~GeneratorAudioPlayer runs without ever having
        // opened a device.
        AudioDeviceHub::get().release(this);
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

        juce::String err;
        // Follows the global SAMPLE RATE / BUFFER SIZE like every other
        // audio component (its own per-engine format went with DESIGN D23).
        auto* device = AudioDeviceHub::get().acquire(this, typeName, devName, false, sampleRate, bufferSize, err);
        if (device == nullptr) return false;

        currentSampleRate    = device->getCurrentSampleRate();
        currentBufferSize    = device->getCurrentBufferSizeSamples();
        numChannelsAvailable = device->getActiveOutputChannels().countNumberOfSetBits();

        if (! backgroundThread.isThreadRunning())
            backgroundThread.startThread();

        // If a file was previously loaded, attach it now that the device SR
        // is known.  deviceOpen is set under the same lock, so a load on the
        // LoaderThread either finishes before this (and is attached here) or
        // sees the device open and attaches itself after.
        {
            const juce::ScopedLock sl(sourceLock);
            attachReaderToTransport();
            deviceOpen.store(true, std::memory_order_relaxed);
        }
        return true;
    }

    void closeDevice()
    {
        if (deviceOpen.load(std::memory_order_relaxed))
        {
            {
                // setSource(nullptr) also stops the transport, under its own
                // callback lock.  No transport.stop() first: stop() waits up
                // to a second (500 x 2 ms) for getNextAudioBlock to see it,
                // and our callback no longer calls getNextAudioBlock once the
                // transport is not playing (nor while paused).
                const juce::ScopedLock sl(sourceLock);
                transport.setSource(nullptr);
                deviceOpen.store(false, std::memory_order_relaxed);
            }
            AudioDeviceHub::get().release(this);   // blocks until any in-flight callback returns
        }
        // Waited for, not killed after two seconds (AUD11 GENERATOR-9): the
        // setSource(nullptr) above has already waited for the read it had
        // in flight, so it has nothing left to do.
        if (backgroundThread.isThreadRunning())
            backgroundThread.stopThread(-1);
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
    /// What the waveform thumbnail is to show after a load or an unload:
    /// a reader of its own on the new file, opened by the load, or none.
    /// `changed` stays false when the load left the loaded file in place.
    struct WaveformChange
    {
        bool changed = false;
        std::unique_ptr<juce::AudioFormatReader> reader;
        juce::int64 hash = 0;   // the thumbnail cache's key for the file
    };

    /// What a load asks of the disk: whether the file is there, a reader on
    /// it for the transport and a second one for the waveform thumbnail.
    /// These are the calls that wait for as long as a disk takes to answer
    /// (a USB disk spinning up, a share that has gone away), and they touch
    /// nothing of the player: they run on a thread of their own, which the
    /// job and its results belong to as much as to the load that waits for
    /// it (askDisk).  A player destroyed meanwhile abandons the job, and
    /// the thread ends by itself whenever the disk lets it (AUD11
    /// GENERATOR-9: the LoaderThread used to make these calls, and was
    /// killed by force when it had not come back after three seconds).
    struct DiskJob
    {
        // Set by the load before the job starts.
        juce::File file;
        juce::File loadedFile;                      // the file the player holds, if any,
        juce::int64 loadedSize = 0, loadedModTime = 0;   // with its size and time when it was loaded
        std::shared_ptr<juce::AudioFormatManager> formats;

        // The answer, for the load to read once `done` is signalled.
        bool found = false;
        juce::int64 size = 0, modTime = 0;          // bytes; ms since the epoch
        bool sameAsLoaded = false;                  // nothing to load: the readers are not opened
        std::unique_ptr<juce::AudioFormatReader> reader, waveformReader;

        juce::WaitableEvent done { true };          // manual reset
        std::atomic<bool>   abandoned { false };    // nobody waits any more: stop at the next step

        void run()
        {
            found = file.existsAsFile();
            if (found && ! abandoned.load(std::memory_order_acquire))
            {
                // The same file is the same path with the same size and
                // modification time (AUD11 GENERATOR-7).  By the path
                // alone, a file replaced on disk under its name -- the
                // track exported again from the DAW during a rehearsal --
                // was never loaded again: GO played the old length (and on
                // POSIX the old data), under the old waveform, until
                // another preset's file had been loaded in between.
                size    = file.getSize();
                modTime = file.getLastModificationTime().toMilliseconds();
                sameAsLoaded = file == loadedFile && size == loadedSize && modTime == loadedModTime;
            }
            if (found && ! sameAsLoaded && ! abandoned.load(std::memory_order_acquire))
            {
                reader.reset(formats->createReaderFor(file));
                // The waveform thumbnail reads the file on its own thread,
                // so it needs a reader of its own -- only for a file the
                // player takes (AUD11 FUZZ-14).  The message thread used to
                // open it in requestLoad, before the load had started
                // (AUD11 GENERATOR-4).
                if (reader != nullptr && isUsable(*reader) && ! abandoned.load(std::memory_order_acquire))
                    waveformReader.reset(formats->createReaderFor(file));
            }
            done.signal();
        }
    };

    /// Load an audio file synchronously.  Replaces any previously loaded
    /// file.  Returns true on success.  On failure, getLoadError() returns
    /// a human-readable description of why.  Runs on the LoaderThread
    /// (requestLoad); it blocks on file I/O, so not for the message thread.
    /// The waveform thumbnail follows through the message queue
    /// (postWaveform).
    bool loadFile(const juce::File& file)
    {
        WaveformChange waveform;
        const bool ok = loadFile(file, waveform);
        postWaveform(waveform);
        return ok;
    }

    /// The load itself.  `waveform` is given what the thumbnail is to show
    /// once the load is done: the LoaderThread posts it after it has
    /// settled the request, so the audio does not wait for the waveform.
    bool loadFile(const juce::File& file, WaveformChange& waveform)
    {
        setLoadError({});
        requestedFileMissing.store(false, std::memory_order_release);

        if (file == juce::File())
        {
            unloadFile();
            waveform.changed = true;
            return false;
        }

        // This is where the disk is asked whether the file is there (AUD11
        // GENERATOR-4): requestLoad and its callers no longer do, so a path
        // that is not on disk gets here.  Asked on a thread of its own
        // (DiskJob), together with the opening of the file.
        auto job = std::make_shared<DiskJob>();
        job->file    = file;
        job->formats = diskFormats;
        {
            const juce::ScopedLock sl(transportLock);
            if (currentReaderSource != nullptr)
            {
                job->loadedFile    = currentFile;
                job->loadedSize    = currentFileSize;
                job->loadedModTime = currentFileModTime;
            }
        }
        if (! askDisk(job))
            return false;   // the player is being destroyed: nothing was touched

        // The message names the file, as the panel's status line did when
        // it was the one to find it missing.
        if (! job->found)
        {
            requestedFileMissing.store(true, std::memory_order_release);
            return failLoad("FILE NOT FOUND: " + file.getFileName(), waveform);
        }

        // Skip reload if same file is already loaded -- the same path,
        // size and modification time (DiskJob, AUD11 GENERATOR-7).
        if (job->sameAsLoaded)
            return true;

        std::unique_ptr<juce::AudioFormatReader> newReader(std::move(job->reader));
        if (newReader == nullptr)
        {
            // Most likely cause: the file's format is not registered with
            // the FormatManager.  For .mp3 specifically, this means the
            // Projucer flag JUCE_USE_MP3AUDIOFORMAT was not set when the
            // juce_audio_formats module was compiled.
            return failLoad("UNSUPPORTED FORMAT: " + file.getFileExtension().removeCharacters(".").toUpperCase(), waveform);
        }

        const double  readerSR     = newReader->sampleRate;
        const int64_t totalSamples = newReader->lengthInSamples;

        // Track identity for the outputs that carry metadata (TCNet, #20).
        // WAV/AIFF carry LIST INFO tags JUCE exposes (IART / INAM); JUCE's
        // MP3 and FLAC readers expose no tags.  Otherwise the file name: the
        // "Artist - Title" convention when present, else the name as title.
        juce::String tagArtist = newReader->metadataValues.getValue("IART", "").trim();
        juce::String tagTitle  = newReader->metadataValues.getValue("INAM", "").trim();
        if (tagTitle.isEmpty())
        {
            const juce::String base = file.getFileNameWithoutExtension().trim();
            if (base.contains(" - "))
            {
                if (tagArtist.isEmpty()) tagArtist = base.upToFirstOccurrenceOf(" - ", false, false).trim();
                tagTitle = base.fromFirstOccurrenceOf(" - ", false, false).trim();
            }
            else
                tagTitle = base;
        }

        // What the reader says of the file is checked before the reader is
        // used (AUD11 FUZZ-14): a header that states an absurd sample size
        // or channel count gets no further than this.
        if (! isUsable(*newReader))
            return failLoad("EMPTY OR CORRUPT FILE", waveform);

        // The thumbnail's own reader, opened by the job with this one.  The
        // thumbnail cache goes by the path with the file's size and time
        // too, or it would bring back the waveform of the file that was
        // there before (AUD11 GENERATOR-7).
        waveform.reader  = std::move(job->waveformReader);
        waveform.hash    = (juce::int64) ((juce::uint64) file.hashCode64()
                                          ^ ((juce::uint64) job->modTime * 31u)
                                          ^ ((juce::uint64) job->size * 0x9E3779B97F4A7C15ull));
        waveform.changed = true;

        // AudioFormatReaderSource takes ownership of the reader.
        std::unique_ptr<juce::AudioFormatReaderSource> newSource(
            new ReaderSource(newReader.release(), shuttingDown));
        newSource->setLooping(loopFlag.load(std::memory_order_relaxed));

        // Detach, swap and re-attach as one step under sourceLock, so the
        // message thread's openDevice / closeDevice / seekSeconds cannot run
        // setSource or setPosition in between (AUDIT LTC-1).  setSource can
        // wait here for the reader thread (the old BufferingAudioSource
        // finishing its current read, the new one prefilling); transportLock
        // is held only for the swap, so the UI accessors never wait on it.
        // No transport.stop() first: setSource stops it (see closeDevice).
        std::unique_ptr<juce::AudioFormatReaderSource> oldSource;
        {
            const juce::ScopedLock srcSl(sourceLock);
            transport.setSource(nullptr);

            // From here the transport no longer points at the old reader
            // source, so it can be destroyed -- after the locks are released.
            {
                const juce::ScopedLock sl(transportLock);
                oldSource            = std::move(currentReaderSource);
                currentReaderSource  = std::move(newSource);
                currentFile          = file;
                currentFileSize      = job->size;
                currentFileModTime   = job->modTime;
                trackArtist          = tagArtist;
                trackTitle           = tagTitle;
                sourceFileSampleRate = readerSR;
                fileLengthSeconds    = (readerSR > 0.0) ? (double) totalSamples / readerSR : 0.0;
            }

            // Update lock-free mirrors AFTER the state is consistent.
            fileLengthAtomic.store(fileLengthSeconds, std::memory_order_release);
            fileLoadedAtomic.store(true, std::memory_order_release);

            // Note: the AudioThumbnail is not touched here.  It is given
            // `waveform` afterwards (postWaveform), once the request is
            // settled: taking the previous file's thumbnail down waits for
            // the thumbnail's own decode job, which can take hundreds of ms
            // with MP3 if that file was still being processed, and the
            // audio does not wait for that.

            if (deviceOpen.load(std::memory_order_relaxed))
                attachReaderToTransport();
        }
        // oldSource destroyed here, outside both locks.

        return true;
    }

    /// Last load failure description, or empty if the last call succeeded
    /// (or no file has been loaded yet).  Written on the LoaderThread, read
    /// by the views' paint: a copy taken under transportLock.
    juce::String getLoadError() const
    {
        const juce::ScopedLock sl(transportLock);
        return loadError;
    }

    void unloadFile()
    {
        // Same pattern as loadFile: detach and swap under sourceLock, our
        // state under transportLock only for the assignments.
        std::unique_ptr<juce::AudioFormatReaderSource> oldSource;
        {
            const juce::ScopedLock srcSl(sourceLock);
            transport.setSource(nullptr);

            const juce::ScopedLock sl(transportLock);
            oldSource            = std::move(currentReaderSource);
            currentFile          = juce::File();
            currentFileSize      = 0;
            currentFileModTime   = 0;
            trackArtist.clear();
            trackTitle.clear();
            sourceFileSampleRate = 0.0;
            fileLengthSeconds    = 0.0;
        }
        // oldSource destroyed here, outside the locks.

        fileLengthAtomic.store(0.0, std::memory_order_release);
        fileLoadedAtomic.store(false, std::memory_order_release);
        // The thumbnail is managed separately (postWaveform).
    }

    /// True when the request the LoaderThread settled last named a file
    /// that is not on disk (AUD11 GENERATOR-4: nobody asks the disk before
    /// the load does).  Read it once the load is no longer pending, as the
    /// onLoadCompleted listener does.  Lock-free, any thread.
    bool isRequestedFileMissing() const
    {
        return requestedFileMissing.load(std::memory_order_acquire);
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

    /// Artist / title of the loaded file (tags, else the file name; see
    /// loadFile).  Empty when nothing is loaded.
    juce::String getTrackArtist() const { const juce::ScopedLock sl(transportLock); return trackArtist; }
    juce::String getTrackTitle()  const { const juce::ScopedLock sl(transportLock); return trackTitle; }

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
    // play() during a pending async load: shouldPlay is set and nothing
    // else.  The start comes once the load is done, from the onLoadCompleted
    // listener (TimecodeEngine's catch-up: seekSeconds at the engine's
    // playhead), or, when no listener is wired, from attachReaderToTransport
    // at position 0.  This avoids the race where transport.start() runs
    // before any source exists, or on the file about to be replaced.
    void play()
    {
        shouldPlay.store(true,  std::memory_order_release);
        userPaused.store(false, std::memory_order_release);
        if (! hasFileLoaded()) return;             // nothing to start yet; the load's catch-up (or attach) will
        // If a load is in flight, defer transport.start() to the
        // onLoadCompleted callback so the audio is started AT the engine's
        // current playhead instead of from position 0.  Without this guard
        // play() called from generatorPlay()/activateGenPreset during a
        // hot-swap would start the previously loaded file from 0, audible
        // for the load duration, and then the new file would also start
        // from 0 when attachReaderToTransport runs -- the persistent
        // cursor/audio desync the operator reported.  (A transport still
        // running from before the load is not stopped here: the audio
        // callback plays silence while the load is pending.)
        if (pendingLoad.load(std::memory_order_acquire)) return;
        if (! deviceOpen.load(std::memory_order_relaxed)) return;
        const juce::ScopedLock sl(sourceLock);
        awaitingSeek.store(false, std::memory_order_release);   // a start the owner asks for after the load: where the transport is
        transport.start();
    }

    /// Pauses playback while preserving the current position.
    /// Sets a flag that the audio callback checks before pulling samples,
    /// so the message thread returns INSTANTLY.  AudioTransportSource::stop()
    /// would not: it waits up to a second for getNextAudioBlock to see the
    /// stop, and our callback stops calling getNextAudioBlock as soon as the
    /// transport is not playing.  The transport keeps "playing" internally
    /// (no audio is produced because the callback skips it), so resume is
    /// instantaneous when play() flips the flag back.
    void pause()
    {
        userPaused.store(true, std::memory_order_release);
    }

    /// Stops and rewinds to the start of the file.  While a load is pending
    /// the rewind is skipped, so the message thread does not wait for the
    /// load's source swap (sourceLock).  A newly loaded file is attached at
    /// position 0 (attachReaderToTransport); a request for the file already
    /// loaded, an unload, or a load with the device closed attaches nothing
    /// and leaves the old position.  A start does not use it: from Stopped
    /// it seeks first (TimecodeEngine's generatorPlay), and the last load's
    /// onLoadCompleted catch-up seeks to the engine's playhead whenever the
    /// generator is not Stopped (seekSeconds).
    void stopAndReset()
    {
        shouldPlay.store(false, std::memory_order_release);
        userPaused.store(true,  std::memory_order_release);
        if (pendingLoad.load(std::memory_order_acquire)) return;
        const juce::ScopedLock sl(sourceLock);
        transport.setPosition(0.0);
    }

    /// Seek to position in seconds, relative to the start of the audio file.
    /// When looping, positions beyond file length are folded back via fmod.
    /// If the player was supposed to be playing (shouldPlay set, not paused)
    /// but the transport had auto-stopped (typically because it reached EOF
    /// before this seek), the transport is re-engaged so audio resumes from
    /// the new position.  start() is a no-op when the transport is already
    /// playing, so the in-flight seek-while-playing case is unaffected.
    /// Message thread.
    ///
    /// While a load is pending it does nothing (AUDIT LTC-4): the position
    /// and the start would go to the file about to be replaced -- a stale
    /// onLoadCompleted catch-up, posted by a load that settled before a
    /// newer requestLoad, started that file until the swap -- and the
    /// message thread would wait for the load's source swap (sourceLock).
    /// The last load's onLoadCompleted catch-up seeks to the engine's
    /// playhead whenever the generator is not Stopped, and a start from
    /// Stopped seeks first (generatorPlay).  While Playing the catch-up
    /// also starts the transport; while Paused it only positions it, which
    /// makes up for a seek dropped here: a click on the timeline while a
    /// preset's file loads (the click turns Stopped into Paused) used to
    /// leave the transport where it was -- 0 in a newly attached file, the
    /// old position otherwise -- and a play from Paused does not seek, so
    /// the audio started there while the timecode ran from the click.
    void seekSeconds(double seconds)
    {
        if (! hasFileLoaded()) return;
        if (pendingLoad.load(std::memory_order_acquire)) return;

        seconds = juce::jmax(0.0, seconds);

        const double len = fileLengthAtomic.load(std::memory_order_acquire);
        if (loopFlag.load(std::memory_order_relaxed) && len > 0.0)
        {
            seconds = std::fmod(seconds, len);
        }
        else if (len > 0.0 && seconds > len)
        {
            seconds = len;
        }

        const juce::ScopedLock sl(sourceLock);
        transport.setPosition(seconds);
        // The transport is where the owner wants it: the audio callback may
        // pull it again (awaitingSeek, AUD11 GENERATOR-10).  After the
        // setPosition, so that no block is taken from the old position.
        awaitingSeek.store(false, std::memory_order_release);

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

    /// True from requestLoad until the LoaderThread has settled the last
    /// queued request (pendingLoad).  Meanwhile the transport holds the file
    /// being replaced: isPlaying() may say true, but the audio callback no
    /// longer pulls it, so its position does not move (TimecodeEngine skips
    /// its audio clock discipline then, AUDIT LTC-4).  Lock-free, any
    /// thread.
    bool   isLoadPending()             const
    {
        return pendingLoad.load(std::memory_order_acquire);
    }

    //==========================================================================
    // Loop
    //==========================================================================
    /// The LoaderThread, for each request (requestLoad); the message thread
    /// through setLoopingWhilePlaying.  AudioFormatReaderSource's flag is a
    /// plain bool the read-ahead and audio threads read, as it always was.
    void setLooping(bool shouldLoop)
    {
        loopFlag.store(shouldLoop, std::memory_order_relaxed);
        const juce::ScopedLock sl(transportLock);
        if (currentReaderSource)
            currentReaderSource->setLooping(shouldLoop);
    }

    bool isLooping() const { return loopFlag.load(std::memory_order_relaxed); }

    /// Switch the loop while the file may be playing: TimecodeEngine's
    /// timecode loop (#24), which loops the file itself when its end is the
    /// file's end and must stop doing so when that changes.  Message thread,
    /// and not while a load is pending (the request in flight carries its own
    /// flag; the engine comes back once it completes).
    ///  - Off: JUCE's read-ahead position is folded into the file only while
    ///    the source loops (BufferingAudioSource::getNextReadPosition), so
    ///    once the file has gone round it lies past the end and the transport
    ///    would stop at once.  It is folded back here, so the pass that is
    ///    playing ends at the file's end.
    ///  - On: a transport that has already stopped at the file's end (the
    ///    engine's timecode trails the audio by a few ms) is started again;
    ///    with the loop on, its position reads folded, from the top.
    /// Both may leave a brief gap: BufferingAudioSource discards its
    /// read-ahead when the loop changes, and the fold is a seek.
    void setLoopingWhilePlaying(bool shouldLoop)
    {
        if (pendingLoad.load(std::memory_order_acquire)) return;
        setLooping(shouldLoop);
        if (! hasFileLoaded()) return;

        const bool wantsSound = shouldPlay.load(std::memory_order_acquire)
                             && ! userPaused.load(std::memory_order_acquire)
                             && deviceOpen.load(std::memory_order_relaxed);
        const double len = fileLengthAtomic.load(std::memory_order_acquire);
        const juce::ScopedLock sl(sourceLock);
        if (shouldLoop)
        {
            if (wantsSound && ! transport.isPlaying())
                transport.start();
            return;
        }
        const double pos = transport.getCurrentPosition();
        if (len <= 0.0 || pos < len) return;
        transport.setPosition(std::fmod(pos, len));
        if (wantsSound)
            transport.start();
    }

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
    // No file is touched here (AUD11 GENERATOR-4).  This used to ask the
    // disk whether the file exists and to give the thumbnail its source,
    // which opens the file and reads its header -- on the caller's thread,
    // the message thread, before the LoaderThread had started: a disk that
    // answered slowly (a USB disk spinning up, a network share) held the
    // window, the cue triggers, OSC and the tick of every engine for as
    // long.  A file that is not on disk is now found missing by the load
    // (isRequestedFileMissing), and the thumbnail gets its reader from the
    // LoaderThread once the load is done (postWaveform); until then it
    // shows the previous file.  With a 32-entry thumbnail cache, navigating
    // recently-seen presets does not re-decode peaks at all -- the cached
    // version is reused immediately.
    void requestLoad(const juce::File& file, bool shouldLoop)
    {
        // Mark the player as having a load in flight so that play(),
        // seekSeconds() and stopAndReset() leave the transport alone until
        // it is done, and the start comes from the onLoadCompleted callback
        // at the engine's playhead.  This is what keeps the audio in sync
        // after a hot-swap: starting the transport eagerly, from play()
        // before the LoaderThread runs, started the old file and then the
        // new one at position 0, the load-duration desync that pause+play
        // used to fix.  (attachReaderToTransport's own auto-start is off
        // whenever onLoadCompleted is wired; it does not read this flag.)
        // Set by LoaderThread::request under its state lock, and cleared by
        // the LoaderThread only when no further request is queued after the
        // load it just finished (AUDIT LTC-4), so it covers every load path
        // including the same-file early return inside loadFile().
        loaderThread.request(file, shouldLoop);
    }

    //==========================================================================
    juce::AudioThumbnail&       getThumbnail()       { return thumbnail; }
    const juce::AudioThumbnail& getThumbnail() const { return thumbnail; }

    /// Optional callback invoked on the message thread once the LoaderThread
    /// has finished the last queued request, whatever its outcome (loaded,
    /// unloaded, same file, failed); a load replaced by a newer request
    /// before it was done does not fire it.  Set by the owner
    /// (TimecodeEngine) once during construction; the lambda is expected to
    /// capture a juce::WeakReference so that a load in flight when the
    /// engine is destroyed does not produce a use-after-free.  See
    /// LoaderThread::run for the firing site and the engine's constructor
    /// for the wiring.
    std::function<void()> onLoadCompleted;

    /// Optional callback invoked on the message thread after the device
    /// this player is open on has been started again under it (AUD11
    /// GENERATOR-8): the hub reopening it for the global SAMPLE RATE /
    /// BUFFER SIZE, for another component joining it, or after the
    /// interface was lost and came back, or the driver restarting it
    /// itself.  The transport keeps its position across the restart while
    /// the owner's timecode has run on, so the owner brings the audio back
    /// to it (TimecodeEngine::resyncGeneratorAudioAfterRestart).  Not for the
    /// player's own openDevice, whose caller does that itself.  Set once,
    /// as onLoadCompleted, with the same care for the engine's lifetime.
    std::function<void()> onDeviceRestarted;

private:
    //==========================================================================
    // LoaderThread -- dedicated I/O thread for asynchronous loadFile.
    //
    // Why a thread is needed:
    //   loadFile opens and parses the file, and AudioTransportSource::
    //   setSource() tears down the previous BufferingAudioSource (waiting for
    //   its background reader to finish the chunk it is decoding) and
    //   prefills the new one from disk.  With MP3 files that can take tens
    //   of ms.  Doing the load on the message thread freezes the UI.
    //
    // Why a single coalescing thread is enough:
    //   Multiple rapid preset changes (e.g. holding NEXT) only matter for the
    //   final destination -- intermediate loads are discarded.  Single-thread
    //   model also means we never have two concurrent loads racing on the
    //   transport; the message thread's own transport calls are serialised
    //   with it by sourceLock.
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

        // Waits for the thread, however long, and never kills it (AUD11
        // GENERATOR-9).  stopThread(3000) killed it by force when a load
        // had not come back in three seconds -- waiting on a disk that had
        // stopped answering, the moment an operator gives up and quits --
        // and the process then aborted (Linux, run) or went on with the
        // thread terminated while it held sourceLock (Windows, by reading).
        // The wait is short: the disk calls of a load run on a thread that
        // can be left behind (DiskJob), and run() starts nothing new once
        // it is told to exit.
        void stop()
        {
            signalThreadShouldExit();
            notify();
            stopThread(-1);
        }

        // Message thread (requestLoad).  pendingLoad is set under stateLock,
        // the lock run() clears it under, so "a request is queued" always
        // implies pendingLoad.
        void request(const juce::File& file, bool loop)
        {
            {
                const juce::ScopedLock sl(stateLock);
                pendingFile = file;
                pendingLoop = loop;
                hasPending  = true;
                owner.pendingLoad.store(true, std::memory_order_release);
                // With a listener wired, the audio also waits for the seek
                // its catch-up makes (awaitingSeek, AUD11 GENERATOR-10).
                if (owner.onLoadCompleted)
                    owner.awaitingSeek.store(true, std::memory_order_release);
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
                    // The player is being destroyed: a request still queued
                    // is not started (AUD11 GENERATOR-9).
                    if (threadShouldExit()) return;

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

                    // What the waveform thumbnail is to show after this
                    // request; posted below, after the audio is settled.
                    WaveformChange waveform;
                    owner.loadFile(file, waveform);   // an empty file unloads

                    // Told to exit meanwhile -- the load may have been
                    // abandoned (askDisk): nothing to settle, nobody to
                    // tell.
                    if (threadShouldExit()) return;

                    // Single chokepoint for "load completed (or unloaded)".
                    // Doing it here, AFTER loadFile / unloadFile returns,
                    // covers every exit path inside loadFile -- success,
                    // the same-file early return near its top, unsupported
                    // format, empty / corrupt file -- so subsequent play()
                    // / seekSeconds calls behave consistently and the
                    // onLoadCompleted listener (TimecodeEngine's catch-up
                    // seek) gets to run regardless of which branch the
                    // load took inside.
                    //
                    // Only when nothing newer is queued (AUDIT LTC-4): with
                    // another request waiting, pendingLoad stays set and the
                    // loop goes straight on to it, so a play() in between
                    // cannot start the file that is about to be replaced.
                    // Cleared BEFORE the callAsync so play() running inside
                    // the callback (via seekSeconds) sees pendingLoad ==
                    // false and is allowed to start the transport.
                    bool settled = false;
                    {
                        const juce::ScopedLock sl(stateLock);
                        if (! hasPending)
                        {
                            owner.pendingLoad.store(false, std::memory_order_release);
                            settled = true;
                        }
                    }
                    //
                    // A copy goes to callAsync: it takes its argument by
                    // forwarding reference and moves from it (JUCE 8 and 9),
                    // so handing it the member itself left the member empty
                    // after the first load.  Every later load then went
                    // without the catch-up, and attachReaderToTransport,
                    // which reads the member, started each new file at 0
                    // itself -- the late audio AUDIT LTC-4 had fixed.
                    if (settled && owner.onLoadCompleted)
                        juce::MessageManager::callAsync(std::function<void()>(owner.onLoadCompleted));

                    owner.postWaveform(waveform);
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
    // the file's native SR for resampling correction.  Caller holds
    // sourceLock (openDevice on the message thread, loadFile on the
    // LoaderThread), so currentReaderSource cannot be swapped and destroyed
    // between reading it here and handing it to setSource.
    //==========================================================================
    void attachReaderToTransport()
    {
        // Read the source pointer + sample rate under transportLock, then
        // call transport.setSource() outside it.  setSource() can block
        // while it prefills the new BufferingAudioSource from disk; doing
        // that under transportLock would block the UI accessors.
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
        // While a load is pending the transport still holds the file being
        // replaced, and it may still be running: stopAndReset() only mutes
        // it, and a preset change during playback (stop, load, play) unmutes
        // it at once, so it played from its start until the LoaderThread's
        // swap stopped it (AUDIT LTC-4).  Silence instead; the new file
        // starts from the onLoadCompleted catch-up.
        if (pendingLoad.load(std::memory_order_acquire)) return;
        // And until the owner has put the transport where it wants it
        // (awaitingSeek, AUD11 GENERATOR-10).
        if (awaitingSeek.load(std::memory_order_acquire)) return;
        if (! transport.isPlaying()) return;
        if (! fileLoadedAtomic.load(std::memory_order_acquire)) return;

        // Pull stereo audio from transport into our scratch buffer.  The
        // buffer was pre-allocated in audioDeviceAboutToStart to the
        // device's buffer size; JUCE's AudioDeviceManager splits a larger
        // driver block to that size before the hub sees it, so this guard
        // is a backstop: skip rather than allocate on the audio thread.
        if (numSamples > scratchBuffer.getNumSamples()) return;
        scratchBuffer.clear(0, numSamples);
        juce::AudioSourceChannelInfo info(&scratchBuffer, 0, numSamples);

        // Neither of our locks here.  The transport's own callback lock,
        // which setSource() also takes to swap the chain, keeps the chain
        // alive for this call; the reader source is destroyed only after
        // setSource(nullptr) has detached it.
        //
        // What this call can wait on (JUCE 9 source, AUDIT LTC-3):
        //  - AudioTransportSource::callbackLock, held for the whole call.
        //    Elsewhere it is held briefly: setSource's swap, and
        //    getCurrentPosition() from the message thread (engine tick).
        //  - ResamplingAudioSource::callbackLock (also taken by
        //    setPosition's flushBuffers, brief) and its ratio SpinLock
        //    (brief).
        //  - BufferingAudioSource::bufferRangeLock, which the read-ahead
        //    thread takes only for its bookkeeping.
        //  - BufferingAudioSource::callbackLock, taken here to copy from the
        //    read-ahead buffer whenever any of the block is buffered.  The
        //    read-ahead thread (backgroundThread) holds that same lock
        //    while it reads and decodes the next chunk of the file (up to
        //    2048 samples).  So a slow read -- a sleeping or network disk, a
        //    stalled decoder -- blocks this callback for as long as that
        //    chunk takes.  The hub calls its clients one after the other
        //    under its fan-out lock, so the stall reaches every client on
        //    the device, LTC out included; and the message thread's
        //    getCurrentPosition() then waits behind it.
        // The lock is inside JUCE's BufferingAudioSource; avoiding it means
        // replacing the read-ahead with our own lock-free one, a rewrite
        // rather than a fix.  Deferred (AUDIT LTC-3).
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

        // sourceLock: setSource reads the prepared rate and block size and
        // prepares the new chain with them, so it must not run halfway
        // through this on the LoaderThread.
        {
            const juce::ScopedLock sl(sourceLock);
            transport.prepareToPlay(currentBufferSize, currentSampleRate);
        }

        // A start while we are open is a restart under us (AUD11
        // GENERATOR-8); the announcement our own openDevice gets from the
        // hub comes before deviceOpen is set.  Posted, as this can be the
        // driver's thread; a copy goes to callAsync, as for onLoadCompleted.
        if (deviceOpen.load(std::memory_order_relaxed) && onDeviceRestarted)
            juce::MessageManager::callAsync(std::function<void()>(onDeviceRestarted));
    }

    void audioDeviceStopped() override
    {
        if (shuttingDown.load(std::memory_order_acquire)) return;

        const juce::ScopedLock sl(sourceLock);
        transport.releaseResources();
    }

    //==========================================================================
    juce::AudioFormatManager  formatManager;
    std::shared_ptr<juce::AudioFormatManager> diskFormats { std::make_shared<juce::AudioFormatManager>() };   // the loads' readers (DiskJob)
    juce::AudioThumbnailCache thumbnailCache { 32 };  // cache the last 32 files' peaks so navigating presets back-and-forth does not re-decode each time
    juce::AudioThumbnail      thumbnail      { 512, formatManager, thumbnailCache };
    juce::TimeSliceThread     backgroundThread { "STC Generator Audio Reader" };

    juce::CriticalSection                          sourceLock;     // transport chain: setSource / prepare / release / setPosition / start (see the class comment)
    juce::CriticalSection                          transportLock;  // the state below and loadError; held only for assignments
    std::unique_ptr<juce::AudioFormatReaderSource> currentReaderSource;
    juce::AudioTransportSource                     transport;
    juce::File   currentFile;
    juce::int64  currentFileSize = 0, currentFileModTime = 0;   // as the load found them (AUD11 GENERATOR-7); under transportLock
    juce::String trackArtist, trackTitle;   // under transportLock
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

    // Lock-free mirrors for hot accessors.  The waveform views repaint at
    // 30 Hz and query hasFileLoaded() / getFileLengthSeconds() each time,
    // and seekSeconds() reads the length; none of them takes a lock for it.
    std::atomic<bool>   fileLoadedAtomic    { false };
    std::atomic<double> fileLengthAtomic    { 0.0 };

    // Set by LoaderThread::request (requestLoad), cleared by the
    // LoaderThread once the last queued request is done.  Read by play(),
    // seekSeconds() and stopAndReset(), which leave the transport alone
    // while a load is in flight, and by the audio callback, which plays
    // silence then -- the onLoadCompleted callback handles the start with
    // the engine's current playhead so the audio does not race ahead of the
    // cursor during a hot-swap.
    std::atomic<bool>   pendingLoad         { false };

    // Set with pendingLoad when an onLoadCompleted listener is wired,
    // cleared by the first seekSeconds() or play() that reaches the
    // transport afterwards -- the listener's catch-up (TimecodeEngine seeks
    // to its playhead), or the owner's next start.  The audio callback
    // plays silence while it is set (AUD11 GENERATOR-10).  pendingLoad
    // ends when the LoaderThread has settled the request, and the catch-up
    // runs on the message thread a moment later.  A newly attached file is
    // stopped in between, but a request for the file that is already
    // loaded leaves the transport as stopAndReset() left it, "playing" at
    // the top: GO on the loaded preset then sounded the top of the file
    // for every device block that fell in that moment (up to 186 ms each
    // at 8192 samples), before the catch-up moved it -- to the A/B loop's
    // In, when GO starts there.  Not set without a listener: then nobody
    // seeks, and attachReaderToTransport starts the file itself.
    std::atomic<bool>   awaitingSeek        { false };

    juce::String currentDeviceName, currentTypeName;
    int    numChannelsAvailable = 0;
    double currentSampleRate    = 0.0;
    int    currentBufferSize    = 0;

    juce::String loadError;          // human-readable last-load-error message; under transportLock

    // LoaderThread (loadFile): the last request named a file that is not on
    // disk.  See isRequestedFileMissing().
    std::atomic<bool> requestedFileMissing { false };

    // Set by the destructor, on the message thread, where the waveform
    // hand-overs posted by postWaveform also run: one still queued when the
    // player goes finds it set.
    std::shared_ptr<std::atomic<bool>> destroyed { std::make_shared<std::atomic<bool>>(false) };

    // LoaderThread (loadFile): the message for getLoadError().
    void setLoadError(const juce::String& message)
    {
        const juce::ScopedLock sl(transportLock);
        loadError = message;
    }

    // LoaderThread (loadFile): does the reader state a frame that can be
    // read (AUD11 FUZZ-14)?  JUCE's WAV and AIFF readers convert through a
    // block of 5,760 bytes and take from the file as many whole frames as
    // fit in it at a time.  A frame larger than the block is none at a
    // time, and their read loop never ends: a 40-byte AIFF whose COMM chunk
    // states a sample size of tens of thousands of bits, or a WAV stating
    // 3000 channels, loaded as a valid file and left the read-ahead thread
    // and the thumbnail's thread spinning, after which the next preset
    // change or the quit never came back.  JUCE's readers accept such a
    // header, so the bounds are set here: 8 to 64 bits a sample and 1 to
    // 256 channels, a frame of at most 2,048 bytes.  Files an audio program
    // writes are far inside them (the transport plays the first two
    // channels of a file with more, as before).  Under 8 bits there is
    // nothing to lose -- JUCE's AIFF reader gives silence for such a file --
    // and a damaged FLAC header stating 1 bit a sample sent JUCE's FLAC
    // reader round the same way.
    static bool hasSaneFrame(const juce::AudioFormatReader& reader)
    {
        return reader.numChannels   >= 1 && reader.numChannels   <= 256
            && reader.bitsPerSample >= 8 && reader.bitsPerSample <= 64;
    }

    // A reader the player takes: it has samples, at a rate, in frames that
    // can be read.  Anything else is EMPTY OR CORRUPT FILE.
    static bool isUsable(const juce::AudioFormatReader& reader)
    {
        return reader.sampleRate > 0.0 && reader.lengthInSamples > 0 && hasSaneFrame(reader);
    }

    // The reader source the transport plays: JUCE's, which stops asking
    // the disk once the player is being destroyed (AUD11 GENERATOR-9).  The
    // read-ahead thread reads through it, and a load's setSource waits
    // until that thread has filled a quarter of a second -- so does the
    // destructor, which waits for the load.  With the reads given up, that
    // wait ends with the one read in flight; the silence returned instead
    // goes to a transport that is about to be taken down.
    struct ReaderSource : public juce::AudioFormatReaderSource
    {
        ReaderSource(juce::AudioFormatReader* reader, const std::atomic<bool>& playerIsGoing)
            : juce::AudioFormatReaderSource(reader, true), playerGoing(playerIsGoing) {}

        void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override
        {
            if (playerGoing.load(std::memory_order_acquire))
            {
                info.clearActiveBufferRegion();
                return;
            }
            juce::AudioFormatReaderSource::getNextAudioBlock(info);
        }

        const std::atomic<bool>& playerGoing;   // the player's shuttingDown
    };

    // LoaderThread (loadFile): run `job` on a thread of its own and wait
    // for its answer.  False when the player is being destroyed: the job is
    // abandoned (it and the thread it runs on end by themselves; they hold
    // nothing of the player) and the load is not to go on -- nor is it
    // after an answer that arrives while the player is going, since the
    // swap that follows could wait for the disk again (AUD11 GENERATOR-9).
    // If no thread can be started, the job runs here, as it did before.
    // A detached std::thread, as StageLinQDbClient's connect has: with
    // juce::Thread::launch a job that is over at once (a file that is not
    // there) can delete its Thread object while the launching thread is
    // still inside startThread, holding that object's lock.
    bool askDisk(const std::shared_ptr<DiskJob>& job)
    {
        try
        {
            std::thread([job] { job->run(); }).detach();
        }
        catch (const std::system_error&)
        {
            job->run();
        }
        while (! job->done.wait(20))
        {
            if (loaderThread.threadShouldExit())
            {
                job->abandoned.store(true, std::memory_order_release);
                return false;
            }
        }
        return ! loaderThread.threadShouldExit();
    }

    // LoaderThread (loadFile): a load that ends with no file -- the message,
    // nothing loaded, and no waveform.
    bool failLoad(const juce::String& message, WaveformChange& waveform)
    {
        setLoadError(message);
        unloadFile();
        waveform.reader.reset();
        waveform.changed = true;
        return false;
    }

    // LoaderThread, after a request is settled: give the thumbnail what the
    // load opened for it (AUD11 GENERATOR-4).  AudioThumbnail belongs to the
    // message thread, so the swap is posted there; it is given a reader that
    // is already open (setReader), where setSource would open the file and
    // read its header on that thread.
    //
    // Replacing a thumbnail's source waits for the thumbnail's own thread
    // (the cache's TimeSliceThread) to finish the block of the previous
    // file it is reading.  That wait is taken here first, by taking the
    // previous source off that thread, so that the message thread does not
    // wait for a disk: the swap then finds nothing in flight.  (What is
    // left: two loads completing within one turn of the message loop can
    // let the first one's thumbnail start reading before the second swap,
    // which then waits for one block of a file that has just loaded.)
    void postWaveform(WaveformChange& waveform)
    {
        if (! waveform.changed) return;
        thumbnailCache.getTimeSliceThread().removeAllClients();
        // callAsync wants something copyable, and the reader must not leak
        // if the message is never delivered (the application quitting).
        auto reader = std::make_shared<std::unique_ptr<juce::AudioFormatReader>>(std::move(waveform.reader));
        juce::MessageManager::callAsync([this, gone = destroyed, reader, hash = waveform.hash]
        {
            if (! gone->load(std::memory_order_acquire))
                thumbnail.setReader(reader->release(), hash);   // no reader: cleared
        });
    }

    // Must be the LAST member declared.  Its destructor stops the thread
    // before any other member is torn down, so the thread cannot race
    // against the destructor of transport / currentReaderSource / thumbnail.
    LoaderThread loaderThread { *this };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GeneratorAudioPlayer)
};
