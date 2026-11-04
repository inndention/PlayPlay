# Scene recording, Star News CG and slave channels

Release additions: 2026-10-30. Executables and web assets are combined in the build directory.

## Automation channels and slaves

Native: **Config → Channels** or **Remote → Automation channels / slave nodes**.
Web: **Channels → Automation channels / slave nodes**.

Choose 1–8 automation slots and Apply count. Existing visible channels are adopted before new slots are allocated. Reducing the count disables the excess slots and stops their engines; their profiles, schedules and jobs stay stored. Disabled managed slots stay available in the configuration form and disappear from the operating view. New slots start disarmed. The normal ARM control enables local schedule execution.

Each channel has three modes:

* **Local automation** runs the channel's own schedule.
* **Slave: receive remote video + audio** receives the upstream program, including its graphics and mixed sound. Choose a registered remote node and its channel ID. The receiver URL may be an NDI name (`ndi://HOST (Program)`) or a network stream URL. A blank URL derives the upstream channel's configured NDI output. The upstream must actually publish that output.
* **Slave: mirror schedule / local media** polls the upstream's resolved current and next hours every two seconds, maps media by hash, hash kind and file size (or a matching registered shared path), and plays those concrete hours locally. Missing media is reported before changing the local hours. This mode does not transfer media files. Returning to Local restores the local hours saved before mirroring.

Both nodes need this build's `/automation/export/{id}` API. Configure nodes and their passwords under Remote. Requests use the saved node password. Source channels must be on another node; slave chains are refused. On an upstream control-connection failure the last valid mirrored schedule remains available locally and status reports the error. Slave polling respects primary/backup standby mode. These are one-to-one channel mappings, not a genlock or frame-synchronous failover system. Schedule mode mirrors the two active resolved hours, not the upstream's entire historical/future schedule database or manual operator takes.

## Layered recording and recomposition

Native and web **On Air / Now Playing** have **Record program + layers**, Stop and Recompose controls. The engine reports the session folder, frame count, completion and errors. Recording is opt-in and does not alter the channel's output definitions.

A session contains:

* `program.mkv`: a normal H.264/AAC recording composed from the same captured frame timeline.
* `media.bin`: lossless compressed BGRA layer pictures (alpha preserved), planar-float source-strip audio and final mixed audio.
* `timeline.jsonl`: per-frame layer order, source/crop rectangles, position, clipping, opacity, alpha convention, audio references and mixer gains.
* `session.json`: raster, exact rational frame rate, audio format, original frame origin, frame count and completion/error status.

Captures are taken once at the compositor, before its optional GPU output delay, and processed on a separate worker. A bounded capture queue stops recording with an explicit error on overrun. Complete recordings drain their encoder and file queues before finalization. A session is marked complete only after final writes succeed. This lossless archive needs storage fast enough for the material being recorded; its queue limit is 60 frames or a conservative 256 MiB of referenced media.

**Scope:** records the program bus and visible compositor layers, plus the audio strips pulled by the compositor. An embedded libobs scene is one compositor layer; its internal cameras, filters and scene items are baked into that layer. Hidden/off-air inputs are not independently ISO-recorded. During an A/B transition both visible program pictures are retained, with the transition's draw instructions; program-bus audio is the transition mix. Layers can be hidden, moved and resized or have their opacity changed during recomposition. This does not turn already-rendered text into editable text objects.

In the native Recompose dialog, choose a completed session and a new MKV output, then optionally hide layer IDs or change one layer's placement/opacity. The web form supports hiding layers. Export runs as a background job on the Supervisor's node and refuses an existing output file. Original recordings remain intact.

The bundled command-line tool supports multiple edits and audio adjustments:

```powershell
.\PlayPlayRecompose.exe --session "D:\Recordings\session" --output "D:\Exports\recomposed.mkv" --edits "D:\Exports\edits.json"
```

Example edits:

```json
{
  "omitLayers": [30, 40],
  "layers": {"15": {"rect": [120, 90, 640, 360], "opacity": 0.9}},
  "audioGainDb": {"program": -3},
  "audioMute": ["L61"]
}
```

Layer 0 identifies the program pictures; ordinary layers retain their compositor IDs. Omitting a layer also omits its isolated audio during remix. Recorded transition/animation gains remain in those audio strips. Incomplete sessions are refused by default. An explicit `"recover": true` edit allows exporting intact timeline records; a malformed or missing record still causes an error.

## Chroma key

Native: **Inputs → OBS scenes → Chroma key**. Select scene and source, choose a color, and use the color dialog's **Pick Screen Color** button to sample the picture. Similarity, smoothing and spill use 1–1000; opacity uses 0–1. Apply key updates the draft. **Apply live** creates or updates the key on the shown source without reopening the scene. Save retains the settings in the scene set.

Web: **OBS Scenes → source → Chroma key filter** has the color picker, browser eyedropper, similarity, smoothness, spill, opacity and Apply live. The browser eyedropper is available only where the browser provides the EyeDropper API; the normal color picker remains available elsewhere. Native sampling uses Qt's screen picker.

## Star News graphics

The template catalog includes **Star News Lower Third**, **Star News Bug** and **Star News Headline**, with a common red/dark/gold treatment and the supplied Star News logo. The native graphics deck defaults to the Star News lower third; web quick buttons include all three.

They support entrances, exits and restrained idle accents and stay on screen until clear unless an explicit auto-hide duration is supplied. Browser template controls support `play`, `stop`, `next` and object/JSON/Caspar-style XML field updates. They use the existing headless Chromium output path; Qt WebEngine is not required.

## Show templates, CG outputs and playlists

The native CG editor and web CG editor provide eight independent output tabs for each channel. Deploy or clear a tab separately; the output page URL includes its output number. Outputs 2–8 use compositor layers 72–78 when placed on air and refuse a layer already occupied by an unrelated graphic.

On Air has separate preview and direct program-cut input buses. Save a show template to capture the input map, transition, camera assignments, graphics slots and the current client layout. Load a show to restore those presets without automatically cutting program. Native and web layouts are stored separately in the same show template.

The hour editor displays estimated start, date added and play count; use Resolve for the actual computed runlist when dynamic blocks or timing constraints are involved. Drag media or the reorder handle to the upper or lower half of a row to insert before or after it. Explorer files and folders can also be dropped into native editors. On Air's rundown accepts media at a particular row, with the Supervisor rechecking the current on-air and elapsed portion before saving a dated override. Dynamic block and automatic filler rows cannot be used as insertion anchors; edit the hour instead. The web rundown exposes a Drag media tray. Browser file drops upload to the library; choose the ingested item before placing it in the schedule.

## Optional VapourSynth filters

`filters/vapoursynth/lightweight.py` provides Levels, a luma LUT, chroma smoothing and bilinear/bicubic 720p scaling. Import `apply(clip, preset)` from a `.vpy` script after opening a source. Input is deliberately restricted to explicitly tagged 8-bit YUV420; choose the correct matrix and range during source conversion. These filters are packaged for offline evaluation; they are not inserted into live channels automatically.

Run `python scripts/benchmark-vapoursynth.py --output artifacts/vapoursynth-benchmark.json` with Python, NumPy and VapourSynth installed. On this workstation the Python module works with R73; the separately installed `vspipe.exe` did not initialize correctly. No global Python configuration was changed.

The recorded synthetic 1080p benchmark (90 changing frames per preset, two VS threads) measured median 2.847 ms for the Python source baseline and 3.724–4.774 ms including each filter. It excludes decoding, encoding, storage, GPU transfers, concurrent channels and real footage. It does not certify live-channel capacity.

References: [VapourSynth installation](https://www.vapoursynth.com/doc/installation.html), [function reference](https://www.vapoursynth.com/doc/functions.html), [resize](https://www.vapoursynth.com/doc/functions/video/resize.html), [LUT](https://www.vapoursynth.com/doc/functions/video/lut.html).

## Verification

The isolated integration test launches a temporary Supervisor and authenticated mock upstream. It checks count boundaries, local-media mapping, missing-media handling, schedule restoration, NDI receiver configuration, actual engine capture, CLI and background-job MKV recomposition, existing-output refusal show templates, CG output isolation, positional rundown insertion and native Remote/Inputs/On Air/CG/Schedule page loading. The screenshot-mode app has intermittently crashed during teardown after producing an image; a traced run passed all pages, but shutdown stability remains under investigation. It uses no live station database. Pixel tests verify live libobs key creation/update and scene scoping. Archive tests verify alpha compositing, layer omission and that all eight test frames reach both recorded/exported MKVs. Browser checks exercise graphics play/clear and field updates.

This does not establish physical two-machine receiver performance, all eight channels under production load, DeckLink/ASI operation, or completion of unrelated pending workstreams in `docs/build-consolidation-status.md`.
