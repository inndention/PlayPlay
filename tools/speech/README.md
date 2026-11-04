# speech: PlayPlaySpeech.exe

The in-house replacement for the spot generator's speech helpers (docs/plans/inhouse-deps.md §6.4). `tts` takes the
arguments of `scripts/spot_tts.py` and prints the same one-line JSON result, so the Supervisor only changes which
program it starts. DEP-13 adds the `spots.engines.tts` switch; until then nothing calls this tool.

| sub-command | does | slice |
|---|---|---|
| `tts` | narration WAV(s): the `spot_tts.py` contract, SAPI 5 through COM | DEP-10 (Kokoro: DEP-11) |
| `voices` | installed SAPI voices, desktop and OneCore, plus the default voice | DEP-10 |
| `env` | which engines can run here and what `--engine auto` resolves to | DEP-10 |
| `version` | `{ok, tool, commands, qt}`: which sub-commands this build has | DEP-10 |

Contract (tools/ppcommon/README.md): the result is the last stdout line and diagnostics go to stderr. Exit codes are 0
ok, 2 failed and 64 unsupported (`{"ok": false, "error": "unsupported: --x", "unsupported": true}`), on which the caller
runs the script instead. The tool spawns nothing: no PowerShell, no .NET, no Python, no Node.

Build: `cmake/lane-inhouse-speech.cmake` (Windows only). Every `tools/speech/*.cpp` except `speech_main.cpp` goes into
the static library `PlayPlaySpeechKit` (globbed); the exe and the tests link it. All targets are EXCLUDE_FROM_ALL:
build `PlayPlaySpeech` / `test_inhouse_speech` by name. Runtime needs only `Qt6Core.dll` and the inbox SAPI.

## tts

```
PlayPlaySpeech tts --text-file in.txt --out voice.wav [--voice af_heart] [--speed 1.0] [--engine auto|kokoro|sapi]
                   [--kokoro-dir D] [--kokoro-model D] [--sherpa-node D] [--max-chars 12000]
-> {"ok": true, "engine": "sapi", "voiceUsed": "Microsoft Zira Desktop", "durationSec": 7.574, "path": "C:\\...\\voice.wav",
    "sampleRate": 48000, "words": 15, "tried": ["sapi"]}
PlayPlaySpeech tts --batch voices.json [same options]      voices.json = {"items": [{"text": "...", "out": "..."}, ...]}
-> {"ok": true, "engine": "sapi", "voiceUsed": "...", "items": [{"path", "durationSec", "words"}], "tried": ["sapi"]}
```

Everything the script does with the text and the files is kept:
- Whitespace runs collapse with Python's `\s`. Single mode cuts at `--max-chars` code points, after the last `". "` when
  one is in the second half; batch items are cut hard. Under 2 characters is an error.
- `path` is absolute with backslashes. A stale output is deleted before synthesis.
- Batch WAVs are trimmed (-42 dBFS, 80 ms margins). Both modes are levelled with `normalize_wav`: voiced RMS to -19 dB,
  then the -1.5 dBFS limiter, skipped inside its 0.5 dB dead band. This is the `wavio` port in tools/ppcommon.
- Each item must last at least 0.3 s. Error strings and key sets are the script's.

Values are parsed like argparse would: Python `float()` / `int()` syntax, `--engine` choices, speed clamped to 0.5..2.0
(`0` means 1.0, NaN means 2.0). The SAPI rate is `round((speed - 1) * 10)` with ties to even. `--serve` and other
unknown options exit 64.

**SAPI (`sapitts.*`).** One `ISpVoice` per run, reused for every batch item, as the script reused one
`SpeechSynthesizer`.
- Each item is spoken into an `ISpStream` bound to a 48 kHz 16-bit mono WAV, as `SPBindToFile` does (sphelper.h needs
  ATL, so it is not used). The file is written under a temporary name next to the target and renamed over it once SAPI
  has finished, so a killed run never leaves a half-written WAV under the expected name.
- Text goes in with `SPF_IS_NOT_XML`, so narration taken from the web cannot switch voices or volume through SAPI XML.
- Speech is asynchronous with a deadline of 120 s + 0.2 s per character (`$PLAYPLAY_SPEECH_TIMEOUT_MS` replaces it);
  SAPI renders far faster than real time (66 s of speech in under a second here). Past the deadline the engine thread
  may be wedged, and purging, closing the stream, releasing the voice or `CoUninitialize` could then wait on it
  forever. So nothing more is asked of SAPI: the failure line (`no TTS engine produced audio` with `tried`) is printed
  and the process ends with `TerminateProcess` and exit 2. `TerminateProcess` is used rather than `ExitProcess`, which
  first takes the loader lock that an engine stuck in its `DllMain` would hold. The temporary
  `<name>.sapi-<pid>-<n>.part.wav` can stay behind, since the stream still holds it; the target name is never written.
- A watchdog thread covers the rest of the SAPI run (creating the voice, loading its engine, closing and releasing):
  60 s plus each item's deadline plus 15 s, or `$PLAYPLAY_SPEECH_WATCHDOG_MS`. It prints the same line and ends the
  process the same way. Firing and the normal end take one mutex, so exactly one result line is printed.
- A crash in an engine DLL ends the process at once (`SetErrorMode` without the GP-fault box, no abort dialog), so the
  caller does not wait for its own timeout behind Windows Error Reporting.
- COM runs as MTA. No console, window or audio device is needed: the test runs it `DETACHED_PROCESS` inside a job
  object.

**Voices (`voicemap.*`).** `sapi_voice_for` is ported as it is, quirks included:
- An exact installed name passes through, in any case.
- `af_*`/`bf_*`/`f*` map to a female voice: Hazel when British, then Zira, then Eva/Aria/Jenny.
- `am_*`/`bm_*`/`m*` map to a male voice: George when British, then David, then Mark/Guy. A missing name such as
  "Microsoft Hazel Desktop" starts with "m" and so gets David, as in Python.

The installed list is the desktop tokens (`SPCAT_VOICES`) followed by the OneCore tokens
(`HKLM\SOFTWARE\Microsoft\Speech_OneCore\Voices`). Kokoro ids therefore pick the voice the script picked
(`af_heart` → "Microsoft Zira Desktop", `am_michael` / `bm_*` → "Microsoft David Desktop"), while the OneCore voices
System.Speech never listed become selectable by name ("Microsoft Mark", "Microsoft Zira", "Microsoft David").
`voicemap` also holds the kokoro-en-v0_19 speaker table and the nearest-voice rule of `kokoro_sherpa.js`, for DEP-11.

**Engine order.** `--engine sapi` uses SAPI. Until DEP-11 a run that would start with Kokoro exits 64 *before touching
any file*, so the Supervisor runs `spot_tts.py` and gets the Kokoro voice it gets today. That covers `--engine kokoro`,
and `--engine auto` while any Kokoro the script tries before SAPI is here:
- the sherpa-onnx model and C API DLL (what DEP-11 will run):
  - model: `--kokoro-model`, `--kokoro-dir`, `$STARMEDIA_KOKORO_DIR` or `D:\StarMedia\models\kokoro-int8-en-v0_19`,
    as `find_kokoro_sherpa` looks;
  - DLL: next to the exe, `$PLAYPLAY_SHERPA_ROOT`, or the `sherpa-onnx-win-x64` folder beside `--sherpa-node` /
    `$SHERPA_ONNX_NODE_DIR` / the StarMedia default;
- `kokoro_onnx`: `kokoro-v1.0.onnx` + `voices-v1.0.bin` in `--kokoro-dir`, `dir/kokoro_models`,
  `dir/../kokoro_models` or `dir/models` (`find_kokoro_models`; `..` is pathlib's lexical parent, `ttscli::pyParent`);
- `kokoro_venv`, the PlayPlay OBS install of docs/spots.md: a `python.exe` (`dir/ttsvenv/Scripts`, `dir/Scripts`,
  `dir/../tools/ttsvenv/Scripts`) and `kokoro_synth.py` (`dir`, `dir/tools`, `dir/../tools`), as `try_kokoro_venv`
  looks;
- `kokoro_sherpa`: the sherpa model plus a sherpa-onnx-node `package.json`. node.exe and the helper script are not
  checked, so this errs towards running the script.

The refusal names them: `reason` lists the script's engines, and `kokoro` (also in `env`) carries `scriptEngines`,
`onnxModelDir`, `venvPython`, `venvSynth` and `sherpaNodeDir`. The `kokoro` pip package in the caller's Python cannot be
seen from here; DEP-13 has to account for it. `auto` with none of these voices with SAPI and reports
`tried: ["kokoro", "sapi"]`. `PLAYPLAY_SPEECH_KOKORO=0` treats every Kokoro as absent, the script's included.

**Compared with the script on this host** (Zira Desktop, David Desktop, OneCore Mark/Zira/David):
- Same keys, voice, words and sample rate; durations within 1 ms in every case measured (identical in most). The level
  is the same.
- Desktop Zira's waveform is the same: correlation 0.9999. David Desktop runs on the OneCore engine, whose output
  System.Speech resamples differently: same envelope (0.98-0.9999), waveform correlation 0.99 on long text but low on
  some short clips. Raw SAPI's resampler leaves less energy above 11 kHz (1e-8 against 1e-6).
- A 7-item batch (66 s of speech) takes 0.8-1.0 s, against 6.7 s for the script, most of which is PowerShell start-up.

## Tests

`test_inhouse_speech` (`tests/inhouse/test_inhouse_speech.cpp`) covers:
- the voice map on fixed lists and against the script's own `sapi_voice_for` (6 lists × 28 voices);
- the Kokoro speaker table, speed/rate values printed by CPython, and the text rules against the script's lines;
- `voices`;
- `tts` single and batch through the real exe (WAV format, level, trim, keys, voices, OneCore Mark, speed, non-ASCII
  paths);
- the engine plan and the error contract, including the script-only Kokoro layouts, compared with the script's own
  `find_kokoro_models` / `try_kokoro_venv` and pathlib's parent;
- the deadline and watchdog exits (exit 2, one line, no target file, within about 120 ms);
- a detached, PowerShell-free run whose job object must count exactly one process, and a source scan for spawn calls;
- with Python + numpy + PowerShell, parity with `spot_tts.py --engine sapi`.

Run it with the Qt DLL folder on PATH.
