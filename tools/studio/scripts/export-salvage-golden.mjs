// Dev time only (ST-07a): records what starmedia's stripFences() / salvageJson() (src/openrouter.ts) answer for a fixed
// set of model outputs, into tests/studio/fixtures/salvage-golden.json, so test_studio_openrouter can check the C++ port
// (provider/openrouter.cpp) against the real TypeScript. Node 22, no packages.
//
//   node tools/studio/scripts/export-salvage-golden.mjs [starmedia root] [out file]
//
// The two functions are cut out of openrouter.ts by their text (they use nothing else from the module), their type
// annotations removed, and evaluated here. The output is deterministic: re-running it reproduces the file byte for byte.
// It only reads the starmedia checkout.
import { readFileSync, writeFileSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const root = resolve(process.argv[2] ?? process.env.STARMEDIA_ROOT ?? (console.error('pass the starmedia checkout as the first argument or set STARMEDIA_ROOT'), process.exit(2)));
const out = resolve(process.argv[3] ?? join(here, '../../../tests/studio/fixtures/salvage-golden.json'));
const file = join(root, 'src/openrouter.ts');
const src = readFileSync(file, 'utf8').replace(/\r\n/g, '\n');

const a = src.indexOf('export function stripFences');
const b = src.indexOf('// ---------------- Images');
if (a < 0 || b < a) throw new Error('stripFences / salvageJson not found in src/openrouter.ts: update this script');
const code = src.slice(a, b)
  .replace(/export function/g, 'function')
  .replace('(text: string): any', '(text)')
  .replace('(s: string): string', '(s)')
  .replace('const stack: string[] = [];', 'const stack = [];')
  .replace('lastStack: string[] = []', 'lastStack = []');
if (/:\s*(string|any|number)\b/.test(code.replace(/"[^"]*"|'[^']*'/g, ''))) throw new Error('type annotations left in the cut-out code: update this script');
const { stripFences, salvageJson } = new Function(`${code}\nreturn { stripFences, salvageJson };`)();

const cases = [
  '{"a":1}', '```json\n{"a":1}\n```', '```\n[1,2]\n```', 'Here is the plan: {"a":[1,2,3]} hope it helps',
  '{"items":[{"x":1},{"x":2},{"x":', '{"items":[1,2,3', '[{"a":"b"},{"a":"c', '{"a":"b","c":', 'no json here', '',
  '{"a": "x}y", "b": [1, {"c": "d\\"e"}, 3', '  ```json {"k":true}```  ', 'prefix [1, 2, ', '{"s":"\u00e9\u2028"}', '[[1,2],[3,',
  '{"a":{"b":{"c":1}},"d":[', 'text } then {"ok":1}', '{"a":1}}', '[1,2]]', '{"a":[{"b":1}, {"c":2}],', '{', '[',
  '{"x":[1,2],"y":"open', '"just a string"', '42', 'true trailing', '{"a":1, "b":2,\n "list": [ "p", "q",\u00a0',
  '```json\n{"a":1}', '{"t":"a\\\\"}  more', '[{"a":1},{"b":[1,2,{"c":', 'x{"a":[1,{"b":[2,[3,',
  '```JSON\n{"a":1}\n```', '``````', '```json```', '````', '\ufeff {"bom":true} \u3000', 'Sure!\n```json\n{"shots":[{"n":1},{"n":2},{"n":',
  '[1, 2, 3]] trailing', '{"deep":[[[[1,2],[3', '{"k":[1,2],}', '["a\\u0041b", "c', 'null', '{"n":-0.5e3,"m":[true,false,null,'
];
const results = cases.map((c) => {
  const r = salvageJson(c);
  return { in: c, strip: stripFences(c), out: r === undefined ? null : JSON.stringify(r), undefined: r === undefined };
});
let commit = '';
try { commit = execFileSync('git', ['-C', root, 'log', '-1', '--format=%H %cs', '--', 'src/openrouter.ts'], { encoding: 'utf8' }).trim(); } catch { /* not a git checkout */ }
const doc = {
  _comment: 'stripFences / salvageJson answers of starmedia src/openrouter.ts for these inputs (tools/studio/scripts/export-salvage-golden.mjs). out = JSON.stringify of the salvaged value, null when salvageJson returned undefined.',
  version: 1,
  source: { file: 'src/openrouter.ts', commit, sha256: createHash('sha256').update(src).digest('hex') },
  cases: results,
};
writeFileSync(out, JSON.stringify(doc, null, 1) + '\n');
console.log(`wrote ${results.length} cases to ${out}`);
