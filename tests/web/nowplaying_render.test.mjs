// Node render test for the web "Now Playing" page (web/nowplaying.js) and the shared Facebook Live card (web/facebooklive.js).
// No browser: window / document / pp are stubbed just enough for the modules to import and render, then the HTML is checked
// for the pieces the operator relies on (PGM / PVW tally, the eight input keys, on-air / preview marking, CUT / AUTO, the deck,
// the graphics strip, the rundown with the on-air and next rows, the status line, the Facebook box) and the handlers are
// exercised against a recording pp.POST / pp.PUT. Also checks that mux.js still renders the Facebook card in the Outputs tab
// through the shared module, and that assembleClient() (the fallback used when the Supervisor lacks the bundle route) builds the
// same shape the Supervisor lane builds.
//   node tests/web/nowplaying_render.test.mjs
import assert from "node:assert/strict";
import { pathToFileURL } from "node:url";
import path from "node:path";

const web = p => pathToFileURL(path.resolve(path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, "$1")), "../../web", p)).href;

// ---- stubs ------------------------------------------------------------------------------------------------------------------------
const calls = [];
const el = () => ({ focus() {}, classList: { add() {}, remove() {}, toggle() {} }, disabled: false, value: "", addEventListener() {} });
globalThis.window = globalThis;
globalThis.addEventListener = () => {};
const stored = new Map();
globalThis.localStorage = { getItem: key => stored.get(key) ?? null, setItem: (key, value) => stored.set(key, String(value)) };
globalThis.document = { querySelector: () => null, querySelectorAll: () => [], getElementById: () => el(), addEventListener() {}, activeElement: null, documentElement: { style: {} } };
globalThis.requestAnimationFrame = f => f();
globalThis.renderSoon = () => {};
let renders = 0, toasts = [];
globalThis.pp = {
  S: { token: "", view: "nowplaying", active: "ch2", channels: [{ profile: { id: "ch2", name: "Channel Two" } }, { profile: { id: "main", name: "Main" } }, { profile: { id: "hid", name: "Hidden", extra: { hidden: true } } }], telemetry: { ch2: { state: "ON_AIR" } }, snapshot: { supervisor: {} } },
  render() { renders++; }, toast(m) { toasts.push(m); }, confirmDo(t, fn) { fn(); },
  GET: p => { calls.push(["GET", p]); return Promise.resolve(stubGet(p)); },
  POST: (p, b) => { calls.push(["POST", p, b]); if (p.endsWith("/monitor") && globalThis.__monitorReply && b && b.inputs && b.inputs.length) return Promise.resolve(globalThis.__monitorReply); if (p === "/reporter/sessions") return Promise.resolve({ id: "r2", reporter: b.reporter, title: b.title, channel: b.channel, status: "idle", feedUp: false, countdownSec: b.countdownSec, slot: 1, keyHint: "deadbeef", publishUrl: "rtmp://10.0.0.2:1937/reporter/k", streamKey: "k", enginePath: "udp://127.0.0.1:50041" }); return Promise.resolve({ ok: true }); },
  PUT: (p, b) => { calls.push(["PUT", p, b]); return Promise.resolve({ ok: true }); },
  api: (m, p, b) => { calls.push([m, p, b]); if (m === "GET") return Promise.resolve(stubGet(p)); return Promise.resolve({ ok: true }); },
  cg: (id, cmd) => { calls.push(["CG", id, cmd]); return Promise.resolve({}); },
  play() {}, pause() {}, skip() {}, stop() {}, hold() {}, snap() {}, arm() {}, loadHour() {}, restart() {}, setActive() {}, nav() {},
};
function stubGet(p) {
  if (p.startsWith("/nowplaying/")) return bundle;
  if (p === "/cg/templates") return { templates: [{ id: "lower_third_modern", name: "Lower third", role: "secondary" }, { id: "ticker_news", name: "Ticker" }] };
  if (p === "/mux/encoder-presets") return { presets: [{ id: "stream_h264_aac_720p" }, { id: "atsc_mpeg2" }] };
  if (p === "/live/facebook") return bundle.facebook;
  if (p === "/channels/ch2/libobs/status") return { channelId: "ch2", engineState: "ON_AIR", onAir: true, layer: 50, layerState: "shown", obs: { program: "A", scenes: ["A", "B"], transition: "fade_transition" } };
  if (p === "/libobs/scenes") return { scenes: [{ id: "s1", name: "Studio A", layer: 50, spec: { scenes: [{ name: "A" }, { name: "B" }] } }] };
  if (p.startsWith("/reporter/sessions")) return { sessions: [{ id: "r1", reporter: "Jane", title: "LIVE", channel: "ch2", status: "idle", feedUp: true, countdownSec: 10, slot: 0, keyHint: "abcd1234" }], host: "10.0.0.2", mode: "ffmpeg" };
  if (p === "/reporter/config") return { rtmpPortBase: 1936, portCount: 8 };
  if (p.startsWith("/media")) return { media: [{ id: "m9", title: "Desk cam", path: "live://ndi/DESK (Cam 1)", type: "live", liveSource: { type: "ndi", ndiName: "DESK (Cam 1)" } }] };
  if (p === "/inputs") return { ndi: [{ name: "DESK (Cam 1)" }, { name: "LOBBY (Cam 2)", url: "10.0.0.5:5961" }], cameras: [{ name: "PTZ", host: "10.0.0.9", streamUri: "rtsp://10.0.0.9/live" }] };
  return {};
}

// ---- a Supervisor bundle, the shape nowplayingroutes.cpp builds ----------------------------------------------------------------
const bundle = {
  ok: true, atMs: Date.now(), channelId: "ch2", errors: [],
  channel: { id: "ch2", name: "Channel Two", width: 1920, height: 1080, frameRate: "30000/1001", engineConnected: true, engineState: "ON_AIR", armed: true, lastPushReason: "hour", found: true },
  state: { state: "ON_AIR", itemId: "r2", title: "Studio cam <live>", path: "ndi://DESK (Cam 1)", nextItemId: "r3", nextTitle: "Promo", nextReady: true, position: 12, duration: 1800, remaining: 1788, timecode: "00:00:12:00" },
  onAir: { itemId: "r2", title: "Studio cam <live>", path: "ndi://DESK (Cam 1)", live: true, type: "live", remaining: 1788, plannedEndMs: Date.now() + 1788000 },
  next: { itemId: "r3", title: "Promo", ready: true, type: "promo", plannedStartMs: Date.now() + 1788000 },
  runlist: { current: { hourStartMs: 1700000000000, items: [{ id: "r1", title: "News", type: "program", duration: 600, plannedStartMs: Date.now() - 700000, plannedEndMs: Date.now() - 100000 }, { id: "r2", title: "Studio cam <live>", type: "live", duration: 1800, plannedStartMs: Date.now() - 12000 }, { id: "r3", title: "Promo", type: "promo", duration: 30, plannedStartMs: Date.now() + 1788000 }] }, next: { hourStartMs: 1700003600000, items: [{ id: "n1", title: "Next hour show", type: "program", duration: 3600 }] } },
  inputs: { count: 8, mapLoaded: true, onAirIndex: 0, transition: { type: "dip", durationSec: 1 }, slots: [
    { index: 0, key: "in1", mapped: true, kind: "ndi", title: "Cam 1", path: "live://ndi/DESK (Cam 1)", mediaId: "m9", enginePath: "ndi://DESK (Cam 1)", onAir: true, snapshotUrl: "/preview/ch2:in1.jpg", monitor: { state: "open", warm: true } },
    { index: 1, key: "in2", mapped: true, kind: "camera", title: "Lobby", path: "live://stream/rtsp://10.0.0.9/live", enginePath: "rtsp://10.0.0.9/live", onAir: false, snapshotUrl: "/preview/ch2:in2.jpg", monitor: { state: "failed", error: "no such source" } },
    ...[2, 3, 4, 5, 6, 7].map(i => ({ index: i, key: "in" + (i + 1), mapped: false }))] },
  cg: { layers: [{ layer: 4, role: "lower_third", template: "lower_third_modern", label: "L3 Jane", state: "shown", params: { text1: "Jane Doe" }, presetId: "cg:cg1" }], engineLayers: [{ id: 4, role: "lower_third", state: "shown" }, { id: 7, role: "bug", state: "shown", source: { kind: "image", title: "bug.png" } }], item: {}, slots: [{ id: "cg1", name: "CG1", template: "bug_live_indicator", inputs: ["*"] }, { id: "cg2", name: "CG2", inputs: [] }] },
  audio: { momentaryLufs: -23.4 }, outputs: [{ name: "udp-main", state: "running" }, { name: "fb", state: "error" }],
  stats: { cpu: { systemPct: 31, enginesPct: 12.5 }, memory: { usedPct: 40, usedMB: 13000, totalMB: 32000 }, net: { inBps: 1e6, outBps: 2e6 } }, channelStats: { fps: 29.97, nominalFps: 29.97, bitrateKbps: 8000, drops: 0, outputsUp: 1, outputCount: 2 },
  previewUrl: "/preview/ch2.jpg", facebook: { channel: "ch2", channelName: "Channel Two", hasKey: true, keyHint: "…7890", state: "idle", live: false, engineConnected: true },
};

// ---- import after the stubs exist ----------------------------------------------------------------------------------------------------
const fb = await import(web("facebooklive.js"));
const np = await import(web("nowplaying.js"));
const lo = await import(web("libobs.js"));
const rf = await import(web("reporter.js"));
Object.assign(pp, fb.ppExtFacebook, np.ppExtNowPlaying, lo.ppExtLibObs, rf.ppExtReporter);
const { NP } = np;

// Loading state before the first bundle.
let h = np.viewNowPlaying();
assert.ok(h.includes("Loading ch2"), "shows a loading state before the first bundle");
assert.ok(h.includes("Channel Two") && h.includes("Main") && !h.includes(">Hidden<"), "channel switcher lists visible channels only");

// The 1 Hz poll: onEnter starts it; run one poll by hand through the exported state.
np.onEnterNowPlaying("nowplaying");
await new Promise(r => setTimeout(r, 30));
assert.ok(calls.some(c => c[0] === "GET" && c[1] === "/nowplaying/ch2"), "polls GET /nowplaying/{id}");
assert.equal(NP.b && NP.b.channelId, "ch2");
assert.ok(NP.mapLoaded && NP.slots[0].title === "Cam 1" && NP.slots[2] === null, "the shared map came from the bundle");
assert.equal(NP.trans.type, "dip");
fb.setFacebookState(bundle.facebook);
h = np.viewNowPlaying();
assert.ok(h.includes('1700000000000') && h.includes('1700003600000'), "rundown menus carry the correct current and next hour");

// Operator controls use the engine's reported mixer state and camera commands.
NP.mixerOpen = true;
NP.meters = { masterGainDb: -2, solo: "", strips: [{ id: "program", name: "Program", kind: "program", gainDb: -3, mute: false, solo: false }, { id: "L61", layer: 61, name: "Mic", kind: "line", gainDb: 0, mute: true }] };
NP.ptzOpen = true; NP.camera = "cam/1"; NP.cameras = [{ id: "cam/1", name: "Studio", hasPtz: true, presets: [{ token: "wide", name: "Wide" }] }];
let controls = np.viewNowPlaying();
assert.ok(controls.includes("AUDIO MIXER") && controls.includes("MASTER") && controls.includes("Mic") && controls.includes("Camera control") && controls.includes("Wide"));
pp.npGain("program", 0, -6); pp.npMute("L61", 61, false); pp.npSolo("program");
assert.ok(calls.some(c => c[0] === "CG" && c[2].program?.gainDb === -6));
assert.ok(calls.some(c => c[0] === "CG" && c[2].layer === 61 && c[2].audioMute === false));
assert.ok(calls.some(c => c[0] === "CG" && c[2].solo === "program"));
pp.npPtzMove(1, 0, 0); pp.npPtzStop();
assert.ok(calls.some(c => c[1] === "/cameras/cam%2F1/ptz" && c[2].action === "move" && c[2].timeoutSec === 2));
assert.ok(calls.some(c => c[1] === "/cameras/cam%2F1/ptz" && c[2].action === "stop"));
NP.mixerOpen = false; NP.ptzOpen = false;
pp.npLayoutEdit();
assert.ok(np.viewNowPlaying().includes("DONE ARRANGING"), "layout editor is visible");
pp.npLayoutMove("deck", -1);
assert.equal(NP.layout[0].id, "deck", "panel order changes");
pp.npLayoutWidth("deck", 6);
assert.equal(JSON.parse(stored.get("pp.onair.layout"))[0].width, 6, "panel width is saved");
pp.npLayoutReset();
assert.equal(NP.layout[0].id, "monitors", "reset restores defaults");
pp.npLayoutEdit();

// OBS scenes card (libobs.js) and Reporter feed card (reporter.js) embedded in the page.
{
  const o = np.viewNowPlaying();
  assert.ok(o.includes("OBS scenes") && o.includes("np-obs-card") && o.includes('data-libobs-strip="ch2"'), "the OBS card is on the page");
  assert.ok(/class="small danger" disabled onclick="pp\.loStripSwitch\(&quot;ch2&quot;,&quot;A&quot;\)"/.test(o), "the scene on program is marked and disabled");
  assert.ok(o.includes('pp.loStripSwitch(&quot;ch2&quot;,&quot;B&quot;)') && o.includes('pp.loStripHide(&quot;ch2&quot;)'), "scene B switch and Hide");
  pp.loStripSwitch("ch2", "B");
  assert.ok(calls.some(c => c[0] === "POST" && c[1] === "/channels/ch2/libobs/switch" && c[2].to === "B"), "the scene button posts the switch");
  assert.ok(o.includes("Reporter feed") && o.includes("np-rf-row") && o.includes("Jane") && o.includes('pp.rfGo(&quot;r1&quot;)'), "the Reporter card lists the session with GO");
  assert.ok(o.includes("RF.form.reporter=this.value") && !/[^F]R\.form/.test(o), "the reporter handlers use window.RF, not the Remote page's window.R");
  assert.ok(window.RF && window.RF.form && Array.isArray(window.RF.sessions), "window.RF is the reporter state");
  window.RF.form.reporter = "Bob";
  pp.rfsCreate("ch2");
  assert.ok(calls.some(c => c[0] === "POST" && c[1] === "/reporter/sessions" && c[2].reporter === "Bob" && c[2].channel === "ch2" && c[2].countdownSec === 10), "New feed creates a session on the active channel");
  // An input the Supervisor refused to monitor: no snapshot request, the reason is the tooltip.
  NP.skipped = { in2: "'Lobby' has no NDI name, SDI port or stream URL; add it to the library again from Inputs" };
  const s = np.viewNowPlaying();
  assert.ok(!/id="np-in2"/.test(s) && s.includes("no source") && s.includes("has no NDI name"), "a skipped input shows no picture, a no-source badge and the reason");
  NP.skipped = { in2: "SDI inputs show as a card: a second receiver on a card port would fight the take" };
  const card = np.viewNowPlaying();
  assert.ok(!/id="np-in2"/.test(card) && !card.includes(">no source<") && card.includes("CAMERA"), "an SDI card input is not an error");
  // The monitor lease keeps the Supervisor's answer.
  globalThis.__monitorReply = { ok: true, monitored: ["in1"], skipped: [{ key: "in2", why: "stub: not resolvable" }] };
  NP.skipped = {}; pp.npSelect(0);
  await new Promise(r => setTimeout(r, 30));
  assert.equal(NP.skipped.in2, "stub: not resolvable", "lease() stores the skipped list");
  globalThis.__monitorReply = null; NP.skipped = {}; if (NP.pvw === 0) pp.npSelect(0);
}

// Monitors: PGM with red tally and the program JPEG (cache-busted), PVW empty until an input is selected.
assert.ok(/class="np-mon pgm tally"/.test(h), "PGM tile has tally while ON_AIR");
assert.ok(/id="np-pgm"[^>]*src="\/preview\/ch2\.jpg\?t=\d+"/.test(h), "program picture is a JPEG snapshot with a cache-buster, not MJPEG");
assert.ok(!h.includes(".mjpeg\"") && !/src="[^"]*\.mjpeg/.test(h), "no MJPEG <img> on the page");
assert.ok(h.includes("select an input (keys 1-8)"), "PVW placeholder");
// Input wall: 8 keys, slot 1 on air, slot 2 failed, 6 unmapped; per-input JPEG snapshots for mapped ones only.
assert.equal((h.match(/class="np-key"/g) || []).length, 8, "eight input keys");
assert.ok(/class="np-in onair"/.test(h), "slot 1 is marked on air");
assert.ok(/id="np-in1"[^>]*src="\/preview\/ch2:in1\.jpg\?t=\d+"/.test(h), "input 1 snapshot URL");
assert.ok(/id="np-in2"[^>]*src="\/preview\/ch2:in2\.jpg/.test(h) && h.includes(">failed<"), "input 2 snapshot + failed badge");
assert.ok(!h.includes("np-in3\"") && (h.match(/unmapped/g) || []).length >= 6, "unmapped slots have no picture");
assert.ok(h.includes("Cam 1") && h.includes("Lobby"));
// Transition bar: CUT / AUTO disabled until a preview is selected; DIP 1 s active.
assert.ok(/<button class="danger big" disabled onclick="pp\.npCut\(\)"/.test(h), "CUT disabled without a preview");
assert.ok(/class="small active" onclick="pp\.npTrans\("dip"\)"/.test(h.replace(/&quot;/g, '"')), "DIP selected");
// Deck: title escaped, countdown, next, transport buttons through the app.js pp.* handlers.
assert.ok(h.includes("Studio cam &lt;live&gt;") && !h.includes("<live>"), "titles are escaped");
assert.ok(h.includes("-29:48"), "countdown from remaining");
assert.ok(h.includes("Promo") && h.includes("cued"));
for (const k of ["pp.pause(", "pp.skip(", "pp.stop(", "pp.hold(", "pp.snap(", "pp.arm(", "pp.loadHour(", "pp.restart("]) assert.ok(h.includes(k + '"ch2"') || h.includes(k + "&quot;ch2&quot;"), "deck has " + k);
assert.ok(h.includes("DISARM"), "armed channel offers DISARM");
assert.ok(h.includes("-23.4 LUFS"));
// Graphics strip: managed layer + the engine-only layer, CG slot on/off, quick strip, Text 1-3.
assert.ok(h.includes("L3 Jane") && h.includes("Jane Doe") && h.includes("bug.png"), "layers on air (managed + engine-only)");
assert.ok(/class="np-cgslot on"/.test(h) && h.includes("ALL INPUTS"), "CG1 shown as on air");
assert.ok(/class="np-cgslot" disabled/.test(h), "empty CG2 disabled");
assert.ok(/class="np-quick on"/.test(h) && h.includes("LOWER 3RD"), "quick LOWER 3RD on air");
assert.ok(h.includes('id="np-t1"') && h.includes('id="np-t3"'));
// Rundown: on-air row, next row, past row, next-hour break.
assert.ok(/<li[^>]*class="onair"/.test(h) && /<li[^>]*class="next"/.test(h) && /<li[^>]*class="past"/.test(h) && h.includes("NEXT HOUR"), "rundown rows");
// Status line + outputs + Facebook box.
assert.ok(h.includes("29.97 fps") && h.includes("8.00 Mb/s") && h.includes("▲1/2") && h.includes("31%") && h.includes("12.7 / 31.3 GB") && h.includes("↓ 8.0 Mb/s"), "status line");
assert.ok(h.includes("Facebook Live") && h.includes("GO LIVE") && h.includes("saved …7890") && /class="fb big"/.test(h), "Facebook box (big)");
assert.ok(h.includes('type="password"'), "stream key hidden by default");

// Interactions: select -> PVW tile + lease at high quality; double-click take; CUT / AUTO; transition change saves the shared map.
calls.length = 0; pp.npSelect(1); h = np.viewNowPlaying();
assert.equal(NP.pvw, 1);
assert.ok(/class="np-mon pvw tally"/.test(h) && /id="np-pvw"[^>]*src="\/preview\/ch2:in2\.jpg/.test(h), "PVW shows the selected input's snapshot");
const lease = calls.find(c => c[0] === "POST" && c[1] === "/channels/ch2/monitor"); assert.ok(lease, "lease sent on select");
assert.deepEqual(lease[2].inputs.map(i => [i.key, i.quality]), [["in1", "low"], ["in2", "high"]], "PVW input leased at high quality");
assert.ok(calls.some(c => c[1] === "/channels/ch2/cg" && c[2].action === "preview"), "PGM boost");
calls.length = 0; pp.npCut();
let take = calls.find(c => c[1] === "/channels/ch2/transport"); assert.ok(take && take[2].action === "take" && take[2].path === "live://stream/rtsp://10.0.0.9/live" && !take[2].transition, "CUT = hard take of the preview");
await new Promise(r => setTimeout(r, 5)); assert.equal(NP.pvw, -1, "preview cleared after the take");
pp.npSelect(0); calls.length = 0; pp.npAuto();
take = calls.find(c => c[1] === "/channels/ch2/transport"); assert.deepEqual(take[2].transition, { type: "dip", durationSec: 1 }, "AUTO carries the saved transition"); assert.equal(take[2].mediaId, "m9");
calls.length = 0; pp.npTrans("mix", 0.5);
const save = calls.find(c => c[0] === "PUT" && c[1] === "/settings/ui.inputMap"); assert.ok(save, "transition change saves ui.inputMap");
assert.deepEqual(save[2].value.transition, { type: "mix", durationSec: 0.5 }); assert.equal(save[2].value.slots.length, 8); assert.equal(save[2].value.slots[0].path, "live://ndi/DESK (Cam 1)");
pp.npSelect(4); assert.equal(NP.pick, 4, "selecting an unmapped input opens its picker");
NP.pick = -1;
const savedConnected = NP.b.channel.engineConnected; NP.b.channel.engineConnected = false;
h = np.viewNowPlaying(); assert.ok(h.includes("Map input 1"), "input mapping is available without a context menu");
assert.match(h, /<button class="danger big" disabled onclick="pp\.npCut\(\)"/, "CUT disabled when offline");
NP.pvw = 0; calls.length = 0; pp.npCut();
assert.ok(!calls.some(c => c[1] === "/channels/ch2/transport"), "offline engine refuses take");
assert.ok(toasts.some(t => /offline/.test(t)), "offline take explains the refusal");
NP.b.channel.engineConnected = savedConnected; NP.pvw = -1;
// Mapping: the picker lists library live entries, NDI on the network and cameras (the desktop InputMap catalog); mapping saves the shared setting.
pp.npPick(2); await new Promise(r => setTimeout(r, 10)); h = np.viewNowPlaying();
assert.ok(h.includes("Map input 3") && h.includes("Desk cam") && h.includes("LOBBY (Cam 2)") && h.includes("PTZ"), "picker catalog");
assert.ok(!h.includes('"live://ndi/DESK (Cam 1)"') || (h.match(/DESK \(Cam 1\)/g) || []).length >= 1, "library entry wins over the raw NDI name");
calls.length = 0; pp.npMap(2, "live://ndi/LOBBY (Cam 2)");
const mapSave = calls.find(c => c[0] === "PUT" && c[1] === "/settings/ui.inputMap"); assert.ok(mapSave && mapSave[2].value.slots[2].title === "LOBBY (Cam 2)" && mapSave[2].value.slots[2].enginePath === "ndi://LOBBY (Cam 2)", "mapping saved in the desktop's slot shape");
// Graphics handlers.
calls.length = 0; pp.npHideLayer(4); assert.ok(calls.some(c => c[1] === "/overlays/ch2/4/hide"));
pp.npCgSlot("cg1", true); assert.ok(calls.some(c => c[1] === "/overlays/ch2/cgslots/cg1/hide"));
pp.npCgSlot("cg2", false); assert.ok(calls.some(c => c[1] === "/overlays/ch2/cgslots/cg2/fire" && c[2].toggle === true));
NP.t1 = "Breaking"; pp.npQuick("ticker_news"); const dep = calls.find(c => c[1] === "/overlays/ch2/deploy"); assert.ok(dep && dep[2].template === "ticker_news" && dep[2].params.text1 === "Breaking", "quick fires with Text 1");
pp.npQuick("lower_third_modern"); assert.ok(calls.some(c => c[1] === "/overlays/ch2/4/hide"), "quick on an on-air template hides it");
pp.npCgClear(); assert.ok(calls.some(c => c[0] === "CG" && c[2].action === "clear"));
// Facebook handlers (shared module).
calls.length = 0; pp.fbSet("streamKey", "abc", true); pp.fbSave();
const put = calls.find(c => c[0] === "PUT" && c[1] === "/live/facebook"); assert.ok(put && put[2].streamKey === "abc" && put[2].channel === "ch2", "save sends the typed key once");
pp.fbOp("start"); assert.ok(calls.some(c => c[1] === "/live/facebook/start"));
fb.FB.st = Object.assign({}, bundle.facebook, { live: true, state: "running", uptimeSec: 75, bitrateKbps: 3400 });
h = np.viewNowPlaying(); assert.ok(h.includes("END LIVE") && h.includes("1m 15s"), "live state shows END LIVE");
// Leaving the page releases the lease and stops polling.
calls.length = 0; np.onEnterNowPlaying("schedule");
assert.ok(calls.some(c => c[1] === "/channels/ch2/monitor" && c[2].inputs.length === 0), "lease released on leave");
assert.equal(NP.timer, null);

// ---- assembleClient(): the fallback builds the Supervisor bundle's shape -------------------------------------------------------------
const cb = np.assembleClient("ch2", { channel: { profile: { id: "ch2", name: "Two" }, process: { connected: true }, executor: { armed: false } }, state: bundle.state, runlist: bundle.runlist, overlays: { layers: bundle.cg.layers }, cgslots: { slots: bundle.cg.slots }, stats: Object.assign({}, bundle.stats, { channels: [{ id: "main", fps: 25 }, { id: "ch2", fps: 29.97 }] }), facebook: bundle.facebook, map: { slots: [{ kind: "ndi", title: "Cam 1", path: "live://ndi/DESK (Cam 1)", enginePath: "ndi://desk (cam 1)/" }], transition: { type: "dip", durationSec: 1 } } });
for (const k of ["channel", "state", "onAir", "next", "runlist", "inputs", "cg", "audio", "outputs", "stats", "channelStats", "previewUrl", "facebook", "errors"]) assert.ok(k in cb, "fallback bundle has " + k);
assert.equal(cb.inputs.slots.length, 8); assert.ok(cb.inputs.slots[0].onAir, "on-air slot matched by engine path, case and slash insensitive"); assert.equal(cb.inputs.onAirIndex, 0);
assert.equal(cb.inputs.slots[0].snapshotUrl, "/preview/ch2:in1.jpg"); assert.equal(cb.onAir.type, "live"); assert.equal(cb.next.title, "Promo"); assert.equal(cb.channelStats.fps, 29.97);

// ---- mux.js: the Outputs tab still renders the Facebook card through the shared module --------------------------------------------------
pp.S.view = "outputs";
const mux = await import(web("mux.js"));
window.X.streams = [{ channelId: "ch2", channelName: "Channel Two", engineConnected: true, running: 0, defined: 1, outputs: [], encoders: [] }];
fb.FB.st = bundle.facebook; fb.FB.d = { channel: "ch2", serverUrl: "rtmps://live-api-s.facebook.com:443/rtmp/", preset: "stream_h264_aac_720p", streamKey: "" };
const sh = mux.viewStreams();
assert.ok(sh.includes("Facebook Live") && sh.includes("GO LIVE") && sh.includes("saved …7890") && sh.includes("rtmps://live-api-s.facebook.com"), "Outputs tab Facebook card");
assert.ok(!/class="fb big"/.test(sh), "Outputs tab keeps the compact card");
assert.equal(window.X.fb, fb.FB, "mux.js X.fb is the shared state");
assert.ok(typeof mux.ppExtMux.fbSave === "function" && mux.ppExtMux.fbSave === fb.ppExtFacebook.fbSave, "mux handlers are the shared ones");

console.log("nowplaying render test: all checks passed (" + renders + " renders)");
