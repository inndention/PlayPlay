// Dev-time generator of compose-golden.json: what the REAL StarMedia composer (mcp/spot/compose.ts) asks ffmpeg to do
// for a set of compositions, which test_studio_compose compares the C++ filtergraph builder against (ST-18a).
//
// How: compose.ts is copied (unchanged) into a temp folder next to a stub "./ffmpeg.js" that records every ffmpeg call
// instead of running it (probe says "readable" except for names containing "junk"; stageFont gives "fontfile=font.ttf").
// renderComposition() then runs for each case exactly as in the MCP server; the recorded argument lists, the join graph
// (graph.txt), the drawtext text files, the files it staged and the returned result go into the fixture. Paths are
// written with tokens ({WORK} the scratch folder, {FX} the input fixtures, {OUT} the output folder) and forward slashes.
// The pure helpers (cleanText, wrapText, hexColor, frameSize, safeArea, cuesForLine) are recorded as tables too.
//
// Usage (Node 22+, no packages needed):
//   node --experimental-strip-types tools/studio/src/compose/golden/gen-compose-golden.mjs [starmedia root] \
//        > tools/studio/src/compose/golden/compose-golden.json
// The root is the first argument or $STARMEDIA_ROOT. Output is deterministic.
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { pathToFileURL } from 'node:url';
import { execFileSync } from 'node:child_process';

const root = path.resolve(process.argv[2] || process.env.STARMEDIA_ROOT || (console.error('pass the starmedia checkout as the first argument or set STARMEDIA_ROOT'), process.exit(2)));
const src = path.join(root, 'mcp/spot/compose.ts');
let head = 'unknown';
try { head = execFileSync('git', ['-C', root, 'log', '-1', '--format=%h %cs'], { encoding: 'utf8' }).trim(); } catch { /* not a checkout */ }

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'compose-golden-'));
const FX = path.join(tmp, 'fx'), WORKROOT = path.join(tmp, 'work'), OUT = path.join(tmp, 'out');
for (const d of [FX, WORKROOT, OUT]) fs.mkdirSync(d, { recursive: true });
fs.writeFileSync(path.join(tmp, 'package.json'), '{"type":"module"}');
fs.copyFileSync(src, path.join(tmp, 'compose.ts'));
fs.writeFileSync(path.join(tmp, 'ffmpeg.js'), `
import fs from 'node:fs';
import path from 'node:path';
export const calls = [];
export async function ffmpeg(args, o = {}) {
  calls.push({ args: [...args], progress: !!o.onProgress });
  const out = args[args.length - 1];                       // every compose.ts call ends with its output file
  fs.writeFileSync(path.resolve(o.cwd ?? '.', out), '');
}
export async function probe(file) { return /junk/i.test(path.basename(file)) ? undefined : { hasVideo: true, hasAudio: false }; }
export function stageFont() { return 'fontfile=font.ttf'; }
`);
const stub = await import(pathToFileURL(path.join(tmp, 'ffmpeg.js')).href);
const C = await import(pathToFileURL(path.join(tmp, 'compose.ts')).href);

// fixtures: empty files are enough (the stub never reads them; compose.ts copies stills and the logo)
const fx = (n) => path.join(FX, n);
for (const n of ['a still.png', 'b%d.png', 'clip.mp4', 'vo.wav', 'vo2.wav', 'music.wav', 'logo.png', 'junk.mp4', 'long.wav', 'logo junk.png']) fs.writeFileSync(fx(n), 'x');
const A = fx('a still.png'), B = fx('b%d.png'), CLIP = fx('clip.mp4'), VO = fx('vo.wav'), VO2 = fx('vo2.wav'), MUSIC = fx('music.wav');
const LOGO = fx('logo.png'), JUNK = fx('junk.mp4'), LONG = fx('long.wav'), BADLOGO = fx('logo junk.png');

// record copies (the stills and the logo compose.ts stages into the work dir)
let copies = [];
const realCopy = fs.copyFileSync;
fs.copyFileSync = (from, to, ...rest) => { copies.push([String(from), String(to)]); return realCopy(from, to, ...rest); };

const tok = (s, work) => {
  let t = String(s);
  for (const [dir, name] of [[work, '{WORK}'], [FX, '{FX}'], [OUT, '{OUT}']]) {
    if (t.startsWith(dir)) t = name + t.slice(dir.length).replace(/\\/g, '/');
  }
  return t;
};

// ---------------------------------------------------------------- cases
const cases = [];
const add = (name, spec, note) => cases.push({ name, note, spec });

// test/mcp-spot.ts "9:16": still with Ken Burns, a clip, a colour card, per-scene VO, music, transcript captions, end card
// with logo, exact target (the spec compose_video builds from that call)
add('vertical', {
  aspect: '9:16', quality: 'draft', transition: 'fade', loudness: 'social', targetSeconds: 12,
  scenes: [
    { kind: 'image', src: A, duration: 2.5, text: 'Fresh coffee, roasted in town', audio: { path: VO, start: 0 } },
    { kind: 'video', src: CLIP, duration: 3, transition: 'dip' },
    { kind: 'color', color: '#0b3d91', duration: 2, text: 'Baked daily from 6 AM', transition: 'slide', audio: { path: VO2, start: 0 } },
    { kind: 'image', src: B, duration: 2, kenburns: 'left', fit: 'contain' },
  ],
  music: { path: MUSIC, volume: 0.22, duck: true },
  captions: C.cuesForLine('Every morning starts with a choice. Make it a good one. Come say hi.', 0, 9.5),
  endCard: { brand: 'Main Street Coffee', headline: 'Fresh roasted daily', cta: 'Come say hi', url: 'mainstreet.coffee', logo: LOGO, colors: ['#0b3d91', '#ff8a3d'], duration: 2.5 },
}, 'test/mcp-spot.ts 9:16 render');
// test/mcp-spot.ts "16:9": timed cues, a whole-video voice-over, no target
add('landscape', {
  aspect: '16:9', quality: 'draft', loudness: 'social',
  scenes: [{ kind: 'image', src: A, duration: 2, kenburns: 'in' }, { kind: 'color', color: '#222222', duration: 1.5, text: 'Two' }],
  voiceover: { path: VO, start: 0 }, captions: [{ start: 0.2, end: 1.5, text: 'Hello there' }, { start: 1.6, end: 3, text: 'Second cue' }],
  endCard: { brand: 'Acme', duration: 1.5 },
}, 'test/mcp-spot.ts 16:9 render');
// test/mcp-spot.ts 1:1 and 4:5: colour cards only, no audio inputs (silent track)
for (const [name, aspect] of [['square', '1:1'], ['portrait45', '4:5']]) {
  add(name, { aspect, quality: 'draft', loudness: 'social', scenes: [{ kind: 'color', color: '#ff8a3d', duration: 1.5, text: 'Square deal' }, { kind: 'color', color: '#111111', duration: 1.5 }] }, `test/mcp-spot.ts ${aspect} render`);
}
// test/mcp-spot.ts: an unreadable clip falls back to a colour card, a long voice-over is cut at the target
add('fallback', { aspect: '9:16', quality: 'draft', loudness: 'social', targetSeconds: 3, scenes: [{ kind: 'video', src: JUNK, duration: 1.5 }, { kind: 'image', src: A, duration: 1.5 }], voiceover: { path: LONG, start: 0 } }, 'test/mcp-spot.ts unreadable clip');
// test/mcp-fix-spot.ts: cut, then fade, then the end card (the xfade time-base fix)
add('cutfade', {
  aspect: '9:16', quality: 'draft', loudness: 'social',
  scenes: [{ kind: 'color', color: '#112233', duration: 2, audio: { path: VO, start: 0 } }, { kind: 'color', color: '#332211', duration: 2, transition: 'fade' }, { kind: 'color', color: '#223311', duration: 2 }],
  endCard: { brand: 'X', duration: 1.5 },
}, 'test/mcp-fix-spot.ts cut then fade then end card');
// every transition, final quality; the second overlap is capped at 40 % of the 1 s last scene
for (const tr of C.TRANSITIONS) {
  add(`tr-${tr}`, { aspect: '16:9', quality: 'final', transitionSeconds: 0.8, scenes: [
    { kind: 'color', color: '#102030', duration: 2, transition: tr }, { kind: 'color', color: '#304050', duration: 1.5, transition: tr }, { kind: 'color', color: '#506070', duration: 1 },
  ] }, `transition ${tr}`);
}
// every Ken Burns mode, both qualities and aspects
C.KENBURNS.forEach((kb, i) => add(`kb-${kb}`, { aspect: i % 2 ? '9:16' : '16:9', quality: i % 3 === 0 ? 'final' : 'draft', scenes: [{ kind: 'image', src: A, duration: 3.2, kenburns: kb }] }, `Ken Burns ${kb}`));
// "auto" cycles in, right, out, left, in, up by scene index; contain fit on one still
add('kb-auto-cycle', { aspect: '1:1', quality: 'draft', scenes: Array.from({ length: 7 }, (_, i) => ({ kind: 'image', src: i === 3 ? B : A, duration: 1, ...(i === 3 ? { fit: 'contain' } : {}) })) }, 'Ken Burns auto cycle');
// ducking and the music bed
add('duck', { aspect: '9:16', quality: 'draft', scenes: [{ kind: 'color', color: '#000000', duration: 4 }], voiceover: { path: VO, start: 0.5 }, music: { path: MUSIC, volume: 0.3, duck: true } }, 'music ducked under the voice');
add('noduck', { aspect: '9:16', quality: 'draft', loudness: 'broadcast', scenes: [{ kind: 'color', color: '#000000', duration: 4 }], voiceover: { path: VO }, music: { path: MUSIC, volume: 0.5, duck: false } }, 'music not ducked, broadcast loudness');
add('musiconly', { aspect: '16:9', quality: 'draft', loudness: 'off', scenes: [{ kind: 'color', color: '#000000', duration: 2.5 }], music: { path: MUSIC } }, 'music only, loudness off');
add('manyvoices', { aspect: '16:9', quality: 'draft', scenes: [{ kind: 'color', duration: 2, audio: { path: VO } }, { kind: 'color', duration: 2, audio: { path: VO2, start: 0.4, volume: 1.4 } }], voiceover: { path: LONG, start: 0.25, volume: 0.5 }, music: { path: MUSIC, volume: 1.7 } }, 'three voice parts mixed, then ducked music (volume clamped)');
// the spot pipeline's shape (mcp/spot/pipeline.ts composeSpecFor): per-scene narration at 0.15 s with tempo and cut,
// end-card line, captions per line, music 0.28 ducked, 25 fps, explicit poster time
add('pipeline', {
  aspect: '4:5', quality: 'final', fps: 25, transition: 'dissolve', loudness: 'social', posterAt: 1.25,
  scenes: [
    { kind: 'image', src: A, duration: 2.6, text: 'Fresh every morning', kenburns: 'auto', audio: { path: VO, start: 0.15, tempo: 1.18, maxSeconds: 2.4 } },
    { kind: 'image', src: B, duration: 3.1, kenburns: 'out', audio: { path: VO2, start: 0.15, tempo: 1 } },
    { kind: 'color', color: '#f5f5dc', accent: '#0a0', duration: 2.3, transition: 'cut', text: 'Roasted here', audio: { path: VO, start: 0.15, tempo: 2.5, volume: 0.8 } },
  ],
  endCard: { brand: 'Main Street Coffee', cta: 'Come say hi', url: 'mainstreet.coffee', colors: ['#fefefe', '#123', '#ff0000'], duration: 2.5, audio: { path: VO2, start: 0.15, tempo: 1.3, maxSeconds: 1.9 } },
  music: { path: MUSIC, volume: 0.28, duck: true },
  captions: [...C.cuesForLine('Fresh coffee, roasted right here on Main Street.', 0.15, 2.1), ...C.cuesForLine('Pastries baked next door — every single day!', 2.75, 2.6)],
}, 'spot pipeline shape');
// headlines (positions, truncation warning, emoji), caption edge cases (zero length, past the end, emoji only), target
// shorter than the scenes
add('text', {
  aspect: '16:9', quality: 'final', targetSeconds: 10.2,
  scenes: [
    { kind: 'color', color: '#fafafa', accent: '#e11d48', duration: 3, text: 'A very long headline that keeps going and going well past what any reasonable on-screen card could ever hold in four lines of big type 🎉 really' },
    { kind: 'image', src: A, duration: 3, text: 'Bottom third headline here', textPosition: 'bottom' },
    { kind: 'image', src: A, duration: 3, text: 'Centered ✨ words', textPosition: 'center', kenburns: 'none' },
    { kind: 'video', src: CLIP, duration: 2, text: 'Top line', fit: 'contain' },
  ],
  captions: [
    { start: 0, end: 2, text: 'First caption with some words' }, { start: 2.5, end: 2.5, text: 'zero length skipped' },
    { start: 3, end: 20, text: 'A caption that runs past the end and has quite a lot of words so it must wrap onto two lines and then be truncated with an ellipsis' },
    { start: 11, end: 12, text: 'after the end' }, { start: 5, end: 6, text: '🙂' }, { start: 6, end: 7.5, text: 'Supercalifragilisticexpialidocious-and-then-some-more-letters' },
  ],
}, 'headlines and captions');
add('pad', { aspect: '9:16', quality: 'draft', targetSeconds: 4.75, scenes: [{ kind: 'color', duration: 1.2, text: 'x' }], endCard: { headline: 'Only a headline', duration: 1, colors: ['#fff', '#000', '#123456'] } }, 'target longer than the scenes (clone pad), 3-digit colours');
add('badlogo', { aspect: '1:1', quality: 'draft', scenes: [{ kind: 'video', src: JUNK, fallbackImage: A, duration: 2, transition: 'wipe' }, { kind: 'color', color: 'nonsense', duration: 2 }], endCard: { brand: 'B', headline: 'H', logo: BADLOGO, duration: 2 } }, 'clip fallback still, bad colour, unreadable logo');
add('endcard-only', { aspect: '16:9', quality: 'draft', scenes: [], endCard: { brand: 'Solo Brand Name That Is Long', headline: 'A headline', cta: 'Call now', url: 'example.com', logo: LOGO, colors: ['#000000'], duration: 3 } }, 'end card alone, landscape logo');
add('tinyslots', { aspect: '9:16', quality: 'final', transition: 'zoom', transitionSeconds: 3, scenes: [{ kind: 'color', duration: 0.1 }, { kind: 'color', duration: 0.7 }, { kind: 'color', duration: 5 }] }, 'slot minimum 0.3 s, transition seconds clamped to 1.5');

const out = [];
for (const c of cases) {
  const work = path.join(WORKROOT, c.name);
  stub.calls.length = 0;
  copies = [];
  let result, error;
  try {
    result = await C.renderComposition(c.spec, { workDir: work, outFile: path.join(OUT, 'v.mp4'), posterFile: path.join(OUT, 'v.png') });
  } catch (e) { error = e.message; }
  const texts = [];
  for (let i = 0; fs.existsSync(path.join(work, `t${i}.txt`)); i++) texts.push(fs.readFileSync(path.join(work, `t${i}.txt`), 'utf8'));
  const graphFile = path.join(work, 'graph.txt');
  const tokSpec = JSON.parse(JSON.stringify(c.spec, (k, v) => (typeof v === 'string' && v.startsWith(FX) ? tok(v, work) : v)));
  out.push({
    name: c.name, note: c.note, spec: tokSpec, error,
    calls: stub.calls.map((k) => ({ args: k.args.map((a) => tok(a, work)), progress: k.progress })),
    graph: fs.existsSync(graphFile) ? fs.readFileSync(graphFile, 'utf8') : undefined,
    texts,
    staged: copies.filter(([, to]) => to.startsWith(work)).map(([from, to]) => [tok(from, work), path.basename(to)]),
    result: result && { duration: result.duration, width: result.width, height: result.height, fps: result.fps, timeline: result.timeline, warnings: result.warnings },
  });
}
// a composition with nothing to render
{
  let error;
  try { await C.renderComposition({ aspect: '9:16', scenes: [] }, { workDir: path.join(WORKROOT, 'empty'), outFile: path.join(OUT, 'e.mp4'), posterFile: path.join(OUT, 'e.png') }); } catch (e) { error = e.message; }
  out.push({ name: 'empty', spec: { aspect: '9:16', scenes: [] }, error, calls: [], texts: [], staged: [] });
}
fs.copyFileSync = realCopy;

// ---------------------------------------------------------------- pure helpers
const cleanInputs = ['  Hello   world  ', 'Tab\there\nnew line\r\nend', 'Emoji 🎉 party 👍🏽 ok', 'Family 👨‍👩‍👧 here', 'Heart ❤️ red', 'NBSP\u00a0and\u2003em\u2028sep', '\u0000\u0001ctrl\u007f', '', '   ', 'Zero\u200bwidth', 'Café — “quotes” ©®™', '#1 ★ star ☀ sun ✓ check', 'Flags 🇺🇸 and keycap 1️⃣', 'Mixed\u0085next\ufeffbom', 'Arrows ↔ ⬆ and 〰 wavy', 'CJK 日本語 テスト'];
const wrapInputs = [
  ['Every morning starts with a choice and this one is a very good choice indeed', 16, 2],
  ['short', 10, 1], ['', 10, 2], ['one two three four five six seven', 9, 3], ['abcdefghijklmnopqrstuvwxyz', 6, 3],
  ['abcdefghijklmnopqrstuvwxyz0123456789', 6, 2], ['Hello, world; this is: a test. Of punctuation, at the end, of lines.', 12, 2],
  ['Supercalifragilisticexpialidocious', 8, 2], ['a b c d e f g h i j k l m n o p', 3, 2], ['Word  🎉 with emoji inside the wrapped text', 10, 2],
  ['exactly sixteen!', 16, 1], ['exactly sixteen!!', 16, 1], ['wrap, wrap, wrap, wrap, wrap, wrap', 11, 1], ['aaaaaa bbbbbb cccccc', 6, 2],
];
const hexInputs = [['#ff8a3d', 'F'], ['ff8a3d', 'F'], ['#FFF', 'F'], ['#abc', 'F'], ['  #0b3d91 ', 'F'], ['#12345', 'F'], ['red', '0x111827'], [undefined, '0x111827'], ['#gggggg', ''], ['0x123456', 'F'], ['#1234567', 'F']];
const cueInputs = [
  ['Every morning starts with a choice. Make it a good one. Come say hi.', 0, 9.5, 6],
  ['Fresh coffee, roasted right here on Main Street. Come say hi.', 1, 4, 6],
  ['Pastries baked next door — every single day!', 2.75, 2.6, 6],
  ['one two three four five six seven', 0, 3, 6], ['one two three four five six seven eight nine ten eleven twelve thirteen', 0.5, 7, 4],
  ['', 0, 3, 6], ['Hi', 0, 0, 6], ['A; b: c! d? e, f. g', 0, 2, 6], ['Wow!', 3, 1.25, 6],
];
const golden = {
  generator: 'tools/studio/src/compose/golden/gen-compose-golden.mjs',
  source: `mcp/spot/compose.ts @ ${head}`,
  transitions: C.TRANSITIONS, kenburns: C.KENBURNS, aspects: C.ASPECTS,
  frameSize: C.ASPECTS.flatMap((a) => ['draft', 'final'].map((q) => ({ aspect: a, quality: q, ...C.frameSize(a, q) }))),
  safeArea: C.ASPECTS.map((a) => ({ aspect: a, ...C.safeArea(a) })),
  cleanText: cleanInputs.map((s) => [s, C.cleanText(s)]),
  wrapText: wrapInputs.map(([t, m, l]) => ({ text: t, maxChars: m, maxLines: l, ...C.wrapText(t, m, l) })),
  hexColor: hexInputs.map(([c, f]) => ({ input: c ?? null, fallback: f, out: C.hexColor(c, f) })),
  cuesForLine: cueInputs.map(([t, s, d, w]) => ({ text: t, start: s, seconds: d, maxWords: w, cues: C.cuesForLine(t, s, d, w) })),
  cases: out,
};
process.stdout.write(JSON.stringify(golden, null, 1) + '\n');
fs.rmSync(tmp, { recursive: true, force: true });
