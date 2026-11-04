# HTML overlay renderer (engine `html` layer source)

The engine renders HTML/CSS/JS overlays (the web/cg template pack) with a headless Chromium-family browser
(Chrome, else Edge; found by `playout/core/browserlocator`) driven over the DevTools protocol. No Qt WebEngine.
Code: `playout/engine/compositor/browserhost.h/.cpp` (BrowserHost, HtmlPage), `livesources.cpp` (HtmlSource).

## Layer spec
`{type:"html", url|path, width, height, fps, nth, fields:{}, reloadOnMiss:true, firstFrameTimeoutMs:8000, transparent:true, label}`
`open()` blocks the loader thread until the first frame (default 8 s) and fails with a precise error: missing browser
(`no Chromium-family browser found (set settings cg.browserPath or PLAYPLAY_BROWSER); tried: ...`), navigation
failure (`net::ERR_...`), HTTP >= 400 on the document, or timeout.

`update(params)` (non-blocking): `{url}` re-take; `{fields:{name:value}}` sets textContent of `[data-field=name]`, `#name`,
`#nameText` (img src, input value) and calls `window.update(fields)` if defined; if nothing matched and `reloadOnMiss`,
re-navigates with the fields merged into the query; `{animateOut|animateIn|replay:true}` drive `window.CGAnim`;
`{eval:"js"}` runs operator JS. `status()` adds url, state, frames, fps, lastFrameAgeMs, restarts, lastError, browser path/version.

## Environment
`PLAYPLAY_BROWSER` (preferred exe), `PLAYPLAY_HTML_NTH_FRAME` (force everyNthFrame), `PLAYPLAY_HTML_FIRST_FRAME_MS`,
`PLAYPLAY_HTML_EXTRA_ARGS`, `PLAYPLAY_HTML_VERBOSE=1` (page console.log too), `PLAYPLAY_HTML_GPU=1` (drop --disable-gpu).

## Proven CDP sequence (node spikes, Chrome 155)
Browser websocket from `/json/version`, flattened sessions. Per page: `Target.createTarget{about:blank,w,h,newWindow:true}`,
`Target.attachToTarget{flatten}`, `Page.enable`, `Emulation.setDeviceMetricsOverride`,
`Emulation.setDefaultBackgroundColorOverride{0,0,0,0}`, `Page.navigate`, wait `Page.loadEventFired`, `Page.bringToFront`,
`Page.startScreencast{png}`; ack every frame.
Measured: bringToFront is required; issuing it right after navigate left 1 in 5 windows frameless, and
`stopScreencast`+`startScreencast` revived all of them (the watchdog does this after 1.5 s without a frame, up to 3 times).
With the order above, 5/5 extra windows streamed while another window streamed, and the first window kept streaming
when others were created and closed. Throughput: one animated page ~29 fps at nth=2 (~58 at nth=1); the browser delivers
~58 frames/s in total shared by animating pages (A+T at nth=1: 31 + 25). JPEG gave the same totals, so it is the
compositor clock, not PNG encoding. Hence auto nth: 2 with one live page, 1 with several. Static pages: one frame ~80 ms
after start, then nothing. Frames are RGBA PNG (corner alpha 0, box alpha 255 verified). Dialogs auto-dismissed, downloads denied,
popups closed. Browser kill closes the websocket in ~0.5 s.

## Robustness
Browser exit/crash/ws close: pages keep their last frame, browser restarts with backoff (1,2,4,8,15 s), pages re-attach and
re-navigate (fields re-applied). Renderer crash / target destroyed: that page re-attaches. Browser dies with the engine
(Windows kill-on-close job). Profile `%TEMP%/playplay-html/<pid>-<n>`, stale ones removed at next launch. Layers nobody polls
for 2 s (hidden) pause their screencast and resume on the next frame request.

## Not verified
The C++ was compile-checked only (no build allowed): end-to-end behaviour of BrowserHost inside the engine, restart/backoff and
the test are unexecuted. `tests/test_browserhost.cpp` skips when no browser is installed.
