# avkit: in-process libav capabilities and probe

`PlayPlayAvKit` (static, Qt6::Core + FFmpeg::FFmpeg; `cmake/lane-inhouse-avkit.cmake`) answers what PlayPlay used to
spawn `ffmpeg.exe` / `ffprobe.exe` for, from the FFmpeg 9 libraries the process already links
(docs/plans/inhouse-deps.md §6.1, DEP-01).

```cmake
target_link_libraries(<target> PRIVATE PlayPlayAvKit)   # from that lane's fragment
```

Users:

- **The app's `MetadataService`** (DEP-02a). `services/metadataservice.cpp` `probeMediaFile` calls `probeJson` with a
  30 s deadline, the old ffprobe wait. It feeds the bulk importer and `enrichMediaItem`. The source is part of the
  `PlayPlayCore` static library, so the fragment links AvKit there link-only:
  `target_link_libraries(PlayPlayCore INTERFACE "$<LINK_ONLY:PlayPlayAvKit>")`. Only targets that pull
  `metadataservice.obj` out of the library get the probe code. Today that is `PlayPlay.exe`; the Supervisor and engine
  binaries do not contain it.
  - Fallback: `PLAYPLAY_PROBE_ENGINE=ffprobe` (read on every call) brings back the old spawn of `ffprobe`. That spawn
    finds `ffprobe` through the Windows process search, which covers PATH and the exe's own folder. Measured: with
    every ffprobe off PATH it still ran the `ffprobe.exe` staged next to the exe.
  - Note: libav's own log lines, such as `Protocol 'concat' not on whitelist`, go to the app's stderr at libav's default
    level. ffprobe's `-v quiet` hid them, and AvKit leaves the process-wide `av_log` level alone.
- Next: `MuxManager::capabilities` (inhouse-mux DEP-02b) and `PlayPlayMediaTool` (DEP-03).

## avcaps.h

| function | replaces | notes |
|---|---|---|
| `encoders()` | `ffmpeg -hide_banner -encoders` | name, long name, descriptor codec, type, the `V.S..D` flag column, `hardware`, `experimental`; same order as fftools |
| `encodersText()` | the same, as text | byte for byte what ffmpeg.exe of the same build prints (checked by the test) |
| `hasEncoder(name)` | grep of that list | |
| `configuration()` | `configuration:` line of `ffmpeg -version` | `avcodec_configuration()` |
| `versionInfo()`, `versionLine()`, `versions()` | first line of `ffmpeg -version` | `versions()` = ffmpeg, license and the seven lib versions |
| `ffmpegCapabilities()` | both spawns in `MuxManager::capabilities` | exactly its `ffmpeg` object: `{path: "(in-process libav)", found, version, video[], audio[], has{}, build{}}`; `has{}` lists the watched encoders as `false` when absent |
| `buildFlags(conf, has)` | MuxManager's `build{}` rules | `{srt, rist, ndi, qsv, nvenc, x264, x265}` |

Listed is not usable: a hardware encoder is listed whenever the build has it. The engine finds out by opening it
(`EncoderStage::actualVideoCodec`); AvKit opens nothing. Cost: about 1 ms for the first call, cached after that. The two
spawns it replaces take about 220 ms on this box, and up to 25 s of waits on the Supervisor's main thread.

## avprobe.h

`probe(path, opts)` is `ffprobe -v quiet -print_format json -show_format -show_streams <path>`:

- `json`: what `QJsonDocument::fromJson(ffprobe stdout)` gives;
- `text`: ffprobe's own layout. Same key order, 4-space indent, its string escaping and UTF-8 validation (each invalid
  sequence becomes U+FFFD). Lines end in `\n`; ffprobe.exe writes `\r\n`.
- On failure: `ok=false`, `averror`, `error`, and `text` = ffprobe's `{\n\n}\n`.

Like ffprobe, it opens with `scan_all_pmts=1`, runs `avformat_find_stream_info` and binds a decoder to each stream
without decoding frames. The decoder supplies `coded_width`, `is_avc`, the AC-3 downmix fields and so on.
`openDecoders=false` skips that; the keys the ingest reads do not depend on it.

`test_inhouse_avkit` compares the result with ffprobe.exe of the same build. The text is byte-identical on 17 files:

- containers and codecs: h264/aac mp4 (and a faststart one), mpeg2/ac3 480i TS (CPB side data, mpegts exported fields),
  mov with a timecode data track, mkv with opus and an srt subtitle;
- stills: PNG, JPEG, WebP;
- audio: mp3 alone and with an attached cover, PCM WAV mono, stereo and 5.1;
- odd cases: a display-matrix rotation, a non-ASCII file name, tags with invalid UTF-8;
- `build\clipC_720p25.mp4` and `clipB_1080p.mp4`;
- truncated MP4s, which match whether ffprobe refuses them or reads them.

Not reproduced yet: `-show_data_hash`, `-count_frames` / `-count_packets`, `-analyze_frames`, and the detail fields of
side-data types other than display matrix, CPB properties, mastering display, content light level and audio service
type (those print `side_data_type` only). One deliberate difference: when a stream's decoder does not open, ffprobe
exits; AvKit prints that stream without the decoder fields.

Safety defaults, for in-process use:

- `localOnly`: `protocol_whitelist=file`, also for nested opens, so a playlist on disk cannot fetch URLs.
- `timeoutMs` deadline (20 s) and a `cancel` flag, both through `AVIOInterruptCB`.
- Decoders run with one thread.

Probing untrusted files in bulk belongs in the MediaTool process (§8), so a malformed file cannot take down the
Supervisor.

`durationOnly(path)` is the drag-and-drop rule of `supervisorcore_library.cpp` `quickDurationSec`: header duration,
stream info only when the header has none, 0 when unknown, local files only, with a deadline. It agrees with the full
probe's `format.duration` within 1.5 ms on every fixture. `formatDurationValue(result)` is what
`-show_entries format=duration -of default=nw=1:nk=1` prints (`N/A` when there is none).
