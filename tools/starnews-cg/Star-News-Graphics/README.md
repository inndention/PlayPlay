# Star News Motion Studio

A complete, editable HTML5 broadcast graphics package using your supplied transparent Star News PNG. No JavaScript frameworks, npm, external fonts, CDN dependencies, or weather API keys are required. The original logo bytes are unchanged.

## Included graphics

| Scene key | Graphic | Background | Motion |
| --- | --- | --- | --- |
| `id` | Station identity | Deep navy starscape | Staggered reveal, floating logo, layered star drift, gentle twinkle, occasional shooting star, dissolve out |
| `lower` | Editorial lower third | Transparent | Masked slide in, subtle light sweep, slide out |
| `breaking` | Breaking news banner | Transparent | Rise in, moving accent lines, fall out |
| `weather` | Rotating city weather bug | Transparent | Perspective entrance, city flips, animated icons, exit |
| `upnext` | Coming-up slate | Deep navy starscape | Rise in, floating logo, star drift, exit |
| `ticker` | Headline crawl | Transparent | Slide in, seamless crawl, live Eastern clock, slide out |

The optional weather bug can appear on the other five scenes. Overlay placements move to avoid lower-third and ticker collisions. All graphics use a 1920 × 1080 design surface and scale uniformly to fit the browser viewport. Preview checkerboard and safe guides are studio-only and never enter the clean output.

## Windows / OBS / vMix

1. Extract the entire ZIP into a folder. Keep `assets`, CSS, and JS alongside the HTML.
2. With Python 3 installed, double-click `Start-Studio.bat`. There are no pip dependencies. Alternatively run `python server.py --open`.
3. Keep the console window open. The local studio opens at `http://127.0.0.1:8765/`.
4. Add a browser source in OBS/vMix: `http://127.0.0.1:8765/output.html?live=1`. Set width **1920**, height **1080**, and preferably **60 FPS**. Keep the page background transparent. In OBS, use the browser source's Interact window if you want keyboard control directly inside that source.
5. Select the template, edit text, and click **Animate in**, **Animate out**, or **Replay**. The local server relays cues to browser sources every 250 ms. This is a graphics cue relay, not a frame-accurate broadcast automation protocol.
6. For continuous weather/ticker playback, avoid source settings that shut down or reload the browser when hidden. Enabling those settings intentionally restarts a scene when reactivated.

One local studio controls one program output. Multiple live output windows mirror it. Fixed scenes without `live=1` remain independent. Start a second server on another port if you need a separate control channel, and use a different port's studio for each.

The server is bound to **127.0.0.1** and is intended for studio and switcher on the same PC. To stop it, press Ctrl+C in its console. If the port is occupied, run `python server.py --port 8766 --open`, then use that port in the browser-source URL. Python relay state is in memory; browser content edits persist in localStorage. One active control tab per port is recommended.

## XAMPP or another static web server

Copy the extracted folder under `htdocs`, then visit its `index.html`. Static hosting supports preview, editing, URL snapshots, automatic entrances, and the JavaScript API. Same-origin browser tabs synchronize through BroadcastChannel. **Static hosting alone does not relay commands into the separate OBS/vMix browser process.** Use the included Python server for that workflow.

The hosted private studio is for preview and design. Use the local package for broadcast so output does not depend on a private website sign-in.

## Direct file preview

Open `index.html` to preview offline. The embedded preview works without a server. Open `output.html?scene=weather` through a web server for a fixed standalone graphic; file URL query support varies by launcher. For production, use HTTP via the included launcher.

## Output URLs

`output.html?scene=id` — automatically play a fixed station ID.

`output.html?scene=lower&autoplay=0` — start a lower third off air.

`output.html?live=1` — follow the local studio through the relay, or same-browser tabs through BroadcastChannel.

The studio's **Copy browser-source URL** button includes a JSON `data` query parameter containing your edited text and settings plus `live=1`. Remove `live=1` to freeze that URL's configuration and keep it independent of future studio cues. URLs contain the entered content; do not put private information in a link you share.

## Weather

Weather values ship as **SAMPLE DATA**, not live observations or forecasts. Enter one city per line:

```
Martinsville | 68 | Partly cloudy | 72 | 51
Danville | 71 | Sunny | 75 | 53
```

Format: `City | Temperature °F | Condition | High °F | Low °F`.

Supported icon keywords include sunny, clear, partly, mostly, cloudy, overcast, fog, rain, shower, storm, snow, and night. Up to 12 cities rotate every 3–30 seconds. Temperatures are Fahrenheit; there is no automatic conversion. Replace the label with a verified source/time when you have updated the values. To connect your own feed later, call `StarNews.update({weatherRows: ...})` with normalized text or send the complete config through `/api/state`.

## Content and keyboard controls

- Enter: animate in. Escape: animate out. R: replay.
- In the studio, 1–6 select scenes; typing in content fields does not trigger shortcuts.
- Play showcase cycles all six scenes every 12 seconds. Stop it before a live show.
- Adjust motion speed from 0.5× to 1.5×. It affects entrance/exit timing, star drift, and ticker speed. Atmospheric CSS loops retain their gentle fixed durations. City rotation uses its own interval.
- The clock displays America/New_York time, including daylight saving changes.
- The default station ID is a branding slate. Replace the editable identification line with the station-specific text you need on air.
- Long lower-third headlines shrink to fit. Use concise text for broadcast legibility.

## JavaScript control API

Available in `output.html`:

```js
StarNews.play();
StarNews.stop();
StarNews.replay();
StarNews.select('lower');
StarNews.update({
  headline: 'Local coverage, every day',
  subtitle: 'Star News • More Local News',
  kicker: 'COMMUNITY',
  location: 'MARTINSVILLE, VA',
  speed: 1,
  bug: true
});
StarNews.getState();
```

If embedded, the parent can send a message:

```js
iframe.contentWindow.postMessage({
  type: 'star-news-control',
  seq: Date.now(), // increase on every cue
  config: {scene: 'lower', headline: 'Your headline'},
  action: 'in', // 'out', 'replay', or null for a content update
  on: true
}, location.origin);
```

The local relay accepts the same JSON via `POST /api/state`, with `Content-Type: application/json`. Each message needs a strictly increasing numeric `seq` and a `config` object. Provide the complete current configuration when automating the relay, so newly opened output sources can reconstruct state. The engine ignores stale/duplicate sequence numbers.

## Files

- `index.html`, `studio.css`, `studio.js`: control studio.
- `output.html`, `graphics.css`, `graphics.js`: broadcast rendering engine.
- `assets/star-news.png`: your original transparent image.
- `server.py`, `Start-Studio.bat`: optional local control relay and launcher.
- `api/health`: static hosting marker.

No audio is included. The page renders animated graphics; it does not export a video or connect to your broadcast switcher automatically. Test a rehearsal in your actual switcher before use on air.
