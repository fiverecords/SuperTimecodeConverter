// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

#pragma once
#include <JuceHeader.h>

//==============================================================================
// OscSender -- Lightweight OSC message sender over UDP.
//
// Builds and sends OSC-formatted UDP packets.
// Supports: int32 (i), float32 (f), string (s) argument types.
//
// Usage:
//   OscSender osc;
//   osc.connect("127.0.0.1", 53000);
//   osc.send("/cue/1/go");                       // no args
//   osc.send("/track/change", "i:42 s:Strobe");  // typed args
//==============================================================================
class OscSender
{
public:
    OscSender() = default;
    ~OscSender() { disconnect(); }

    //--------------------------------------------------------------------------
    // Connection
    //--------------------------------------------------------------------------
    bool connect(const juce::String& ip, int port)
    {
        juce::SpinLock::ScopedLockType lock(socketLock);
        socket.reset();
        connected = false;

        destIp = ip;
        destPort = port;

        socket = std::make_unique<juce::DatagramSocket>(false);
        // Bind to any local port (ephemeral)
        if (!socket->bindToPort(0))
        {
            socket.reset();
            return false;
        }
        connected = true;
        return true;
    }

    void disconnect()
    {
        juce::SpinLock::ScopedLockType lock(socketLock);
        socket.reset();
        connected = false;
    }

    bool isConnected() const
    {
        juce::SpinLock::ScopedLockType lock(socketLock);
        return connected;
    }

    void setDestination(const juce::String& ip, int port)
    {
        juce::SpinLock::ScopedLockType lock(socketLock);
        destIp = ip;
        destPort = port;
    }

    //--------------------------------------------------------------------------
    // Send an OSC message
    //--------------------------------------------------------------------------

    /// Send with pre-parsed args string: "i:42 s:hello f:3.14"
    /// Each token is "type:value" separated by spaces.
    /// Supported types: i (int32), f (float32), s (string)
    bool send(const juce::String& address, const juce::String& argsString = {})
    {
        if (address.isEmpty()) return false;

        // Build the packet outside the lock (CPU-only, no shared state)
        juce::MemoryBlock packet;

        // 1. Write address pattern (null-terminated, padded to 4 bytes)
        writeOscString(packet, address);

        // 2. Parse args and build type tag + arg data
        juce::MemoryBlock argData;
        juce::String typeTags = ",";

        if (argsString.isNotEmpty())
        {
            auto tokens = juce::StringArray::fromTokens(argsString, " ", "\"");
            for (auto& token : tokens)
            {
                if (token.isEmpty()) continue;

                if (token.length() < 3 || token[1] != ':')
                {
                    DBG("OscSender: skipping malformed arg token: " + token);
                    continue;
                }

                auto typeChar = static_cast<char>(token[0]);
                auto value = token.substring(2);

                switch (typeChar)
                {
                    case 'i':
                    {
                        typeTags += "i";
                        int32_t val = (int32_t)value.getIntValue();
                        writeInt32(argData, val);
                        break;
                    }
                    case 'f':
                    {
                        typeTags += "f";
                        float val = value.getFloatValue();
                        writeFloat32(argData, val);
                        break;
                    }
                    case 's':
                    {
                        typeTags += "s";
                        // Strip surrounding quotes if present (used for values with spaces)
                        if (value.startsWithChar('"') && value.endsWithChar('"') && value.length() >= 2)
                            value = value.substring(1, value.length() - 1);
                        writeOscString(argData, value);
                        break;
                    }
                    default:
                        DBG("OscSender: unknown arg type '" + juce::String::charToString(typeChar)
                            + "' in token: " + token);
                        break;  // Unknown type -- skip
                }
            }
        }

        // 3. Write type tag string
        writeOscString(packet, typeTags);

        // 4. Append arg data
        packet.append(argData.getData(), argData.getSize());

        // 5. Send under lock (protects socket pointer against concurrent disconnect)
        juce::SpinLock::ScopedLockType lock(socketLock);
        if (!connected || !socket) return false;
        return socket->write(destIp, destPort,
                             packet.getData(), (int)packet.getSize()) > 0;
    }

    /// Convenience: send with a single int32 argument
    bool sendInt(const juce::String& address, int32_t value)
    {
        return send(address, "i:" + juce::String(value));
    }

    /// Convenience: send with a single float argument
    bool sendFloat(const juce::String& address, float value)
    {
        return send(address, "f:" + juce::String(value));
    }

    /// Convenience: send with a single string argument
    bool sendString(const juce::String& address, const juce::String& value)
    {
        return send(address, "s:\"" + value + "\"");
    }

private:
    mutable juce::SpinLock socketLock;       // protects socket + connected + dest
    std::unique_ptr<juce::DatagramSocket> socket;
    juce::String destIp = "127.0.0.1";
    int destPort = 53000;
    bool connected = false;

    //--------------------------------------------------------------------------
    // OSC encoding helpers
    //--------------------------------------------------------------------------

    /// Write a null-terminated string padded to 4-byte boundary
    static void writeOscString(juce::MemoryBlock& block, const juce::String& s)
    {
        auto utf8 = s.toRawUTF8();
        size_t len = std::strlen(utf8) + 1;             // include null terminator
        size_t padded = (len + 3) & ~(size_t)3;         // round up to 4-byte boundary
        block.append(utf8, len);
        // Pad with zeros
        while (len < padded)
        {
            block.append("\0", 1);
            ++len;
        }
    }

    /// Write a big-endian int32
    static void writeInt32(juce::MemoryBlock& block, int32_t val)
    {
        uint8_t bytes[4];
        bytes[0] = (uint8_t)((val >> 24) & 0xFF);
        bytes[1] = (uint8_t)((val >> 16) & 0xFF);
        bytes[2] = (uint8_t)((val >> 8) & 0xFF);
        bytes[3] = (uint8_t)(val & 0xFF);
        block.append(bytes, 4);
    }

    /// Write a big-endian float32 (IEEE 754)
    static void writeFloat32(juce::MemoryBlock& block, float val)
    {
        static_assert(sizeof(float) == 4, "float must be 4 bytes");
        int32_t asInt;
        std::memcpy(&asInt, &val, 4);
        writeInt32(block, asInt);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OscSender)
};
