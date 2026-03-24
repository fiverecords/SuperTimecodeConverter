# Super Timecode Converter

A professional timecode routing and conversion tool built with C++ and [JUCE](https://juce.com/). Run up to **8 independent timecode engines** simultaneously — each with its own input source, output destinations, frame rate, and offset. Connect directly to **Pioneer CDJ/DJM hardware** via native Pro DJ Link integration and to **Denon Engine OS hardware** via StageLinQ — no additional software required. Ideal for live events, broadcast, post-production, and AV installations.

[![GitHub Downloads](https://img.shields.io/github/downloads/fiverecords/SuperTimecodeConverter/total?label=Downloads&color=blue)](https://github.com/fiverecords/SuperTimecodeConverter/releases)
[![Latest Release](https://img.shields.io/github/v/release/fiverecords/SuperTimecodeConverter?label=Release&color=blue)](https://github.com/fiverecords/SuperTimecodeConverter/releases/latest)
[![License](https://img.shields.io/badge/license-MIT-brightgreen)](LICENSE)
![Windows](https://img.shields.io/badge/platform-Windows-blue)
![macOS](https://img.shields.io/badge/platform-macOS-lightgrey)
![Linux](https://img.shields.io/badge/platform-Linux-yellow)
![C++17](https://img.shields.io/badge/language-C%2B%2B17-orange)
![JUCE 8](https://img.shields.io/badge/framework-JUCE%208-green)
![Ableton Link](https://img.shields.io/badge/Ableton_Link-Supported-brightgreen)
![Pro DJ Link](https://img.shields.io/badge/Pro_DJ_Link-Native-00BCD4)
![StageLinQ](https://img.shields.io/badge/StageLinQ-Native-00CC66)

![Super Timecode Converter](docs/screenshotV1.5.png)

-----

## Features

### Multi-Engine Routing

Run **up to 8 independent timecode engines** simultaneously. Each engine has its own input source, frame rate, output destinations, and offset settings — all running in parallel.

- Route system clock to MTC on one engine while converting LTC input to Art-Net on another
- Feed the same source to multiple outputs with different frame offsets
- Run independent MTC, Art-Net, and LTC pipelines side by side — each with its own device assignments
- Monitor different CDJ or Denon players on separate engines — each generating independent timecode streams

Engines are managed through the tab bar at the top of the window. Click **+** to add a new engine, right-click a tab to rename or remove it. The dashboard in the centre shows all engines at a glance with their current timecodes and status.

Audio passthrough (channel 2 thru) remains tied to the primary engine (Engine 1), since it shares the audio device with LTC input.

### Inputs (select one per engine)

- **Pro DJ Link** — connect directly to Pioneer CDJ/XDJ/DJM hardware on the network (see below)
- **StageLinQ** — connect directly to Denon Engine OS hardware on the network (see below)
- **MTC (MIDI Time Code)** — receive timecode from any MIDI device
- **Art-Net** — receive Art-Net timecode over the network (configurable interface/port)
- **LTC (Linear Time Code)** — decode LTC audio signal from any audio input device and channel
- **System Time** — use the system clock as a timecode source

### Outputs (enable any combination per engine)

- **MTC Out** — transmit MIDI Time Code (Quarter Frame + Full Frame messages)
- **Art-Net Out** — broadcast ArtTimeCode packets on any network interface
- **LTC Out** — generate LTC audio signal on any audio output device and channel
- **TCNet Out** — broadcast TCNet timecode, playhead, BPM, and beat data (see below)
- **Audio Thru** — passthrough audio from the LTC input device to a separate output device (Engine 1 only, since it shares the audio device with LTC input)

### Pro DJ Link Integration

STC connects directly to Pioneer CDJ and DJM hardware on the network as a Virtual CDJ, converting the DJ’s playhead position into frame-accurate SMPTE timecode in real time.

**Tested hardware:** CDJ-3000, CDJ-3000X, DJM-900NXS2, DJM-V10, DJM-A9. Other Pro DJ Link compatible hardware (CDJ-2000NXS2, XDJ series, etc.) should work but has not been verified yet – please report any issues on GitHub.

**Player tracking:**

- Automatic player discovery on the Pro DJ Link network
- Absolute position tracking from CDJ-3000 / CDJ-3000X (30Hz, millisecond precision)
- Beat-derived position for NXS2 and older models
- Play state detection: Playing, Paused, Cued, Looping, Seeking, End of Track
- BPM, pitch fader, actual playback speed (including motor ramp)
- On-air status, master player detection, beat position
- Per-player monitoring: any engine can follow any of 6 players independently
- **Crossfader auto-follow (XF-A / XF-B):** instead of monitoring a fixed player, an engine can follow whichever deck is live on crossfader side A or B. When the DJ swaps decks, timecode seamlessly switches to the new player. Ideal for two-engine setups where each engine tracks one side of the crossfader.

**Smooth timecode generation:**

- Direct CDJ playhead display with linear interpolation between packets (60Hz smooth output from 30Hz CDJ data)
- PLL-driven pitch calculation for LTC bit-rate scaling
- Instant resync on seek, hot cue, or track load
- Clean pause/stop handling: outputs decelerate naturally following the CDJ motor ramp

**DJM mixer integration:**

- Real-time mixer data from DJM-900NXS2, DJM-A9, and DJM-V10
- **DJM-900NXS2:** per-channel faders, trim, EQ (3-band), color/FX, CUE buttons, crossfader, master fader, booth, headphones, beat FX, color FX, mic EQ (58 parameters)
- **DJM-A9:** all of the above plus CUE A/B dual-cue, master CUE B, headphone B, booth EQ, multi I/O, Bluetooth and USB input sources. Beat FX names match the A9 panel (including Triplet Filter, Triplet Roll, Mobius). Mic FX section is internal to the mixer and not available via protocol (71 parameters).
- **DJM-V10:** all of the above plus compressor, EQ (4-band), send, isolator, filter, master mix (sends), channels 5-6 (123 parameters). Auto-detected from DJM model name.
- VU meters: per-channel peak metering + Master L/R stereo, 15 segments per channel. 4-channel (900NXS2, A9) or 6-channel (V10) layouts.
- Per-channel on-air status

**Track metadata (via dbserver):**

- Artist, title, album, genre, key, BPM, duration
- Album artwork
- Color preview waveform (ThreeBand and ColorNxs2 formats)

### StageLinQ Integration (Denon) – Preliminary

STC connects to Denon Engine OS hardware via the StageLinQ protocol, receiving deck state, track metadata, mixer fader positions, and beat information in real time.

**Note:** This implementation is based entirely on open-source protocol references and has not yet been tested with real Denon hardware. If you have access to Denon Prime equipment, please try it and report results on [GitHub](https://github.com/fiverecords/SuperTimecodeConverter/issues).

**Supported hardware:** SC5000, SC6000, SC6000M, LC6000, Prime 4, Prime 2, Prime Go, X1800, X1850. Other StageLinQ-compatible hardware should work but has not been verified yet – please report any issues on GitHub.

**Deck state (via StateMap service):**

- Play/pause/cue state per deck (up to 4 decks per device)
- Current BPM, pitch/speed, speed state
- Track metadata: artist, title, duration, loaded state
- Channel fader positions and crossfader position

**Beat information (via BeatInfo service):**

- Real-time beat position, total beats, and BPM per deck
- Timeline position for playhead tracking
- Beat-in-bar derived from beat position

**Protocol notes:**

- Discovery via UDP broadcast on port 51337
- StateMap and BeatInfo data received over persistent TCP connections
- All strings encoded as UTF-16BE with JSON values
- No additional software or hardware bridge required
- StageLinQ implementation based on open-source reverse-engineering work by chrisle/StageLinq, icedream/go-stagelinq, and Jaxc/PyStageLinQ (all MIT licensed)

**Track metadata (via FileTransfer + Engine Library database):**

- Album artwork
- Overview waveform preview (3-band frequency display)
- Extended metadata: album, genre, key, BPM, rating

**Known limitations:**

- No DJM-style mixer parameter mapping (Denon mixers expose basic fader data only via StateMap)
- XF-A/XF-B crossfader auto-follow is preliminary – channel assignment values (0=THRU, 1=A, 2=B) assumed from Pioneer convention, awaiting confirmation with real Denon hardware

### Track Map

Map tracks by **artist + title + duration** to timecode offsets and show control triggers. When a mapped track is loaded on a CDJ or Denon deck, STC automatically applies the timecode offset and fires the configured triggers. Tracks are identified universally regardless of which USB/SD they are loaded from. The duration acts as a fingerprint to distinguish different versions of the same track (e.g. radio edit vs extended mix).

- Per-track timecode offset (HH:MM:SS:FF)
- Per-track BPM multiplier (/4, /2, 1x, x2, x4) – applied to MIDI Clock, Ableton Link, and OSC BPM forward
- Duration-based track identification – same artist+title with different lengths are treated as separate tracks
- Learn mode: capture tracks live from any CDJ or Denon deck (auto-captures duration)
- Auto-fill artist/title from CDJ metadata

**Per-track triggers (any combination, fired simultaneously on track change):**

- MIDI Note On (+ immediate Note Off)
- MIDI CC (controller + value)
- OSC (address + typed arguments with variable expansion)
- Art-Net DMX (channel + value, configurable universe)

### Cue Points

Per-track timed triggers that fire at specific playhead positions during playback. While track-change triggers fire once when a track is loaded, cue points fire at precise moments within the track – ideal for lighting cues, pyro triggers, video transitions, and show automation.

- **Capture from playhead:** play the track on a CDJ or Denon deck, press Capture at each key moment. STC reads the current playhead position and creates a cue point there.
- **Visual cue placement:** click or drag anywhere on the waveform strip to place an edit cursor, then Capture to create a cue at that exact position. The edit cursor clears automatically when the playhead moves (jog, scrub, play) or on click outside the waveform.
- **Waveform strip** with four cursor layers: playback position (red, real-time), edit cursor (cyan, click-to-place), cue markers (yellow), selected cue (white, highlighted for identification).
- **Album artwork** displayed in the editor header from live CDJ/Denon or from cached PNG when offline.
- **Multi-selection:** Ctrl+click and Shift+click to select multiple cues. Delete button removes all selected cues at once.
- **Manual entry:** add cue points by typing the position (MM:SS.mmm) directly.
- **Same trigger types as track change:** MIDI Note, MIDI CC, OSC, Art-Net DMX – any combination per cue point.
- **Seek-aware:** seeking backward resets cues so they fire again. Seeking forward skips already-passed cues.
- **Playback-only firing:** cue points only fire during actual playback. Scrub, jog, and cue preview do not trigger cues – DJs can preview tracks freely without causing spurious output.
- **Live editing:** cue points added or modified while a track is playing take effect immediately without reloading the track.
- **Waveform and artwork cache:** waveform preview data and album artwork are saved to disk the first time a track is seen. The cue editor shows both even when the CDJ is not connected, enabling offline cue programming.
- **Works with both Pioneer and Denon** hardware via Pro DJ Link and StageLinQ.
- Cue points are stored in the Track Map (trackmap.json) alongside the track’s offset and track-change triggers.
- Maximizable window with persisted position across sessions.
- Blocked during Show Lock to prevent accidental changes during a live show.

### Mixer Map

Configurable mapping from every DJM mixer parameter to show control protocols. From 58 parameters (DJM-900NXS2) to 123 (DJM-V10), each independently routed to any combination of:

- **OSC** – configurable address per parameter
- **MIDI CC** – configurable CC number and channel
- **MIDI Note** – velocity-based (for grandMA2/MA3 executor faders)
- **Art-Net DMX** – configurable DMX channel and universe

Value mapping is automatic based on parameter type:

|Type      |Examples                                               |OSC      |MIDI CC      |DMX      |
|----------|-------------------------------------------------------|---------|-------------|---------|
|Continuous|faders, EQ, knobs, levels                              |0.0-1.0  |0-127        |0-255    |
|Toggle    |CUE buttons, Beat FX On, Isolator On, Filter LPF/HPF   |0.0 / 1.0|0 / 127      |0 / 255  |
|Discrete  |Beat FX Select, XF Assign, Color FX Select, Fader Curve|integer  |0-127 clamped|raw value|

Table editor with per-parameter enable/disable, editable addresses and CC/Note/DMX numbers. Values only sent when changed. Three-way DJM model toggle (DJM-900NXS2 / DJM-A9 / DJM-V10) shows or hides model-specific parameters. Export and import mixer maps as JSON files for backup or sharing between machines.

### PDL View

External window showing the full Pro DJ Link network state at 30Hz:

- 4-deck display: artwork, color waveform with playhead, track info, BPM (with multiplied value when active), play state, engine assignments
- Per-deck BPM multiplier buttons (single click for session override, double click to save to Track Map)
- Mixer strip: channel faders with segmented VU meters, crossfader, master fader with stereo VU
- DJM-V10 enhanced view: compressor, 4-band EQ, send knobs, dual CUE A/B buttons per channel
- DJM-A9 / DJM-V10: dual CUE A/B buttons rendered automatically when detected
- On-air, master, and beat indicators per player

### BPM Multiplier

Scale the BPM sent to MIDI Clock, Ableton Link, and OSC before forwarding — useful for half-time, double-time, or genre-specific workflows where the DJ’s BPM doesn’t match the lighting or video system’s expected tempo.

- **5 multiplier values:** /4, /2, 1x (passthrough), x2, x4
- **Single click** — temporary session override (cleared on next track load)
- **Double click** — save to Track Map (automatically applied every time the track is loaded on any engine)
- **Double click on 1x** — clears any saved multiplier from the Track Map
- Available in both the engine panel and PDL View
- Visual feedback: active multiplier highlighted in blue, saved Track Map value always shown in gold text
- Effective (multiplied) BPM displayed next to the original BPM in both views: `128.0 BPM → 256.0 (x2)`

### Ableton Link

BPM from the selected CDJ player or Denon deck is published to an Ableton Link session. Any Link-enabled peer on the LAN (Resolume, Ableton Live, Traktor, etc.) syncs automatically.

### MIDI Clock & OSC BPM Forward

- **MIDI Clock:** 24ppqn clock driven by CDJ or Denon deck BPM
- **OSC BPM:** sends current BPM as float to configurable OSC address (default: Resolume tempo controller)

### TCNet Output

Full TCNet server for direct integration with Resolume Arena, ChamSys, Avolites, madMapper, and other TCNet-compatible lighting and video systems. No intermediate hardware or bridge software required – STC replaces the PRO DJ LINK Bridge entirely.

**Why TCNet instead of LTC/MTC for video?**

- **60Hz position updates** vs 24-30Hz with LTC/MTC – Resolume interpolates between samples, enabling smooth 60fps video playback even from a 24fps timecode source. This breaks the traditional 30fps ceiling that LTC imposed on Resolume clips.
- **Automatic clip triggering** – Resolume assigns clips by track name. When a CDJ loads a track, Resolume can automatically trigger the matching clip. For non-DJ sources, the engine name becomes the track title, allowing pre-mapped clip assignments per engine.
- **Track metadata** – artist, title, and album artwork appear in Resolume’s deck display. CDJ/Denon tracks show real metadata; non-DJ sources show the input type and engine name.
- **Play/pause sync** – Resolume follows the CDJ’s transport state in real time. Pause on the CDJ pauses the clip in Resolume.
- **Millisecond position** – TCNet carries playhead position in milliseconds, not frame-quantized SMPTE. Resolume receives sub-frame precision for smoother scrubbing and seeking.
- **Fader-controlled opacity** – when a DJM or Denon mixer is connected, the channel fader position drives clip opacity in Resolume. Pull down the fader, the video fades out. No mixer = always fully visible.

**Architecture:**

- Broadcast: OptIn + Status on port 60000 (1Hz), Time on port 60001 (60Hz)
- Unicast: automatic slave discovery, Request/Response negotiation, Metrics streaming at 30Hz, Metadata + Artwork on track change
- Per-engine toggle “TCNET OUT” in the outputs panel with layer selector (1-4) and network interface selector
- Works with all input sources: Pro DJ Link, StageLinQ, MTC, Art-Net, LTC, System Time

Protocol reference: https://www.tc-supply.com/tcnet

### Shared MIDI Output

When MTC output and MIDI triggers/clock/mixer forward target the same MIDI port, STC automatically shares the connection. No configuration needed — both features work simultaneously on a single port, even on Windows where MIDI ports allow only one handle at a time.

### Frame Rate Support

- **23.976 fps** (24000/1001) — cinema and digital cinema workflows
- **24 fps** — film
- **25 fps** — PAL / EBU broadcast
- **29.97 fps drop-frame** — NTSC broadcast (SMPTE-compliant DF counting)
- **30 fps** — non-drop NTSC and general use
- **Auto-detection** from incoming MTC, Art-Net, or LTC signals
- **Output frame rate conversion** — independently convert the output rate from the input rate (e.g. receive 25fps LTC, transmit 29.97 MTC)

> **Note on 23.976 and LTC:** The LTC bitstream cannot distinguish 23.976 from 24fps. When receiving LTC at either of these rates, select the correct rate manually — the app will preserve your selection and suppress auto-detection for that ambiguous pair only.

### Audio Monitoring

- **VU Meters** — real-time level metering for all audio paths (LTC input, LTC output, Audio Thru input, Audio Thru output) with colour-coded feedback (green → yellow → red)
- **Per-channel gain control** — independent input/output gain for LTC and Audio Thru paths

### Synchronization

- **Output frame offsets** — independent offset per output (MTC, Art-Net, LTC) from −30 to +30 frames, to compensate for device latency or synchronization differences

### Show Lock

Prevents accidental changes during live shows. A single click on the **SHOW LOCK** button in the tab bar locks all configuration: input sources, frame rates, device assignments, engine add/remove, TrackMap editor, output frame offsets, integration toggles (OSC, MIDI clock, MIDI/ArtNet mixer forward, Ableton Link, triggers), and backup/restore. Attempting a blocked action flashes the lock button as visual feedback; ComboBox and slider selections revert automatically.

Operational controls remain active while locked: output enable/disable (for emergency silencing), gain sliders, BPM multiplier, freewheel controls, and view windows. Unlock requires a confirmation dialog to prevent accidental unlock. Lock state persists across restarts.

### Additional Capabilities

- **Stereo or mono output:** configurable per output (LTC Out and Audio Thru)
- **Driver type filtering:** filter audio devices by driver type (WASAPI, ASIO, DirectSound on Windows; CoreAudio on macOS; ALSA on Linux)
- **Configurable sample rate and buffer size**
- **ASIO support** for low-latency professional audio interfaces (Windows)
- **Cross-engine device conflict detection** — device selectors show which devices are in use by other engines with colour-coded indicators (cyan for current engine, amber for other engines)
- **Check for updates** — manually check for new versions from the title bar, with automatic check on startup
- **Refresh Devices** — scan for newly connected interfaces without losing existing configuration
- **Collapsible UI panels** to reduce clutter and focus on active sections
- **Persistent settings** — all configuration saved automatically per engine and restored on launch
- **Dark theme UI** with a clean, professional look

-----

## Getting Started

### Prerequisites

- **JUCE Framework 8.x** — download from [juce.com](https://juce.com/get-juce/) or clone from [GitHub](https://github.com/juce-framework/JUCE)

#### Windows

- Windows 10/11
- Visual Studio 2022 (Community, Professional, or Enterprise)
- ASIO SDK (optional, for ASIO device support) — download from [Steinberg](https://www.steinberg.net/developers/)

#### macOS

- macOS 12 Monterey or later
- Xcode 14+

#### Linux

- Ubuntu 22.04+ / Debian 12+ (or equivalent)
- GCC 11+ or Clang 14+
- Development packages: `libfreetype-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxcomposite-dev libfontconfig1-dev libasound2-dev libcurl4-openssl-dev`

### Build Instructions

#### Option A: CMake (all platforms)

1. **Clone the repository:**
   
   ```bash
   git clone https://github.com/fiverecords/SuperTimecodeConverter.git
   cd SuperTimecodeConverter
   ```
1. **Clone JUCE** (if you don’t have it already):
   
   ```bash
   git clone --depth 1 --branch 8.0.6 https://github.com/juce-framework/JUCE.git ../JUCE
   ```
1. **Create a `CMakeLists.txt`** in the project root:
   
   ```cmake
   cmake_minimum_required(VERSION 3.22)
   project(SuperTimecodeConverter VERSION 1.7.0)
   
   set(CMAKE_CXX_STANDARD 17)
   set(CMAKE_CXX_STANDARD_REQUIRED ON)
   
   add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../JUCE ${CMAKE_BINARY_DIR}/JUCE)
   
   juce_add_gui_app(SuperTimecodeConverter
       PRODUCT_NAME "Super Timecode Converter"
       COMPANY_NAME "Fiverecords"
       VERSION "1.7.0"
   )
   
   juce_generate_juce_header(SuperTimecodeConverter)
   
   target_sources(SuperTimecodeConverter PRIVATE
       Main.cpp
       MainComponent.cpp
       sqlite3.c
   )
   
   target_compile_definitions(SuperTimecodeConverter PRIVATE
       JUCE_WEB_BROWSER=0
       JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:SuperTimecodeConverter,JUCE_PRODUCT_NAME>"
       JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:SuperTimecodeConverter,JUCE_VERSION>"
       SQLITE_THREADSAFE=1
   )
   
   target_link_libraries(SuperTimecodeConverter PRIVATE
       juce::juce_audio_basics
       juce::juce_audio_devices
       juce::juce_audio_formats
       juce::juce_audio_utils
       juce::juce_core
       juce::juce_events
       juce::juce_graphics
       juce::juce_gui_basics
       juce::juce_gui_extra
       juce::juce_cryptography
       juce::juce_recommended_config_flags
   )
   ```
1. **Build:**
   
   ```bash
   cmake -B build -DCMAKE_BUILD_TYPE=Release
   cmake --build build -j$(nproc)
   ```
1. The binary will be in `build/SuperTimecodeConverter_artefacts/Release/`

#### Option B: Projucer (Windows / macOS)

1. Clone the repository.
1. Open Projucer and create a new GUI Application project pointing to the source files.
1. Set the JUCE modules path to your local JUCE installation. If using ASIO on Windows, set the ASIO SDK path in the exporter settings.
1. Click “Save and Open in IDE” and build in Visual Studio or Xcode.

### ASIO Setup (Windows, Optional)

1. Download the ASIO SDK from Steinberg.
1. Extract it to a known path (e.g. `C:\SDKs\asiosdk_2.3.3_2019-06-14`).
1. Add the ASIO SDK path to your project’s header search paths.
1. Enable `JUCE_ASIO=1` in the project preprocessor definitions.

### Ableton Link (Optional)

To enable Ableton Link tempo sync:

1. Clone the Link SDK: `git clone --recurse-submodules https://github.com/Ableton/link.git`
1. Add include paths: `<link_repo>/include` and `<link_repo>/modules/asio-standalone/asio/include`
1. Define `STC_ENABLE_LINK=1` (plus `LINK_PLATFORM_WINDOWS=1`, `_WIN32_WINNT=0x0602`, `NOMINMAX=1` on Windows)
1. Link against `ws2_32`, `iphlpapi`, `winmm` on Windows (no extra libraries on macOS/Linux)

Without these steps, Link is compiled as a no-op stub and the rest of the app works normally.

-----

## Usage

### Basic Workflow

1. **Engine 1** is ready by default. Click **+** in the tab bar to add more engines (up to 8).
1. **Select an input source** from the left panel (Pro DJ Link, MTC, Art-Net, System, or LTC).
1. **Enable one or more outputs** from the right panel.
1. **Select the frame rate** or let it auto-detect from the input signal.
1. The timecode display in the centre shows the current time in real-time.
1. Switch between engines via the tab bar. The dashboard shows all engines at a glance.

### Pro DJ Link Workflow

1. Connect your computer to the same network as your CDJ/DJM setup.
1. **macOS only:** make sure the firewall is disabled or STC is in the allowed list (see [Known Issues](#known-issues--platform-notes)).
1. Select **Pro DJ Link** as input source and choose the network interface.
1. STC will discover players automatically. Select which player (1–6) to follow.
1. Timecode is generated from the CDJ playhead position — load a track, press play, and timecode flows.
1. Open **PDL View** for a full network overview with waveforms, artwork, and mixer state.
1. Open **Track Map** to assign timecode offsets and triggers per track.
1. Open **Mixer Map** to route DJM fader data to OSC, MIDI, or DMX.

### Output Frame Rate Conversion

Enable **FPS Convert** to re-stamp outgoing timecode at a different frame rate from the input. For example, receive 25fps LTC and simultaneously transmit 29.97 MTC and 30fps Art-Net. Each output independently applies this conversion before its frame offset.

### Output Frame Offsets

Each output (MTC, Art-Net, LTC) has an independent frame offset control (−30 to +30 frames). Use this to compensate for device latency or to intentionally advance/delay timecode to specific destinations. Double-click the offset slider to reset to zero.

### Audio Thru

Routes audio from a channel on the LTC input device to a separate output device. Useful when LTC and program audio share a physical device — you can decode LTC on one channel and simultaneously pass through the audio on another.

### Refresh Devices

The **Refresh Devices** button scans for newly connected MIDI devices, audio interfaces, and network interfaces without disrupting existing configuration. Already-configured devices remain selected if they are still present.

### Configuration Backup & Restore

The **Backup** and **Restore** buttons in the title bar let you export and import the entire STC configuration as a single JSON file. The backup bundles all engine settings, Track Map entries, and Mixer Map mappings into one portable file (`stc_backup.json`). Useful for migrating to a new machine, keeping a safety copy before a show, or sharing a known-good setup between systems. Restore replaces all config files and prompts for a restart to fully apply changes.

### Settings

All settings are automatically saved per engine to:

- **Windows:** `%APPDATA%\SuperTimecodeConverter\settings.json`
- **macOS:** `~/Library/Application Support/SuperTimecodeConverter/settings.json`
- **Linux:** `~/.local/share/SuperTimecodeConverter/settings.json`

-----

## Known Issues & Platform Notes

### macOS: Disable the Firewall for Pro DJ Link

**This is critical for reliable timecode on macOS.** The macOS application firewall inspects every incoming UDP packet before delivering it to the application. STC’s Pro DJ Link network thread receives hundreds of packets per second (CDJ status at 30Hz per player, beat data, DJM mixer/VU data). The firewall inspection adds latency to each packet, and under load the firewall thread itself consumes significant CPU – enough to delay packet delivery to STC and cause timecode fluctuation, missed beats, and sluggish waveform cursor updates.

**Workaround:** Disable the macOS firewall entirely, or add STC to the firewall’s allowed applications list:

1. Open **System Settings > Network > Firewall**
1. Either turn the firewall **Off**, or click **Options…** and add Super Timecode Converter to the allowed list

On Windows this is not an issue – Windows Firewall prompts once on first launch and does not intercept packets after the rule is created.

### macOS: Unsigned Application

STC is open-source software and is not signed with an Apple Developer certificate. On first launch, macOS Gatekeeper will block the application. To open it:

1. Right-click (or Control-click) the application and select **Open**
1. Click **Open** in the confirmation dialog
1. Alternatively: **System Settings > Privacy & Security > Security**, find the blocked app and click **Open Anyway**

This is a one-time step – macOS remembers the exception after the first launch.

### macOS: DJM Subscribe Race Condition

On macOS, the DJM-900NXS2 / DJM-A9 / DJM-V10 may occasionally fail to deliver mixer fader data on the first connection after the DJM is powered on. This is a timing issue in the subscribe handshake. Workaround: restart STC or toggle the Pro DJ Link interface off and on. A delayed-subscribe fix is planned for a future release.

-----

## Virtual Routing (Software Loopback)

When STC runs on the same machine as your lighting console, DAW, or VJ software, you need virtual devices to route MTC and LTC between applications. These are free tools that create loopback devices on your system:

### MIDI (for MTC output to lighting/VJ software on the same machine)

|Platform|Tool                                                             |Notes                                                                                                                      |
|--------|-----------------------------------------------------------------|---------------------------------------------------------------------------------------------------------------------------|
|Windows |[loopMIDI](https://www.tobias-erichsen.de/software/loopmidi.html)|Creates virtual MIDI ports. Free, lightweight, runs in system tray.                                                        |
|macOS   |IAC Driver (built-in)                                            |Open **Audio MIDI Setup** > **MIDI Studio** > double-click **IAC Driver** > enable **Device is online**. No install needed.|
|Linux   |`snd-virmidi` kernel module                                      |`sudo modprobe snd-virmidi`. Creates virtual MIDI ports automatically.                                                     |

### Audio (for LTC output to other software on the same machine)

|Platform|Tool                                             |Notes                                                                                      |
|--------|-------------------------------------------------|-------------------------------------------------------------------------------------------|
|Windows |[VB-CABLE](https://vb-audio.com/Cable/)          |Virtual audio cable. Free for one cable, donationware for additional cables.               |
|macOS   |[BlackHole](https://existential.audio/blackhole/)|Open-source virtual audio driver (2ch or 16ch). Free.                                      |
|Linux   |PulseAudio / PipeWire                            |Built-in loopback modules. `pactl load-module module-loopback` or PipeWire virtual devices.|

STC outputs MTC and LTC on standard system devices. Select the virtual device as STC’s output, and the same virtual device as input in your receiving application (e.g. grandMA2/3, Resolume, Reaper, ChamSys).

-----

## Architecture

The application is built around a modular, header-only architecture:

|Component            |Description                                                                                                                                |
|---------------------|-------------------------------------------------------------------------------------------------------------------------------------------|
|`TimecodeCore.h`     |Core timecode types, frame rate utilities, SMPTE drop-frame logic, atomic pack/unpack helpers                                              |
|`TimecodeEngine.h`   |Per-engine state container: input/output routing, PLL, TrackMap/MixerMap forwarding                                                        |
|`ProDJLinkInput.h`   |Native Pro DJ Link protocol: player discovery, status, absolute position, DJM mixer/VU data                                                |
|`DbServerClient.h`   |Background TCP client for CDJ metadata queries (title, artist, artwork, waveform)                                                          |
|`MtcInput.h`         |MIDI Time Code receiver (Quarter Frame + Full Frame) with interpolation                                                                    |
|`MtcOutput.h`        |MIDI Time Code transmitter (high-resolution timer with fractional accumulator)                                                             |
|`ArtnetInput.h`      |Art-Net timecode receiver (UDP) with bind fallback                                                                                         |
|`ArtnetOutput.h`     |Art-Net timecode and DMX broadcaster (UDP) with drift-free timing                                                                          |
|`TCNetOutput.h`      |Full TCNet server: broadcast + unicast with slave discovery, Metrics streaming, Metadata, Artwork                                          |
|`StcLogoData.h`      |Embedded STC logo JPEG (300x300) for TCNet artwork fallback                                                                                |
|`LtcInput.h`         |LTC audio decoder with passthrough ring buffer (SPSC)                                                                                      |
|`LtcOutput.h`        |LTC audio encoder with auto-increment and biphase parity                                                                                   |
|`AudioThru.h`        |Audio passthrough with independent device routing (Engine 1 only)                                                                          |
|`NetworkUtils.h`     |Cross-platform network interface enumeration (Windows / macOS / Linux)                                                                     |
|`AppSettings.h`      |JSON-based persistent settings, TrackMap and TrackMapEntry types                                                                           |
|`MixerMap.h`         |DJM parameter mapping with three-tier model support (900NXS2 / A9 / V10) and ParamType-aware value mapping (Continuous / Toggle / Discrete)|
|`OscSender.h`        |Lightweight OSC 1.0 sender (int32, float32, string arguments)                                                                              |
|`TriggerOutput.h`    |MIDI and OSC dispatch for track change triggers + continuous mixer forwarding                                                              |
|`LinkBridge.h`       |Ableton Link tempo sync (compile-time optional, no-op stub when disabled)                                                                  |
|`ProDJLinkView.h`    |External window: 4-deck display with waveforms, artwork, mixer strip with VU meters (A9/V10 enhanced)                                      |
|`MediaDisplay.h`     |Color waveform renderer (ThreeBand and ColorNxs2 formats)                                                                                  |
|`WaveformCache.h`    |Disk cache for waveform preview data and album artwork (offline cue editing)                                                               |
|`TrackMapEditor.h`   |Table editor for artist+title -> timecode offset + trigger mapping                                                                         |
|`CuePointEditor.h`   |Table editor for per-track cue points with waveform strip, click + drag cursor, Capture from live playhead                                 |
|`MixerMapEditor.h`   |Table editor for DJM parameter -> protocol output mapping                                                                                  |
|`TimecodeDisplay.h`  |Real-time timecode display widget                                                                                                          |
|`LevelMeter.h`       |Real-time VU meter component with clipping indicator                                                                                       |
|`CustomLookAndFeel.h`|Dark theme UI styling, device conflict markers, and cross-platform font selection                                                          |
|`UpdateChecker.h`    |GitHub release version checker (automatic on startup + manual)                                                                             |
|`MainComponent.*`    |Main UI, engine tab management, routing logic, and device management                                                                       |

### Key Design Decisions

- **Multi-engine architecture:** each engine encapsulates its own input/output state, enabling up to 8 independent timecode pipelines
- **Lock-free audio:** LTC decode and audio passthrough use lock-free ring buffers (SPSC) for real-time safety
- **Thread safety:** atomics with explicit memory ordering for cross-thread data, SpinLocks for composite structures, SPSC queues for producer-consumer patterns
- **Independent audio devices:** LTC Input, LTC Output, and Audio Thru each manage their own `AudioDeviceManager`, allowing independent device selection
- **Fractional accumulators:** MTC and Art-Net outputs use fractional timing accumulators to eliminate drift from integer-ms timer resolution
- **Native rendering:** OpenGL context intentionally disabled to prevent GL-thread data races on juce::String refcounts (paint() vs timerCallback()). Windows DWM already hardware-accelerates the native GDI composite path, and the waveform/deck image caches minimize per-frame paint work
- **PLL-based timecode:** Pro DJ Link input uses a phase-locked loop driven by CDJ actual motor speed for jitter-free LTC bit-rate scaling
- **Interface-bound sockets:** Pro DJ Link UDP sockets (beat, status, bridge) are bound to the specific network interface IP, not INADDR_ANY, preventing duplicate packet delivery on multi-interface systems. The keepalive socket binds to the interface IP on Windows (to force the correct outgoing NIC on multi-adapter systems) but to INADDR_ANY on macOS (where broadcast reception requires it). Beat and status sockets avoid SO_REUSEPORT to prevent kernel packet distribution across stale/duplicate sockets on macOS
- **Background device scanning:** audio devices are scanned on a background thread to avoid blocking the UI on startup
- **Two-phase initialization:** non-audio settings are applied immediately; audio device settings are applied after the background scan completes
- **Cross-engine device conflict detection:** custom popup menu rendering highlights devices in use with colour-coded markers (cyan for current engine, amber with engine name for others)
- **Cross-platform:** built with JUCE for native performance on Windows, macOS, and Linux

-----

## Contributing

Contributions are welcome! Please see <CONTRIBUTING.md> for guidelines.

-----

## License

This project is licensed under the MIT License — see the <LICENSE> file for details.

-----

## Disclaimer

This project is **not affiliated with, endorsed by, or associated with AlphaTheta Corporation or Pioneer DJ** in any way. PRO DJ LINK is a trademark of AlphaTheta Corporation. Pioneer DJ is a trademark of Pioneer Corporation, used under license by AlphaTheta Corporation.

This project is **not affiliated with, endorsed by, or associated with inMusic Brands, Inc., Denon DJ, or Engine DJ** in any way. Denon is a trademark of D&M Holdings Inc. Denon DJ and Engine DJ are trademarks of inMusic Brands, Inc. StageLinQ is a protocol developed by inMusic/Denon DJ for their Engine OS hardware.

This project is **not affiliated with, endorsed by, or associated with TC-Supply or Event Imagineering Group** in any way. TCNet is an open protocol created by TC-Supply / Event Imagineering Group.

This project is **not affiliated with, endorsed by, or associated with Resolume B.V.** in any way. Resolume Arena is a trademark of Resolume B.V.

Ableton Link is a trademark of Ableton AG. This project is not affiliated with, endorsed by, or associated with Ableton AG. The Link SDK is used under the terms of the Ableton Link License.

ChamSys, Avolites, madMapper, and all other product names, trademarks, and registered trademarks mentioned in this project are the property of their respective owners.

This project has not been developed using any proprietary documentation, SDK, or confidential information from any of the above companies. The Pro DJ Link and StageLinQ implementations are based on independent community research. The TCNet implementation is based on the [TCNet Link Specification V3.5.1B](https://www.tc-supply.com/tcnet) (open protocol, free to use).

**Use at your own risk.** This software communicates with DJ hardware and lighting/video systems using a combination of documented open protocols (TCNet) and undocumented protocols (Pro DJ Link, StageLinQ). While it has been tested with the hardware listed above, behaviour may change with future firmware updates or on untested hardware. The authors accept no responsibility for any issues arising from the use of this software.

-----

## Credits

Developed by **Joaquin Villodre** – [github.com/fiverecords](https://github.com/fiverecords)

Built with [JUCE](https://juce.com/) – the cross-platform C++ framework for audio applications.

The Pro DJ Link implementation would not have been possible without the incredible protocol documentation by **Deep Symmetry** – their [DJ Link Ecosystem Analysis](https://djl-analysis.deepsymmetry.org/djl-analysis/) provided the foundation for understanding the Pioneer protocol and paved the way for this integration.

The StageLinQ implementation is built on the open-source reverse-engineering work of three projects: **chrisle/StageLinq** by Chris Le and Martijn Reuvers (TypeScript, the most complete implementation including FileTransfer and database access), **icedream/go-stagelinq** by Carl Kittelberger (Go, clean protocol reference and BeatInfo), and **Jaxc/PyStageLinQ** by Jaxc (Python, byte-level protocol documentation and Wireshark dissector). Their collective work made third-party StageLinQ integration possible.

-----

## Links

- [GitHub Repository](https://github.com/fiverecords/SuperTimecodeConverter)
- [Deep Symmetry – DJ Link Ecosystem Analysis](https://djl-analysis.deepsymmetry.org/djl-analysis/)
- [chrisle/StageLinq – TypeScript StageLinQ library](https://github.com/chrisle/StageLinq)
- [icedream/go-stagelinq – Go StageLinQ library](https://github.com/icedream/go-stagelinq)
- [Jaxc/PyStageLinQ – Python StageLinQ library](https://github.com/Jaxc/PyStageLinQ)
- [JUCE Framework](https://juce.com/)
- [Art-Net Protocol](https://art-net.org.uk/)
- [TCNet Protocol Specification](https://www.tc-supply.com/tcnet)
- [MIDI Time Code Specification](https://en.wikipedia.org/wiki/MIDI_timecode)