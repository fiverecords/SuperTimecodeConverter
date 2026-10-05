// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

#pragma once
#include <JuceHeader.h>
#include "TimecodeCore.h"
#include "NetworkUtils.h"
#include <atomic>

class ArtnetInput : public juce::Thread
{
public:
    ArtnetInput()
        : Thread("ArtNet Input")
    {
    }

    ~ArtnetInput() override
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
    bool start(int interfaceIndex = 0, int port = 6454)
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

        bool bound = false;
        bool fellBack = false;
        if (bindIp != "0.0.0.0")
            bound = socket->bindToPort(listenPort, bindIp);
        if (!bound)
        {
            bound = socket->bindToPort(listenPort);
            if (bound)
            {
                fellBack = (bindIp != "0.0.0.0");  // only a fallback if we tried a specific IP
                bindIp = "0.0.0.0";   // reflect actual bind address
            }
        }
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

    /// Freewheel (D10): how long after the last frame/packet the source still
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

            int bytesRead = sock->read(buffer, sizeof(buffer), false);

            if (bytesRead >= 19)
                parseArtNetPacket(buffer, bytesRead);
        }
    }

    void parseArtNetPacket(const uint8_t* data, int size)
    {
        if (size < 19)
            return;

        if (data[0] != 'A' || data[1] != 'r' || data[2] != 't' ||
            data[3] != '-' || data[4] != 'N' || data[5] != 'e' ||
            data[6] != 't' || data[7] != 0)
            return;

        uint16_t opcode = (uint16_t)((uint16_t)data[8] | ((uint16_t)data[9] << 8));
        if (opcode != 0x9700)
            return;

        // ProtVer is big-endian (Hi byte at offset 10, Lo at 11)
        // Art-Net 4 requires ProtVer >= 14; accept anything >= 14 for compatibility
        uint16_t protVer = (uint16_t)(((uint16_t)data[10] << 8) | (uint16_t)data[11]);
        if (protVer < 14)
            return;

        int frames  = data[14];
        int seconds = data[15];
        int minutes = data[16];
        int hours   = data[17];
        int rateCode = data[18] & 0x03;

        // Art-Net 4 spec: bits 2-7 of the Type field are reserved and must be 0.
        // Log a warning if they are non-zero (malformed sender), but still
        // process the packet -- the frame-rate bits 0-1 remain valid.
        if ((data[18] & 0xFC) != 0)
        {
            DBG("ArtTimeCode: reserved bits in Type field are non-zero (0x"
                + juce::String::toHexString(data[18]) + "). Packet may be malformed.");
        }

        // Validate ranges -- discard malformed packets
        // (lastPacketTime is updated AFTER validation so isReceiving()
        //  only returns true when we actually accepted valid data)
        if (hours > 23 || minutes > 59 || seconds > 59 || frames > 29)
            return;

        const double arrivalMs = juce::Time::getMillisecondCounterHiRes();

        switch (rateCode)
        {
            case 0:
                // Art-Net rate code 0 means "24fps".  Like MTC, Art-Net has
                // no dedicated code for 23.976, so if the user has already
                // selected FPS_2398 we preserve it rather than silently
                // overwriting with FPS_24.
                if (detectedFps.load(std::memory_order_relaxed) != FrameRate::FPS_2398)
                    detectedFps.store(FrameRate::FPS_24, std::memory_order_relaxed);
                break;
            case 1: detectedFps.store(FrameRate::FPS_25, std::memory_order_relaxed);   break;
            case 2: detectedFps.store(FrameRate::FPS_2997, std::memory_order_relaxed); break;
            case 3: detectedFps.store(FrameRate::FPS_30, std::memory_order_relaxed);   break;
            default: break;  // mask guarantees 0-3, but be explicit
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
    int listenPort = 6454;
    int selectedInterface = 0;
    std::atomic<bool> isRunningFlag { false };
    std::atomic<double> timeoutMs { kSourceTimeoutMs };   // freewheel window (D10)
    std::atomic<bool> bindFellBack { false };

    juce::Array<NetworkInterface> availableInterfaces;
    std::atomic<double> lastPacketTime { 0.0 };

    std::atomic<uint64_t> packedTimecode { 0 };
    std::atomic<FrameRate> detectedFps { FrameRate::FPS_25 };
    // Sequence lock over packedTimecode and lastPacketTime: odd while the
    // receive thread is storing a frame (getLastFrame).
    std::atomic<uint32_t> frameSeq { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ArtnetInput)
};
