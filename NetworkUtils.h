// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

#pragma once
#include <JuceHeader.h>
#include <vector>

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

//==============================================================================
// Enumerate active IPv4 network interfaces.
//
// includeLoopback appends "Localhost (127.0.0.1)" as the LAST entry, so the
// interface indices stored in settings keep their meaning.  Its "broadcast"
// address is 127.0.0.1 itself: a sender writing to it reaches every socket
// on this machine bound to that port (Resolume, a console emulator, another
// STC), which is what the software protocols -- TCNet, Art-Net, LA-Net,
// HippoNet, OSC -- need when the receiver runs on the same PC (#20).  The
// hardware protocols (Pro DJ Link, StageLinQ) do not ask for it.
//==============================================================================
inline juce::Array<NetworkInterface> getNetworkInterfaces(bool includeLoopback = false)
{
    juce::Array<NetworkInterface> interfaces;

    auto appendLoopback = [&]()
    {
        if (! includeLoopback) return;
        NetworkInterface lo;
        lo.name      = "Localhost (127.0.0.1)";
        lo.ip        = "127.0.0.1";
        lo.broadcast = "127.0.0.1";
        lo.subnet    = "255.0.0.0";
        interfaces.add(lo);
    };

#ifdef _WIN32
    ULONG bufSize = 15000;
    std::vector<uint8_t> buffer(bufSize);
    PIP_ADAPTER_ADDRESSES addresses = nullptr;
    ULONG result = 0;

    for (int attempts = 0; attempts < 3; attempts++)
    {
        buffer.resize(bufSize);
        addresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());

        result = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, nullptr, addresses, &bufSize);
        if (result == ERROR_BUFFER_OVERFLOW)
        {
            addresses = nullptr;
            continue;
        }
        break;
    }

    if (result != NO_ERROR || addresses == nullptr)
    {
        appendLoopback();
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
                uint32_t mask = (prefixLength == 0) ? 0u : (prefixLength >= 32) ? ~0u : (~0u << (32 - prefixLength));
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
#else
    struct ifaddrs* ifaddr;
    if (getifaddrs(&ifaddr) == -1)
    {
        appendLoopback();
        return interfaces;
    }

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

        char maskStr[INET_ADDRSTRLEN] = "255.255.255.0";
        if (ifa->ifa_netmask)
        {
            auto* maddr = (sockaddr_in*)ifa->ifa_netmask;
            inet_ntop(AF_INET, &maddr->sin_addr, maskStr, sizeof(maskStr));
        }

        NetworkInterface ni;
        ni.name = juce::String(ifa->ifa_name);
        ni.ip = juce::String(ipStr);
        ni.broadcast = juce::String(broadcastStr);
        ni.subnet = juce::String(maskStr);

        interfaces.add(ni);
    }

    freeifaddrs(ifaddr);
#endif

    appendLoopback();
    return interfaces;
}
