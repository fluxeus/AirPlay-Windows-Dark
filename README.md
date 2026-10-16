# AirPlay Windows Receiver

Native AirPlay receiver for Windows: screen mirroring, RAOP audio and MP4
recording, based on [UxPlay](https://github.com/FDH2/UxPlay).

Latest release: [v0.3.0](https://github.com/moieric11/AirPlay-Windows/releases/tag/v0.3.0)
— [what changed](docs/releases/v0.3.0.md).

## Get started

1. Download the Windows x64 ZIP from [Releases](https://github.com/moieric11/AirPlay-Windows/releases).
2. Extract the complete ZIP and run `airplay-windows.exe`, keeping the supplied DLLs alongside it.
3. Connect the sending device and the PC to the same local network.
4. Open Screen Mirroring on the sending device and select `AirPlay-Windows`.

The receiver uses native Windows mDNS; Bonjour SDK and Apple software are not
required for normal network mirroring. The optional USB Personal Hotspot route
has separate driver requirements described below.

**Runtime:** Windows 10 1809+ or Windows 11, x64. GPU features require compatible
graphics hardware and drivers. Software decoding is available as a fallback.

## Features and current limits

| Feature | Current behavior |
|---|---|
| Screen mirroring | H.264 and HEVC, resizable window, preserved aspect ratio |
| GPU display | D3D11VA decode, GPU color conversion and shared GPU textures without CPU pixel readback |
| Audio | RAOP AAC-ELD/ALAC, stereo output through SDL/WASAPI, sender volume updates |
| Music information | Cover art, title/artist/album, elapsed/total time and pause state when supplied by the sender |
| Display controls | Fullscreen, borderless Hide UI, automatic/manual rotation |
| Recording | MP4 video with a stereo AAC audio track when RAOP audio is received |
| Settings | Saved in `%APPDATA%\AirPlay-Windows\settings.ini`, including rotation and display preferences |
| Sessions | One active device session at a time |
| Direct HLS playback | Experimental, opt-in with `--hls-proxy-playback` |
| DRM-protected streaming | Not supported |

The sidebar shows the active device. **Disconnect** drops the active AirPlay
connection; the sending device may reconnect automatically. Sender/app behavior
can differ between screen mirroring and direct media streaming.

## Display and resolution

In **OPTIONS > Rotation**, choose **Automatic (device)** or a clockwise manual
rotation of **0, 90, 180 or 270 degrees** relative to the received image.
Automatic follows the orientation encoded in the incoming video, not the
device's physical orientation sensor. Changes apply immediately to display and
are saved when the application closes. Manual display rotation does not rotate
recorded video.

**Mirror Resolution** advertises a maximum stream box, not the sending device's
native screen resolution. The device chooses the actual resolution and aspect
ratio. Square limits allow room for both portrait and landscape; rectangular
presets and custom dimensions remain available. The panel shows the received
resolution. Reconnect the sender after changing the resolution.

## GPU decoding and display

Enable **OPTIONS > Mirror Decoder > GPU decode**, then reconnect the sender.
Decoded D3D11 surfaces remain on the GPU through color conversion and display.
The decoder and SDL renderer use the same graphics adapter, shared textures and
GPU fences. Decoder surface views and render targets are reused, and the display
queue is limited to one frame.

Software decoding remains selectable. **VSYNC** is a separate display setting:
enabled synchronizes presentation to the display; disabled can reduce waiting
but may introduce tearing. Keep the setting that works best on your display.

The status bar's **Lat** value is a local pipeline estimate. It does not measure
the sender's capture time, network transit time or exact display scan-out.

## MP4 recording

Use **REC** or `R` to start/stop recording. In **OPTIONS > Recording**:

- Select the destination with **Browse...**. The default is the executable's folder.
- Select **H.264** or **H.265/HEVC**, encoder mode **Auto / GPU / CPU**, and a 2–40 Mbps bitrate.
- Settings selected while recording apply to the next recording.

The recording keeps its initial canvas size. Incoming resolution/orientation
changes are fitted into that canvas with letterboxing and without stretching.
RAOP audio is recorded as stereo AAC at 192 kbps when received, even if Windows
has no available playback device. There is no microphone or system-audio capture.

With GPU decoding and a compatible encoder, scaling, color conversion and
encoding stay on the GPU. The recorder uses its own surfaces so encoder
buffering does not retain the decoder's surface pool. Encoding runs on a bounded
worker queue; frames can be dropped if recording cannot keep up.

The actual path is shown in the recording codec status:

| Status | Meaning |
|---|---|
| **[GPU resident]** | GPU-decoded pixels remain on the GPU through video encoding |
| **[GPU encoder, CPU pixels]** | Hardware encoding with frames prepared in system memory |
| **[CPU]** | Software encoding |

**GPU** mode does not silently download GPU-decoded images: it reports an error
if no compatible direct encoder is available. **Auto** may fall back to a CPU
pixel path. Audio encoding and writing compressed MP4 packets still use the CPU.

Encoder availability depends on the FFmpeg build and GPU. The repository's
vcpkg manifest enables NVIDIA NVENC. The recorder can also try AMD AMF, Intel
Quick Sync or Media Foundation when the FFmpeg build exposes them; direct GPU
recording additionally requires an encoder accepting D3D11 surfaces. CPU H.264
selection can fall back to MPEG-4 Part 2 if `libx264` is unavailable; CPU HEVC
requires `libx265`. The codec status identifies the encoder actually opened.

The GPU path has been tested on an NVIDIA GeForce RTX 3080 Laptop GPU. Local
tests cover both H.264 and HEVC recording, with zero CPU pixel readbacks in the
resident path.

## Shortcuts

| Key | Action |
|---|---|
| `H` | Toggle borderless Hide UI; retain dragging/resizing and video aspect ratio |
| `F` / `F11` / double-click | Toggle fullscreen |
| `R` | Start/stop MP4 recording |
| `ESC` | Exit fullscreen, then Hide UI, then quit |

## HLS and protected content

Normal operation advertises screen mirroring and RAOP audio. The experimental
`--hls-proxy-playback` flag also enables a direct media URL/HLS proxy path.
Compatibility and buffering depend on the sending app and stream.

HLS playback uses **libavformat** unless the project was built with the optional
GStreamer backend. **HLS backend** in the status bar identifies that media-player
backend; it does not identify the mirror decoder and stays unchanged when GPU
mirror decoding is selected.

The bundled FairPlay pairing/mirror handshake is not a DRM streaming license.
DRM-protected direct streaming is unsupported. A successful screen-mirroring
connection does not guarantee that an app will allow its protected video to be
captured or played.

## Logs and command-line options

The released application opens without a console. Use `--log` or `--verbose`
for startup/runtime logging when launched from a terminal:

```powershell
.\airplay-windows.exe --log
```

| Option | Purpose |
|---|---|
| `--log`, `--verbose` | Enable logs |
| `--mirror-hwaccel` | Request GPU mirror decoding |
| `--mirror-res WxH`, `-r WxH` | Override the advertised mirror resolution |
| `--hls-proxy-playback`, `--hls` | Enable experimental direct HLS playback |
| `--help`, `-h` | Show command-line help |

Keep `identity.key` private to your installation: it is generated at first run
and helps senders recognize the receiver across restarts. It is not included in
the release ZIP. Software decoding can also write a first-frame PPM diagnostic;
GPU decoding skips this diagnostic to avoid a readback.

## Networking and USB

Normal use requires local discovery and connectivity between the sender and PC.
Allow the application through Windows Firewall on the intended local network.
Guest-network isolation, multicast filtering and VPN routing can prevent discovery.
The main receiver listens on TCP 7000; media sessions allocate additional ports.

A USB cable is usable through **Apple Personal Hotspot networking**, rather than
a direct USB video protocol implemented by this application:

1. Install Apple Devices or iTunes on Windows for the USB networking drivers.
2. Connect a supported device by USB and accept **Trust This Computer** when prompted.
3. Enable **Personal Hotspot > Allow Others to Join** on the device.
4. Confirm Windows has a working Apple USB Ethernet connection, then select
   `AirPlay-Windows` in Screen Mirroring.

Availability depends on the device, hotspot support, drivers and network setup;
Apple documents the [USB Personal Hotspot requirements](https://support.apple.com/en-au/111785).
The USB supervisor only detects devices and logs a reminder. It does not install
drivers or implement native QuickTime-style USB capture. Do not assume fixed
interface addresses or that enabling a hotspot prevents other PC traffic from
using cellular data.

## Build from source

Use Visual Studio 2022 or 2026 / Build Tools with the desktop C++ workload and a
Windows SDK. The project requires CMake 3.21 or newer; your CMake must also
support the selected Visual Studio generator. Git and network access are needed
for the first dependency install.

The commands below use a fresh vcpkg checkout pinned to the manifest baseline,
matching the GitHub build rather than relying on a bundled vcpkg version:

```powershell
git clone https://github.com/moieric11/AirPlay-Windows.git
cd AirPlay-Windows
git clone https://github.com/microsoft/vcpkg.git build/vcpkg
$baseline = (Get-Content vcpkg.json | ConvertFrom-Json).'builtin-baseline'
git -C build/vcpkg checkout $baseline
.\build\vcpkg\bootstrap-vcpkg.bat -disableMetrics

cmake -S . -B build `
  -DCMAKE_TOOLCHAIN_FILE="$PWD/build/vcpkg/scripts/buildsystems/vcpkg.cmake" `
  -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
.\build\Release\airplay-windows.exe
```

For Visual Studio 2026, use `-G "Visual Studio 18 2026"` with a CMake version
supporting that generator. vcpkg builds libplist, OpenSSL, FFmpeg, SDL2 and
SDL2_ttf; the first install can take several minutes and later builds use caches.
The `/fp-setup` reply data and playfair implementation are bundled in
`third_party/`; no manual provisioning is needed.

GStreamer is optional: CMake's `USE_GSTREAMER_HLS=AUTO` uses it when found and
otherwise selects libavformat. Use `ON` to require it or `OFF` to disable it.

## Tests

The protocol suite simulates a sender for pairing, corrupted-signature handling,
concurrent connections, FairPlay setup and the legacy audio handshake. Start the
receiver first; the suite expects `127.0.0.1:7000`:

```powershell
python -m pip install cryptography
python tools/test_pair_verify.py
```

Socket shutdown regression tests are available with `-DBUILD_NETWORK_TESTS=ON`.
Build Release and run `ctest --test-dir build -C Release --output-on-failure`.
On Windows, `build/Release/test-shutdown.exe --mdns` also exercises real DNS-SD
registration/deregistration; inspect the callback status logs. These checks do
not validate the mirroring indicator on an actual iOS device.

For Windows GPU integration tests, configure with `-DBUILD_GPU_TESTS=ON`, build
`test-gpu-video` in Release and run `build/Release/test-gpu-video.exe`. These tests
require a compatible NVIDIA GPU and NVENC-enabled FFmpeg for their fixtures.
They verify CPU/GPU decoding, codec/resolution switches, texture array slices,
color range, rotations, concurrent display/recording, MP4 pixels, AAC audio and
timestamps, and zero-readback counters. Pixel transfers inside the tests are
validation instrumentation only. Add `--benchmark` for local 1080p timings;
these are not end-to-end AirPlay latency measurements.

GitHub CI builds on Windows and runs the protocol suite. The tag workflow builds
the release ZIP and SHA-256 checksum; hardware integration tests run locally on
compatible GPU hardware.

## Project notes

One active device session is supported. Multi-device routing and broader
experimental HLS compatibility remain areas for future work; metadata progress
display and MP4 recording are already implemented.

Protocol and implementation details:
[PROTOCOL.md](docs/PROTOCOL.md), [FAIRPLAY.md](docs/FAIRPLAY.md).
Primary references: [UxPlay](https://github.com/FDH2/UxPlay),
[RPiPlay](https://github.com/FD-/RPiPlay),
[OpenAirplay](https://openairplay.github.io/airplay-spec/),
[playfair](https://github.com/EstebanKubata/playfair) and
[shairport-sync](https://github.com/mikebrady/shairport-sync).

## License

Project code is GPL-3.0; see [LICENSE](LICENSE). Component origins and licensing,
including the bundled FairPlay reply data, are documented in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). No Apple runtime is needed for
normal network mirroring; the Apple USB driver requirement applies only to the
optional Personal Hotspot route.
