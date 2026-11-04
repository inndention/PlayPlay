# spotrender: PlayPlaySpotRender.exe

The in-house replacement for the spot generator's Python helpers (docs/plans/inhouse-deps.md §6.3). Each sub-command
takes the arguments of the script it replaces and prints the same one-line JSON result, so the Supervisor only changes
which program it starts (DEP-13 adds the `spots.engines` switch; until then nothing calls this tool).

| sub-command | replaces | slice |
|---|---|---|
| `music` | `python -I scripts/spot_music.py` | DEP-07 |
| `version` | (new) `{ok, tool, commands, qt}`: which sub-commands this build has | DEP-07 |
| `card`, `scenes` | `scripts/spot_card.py` (`--spec/--out/--bed`, `--scenes`) | DEP-08b |
| animated scenes (`anim`) | `scripts/weather_cards.py` | DEP-09 (not yet: exit 64) |

Contract (tools/ppcommon/README.md): the result is the last stdout line; exit 0 ok, 2 failed, 64 for an unknown
sub-command or option (`{"ok": false, "error": "unsupported: --x", "unsupported": true}`: the caller runs the script
instead). `music` runs under a `QCoreApplication` (UTF-16 command line, no platform plugin needed); `card` / `scenes`
run under a `QGuiApplication` on the native `windows` platform (fonts; no window is ever created), so they need
`platforms/qwindows.dll` and `imageformats/qjpeg.dll` staged with the Qt DLLs (DEP-22). A crash ends the process
(`SetErrorMode`), it never waits behind a WER dialog.

Build: `cmake/lane-inhouse-spotrender.cmake`. Every `tools/spotrender/*.cpp` except `spotrender_main.cpp` goes into the
static library `PlayPlaySpotRenderKit` (globbed, so new files need no CMake edit); the exe and the tests link it. All
targets are EXCLUDE_FROM_ALL: build `PlayPlaySpotRender` / `test_inhouse_music` by name.

## music

```
PlayPlaySpotRender music --out bed.wav [--mid bed.mid] --seconds 45 [--mood news] [--energy 7] [--seed 12345]
-> {"ok": true, "wav": "bed.wav", "mid": "bed.mid", "bpm": 114, "key": "F dorian", "mood": "news", "seconds": 45,
    "tracks": ["pad", "bass", "arp", "drums"], "notes": 968}
```

Same options, defaults (`--mood warm --energy 5 --seed 1`, `--mid ""` = no MIDI file), value syntax (Python `int()` /
`float()`: whitespace, `+`/`-`, `1_000`, `1e1`, `nan` / `inf`, ints of any width) and clamps (seconds 3..900 with NaN →
900, energy 1..10) as the script. The JSON has the script's keys; numbers are printed compactly (`45` where Python
prints `45.0`), which every caller reads the same.

- **Composer and MIDI: bit-exact.** `pyrandom.{h,cpp}` replicates CPython 3.11 `random.Random` (MT19937
  `init_by_array` over the 32-bit words of `abs(seed)`, `random()`, `getrandbits`, `_randbelow` with its retry,
  `uniform`, `choice`) and `round()` (ties to even; `round(x, 3)` correctly rounded via `std::to_chars`). The composer
  in `musicsynth.cpp` keeps the script's operand order, so tempo, key, progression, melody and velocities are the same
  and the `.mid` file is byte-identical for every seed (119 goldens + 400 random argument sets in the test).
- **Synth: the same sound, not the same samples.** Same instruments, envelopes, mix, echo, Haas pad and fades. The
  oscillators run in double precision (numpy used float32 phases) and the drum noise comes from a fixed mt19937_64 +
  Box-Muller sequence (numpy used PCG64), so samples differ: waveform correlation with the script's WAV is 0.9995+ for
  drum-less beds and about 0.955 with drums (the noise), and the 50 ms loudness contour correlates 0.998+.
- **Level: one intended difference.** The script scales to -12 dBFS RMS and then applies `tanh(0.9 x) / 0.9 * 0.95`,
  whose ceiling (1.056) is above full scale, so loud moods hard-clip at 1.0 and the bed ends at -13.0..-13.4 dBFS
  (measured on all 108 mood/seed/energy goldens; 13 of them peak at 0.999-1.0). Here the limiter is
  `0.95 * tanh(x / 0.95)` and the gain is iterated for what the limiter and the fades take off: the WAV is -12.0 dBFS
  RMS, never over 0.95, never clipped. The spot mixer ducks the bed under the voice either way.
- Exactly `ceil(seconds * 48000)` frames, 48 kHz stereo 16-bit; the MIDI file is written first, then the WAV, both
  atomically (`QSaveFile`): a failed run never leaves half a file for the compose step.
- Speed (this host): 30 s bed 0.34 s, 120 s 0.79 s (script 1.24 s incl. interpreter start), 900 s 7.9 s at ~514 MB
  peak (the script needs about 2 GB there).

## Card kit (DEP-08a): `cardkit`, `blur`, `imageio`

The primitives `card` / `scenes` (DEP-08b) are built from; each function names the `spot_card.py` helper it ports.

- **`cardkit`**:
  - Exact: `clean()`, `hexRgb()` (Python `int(s, 16)` rules, including that `int()` skips only ASCII and non-ASCII
    Unicode spaces, not `\x1c..\x1f`), and the `wrap()` / `ellipsize()` algorithms (given the same widths).
  - The `scrim` / `bottomVignette` / `wash` ramps, `chip`, `spaced`, and `setRect` (ImageDraw on an RGBA image
    *replaces* pixels; `fillRect` composites as a layer). `ring` gives outline widths.
  - `gradientBed()` is numpy's float32 `render_bed`, byte-identical.
  - Fonts load like `ImageFont.truetype`: the path, else the bare name in `%WINDIR%\Fonts`, else Segoe UI by family.
    They are drawn by Qt's DirectWrite engine, unhinted with grey antialiasing.
  - `Face::width()` is Pillow's textbbox width. `ascent()` / `descent()` are FreeType's hhea-based size metrics,
    rounded up in 26.6 (DirectWrite's OS/2 win metrics differ, e.g. Calibri by 7 px at 32 px).
- **`blur`**: Pillow's `GaussianBlur` / `BoxBlur` (libImaging/BoxBlur.c), bit for bit. `shadowText()` blurs only a
  crop with `spread() + 1` margin, which equals the full-frame result.
- **`imageio`**: `load()` = `exif_transpose(Image.open(p))`:
  - QImageReader with EXIF auto-transform. For handlers that do not turn the picture (PNG eXIf, WebP EXIF), the
    file's own EXIF block is applied.
  - libav when Qt cannot read the file (WebP with only qjpeg staged, GIF, ...). Orientation comes from the display
    matrix by ffmpeg's autorotate rule, else from the file's EXIF.
  - Local files only, with a deadline and a cancel flag. `maxPixels` (Pillow's bomb threshold) is checked before
    allocating; on the libav path through the decoders' `max_pixels`.
  - **Truncated JPEG/PNG/WebP files are refused**, as Pillow refuses them, so the scene gets the gradient bed instead
    of a half-grey photo.
  - `resizeHQ` (Qt smooth for 1-2x down, swscale Lanczos otherwise), `cover` = `_cover`, `brightness` =
    `ImageEnhance.Brightness`.
  - `save` writes PNG at zlib level 3 or JPEG q90, atomically.

## card / scenes (DEP-08b): `spotcard`

```
PlayPlaySpotRender card --spec spec.json --out furniture.png [--bed bed.png]
-> {"ok": true, "furniture": "C:\...\furniture.png", "bed": "C:\...\bed.png", "headlineLines": [...], "lines": 3}
PlayPlaySpotRender card --scenes scenes.json        (or: PlayPlaySpotRender scenes scenes.json)
-> {"ok": true, "scenes": [{"kind", "furniture", "bed", "photo", "layout", "photoSize"}, ...]}
```

`card` takes the script's argparse exactly (`--opt value`, `--opt=value`, unique prefixes, the last one wins; `--scenes`
wins over `--spec`), so DEP-13 swaps `python -I spot_card.py` for `PlayPlaySpotRender.exe card` and keeps the arguments.
The specs are the script's (`spot_card.py` module doc), read as utf-8-sig. Results carry exactly the script's keys and
values; paths are absolute and native as `Path.resolve()` prints them. Errors are the script's: `--spec and --out are
required (or --scenes)`, `bad spec: ...`, a missing scene key as `'bedBase'`, `invalid literal for int() ...`.

- **Layouts** (`spotcard.cpp`): `render_furniture`, `render_story` (boxed picture, category / counter chips, place,
  headline 3-4 lines that shrinks once, market rows with sparklines, detail line, sparkline panel, summary),
  `render_intro` (rundown of 5), `render_outro`, `draw_strap`. Same arithmetic in the same order (`int()` truncation,
  `//` floor, Python truthiness for `x or default`), so every text lands where the script puts it: the test finds all
  172 fixture texts within 1 px, same strings, same sizes. Pixels differ only in glyph rasterisation (DirectWrite vs
  FreeType) and Pillow's aliased sparkline polygons: 30-38 dB composited over grey.
- **Photo beds** (2880x1620 JPEG q90): `cover` (landscape, ratio >= 1.5), `inset` (square / portrait on the right over
  a blurred copy, with a blurred shadow), `backdrop` (intro / outro, dimmed 0.42) and `box` (boxed story layout, dimmed
  0.55). Pillow's own resampling (Resample.c: per-axis double coefficients, 22-bit fixed point, horizontal then vertical
  pass) is ported for these, so the beds decode **pixel-identical** to the script's on every fixture (landscape,
  portrait, square PNG, EXIF-6 JPEG, WebP). A picture under 80 px a side, unreadable or truncated gives the W x H
  gradient PNG bed and `photo: false`, as in the script; an unreadable boxed picture falls back to the plain photo
  layout as `_story_box` returning None does.
- **Animated scenes** (`anim`, weather_cards.py) are DEP-09. Until then any `anim` scene makes the whole run exit 64
  with `{"unsupported": true, "scene": i}` before a file is written, so the Supervisor runs the script for that spot.
- **Speed**: scenes render on up to 4 threads (`PLAYPLAY_SPOTRENDER_THREADS` overrides; 1 = serial); the gradient bed is
  painted once per run. 7 scenes (4 photo beds) 1.4 s and 11 scenes 1.6 s, against 6.4 s and 10.3 s for the script
  (3.4 s / 4.8 s on one thread). A failed scene stops new ones from starting; the first failure in scene order is
  reported (the script stops at the first failing scene).

## Tests

`test_inhouse_music` (`tests/inhouse/test_inhouse_music.cpp`, fixtures `tests/inhouse/data/music/`): CPython reference
values for pyrandom/round, the MIDI goldens (remade with `python -I tests/inhouse/data/music/make_goldens.py`), the CLI
contract and WAV properties through the real exe, and, when Python + numpy are on the machine (`PLAYPLAY_TEST_PYTHON`
overrides the search), 400 random argument sets through the script's own composer and a WAV comparison per mood class.
Run it with the Qt DLL folder on PATH (the test starts `PlayPlaySpotRender.exe`).

`test_inhouse_card` (`tests/inhouse/test_inhouse_card.cpp`) covers the card kit and, with the fixtures in
`tests/inhouse/data/cards/` (specs plus synthetic pictures made by `make_fixtures.py`), the `card` / `scenes` layouts. It runs under a `QGuiApplication` on
the native platform. With Python + Pillow + numpy present it compares against `spot_card.py`'s own functions and
Pillow (the `*VsScript` / `*VsPillow` tests); without them those tests skip. It re-runs itself as `--load-child` with a
plugin path holding only `qjpeg.dll` to prove the WebP fallback.
