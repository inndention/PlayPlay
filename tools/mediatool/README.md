# PlayPlayMediaTool

The in-house replacement for the `ffprobe` / `ffmpeg` processes PlayPlay spawns in its media path
(docs/plans/inhouse-deps.md §6.2). It runs on the FFmpeg 9 libraries the engines link, so the tool and the engines always
agree on what a file is. It is a separate process on purpose: probing untrusted files must never be able to take down the
on-air Supervisor (§8).

Build: `PlayPlayMediaTool` (EXCLUDE_FROM_ALL, from `cmake/lane-inhouse-mediatool.cmake`). Test: `test_inhouse_mediatool`.
At run time it needs the libav DLLs (`avcodec-63.dll` ...) and `Qt6Core.dll` next to it or on PATH.

## Sub-commands

| command | replaces | contract |
|---|---|---|
| `probe -v quiet -print_format json -show_format -show_streams <file>` | `ingestmanager.cpp` `pump` | stdout byte-identical to ffprobe.exe (CRLF, key order, escaping), exit 0; `{\r\n\r\n}\r\n` and exit 1 when the input cannot be opened |
| `probe -v error -show_entries format=duration -of default=nw=1:nk=1 <file>` | `renderfarm.cpp` `probeDuration` | `2.000000\r\n` or `N/A\r\n`; nothing and exit 1 on failure, with ffprobe's `<file>: <error>` line on stderr |
| `thumb -y -v quiet [-ss t] -i <in> -frames:v 1 -vf scale=320:-2 -q:v 4 <out>.jpg` | `ingestmanager.cpp` thumbnail | the JPEG ffmpeg.exe writes (byte-identical on the test fixtures, autorotate included), written atomically; one JSON result line, exit 0 / 2 (64 for a rotation not reproduced, see below) |
| `<ffmpeg arguments>` (no sub-command) | `spotgenerator.cpp` `composeStories` / `compose`, `renderfarm.cpp` `startTranscode` / `startSegment` / `concatSegments` | the thumbnail form runs as `thumb`; every other command line in the graph runner's subset (below) runs in-process: `{ok, out, duration, frames, samples, bytes, dup, drop, streams, elapsed}` on the last stdout line, exit 0 / 2, with `-progress pipe:1` blocks before it; outside the subset exit 64 |
| `parse <ffmpeg arguments>` | | diagnostic: `{ok, command}` when the command line is in the graph runner's subset; the same 64 / 2 lines the ffmpeg mode gives otherwise |
| `--serve [--workers 3] [--timeout-ms 60000] [--idle-exit-ms 0] [--max-pending 256] [--hang-grace-ms 10000] [--threads 2] [-v level]` | many spawns per ingest scan | JSON lines, see below |
| `version` | | `{ok, tool, commands, serveOps, ffmpeg, libav}` |

Also accepted by `probe`, because they print the same bytes: `-show_format` or `-show_streams` alone, `-of`, `-i <file>`,
`-hide_banner`, any `-v`, and the default writer with or without wrappers / keys (`nw`, `nk`, `noprint_wrappers`, `nokey`).
`thumb` also takes `-ss` after `-i`, `-vframes 1`, `-filter:v`, `-qscale:v`, `-n`, `-an`, `-update 1`, `-f image2|mjpeg`,
`-c:v mjpeg`, any `scale=W:H`. Options must sit where ffmpeg reads them the same way: one `-ss`, before or after `-i`; the
output options (`-frames:v`, `-vf`, `-q:v`, `-update`, `-c:v`) after `-i`; nothing per-file after the output file (ffmpeg
ignores trailing options). Global options (`-y`, `-n`, `-v`, `-hide_banner` ...) go anywhere.

**Autorotate.** Like ffmpeg (on by default), `thumb` applies the frame's display matrix before the scale: a portrait phone
video or an EXIF-oriented JPEG (orientation 6 / 8 / 3 ...) comes out the right way up, by ffmpeg_filter.c's rule
(`transpose=clock|cclock|clock_flip|cclock_flip`, `hflip`, `vflip`). The remap is lossless and plane by plane, so the JPEG
stays byte-identical to ffmpeg.exe's (tested on all four transposes, 180°, flips, EXIF 2-8, odd sizes, 4:4:4, B-frames
with `-ss`). The result line names it: `"autorotate": "transpose=clock"`. Two cases are not reproduced and exit 64 after
decoding, with no file written: a 90° / 270° turn of a picture with unequal chroma subsampling (4:2:2, as many DSLR JPEGs
are, which ffmpeg converts to 4:4:4 before the transpose) or a palette format, and an angle that is not a multiple of 90
(ffmpeg's rotate filter).

**Unsupported means fallback, never a different result** (inhouse-deps.md §5.2). Any other argument, writer or filter, a
second input, a non-JPEG output, a thumbnail without `-frames:v 1` or `-q:v`, an option in a position ffmpeg reads
differently, a rotation not reproduced, and any input that is not a local file (`http://`, `concat:`, `pipe:`, `-`) exits
**64** with `{"ok": false, "error": "unsupported: <arg>", "unsupported": true}`.
The caller then runs ffprobe / ffmpeg. Usage errors (missing value, no input) exit 2 with `{"ok": false, "error"}`.

## --serve

One process, N worker threads (default 3), on `ppcli::ServeLoop` (tools/ppcommon). Requests are JSON lines on stdin;
replies come in completion order on stdout, matched by `id`, each with `ms` and `worker`.

```
{"id": 1, "op": "probe", "path": "D:\\media\\a.mp4"}
  -> {"id": 1, "ok": true, "probe": {"streams": [...], "format": {...}}, "ms": 3, "worker": 0}
{"id": 2, "op": "duration", "path": "..."}            -> {"ok": true, "duration": 2.0, "value": "2.000000"}
{"id": 3, "op": "thumb", "path": "...", "out": "x.jpg", "at": 1.5, "width": 320, "height": -2, "q": 4}
  -> {"ok": true, "out", "width", "height", "srcWidth", "srcHeight", "at", "frameTime", "bytes", "pixFmt", "autorotate"?}
{"id": 4, "op": "ping"} / {"op": "quit"} / {"op": "version"}
```

- `probe` returns the same object ffprobe's JSON form prints, so the ingest reads it unchanged.
- In `thumb`, `at` is ffmpeg's `-ss`: leave it out for stills. `width` / `height` follow `scale=W:H`, `q` is `-q:v`.
  A rotation not reproduced answers `{"ok": false, "unsupported": true}`: run ffmpeg for that file.
- Per-request `timeoutMs` (default `--timeout-ms`): the reply `{"ok": false, "timeout": true}` comes at the deadline. The
  probe and the thumbnailer also stop themselves at that deadline (interrupt callback and decode loop), and at once when
  the loop cancels the job (its timeout reply, a closed stdout): the loop's cancel flag reaches both. A thumbnail
  cancelled before its commit leaves no file.
- A bad file costs one `{"ok": false, "error", "averror"}` reply, never the process.
- A request still running `--hang-grace-ms` after its timeout makes the tool exit 2, so the client restarts it and marks
  only the in-flight files failed.
- Crashes end the process without a Windows error dialog (`SetErrorMode`), for the same reason.
- stdin EOF or `quit` drains in-flight work, then exits 0.
- libav logging is off unless `-v` asks for it.

## Safety defaults

- Local files only: `protocol_whitelist=file`, also for nested opens, so a playlist on disk cannot make the tool fetch URLs.
- One-shot deadline `$PLAYPLAY_MEDIATOOL_TIMEOUT_MS` (default 120 s). ffprobe has none; this only stops a wedged read, for
  example on a dead share.
- Two decoder threads per thumbnail.
- Thumbnails go through `QSaveFile`, so no half-written JPEG ever sits under the expected name. The cancel flag and the
  deadline are checked once more just before the commit.

## The graph runner's argument subset (DEP-05a, `argparse.h`)

The Supervisor hands the tool ffmpeg's own command line for spot composition (`SpotGenerator::composeStories` with
`headline::storiesFilter`, `SpotGenerator::compose`) and render-farm jobs (`suites::presetArgs` in transcode, split
segment and concat form). `parseGraphArgs` takes exactly those argument lists and nothing else:

- global: `-y -n -v|-loglevel -hide_banner -nostdin -nostats -progress pipe:1 -filter_complex <graph>` (once);
- per input: `-ss -t -loop 0|1 -stream_loop -1 -framerate -f concat -safe 0`, then `-i <local file>`;
- per output: `-map [label] -vf|-filter:v -af|-filter:a -vn -an -c copy -c:v libx264|copy|mjpeg -c:a aac|pcm_s16le|copy
  -preset -crf -g -pix_fmt -color_range -r -b:a -ar -ac -movflags +faststart -avoid_negative_ts make_zero -frames:v -q:v
  -t`, then one output file (`.mp4 .m4v .mov .wav .jpg`).

Positions follow ffmpeg (a per-input option applies to the next `-i`, a per-output option to the output after the last
`-i`; anything per-file after the output file, a second output or a second `-filter_complex` is refused rather than
reinterpreted). Filtergraphs are tokenised like libavfilter's graphparser: every filter must exist in this libavfilter,
every `[k:v]` / `[k:a]` must read an input that exists, every labelled output must be mapped exactly once and no chain may
end unlabelled (ffmpeg would map it on its own). The strings themselves stay verbatim for `avfilter_graph_parse_ptr`.
Outside the subset → exit 64 (`unsupported: ...`), the Supervisor runs ffmpeg.exe. A command line ffmpeg itself would
refuse (a missing value, `-y` with `-n`, an unknown `-map` label, an input index past the inputs) → exit 2.
`test_inhouse_graphrunner` parses every argument list the three builders produce (all presets in all three render-farm
forms; 1 / 3 / 7 scenes with and without music and animated beds; the single-card compose with and without music).

## The graph runner (DEP-05b, `graphrunner.h`, `concat.h`)

A command line the parser accepted runs in-process the way ffmpeg.exe runs it:

- every `-i` is opened with libavformat: the image2 demuxer with `loop` / `framerate` for `-loop 1` stills and
  `frame_%04d.png` sequences; `-stream_loop -1` re-seeks to the start on EOF and adds ffmpeg's timestamp offset (the
  longest stream read so far); `-ss` seeks to the keyframe before t (3/23 s earlier with B-frame delay, as
  `ffmpeg_demux.c`), rebases timestamps so t is 0 and drops / cuts what is before it (ffmpeg's accurate seek);
  `-t` on an input ends each stream at its first packet with dts at or past t and cuts the decoded frames there;
- only the streams the graph reads are decoded (two threads each) and fed to `buffer` / `abuffer` sources standing for
  the `[k:v]` / `[k:a]` labels (the first stream of that type in input k; `-vf` / `-af` use ffmpeg's automatic mapping:
  largest picture, most channels), with ffmpeg's autorotate (display matrix → `transpose` / `hflip` / `vflip`) first;
- the `-filter_complex` string goes unchanged into `avfilter_graph_parse_ptr`; behind every mapped output the filters
  ffmpeg adds: `format` (the `-pix_fmt` and `-color_range`, else what the encoder takes), `aformat` (`-ar`, `-ac`, the
  encoder's sample formats) and `trim` / `atrim` for the output `-t`, then the sink (sized to the AAC frame);
- the graph is driven as fftools did: `avfilter_graph_request_oldest`, then the input whose sources failed the most
  requests gets one more packet; an input all of whose uses the graph closed (a looped still after its `trim`) is not
  read again; frames go through ffmpeg's cfr video sync (`-r`: duplicate / drop onto the output grid; vfr rounding for
  an mp4 without `-r`) into libx264 (`-preset -crf -g`, x264's own defaults otherwise: keyint 250, threads auto), aac
  (`-b:a`) or pcm_s16le, and into the mp4 / mov / wav muxer with `+faststart` and `-avoid_negative_ts make_zero`;
  the muxer gets ffmpeg's `Lavc... libx264` encoder tag and its DTS fix-ups (`ffmpeg_mux.c`: a non-monotonic dts is
  moved up with the same warning), so the files come out byte-identical on the fixtures;
- `-c copy` next to an encoded stream (the `loudnorm` preset) is a packet remux of that stream; a command line with
  nothing to encode (the concat join, a `copy` segment) is the remux of `concat.h`: the input's best video and audio
  stream with their parameters and tags, the packets from the keyframe before `-ss` kept as ffmpeg keeps them, the
  concat demuxer's continuous timestamps.
- `-progress pipe:1` prints ffmpeg's blocks (`frame fps bitrate total_size out_time_us out_time_ms out_time dup_frames
  drop_frames speed progress=continue|end`) on stdout every 500 ms; `suites::progressSeconds` reads them as it reads
  ffmpeg's. The result line still comes last.

Not reproduced (exit 64 from the parser, so ffmpeg.exe runs): anything outside the subset, the render farm's `custom`
preset, a `-map` of a file stream, non-file inputs. A render has no default deadline (ffmpeg has none);
`PLAYPLAY_MEDIATOOL_RENDER_TIMEOUT_MS` sets one. A failed run leaves the partial output as ffmpeg does; the caller
deletes it as it does after an ffmpeg.exe failure.

`test_inhouse_graphrunner` renders the 3-scene stories spot of `tests/inhouse/data/graph` (two JPEG stills, one
`-stream_loop` frame sequence, three looped furniture PNGs, three narration WAVs, a looped music bed) with the tool and
with the reference ffmpeg.exe: same duration, stream layout, `moov` before `mdat`, PSNR >= 40 dB on sampled frames
(byte-identical on this host), then the 720p transcode with `-progress`, three `proxy_540p` split segments joined by the
concat remux (the single-pass duration), and the copy / wav / loudnorm forms, all byte-identical to ffmpeg.exe.

## Measured (this host, FFmpeg 9.0.2)

- One-shot `probe` of the JSON form takes about 62-66 ms per spawn, the same as ffprobe.exe; process start dominates.
- `--serve` answers 200 mixed requests (probes, durations, 8 thumbnails, failures) in 355-395 ms over 3 workers.

## Users

None yet. DEP-04 adds `mediatoolclient` (kept out of the kit, compiled into the Supervisor) and the ingest switch
`ingest.probeEngine` / `ingest.probeParallel`; DEP-06 switches spot composition (`spots.engines.compose`) and the render
farm (`render.engine`) to the ffmpeg default mode, with ffmpeg.exe as the fallback on exit 64.
