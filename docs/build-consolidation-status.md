# Build consolidation status — 2026-10-29

`build` is configured with NDI, DeckLink, DekTec, libobs, MasterPlay, Now Playing, Reporter Feed, Studio, AN-FS and legacy clients enabled. The main app, Supervisor, channel engine, Studio, AN-FS foundation and the in-house media, mux, speech, spot-render, watchdog and button-gallery tools were built. The libobs branch's implementation is present in the working tree.

This is a combined build of the code that exists, not a completed implementation of all planned features. In particular, the AN-FS peer and server modules are not present. The Studio plan still calls for components including the spend gate, executor, spot pipeline, Jobs API, film phases and Supervisor bridge; their planned source files do not exist. Cross-type automatic LAN pairing for Star agents and AN-FS also remains unfinished.

The live station and the extras bench have separate databases and separate `main` channels. Both contain schedules and spot jobs; 76 `main` slots overlap and all differ. Do not merge or overwrite them until the operator chooses how to handle those conflicts. SQLite backups were made under `%LOCALAPPDATA%/PlayPlay-Consolidation-Backups/20261029-184918`; both passed `PRAGMA quick_check`. The extras bench uses its own data directory, IPC name and ports with engine autostart disabled.

Validation: 15 targeted tests passed in `build`, covering schedule, programming, flight plan, suites, MasterPlay parsing/import and routes, Studio graph/run queue/safety, publisher rails, Reporter Feed, Now Playing, LAN authentication and WebSocket behavior. The web Now Playing render and layout tests also passed. These tests do not establish that unfinished AN-FS or Studio features exist, or validate live outputs or job execution.
