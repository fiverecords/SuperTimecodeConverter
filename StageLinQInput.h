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

#pragma once
#include <JuceHeader.h>
#include "TimecodeCore.h"
#include "NetworkUtils.h"
#include <atomic>
#include <array>
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
    static constexpr uint32_t kSmaaSubscribe        = 0x000007D2;  // us -> device: subscribe request

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

    // Maximum supported decks per device (Prime 4 has 4)
    static constexpr int kMaxDecksPerDevice = 4;

    // Maximum total decks (we map to STC players 1-4)
    static constexpr int kMaxDecks = 4;

    // Maximum mixer channels
    static constexpr int kMaxMixerChannels = 4;

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
    // Encode ASCII/Latin1 string to UTF-16BE bytes.
    // Returns a vector of the UTF-16BE encoded bytes (no length prefix).
    inline std::vector<uint8_t> encodeUTF16BE(const juce::String& str)
    {
        std::vector<uint8_t> result;
        result.reserve(str.length() * 2);
        for (int i = 0; i < str.length(); ++i)
        {
            auto ch = (uint16_t)str[i];
            result.push_back(uint8_t(ch >> 8));
            result.push_back(uint8_t(ch & 0xFF));
        }
        return result;
    }

    // Decode UTF-16BE bytes to a juce::String.
    inline juce::String decodeUTF16BE(const uint8_t* data, int byteLen)
    {
        int numChars = byteLen / 2;
        if (numChars <= 0) return {};

        // Build into a pre-allocated buffer to avoid O(n^2) appends
        juce::String result;
        result.preallocateBytes((size_t)(numChars + 1) * sizeof(juce::juce_wchar));
        for (int i = 0; i + 1 < byteLen; i += 2)
        {
            juce::juce_wchar ch = (juce::juce_wchar(data[i]) << 8) | data[i + 1];
            result += ch;
        }
        return result;
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
        if (offset + 4 > dataLen) return -1;
        uint32_t strLen = readU32BE(data + offset);
        offset += 4;
        if (offset + (int)strLen > dataLen) return -1;
        out = decodeUTF16BE(data + offset, (int)strLen);
        return offset + (int)strLen;
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
        int    asInt()     const { return (type == kInt) ? (int)intVal : (type == kDouble) ? (int)doubleVal : (boolVal ? 1 : 0); }
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
                    // PlayState sends state as integer
                    v.type = JsonValue::kInt;
                    v.intVal = (int64_t)(int)stateVal;
                    v.doubleVal = (double)(int)stateVal;
                }
            }
            else if (obj->hasProperty("value"))
            {
                v.type = JsonValue::kDouble;
                v.doubleVal = (double)obj->getProperty("value");
                v.intVal = (int64_t)v.doubleVal;
            }
            else if (obj->hasProperty("color"))
            {
                v.type = JsonValue::kInt;
                v.intVal = (int64_t)(int)obj->getProperty("color");
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
            v.intVal = (int64_t)v.doubleVal;
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
}

//==============================================================================
// Per-deck state -- atomics for cross-thread access
//==============================================================================
struct StageLinQDeckState
{
    std::atomic<bool>     active { false };       // deck exists / has data
    std::atomic<int>      deckNumber { 0 };       // 1-4

    // Playback
    std::atomic<bool>     isPlaying { false };     // from /Engine/DeckN/Play
    std::atomic<int>      playState { 0 };         // from /Engine/DeckN/PlayState
    std::atomic<double>   currentBPM { 0.0 };      // from /Engine/DeckN/CurrentBPM
    std::atomic<double>   speed { 0.0 };           // from /Engine/DeckN/Speed (pitch multiplier)
    std::atomic<bool>     speedReceived { false };  // true once Speed path has sent at least one value
    std::atomic<int>      speedState { 0 };        // from /Engine/DeckN/SpeedState
    std::atomic<double>   speedNeutral { 1.0 };    // from SpeedNeutral (pitch at 0%)
    std::atomic<double>   speedRange { 0.08 };     // from SpeedRange (+/-8% default)
    std::atomic<double>   speedOffsetUp { 0.0 };   // from SpeedOffsetUp (pitch bend)
    std::atomic<double>   speedOffsetDown { 0.0 }; // from SpeedOffsetDown (pitch bend)
    std::atomic<int>      syncMode { 0 };          // from SyncMode
    std::atomic<bool>     scratchWheelTouch { false }; // from ExternalScratchWheelTouch
    std::atomic<int>      padsView { 0 };          // from Pads/View
    std::atomic<bool>     bleep { false };         // from Track/Bleep (reverse mode)

    // Track metadata
    juce::String          artistName;              // guarded by metaMutex
    juce::String          songName;                // guarded by metaMutex
    juce::String          trackNetworkPath;        // from Track/TrackNetworkPath (guarded by metaMutex)
    juce::String          trackUri;                // from Track/TrackUri (streaming) (guarded by metaMutex)
    juce::String          soundSwitchGuid;         // from Track/SoundSwitchGuid (guarded by metaMutex)
    std::atomic<double>   trackLength { 0.0 };     // seconds, from Track/TrackLength
    std::atomic<bool>     songLoaded { false };     // from Track/SongLoaded
    std::atomic<bool>     songAnalyzed { false };   // from Track/SongAnalyzed
    std::atomic<double>   trackBPM { 0.0 };        // from Track/CurrentBPM
    std::atomic<double>   cuePosition { 0.0 };     // from Track/CuePosition
    std::atomic<int>      currentKeyIndex { -1 };  // from Track/CurrentKeyIndex (live, changes with key shift)
    std::atomic<bool>     keyLock { false };        // from Track/KeyLock
    std::atomic<double>   sampleRate { 44100.0 };  // from Track/SampleRate
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
    std::atomic<double>   beatInfoTimeline { 0.0 };    // timeline position (ms?)

    // Mixer (per-channel)
    std::atomic<double>   faderPosition { 0.0 };   // from /Mixer/CH{N}faderPosition (0-1)
    std::atomic<double>   externalVolume { 0.0 };   // from ExternalMixerVolume
    std::atomic<bool>     isMaster { false };        // from /Client/DeckN/DeckIsMaster
    std::atomic<int>      channelAssignment { 0 };   // from /Mixer/ChannelAssignment{N} (deck->channel map)

    // Timing
    std::atomic<double>   lastUpdateTime { 0.0 };     // juce hiRes ms
    std::atomic<uint32_t> trackVersion { 0 };          // incremented on track change

    // Derived playhead in ms (computed from beatInfo timeline or speed+time)
    std::atomic<uint32_t> playheadMs { 0 };

    mutable std::mutex metaMutex;

    void reset()
    {
        active.store(false, std::memory_order_release);
        deckNumber.store(0);
        isPlaying.store(false);
        playState.store(0);
        currentBPM.store(0.0);
        speed.store(0.0);
        speedReceived.store(false);
        speedState.store(0);
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
        trackLength.store(0.0);
        songLoaded.store(false);
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
        faderPosition.store(0.0);
        externalVolume.store(0.0);
        isMaster.store(false);
        channelAssignment.store(0);
        lastUpdateTime.store(0.0);
        trackVersion.store(0);
        playheadMs.store(0);
    }
};

//==============================================================================
// Mixer state (crossfader, global)
//==============================================================================
struct StageLinQMixerState
{
    std::atomic<double> crossfaderPosition { 0.0 };  // 0=left, 0.5=center, 1=right
    std::atomic<double> masterBPM { 0.0 };           // from /Engine/Master/MasterTempo
    std::atomic<int>    numChannels { 0 };            // from /Mixer/NumberOfChannels

    // Deck ring LED colors (from /Client/Preferences/Profile/Application/PlayerColorN)
    // Index 0-3 = deck 1-4.  Each has base, A (layer A), B (layer B) variants.
    // Value is an integer color from Engine OS (interpretation TBD with hardware).
    std::atomic<int>    playerColor[4]  = {};    // PlayerColor1-4
    std::atomic<int>    playerColorA[4] = {};    // PlayerColor1A-4A
    std::atomic<int>    playerColorB[4] = {};    // PlayerColor1B-4B
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

        // Reset all decks
        for (auto& d : decks) d.reset();

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

        // Close all device connections
        closeAllDeviceConnections();

        stopThread(3000);

        for (auto& d : decks) d.reset();

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

    double getActualSpeed(int deckNum) const
    {
        int idx = deckNum - 1;
        if (idx < 0 || idx >= StageLinQ::kMaxDecks) return 0.0;
        double spd = decks[idx].speed.load(std::memory_order_relaxed);
        // Fall back to 1.0 ONLY if the Speed path has NEVER sent a value.
        // Some firmware versions may not emit Speed at all (chrisle/StageLinq
        // doesn't even subscribe to it).  Without the fallback, the PLL gets
        // 0 velocity forever and timecode interpolation stops.
        //
        // Previously this checked isPlaying instead of speedReceived, which
        // caused a race: if Speed=0.0 (pause) arrived before Play=false
        // (two different StateMap paths, order not guaranteed), the fallback
        // returned 1.0 -- telling the PLL we're at full speed when actually
        // stopped.  Using speedReceived avoids this: once Speed has sent any
        // value (including 0.0 for pause), we trust it.
        if (spd == 0.0 && !decks[idx].speedReceived.load(std::memory_order_relaxed))
            return 1.0;
        return spd;
    }

    uint32_t getTrackLengthSec(int deckNum) const
    {
        int idx = deckNum - 1;
        if (idx < 0 || idx >= StageLinQ::kMaxDecks) return 0;
        return (uint32_t)decks[idx].trackLength.load(std::memory_order_relaxed);
    }

    float getPlayPositionRatio(int deckNum) const
    {
        int idx = deckNum - 1;
        if (idx < 0 || idx >= StageLinQ::kMaxDecks) return 0.0f;
        double len = decks[idx].trackLength.load(std::memory_order_relaxed);
        if (len <= 0.0) return 0.0f;
        double posMs = (double)decks[idx].playheadMs.load(std::memory_order_relaxed);
        return juce::jlimit(0.0f, 1.0f, (float)(posMs / (len * 1000.0)));
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

        // Return device name from discovery.
        // TODO: with multi-device, track which device sent each deck's data
        // to return the correct model per deck.
        std::lock_guard<std::mutex> lock(devicesMutex);
        if (!discoveredDevices.empty())
            return discoveredDevices.begin()->second.deviceName;
        return "Denon";
    }

    double getCrossfaderPosition() const
    {
        return mixerState.crossfaderPosition.load(std::memory_order_relaxed);
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

    double getFaderPosition(int channel) const
    {
        int idx = channel - 1;
        if (idx < 0 || idx >= StageLinQ::kMaxDecks) return 0.0;
        return decks[idx].faderPosition.load(std::memory_order_relaxed);
    }

    /// Crossfader assignment for a channel.
    /// Values assumed to be: 0=THRU, 1=A, 2=B (same as Pioneer DJM).
    /// Not yet confirmed with real hardware -- awaiting community testing.
    int getChannelAssignment(int channel) const
    {
        int idx = channel - 1;
        if (idx < 0 || idx >= StageLinQ::kMaxDecks) return 0;
        return decks[idx].channelAssignment.load(std::memory_order_relaxed);
    }

    /// Derive on-air status from fader + crossfader + channel assignment.
    /// A deck is "on-air" when its fader is up AND it's not cut by crossfader.
    bool isDeckOnAir(int deckNum) const
    {
        static constexpr double kFaderThreshold = 0.02;  // ~2% above zero
        static constexpr double kXfCutThreshold = 0.02;  // crossfader fully to opposite side

        int idx = deckNum - 1;
        if (idx < 0 || idx >= StageLinQ::kMaxDecks) return false;

        double fader = decks[idx].faderPosition.load(std::memory_order_relaxed);
        if (fader < kFaderThreshold) return false;  // fader down = not on-air

        int assign = decks[idx].channelAssignment.load(std::memory_order_relaxed);
        double xf = mixerState.crossfaderPosition.load(std::memory_order_relaxed);

        // Assumed: 0=THRU, 1=A (left), 2=B (right)
        if (assign == 1 && xf > (1.0 - kXfCutThreshold)) return false;  // A-side, xf fully right
        if (assign == 2 && xf < kXfCutThreshold)          return false;  // B-side, xf fully left

        return true;
    }

    /// Returns true if we have mixer fader data (any fader ever received)
    bool hasMixerData() const
    {
        return mixerState.numChannels.load(std::memory_order_relaxed) > 0;
    }

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

    // Timeline position from BeatInfo (milliseconds)
    double getAbsPositionTs(int deckNum) const
    {
        int idx = deckNum - 1;
        if (idx < 0 || idx >= StageLinQ::kMaxDecks) return 0.0;
        return decks[idx].lastUpdateTime.load(std::memory_order_relaxed);
    }

    bool isPositionMoving(int deckNum) const
    {
        return isPlayerPlaying(deckNum);
    }

    // For compatibility: always true for StageLinQ (position comes from BeatInfo timeline)
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
        if (beat < 0.0) return 1;
        int beatInBar = ((int)beat % 4) + 1;
        return (uint8_t)juce::jlimit(1, 4, beatInBar);
    }

    // Set track length from TrackMap (same pattern as ProDJLinkInput)
    void setTrackLengthSec(int deckNum, uint32_t seconds)
    {
        int idx = deckNum - 1;
        if (idx < 0 || idx >= StageLinQ::kMaxDecks) return;
        decks[idx].trackLength.store((double)seconds, std::memory_order_relaxed);
    }

private:
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
        std::lock_guard<std::mutex> lock(devicesMutex);

        double now = juce::Time::getMillisecondCounterHiRes();

        for (auto& [ip, dev] : discoveredDevices)
        {
            // Skip already connected
            if (dev.connected) continue;

            // Skip devices without service port
            if (dev.servicePort == 0) continue;

            // Only attempt connection if we've seen a discovery frame recently
            // (prevents reconnect loops to devices that have gone offline)
            double age = now - dev.lastSeenTime;
            if (age > StageLinQ::kDeviceTimeoutSec * 1000.0) continue;

            // Wait 500ms after first discovery for stability, and enforce a
            // minimum cooldown between reconnect attempts
            if (age < 500.0) continue;
            if (dev.lastConnectAttempt > 0.0
                && (now - dev.lastConnectAttempt) < StageLinQ::kReconnectDelay * 1000.0)
                continue;

            // Connect in a background thread to avoid blocking discovery
            dev.connected = true;  // Mark as connecting to prevent re-entry
            dev.lastConnectAttempt = now;

            // Kill any stale thread for the same IP before launching a new one
            // (can happen if device did EXIT + re-announce faster than thread teardown)
            {
                std::lock_guard<std::mutex> cLock(connThreadsMutex);
                for (auto& ct : connectionThreads)
                {
                    if (ct && ct->getDeviceIp() == dev.ip && ct->isThreadRunning())
                    {
                        DBG("StageLinQ: Stopping stale thread for " + dev.ip + " before reconnect");
                        ct->signalThreadShouldExit();
                        ct->closeSocket();
                    }
                }
            }

            // Launch connection thread (make_unique first for exception safety --
            // if push_back throws during vector realloc, the thread is still owned)
            auto connThread = std::make_unique<DeviceConnectionThread>(*this, dev);
            auto* connPtr = connThread.get();
            {
                std::lock_guard<std::mutex> cLock(connThreadsMutex);
                connectionThreads.push_back(std::move(connThread));
            }
            connPtr->startThread();
        }

        // Prune stopped connection threads to prevent unbounded growth
        {
            std::lock_guard<std::mutex> cLock(connThreadsMutex);
            connectionThreads.erase(
                std::remove_if(connectionThreads.begin(), connectionThreads.end(),
                    [](const std::unique_ptr<DeviceConnectionThread>& ct) {
                        return !ct->isThreadRunning();
                    }),
                connectionThreads.end());
        }
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
        std::lock_guard<std::mutex> lock(connThreadsMutex);
        for (auto& ct : connectionThreads)
        {
            ct->signalThreadShouldExit();
            ct->closeSocket();
        }
        for (auto& ct : connectionThreads)
        {
            ct->stopThread(2000);
        }
        connectionThreads.clear();
    }

    //==========================================================================
    // Update derived state (playhead from BeatInfo timeline)
    //==========================================================================
    void updateDerivedState()
    {
        for (int i = 0; i < StageLinQ::kMaxDecks; ++i)
        {
            auto& dk = decks[i];
            if (!dk.active.load(std::memory_order_relaxed)) continue;

            // Compute playhead from BeatInfo: beat position + BPM.
            // This is the most reliable method -- both values are confirmed
            // correct by all three reference implementations.
            //
            // The "timeline" field (called "samples" in chrisle/StageLinq) has
            // unknown units -- could be audio samples, not milliseconds.
            // Using beat/BPM avoids the ambiguity entirely.
            double beat = dk.beatInfoBeat.load(std::memory_order_relaxed);
            double bpm  = dk.beatInfoBPM.load(std::memory_order_relaxed);

            // Gate on BPM only -- BPM > 0 means BeatInfo is active and a track
            // is loaded.  beat=0 is valid (track cued to the very beginning).
            if (bpm > 0.0)
            {
                // beat is the current beat position (e.g. 128.5 = halfway through beat 129)
                // playheadMs = (beat / BPM) * 60000
                // Clamp beat >= 0 -- negative values (scratch before start) would
                // cause undefined behavior when cast to uint32_t.
                double clampedBeat = juce::jmax(0.0, beat);
                double ms = (clampedBeat / bpm) * 60000.0;
                dk.playheadMs.store((uint32_t)ms, std::memory_order_relaxed);
            }
        }
    }

    //==========================================================================
    // Handle a StateMap value update from a device
    //==========================================================================
    void handleStateMapValue(const juce::String& path, const StageLinQ::JsonValue& value,
                             int deckOffset = 0)
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
                dk.speedState.store(value.asInt(), std::memory_order_relaxed);
            }
            else if (sub == "Track/ArtistName")
            {
                std::lock_guard<std::mutex> lock(dk.metaMutex);
                juce::String newArtist = value.asString();
                if (newArtist != dk.artistName)
                {
                    dk.artistName = newArtist;
                    dk.trackVersion.fetch_add(1, std::memory_order_relaxed);
                }
            }
            else if (sub == "Track/SongName")
            {
                std::lock_guard<std::mutex> lock(dk.metaMutex);
                juce::String newTitle = value.asString();
                if (newTitle != dk.songName)
                {
                    dk.songName = newTitle;
                    dk.trackVersion.fetch_add(1, std::memory_order_relaxed);
                }
            }
            else if (sub == "Track/TrackLength")
            {
                dk.trackLength.store(value.asDouble(), std::memory_order_relaxed);
            }
            else if (sub == "Track/SongLoaded")
            {
                bool newLoaded = value.asBool();
                bool wasLoaded = dk.songLoaded.load(std::memory_order_relaxed);
                dk.songLoaded.store(newLoaded, std::memory_order_relaxed);
                if (newLoaded && !wasLoaded)
                    dk.trackVersion.fetch_add(1, std::memory_order_relaxed);
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
                decks[ch - 1].faderPosition.store(value.asDouble(), std::memory_order_relaxed);
        }
        else if (path == "/Mixer/CrossfaderPosition")
        {
            mixerState.crossfaderPosition.store(value.asDouble(), std::memory_order_relaxed);
        }
        else if (path.startsWith("/Mixer/ChannelAssignment"))
        {
            // /Mixer/ChannelAssignment1 -> channel 1 is assigned to deck N
            int ch = path[24] - '0';
            if (ch >= 1 && ch <= StageLinQ::kMaxMixerChannels)
                decks[ch - 1].channelAssignment.store(value.asInt(), std::memory_order_relaxed);
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
    void handleBeatInfo(uint64_t /*clock*/, const std::vector<PlayerInfo>& players,
                        const std::vector<double>& timelines, int deckOffset = 0)
    {
        int numDecks = juce::jmin((int)players.size(), StageLinQ::kMaxDecks);
        for (int i = 0; i < numDecks; ++i)
        {
            int mapped = i + deckOffset;
            if (mapped < 0 || mapped >= StageLinQ::kMaxDecks) continue;

            auto& dk = decks[mapped];
            dk.active.store(true, std::memory_order_relaxed);
            dk.deckNumber.store(mapped + 1, std::memory_order_relaxed);
            dk.beatInfoBeat.store(players[i].beat, std::memory_order_relaxed);
            dk.beatInfoTotalBeats.store(players[i].totalBeats, std::memory_order_relaxed);
            dk.beatInfoBPM.store(players[i].bpm, std::memory_order_relaxed);
            dk.lastUpdateTime.store(juce::Time::getMillisecondCounterHiRes(), std::memory_order_relaxed);

            if (i < (int)timelines.size())
                dk.beatInfoTimeline.store(timelines[i], std::memory_order_relaxed);
        }
    }

    //==========================================================================
    // Per-device TCP connection thread
    //==========================================================================
    class DeviceConnectionThread : public juce::Thread
    {
    public:
        DeviceConnectionThread(StageLinQInput& owner, const StageLinQDeviceInfo& device)
            : Thread("SLQ-" + device.ip.fromLastOccurrenceOf(".", false, false)),
              owner(owner), deviceIp(device.ip), devicePort(device.servicePort)
        {
            std::memcpy(deviceToken, device.token, StageLinQ::kTokenLen);
            deviceName = device.deviceName;
        }

        ~DeviceConnectionThread() override
        {
            stopThread(2000);
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
                        juce::Thread::sleep(500);
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
            juce::Thread::sleep(500);
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

                // Detect dead service sockets.  If StateMap dies (the primary
                // data channel), the connection is useless -- exit the loop so
                // markDisconnected() fires and manageConnections() can relaunch.
                // Without this check the thread becomes a zombie: keepalives
                // keep the main socket alive but no deck data flows, and
                // dev.connected stays true so no reconnect is attempted.
                // chrisle and go-stagelinq both tear down the entire connection
                // when any service socket fails.
                {
                    std::lock_guard<std::mutex> lock(sockMutex);
                    bool stateMapLost = stateMapPort > 0
                        && (!stateSocket || !stateSocket->isConnected());
                    bool beatInfoLost = beatInfoPort > 0
                        && (!beatSocket || !beatSocket->isConnected());

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
        uint8_t deviceToken[StageLinQ::kTokenLen] = {};
        juce::String deviceName;

        // Multi-device deck mapping:
        // SC6000 player 1 -> deckOffset=0 (STC decks 1-2)
        // SC6000 player 2 -> deckOffset=2 (STC decks 3-4)
        // Prime 4 player 1 -> deckOffset=0 (STC decks 1-4)
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
        void markDisconnected()
        {
            std::lock_guard<std::mutex> lock(owner.devicesMutex);
            auto it = owner.discoveredDevices.find(deviceIp.toStdString());
            if (it != owner.discoveredDevices.end())
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

                        // Reference after services = end of list
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
            if (!mainSocket->waitUntilReady(true, 1)) return;

            uint8_t tmp[4096];
            int bytesRead = mainSocket->read(tmp, sizeof(tmp), false);
            if (bytesRead <= 0) return;

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
        bool tcpReadAvailable(juce::StreamingSocket* sock, std::vector<uint8_t>& buf)
        {
            if (!sock || !sock->isConnected()) return false;

            if (!sock->waitUntilReady(true, 5))
                return true;  // no data, but no error

            uint8_t tmp[8192];
            int bytesRead = sock->read(tmp, sizeof(tmp), false);
            if (bytesRead <= 0)
                return false;  // connection closed or error

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
                if (blockLen == 0 || blockLen > 65536)
                {
                    // Malformed or corrupt -- scan forward for "smaa" magic to
                    // re-synchronize, instead of blind 4-byte skip which can
                    // burn CPU if the stream is badly corrupted.
                    bool resynced = false;
                    for (size_t i = 4; i + 4 <= stateReadBuf.size(); ++i)
                    {
                        if (std::memcmp(stateReadBuf.data() + i, StageLinQ::kSmaaMagic, 4) == 0)
                        {
                            // Found smaa at offset i -- the length field is 4 bytes before it
                            size_t frameStart = i - 4;
                            if (frameStart > 0)
                                stateReadBuf.erase(stateReadBuf.begin(),
                                                   stateReadBuf.begin() + (int)frameStart);
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

                                owner.handleStateMapValue(path, val, deckOffset);
                            }
                        }
                    }
                    // kSmaaEmitResponse (0x7D1) -- subscription ack, ignore
#if JUCE_DEBUG
                    else if (subType != StageLinQ::kSmaaEmitResponse)
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
                if (blockLen == 0 || blockLen > 65536)
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
                    // Each player record: beat[8] + totalBeats[8] + bpm[8] = 24 bytes
                    int playerDataSize = (int)numRecords * 24;
                    int timelineDataSize = (int)numRecords * 8;

                    if (pos + playerDataSize + timelineDataSize <= (int)blockLen)
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

                        owner.handleBeatInfo(clock, players, timelines, deckOffset);
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

    // Deck state (decks 1-4 mapped to index 0-3)
    mutable std::array<StageLinQDeckState, StageLinQ::kMaxDecks> decks;

    // Mixer state
    StageLinQMixerState mixerState;

    // Unknown path logging (debug aid -- logs each unknown path once)
#if JUCE_DEBUG
    std::set<std::string> loggedUnknownPaths;
    std::mutex unknownPathsMutex;
#endif

    // Running flag
    std::atomic<bool> isRunningFlag { false };
};
