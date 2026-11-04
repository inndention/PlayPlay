# libobs as a live-input layer — hybrid spike (2026-10-26)

**Question.** Could PlayPlay use libobs (OBS Studio's core) for composing and switching live cameras, filters and
stingers without giving up its own frame-exact scheduled playout? The agreed shape is a **hybrid**, not a swap:
libobs runs headless *inside* the channel engine as **one live input source**; its scene graph renders free-running
on its own thread; each frame it produces is pulled into the PlayPlay compositor as a layer exactly the way an NDI
input is (newest frame wins, the clock thread never waits).

**Answer in one line.** It works and it is cheap enough (libobs itself ≈ 15 % of one core, ≈ 150 MB), the engine
stayed frame-exact with the layer on air (0 late ticks), a cut lands 2 frames after the command, and the price of the
hybrid is a picture that is 1.3–1.9 frames old at pull time — see [Measurements](#4-measurements) and
[Recommendation](#7-recommendation).

Everything here is behind `-DPLAYPLAY_WITH_LIBOBS=ON` (default OFF). With the option off, the lane fragment adds
nothing to any target and the only source touched (`sourcefactory.cpp`) compiles to the same preprocessed text.

---

## 1. What was built

| Piece | Where |
|---|---|
| libobs 33.0.0 + D3D11 backend + 6 modules, installed | `D:\SDKs\obs\install` (sources `D:\SDKs\obs\src`, deps `D:\SDKs\obs\deps`, build `D:\SDKs\obs\build`) |
| CMake lane fragment (option, imported target, DLL staging, test, driver) | `cmake/lane-libobs.cmake` |
| Engine source `type: "libobs"` | `playout/engine/compositor/libobssource.{h,cpp}` (+3 `#ifdef` lines in `sourcefactory.cpp`) |
| Isolated Qt Test | `tests/test_libobssource.cpp` → `test_libobssource` |
| Engine-process perf driver | `tests/libobs_spike_driver.cpp` → `libobs_spike_driver` |
| Lane auto-include (same 7 lines the main checkout already has, uncommitted there) | root `CMakeLists.txt` |

## 2. Build recipe (exact)

### 2.1 Versions

| Component | Version | Note |
|---|---|---|
| obs-studio | **33.0.0** (tag, commit `72e86655715d9a25d69ce446aa4dd44ba58e10d2`, released 2026-10-23) | LIBOBS_API 33.0.0 |
| obs-deps prebuilt (windows-x64) | **2026-10-23** (the version 33.0.0's `CMakePresets.json` pins) | sha256 `7508972a…bfbe` verified |
| obs-deps prebuilt (windows-x86) | 2026-10-23 | only needed by the x86 child configure that OBS's CMake always runs |
| obs-deps Qt6 (windows-x64) | 2026-10-23 | **downloaded but unused** (frontend off); OBS's Windows buildspec reads its `mkspecs/qconfig.pri` unconditionally |
| CEF | 7871 (pinned by 33.0.0) | **not downloaded**: obs-browser needs it plus a Qt-linked panel build; see §6 |
| Toolchain | MSVC 19.44.35221 (VS Build Tools 17, `C:\BuildTools`), Windows SDK 10.0.26100, CMake 4.1.0, **Visual Studio 17 2022 generator** | OBS's `cmake/windows/architecture.cmake` refuses anything but the VS generator |
| Submodules | obs-browser `85c7f26`, obs-websocket `e28f849`, libdshowcapture `8878638` (+ nested `capture-device-support`) | needed only so `plugins/CMakeLists.txt` configures; all three modules are OFF |

The local `D:\obs-studio-master` (API 32.1.2, May 2026 snapshot, no `.git`) was not used; the release tag is cleaner.
Its `.deps` holds the older 2025-08-23 bundles.

### 2.2 Commands

```powershell
# 1. source (shallow, release tag) and the three submodules OBS's configure insists on
git clone --depth 1 --branch 33.0.0 https://github.com/obsproject/obs-studio.git D:\SDKs\obs\src
cd D:\SDKs\obs\src; git submodule update --init --recursive --depth 1

# 2. dependency bundles (the buildspec downloads into <src>\.deps; a junction keeps them under D:\SDKs\obs\deps)
mkdir D:\SDKs\obs\deps
curl -L -o D:\SDKs\obs\deps\windows-deps-2026-10-23-x64.zip     https://github.com/obsproject/obs-deps/releases/download/2026-10-23/windows-deps-2026-10-23-x64.zip
curl -L -o D:\SDKs\obs\deps\windows-deps-2026-10-23-x86.zip     https://github.com/obsproject/obs-deps/releases/download/2026-10-23/windows-deps-2026-10-23-x86.zip
curl -L -o D:\SDKs\obs\deps\windows-deps-qt6-2026-10-23-x64.zip https://github.com/obsproject/obs-deps/releases/download/2026-10-23/windows-deps-qt6-2026-10-23-x64.zip
cmd /c mklink /J D:\SDKs\obs\src\.deps D:\SDKs\obs\deps
# (OBS verifies the sha256 against CMakePresets.json and extracts obs-deps-2026-10-23-x64\ etc. itself)

# 3. configure: frontend, scripting, browser, websocket and every hardware / heavy module off
cmake -S D:/SDKs/obs/src -B D:/SDKs/obs/build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_INSTALL_PREFIX=D:/SDKs/obs/install -DVIRTUALCAM_GUID=A3FCE0F5-3493-419F-958A-ABA1250EC20B `
  -DENABLE_FRONTEND=OFF -DENABLE_SCRIPTING=OFF -DENABLE_BROWSER=OFF -DENABLE_WEBSOCKET=OFF `
  -DENABLE_AJA=OFF -DENABLE_DECKLINK=OFF -DENABLE_VST=OFF -DENABLE_VLC=OFF -DENABLE_NVENC=OFF -DENABLE_QSV11=OFF `
  -DENABLE_WEBRTC=OFF -DENABLE_LIBFDK=OFF -DENABLE_NVAFX=OFF -DENABLE_NVVFX=OFF -DENABLE_VIRTUALCAM=OFF `
  -DENABLE_COREAUDIO_ENCODER=OFF -DENABLE_SERVICE_UPDATES=OFF -DENABLE_COMPAT_UPDATES=OFF -DENABLE_CCACHE=OFF `
  -DENABLE_TEST_INPUT=ON
# (33.0.0 renamed ENABLE_UI -> ENABLE_FRONTEND and ENABLE_PLUGINS -> ENABLE_CORE_MODULES.)

# 4. build only what the spike needs; OBS's /MP has no count, so cap the compiler with _CL_ and MSBuild with /m:1
$env:_CL_ = "/MP4"
cmake --build D:/SDKs/obs/build --config RelWithDebInfo `
  --target libobs libobs-d3d11 libobs-winrt image-source obs-transitions text-freetype2 obs-filters test-input obs-ffmpeg -- /m:1
# libobs + backends + 5 modules: 3 min 20 s wall; obs-ffmpeg: another ~2 min

# 5. install only the built pieces (a plain --install trips over the unbuilt modules): per-directory install scripts
cd D:\SDKs\obs\build
foreach ($d in "deps/w32-pthreads","libobs","libobs-d3d11","libobs-winrt","plugins/image-source","plugins/obs-transitions","plugins/obs-filters","plugins/text-freetype2","test/test-input") {
  cmake -DCMAKE_INSTALL_CONFIG_NAME=RelWithDebInfo -DCMAKE_INSTALL_PREFIX=D:/SDKs/obs/install -P $d/cmake_install.cmake }
cmake -DCMAKE_INSTALL_CONFIG_NAME=RelWithDebInfo -DCMAKE_INSTALL_COMPONENT=Development -DCMAKE_INSTALL_PREFIX=D:/SDKs/obs/install -P libobs/cmake_install.cmake
# obs-ffmpeg's install script also wants obs-amf-test.exe etc.: copy its rundir output instead
Copy-Item -Recurse D:\SDKs\obs\build\rundir\RelWithDebInfo\core\obs-ffmpeg D:\SDKs\obs\install\core\
# obs.dll's runtime deps (and obs-ffmpeg's) from the bundle, so bin\64bit is self-contained
Copy-Item D:\SDKs\obs\deps\obs-deps-2026-10-23-x64\bin\{avcodec-62,avformat-62,avutil-60,avdevice-62,swscale-9,swresample-6,zlib,librist,srt}.dll D:\SDKs\obs\install\bin\64bit\
```

Resulting install (OBS 33 layout):

```
D:\SDKs\obs\install
  bin\64bit\   obs.dll libobs-d3d11.dll libobs-winrt.dll w32-pthreads.dll + avcodec-62 avformat-62 avutil-60 avdevice-62 swscale-9 swresample-6 zlib librist srt (+ .pdb)
  lib\         obs.lib w32-pthreads.lib
  include\     obs.h ... util\ graphics\ media-io\ callback\
  cmake\       libobsConfig.cmake, libobsTargets*.cmake (wants obs-deps finders on CMAKE_PREFIX_PATH; the lane uses a plain imported target)
  core\<module>\<module>.dll + data\     image-source  obs-transitions  obs-filters  text-freetype2  test-input  obs-ffmpeg
  data\libobs\ *.effect
```

The obs-deps FFmpeg (avcodec-**62**, avutil-**60**) and PlayPlay's FFmpeg 9 (avcodec-**63**, avutil-**61**) have different
DLL names, so both live in one process without a clash. Qt is not involved on the libobs side at all.

### 2.3 PlayPlay side

```powershell
& 'C:\BuildTools\Common7\Tools\Launch-VsDevShell.ps1' -Arch amd64 -SkipAutomaticLocation
cmake -S . -B build-obs -G Ninja -DCMAKE_BUILD_TYPE=Release -DPLAYPLAY_WITH_LIBOBS=ON   # -DPLAYPLAY_LIBOBS_ROOT=... to relocate
cmake --build build-obs --target PlayPlayChannelEngine test_libobssource libobs_spike_driver stage_runtime -j 4
build-obs\test_libobssource.exe            # or ctest -R libobs
build-obs\libobs_spike_driver.exe --out build-obs\spike   # ~2 min 45 s, drives build-obs\PlayPlayChannelEngine.exe
```

`stage_libobs` copies `bin\64bit\*.dll` next to the executables (obs.dll is import-linked; libobs loads
`libobs-d3d11.dll` by name from the executable's directory). Modules and effects are loaded from
`PLAYPLAY_LIBOBS_ROOT` at runtime (`PLAYPLAY_LIBOBS_ROOT` env var or spec `root` override it).

## 3. The source

`LibObsSource` (`type: "libobs"`) derives from `LiveSourceBase` like `NdiSource`: the libobs video thread copies each
rendered BGRA frame into a pooled `FramePtr` and `publishVideo()`s it; the clock thread's `video()` returns the latest
one under a mutex that is never held across a wait. Audio comes through `obs_add_raw_audio_callback` (FLTP 48 kHz) into
the same ¼ s FIFO every live input uses.

Process-wide runtime, started by the first `open()` and shut down at `QCoreApplication` exit:
`obs_startup` → `obs_add_data_path(<root>/data/libobs/)` → `obs_reset_video{libobs-d3d11, fps = channel rate,
base = output = raster, output_format = BGRA}` → `obs_reset_audio{48 kHz stereo}` → `obs_add_module_path(<root>/core/%module%)`,
`obs_find_modules2` + `obs_open_module`/`obs_init_module` per module → `obs_post_load_modules`. libobs log lines are
routed into the engine log as `[libobs]`.

**One source per process**: libobs has a single program output (channel 0); a second `open()` fails with a clear
error. (libobs 33's `obs_canvas_*` API would allow independent canvases later.)

### 3.1 Spec

```json
{ "type": "libobs",
  "root": "D:/SDKs/obs/install",              // optional
  "audio": true,
  "scenes": [
    { "name": "A", "items": [
        { "id": "color_source_v3", "name": "bg",    "settings": { "color": "#2040c0" } },
        { "id": "image_source",    "name": "logo",  "settings": { "file": "C:/art/logo.png" }, "fit": "inner",
          "size": { "width": 480, "height": 270 }, "pos": { "x": 1380, "y": 60 },
          "filters": [ { "id": "color_filter_v2", "settings": { "opacity": 0.8 } } ] },
        { "id": "ffmpeg_source",   "name": "clip",  "settings": { "local_file": "C:/media/promo.mp4", "looping": true } },
        { "id": "text_ft2_source_v2", "name": "cap", "settings": { "text": "LIVE", "font": { "face": "Arial", "size": 72 } } } ] },
    { "name": "B", "items": [ { "id": "random", "name": "noise" } ] } ],
  "program": "A",
  "transition": { "id": "fade_transition", "durationMs": 500 } }
```

* `id` is the libobs source id; `settings` are that module's own keys passed through as JSON. A `"color"` given as
  `"#rrggbb"` / `"#aarrggbb"` becomes libobs' `0xAABBGGRR` integer; a colour source without `width`/`height` gets the raster.
* `fit`: `stretch` (default, bounds = raster or `size`), `inner`, `outer`, `none`; `pos`, `visible`, `filters[]`.
* `transition`: `cut_transition`, `fade_transition`, `fade_to_color_transition`, `swipe_transition`, `slide_transition`,
  `wipe_transition`, `obs_stinger_transition` (`settings: { path, transition_point, tp_type }`).
* Available ids with the modules built here: `color_source_v3`, `image_source`, `slideshow` (image-source);
  `ffmpeg_source` (obs-ffmpeg); `text_ft2_source_v2` (text-freetype2); all obs-filters (`color_filter_v2`,
  `chroma_key_filter_v2`, `color_key_filter_v2`, `scale_filter`, `sharpness_filter_v2`, `gain_filter`, …);
  `random`, `test_sinewave`, `test_filter` (test-input). **No NDI** and **no browser** — §6.

### 3.2 Commands (engine's existing layer `update` path)

`{action:"update", layer:50, source:{...}}` reaches `LibObsSource::update()`:

| params | effect |
|---|---|
| `{ "switch": "B" }` | `obs_transition_start(AUTO, spec durationMs)` to scene B with the current transition |
| `{ "switch": "B", "transition": "cut_transition" }` | same, swapping the program transition first |
| `{ "switch": "B", "transition": { "id": "obs_stinger_transition", "durationMs": 800, "settings": { "path": "...", "transition_point": 400 } } }` | stinger |
| `{ "item": "cap", "settings": { "text": "BREAKING" } }` | `obs_source_update` on one item |
| `{ "item": "logo", "visible": false }` | `obs_sceneitem_set_visible` |

Commands are queued to the source's own thread, so a stinger's media load never runs on the clock thread.
`status()` (1 Hz telemetry, `layers[].sourceStatus.obs`) reports version, modules, program, callbacks, frame-age
p50/p99/max over the last 4096 pulls, `obs_get_active_fps`, lagged frames and the last switch's dispatch time.

## 4. Measurements

Box: Xeon E5-2697A v4 (16c/32t), 32 GB, Intel Arc A380 (adapter 0, driver 32.0.101.9033), Windows 11 26100. The live
broadcast stack (two channels on air) was running on the same machine throughout, so absolute CPU numbers carry that
background; deltas are what matter. Raster 1920×1080 @ 30000/1001, BGRA, libobs rendering free-running at the same rate.

### 4.1 Isolated source (`test_libobssource`, 5 runs)

| | result |
|---|---|
| open() → first frame (first open: D3D11 init + 6 modules) | 0.41 – 0.55 s |
| 120 pulls at 29.97 | 120 non-null, 120 distinct, 0 repeats, 1 drop (the frame that overlapped the first pull) every run |
| **(a) frame age at pull** (libobs frame timestamp → pull, `os_gettime_ns`) | **p50 52 – 62 ms, p99 = p50 + 0.8 ms, max ≤ 63 ms, min ≥ 50 ms** (1.6 – 1.9 frames; spread inside one run < 2.5 ms, the spread *between* runs is the phase the two free-running clocks happened to start with) |
| time from the raw-callback copy to the pull | p50 14 – 24 ms (same phase effect) |
| **(b) cut latency** (`update({switch})` → first pulled frame showing the new scene) | **2 frames in 6/6 trials in every run** (65 – 68 ms); `update()` → `obs_transition_start` 0.05 – 0.10 ms |
| 300 ms fade | first changed frame at tick 2, fully arrived at tick 10 (= 9 frames of fade, as configured) |
| **(c) process CPU** (one core = 100) | before libobs: 0.0 – 0.4 %; **libobs open (colour + full-noise 960×540 item): 12.9 – 26.5 %** (libobs render 1.6 – 2.4 ms/frame, incl. the 8 MB/frame BGRA copy into the pool); runtime up but source closed: 2.0 – 3.1 % |
| **(c) memory** | working set 33 → 165 – 193 MB (**+132 – 160 MB**), private 43 → 191 – 209 MB; after close the runtime keeps ≈ 150 – 177 MB (D3D11 device, shaders, modules) |
| audio (`test_sinewave` in the scene) | heard within the first ticks, peak 0.51 FS; FLTP 48 kHz through the live-input FIFO, 0 overflow |
| second `open()` while one is active | refused with "one source per process" |

### 4.2 Real engine process (`libobs_spike_driver`, 165 s, `PlayPlayChannelEngine.exe --id perftest-obs`)

Driver plays supervisor over IPC `PlayPlayLibObsSpike`; one output, `udp://127.0.0.1:50010` h264_aac (libx264 in the
engine, so the baseline is already > 1 core); slate program; libobs scene as layer 50 (scene A blue + noise item, scene B
red; cut to B at 90 s, 500 ms fade back to A at 115 s); layer removed at 140 s. Report:
`build-obs\spike\libobs-spike-report.json`, engine log `build-obs\spike\libobs-spike-engine.log`.

| phase | s | engine CPU avg / median % | WS avg / max MB | private avg / max MB | clock-thread tick median / max ms | late ticks | libobs age p50 / p99 / max ms | frames in / repeats / drops |
|---|---|---|---|---|---|---|---|---|
| baseline (slate + x264) | 36 | 117.6 / 119.6 | 810 / 815 | 1186 / 1210 | 0.12 / 0.47 | 0 | – | – |
| **with libobs layer** | 100 | **178.3 / 172.2** (one 533 % second at open) | **1034 / 1065** | **1405 / 1454** | **4.56 / 16.43** (the 16 ms is the open tick) | **0** | **44.7 / 45.4 / 78.9** (2981 samples) | 2982 / 4 / 5 |
| after remove (runtime idle) | 25 | 110.0 / 107.8 | 1040 / 1063 | 1409 / 1430 | 0.09 / 0.17 | 0 | – | – |

Reading it:

* **The engine never missed a tick** with the layer on air, including the two switches. The clock-thread render went
  from 0.12 ms to 4.6 ms per frame: that is PlayPlay's own CPU compositor blending a full-raster 1080p BGRA layer over
  the program (the same cost an NDI camera layer carries; the GPU path, `gpu: true`, would take it off the clock thread).
* **CPU delta ≈ +55 – 60 % of one core** for layer + libobs together; of that, libobs itself is the 13 – 17 % measured in
  isolation (§4.1), the rest is the compositor blend and the preview/encode of a now-changing picture.
* **Memory delta ≈ +225 MB working set / +220 MB private**, and it stays after the layer is removed because the runtime
  (D3D11 device, module DLLs, effect shaders) lives for the process. The isolated test's five open/close cycles in one
  process stayed within the same 165 – 193 MB band, so repeated show/remove does not grow it (not re-measured in the engine).
* **Frame age p50 44.7 ms (1.34 frames), p99 45.4 ms, max 78.9 ms** — steady for the whole 100 s (the max is the
  switch second). Why ≥ 1 frame: libobs renders frame N, then reads back frame N−1's staging texture
  (`download_frame(prev_texture)`, a deliberate one-frame GPU pipeline), hands it to `video_output`'s own thread, which
  calls the raw callback; then our clock tick lands somewhere in the next 33 ms. So the floor is 1 frame + readback
  (~2 – 5 ms) + 0…1 frame of phase. Two free-running clocks at the same nominal rate drift slowly, which is the
  1 drop / 4 repeats per 3000 frames.
* In-engine switch dispatch (`update()` on the clock thread → the source's worker → `obs_transition_start`) 0.78 ms; the
  cut itself reached the layer 2 frames later, as in the isolated test.

## 5. What works

* **Build**: libobs 33.0.0 + D3D11 backend + image-source, obs-transitions, obs-filters, text-freetype2, test-input,
  obs-ffmpeg build and install from the release tag with the stock deps bundle in ≈ 6 min at 4 threads; nothing patched
  in OBS. The install is self-contained (`bin\64bit` carries the bundle DLLs).
* **In-process coexistence**: libobs' FFmpeg 8 (avcodec-62) and PlayPlay's FFmpeg 9 (avcodec-63) load side by side;
  no Qt on the libobs side; libobs log goes into the engine log.
* **Source**: scenes with colour / image / media (`ffmpeg_source`) / FreeType text / noise items, per-item filters, bounds
  (`fit`), position, visibility; cut / fade / stinger transitions; `switch` and item updates over the engine's existing
  layer `update` command; audio from the scene mixed like any live input; `status()` with libobs timing in the 1 Hz telemetry.
* **Engine**: the layer behaves exactly like an NDI input to the compositor (newest frame wins, no waits on the clock
  thread); show / switch / remove over IPC with 0 late ticks; the OFF build is byte-for-byte unaffected (the
  preprocessed `sourcefactory.cpp` differs from HEAD only by six empty lines where the `#ifdef`s sit; `build-off`'s
  engine imports no `obs.dll` and contains no "libobs" string).
* **Lifecycle**: start once per process, clean `obs_shutdown` at exit (0 sources remaining), repeated open/close.

## 6. What does not (and why)

| Item | Status | Why / what it would take |
|---|---|---|
| **obs-browser (CEF)** | not built | CEF 7871 is a separate ~150 MB download, and obs-browser's CMake builds the Qt "browser panels" half unless `ENABLE_BROWSER_PANELS=OFF`; with the frontend off it may still want `obs-frontend-api`. Doable (one more download + two options) but it was "only if it builds without a fight"; it also brings a Chromium subprocess per source. PlayPlay already has `HtmlSource` on its own headless browser, so there is no gap. |
| **NDI inside libobs** | skipped | The only NDI plugin is DistroAV (installed under `C:\ProgramData\obs-studio\plugins\distroav`). It links `obs-frontend-api.dll` and OBS's own Qt6; neither can be loaded into a Qt 6.9 process. PlayPlay's `NdiSource` covers NDI; cameras for a libobs scene would come in through it (or DeckLink/dshow modules, not built). |
| **Media items** | works, with a note | `obs-ffmpeg` loads, but at load it spawns `obs-nvenc-test.exe` / `obs-amf-test.exe` (not built) and logs `CreateProcessW failed: 2`; harmless, `ffmpeg_source` is registered. Its decoding uses the obs-deps FFmpeg, not PlayPlay's. |
| **More than one libobs layer per process** | refused by design | libobs has one program output (channel 0). libobs 32+'s `obs_canvas_create` + `obs_canvas_set_channel` and per-canvas raw callbacks would give independent canvases; not needed for the spike. |
| **Frame-exact timing of the libobs picture** | inherent 1.3 – 1.9 frames | see §4.2 and §7: two free-running clocks. |
| **Runtime memory after close** | stays | by design (runtime per process); ≈ 150 MB. |
| **DLL staging** | configure-time glob | after adding a DLL to `bin\64bit`, re-run cmake (same rule as the FFmpeg DLLs). Caught once: `libx264-164.dll` was silently satisfied from conda's `Library\bin` on PATH. |
| Qt6 bundle | 260 MB downloaded, unused | OBS's Windows buildspec reads its `mkspecs/qconfig.pri` even with the frontend off. One `cmake/windows/buildspec.cmake` edit would skip it; not worth patching OBS for. |

## 7. Recommendation

**Worth productising — as the hybrid, not a swap.** The cost side is small and bounded (one libobs render thread at
1.5 – 2.5 ms/frame, ≈ 150 MB, zero effect on the clock thread beyond the normal full-raster layer blend), the
integration surface is one source class behind the existing layer commands, and nothing in OBS needed patching. What
you get is OBS's scene graph, every OBS filter, stingers, and a path to the OBS plugin ecosystem (DeckLink, dshow,
text, slideshows) for the live-switcher use case, while scheduled playout, outputs, CG and timing stay PlayPlay's.

What to decide before productising:

1. **Latency budget.** 1.3 – 1.9 frames of age is fine for a live camera switcher layer (an NDI input is in the same
   range, 1 – 2 frames) and for stingers over program. It is *not* fine if the libobs picture must land on a scheduled
   frame boundary (a stinger timed to a playlist cut): for that, either accept 2 frames of pre-roll in the schedule or do
   the external-tick patch below.
2. **Composite on the GPU.** With `gpu: true` the layer blend leaves the clock thread (4.6 ms → the GPU compositor's
   upload). Today's measurement is the CPU path.
3. **Where libobs' inputs come from.** Without DistroAV, cameras reach a libobs scene through PlayPlay's own inputs or
   through OBS's DeckLink / win-dshow modules (both configured, not built: `--target decklink win-dshow`, DeckLink needs
   `ENABLE_DECKLINK=ON`). Deciding this fixes the module list and the install size.
4. **Keep it one source per process** until a second canvas is actually needed; `obs_canvas_*` is the path then.

### "Render on external tick" — what a libobs patch would need

libobs' graphics thread (`obs-video.c: obs_graphics_thread_loop`) does, per iteration: `tick_sources(video_time)` →
`output_frames()` (render all mixes into the current texture, stage it, **download the previous texture** and push it
to `video_output`) → `render_displays()` → `video_sleep(&video_time, interval)` (`os_sleepto_ns` to `video_time +
interval`, counting lagged frames). `video_output` (`media-io/video-io.c`) has its own thread that pops frames and calls
the raw callbacks. Making PlayPlay's frame clock the master needs:

1. **Replace `video_sleep`** with a wait on an external trigger: a new `obs_video_tick_external(uint64_t timestamp_ns)`
   (or an `obs_video_info` flag `external_clock` plus `obs_video_render_frame()` that the clock thread calls). The
   loop would block on a semaphore instead of `os_sleepto_ns`, and use the caller's timestamp as `video_time` so
   libobs' source ticks, transition progress and audio sync run on PlayPlay's timeline (≈ 40 lines in `obs-video.c`,
   `obs-internal.h`).
2. **Synchronous readback for the external mode**: `output_frame()` downloads `prev_texture` to keep the GPU a frame
   ahead; the external mode should stage and map the *current* texture (`gs_stagesurface_map` right after
   `gs_stage_texture`), accepting the ≈ 2 – 5 ms stall on the A380 — that is what turns 1.3 frames into 0 frames of age.
3. **Bypass `video_output`'s thread** for the external mode: hand the mapped frame straight to the caller (an
   `obs_video_render_frame(struct video_data *out)` that returns the BGRA pointer for the duration of the call), so the
   compositor copies or uploads it inside the same tick; the raw-callback path stays for the free-running mode.
4. **Audio**: `obs_audio_thread` keeps running on `os_gettime_ns`; with an external video clock the audio timestamps
   should be derived from the same tick (`audio_output` already takes timestamps per mix; feed the tick time in).
5. The clock thread would then spend libobs' render time (1.5 – 2.5 ms here, more with cameras and filters) *inside*
   the tick; on the GPU compositor path the libobs output texture could even be shared (`gs_texture_get_obj` →
   `ID3D11Texture2D`) instead of read back, if both sit on the same D3D11 device.

Effort: a focused patch of a few hundred lines in `obs-video.c` / `video-io.c` kept as a local patch set on the
release tag (or upstreamed as "external clock mode"); the source class here already isolates the call site
(`rawVideo` becomes "pull on tick"). Until then the free-running hybrid above is usable as is for a camera / stinger
layer with a 2-frame budget.
