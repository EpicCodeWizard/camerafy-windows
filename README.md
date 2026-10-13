<p align="center">
  <img src="assets/camerafy-icon.png" width="128" alt="camerafy-windows icon">
</p>

<h1 align="center">camerafy-windows</h1>

<p align="center">The Windows receiver for camerafy-ios. It turns an iPhone into a webcam over the USB cable.</p>

<p align="center">The iPhone app lives in <a href="https://github.com/camerafy/camerafy-ios">camerafy-ios</a>.</p>

## What it does

Plug in an iPhone running [camerafy-ios](https://github.com/camerafy/camerafy-ios) (the app is called Camerafy on the phone) and two webcams show up in Windows. Video travels over the USB cable only, so the receiver works with every network adapter turned off. You need Apple's Apple Devices app installed, because Windows reaches the phone through it.

| Camera | Backend | Seen by |
|---|---|---|
| `<iPhone name> Webcam` | DirectShow source filter, loaded inside the app that uses it | DirectShow apps: OBS, ffmpeg, Zoom, Chrome on most setups |
| `<iPhone name> Webcam MediaFoundation (slow)` | Media Foundation virtual camera, served by the Frame Server | Media Foundation and DirectShow apps, including the Windows Camera app |

The DirectShow camera is the faster one because frames skip the extra Frame Server process.

The receiver decodes the H.264 stream on the GPU with NVDEC and hands each frame to both cameras through shared memory. In testing, the receiver added about 2 ms at 1080p60, most of it decode time.

## Requirements

- Windows 11, build 22000 or later. The Media Foundation virtual camera API starts there.
- An NVIDIA GPU with H.264 NVDEC and [driver 570 or later](https://www.nvidia.com/en-us/drivers/). The driver ships the CUDA and NVDEC DLLs the receiver uses for decoding.
- The [Apple Devices app](https://apps.microsoft.com/detail/9np83lwlpz9k) from the Microsoft Store, or [iTunes](https://www.apple.com/itunes/). This is required: its Apple Mobile Device Service is the only way to reach the phone over USB.
- The [camerafy-ios](https://github.com/camerafy/camerafy-ios) app open on the iPhone. It sends the video.

The receiver downloads nothing at runtime and needs no firewall rule.

## Building

Building needs:

- [Visual Studio 2022 Build Tools](https://visualstudio.microsoft.com/downloads/) with the C++ workload.
- The [Windows SDK](https://developer.microsoft.com/windows/downloads/windows-sdk/), 10.0.22000 or later.
- [CMake](https://cmake.org/download/) 3.25 or later.

| Command | What it builds |
|---|---|
| `cmake --preset x64 && cmake --build --preset x64` | `camerafy.exe`, both DLLs, `ds_probe` and `cf_test_reader` |
| `cmake --preset x86 && cmake --build --preset x86` | The 32-bit DirectShow filter for 32-bit apps |

Third-party code is vendored in `third_party/`. `third_party/PROVENANCE.md` says where each piece came from.

## Registering the cameras

Run `register.cmd` as administrator once per build. It copies the DLLs to `%ProgramFiles%\camerafy` and registers them. The Frame Server loads the Media Foundation camera as a system service, so the DLL has to live somewhere that service can read. `unregister.cmd` undoes all of it.

Only the Media Foundation camera needs this step. If you skip it, `camerafy.exe` registers the DirectShow filter for your user when it starts and removes it again on exit, with no admin rights needed.

## Running

| Command | What it does |
|---|---|
| `camerafy.exe` | Find the phone, create both cameras and stream until Ctrl+C |
| `camerafy.exe --stats` | Also print the per-stage latency table on exit |
| `camerafy.exe --list-devices` | List USB-connected iPhones |
| `camerafy.exe --udid <udid>` | Pick one phone when several are connected |
| `camerafy.exe --cams ds\|mf\|both` | Choose which cameras to create (default: both) |
| `camerafy.exe --keepalive off` | Let the GPU idle (lower power, higher latency) |
| `camerafy.exe --no-boost` | Leave process priorities and the power plan alone |
| `camerafy.exe --no-catchup` | Show every frame after a stall instead of only the newest |
| `camerafy.exe --csv timings.csv` | Export per-frame timings |
| `camerafy.exe --preview` | Open a preview window |
| `camerafy.exe --verify-no-network` | Run for 10 s and list every socket the process opened |
| `camerafy.exe --self-test` | Run the test suite |
| `camerafy.exe --bench` | Run the benchmarks |
| `camerafy.exe --verbose` | Log every event on the frame path |

No admin rights are needed to run it. The cameras exist only while `camerafy.exe` runs, so they disappear when it exits or crashes. Keep the iPhone unlocked with the app open. If you unplug the phone, the cameras stay and the receiver reconnects when it comes back.

By default the receiver raises its own priority, the Apple Mobile Device Service and the Frame Server to HIGH, and switches to the High performance power plan. Never use `--realtime-unsafe` while the phone streams: the REALTIME priority class can freeze the whole PC.

### Tuning

These environment variables override the measured defaults:

- `CAMERAFY_COPY_THREADS` sets the number of frame copy threads (default: the smaller of 6 and a third of the CPU count).
- `CAMERAFY_KA_ON_MS`, `CAMERAFY_KA_OFF_MS` and `CAMERAFY_KA_SLEEP_NS` shape the GPU keepalive kernel. `CAMERAFY_KEEPALIVE_MB`, `_US` and `_REPS` do the same for `--keepalive memset`.
- `CAMERAFY_MEMTRACE=1` prints memory use at startup checkpoints.
- `CAMERAFY_DS_BUFFERS` sets the DirectShow allocator buffer count (default 1).

### Camera names

The cameras take the name the phone reports. Since iOS 16, apps without a special entitlement only see the generic name "iPhone". When that happens the receiver asks the phone's lockdown service for the name you gave it in Settings. `--name` overrides both.

## Testing without a phone

`camerafy.exe --replay testdata\1080p60_4slice_aud.h264 --name iPhone-Sim` plays a recorded stream through the same pipeline at its own frame rate. It creates `iPhone-Sim Webcam` and `iPhone-Sim Webcam MediaFoundation (slow)`.

The repository doesn't include the test streams. Generate them with ffmpeg:

```
mkdir testdata && cd testdata
ffmpeg -f lavfi -i testsrc2=size=1920x1080:rate=60 -t 10 -c:v libx264 -profile:v high -pix_fmt yuv420p -bf 0 -g 600 ^
  -x264-params aud=1:slices=4:repeat-headers=1:keyint=600:min-keyint=600:scenecut=0 -f h264 1080p60_4slice_aud.h264
ffmpeg -f lavfi -i testsrc2=size=1280x720:rate=30 -t 5 -c:v libx264 -profile:v main -bf 0 -g 60 ^
  -x264-params slices=1:scenecut=0 -f h264 720p30_noaud.h264
ffmpeg -f lavfi -i testsrc2=size=1920x1080:rate=60 -t 10 -c:v libx264 -profile:v baseline -pix_fmt yuv420p -bf 0 -g 600 ^
  -b:v 50M -maxrate 100M -bufsize 2M -x264-params aud=1:slices=1:repeat-headers=1:keyint=600:min-keyint=600:scenecut=0 ^
  -f h264 1080p60_cbp_1slice_aud.h264
ffmpeg -i 1080p60_4slice_aud.h264 -pix_fmt nv12 -f rawvideo 1080p60_ref.nv12
```

The third stream matches what the phone sends by default: Constrained Baseline, one slice, 50 Mbps.

## Contention test

`cf_contention.exe` reads the camera the way a DirectShow app does while it loads every CPU core, memory bandwidth, the GPU and PCIe. Run it in a second terminal while `camerafy.exe` streams (or replays):

| Command | What it does |
|---|---|
| `cf_contention.exe` | 8 phases of 15 s: idle, cpu, mem, gpu, gpumem, pcie, all, idle |
| `cf_contention.exe --seconds 30 --phases idle,all,idle` | Custom phase length and order |
| `cf_contention.exe --high` | Run the load threads at HIGH priority |

For each phase it prints the frame rate, dropped frames, latency and frame-interval p99. Compare each phase with `idle`.

## Checking it works

`camerafy.exe --self-test` runs every automated check and prints a pass, fail or skip table. An integration test skips, and says why, when something it needs is missing.

With the phone connected:

1. `ffmpeg -hide_banner -list_devices true -f dshow -i dummy` lists both cameras. The Media Foundation one has a `@device_pnp_` name and the DirectShow one a `@device_sw_` name. `ds_probe.exe` shows the same list.
2. The Windows Camera app shows live video from the Media Foundation camera.
3. `ffplay -f dshow -i video="iPhone-Sim Webcam"` shows live video from the DirectShow camera.
4. Chrome and OBS list and show both cameras.
5. With every network adapter disabled, `camerafy.exe --verify-no-network` reports that only the local Apple Mobile Device Service socket was used, and video still flows.
6. `camerafy.exe --stats` prints the per-stage latency table on exit. The phone-to-PC rows are clock corrected and carry an error bar of half the round trip.
7. `camerafy.exe --stats --duration 600` runs for 10 minutes. `drops ring` should stay at 0.

## Project layout

| Path | What's there |
|---|---|
| `CMakeLists.txt`, `CMakePresets.json` | Build files with x64 and x86 presets |
| `src/common` | Shared headers: IPC layout, SPSC ring, timing, NAL scanner, H.264 parsing |
| `src/cli` | `camerafy.exe`, the receiver |
| `src/mf_source` | `camerafy_mf.dll`, the Media Foundation virtual camera |
| `src/ds_filter` | `camerafy_ds.dll`, the DirectShow source filter (x64 and x86) |
| `src/ds_probe` | `ds_probe.exe`, a DirectShow enumeration and frame-pull test |
| `src/test_reader` | `cf_test_reader.exe`, a shared-memory reader with latency stats |
| `src/contention` | `cf_contention.exe`, the contention test |
| `third_party` | NVDEC headers and the DirectShow base classes |
| `assets` | The app icon |
| `register.cmd`, `unregister.cmd` | Register and remove the cameras |
