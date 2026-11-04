import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import ".."

/**
 * SegmentTimeline: frame-accurate segment editor for one media item, working on its transcript.
 *
 *  - Preview: the exact frame at the playhead (Supervisor /frames/<id>.jpg?frame=n, double buffered so it never
 *    flashes); while a frame is on its way the storyboard tile for that spot is shown, YouTube style.
 *  - Timeline: ruler, storyboard filmstrip, segment lane and, with a transcript, a speech lane (sentences, and words
 *    from 40 px/s), zoomable (Ctrl+wheel, slider, +/-) with horizontal scroll; hovering shows a thumbnail and
 *    timecode. Click or drag to scrub. Segment edges drag and snap to frames, or to the silence between words while
 *    "snap to words" is on (hold Alt for frames only).
 *  - Transcript panel (right of the details when the editor is 1100 px or wider, else under the timeline): the words,
 *    the one at the playhead lit and followed. Click a word to go there; shift-click or drag across words, or
 *    double-click a sentence, to put in/out in the silences around them; Enter makes the segment, labelled with its
 *    first words. Find box: Enter next match, Shift+Enter previous. Without a transcript it offers to transcribe.
 *  - Keys: Left/Right one frame (Shift one second, Ctrl ten seconds), Up/Down previous/next edge, , . previous/next
 *    word, < > previous/next sentence, Home/End, Space play/pause, J/K/L shuttle (press again for 2x / 4x), I/O in/out,
 *    Enter add a segment from in to out, [ / ] set the selected segment's start/end to the playhead, Delete remove it,
 *    +/- zoom.
 * Cuts around words follow playout/supervisor/transcripttools.cpp (half the gap to the neighbour, at most 0.25 s; a
 * word belongs to the range that holds its midpoint), like web/segments.js. Edits are handed to the owner with
 * segmentsEdited(newArray); the owner saves them (the Supervisor then stores each segment's spoken text).
 */
FocusScope {
    id: tl
    property string mediaId: ""
    property real duration: 0
    property var segments: []
    signal segmentsEdited(var segs)
    implicitHeight: col.implicitHeight
    readonly property bool wide: width >= 1100

    // ---- frame geometry (from /media/{id}/frameinfo) ----
    property int fpsNum: 25
    property int fpsDen: 1
    readonly property real fps: fpsDen > 0 && fpsNum > 0 ? fpsNum / fpsDen : 25
    property real infoDuration: 0
    readonly property real dur: infoDuration > 0 ? infoDuration : duration
    property real frameCount: 0
    readonly property int frames: Math.max(1, frameCount > 0 ? frameCount : Math.round(dur * fps))
    property bool dropFrame: false
    property var sb: null                       // storyboard layout
    property string spriteUrl: ""
    property string infoError: ""

    // ---- editor state ----
    property int frame: 0
    property int inFrame: -1
    property int outFrame: -1
    property int sel: -1
    property real speed: 0
    property real pps: 10                       // timeline pixels per second
    property var work: null                     // working copy while a handle is dragged
    readonly property var view: work !== null ? work : (segments || [])
    readonly property var palette: ["#00d4ff", "#ff9f1c", "#7bd389", "#e86fd8", "#ffd23f", "#5b8cff", "#ff5d73", "#41ead4"]

    // ---- transcript (GET /media/{id}/transcript): words [{t, s, e, si}], sentences [{s, e, a, b, txt}] ----
    property var trWords: []
    property var trSents: []
    property var trCuts: []                     // every cut next to a word, sorted: the snap targets
    property string trText: ""                  // the words without punctuation, for find
    property var trPos: []                      // where each word starts in trText
    property string trLang: ""
    property string trModel: ""
    property string trState: ""                 // loading, none, queued, running, failed, done
    property real trPct: 0
    property string trErr: ""
    property var trLevels: []
    property string trLevel: ""
    readonly property bool hasWords: trWords.length > 0
    property int cur: -1                        // the word at the playhead (-1 in a pause)
    property int curSent: -1
    property int anchor: -1                     // the word a shift-click extends from
    property int selA: -1                       // word selection that set in/out
    property int selB: -1
    property bool follow: true
    property bool snap: true
    property real userScrollT: 0
    property var hits: []                       // find: [first word, last word] per match
    property int hit: -1
    property var hitMask: null                  // word index -> 1 match, 2 current match
    property int hitRev: 0
    readonly property var segRange: { var g = sel >= 0 && sel < view.length ? view[sel] : null; return g && hasWords ? wordRange(g.startTime, g.endTime) : [0, 0] }
    readonly property var ioRange: inFrame >= 0 && outFrame > inFrame && hasWords ? wordRange(tf(inFrame), tf(outFrame)) : [0, 0]
    readonly property string segTint: sel >= 0 ? "#40" + palette[sel % palette.length].substring(1) : "transparent"
    readonly property string selText: { var g = sel >= 0 && sel < view.length ? view[sel] : null; if (!g) return ""; return hasWords ? textBetween(g.startTime, g.endTime) : (g.text || "") }

    function tf(f) { return f * fpsDen / fpsNum }
    function ft(t) { return Math.floor(t * fps + 1e-6) }
    function clampF(f) { return Math.max(0, Math.min(frames - 1, Math.round(f))) }
    function inFrameOf(t) { return clampF(Math.floor(t * fps + 1e-6)) }     // an in point never clips the word after it
    function outFrameOf(t) { return clampF(Math.ceil(t * fps - 1e-6)) }     // an out point never clips the word before it
    function pad(n, w) { var s = String(Math.floor(Math.abs(n))); while (s.length < (w || 2)) s = "0" + s; return s }
    /// SMPTE timecode; drop-frame (;) at 29.97 / 59.94.
    function tc(f) {
        var base = Math.round(fps), drop = fpsDen === 1001 && (fpsNum === 30000 || fpsNum === 60000)
        f = Math.max(0, Math.round(f))
        if (drop) {
            var dn = base === 60 ? 4 : 2, per10 = base * 600 - 9 * dn, perMin = base * 60 - dn
            var d = Math.floor(f / per10), m = f % per10
            f += 9 * dn * d + (m > dn ? dn * Math.floor((m - dn) / perMin) : 0)
        }
        var ff = f % base, s = Math.floor(f / base)
        return pad(Math.floor(s / 3600)) + ":" + pad(Math.floor(s / 60) % 60) + ":" + pad(s % 60) + (drop ? ";" : ":") + pad(ff)
    }
    function clock(t) { t = Math.max(0, Math.floor(t)); var h = Math.floor(t / 3600), m = Math.floor(t / 60) % 60; return (h ? h + ":" + pad(m) : String(m)) + ":" + pad(t % 60) }
    function seek(f) { frame = clampF(f); ensureVisible() }
    function setSpeed(s) { speed = s; player.startT = Date.now(); player.startF = frame }
    function edges() {
        var e = [0, frames - 1]
        for (var i = 0; i < view.length; ++i) { e.push(ft(view[i].startTime)); e.push(ft(view[i].endTime)) }
        if (inFrame >= 0) e.push(inFrame); if (outFrame >= 0) e.push(outFrame)
        e.sort(function(a, b) { return a - b }); return e
    }
    function nextEdge(dir) {
        var e = edges()
        if (dir > 0) { for (var i = 0; i < e.length; ++i) if (e[i] > frame) return e[i]; return frames - 1 }
        for (var j = e.length - 1; j >= 0; --j) if (e[j] < frame) return e[j]; return 0
    }
    function copySegs() { return JSON.parse(JSON.stringify(segments || [])) }
    function commit(a) { work = null; segmentsEdited(a) }
    function addSegment() {
        var a = inFrame >= 0 ? inFrame : frame, b = outFrame >= 0 ? outFrame : frame
        if (b < a) { var t = a; a = b; b = t }
        if (b - a < 1) { window.toast(hasWords ? "Set in (I) and out (O), or shift-click words in the transcript" : "Set an in point (I) and an out point (O) first"); return }
        var said = textBetween(tf(a), tf(b)), s = copySegs()
        var g = { label: said.length ? labelFrom(said) : "Segment " + (s.length + 1), startTime: tf(a), endTime: tf(b) }
        if (said.length) g.text = said
        if (selA >= 0) g.source = "transcript"
        s.push(g)
        s.sort(function(x, y) { return x.startTime - y.startTime })
        for (var i = 0; i < s.length; ++i) if (Math.abs(s[i].startTime - tf(a)) < 1e-6 && Math.abs(s[i].endTime - tf(b)) < 1e-6) sel = i
        inFrame = -1; outFrame = -1; selA = -1; selB = -1
        commit(s)
    }
    function setEdge(which) {
        if (sel < 0 || sel >= view.length) return
        var s = copySegs(); var g = s[sel]
        if (which === "start") g.startTime = Math.min(tf(frame), g.endTime - tf(1)); else g.endTime = Math.max(tf(frame), g.startTime + tf(1))
        commit(s)
    }
    function removeSel() { if (sel < 0 || sel >= view.length) return; var s = copySegs(); s.splice(sel, 1); sel = Math.min(sel, s.length - 1); commit(s) }
    function labelFromWords() {
        if (sel < 0 || sel >= view.length) return
        var s = copySegs(), g = s[sel], said = textBetween(g.startTime, g.endTime)
        if (!said.length) { window.toast("No words in this segment"); return }
        g.label = labelFrom(said); commit(s)
    }
    function fitZoom() { if (dur > 0 && flick.width > 0) pps = Math.max(0.01, flick.width / dur) }
    function zoomBy(k, anchorX) {
        var ax = anchorX === undefined ? flick.width / 2 : anchorX
        var tAt = (flick.contentX + ax) / pps
        pps = Math.max(flick.width / Math.max(1, dur), Math.min(fps * 14, pps * k))
        flick.contentX = Math.max(0, Math.min(flick.contentWidth - flick.width, tAt * pps - ax))
    }
    function ensureVisible() {
        var x = tf(frame) * pps
        if (x < flick.contentX + 20 || x > flick.contentX + flick.width - 20) flick.contentX = Math.max(0, Math.min(flick.contentWidth - flick.width, x - flick.width * 0.3))
    }
    function tileIndexAt(t) { if (!sb) return 0; return Math.max(0, Math.min(sb.n - 1, Math.floor(t / sb.interval))) }

    // ---- transcript helpers (same rules as transcripttools.cpp / web/segments.js) ----
    readonly property real cutPad: 0.25
    function firstGE(a, x, key) { var lo = 0, hi = a.length; while (lo < hi) { var m = (lo + hi) >> 1; if (key(a[m]) < x) lo = m + 1; else hi = m } return lo }
    function lastLE(a, x, key) { return firstGE(a, x + 1e-9, key) - 1 }
    function startOf(x) { return x.s }
    function midOf(x) { return (x.s + x.e) / 2 }
    function idOf(x) { return x }
    function norm(s) { return String(s).toLowerCase().replace(/[.,!?;:"()\[\]{}…—–“”«»¿¡-]+/g, "") }
    function cutBefore(w, i) { var gap = i > 0 ? w[i].s - w[i - 1].e : 2 * cutPad; return Math.max(0, w[i].s - Math.min(cutPad, Math.max(0, gap / 2))) }
    function cutAfter(w, i) { var gap = i + 1 < w.length ? w[i + 1].s - w[i].e : 2 * cutPad; return w[i].e + Math.min(cutPad, Math.max(0, gap / 2)) }
    function wordRange(a, b) { if (!trWords.length) return [0, 0]; return [firstGE(trWords, a, midOf), firstGE(trWords, b, midOf)] }
    function wordsText(a, b) { var out = []; for (var i = Math.max(0, a); i <= b && i < trWords.length; ++i) out.push(trWords[i].t); return out.join(" ") }
    function textBetween(a, b) { var r = wordRange(a, b); return r[1] > r[0] ? wordsText(r[0], r[1] - 1) : "" }
    function labelFrom(text) { var s = "", p = text.split(" "); for (var i = 0; i < p.length; ++i) { var n = s.length ? s + " " + p[i] : p[i]; if (n.length > 40) break; s = n } return s.length ? s : text.substring(0, 40) }
    function snapT(t) {
        var c = trCuts, i = firstGE(c, t, idOf), best = t, bd = 10 / pps     // within 10 px
        for (var j = i - 1; j <= i; ++j) if (j >= 0 && j < c.length && Math.abs(c[j] - t) <= bd) { bd = Math.abs(c[j] - t); best = c[j] }
        return best
    }
    function buildTr(t) {
        var words = [], sents = [], segs = t.segments || []
        for (var i = 0; i < segs.length; ++i) {
            var s = segs[i], a = words.length, si = sents.length, ws = s.words || []
            for (var j = 0; j < ws.length; ++j) { var x = String(ws[j].word || "").trim(); if (x.length) { var st = Number(ws[j].start) || 0; words.push({ t: x, s: st, e: Math.max(st, Number(ws[j].end) || 0), si: si }) } }
            if (words.length === a && String(s.text || "").trim().length) { var ss = Number(s.start) || 0; words.push({ t: String(s.text).trim(), s: ss, e: Math.max(ss, Number(s.end) || 0), si: si }) }   // no word timings: the sentence is one "word"
            if (words.length > a) { var parts = []; for (var g = a; g < words.length; ++g) parts.push(words[g].t); sents.push({ s: words[a].s, e: words[words.length - 1].e, a: a, b: words.length, txt: parts.join(" ") }) }
        }
        var cuts = []; for (var k = 0; k < words.length; ++k) { cuts.push(cutBefore(words, k)); cuts.push(cutAfter(words, k)) }
        cuts.sort(function(p, q) { return p - q })
        var text = "", pos = []
        for (var n = 0; n < words.length; ++n) { pos.push(text.length); text += norm(words[n].t) + " " }
        trCuts = cuts; trText = text; trPos = pos; trLang = t.language || ""; trModel = t.model || ""
        cur = -1; curSent = -1; anchor = -1; selA = -1; selB = -1; hits = []; hit = -1; hitMask = null; hitRev++
        trSents = sents; trWords = words; trState = "done"
        if (findField.text.length) runFind(findField.text)
        setCur(true)
    }
    function trLoad() {
        var id = mediaId
        trWords = []; trSents = []; trCuts = []; trText = ""; trPos = []; trErr = ""; trPct = 0
        cur = -1; curSent = -1; anchor = -1; selA = -1; selB = -1; hits = []; hit = -1; hitMask = null; hitRev++
        trState = id.length ? "loading" : ""
        if (!id.length) return
        api.get("/media/" + id, function(ok, m) {
            if (id !== tl.mediaId) return
            var st = ok && m.transcript ? (m.transcript.status || "") : ""
            tl.trErr = ok && m.transcript ? (m.transcript.error || "") : ""
            if (st === "done") api.get("/media/" + id + "/transcript", function(ok2, t) {
                if (id !== tl.mediaId) return
                if (ok2) tl.buildTr(t); else { tl.trState = "failed"; tl.trErr = "the transcript file could not be read"; tl.trLevelsLoad() }
            })
            else if (st === "queued" || st === "running") { tl.trState = st; trPoll.restart() }
            else { tl.trState = st === "failed" || st === "cancelled" ? "failed" : "none"; tl.trLevelsLoad() }
        })
    }
    /// The owner calls this when a transcription of this media finished (supervisor transcriptReady).
    function reloadTranscript() { if (mediaId.length) trLoad() }
    function trLevelsLoad() {
        if (trLevels.length) return
        api.get("/transcribe/levels", function(ok, d) { if (!ok) return; tl.trLevels = d.levels || []; if (!tl.trLevel.length) tl.trLevel = (d.config || {}).defaultLevel || "balanced" })
    }
    function levelNote() { for (var i = 0; i < trLevels.length; ++i) if (trLevels[i].id === trLevel) return trLevels[i].model + " — " + trLevels[i].note; return "" }
    function transcribe() {
        var id = mediaId, lv = trLevel.length ? trLevel : "balanced"
        api.post("/media/" + id + "/transcribe", { level: lv }, function(ok, j) {
            if (id !== tl.mediaId) return
            if (!ok || (j && j.error)) { tl.trState = "failed"; tl.trErr = j && j.error ? j.error : "the job could not be queued"; return }
            window.toast("Transcription queued (" + lv + ")", "ok")
            tl.trState = (j && j.state) || "queued"; tl.trPct = 0; tl.trErr = ""; trPoll.restart()
        })
    }
    Timer {
        id: trPoll
        interval: 2500; repeat: false
        onTriggered: {
            if (!(tl.trState === "queued" || tl.trState === "running") || !tl.mediaId.length) return
            var id = tl.mediaId
            api.get("/transcribe/jobs", function(ok, d) {
                if (id !== tl.mediaId) return
                if (!ok) { trPoll.restart(); return }
                var js = (d.jobs || []).filter(function(j) { return j.mediaId === id }), j = js[js.length - 1]
                if (!j || j.state === "done") { tl.trLoad(); return }      // finished, or the job list was cleared: the media record knows
                if (j.state === "failed" || j.state === "cancelled") { tl.trState = "failed"; tl.trErr = j.error || j.state; tl.trLevelsLoad(); return }
                tl.trState = j.state; tl.trPct = j.pct || 0; trPoll.restart()
            })
        }
    }
    // The word at the playhead: a word starting inside the frame on screen counts; none in a pause. The panel follows
    // it unless the user scrolled or clicked the panel in the last 3 s.
    function setCur(force) {
        if (!hasWords) { cur = -1; return }
        var w = trWords, t = tf(frame)
        var i = lastLE(w, tf(frame + 1) - 1e-6, startOf)
        var si = i >= 0 ? w[i].si : 0
        if (i >= 0 && t > w[i].e + 0.3) i = -1
        var moved = i !== cur || si !== curSent
        cur = i; curSent = si
        if ((moved || force) && follow && Date.now() - userScrollT > 3000) revealSent(si)
    }
    function revealSent(si) { if (si >= 0 && si < trSents.length && sentList.visible && sentList.height > 0) sentList.positionViewAtIndex(si, ListView.Contain) }
    function selectWords(a, b) {
        if (!hasWords) return
        if (a > b) { var t = a; a = b; b = t }
        selA = a; selB = b
        inFrame = inFrameOf(cutBefore(trWords, a)); outFrame = Math.max(inFrame + 1, outFrameOf(cutAfter(trWords, b)))
    }
    function clearWordSel() { selA = -1; selB = -1 }
    function wordStep(d) {
        if (!hasWords) return
        var w = trWords, i = d > 0 ? firstGE(w, tf(frame + 1) - 1e-9, startOf) : lastLE(w, tf(frame) - 1e-6, startOf)
        if (i >= 0 && i < w.length) seek(ft(w[i].s))
    }
    function sentStep(d) {
        if (!trSents.length) return
        var s = trSents, i = d > 0 ? firstGE(s, tf(frame + 1) - 1e-9, startOf) : lastLE(s, tf(frame) - 1e-6, startOf)
        if (i >= 0 && i < s.length) seek(ft(s[i].s))
    }
    function runFind(q) {
        var hs = [], n = norm(q || "").replace(/\s+/g, " ").trim()
        if (n.length && trText.length) {
            var p = trText.indexOf(n)
            while (p >= 0 && hs.length < 5000) { hs.push([lastLE(trPos, p, idOf), lastLE(trPos, p + n.length - 1, idOf)]); p = trText.indexOf(n, p + 1) }
        }
        hits = hs; hit = -1; markHits()
    }
    function markHits() {
        var m = {}
        for (var k = 0; k < hits.length; ++k) for (var i = hits[k][0]; i <= hits[k][1]; ++i) m[i] = Math.max(m[i] || 0, k === hit ? 2 : 1)
        hitMask = m; hitRev++
    }
    function gotoHit(d) {
        if (!hits.length) { if (findField.text.trim().length) window.toast("Not in the transcript"); return }
        var w = trWords
        if (hit < 0) {
            var t = tf(frame), k = -1
            for (var i = 0; i < hits.length; ++i) if (w[hits[i][0]].s > t + 1e-3) { k = i; break }
            hit = d > 0 ? (k < 0 ? 0 : k) : ((k < 0 ? hits.length : k) - 1 + hits.length) % hits.length
        } else hit = (hit + d + hits.length) % hits.length
        markHits()
        var a = hits[hit][0]; setSpeed(0); anchor = a; userScrollT = 0; seek(ft(w[a].s)); revealSent(w[a].si)
    }
    // The word under a point of the transcript list (list coordinates): {gi, si, ts} (ts: the sentence's time stamp).
    function wordAtPoint(mx, my) {
        var cx = mx + sentList.contentX, cy = my + sentList.contentY
        var d = sentList.itemAt(cx, cy)
        if (!d) return { gi: -1, si: -1, ts: false }
        var p = sentList.contentItem.mapToItem(d, cx, cy), c = d.childAt(p.x, p.y)
        if (!c) return { gi: -1, si: d.si, ts: false }
        if (c.isTs === true) return { gi: -1, si: d.si, ts: true }
        return { gi: c.gi !== undefined ? c.gi : -1, si: d.si, ts: false }
    }

    function load() {
        sb = null; spriteUrl = ""; infoError = ""; frame = 0; inFrame = -1; outFrame = -1; sel = -1; speed = 0; work = null
        shown = -1; requested = -1; loading = false
        trLoad()
        if (!mediaId.length) return
        var id = mediaId
        api.get("/media/" + id + "/frameinfo", function(ok, d) {
            if (id !== tl.mediaId) return
            if (!ok) { tl.infoError = d && d.error ? d.error : "no frame info"; return }
            tl.fpsNum = d.fps.num; tl.fpsDen = d.fps.den; tl.infoDuration = d.durationSec; tl.frameCount = d.frames
            tl.dropFrame = !!(d.timecode && d.timecode.dropFrame)
            tl.fitZoom(); tl.requestFrame()
            var args = Qt.application.arguments, j = args.indexOf("--seg-seek"), k = args.indexOf("--seg-inout")   // screenshot/test hooks
            if (k > 0 && k + 1 < args.length) { var io = args[k + 1].split(","); tl.inFrame = parseInt(io[0]); tl.outFrame = parseInt(io[1]) }
            if (j > 0 && j + 1 < args.length) { tl.zoomBy(8); tl.seek(parseInt(args[j + 1])) }
        })
        api.get("/media/" + id + "/storyboard?n=120&w=160", function(ok, d) {
            if (id !== tl.mediaId || !ok) return
            tl.sb = d; tl.spriteUrl = api.rawUrl(d.url)
        })
    }
    onMediaIdChanged: load()
    Component.onCompleted: load()

    // ---- exact frame fetch: one request in flight, the newest wanted frame wins ----
    property int front: 0
    property int requested: -1
    property int shown: -1
    property bool loading: false
    readonly property int reqW: Math.min(1280, Math.max(320, Math.ceil(previewBox.width * Screen.devicePixelRatio / 160) * 160))
    function requestFrame() {
        if (!mediaId.length || loading) return
        if (frame === shown) return
        var back = front === 0 ? imgB : imgA
        var url = api.rawUrl("/frames/" + mediaId + ".jpg?frame=" + frame + "&w=" + reqW)
        if (String(back.source) === url && back.status === Image.Ready) { front = front === 0 ? 1 : 0; shown = frame; return }
        loading = true; requested = frame
        back.source = url
    }
    function landed(img, isB) {
        if (img.status === Image.Ready && ((isB && front === 0) || (!isB && front === 1))) { front = isB ? 1 : 0; shown = requested; loading = false; requestFrame() }
        else if (img.status === Image.Error) { loading = false; shown = -1 }
    }
    onFrameChanged: { requestFrame(); setCur(false) }
    Timer {
        id: player
        property real startT: 0
        property int startF: 0
        interval: 40; repeat: true; running: tl.speed !== 0
        onTriggered: {
            var f = startF + Math.round((Date.now() - startT) / 1000 * tl.fps * tl.speed)
            if (f >= tl.frames - 1) { f = tl.frames - 1; tl.speed = 0 } else if (f <= 0) { f = 0; tl.speed = 0 }
            tl.frame = f; tl.ensureVisible()
        }
    }

    Keys.onPressed: function(e) {
        var step = (e.modifiers & Qt.ShiftModifier) ? Math.round(fps) : (e.modifiers & Qt.ControlModifier) ? Math.round(fps * 10) : 1
        var k = e.key, h = true
        if (k === Qt.Key_Right) { setSpeed(0); seek(frame + step) }
        else if (k === Qt.Key_Left) { setSpeed(0); seek(frame - step) }
        else if (k === Qt.Key_Up) { setSpeed(0); seek(nextEdge(-1)) }
        else if (k === Qt.Key_Down) { setSpeed(0); seek(nextEdge(1)) }
        else if (k === Qt.Key_Comma) { setSpeed(0); wordStep(-1) }
        else if (k === Qt.Key_Period) { setSpeed(0); wordStep(1) }
        else if (k === Qt.Key_Less) { setSpeed(0); sentStep(-1) }
        else if (k === Qt.Key_Greater) { setSpeed(0); sentStep(1) }
        else if (k === Qt.Key_Home) { setSpeed(0); seek(0) }
        else if (k === Qt.Key_End) { setSpeed(0); seek(frames - 1) }
        else if (k === Qt.Key_Space) setSpeed(speed === 0 ? 1 : 0)
        else if (k === Qt.Key_K) setSpeed(0)
        else if (k === Qt.Key_L) setSpeed(speed <= 0 ? 1 : Math.min(4, speed * 2))
        else if (k === Qt.Key_J) setSpeed(speed >= 0 ? -1 : Math.max(-4, speed * 2))
        else if (k === Qt.Key_I) { inFrame = frame; clearWordSel() }
        else if (k === Qt.Key_O) { outFrame = frame; clearWordSel() }
        else if (k === Qt.Key_Return || k === Qt.Key_Enter) addSegment()
        else if (k === Qt.Key_BracketLeft) setEdge("start")
        else if (k === Qt.Key_BracketRight) setEdge("end")
        else if (k === Qt.Key_Delete || k === Qt.Key_Backspace) removeSel()
        else if (k === Qt.Key_Plus || k === Qt.Key_Equal) zoomBy(1.5)
        else if (k === Qt.Key_Minus) zoomBy(1 / 1.5)
        else h = false
        e.accepted = h
    }

    ColumnLayout {
        id: col
        anchors.left: parent.left; anchors.right: parent.right
        spacing: 6

        RowLayout {
            Layout.fillWidth: true; spacing: 10
            // ---- preview ----
            Rectangle {
                id: previewBox
                Layout.preferredWidth: Math.min(tl.width * (tl.wide ? 0.42 : 0.58), 560); Layout.preferredHeight: Layout.preferredWidth * 9 / 16
                Layout.alignment: Qt.AlignTop
                color: "#000"; radius: Theme.r5; border.color: tl.activeFocus ? Theme.accent0 : Theme.border1; clip: true
                Image { id: imgA; anchors.fill: parent; anchors.margins: 1; fillMode: Image.PreserveAspectFit; asynchronous: true; cache: false; visible: tl.front === 0; onStatusChanged: tl.landed(imgA, false) }
                Image { id: imgB; anchors.fill: parent; anchors.margins: 1; fillMode: Image.PreserveAspectFit; asynchronous: true; cache: false; visible: tl.front === 1; onStatusChanged: tl.landed(imgB, true) }
                // While the exact frame is on its way (a cold jump into a 4K file can take seconds), the storyboard tile.
                Item {
                    id: ph
                    anchors.centerIn: parent
                    visible: tl.sb !== null && tl.spriteUrl.length > 0 && (tl.shown !== tl.frame)
                    readonly property real k: tl.sb ? Math.min((previewBox.width - 2) / tl.sb.tileW, (previewBox.height - 2) / tl.sb.tileH) : 1
                    width: tl.sb ? tl.sb.tileW * k : 0; height: tl.sb ? tl.sb.tileH * k : 0; clip: true
                    readonly property int idx: tl.tileIndexAt(tl.tf(tl.frame))
                    Image { source: tl.spriteUrl; smooth: true; cache: true
                        width: tl.sb ? tl.sb.spriteW * ph.k : 0; height: tl.sb ? tl.sb.spriteH * ph.k : 0
                        x: tl.sb ? -(ph.idx % tl.sb.cols) * tl.sb.tileW * ph.k : 0; y: tl.sb ? -Math.floor(ph.idx / tl.sb.cols) * tl.sb.tileH * ph.k : 0 }
                }
                Rectangle { anchors.left: parent.left; anchors.bottom: parent.bottom; anchors.margins: 8; color: Qt.rgba(0, 0, 0, 0.7); radius: 3; width: tcText.implicitWidth + 14; height: 24
                    Text { id: tcText; anchors.centerIn: parent; text: tl.tc(tl.frame) + "   f " + tl.frame; color: "#fff"; font.family: Theme.fontMono; font.pixelSize: Theme.fs12; font.bold: true } }
                Rectangle { visible: tl.speed !== 0; anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 8; color: Qt.rgba(0, 0, 0, 0.7); radius: 3; width: spd.implicitWidth + 12; height: 22
                    Text { id: spd; anchors.centerIn: parent; text: (tl.speed < 0 ? "◀◀ " : "▶ ") + Math.abs(tl.speed) + "x"; color: Theme.accent0; font.pixelSize: Theme.fs11; font.bold: true } }
                Text { anchors.centerIn: parent; visible: tl.infoError.length > 0; text: tl.infoError; color: Theme.danger; font.pixelSize: Theme.fs12; width: parent.width - 30; wrapMode: Text.WordWrap; horizontalAlignment: Text.AlignHCenter }
                MouseArea { anchors.fill: parent; onClicked: { tl.forceActiveFocus(); tl.setSpeed(tl.speed === 0 ? 1 : 0) } }
            }
            // ---- details ----
            ColumnLayout {
                Layout.fillWidth: !tl.wide; Layout.preferredWidth: tl.wide ? 300 : -1; Layout.maximumWidth: tl.wide ? 320 : Number.POSITIVE_INFINITY
                Layout.alignment: Qt.AlignTop; spacing: 6
                Text { text: tl.tc(tl.frame); color: Theme.text0; font.family: Theme.fontMono; font.pixelSize: Theme.fs20; font.bold: true }
                Text { Layout.fillWidth: true; elide: Text.ElideRight; text: "of " + tl.tc(tl.frames - 1) + "  ·  " + tl.fps.toFixed(3).replace(/\.?0+$/, "") + " fps" + (tl.dropFrame ? " drop-frame" : "") + "  ·  " + tl.frames + " frames"; color: Theme.text2; font.pixelSize: Theme.fs11 }
                RowLayout { spacing: 6
                    Text { text: "IN"; color: Theme.text3; font.pixelSize: Theme.fs10; font.bold: true }
                    Text { text: tl.inFrame >= 0 ? tl.tc(tl.inFrame) : "—"; color: Theme.ready; font.family: Theme.fontMono; font.pixelSize: Theme.fs12
                        MouseArea { anchors.fill: parent; enabled: tl.inFrame >= 0; onClicked: tl.seek(tl.inFrame) } }
                    Text { text: "OUT"; color: Theme.text3; font.pixelSize: Theme.fs10; font.bold: true; Layout.leftMargin: 8 }
                    Text { text: tl.outFrame >= 0 ? tl.tc(tl.outFrame) : "—"; color: Theme.danger; font.family: Theme.fontMono; font.pixelSize: Theme.fs12
                        MouseArea { anchors.fill: parent; enabled: tl.outFrame >= 0; onClicked: tl.seek(tl.outFrame) } }
                    Text { visible: tl.inFrame >= 0 && tl.outFrame > tl.inFrame; text: "= " + tl.tc(tl.outFrame - tl.inFrame); color: Theme.text1; font.family: Theme.fontMono; font.pixelSize: Theme.fs11 }
                }
                Flow { Layout.fillWidth: true; spacing: 6
                    CButton { text: "In (I)"; small: true; onClicked: { tl.inFrame = tl.frame; tl.clearWordSel(); tl.forceActiveFocus() } }
                    CButton { text: "Out (O)"; small: true; onClicked: { tl.outFrame = tl.frame; tl.clearWordSel(); tl.forceActiveFocus() } }
                    CButton { text: "+ Segment (Enter)"; small: true; variant: "primary"; onClicked: { tl.addSegment(); tl.forceActiveFocus() } }
                }
                Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border0; visible: tl.sel >= 0 && tl.sel < tl.view.length }
                ColumnLayout {
                    id: selBox
                    visible: tl.sel >= 0 && tl.sel < tl.view.length; Layout.fillWidth: true; spacing: 4
                    property var g: tl.sel >= 0 && tl.sel < tl.view.length ? tl.view[tl.sel] : ({})
                    RowLayout { Layout.fillWidth: true; spacing: 6
                        Rectangle { width: 10; height: 10; radius: 2; color: tl.sel >= 0 ? tl.palette[tl.sel % tl.palette.length] : Theme.text3 }
                        TextField { id: selLabel; Layout.fillWidth: true; implicitHeight: 28; text: selBox.g.label || ""; color: Theme.text0; font.pixelSize: Theme.fs12
                            background: Rectangle { color: Theme.bg4; radius: Theme.r3; border.color: selLabel.activeFocus ? Theme.accent0 : Theme.border1 }
                            onEditingFinished: { if (tl.sel < 0) return; var s = tl.copySegs(); if (s[tl.sel] && s[tl.sel].label !== text) { s[tl.sel].label = text; tl.commit(s) } } }
                    }
                    RowLayout { spacing: 8
                        Text { text: tl.tc(tl.ft(selBox.g.startTime || 0)); color: Theme.text1; font.family: Theme.fontMono; font.pixelSize: Theme.fs11
                            MouseArea { anchors.fill: parent; onClicked: tl.seek(tl.ft(selBox.g.startTime || 0)) } }
                        Text { text: "→"; color: Theme.text3 }
                        Text { text: tl.tc(tl.ft(selBox.g.endTime || 0)); color: Theme.text1; font.family: Theme.fontMono; font.pixelSize: Theme.fs11
                            MouseArea { anchors.fill: parent; onClicked: tl.seek(tl.ft(selBox.g.endTime || 0)) } }
                        Text { text: tl.tc(tl.ft((selBox.g.endTime || 0) - (selBox.g.startTime || 0))); color: Theme.text2; font.family: Theme.fontMono; font.pixelSize: Theme.fs11 }
                    }
                    // what is said in the segment
                    Text { visible: tl.selText.length > 0; Layout.fillWidth: true; Layout.maximumHeight: 44; clip: true; wrapMode: Text.WordWrap; elide: Text.ElideRight; maximumLineCount: 3
                        text: "“" + tl.selText + "”"; color: Theme.text2; font.pixelSize: Theme.fs11 }
                    Flow { Layout.fillWidth: true; spacing: 6
                        CButton { text: "Start = here ["; small: true; variant: "ghost"; onClicked: { tl.setEdge("start"); tl.forceActiveFocus() } }
                        CButton { text: "End = here ]"; small: true; variant: "ghost"; onClicked: { tl.setEdge("end"); tl.forceActiveFocus() } }
                        CButton { text: "▶ from start"; small: true; variant: "ghost"; onClicked: { tl.seek(tl.ft(tl.view[tl.sel].startTime)); tl.setSpeed(1); tl.forceActiveFocus() } }
                        CButton { visible: tl.hasWords && tl.selText.length > 0; text: "Label from words"; small: true; variant: "ghost"; onClicked: { tl.labelFromWords(); tl.forceActiveFocus() } ToolTip.text: "Use the segment's first words as its label" }
                        CButton { text: "Delete"; small: true; variant: "ghost"; onClicked: { tl.removeSel(); tl.forceActiveFocus() } }
                    }
                }
                Item { Layout.fillHeight: true }
                Text { Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.text3; font.pixelSize: Theme.fs10
                    text: "←/→ frame · Shift 1 s · Ctrl 10 s · ↑/↓ edges · , . words · < > sentences · Space play · J K L shuttle · I/O in-out · Enter add · [ ] set edges · Del remove · Ctrl+wheel zoom" }
            }
            // the transcript panel sits here when the editor is wide (see `script` below)
            Item { id: scriptWide; visible: tl.wide; Layout.fillWidth: true; Layout.fillHeight: true; Layout.minimumHeight: previewBox.height }
        }

        // ---- transport ----
        RowLayout {
            Layout.fillWidth: true; spacing: 4
            CButton { text: "⏮"; small: true; variant: "ghost"; onClicked: { tl.setSpeed(0); tl.seek(tl.nextEdge(-1)); tl.forceActiveFocus() } ToolTip.text: "Previous edge (↑)" }
            CButton { text: "◀❘"; small: true; variant: "ghost"; onClicked: { tl.setSpeed(0); tl.seek(tl.frame - 1); tl.forceActiveFocus() } ToolTip.text: "Back one frame (←)" }
            CButton { text: tl.speed === 0 ? "▶" : "⏸"; small: true; variant: "primary"; onClicked: { tl.setSpeed(tl.speed === 0 ? 1 : 0); tl.forceActiveFocus() } ToolTip.text: "Play / pause (Space)" }
            CButton { text: "❘▶"; small: true; variant: "ghost"; onClicked: { tl.setSpeed(0); tl.seek(tl.frame + 1); tl.forceActiveFocus() } ToolTip.text: "Forward one frame (→)" }
            CButton { text: "⏭"; small: true; variant: "ghost"; onClicked: { tl.setSpeed(0); tl.seek(tl.nextEdge(1)); tl.forceActiveFocus() } ToolTip.text: "Next edge (↓)" }
            CButton { text: "J"; small: true; variant: "ghost"; active: tl.speed < 0; onClicked: { tl.setSpeed(tl.speed >= 0 ? -1 : Math.max(-4, tl.speed * 2)); tl.forceActiveFocus() } }
            CButton { text: "K"; small: true; variant: "ghost"; onClicked: { tl.setSpeed(0); tl.forceActiveFocus() } }
            CButton { text: "L"; small: true; variant: "ghost"; active: tl.speed > 0; onClicked: { tl.setSpeed(tl.speed <= 0 ? 1 : Math.min(4, tl.speed * 2)); tl.forceActiveFocus() } }
            TextField {
                id: tcField; implicitWidth: 120; implicitHeight: 28; font.family: Theme.fontMono; font.pixelSize: Theme.fs12; color: Theme.text0
                placeholderText: "go to TC / s"; placeholderTextColor: Theme.text3
                background: Rectangle { color: Theme.bg4; radius: Theme.r3; border.color: tcField.activeFocus ? Theme.accent0 : Theme.border1 }
                onAccepted: {
                    var v = text.trim(), f = -1
                    var m = v.match(/^(\d+):(\d+):(\d+)[:;.](\d+)$/)
                    if (m) {
                        // timecode -> frame (drop-frame aware: invert by search around the nominal value)
                        var nominal = ((parseInt(m[1]) * 60 + parseInt(m[2])) * 60 + parseInt(m[3])) * Math.round(tl.fps) + parseInt(m[4])
                        f = nominal; for (var k = 0; k < 400 && tl.tc(f) !== v.replace(".", ":"); ++k) f = nominal - k
                    } else if (!isNaN(parseFloat(v))) f = tl.ft(parseFloat(v))
                    if (f >= 0) tl.seek(f); text = ""; tl.forceActiveFocus()
                }
            }
            Item { Layout.fillWidth: true }
            Text { text: "zoom"; color: Theme.text3; font.pixelSize: Theme.fs10 }
            Slider {
                id: zoomSl; implicitWidth: 160; from: 0; to: 1
                readonly property real minP: flick.width / Math.max(1, tl.dur)
                readonly property real maxP: tl.fps * 14
                value: maxP > minP ? Math.log(tl.pps / minP) / Math.log(maxP / minP) : 0
                onMoved: tl.pps = minP * Math.pow(maxP / minP, value)
            }
            CButton { text: "Fit"; small: true; variant: "ghost"; onClicked: { tl.fitZoom(); flick.contentX = 0 } }
        }

        // ---- timeline ----
        Item {
            id: tlArea
            Layout.fillWidth: true; implicitHeight: 18 + 50 + 30 + (tl.hasWords ? 24 : 0) + 14
            readonly property real laneBottom: tl.hasWords ? 124 : 100
            Flickable {
                id: flick
                anchors.fill: parent
                clip: true; interactive: false; boundsBehavior: Flickable.StopAtBounds
                contentWidth: Math.max(width, tl.dur * tl.pps); contentHeight: height
                onWidthChanged: if (tl.pps < width / Math.max(1, tl.dur)) tl.fitZoom()
                ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AlwaysOn; height: 10 }
                readonly property real t0: contentX / tl.pps
                readonly property real t1: (contentX + width) / tl.pps
                Item {
                    id: content
                    width: flick.contentWidth; height: flick.height
                    Rectangle { anchors.fill: parent; color: Theme.bg1 }
                    // ruler
                    Repeater {
                        model: {
                            var cands = [1 / tl.fps, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600]
                            var step = cands[cands.length - 1]
                            for (var i = 0; i < cands.length; ++i) if (cands[i] * tl.pps >= 80) { step = cands[i]; break }
                            var a = [], first = Math.floor(flick.t0 / step) * step
                            for (var t = first; t <= flick.t1 + step && a.length < 400; t += step) if (t >= 0 && t <= tl.dur) a.push(t)
                            return a
                        }
                        delegate: Item {
                            x: modelData * tl.pps; y: 0; width: 1; height: 18
                            Rectangle { width: 1; height: 8; y: 10; color: Theme.border2 }
                            Text { x: 3; y: 0; text: tl.tc(tl.ft(modelData)); color: Theme.text2; font.family: Theme.fontMono; font.pixelSize: Theme.fs10 }
                        }
                    }
                    // filmstrip from the storyboard
                    Item {
                        id: strip
                        y: 18; width: parent.width; height: 50
                        readonly property real tileH: height
                        readonly property real tileW: tl.sb ? tileH * tl.sb.tileW / tl.sb.tileH : 89
                        readonly property int k0: Math.max(0, Math.floor(flick.contentX / tileW))
                        Repeater {
                            model: tl.sb ? Math.ceil(flick.width / strip.tileW) + 2 : 0
                            delegate: Item {
                                readonly property int k: strip.k0 + index
                                readonly property int idx: tl.tileIndexAt((k + 0.5) * strip.tileW / tl.pps)
                                readonly property real s: strip.tileH / tl.sb.tileH
                                x: k * strip.tileW; width: strip.tileW; height: strip.tileH; clip: true
                                visible: x < content.width
                                Image { source: tl.spriteUrl; cache: true; smooth: true
                                    width: tl.sb.spriteW * parent.s; height: tl.sb.spriteH * parent.s
                                    x: -(parent.idx % tl.sb.cols) * tl.sb.tileW * parent.s; y: -Math.floor(parent.idx / tl.sb.cols) * tl.sb.tileH * parent.s }
                                Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Qt.rgba(0, 0, 0, 0.6) }
                            }
                        }
                        Text { visible: !tl.sb; anchors.centerIn: parent; text: "building storyboard…"; color: Theme.text3; font.pixelSize: Theme.fs11 }
                    }
                    // in/out range
                    Rectangle {
                        visible: tl.inFrame >= 0 || tl.outFrame >= 0
                        readonly property int a: tl.inFrame >= 0 ? tl.inFrame : tl.frame
                        readonly property int b: tl.outFrame >= 0 ? tl.outFrame : tl.frame
                        x: tl.tf(Math.min(a, b)) * tl.pps; width: Math.max(2, tl.tf(Math.abs(b - a)) * tl.pps); y: 18; height: tlArea.laneBottom - 20
                        color: Qt.rgba(1, 1, 1, 0.12); border.color: Theme.ready; border.width: 1
                    }
                    // scrub: click / drag anywhere on the ruler or the strip; wheel scrolls, Ctrl+wheel zooms
                    MouseArea {
                        id: scrub
                        x: 0; y: 0; width: parent.width; height: 68
                        hoverEnabled: true; acceptedButtons: Qt.LeftButton
                        onPressed: function(m) { tl.forceActiveFocus(); tl.setSpeed(0); tl.frame = tl.clampF(tl.ft(m.x / tl.pps)) }
                        onPositionChanged: function(m) { if (pressed) tl.frame = tl.clampF(tl.ft(Math.max(0, m.x) / tl.pps)); hover.at(m.x, true) }
                        onExited: hover.at(0, false)
                        onWheel: function(w) {
                            if (w.modifiers & Qt.ControlModifier) tl.zoomBy(w.angleDelta.y > 0 ? 1.25 : 0.8, w.x - flick.contentX)
                            else flick.contentX = Math.max(0, Math.min(flick.contentWidth - flick.width, flick.contentX - (w.angleDelta.y !== 0 ? w.angleDelta.y : w.angleDelta.x)))
                        }
                    }
                    // segments lane
                    Rectangle { y: 70; width: parent.width; height: 28; color: Theme.bg2 }
                    Repeater {
                        model: tl.view.length
                        delegate: Rectangle {
                            id: segR
                            readonly property var seg: tl.view[index] || ({ startTime: 0, endTime: 0 })
                            readonly property color c: tl.palette[index % tl.palette.length]
                            x: seg.startTime * tl.pps; width: Math.max(4, (seg.endTime - seg.startTime) * tl.pps)
                            y: 70; height: 28; radius: 3
                            z: tl.sel === index ? 4 : width < 24 ? 3 : width < 120 ? 2 : 1   // short / selected segments stay reachable
                            color: Qt.rgba(c.r, c.g, c.b, tl.sel === index ? 0.85 : 0.55); border.color: tl.sel === index ? "#ffffff" : c; border.width: tl.sel === index ? 2 : 1
                            Text { anchors.fill: parent; anchors.leftMargin: 8; anchors.rightMargin: 8; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight
                                text: (index + 1) + "  " + (segR.seg.label || ""); color: "#05050a"; font.pixelSize: Theme.fs11; font.bold: true }
                            MouseArea {
                                anchors.fill: parent; anchors.leftMargin: 7; anchors.rightMargin: 7
                                onPressed: { tl.forceActiveFocus(); tl.sel = index }
                                onDoubleClicked: tl.seek(tl.ft(segR.seg.startTime))
                            }
                            // drag handles snap to frame boundaries, or to the silence between words (Alt: frames only)
                            Repeater {
                                model: ["start", "end"]
                                delegate: Rectangle {
                                    x: modelData === "start" ? -4 : segR.width - 3; width: 7; height: segR.height; radius: 2
                                    color: hm.containsMouse || hm.pressed ? "#ffffff" : Qt.rgba(1, 1, 1, 0.35)
                                    MouseArea {
                                        id: hm
                                        anchors.fill: parent; anchors.margins: -3; hoverEnabled: true; cursorShape: Qt.SizeHorCursor
                                        preventStealing: true
                                        onPressed: { tl.forceActiveFocus(); tl.sel = segR.idx; tl.work = tl.copySegs() }
                                        onPositionChanged: function(m) {
                                            if (!pressed || tl.work === null) return
                                            var p = mapToItem(content, m.x, m.y), t = Math.max(0, p.x) / tl.pps
                                            var snapOn = tl.snap && tl.trCuts.length > 0 && !(m.modifiers & Qt.AltModifier)
                                            var f = snapOn ? (modelData === "start" ? tl.inFrameOf(tl.snapT(t)) : tl.outFrameOf(tl.snapT(t))) : tl.clampF(tl.ft(t))
                                            var a = tl.work.slice(), g = Object.assign({}, a[segR.idx])
                                            if (modelData === "start") g.startTime = Math.min(tl.tf(f), g.endTime - tl.tf(1)); else g.endTime = Math.max(tl.tf(f), g.startTime + tl.tf(1))
                                            a[segR.idx] = g; tl.work = a; tl.frame = f
                                        }
                                        onReleased: { if (tl.work !== null) tl.commit(tl.work) }
                                    }
                                }
                            }
                            readonly property int idx: index
                        }
                    }
                    // speech lane: sentences, or single words from 40 px/s; only what is in view (the delegates rebind
                    // to the window's items as it moves instead of being made again)
                    Item {
                        id: speech
                        visible: tl.hasWords
                        y: 100; width: parent.width; height: 22
                        readonly property bool wordsMode: tl.pps >= 40
                        readonly property var arr: wordsMode ? tl.trWords : tl.trSents
                        readonly property int first: visible ? tl.firstGE(arr, flick.t0 - (wordsMode ? 10 : 60), tl.startOf) : 0
                        readonly property int last: visible ? tl.firstGE(arr, flick.t1 + 1e-6, tl.startOf) : 0
                        Rectangle { anchors.fill: parent; color: Theme.bg1 }
                        Repeater {
                            model: Math.max(0, Math.min(speech.last - speech.first, speech.wordsMode ? 800 : 1500))
                            delegate: Rectangle {
                                readonly property int k: speech.first + index
                                readonly property var it: speech.arr[k] || ({ s: 0, e: 0, t: "", txt: "" })
                                readonly property bool lit: speech.wordsMode && k === tl.cur
                                visible: it.e >= flick.t0
                                x: it.s * tl.pps; y: 3; height: 16; radius: 2
                                width: Math.max(speech.wordsMode ? 2 : 1, (it.e - it.s) * tl.pps)
                                color: lit ? Theme.accent2 : spm.containsMouse ? Qt.rgba(0.66, 0.33, 0.97, 0.6) : Qt.rgba(0.66, 0.33, 0.97, speech.wordsMode ? 0.22 : 0.32)
                                border.width: speech.wordsMode ? 1 : 0; border.color: Qt.rgba(0.66, 0.33, 0.97, 0.55)
                                clip: true
                                Text { anchors.fill: parent; anchors.leftMargin: 3; anchors.rightMargin: 2; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight
                                    visible: parent.width > 24 || speech.wordsMode
                                    text: speech.wordsMode ? parent.it.t : parent.it.txt; color: parent.lit ? "#05050a" : Theme.text1; font.pixelSize: Theme.fs10 }
                                MouseArea {
                                    id: spm
                                    anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                    ToolTip.visible: containsMouse && !speech.wordsMode; ToolTip.delay: 400
                                    ToolTip.text: speech.wordsMode ? "" : tl.clock(parent.it.s) + "  " + parent.it.txt
                                    onPressed: function(m) {
                                        tl.forceActiveFocus(); tl.setSpeed(0)
                                        var k = parent.k
                                        if (speech.wordsMode) {
                                            if ((m.modifiers & Qt.ShiftModifier) && tl.anchor >= 0) tl.selectWords(tl.anchor, k)
                                            else { tl.anchor = k; tl.frame = tl.clampF(tl.ft(tl.trWords[k].s)) }
                                        } else {
                                            var s = tl.trSents[k]
                                            if (m.modifiers & Qt.ShiftModifier) tl.selectWords(s.a, s.b - 1)
                                            else { tl.anchor = s.a; tl.frame = tl.clampF(tl.ft(s.s)) }
                                        }
                                    }
                                }
                            }
                        }
                    }
                    // playhead
                    Rectangle { x: tl.tf(tl.frame) * tl.pps - 1; y: 0; width: 2; height: tlArea.laneBottom; color: Theme.onAir
                        Rectangle { x: -5; y: 0; width: 12; height: 8; radius: 2; color: Theme.onAir } }
                }
            }
            // hover preview (outside the clipped flickable so it can sit above the timeline)
            Rectangle {
                id: hover
                property real t: 0
                function at(cx, on) {
                    visible = on && tl.sb !== null
                    if (!visible) return
                    t = Math.max(0, Math.min(tl.dur, cx / tl.pps))
                    var p = flick.contentItem.mapToItem(tlArea, cx, 0)
                    x = Math.max(0, Math.min(tlArea.width - width, p.x - width / 2))
                }
                visible: false; z: 50
                width: 176; height: hoverTile.height + 26; y: -height - 4
                color: Theme.bg0; border.color: Theme.accent0; radius: 4
                Item {
                    id: hoverTile
                    x: 8; y: 6; width: 160; height: tl.sb ? 160 * tl.sb.tileH / tl.sb.tileW : 90; clip: true
                    readonly property int idx: tl.tileIndexAt(hover.t)
                    readonly property real s: tl.sb ? 160 / tl.sb.tileW : 1
                    Image { source: tl.spriteUrl; cache: true
                        width: tl.sb ? tl.sb.spriteW * hoverTile.s : 0; height: tl.sb ? tl.sb.spriteH * hoverTile.s : 0
                        x: tl.sb ? -(hoverTile.idx % tl.sb.cols) * tl.sb.tileW * hoverTile.s : 0; y: tl.sb ? -Math.floor(hoverTile.idx / tl.sb.cols) * tl.sb.tileH * hoverTile.s : 0 }
                }
                Text { anchors.bottom: parent.bottom; anchors.bottomMargin: 3; anchors.horizontalCenter: parent.horizontalCenter; text: tl.tc(tl.ft(hover.t)); color: Theme.text0; font.family: Theme.fontMono; font.pixelSize: Theme.fs11; font.bold: true }
            }
        }

        // the transcript panel sits here when the editor is narrow (see `script` below)
        Item { id: scriptNarrow; visible: !tl.wide; Layout.fillWidth: true; Layout.preferredHeight: 210 }
    }

    // ---- transcript panel: the words, find, follow / snap, the word selection; or the way to get a transcript ----
    Rectangle {
        id: script
        parent: tl.wide ? scriptWide : scriptNarrow
        anchors.fill: parent
        color: Theme.bg1; radius: Theme.r5; border.color: Theme.border0
        ColumnLayout {
            anchors.fill: parent; anchors.margins: 1; spacing: 0
            RowLayout {
                Layout.fillWidth: true; Layout.leftMargin: 8; Layout.rightMargin: 6; Layout.topMargin: 3; Layout.bottomMargin: 3; spacing: 6
                Text { text: "TRANSCRIPT"; color: Theme.text1; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1 }
                Text { visible: tl.hasWords; text: tl.trWords.length + " words" + (tl.trLang.length ? " · " + tl.trLang : "") + (tl.trModel.length ? " · " + tl.trModel : ""); color: Theme.text3; font.pixelSize: Theme.fs10; elide: Text.ElideRight; Layout.maximumWidth: 170 }
                TextField {
                    id: findField
                    visible: tl.hasWords; implicitWidth: 150; implicitHeight: 26
                    placeholderText: "find words…"; placeholderTextColor: Theme.text3; color: Theme.text0; font.pixelSize: Theme.fs12
                    background: Rectangle { color: Theme.bg4; radius: Theme.r3; border.color: findField.activeFocus ? Theme.accent0 : Theme.border1 }
                    onTextChanged: tl.runFind(text)
                    Keys.onPressed: function(e) {
                        if (e.key === Qt.Key_Return || e.key === Qt.Key_Enter) { tl.gotoHit((e.modifiers & Qt.ShiftModifier) ? -1 : 1); e.accepted = true }
                        else if (e.key === Qt.Key_Escape) { text = ""; tl.forceActiveFocus(); e.accepted = true }
                    }
                }
                Text { visible: tl.hasWords; Layout.preferredWidth: 46; text: tl.hits.length ? (tl.hit >= 0 ? tl.hit + 1 : "–") + "/" + tl.hits.length : (findField.text.trim().length ? "0" : ""); color: Theme.text2; font.family: Theme.fontMono; font.pixelSize: Theme.fs10 }
                CButton { visible: tl.hasWords; text: "↑"; small: true; variant: "ghost"; onClicked: { tl.gotoHit(-1) } ToolTip.text: "Previous match (Shift+Enter)" }
                CButton { visible: tl.hasWords; text: "↓"; small: true; variant: "ghost"; onClicked: { tl.gotoHit(1) } ToolTip.text: "Next match (Enter)" }
                Item { Layout.fillWidth: true }
                CToggle { visible: tl.hasWords; label: "follow"; checked: tl.follow; Layout.preferredWidth: 86; onToggled: function(v) { tl.follow = v; if (v) { tl.userScrollT = 0; tl.revealSent(tl.curSent) } } }
                CToggle { visible: tl.hasWords; label: "snap to words"; checked: tl.snap; Layout.preferredWidth: 136; onToggled: function(v) { tl.snap = v } }
            }
            Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border0 }
            // the word selection, or how to make one: one line of fixed height so the words never move under the pointer
            RowLayout {
                id: wselRow
                visible: tl.hasWords
                readonly property bool has: tl.selA >= 0 && tl.inFrame >= 0 && tl.outFrame > tl.inFrame
                Layout.fillWidth: true; Layout.preferredHeight: 28; Layout.leftMargin: 8; Layout.rightMargin: 6; spacing: 6
                Text { visible: !wselRow.has; Layout.fillWidth: true; elide: Text.ElideRight; color: Theme.text3; font.pixelSize: Theme.fs10
                    text: "Click a word to go there · shift-click or drag across words for in/out · double-click for the sentence · Enter makes the segment" }
                Text { visible: wselRow.has; Layout.fillWidth: true; elide: Text.ElideRight; color: Theme.text1; font.pixelSize: Theme.fs11
                    text: wselRow.has ? "“" + tl.wordsText(tl.selA, tl.selB) + "”" : "" }
                Text { visible: wselRow.has; text: wselRow.has ? tl.tc(tl.outFrame - tl.inFrame) : ""; color: Theme.text2; font.family: Theme.fontMono; font.pixelSize: Theme.fs11 }
                CButton { visible: wselRow.has; text: "+ Segment"; small: true; variant: "primary"; onClicked: { tl.addSegment(); tl.forceActiveFocus() } ToolTip.text: "Make a segment of these words (Enter)" }
                CButton { visible: wselRow.has; text: "✕"; small: true; variant: "ghost"; onClicked: { tl.clearWordSel(); tl.inFrame = -1; tl.outFrame = -1; tl.forceActiveFocus() } ToolTip.text: "Clear the selection and in/out" }
            }
            Rectangle { visible: tl.hasWords; Layout.fillWidth: true; height: 1; color: Theme.border0 }
            Item {
                Layout.fillWidth: true; Layout.fillHeight: true
                ListView {
                    id: sentList
                    anchors.fill: parent; anchors.margins: 6
                    visible: tl.hasWords
                    clip: true; interactive: false; boundsBehavior: Flickable.StopAtBounds
                    model: tl.trSents.length
                    spacing: 4
                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded; width: 10 }
                    delegate: Flow {
                        id: sentD
                        width: sentList.width - 14
                        spacing: 3
                        readonly property var sent: tl.trSents[index] || ({ s: 0, a: 0, b: 0 })
                        readonly property int si: index
                        Text { readonly property bool isTs: true; height: 20; verticalAlignment: Text.AlignVCenter; rightPadding: 4
                            text: tl.clock(sentD.sent.s); color: Theme.accent0; font.family: Theme.fontMono; font.pixelSize: Theme.fs10 }
                        Repeater {
                            model: sentD.sent.b - sentD.sent.a
                            delegate: Rectangle {
                                readonly property int gi: sentD.sent.a + index
                                readonly property var w: tl.trWords[gi] || ({ t: "" })
                                readonly property int hm: tl.hitRev >= 0 && tl.hitMask ? (tl.hitMask[gi] || 0) : 0
                                readonly property bool isCur: gi === tl.cur
                                readonly property bool inIo: gi >= tl.ioRange[0] && gi < tl.ioRange[1]
                                readonly property bool inSeg: gi >= tl.segRange[0] && gi < tl.segRange[1]
                                width: wtx.implicitWidth + 4; height: 20; radius: 3
                                color: isCur ? Theme.accent2 : hm === 2 ? "#66ffd60a" : inIo ? "#4d30d158" : inSeg ? tl.segTint : "transparent"
                                Rectangle { visible: parent.hm === 1; anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 2; color: Theme.warn }
                                Text { id: wtx; anchors.centerIn: parent; text: parent.w.t; color: parent.isCur ? "#05050a" : Theme.text0; font.pixelSize: Theme.fs12 }
                            }
                        }
                    }
                }
                // One area for the words: click, shift-click, drag, double-click, and the wheel (the list itself is not
                // interactive, so dragging selects words instead of scrolling).
                MouseArea {
                    anchors.fill: sentList; anchors.rightMargin: 12
                    visible: sentList.visible
                    property int downGi: -1
                    onPressed: function(m) {
                        tl.forceActiveFocus(); tl.setSpeed(0); tl.userScrollT = Date.now(); downGi = -1
                        var h = tl.wordAtPoint(m.x, m.y)
                        if (h.ts) { tl.seek(tl.ft(tl.trSents[h.si].s)); return }
                        if (h.gi < 0) return
                        if (m.modifiers & Qt.ShiftModifier) { tl.selectWords(tl.anchor >= 0 ? tl.anchor : (tl.cur >= 0 ? tl.cur : h.gi), h.gi); return }
                        downGi = h.gi; tl.anchor = h.gi; tl.seek(tl.ft(tl.trWords[h.gi].s))
                    }
                    onPositionChanged: function(m) {
                        if (!pressed || downGi < 0) return
                        tl.userScrollT = Date.now()
                        var h = tl.wordAtPoint(m.x, m.y)
                        if (h.gi >= 0 && h.gi !== downGi) tl.selectWords(downGi, h.gi)
                    }
                    onReleased: function(m) {
                        if (downGi >= 0) { var h = tl.wordAtPoint(m.x, m.y); if (h.gi >= 0 && h.gi !== downGi) tl.selectWords(downGi, h.gi) }
                        downGi = -1
                    }
                    onDoubleClicked: function(m) {
                        var h = tl.wordAtPoint(m.x, m.y); if (h.gi < 0) return
                        var s = tl.trSents[tl.trWords[h.gi].si]; tl.selectWords(s.a, s.b - 1)
                    }
                    onWheel: function(wh) {
                        tl.userScrollT = Date.now()
                        var top = sentList.originY, bottom = sentList.originY + Math.max(0, sentList.contentHeight - sentList.height)
                        sentList.contentY = Math.max(top, Math.min(bottom, sentList.contentY - wh.angleDelta.y))
                    }
                }
                // no words: say why, and offer to transcribe (with the job's progress while it runs)
                ColumnLayout {
                    visible: !tl.hasWords
                    anchors.fill: parent; anchors.margins: 10; spacing: 8
                    Text {
                        Layout.fillWidth: true; wrapMode: Text.WordWrap; font.pixelSize: Theme.fs11
                        color: tl.trState === "failed" ? Theme.danger : Theme.text2
                        text: tl.trState === "failed" ? "Transcription failed: " + (tl.trErr.length ? tl.trErr : "unknown error")
                            : tl.trState === "none" ? "No transcript yet. Transcribe this media to see its words here, find phrases and cut segments on the words."
                            : tl.trState === "queued" ? "Transcription queued… the words appear here when it is done."
                            : tl.trState === "running" ? "Transcribing… the words appear here when it is done."
                            : tl.trState === "done" ? "The transcript has no speech in it."
                            : "Loading the transcript…"
                    }
                    RowLayout {
                        visible: tl.trState === "queued" || tl.trState === "running"; spacing: 8
                        Rectangle { width: 220; height: 6; radius: 3; color: Theme.bg4
                            Rectangle { width: parent.width * Math.max(0, Math.min(100, tl.trPct)) / 100; height: parent.height; radius: 3; color: Theme.accent0 } }
                        Text { text: Math.round(tl.trPct) + "%"; color: Theme.text2; font.family: Theme.fontMono; font.pixelSize: Theme.fs11 }
                    }
                    RowLayout {
                        visible: tl.trState === "none" || tl.trState === "failed" || (tl.trState === "done" && !tl.hasWords); spacing: 8
                        CSelect { id: trLvl; label: "Accuracy"; Layout.preferredWidth: 200; model: tl.trLevels; textRole: "id"
                            currentIndex: { for (var i = 0; i < tl.trLevels.length; ++i) if (tl.trLevels[i].id === tl.trLevel) return i; return 0 }
                            onActivated: function(i) { tl.trLevel = tl.trLevels[i].id } }
                        CButton { text: tl.trState === "done" ? "Re-transcribe" : "Transcribe"; variant: "primary"; Layout.alignment: Qt.AlignBottom; onClicked: tl.transcribe() }
                    }
                    Text { visible: (tl.trState === "none" || tl.trState === "failed") && tl.levelNote().length > 0; Layout.fillWidth: true; wrapMode: Text.WordWrap; text: tl.levelNote(); color: Theme.text3; font.pixelSize: Theme.fs10 }
                    Item { Layout.fillHeight: true }
                }
            }
        }
    }
}
