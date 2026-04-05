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
// pattern and parsed arguments.  Supports int32 (i), float32 (f), and
// string (s) argument types.
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
        enum Type { Int, Float, String };
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
            if (a.type == Arg::Float) return (int)a.floatVal;
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
        availableInterfaces = ::getNetworkInterfaces();
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

        bool bound = false;
        if (interfaceIndex > 0 && (interfaceIndex - 1) < availableInterfaces.size())
        {
            selectedInterface = interfaceIndex;
            bindIp = availableInterfaces[interfaceIndex - 1].ip;
            bound = socket->bindToPort(port, bindIp);
        }
        else
        {
            selectedInterface = 0;
            bindIp = "0.0.0.0";
            bound = socket->bindToPort(port);
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

            juce::String senderIp;
            int senderPort = 0;
            int bytesRead = sock->read(buf, sizeof(buf), false, senderIp, senderPort);

            if (bytesRead > 0)
            {
                Message msg;
                if (parseOscMessage(buf, bytesRead, msg) && onMessage)
                    onMessage(msg);
            }
        }
    }

    //--------------------------------------------------------------------------
    // OSC 1.0 parser
    //--------------------------------------------------------------------------
    static bool parseOscMessage(const uint8_t* data, int size, Message& msg)
    {
        if (size < 4 || data[0] != '/') return false;

        int pos = 0;

        // Address string (null-terminated, padded to 4 bytes)
        int addrEnd = findNull(data, pos, size);
        if (addrEnd < 0) return false;
        msg.address = juce::String::fromUTF8(reinterpret_cast<const char*>(data + pos), addrEnd - pos);
        pos = padTo4(addrEnd + 1);

        // Type tag string (starts with ',')
        if (pos >= size) return true;  // no args
        if (data[pos] != ',') return true;  // no type tag = no args

        int tagEnd = findNull(data, pos, size);
        if (tagEnd < 0) return true;
        juce::String typeTags = juce::String::fromUTF8(
            reinterpret_cast<const char*>(data + pos + 1), tagEnd - pos - 1);  // skip ','
        pos = padTo4(tagEnd + 1);

        // Parse arguments
        for (int i = 0; i < typeTags.length(); ++i)
        {
            if (pos >= size) break;
            char tag = (char)typeTags[i];

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
                    arg.strVal = juce::String::fromUTF8(
                        reinterpret_cast<const char*>(data + pos), strEnd - pos);
                    pos = padTo4(strEnd + 1);
                    break;
                }

                default:
                    return true;  // unknown type — stop parsing args
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
    std::atomic<bool> running { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OscInputServer)
};
