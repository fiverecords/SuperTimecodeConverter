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
    #include <netinet/in.h>
    #include <sys/socket.h>
    #include <cstring>
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
// address is 127.0.0.1 itself, so the software protocols -- TCNet, Art-Net,
// LA-Net, HippoNet, OSC -- reach a receiver running on the same PC (#20).
// That is a unicast, and it reaches ONE socket bound to the port, not every
// one: with several (Resolume and a console emulator, another STC, or STC's
// own TCNet discovery socket on 60000) the kernel picks -- on Linux the most
// specific bind, then the one opened last (net_localhost_sim; macOS and
// Windows untested, BENCH B36; AUDIT NET-3).  The hardware protocols (Pro DJ
// Link, StageLinQ) do not ask for it.
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

        // The broadcast address only on an interface that has one.  On a
        // point-to-point link (a VPN's utun or tun, ppp) the same field
        // holds the peer's address -- ifa_broadaddr and ifa_dstaddr share
        // storage -- and it was offered as the "broadcast" (AUDIT NET-16).
        // Every user of the field now sends to 255.255.255.255 on such a
        // link instead of to the peer: the Art-Net, LA-Net, TCNet and
        // HippoNet outputs, and ProDJLinkInput's keepalives, StageLinQInput's
        // discovery and HippotizerInput's announce.
        char broadcastStr[INET_ADDRSTRLEN] = "255.255.255.255";
        if ((ifa->ifa_flags & IFF_BROADCAST) != 0 && ifa->ifa_broadaddr != nullptr
            && ifa->ifa_broadaddr->sa_family == AF_INET)
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

//==============================================================================
// Inputs listening on one interface (Art-Net, LA-Net, OSC).
//
// Windows delivers broadcasts to a UDP socket bound to the interface's
// unicast address; macOS and Linux do not -- such a socket gets only what is
// sent to that address, and Art-Net and LA-Net timecode are broadcast (the
// same reason ProDJLinkInput binds its broadcast ports to INADDR_ANY outside
// Windows).  The bind succeeded, so nothing told the operator: silence.  On
// macOS and Linux an input therefore binds INADDR_ANY, has the kernel report
// each datagram's destination address (IP_RECVDSTADDR on macOS, IP_PKTINFO
// on Linux), and keeps a datagram that was sent to the interface's own
// address, from any sender as before (a controller behind a router or a
// VPN), or whose sender is on the interface's subnet, which is where its
// broadcasts come from (AUDIT NET-2).  Localhost has no broadcast to miss
// and keeps its own address; so does HippoNet, whose timecode is unicast
// (HippotizerInput.h).
//
// What it costs (Linux by the delivery rule measured on loopback, macOS by
// the BSD bind rules; neither tried with two interfaces; BENCH):
//  - Two inputs on one port on two different interfaces (two engines) both
//    want INADDR_ANY.  Linux gives it to both: a broadcast reaches both, a
//    unicast only the one bound last, which drops it if it was sent to the
//    other.  macOS refuses the second wildcard bind (SO_REUSEPORT would be
//    needed on both), so the second input binds its own address as before:
//    it gets its unicast and no broadcasts.
//  - macOS: while STC holds INADDR_ANY:port, another program that binds the
//    same port on INADDR_ANY after it fails; before, STC held only
//    <interface>:port and the other program could bind.  Started before
//    STC, the other program keeps the port and STC's input binds its own
//    address as before.  SO_REUSEPORT is not set: it helps only a program
//    that sets it too, and then the kernel hands each unicast to one of the
//    sockets, so the other program could take STC's (ProDJLinkInput keeps
//    it off its status socket for that reason).
//  - One input on an interface and another engine's on ALL INTERFACES,
//    same protocol and port: before, one held <interface>:port and the
//    other 0.0.0.0:port, and both received.  Now both want 0.0.0.0:port.
//    Linux: a unicast to the interface goes to whichever bound last
//    (nic_all_sim: the interface input lost every one when the ALL input
//    started after it).  macOS: the ALL input started second has no
//    fallback and fails to bind.
//  - A sender on another subnet of the same network that broadcasts to
//    255.255.255.255 is dropped by the filter on macOS and Linux (its
//    address is not on the interface's subnet); Windows receives it.
//  - The input now also hears STC's own broadcasts on that interface and
//    port -- Art-Net or LA-Net out of another engine, or of the same engine
//    -- as it already did on Windows and with ALL INTERFACES.  It cannot
//    tell them from another sender's (AUDIT NET-14: StreamId not checked).
//==============================================================================

/// A dotted-quad IPv4 address as a host-order integer; false if `text` is
/// not one.  Used when an input starts, not per datagram.
inline bool parseIPv4(const juce::String& text, uint32_t& out)
{
    juce::StringArray parts;
    parts.addTokens(text, ".", "");
    if (parts.size() != 4)
        return false;
    uint32_t value = 0;
    for (auto& part : parts)
    {
        if (part.isEmpty() || part.length() > 3 || ! part.containsOnly("0123456789"))
            return false;
        const int byte = part.getIntValue();
        if (byte > 255)
            return false;
        value = (value << 8) | (uint32_t) byte;
    }
    out = value;
    return true;
}

/// Keeps the datagrams sent to one interface's own address, or sent from its
/// subnet.  Inactive (accepts everything) until set() succeeds.  Set before
/// the receive thread starts and read only by it.
struct SubnetFilter
{
    /// False, leaving the filter inactive, when the interface has no subnet
    /// to match: an address or mask that does not parse, a 0.0.0.0 mask, or
    /// a /32 (a point-to-point link, whose peer is outside its own
    /// "subnet").
    bool set(const NetworkInterface& ni)
    {
        clear();
        uint32_t ip = 0, m = 0;
        if (! parseIPv4(ni.ip, ip) || ! parseIPv4(ni.subnet, m) || m == 0u || m == 0xFFFFFFFFu)
            return false;
        address = ip;
        mask = m;
        network = ip & m;
        return true;
    }

    void clear() { address = 0; network = 0; mask = 0; }
    bool isActive() const { return mask != 0; }

    /// Host-order addresses; `destination` is 0 when the socket did not
    /// report it, and then only the sender's subnet counts.
    bool accepts(uint32_t source, uint32_t destination) const
    {
        return ! isActive() || destination == address || (source & mask) == network;
    }

    uint32_t address = 0, network = 0, mask = 0;   // mask 0 = inactive
};

#ifndef _WIN32
/// Asks the kernel to report each datagram's destination address on this
/// socket (readInputDatagram reads it).  False if it cannot.
inline bool reportDestinationAddress(juce::DatagramSocket& socket)
{
    const int on = 1;
   #if defined(IP_RECVDSTADDR)     // macOS, BSD
    return setsockopt(socket.getRawSocketHandle(), IPPROTO_IP, IP_RECVDSTADDR, &on, sizeof(on)) == 0;
   #elif defined(IP_PKTINFO)       // Linux
    return setsockopt(socket.getRawSocketHandle(), IPPROTO_IP, IP_PKTINFO, &on, sizeof(on)) == 0;
   #else
    juce::ignoreUnused(socket, on);
    return false;
   #endif
}
#endif

/// Binds an input's socket to `port` on `ni` (nullptr: all interfaces), as
/// the block above describes.  Windows: the interface's address, falling
/// back to all interfaces if `allowFallback`.  macOS/Linux: all interfaces
/// with `filter` set to the interface; if the destination address cannot be
/// reported or that bind fails, or the interface has no subnet to filter on,
/// or it is Localhost (127.x.x.x), the interface's address as on Windows
/// (unicast to it still arrives).  `filter` is left inactive unless it is in
/// use.  Returns whether the socket is bound; `fellBack` says it is on all
/// interfaces after `ni`'s own address failed.  Called from the inputs'
/// start(), before their receive thread runs.
inline bool bindInputSocket(juce::DatagramSocket& socket, int port, const NetworkInterface* ni,
                            bool allowFallback, SubnetFilter& filter, bool& fellBack)
{
    filter.clear();
    fellBack = false;
    if (ni == nullptr)
        return socket.bindToPort(port);

   #ifndef _WIN32
    // Localhost keeps 127.0.0.1, as before: there is no broadcast to miss,
    // and on INADDR_ANY it would lose its unicast to another input on the
    // port (another engine on a network interface) bound after it.
    uint32_t address = 0;
    const bool loopback = parseIPv4(ni->ip, address) && (address >> 24) == 127u;
    if (! loopback && filter.set(*ni))
    {
        if (reportDestinationAddress(socket) && socket.bindToPort(port))
            return true;
        filter.clear();
    }
   #endif

    if (socket.bindToPort(port, ni->ip))
        return true;
    if (allowFallback && socket.bindToPort(port))
    {
        fellBack = true;
        return true;
    }
    return false;
}

/// Reads one waiting datagram from a socket bound by bindInputSocket, as
/// juce::DatagramSocket::read(buffer, size, false) does, and returns 0 for
/// one that `filter` drops.  With the filter inactive (Windows, ALL
/// INTERFACES, Localhost, a fallback) it is that call.  Otherwise it reads
/// with recvmsg to get the destination address, the one thing
/// DatagramSocket::read cannot give.  Receive thread, after waitUntilReady.
/// That read uses the raw descriptor without the lock DatagramSocket's
/// read() and shutdown() share (it is private), so with the filter active
/// the owner's stop() must end the receive thread before it shuts the
/// socket down (ArtnetInput::stop): otherwise a stop() between this taking
/// the descriptor and reading it closes it under the read, and its number
/// can meanwhile belong to another socket.
inline int readInputDatagram(juce::DatagramSocket& socket, void* buffer, int size,
                             const SubnetFilter& filter)
{
   #ifdef _WIN32
    juce::ignoreUnused(filter);   // never active on Windows (bindInputSocket)
   #else
    if (filter.isActive())
    {
        const int fd = socket.getRawSocketHandle();
        if (fd < 0)
            return -1;   // shut down, as read() says

        sockaddr_in from {};
        iovec iov {};
        iov.iov_base = buffer;
        iov.iov_len = (size_t) size;
        union { cmsghdr align; unsigned char bytes[64]; } control {};
        msghdr msg {};
        msg.msg_name = &from;
        msg.msg_namelen = (socklen_t) sizeof(from);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control.bytes;
        msg.msg_controllen = (decltype(msg.msg_controllen)) sizeof(control.bytes);

        const auto received = ::recvmsg(fd, &msg, MSG_DONTWAIT);
        if (received <= 0)
            return 0;

        uint32_t destination = 0;
        for (auto* c = CMSG_FIRSTHDR(&msg); c != nullptr; c = CMSG_NXTHDR(&msg, c))
        {
           #if defined(IP_RECVDSTADDR)
            if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_RECVDSTADDR)
            {
                in_addr dst {};
                std::memcpy(&dst, CMSG_DATA(c), sizeof(dst));
                destination = ntohl(dst.s_addr);
            }
           #elif defined(IP_PKTINFO)
            if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_PKTINFO)
            {
                in_pktinfo info {};
                std::memcpy(&info, CMSG_DATA(c), sizeof(info));
                destination = ntohl(info.ipi_addr.s_addr);
            }
           #endif
        }

        return filter.accepts(ntohl(from.sin_addr.s_addr), destination) ? (int) received : 0;
    }
   #endif
    return socket.read(buffer, size, false);
}

//==============================================================================
/// Text received from the network (an OSC address or string, a HippoNet
/// machine name) as a juce::String: UTF-8 when the bytes up to the first
/// NUL, at most numBytes, are valid UTF-8; otherwise each byte as one
/// character (ISO 8859-1).  juce::String's constructors assert on what they
/// cannot decode -- 8-bit text in (const char*, size_t), malformed UTF-8 in
/// fromUTF8 -- and decode it as mojibake in release builds; nothing from the
/// wire reaches them unchecked this way.
inline juce::String stringFromWire(const void* data, int numBytes)
{
    auto* p = static_cast<const char*>(data);
    if (p == nullptr || numBytes <= 0)
        return {};
    int len = 0;
    while (len < numBytes && p[len] != 0)
        ++len;
    if (juce::CharPointer_UTF8::isValidString(p, len))
        return juce::String::fromUTF8(p, len);
    juce::String latin1;
    latin1.preallocateBytes((size_t) len * 2);
    for (int i = 0; i < len; ++i)
        latin1 += (juce::juce_wchar) (uint8_t) p[i];
    return latin1;
}
