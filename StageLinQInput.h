// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter
//
// StageLinQInput -- Denon StageLinQ protocol implementation.
//
// Connects to Denon Engine OS hardware (SC5000, SC6000, Prime 4/2/Go,
// X1800, X1850) via the StageLinQ network protocol.  Receives deck state,
// track metadata, mixer fader positions, and beat info.
//
// Protocol overview:
//   1. Discovery: UDP broadcast on port 51337 ("airD" magic, both sides)
//   2. Service request: TCP to the device's advertised port -> device replies with services
//   3. StateMap: TCP subscription to key/value paths (UTF-16BE + JSON)
//   4. BeatInfo: TCP binary stream (beat/totalBeats/BPM per deck)
//
// Protocol references (all MIT licensed):
//   - chrisle/StageLinq (TypeScript) -- most complete implementation
//   - icedream/go-stagelinq (Go) -- clean C-like reference
//   - Jaxc/PyStageLinQ (Python) -- byte-level protocol documentation
//
// NOTE: This implementation is based entirely on the open-source reverse-
// engineering work cited above.  No Denon hardware was available during
// development.  Extensive logging is included for Wireshark-assisted
// debugging when hardware becomes available.
//
// Two field captures have been read since: a PRIME 4+ on Engine OS 5.0.4,
// from issue #23 (replayed by tools/audit/slq_capture_replay.cpp;
// DESIGN D35).  They set the playback speed (from BeatInfo, not
// /Engine/DeckN/Speed), the 0 - 1.27 range of its faders and crossfader,
// ExternalMixerVolume as the on-air signal, Track/TrackLength in samples,
// and the FileTransfer messages the unit sends unasked.  Everything else
// here is still unconfirmed on Denon hardware.
//
// One device at a time (AUDIT SLQ-3): decks map to STC decks 1-4 by the
// device's /Client/Preferences/Player -- 1 (a PRIME 4+ sends "1") to 1-4,
// 2 to 3-4 -- so two units that both say Player 1 (two PRIME units), or
// SC6000 players 3 and 4, write into the same decks in turn.  The speed
// tracker, isReceiving(), getPlayerModel() and the database client (the
// first device to offer FileTransfer) are not per device either.

#pragma once
#include <JuceHeader.h>
#include "TimecodeCore.h"
#include "NetworkUtils.h"
#include <atomic>
#include <array>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <map>
#include <set>
#include <vector>
#include <functional>

//==============================================================================
// Protocol constants
//==============================================================================
namespace StageLinQ
{
    // Discovery UDP port -- all StageLinQ devices broadcast here
    static constexpr int kDiscoveryPort = 51337;

    // Magic bytes for discovery frames
    static constexpr uint8_t kDiscoveryMagic[4] = { 'a', 'i', 'r', 'D' };

    // Magic bytes for StateMap messages
    static constexpr uint8_t kSmaaMagic[4] = { 0x73, 0x6D, 0x61, 0x61 };  // "smaa"

    // StateMap sub-types (bytes 8-11 inside smaa block)
    static constexpr uint32_t kSmaaStateEmit       = 0x00000000;  // device -> us: state value
    static constexpr uint32_t kSmaaEmitResponse     = 0x000007D1;  // device -> us: subscription ack
    // 0x07D2 is used in both directions:
    //  - us -> device: subscribe request, with min interval (ms) appended after the path
    //  - device -> us: periodic catalogue announcement of the paths the device offers
    //    (no JSON value, just path + 4-byte interval).  Some firmware revisions
    //    re-broadcast their catalogue on this opcode every few seconds; we
    //    silently consume them since we already know what we asked for.
    static constexpr uint32_t kSmaaSubscribe        = 0x000007D2;  // bidirectional, see above

    // TCP message IDs (first 4 bytes of TCP messages)
    static constexpr uint32_t kMsgServiceAnnounce   = 0x00000000;
    static constexpr uint32_t kMsgReference         = 0x00000001;
    static constexpr uint32_t kMsgServiceRequest    = 0x00000002;

    // BeatInfo message types
    static constexpr uint32_t kBeatStartStream      = 0x00000000;
    static constexpr uint32_t kBeatStopStream       = 0x00000001;
    static constexpr uint32_t kBeatEmit             = 0x00000002;

    // Connection action strings
    static constexpr const char* kActionHowdy = "DISCOVERER_HOWDY_";
    static constexpr const char* kActionExit  = "DISCOVERER_EXIT_";

    // Token length in bytes
    static constexpr int kTokenLen = 16;

    // Our identity
    static constexpr const char* kOurDeviceName  = "SuperTimecodeConverter";
    static constexpr const char* kOurSwName       = "STC";
    static constexpr const char* kOurSwVersion    = "1.7.0";

    // Timing
    static constexpr double kDiscoveryInterval     = 1.0;   // seconds between our announcements
    static constexpr double kReferenceInterval     = 0.25;   // seconds between reference keepalives
    static constexpr double kReconnectDelay        = 3.0;   // seconds before reconnect attempt
    static constexpr int    kSocketTimeoutMs       = 2000;   // TCP read timeout
    static constexpr double kDeviceTimeoutSec      = 5.0;   // no discovery = device gone
    // How long stop() waits for a connection thread.  Its longest step that
    // cannot be interrupted is one TCP connect (kSocketTimeoutMs); every wait
    // besides is cut short by stopThread() or checks threadShouldExit()
    // (AUDIT SLQ-6: a 2 s connect plus a 500 ms sleep outlasted the 2 s
    // stopThread, which then killed the thread).  The database client's
    // start(), called on a connection thread, does not wait for its session.
    static constexpr int    kConnectionStopMs      = kSocketTimeoutMs + 2000;

    // Largest StateMap or BeatInfo block accepted from the wire; a longer
    // length field is taken for corruption (AUDIT WIRE-3).  A StateMap value
    // is a path and a short JSON string, a BeatInfo block 16 bytes plus 32
    // per deck: the largest in the #23 PRIME 4+ captures are 322 and 144.
    static constexpr uint32_t kMaxServiceBlock = 65536;

    // Maximum supported decks per device (Prime 4 has 4)
    static constexpr int kMaxDecksPerDevice = 4;

    // Maximum total decks (we map to STC players 1-4)
    static constexpr int kMaxDecks = 4;

    // Maximum mixer channels
    static constexpr int kMaxMixerChannels = 4;

    // Mixer positions.  A PRIME 4+ on Engine OS 5.0.4 reports its channel
    // faders and crossfader as 0 - 1.27, not 0 - 1 (#23 capture: top of
    // travel 1.2699999809, crossfader near the centre 0.56 - 0.60).  STC
    // keeps 1.0 as full scale until a position above kPositionOverrange
    // arrives, then uses 1.27 -- or the highest position seen, should a
    // device go further still.  The margin keeps a device whose top is a
    // float rounding of 1.0 at full scale 1.0.
    static constexpr double kExtendedPositionScale = 1.27;
    static constexpr double kPositionOverrange     = 1.01;

    // /Engine/DeckN/ExternalMixerVolume is the deck's level after the channel
    // fader AND the crossfader (#23 captures, PRIME 4+): 0 with the fader
    // down, 0 with the crossfader cutting the deck's side, 1 with the fader
    // up and the crossfader on its side, about half each near the centre.
    // A deck is off air at or below the level the engine's OFF AIR AT
    // setting names (DESIGN D34): SILENCE (the default) is exactly 0, which the
    // PRIME 4+ sends with the fader in the bottom 1-4 % of its travel or the
    // crossfader in the last 3-6 %; -80, -60 and -40 dB go off air earlier
    // (-60 dB, beta1's fixed value, around 13-20 % of the fader and 10-15 %
    // from the crossfader's end).
    static constexpr double kOnAirQuietLevels[4] = { 0.0, 1.0e-4, 1.0e-3, 1.0e-2 };
    inline double onAirQuietLevel(int step) { return kOnAirQuietLevels[juce::jlimit(0, 3, step)]; }

    // Playback speed from BeatInfo (see StageLinQInput::handleBeatInfo):
    // an estimate older than this is not used, and a pair of messages that
    // gives more than this speed is a jump (cue, hot cue, track load), not
    // playback.
    static constexpr double kBeatSpeedMaxAgeMs  = 500.0;
    static constexpr double kMaxPlausibleSpeed  = 4.0;

    //==========================================================================
    // Big-endian byte helpers
    //==========================================================================
    inline uint16_t readU16BE(const uint8_t* p) { return (uint16_t(p[0]) << 8) | p[1]; }
    inline uint32_t readU32BE(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16)
                                                       | (uint32_t(p[2]) << 8)  | p[3]; }
    inline uint64_t readU64BE(const uint8_t* p) {
        return (uint64_t(readU32BE(p)) << 32) | readU32BE(p + 4);
    }
    inline double readF64BE(const uint8_t* p) {
        uint64_t raw = readU64BE(p);
        double result;
        std::memcpy(&result, &raw, 8);
        return result;
    }

    inline void writeU16BE(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
    inline void writeU32BE(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16);
                                                      p[2] = uint8_t(v >> 8);  p[3] = uint8_t(v); }

    //==========================================================================
    // UTF-16BE string encoding/decoding
    //==========================================================================
    // Encode a string to UTF-16BE bytes (no length prefix).  A character
    // above U+FFFF becomes a surrogate pair (AUDIT WIRE-9: it was cut to its
    // low 16 bits).
    inline std::vector<uint8_t> encodeUTF16BE(const juce::String& str)
    {
        std::vector<uint8_t> result;
        result.reserve(str.getNumBytesAsUTF8() * 2);
        auto put = [&result](uint32_t unit)
        {
            result.push_back(uint8_t(unit >> 8));
            result.push_back(uint8_t(unit & 0xFF));
        };
        for (auto p = str.getCharPointer(); ! p.isEmpty();)
        {
            const uint32_t ch = (uint32_t)p.getAndAdvance();
            if (ch > 0xFFFF && ch <= 0x10FFFF)
            {
                put(0xD800 + ((ch - 0x10000) >> 10));
                put(0xDC00 + ((ch - 0x10000) & 0x3FF));
            }
            else
            {
                put(ch <= 0xFFFF ? ch : 0xFFFD);
            }
        }
        return result;
    }

    // Decode UTF-16BE bytes to a juce::String.  A surrogate pair becomes the
    // character it encodes, a lone surrogate U+FFFD (AUDIT WIRE-9: each half
    // was appended as a character of its own).  NUL code units are dropped,
    // as appending them did before.  The characters are collected first and
    // converted once: appending to a juce::String one character at a time
    // looks for its end on every append.
    inline juce::String decodeUTF16BE(const uint8_t* data, int byteLen)
    {
        const int numUnits = byteLen / 2;
        if (numUnits <= 0) return {};

        std::vector<juce::juce_wchar> chars;
        chars.reserve((size_t)numUnits + 1);
        for (int i = 0; i < numUnits; ++i)
        {
            const uint32_t unit = (uint32_t(data[2 * i]) << 8) | data[2 * i + 1];
            uint32_t ch = unit;
            if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < numUnits)
            {
                const uint32_t low = (uint32_t(data[2 * i + 2]) << 8) | data[2 * i + 3];
                if (low >= 0xDC00 && low <= 0xDFFF)
                {
                    ch = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
                    ++i;
                }
            }
            if (ch >= 0xD800 && ch <= 0xDFFF) ch = 0xFFFD;   // unpaired half
            if (ch != 0) chars.push_back((juce::juce_wchar)ch);
        }
        chars.push_back(0);
        return juce::String(juce::CharPointer_UTF32(chars.data()));
    }

    //==========================================================================
    // Write a network string: [length:u32BE][UTF-16BE bytes]
    //==========================================================================
    inline void appendNetworkString(std::vector<uint8_t>& buf, const juce::String& str)
    {
        auto encoded = encodeUTF16BE(str);
        uint8_t lenBytes[4];
        writeU32BE(lenBytes, (uint32_t)encoded.size());
        buf.insert(buf.end(), lenBytes, lenBytes + 4);
        buf.insert(buf.end(), encoded.begin(), encoded.end());
    }

    //==========================================================================
    // Read a network string from buffer at offset.
    // Returns the new offset, or -1 on error.
    //==========================================================================
    inline int readNetworkString(const uint8_t* data, int dataLen, int offset, juce::String& out)
    {
        if (offset < 0 || offset + 4 > dataLen) return -1;
        uint32_t strLen = readU32BE(data + offset);
        offset += 4;
        // Bound the wire length in unsigned arithmetic before casting: a
        // value >= 2^31 cast to int went negative and passed the check.
        if (strLen > (uint32_t)(dataLen - offset)) return -1;
        out = decodeUTF16BE(data + offset, (int)strLen);
        return offset + (int)strLen;
    }

    //==========================================================================
    // A number off the wire into an integer: NaN reads 0 and a value out of
    // range saturates (a JSON value of 1e300 was cast straight to int64_t or
    // int, which is undefined behaviour).
    inline int64_t toInt64Saturated(double d)
    {
        if (std::isnan(d)) return 0;
        if (d >= 9.2e18)  return INT64_MAX;
        if (d <= -9.2e18) return INT64_MIN;
        return (int64_t)d;
    }
    inline int toIntSaturated(double d)
    {
        if (std::isnan(d)) return 0;
        return (int)juce::jlimit((double)INT_MIN, (double)INT_MAX, d);
    }

    //==========================================================================
    // JSON value parser for StateMap values
    //
    // StageLinQ StateMap values are NOT simple primitives. They are JSON
    // objects with named fields (confirmed from chrisle/StageLinq Player.ts):
    //
    //   {"string": "Artist Name"}     -- text values (artist, title, paths)
    //   {"state": true}               -- boolean values (play, songLoaded)
    //   {"value": 128.0}              -- numeric values (BPM, volume, pitch)
    //   {"color": 12345}              -- color values (jog colors)
    //
    // Some paths may also send the "type" field: {"type": 0, "value": 128.0}
    //==========================================================================
    struct JsonValue
    {
        enum Type { kNull, kBool, kInt, kDouble, kString };
        Type type = kNull;
        bool   boolVal   = false;
        int64_t intVal   = 0;
        double  doubleVal = 0.0;
        juce::String stringVal;

        bool   asBool()   const { return (type == kBool) ? boolVal : (intVal != 0); }
        int    asInt()     const { return (type == kInt) ? (int)intVal : (type == kDouble) ? toIntSaturated(doubleVal) : (boolVal ? 1 : 0); }
        double asDouble()  const { return (type == kDouble) ? doubleVal : (type == kInt) ? (double)intVal : 0.0; }
        juce::String asString() const { return stringVal; }
    };

    inline JsonValue parseJsonValue(const juce::String& json)
    {
        JsonValue v;
        auto trimmed = json.trim();
        if (trimmed.isEmpty()) return v;

        // Try JUCE JSON parser first (handles objects, arrays, primitives)
        auto parsed = juce::JSON::parse(trimmed);

        if (auto* obj = parsed.getDynamicObject())
        {
            // Object: extract named fields per StageLinQ convention
            // Priority: "string" > "state" > "value" > "color"
            if (obj->hasProperty("string"))
            {
                v.type = JsonValue::kString;
                v.stringVal = obj->getProperty("string").toString();
            }
            else if (obj->hasProperty("state"))
            {
                auto stateVal = obj->getProperty("state");
                if (stateVal.isBool())
                {
                    v.type = JsonValue::kBool;
                    v.boolVal = (bool)stateVal;
                }
                else
                {
                    // A "state" that is not a boolean, read as a number.  None
                    // seen: PlayState, like every "state" in the #23 PRIME 4+
                    // captures, is {"state": true/false}.
                    v.type = JsonValue::kInt;
                    v.intVal = toIntSaturated((double)stateVal);
                    v.doubleVal = (double)v.intVal;
                }
            }
            else if (obj->hasProperty("value"))
            {
                v.type = JsonValue::kDouble;
                v.doubleVal = (double)obj->getProperty("value");
                v.intVal = toInt64Saturated(v.doubleVal);
            }
            else if (obj->hasProperty("color"))
            {
                v.type = JsonValue::kInt;
                v.intVal = toIntSaturated((double)obj->getProperty("color"));
                v.doubleVal = (double)v.intVal;
            }
            else
            {
                // Unknown object structure -- log the first few for debugging
                v.type = JsonValue::kString;
                v.stringVal = trimmed;
            }
        }
        else if (parsed.isBool())
        {
            // Bare primitive (unlikely but handle gracefully)
            v.type = JsonValue::kBool;
            v.boolVal = (bool)parsed;
        }
        else if (parsed.isDouble() || parsed.isInt() || parsed.isInt64())
        {
            v.type = JsonValue::kDouble;
            v.doubleVal = (double)parsed;
            v.intVal = toInt64Saturated(v.doubleVal);
        }
        else if (parsed.isString())
        {
            v.type = JsonValue::kString;
            v.stringVal = parsed.toString();
        }

        return v;
    }

    //==========================================================================
    // Token generation
    //
    // Known-good token from chrisle/StageLinq (SoundSwitch identity).
    // Random tokens can fail silently on some firmware versions due to
    // undocumented constraints.  Both chrisle/StageLinq and djctl switched
    // to pre-defined tokens after encountering issues with random generation.
    //
    // Every STC instance announces this same token, and so does SoundSwitch.
    // STC drops discovery frames carrying its own token, so it ignores
    // another STC's (it offers no services anyway) and SoundSwitch's (also
    // skipped by name).  What a Denon unit does with two clients of one
    // token -- a main and a backup STC, or STC beside SoundSwitch -- is not
    // known: a wire question for a capture (AUDIT SLQ-10, DESIGN D2).
    //
    // CRITICAL CONSTRAINT (from PyStageLinQ protocol docs): if the most
    // significant bit of the token's first byte is 1, the device will
    // silently ignore the service request and never reply with services.
    // Our known-good token has byte[0]=0x52 (MSB=0), so this is safe.
    // If you ever switch to random tokens, mask byte[0] with 0x7F.
    //
    // The SoundSwitch token is used by multiple third-party implementations
    // and is known to work with all tested Denon firmware versions.
    //==========================================================================
    static constexpr uint8_t kKnownGoodToken[kTokenLen] = {
        82, 253, 252, 7, 33, 130, 101, 79, 22, 63, 95, 15, 154, 98, 29, 114
    };

    inline void generateToken(uint8_t token[kTokenLen])
    {
        // Use the known-good SoundSwitch token for maximum compatibility
        std::memcpy(token, kKnownGoodToken, kTokenLen);
    }

    //==========================================================================
    // Build a discovery frame
    //==========================================================================
    inline std::vector<uint8_t> buildDiscoveryFrame(
        const uint8_t token[kTokenLen],
        const juce::String& deviceName,
        const juce::String& action,
        const juce::String& swName,
        const juce::String& swVersion,
        uint16_t servicePort)
    {
        std::vector<uint8_t> frame;
        frame.reserve(256);

        // Magic "airD"
        frame.insert(frame.end(), kDiscoveryMagic, kDiscoveryMagic + 4);
        // Token
        frame.insert(frame.end(), token, token + kTokenLen);
        // Network strings
        appendNetworkString(frame, deviceName);
        appendNetworkString(frame, action);
        appendNetworkString(frame, swName);
        appendNetworkString(frame, swVersion);
        // Service port
        uint8_t portBytes[2];
        writeU16BE(portBytes, servicePort);
        frame.push_back(portBytes[0]);
        frame.push_back(portBytes[1]);

        return frame;
    }

    //==========================================================================
    // Build a service request frame (TCP)
    //==========================================================================
    inline std::vector<uint8_t> buildServiceRequest(const uint8_t token[kTokenLen])
    {
        std::vector<uint8_t> frame;
        frame.reserve(20);
        // Message ID: 0x00000002
        uint8_t id[4];
        writeU32BE(id, kMsgServiceRequest);
        frame.insert(frame.end(), id, id + 4);
        // Token
        frame.insert(frame.end(), token, token + kTokenLen);
        return frame;
    }

    //==========================================================================
    // Build a service announcement frame (TCP, sent to service ports)
    //==========================================================================
    inline std::vector<uint8_t> buildServiceAnnouncement(
        const uint8_t token[kTokenLen],
        const juce::String& serviceName,
        uint16_t port)
    {
        std::vector<uint8_t> frame;
        frame.reserve(64);
        uint8_t id[4];
        writeU32BE(id, kMsgServiceAnnounce);
        frame.insert(frame.end(), id, id + 4);
        frame.insert(frame.end(), token, token + kTokenLen);
        appendNetworkString(frame, serviceName);
        uint8_t portBytes[2];
        writeU16BE(portBytes, port);
        frame.push_back(portBytes[0]);
        frame.push_back(portBytes[1]);
        return frame;
    }

    //==========================================================================
    // Build a reference/keepalive frame (TCP)
    //==========================================================================
    inline std::vector<uint8_t> buildReferenceFrame(
        const uint8_t ownToken[kTokenLen],
        const uint8_t deviceToken[kTokenLen],
        int64_t reference)
    {
        std::vector<uint8_t> frame;
        frame.reserve(44);
        uint8_t id[4];
        writeU32BE(id, kMsgReference);
        frame.insert(frame.end(), id, id + 4);
        frame.insert(frame.end(), ownToken, ownToken + kTokenLen);
        frame.insert(frame.end(), deviceToken, deviceToken + kTokenLen);
        uint8_t refBytes[8] = {};
        for (int i = 0; i < 8; ++i)
            refBytes[i] = uint8_t(reference >> (56 - i * 8));
        frame.insert(frame.end(), refBytes, refBytes + 8);
        return frame;
    }

    //==========================================================================
    // Build a StateMap subscribe frame
    //==========================================================================
    inline std::vector<uint8_t> buildStateMapSubscribe(const juce::String& path)
    {
        // Body: "smaa"[4] + subtype[4] + pathString(UTF16BE) + interval[4]
        auto pathEncoded = encodeUTF16BE(path);

        // Body size = 4(smaa) + 4(subtype) + 4(pathLen) + pathEncoded.size() + 4(interval)
        uint32_t bodySize = 4 + 4 + 4 + (uint32_t)pathEncoded.size() + 4;

        std::vector<uint8_t> frame;
        frame.reserve(4 + bodySize);

        // Length prefix
        uint8_t lenBytes[4];
        writeU32BE(lenBytes, bodySize);
        frame.insert(frame.end(), lenBytes, lenBytes + 4);

        // "smaa"
        frame.insert(frame.end(), kSmaaMagic, kSmaaMagic + 4);

        // Sub-type: subscribe
        uint8_t subType[4];
        writeU32BE(subType, kSmaaSubscribe);
        frame.insert(frame.end(), subType, subType + 4);

        // Path as network string
        uint8_t pathLenBytes[4];
        writeU32BE(pathLenBytes, (uint32_t)pathEncoded.size());
        frame.insert(frame.end(), pathLenBytes, pathLenBytes + 4);
        frame.insert(frame.end(), pathEncoded.begin(), pathEncoded.end());

        // Interval (0)
        uint8_t intervalBytes[4] = { 0, 0, 0, 0 };
        frame.insert(frame.end(), intervalBytes, intervalBytes + 4);

        return frame;
    }

    //==========================================================================
    // Build a BeatInfo start-stream frame
    //==========================================================================
    //
    // Handshake is exactly 8 bytes: length=4 (BE), magic=0x00000000 (BE).
    // Adding any payload (e.g. our token) is a known way to make the device
    // silently refuse to stream -- the connection stays open but no packets
    // ever arrive.  Independent third-party Python implementations against
    // SC5000 have reproduced this; keep the frame minimal.
    //
    // BeatInfo silence conditions (field-reported, not all firmware-confirmed):
    //   - The deck has no track loaded, or is not in playback.
    //   - Some firmware will only emit if the consumer was already listening
    //     when the device booted; power-cycling the device after STC has
    //     started restores the stream.
    //   - The physical LINK button on the SC5000 is on.  Linking decks at
    //     the device disables BeatInfo on the device side.
    //   - Decks are linked from Engine DJ (same effect as the physical LINK
    //     button).
    //
    // If a user reports "StageLinQ playhead never moves", these are the four
    // things to walk through before assuming a bug in this code path.
    inline std::vector<uint8_t> buildBeatInfoStart()
    {
        std::vector<uint8_t> frame;
        uint8_t lenBytes[4];
        writeU32BE(lenBytes, 4);
        frame.insert(frame.end(), lenBytes, lenBytes + 4);
        uint8_t magic[4];
        writeU32BE(magic, kBeatStartStream);
        frame.insert(frame.end(), magic, magic + 4);
        return frame;
    }

    // Build a BeatInfo stop frame.  Should be sent before closing the
    // BeatInfo socket for a clean disconnect (otherwise the device only
    // learns we left via TCP RST).
    inline std::vector<uint8_t> buildBeatInfoStop()
    {
        std::vector<uint8_t> frame;
        uint8_t lenBytes[4];
        writeU32BE(lenBytes, 4);
        frame.insert(frame.end(), lenBytes, lenBytes + 4);
        uint8_t magic[4];
        writeU32BE(magic, kBeatStopStream);
        frame.insert(frame.end(), magic, magic + 4);
        return frame;
    }

    //==========================================================================
    // StateMap paths we subscribe to for each deck
    //==========================================================================
    inline juce::StringArray getDeckPaths(int deckNum)
    {
        juce::String d = "/Engine/Deck" + juce::String(deckNum);
        return {
            // Playback state
            d + "/Play",
            d + "/PlayState",
            d + "/PlayStatePath",
            d + "/CurrentBPM",
            d + "/Speed",
            d + "/SpeedState",
            d + "/SpeedNeutral",
            d + "/SpeedRange",
            d + "/SpeedOffsetUp",
            d + "/SpeedOffsetDown",
            d + "/SyncMode",
            d + "/ExternalMixerVolume",
            d + "/ExternalScratchWheelTouch",
            d + "/Pads/View",
            // Track metadata
            d + "/Track/ArtistName",
            d + "/Track/SongName",
            d + "/Track/TrackName",
            d + "/Track/TrackLength",
            d + "/Track/SongLoaded",
            d + "/Track/SongAnalyzed",
            d + "/Track/CurrentBPM",
            d + "/Track/CurrentKeyIndex",
            d + "/Track/KeyLock",
            d + "/Track/CuePosition",
            d + "/Track/SampleRate",
            d + "/Track/TrackNetworkPath",
            d + "/Track/TrackUri",
            d + "/Track/TrackData",
            d + "/Track/TrackBytes",
            d + "/Track/TrackWasPlayed",
            d + "/Track/Bleep",
            d + "/Track/SoundSwitchGuid",
            d + "/Track/PlayPauseLEDState",
            // Live loop state
            d + "/Track/CurrentLoopInPosition",
            d + "/Track/CurrentLoopOutPosition",
            d + "/Track/CurrentLoopSizeInBeats",
            d + "/Track/LoopEnableState",
            d + "/Track/Loop/QuickLoop1",
            d + "/Track/Loop/QuickLoop2",
            d + "/Track/Loop/QuickLoop3",
            d + "/Track/Loop/QuickLoop4",
            d + "/Track/Loop/QuickLoop5",
            d + "/Track/Loop/QuickLoop6",
            d + "/Track/Loop/QuickLoop7",
            d + "/Track/Loop/QuickLoop8",
            // DeckIsMaster lives under /Client, not /Engine
            "/Client/Deck" + juce::String(deckNum) + "/DeckIsMaster",
        };
    }

    inline juce::StringArray getMixerPaths()
    {
        return {
            "/Mixer/CH1faderPosition",
            "/Mixer/CH2faderPosition",
            "/Mixer/CH3faderPosition",
            "/Mixer/CH4faderPosition",
            "/Mixer/CrossfaderPosition",
            "/Mixer/ChannelAssignment1",
            "/Mixer/ChannelAssignment2",
            "/Mixer/ChannelAssignment3",
            "/Mixer/ChannelAssignment4",
            "/Mixer/NumberOfChannels",
        };
    }

    inline juce::StringArray getGlobalPaths()
    {
        return {
            "/Engine/DeckCount",
            "/Engine/Master/MasterTempo",
            "/Engine/Sync/Network/MasterStatus",
            "/Client/Preferences/LayerA",
            "/Client/Preferences/LayerB",
            "/Client/Preferences/Player",
            "/Client/Preferences/PlayerJogColorA",
            "/Client/Preferences/PlayerJogColorB",
            "/Client/Preferences/Profile/Application/SyncMode",
            "/Client/Preferences/Profile/Application/PlayerColor1",
            "/Client/Preferences/Profile/Application/PlayerColor1A",
            "/Client/Preferences/Profile/Application/PlayerColor1B",
            "/Client/Preferences/Profile/Application/PlayerColor2",
            "/Client/Preferences/Profile/Application/PlayerColor2A",
            "/Client/Preferences/Profile/Application/PlayerColor2B",
            "/Client/Preferences/Profile/Application/PlayerColor3",
            "/Client/Preferences/Profile/Application/PlayerColor3A",
            "/Client/Preferences/Profile/Application/PlayerColor3B",
            "/Client/Preferences/Profile/Application/PlayerColor4",
            "/Client/Preferences/Profile/Application/PlayerColor4A",
            "/Client/Preferences/Profile/Application/PlayerColor4B",
            "/Client/Librarian/DevicesController/CurrentDevice",
            "/Client/Librarian/DevicesController/HasSDCardConnected",
            "/Client/Librarian/DevicesController/HasUsbDeviceConnected",
            "/GUI/Decks/Deck/ActiveDeck",
            "/GUI/ViewLayer/LayerB",
        };
    }

    //==========================================================================
    // Convert playhead position (ms) to SMPTE Timecode
    //==========================================================================
    inline Timecode playheadToTimecode(uint32_t playheadMs, FrameRate fps)
    {
        return wallClockToTimecode(double(playheadMs), fps);
    }

    //==========================================================================
    // Playback speed of one deck from consecutive BeatInfo messages.
    //
    // Each BeatInfo message carries the device's clock in nanoseconds
    // (chrisle/StageLinq's protocol notes; in the #23 capture it advanced
    // 76.0376 s while the capture's own clock advanced 76.0327 s) and every
    // deck's position in samples.  The speed is the position's advance in
    // seconds of track over the clock's advance.  On a PRIME 4+ the messages
    // come every 35 ms of device clock and the estimate scatters by 0.3 %
    // (1 sd) around the true rate, so it is used as it comes, unsmoothed:
    // 0.994 and 1.000 for the two unsynced decks of the capture, 0.803 for
    // the synced one, 0 while paused, and the ramp as SYNC pulls a deck in.
    //
    // A pair more than 0.5 s apart, less than 1 ms apart, or giving more
    // than kMaxPlausibleSpeed is not a measurement: the first two are a
    // stalled stream or a clock in other units, the last a jump in position
    // (cue, hot cue, track load).  Those return false and the caller keeps
    // the last estimate.
    //
    // A shorter jump -- under about 0.1 s forward or 0.18 s back at 1x, with
    // messages 35 ms apart -- is not caught and reads as one wrong speed for
    // 35 ms: a short loop or a roll going
    // back (around -0.7 for a 1/8-beat roll), a small beat jump forward
    // (2-3x).  Left so (AUDIT SLQ-8): the #23 capture has none of those,
    // and its real changes are as abrupt -- play 0 -> 0.33 -> 0.99, SYNC
    // -0.15 and -0.20 per message, pause 0.99 -> 0 -- so a stricter filter
    // would delay them by a message or more, with nothing to weigh it
    // against.  A capture with loops, rolls and beat jumps would settle it.
    //==========================================================================
    struct BeatSpeedTracker
    {
        uint64_t lastClock    = 0;      // device clock of the previous message (ns)
        double   lastTimeline = 0.0;    // this deck's position then (samples)
        bool     haveLast     = false;

        void reset() { *this = BeatSpeedTracker{}; }

        bool update(uint64_t clock, double timeline, double sampleRate, double& speedOut)
        {
            bool measured = false;
            if (haveLast && clock > lastClock && sampleRate > 0.0 && std::isfinite(timeline))
            {
                const double dtSec = double(clock - lastClock) * 1.0e-9;
                if (dtSec >= 0.001 && dtSec <= 0.5)
                {
                    const double speed = (timeline - lastTimeline) / sampleRate / dtSec;
                    if (std::isfinite(speed) && std::abs(speed) < kMaxPlausibleSpeed)
                    {
                        speedOut = speed;
                        measured = true;
                    }
                }
            }
            lastClock    = clock;
            lastTimeline = timeline;
            haveLast     = std::isfinite(timeline);
            return measured;
        }
    };
}

//==============================================================================
// Per-deck state -- atomics for cross-thread access
//==============================================================================
struct StageLinQDeckState
{
    std::atomic<bool>     active { false };       // deck exists / has data
    std::atomic<uint32_t> sourceId { 0 };         // connection that last wrote its /Engine/DeckN values or BeatInfo (StageLinQInput::clearDecksOf)
    std::atomic<uint32_t> mixerSourceId { 0 };    // connection that last wrote its /Mixer/ channel values
    std::atomic<int>      deckNumber { 0 };       // 1-4

    // Playback
    std::atomic<bool>     isPlaying { false };     // from /Engine/DeckN/Play
    std::atomic<int>      playState { 0 };         // from /Engine/DeckN/PlayState
    std::atomic<double>   currentBPM { 0.0 };      // from /Engine/DeckN/CurrentBPM
    // /Engine/DeckN/Speed was taken for the playback rate.  It is the pitch
    // fader's position, 0 to 1 with 0.5 at 0 % (#23 captures, PRIME 4+ on
    // Engine OS 5.0.4, range +/-20 %): 0.4853 at -0.59 %, 0.5000 at 0, and
    // 0.0085 for a deck synced to -19.66 %.  It does not change on pause.
    // The rate comes from BeatInfo (beatSpeed below); without BeatInfo,
    // from SpeedState and Play (getActualSpeedAt).  Speed is kept as sent.
    std::atomic<double>   speed { 0.0 };           // from /Engine/DeckN/Speed (see above)
    std::atomic<bool>     speedReceived { false };  // true once Speed path has sent at least one value
    // SpeedState is the pitch in percent (-19.66 at 0.803x, -0.59 at
    // 0.9941): the fallback rate.  The five after it are stored but drive
    // nothing (SyncMode reaches the SLQ View's deck state, which does not
    // draw it); on the PRIME 4+ SpeedRange is {"string":"20"} and
    // SpeedNeutral / SpeedOffsetUp are {"state": bool}, all read here as 0.
    std::atomic<double>   speedState { 0.0 };      // from /Engine/DeckN/SpeedState (percent)
    std::atomic<bool>     speedStateReceived { false };
    std::atomic<double>   speedNeutral { 1.0 };    // from SpeedNeutral
    std::atomic<double>   speedRange { 0.08 };     // from SpeedRange (+/-8% default)
    std::atomic<double>   speedOffsetUp { 0.0 };   // from SpeedOffsetUp
    std::atomic<double>   speedOffsetDown { 0.0 }; // from SpeedOffsetDown
    std::atomic<int>      syncMode { 0 };          // from SyncMode ({"string":"BeatOrBarSync"} on the PRIME 4+, read as 0)
    std::atomic<bool>     scratchWheelTouch { false }; // from ExternalScratchWheelTouch
    std::atomic<int>      padsView { 0 };          // from Pads/View
    std::atomic<bool>     bleep { false };         // from Track/Bleep (reverse mode)

    // Track metadata
    juce::String          artistName;              // guarded by metaMutex
    juce::String          songName;                // guarded by metaMutex
    juce::String          trackNetworkPath;        // from Track/TrackNetworkPath (guarded by metaMutex)
    juce::String          trackUri;                // from Track/TrackUri (streaming) (guarded by metaMutex)
    juce::String          soundSwitchGuid;         // from Track/SoundSwitchGuid (guarded by metaMutex)
    // Track/TrackLength is in samples, not seconds (AUDIT SLQ-2): the #23
    // PRIME 4+ capture has 49,545,472 for a 1123.5 s track at 44.1 kHz, and
    // its TrackBytes, 297,272,864, is six bytes a sample (24-bit stereo)
    // plus a 32-byte header.  Seconds are worked out with Track/SampleRate
    // when asked (getTrackLengthSec): the rate may come after the length,
    // as it does in that capture's subscription dump.  Samples are
    // confirmed on that unit only (Engine OS 5.0.4); chrisle's
    // docs/protocol.md calls it a duration in seconds, and a unit that
    // sent seconds would read as a length of 0 s here (BENCH).
    std::atomic<double>   trackLengthSamples { 0.0 }; // from Track/TrackLength (samples)
    std::atomic<bool>     songLoaded { false };     // from Track/SongLoaded
    std::atomic<bool>     songLoadedReceived { false };
    std::atomic<bool>     songAnalyzed { false };   // from Track/SongAnalyzed
    std::atomic<double>   trackBPM { 0.0 };        // from Track/CurrentBPM
    std::atomic<double>   cuePosition { 0.0 };     // from Track/CuePosition (samples: 11659.7 in the #23 capture, 0.26 s)
    std::atomic<int>      currentKeyIndex { -1 };  // from Track/CurrentKeyIndex (live, changes with key shift)
    std::atomic<bool>     keyLock { false };        // from Track/KeyLock
    std::atomic<double>   sampleRate { 44100.0 };  // from Track/SampleRate (44100 in the second #23 capture; the first has none)
    std::atomic<int>      trackBytes { 0 };        // from Track/TrackBytes (file size)
    std::atomic<bool>     trackWasPlayed { false }; // from Track/TrackWasPlayed
    std::atomic<int>      playPauseLEDState { 0 }; // from Track/PlayPauseLEDState

    // Live loop state (sample offsets, from StateMap -- real-time, not from DB)
    std::atomic<double>   loopInPosition { -1.0 };    // from Track/CurrentLoopInPosition (samples)
    std::atomic<double>   loopOutPosition { -1.0 };   // from Track/CurrentLoopOutPosition (samples)
    std::atomic<double>   loopSizeInBeats { 0.0 };    // from Track/CurrentLoopSizeInBeats
    std::atomic<bool>     loopEnabled { false };       // from Track/LoopEnableState
    std::atomic<bool>     quickLoops[8] = {};          // from Track/Loop/QuickLoop1-8

    // From BeatInfo service
    std::atomic<double>   beatInfoBeat { 0.0 };       // current beat position
    std::atomic<double>   beatInfoTotalBeats { 0.0 };  // total beats in track
    std::atomic<double>   beatInfoBPM { 0.0 };         // BPM from BeatInfo
    std::atomic<double>   beatInfoTimeline { 0.0 };    // current sample position

    // Playback speed measured from BeatInfo (StageLinQ::BeatSpeedTracker)
    std::atomic<double>   beatSpeed { 0.0 };         // 1.0 = normal, 0 = paused
    std::atomic<double>   beatSpeedTime { 0.0 };     // hiRes ms of the last measurement, 0 = none

    // Mixer (per-channel)
    std::atomic<double>   faderPosition { 0.0 };   // from /Mixer/CH{N}faderPosition, as sent (0-1, or 0-1.27 on a PRIME 4+)
    std::atomic<double>   externalVolume { 0.0 };   // from ExternalMixerVolume: level after fader and crossfader, 0-1
    std::atomic<bool>     externalVolumeReceived { false };
    std::atomic<bool>     isMaster { false };        // from /Client/DeckN/DeckIsMaster
    std::atomic<int>      channelAssignment { 0 };   // from /Mixer/ChannelAssignment{N}: crossfader side, assumed 0=THRU 1=A 2=B (never sent by a PRIME 4+)

    // Timing
    std::atomic<double>   lastUpdateTime { 0.0 };     // juce hiRes ms
    // Track identity (AUDIT ENG-7).  trackVersion goes up once per track
    // loaded: when artist, title, length and SongLoaded have settled
    // (StageLinQInput::publishSettledTracks) and differ from the identity
    // last published.  It never goes back (reset() leaves it and the
    // published identity alone), so an engine never mistakes a new track
    // for the one it last saw.  Guarded by metaMutex but trackVersion.
    std::atomic<uint32_t> trackVersion { 0 };
    juce::String          publishedArtist, publishedTitle;
    uint32_t              publishedLengthSec = 0;
    bool                  identityPending = false;  // a field changed since the last publication check
    double                identityFirstChange = 0.0, identityLastChange = 0.0;  // hiRes ms

    // Playhead in ms, from each BeatInfo message's timeline over the sample
    // rate (handleBeatInfo), and the host time that message was read
    std::atomic<uint32_t> playheadMs { 0 };
    std::atomic<double>   playheadTime { 0.0 };    // hiRes ms, 0 = none yet

    mutable std::mutex metaMutex;

    // keepMixerChannel: leave the channel's /Mixer/ values (fader,
    // crossfader side), which have an owner of their own (mixerSourceId)
    void reset(bool keepMixerChannel = false)
    {
        active.store(false, std::memory_order_release);
        sourceId.store(0);
        deckNumber.store(0);
        isPlaying.store(false);
        playState.store(0);
        currentBPM.store(0.0);
        speed.store(0.0);
        speedReceived.store(false);
        speedState.store(0.0);
        speedStateReceived.store(false);
        speedNeutral.store(1.0);
        speedRange.store(0.08);
        speedOffsetUp.store(0.0);
        speedOffsetDown.store(0.0);
        syncMode.store(0);
        scratchWheelTouch.store(false);
        padsView.store(0);
        bleep.store(false);
        {
            std::lock_guard<std::mutex> lock(metaMutex);
            artistName.clear();
            songName.clear();
            trackNetworkPath.clear();
            trackUri.clear();
            soundSwitchGuid.clear();
        }
        trackLengthSamples.store(0.0);
        songLoaded.store(false);
        songLoadedReceived.store(false);
        songAnalyzed.store(false);
        trackBPM.store(0.0);
        cuePosition.store(0.0);
        currentKeyIndex.store(-1);
        keyLock.store(false);
        sampleRate.store(44100.0);
        trackBytes.store(0);
        trackWasPlayed.store(false);
        playPauseLEDState.store(0);
        loopInPosition.store(-1.0);
        loopOutPosition.store(-1.0);
        loopSizeInBeats.store(0.0);
        loopEnabled.store(false);
        for (auto& ql : quickLoops) ql.store(false);
        beatInfoBeat.store(0.0);
        beatInfoTotalBeats.store(0.0);
        beatInfoBPM.store(0.0);
        beatInfoTimeline.store(0.0);
        beatSpeed.store(0.0);
        beatSpeedTime.store(0.0);
        if (!keepMixerChannel)
        {
            mixerSourceId.store(0);
            faderPosition.store(0.0);
            channelAssignment.store(0);
        }
        externalVolume.store(0.0);
        externalVolumeReceived.store(false);
        isMaster.store(false);
        lastUpdateTime.store(0.0);
        {
            std::lock_guard<std::mutex> lock(metaMutex);
            identityPending = false;        // trackVersion and the published identity stay
        }
        playheadMs.store(0);
        playheadTime.store(0.0);
    }
};

//==============================================================================
// Mixer state (crossfader, global)
//==============================================================================
struct StageLinQMixerState
{
    std::atomic<double> crossfaderPosition { 0.0 };  // as sent: 0=left, 1 (or 1.27) = right
    std::atomic<double> masterBPM { 0.0 };           // from /Engine/Master/MasterTempo
    std::atomic<int>    numChannels { 0 };            // from /Mixer/NumberOfChannels
    std::atomic<double> positionScale { 1.0 };       // full scale of faders and crossfader (kExtendedPositionScale)
    std::atomic<bool>   valuesReceived { false };    // any fader, crossfader or ExternalMixerVolume value
    std::atomic<bool>   assignmentReceived { false }; // any /Mixer/ChannelAssignmentN (never, from a PRIME 4+)

    // Deck ring LED colors (from /Client/Preferences/Profile/Application/PlayerColorN)
    // Index 0-3 = deck 1-4.  Each has base, A (layer A), B (layer B) variants.
    // Value is an integer color from Engine OS (interpretation TBD with hardware).
    std::atomic<int>    playerColor[4]  = {};    // PlayerColor1-4
    std::atomic<int>    playerColorA[4] = {};    // PlayerColor1A-4A
    std::atomic<int>    playerColorB[4] = {};    // PlayerColor1B-4B

    // Cleared with the decks on start/stop, so a scale or a crossfader learned
    // from one device does not carry over to the next session.
    void reset()
    {
        crossfaderPosition.store(0.0);
        masterBPM.store(0.0);
        numChannels.store(0);
        positionScale.store(1.0);
        valuesReceived.store(false);
        assignmentReceived.store(false);
        for (auto& c : playerColor)  c.store(0);
        for (auto& c : playerColorA) c.store(0);
        for (auto& c : playerColorB) c.store(0);
    }
};

//==============================================================================
// Discovered device info
//==============================================================================
struct StageLinQDeviceInfo
{
    juce::String  ip;
    juce::String  deviceName;
    juce::String  swName;
    juce::String  swVersion;
    uint16_t      servicePort = 0;
    uint8_t       token[StageLinQ::kTokenLen] = {};
    double        lastSeenTime = 0.0;         // hiRes ms -- updated on each discovery frame
    double        lastConnectAttempt = 0.0;   // hiRes ms -- for reconnect cooldown
    int           deckCount = 0;              // from /Engine/DeckCount or default 2
    bool          connected = false;
    uint32_t      connectionId = 0;           // the connection thread launched last (markDisconnected)
};

//==============================================================================
// StageLinQInput -- main network handler
//==============================================================================
class StageLinQInput : public juce::Thread
{
public:
    //--------------------------------------------------------------------------
    // TrackInfo -- per-deck track metadata (matches ProDJLinkInput::TrackInfo)
    //--------------------------------------------------------------------------
    struct TrackInfo
    {
        juce::String artist;
        juce::String title;
    };

    //--------------------------------------------------------------------------
    StageLinQInput()
        : Thread("StageLinQ Input")
    {
        for (auto& d : decks) d.reset();
        StageLinQ::generateToken(ownToken);
    }

    ~StageLinQInput() override
    {
        stop();
    }

    //==========================================================================
    // Network interface management
    //==========================================================================
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

    juce::String getBindInfo() const { return bindIp; }
    int getSelectedInterface() const { return selectedInterface; }

    //==========================================================================
    // Start / Stop
    //==========================================================================
    bool start(int interfaceIndex = 0)
    {
        if (isRunningFlag.load(std::memory_order_relaxed))
            return true;

        refreshNetworkInterfaces();
        if (availableInterfaces.isEmpty())
        {
            DBG("StageLinQ: No network interfaces available");
            return false;
        }

        int idx = juce::jlimit(0, availableInterfaces.size() - 1, interfaceIndex);
        const auto& iface = availableInterfaces[idx];
        bindIp = iface.ip;
        selectedInterface = idx;

        // Reset all decks and the mixer
        for (auto& d : decks) d.reset();
        mixerState.reset();
        resetBeatSpeedTrackers();

        // Clear discovered devices
        {
            std::lock_guard<std::mutex> lock(devicesMutex);
            discoveredDevices.clear();
        }

        // Generate fresh token each start
        StageLinQ::generateToken(ownToken);

        isRunningFlag.store(true, std::memory_order_release);
        startThread(juce::Thread::Priority::normal);

        DBG("StageLinQ: Started on " + bindIp);
        return true;
    }

    void stop()
    {
        if (!isRunningFlag.load(std::memory_order_relaxed))
            return;

        isRunningFlag.store(false, std::memory_order_release);
        signalThreadShouldExit();

        // Close sockets to unblock reads
        {
            std::lock_guard<std::mutex> lock(socketMutex);
            if (discoverySocket)
            {
                discoverySocket->shutdown();
                discoverySocket.reset();
            }
        }

        // The discovery thread first: it is the one that launches connection
        // threads.  Closing the connections before it had stopped let it
        // launch a new one that outlived stop() and, at destruction, ran
        // against freed decks (AUDIT SLQ-6).
        stopThread(3000);

        // Then every device connection
        closeAllDeviceConnections();

        for (auto& d : decks) d.reset();
        mixerState.reset();
        resetBeatSpeedTrackers();

        DBG("StageLinQ: Stopped");
    }

    bool getIsRunning() const { return isRunningFlag.load(std::memory_order_acquire); }

    // Callbacks for database integration (set by MainComponent to avoid
    // circular header dependency with StageLinQDbClient).
    // onMetadataRequest: called when a new TrackNetworkPath arrives.
    // onFileTransferAvailable: called when a device's FileTransfer port is discovered.
    std::function<void(const juce::String& networkPath)> onMetadataRequest;
    std::function<void(const juce::String& ip, uint16_t port, const uint8_t* token)> onFileTransferAvailable;

    //==========================================================================
    // Public getters -- match ProDJLinkInput API patterns
    //==========================================================================

    // Deck number is 1-based (1-4)
    bool isDeckActive(int deckNum) const
    {
        int idx = deckNum - 1;
        if (idx < 0 || idx >= StageLinQ::kMaxDecks) return false;
        return decks[idx].active.load(std::memory_order_acquire);
    }

    bool isPlayerPlaying(int deckNum) const
    {
        int idx = deckNum - 1;
        if (idx < 0 || idx >= StageLinQ::kMaxDecks) return false;
        return decks[idx].isPlaying.load(std::memory_order_relaxed);
    }

    uint32_t getPlayheadMs(int deckNum) const
    {
        int idx = deckNum - 1;
        if (idx < 0 || idx >= StageLinQ::kMaxDecks) return 0;
        return decks[idx].playheadMs.load(std::memory_order_relaxed);
    }

    double getBPM(int deckNum) const
    {
        int idx = deckNum - 1;
        if (idx < 0 || idx >= StageLinQ::kMaxDecks) return 0.0;
        // Prefer BeatInfo BPM (higher rate), fall back to StateMap
        double biBpm = decks[idx].beatInfoBPM.load(std::memory_order_relaxed);
        if (biBpm > 0.0) return biBpm;
        return decks[idx].currentBPM.load(std::memory_order_relaxed);
    }

    /// Playback speed: 1.0 = normal, 0 = stopped.  Drives the engine's PLL,
    /// its interpolation and its source-active test, like the CDJ's actual
    /// speed does for Pro DJ Link.
    double getActualSpeed(int deckNum) const
    {
        return getActualSpeedAt(deckNum, juce::Time::getMillisecondCounterHiRes());
    }

    double getActualSpeedAt(int deckNum, double nowMs) const
    {
        int idx = deckNum - 1;
        if (idx < 0 || idx >= StageLinQ::kMaxDecks) return 0.0;

        // The rate measured from BeatInfo, while it is being measured.  A
        // PRIME 4+ streams BeatInfo for every deck all the time, paused or
        // not, so on that hardware this is the answer whenever STC has a
        // position at all (the position comes from the same messages).
        const double measuredAt = decks[idx].beatSpeedTime.load(std::memory_order_acquire);
        if (measuredAt > 0.0 && (nowMs - measuredAt) < StageLinQ::kBeatSpeedMaxAgeMs)
            return decks[idx].beatSpeed.load(std::memory_order_relaxed);

        // No BeatInfo (none yet, or it stalled).  Speed is the pitch fader's
        // position, not a rate (see the deck state), and it does not drop on
        // pause, so it is not used: a deck that is not playing is at 0, a
        // playing one at 1 + SpeedState / 100 -- 0.9941 and 0.8034 for the
        // two decks of the #23 captures, what BeatInfo measured for them --
        // or 1.0 from a device that sends no SpeedState.  (Before, Speed was
        // returned as the rate: 0.49 for a PRIME 4+ deck at -0.59 %, and a
        // paused deck never read 0.)
        if (!decks[idx].isPlaying.load(std::memory_order_relaxed))
            return 0.0;
        if (decks[idx].speedStateReceived.load(std::memory_order_acquire))
            return 1.0 + decks[idx].speedState.load(std::memory_order_relaxed) / 100.0;
        return 1.0;
    }

    /// The pitch in percent as the device states it (/Engine/DeckN/SpeedState:
    /// -19.66 for a deck synced to 0.803x, -0.59 at 0.9941x, #23 captures).
    /// False when the device has not sent it.  Exact where the speed measured
    /// from BeatInfo scatters by 0.3 % a message.
    bool getPitchPercent(int deckNum, double& percentOut) const
    {
        int idx = deckNum - 1;
        if (idx < 0 || idx >= StageLinQ::kMaxDecks) return false;
        if (!decks[idx].speedStateReceived.load(std::memory_order_acquire)) return false;
        percentOut = decks[idx].speedState.load(std::memory_order_relaxed);
        return std::isfinite(percentOut);
    }

    /// Track length in seconds: Track/TrackLength (samples) over
    /// Track/SampleRate (AUDIT SLQ-2; it was stored as seconds, so a 1123 s
    /// track read 49.5 million seconds).
    uint32_t getTrackLengthSec(int deckNum) const
    {
        int idx = deckNum - 1;
        if (idx < 0 || idx >= StageLinQ::kMaxDecks) return 0;
        const double sec = getTrackLengthSecExact(idx);
        return (uint32_t)juce::jlimit(0.0, 86400.0, sec);   // a day at most: off the wire
    }

    float getPlayPositionRatio(int deckNum) const
    {
        int idx = deckNum - 1;
        if (idx < 0 || idx >= StageLinQ::kMaxDecks) return 0.0f;
        const double len = getTrackLengthSecExact(idx);
        if (!(len > 0.0)) return 0.0f;
        double posMs = (double)decks[idx].playheadMs.load(std::memory_order_relaxed);
        return (float)juce::jlimit(0.0, 1.0, posMs / (len * 1000.0));
    }

    TrackInfo getTrackInfo(int deckNum) const
    {
        int idx = deckNum - 1;
        if (idx < 0 || idx >= StageLinQ::kMaxDecks) return {};
        std::lock_guard<std::mutex> lock(decks[idx].metaMutex);
        return { decks[idx].artistName, decks[idx].songName };
    }

    juce::String getTrackNetworkPath(int deckNum) const
    {
        int idx = deckNum - 1;
        if (idx < 0 || idx >= StageLinQ::kMaxDecks) return {};
        std::lock_guard<std::mutex> lock(decks[idx].metaMutex);
        return decks[idx].trackNetworkPath;
    }

    // --- New getters for extended StateMap data ---

    int getCurrentKeyIndex(int deckNum) const
    {
        int idx = deckNum - 1;
        if (idx < 0 || idx >= StageLinQ::kMaxDecks) return -1;
        return decks[idx].currentKeyIndex.load(std::memory_order_relaxed);
    }

    juce::String getCurrentKeyString(int deckNum) const
    {
        int keyIdx = getCurrentKeyIndex(deckNum);
        if (keyIdx < 0 || keyIdx > 23) return {};
        static const char* const keys[] = {
            "C",  "Am",  "G",   "Em",   "D",   "Bm",
            "A",  "F#m", "E",   "Dbm",  "B",   "Abm",
            "F#", "Ebm", "Db",  "Bbm",  "Ab",  "Fm",
            "Eb", "Cm",  "Bb",  "Gm",   "F",   "Dm"
        };
        return keys[keyIdx];
    }

    bool getKeyLock(int deckNum) const
    {
        int idx = deckNum - 1;
        if (idx < 0 || idx >= StageLinQ::kMaxDecks) return false;
        return decks[idx].keyLock.load(std::memory_order_relaxed);
    }

    double getSampleRate(int deckNum) const
    {
        int idx = deckNum - 1;
        if (idx < 0 || idx >= StageLinQ::kMaxDecks) return 44100.0;
        return decks[idx].sampleRate.load(std::memory_order_relaxed);
    }

    double getSpeedRange(int deckNum) const
    {
        int idx = deckNum - 1;
        if (idx < 0 || idx >= StageLinQ::kMaxDecks) return 0.08;
        return decks[idx].speedRange.load(std::memory_order_relaxed);
    }

    bool isLoopEnabled(int deckNum) const
    {
        int idx = deckNum - 1;
        if (idx < 0 || idx >= StageLinQ::kMaxDecks) return false;
        return decks[idx].loopEnabled.load(std::memory_order_relaxed);
    }

    double getLoopInPosition(int deckNum) const
    {
        int idx = deckNum - 1;
        if (idx < 0 || idx >= StageLinQ::kMaxDecks) return -1.0;
        return decks[idx].loopInPosition.load(std::memory_order_relaxed);
    }

    double getLoopOutPosition(int deckNum) const
    {
        int idx = deckNum - 1;
        if (idx < 0 || idx >= StageLinQ::kMaxDecks) return -1.0;
        return decks[idx].loopOutPosition.load(std::memory_order_relaxed);
    }

    double getLoopSizeInBeats(int deckNum) const
    {
        int idx = deckNum - 1;
        if (idx < 0 || idx >= StageLinQ::kMaxDecks) return 0.0;
        return decks[idx].loopSizeInBeats.load(std::memory_order_relaxed);
    }

    bool isScratchWheelTouched(int deckNum) const
    {
        int idx = deckNum - 1;
        if (idx < 0 || idx >= StageLinQ::kMaxDecks) return false;
        return decks[idx].scratchWheelTouch.load(std::memory_order_relaxed);
    }

    bool isBleep(int deckNum) const
    {
        int idx = deckNum - 1;
        if (idx < 0 || idx >= StageLinQ::kMaxDecks) return false;
        return decks[idx].bleep.load(std::memory_order_relaxed);
    }

    int getSyncMode(int deckNum) const
    {
        int idx = deckNum - 1;
        if (idx < 0 || idx >= StageLinQ::kMaxDecks) return 0;
        return decks[idx].syncMode.load(std::memory_order_relaxed);
    }

    juce::String getSoundSwitchGuid(int deckNum) const
    {
        int idx = deckNum - 1;
        if (idx < 0 || idx >= StageLinQ::kMaxDecks) return {};
        std::lock_guard<std::mutex> lock(decks[idx].metaMutex);
        return decks[idx].soundSwitchGuid;
    }

    // Deck ring LED color (base/A/B variant, 0=base, 1=layerA, 2=layerB)
    int getPlayerColor(int deckNum, int variant = 0) const
    {
        int idx = deckNum - 1;
        if (idx < 0 || idx >= StageLinQ::kMaxDecks) return 0;
        switch (variant)
        {
            case 1:  return mixerState.playerColorA[idx].load(std::memory_order_relaxed);
            case 2:  return mixerState.playerColorB[idx].load(std::memory_order_relaxed);
            default: return mixerState.playerColor[idx].load(std::memory_order_relaxed);
        }
    }

    juce::String getPlayStateString(int deckNum) const
    {
        int idx = deckNum - 1;
        if (idx < 0 || idx >= StageLinQ::kMaxDecks) return "N/A";
        if (decks[idx].isPlaying.load(std::memory_order_relaxed)) return "PLAYING";
        if (decks[idx].songLoaded.load(std::memory_order_relaxed)) return "PAUSED";
        return "NO TRACK";
    }

    uint32_t getTrackVersion(int deckNum) const
    {
        int idx = deckNum - 1;
        if (idx < 0 || idx >= StageLinQ::kMaxDecks) return 0;
        return decks[idx].trackVersion.load(std::memory_order_relaxed);
    }

    juce::String getPlayerModel(int deckNum) const
    {
        int idx = deckNum - 1;
        if (idx < 0 || idx >= StageLinQ::kMaxDecks) return "Denon";
        if (!decks[idx].active.load(std::memory_order_relaxed)) return {};

        // The name of whichever discovered device sorts first by IP, not
        // necessarily the one feeding this deck: STC handles one device at a
        // time (AUDIT SLQ-3, the file's header).
        std::lock_guard<std::mutex> lock(devicesMutex);
        if (!discoveredDevices.empty())
            return discoveredDevices.begin()->second.deviceName;
        return "Denon";
    }

    /// Crossfader, 0 = left end ... 1 = right end of its travel.
    double getCrossfaderPosition() const
    {
        return normalisePosition(mixerState.crossfaderPosition.load(std::memory_order_relaxed));
    }

    double getMasterBPM() const
    {
        double masterBpm = mixerState.masterBPM.load(std::memory_order_relaxed);
        if (masterBpm > 0.0) return masterBpm;
        // Fallback: return BPM from the master deck (if any)
        for (int i = 0; i < StageLinQ::kMaxDecks; ++i)
            if (decks[i].isMaster.load(std::memory_order_relaxed))
                return getBPM(i + 1);
        return 0.0;
    }

    bool isDeckMaster(int deckNum) const
    {
        int idx = deckNum - 1;
        if (idx < 0 || idx >= StageLinQ::kMaxDecks) return false;
        return decks[idx].isMaster.load(std::memory_order_relaxed);
    }

    /// Channel fader, 0 = closed ... 1 = top of its travel.
    double getFaderPosition(int channel) const
    {
        int idx = channel - 1;
        if (idx < 0 || idx >= StageLinQ::kMaxDecks) return 0.0;
        return normalisePosition(decks[idx].faderPosition.load(std::memory_order_relaxed));
    }

    /// A fader or crossfader value as sent, over the full scale learned so
    /// far (StageLinQ::kExtendedPositionScale), clamped to 0-1.
    double normalisePosition(double raw) const
    {
        const double scale = mixerState.positionScale.load(std::memory_order_relaxed);
        return juce::jlimit(0.0, 1.0, raw / (scale > 0.0 ? scale : 1.0));
    }

    /// Crossfader assignment for a channel.
    /// Values assumed to be: 0=THRU, 1=A, 2=B (same as Pioneer DJM).
    /// Not confirmed on any hardware: a PRIME 4+ never sends it (#23 second
    /// capture -- STC subscribes, nothing comes; hasChannelAssignment()).
    int getChannelAssignment(int channel) const
    {
        int idx = channel - 1;
        if (idx < 0 || idx >= StageLinQ::kMaxDecks) return 0;
        return decks[idx].channelAssignment.load(std::memory_order_relaxed);
    }

    /// Has the device ever sent a channel's crossfader assignment?  XF-A /
    /// XF-B cannot tell sides without it.
    bool hasChannelAssignment() const
    {
        return mixerState.assignmentReceived.load(std::memory_order_relaxed);
    }

    /// Is the deck heard on the mixer's output?
    ///
    /// From the deck's ExternalMixerVolume when the device sends it: that is
    /// the mixer's own answer, after channel fader and crossfader, with the
    /// crossfader assignment the DJ actually set (#23 captures, PRIME 4+) --
    /// on air above `quietLevel` (StageLinQ::onAirQuietLevel; 0 = anything
    /// but silence).  Otherwise derived from the fader, the crossfader and
    /// the channel's assumed assignment (0=THRU, 1=A, 2=B), as before.
    bool isDeckOnAir(int deckNum, double quietLevel = 0.0) const
    {
        static constexpr double kFaderThreshold = 0.02;  // ~2% above zero
        static constexpr double kXfCutThreshold = 0.02;  // crossfader fully to opposite side

        int idx = deckNum - 1;
        if (idx < 0 || idx >= StageLinQ::kMaxDecks) return false;

        if (decks[idx].externalVolumeReceived.load(std::memory_order_acquire))
            return decks[idx].externalVolume.load(std::memory_order_relaxed) > quietLevel;

        double fader = getFaderPosition(deckNum);
        if (fader < kFaderThreshold) return false;  // fader down = not on-air

        int assign = decks[idx].channelAssignment.load(std::memory_order_relaxed);
        double xf = getCrossfaderPosition();

        // Assumed: 0=THRU, 1=A (left), 2=B (right)
        if (assign == 1 && xf > (1.0 - kXfCutThreshold)) return false;  // A-side, xf fully right
        if (assign == 2 && xf < kXfCutThreshold)          return false;  // B-side, xf fully left

        return true;
    }

    /// ON AIR follow (DESIGN D34): how loud the deck is among the decks on air --
    /// its ExternalMixerVolume when the device sends it, otherwise its
    /// channel fader.  Negative when isDeckOnAir(deckNum, quietLevel) says
    /// it is not on air.
    double getOnAirLevel(int deckNum, double quietLevel = 0.0) const
    {
        if (!isDeckOnAir(deckNum, quietLevel)) return -1.0;
        const int idx = deckNum - 1;
        if (decks[idx].externalVolumeReceived.load(std::memory_order_acquire))
            return decks[idx].externalVolume.load(std::memory_order_relaxed);
        return getFaderPosition(deckNum);
    }

    /// Returns true once the device has sent any mixer data: its channel
    /// count, a fader, the crossfader or a deck's ExternalMixerVolume.
    /// (The channel count alone is not enough: the #23 PRIME 4+ capture
    /// has faders, crossfader and ExternalMixerVolume from start to end,
    /// and there is no telling from it whether NumberOfChannels was ever
    /// sent -- it would only have come once, when STC subscribed.)
    bool hasMixerData() const
    {
        return mixerState.numChannels.load(std::memory_order_relaxed) > 0
            || mixerState.valuesReceived.load(std::memory_order_relaxed);
    }

    // Any deck updated in the last 3 s, from any device (not per device:
    // AUDIT SLQ-3)
    bool isReceiving() const
    {
        double now = juce::Time::getMillisecondCounterHiRes();
        for (int i = 0; i < StageLinQ::kMaxDecks; ++i)
        {
            double lastTime = decks[i].lastUpdateTime.load(std::memory_order_relaxed);
            if (lastTime > 0.0 && (now - lastTime) < 3000.0)
                return true;
        }
        return false;
    }

    bool hasTimecodeData(int deckNum) const
    {
        int idx = deckNum - 1;
        if (idx < 0 || idx >= StageLinQ::kMaxDecks) return false;
        return decks[idx].lastUpdateTime.load(std::memory_order_relaxed) > 0.0;
    }

    // When the current playhead was read: the host time (hiRes ms) of the
    // BeatInfo message it comes from, 0 before any.  The engine's PLL takes
    // a change of it for a new position.  (It returned the time of the
    // deck's last update of any kind, a StateMap value included.)
    double getAbsPositionTs(int deckNum) const
    {
        int idx = deckNum - 1;
        if (idx < 0 || idx >= StageLinQ::kMaxDecks) return 0.0;
        return decks[idx].playheadTime.load(std::memory_order_acquire);
    }

    bool isPositionMoving(int deckNum) const
    {
        return isPlayerPlaying(deckNum);
    }

    // For compatibility with the Pro DJ Link API: always false -- StageLinQ
    // has no end-of-track state STC reads (the position comes from BeatInfo)
    bool isEndOfTrack(int /*deckNum*/) const { return false; }

    // For compatibility: StageLinQ doesn't have track IDs like rekordbox
    uint32_t getTrackID(int deckNum) const
    {
        return getTrackVersion(deckNum);
    }

    // Expose beat-in-bar from BeatInfo
    uint8_t getBeatInBar(int deckNum) const
    {
        int idx = deckNum - 1;
        if (idx < 0 || idx >= StageLinQ::kMaxDecks) return 1;
        double beat = decks[idx].beatInfoBeat.load(std::memory_order_relaxed);
        if (!std::isfinite(beat) || beat < 0.0) return 1;   // off the wire: (int)NaN or 1e300 is undefined
        int beatInBar = (int)std::fmod(std::floor(beat), 4.0) + 1;
        return (uint8_t)juce::jlimit(1, 4, beatInBar);
    }

    // Set track length from TrackMap (same pattern as ProDJLinkInput; no
    // caller today).  Stored in samples, as Track/TrackLength is.
    void setTrackLengthSec(int deckNum, uint32_t seconds)
    {
        int idx = deckNum - 1;
        if (idx < 0 || idx >= StageLinQ::kMaxDecks) return;
        decks[idx].trackLengthSamples.store((double)seconds * effectiveSampleRate(idx), std::memory_order_relaxed);
    }

private:
    /// Track/SampleRate, or 44100 when the device has not sent a usable one
    /// (STC's default all along; the first #23 capture has no SampleRate,
    /// so DESIGN D35's speeds rest on it; the second capture sends 44100).
    double effectiveSampleRate(int idx) const
    {
        const double sr = decks[(size_t)idx].sampleRate.load(std::memory_order_relaxed);
        return (std::isfinite(sr) && sr > 0.0) ? sr : 44100.0;
    }

    double getTrackLengthSecExact(int idx) const
    {
        const double samples = decks[(size_t)idx].trackLengthSamples.load(std::memory_order_relaxed);
        if (!std::isfinite(samples) || samples <= 0.0) return 0.0;
        return samples / effectiveSampleRate(idx);
    }

    //==========================================================================
    // Thread main loop
    //==========================================================================
    void run() override
    {
        DBG("StageLinQ: Thread started");

        // Create discovery listen socket (port 51337)
        // JUCE bindToPort() sets SO_REUSEADDR internally, allowing
        // SoundSwitch/Resolume to share the port.
        // DatagramSocket(true) additionally enables SO_BROADCAST for
        // sending our announcement frames.
        {
            std::lock_guard<std::mutex> lock(socketMutex);
            discoverySocket = std::make_unique<juce::DatagramSocket>(true);
            if (!discoverySocket->bindToPort(StageLinQ::kDiscoveryPort))
            {
                DBG("StageLinQ: Failed to bind UDP port " + juce::String(StageLinQ::kDiscoveryPort)
                    + " -- another application may be using it. Retrying without SO_BROADCAST...");
                discoverySocket = std::make_unique<juce::DatagramSocket>(false);
                if (!discoverySocket->bindToPort(StageLinQ::kDiscoveryPort))
                {
                    DBG("StageLinQ: Still failed to bind. Will send discovery but cannot receive.");
                }
            }
        }

        double lastAnnounceTime = 0.0;

        while (!threadShouldExit() && isRunningFlag.load(std::memory_order_relaxed))
        {
            double now = juce::Time::getMillisecondCounterHiRes();

            // --- Send our discovery announcement periodically ---
            if ((now - lastAnnounceTime) > StageLinQ::kDiscoveryInterval * 1000.0)
            {
                sendDiscoveryAnnouncement();
                lastAnnounceTime = now;
            }

            // --- Listen for discovery frames ---
            listenForDiscovery();

            // --- Manage device connections ---
            manageConnections();

            // --- Compute derived state (playhead) ---
            updateDerivedState();

            // Small sleep to avoid busy-wait -- discovery socket read has timeout
            juce::Thread::sleep(10);
        }

        // Send exit announcement
        sendDiscoveryExit();

        // Cleanup
        {
            std::lock_guard<std::mutex> lock(socketMutex);
            if (discoverySocket)
            {
                discoverySocket->shutdown();
                discoverySocket.reset();
            }
        }

        DBG("StageLinQ: Thread stopped");
    }

    //==========================================================================
    // Discovery: send our announcement to all network interfaces
    // On Windows, broadcasts don't route across interfaces from a single
    // socket.  Go-stagelinq and chrisle/StageLinq both send per-interface.
    // We create a temporary socket per interface, bound to its local IP,
    // and send to its subnet broadcast address.
    //==========================================================================
    void sendDiscoveryBroadcast(const juce::String& action)
    {
        auto frame = StageLinQ::buildDiscoveryFrame(
            ownToken,
            StageLinQ::kOurDeviceName,
            action,
            StageLinQ::kOurSwName,
            StageLinQ::kOurSwVersion,
            0  // We don't offer services
        );

        auto interfaces = ::getNetworkInterfaces();
        for (auto& iface : interfaces)
        {
            // Skip link-local 169.254.x.x -- chrisle/StageLinq skips these too.
            //
            // NOTE for direct cable setups: Denon Prime Go (and possibly other
            // standalone units) defaults to a 169.254.x.x link-local address
            // when no DHCP is available (PyStageLinQ protocol docs).  Our
            // listen socket is bound to INADDR_ANY so we DO receive discovery
            // from these devices.  But for the broadcast SEND to reach them,
            // the PC must also have an IP in the 169.254.0.0/16 range on the
            // same interface.  This skip only affects outbound announcements
            // -- it does NOT prevent connecting to link-local devices.
            if (iface.ip.startsWith("169.254.")) continue;

            juce::DatagramSocket sock(true);  // SO_BROADCAST required for broadcast send
            sock.bindToPort(0, iface.ip);  // bind to this interface's IP, random port

            juce::String broadcastIp = iface.broadcast;
            if (broadcastIp.isEmpty()) broadcastIp = "255.255.255.255";

            int sent = sock.write(broadcastIp, StageLinQ::kDiscoveryPort,
                                  frame.data(), (int)frame.size());
            if (sent <= 0)
            {
                DBG("StageLinQ: Failed to send discovery on " + iface.ip
                    + " -> " + broadcastIp);
            }
        }
    }

    void sendDiscoveryAnnouncement()
    {
        sendDiscoveryBroadcast(StageLinQ::kActionHowdy);
    }

    void sendDiscoveryExit()
    {
        sendDiscoveryBroadcast(StageLinQ::kActionExit);
    }

    //==========================================================================
    // Discovery: listen for device announcements
    //==========================================================================
    void listenForDiscovery()
    {
        std::lock_guard<std::mutex> slock(socketMutex);
        if (!discoverySocket) return;

        uint8_t buf[2048];
        juce::String senderIp;
        int senderPort = 0;

        // Non-blocking read with short wait
        if (!discoverySocket->waitUntilReady(true, 50))
            return;

        int bytesRead = discoverySocket->read(buf, sizeof(buf), false, senderIp, senderPort);
        if (bytesRead < 24) return;  // too short

        // Ignore our own broadcasts -- match by token (more reliable than IP
        // when multiple interfaces are present or behind NAT)
        if (bytesRead >= 20 && std::memcmp(buf + 4, ownToken, StageLinQ::kTokenLen) == 0)
            return;

        parseDiscoveryFrame(buf, bytesRead, senderIp);
    }

    //==========================================================================
    // Parse a discovery frame
    //==========================================================================
    void parseDiscoveryFrame(const uint8_t* data, int len, const juce::String& senderIp)
    {
        // Check magic "airD"
        if (std::memcmp(data, StageLinQ::kDiscoveryMagic, 4) != 0) return;

        int offset = 4;

        // Token (16 bytes)
        if (offset + StageLinQ::kTokenLen > len) return;
        uint8_t deviceToken[StageLinQ::kTokenLen];
        std::memcpy(deviceToken, data + offset, StageLinQ::kTokenLen);
        offset += StageLinQ::kTokenLen;

        // Device name
        juce::String deviceName;
        offset = StageLinQ::readNetworkString(data, len, offset, deviceName);
        if (offset < 0) return;

        // Connection type (action)
        juce::String action;
        offset = StageLinQ::readNetworkString(data, len, offset, action);
        if (offset < 0) return;

        // Software name
        juce::String swName;
        offset = StageLinQ::readNetworkString(data, len, offset, swName);
        if (offset < 0) return;

        // Software version
        juce::String swVersion;
        offset = StageLinQ::readNetworkString(data, len, offset, swVersion);
        if (offset < 0) return;

        // Service port
        if (offset + 2 > len) return;
        uint16_t servicePort = StageLinQ::readU16BE(data + offset);

        // Handle action
        if (action == StageLinQ::kActionExit)
        {
            DBG("StageLinQ: Device leaving: " + deviceName + " at " + senderIp);
            // Stop active connection threads BEFORE erasing the device entry
            // (must be outside devicesMutex to avoid lock order inversion)
            stopConnectionThreadsForIp(senderIp);
            std::lock_guard<std::mutex> lock(devicesMutex);
            discoveredDevices.erase(senderIp.toStdString());
            return;
        }

        if (action != StageLinQ::kActionHowdy)
        {
            DBG("StageLinQ: Unknown action '" + action + "' from " + senderIp);
            return;
        }

        // Skip non-player software -- per chrisle/StageLinq isIgnored():
        //   OfflineAnalyzer: Engine internal analysis process
        //   SoundSwitch*:    SoundSwitch lighting software
        //   Resolume*:       Resolume Arena/Avenue
        //   JM08:            X1800/X1850 mixer firmware (mixer data comes via StateMap)
        //   SSS0:            SoundSwitchEmbedded on players
        if (swName == "OfflineAnalyzer" || swName == "Offline Analyzer"
            || swName.startsWith("SoundSwitch")
            || swName.startsWith("Resolume")
            || swName == "JM08"
            || swName == "SSS0")
            return;

        // Register or update device
        double now = juce::Time::getMillisecondCounterHiRes();

        std::lock_guard<std::mutex> lock(devicesMutex);
        auto& dev = discoveredDevices[senderIp.toStdString()];
        bool isNew = dev.ip.isEmpty();

        dev.ip = senderIp;
        dev.deviceName = deviceName;
        dev.swName = swName;
        dev.swVersion = swVersion;
        dev.servicePort = servicePort;
        std::memcpy(dev.token, deviceToken, StageLinQ::kTokenLen);
        dev.lastSeenTime = now;

        if (isNew && servicePort > 0)
        {
            DBG("StageLinQ: Discovered " + deviceName + " (" + swName + " " + swVersion
                + ") at " + senderIp + ":" + juce::String(servicePort));
        }
    }

    //==========================================================================
    // Connection management
    //==========================================================================
    void manageConnections()
    {
        // Decide under devicesMutex, launch under connThreadsMutex -- never
        // one inside the other.  It took connThreadsMutex inside
        // devicesMutex while closeAllDeviceConnections() held
        // connThreadsMutex and waited for threads whose markDisconnected()
        // needs devicesMutex: a lock-order cycle (AUDIT SLQ-6).
        std::vector<StageLinQDeviceInfo> toLaunch;
        {
            std::lock_guard<std::mutex> lock(devicesMutex);

            double now = juce::Time::getMillisecondCounterHiRes();

            for (auto it = discoveredDevices.begin(); it != discoveredDevices.end();)
            {
                auto& dev = it->second;
                double age = now - dev.lastSeenTime;

                // A device that stopped announcing itself and has no
                // connection left is forgotten (AUDIT SLQ-7: entries were
                // never removed but on EXIT).  It is found again by its
                // next discovery frame.
                if (!dev.connected && age > StageLinQ::kDeviceTimeoutSec * 1000.0)
                {
                    it = discoveredDevices.erase(it);
                    continue;
                }
                ++it;

                // Skip already connected
                if (dev.connected) continue;

                // Skip devices without service port
                if (dev.servicePort == 0) continue;

                // Connect only 0.5 s or more after the device's LAST discovery
                // frame (not its first, as this said): a PRIME 4+ announces
                // once a second (#23 captures), so the window opens every
                // second, but a device announcing more often than every 0.5 s
                // would never be connected.  And a minimum cooldown between
                // reconnect attempts.
                if (age < 500.0) continue;
                if (dev.lastConnectAttempt > 0.0
                    && (now - dev.lastConnectAttempt) < StageLinQ::kReconnectDelay * 1000.0)
                    continue;

                // Connect in a background thread to avoid blocking discovery
                dev.connected = true;  // Mark as connecting to prevent re-entry
                dev.lastConnectAttempt = now;
                dev.connectionId = nextConnectionId++;
                toLaunch.push_back(dev);
            }
        }

        std::lock_guard<std::mutex> cLock(connThreadsMutex);
        for (const auto& dev : toLaunch)
        {
            // Kill any stale thread for the same IP before launching a new one
            // (can happen if device did EXIT + re-announce faster than thread teardown)
            for (auto& ct : connectionThreads)
            {
                if (ct && ct->getDeviceIp() == dev.ip && ct->isThreadRunning())
                {
                    DBG("StageLinQ: Stopping stale thread for " + dev.ip + " before reconnect");
                    ct->signalThreadShouldExit();
                    ct->closeSocket();
                }
            }

            // Launch connection thread (make_unique first for exception safety --
            // if push_back throws during vector realloc, the thread is still owned)
            auto connThread = std::make_unique<DeviceConnectionThread>(*this, dev);
            auto* connPtr = connThread.get();
            connectionThreads.push_back(std::move(connThread));
            connPtr->startThread();
        }

        // Prune stopped connection threads to prevent unbounded growth
        connectionThreads.erase(
            std::remove_if(connectionThreads.begin(), connectionThreads.end(),
                [](const std::unique_ptr<DeviceConnectionThread>& ct) {
                    return !ct->isThreadRunning();
                }),
            connectionThreads.end());
    }

    //==========================================================================
    // Stop and remove connection threads for a specific device IP.
    // Called when a device sends EXIT or before launching a new connection
    // to the same IP (prevents duplicate threads).
    // Caller must NOT hold devicesMutex (this method locks connThreadsMutex).
    //==========================================================================
    void stopConnectionThreadsForIp(const juce::String& ip)
    {
        std::lock_guard<std::mutex> lock(connThreadsMutex);
        for (auto& ct : connectionThreads)
        {
            if (ct && ct->getDeviceIp() == ip && ct->isThreadRunning())
            {
                DBG("StageLinQ: Stopping connection thread for " + ip);
                ct->signalThreadShouldExit();
                ct->closeSocket();
            }
        }
    }

    //==========================================================================
    // Close all device connection threads
    //==========================================================================
    void closeAllDeviceConnections()
    {
        // Taken out of the list under the lock, joined outside it: a thread
        // that is ending takes devicesMutex (markDisconnected), and nothing
        // may wait on it while holding a lock someone else needs.
        std::vector<std::unique_ptr<DeviceConnectionThread>> threads;
        {
            std::lock_guard<std::mutex> lock(connThreadsMutex);
            threads.swap(connectionThreads);
        }
        for (auto& ct : threads)
        {
            ct->signalThreadShouldExit();
            ct->closeSocket();
        }
        for (auto& ct : threads)
        {
            ct->stopThread(StageLinQ::kConnectionStopMs);
        }
    }

    //==========================================================================
    // A connection has ended (its thread is exiting): clear the decks it fed,
    // so they do not stay PLAYING with a frozen position -- and, through the
    // 1 + SpeedState/100 fallback, keep an engine active while another
    // device keeps isReceiving() true (AUDIT SLQ-7).  The track version and
    // the identity last published stay (StageLinQDeckState), so a device
    // that comes back with the same track does not fire it again.
    // A deck is owned through its /Engine/DeckN values and BeatInfo; the
    // mixer channel of the same number through its /Mixer/ values, which
    // may come from another unit (a mixer, or a second player relaying
    // one).  Each is cleared only with its own connection: a unit that only
    // sent a channel's fader leaves the deck another unit plays alone.
    //==========================================================================
    void clearDecksOf(uint32_t connectionId)
    {
        if (connectionId == 0) return;
        for (int i = 0; i < StageLinQ::kMaxDecks; ++i)
        {
            auto& dk = decks[(size_t)i];
            uint32_t mixerOwner = connectionId;
            if (dk.mixerSourceId.compare_exchange_strong(mixerOwner, 0u))
            {
                dk.faderPosition.store(0.0, std::memory_order_relaxed);
                dk.channelAssignment.store(0, std::memory_order_relaxed);
            }
            uint32_t owner = connectionId;
            if (!dk.sourceId.compare_exchange_strong(owner, 0u)) continue;
            dk.reset(true);
            std::lock_guard<std::mutex> lock(beatSpeedMutex);
            beatSpeedTrackers[(size_t)i].reset();
        }
    }

    //==========================================================================
    // Update derived state, on the discovery loop (about every 60 ms): the
    // track versions.  (The playhead is derived per BeatInfo message, in
    // handleBeatInfo -- AUDIT SLQ-4.)
    //==========================================================================
    void updateDerivedState()
    {
        publishSettledTracks(juce::Time::getMillisecondCounterHiRes());
    }

    //==========================================================================
    // Track identity (AUDIT ENG-7).  A load arrives as several StateMap
    // values -- ArtistName, SongName, TrackLength, SongLoaded, SampleRate,
    // in that order in the #23 PRIME 4+ subscription dump, all within a
    // millisecond; the order and spacing of a live load are not captured.
    // Each change is noted here (caller holds metaMutex) ...
    //==========================================================================
    static constexpr double kTrackSettleMs = 250.0;   // quiet this long = the load is complete
    static constexpr double kTrackSettleMaxMs = 1000.0;   // ... or this long after its first value

    static void noteIdentityChange(StageLinQDeckState& dk)
    {
        const double now = juce::Time::getMillisecondCounterHiRes();
        if (!dk.identityPending)
            dk.identityFirstChange = now;
        dk.identityPending = true;
        dk.identityLastChange = now;
    }

    // ... and once nothing has changed for kTrackSettleMs (kTrackSettleMaxMs
    // at most), the identity is compared with the one last published.  The
    // version goes up when a track is loaded (SongLoaded true, when the
    // device sends it) with an artist or a title, and differs from the last
    // one published: by artist or title, or by length when both lengths
    // are known.  A length that arrives after the rest completes the same
    // track (the engine picks it up without firing again), and reloading
    // the track already published is not a change (Pro DJ Link's engine
    // branch has the same guard on the rekordbox ID).
    void publishSettledTracks(double nowMs)
    {
        for (int i = 0; i < StageLinQ::kMaxDecks; ++i)
        {
            auto& dk = decks[(size_t)i];
            std::lock_guard<std::mutex> lock(dk.metaMutex);
            if (!dk.identityPending) continue;
            if (nowMs - dk.identityLastChange < kTrackSettleMs
                && nowMs - dk.identityFirstChange < kTrackSettleMaxMs)
                continue;
            dk.identityPending = false;

            if (dk.songLoadedReceived.load(std::memory_order_relaxed)
                && !dk.songLoaded.load(std::memory_order_relaxed))
                continue;                                   // nothing loaded
            if (dk.artistName.isEmpty() && dk.songName.isEmpty())
                continue;

            const uint32_t lengthSec = getTrackLengthSec(i + 1);
            if (dk.artistName == dk.publishedArtist && dk.songName == dk.publishedTitle)
            {
                if (lengthSec == dk.publishedLengthSec) continue;          // the same track
                if (dk.publishedLengthSec == 0 || lengthSec == 0)
                {
                    dk.publishedLengthSec = juce::jmax(dk.publishedLengthSec, lengthSec);
                    continue;                               // its length came late
                }
            }
            dk.publishedArtist = dk.artistName;
            dk.publishedTitle = dk.songName;
            dk.publishedLengthSec = lengthSec;
            dk.trackVersion.fetch_add(1, std::memory_order_release);
        }
    }

    //==========================================================================
    // Handle a StateMap value update from a device
    //==========================================================================
    // sourceId: the connection the value came from (0: none, as in the
    // replay tools); a deck remembers the last one that wrote its
    // /Engine/DeckN values, a mixer channel the last one that wrote its
    // /Mixer/ values (clearDecksOf).
    void handleStateMapValue(const juce::String& path, const StageLinQ::JsonValue& value,
                             int deckOffset = 0, uint32_t sourceId = 0)
    {
        // Parse deck number from path: /Engine/Deck{N}/...
        // Must check for digit at pos 12 -- "/Engine/DeckCount" also starts
        // with "/Engine/Deck" but is NOT a deck path.
        if (path.startsWith("/Engine/Deck") && path.length() > 12
            && path[12] >= '1' && path[12] <= '4')
        {
            int deckChar = path[12] - '0';  // "Deck1" -> 1
            // Apply multi-device offset: SC6000 player 2 sends Deck1/Deck2
            // but they map to STC decks 3-4 (deckOffset=2)
            int mappedDeck = deckChar + deckOffset;
            if (mappedDeck < 1 || mappedDeck > StageLinQ::kMaxDecks) return;
            int idx = mappedDeck - 1;

            auto& dk = decks[idx];
            if (sourceId != 0) dk.sourceId.store(sourceId, std::memory_order_relaxed);
            dk.active.store(true, std::memory_order_relaxed);
            dk.deckNumber.store(mappedDeck, std::memory_order_relaxed);
            dk.lastUpdateTime.store(juce::Time::getMillisecondCounterHiRes(), std::memory_order_relaxed);

            // Extract sub-path after /Engine/DeckN/
            juce::String sub = path.fromFirstOccurrenceOf("/Engine/Deck" + juce::String(deckChar) + "/", false, false);

            if (sub == "Play")
            {
                dk.isPlaying.store(value.asBool(), std::memory_order_relaxed);
            }
            else if (sub == "PlayState")
            {
                dk.playState.store(value.asInt(), std::memory_order_relaxed);
            }
            else if (sub == "CurrentBPM")
            {
                dk.currentBPM.store(value.asDouble(), std::memory_order_relaxed);
            }
            else if (sub == "Speed")
            {
                dk.speed.store(value.asDouble(), std::memory_order_relaxed);
                dk.speedReceived.store(true, std::memory_order_relaxed);
            }
            else if (sub == "SpeedState")
            {
                dk.speedState.store(value.asDouble(), std::memory_order_relaxed);
                dk.speedStateReceived.store(true, std::memory_order_release);
            }
            // Artist, title, length and SongLoaded make up the track's
            // identity: each change is noted, and the track version goes up
            // once they have settled (publishSettledTracks, ENG-7) -- not on
            // each of them, which bumped it two or three times per load
            // with artist and title from different tracks in between.
            else if (sub == "Track/ArtistName")
            {
                std::lock_guard<std::mutex> lock(dk.metaMutex);
                juce::String newArtist = value.asString();
                if (newArtist != dk.artistName)
                {
                    dk.artistName = newArtist;
                    noteIdentityChange(dk);
                }
            }
            else if (sub == "Track/SongName")
            {
                std::lock_guard<std::mutex> lock(dk.metaMutex);
                juce::String newTitle = value.asString();
                if (newTitle != dk.songName)
                {
                    dk.songName = newTitle;
                    noteIdentityChange(dk);
                }
            }
            else if (sub == "Track/TrackLength")
            {
                dk.trackLengthSamples.store(value.asDouble(), std::memory_order_relaxed);
                std::lock_guard<std::mutex> lock(dk.metaMutex);
                noteIdentityChange(dk);
            }
            else if (sub == "Track/SongLoaded")
            {
                dk.songLoaded.store(value.asBool(), std::memory_order_relaxed);
                dk.songLoadedReceived.store(true, std::memory_order_relaxed);
                std::lock_guard<std::mutex> lock(dk.metaMutex);
                noteIdentityChange(dk);
            }
            else if (sub == "Track/CurrentBPM")
            {
                dk.trackBPM.store(value.asDouble(), std::memory_order_relaxed);
            }
            else if (sub == "Track/CuePosition")
            {
                dk.cuePosition.store(value.asDouble(), std::memory_order_relaxed);
            }
            else if (sub == "Track/TrackNetworkPath")
            {
                juce::String netPath = value.asString();
                std::lock_guard<std::mutex> lock(dk.metaMutex);
                if (netPath != dk.trackNetworkPath)
                {
                    dk.trackNetworkPath = netPath;
                    // Trigger database metadata request (artwork, extended info)
                    if (onMetadataRequest && !netPath.isEmpty())
                        onMetadataRequest(netPath);
                }
            }
            // --- New paths ---
            else if (sub == "SpeedNeutral")
            {
                dk.speedNeutral.store(value.asDouble(), std::memory_order_relaxed);
            }
            else if (sub == "SpeedRange")
            {
                dk.speedRange.store(value.asDouble(), std::memory_order_relaxed);
            }
            else if (sub == "SpeedOffsetUp")
            {
                dk.speedOffsetUp.store(value.asDouble(), std::memory_order_relaxed);
            }
            else if (sub == "SpeedOffsetDown")
            {
                dk.speedOffsetDown.store(value.asDouble(), std::memory_order_relaxed);
            }
            else if (sub == "SyncMode")
            {
                dk.syncMode.store(value.asInt(), std::memory_order_relaxed);
            }
            else if (sub == "ExternalScratchWheelTouch")
            {
                dk.scratchWheelTouch.store(value.asBool(), std::memory_order_relaxed);
            }
            else if (sub == "Pads/View")
            {
                dk.padsView.store(value.asInt(), std::memory_order_relaxed);
            }
            else if (sub == "Track/Bleep")
            {
                dk.bleep.store(value.asBool(), std::memory_order_relaxed);
            }
            else if (sub == "Track/CurrentKeyIndex")
            {
                dk.currentKeyIndex.store(value.asInt(), std::memory_order_relaxed);
            }
            else if (sub == "Track/KeyLock")
            {
                dk.keyLock.store(value.asBool(), std::memory_order_relaxed);
            }
            else if (sub == "Track/SampleRate")
            {
                dk.sampleRate.store(value.asDouble(), std::memory_order_relaxed);
                std::lock_guard<std::mutex> lock(dk.metaMutex);
                noteIdentityChange(dk);     // the length in seconds depends on it
            }
            else if (sub == "Track/SongAnalyzed")
            {
                dk.songAnalyzed.store(value.asBool(), std::memory_order_relaxed);
            }
            else if (sub == "Track/TrackUri")
            {
                std::lock_guard<std::mutex> lock(dk.metaMutex);
                dk.trackUri = value.asString();
            }
            else if (sub == "Track/TrackBytes")
            {
                dk.trackBytes.store(value.asInt(), std::memory_order_relaxed);
            }
            else if (sub == "Track/TrackWasPlayed")
            {
                dk.trackWasPlayed.store(value.asBool(), std::memory_order_relaxed);
            }
            else if (sub == "Track/SoundSwitchGuid")
            {
                std::lock_guard<std::mutex> lock(dk.metaMutex);
                dk.soundSwitchGuid = value.asString();
            }
            else if (sub == "Track/PlayPauseLEDState")
            {
                dk.playPauseLEDState.store(value.asInt(), std::memory_order_relaxed);
            }
            // Live loop state
            else if (sub == "Track/CurrentLoopInPosition")
            {
                dk.loopInPosition.store(value.asDouble(), std::memory_order_relaxed);
            }
            else if (sub == "Track/CurrentLoopOutPosition")
            {
                dk.loopOutPosition.store(value.asDouble(), std::memory_order_relaxed);
            }
            else if (sub == "Track/CurrentLoopSizeInBeats")
            {
                dk.loopSizeInBeats.store(value.asDouble(), std::memory_order_relaxed);
            }
            else if (sub == "Track/LoopEnableState")
            {
                dk.loopEnabled.store(value.asBool(), std::memory_order_relaxed);
            }
            else if (sub.startsWith("Track/Loop/QuickLoop"))
            {
                // "Track/Loop/QuickLoop1" -> index 0
                int qlIdx = sub.getLastCharacter() - '1';
                if (qlIdx >= 0 && qlIdx < 8)
                    dk.quickLoops[qlIdx].store(value.asBool(), std::memory_order_relaxed);
            }
            else if (sub == "ExternalMixerVolume")
            {
                dk.externalVolume.store(value.asDouble(), std::memory_order_relaxed);
                dk.externalVolumeReceived.store(true, std::memory_order_release);
                mixerState.valuesReceived.store(true, std::memory_order_relaxed);
            }
            // Subscribed but not stored (logged once for discovery, then ignored)
            // PlayStatePath: string version of PlayState (redundant with getPlayStateString)
            // Track/TrackName: raw filename on device (not the metadata title)
            // Track/TrackData: flag indicating track performance data availability
            else if (sub == "PlayStatePath" || sub == "Track/TrackName" || sub == "Track/TrackData")
            {
                // Intentionally ignored -- data is redundant or not actionable.
                // Handled here to avoid polluting the unknown path log.
            }
        }
        // Mixer paths
        else if (path.startsWith("/Mixer/CH") && path.endsWith("faderPosition"))
        {
            int ch = path[9] - '0';  // /Mixer/CH1faderPosition -> 1
            if (ch >= 1 && ch <= StageLinQ::kMaxMixerChannels)
            {
                const double pos = value.asDouble();
                notePositionRange(pos);
                if (sourceId != 0) decks[ch - 1].mixerSourceId.store(sourceId, std::memory_order_relaxed);
                decks[ch - 1].faderPosition.store(pos, std::memory_order_relaxed);
                mixerState.valuesReceived.store(true, std::memory_order_relaxed);
            }
        }
        else if (path == "/Mixer/CrossfaderPosition")
        {
            const double pos = value.asDouble();
            notePositionRange(pos);
            mixerState.crossfaderPosition.store(pos, std::memory_order_relaxed);
            mixerState.valuesReceived.store(true, std::memory_order_relaxed);
        }
        else if (path.startsWith("/Mixer/ChannelAssignment"))
        {
            // /Mixer/ChannelAssignment1 -> channel 1's crossfader side, as
            // getChannelAssignment() reads it (assumed 0=THRU, 1=A, 2=B;
            // a PRIME 4+ never sends it: the second #23 capture has it
            // neither at subscription nor at any of six assignment
            // switches; DESIGN D35, after the first, had left it open)
            int ch = path[24] - '0';
            if (ch >= 1 && ch <= StageLinQ::kMaxMixerChannels)
            {
                if (sourceId != 0) decks[ch - 1].mixerSourceId.store(sourceId, std::memory_order_relaxed);
                decks[ch - 1].channelAssignment.store(value.asInt(), std::memory_order_relaxed);
                mixerState.assignmentReceived.store(true, std::memory_order_relaxed);
            }
        }
        else if (path == "/Mixer/NumberOfChannels")
        {
            mixerState.numChannels.store(value.asInt(), std::memory_order_relaxed);
        }
        // DeckIsMaster lives under /Client, not /Engine
        else if (path.startsWith("/Client/Deck") && path.endsWith("/DeckIsMaster"))
        {
            int deckChar = path[12] - '0';
            int mapped = deckChar + deckOffset;
            if (mapped >= 1 && mapped <= StageLinQ::kMaxDecks)
                decks[mapped - 1].isMaster.store(value.asBool(), std::memory_order_relaxed);
        }
        // Global paths
        else if (path == "/Engine/Master/MasterTempo")
        {
            mixerState.masterBPM.store(value.asDouble(), std::memory_order_relaxed);
        }
        else if (path == "/Engine/DeckCount")
        {
            DBG("StageLinQ: DeckCount = " + juce::String(value.asInt()));
        }
        // Deck ring LED colors: /Client/Preferences/Profile/Application/PlayerColor{1-4}{,A,B}
        else if (path.startsWith("/Client/Preferences/Profile/Application/PlayerColor"))
        {
            // Path ends with: "PlayerColor1", "PlayerColor2A", "PlayerColor3B", etc.
            juce::String suffix = path.fromLastOccurrenceOf("PlayerColor", false, false);
            if (suffix.isNotEmpty())
            {
                int deckIdx = suffix[0] - '1';  // '1'->'4' -> 0-3
                if (deckIdx >= 0 && deckIdx < 4)
                {
                    int colorVal = value.asInt();
                    if (suffix.length() == 1)
                        mixerState.playerColor[deckIdx].store(colorVal, std::memory_order_relaxed);
                    else if (suffix.endsWith("A"))
                        mixerState.playerColorA[deckIdx].store(colorVal, std::memory_order_relaxed);
                    else if (suffix.endsWith("B"))
                        mixerState.playerColorB[deckIdx].store(colorVal, std::memory_order_relaxed);
                }
            }
        }
        else
        {
#if JUCE_DEBUG
            // Log unknown paths once -- helpful for discovering new data
            // available from Denon hardware during initial testing
            std::lock_guard<std::mutex> lock(unknownPathsMutex);
            if (loggedUnknownPaths.find(path.toStdString()) == loggedUnknownPaths.end())
            {
                loggedUnknownPaths.insert(path.toStdString());
                DBG("StageLinQ: Unknown path '" + path + "' = " + value.asString());
            }
#endif
        }
    }

    //==========================================================================
    // Learn the full scale of the faders and crossfader (see
    // StageLinQ::kExtendedPositionScale).  Only ever grows during a session.
    //==========================================================================
    void notePositionRange(double raw)
    {
        if (!(raw > StageLinQ::kPositionOverrange) || !std::isfinite(raw)) return;
        const double wanted = juce::jmax(StageLinQ::kExtendedPositionScale, raw);
        double current = mixerState.positionScale.load(std::memory_order_relaxed);
        while (wanted > current
               && !mixerState.positionScale.compare_exchange_weak(current, wanted,
                                                                   std::memory_order_relaxed))
        {
        }
    }

    //==========================================================================
    // BeatInfo PlayerInfo (mirrors go-stagelinq)
    //==========================================================================
    struct PlayerInfo
    {
        double beat;
        double totalBeats;
        double bpm;
    };

    //==========================================================================
    // Handle BeatInfo data from a device
    //==========================================================================
    //
    // `clock` is the device's clock (ns) and `hostMs` the moment the message
    // was read; together with each deck's timeline they give the playback
    // speed (StageLinQ::BeatSpeedTracker), which getActualSpeed() returns.
    void handleBeatInfo(uint64_t clock, const std::vector<PlayerInfo>& players,
                        const std::vector<double>& timelines, int deckOffset,
                        double hostMs, uint32_t sourceId = 0)
    {
        int numDecks = juce::jmin((int)players.size(), StageLinQ::kMaxDecks);
        for (int i = 0; i < numDecks; ++i)
        {
            int mapped = i + deckOffset;
            if (mapped < 0 || mapped >= StageLinQ::kMaxDecks) continue;

            auto& dk = decks[mapped];
            if (sourceId != 0) dk.sourceId.store(sourceId, std::memory_order_relaxed);
            dk.active.store(true, std::memory_order_relaxed);
            dk.deckNumber.store(mapped + 1, std::memory_order_relaxed);
            dk.beatInfoBeat.store(players[i].beat, std::memory_order_relaxed);
            dk.beatInfoTotalBeats.store(players[i].totalBeats, std::memory_order_relaxed);
            dk.beatInfoBPM.store(players[i].bpm, std::memory_order_relaxed);
            dk.lastUpdateTime.store(hostMs, std::memory_order_relaxed);

            if (i < (int)timelines.size())
            {
                dk.beatInfoTimeline.store(timelines[i], std::memory_order_relaxed);

                // The playhead, from this message and stamped with the time it
                // was read (AUDIT SLQ-4).  It was derived on the discovery loop
                // instead, every ~60 ms against BeatInfo's 35 ms, so the value
                // the engine snapped to was up to a loop old and changed in
                // steps the engine's 50 ms interpolation could not bridge: on
                // the #23 capture 11-16 % of the engine's ticks stepped a frame
                // back (slq_capture_replay playhead).
                // The timeline is the position in samples (the #23 PRIME 4+
                // capture: BeatInfo's timeline over Track/SampleRate gives the
                // playback speed, DESIGN D35); dividing by the rate gives the
                // position in the track, whatever the pitch.  BPM > 0 means a
                // track is loaded; samples = 0 is the start of one.
                const double bpm = players[i].bpm;
                const double samples = timelines[i];
                if (bpm > 0.0 && std::isfinite(samples))
                {
                    const double ms = juce::jmax(0.0, samples) / effectiveSampleRate(mapped) * 1000.0;
                    dk.playheadMs.store((uint32_t)juce::jmin(ms, 4.0e9), std::memory_order_relaxed);
                    dk.playheadTime.store(hostMs, std::memory_order_release);
                }

                double speed = 0.0;
                bool measured = false;
                {
                    // One connection thread per deck while one device feeds
                    // it; the lock covers two devices mapped onto the same
                    // deck -- two units that both say Player 1, or SC6000
                    // players 3/4 (AUDIT SLQ-3), which then mix two clocks
                    // in one tracker.
                    std::lock_guard<std::mutex> lock(beatSpeedMutex);
                    measured = beatSpeedTrackers[(size_t)mapped].update(
                        clock, timelines[i], effectiveSampleRate(mapped), speed);
                }
                if (measured)
                {
                    dk.beatSpeed.store(speed, std::memory_order_relaxed);
                    dk.beatSpeedTime.store(hostMs, std::memory_order_release);
                }
            }
        }
    }

    void resetBeatSpeedTrackers()
    {
        std::lock_guard<std::mutex> lock(beatSpeedMutex);
        for (auto& t : beatSpeedTrackers) t.reset();
    }

    //==========================================================================
    // Per-device TCP connection thread
    //==========================================================================
    class DeviceConnectionThread : public juce::Thread
    {
    public:
        DeviceConnectionThread(StageLinQInput& ownerRef, const StageLinQDeviceInfo& device)
            : Thread("SLQ-" + device.ip.fromLastOccurrenceOf(".", false, false)),
              owner(ownerRef), deviceIp(device.ip), devicePort(device.servicePort),
              connectionId(device.connectionId)
        {
            std::memcpy(deviceToken, device.token, StageLinQ::kTokenLen);
            deviceName = device.deviceName;
        }

        ~DeviceConnectionThread() override
        {
            stopThread(StageLinQ::kConnectionStopMs);
        }

        const juce::String& getDeviceIp() const { return deviceIp; }

        void closeSocket()
        {
            std::lock_guard<std::mutex> lock(sockMutex);
            if (mainSocket)   { mainSocket->close(); }
            if (stateSocket)  { stateSocket->close(); }
            if (beatSocket)   { beatSocket->close(); }
        }

        void run() override
        {
            DBG("StageLinQ: Connecting to " + deviceName + " at " + deviceIp + ":" + juce::String(devicePort));

            // --- Phase 1: Connect to main TCP port (with retry) ---
            static constexpr int kMaxRetries = 3;
            for (int attempt = 1; attempt <= kMaxRetries; ++attempt)
            {
                if (threadShouldExit()) { markDisconnected(); return; }

                auto main = std::make_unique<juce::StreamingSocket>();
                if (!main->connect(deviceIp, devicePort, StageLinQ::kSocketTimeoutMs))
                {
                    DBG("StageLinQ: TCP connect failed to " + deviceIp
                        + " (attempt " + juce::String(attempt) + "/" + juce::String(kMaxRetries) + ")");
                    if (attempt < kMaxRetries)
                    {
                        wait(500);   // cut short by stopThread()
                        continue;
                    }
                    markDisconnected();
                    return;
                }

                {
                    std::lock_guard<std::mutex> lock(sockMutex);
                    mainSocket = std::move(main);
                }
                break;  // connected
            }

            // --- Phase 1b: Service handshake ---
            // Unified handshake: wait for device ServiceRequest, send ours,
            // collect service announcements, all in one buffer (no data loss).
            // Per chrisle/StageLinq: device sends ServiceRequest (0x02) first.
            // Per Go: just send and read -- works without wait on some firmware.
            // We try the TS approach (wait) with Go fallback (timeout + proceed).
            uint16_t stateMapPort = 0;
            uint16_t beatInfoPort = 0;
            uint16_t fileTransferPort = 0;

            if (!performServiceHandshake(stateMapPort, beatInfoPort, fileTransferPort))
            {
                DBG("StageLinQ: Service handshake failed");
                markDisconnected();
                return;
            }

            DBG("StageLinQ: Services from " + deviceName
                + " -- StateMap:" + juce::String(stateMapPort)
                + " BeatInfo:" + juce::String(beatInfoPort)
                + " FileTransfer:" + juce::String(fileTransferPort));

            // Start database client if FileTransfer is available
            if (fileTransferPort > 0 && owner.onFileTransferAvailable)
            {
                owner.onFileTransferAvailable(deviceIp, fileTransferPort, owner.ownToken);
            }

            // --- Phase 2: Connect to StateMap service ---
            // chrisle/StageLinq adds a 500ms delay before connecting to services
            // ("find out why we need these waits before connecting to a service")
            // Some firmware versions may need time between main handshake and service connect.
            wait(500);   // cut short by stopThread()
            if (threadShouldExit()) { markDisconnected(); return; }
            if (stateMapPort > 0)
            {
                auto sm = std::make_unique<juce::StreamingSocket>();
                if (sm->connect(deviceIp, stateMapPort, StageLinQ::kSocketTimeoutMs))
                {
                    // Announce ourselves on the StateMap service connection.
                    // Port field is ignored by the device (TS: "0 or any other
                    // 16 bit value seems to work fine").
                    auto announceFrame = StageLinQ::buildServiceAnnouncement(
                        owner.ownToken, "StateMap", 0);
                    tcpWrite(sm.get(), announceFrame);

                    // Subscribe to all relevant paths
                    subscribeToStatePaths(sm.get());

                    {
                        std::lock_guard<std::mutex> lock(sockMutex);
                        stateSocket = std::move(sm);
                    }

                    DBG("StageLinQ: StateMap connected and subscribed");
                }
                else
                {
                    DBG("StageLinQ: StateMap connect failed on port " + juce::String(stateMapPort));
                }
            }

            // --- Phase 3: Connect to BeatInfo service ---
            if (threadShouldExit()) { markDisconnected(); return; }
            if (beatInfoPort > 0)
            {
                auto bi = std::make_unique<juce::StreamingSocket>();
                if (bi->connect(deviceIp, beatInfoPort, StageLinQ::kSocketTimeoutMs))
                {
                    auto announceFrame = StageLinQ::buildServiceAnnouncement(
                        owner.ownToken, "BeatInfo", 0);
                    tcpWrite(bi.get(), announceFrame);

                    // Start beat stream
                    auto startFrame = StageLinQ::buildBeatInfoStart();
                    tcpWrite(bi.get(), startFrame);

                    {
                        std::lock_guard<std::mutex> lock(sockMutex);
                        beatSocket = std::move(bi);
                    }

                    DBG("StageLinQ: BeatInfo connected and streaming");
                }
                else
                {
                    DBG("StageLinQ: BeatInfo connect failed on port " + juce::String(beatInfoPort));
                }
            }

            // --- Phase 4: Main read loop ---
            // Send reference keepalives, drain main socket, read StateMap and BeatInfo.
            // CRITICAL: mainSocket MUST be drained -- Go and TS both continuously
            // read Reference/Timestamp messages from the device.  Without draining,
            // the TCP receive buffer fills and the device stops sending.
            double lastRefTime = 0.0;
            lastDeviceRefTime = juce::Time::getMillisecondCounterHiRes();

            while (!threadShouldExit())
            {
                double now = juce::Time::getMillisecondCounterHiRes();

                // Proactive liveness check: if the device hasn't sent us anything
                // on the main socket for longer than the timeout, it's gone.
                // Exit cleanly instead of waiting for the next write to fail.
                if ((now - lastDeviceRefTime) > StageLinQ::kDeviceTimeoutSec * 1000.0)
                {
                    DBG("StageLinQ: No data from " + deviceName
                        + " for " + juce::String(StageLinQ::kDeviceTimeoutSec, 0) + "s -- disconnecting");
                    break;
                }

                // Send reference keepalive to main connection
                if ((now - lastRefTime) > StageLinQ::kReferenceInterval * 1000.0)
                {
                    auto refFrame = StageLinQ::buildReferenceFrame(
                        owner.ownToken, deviceToken, 0);
                    std::lock_guard<std::mutex> lock(sockMutex);
                    if (mainSocket && mainSocket->isConnected())
                        tcpWrite(mainSocket.get(), refFrame);
                    lastRefTime = now;
                }

                // Drain main socket (Reference/Timestamp messages from device)
                {
                    std::lock_guard<std::mutex> lock(sockMutex);
                    if (mainSocket && mainSocket->isConnected())
                        drainMainSocket();
                }

                // Read StateMap data
                {
                    std::lock_guard<std::mutex> lock(sockMutex);
                    if (stateSocket && stateSocket->isConnected())
                        readStateMapData(stateSocket.get());
                }

                // Read BeatInfo data
                {
                    std::lock_guard<std::mutex> lock(sockMutex);
                    if (beatSocket && beatSocket->isConnected())
                        readBeatInfoData(beatSocket.get());
                }

                // Detect dead sockets.  If StateMap dies (the primary data
                // channel), the connection is useless -- exit the loop so
                // markDisconnected() fires and manageConnections() can relaunch.
                // Without this check the thread becomes a zombie: keepalives
                // keep the main socket alive but no deck data flows, and
                // dev.connected stays true so no reconnect is attempted.
                // chrisle and go-stagelinq both tear down the entire connection
                // when any service socket fails.  A socket counts as dead once
                // a read on it has failed or found the device's close: the
                // readers close it then (tcpReadAvailable, drainMainSocket),
                // because JUCE's isConnected() stays true after a FIN or RST
                // and this test never fired before (AUDIT SLQ-5).
                {
                    std::lock_guard<std::mutex> lock(sockMutex);
                    bool mainLost = !mainSocket || !mainSocket->isConnected();
                    bool stateMapLost = stateMapPort > 0
                        && (!stateSocket || !stateSocket->isConnected());
                    bool beatInfoLost = beatInfoPort > 0
                        && (!beatSocket || !beatSocket->isConnected());

                    if (mainLost)
                    {
                        DBG("StageLinQ: Main connection lost on " + deviceName
                            + " -- disconnecting for reconnect");
                        break;
                    }

                    if (stateMapLost)
                    {
                        DBG("StageLinQ: StateMap socket lost on " + deviceName
                            + " -- disconnecting for reconnect");
                        break;
                    }
                    if (beatInfoLost)
                    {
                        DBG("StageLinQ: BeatInfo socket lost on " + deviceName
                            + " -- disconnecting for reconnect");
                        break;
                    }
                }

                juce::Thread::sleep(5);
            }

            // --- Clean shutdown: send BeatInfo stop before closing sockets ---
            // Without this the device only learns we left via TCP RST.
            {
                std::lock_guard<std::mutex> lock(sockMutex);
                if (beatSocket && beatSocket->isConnected())
                {
                    auto stopFrame = StageLinQ::buildBeatInfoStop();
                    tcpWrite(beatSocket.get(), stopFrame);
                }
            }

            markDisconnected();
        }

    private:
        StageLinQInput& owner;
        juce::String deviceIp;
        int devicePort;
        uint32_t connectionId = 0;   // StageLinQDeviceInfo::connectionId; marks the decks this connection feeds
        uint8_t deviceToken[StageLinQ::kTokenLen] = {};
        juce::String deviceName;

        // Multi-device deck mapping:
        // SC6000 player 1 -> deckOffset=0 (STC decks 1-2)
        // SC6000 player 2 -> deckOffset=2 (STC decks 3-4)
        // Prime 4 player 1 -> deckOffset=0 (STC decks 1-4; the PRIME 4+
        //   sends Player "1", #23 capture)
        // Players 3 and 4, and a second unit that is also player 1, get
        // offset 0 too and share decks with the first (AUDIT SLQ-3).
        int deckOffset = 0;   // set when /Client/Preferences/Player arrives

        std::unique_ptr<juce::StreamingSocket> mainSocket;
        std::unique_ptr<juce::StreamingSocket> stateSocket;
        std::unique_ptr<juce::StreamingSocket> beatSocket;
        std::mutex sockMutex;

        // TCP read buffers
        std::vector<uint8_t> stateReadBuf;
        std::vector<uint8_t> beatReadBuf;
        std::vector<uint8_t> mainReadBuf;

        // Device liveness tracking -- updated when we receive data from the
        // main socket (Reference/Timestamp messages).  If no data arrives
        // within kDeviceTimeoutSec the device is considered gone and the
        // connection thread exits proactively.
        double lastDeviceRefTime = 0.0;

        //----------------------------------------------------------------------
        // The connection is over (every exit of run()): its decks are
        // cleared (AUDIT SLQ-7), and the device may be connected again --
        // unless a newer connection to it has been launched meanwhile (a
        // stale thread ending after an EXIT and a quick re-announce marked
        // the new connection's device as disconnected, and a third thread
        // was launched over it).
        void markDisconnected()
        {
            owner.clearDecksOf(connectionId);
            std::lock_guard<std::mutex> lock(owner.devicesMutex);
            auto it = owner.discoveredDevices.find(deviceIp.toStdString());
            if (it != owner.discoveredDevices.end() && it->second.connectionId == connectionId)
                it->second.connected = false;
        }

        //----------------------------------------------------------------------
        // Unified service handshake (replaces separate wait + read methods).
        // Single persistent buffer prevents data loss in TCP bursts.
        //   1. Wait for device ServiceRequest (0x02) -- timeout OK
        //   2. Send our ServiceRequest
        //   3. Collect ServiceAnnouncements (0x00)
        //   4. Reference (0x01) after announcements = end of list
        //----------------------------------------------------------------------
        bool performServiceHandshake(uint16_t& stateMapPort, uint16_t& beatInfoPort,
                                     uint16_t& fileTransferPort)
        {
            double deadline = juce::Time::getMillisecondCounterHiRes() + 5000.0;
            std::vector<uint8_t> buf;
            bool sentOurRequest = false;

            while (juce::Time::getMillisecondCounterHiRes() < deadline && !threadShouldExit())
            {
                // Read available data
                {
                    std::lock_guard<std::mutex> lock(sockMutex);
                    if (!mainSocket || !mainSocket->isConnected()) return false;

                    if (mainSocket->waitUntilReady(true, 100))
                    {
                        uint8_t tmp[4096];
                        int bytesRead = mainSocket->read(tmp, sizeof(tmp), false);
                        if (bytesRead <= 0) return false;
                        buf.insert(buf.end(), tmp, tmp + bytesRead);
                    }
                }

                // Parse all complete messages from buffer
                int offset = 0;
                while (offset + 4 <= (int)buf.size())
                {
                    uint32_t msgId = StageLinQ::readU32BE(buf.data() + offset);

                    if (msgId == StageLinQ::kMsgServiceRequest)
                    {
                        // Device is ready for our request
                        int msgSize = 4 + StageLinQ::kTokenLen;
                        if (offset + msgSize > (int)buf.size()) break;
                        offset += msgSize;

                        if (!sentOurRequest)
                        {
                            auto reqFrame = StageLinQ::buildServiceRequest(owner.ownToken);
                            std::lock_guard<std::mutex> lock(sockMutex);
                            if (!tcpWrite(mainSocket.get(), reqFrame)) return false;
                            sentOurRequest = true;
                            DBG("StageLinQ: Sent service request (after device 0x02)");
                        }
                    }
                    else if (msgId == StageLinQ::kMsgServiceAnnounce)
                    {
                        // Service: ID[4] + Token[16] + Name(netstr) + Port[2]
                        int pos = offset + 4 + StageLinQ::kTokenLen;
                        juce::String serviceName;
                        pos = StageLinQ::readNetworkString(buf.data(), (int)buf.size(), pos, serviceName);
                        if (pos < 0 || pos + 2 > (int)buf.size()) break;
                        uint16_t port = StageLinQ::readU16BE(buf.data() + pos);
                        pos += 2;

                        DBG("StageLinQ: Service '" + serviceName + "' on port " + juce::String(port));
                        if (serviceName == "StateMap")     stateMapPort = port;
                        if (serviceName == "BeatInfo")     beatInfoPort = port;
                        if (serviceName == "FileTransfer") fileTransferPort = port;
                        offset = pos;
                    }
                    else if (msgId == StageLinQ::kMsgReference)
                    {
                        int refSize = 4 + StageLinQ::kTokenLen * 2 + 8;
                        if (offset + refSize > (int)buf.size()) break;
                        offset += refSize;

                        // Reference after services = end of list.  What
                        // is left in buf goes with it, and a service
                        // announced after this Reference is not seen
                        // (AUDIT SLQ-11; the PRIME 4+ announces its five
                        // services before its Reference, #23 capture).
                        if (stateMapPort > 0 || beatInfoPort > 0)
                        {
                            buf.erase(buf.begin(), buf.begin() + offset);
                            return true;
                        }

                        // No services yet -- send request (Go-style fallback)
                        if (!sentOurRequest)
                        {
                            auto reqFrame = StageLinQ::buildServiceRequest(owner.ownToken);
                            std::lock_guard<std::mutex> lock(sockMutex);
                            if (!tcpWrite(mainSocket.get(), reqFrame)) return false;
                            sentOurRequest = true;
                            DBG("StageLinQ: Sent service request (after Reference)");
                        }
                    }
                    else
                    {
                        DBG("StageLinQ: Unknown main msg 0x" + juce::String::toHexString((int)msgId));
                        offset += 4;
                    }
                }

                if (offset > 0)
                    buf.erase(buf.begin(), buf.begin() + juce::jmin(offset, (int)buf.size()));
            }

            // Timeout: last-resort send
            if (!sentOurRequest)
            {
                DBG("StageLinQ: Timeout, sending service request as last resort");
                auto reqFrame = StageLinQ::buildServiceRequest(owner.ownToken);
                std::lock_guard<std::mutex> lock(sockMutex);
                tcpWrite(mainSocket.get(), reqFrame);
            }
            return (stateMapPort > 0 || beatInfoPort > 0);
        }

        //----------------------------------------------------------------------
        // Drain main socket -- read and discard Reference/Timestamp messages.
        // MUST be called in Phase 4 loop.  Go-stagelinq has a dedicated
        // goroutine that reads mainSocket continuously.  Without draining,
        // the TCP receive window fills and the device stops sending.
        //----------------------------------------------------------------------
        void drainMainSocket()
        {
            if (!mainSocket || !mainSocket->isConnected()) return;
            const int ready = mainSocket->waitUntilReady(true, 1);
            if (ready == 0) return;

            uint8_t tmp[4096];
            int bytesRead = ready > 0 ? mainSocket->read(tmp, sizeof(tmp), false) : -1;
            if (bytesRead <= 0)
            {
                mainSocket->close();   // ready but nothing: the device closed it (or an error)
                return;
            }

            // Any data at all means the device is alive
            lastDeviceRefTime = juce::Time::getMillisecondCounterHiRes();

            // Minimally parse to consume complete Reference frames so the
            // buffer doesn't grow.  Reference: ID[4] + Token[16] + Token[16] + Clock[8] = 44 bytes.
            mainReadBuf.insert(mainReadBuf.end(), tmp, tmp + bytesRead);
            static constexpr int kRefFrameSize = 4 + StageLinQ::kTokenLen * 2 + 8;

            while (mainReadBuf.size() >= 4)
            {
                uint32_t msgId = StageLinQ::readU32BE(mainReadBuf.data());
                if (msgId == StageLinQ::kMsgReference && mainReadBuf.size() >= (size_t)kRefFrameSize)
                {
                    mainReadBuf.erase(mainReadBuf.begin(), mainReadBuf.begin() + kRefFrameSize);
                }
                else if (msgId == StageLinQ::kMsgServiceRequest)
                {
                    // Unexpected but harmless -- consume token
                    int frameSize = 4 + StageLinQ::kTokenLen;
                    if (mainReadBuf.size() < (size_t)frameSize) break;
                    mainReadBuf.erase(mainReadBuf.begin(), mainReadBuf.begin() + frameSize);
                }
                else
                {
                    // Unknown message ID (could be ServiceAnnounce=0x0 with
                    // variable-length name if device re-announces services after
                    // a USB insert, or any future protocol addition).  Cannot
                    // safely skip a fixed number of bytes because the frame size
                    // is unknown.  Clear the entire buffer -- loss of one
                    // Reference at most, which is harmless for a drain loop.
#if JUCE_DEBUG
                    DBG("StageLinQ: Unknown main msg 0x"
                        + juce::String::toHexString((int)msgId) + " from " + deviceName
                        + " -- clearing drain buffer (" + juce::String(mainReadBuf.size()) + " bytes)");
#endif
                    mainReadBuf.clear();
                    break;
                }
            }
        }

        //----------------------------------------------------------------------
        bool tcpWrite(juce::StreamingSocket* sock, const std::vector<uint8_t>& data)
        {
            if (!sock || !sock->isConnected()) return false;
            int written = sock->write(data.data(), (int)data.size());
            return written == (int)data.size();
        }

        //----------------------------------------------------------------------
        // Read bytes from TCP socket into a vector (non-blocking check)
        //----------------------------------------------------------------------
        // A failed read, or the device's close (ready, nothing to read),
        // closes the socket so that the dead-socket test in run() sees it
        // (AUDIT SLQ-5).
        bool tcpReadAvailable(juce::StreamingSocket* sock, std::vector<uint8_t>& buf)
        {
            if (!sock || !sock->isConnected()) return false;

            const int ready = sock->waitUntilReady(true, 5);
            if (ready == 0)
                return true;  // no data, but no error

            uint8_t tmp[8192];
            int bytesRead = ready > 0 ? sock->read(tmp, sizeof(tmp), false) : -1;
            if (bytesRead <= 0)
            {
                sock->close();
                return false;  // connection closed or error
            }

            buf.insert(buf.end(), tmp, tmp + bytesRead);
            return true;
        }

        //----------------------------------------------------------------------
        // Subscribe to all StateMap paths
        //----------------------------------------------------------------------
        void subscribeToStatePaths(juce::StreamingSocket* sock)
        {
            // CRITICAL: Subscribe to /Client/Preferences/Player FIRST.
            // On multi-device setups (e.g. SC6000 player 2), this path sets
            // the deckOffset that maps device-local Deck1/Deck2 to STC's
            // Deck3/Deck4.  If we subscribe to deck paths first, the device
            // replies with ~180 deck state values that arrive with deckOffset
            // still at 0 (wrong indices).  Subscribing Player first ensures
            // the offset is set before any deck data flows.
            {
                auto frame = StageLinQ::buildStateMapSubscribe(
                    "/Client/Preferences/Player");
                tcpWrite(sock, frame);
            }

            // Subscribe to 4 decks + mixer + global
            for (int d = 1; d <= StageLinQ::kMaxDecks; ++d)
            {
                for (const auto& path : StageLinQ::getDeckPaths(d))
                {
                    auto frame = StageLinQ::buildStateMapSubscribe(path);
                    tcpWrite(sock, frame);
                }
            }

            for (const auto& path : StageLinQ::getMixerPaths())
            {
                auto frame = StageLinQ::buildStateMapSubscribe(path);
                tcpWrite(sock, frame);
            }

            for (const auto& path : StageLinQ::getGlobalPaths())
            {
                // Player already subscribed above -- skip duplicate
                if (path == "/Client/Preferences/Player") continue;

                auto frame = StageLinQ::buildStateMapSubscribe(path);
                tcpWrite(sock, frame);
            }
        }

        //----------------------------------------------------------------------
        // Read and parse StateMap data (non-blocking)
        //----------------------------------------------------------------------
        void readStateMapData(juce::StreamingSocket* sock)
        {
            if (!tcpReadAvailable(sock, stateReadBuf)) return;

            // Parse complete smaa blocks from the buffer
            while (stateReadBuf.size() >= 4)
            {
                uint32_t blockLen = StageLinQ::readU32BE(stateReadBuf.data());
                if (blockLen == 0 || blockLen > StageLinQ::kMaxServiceBlock)
                {
                    // Malformed or corrupt -- scan forward for "smaa" magic to
                    // re-synchronize, instead of blind 4-byte skip which can
                    // burn CPU if the stream is badly corrupted.  The scan
                    // starts past this block's own magic (offset 4), so at
                    // least one byte is dropped: starting at 4 found that
                    // magic again, erased nothing and looped for ever under
                    // sockMutex (AUDIT WIRE-3).
                    bool resynced = false;
                    for (size_t i = 5; i + 4 <= stateReadBuf.size(); ++i)
                    {
                        if (std::memcmp(stateReadBuf.data() + i, StageLinQ::kSmaaMagic, 4) == 0)
                        {
                            // Found smaa at offset i -- the length field is 4 bytes before it
                            const size_t frameStart = i - 4;   // >= 1
                            stateReadBuf.erase(stateReadBuf.begin(),
                                               stateReadBuf.begin() + (std::ptrdiff_t)frameStart);
                            resynced = true;
                            break;
                        }
                    }
                    if (!resynced)
                    {
                        // No smaa found anywhere -- discard entire buffer
                        stateReadBuf.clear();
                    }
                    continue;
                }

                // Need blockLen + 4 bytes total (length field not included in blockLen)
                if (stateReadBuf.size() < blockLen + 4)
                    break;  // incomplete block, wait for more data

                // Parse the block
                const uint8_t* block = stateReadBuf.data() + 4;

                // Verify smaa magic
                if (blockLen >= 8 && std::memcmp(block, StageLinQ::kSmaaMagic, 4) == 0)
                {
                    uint32_t subType = StageLinQ::readU32BE(block + 4);

                    if (subType == StageLinQ::kSmaaStateEmit && blockLen > 12)
                    {
                        // State emit: smaa[4] + subtype[4] + path(netstr) + value(netstr)
                        int pos = 8;
                        juce::String path;
                        pos = StageLinQ::readNetworkString(block, (int)blockLen, pos, path);
                        if (pos >= 0)
                        {
                            juce::String jsonStr;
                            pos = StageLinQ::readNetworkString(block, (int)blockLen, pos, jsonStr);
                            if (pos >= 0)
                            {
                                auto val = StageLinQ::parseJsonValue(jsonStr);

                                // Intercept /Client/Preferences/Player to set
                                // multi-device deck offset. SC6000 player 2
                                // sends value {"string":"2"} -> deckOffset=2
                                if (path == "/Client/Preferences/Player")
                                {
                                    int playerNum = val.asString().getIntValue();
                                    if (playerNum >= 1 && playerNum <= 2)
                                    {
                                        deckOffset = (playerNum - 1) * 2;
                                        DBG("StageLinQ: Device " + deviceName
                                            + " player=" + juce::String(playerNum)
                                            + " -> deckOffset=" + juce::String(deckOffset));
                                    }
                                }

                                owner.handleStateMapValue(path, val, deckOffset, connectionId);
                            }
                        }
                    }
                    // kSmaaEmitResponse (0x7D1) -- subscription ack, ignore.
                    // kSmaaSubscribe (0x7D2) from device direction -- periodic
                    // catalogue announce (path + interval, no JSON), ignore.
#if JUCE_DEBUG
                    else if (subType != StageLinQ::kSmaaEmitResponse
                             && subType != StageLinQ::kSmaaSubscribe)
                    {
                        DBG("StageLinQ: Unknown smaa subtype 0x"
                            + juce::String::toHexString((int)subType)
                            + " (" + juce::String(blockLen) + " bytes) from " + deviceName);
                    }
#endif
                }
#if JUCE_DEBUG
                else if (blockLen >= 4)
                {
                    // Valid-length block that is NOT smaa -- could be a
                    // Reference/Timestamp on the service connection, or a
                    // new protocol message type.  Log for hardware debugging.
                    uint32_t firstWord = StageLinQ::readU32BE(block);
                    DBG("StageLinQ: Non-smaa StateMap block: 0x"
                        + juce::String::toHexString((int)firstWord)
                        + " (" + juce::String(blockLen) + " bytes) from " + deviceName);
                }
#endif

                // Consume the block
                stateReadBuf.erase(stateReadBuf.begin(), stateReadBuf.begin() + 4 + blockLen);
            }
        }

        //----------------------------------------------------------------------
        // Read and parse BeatInfo data (non-blocking)
        //----------------------------------------------------------------------
        void readBeatInfoData(juce::StreamingSocket* sock)
        {
            if (!tcpReadAvailable(sock, beatReadBuf)) return;

            // Parse complete BeatInfo blocks
            while (beatReadBuf.size() >= 8)
            {
                uint32_t blockLen = StageLinQ::readU32BE(beatReadBuf.data());
                if (blockLen == 0 || blockLen > StageLinQ::kMaxServiceBlock)
                {
                    // BeatInfo has no magic bytes for re-sync.  Discard entire
                    // buffer -- beat data is real-time and frame loss is harmless.
                    beatReadBuf.clear();
                    break;
                }

                if (beatReadBuf.size() < blockLen + 4)
                    break;

                const uint8_t* block = beatReadBuf.data() + 4;
                uint32_t magic = StageLinQ::readU32BE(block);

                if (magic == StageLinQ::kBeatEmit && blockLen >= 16)
                {
                    // Beat emit: magic[4] + clock[8] + numRecords[4] + records...
                    uint64_t clock = StageLinQ::readU64BE(block + 4);
                    uint32_t numRecords = StageLinQ::readU32BE(block + 12);

                    int pos = 16;
                    // Each player record: beat[8] + totalBeats[8] + bpm[8] = 24 bytes,
                    // plus one 8-byte timeline entry per record.  numRecords comes
                    // off the wire: bound it in unsigned arithmetic BEFORE any
                    // multiplication or cast -- (int)numRecords * 24 overflowed for
                    // a crafted count and the loop below then read past the buffer.
                    const uint32_t bytesForRecords = blockLen - 16;   // blockLen >= 16 here
                    if (numRecords <= bytesForRecords / 32)
                    {
                        std::vector<StageLinQInput::PlayerInfo> players;
                        std::vector<double> timelines;

                        for (uint32_t r = 0; r < numRecords; ++r)
                        {
                            StageLinQInput::PlayerInfo pi;
                            pi.beat       = StageLinQ::readF64BE(block + pos);      pos += 8;
                            pi.totalBeats = StageLinQ::readF64BE(block + pos);      pos += 8;
                            pi.bpm        = StageLinQ::readF64BE(block + pos);      pos += 8;
                            players.push_back(pi);
                        }

                        for (uint32_t r = 0; r < numRecords; ++r)
                        {
                            timelines.push_back(StageLinQ::readF64BE(block + pos));
                            pos += 8;
                        }

                        owner.handleBeatInfo(clock, players, timelines, deckOffset,
                                             juce::Time::getMillisecondCounterHiRes(), connectionId);
                    }
                }
#if JUCE_DEBUG
                else
                {
                    DBG("StageLinQ: Unknown BeatInfo msg 0x"
                        + juce::String::toHexString((int)magic)
                        + " (" + juce::String(blockLen) + " bytes) from " + deviceName);
                }
#endif

                beatReadBuf.erase(beatReadBuf.begin(), beatReadBuf.begin() + 4 + blockLen);
            }
        }
    };

    //==========================================================================
    // Member data
    //==========================================================================

    // Our identity
    uint8_t ownToken[StageLinQ::kTokenLen] = {};

    // Network
    juce::String bindIp;
    int selectedInterface = 0;
    juce::Array<NetworkInterface> availableInterfaces;

    // Discovery socket
    std::unique_ptr<juce::DatagramSocket> discoverySocket;
    std::mutex socketMutex;

    // Discovered devices
    std::map<std::string, StageLinQDeviceInfo> discoveredDevices;
    mutable std::mutex devicesMutex;

    // Per-device connection threads
    std::vector<std::unique_ptr<DeviceConnectionThread>> connectionThreads;
    std::mutex connThreadsMutex;
    uint32_t nextConnectionId = 1;      // guarded by devicesMutex

    // Deck state (decks 1-4 mapped to index 0-3)
    mutable std::array<StageLinQDeckState, StageLinQ::kMaxDecks> decks;

    // Mixer state
    StageLinQMixerState mixerState;

    // Playback speed from BeatInfo, one tracker per STC deck
    std::array<StageLinQ::BeatSpeedTracker, StageLinQ::kMaxDecks> beatSpeedTrackers {};
    std::mutex beatSpeedMutex;

    // Unknown path logging (debug aid -- logs each unknown path once)
#if JUCE_DEBUG
    std::set<std::string> loggedUnknownPaths;
    std::mutex unknownPathsMutex;
#endif

    // Running flag
    std::atomic<bool> isRunningFlag { false };
};
