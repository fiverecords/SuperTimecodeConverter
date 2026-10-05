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
            std::atomic_thread_fence(std::memory_order_acquire);
            if (frameSeq.load(std::memory_order_relaxed) == before)
                return f;
        }
    }

    /// Freewheel (AUDIT D10): how long after the last frame/packet the source still
    /// counts as present.  The senders count on their own through it, so a
    /// short dropout -- a USB stall, a display wake -- never reaches the
    /// wire; the price is that a real stop takes this long to reach the
    /// outputs.  The operator sets it (engine setting), default
    /// kSourceTimeoutMs.
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
        const uint32_t seq = frameSeq.load(std::memory_order_relaxed);
        frameSeq.store(seq + 1u, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        lastPacketTime.store(arrivalMs, std::memory_order_relaxed);
        packedTimecode.store(packTimecode(hours, minutes, seconds, frames),
                             std::memory_order_relaxed);
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
    // Sequence lock over packedTimecode and lastPacketTime: odd while the
    // receive thread is storing a frame (getLastFrame).
    std::atomic<uint32_t> frameSeq { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LANetTimecodeInput)
};
