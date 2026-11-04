// Dev-time generator of tests/studio/fixtures/core-golden.json: the TypeScript values that test_studio_core compares the
// C++ core against (ST-02). It imports the REAL StarMedia helpers (no copies), so a change there shows up as a diff of
// the regenerated fixture:
//   - mcp/core/util.ts   stableJson, sha256, round, usd   (spend gate, approval args hash, ledger)
//   - src/pricing.ts     fmtUsd                           (harness plan details)
//   - mcp/core/register.ts CONTROL_KEYS (read from the source text; importing register.ts would pull the whole server)
//
// Usage (Node 22+, no packages needed):
//   node --experimental-strip-types tools/studio/scripts/golden-core.mjs [starmedia root] > tests/studio/fixtures/core-golden.json
// The root is the first argument or $STARMEDIA_ROOT. Output is deterministic (seeded PRNG).
import fs from 'node:fs';
import path from 'node:path';
import { pathToFileURL } from 'node:url';
import { execFileSync } from 'node:child_process';

const root = path.resolve(process.argv[2] || process.env.STARMEDIA_ROOT || (console.error('pass the starmedia checkout as the first argument or set STARMEDIA_ROOT'), process.exit(2)));
const util = await import(pathToFileURL(path.join(root, 'mcp/core/util.ts')).href);
const pricing = await import(pathToFileURL(path.join(root, 'src/pricing.ts')).href);
const registerSrc = fs.readFileSync(path.join(root, 'mcp/core/register.ts'), 'utf8');
const ck = /const CONTROL_KEYS = (\[[^\]]*\]);/.exec(registerSrc);
if (!ck) throw new Error('CONTROL_KEYS not found in mcp/core/register.ts');
const CONTROL_KEYS = JSON.parse(ck[1].replace(/'/g, '"'));
// The same expression as register.ts argsHash (L193), over the imported sha256 / stableJson.
const argsHash = (tool, args) => util.sha256(tool + util.stableJson(Object.fromEntries(Object.entries(args).filter(([k]) => !CONTROL_KEYS.includes(k))))).slice(0, 32);

let head = 'unknown';
try { head = execFileSync('git', ['-C', root, 'log', '-1', '--format=%h %cs'], { encoding: 'utf8' }).trim(); } catch { /* not a checkout */ }

// mulberry32: deterministic, so the fixture only changes when the TS code does.
let seed = 0x5713d10;
const rnd = () => { seed |= 0; seed = (seed + 0x6d2b79f5) | 0; let t = Math.imul(seed ^ (seed >>> 15), 1 | seed); t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t; return ((t ^ (t >>> 14)) >>> 0) / 4294967296; };
const rint = (n) => Math.floor(rnd() * n);

const dv = new DataView(new ArrayBuffer(8));
const bitsOf = (x) => { dv.setFloat64(0, x); return dv.getBigUint64(0).toString(16).padStart(16, '0'); };
const fromBits = (b) => { dv.setBigUint64(0, b); return dv.getFloat64(0); };

// ---------------- stableJson over JSON texts (both sides parse the same text) ----------------
const texts = [
  ['null', 'null'], ['true', 'true'], ['false', 'false'], ['zero', '0'], ['negzero', '-0'], ['one', '1'], ['neg', '-1'],
  ['half', '1.5'], ['tenth', '0.1'], ['e21', '1e21'], ['e20', '1e20'], ['e-7', '1e-7'], ['micro', '0.000001'],
  ['bigint', '123456789012345678901234567890'], ['2^53+1', '9007199254740993'], ['denorm', '5e-324'],
  ['maxdbl', '1.7976931348623157e308'], ['small', '123e-20'], ['price', '0.0000003'], ['usd', '19.99'],
  ['empty string', '""'], ['ascii', '"a"'], ['escapes', '"quote\\" backslash\\\\ slash/ tab\\t"'],
  ['controls', '"\\u0000\\u0001\\b\\f\\n\\r\\t\\u001f\\u007f"'], ['unicode', '"héllo wörld — ✓ 😀 日本"'],
  ['line separators', '"\\u2028\\u2029"'], ['bmp edge', '"\\uffff\\ue000\\u00a0"'],
  ['empty object', '{}'], ['empty array', '[]'], ['two keys', '{"b":1,"a":2}'],
  ['nested', '{"z":{"y":[3,{"b":null,"a":"x"}],"x":true},"a":[]}'],
  ['key order', '{"é":1,"e":2,"E":3,"😀":4,"\\uff01":5,"10":6,"9":7,"":8,"a b":9,"_":10,"~":11}'],
  ['proto key', '{"__proto__":1,"constructor":2,"toString":3}'],
  ['deep array', '[1,[2,[3,[]]],null,{"a":[{}]}]'],
  ['tool args', '{"prompt":"A red car on a coastal road","model":"x-ai/grok-4","n":2,"size":"1024x1024","options":{"seed":42,"quality":"high","steps":null},"dry_run":true}'],
  ['plan', '{"phase":"assets","tier":"B","nodes":[{"id":"st_key_1","kind":"image","estimate_usd":0.04},{"id":"st_vid_1","kind":"video","estimate_usd":0.6,"deps":["st_key_1"]}],"budget_usd":5}'],
];
{ // 40 keys in random order with mixed values
  const o = {};
  for (let i = 0; i < 40; i++) o[`k${rint(1000)}_${i}`] = i % 3 === 0 ? i * 1.25 : i % 3 === 1 ? `v${i}` : [i, { q: i }];
  texts.push(['many keys', JSON.stringify(o)]);
}
const stableJson = texts.map(([name, input]) => {
  const v = JSON.parse(input);
  const s = util.stableJson(v);
  return { name, input, stable: s, sha256: util.sha256(s) };
});

// ---------------- values JSON text cannot carry identically: built the same way in C++ by id ----------------
const specials = {
  lone_high: '\ud800', lone_low: 'a\udc00b', pair: '\ud83d\ude00', reversed_pair: '\ude00\ud83d',
  undefined_in_array: [1, undefined, 2], undefined_member: { b: 1, a: undefined, c: [undefined] },
  nan: NaN, infinity: Infinity, neg_infinity: -Infinity, nan_in_object: { x: NaN, y: -0 },
  top_undefined: undefined,
};
const special = Object.entries(specials).map(([id, v]) => { const s = util.stableJson(v); return { id, stable: s, sha256: util.sha256(s) }; });

// ---------------- ECMAScript Number::toString over many doubles (as exact bit patterns) ----------------
const nums = new Set();
const addNum = (x) => { if (Number.isFinite(x)) nums.add(bitsOf(x)); };
[0.1, 0.2, 0.1 + 0.2, 1 / 3, 2 / 3, 100, 1e21, 1e20, 123456789012345680000, 1e-6, 1e-7, 1.5e-7, 0.000001234, 5e-324,
  1.7976931348623157e308, 2 ** 53, 2 ** 53 + 2, 2 ** 63, 2 ** 64, 4.35, 0.3, 1.005, 2.675, 9.95, 1e100, 1.2345e-100,
  123e-20, 4294967295, 4294967296, -1.5, -0.000001, 25, 0.25, 5e-7, 1.7976931348623157e307].forEach(addNum);
for (let e = -30; e <= 30; e++) for (let j = 0; j < 4; j++) addNum((rint(1e9) + 1) * 10 ** e / 1e3);
for (let i = 0; i < 160; i++) {
  const b = (BigInt(rint(2 ** 32)) << 32n) | BigInt(rint(2 ** 32));
  addNum(fromBits(b));
}
for (let i = 0; i < 40; i++) addNum(rint(2 ** 31) * rint(2 ** 22) + rint(1000));
for (let i = 0; i < 60; i++) addNum(rint(100000) / 10 ** (1 + rint(8)));
const numbers = [...nums].map((b) => ({ bits: b, js: String(fromBits(BigInt('0x' + b))) }));

// ---------------- money: round(n, 6) (fromUsd) and the two USD display formats ----------------
const usdIn = [0, 1, 0.25, 5, 20, 0.0000035, 1.0000005, 2.5e-7, 0.1 + 0.2, -0.0000005, -1.5e-6, -2.5e-6, 1e-7, 12345.678901234,
  0.000001, 0.0000015, 0.0000025, 0.04, 0.6, 0.006, 0.012, 1e-12, 0.000999999, 9007.199254740993, 1234567.8912345];
for (let i = 0; i < 60; i++) usdIn.push(rint(1e9) / 10 ** (3 + rint(8)));
for (let i = 0; i < 20; i++) usdIn.push(rint(2000) / 1e6 + 0.5e-6); // half-micro ties
const fromUsd = usdIn.map((n) => ({ bits: bitsOf(n), usd: n, micro: Math.round(n * 1e6) }));

const micros = [0, 1, 49, 50, 51, 99, 100, 101, 149, 150, 151, 999, 1000, 4999, 5000, 5001, 9949, 9950, 9951, 9999, 10000, 10001,
  14999, 15000, 15001, 25000, 99999, 100000, 250000, 999499, 999500, 999999, 1000000, 1004999, 1005000, 1005001, 1234567,
  19990000, 99999999, 123456789012, 9007199254740];
for (let i = 0; i < 80; i++) micros.push(rint(10 ** (1 + rint(9))));
const usd = micros.map((m) => ({ micro: m, mcp: util.usd(m / 1e6), plan: pricing.fmtUsd(m / 1e6) }));

// ---------------- approval args hash (register.ts argsHash) ----------------
const argsCases = [
  ['generate_image', { prompt: 'a lighthouse at dusk', n: 2 }],
  ['generate_image', { n: 2, prompt: 'a lighthouse at dusk', dry_run: true, approve: 'apv_0123456789abcdef01234567' }],
  ['generate_video', { prompt: 'drone shot over hills', seconds: 6, model: 'google/veo-3.1', wait: 30, first_frame: null }],
  ['spot_generate', { brief: { product: 'Café Ünïcode', cta: 'Call now — 555-0100' }, length_s: 30, voices: ['af_heart', 'am_adam'] }],
  ['ask', {}],
];
const argsHashes = argsCases.map(([tool, args]) => ({ tool, args: JSON.stringify(args), hash: argsHash(tool, args) }));

const out = {
  generatedBy: 'tools/studio/scripts/golden-core.mjs',
  source: `starmedia ${head}: mcp/core/util.ts (stableJson, sha256, round, usd), src/pricing.ts (fmtUsd), mcp/core/register.ts (argsHash)`,
  controlKeys: CONTROL_KEYS,
  stableJson, special, numbers, fromUsd, usd, argsHash: argsHashes,
};
process.stdout.write(JSON.stringify(out, null, 1) + '\n');
