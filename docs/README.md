# PLAY PLAY documentation

Feature notes written while the system was built, one file per area. They describe what the code does and, in places,
how it was verified; dates are the day a note was written, so a few details may be older than the code. Start with the
[repository README](../README.md) for building and running.

Many pages mention "PlayPlay OBS" (an earlier Python station controller whose feature set PLAY PLAY ports) and
"PlayPlayClean" (the original Electron/React operator app, kept under [reference/](../reference/README.md)).

## Overview and status

| File | What it covers |
|---|---|
| [ROADMAP.md](ROADMAP.md) | Phase plan and status, the design decisions behind the Supervisor/engine split, REST and web dashboard notes. |
| [requirements-status.md](requirements-status.md) | Audit of scheduling, playback, media, graphics and reliability against the requirements, with built/partial/missing verdicts. |
| [port-parity-checklist.md](port-parity-checklist.md) | Feature-by-feature parity checklist against the original operator app. |
| [playplay-obs-parity.md](playplay-obs-parity.md) | Parity inventory against PlayPlay OBS route groups and docks. |
| [build-consolidation-status.md](build-consolidation-status.md) | Status note from a combined build of the optional lanes. |

## Scheduling and programming

| File | What it covers |
|---|---|
| [programming.md](programming.md) | Query blocks, hour recipes, the format clock, autofill and contracts. |
| [reports.md](reports.md) | Branded HTML/CSV/JSON reports over the as-run log, library, schedule and contracts. |
| [nowplaying.md](nowplaying.md) | The web "Now Playing" page: monitors, input wall and Facebook Live controls. |
| [onair-switcher.md](onair-switcher.md) | The On Air page: monitor wall, input bus, status bar, CUT/AUTO takes. |
| [scene-recording-and-slaves.md](scene-recording-and-slaves.md) | Scene recording, the Star News CG and slave channels. |

## Graphics and overlays

| File | What it covers |
|---|---|
| [overlays.md](overlays.md) | The overlay manager: presets, rules, defaults and cues on the web CG pack. |
| [overlay-renderer.md](overlay-renderer.md) | The engine's HTML layer source (headless Chromium over the DevTools protocol). |
| [cg-captions.md](cg-captions.md) | Karaoke captions and the fact box. |
| [cg-designer.md](cg-designer.md) | The AI generator and WYSIWYG editor for CG templates. |
| [pillar-panels.md](pillar-panels.md) | Information panels beside narrow (9:16, 4:3) pictures. |
| [libobs.md](libobs.md) | Operator guide to the libobs live-input layer (OBS scenes). |
| [libobs-spike.md](libobs-spike.md) | Engineering write-up of the libobs hybrid spike: build recipe, measurements, limits. |
| [mediapanache-overlay-readiness.md](mediapanache-overlay-readiness.md) | What would have to be settled before supporting a third-party advertising overlay. |

## Spots, media and publishing

| File | What it covers |
|---|---|
| [spots.md](spots.md) | Generated spots: recipes, jobs, GEN markers, weather, area markets, music, and data-source terms. |
| [spot-studio.md](spot-studio.md) | The guided spot studio: review, revise, ad rotation. |
| [publisher.md](publisher.md) | Publishing finished spots and publications as immutable records. |
| [reporter-feed.md](reporter-feed.md) | A reporter going live from a phone app into PlayPlay. |
| [mux.md](mux.md) | The ATSC multiplexer, encoders, PSIP, SCTE-35 and streaming outputs. |

## Network, nodes and integrations

| File | What it covers |
|---|---|
| [integrations.md](integrations.md) | Live inputs (NDI scan), cameras, HTML CG, the AN-FS mesh, Star agents, mux and transcription. |
| [redundancy.md](redundancy.md) | Sessions, remote nodes and primary/backup failover. |
| [remote-pad.md](remote-pad.md) | Mappable buttons that drive this station or any remote node. |
| [render-nodes.md](render-nodes.md) | Dispatching renders to peer nodes and the Star split render. |
| [star-nodes.md](star-nodes.md) | The Star node registry, enrolment and allow-listed actions. |
| [agents.md](agents.md) | The agent suite and its task runner. |

## Reference inventories

| File | What it covers |
|---|---|
| [reference-feature-inventory.md](reference-feature-inventory.md) | Scheduling and playout-automation inventory of the original Electron app. |
| [reference-playplaypy-inventory.md](reference-playplaypy-inventory.md) | Capability inventory of the earlier Python/PySide "PlayPlayPy" app. |
| [ui-reference/](ui-reference/) | Screenshots of the original operator UI. |
