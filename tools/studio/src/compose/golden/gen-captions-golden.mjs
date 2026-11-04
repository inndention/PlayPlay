// Dev-time generator of captions-golden.json: what the REAL StarMedia subtitle writers (src/media.ts toSrt / toVtt)
// print for a set of cue lists, which test_studio_compose compares the C++ captions module (compose/captions.h, ST-18b)
// against byte for byte.
//
// How: media.ts has no imports, so it is copied (unchanged) into a temp folder marked as an ES module and imported with
// Node's type stripping. Each case's cues go through toSrt and toVtt; a table of single times records ts() for both
// separators. Times that JSON cannot hold are written as strings ("NaN", "Infinity", "-Infinity", "-0") and decoded
// the same way here and in the test.
//
// Usage (Node 22+, no packages needed):
//   node --experimental-strip-types tools/studio/src/compose/golden/gen-captions-golden.mjs [starmedia root] \
//        > tools/studio/src/compose/golden/captions-golden.json
// The root is the first argument or $STARMEDIA_ROOT. Output is deterministic.
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { pathToFileURL } from 'node:url';
import { execFileSync } from 'node:child_process';

const root = path.resolve(process.argv[2] || process.env.STARMEDIA_ROOT || (console.error('pass the starmedia checkout as the first argument or set STARMEDIA_ROOT'), process.exit(2)));
const src = path.join(root, 'src/media.ts');
let head = 'unknown';
try { head = execFileSync('git', ['-C', root, 'log', '-1', '--format=%h %cs', '--', 'src/media.ts'], { encoding: 'utf8' }).trim(); } catch { /* not a checkout */ }

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'captions-golden-'));
fs.writeFileSync(path.join(tmp, 'package.json'), '{"type":"module"}');
fs.copyFileSync(src, path.join(tmp, 'media.ts'));
const M = await import(pathToFileURL(path.join(tmp, 'media.ts')).href);

const enc = (n) => (Number.isNaN(n) ? 'NaN' : n === Infinity ? 'Infinity' : n === -Infinity ? '-Infinity' : Object.is(n, -0) ? '-0' : n);
const cue = (start, end, text) => ({ start, end, text });

const cases = [];
const add = (name, cues) => cases.push({
  name,
  cues: cues.map((c) => ({ start: enc(c.start), end: enc(c.end), text: c.text })),
  srt: M.toSrt(cues),
  vtt: M.toVtt(cues),
});

add('empty', []);
add('basic', [cue(0, 1.5, 'Hello world'), cue(1.5, 3.25, 'Second line')]);
add('millisecond floor', [cue(0.3, 1.001, 'a'), cue(2.3, 3661.001, 'b'), cue(59.9999, 60, 'c'), cue(0.1 + 0.2, 0.7, 'd'), cue(4.35, 8.2, 'e')]);
add('hours', [cue(3599.999, 3600, 'h1'), cue(86399.5, 86400.25, 'h24'), cue(359999.999, 360000, 'h100'), cue(362439.5, 400000, 'big')]);
add('negative and zero', [cue(-0.5, 0, 'n1'), cue(-0, 0.0004, 'n2'), cue(-3601.25, -0.001, 'n3')]);
add('not finite', [cue(NaN, Infinity, 'x'), cue(-Infinity, 1, 'y')]);
add('text', [
  cue(1, 2, 'a < b & c > d'), cue(2, 3, '<i>tag</i> &amp;'), cue(3, 4, 'line one\nline two'), cue(4, 5, 'arrow --> here'),
  cue(5, 6, ''), cue(6, 7, 'Café ünïcode 🎉 emoji'), cue(7, 8, '  spaced  '), cue(8, 9, 'tab\there'),
]);
// frame-aligned times as the composer's caption cues produce them (k / 30 s)
add('frame times', Array.from({ length: 40 }, (_, i) => cue((7 * i) / 30, (7 * i + 19) / 30, `cue ${i + 1}`)));
// cuesForLine-like spans and a long list (numbering past 9 and 99)
add('long list', Array.from({ length: 120 }, (_, i) => cue(i * 1.37, i * 1.37 + 0.9 + (i % 3) * 0.333, `line ${i}`)));

// ts() for single times, both separators
const times = [0, 0.001, 0.0005, 0.3, 0.7, 1, 1.001, 1.1, 1.3, 2.3, 4.35, 9.999, 10.01, 59.9999, 60, 61.5, 599.4, 3599.999, 3600,
  3661.001, 7322.123, 86400, 359999.999, 360000, 1e7, -0.25, -1, -61.5, 1 / 3, 2 / 3, 1e-9, 123.456, 0.1 + 0.2, NaN, Infinity, -Infinity, -0];
const tsOf = (t, fn) => {
  const out = fn([cue(t, 0, 'x')]);
  return out.match(/(\S+) --> /)[1];
};
const table = times.map((t) => ({ t: enc(t), srt: tsOf(t, M.toSrt), vtt: tsOf(t, M.toVtt) }));

fs.rmSync(tmp, { recursive: true, force: true });
process.stdout.write(JSON.stringify({
  generator: 'tools/studio/src/compose/golden/gen-captions-golden.mjs',
  source: `src/media.ts @ ${head}`,
  times: table,
  cases,
}, null, 1) + '\n');
