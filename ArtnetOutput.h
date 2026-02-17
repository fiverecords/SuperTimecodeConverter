// Super Timecode Converter
// Copyright (c) 2026 Fiverecords — MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

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

class ArtnetOutput : public juce::HighResolutionTimer
{
public:
    ArtnetOutput()
    {
        refreshNetworkInterfaces();
    }

    ~ArtnetOutput() override
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
    bool start(int interfaceIndex = -1, int targetPort = 6454)
    {
        stop();

        destPort = targetPort;

        if (interfaceIndex >= 0 && interfaceIndex < availableInterfaces.size())
        {
            selectedInterface = interfaceIndex;
            broadcastIp = availableInterfaces[interfaceIndex].broadcast;
            bindIp = availableInterfaces[interfaceIndex].ip;
        }
        else
        {
            selectedInterface = -1;
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

        if (shouldPause)
        {
            stopTimer();
        }
        else if (isRunningFlag.load(std::memory_order_relaxed))
        {
            lastFrameSendTime.store(juce::Time::getMillisecondCounterHiRes(), std::memory_order_relaxed);
            updateTimerRate();
        }
    }

    bool isPaused() const { return paused.load(std::memory_order_relaxed); }

private:
    void hiResTimerCallback() override
    {
        if (!isRunningFlag.load(std::memory_order_relaxed)
            || paused.load(std::memory_order_relaxed)
            || socket == nullptr)
            return;

        // Single atomic read — guarantees frame interval and packet rate code are consistent
        FrameRate fps = currentFps.load(std::memory_order_relaxed);

        // Fractional accumulator: compare real elapsed time against ideal frame interval
        // to eliminate drift caused by integer-ms timer resolution
        double now = juce::Time::getMillisecondCounterHiRes();
        double frameInterval = 1000.0 / frameRateToDouble(fps);

        // Allow up to 2 catch-up sends per callback to handle jitter
        int sent = 0;
        double lastSend = lastFrameSendTime.load(std::memory_order_relaxed);
        while ((now - lastSend) >= frameInterval && sent < 2)
        {
            sendArtTimeCode(fps);
            // Advance by ideal interval (not by 'now') to prevent cumulative drift
            lastSend += frameInterval;
            sent++;
        }
        lastFrameSendTime.store(lastSend, std::memory_order_relaxed);

        // If we fell too far behind (>100ms), reset to avoid a burst
        if ((now - lastSend) > 100.0)
            lastFrameSendTime.store(now, std::memory_order_relaxed);
    }

    void sendArtTimeCode(FrameRate fps)
    {
        Timecode tc;
        {
            const juce::SpinLock::ScopedLockType lock(tcLock);
            tc = timecodeToSend;
        }

        // Validate ranges -- don't send corrupt data to the network
        int maxFrames = frameRateToInt(fps);
        if (tc.hours > 23 || tc.minutes > 59 || tc.seconds > 59 || tc.frames >= maxFrames)
            return;

        uint8_t packet[19] = {};

        packet[0] = 'A';
        packet[1] = 'r';
        packet[2] = 't';
        packet[3] = '-';
        packet[4] = 'N';
        packet[5] = 'e';
        packet[6] = 't';
        packet[7] = 0;

        packet[8] = 0x00;
        packet[9] = 0x97;

        packet[10] = 0x00;  // ProtVer Hi (big-endian)
        packet[11] = 0x0E;  // ProtVer Lo = 14 (Art-Net 4 standard)
        packet[12] = 0;
        packet[13] = 0;

        packet[14] = (uint8_t)tc.frames;
        packet[15] = (uint8_t)tc.seconds;
        packet[16] = (uint8_t)tc.minutes;
        packet[17] = (uint8_t)tc.hours;

        packet[18] = (uint8_t)fpsToRateCode(fps);

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
    int destPort = 6454;
    int selectedInterface = -1;
    std::atomic<bool> isRunningFlag { false };
    std::atomic<bool> paused { false };

    juce::Array<NetworkInterface> availableInterfaces;

    juce::SpinLock tcLock;
    Timecode timecodeToSend;        // Written by UI thread under tcLock, read by timer thread under tcLock
    std::atomic<FrameRate> currentFps { FrameRate::FPS_25 };
    std::atomic<double> lastFrameSendTime { 0.0 };
    std::atomic<uint32_t> sendErrors { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ArtnetOutput)
};
