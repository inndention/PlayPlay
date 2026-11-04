# Third-party notices

This file lists the third-party software, data and trademarks that PLAY PLAY contains, links against, loads or can
drive. It is a good-faith summary, not legal advice; the authoritative terms are those of each upstream project.

PLAY PLAY's own code has **no licence yet** (see the README). Nothing below grants any right to PLAY PLAY itself.

## Source in this repository

| Component | Where | Licence | Notes |
|---|---|---|---|
| SQLite 3.54.0 amalgamation | `tools/anfs/third_party/sqlite/` | Public domain | Used by `tools/anfs`. See the notice at the top of `sqlite3.c` and <https://sqlite.org/copyright.html>. No attribution required. |
| mime-db 1.52.0, mime-types 2.1.35, mime 1.6.0 | `tools/anfs/src/core/mime.cpp` | MIT | The extension tables in that file are generated from these npm packages. Notices below. |
| DekTec DTAPI constants | `tools/mux/dtdevice.h` | n/a | The numeric `DTAPI_IOCONFIG_*` values are mirrored from DekTec's public `DTAPI.h` for interoperability; `dtdevice.cpp` checks them against the real header when DTAPI is compiled in. The DTAPI SDK itself is not included. |
| Original Electron/React application | `reference/electron-src/` | Same as the rest of PLAY PLAY (no licence yet) | A non-buildable feature reference. It imports third-party npm packages (for example React, lucide-react, react-router-dom, clsx, tailwind-merge, fastify, rss-parser) that are **not** vendored here; each has its own (permissive) licence. |

### MIT licence text for mime-db, mime-types and mime

```
mime-db
Copyright (c) 2014 Jonathan Ong <me@jongleberry.com>
Copyright (c) 2015-2022 Douglas Christopher Wilson <doug@somethingdoug.com>

mime-types
Copyright (c) 2014 Jonathan Ong <me@jongleberry.com>
Copyright (c) 2015 Douglas Christopher Wilson <doug@somethingdoug.com>

mime
Copyright (c) 2010 Benjamin Thomas, Robert Kieffer

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

## Libraries linked at build time (not included in the repository)

| Component | Licence | How it is used |
|---|---|---|
| Qt 6.9 (Core, Gui, Widgets, Network, Sql, Qml, Quick, QuickControls2, WebSockets, Concurrent, Test) | LGPL-3.0 (and commercial) | Dynamically linked. Anyone who distributes binaries must follow the LGPL (provide the licence text, allow relinking, make the Qt source available). |
| FFmpeg 9 (libavformat, libavcodec, libavutil, libswscale, libavfilter, libswresample, libavdevice) | LGPL-2.1+ or GPL-2.0+ depending on the build | The project is developed against a **GPL** shared build that includes libx264 (GPL-2.0-or-later). Source-only publication that links dynamically is fine; **binaries built or packaged against a GPL FFmpeg must be distributed under the GPL with source.** To distribute under a permissive licence, build against an LGPL FFmpeg (without libx264) and keep libobs off. The `ffmpeg.exe` and `ffprobe.exe` from the same build are also staged next to the executables. |
| libobs (OBS Studio core) and obs-deps | GPL-2.0-or-later | Optional, `PLAYPLAY_WITH_LIBOBS=ON` (default OFF). A binary built with it is a GPL combined work. |
| NDI SDK 6 | Proprietary (Vizrt Group / NDI) | Optional (`PLAYPLAY_WITH_NDI`), installed by the user under NDI's own licence. The NDI runtime DLL is copied next to the executables by the build when found; check NDI's redistribution terms before shipping it. |
| DekTec DTAPI | Proprietary (DekTec) | Optional (`PLAYPLAY_WITH_DEKTEC`), installed by the user. Not redistributed here. |
| Blackmagic DeckLink SDK | Proprietary (Blackmagic Design) | Optional (`PLAYPLAY_WITH_DECKLINK`), installed by the user; the build generates COM headers from its IDL files. The Desktop Video driver is a run-time requirement. Not redistributed here. |
| Windows SDK, Direct3D 11 / DXGI, SAPI | Microsoft | Platform components. |

NDI(R) is a registered trademark of Vizrt Group.

## Programs and runtimes used at run time (optional, installed separately)

| Component | Licence | Used for |
|---|---|---|
| TSDuck (`tsp`, `tstabcomp`, `tsdektec`) | BSD-2-Clause | Multiplexing, table compilation and DekTec access in `docs/mux.md`. Not bundled. |
| Python 3 with numpy and Pillow | PSF; BSD; HPND | The spot scripts in `scripts/` (card rendering, music, TTS glue). |
| Kokoro-82M TTS model and the `kokoro` / `kokoro-onnx` packages | Apache-2.0 | Narration (`scripts/spot_tts.py`). Model files are downloaded by the user. |
| sherpa-onnx (`sherpa-onnx-node`) | Apache-2.0 | Narration through `scripts/kokoro_sherpa.js` (Node.js). |
| espeak-ng (its data folder is part of the Kokoro/sherpa model bundles) | GPL-3.0-or-later | Phonemization used by those bundles. Distributing the data or library with PLAY PLAY binaries has GPL-3.0 consequences. |
| faster-whisper, CTranslate2 and Whisper models | MIT | Transcription (`scripts/transcribe.py`). Models are downloaded from Hugging Face on first use. |
| VapourSynth | LGPL-2.1+ | Optional filter scripts in `filters/vapoursynth/` and `scripts/benchmark-vapoursynth.py`. |
| Chrome or Edge (headless) | Chromium and vendor licences | Rendering the HTML CG layers through the DevTools protocol. Not bundled. |
| Inno Setup | Inno Setup licence | Building the optional installer. |

## Node packages of `tools/starnews-cg/desktop`

From `tools/starnews-cg/desktop/package.json`: `yauzl` 3.2.0 (MIT) at run time; `electron` 37.10.3 (MIT, and it
embeds Chromium and other components under their own licences) and `electron-builder` 26.0.12 (MIT) as development
dependencies. If you distribute a built Electron application, include Electron's licence files and Chromium's notices.
`package-lock.json` lists the transitive packages; their `license` fields are MIT, ISC, Apache-2.0, BSD-2/3-Clause,
BlueOak-1.0.0, Python-2.0, WTFPL and CC0 variants (one entry declares none), all permissive.

## Fonts

No font files are included. The web pages `web/cg/index.html` and `web/cg/launcher.html` load **Inter** and
**JetBrains Mono** (both SIL Open Font License 1.1) from Google Fonts at run time, which sends the visitor's address to
Google. CSS also names Roboto (Apache-2.0), Arial, Segoe UI, Courier New and Georgia, which are used only if installed on
the viewing machine. The UI icons in `assets/ui/` and `web/icons.svg` are original artwork.

## Data and services

- **Open-Meteo** (`api.open-meteo.com`): weather data under CC BY 4.0, which requires attribution. The free tier is for
  non-commercial use only; commercial use needs a paid plan.
- **NWS (api.weather.gov), USGS Water Services, Virginia 511 (VDOT), Wikipedia "On this day" (CC BY-SA), TMDB, MaxPreps,
  news RSS feeds and listing websites** are read by the spot recipes. Their terms apply to the people running the
  software, who are responsible for the rights to anything they put on air. See "Data sources and terms" in
  [docs/spots.md](docs/spots.md).
- **OpenRouter** and the models behind it, when the AI features are enabled with the user's own API key.

## Trademarks

Star News, the Star News logo and PLAY PLAY are the owner's marks and artwork and are not licensed for reuse. MasterPlay
is a trademark of its owner; PLAY PLAY imports and exports files with a similar structure but is not affiliated with or
endorsed by MasterPlay or its vendor. NDI is a registered trademark of Vizrt Group. OBS and OBS Studio belong to the OBS
Project. DekTec and DTAPI belong to DekTec Digital Video B.V. Blackmagic Design and DeckLink belong to Blackmagic
Design Pty Ltd. Intel and Quick Sync Video belong to Intel Corporation. Twitter/X, YouTube and Google (including Google
Fonts) belong to their respective owners, and template names such as "Twitter/X Post" or "YouTube Subscribe" in the CG
pack are descriptive only. All other product names are the property of their owners. No affiliation or endorsement is
implied.
