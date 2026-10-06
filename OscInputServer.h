// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <functional>
#include "NetworkUtils.h"

//==============================================================================
// OscInputServer -- Listens for incoming OSC messages on a UDP port.
//
// Parses OSC 1.0 packets and dispatches to a callback with the address
// pattern and parsed arguments.  Reads int32 (i), float32 (f), and string
// (s) arguments; the other OSC 1.0 type tags are skipped by their size and
// kept as Arg::Other so the positions stay right.  A #bundle is unpacked,
// nested bundles included, and its messages dispatched at once, in order
// (the time tag is ignored).  No address pattern matching: the address is
// compared as written.
//
// Usage:
//   OscInputServer osc;
//   osc.onMessage = [](const OscInputServer::Message& msg) { ... };
//   osc.start(7001);
//   ...
//   osc.stop();
//==============================================================================
class OscInputServer : public juce::Thread
{
public:
    //--------------------------------------------------------------------------
    // Parsed OSC argument
    //--------------------------------------------------------------------------
    struct Arg
    {
        enum Type { Int, Float, String, Other };   // Other: a type STC does not read (T, h, b, ...)
        Type type = Int;
        int32_t     intVal   = 0;
        float       floatVal = 0.0f;
        juce::String strVal;
    };

    //--------------------------------------------------------------------------
    // Parsed OSC message
    //--------------------------------------------------------------------------
    struct Message
    {
        juce::String address;       // e.g. "/stc/gen/play"
        std::vector<Arg> args;

        int getInt(int index, int def = 0) const
        {
            if (index < 0 || index >= (int)args.size()) return def;
            auto& a = args[(size_t)index];
            if (a.type == Arg::Int)   return a.intVal;
            if (a.type == Arg::Float)   // a NaN or an out-of-range float made the cast undefined
                return std::isfinite(a.floatVal)
                         ? (int) juce::jlimit(-2147483648.0, 2147483647.0, (double) a.floatVal)
                         : def;
            if (a.type == Arg::String) return a.strVal.getIntValue();
            return def;
        }

        float getFloat(int index, float def = 0.0f) const
        {
            if (index < 0 || index >= (int)args.size()) return def;
            auto& a = args[(size_t)index];
            if (a.type == Arg::Float) return a.floatVal;
            if (a.type == Arg::Int)   return (float)a.intVal;
            return def;
        }

        juce::String getString(int index, const juce::String& def = {}) const
        {
            if (index < 0 || index >= (int)args.size()) return def;
            auto& a = args[(size_t)index];
            if (a.type == Arg::String) return a.strVal;
            if (a.type == Arg::Int)    return juce::String(a.intVal);
            if (a.type == Arg::Float)  return juce::String(a.floatVal);
            return def;
        }
    };

    //--------------------------------------------------------------------------
    // Callback: called on the listener thread. Route to message thread if needed.
    //--------------------------------------------------------------------------
    std::function<void(const Message&)> onMessage;

    //--------------------------------------------------------------------------
    OscInputServer() : Thread("OSC Input") {}

    ~OscInputServer() override { stop(); }

    void refreshNetworkInterfaces()
    {
        availableInterfaces = ::getNetworkInterfaces(true);   // software protocol: localhost too (#20)
    }

    juce::StringArray getInterfaceNames() const
    {
        juce::StringArray names;
        names.add("All interfaces");
        for (auto& ni : availableInterfaces)
            names.add(ni.name + " (" + ni.ip + ")");
        return names;
    }

    int getInterfaceCount() const { return availableInterfaces.size() + 1; }
    int getSelectedInterface() const { return selectedInterface; }
    juce::String getBindInfo() const { return bindIp + ":" + juce::String(listenPort); }

    bool start(int port, int interfaceIndex = 0)
    {
        stop();

        listenPort = port;
        socket = std::make_unique<juce::DatagramSocket>(false);

        // On macOS and Linux a selected interface is all interfaces plus a
        // filter -- sent to its address, or from its subnet -- so its
        // broadcasts arrive (bindInputSocket, AUDIT NET-2); Windows binds
        // the interface's address, with no fallback.
        bool bound = false;
        bool fellBack = false;
        if (interfaceIndex > 0 && (interfaceIndex - 1) < availableInterfaces.size())
        {
            selectedInterface = interfaceIndex;
            bindIp = availableInterfaces[interfaceIndex - 1].ip;
            bound = bindInputSocket(*socket, port, &availableInterfaces.getReference(interfaceIndex - 1),
                                    false, sourceFilter, fellBack);
        }
        else
        {
            selectedInterface = 0;
            bindIp = "0.0.0.0";
            bound = bindInputSocket(*socket, port, nullptr, false, sourceFilter, fellBack);
        }

        if (!bound)
        {
            DBG("OscInputServer: failed to bind to port " + juce::String(port));
            socket = nullptr;
            return false;
        }

        running.store(true, std::memory_order_relaxed);
        startThread();
        DBG("OscInputServer: listening on " + bindIp + ":" + juce::String(port));
        return true;
    }

    void stop()
    {
        running.store(false, std::memory_order_relaxed);
        // With the interface filter in use the receive thread reads the
        // socket's descriptor itself (readInputDatagram), outside JUCE's
        // read/shutdown lock: it is ended before the descriptor is closed,
        // as ArtnetInput::stop() does (AUDIT NET-2).  ::shutdown() without
        // the close wakes its wait at once on Linux.  Message thread.
        if (sourceFilter.isActive() && isThreadRunning())
        {
           #ifndef _WIN32
            if (socket != nullptr && socket->getRawSocketHandle() >= 0)
                ::shutdown(socket->getRawSocketHandle(), SHUT_RDWR);
           #endif
            stopThread(1000);
        }
        if (socket) socket->shutdown();
        if (isThreadRunning()) stopThread(1000);
        socket = nullptr;
    }

    bool getIsRunning() const { return running.load(std::memory_order_relaxed); }
    int  getListenPort() const { return listenPort; }

private:
    void run() override
    {
        uint8_t buf[2048];

        while (!threadShouldExit() && running.load(std::memory_order_relaxed))
        {
            auto* sock = socket.get();
            if (!sock) break;

            if (!sock->waitUntilReady(true, 100))
                continue;

            // A datagram the interface filter drops reads as 0 bytes
            // (readInputDatagram, AUDIT NET-2).
            int bytesRead = readInputDatagram(*sock, buf, sizeof(buf), sourceFilter);

            if (bytesRead > 0 && onMessage)
                parsePacket(buf, bytesRead, [this](const Message& msg) { onMessage(msg); });
        }
    }

    //--------------------------------------------------------------------------
    // OSC 1.0 parser
    //--------------------------------------------------------------------------
    static constexpr int kMaxBundleDepth = 8;

    /// One datagram: a message, or a bundle.  `emit` gets every message, in
    /// order.  A bundle is "#bundle\0", an 8-byte time tag, then elements of
    /// int32 size + content, each a message or a bundle; the time tag is
    /// ignored and the contents dispatched at once (STC acts on arrival).
    /// Each size is checked against what is left, and the first one that
    /// does not fit ends the bundle; nesting deeper than kMaxBundleDepth is
    /// dropped.  Receive thread (and the fuzz harness).
    template <typename Emit>
    static void parsePacket(const uint8_t* data, int size, Emit&& emit, int depth = 0)
    {
        if (size >= 16 && std::memcmp(data, "#bundle", 8) == 0)   // 8 bytes: the NUL too
        {
            if (depth >= kMaxBundleDepth)
                return;
            int pos = 16;   // past "#bundle\0" and the time tag
            while (size - pos >= 4)
            {
                const int32_t elementSize = readInt32BE(data + pos);
                pos += 4;
                if (elementSize <= 0 || elementSize > size - pos)
                    return;
                parsePacket(data + pos, elementSize, emit, depth + 1);
                pos += elementSize;
            }
            return;
        }

        Message msg;
        if (parseOscMessage(data, size, msg))
            emit(msg);
    }

    static bool parseOscMessage(const uint8_t* data, int size, Message& msg)
    {
        if (size < 4 || data[0] != '/') return false;

        int pos = 0;

        // Address string (null-terminated, padded to 4 bytes)
        int addrEnd = findNull(data, pos, size);
        if (addrEnd < 0) return false;
        msg.address = stringFromWire(data + pos, addrEnd - pos);
        pos = padTo4(addrEnd + 1);

        // Type tag string (starts with ',')
        if (pos >= size) return true;  // no args
        if (data[pos] != ',') return true;  // no type tag = no args

        int tagEnd = findNull(data, pos, size);
        if (tagEnd < 0) return true;
        const uint8_t* typeTags = data + pos + 1;   // skip ','
        const int numTags = tagEnd - pos - 1;
        pos = padTo4(tagEnd + 1);

        // Parse arguments.  Each case checks that its own data is there, so
        // a tag with no data (T, F, N, I) is read even at the very end.
        for (int i = 0; i < numTags; ++i)
        {
            const char tag = (char) typeTags[i];

            Arg arg;
            switch (tag)
            {
                case 'i':
                    if (pos + 4 > size) return true;
                    arg.type = Arg::Int;
                    arg.intVal = readInt32BE(data + pos);
                    pos += 4;
                    break;

                case 'f':
                    if (pos + 4 > size) return true;
                    arg.type = Arg::Float;
                    arg.floatVal = readFloat32BE(data + pos);
                    pos += 4;
                    break;

                case 's':
                {
                    int strEnd = findNull(data, pos, size);
                    if (strEnd < 0) return true;
                    arg.type = Arg::String;
                    arg.strVal = stringFromWire(data + pos, strEnd - pos);
                    pos = padTo4(strEnd + 1);
                    break;
                }

                // The rest of OSC 1.0's types are skipped by their size and
                // kept as Other.  Stopping at the first of them, as before,
                // dropped every argument after it (AUDIT WIRE-10).
                case 'T': case 'F': case 'N': case 'I':   // true, false, nil, impulse: no data
                    arg.type = Arg::Other;
                    break;

                case 'h': case 't': case 'd':             // int64, time tag, double
                    if (pos + 8 > size) return true;
                    arg.type = Arg::Other;
                    pos += 8;
                    break;

                case 'c': case 'r': case 'm':             // char, RGBA, MIDI
                    if (pos + 4 > size) return true;
                    arg.type = Arg::Other;
                    pos += 4;
                    break;

                case 'S':                                 // symbol: laid out as a string
                {
                    int strEnd = findNull(data, pos, size);
                    if (strEnd < 0) return true;
                    arg.type = Arg::Other;
                    pos = padTo4(strEnd + 1);
                    break;
                }

                case 'b':                                 // blob: int32 size, bytes, padding
                {
                    if (pos + 4 > size) return true;
                    const int32_t blobSize = readInt32BE(data + pos);
                    if (blobSize < 0 || blobSize > size - pos - 4) return true;
                    arg.type = Arg::Other;
                    pos = padTo4(pos + 4 + blobSize);
                    break;
                }

                default:
                    return true;  // a type whose size is unknown: stop parsing args
            }
            msg.args.push_back(std::move(arg));
        }

        return true;
    }

    static int findNull(const uint8_t* data, int start, int size)
    {
        for (int i = start; i < size; ++i)
            if (data[i] == 0) return i;
        return -1;
    }

    static int padTo4(int pos) { return (pos + 3) & ~3; }

    static int32_t readInt32BE(const uint8_t* p)
    {
        return (int32_t)((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16
                       | (uint32_t)p[2] << 8  | (uint32_t)p[3]);
    }

    static float readFloat32BE(const uint8_t* p)
    {
        int32_t i = readInt32BE(p);
        float f;
        std::memcpy(&f, &i, 4);
        return f;
    }

    std::unique_ptr<juce::DatagramSocket> socket;
    int listenPort = 9800;
    int selectedInterface = 0;
    juce::String bindIp { "0.0.0.0" };
    juce::Array<NetworkInterface> availableInterfaces;
    SubnetFilter sourceFilter;   // set in start(), read by run() (macOS/Linux, AUDIT NET-2)
    std::atomic<bool> running { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OscInputServer)
};
