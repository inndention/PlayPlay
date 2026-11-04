# ppcommon: shared code of the in-house tools

`PlayPlayInhouseCommon` (static, QtCore only; `cmake/lane-inhouse-common.cmake`) is linked by PlayPlaySpeech,
PlayPlaySpotRender and PlayPlayMediaTool (docs/plans/inhouse-deps.md §5). It holds the command-line contract those tools
share with the Python / Node scripts and FFmpeg tools they replace, so a Supervisor call site only changes which program
it starts.

## ppcli.h

| piece | what it does |
|---|---|
| `initStdio()` | binary stdout (lines end in `\n`) and Qt logging forced to stderr; call first in `main()` |
| `finish(r)` / `fail(error)` / `unsupported(what)` | print the result as the **last** stdout line and return the exit code: 0 ok, 2 failed, 64 unsupported (`{"ok": false, "error": "unsupported: <what>", "unsupported": true}`) |
| `log(msg)`, `progress(done, total)` | stderr lines; `PROGRESS <done> <total>` matches the Supervisor's regex in `transcriptionmanager.cpp` |
| `parseResult(stdout)`, `parseProgress(line)`, `isUnsupported(code, r)` | the caller's side, for the Supervisor client classes |
| `Args(args, spec)` | argparse-style options: `"--out="` takes a value (`--out x` or `--out=x`), `"--serve"` is a flag; an option outside the spec is an unsupported argument (exit 64 via `report()`), a missing value exit 2 |
| `ServeLoop` | the `--serve` mode (below) |

### `--serve` protocol

One JSON object per line on stdin, one reply per request on stdout:

```text
-> {"id": 7, "op": "probe", "path": "C:/clips/a.mp4", "timeoutMs": 20000}
<- {"id": 7, "event": "progress", "done": 1, "total": 3}        (optional, from ctx.progress / ctx.event)
<- {"id": 7, "ok": true, ...}                                   (or "ok": false + "error")
```

- `workers` threads run handlers; replies come in completion order, so match them by `id` (any JSON value; `null`
  for a line that is not a JSON object).
- Built in: `ping` (`{ok, pong, pid, busy}`), `quit` / `shutdown` (`{ok, bye}`, then exit 0 once in-flight work is done).
  Unknown ops: return `ServeLoop::unsupportedOp(op)`.
- Timeout per request (`timeoutMs`, else `defaultTimeoutMs`), counted from when a worker starts it. At the deadline the
  reply `{"ok": false, "error": "timeout after N ms", "timeout": true}` goes out, `ctx.cancelled()` turns true and the
  late result is dropped. A handler still running `hangGraceMs` later means the process is wedged: queued requests get
  `{"ok": false, "restart": true}` and the tool exits 2 at once (`hardExitOnHang`), so the client restarts it.
- stdin EOF: finish queued and running work, exit 0. `idleExitMs`: exit 0 after that long without work. Broken stdout:
  exit 2.
- Handlers never write to stdout themselves. stdin is read with `ReadFile` (not the CRT), so a reader waiting for the
  next request never blocks process exit.

## wavio.h

PCM WAV read / write (reads 8/16/24/32-bit PCM and 32/64-bit float, incl. `WAVE_FORMAT_EXTENSIBLE`, odd chunks, cut-off
and streamed data sizes; writes 16-bit PCM atomically) and the speech levelling of `scripts/spot_tts.py`:

| function | Python | rule |
|---|---|---|
| `trim` / `trimFile` | `trim_wav` | drop leading / trailing audio under -42 dBFS, keep 80 ms margins |
| `normalize` / `normalizeFile` | `normalize_wav` | RMS of the voiced 50 ms blocks (above -50 dBFS) to -19 dBFS, gain capped at +24 dB, nothing done under 0.5 dB |
| `limitPeaks` | `limit_peaks` | 2 ms blocks, attack one block early, ~60 ms release, interpolated; peaks never above the ceiling (-1.5 dBFS) |

`test_inhouse_common` runs the same WAVs through `spot_tts.py` when Python + numpy are on the machine: trim is identical,
the gain agrees to 0.01 dB and every sample to one LSB when the limiter is idle. The one deliberate difference is a cap
in `limitPeaks`: the Python version can let a peak through up to ~0.35 dB over the ceiling when it sits at the end of
its 2 ms block (the gain is interpolated towards the next block's release); the C++ gain never exceeds the block's own
need.

## Adding a tool or a test

A sibling fragment starts with `include("${CMAKE_CURRENT_LIST_DIR}/lane-inhouse-common.cmake")` (guarded, so including it
twice is fine), links `PlayPlayInhouseCommon`, keeps its executable `EXCLUDE_FROM_ALL`, and registers its tests with
`playplay_add_inhouse_test(<name> SOURCES tests/inhouse/<file>.cpp LIBS <libs>)` (Qt Test, `PP_SOURCE_DIR` defined).
