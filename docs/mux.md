# ATSC multiplexer, encoders and streaming outputs

`playout/supervisor/muxmanager.*` (Supervisor), `playout/engine/encoderstage.*` + `engine.cpp` outputs
(engine), REST in `supervisorcore_library.cpp`, UIs `web/mux.js`, `qml/views/MuxView.qml`,
`qml/views/OutputView.qml`. Ported from the Starfile `broadcast_gui` controller; verified on the
DTA-2154 (`tsdektec -a`: device 0, output channel 0 = port 2) with TSDuck 3.41 and conda FFmpeg 7.1.1
(libx264/libx265/mpeg2video/ac3/aac/mp2, NVENC present, **no QSV, no libsrt, no libndi**).

## Services (`mux.services`)

One service per sub-channel of the transport. Source kinds:

| kind | feed | notes |
|---|---|---|
| `channel` | a PlayPlay channel engine gets a UDP TS output `mux:<id>` (multicast `239.255.25.N:udpPort`) | encoder profile below is pushed into the engine output |
| `ndi` | `{kind:"ndi", ndiName:"MACHINE (Source)", audio:true}` → hidden channel `mux-<id>` | profile created by `MuxManager::syncManagedChannels()`: `extra.hidden`, `extra.managedBy="mux"`, `extra.fixedSource={type:"ndi", name, title, audio}`, raster/fps/audio channels from the service encoder, outputs = the same UDP TS output. The ScheduleExecutor pins such a channel to the fixed source; `SupervisorCore::applyMuxOutputs()` spawns/restarts/kills the engine. `{kind:"input", input:{type:"ndi"}}` is normalised to this. |
| `input` (network) | `{kind:"input", input:{type: srt\|rtmp\|rtsp\|udp\|http\|https, location, transport}}` → hidden channel `mux-<id>` (DEP-18) | same managed channel as `ndi`, pinned to `extra.fixedSource={type:"stream", url: location, realtime:true, inputOptions:{rw_timeout:"10000000", rtsp_transport}, firstFrameTimeoutMs:8000, title}`; slate caption "STREAM SOURCE OFFLINE". The engine's `EncoderStage` encodes it, so every encoder field applies (closed GOP, interlaced coding). `input.feed:"ffmpeg"` keeps the old ffmpeg encoder for that service. |
| `input` (local) | an ffmpeg process we spawn: `test`, `file`, `dshow` (and network types with `feed:"ffmpeg"`) | `MuxManager::ffmpegArgs` |
| `udp` | an externally produced CBR TS | merged as-is |

Network inputs as managed channels (DEP-18), what changes for the operator:

- The hidden channel and its engine are created when the service list is saved (`servicesChanged` → `applyMuxOutputs`), as for
  `ndi`, and keep running while the mux is stopped (the ffmpeg encoder ran only while the mux ran). Changing the URL updates
  `fixedSource` and the executor re-pins the item (no engine restart); a raster / fps / audio-channel change restarts it.
- A service that turns engine-fed while the mux runs loses its ffmpeg encoder at once (kill, not waited for), so only one
  sender ever feeds the service multicast; `startEncoders` and the watchdog skip engine-fed services. Turning a service back to
  ffmpeg (`feed:"ffmpeg"`, or a local type) removes the channel; its encoder starts with the next mux start / restart.
- A source that stops sending ends the read after 10 s (`rw_timeout`); the item ends and the ScheduleExecutor re-arms it
  (slate meanwhile). Log lines show stream URLs without user info (`rtsp://user:pass@…` → `rtsp://…`).
- `GET /mux/dryrun` lists these services under `channels[]` (`managed:true`, `fixedSource`), not `encoders[]`;
  `GET /mux/status` lists them in `managedChannels`.
- `test` (lavfi), `file` and `dshow` stay on ffmpeg: the engine's generator, looping-file and dshow paths are not verified as
  mux feeds yet.

NDI sources for the picker come from `GET /api/v1/inputs` (`ndi[]`, engine discovery).

### Encoder (per service, `service.encoder`)

```json
{ "encoderPreset": "atsc_h264_ac3", "codec": "libx264|libx265|mpeg2video|h264_qsv|hevc_qsv|h264_nvenc|hevc_nvenc",
  "width": 1280, "height": 720, "fps": "30000/1001", "vidBitrateKbps": 3000, "maxrateKbps": 0, "bufsizeKbps": 0,
  "rateControl": "cbr|vbr|cqp", "qp": 23, "gop": 30, "keyintMin": 0, "bframes": 2, "closedGop": true, "scenecut": false,
  "preset": "veryfast", "profile": "high", "level": "4.1", "interlaced": false, "afd": -1, "ccPassthrough": true, "extra": "x264 params",
  "audio": { "codec": "ac3|aac|mp2|eac3", "bitrateKbps": 192, "channels": 2, "sampleRate": 48000, "dialnorm": -27, "mixingLevel": -1, "aacProfile": "aac_low", "normalize": false },
  "ts": { "pmtPid": 4096, "pcrPid": 48, "pcrPeriodMs": 20, "patPeriodSec": 0.1, "provider": "PlayPlay", "muxrate": 0 } }
```

`vidPid`, `audPid`, `serviceId`, `udpPort`, `major/minor`, `shortName/longName` stay on the service
(`encoder.ts.vidPid/audPid/serviceId`, when sent, are copied up). Legacy flat `audCodec/audBitrateKbps/
audChannels` are still read and mirrored. `encoder.applyPreset: "<id>"` in a PUT merges a preset server-side.

Both feed paths honour every field: `engineOutputsFor` → engine `EncoderProfile` (`videoCodec, width,
height, rateControl, maxrateKbps, bufsizeKbps, qp, gopFrames, keyintMin, scenecut, closedGop, bFrames,
x264Preset, profile, level, interlaced, ccPassthrough, afd, audioCodec, audioBitrateKbps, audioChannels,
dialnorm, mixingLevel, aacProfile, extra`) + TS options (`muxrate, serviceId, serviceName, provider,
videoPid, audioPid, pmtPid, pcrPid, pcrPeriodMs, patPeriodSec, tsId`); `ffmpegArgs` → the equivalent
ffmpeg flags (`-minrate/-maxrate/-bufsize`, `-x264-params nal-hrd=cbr:keyint:min-keyint:scenecut:open-gop`,
mpeg2 `-intra_vlc -non_linear_quant -seq_disp_ext -video_format component -flags +cgop(+ilme+ildct)
-alternate_scan`, `-a53cc`, NVENC `-rc/-zerolatency/-forced-idr/-strict_gop`, QSV `-forced_idr -look_ahead 0`,
AC-3 `-dialnorm/-mixing_level`, AAC `-profile:a`, optional `loudnorm=I=-24` on `audio.normalize`).
When `statmux.managed` the allocation **caps** the video rate (capped VBR); otherwise the service's
`rateControl` applies. A codec missing from the ffmpeg build falls back to libx264 (libx265 for HEVC)
with a warning in the log/alerts; the engine does the same (`EncoderStage::actualVideoCodec()`), including
hardware encoders that are listed but cannot open. `afd` is stored and shown; it is signalled on the
SDI/VANC path, not by the MPEG encoders.

Interlaced coding is applied only on 1080/480/576-line output (720p/1080p stay progressive even when
the flag is set). `EncoderProfile.width/height` scale the channel canvas before encoding (a 1080p channel
can feed a 1280x720 service).

mpeg2 `input` services: `-flags` carries `+cgop` when `closedGop` (default true; same rule as the engine's
`AV_CODEC_FLAG_CLOSED_GOP`) and `+ilme+ildct` (with `-alternate_scan 1`) on an interlaced raster, so the
1080i/480i presets get `-flags +cgop+ilme+ildct`; an open-GOP progressive service gets no `-flags`. Until
2026-10-26 the statement sat inside a trailing `//` comment and was never emitted, and the capabilities probe
never listed `mpeg2video` (its `ffmpeg -encoders` pattern missed the `S` slice-threading flag of `V.S..D`), so
once probed every mpeg2 input service silently fell back to libx264 with a warning. `test_inhouse_psip` checks
both; with FFmpeg 9 the 1080i/480i presets now produce top-field-first interlaced MPEG-2 with closed GOPs.
FFmpeg's mpeg2video cannot do closed GOPs with scene-change detection (the encoder refuses to open), so on an
mpeg2 service closed GOP wins: `scenecut` is honoured (`-sc_threshold 0`) only when `closedGop` is false;
otherwise `-sc_threshold 1000000000` and the dry run / start-up warning says scene-cut was ignored.

### Presets (`GET /api/v1/mux/encoder-presets`, `EncoderProfile::preset`)

`atsc_mpeg2_ac3_hd` (1080i MP@HL 12 Mb/s CBR, AC-3 384k), `atsc_mpeg2_ac3_sd` (704x480i MP@ML 4 Mb/s,
AC-3 192k), `atsc_h264_ac3` (720p high@4.1 6 Mb/s CBR, AC-3 384k), `stream_h264_aac_1080p` (6 Mb/s VBR
max 7, AAC 160k), `stream_h264_aac_720p` (3.5 Mb/s VBR, AAC 128k), `stream_hevc_aac` (libx265 4 Mb/s VBR),
`qsv_h264_aac` (h264_qsv, falls back to libx264 here), plus the legacy `mpeg2_ac3`, `h264_aac`, `h264_aac_low`.
All presets: closed GOP, scene-cut off, dialnorm −27, loudness target −24 LKFS (A/85, informational).

## Transport, PSIP and the tsp master

Config `mux.config`: `output {kind: dektec|udp|srt|file, device, channel (tsp --channel), port (tsdektec
--output, 1-based), url, bitrate (default 19392658), preloadMs (4000), maintainPreload, stuffing, ttl,
latencyMs}`, `tsid`, `timezone`, `mcastBase`, `statmux {enabled, reservedBps, defaultMinKbps, defaultMaxKbps}`,
`tsduckDir`, `ffmpegPath`, `autoStart`, `alerts`, `tables` (`native` default | `tstabcomp`), `tools` (`auto` default |
`native` | `tsduck`), `playplayMux` (path; default next to the Supervisor, else PATH) — the last three: DEP-15 below. The
transport bitrate drives `--bitrate`, the statmux budget and the deviation alarm.

Tables are written to `<data>/mux/tables` (`pat.xml`, `mgt.xml`, `tvct.xml`, `stt.xml`, `eit<k>.xml`,
`ett<k>.xml`, optional `rrt.xml`, `custom.xml`), checked by compiling them (in-process with PlayPlayTsKit since DEP-15, or
`tstabcomp --atsc`; the `.bin` copies go to `<data>/mux/compiled`), injected with `--poll-files` so rewrites are picked up
live. STT `system_time` is GPS seconds (integer, UTC + 18).
Overrides (`mux.tableOverrides`, `PUT /api/v1/mux/tables/overrides`):

```json
{ "tvct": { "modulation": "8-VSB", "channelTsid": 25, "carrierFrequency": 0, "extraXml": "" },
  "services": { "svc1": { "sourceId": 101, "serviceType": "dtv", "hidden": false, "hideGuide": false, "accessControlled": false, "etm": 0, "extraXml": "" } },
  "rrt": { "enabled": false, "region": 1, "xml": "" }, "eitDays": 1, "eitIntervalSec": 0, "sttIntervalSec": 5, "mgtIntervalSec": 0,
  "customXml": "", "customPid": "0x1FFA" }
```

`eitDays` sets the EIT/ETT depth (8 tables per day, max 128); interval overrides convert seconds to
`--inter-packet` counts (defaults are the proven 300 / 1200 / 6000). The US RRT is only sent when enabled
(A/65 treats region 1 as built in). `GET /api/v1/mux/tables` returns every XML file plus a structured
summary (`psip.tvct[]`, `psip.mgt[]`, `psip.eit[].events[]`, `psip.stt`); `POST .../tables/compile` gives the
per-file compiler result (`files[] {file, ok, exitCode, output, engine, ms, sections?, fallback?, nativeError?}`, `engine`,
`writeMs`, `totalMs`); `GET .../tables/analyze?mode=tables|analyze&service=` runs `PlayPlayMux analyze` (or
`tsp -I ip … -P until --seconds 3 -P tables|analyze -O drop`) against the UDP output (PSIP pid), the output file
(`output.kind` file: the first 3 s of packets at the transport rate) or a service multicast and returns the text
(asynchronous, 20 s cap; `engine`, `summary` = PlayPlayMux's JSON).

Auto-start: `autoStart` starts the mux 3 s after the Supervisor comes up, retrying with 3/6/12/24/48 s
backoff; after the fifth failure a critical alert (and Twilio SMS when configured) is raised.

`POST /api/v1/mux/dryrun` returns the exact command lines without starting: `dektecPrep` (`tsdektec
--output <port> <device>` or `PlayPlayMux dektec prep --port <port> --device <device>`), `tools {engine, playplayMux,
tables, portSetup}`, `tsp {exe, args[], commandLine}`, `encoders[] {serviceId, commandLine, error}`,
`channels[] {channelId, managed, fixedSource, outputs[]}`, `tables`, `warnings[]`. `GET /api/v1/mux/dektec`
lists the cards (`PlayPlayMux dektec list -v` or `tsdektec -a -v`, asynchronous since DEP-15: `devices[] {index, model,
description, ports, inputs, outputs, serial, firmware, channels[] {kind, index, port, label, standards}}`, `dtapi`, `dta`,
`engine`, `tool`). `GET /api/v1/mux/capabilities[?refresh=1]`
(cached 10 min): `ffmpeg {path, version, found, video[], audio[], has{codec:bool}, build{srt, rist, ndi, qsv, nvenc, x264, x265}}`,
`tsduck {dir, version, found, tools{tsp, tstabcomp, tsdektec, tsanalyze, tspacketize}}`, `dektec`, `presets[]`.

**Capabilities never wait on a child process (DEP-02b, 2026-10-26).** They used to spawn `ffmpeg -hide_banner -encoders`
(waiting up to 15 s), `ffmpeg -version` (10 s), `tsp --version` (10 s) and `tsdektec -a -v` (15 s) one after the other on the
Supervisor main thread, which also serves REST, IPC and the schedule clock: 0.45-0.65 s per refresh on this box, up to 50 s
when a tool hangs, and every dry run and mux start paid it on a cold cache. Now:

- `ffmpeg` comes from **PlayPlayAvKit** in-process (`avkit::ffmpegCapabilities()`, `tools/avkit/avcaps.h`): the encoders and
  `avcodec_configuration()` of the libav the Supervisor links, which is the FFmpeg build staged next to it and the engines
  (the `ffmpeg.exe` that `input` services run). Same keys; `has{}`, `build{}`, `video[]`, `audio[]` equal to the old parse
  (checked by `test_inhouse_psip` against the build's ffmpeg.exe); `path` is `"(in-process libav)"` and `version`
  `"ffmpeg version n9.0.2-... (in-process libav)"`. A `mux.config.ffmpegPath` that points at a different FFmpeg build is not
  what this list describes (the codec fallback reads it).
- `tsduck.version` is read from `tsp.exe`'s version resource (TSDuck stamps the `3.41-4299` that `tsp --version` ends with);
  nothing is started. Without TSDuck: `found: false`, `version: ""`, every tool `found: false`, `dektec {devices: [],
  found: false}`, no process, about 25 ms (the PATH lookup of the missing tools now asks for `tsp.exe` itself, not every
  PATHEXT variant of `tsp`, which cost about 50 ms per tool).
- `dektec` is the last `tsdektec -a -v` result. A refresh (or a result older than 10 min) starts the listing **in the
  background** (`dektec.probing: true`, 15 s kill timer, adopted into the kill-on-close job) and the next call has the new
  list; until the first listing ends, `dektec.raw` says it is running (both UIs show `raw` under "no DekTec device found").
  `GET /api/v1/mux/dektec` reuses a background result younger than 10 min (and since DEP-15 never waits either). A mux start
  no longer runs the listing at all (it only needs the encoder list, which it takes in-process), so it can no longer run next
  to the starting tsp attaching the DekTec output. The background listing runs `PlayPlayMux dektec list -v` when
  PlayPlayMux.exe is found (`mux.config.tools` auto / native; `dektec.engine: "native"`) and `tsdektec -a -v` otherwise; same
  keys either way.

Measured on a test Supervisor (DTA-2154 present, TSDuck 3.41).

### In-house section library: PlayPlayTsKit and `PlayPlayMux` (DEP-14)

`tools/mux/` (static library `PlayPlayTsKit`, QtCore only; `cmake/lane-inhouse-mux.cmake`) builds every table the mux
injects without TSDuck: MPEG-2 CRC32, sections and their TS packetization (`tssection.h`), PAT, PMT and the SCTE-35 PMT
rewrite, MGT, TVCT (extended channel names, hidden / hide_guide), RRT, EIT, ETT, STT (`psip.h`), the SCTE-35
splice_info_section (`scte35bin.h`: splice_insert out / in / cancel, timed or immediate, components, splice_null,
time_signal), and a compiler for the TSDuck XML that `writeTables` and `scte35.cpp` write (`tskit::compileXml`). Output is
byte-identical with `tstabcomp --atsc` / `tspacketize` of TSDuck 3.41: `tests/inhouse/data/psip/*.xml` with the `*.bin` /
`*.ts` those tools made, checked by `test_inhouse_psip` (which needs no TSDuck; with TSDuck installed it also compiles what
`writeTables` writes today and compares). Since DEP-15 `MuxManager::compileTables` uses it in-process
(`mux.config.tables = native`, the default) and keeps `tstabcomp` for files it rejects (below).

`PlayPlayMux.exe` (built by name, not deployed; `version` lists its commands and whether DTAPI is compiled in) wraps it for scripts, with the in-house tool contract (last stdout line is
JSON; exit 0 ok, 2 failed, 64 unsupported = use the TSDuck tool):

| command | replaces |
|---|---|
| `compile [--atsc] [--output F \| --outdir D] [--check] <xml>...` | `tstabcomp --atsc` (same `.bin` layout: sections back to back) |
| `pmt-scte35 --input pmt.bin --pid 0x0400 [--output F]` | `tsp -P pmt --add-pid <pid>/0x86 --add-registration 0x43554549` |
| `packetize --pid P [--stuffing] [--continuity N] --output F <bin\|xml>...` | `tspacketize` (packed sections; `--stuffing` = one section per packet start) |
| `analyze [--mode tables\|analyze\|both] ... <file \| addr:port>` | `tsp -I file\|ip ... -P tables` / `-P analyze` (DEP-16, below) |
| `dektec list [-v] [--probe]` | `tsdektec -a [-v]` (DEP-17, below) |
| `dektec prep --port P [--device D] [--check] [--force]` | `tsdektec --output P D` (DEP-17, below) |

TSDuck behaviours it copies, so the bytes on air do not change: string mode 0x00 (Latin-1) or the common Unicode page,
else UTF-16 (mode 0x3F); an empty EIT title is one empty segment, other empty strings have none; EIT `start_time` is
seconds since 1980-01-06 **without** the 18 leap seconds (the STT carries `GPS_UTC_offset` 18, so receivers show events 18 s
early, as today); XML names are case-insensitive and `&#233;`-style references stay literal text; PAT / TVCT split at 1024
bytes, EIT at 4096. **One deliberate difference:** `tstabcomp` truncates a string longer than 255 encoded bytes to 256 and
writes `number_bytes = 0`, which corrupts the table. `writeTables` puts descriptions of up to 1000 characters into the ETTs,
so a schedule description over 255 characters (127 in UTF-16) produces a broken ETT on air today. PlayPlayTsKit splits such
text into several segments of at most 255 bytes, as A/65 allows.

### TS analyzer: `PlayPlayMux analyze` (DEP-16)

`tools/mux/tsanalyzer.*` (in PlayPlayTsKit) analyses a transport stream from a file or from UDP (`udpin.*`: unicast or
multicast, `SO_REUSEADDR` so it can listen next to the mux, default interface or `--local-address`, `source@group:port` for
SSM, RTP headers stripped). It replaces the `tsp -I ip ... -P tables / -P analyze -O drop` that `MuxManager::analyze` ran
(since DEP-15 it runs PlayPlayMux, asynchronously, when the tool is found) and the TSDuck checks of `scripts/soak.ps1`.

```
PlayPlayMux analyze [--mode tables|analyze|both] [--seconds S] [--receive-timeout MS] [--pid P|P1-P2]... [--max-tables N]
                    [--max-packets N] [--local-address A] [--buffer-size B] [--jitter-max TICKS] [--output F] [--quiet]
                    <file.ts | [source@]address:port | udp://...>
```

- **`--mode tables`** (default) prints the tables in the layout of `tsp -P tables`, line for line: PAT, PMT, SDT, ATSC
  MGT / TVCT / RRT / EIT / ETT / STT and SCTE-35 splice_info_section, with registration, ISO-639, service, stream identifier,
  extended channel name, service location, caption service and content advisory descriptors; other tables and descriptors
  get TSDuck's name and its hex dump. The demux follows TSDuck: sections on every PID (or `--pid`), PES PIDs skipped,
  sections with a bad CRC32 or broken by a continuity error dropped, "next" sections ignored, a long table shown each
  time a new version completes, every short section (SCTE-35 cues) shown, DVB descriptors decoded only after a DVB table.
- **`--mode analyze`** prints a report: TS bitrate (from the PCRs of a CBR mux, else arrival times), services (PAT + PMT +
  SDT names + TVCT channel numbers), per PID packets / bitrate / % / continuity errors and what the PID is, PCR interval
  (min / avg / max, intervals over 100 ms), PCR jitter against the constant TS rate (`tsp pcrverify` style, threshold
  `--jitter-max`, default 1350 ticks = 50 us) and, over UDP, peak-to-peak PCR-vs-arrival jitter, PTS / DTS minus PCR per
  PES PID, table repetition intervals, decoded SCTE-35 cues and the error list (sync loss, transport error, continuity with
  expected / got CC, CRC with table id). `both` prints tables then the report.
- The last stdout line is the JSON summary (`packets`, `bitrate`, `bitrateSource`, `durationSec`, `ccErrors`, `crcErrors`,
  `pids[]` with `pcr {intervalMs, jitterNs, ...}` and `ptsPcrMs`, `services[]`, `tables[]` with `intervalMs`, `errors[]`,
  `scte35[]`, `tablesDisplayed`; UDP adds `datagrams`, `receiveTimeout`). UDP without any datagram within
  `--receive-timeout` (default 5000 ms) is `ok: false` (exit 2). UDP listens `--seconds` (default 3 s, like
  `tsp -P until --seconds 3`); for a file `--seconds` limits the stream (PCR) time read.

Checked by `test_inhouse_tsanalyzer`: the tsp text of every DEP-14 golden (`tests/inhouse/data/psip/tables/*.txt`, recorded
once with `tsp --atsc -P tables`, so the test needs no TSDuck), a stream written by the engine's `TsMuxer` with injected PSIP
and cues (bitrate = muxrate, PCR interval = `pcrPeriodMs`, an injected CC break and a bad TVCT CRC reported, and with TSDuck
installed the text equals `tsp -I file -P tables`), UDP unicast and multicast on loopback, and the CLI. On this box the
live station's last written table set (PAT, MGT, TVCT, STT, eight EIT and ETT PIDs) also decodes identically to `tsp`, and
30 s of a 19.39 Mb/s stream is analysed in about 0.05 s (tsp `-P analyze`: 0.36 s).

### DekTec device tool: `PlayPlayMux dektec` (DEP-17)

`tools/mux/dtdevice.*` (static library `PlayPlayDtDevice`, linked with DTAPI where the SDK is found) replaces the two
tsdektec calls of `MuxManager`: `tsdektec -a -v` in `dektecDevices` and `tsdektec --output <port> <device>` before tsp starts.
MuxManager runs it asynchronously since DEP-15 (the listing whenever PlayPlayMux is found, the port setup only with
`mux.config.tools = native` or without tsdektec.exe; below).

```
PlayPlayMux dektec list [-v|--verbose] [--probe] [--quiet]
PlayPlayMux dektec prep --port P [--direction output|input] [--device D | --serial S | <device>] [--io-standard asi|keep]
                        [--check] [--force]            (tsdektec spelling: prep --output P [D] / --input P [D])
```

- **`list`** prints `tsdektec -a` lines (`-v`: the `tsdektec -a -v` layout, which `dektecDevices`' parser reads unchanged,
  plus one `Port N: <direction>, <standard>[, free|in use]` line per port), then the JSON: `devices[] {index, model,
  description, typeNumber, serial, ports, inputs, outputs, firmware, firmwareVariant, pciBus, slot, channels[] {kind,
  index, port, name, label, standards, interface, caps[], inUse}, portConfig[] {port, ioDir, ioStd, canInput, canOutput,
  canAsi, canSdi}, vpd}`, `dtapi`, `dta`, `service`. The fields `dektecDevices` returns today (`index, model, description,
  ports, inputs, outputs, serial, firmware, channels {kind, index, port, label, standards}`, `dta`) are identical to what it
  parses from `tsdektec -a -v` on this box's DTA-2154 (`name` / `standards` follow tsdektec, which composes them itself:
  "DTA-2154 port 1 (top)", "ASI, SDI"; DTAPI 6.13's own text "ASI/SDI" is `interface`). `dtapi` is the DTAPI linked into
  PlayPlayMux (6.13), not tsdektec's (6.9). No card, no driver or no DTAPI compiled in: `devices: []`, exit 0 (a failed scan
  adds `scanError`). `--probe` asks DTAPI whether each channel is attached (`AttachToPort(..., ProbeOnly)`: nothing is
  held); plain `list` reads only (shared device attach, VPD, `GetIoConfig`), like tsdektec.
- **`prep`** makes a port an output and, unless `--io-standard keep`, DVB-ASI (`SetIoConfig` IODIR = OUTPUT / OUTPUT, then
  IOSTD = ASI, as `DekTecAsiSink` does); `--direction input` (`--input P`) sets an input and leaves the standard. It is
  stricter than tsdektec, which re-applies the direction every time: it **refuses a port whose channel is attached** by any
  process (the channel its current direction gives it: an engine's DekTec ASI sink or a running tsp) or whose state cannot
  be probed, with `ok: false, refused: true, inUse`, exit 2 and nothing changed, even when nothing needed changing; it
  re-applies nothing that already matches (`--force` does, still refusing an attached port); a double-buffered copy
  (`ioDirSub: dblbuf`) is made a plain output. `SetIoConfig` answering "attached" (a channel taken between probe and write)
  is the same refusal. After writing it reads the port back and fails if the configuration did not take. `--check`
  reports `wouldChange` and the planned `actions` without writing. Result: `{ok, device {index, model, serial}, port,
  direction, before, after, actions[], changed, wouldChange, inUse, check}`. Without DTAPI: `unsupported` (exit 64), so a
  caller falls back to tsdektec.
- **`--sim F`** (or `PLAYPLAYMUX_DEKTEC_SIM=F`) runs both commands against a JSON-described card instead of the hardware
  (`tests/inhouse/data/psip/dektec/dta2154_sim.json` is this box's card, with port 2 marked in use); `prep` writes the
  new port state back to F. Results carry `"backend": "sim", "simulated": F`. A test Supervisor can use it so no test
  ever touches the on-air card.

Checked by `test_inhouse_dtdevice` (built by name): 22 prep decisions on simulated cards (refusals, check, force, read-back,
port / capability / device errors, call order), list JSON and text through a copy of `dektecDevices`' parser (and that
parser on a recorded `tsdektec -a -v` of this box), the CLI with `--sim`, and on the real DTA-2154 `list` equal to
`tsdektec -a -v` plus `prep --check` on every input port (read-only). With `PLAYPLAY_DEKTEC_HWTEST=1` it holds a free input
port's channel and sees `prep` refuse it (`=2` also re-applies that port's own direction with `--force`); output ports are
never prepared by the test. `list` takes about 0.25 s here (tsdektec -a -v: 0.3 s).

### MuxManager without waits: in-process tables, asynchronous tools (DEP-15)

`MuxManager` used to wait on child processes on the Supervisor's main thread (the thread that serves REST and IPC and runs the
schedule clock): up to 260 sequential `tstabcomp` spawns (10 s cap each) at every validate, compile and mux start,
`tsdektec -a -v` (15 s) for `GET /mux/dektec`, `tsdektec --output` (10 s) before tsp, and up to 3 s in `waitForStarted` for
the analyzer. Now:

- **Tables** (`mux.config.tables`, default `native`): every table file is compiled in-process by PlayPlayTsKit (DEP-14,
  byte-identical with `tstabcomp --atsc`); nothing is spawned while every file compiles. A file it rejects goes to
  `tstabcomp` when TSDuck is installed, whose verdict decides as before (tsp injects the XML itself, so a file tstabcomp
  accepts never stops a start): `fallback` (outside the subset, e.g. an SDT in `customXml`) or `nativeError` (any other
  rejection; the Supervisor log says when tstabcomp accepted it). Without tstabcomp such a file fails with exit 64 / 2 and
  the in-house message. **Operator XML** (`customXml` → `custom.xml`, an `rrt.xml` override, any TVCT / service
  `extraXml` → `tvct.xml`) also goes through `tstabcomp` when it is installed, even when PlayPlayTsKit compiles it:
  PlayPlayTsKit ignores attributes it does not know, while TSDuck's model (tstabcomp, and tsp's inject plugin that puts
  the XML on air) rejects them, so a mistyped attribute still fails Validate / start with tstabcomp's message (report:
  `engine: tstabcomp`, `operatorXml: true`). Generated files never spawn anything. `tables = tstabcomp` is the old path
  (all files spawned). The compile report adds `engine`, `ms`, `sections` per file and `writeMs` / `totalMs`.
- **Table files are replaced without a disk flush**: `writeTables`, `stt.xml` (every 5 s while running) and the compiled
  `.bin` copies are written to `<name>.writing` and renamed over the old file (still atomic for tsp's `--poll-files`), not
  with QSaveFile, whose `FlushFileBuffers` cost ~6 ms per file here (1.6 s for eitDays 16's 261 files, as much again for
  the `.bin` copies). A file that already holds the same bytes is not rewritten (same minute = same table version).
- **Tools** (`mux.config.tools`): `auto` (default) = `PlayPlayMux.exe` when found (`mux.config.playplayMux`, else next to
  the Supervisor, else PATH), else TSDuck; `native`; `tsduck`. Each runs as an asynchronous child (`runTool`: adopted
  into the kill-on-close job, kill timer, FailedToStart answered in a later event-loop turn):
  - `GET /tables/analyze`: `PlayPlayMux analyze --mode tables|analyze --input udp|file ...` (same PIDs, `--max-tables 12`,
    3 s) or tsp as before; with `output.kind` file it reads the output file (3 s of packets at the transport rate) instead
    of the first service multicast. PlayPlayMux not starting or answering "unsupported" falls back to tsp (`fallback`).
  - DekTec listing (`GET /mux/dektec`, `capabilities`): `PlayPlayMux dektec list -v` (same device keys plus the tool's
    `portConfig` / `inUse` / ...) or `tsdektec -a -v`; a PlayPlayMux without DTAPI (`available: false`) falls back to
    tsdektec. `GET /mux/dektec` is asynchronous: `refresh=0` answers from a listing younger than 10 min at once, otherwise
    the reply waits for the background listing (the socket is held, the event loop keeps running).
  - Port setup before tsp: `start()` does the in-process part (allocation, tables, tsp present) and, for a DekTec output,
    returns with state `starting`; `PlayPlayMux dektec prep --port P --device D` (with `tools = native`, or when
    tsdektec.exe is missing) or `tsdektec --output P D` (`auto` while tsdektec exists, i.e. whenever TSDuck runs the mux)
    then runs as a child and tsp starts when it succeeds. PlayPlayMux refuses a port whose channel is attached (an
    engine's ASI sink, a leftover tsp) with that reason instead of reconfiguring it; exit 64 / no JSON / crash falls back to
    tsdektec. `POST /mux/start` and `/mux/restart` reply when the start completed (200 + status) or failed (409 + reason);
    callers joining a start in progress get the same answer; `stop()` during the port setup answers them and the late
    result starts nothing. Auto-start and the tsp restart after an exit use the same path; the 3 s restart after a tsp
    exit does nothing when a Start / Restart / Stop came in meanwhile (a start still in its port setup keeps it).
- Routes: `installAsyncRoutes` (registered through `laneext.h`, after `registerRoutes`) replaces `GET /mux/dektec`,
  `POST /mux/start` and `/mux/restart` with asynchronous handlers; `/tables/analyze` was already asynchronous.
  `capabilities.playplayMux {path, found, tools, toolsSetting, tables}` and `dryrun.tools {engine, playplayMux, tables,
  portSetup}` show what is used.

Still synchronous (bounded, not tool runs): `stop()` kills tsp / encoders and waits up to 3 s / 2 s for them to exit (so the
DekTec port is free for a restart); the `tstabcomp` fallback and operator-XML check above (rare, one file each).

Known, outside the mux files: the station API listener (`station.port` > 0) copies the route table, and every
`registerAsync` route copied there (now also `/mux/dektec`, `/mux/start`, `/mux/restart`) answers through the main
listener's current socket, not its own (RestApiServer); use the main API port for these until that is fixed.

Checked by `test_inhouse_psip` (DEP-15 cases): eitDays 16 + RRT = 261 files compiled in ~0.5 s with no child, `.bin` equal
to PlayPlayTsKit's and to tstabcomp's; fallbacks and errors with and without tstabcomp; 1750 mangled XML documents rejected
with a message or compiled, never a crash; analyze of an output file and of UDP on loopback (answer after ~55 ms, event loop
never stalled over ~15 ms, text identical to `tsp -P tables` line for line), tsp fallback; `prepVerdict` /
`parseDektecList`; the DekTec listing and the start's port setup through PlayPlayMux on a simulated DTA-2154 (refused
port, a port switched to output, stop during the setup, missing tool); tool selection; the async routes over HTTP. No test
touches the real card: PlayPlayMux sees `PLAYPLAYMUX_DEKTEC_SIM`, PATH has no TSDuck and tsp is a stand-in.

## Streaming outputs (engine)

`profile.outputs[]` entries (`type: ndi|udp|srt|rtmp|rtmps|file|rtp|dektec_asi|dektec_sdi|null|nullbaseband`,
`name`, `url`, `profile` (preset name or EncoderProfile JSON), `options`, `enabled`, `autoStart`,
`retry {enabled, maxAttempts (0 = forever), backoffSec}`) become **output slots** in `PlayoutEngine`.
Packet outputs share one `EncoderStage` per identical profile; a stage is released when its last slot
stops. `configureOutputs` diffs by name: untouched definitions keep running (no on-air glitch on ASI/NDI
when one RTMP stream is toggled), changed ones restart, removed ones stop, new ones start when
`enabled && autoStart`. `enabled:false` = defined but off (`state: disabled`); `autoStart:false` = defined,
started manually. Failed starts and sinks that stop delivering (not running, or 5 s of write failures)
retry per policy with linear backoff; `state: error` + `lastError` + `retryInSec` otherwise.

Telemetry `outputs[]` keeps the encoder entries (`kind:"encoder"`, now with `videoCodec`, `width`,
`height`, `sinks`) and has one entry per slot: `{kind (sink type or def type), name, type, url, state
running|starting|stopped|error|disabled, lastError, enabled, autoStart, restarts, attempts, uptimeSec,
bitrateKbps, retryInSec, profile, videoCodec, audioCodec, encodedWidth/Height, packetsOut|framesOut,
dropped, bytesOut, connected, error, …sink extras}`. Sink alarms are only raised for running slots.

Engine command: `{action:"output", name, op:"start|stop|restart"}` (transport path; the Supervisor's
`onUiCommand` already forwards unknown actions as MSG_TRANSPORT_COMMAND) or MSG_OUTPUT_CONFIG /
config `{outputs:[…], op:{name, action}}`.

REST (`supervisorcore_library.cpp`):

| route | body / response |
|---|---|
| `GET /api/v1/channels/{id}/outputs` | `{channelId, channelName, engineConnected, hidden, running, defined, outputs:[{…def, state, live:{…telemetry}}], encoders:[…]}` |
| `GET /api/v1/outputs` | `{channels:[the same per channel]}` |
| `POST /api/v1/channels/{id}/outputs/{name}/start|stop|restart` | `{ok, queued}`; 404 unknown output, 409 engine offline |
| `POST /api/v1/channels/{id}/outputs/add` | `{output:{type, name?, url?, profile?, options?, enabled?, autoStart?, retry?}}` → merged list (unique name, defaults filled) |
| `PUT /api/v1/channels/{id}/outputs/{name}` | partial definition (e.g. `{autoStart:false}`) → merged list |
| `DELETE /api/v1/channels/{id}/outputs/{name}` | → merged list |

`PUT /api/v1/channels/{id}/outputs` (whole list) is unchanged in `supervisorcore.cpp`.

Mux feeds (`mux:<id>`) are regular slots with `managedBy:"mux"`, `autoStart:true`, unlimited retry.

### Facebook Live (`live.facebook`)

One RTMPS output named `facebook-live` on a chosen channel (web: Outputs tab, "Facebook Live" card; REST below). The operator pastes the
stream key from Facebook Live Producer, picks the channel and a quality profile (default `stream_h264_aac_720p`: H.264 3.5 Mb/s, 2 s keyframes, AAC 128k),
saves, then presses **GO LIVE** (and **Go live** on Facebook once its preview shows) and **END LIVE** to stop.

- **The key never leaves the Supervisor except to the engine.** It is stored in settings `live.facebook` `{channel, serverUrl, streamKey, preset}`; the output
  definition holds only `url` = the server (default `rtmps://live-api-s.facebook.com:443/rtmp/`) and `keyRef: "live.facebook"`. `ChannelManager::setOutputsResolver`
  (`livestream.h`) merges `server/key` into every `outputs` array on its way to an engine, so channel JSON, `/outputs`, session bundles and the database do not contain it.
  The engine reports and logs RTMP URLs masked (`rtmps://host/app/…xyz`), and the status route scrubs the key from error text. (The engine's own
  `<data>/channels/<id>/channel.json`, written when the engine is spawned, holds it if the key was saved by then, like any engine config; settings
  `live.facebook` in the database holds it too.) No key saved = the output stays disabled.
- `GET /api/v1/live/facebook` → `{channel, channelName, serverUrl, preset, hasKey, keyHint ("…abcd"), state (idle|pending|starting|running|error|stopped|offline|channel missing),
  live, uptimeSec, bitrateKbps, width, height, attempts, retryInSec?, lastError?, engineConnected, configured}`. `GET /settings/live.facebook` answers without the key.
- `PUT /api/v1/live/facebook {channel?, serverUrl?, streamKey?, preset?, clearKey?}` saves and syncs the output (moved off other channels, `autoStart:false`, 5 retries
  at 5 s). `streamKey` also accepts a pasted full address (`rtmps://…/rtmp/KEY`): the server part is split off. Empty `streamKey` keeps the saved key.
- `POST /api/v1/live/facebook/start|stop` → status (409 when no channel / key / saved output / engine offline).

## TS mux options (`TsMuxer::applyOptions`, shared by ASI and IP sinks)

`muxrate`, `pcrPeriodMs`, `patPeriodSec`, `sdtPeriodSec`, `serviceId`, `serviceName`, `provider`,
`serviceType`, `pmtPid`, `videoPid`, `audioPid`, `tsId`, `networkId`, `tablesVersion`, `pesPayloadSize`,
`muxDelayMs`. `pcrPid` must equal the video PID (libavformat carries the PCR there); a mismatch logs a
warning and is ignored.

## SCTE-35

`playout/supervisor/scte35.{h,cpp}` (pure helpers) + `MuxManager::{scte35Config, setScte35, scte35Status, cue, onChannelItem}`.
Off by default; while off, the tsp command line is exactly as before.

On the wire, for every service with SCTE-35 on (inserted after the merges, before the monitors):

```
-P pmt --service <sid> --add-pid <pid>/0x86 --add-registration 0x43554549          (SCTE-35 component + CUEI registration in the PMT)
-P spliceinject --service <sid> --pid <pid> --udp 127.0.0.1:<basePort + index - 1> --inject-count <n>
```

Cues are immediate `splice_insert()` sections sent to that UDP port as TSDuck XML: cue out (`out_of_network=1`, `break_duration` with
`auto_return` when the length is known), cue in (`out_of_network=0`), cancel (`splice_event_cancel=1`), and optional `splice_null`
keep-alives. `splice_event_id` starts from the clock (unique across restarts). The injector replaces null packets, so it costs no
service bandwidth.

Automatic cues: the Supervisor reports every new on-air item of a channel (`onChannelItem`, from telemetry). When its type is a break
type (`breakTypes`, default ad / promo / psa) and the service that channel feeds is not in a break, a cue out goes with the break's
planned length (the item plus the break items right after it in the runlists, across the hour boundary; an operator take of a library
ad counts, without a length). The first non-break item after it sends a cue in (`cueIn`). Breaks shorter than `minBreakSec` get no cue.

Config `mux.config.scte35`: `{enabled, autoBreaks, cueIn, autoReturn, breakTypes, minBreakSec, offsetMs (send delay, to line the cue
up with encoder latency), basePort (5310), injectCount (2), heartbeatSec (0 = no splice_null)}`. Per service `scte35: {enabled
(default true), pid (default 0x0400 + index - 1; EIT/ETT use 0x0200-0x02FF)}`. `enabled` / per-service on-off / PID change the PMT
and the injectors, so they apply at the next mux start (`restartRequired` in the status); the cue toggles apply at once.

| method path | body -> response |
|---|---|
| GET `/api/v1/mux/scte35` | `{config, running, restartRequired, services:[{id, name, serviceId, enabled, active, pid, pidHex, port, channelId, live, inBreak, eventId, breakSec, lastAction, lastCueMs}], log:[...100]}` |
| PUT `/api/v1/mux/scte35` | config keys and/or `services:{<id>:{enabled, pid}}` -> the status |
| POST `/api/v1/mux/scte35/cue` | `{service:"svc1"|"*", action:"out"|"in"|"cancel"|"null", durationSec, autoReturn}` -> `{sent:[{service, action, eventId, delivered}], status}`; `delivered:false` = the running mux has no injector for that service |

`GET /api/v1/mux/status` carries `scte35:{enabled, autoBreaks, restartRequired}`. Web Mux page → **SCTE-35** tab: the toggles,
per-service on/off and PID, CUE OUT / CUE IN / cancel per service or all, and the cue log.

Verified 2026-10-23 with TSDuck 3.41: the PMT gets the CUEI registration and a type 0x86 component; the injected section decodes as
SpliceInsert (event id, out of network, 30 s break_duration with auto return, unique program id); on a test mux (file output, own
multicast base) an ad played on the feeding channel produced the cue out and the next item the cue in, both present in the output TS.

`MuxManager::stop()` used to crash the Supervisor (QProcess::finished, delivered inside waitForFinished, nulled `m_tsp` before
`deleteLater`); fixed the same day, so Stop and Restart mux are safe.

## UI

Web `Mux` page (`web/mux.js`): tabs Services / Encoders / Tables / Output / Streams / SCTE-35 / Guide / Log;
`viewStreams()` is exported for the Outputs tab. QML `MuxView.qml` has the same tabs (compact);
`OutputView.qml` is the per-channel streams manager (state badges, Start/Stop/Restart, autoStart/retry,
add/remove, encoder settings incl. encode size and rate control). Channels with `extra.hidden` are
mux-managed NDI feeds; both UIs hide them unless "show mux-managed channels" is ticked.
