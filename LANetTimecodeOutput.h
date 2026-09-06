// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

// LANetTimecodeOutput
// Copyright (c) 2026 LaserAnimation Sollinger GmbH, https://www.laseranimation.com
// Written by Ingo Randolf (based on ArtnetOutput)


#pragma once
#include <JuceHeader.h>
#include "TimecodeCore.h"
#include "NetworkUtils.h"
#include <atomic>

#ifdef _WIN32
    #include <winsock2.h>
#else
    #include <sys/socket.h>
#endif

static const uint8_t laNetPacketHeader[24] = {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 12, 0, 0, 0, 0, 0, 0, 0, 0};

class LANetTimecodeOutput : public juce::HighResolutionTimer
{
public:
    LANetTimecodeOutput()
    {
        refreshNetworkInterfaces();
    }

    ~LANetTimecodeOutput() override
    {
        stop();
    }

    //==============================================================================
    void refreshNetworkInterfaces()
    {
        availableInterfaces = ::getNetworkInterfaces();
    }

    juce::StringArray getInterfaceNames() const
    {
        juce::StringArray names;
        for (auto& ni : availableInterfaces)
            names.add(ni.name + " (" + ni.ip + ")");
        return names;
    }

    int getInterfaceCount() const { return availableInterfaces.size(); }

    juce::String getInterfaceInfo(int index) const
    {
        if (index >= 0 && index < availableInterfaces.size())
            return availableInterfaces[index].ip + " -> " + availableInterfaces[index].broadcast;
        return "";
    }

    //==============================================================================
    bool start(int interfaceIndex = -1, int targetPort = 8201)
    {
        stop();

        destPort = targetPort;

        if (interfaceIndex >= 0 && interfaceIndex < availableInterfaces.size())
        {
            selectedInterface = interfaceIndex;
            broadcastIp = availableInterfaces[interfaceIndex].broadcast;
            bindIp = availableInterfaces[interfaceIndex].ip;
        }
        else if (interfaceIndex == -1)
        {
            selectedInterface = -1;
            broadcastIp = "127.0.0.1";
            bindIp = "127.0.0.1";
        }
        else
        {
            selectedInterface = -2;
            broadcastIp = "255.255.255.255";
            bindIp = "0.0.0.0";
        }


        socket = std::make_unique<juce::DatagramSocket>(false);

        if (!socket->bindToPort(0, bindIp))
        {
            if (!socket->bindToPort(0))
            {
                socket = nullptr;
                return false;
            }
        }

        // Enable SO_BROADCAST so the OS allows sending to broadcast addresses.
        // Some systems (especially Linux) reject broadcast sends without this.
        auto rawSock = socket->getRawSocketHandle();
        if (rawSock >= 0)
        {
            int broadcastFlag = 1;
#ifdef _WIN32
            setsockopt(rawSock, SOL_SOCKET, SO_BROADCAST,
                       (const char*)&broadcastFlag, sizeof(broadcastFlag));
#else
            setsockopt(rawSock, SOL_SOCKET, SO_BROADCAST,
                       &broadcastFlag, sizeof(broadcastFlag));
#endif
        }

        isRunningFlag.store(true, std::memory_order_relaxed);
        paused.store(false, std::memory_order_relaxed);
        sendErrors.store(0, std::memory_order_relaxed);
        seeded = false;
        updateTimerRate();
        return true;
    }

    void stop()
    {
        stopTimer();
        isRunningFlag.store(false, std::memory_order_relaxed);
        paused.store(false, std::memory_order_relaxed);

        if (socket != nullptr)
        {
            socket->shutdown();
            socket = nullptr;
        }
    }

    bool getIsRunning() const { return isRunningFlag.load(std::memory_order_relaxed); }
    juce::String getBroadcastIp() const { return broadcastIp; }
    int getSelectedInterface() const { return selectedInterface; }
    uint32_t getSendErrors() const { return sendErrors.load(std::memory_order_relaxed); }

    //==============================================================================
    void setTimecode(const Timecode& tc)
    {
        const juce::SpinLock::ScopedLockType lock(tcLock);
        timecodeToSend = tc;
    }

    // Called from UI thread.  startTimer() is internally serialised in JUCE's
    // HighResolutionTimer, so calling it from the message thread is safe.
    void setFrameRate(FrameRate fps)
    {
        auto prev = currentFps.load(std::memory_order_relaxed);
        if (prev != fps)
        {
            currentFps.store(fps, std::memory_order_relaxed);
            if (isRunningFlag.load(std::memory_order_relaxed) && !paused.load(std::memory_order_relaxed))
                updateTimerRate();
        }
    }

    // Pause/resume transmission
    void setPaused(bool shouldPause)
    {
        if (paused.load(std::memory_order_relaxed) == shouldPause)
            return;

        paused.store(shouldPause, std::memory_order_relaxed);

        std::cout << "set paused: " << shouldPause << std::endl;

        if (shouldPause)
        {
            stopTimer();
        }
        else if (isRunningFlag.load(std::memory_order_relaxed))
        {
            seeded = false;
            lastFrameSendTime.store(juce::Time::getMillisecondCounterHiRes(), std::memory_order_relaxed);
            updateTimerRate();
        }
    }

    bool isPaused() const { return paused.load(std::memory_order_relaxed); }

    /// Force immediate ArtTimeCode frame send.
    /// Call on seek/hot cue/track change so receivers update instantly
    /// instead of waiting for the next timer tick (up to 1 frame latency).
    void forceResync()
    {
        if (!isRunningFlag.load(std::memory_order_relaxed)
            || paused.load(std::memory_order_relaxed)
            || socket == nullptr)
            return;
        seeded = false;
        FrameRate fps = currentFps.load(std::memory_order_relaxed);
        sendLANetTimecode(fps);
    }


private:
    void hiResTimerCallback() override
    {
        if (!isRunningFlag.load(std::memory_order_relaxed)
            || paused.load(std::memory_order_relaxed)
            || socket == nullptr)
        {
            stopTimer();   // Don't spin at 1000Hz when there's nothing to send
            return;
        }

        // Single atomic read -- guarantees frame interval and packet rate code are consistent
        FrameRate fps = currentFps.load(std::memory_order_relaxed);

        // Fractional accumulator: compare real elapsed time against ideal frame interval
        // to eliminate drift caused by integer-ms timer resolution
        double now = juce::Time::getMillisecondCounterHiRes();
        // LaserAnimation Net TimeCode is a digital protocol -- always send at nominal frame rate.
        // The timecode VALUES advance slower at low pitch (PLL handles that),
        // producing repeated frames, which is correct. Scaling the interval
        // caused receivers to lose sync or stop at low pitch.
        double frameInterval = 1000.0 / frameRateToDouble(fps);

        // Allow up to 2 catch-up sends per callback to handle jitter
        int sent = 0;
        double lastSend = lastFrameSendTime.load(std::memory_order_relaxed);
        while ((now - lastSend) >= frameInterval && sent < 2)
        {
            sendLANetTimecode(fps);
            // Advance by ideal interval (not by 'now') to prevent cumulative drift
            lastSend += frameInterval;
            sent++;
        }
        lastFrameSendTime.store(lastSend, std::memory_order_relaxed);

        // If we fell too far behind (>100ms), reset to avoid a burst
        if ((now - lastSend) > 100.0)
            lastFrameSendTime.store(now, std::memory_order_relaxed);
    }

    void sendLANetTimecode(FrameRate fps)
    {
        Timecode pending;
        {
            const juce::SpinLock::ScopedLockType lock(tcLock);
            pending = timecodeToSend;
        }

        // Auto-increment: advance by 1 frame per send.  Compare with
        // pendingTimecode and only resync on diff > 1 (seek/jump).
        // Prevents 1-frame backward jitter from interpolation overshoot.
        // Same architectural pattern as the LTC, MTC and ArtNet encoders.
        Timecode tc;
        if (!seeded)
        {
            tc = pending;
            seeded = true;
        }
        else
        {
            tc = incrementFrame(encoderTc, fps);

            int maxFrames = frameRateToInt(fps);
            auto toTotal = [maxFrames](const Timecode& t) -> int64_t {
                return (int64_t)t.hours * 3600 * maxFrames
                     + (int64_t)t.minutes * 60 * maxFrames
                     + (int64_t)t.seconds * maxFrames
                     + (int64_t)t.frames;
            };
            int64_t dayFrames = (int64_t)24 * 3600 * maxFrames;
            int64_t rawDiff = toTotal(pending) - toTotal(tc);
            int64_t diff = ((rawDiff % dayFrames) + dayFrames) % dayFrames;
            if (diff > dayFrames / 2) diff = dayFrames - diff;
            if (diff > 1)
                tc = pending;
        }
        encoderTc = tc;

        // Validate ranges -- don't send corrupt data to the network
        int maxFrames = frameRateToInt(fps);
        if (tc.hours > 23 || tc.minutes > 59 || tc.seconds > 59 || tc.frames >= maxFrames)
            return;


        uint8_t packet[28];

        std::memcpy(packet, laNetPacketHeader, sizeof(laNetPacketHeader));

        // timecode format
        packet[19] = uint8_t(maxFrames);

        // endian conversion
        uint32_t be = juce::ByteOrder::swapIfLittleEndian((int32_t(tc.hours) * 3600 * maxFrames)
                                                    + (int32_t(tc.minutes) * 60 * maxFrames)
                                                    + (int32_t(tc.seconds) * maxFrames)
                                                    + int32_t(tc.frames));

        std::memcpy(&packet[24], &be, sizeof(uint32_t));


        int written = socket->write(broadcastIp, destPort, packet, sizeof(packet));        
        if (written < 0)
            sendErrors.fetch_add(1, std::memory_order_relaxed);
    }

    void updateTimerRate()
    {
        // Run timer at 1ms fixed rate -- the fractional accumulator in
        // hiResTimerCallback handles exact frame timing to avoid drift
        lastFrameSendTime.store(juce::Time::getMillisecondCounterHiRes(), std::memory_order_relaxed);
        startTimer(1);
    }

    std::unique_ptr<juce::DatagramSocket> socket;
    juce::String broadcastIp = "255.255.255.255";
    juce::String bindIp = "0.0.0.0";
    int destPort = 8201;
    int selectedInterface = -1;
    std::atomic<bool> isRunningFlag { false };
    std::atomic<bool> paused { false };

    juce::Array<NetworkInterface> availableInterfaces;

    juce::SpinLock tcLock;
    Timecode timecodeToSend;        // Written by UI thread under tcLock, read by timer thread under tcLock
    Timecode encoderTc;             // Auto-increment: last sent timecode (timer thread only)
    bool     seeded = false;  // Auto-increment: false until first frame seeds encoderTc
    std::atomic<FrameRate> currentFps { FrameRate::FPS_25 };
    std::atomic<double> lastFrameSendTime { 0.0 };
    std::atomic<uint32_t> sendErrors { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LANetTimecodeOutput)
};
