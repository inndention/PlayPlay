/* PLAY PLAY web dashboard: frame-accurate segment editor (mirrors Play_Play_Native/qml/ui/SegmentTimeline.qml).
   - Preview of the exact frame at the playhead (GET /frames/<id>.jpg?frame=n&w=..), double buffered, with the
     storyboard tile shown while a frame is on its way (a cold jump into a 4K file can take seconds).
   - Timeline: ruler, storyboard filmstrip, segment lane and, with a transcript, a speech lane (sentences, and words
     from 40 px/s), zoomable (Ctrl+wheel, slider, +/-), scrollable, YouTube-style hover thumbnails. Segment edges
     drag and snap to frames, or to the silence between words while "snap to words" is on (hold Alt for frames).
   - Transcript panel: the media's words with the word at the playhead lit and followed. Click a word to go there;
     shift-click, drag or double-click (a sentence) to put in/out in the silences around the words, then Enter makes
     the segment, labelled with its first words. Find box: Enter next match, Shift+Enter previous. Without a
     transcript the panel offers to transcribe and shows the job's progress.
   - Keys (after a click in the editor): Left/Right frame (Shift 1 s, Ctrl 10 s), Up/Down edges, , . words, < >
     sentences, Home/End, Space, J/K/L shuttle, I/O, Enter add, [ ] set selected edges, Delete, +/- zoom.
   Cuts around words follow playout/supervisor/transcripttools.cpp: half the gap to the neighbour, at most 0.25 s;
   a word belongs to the range that holds its midpoint.
   The dashboard re-renders the page about once a second, so the editor keeps ONE element of its own and re-attaches
   it to the #segEditorHost placeholder whenever a render puts a new placeholder in the page. */

const PALETTE = ["#00d4ff", "#ff9f1c", "#7bd389", "#e86fd8", "#ffd23f", "#5b8cff", "#ff5d73", "#41ead4"];
const esc = s => String(s ?? "").replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
const PAD = 0.25;   // most silence kept next to a word at a cut

const E = {
  el: null, mediaId: "", duration: 0, segs: [], work: null, onEdit: null,
  fpsNum: 25, fpsDen: 1, frames: 1, dropFrame: false, sb: null, err: "",
  frame: 0, inF: -1, outF: -1, sel: -1, speed: 0, pps: 10, armed: false,
  front: 0, shown: -1, requested: -1, loading: false, playT0: 0, playF0: 0, timer: 0, hoverT: -1,
  scrollLeft: 0, textScroll: 0, focus: null,   // kept across the dashboard's re-renders (a detached element loses them)
  // transcript: tr = { words: [{t, s, e, si}], sents: [{s, e, a, b}], cuts, text, pos } (see buildTr)
  tr: null, trState: "", trPct: 0, trErr: "", trTimer: 0, trKey: "", levels: null, level: "",
  wEls: null, ranges: {}, cur: -1, curSent: -1, anchor: -1, selA: -1, selB: -1,
  follow: true, snap: true, userScrollT: 0, hits: [], hit: -1, segTextKey: "", segTextVal: "",
};
window.SEGED = E;   // for debugging and tests

const fps = () => (E.fpsDen > 0 && E.fpsNum > 0 ? E.fpsNum / E.fpsDen : 25);
const tf = f => f * E.fpsDen / E.fpsNum;
const ft = t => Math.floor(t * fps() + 1e-6);
const clampF = f => Math.max(0, Math.min(E.frames - 1, Math.round(f)));
const inFrame = t => clampF(Math.floor(t * fps() + 1e-6));   // an in point never clips the word after it
const outFrame = t => clampF(Math.ceil(t * fps() - 1e-6));   // an out point never clips the word before it
const view = () => (E.work || E.segs || []);
const pad = (n, w = 2) => String(Math.floor(Math.abs(n))).padStart(w, "0");
export function timecode(f, num = E.fpsNum, den = E.fpsDen) {
  const base = Math.round(num / den), drop = den === 1001 && (num === 30000 || num === 60000);
  f = Math.max(0, Math.round(f));
  if (drop) {
    const dn = base === 60 ? 4 : 2, per10 = base * 600 - 9 * dn, perMin = base * 60 - dn;
    const d = Math.floor(f / per10), m = f % per10;
    f += 9 * dn * d + (m > dn ? dn * Math.floor((m - dn) / perMin) : 0);
  }
  const ff = f % base, s = Math.floor(f / base);
  return `${pad(Math.floor(s / 3600))}:${pad(Math.floor(s / 60) % 60)}:${pad(s % 60)}${drop ? ";" : ":"}${pad(ff)}`;
}
const tc = f => timecode(f);
const clock = t => { t = Math.max(0, Math.floor(t)); const h = Math.floor(t / 3600), m = Math.floor(t / 60) % 60; return (h ? h + ":" + pad(m) : String(m)) + ":" + pad(t % 60); };
// binary searches over arrays sorted by key: first index with key >= x (or length), last index with key <= x (or -1)
function firstGE(a, x, key) { let lo = 0, hi = a.length; while (lo < hi) { const m = (lo + hi) >> 1; if (key(a[m]) < x) lo = m + 1; else hi = m; } return lo; }
const lastLE = (a, x, key) => firstGE(a, x + 1e-9, key) - 1;
const midOf = w => (w.s + w.e) / 2;
const norm = s => String(s).toLowerCase().replace(/[^\p{L}\p{N}'’ ]+/gu, "");

/** Called by the library view on every render: hands the editor its media and segments (no DOM work here). */
export function syncSegEditor(mediaId, durationSec, segments, onEdit) {
  E.onEdit = onEdit;
  if (E.work === null) E.segs = JSON.parse(JSON.stringify(segments || []));
  if (mediaId !== E.mediaId) { E.mediaId = mediaId; E.duration = durationSec || 0; load(); }
  else if (E.el) drawLane();
}
export const segEditorHost = () => `<div id="segEditorHost" class="seged-host"></div>`;
/** A transcription finished (WebSocket "transcriptReady"): load the words now rather than at the next poll. */
export function segEditorTranscriptReady(mediaId) { if (mediaId === E.mediaId && E.el) trLoad(); }

function load() {
  stop(); clearTimeout(E.trTimer);
  Object.assign(E, { sb: null, err: "", frame: 0, inF: -1, outF: -1, sel: -1, shown: -1, requested: -1, loading: false, frames: Math.max(1, Math.round(E.duration * 25)),
    tr: null, trState: "", trPct: 0, trErr: "", trTimer: 0, trKey: "", wEls: null, ranges: {}, cur: -1, curSent: -1, anchor: -1, selA: -1, selB: -1, hits: [], hit: -1, textScroll: 0, segTextKey: "" });
  build();
  const fi = $(".seged-find"); if (fi) fi.value = "";
  if (!E.mediaId) return;
  const id = E.mediaId;
  window.pp.GET("/media/" + id + "/frameinfo").then(d => {
    if (id !== E.mediaId) return;
    Object.assign(E, { fpsNum: d.fps.num, fpsDen: d.fps.den, frames: Math.max(1, Math.round(d.frames || d.durationSec * d.fps.num / d.fps.den)), duration: d.durationSec || E.duration, dropFrame: !!(d.timecode && d.timecode.dropFrame) });
    fit(); drawAll(); request();
  }).catch(e => { E.err = String(e && e.message || e || "no frame info"); drawInfo(); });
  window.pp.GET("/media/" + id + "/storyboard?n=120&w=160").then(d => { if (id !== E.mediaId) return; E.sb = d; drawStrip(); drawPlaceholder(); }).catch(() => {});
  trLoad();
}

// ---------------------------------------------------------------- transcript data
function cutBefore(w, i) { const gap = i > 0 ? w[i].s - w[i - 1].e : 2 * PAD; return Math.max(0, w[i].s - Math.min(PAD, Math.max(0, gap / 2))); }
function cutAfter(w, i) { const gap = i + 1 < w.length ? w[i + 1].s - w[i].e : 2 * PAD; return w[i].e + Math.min(PAD, Math.max(0, gap / 2)); }
function buildTr(t) {
  const words = [], sents = [];
  for (const s of t.segments || []) {
    const a = words.length, si = sents.length;
    for (const w of s.words || []) { const x = String(w.word ?? "").trim(); if (x) words.push({ t: x, s: +w.start || 0, e: Math.max(+w.start || 0, +w.end || 0), si }); }
    if (words.length === a && String(s.text ?? "").trim()) words.push({ t: String(s.text).trim(), s: +s.start || 0, e: Math.max(+s.start || 0, +s.end || 0), si });   // no word timings: the sentence is one "word"
    if (words.length > a) sents.push({ s: words[a].s, e: words[words.length - 1].e, a, b: words.length, txt: "" });
  }
  const cuts = []; for (let i = 0; i < words.length; i++) cuts.push(cutBefore(words, i), cutAfter(words, i));
  cuts.sort((x, y) => x - y);
  let text = ""; const pos = [];
  for (const w of words) { pos.push(text.length); text += norm(w.t) + " "; }
  return { words, sents, cuts, text, pos, language: t.language || "", model: t.model || "" };
}
function wordRange(a, b) { if (!E.tr) return [0, 0]; const w = E.tr.words; return [firstGE(w, a, midOf), firstGE(w, b, midOf)]; }
function textBetween(a, b) { if (!E.tr) return ""; const [i, j] = wordRange(a, b); return E.tr.words.slice(i, j).map(w => w.t).join(" "); }
function labelFrom(text) { let s = ""; for (const p of text.split(" ")) { const n = s ? s + " " + p : p; if (n.length > 40) break; s = n; } return s || text.slice(0, 40); }
function sentText(i) { const s = E.tr.sents[i]; if (!s.txt) s.txt = E.tr.words.slice(s.a, s.b).map(w => w.t).join(" "); return s.txt; }
function snapT(t) {
  const c = E.tr.cuts, i = firstGE(c, t, x => x); let best = t, bd = 10 / E.pps;   // within 10 px
  for (const j of [i - 1, i]) if (j >= 0 && j < c.length && Math.abs(c[j] - t) <= bd) { bd = Math.abs(c[j] - t); best = c[j]; }
  return best;
}
function segText(g) {
  if (!E.tr) return g.text || "";
  const key = g.startTime + "|" + g.endTime;
  if (key !== E.segTextKey) { E.segTextKey = key; E.segTextVal = textBetween(g.startTime, g.endTime); }
  return E.segTextVal;
}

function trLoad() {
  const id = E.mediaId; E.tr = null; E.trState = "loading"; E.trKey = ""; drawTr();
  window.pp.GET("/media/" + id).then(m => {
    if (id !== E.mediaId) return;
    const st = (m.transcript || {}).status || "";
    E.trErr = (m.transcript || {}).error || "";
    if (st === "done") window.pp.GET("/media/" + id + "/transcript").then(t => { if (id !== E.mediaId) return; E.tr = buildTr(t); E.trState = "done"; E.trKey = ""; drawTr(); drawStrip(); drawLane(); drawInfo(); })
      .catch(() => { if (id !== E.mediaId) return; E.trState = "failed"; E.trErr = "the transcript file could not be read"; drawTr(); });
    else if (st === "queued" || st === "running") { E.trState = st; drawTr(); trPoll(); }
    else { E.trState = st === "failed" || st === "cancelled" ? "failed" : "none"; drawTr(); trLevels(); }
  }).catch(() => { if (id !== E.mediaId) return; E.trState = "none"; drawTr(); trLevels(); });
}
function trLevels() {
  if (E.levels) return;
  window.pp.GET("/transcribe/levels").then(d => { E.levels = d.levels || []; E.level = E.level || (d.config || {}).defaultLevel || "balanced"; drawTr(); }).catch(() => {});
}
function trPoll() {
  clearTimeout(E.trTimer);
  E.trTimer = setTimeout(() => {
    E.trTimer = 0;
    if (!E.el || !E.el.isConnected || !(E.trState === "queued" || E.trState === "running")) return;   // resumed on re-attach
    const id = E.mediaId;
    window.pp.GET("/transcribe/jobs").then(d => {
      if (id !== E.mediaId) return;
      const js = (d.jobs || []).filter(j => j.mediaId === id), j = js[js.length - 1];
      if (!j || j.state === "done") { trLoad(); return; }            // finished (or the job list was cleared): the media record knows
      if (j.state === "failed" || j.state === "cancelled") { E.trState = "failed"; E.trErr = j.error || j.state; drawTr(); trLevels(); return; }
      E.trState = j.state; E.trPct = j.pct || 0; drawTr(); trPoll();
    }).catch(() => trPoll());
  }, 2500);
}
function transcribe() {
  const id = E.mediaId, s = E.el.querySelector(".seged-level"), lv = (s && s.value) || E.level || "balanced"; E.level = lv;
  window.pp.POST("/media/" + id + "/transcribe", { level: lv }).then(j => {
    if (id !== E.mediaId) return;
    if (j && j.error) { E.trState = "failed"; E.trErr = j.error; drawTr(); return; }
    window.pp.toast("Transcription queued (" + lv + ")", "ok");
    E.trState = (j && j.state) || "queued"; E.trPct = 0; E.trErr = ""; drawTr(); trPoll();
  }).catch(() => {});
}

// ---------------------------------------------------------------- DOM
function build() {
  if (!E.el) {
    const el = document.createElement("div");
    el.className = "seged"; el.tabIndex = 0;
    el.innerHTML = `
      <div class="seged-top">
        <div class="seged-prev"><img class="seged-a" alt=""><img class="seged-b" alt="" style="display:none"><div class="seged-ph"></div>
          <div class="seged-tc"></div><div class="seged-spd"></div><div class="seged-err"></div></div>
        <div class="seged-side">
          <div class="seged-big mono"></div><div class="seged-meta"></div>
          <div class="seged-io"></div>
          <div class="row" style="gap:6px;flex-wrap:wrap"><button class="small" data-a="in">In (I)</button><button class="small" data-a="out">Out (O)</button><button class="small primary" data-a="add">+ Segment (Enter)</button></div>
          <div class="seged-selbox"></div>
          <div class="seged-help">&#8592;/&#8594; frame &middot; Shift 1 s &middot; Ctrl 10 s &middot; &#8593;/&#8595; edges &middot; , . words &middot; &lt; &gt; sentences &middot; Space play &middot; J K L shuttle &middot; I/O in-out &middot; Enter add &middot; [ ] set edges &middot; Del remove &middot; Ctrl+wheel zoom &middot; shift-click words for in/out</div>
        </div>
      </div>
      <div class="seged-transport row">
        <button class="ghost small" data-a="prevEdge" title="previous edge (&#8593;)">&#9198;</button><button class="ghost small" data-a="back" title="back one frame (&#8592;)">&#9664;&#10072;</button>
        <button class="small primary seged-play" data-a="play" title="play / pause (Space)">&#9654;</button>
        <button class="ghost small" data-a="fwd" title="forward one frame (&#8594;)">&#10072;&#9654;</button><button class="ghost small" data-a="nextEdge" title="next edge (&#8595;)">&#9197;</button>
        <button class="ghost small" data-a="J">J</button><button class="ghost small" data-a="K">K</button><button class="ghost small" data-a="L">L</button>
        <input class="seged-go mono" name="segedGo" placeholder="go to TC / s" style="width:120px">
        <span class="fill"></span><span class="muted" style="font-size:10px">zoom</span><input type="range" class="seged-zoom" min="0" max="1000" value="0" style="width:150px">
        <button class="ghost small" data-a="fit">Fit</button>
      </div>
      <div class="seged-tl">
        <div class="seged-scroll"><div class="seged-content">
          <div class="seged-ruler"></div><div class="seged-strip"></div><div class="seged-inout"></div><div class="seged-lane"></div><div class="seged-words"></div><div class="seged-head"></div>
        </div></div>
        <div class="seged-hover"><div class="seged-hover-img"></div><div class="seged-hover-tc mono"></div></div>
      </div>
      <div class="seged-script">
        <div class="seged-script-bar">
          <b>Transcript</b><span class="seged-trinfo"></span>
          <span class="seged-trtools">
            <input class="seged-find" name="segedFind" placeholder="find words&hellip;" autocomplete="off">
            <span class="seged-findn mono"></span>
            <button class="ghost small" data-a="findPrev" title="previous match (Shift+Enter)">&#8593;</button><button class="ghost small" data-a="findNext" title="next match (Enter)">&#8595;</button>
            <span class="fill"></span>
            <label class="seged-chk" title="keep the word at the playhead in view"><input type="checkbox" class="seged-follow" checked> follow</label>
            <label class="seged-chk" title="dragged segment edges land in the silence between words (hold Alt for frames)"><input type="checkbox" class="seged-snap" checked> snap to words</label>
          </span>
        </div>
        <div class="seged-wsel"></div>
        <div class="seged-text"></div>
      </div>`;
    E.el = el; wire(el);
  }
  drawAll();
}
const $ = s => E.el.querySelector(s);

function drawAll() { if (!E.el) return; drawInfo(); drawContent(); drawStrip(); drawLane(); drawHead(); drawPlaceholder(); drawTr(); }
function drawInfo() {
  if (!E.el) return;
  $(".seged-big").textContent = tc(E.frame);
  $(".seged-meta").textContent = `of ${tc(E.frames - 1)} · ${+fps().toFixed(3)} fps${E.dropFrame ? " drop-frame" : ""} · ${E.frames} frames`;
  $(".seged-tc").textContent = `${tc(E.frame)}   f ${E.frame}`;
  const spd = $(".seged-spd"); spd.style.display = E.speed ? "" : "none"; spd.textContent = (E.speed < 0 ? "◀◀ " : "▶ ") + Math.abs(E.speed) + "x";
  $(".seged-play").innerHTML = E.speed ? "&#9208;" : "&#9654;";
  $(".seged-err").textContent = E.err;
  const io = E.inF >= 0 || E.outF >= 0;
  $(".seged-io").innerHTML = `<span class="muted">IN</span> <a class="mono" data-seek="${E.inF}" style="color:var(--ready)">${E.inF >= 0 ? tc(E.inF) : "—"}</a>
    <span class="muted" style="margin-left:8px">OUT</span> <a class="mono" data-seek="${E.outF}" style="color:var(--danger)">${E.outF >= 0 ? tc(E.outF) : "—"}</a>
    ${io && E.outF > E.inF && E.inF >= 0 ? `<span class="mono muted">= ${tc(E.outF - E.inF)}</span>` : ""}`;
  const v = view(), g = E.sel >= 0 && E.sel < v.length ? v[E.sel] : null;
  const box = $(".seged-selbox");
  if (!g) { box.innerHTML = ""; return; }
  const typing = E.focus && E.focus.cls === "seged-label" && E.focus.sel === E.sel ? E.focus : null;
  const said = segText(g);
  box.innerHTML = `<div class="row" style="gap:6px"><span class="seged-dot" style="background:${PALETTE[E.sel % PALETTE.length]}"></span>
      <input class="seged-label" name="segedLabel" placeholder="segment label (timeline)" value="${esc(typing ? typing.v : (g.label ?? ""))}" style="flex:1"></div>
    <div class="row mono" style="gap:8px;font-size:11px"><a data-seek="${ft(g.startTime)}">${tc(ft(g.startTime))}</a> &#8594; <a data-seek="${ft(g.endTime)}">${tc(ft(g.endTime))}</a> <span class="muted">${tc(ft(g.endTime - g.startTime))}</span></div>
    ${said ? `<div class="seged-segtext" title="${esc(said)}">&ldquo;${esc(said.length > 240 ? said.slice(0, 238) + "…" : said)}&rdquo;</div>` : ""}
    <div class="row" style="gap:6px;flex-wrap:wrap"><button class="ghost small" data-a="setStart">Start = here [</button><button class="ghost small" data-a="setEnd">End = here ]</button>
      <button class="ghost small" data-a="playSel">&#9654; from start</button>${E.tr && said ? `<button class="ghost small" data-a="labelWords" title="use the segment's first words as its label">Label from words</button>` : ""}<button class="ghost small" data-a="del">Delete</button></div>`;
  if (typing && E.el.isConnected) { const i = box.querySelector(".seged-label"); i.focus(); try { i.setSelectionRange(typing.s, typing.e); } catch {} }
}
const W = () => (E.el ? $(".seged-scroll").clientWidth : 600);
function contentWidth() { return Math.max(W(), E.duration * E.pps); }
function drawContent() { if (!E.el) return; $(".seged-content").style.width = contentWidth() + "px"; syncZoomSlider(); }
function visRange() { const sc = $(".seged-scroll"); return [sc.scrollLeft / E.pps, (sc.scrollLeft + sc.clientWidth) / E.pps]; }
function drawRuler() {
  const cands = [1 / fps(), 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600];
  let step = cands[cands.length - 1]; for (const c of cands) if (c * E.pps >= 80) { step = c; break; }
  const [t0, t1] = visRange(); let h = "";
  for (let t = Math.floor(t0 / step) * step, n = 0; t <= t1 + step && n < 400; t += step, n++) if (t >= 0 && t <= E.duration) h += `<div class="seged-tick" style="left:${t * E.pps}px"><span>${tc(ft(t))}</span></div>`;
  $(".seged-ruler").innerHTML = h;
}
function tileIndexAt(t) { return E.sb ? Math.max(0, Math.min(E.sb.n - 1, Math.floor(t / E.sb.interval))) : 0; }
function spriteCss(idx, w) {
  const sb = E.sb, k = w / sb.tileW, url = window.pp.rawUrl ? window.pp.rawUrl(sb.url) : sb.url;
  return `background-image:url('${url}');background-size:${sb.spriteW * k}px ${sb.spriteH * k}px;background-position:-${(idx % sb.cols) * sb.tileW * k}px -${Math.floor(idx / sb.cols) * sb.tileH * k}px`;
}
function setSprite(elm, idx, w) {
  const sb = E.sb, k = w / sb.tileW, url = window.pp.rawUrl ? window.pp.rawUrl(sb.url) : sb.url;
  elm.style.backgroundImage = `url('${url}')`; elm.style.backgroundSize = `${sb.spriteW * k}px ${sb.spriteH * k}px`;
  elm.style.backgroundPosition = `-${(idx % sb.cols) * sb.tileW * k}px -${Math.floor(idx / sb.cols) * sb.tileH * k}px`;
}
function drawStrip() { if (!E.el) return; drawRuler(); drawWords(); drawTiles(); }
function drawTiles() {
  const strip = $(".seged-strip");
  if (!E.sb) { strip.innerHTML = `<div class="muted" style="padding:16px;font-size:11px">building storyboard…</div>`; return; }
  const h = 50, w = h * E.sb.tileW / E.sb.tileH, sc = $(".seged-scroll"), k0 = Math.floor(sc.scrollLeft / w), n = Math.ceil(sc.clientWidth / w) + 2;
  let out = "";
  for (let k = k0; k < k0 + n; k++) { const x = k * w; if (x > contentWidth()) break; out += `<div class="seged-tile" style="left:${x}px;width:${w}px;height:${h}px;${spriteCss(tileIndexAt((k + 0.5) * w / E.pps), w)}"></div>`; }
  strip.innerHTML = out;
}
// Speech lane: only what is in view; sentences, or single words once a word is wide enough to read (40 px/s).
function drawWords() {
  const lane = $(".seged-words");
  if (!E.tr) { lane.innerHTML = ""; return; }
  const [t0, t1] = visRange(), out = [];
  if (E.pps >= 40) {
    const w = E.tr.words;
    for (let i = firstGE(w, t0 - 10, x => x.s); i < w.length && w[i].s <= t1 && out.length < 800; i++) {
      if (w[i].e < t0) continue;
      out.push(`<div class="seged-wd${i === E.cur ? " cur" : ""}" data-w="${i}" style="left:${w[i].s * E.pps}px;width:${Math.max(2, (w[i].e - w[i].s) * E.pps)}px">${esc(w[i].t)}</div>`);
    }
  } else {
    const s = E.tr.sents;
    for (let i = firstGE(s, t0 - 60, x => x.s); i < s.length && s[i].s <= t1 && out.length < 1500; i++) {
      if (s[i].e < t0) continue;
      const wd = Math.max(1, (s[i].e - s[i].s) * E.pps), txt = sentText(i);
      out.push(`<div class="seged-st" data-si="${i}" title="${clock(s[i].s)}  ${esc(txt)}" style="left:${s[i].s * E.pps}px;width:${wd}px">${wd > 24 ? esc(txt) : ""}</div>`);
    }
  }
  lane.innerHTML = out.join("");
}
function drawLane() {
  if (!E.el) return;
  const v = view();
  $(".seged-lane").innerHTML = v.map((g, i) => {
    const c = PALETTE[i % PALETTE.length], x = g.startTime * E.pps, w = Math.max(4, (g.endTime - g.startTime) * E.pps);
    const z = E.sel === i ? 4 : w < 24 ? 3 : w < 120 ? 2 : 1;   // the selected and the short segments stay reachable on top of long ones
    return `<div class="seged-seg${E.sel === i ? " sel" : ""}" data-i="${i}" title="${esc((g.label || "") + (g.text ? "\n" + g.text : ""))}" style="left:${x}px;width:${w}px;z-index:${z};background:${c}${E.sel === i ? "dd" : "88"};border-color:${E.sel === i ? "#fff" : c}">
      <span>${i + 1}&nbsp; ${esc(g.label || "")}</span><b class="seged-h l" data-i="${i}" data-e="start"></b><b class="seged-h r" data-i="${i}" data-e="end"></b></div>`;
  }).join("");
  const io = $(".seged-inout");
  if (E.inF >= 0 || E.outF >= 0) { const a = E.inF >= 0 ? E.inF : E.frame, b = E.outF >= 0 ? E.outF : E.frame; io.style.display = ""; io.style.left = tf(Math.min(a, b)) * E.pps + "px"; io.style.width = Math.max(2, tf(Math.abs(b - a)) * E.pps) + "px"; }
  else io.style.display = "none";
  drawInfo(); tintRanges();
}
function drawHead() { if (!E.el) return; $(".seged-head").style.left = (tf(E.frame) * E.pps - 1) + "px"; }
function drawPlaceholder() {
  if (!E.el) return;
  const ph = $(".seged-ph");
  if (!E.sb || E.shown === E.frame) { ph.style.display = "none"; return; }
  const box = $(".seged-prev"), bw = box.clientWidth - 2, bh = box.clientHeight - 2, k = Math.min(bw / E.sb.tileW, bh / E.sb.tileH);
  ph.style.display = ""; ph.style.width = E.sb.tileW * k + "px"; ph.style.height = E.sb.tileH * k + "px";
  setSprite(ph, tileIndexAt(tf(E.frame)), E.sb.tileW * k);
}

// ---------------------------------------------------------------- transcript panel
function drawTr() {
  if (!E.el) return;
  E.el.classList.toggle("has-tr", !!E.tr);
  const info = $(".seged-trinfo"), box = $(".seged-text");
  if (E.tr) {
    info.textContent = `${E.tr.words.length.toLocaleString()} words${E.tr.language ? " · " + E.tr.language : ""}${E.tr.model ? " · " + E.tr.model : ""}`;
    if (E.trKey === "done") return;
    E.trKey = "done";   // the words are built once per transcript
    const w = E.tr.words, h = [];
    E.tr.sents.forEach((s, si) => {
      h.push(`<p class="seged-sent" data-si="${si}"><a class="seged-ts mono" data-seek="${ft(s.s)}" title="${tc(ft(s.s))}">${clock(s.s)}</a>`);
      for (let i = s.a; i < s.b; i++) h.push(`<span class="w" data-w="${i}">${esc(w[i].t)}</span> `);
      h.push("</p>");
    });
    box.innerHTML = h.join("") || `<div class="seged-trmsg">The transcript has no speech in it.</div>`;
    E.wEls = Array.from(box.querySelectorAll(".w")); E.ranges = {}; E.cur = -1; E.curSent = -1;
    tintRanges(); setCur(true); runFind($(".seged-find").value); drawWsel();
    return;
  }
  info.textContent = "";
  const key = `${E.trState}|${Math.round(E.trPct)}|${E.trErr}|${E.levels ? E.levels.length : -1}|${E.level}`;
  if (key === E.trKey) return;
  E.trKey = key; E.wEls = null;
  if (E.trState === "queued" || E.trState === "running")
    box.innerHTML = `<div class="seged-trmsg row">Transcribing${E.trState === "queued" ? " (queued)" : ""}&hellip; <span class="seged-trbar"><i style="width:${Math.round(E.trPct)}%"></i></span><span class="mono">${Math.round(E.trPct)}%</span><span class="muted">the words appear here when it is done</span></div>`;
  else if (E.trState === "none" || E.trState === "failed") {
    const lv = E.levels || [];
    box.innerHTML = `<div class="seged-trmsg">${E.trState === "failed" ? `<div style="color:var(--danger);margin-bottom:6px">Transcription failed: ${esc(E.trErr || "unknown error")}</div>` : "No transcript yet. Transcribe this media to see its words here, find phrases and cut segments on the words."}
      <div class="row" style="margin-top:6px"><select class="seged-level small">${lv.map(l => `<option value="${esc(l.id)}"${l.id === E.level ? " selected" : ""}>${esc(l.id)} — ${esc(l.model)}</option>`).join("")}</select>
        <button class="small primary" data-a="transcribe">Transcribe</button><span class="muted" style="font-size:11px">${esc((lv.find(l => l.id === E.level) || {}).note || "")}</span></div></div>`;
  } else box.innerHTML = `<div class="seged-trmsg">loading transcript&hellip;</div>`;
}
// The word at the playhead (none in a pause); the panel follows it unless the user scrolled it in the last 3 s.
function setCur(force) {
  if (!E.tr || !E.wEls || !E.el) return;
  const w = E.tr.words, t = tf(E.frame);
  let i = lastLE(w, tf(E.frame + 1) - 1e-6, x => x.s);   // a word starting inside the frame on screen is the current one
  const si = i >= 0 ? w[i].si : 0;
  if (i >= 0 && t > w[i].e + 0.3) i = -1;
  const moved = i !== E.cur || si !== E.curSent;
  if (i !== E.cur) {
    if (E.cur >= 0 && E.wEls[E.cur]) E.wEls[E.cur].classList.remove("cur");
    if (i >= 0 && E.wEls[i]) E.wEls[i].classList.add("cur");
    const lane = $(".seged-words"), old = lane.querySelector(".seged-wd.cur"), now = i >= 0 ? lane.querySelector(`.seged-wd[data-w="${i}"]`) : null;
    if (old) old.classList.remove("cur"); if (now) now.classList.add("cur");
    E.cur = i;
  }
  E.curSent = si;
  if ((moved || force) && E.follow && performance.now() - E.userScrollT > 3000) reveal(i >= 0 ? E.wEls[i] : $(`.seged-sent[data-si="${si}"]`));
}
function reveal(elm) {
  const box = $(".seged-text"); if (!elm || !box.clientHeight) return;
  const top = elm.offsetTop, bot = top + elm.offsetHeight;
  if (top < box.scrollTop + 8 || bot > box.scrollTop + box.clientHeight - 8) { box.scrollTop = Math.max(0, top - box.clientHeight * 0.3); E.textScroll = box.scrollTop; }
}
// Tints: the words of the selected segment (its colour) and of the in/out range. Only the difference is touched.
function tintRanges() {
  if (!E.tr || !E.wEls || !E.el) return;
  const v = view(), g = E.sel >= 0 && E.sel < v.length ? v[E.sel] : null;
  if (g) $(".seged-text").style.setProperty("--segc", PALETTE[E.sel % PALETTE.length] + "40");
  const [a, b] = g ? wordRange(g.startTime, g.endTime) : [0, 0];
  setRange("inseg", a, b);
  const [c, d] = E.inF >= 0 && E.outF > E.inF ? wordRange(tf(E.inF), tf(E.outF)) : [0, 0];
  setRange("inio", c, d);
}
function setRange(cls, a, b) {
  const old = E.ranges[cls] || [0, 0], els = E.wEls; E.ranges[cls] = [a, b];
  for (let i = old[0]; i < old[1]; i++) if ((i < a || i >= b) && els[i]) els[i].classList.remove(cls);
  for (let i = a; i < b; i++) if ((i < old[0] || i >= old[1]) && els[i]) els[i].classList.add(cls);
}
// Word selection: in/out go in the silences around the words.
function selectWords(a, b) {
  if (!E.tr) return; if (a > b) [a, b] = [b, a];
  const w = E.tr.words; E.selA = a; E.selB = b;
  E.inF = inFrame(cutBefore(w, a)); E.outF = Math.max(E.inF + 1, outFrame(cutAfter(w, b)));
  drawLane(); drawWsel();
}
function clearWordSel() { if (E.selA < 0) return; E.selA = -1; E.selB = -1; drawWsel(); }
// The selection line keeps one height whatever it shows, so the words never move under a pointer that is selecting them.
function drawWsel() {
  if (!E.el) return; const box = $(".seged-wsel");
  if (E.selA < 0 || !E.tr || E.inF < 0 || E.outF <= E.inF) { box.innerHTML = `<span class="muted">Click a word to go there &middot; shift-click or drag across words for in/out &middot; double-click for the sentence &middot; Enter makes the segment</span>`; return; }
  const txt = E.tr.words.slice(E.selA, E.selB + 1).map(x => x.t).join(" ");
  box.innerHTML = `<span class="seged-wq">&ldquo;${esc(txt.length > 60 ? txt.slice(0, 58) + "…" : txt)}&rdquo;</span> <span class="mono muted">${tc(E.outF - E.inF)}</span>
    <button class="small primary" data-a="add" title="make a segment of these words (Enter)">+ Segment</button><button class="ghost small" data-a="clearSel" title="clear the selection and in/out">&#10005;</button>`;
}
function wordStep(d) {
  if (!E.tr || !E.tr.words.length) return; const w = E.tr.words;
  const i = d > 0 ? firstGE(w, tf(E.frame + 1) - 1e-9, x => x.s) : lastLE(w, tf(E.frame) - 1e-6, x => x.s);
  if (i >= 0 && i < w.length) setFrame(ft(w[i].s));
}
function sentStep(d) {
  if (!E.tr || !E.tr.sents.length) return; const s = E.tr.sents;
  const i = d > 0 ? firstGE(s, tf(E.frame + 1) - 1e-9, x => x.s) : lastLE(s, tf(E.frame) - 1e-6, x => x.s);
  if (i >= 0 && i < s.length) setFrame(ft(s[i].s));
}
// Find: matches on the words without punctuation; a hit is [first word, last word].
function runFind(q) {
  if (E.wEls) for (const [a, b] of E.hits) for (let i = a; i <= b; i++) if (E.wEls[i]) E.wEls[i].classList.remove("hit", "hitcur");
  E.hits = []; E.hit = -1;
  const n = norm(q || "").replace(/\s+/g, " ").trim();
  if (n && E.tr) {
    const T = E.tr.text, P = E.tr.pos; let p = T.indexOf(n);
    while (p >= 0 && E.hits.length < 5000) { E.hits.push([lastLE(P, p, x => x), lastLE(P, p + n.length - 1, x => x)]); p = T.indexOf(n, p + 1); }
  }
  markHits(); drawFindN();
}
function markHits() { if (!E.wEls) return; E.hits.forEach(([a, b], k) => { for (let i = a; i <= b; i++) if (E.wEls[i]) { E.wEls[i].classList.add("hit"); E.wEls[i].classList.toggle("hitcur", k === E.hit); } }); }
function drawFindN() { const n = $(".seged-findn"); n.textContent = E.hits.length ? `${E.hit >= 0 ? E.hit + 1 : "–"}/${E.hits.length}` : ($(".seged-find").value.trim() ? "0" : ""); }
function gotoHit(d) {
  if (!E.tr) return;
  if (!E.hits.length) { if ($(".seged-find").value.trim()) window.pp.toast("Not in the transcript"); return; }
  const w = E.tr.words;
  if (E.hit < 0) { const t = tf(E.frame), k = E.hits.findIndex(h => w[h[0]].s > t + 1e-3); E.hit = d > 0 ? (k < 0 ? 0 : k) : ((k < 0 ? E.hits.length : k) - 1 + E.hits.length) % E.hits.length; }
  else E.hit = (E.hit + d + E.hits.length) % E.hits.length;
  markHits(); drawFindN();
  const a = E.hits[E.hit][0]; setSpeed(0); E.anchor = a; E.userScrollT = 0; setFrame(ft(w[a].s)); reveal(E.wEls[a]);
}

// ---------------------------------------------------------------- frames
function frameUrl(f) { const w = Math.min(1280, Math.max(320, Math.ceil(($(".seged-prev").clientWidth || 480) * (window.devicePixelRatio || 1) / 160) * 160)); return `/frames/${encodeURIComponent(E.mediaId)}.jpg?frame=${f}&w=${w}`; }
function request() {
  if (!E.el || !E.mediaId || E.loading || E.frame === E.shown) { drawPlaceholder(); return; }
  const imgs = [$(".seged-a"), $(".seged-b")], back = imgs[1 - E.front], url = frameUrl(E.frame);
  if (back.getAttribute("src") === url && back.complete && back.naturalWidth) { swap(E.frame); return; }
  E.loading = true; E.requested = E.frame; drawPlaceholder();
  back.onload = () => { E.loading = false; swap(E.requested); request(); };
  back.onerror = () => { E.loading = false; E.shown = -1; };
  back.src = url;
}
function swap(f) { const imgs = [$(".seged-a"), $(".seged-b")]; E.front = 1 - E.front; imgs[E.front].style.display = ""; imgs[1 - E.front].style.display = "none"; E.shown = f; drawPlaceholder(); }
function setFrame(f, scroll = true) { E.frame = clampF(f); drawHead(); drawInfo(); if (scroll) ensureVisible(); request(); setCur(); }
function ensureVisible() { const sc = $(".seged-scroll"), x = tf(E.frame) * E.pps; if (x < sc.scrollLeft + 20 || x > sc.scrollLeft + sc.clientWidth - 20) { sc.scrollLeft = Math.max(0, x - sc.clientWidth * 0.3); drawStrip(); } }
function setSpeed(s) {
  E.speed = s; E.playT0 = performance.now(); E.playF0 = E.frame; drawInfo();
  clearInterval(E.timer); E.timer = 0;
  if (s) E.timer = setInterval(() => {
    if (!E.el || !E.el.isConnected) return;
    let f = E.playF0 + Math.round((performance.now() - E.playT0) / 1000 * fps() * E.speed);
    if (f >= E.frames - 1) { f = E.frames - 1; setSpeed(0); } else if (f <= 0) { f = 0; setSpeed(0); }
    setFrame(f);
  }, 40);
}
function stop() { clearInterval(E.timer); E.timer = 0; E.speed = 0; }

// ---------------------------------------------------------------- edits
function edges() { const e = [0, E.frames - 1]; for (const g of view()) e.push(ft(g.startTime), ft(g.endTime)); if (E.inF >= 0) e.push(E.inF); if (E.outF >= 0) e.push(E.outF); return e.sort((a, b) => a - b); }
function nextEdge(d) { const e = edges(); if (d > 0) return e.find(x => x > E.frame) ?? E.frames - 1; return [...e].reverse().find(x => x < E.frame) ?? 0; }
const copy = () => JSON.parse(JSON.stringify(E.segs || []));
function commit(a) { E.work = null; E.segs = a; drawLane(); if (E.onEdit) E.onEdit(JSON.parse(JSON.stringify(a))); }
function addSeg() {
  let a = E.inF >= 0 ? E.inF : E.frame, b = E.outF >= 0 ? E.outF : E.frame; if (b < a) [a, b] = [b, a];
  if (b - a < 1) { window.pp.toast(E.tr ? "Set in (I) and out (O), or shift-click words in the transcript" : "Set an in point (I) and an out point (O) first"); return; }
  const said = textBetween(tf(a), tf(b)), s = copy();
  const g = { label: said ? labelFrom(said) : "Segment " + (s.length + 1), startTime: tf(a), endTime: tf(b) };
  if (said) g.text = said; if (E.selA >= 0) g.source = "transcript";
  s.push(g); s.sort((x, y) => x.startTime - y.startTime);
  E.sel = s.findIndex(x => Math.abs(x.startTime - tf(a)) < 1e-6 && Math.abs(x.endTime - tf(b)) < 1e-6); E.inF = -1; E.outF = -1; E.selA = -1; E.selB = -1; drawWsel(); commit(s);
}
function setEdge(which) { if (E.sel < 0) return; const s = copy(), g = s[E.sel]; if (!g) return; if (which === "start") g.startTime = Math.min(tf(E.frame), g.endTime - tf(1)); else g.endTime = Math.max(tf(E.frame), g.startTime + tf(1)); commit(s); }
function delSel() { if (E.sel < 0) return; const s = copy(); s.splice(E.sel, 1); E.sel = Math.min(E.sel, s.length - 1); commit(s); }
function labelWords() { const s = copy(), g = s[E.sel]; if (!g) return; const said = textBetween(g.startTime, g.endTime); if (!said) { window.pp.toast("No words in this segment"); return; } g.label = labelFrom(said); commit(s); }
function fit() { if (E.el && E.duration > 0) { E.pps = Math.max(0.01, W() / E.duration); drawContent(); } }
function zoomBy(k, ax) {
  const sc = $(".seged-scroll"); ax = ax ?? sc.clientWidth / 2;
  const tAt = (sc.scrollLeft + ax) / E.pps; E.pps = Math.max(W() / Math.max(1, E.duration), Math.min(fps() * 14, E.pps * k));
  drawContent(); sc.scrollLeft = Math.max(0, tAt * E.pps - ax); drawStrip(); drawLane(); drawHead();
}
function syncZoomSlider() { const s = $(".seged-zoom"), minP = W() / Math.max(1, E.duration), maxP = fps() * 14; if (document.activeElement !== s && maxP > minP) s.value = String(Math.round(1000 * Math.log(E.pps / minP) / Math.log(maxP / minP))); }

// ---------------------------------------------------------------- input
function wire(el) {
  const act = {
    in: () => { E.inF = E.frame; clearWordSel(); drawLane(); }, out: () => { E.outF = E.frame; clearWordSel(); drawLane(); }, add: addSeg,
    prevEdge: () => { setSpeed(0); setFrame(nextEdge(-1)); }, nextEdge: () => { setSpeed(0); setFrame(nextEdge(1)); },
    back: () => { setSpeed(0); setFrame(E.frame - 1); }, fwd: () => { setSpeed(0); setFrame(E.frame + 1); },
    play: () => setSpeed(E.speed ? 0 : 1), J: () => setSpeed(E.speed >= 0 ? -1 : Math.max(-4, E.speed * 2)), K: () => setSpeed(0), L: () => setSpeed(E.speed <= 0 ? 1 : Math.min(4, E.speed * 2)),
    fit: () => { fit(); $(".seged-scroll").scrollLeft = 0; drawStrip(); drawLane(); drawHead(); },
    setStart: () => setEdge("start"), setEnd: () => setEdge("end"), del: delSel, labelWords,
    playSel: () => { const g = view()[E.sel]; if (g) { setFrame(ft(g.startTime)); setSpeed(1); } },
    transcribe, findNext: () => gotoHit(1), findPrev: () => gotoHit(-1),
    clearSel: () => { E.selA = -1; E.selB = -1; E.inF = -1; E.outF = -1; drawWsel(); drawLane(); },
  };
  el.addEventListener("click", e => {
    const a = e.target.closest("[data-a]"); if (a) { e.preventDefault(); act[a.dataset.a] && act[a.dataset.a](); return; }
    const s = e.target.closest("[data-seek]"); if (s && +s.dataset.seek >= 0) { setSpeed(0); setFrame(+s.dataset.seek); }
  });
  el.querySelector(".seged-prev").addEventListener("click", () => setSpeed(E.speed ? 0 : 1));
  el.addEventListener("change", e => {
    const c = e.target.classList;
    if (c.contains("seged-label") && E.sel >= 0) { E.focus = null; const s = copy(); if (s[E.sel] && s[E.sel].label !== e.target.value) { s[E.sel].label = e.target.value; commit(s); } }
    else if (c.contains("seged-follow")) { E.follow = e.target.checked; if (E.follow) { E.userScrollT = 0; setCur(true); } }
    else if (c.contains("seged-snap")) E.snap = e.target.checked;
    else if (c.contains("seged-level")) { E.level = e.target.value; drawTr(); }
  });
  const go = el.querySelector(".seged-go");
  go.addEventListener("keydown", e => {
    if (e.key !== "Enter") return; const v = go.value.trim(); let f = -1;
    const m = v.match(/^(\d+):(\d+):(\d+)[:;.](\d+)$/);
    if (m) { const nominal = ((+m[1] * 60 + +m[2]) * 60 + +m[3]) * Math.round(fps()) + +m[4]; f = nominal; for (let k = 0; k < 400 && tc(f) !== v.replace(".", ":"); k++) f = nominal - k; }
    else if (!isNaN(parseFloat(v))) f = ft(parseFloat(v));
    if (f >= 0) setFrame(f); go.value = ""; el.focus(); e.preventDefault();
  });
  const find = el.querySelector(".seged-find");
  find.addEventListener("input", () => runFind(find.value));
  find.addEventListener("keydown", e => {
    if (e.key === "Enter") { e.preventDefault(); gotoHit(e.shiftKey ? -1 : 1); }
    else if (e.key === "Escape") { e.preventDefault(); find.value = ""; runFind(""); el.focus({ preventScroll: true }); }
  });
  const zoom = el.querySelector(".seged-zoom");
  zoom.addEventListener("input", () => { const minP = W() / Math.max(1, E.duration), maxP = fps() * 14; const sc = $(".seged-scroll"), tAt = (sc.scrollLeft + sc.clientWidth / 2) / E.pps; E.pps = minP * Math.pow(maxP / minP, +zoom.value / 1000); drawContent(); sc.scrollLeft = Math.max(0, tAt * E.pps - sc.clientWidth / 2); drawStrip(); drawLane(); drawHead(); });
  const sc = el.querySelector(".seged-scroll");
  sc.addEventListener("scroll", () => { if (E.el.isConnected) E.scrollLeft = sc.scrollLeft; drawStrip(); });
  // Remember what has focus so a re-render can give it back: a text input (with its caret), or the editor itself.
  // Focus moving to anything else in the editor forgets an input, so Escape / Enter in a box hands the keys back.
  const noteFocus = e => {
    const t = e.target; if (!t) return;
    if (t === E.el) { if (e.type === "focusin") E.focus = { cls: "", el: true }; return; }
    if (t.tagName !== "INPUT" || t.type === "range" || t.type === "checkbox") { if (e.type === "focusin") E.focus = null; return; }
    E.focus = { cls: t.className.split(" ")[0], s: t.selectionStart, e: t.selectionEnd, v: t.value, sel: E.sel };
  };
  el.addEventListener("focusin", noteFocus); el.addEventListener("input", noteFocus); el.addEventListener("keyup", noteFocus); el.addEventListener("click", noteFocus);
  // a real blur (focus moved elsewhere while we are in the page) forgets it; a re-render detaches us without a blur
  el.addEventListener("focusout", () => setTimeout(() => { if (E.el.isConnected && !E.el.contains(document.activeElement)) E.focus = null; }, 0));
  sc.addEventListener("wheel", e => {
    if (e.ctrlKey) { e.preventDefault(); zoomBy(e.deltaY < 0 ? 1.25 : 0.8, e.clientX - sc.getBoundingClientRect().left); }
    else if (Math.abs(e.deltaY) > Math.abs(e.deltaX)) { e.preventDefault(); sc.scrollLeft += e.deltaY; }
  }, { passive: false });
  // scrub on the ruler / strip; drag segment edges; select segments; click speech blocks
  const content = el.querySelector(".seged-content");
  const tAtX = clientX => Math.max(0, (clientX - content.getBoundingClientRect().left) / E.pps);
  content.addEventListener("pointerdown", e => {
    el.focus(); E.armed = true;
    const h = e.target.closest(".seged-h");
    if (h) {
      e.preventDefault(); const i = +h.dataset.i, which = h.dataset.e; E.sel = i; E.work = copy();
      const move = ev => {
        const t = tAtX(ev.clientX), snap = E.snap && E.tr && E.tr.cuts.length && !ev.altKey;
        const f = snap ? (which === "start" ? inFrame(snapT(t)) : outFrame(snapT(t))) : clampF(ft(t));
        const a = E.work.slice(), g = { ...a[i] };
        if (which === "start") g.startTime = Math.min(tf(f), g.endTime - tf(1)); else g.endTime = Math.max(tf(f), g.startTime + tf(1));
        a[i] = g; E.work = a; drawLane(); setFrame(f, false);
      };
      const up = () => { window.removeEventListener("pointermove", move); window.removeEventListener("pointerup", up); if (E.work) commit(E.work); };
      window.addEventListener("pointermove", move); window.addEventListener("pointerup", up); return;
    }
    const sp = e.target.closest(".seged-wd, .seged-st");
    if (sp && E.tr) {
      setSpeed(0);
      if (sp.dataset.w !== undefined) { const gi = +sp.dataset.w; if (e.shiftKey && E.anchor >= 0) selectWords(E.anchor, gi); else { E.anchor = gi; setFrame(ft(E.tr.words[gi].s), false); } }
      else { const s = E.tr.sents[+sp.dataset.si]; if (e.shiftKey) selectWords(s.a, s.b - 1); else { E.anchor = s.a; setFrame(ft(s.s), false); } }
      return;
    }
    const seg = e.target.closest(".seged-seg");
    if (seg) { E.sel = +seg.dataset.i; drawLane(); if (e.detail === 2) setFrame(ft(view()[E.sel].startTime)); return; }
    setSpeed(0); setFrame(ft(tAtX(e.clientX)), false);
    const move = ev => setFrame(ft(tAtX(ev.clientX)), false);
    const up = () => { window.removeEventListener("pointermove", move); window.removeEventListener("pointerup", up); };
    window.addEventListener("pointermove", move); window.addEventListener("pointerup", up);
  });
  const hover = el.querySelector(".seged-hover");
  content.addEventListener("pointermove", e => {
    if (!E.sb || e.target.closest(".seged-lane, .seged-words")) { hover.style.display = "none"; return; }
    const t = Math.min(E.duration, tAtX(e.clientX)), tlr = el.querySelector(".seged-tl").getBoundingClientRect();
    hover.style.display = "block";
    hover.style.left = Math.max(0, Math.min(tlr.width - 176, e.clientX - tlr.left - 88)) + "px";
    const hi = hover.querySelector(".seged-hover-img"); hi.style.width = "160px"; hi.style.height = (160 * E.sb.tileH / E.sb.tileW) + "px"; setSprite(hi, tileIndexAt(t), 160);
    hover.querySelector(".seged-hover-tc").textContent = tc(ft(t));
  });
  content.addEventListener("pointerleave", () => { hover.style.display = "none"; });
  // transcript panel: click a word to go there; shift-click / drag across words for in/out; double-click a sentence
  const text = el.querySelector(".seged-text"), lastDown = { gi: -1, t: 0 };
  text.addEventListener("pointerdown", e => {
    const sp = e.target.closest(".w");
    if (!sp || !E.tr) { if (e.target === text) E.userScrollT = performance.now(); return; }
    e.preventDefault(); el.focus({ preventScroll: true }); E.armed = true; setSpeed(0);
    const gi = +sp.dataset.w, now = performance.now();
    E.userScrollT = now;   // the words must not move under the pointer while it clicks or drags (follow waits)
    if (e.shiftKey) { selectWords(E.anchor >= 0 ? E.anchor : (E.cur >= 0 ? E.cur : gi), gi); return; }
    if (lastDown.gi === gi && now - lastDown.t < 400) { const s = E.tr.sents[E.tr.words[gi].si]; lastDown.gi = -1; selectWords(s.a, s.b - 1); return; }   // double click: the sentence
    lastDown.gi = gi; lastDown.t = now;
    E.anchor = gi; setFrame(ft(E.tr.words[gi].s));
    const move = ev => { E.userScrollT = performance.now(); const o = document.elementFromPoint(ev.clientX, ev.clientY), w2 = o && o.closest ? o.closest(".seged-text .w") : null; if (w2 && +w2.dataset.w !== gi) selectWords(gi, +w2.dataset.w); };
    const up = ev => { move(ev); window.removeEventListener("pointermove", move); window.removeEventListener("pointerup", up); };   // the release point counts too
    window.addEventListener("pointermove", move); window.addEventListener("pointerup", up);
  });
  text.addEventListener("wheel", () => { E.userScrollT = performance.now(); }, { passive: true });
  text.addEventListener("scroll", () => { if (E.el.isConnected) E.textScroll = text.scrollTop; });
}
document.addEventListener("pointerdown", e => { if (E.el && !E.el.contains(e.target)) E.armed = false; else if (E.el) E.armed = true; }, true);
document.addEventListener("keydown", e => {
  if (!E.el || !E.el.isConnected || !E.armed) return;
  const t = e.target; if (t && (t.tagName === "INPUT" || t.tagName === "TEXTAREA" || t.tagName === "SELECT")) return;
  const step = e.shiftKey ? Math.round(fps()) : e.ctrlKey ? Math.round(fps() * 10) : 1;
  const k = e.key; let h = true;
  if (k === "ArrowRight") { setSpeed(0); setFrame(E.frame + step); }
  else if (k === "ArrowLeft") { setSpeed(0); setFrame(E.frame - step); }
  else if (k === "ArrowUp") { setSpeed(0); setFrame(nextEdge(-1)); }
  else if (k === "ArrowDown") { setSpeed(0); setFrame(nextEdge(1)); }
  else if (k === "Home") { setSpeed(0); setFrame(0); }
  else if (k === "End") { setSpeed(0); setFrame(E.frames - 1); }
  else if (k === " ") setSpeed(E.speed ? 0 : 1);
  else if (k === "k" || k === "K") setSpeed(0);
  else if (k === "l" || k === "L") setSpeed(E.speed <= 0 ? 1 : Math.min(4, E.speed * 2));
  else if (k === "j" || k === "J") setSpeed(E.speed >= 0 ? -1 : Math.max(-4, E.speed * 2));
  else if (k === "i" || k === "I") { E.inF = E.frame; clearWordSel(); drawLane(); }
  else if (k === "o" || k === "O") { E.outF = E.frame; clearWordSel(); drawLane(); }
  else if (k === ",") { setSpeed(0); wordStep(-1); }
  else if (k === ".") { setSpeed(0); wordStep(1); }
  else if (k === "<") { setSpeed(0); sentStep(-1); }
  else if (k === ">") { setSpeed(0); sentStep(1); }
  else if (k === "Enter") addSeg();
  else if (k === "[") setEdge("start");
  else if (k === "]") setEdge("end");
  else if (k === "Delete" || k === "Backspace") delSel();
  else if (k === "+" || k === "=") zoomBy(1.5);
  else if (k === "-") zoomBy(1 / 1.5);
  else h = false;
  if (h) e.preventDefault();
});
// The dashboard rebuilds #view on every render: put the editor (same element, same state) back into the new placeholder.
new MutationObserver(() => {
  const host = document.getElementById("segEditorHost");
  if (!host) { if (E.speed) setSpeed(0); return; }
  if (E.el && E.el.parentNode !== host) {
    host.appendChild(E.el);
    const sc = E.el.querySelector(".seged-scroll");
    if (!E.duration || E.pps * E.duration < W() - 1) fit();
    drawContent(); sc.scrollLeft = E.scrollLeft; drawAll();
    E.el.querySelector(".seged-text").scrollTop = E.textScroll;
    if (E.focus && E.focus.el) E.el.focus({ preventScroll: true });
    else if (E.focus) { const i = E.el.querySelector("." + E.focus.cls); if (i) { if (i.value !== E.focus.v && i.classList.contains("seged-go")) i.value = E.focus.v; i.focus({ preventScroll: true }); try { i.setSelectionRange(E.focus.s, E.focus.e); } catch {} } }
    if ((E.trState === "queued" || E.trState === "running") && !E.trTimer) trPoll();
    request();
  }
}).observe(document.body, { childList: true, subtree: true });
