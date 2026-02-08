#pragma once
#include <JuceHeader.h>
#include "TimecodeCore.h"

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <iphlpapi.h>
    #pragma comment(lib, "iphlpapi.lib")
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <ifaddrs.h>
    #include <net/if.h>
    #include <arpa/inet.h>
#endif

struct NetworkInterface
{
    juce::String name;
    juce::String ip;
    juce::String broadcast;
    juce::String subnet;
};

class ArtnetOutput : public juce::Timer
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
    static juce::Array<NetworkInterface> getNetworkInterfaces()
    {
        juce::Array<NetworkInterface> interfaces;

#ifdef _WIN32
        ULONG bufSize = 15000;
        PIP_ADAPTER_ADDRESSES addresses = nullptr;
        ULONG result = 0;

        for (int attempts = 0; attempts < 3; attempts++)
        {
            addresses = (PIP_ADAPTER_ADDRESSES)malloc(bufSize);
            if (addresses == nullptr) return interfaces;

            result = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, nullptr, addresses, &bufSize);
            if (result == ERROR_BUFFER_OVERFLOW)
            {
                free(addresses);
                addresses = nullptr;
                continue;
            }
            break;
        }

        if (result != NO_ERROR || addresses == nullptr)
        {
            if (addresses) free(addresses);
            return interfaces;
        }

        for (auto* adapter = addresses; adapter != nullptr; adapter = adapter->Next)
        {
            if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
                continue;
            if (adapter->OperStatus != IfOperStatusUp)
                continue;

            for (auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr; unicast = unicast->Next)
            {
                if (unicast->Address.lpSockaddr->sa_family == AF_INET)
                {
                    auto* addr = (sockaddr_in*)unicast->Address.lpSockaddr;
                    char ipStr[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &addr->sin_addr, ipStr, sizeof(ipStr));

                    ULONG prefixLength = unicast->OnLinkPrefixLength;
                    uint32_t ip = ntohl(addr->sin_addr.s_addr);
                    uint32_t mask = (prefixLength > 0) ? (~0u << (32 - prefixLength)) : 0;
                    uint32_t broadcast = ip | ~mask;

                    char broadcastStr[INET_ADDRSTRLEN];
                    struct in_addr broadcastAddr;
                    broadcastAddr.s_addr = htonl(broadcast);
                    inet_ntop(AF_INET, &broadcastAddr, broadcastStr, sizeof(broadcastStr));

                    char maskStr[INET_ADDRSTRLEN];
                    struct in_addr maskAddr;
                    maskAddr.s_addr = htonl(mask);
                    inet_ntop(AF_INET, &maskAddr, maskStr, sizeof(maskStr));

                    NetworkInterface ni;
                    ni.name = juce::String(adapter->FriendlyName);
                    ni.ip = juce::String(ipStr);
                    ni.broadcast = juce::String(broadcastStr);
                    ni.subnet = juce::String(maskStr);

                    interfaces.add(ni);
                }
            }
        }

        free(addresses);
#else
        struct ifaddrs* ifaddr;
        if (getifaddrs(&ifaddr) == -1)
            return interfaces;

        for (auto* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next)
        {
            if (ifa->ifa_addr == nullptr || ifa->ifa_addr->sa_family != AF_INET)
                continue;
            if (ifa->ifa_flags & IFF_LOOPBACK)
                continue;
            if (!(ifa->ifa_flags & IFF_UP))
                continue;

            auto* addr = (sockaddr_in*)ifa->ifa_addr;
            char ipStr[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &addr->sin_addr, ipStr, sizeof(ipStr));

            char broadcastStr[INET_ADDRSTRLEN] = "255.255.255.255";
            if (ifa->ifa_broadaddr)
            {
                auto* baddr = (sockaddr_in*)ifa->ifa_broadaddr;
                inet_ntop(AF_INET, &baddr->sin_addr, broadcastStr, sizeof(broadcastStr));
            }

            NetworkInterface ni;
            ni.name = juce::String(ifa->ifa_name);
            ni.ip = juce::String(ipStr);
            ni.broadcast = juce::String(broadcastStr);

            interfaces.add(ni);
        }

        freeifaddrs(ifaddr);
#endif

        return interfaces;
    }

    void refreshNetworkInterfaces()
    {
        availableInterfaces = getNetworkInterfaces();
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

        isRunning = true;
        paused = false;
        updateTimerRate();
        return true;
    }

    void stop()
    {
        stopTimer();
        isRunning = false;
        paused = false;

        if (socket != nullptr)
        {
            socket->shutdown();
            socket = nullptr;
        }
    }

    bool getIsRunning() const { return isRunning; }
    juce::String getBroadcastIp() const { return broadcastIp; }

    //==============================================================================
    void setTimecode(const Timecode& tc)
    {
        timecodeToSend = tc;
    }

    void setFrameRate(FrameRate fps)
    {
        if (currentFps != fps)
        {
            currentFps = fps;
            if (isRunning && !paused)
                updateTimerRate();
        }
    }

    void setEnabled(bool enabled)
    {
        outputEnabled = enabled;
    }

    // Pause/resume transmission
    void setPaused(bool shouldPause)
    {
        if (paused == shouldPause)
            return;

        paused = shouldPause;

        if (paused)
        {
            stopTimer();
        }
        else if (isRunning)
        {
            updateTimerRate();
        }
    }

    bool isPaused() const { return paused; }

private:
    void timerCallback() override
    {
        if (!isRunning || !outputEnabled || paused || socket == nullptr)
            return;

        sendArtTimeCode();
    }

    void sendArtTimeCode()
    {
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

        packet[10] = 0;
        packet[11] = 0;
        packet[12] = 0;
        packet[13] = 0;

        packet[14] = (uint8_t)timecodeToSend.frames;
        packet[15] = (uint8_t)timecodeToSend.seconds;
        packet[16] = (uint8_t)timecodeToSend.minutes;
        packet[17] = (uint8_t)timecodeToSend.hours;

        int rateCode = 0;
        switch (currentFps)
        {
            case FrameRate::FPS_24:   rateCode = 0; break;
            case FrameRate::FPS_25:   rateCode = 1; break;
            case FrameRate::FPS_2997: rateCode = 2; break;
            case FrameRate::FPS_30:   rateCode = 3; break;
        }
        packet[18] = (uint8_t)rateCode;

        socket->write(broadcastIp, destPort, packet, sizeof(packet));
    }

    void updateTimerRate()
    {
        double fps = frameRateToDouble(currentFps);
        int intervalMs = juce::jmax(1, (int)(1000.0 / fps));
        startTimer(intervalMs);
    }

    std::unique_ptr<juce::DatagramSocket> socket;
    juce::String broadcastIp = "255.255.255.255";
    juce::String bindIp = "0.0.0.0";
    int destPort = 6454;
    int selectedInterface = -1;
    bool isRunning = false;
    bool outputEnabled = true;
    bool paused = false;

    juce::Array<NetworkInterface> availableInterfaces;

    Timecode timecodeToSend;
    FrameRate currentFps = FrameRate::FPS_30;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ArtnetOutput)
};
