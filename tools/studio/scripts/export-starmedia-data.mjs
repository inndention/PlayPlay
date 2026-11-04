// Dev-time exporter of the StarMedia data PlayPlayStudio needs at run time (ST-25): tools/studio/data/model-limits.json,
// realpeople.json, model-prompts.json and styles.json. Node is used at dev time only: the committed JSON files are the
// Studio's runtime source (compiled into PlayPlayStudioLib through tools/studio/resources.qrc and read by
// studio::data in tools/studio/src/core/datafiles.*). Re-run this script whenever the TypeScript below changes;
// test_studio_core (dataMatchesStarmedia) fails until the committed files match the checkout again.
//
// It imports the REAL TypeScript modules (no hand copies), so the values are exactly what StarMedia uses:
//   model-limits   src/model-limits.data.ts (MODEL_LIMITS, PLATFORM_LIMITS), src/limits.ts (RETIRED, REQUEST_CAPS),
//                  src/payload.ts (request-size constants)
//   realpeople     mcp/media/realpeople.ts (name lists, look-alike letters, the picture-rule patterns)
//   model-prompts  src/studio/model-prompts.data.ts (MODEL_GUIDES), src/studio/modelprompts.ts (the enhancePrompt and
//                  recommendModels tables), mcp/spot/guides.ts (the spot tools' fallback guides)
//   styles         src/studio/styles.ts (STYLES)
// Constants a module does not export are exposed by a module-loader hook that appends `export { ... }` to that module
// while it loads (PRIVATE below); the hook also resolves the repo's `./x.js` imports to the `./x.ts` sources. A constant
// that was renamed or removed fails the export with its name, never silently.
//
// Every file is checked before anything is written: against tools/studio/data/schema.json (the same schema
// studio::data::validate applies in C++; this script implements the same keyword subset) and for uniqueness and regex
// validity. Output is deterministic: key order as in the TS, no timestamps; `source` records each input file's last
// commit and the sha256 of its text with CRLF folded to LF (the C++ drift check hashes the checkout the same way).
//
// Usage (Node 22.6+, no packages):
//   node --experimental-strip-types tools/studio/scripts/export-starmedia-data.mjs [starmedia root] [out dir]
//   node --experimental-strip-types tools/studio/scripts/export-starmedia-data.mjs --check [starmedia root] [dir]
// Defaults: env STARMEDIA_ROOT (required unless passed as the first argument); out dir = <this repo>/tools/studio/data.
// --check writes nothing: it compares the export with the files in [dir] (default the committed ones) and exits 0 when
// every file matches (CRLF and LF line ends compare equal), 1 when any differs, 2 on errors. The starmedia checkout is
// only read; an out dir inside it is refused.
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { register } from 'node:module';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { execFileSync } from 'node:child_process';

const FORMAT = 1;
const here = path.dirname(fileURLToPath(import.meta.url));
const committedDir = path.resolve(here, '..', 'data');
const schemaFile = path.join(committedDir, 'schema.json');

const fail = (msg, code = 2) => { process.stderr.write(`export-starmedia-data: ${msg}\n`); process.exit(code); };

const argv = process.argv.slice(2);
const check = argv.includes('--check');
const badOpt = argv.find((a) => a.startsWith('--') && a !== '--check');
if (badOpt) fail(`unknown option ${badOpt}`);
const pos = argv.filter((a) => !a.startsWith('--'));
if (pos.length > 2) fail('too many arguments');
const root = path.resolve(pos[0] || process.env.STARMEDIA_ROOT || (console.error('pass the starmedia checkout as the first argument or set STARMEDIA_ROOT'), process.exit(2)));
const target = path.resolve(pos[1] || committedDir);
const inside = (child, parent) => { const r = path.relative(parent, child); return r === '' || (!r.startsWith('..') && !path.isAbsolute(r)); };
if (!check && inside(target, root)) fail(`refusing to write into the starmedia checkout (${target})`);

// output file → the TS files it is made from (all are recorded in its `source`)
const FILES = {
  'model-limits': ['src/model-limits.data.ts', 'src/limits.ts', 'src/payload.ts'],
  realpeople: ['mcp/media/realpeople.ts'],
  'model-prompts': ['src/studio/model-prompts.data.ts', 'src/studio/modelprompts.ts', 'mcp/spot/guides.ts'],
  styles: ['src/studio/styles.ts'],
};
// module-private constants this exporter needs (appended as `export { ... }` while the module loads)
const PRIVATE = {
  'src/limits.ts': ['REQUEST_CAPS'],
  'mcp/media/realpeople.ts': ['CONFUSABLES', 'TITLE_NAME', 'REAL_WORDS', 'PHOTO_WORDS', 'STYLIZED', 'NOT_PERSON_WORD', 'GIVEN'],
  'src/studio/modelprompts.ts': ['TV_LINE', 'TV_LINE_POSITIVE', 'POSITIVE', 'LETTERING', 'MOVES', 'MARKERS', 'PRESETS', 'BUDGET_SHARE', 'RES_RANK'],
  'mcp/spot/guides.ts': ['GUIDES', 'GENERIC'],
};

for (const rel of new Set(Object.values(FILES).flat())) {
  if (!fs.existsSync(path.join(root, rel))) fail(`${rel} not found under ${root} (not a starmedia checkout, or the file moved)`);
}
const urlOf = (rel) => pathToFileURL(path.join(root, rel)).href;

// ---------------- loader hooks (serialized into a data: module; they run on Node's loader thread) ----------------
let exposed = {};
async function initialize(data) { exposed = data; }
async function resolve(specifier, context, nextResolve) {
  try { return await nextResolve(specifier, context); } catch (e) {
    // the repo imports its TypeScript siblings as './x.js' (tsc output names); load the './x.ts' source instead
    const relative = specifier.startsWith('./') || specifier.startsWith('../');
    if (e?.code !== 'ERR_MODULE_NOT_FOUND' || !context.parentURL || !relative || !specifier.endsWith('.js')) throw e;
    return nextResolve(new URL(specifier.slice(0, -3) + '.ts', context.parentURL).href, context);
  }
}
async function load(url, context, nextLoad) {
  const r = await nextLoad(url, context);
  const names = exposed[url];
  return names ? { ...r, source: String(r.source) + '\nexport { ' + names.join(', ') + ' };\n' } : r;
}
register('data:text/javascript,' + encodeURIComponent(`let exposed = {};\nexport ${initialize}\nexport ${resolve}\nexport ${load}\n`),
  { data: Object.fromEntries(Object.entries(PRIVATE).map(([rel, names]) => [urlOf(rel), names])) });

const mod = async (rel) => {
  try { return await import(urlOf(rel)); } catch (e) {
    if (e?.code === 'ERR_UNKNOWN_FILE_EXTENSION') fail('Node cannot load .ts here: run it with node --experimental-strip-types (Node 22.6 or later)');
    if (e instanceof SyntaxError && /export/i.test(e.message)) fail(`${rel}: ${e.message} (a constant in PRIVATE was renamed or removed: update this script)`);
    fail(`cannot load ${rel}: ${String(e?.message ?? e).split('\n')[0]}`);
  }
};
const [limitsData, limits, payload, realpeople, guidesData, modelprompts, spotGuides, styles] = await Promise.all(
  ['src/model-limits.data.ts', 'src/limits.ts', 'src/payload.ts', 'mcp/media/realpeople.ts', 'src/studio/model-prompts.data.ts',
    'src/studio/modelprompts.ts', 'mcp/spot/guides.ts', 'src/studio/styles.ts'].map(mod));

// ---------------- provenance ----------------
const git = (...args) => { try { return execFileSync('git', ['-C', root, ...args], { encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'] }).trim(); } catch { return ''; } };
const lfSha256 = (file) => crypto.createHash('sha256').update(fs.readFileSync(file, 'utf8').replace(/\r\n/g, '\n'), 'utf8').digest('hex');
const sourceOf = (rels) => ({
  repo: 'starmedia',
  files: rels.map((rel) => {
    if (git('status', '--porcelain', '--', rel)) process.stderr.write(`warning: ${rel} has uncommitted changes; the export records its working-copy text\n`);
    return { path: rel, commit: git('log', '-1', '--format=%H %cs', '--', rel) || 'unknown', sha256: lfSha256(path.join(root, rel)) };
  }),
});

const pat = (re, what) => {
  if (!(re instanceof RegExp)) fail(`${what} is not a RegExp`);
  return { source: re.source, flags: re.flags };
};
const plain = (v) => JSON.parse(JSON.stringify(v));
const comment = (name, text) => `Generated by tools/studio/scripts/export-starmedia-data.mjs from starmedia ${FILES[name].join(', ')}; do not edit by hand. ${text}`;

// ---------------- the four files ----------------
const out = {};
out['model-limits'] = {
  _comment: comment('model-limits', 'models = MODEL_LIMITS: researched input limits per model id (a ":free" id uses its paid model\'s entry; live catalog values win over these); platform = PLATFORM_LIMITS; retired = RETIRED (id → reason; never planned, a hand-picked one fails with the reason); requestCaps = REQUEST_CAPS (whole-request byte caps by model-id pattern); payload = the request-size constants of payload.ts (bodyCap: /videos* paths use videosBody, every other path defaultBody).'),
  version: FORMAT,
  source: sourceOf(FILES['model-limits']),
  models: plain(limitsData.MODEL_LIMITS),
  platform: plain(limitsData.PLATFORM_LIMITS),
  retired: plain(limits.RETIRED),
  requestCaps: limits.REQUEST_CAPS.map(([re, bytes], i) => ({ pattern: pat(re, `REQUEST_CAPS[${i}]`), bytes })),
  payload: {
    mb: payload.MB,
    openrouterMaxBody: payload.OPENROUTER_MAX_BODY,
    safeBody: payload.SAFE_BODY,
    videosMaxBody: payload.VIDEOS_MAX_BODY,
    defaultBody: plain(payload.bodyCap('/chat/completions')),
    videosBody: plain(payload.bodyCap('/videos')),
    referenceImage: plain(payload.REFERENCE_IMAGE),
    firstFrame: plain(payload.FIRST_FRAME),
    sttPieceSeconds: payload.STT_PIECE_SECONDS,
    sttMaxSingle: payload.STT_MAX_SINGLE,
    uploadChunk: payload.UPLOAD_CHUNK,
  },
};
out.realpeople = {
  _comment: comment('realpeople', 'Names are normalised as normName() writes them (NFKC, invisible characters removed, look-alike letters folded, accents and punctuation removed, lower case). publicFigures and stageNames match anywhere as whole words; ambiguousNames match in free text only when Capitalised (any case in a NAME field); givenNames = GIVEN (a Capitalised given name + Capitalised surname reads as a person); confusables = the Cyrillic / Greek look-alikes folded to Latin; patterns are the picture rule\'s JavaScript regular expressions (studio::data::regex keeps their JS meaning).'),
  version: FORMAT,
  source: sourceOf(FILES.realpeople),
  publicFigures: plain(realpeople.PUBLIC_FIGURES),
  stageNames: plain(realpeople.STAGE_NAMES),
  ambiguousNames: plain(realpeople.AMBIGUOUS_NAMES),
  givenNames: [...realpeople.GIVEN],
  confusables: plain(realpeople.CONFUSABLES),
  patterns: {
    titleName: pat(realpeople.TITLE_NAME, 'TITLE_NAME'),
    realWords: pat(realpeople.REAL_WORDS, 'REAL_WORDS'),
    photoWords: pat(realpeople.PHOTO_WORDS, 'PHOTO_WORDS'),
    stylized: pat(realpeople.STYLIZED, 'STYLIZED'),
    notPersonWord: pat(realpeople.NOT_PERSON_WORD, 'NOT_PERSON_WORD'),
  },
};
out['model-prompts'] = {
  _comment: comment('model-prompts', 'guides = MODEL_GUIDES: researched prompting guides per model family (match = JavaScript regex sources tested case-insensitively against the model id; the first guide of the model\'s kind that matches wins). enhance = the enhancePrompt tables (TV framing lines, avoid-word → positive wording, the lettering wordings, camera-move vocabulary, the studio prompt labels in order); recommend = the recommendModels tables (content presets, budget share per medium, resolution rank); spotFallback = the spot tools\' local guides, used only when no researched guide matches (generic when none matches at all).'),
  version: FORMAT,
  source: sourceOf(FILES['model-prompts']),
  guides: plain(guidesData.MODEL_GUIDES),
  enhance: {
    tvLine: modelprompts.TV_LINE,
    tvLinePositive: modelprompts.TV_LINE_POSITIVE,
    positive: modelprompts.POSITIVE.map(([re, wording], i) => ({ pattern: pat(re, `POSITIVE[${i}]`), wording })),
    lettering: [...modelprompts.LETTERING],
    moves: plain(modelprompts.MOVES),
    markers: modelprompts.MARKERS.map(([key, label]) => ({ key, label })),
  },
  recommend: {
    presets: plain(modelprompts.PRESETS),
    budgetShare: plain(modelprompts.BUDGET_SHARE),
    resRank: plain(modelprompts.RES_RANK),
  },
  spotFallback: {
    guides: spotGuides.GUIDES.map(({ re, g }, i) => ({ pattern: pat(re, `GUIDES[${i}]`), ...plain(g) })),
    generic: plain(spotGuides.GENERIC),
  },
};
out.styles = {
  _comment: comment('styles', 'styles = STYLES in picker order; defaultId = the first. styleOf(id, custom): an unknown id or "custom" gives the default style with id "custom", name "Custom" and visual = the trimmed custom text (the default\'s visual when that is empty). visual is appended to every image / video prompt; the picker labels may name a studio, the prompts never do.'),
  version: FORMAT,
  source: sourceOf(FILES.styles),
  defaultId: styles.STYLES[0]?.id,
  groups: [...new Set(styles.STYLES.map((s) => s.group))],
  styles: plain(styles.STYLES),
};

// ---------------- checks: schema (same keyword subset as studio::data::validate) + semantics ----------------
const KEYWORDS = new Set(['$schema', '$id', '$comment', '$defs', 'title', 'description', '$ref', 'type', 'enum', 'const', 'anyOf',
  'minLength', 'maxLength', 'pattern', 'minimum', 'maximum', 'exclusiveMinimum', 'items', 'minItems', 'maxItems', 'uniqueItems',
  'properties', 'required', 'additionalProperties', 'propertyNames', 'minProperties']);
const typeOf = (v) => (v === null ? 'null' : Array.isArray(v) ? 'array' : typeof v === 'number' ? (Number.isInteger(v) ? 'integer' : 'number') : typeof v);
const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);
function validate(v, s, rootSchema, at, errs) {
  if (errs.length >= 50) return errs;
  if (s === true) return errs;
  if (s === false) { errs.push(`${at}: not allowed`); return errs; }
  for (const k of Object.keys(s)) if (!KEYWORDS.has(k)) errs.push(`schema at ${at}: unsupported keyword ${k}`);
  if (s.$ref !== undefined) {
    const m = /^#\/\$defs\/(.+)$/.exec(s.$ref);
    const def = m && rootSchema.$defs?.[m[1]];
    if (def === undefined) errs.push(`schema at ${at}: unresolved $ref ${s.$ref}`); else validate(v, def, rootSchema, at, errs);
  }
  const t = typeOf(v);
  if (s.type !== undefined) {
    const types = Array.isArray(s.type) ? s.type : [s.type];
    if (!types.some((x) => x === t || (x === 'number' && t === 'integer'))) { errs.push(`${at}: expected ${types.join(' or ')}, got ${t}`); return errs; }
  }
  if (s.enum !== undefined && !s.enum.some((e) => same(e, v))) errs.push(`${at}: ${JSON.stringify(v)} is not one of ${JSON.stringify(s.enum)}`);
  if (s.const !== undefined && !same(s.const, v)) errs.push(`${at}: expected ${JSON.stringify(s.const)}`);
  if (s.anyOf !== undefined && !s.anyOf.some((x) => validate(v, x, rootSchema, at, []).length === 0)) errs.push(`${at}: matches none of anyOf`);
  if (t === 'string') {
    if (s.minLength !== undefined && v.length < s.minLength) errs.push(`${at}: shorter than ${s.minLength}`);
    if (s.maxLength !== undefined && v.length > s.maxLength) errs.push(`${at}: longer than ${s.maxLength}`);
    if (s.pattern !== undefined) {
      let re;
      try { re = new RegExp(s.pattern, 'u'); } catch (e) { errs.push(`schema at ${at}: pattern ${s.pattern}: ${e.message}`); }
      if (re && !re.test(v)) errs.push(`${at}: ${JSON.stringify(v)} does not match ${s.pattern}`);
    }
  }
  if (t === 'number' || t === 'integer') {
    if (s.minimum !== undefined && v < s.minimum) errs.push(`${at}: below ${s.minimum}`);
    if (s.maximum !== undefined && v > s.maximum) errs.push(`${at}: above ${s.maximum}`);
    if (s.exclusiveMinimum !== undefined && v <= s.exclusiveMinimum) errs.push(`${at}: not above ${s.exclusiveMinimum}`);
  }
  if (t === 'array') {
    if (s.minItems !== undefined && v.length < s.minItems) errs.push(`${at}: fewer than ${s.minItems} items`);
    if (s.maxItems !== undefined && v.length > s.maxItems) errs.push(`${at}: more than ${s.maxItems} items`);
    if (s.uniqueItems) { const seen = new Set(); for (const x of v) { const k = JSON.stringify(x); if (seen.has(k)) errs.push(`${at}: duplicate item ${k}`); seen.add(k); } }
    if (s.items !== undefined) v.forEach((x, i) => validate(x, s.items, rootSchema, `${at}[${i}]`, errs));
  }
  if (t === 'object') {
    const keys = Object.keys(v);
    if (s.minProperties !== undefined && keys.length < s.minProperties) errs.push(`${at}: fewer than ${s.minProperties} members`);
    for (const r of s.required ?? []) if (!Object.hasOwn(v, r)) errs.push(`${at}: missing ${r}`);
    for (const k of keys) {
      const sub = `${at}.${k}`;
      if (s.propertyNames !== undefined) validate(k, s.propertyNames, rootSchema, `${sub} (name)`, errs);
      if (s.properties?.[k] !== undefined) validate(v[k], s.properties[k], rootSchema, sub, errs);
      else if (s.additionalProperties !== undefined) validate(v[k], s.additionalProperties, rootSchema, sub, errs);
    }
  }
  return errs;
}
function semantics(name, d) {
  const errs = [];
  const unique = (what, xs) => { const seen = new Set(); for (const x of xs) { if (seen.has(x)) errs.push(`${what}: duplicate ${JSON.stringify(x)}`); seen.add(x); } };
  const regex = (what, p) => { try { new RegExp(p.source, p.flags); } catch (e) { errs.push(`${what}: ${e.message}`); } };
  if (name === 'model-limits') {
    unique('models[].id', d.models.map((m) => m.id));
    d.requestCaps.forEach((c, i) => regex(`requestCaps[${i}]`, c.pattern));
  } else if (name === 'realpeople') {
    for (const [k, p] of Object.entries(d.patterns)) regex(`patterns.${k}`, p);
  } else if (name === 'model-prompts') {
    unique('guides[] kind/family', d.guides.map((g) => `${g.kind}/${g.family}`));
    d.guides.forEach((g, i) => g.match.forEach((m, j) => regex(`guides[${i}].match[${j}]`, { source: m, flags: 'i' })));
    d.enhance.positive.forEach((p, i) => regex(`enhance.positive[${i}]`, p.pattern));
    d.spotFallback.guides.forEach((g, i) => regex(`spotFallback.guides[${i}]`, g.pattern));
    unique('enhance.moves[].id', d.enhance.moves.map((m) => m.id));
  } else if (name === 'styles') {
    unique('styles[].id', d.styles.map((s) => s.id));
    if (!d.styles.some((s) => s.id === d.defaultId)) errs.push(`defaultId ${d.defaultId} is not a style`);
    if (d.styles.some((s) => s.id === 'custom')) errs.push('a style may not use the reserved id "custom"');
  }
  return errs;
}

let schema;
try { schema = JSON.parse(fs.readFileSync(schemaFile, 'utf8')); } catch (e) { fail(`cannot read ${schemaFile}: ${e.message}`); }
const texts = {};
let bad = 0;
for (const [name, data] of Object.entries(out)) {
  if (!schema.$defs?.[name]) fail(`schema.json has no $defs entry for ${name}`);
  texts[name] = JSON.stringify(data, null, 1) + '\n';
  // check the text as it will be read back (JSON drops undefined members and writes NaN / Infinity as null)
  const parsed = JSON.parse(texts[name]);
  let errs = validate(parsed, { $ref: `#/$defs/${name}` }, schema, '$', []);
  if (!errs.length) errs = semantics(name, parsed); // the semantic checks assume the shape the schema guarantees
  for (const e of errs.slice(0, 20)) process.stderr.write(`${name}.json: ${e}\n`);
  if (errs.length) bad++;
}
if (bad) fail(`${bad} file(s) failed the checks; nothing was written`);

// ---------------- write or compare ----------------
const summary = `models ${out['model-limits'].models.length}, public figures ${out.realpeople.publicFigures.length}, guides ${out['model-prompts'].guides.length}, styles ${out.styles.styles.length}`;
if (check) {
  let differs = 0;
  for (const [name, text] of Object.entries(texts)) {
    const file = path.join(target, `${name}.json`);
    // git's text=auto may check the committed files out with CRLF: line ends are not a difference
    const have = fs.existsSync(file) ? fs.readFileSync(file, 'utf8').replace(/\r\n/g, '\n') : null;
    if (have === text) { process.stderr.write(`${name}.json: same\n`); continue; }
    differs++;
    if (have === null) { process.stderr.write(`${name}.json: missing in ${target}\n`); continue; }
    const a = have.split('\n'), b = text.split('\n');
    let i = 0;
    while (i < a.length && i < b.length && a[i] === b[i]) i++;
    process.stderr.write(`${name}.json: differs from line ${i + 1}\n  committed: ${(a[i] ?? '(end)').slice(0, 160)}\n  exported:  ${(b[i] ?? '(end)').slice(0, 160)}\n`);
  }
  process.stderr.write(differs ? `${differs} file(s) differ from ${target}: re-run the export\n` : `all ${Object.keys(texts).length} files match (${summary})\n`);
  process.exit(differs ? 1 : 0);
}
fs.mkdirSync(target, { recursive: true });
for (const [name, text] of Object.entries(texts)) {
  const file = path.join(target, `${name}.json`);
  fs.writeFileSync(file + '.tmp', text);
  fs.renameSync(file + '.tmp', file);
}
process.stderr.write(`wrote ${Object.keys(texts).length} files to ${target} (${summary})\n`);
