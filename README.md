# Super Timecode Converter

A professional timecode routing and conversion tool built with C++ and [JUCE](https://juce.com/). Receives timecode from multiple sources and routes it to multiple outputs simultaneously — ideal for live events, broadcast, post-production, and AV installations.

![Windows](https://img.shields.io/badge/platform-Windows-blue)
![C++](https://img.shields.io/badge/language-C%2B%2B-orange)
![JUCE](https://img.shields.io/badge/framework-JUCE-green)
![License](https://img.shields.io/badge/license-MIT-brightgreen)

---

## Features

### Inputs (select one)
- **MTC (MIDI Time Code)** — receive timecode from any MIDI device
- **Art-Net** — receive Art-Net timecode over the network (configurable interface/port)
- **LTC (Linear Time Code)** — decode LTC audio signal from any audio input device and channel
- **System Time** — use the system clock as a timecode source

### Outputs (enable any combination)
- **MTC Out** — transmit MIDI Time Code (Quarter Frame + Full Frame messages)
- **Art-Net Out** — broadcast ArtTimeCode packets on any network interface
- **LTC Out** — generate LTC audio signal on any audio output device and channel
- **Audio Thru** — passthrough audio from the LTC input device to a separate output device (independent routing)

### Additional Capabilities
- **Frame rate support:** 24, 25, 29.97 (drop-frame), 30 fps
- **Auto-detection:** frame rate is automatically detected from incoming MTC, Art-Net, or LTC signals
- **Per-channel gain control:** independent input/output gain for LTC and Audio Thru paths
- **Stereo or mono output:** configurable per output (LTC Out and Audio Thru)
- **Driver type filtering:** filter audio devices by driver type (WASAPI, ASIO, DirectSound, etc.)
- **Configurable sample rate and buffer size**
- **ASIO support** for low-latency professional audio interfaces
- **Device conflict detection** to prevent multiple outputs from opening the same device
- **Persistent settings** — all configuration is saved automatically and restored on launch
- **Dark theme UI** with a clean, professional look

---

## Screenshot

<!-- Replace with an actual screenshot of the application -->
![Super Timecode Converter](docs/screenshot.png)

---

## Getting Started

### Prerequisites

- **Windows 10/11** (primary platform)
- **Visual Studio 2022** (Community, Professional, or Enterprise)
- **JUCE Framework** — download from [juce.com](https://juce.com/get-juce/)
- **Projucer** (included with JUCE) — used to generate the Visual Studio project
- **ASIO SDK** (optional, for ASIO device support) — download from [Steinberg](https://www.steinberg.net/developers/)

### Build Instructions

1. **Clone the repository:**
   ```bash
   git clone https://github.com/fiverecords/SuperTimecodeConverter.git
   cd SuperTimecodeConverter
   ```

2. **Open the `.jucer` file in Projucer:**
   - Set the JUCE modules path to your local JUCE installation
   - If using ASIO: set the ASIO SDK path in the exporter settings

3. **Export and build:**
   - Click "Save and Open in IDE" in Projucer
   - Build the solution in Visual Studio (Release or Debug)

4. **Run:**
   - The built executable will be in `Builds/VisualStudio2022/x64/Release/` (or Debug)

### ASIO Setup (Optional)

To enable ASIO support:
1. Download the ASIO SDK from Steinberg
2. Extract it to a known path (e.g., `C:\SDKs\asiosdk_2.3.3_2019-06-14`)
3. In Projucer, go to the Visual Studio exporter settings and add the ASIO SDK path to the header search paths
4. Enable `JUCE_ASIO=1` in the project preprocessor definitions

---

## Usage

### Basic Workflow

1. **Select an input source** from the left panel (MTC, Art-Net, System, or LTC)
2. **Enable one or more outputs** from the right panel
3. **Select the frame rate** or let it auto-detect from the input signal
4. The timecode display in the center shows the current time in real-time

### Audio Thru

The Audio Thru feature lets you route audio (e.g., music or program audio) from a channel on the LTC input device to a separate output device. This is useful when your LTC signal arrives on one channel while program audio arrives on another — you can decode LTC and pass through the audio independently.

### Settings

All settings are automatically saved to:
```
%APPDATA%/SuperTimecodeConverter/settings.json
```

---

## Architecture

The application is built around a modular architecture:

| Component | Description |
|---|---|
| `TimecodeCore.h` | Core timecode and frame rate types |
| `TimecodeDisplay.h` | Real-time timecode display widget |
| `MtcInput.h` | MIDI Time Code receiver (Quarter Frame + Full Frame) |
| `MtcOutput.h` | MIDI Time Code transmitter (high-resolution timer) |
| `ArtnetInput.h` | Art-Net timecode receiver (UDP) |
| `ArtnetOutput.h` | Art-Net timecode broadcaster (UDP) |
| `LtcInput.h` | LTC audio decoder with passthrough ring buffer |
| `LtcOutput.h` | LTC audio encoder/generator |
| `AudioThru.h` | Audio passthrough with independent device routing |
| `AppSettings.h` | JSON-based persistent settings |
| `MainComponent.*` | Main UI and routing logic |

### Key Design Decisions

- **Lock-free audio:** LTC decode and audio passthrough use lock-free ring buffers (SPSC) for real-time safety
- **Independent audio devices:** LTC Input, LTC Output, and Audio Thru each manage their own `AudioDeviceManager`, allowing independent device selection
- **Background device scanning:** audio devices are scanned on a background thread to avoid blocking the UI on startup
- **Two-phase initialization:** settings are loaded in two phases — non-audio settings are applied immediately, while audio device settings are applied after the background scan completes

---

## Contributing

Contributions are welcome! Please see [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

---

## License

This project is licensed under the MIT License — see the [LICENSE](LICENSE) file for details.

---

## Credits..

Developed by **Fiverecords** — [fiverecords.com](https://github.com/fiverecords]((https://github.com/fiverecords/SuperTimecodeConverter)

Built with [JUCE](https://juce.com/) — the cross-platform C++ framework for audio applications.

---

## Links

- [GitHub Repository](https://github.com/fiverecords/SuperTimecodeConverter)
- [JUCE Framework](https://juce.com/)
- [Art-Net Protocol](https://art-net.org.uk/)
- [MIDI Time Code Specification](https://en.wikipedia.org/wiki/MIDI_timecode)
