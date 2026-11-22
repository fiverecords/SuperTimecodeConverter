// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

// LANetTimecodeInput
// Copyright (c) 2026 LaserAnimation Sollinger GmbH, https://www.laseranimation.com
// Written by Ingo Randolf (based on ArtnetInput)

#pragma once
#include <JuceHeader.h>
#include "TimecodeCore.h"
#include "NetworkUtils.h"
#include <atomic>

class LANetTimecodeInput : public juce::Thread
{
public:
    LANetTimecodeInput()
        : Thread("LA-Net Input")
    {
    }

    ~LANetTimecodeInput() override
    {
        stop();
    }

    //==============================================================================
    void refreshNetworkInterfaces()
    {
        availableInterfaces = ::getNetworkInterfaces(true);   // software protocol: localhost too (#20)
    }

    juce::StringArray getInterfaceNames() const
    {
        juce::StringArray names;
        names.add("ALL INTERFACES (0.0.0.0)");
        for (auto& ni : availableInterfaces)
            names.add(ni.name + " (" + ni.ip + ")");
        return names;
    }

    int getInterfaceCount() const { return availableInterfaces.size() + 1; }

    juce::String getBindInfo() const { return bindIp + ":" + juce::String(listenPort); }
    bool didFallBackToAllInterfaces() const { return bindFellBack.load(std::memory_order_relaxed); }
    int getSelectedInterface() const { return selectedInterface; }

    //==============================================================================
    bool start(int interfaceIndex = 0, int port = 8201)
    {
        stop();

        listenPort = port;

        if (interfaceIndex > 0 && (interfaceIndex - 1) < availableInterfaces.size())
        {
            selectedInterface = interfaceIndex;
            bindIp = availableInterfaces[interfaceIndex - 1].ip;
        }
        else
        {
            selectedInterface = 0;
            bindIp = "0.0.0.0";
        }

        socket = std::make_unique<juce::DatagramSocket>(false);


        // Enable SO_REUSEADDR before binding
        auto rawSock = socket->getRawSocketHandle();
        if (rawSock >= 0)
        {
            const int flag = 1;
#ifdef _WIN32
            setsockopt(rawSock, SOL_SOCKET, SO_REUSEADDR,
                       (const char*)&flag, sizeof(flag));
#else
            setsockopt(rawSock, SOL_SOCKET, SO_REUSEADDR,
                       &flag, sizeof(flag));
#endif
        }


        // On macOS and Linux a selected interface is all interfaces plus a
        // filter -- sent to its address, or from its subnet -- so its
        // broadcasts arrive (bindInputSocket, AUDIT NET-2); Windows binds
        // the interface's address.
        bool fellBack = false;
        const bool bound = bindInputSocket(*socket, listenPort,
                                           selectedInterface > 0 ? &availableInterfaces.getReference(selectedInterface - 1) : nullptr,
                                           true, sourceFilter, fellBack);
        if (fellBack)
            bindIp = "0.0.0.0";   // reflect actual bind address

        bindFellBack.store(fellBack, std::memory_order_relaxed);


        if (bound)
        {
            isRunningFlag.store(true, std::memory_order_relaxed);
            startThread();
            return true;
        }

        socket = nullptr;
        return false;
    }

    void stop()
    {
        isRunningFlag.store(false, std::memory_order_relaxed);
        bindFellBack.store(false, std::memory_order_relaxed);

        // With the interface filter in use, the receive thread reads the
        // socket's descriptor itself (readInputDatagram), outside the lock
        // JUCE's own read() and shutdown() share.  So the thread is ended
        // first, and the descriptor closed after: it never reads one that
        // shutdown() has closed, or whose number has been given to another
        // socket meanwhile -- as ArtnetInput::stop() does (AUDIT NET-2).
        // ::shutdown() without the close wakes its wait at once on Linux
        // (on macOS it sees the flag within its 100 ms wait, as before).
        // Message thread; sourceFilter is only read here, as by the thread.
        if (sourceFilter.isActive() && isThreadRunning())
        {
           #ifndef _WIN32
            if (socket != nullptr && socket->getRawSocketHandle() >= 0)
                ::shutdown(socket->getRawSocketHandle(), SHUT_RDWR);
           #endif
            stopThread(1000);
        }

        if (socket != nullptr)
            socket->shutdown();

        if (isThreadRunning())
            stopThread(1000);

        socket = nullptr;
    }

    bool getIsRunning() const { return isRunningFlag.load(std::memory_order_relaxed); }
    int getListenPort() const { return listenPort; }

    //==============================================================================
    /// Arrival instant of the last valid timecode packet (hi-res ms), which
    /// the sender emits at its frame boundary -- so it is the start of the
    /// frame carried in the packet.  0.0 when nothing has arrived.
    double getLastFrameArrivalMs() const
    {
        return lastPacketTime.load(std::memory_order_relaxed);
    }

    /// The last valid frame as the receive thread stored it: the value and
    /// its arrival instant (see getLastFrameArrivalMs).  getCurrentTimecode()
    /// and getLastFrameArrivalMs() read one field each, and a packet stored
    /// between the two calls pairs a new value with an old arrival -- one
    /// frame off for one tick.  This reads them as one record (AUDIT LTC-8).
    struct ReceivedFrame
    {
        Timecode tc;
        double   arrivalMs = 0.0;   // 0 = nothing received yet
        int      repeats = 0;       // packets in a row that repeated the value before them (0-2)
    };

    /// Any thread.  A sequence lock: the writer (the receive thread, once
    /// per packet) never waits; a reader that overlaps a write reads again.
    ReceivedFrame getLastFrame() const
    {
        ReceivedFrame f;
        for (;;)
        {
            const uint32_t before = frameSeq.load(std::memory_order_acquire);
            if ((before & 1u) != 0)
            {
                juce::Thread::yield();   // a write is in progress
                continue;
            }
            f.tc        = unpackTimecode(packedTimecode.load(std::memory_order_relaxed));
            f.arrivalMs = lastPacketTime.load(std::memory_order_relaxed);
            f.repeats   = frameRepeats.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (frameSeq.load(std::memory_order_relaxed) == before)
                return f;
        }
    }

    /// What the engine publishes from this input: the live value, its phase
    /// inside the frame, and whether the source counts as present -- all
    /// from one snapshot and one clock reading, as LtcInput::getLiveTimecode
    /// (AUDIT LTC-10; the principle of DESIGN D29).
    struct LiveTimecode
    {
        Timecode tc;
        double   phaseMs = 0.0;
        bool     phaseValid = false;   // false until a packet has arrived
        bool     receiving = false;
    };

    /// `atMs` is the hi-res ms counter, `fps` the engine's rate.  Any thread.
    ///
    /// The sender emits a packet at its frame boundary, so the packet's
    /// arrival is the start of the frame it carries.  While packets keep
    /// coming the engine reads the value less than a frame later and it is
    /// the packet's own.  When they stop for a while inside the freewheel
    /// window -- a switch reconverging, a Wi-Fi gap -- the source still
    /// counts as present, and the value is aged by the whole frames elapsed
    /// since the arrival with the phase what remains: one measurement
    /// expressed two ways, as LtcInput and MtcInput do.  Before, the value
    /// stood still for the whole window with a phase that kept turning, and
    /// the outputs followed a reference that did not move: LTC OUT ran
    /// ahead and snapped back every four frames, MTC OUT stepped backwards,
    /// Art-Net and LA-Net OUT repeated one frame until the packets came back (AUD11
    /// TC-IO-3).
    ///
    /// A sender parked on a frame -- kHeldRepeats packets running that
    /// repeat the value before them -- is not aged: its packets say it is
    /// not moving.  The price of the aging is LtcInput's (AUDIT LTC-10): a
    /// sender that really stops is counted on through the window, and the
    /// value steps back to the last packet's when the window closes.
    ///
    /// While not receiving the value is the last packet's, as before.  The
    /// phase is the remainder in every case, which is what the engine
    /// published before.
    LiveTimecode getLiveTimecode(double atMs, FrameRate fps) const
    {
        LiveTimecode live;
        const ReceivedFrame f = getLastFrame();
        live.tc = f.tc;
        if (f.arrivalMs <= 0.0)
            return live;

        // A packet stored after the caller read the clock: it has just arrived.
        const double elapsed = juce::jmax(0.0, atMs - f.arrivalMs);
        live.receiving = elapsed < timeoutMs.load(std::memory_order_relaxed);   // isReceiving() on this snapshot

        const double rate = frameRateToDouble(fps);
        if (rate <= 0.0)
            return live;
        const double frameMs = 1000.0 / rate;
        const double frames = elapsed / frameMs;
        const double fl = std::floor(frames);
        live.phaseMs = (frames - fl) * frameMs;
        live.phaseValid = true;
        if (live.receiving && fl >= 1.0 && f.repeats < kHeldRepeats)
            live.tc = offsetTimecode(f.tc, (int) fl, fps);
        return live;
    }

    /// Freewheel (AUDIT D10): how long after the last frame/packet the source still
    /// counts as present.  The engine counts the value on through it
    /// (getLiveTimecode), so a short dropout -- a USB stall, a display wake
    /// -- never reaches the wire; the price is that a real stop takes this
    /// long to reach the outputs.  The operator sets it (engine setting),
    /// default kSourceTimeoutMs.
    void setTimeoutMs(double ms) { timeoutMs.store(juce::jmax(50.0, ms), std::memory_order_relaxed); }
    double getTimeoutMs() const  { return timeoutMs.load(std::memory_order_relaxed); }

    /// True while valid timecode packets keep arriving: the last one came
    /// within the freewheel window.
    bool isReceiving() const
    {
        double lpt = lastPacketTime.load(std::memory_order_relaxed);
        if (lpt == 0.0)
            return false;

        double now = juce::Time::getMillisecondCounterHiRes();
        double elapsed = now - lpt;

        // At 24fps a packet arrives every ~41ms, at 30fps ~33ms
        return elapsed < timeoutMs.load(std::memory_order_relaxed);
    }

    Timecode getCurrentTimecode() const
    {
        return unpackTimecode(packedTimecode.load(std::memory_order_relaxed));
    }
    FrameRate getDetectedFrameRate() const { return detectedFps.load(std::memory_order_relaxed); }

private:
    void run() override
    {
        uint8_t buffer[1024];

        while (!threadShouldExit() && isRunningFlag.load(std::memory_order_relaxed))
        {
            // Capture local pointer: stop() may nullify `socket` from another thread
            // after calling socket->shutdown().  The shutdown unblocks waitUntilReady,
            // and then the while-condition will fail on the next iteration.  The local
            // pointer ensures we don't dereference a null between the check and use.
            auto* sock = socket.get();
            if (sock == nullptr)
                break;

            // Wait up to 100ms for data -- allows periodic threadShouldExit() checks
            // so the thread can shut down cleanly even if no packets are arriving
            if (!sock->waitUntilReady(true, 100))
                continue;

            // A datagram the interface filter drops reads as 0 bytes
            // (readInputDatagram, AUDIT NET-2).
            int bytesRead = readInputDatagram(*sock, buffer, sizeof(buffer), sourceFilter);

            if (bytesRead == 28)
                parseLANetTimecodePacket(buffer, bytesRead);
        }
    }

    void parseLANetTimecodePacket(const uint8_t* data, int size)
    {
        if (size != 28)
        {
            // invalid data
            return;
        }


        // Seven big-endian 32-bit words, read byte by byte: the receive
        // buffer is a byte array, and reading it through a uint32_t pointer
        // assumed an alignment nothing guarantees (and broke aliasing).
        auto word = [data](int i) { return juce::ByteOrder::bigEndianInt(data + 4 * i); };

        // message type
        uint32_t message_type = word(0);

        if (message_type != 1)
        {
            return;
        }

        // version parsing
        uint32_t version = word(1);

        if (version != 1)
        {
            return;
        }

        uint32_t len = word(3);

        if (len != 12)
        {
            return;
        }


        uint32_t fps = word(4);

        if (fps == 0)
        {
            fps = 25;
        }

        uint32_t timestamp = word(6);

        if (timestamp == 0xffffffff)
        {
            return;
        }

        // check valid fps
        // ignore not supported fps: 50, 60, 100
        if (fps != 24 && fps != 25 && fps != 30)
        {
            // unsupported fps could be converted
            return;
        }

        int frames = timestamp % fps;
        timestamp -= frames;

        int seconds = (timestamp / fps) % 60;
        timestamp -= seconds * fps;

        int minutes = (timestamp / (60 * fps)) % 60;
        timestamp -= minutes * fps * 60;

        int hours = (timestamp / (60 * 60 * fps));

        // Validate ranges -- discard malformed packets
        // (lastPacketTime is updated AFTER validation so isReceiving()
        //  only returns true when we actually accepted valid data)
        if (hours > 23 || minutes > 59 || seconds > 59 || frames > 29)
        {
            return;
        }

        const double arrivalMs = juce::Time::getMillisecondCounterHiRes();

        switch (fps)
        {
            case 24: detectedFps.store(FrameRate::FPS_24, std::memory_order_relaxed);   break;
            case 25: detectedFps.store(FrameRate::FPS_25, std::memory_order_relaxed);   break;
            case 30: detectedFps.store(FrameRate::FPS_30, std::memory_order_relaxed);   break;
            default: break;
        }

        // Value and arrival go out as one record: the sequence is odd while
        // they are being stored, and a reader that saw it odd or changed
        // reads again (getLastFrame, AUDIT LTC-8).  Only this thread writes,
        // so it never waits.
        //
        // A value that repeats the packet before it is counted, up to
        // kHeldRepeats: a sender parked on a frame, which getLiveTimecode
        // must not count on (AUD11 TC-IO-3).  The first packet ever
        // repeats nothing.
        const uint64_t packed = packTimecode(hours, minutes, seconds, frames);
        const bool repeated = lastPacketTime.load(std::memory_order_relaxed) != 0.0
                           && packed == packedTimecode.load(std::memory_order_relaxed);
        valueRepeats = repeated ? juce::jmin(valueRepeats + 1, kHeldRepeats) : 0;

        const uint32_t seq = frameSeq.load(std::memory_order_relaxed);
        frameSeq.store(seq + 1u, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        lastPacketTime.store(arrivalMs, std::memory_order_relaxed);
        packedTimecode.store(packed, std::memory_order_relaxed);
        frameRepeats.store(valueRepeats, std::memory_order_relaxed);
        frameSeq.store(seq + 2u, std::memory_order_release);
    }

    std::unique_ptr<juce::DatagramSocket> socket;
    juce::String bindIp = "0.0.0.0";
    int listenPort = 8201;
    int selectedInterface = 0;
    std::atomic<bool> isRunningFlag { false };
    std::atomic<double> timeoutMs { kSourceTimeoutMs };   // freewheel window (AUDIT D10)
    std::atomic<bool> bindFellBack { false };

    juce::Array<NetworkInterface> availableInterfaces;
    SubnetFilter sourceFilter;   // set in start(), read by run() (macOS/Linux, AUDIT NET-2)
    std::atomic<double> lastPacketTime { 0.0 };

    std::atomic<uint64_t> packedTimecode { 0 };
    std::atomic<FrameRate> detectedFps { FrameRate::FPS_25 };
    // Sequence lock over packedTimecode, lastPacketTime and frameRepeats:
    // odd while the receive thread is storing a frame (getLastFrame).
    std::atomic<uint32_t> frameSeq { 0 };
    std::atomic<int> frameRepeats { 0 };         // ReceivedFrame::repeats
    int valueRepeats = 0;                        // receive thread only
    // Packets in a row repeating the value before them from which the
    // sender counts as parked on a frame, and the value is not aged
    // (getLiveTimecode); LtcInput's figure.
    static constexpr int kHeldRepeats = 2;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LANetTimecodeInput)
};
