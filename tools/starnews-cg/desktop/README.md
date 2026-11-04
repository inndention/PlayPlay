# Star News CG Studio desktop

Electron wrapper for the existing Star News Motion Studio with candidate and results graphics, a preview/program split, a local browser-source relay, and a direct PLAY PLAY Supervisor CG command. It uses the existing logo file from `Star-News-Graphics`.

## Run

Install Node.js 22 and npm, then run `Start-Star-News-CG.cmd`. First launch installs dependencies. Or run `npm install` and `npm start` in this directory. To build an unpacked Windows application, run `npm run dist`; the resulting `dist/win-unpacked` folder must be kept together. No Python is required for this desktop version. Port 8765 on loopback must be free; do not run the original Python relay on the same port.

Preview edits are local to the desktop preview. **TAKE / UPDATE** sends the current snapshot to program; **PLAY OUT** cues the current output to exit. The browser source is `http://127.0.0.1:8765/output.html?live=1`, 1920 × 1080, and the desktop app must remain open. Program starts cleared each time the app starts. The relay holds the last cue in memory while the app runs; it is not frame accurate. Do not rely on it for a fail-safe on-air system without rehearsal. The studio's earlier Animate in/out controls affect preview only.

## PLAY PLAY

Start the PLAY PLAY Supervisor and channel engine. In the studio, enter the Supervisor REST port (default 7700), the actual channel ID, a dedicated CG layer number, and the Supervisor token if configured. Press **Attach output** to send the HTML source to that channel, then **TAKE / UPDATE** to cue content. **Hide layer** removes the selected layer. The Supervisor's CG endpoint acknowledges command submission, not successful on-air rendering: verify the layer and picture in PLAY PLAY. This requires an engine build with the HTML browser source enabled. Layer numbers are shared with other graphics: choose an unused layer. These controls affect a live channel; use a test channel first. The token is not saved.

OBS/vMix can use the same URL as a browser source, independent of PLAY PLAY. This tool does not control OBS.

## Election graphics

Candidate Slate and Election Results are editable templates. Replace the sample label and values only after checking the results. For North Carolina, use **Fetch NC results to preview** with an election date from July 2018 onward and the exact county and contest name shown by NCSBE. It fetches the state's published precinct ZIP, sums all matching precinct and administrative records for that county and contest, and marks the result unofficial for review. It does not calculate percentages; it refuses contests with more than six choices or invalid data. This is a snapshot, not a live subscription. Fetch again to refresh; changes are never sent on air without TAKE. Always compare against the official dashboard, including election date, county, contest, vote-for rules, reporting and certification. A precinct file is not itself proof of certification. The Virginia button opens the official state results site; automatic Virginia import is not implemented.

Official references: https://www.ncsbe.gov/results-data/election-results/about-election-results-dashboard and https://www.elections.virginia.gov/results/ .

## Tests and limitations

`npm test` exercises the results aggregation and cue snapshot/relay behavior. `node --check` can check the JavaScript files. The project does not include AI generation, a drag-and-drop editor, candidate photographs, multiple simultaneous program channels, automatic feed refresh, or Virginia data import. The existing scene styling and fixed 1080p design are inherited from the Motion Studio. PLAY PLAY output and packaged Electron should be checked on the intended production system before air.
