# Star News AI CG Studio — offline foundation

This is an independent loopback-only CG application, separate from the PLAY PLAY Supervisor and the existing CG control center. It uses Python's standard library and SQLite; no installation, API key, Docker, or internet connection is needed. Python 3.10+ is required. The scene renderer is shared by preview and browser output.

## Start on Windows

1. Install Python 3.10 or newer and make `py` or `python` available on PATH.
2. Run `tools\starnews-cg\Start-CG-Studio.cmd`, and leave its window open.
3. Open `http://127.0.0.1:7866/`. Choose a template, edit its fields, and preview it. Select a channel and press **TAKE / UPDATE**. **PLAY OUT** animates the exit; **CLEAR** removes it immediately. **PANIC CLEAR ALL** clears every channel after confirmation.
4. In OBS add a Browser source, paste the channel URL from the studio, and set width to **1920** and height to **1080**. The page is transparent. Each channel has its own URL. Browser-source rendering does not require the OBS WebSocket.

From a terminal on Windows, macOS, or Linux: `python tools/starnews-cg/server.py`. Options: `--port 7866` and `--data PATH`. The server always binds to `127.0.0.1`, and rejects other Host headers and cross-origin control requests. LAN access and remote viewing are intentionally unavailable. If the port is occupied, use another port and open the printed URL.

The database is `%LOCALAPPDATA%\StarNewsCG\studio.sqlite3` on Windows; elsewhere it is `~/StarNewsCG/studio.sqlite3`. Stop the server before copying this file for backup or restoring it. The current editable scene and each channel's scene and state survive refresh and server restart. On restart, an elapsed IN becomes IDLE and an elapsed OUT becomes CLEARED. Output polls every 250 ms; if disconnected, it holds the last rendered graphic. Reconnect is automatic. Browser outputs loaded during a disconnect cannot retrieve the stored state until the server returns.

Editing is separate from program: TAKE copies the current scene to the selected channel. TAKE during an animation starts a fresh IN; OUT during IN starts OUT; repeated OUT restarts OUT; CLEAR always clears immediately. These browser animations and polling are not frame accurate, and the program monitor can lag by the polling interval. Test the workflow on your hardware before using it on air. Long text is clipped with ellipsis. Weather is prominently marked SAMPLE and must be verified. The text label is a generic placeholder; no station logo is supplied.

## Feature status

| Capability | Status |
| --- | --- |
| Offline editable lower third, ticker, sample weather bug, starfield ID | Working; fixed layouts and basic IN/OUT animation |
| Separate preview and five persistent browser-output channels | Working; polling-based, localhost only |
| SQLite scene autosave and on-air scene snapshots | Working; one editable project |
| AI / OpenRouter, model catalog, costs, specialist orchestration | Planned; no generation is performed |
| Logos/assets, brand kits, layers/timeline, safe-area warnings, undo | Planned |
| Rundowns, live feeds, data bindings, OBS WebSocket, diagnostics | Planned |
| PNG/video export, portable projects, remote tokens, NDI/SDI/key-fill | Planned |

No external provider credentials are stored or exposed by this foundation. It is not connected to PLAY PLAY's CG engine, CasparCG, or hardware outputs.

## Tests

From the repository root: `python -m unittest discover -s tools/starnews-cg -p "test_*.py"`. Tests use a temporary database and an ephemeral loopback port; they do not touch production state. Browser rendering and OBS output still require a manual visual check.
