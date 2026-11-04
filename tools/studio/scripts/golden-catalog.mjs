// Dev-time generator of tests/studio/fixtures/catalog-golden.json (ST-08): what the REAL StarMedia catalog, pricing and
// selector code (src/catalog.ts, src/pricing.ts, src/selector.ts) answers for fixed inputs, so test_studio_catalog
// can hold the C++ port to it value by value. Nothing is copied by hand: a change in the TS shows up as a diff of
// the regenerated file (the test also checks the three files' hashes against the checkout).
//
// Usage (Node 22+, no packages needed):
//   node --experimental-transform-types tools/studio/scripts/golden-catalog.mjs [starmedia root] [out file]
// Defaults: $STARMEDIA_ROOT (or the first argument) and <this repo>/tests/studio/fixtures/catalog-golden.json. The clock is
// pinned to REFERENCE_NOW (the same instant tests/studio/fixtures/models.json is rebased to) before the TS test mock
// starts, so the mock's `created` stamps, the quality scores (recency) and the expiry checks are all deterministic.
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { execFileSync } from 'node:child_process';
import { register } from 'node:module';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(process.argv[2] || process.env.STARMEDIA_ROOT || (console.error('pass the starmedia checkout as the first argument or set STARMEDIA_ROOT'), process.exit(2)));
const outFile = path.resolve(process.argv[3] || path.join(here, '..', '..', '..', 'tests', 'studio', 'fixtures', 'catalog-golden.json'));

const REFERENCE_NOW = Date.UTC(2026, 9, 26) / 1000; // 2026-10-26T00:00:00Z, as export-mock-fixture.mjs
Date.now = () => REFERENCE_NOW * 1000;             // before the mock and the catalog load (created stamps, recency, expiry)

const FILES = ['src/catalog.ts', 'src/pricing.ts', 'src/selector.ts'];
const sha = (p) => crypto.createHash('sha256').update(fs.readFileSync(path.join(root, p), 'utf8').replace(/\r\n/g, '\n')).digest('hex');
let head = 'unknown';
try { head = execFileSync('git', ['-C', root, 'log', '-1', '--format=%h %cs', '--', ...FILES], { encoding: 'utf8' }).trim(); } catch { /* not a checkout */ }

// The repo imports its TypeScript siblings as './x.js' (tsc output names): load './x.ts' instead (the same hook as
// export-starmedia-data.mjs). catalog.ts has a parameter property, so this needs --experimental-transform-types.
async function resolve(specifier, context, nextResolve) {
  try { return await nextResolve(specifier, context); } catch (e) {
    const relative = specifier.startsWith('./') || specifier.startsWith('../');
    if (e?.code !== 'ERR_MODULE_NOT_FOUND' || !context.parentURL || !relative || !specifier.endsWith('.js')) throw e;
    return nextResolve(new URL(specifier.slice(0, -3) + '.ts', context.parentURL).href, context);
  }
}
register('data:text/javascript,' + encodeURIComponent(`export ${resolve}\n`));

const mod = (p) => import(pathToFileURL(path.join(root, p)).href);
const mockMod = await mod('test/mock.ts');
const mock = await mockMod.startMock();
process.env.OPENROUTER_BASE_URL = mock.url;
process.env.OPENROUTER_API_KEY = 'test-key';

const C = await mod('src/catalog.ts');
const P = await mod('src/pricing.ts');
const S = await mod('src/selector.ts');

const plain = (v) => JSON.parse(JSON.stringify(v, (k, x) => (x === Infinity ? null : x)));

try {
  // ---------------- the catalog the TS builds from the mock ----------------
  const cat = await C.loadCatalog({ refresh: true });
  const byId = (id) => { const m = cat.byId.get(id); if (!m) throw new Error(`fixture model ${id} missing`); return m; };
  const summary = C.summarize(cat);

  // ---------------- hand-made models (ModelInfo objects the C++ reads back with fromJson) ----------------
  const base = { created: REFERENCE_NOW - 30 * 86400, ctx: 0, inMods: ['text'], outMods: ['text'], free: false, params: [], quality: 0.5 };
  const text = (id, o = {}) => ({ ...base, id, name: o.name ?? id, kind: 'text', ctx: 200_000, price: { in: 1e-6, out: 5e-6 }, params: ['tools', 'structured_outputs', 'response_format', 'max_tokens'], ...o });
  const video = (id, o = {}) => ({ ...base, id, name: o.name ?? id, kind: 'video', inMods: ['text', 'image'], outMods: ['video'], price: { in: 0, out: o.out ?? 0 }, free: !!o.free, quality: o.quality ?? 0.5, expires: o.expires,
    video: { durations: [5, 10], resolutions: ['480p', '720p', '1080p'], aspects: ['9:16', '16:9'], audio: false, frames: ['first_frame'], skus: o.skus ?? { duration_seconds: '0.1' }, edit: o.edit } });
  const image = (id, spec, o = {}) => ({ ...base, id, name: id, kind: 'image', outMods: ['image'], price: { in: 0, out: 0 }, image: spec === null ? undefined : { aspects: ['1:1'], qualities: ['low', 'high'], maxN: 4, maxRefs: 2, streaming: false, ...spec }, ...o });
  const speech = (id, o = {}) => ({ ...base, id, name: id, kind: 'speech', outMods: ['speech'], price: { in: 0.000015, out: 0 }, voices: ['alloy'], ...o });
  const stt = (id, price) => ({ ...base, id, name: id, kind: 'stt', inMods: ['audio'], outMods: ['transcription'], price });
  const embed = (id, price) => ({ ...base, id, name: id, kind: 'embed', outMods: ['embeddings'], price });
  const rerank = (id, price) => ({ ...base, id, name: id, kind: 'rerank', outMods: ['rerank'], price });
  const local = (m, throughput) => ({ ...m, id: m.id.replace(/^[^/]+\//, 'local/'), name: m.id.replace(/^[^/]+\//, 'local/'), provider: 'local', free: false, price: { in: 0, out: 0 }, quality: 0.38,
    local: { backend: 'test', verified: false, ...(throughput ? { throughput } : {}) } });

  const M = {
    nativeSearch: text('openai/gpt-x', { price: { in: 2e-6, out: 8e-6, webSearch: 0.02 } }),
    nativeUnlisted: text('anthropic/claude-x', { price: { in: 3e-6, out: 1.5e-5 } }),
    reasonOn: text('acme/thinker', { price: { in: 1e-6, out: 4e-6 }, reasoning: { efforts: ['low', 'medium', 'high'], mandatory: false, defaultEnabled: true } }),
    reasonMandatory: text('acme/always', { reasoning: { efforts: [], mandatory: true, defaultEnabled: true } }),
    reasonNone: text('acme/optional', { reasoning: { efforts: ['none', 'low', 'xhigh'], mandatory: false, defaultEnabled: false } }),
    reasonOdd: text('acme/odd', { reasoning: { efforts: ['none', 'weird'], mandatory: false, defaultEnabled: false } }),
    localText: local(text('acme/qwen')),
    localTextFast: local(text('acme/qwen'), { tokensPerSec: 40 }),
    imgMp: image('acme/img-mp', { perMegapixel: 0.02 }),
    imgTok: image('acme/img-tok', { perOutputToken: 0.00003, perInputToken: 0.000005 }),
    imgOutTok: image('acme/img-outtok', { perOutputToken: 0.00004 }),
    imgUnknown: image('acme/img-unknown', {}),
    imgUnknownFree: image('acme/img-unknown:free', {}, { free: true }),
    imgNoSpec: image('acme/img-nospec', null),
    imgNoSpecFree: image('acme/img-nospec:free', null, { free: true }),
    localImg: local(image('acme/img', { perImage: 0 })),
    localImgFast: local(image('acme/img', { perImage: 0 }), { secondsPerImage: 20 }),
    vidCents: video('acme/vid-cents', { skus: { cents_per_second_output_720p: '5', cents_per_second_output: '4' } }),
    vidTokens: video('acme/vid-tokens', { skus: { video_tokens_1080p: '0.0000002', video_tokens: '0.0000001' } }),
    vidAudio: video('acme/vid-audio', { skus: { duration_seconds_with_audio: '0.2', duration_seconds_without_audio: '0.1' } }),
    vidMin: video('acme/vid-min', { skus: { duration_seconds: '0.15', minimum_cents_per_generation: '50' } }),
    vidMinZero: video('acme/vid-min0', { skus: { duration_seconds: '0.15', minimum_cents_per_generation: '0' } }),
    vidAny: video('acme/vid-any', { skus: { price_per_second_x: '0.3' } }),
    vidNone: video('acme/vid-none', { skus: {} }),
    vidText: video('acme/vid-t2v', { skus: { text_to_video_duration_seconds_720p: '0.12' } }),
    localVid: local(video('acme/vid', { skus: { duration_seconds: '0' } })),
    localVidFast: local(video('acme/vid', { skus: { duration_seconds: '0' } }), { secondsPerVideoSecond: 30 }),
    ttsTok: speech('acme/tts-tok', { price: { in: 0.000001, out: 0.00001 }, voices: ['a'] }),
    localTts: local(speech('acme/tts')),
    localTtsFast: local(speech('acme/tts'), { secondsPerSpokenSecond: 0.1 }),
    sttHour: stt('acme/stt-hour', { in: 0.36, out: 0 }),
    sttTok: stt('acme/stt-tok', { in: 0.000001, out: 0.00001 }),
    localStt: local(stt('acme/stt', { in: 0, out: 0 })),
    localSttFast: local(stt('acme/stt', { in: 0, out: 0 }), { secondsPerAudioMinute: 4 }),
    embedPaid: embed('acme/embed-x', { in: 0.00000002, out: 0 }),
    localEmbed: local(embed('acme/embed', { in: 0, out: 0 })),
    localEmbedFast: local(embed('acme/embed', { in: 0, out: 0 }), { tokensPerSec: 300 }),
    rerankPaid: rerank('acme/rerank-x', { in: 0.000001, out: 0 }),
    localRerank: local(rerank('acme/rerank', { in: 0, out: 0 })),
    localRerankFast: local(rerank('acme/rerank', { in: 0, out: 0 }), { tokensPerSec: 300 }),
  };
  const model = (ref) => (typeof ref === 'string' ? byId(ref) : ref.id ? byId(ref.id) : M[ref.hand]);
  const ref = (name) => ({ hand: name });

  // ---------------- pricing cases ----------------
  const pricing = [];
  const pc = (fn, m, args, result) => pricing.push({ fn, model: typeof m === 'string' ? { id: m } : { hand: m.hand }, args, result: plain(result) });
  const textIds = ['acme/writer-pro', 'beta/writer-2', 'epsilon/deep', 'gamma/fast-mini', 'zeta/lite', 'delta/free-model:free'];
  const textArgs = [[1000, 500, {}], [12345, 2000, { effort: 'high' }], [1000, 500, { effort: 'none' }], [800, 400, { searches: 3 }],
    [800, 400, { searches: 2, searchResults: 15 }], [1000, 500, { reasoningIncluded: true, effort: 'high' }], [0, 0, {}], [250000, 100000, { effort: 'max' }]];
  for (const id of textIds) for (const a of textArgs) pc('textCost', id, a, P.textCost(byId(id), ...a));
  for (const h of ['nativeSearch', 'nativeUnlisted', 'reasonOn', 'reasonMandatory', 'reasonNone', 'reasonOdd', 'localText', 'localTextFast'])
    for (const a of [[1000, 500, {}], [1000, 500, { effort: 'high' }], [1000, 500, { effort: 'none' }], [800, 400, { searches: 1 }], [800, 400, { searches: 2, searchResults: 25 }]])
      pc('textCost', ref(h), a, P.textCost(M[h], ...a));
  for (const id of ['acme/img-1', 'beta/img-free']) for (const o of [{ n: 1, quality: 'auto' }, { n: 3, quality: 'high' }, { n: 2, quality: 'low' }]) pc('imageCost', id, [o], P.imageCost(byId(id), o));
  for (const h of ['imgMp', 'imgTok', 'imgOutTok', 'imgUnknown', 'imgUnknownFree', 'imgNoSpec', 'imgNoSpecFree', 'localImg', 'localImgFast'])
    for (const o of [{ n: 1, quality: 'auto' }, { n: 4, quality: 'high' }, { n: 2, quality: 'low' }, { n: 1, quality: 'medium' }, { n: 1, quality: 'odd' }])
      pc('imageCost', ref(h), [o], P.imageCost(M[h], o));
  const vidArgs = [{ seconds: 4, resolution: '720p', audio: false }, { seconds: 8, resolution: '480p', audio: true }, { seconds: 2.5, resolution: '720P', audio: true },
    { seconds: 10, resolution: '1080p', audio: false }, { seconds: 5, resolution: '4k', audio: false }, { seconds: 1, resolution: '720p', audio: false }];
  for (const a of vidArgs.slice(0, 3)) pc('videoCost', 'acme/vid-1', [a], P.videoCost(byId('acme/vid-1'), a));
  for (const h of ['vidCents', 'vidTokens', 'vidAudio', 'vidMin', 'vidMinZero', 'vidAny', 'vidNone', 'vidText', 'localVid', 'localVidFast'])
    for (const a of vidArgs) pc('videoCost', ref(h), [a], P.videoCost(M[h], a));
  for (const [res, audio] of [['720p', false], ['720p', true], ['1080p', false], ['abc', false]]) {
    // acme/vid-1 lists only per-resolution keys: another resolution falls to the key scan, whose listing order the C++ cannot keep (catalog.h)
    for (const id of res === '720p' ? ['acme/vid-1'] : []) pricing.push({ fn: 'videoRate', model: { id }, args: [res, audio], result: plain(P.videoRate(byId(id), res, audio) ?? null) });
    for (const h of ['vidCents', 'vidTokens', 'vidAudio', 'vidMin', 'vidAny', 'vidNone', 'vidText']) pricing.push({ fn: 'videoRate', model: ref(h), args: [res, audio], result: plain(P.videoRate(M[h], res, audio) ?? null) });
  }
  for (const id of ['acme/tts-1', 'acme/tts-clone', 'acme/audio-gen', 'beta/tts-free:free']) for (const c of [0, 1234, 50000]) pc('ttsCost', id, [c], P.ttsCost(byId(id), c));
  for (const h of ['ttsTok', 'localTts', 'localTtsFast']) for (const c of [1234, 50000]) pc('ttsCost', ref(h), [c], P.ttsCost(M[h], c));
  for (const s of [0, 90.4, 3600]) pc('sttCost', 'acme/stt-1', [s], P.sttCost(byId('acme/stt-1'), s));
  for (const h of ['sttHour', 'sttTok', 'localStt', 'localSttFast']) for (const s of [90.4, 3600]) pc('sttCost', ref(h), [s], P.sttCost(M[h], s));
  for (const t of [0, 12345, 1000000]) pc('embedCost', 'acme/embed-1', [t], P.embedCost(byId('acme/embed-1'), t));
  for (const h of ['embedPaid', 'localEmbed', 'localEmbedFast']) for (const t of [12345]) pc('embedCost', ref(h), [t], P.embedCost(M[h], t));
  pc('rerankCost', 'acme/rerank-1', [], P.rerankCost(byId('acme/rerank-1')));
  for (const h of ['rerankPaid', 'localRerank', 'localRerankFast']) pc('rerankCost', ref(h), [], P.rerankCost(M[h]));

  // ---------------- small helpers ----------------
  const misc = {
    fmtUsd: [0, 0.0001234, 0.00005, 0.005, 0.0099999, 0.01, 0.0125, 0.125, 0.5, 0.999, 0.9995, 1, 1.005, 1.5, 12.345, 1234.5678, 2.675, 0.045, 1e-7].map((v) => [v, P.fmtUsd(v)]),
    fmtSeconds: [0, 0.2, 1, 12, 89.9, 90, 150, 600, 3599, 100000].map((v) => [v, P.fmtSeconds(v)]),
    fmtSecondsSpecial: { inf: P.fmtSeconds(Infinity), nan: P.fmtSeconds(NaN) },
    approxTokens: [0, 1, 36, 37, 1000, 123456].map((v) => [v, P.approxTokens(v)]),
    chooseEffort: [],
    searchRequestUsd: [],
    variantLean: [['acme/vid-turbo'], ['google/gemini-2.5-flash'], ['openai/gpt-4o-mini'], ['google/gemini-2.5-flash-lite'], ['black-forest-labs/flux-1-schnell'],
      ['openai/gpt-5-nano'], ['google/veo-3.1-fast'], ['acme/vid-pro'], ['google/gemini-2.5-pro'], ['openai/tts-1-hd'], ['kwaivgi/kling-v2.1-master-pro'],
      ['qwen/qwen3-max'], ['mistralai/mistral-large'], ['openai/sora-2-pro'], ['google/gemini-x'], ['minimax/hailuo-02', 'MiniMax: Hailuo 02'],
      ['fastco/model-a', 'Fastco: Model A'], ['acme/improved'], ['acme/v2', 'Acme: V2 Turbo'], ['openai/whisper-large-v3-turbo'],
      ['acme/vid-pro', 'Acme: Vid Turbo'], ['acme/vid-lite', 'Acme: Vid Max'], ['openai/whisper-large-v3-turbo', 'OpenAI: Whisper Pro'],
      ['google/gemini-3-pro-image', 'Google: Nano Banana Pro (Gemini 3 Pro Image)'], ['google/gemini-3.1-flash-image', 'Google: Nano Banana 2 (Gemini 3.1 Flash Image)'],
      ['google/banana-x', 'Google: Nano Banana X'], ['openai/gpt-5-nano', 'OpenAI: GPT-5 Nano'], ['noslash'], ['', ''], ['acme/', 'Acme: Pro Max Ultra'],
      ['ACME/VID-TURBO'], ['acme/x', 'Acme Turbo'], ['acme/x', 'acme: turbo: pro']].map(([id, name]) => [id, name ?? id, S.variantLean({ id, name: name ?? id })]),
    speedBias: [],
    tierFor: [],
    qualityWeight: [],
  };
  const efforts = ['none', 'minimal', 'low', 'medium', 'high', 'xhigh', 'max', 'weird', ''];
  for (const id of textIds) for (const e of efforts) misc.chooseEffort.push([{ id }, e, P.chooseEffort(byId(id), e) ?? null]);
  for (const h of ['reasonOn', 'reasonMandatory', 'reasonNone', 'reasonOdd', 'localText']) for (const e of efforts) misc.chooseEffort.push([ref(h), e, P.chooseEffort(M[h], e) ?? null]);
  for (const m of [{ id: 'acme/writer-pro' }, ref('nativeSearch'), ref('nativeUnlisted'), ref('localText')]) for (const r of [undefined, 1, 5, 10, 11, 25, 0])
    misc.searchRequestUsd.push([m, r ?? null, r === undefined ? P.searchRequestUsd(model(m)) : P.searchRequestUsd(model(m), r)]);
  const opts = (o = {}) => ({ objective: 'story-video', sliders: { cost: 60, complexity: 50 }, brief: { topic: 'golden' }, ...o });
  const optsJson = (o) => { const { objective, brief, ...rest } = o; return rest; };
  const dialOpts = [opts(), opts({ speed: { video: 50 } }), opts({ speed: { video: 100 } }), opts({ speed: { video: 0 } }), opts({ speed: { text: 0 } }), opts({ speed: {} }),
    opts({ tiers: { video: 0 }, speed: { video: 100 } }), opts({ tiers: { video: 50 }, speed: { video: 100 } }), opts({ tiers: { video: 100 }, speed: { video: 100 } }),
    opts({ tiers: {}, speed: { video: 100 } }), opts({ tiers: { video: 150 } }), opts({ tiers: { video: -5 }, speed: { video: 200 } }), opts({ sliders: { cost: 0, complexity: 50 } }),
    opts({ sliders: { cost: 100, complexity: 50 }, tiers: { text: 25 } })];
  const dialModels = [video('acme/vid-pro'), video('acme/vid-turbo'), video('acme/vid-1'), text('acme/writer-lite'), speech('acme/tts-1-hd'), image('acme/img-max', { perImage: 0.1 })];
  for (const o of dialOpts) {
    for (const m of dialModels) misc.speedBias.push([optsJson(o), m.id, S.speedBias(o, m)]);
    for (const k of ['text', 'image', 'video', 'speech', 'stt', 'embed', 'rerank']) { misc.tierFor.push([optsJson(o), k, S.tierFor(o, k)]); misc.qualityWeight.push([optsJson(o), k, S.qualityWeight(o, k)]); }
  }

  // ---------------- selector cases ----------------
  // estimators the C++ test can rebuild: {kind:'flat', usd} | {kind:'priced', map} | the pricing estimators
  const est = (spec) => {
    switch (spec.kind) {
      case 'flat': return () => ({ usd: spec.usd, confidence: 'exact', detail: '' });
      case 'priced': return (m) => ({ usd: spec.map[m.id] ?? spec.other ?? 1, confidence: 'exact', detail: '' });
      case 'text': return (m) => P.textCost(m, spec.in, spec.out, spec.opts ?? {});
      case 'image': return (m) => P.imageCost(m, { n: spec.n, quality: spec.quality });
      case 'video': return (m) => P.videoCost(m, { seconds: spec.seconds, resolution: spec.resolution, audio: spec.audio });
      case 'tts': return (m) => P.ttsCost(m, spec.chars);
      case 'stt': return (m) => P.sttCost(m, spec.seconds);
      case 'embed': return (m) => P.embedCost(m, spec.tokens);
      case 'rerank': return (m) => P.rerankCost(m);
      default: throw new Error(`estimator ${spec.kind}`);
    }
  };
  const catalogs = {
    fixture: cat.models,
    fixtureLocal: [...cat.models, M.localText, M.localTextFast === undefined ? undefined : { ...M.localTextFast, id: 'local/qwen-fast', name: 'local/qwen-fast' }, M.localImg, M.localTts].filter(Boolean),
    dials: [video('acme/vid-turbo'), video('acme/vid-pro')],
    dialsRev: [video('acme/vid-pro'), video('acme/vid-turbo')],
    strong: [video('acme/vid-turbo', { quality: 0.2 }), video('beta/vid-x', { quality: 0.8 })],
    freeVariants: [video('beta/vid-x', { quality: 0.8, free: true }), video('acme/vid-max', { quality: 0.47, free: true }), video('acme/vid-turbo', { quality: 0.47, free: true }), video('acme/vid-pro', { quality: 0.47, free: true })],
    reviewer: [video('acme/vid-max', { quality: 0.45, out: 0.1 }), video('beta/vid-x', { quality: 0.85, out: 0.11 }), ...Array.from({ length: 8 }, (_, i) => video(`zeta/vid-edit-${i}`, { edit: true, out: 1 }))],
    eligibility: [video('acme/vid-turbo'), video('acme/vid-pro-edit', { edit: true, quality: 0.9 }), video('beta/vid-ultra', { quality: 0.9, expires: REFERENCE_NOW + 86400 }),
      video('gamma/vid-soon', { quality: 0.95, expires: REFERENCE_NOW + 8 * 86400 }), video('openai/sora-2-pro', { quality: 0.99 }), video('delta/vid-free:free', { quality: 0.6, free: true })],
    writers: [text('acme/writer-lite'), text('acme/writer-pro')],
    tts: [speech('acme/tts-1-hd'), speech('acme/tts-1')],
    costQuality: [video('beta/vid-capable', { quality: 0.9, out: 1 }), video('acme/vid-cheap', { quality: 0.3, out: 0.1 })],
    providers: [text('acme/a1', { quality: 0.9 }), text('acme/a2', { quality: 0.85 }), text('beta/b1', { quality: 0.8 }), text('gamma/c1', { quality: 0.7 }), text('beta/b2', { quality: 0.6 })],
    dup: [video('acme/dup', { quality: 0.1 }), video('acme/dup', { quality: 0.9 })],
  };
  const ctxOf = (name, o, freeOnly = false) => ({ catalog: C.wrap(REFERENCE_NOW * 1000, catalogs[name]), opts: o, freeOnly });
  const selector = [];
  const sc = (fn, catalog, o, freeOnly, args, result) => selector.push({ fn, catalog, opts: optsJson(o), freeOnly, args, result: plain(result ?? null) });
  const pickOut = (p) => (p ? { model: p.model.id, estimate: p.estimate, fallbacks: p.fallbacks, pinned: !!p.pinned } : null);
  const roles = ['writer', 'editor', 'judge', 'reasoner', 'research', 'fast', 'vision', 'image', 'imageEdit', 'video', 'videoI2V', 'videoEdit', 'tts', 'ttsClone', 'stt', 'embed', 'rerank'];
  const eligOpts = [[opts(), false], [opts(), true], [opts({ allowFree: true }), false], [opts({ allowFree: true, denyDataCollection: true }), false], [opts({ exclude: ['acme'] }), false],
    [opts({ exclude: ['acme', 'zeta'], allowFree: true }), false], [opts({ local: 'off' }), false], [opts({ local: 'only' }), false], [opts({ local: 'prefer' }), false], [opts({ local: 'only' }), true]];
  for (const [o, fo] of eligOpts) for (const name of ['fixture', 'fixtureLocal']) for (const role of roles) for (const json of [false, true])
    sc('eligible', name, o, fo, [role, { json }], S.eligible(role, ctxOf(name, o, fo), { json }).map((m) => m.id));
  // pickModel: dials.ts cases
  const T = { video: 50 };
  const pm = (name, role, o, e, allowance = 10, po = {}, fo = false) => sc('pickModel', name, o, fo, [role, allowance, e, po], pickOut(S.pickModel(role, ctxOf(name, o, fo), allowance, est(e), {
    json: po.json, exclude: po.exclude ? new Set(po.exclude) : undefined, distinctProviderFrom: po.distinctProviderFrom ? new Set(po.distinctProviderFrom) : undefined, secondsLeft: po.secondsLeft })));
  const flat = (usd) => ({ kind: 'flat', usd });
  const priced = (map) => ({ kind: 'priced', map });
  for (const name of ['dials', 'dialsRev']) {
    for (const o of [opts({ tiers: T, speed: { video: 0 } }), opts({ tiers: T, speed: { video: 100 } }), opts({ tiers: T }), opts({ tiers: T, speed: { video: 50 } }),
      opts({ tiers: T, speed: { text: 0, image: 0, audio: 0 } }), opts({ tiers: T, speed: {} }), opts({ sliders: { cost: 30, complexity: 50 }, speed: { video: 0 } }),
      opts({ sliders: { cost: 30, complexity: 50 }, speed: { video: 100 } }), opts({ tiers: T, speed: { video: 100 }, exclude: ['vid-pro'] }), opts({ tiers: T, speed: { video: 100 }, pins: { video: 'acme/vid-turbo' } }),
      opts({ tiers: T, speed: { video: 100 }, pins: { video: 'nope/x' } }), opts({ tiers: T, pins: { video: 'acme/vid-pro' }, exclude: ['vid-pro'] })])
      pm(name, 'video', o, flat(0.2));
    pm(name, 'video', opts({ tiers: T, speed: { video: 100 } }), priced({ 'acme/vid-turbo': 0.3, 'acme/vid-pro': 1 }), 0.5);
    pm(name, 'video', opts({ tiers: T, speed: { video: 100 } }), priced({ 'acme/vid-turbo': 0.3, 'acme/vid-pro': 1 }), 1);
    pm(name, 'video', opts({ tiers: T, speed: { video: 0 } }), flat(2), 1);
    pm(name, 'video', opts({ tiers: T }), flat(0.2), 10, { exclude: ['acme/vid-turbo'] });
    pm(name, 'video', opts({ tiers: T }), flat(0.2), 10, { distinctProviderFrom: ['acme'] });
    pm(name, 'image', opts(), flat(0.2));
  }
  pm('strong', 'video', opts({ tiers: T, speed: { video: 0 } }), flat(0.2));
  for (const t of [0, 25, 50, 75, 100]) for (const s of [0, 100]) pm('freeVariants', 'video', opts({ tiers: { video: t }, speed: { video: s }, allowFree: true }), flat(0.2));
  for (const s of [50, 100]) pm('reviewer', 'video', opts({ tiers: { video: 0 }, speed: { video: s } }), flat(0.2));
  for (const o of [opts({ tiers: T, speed: { video: 100 } }), opts({ allowFree: true }), opts({ allowFree: true, denyDataCollection: true }), opts()]) pm('eligibility', 'video', o, flat(0.2));
  pm('eligibility', 'video', opts(), flat(0.2), 10, {}, true);
  pm('eligibility', 'video', opts({ pins: { video: 'openai/sora-2-pro' } }), flat(0.2));
  for (const o of [{ plan: 0 }, { plan: 100 }]) {
    const tiers = { text: 55, image: 55, video: 50, audio: 55 }, speed = { text: o.plan, image: 50, video: 50, audio: 50 };
    pm('writers', 'writer', opts({ tiers, speed }), flat(0.2));
  }
  for (const a of [0, 100]) pm('tts', 'tts', opts({ tiers: { text: 55, image: 55, video: 50, audio: 55 }, speed: { text: 50, image: 50, video: 50, audio: a } }), flat(0.2));
  const cq = priced({ 'acme/vid-cheap': 0.5, 'beta/vid-capable': 2 });
  for (const q of [0, 100]) for (const al of [10, 1]) pm('costQuality', 'video', opts({ tiers: { text: 55, image: 55, video: q, audio: 55 } }), cq, al);
  for (const o of [opts(), opts({ sliders: { cost: 100, complexity: 50 } }), opts({ sliders: { cost: 10, complexity: 50 } })]) {
    pm('providers', 'writer', o, flat(0.1));
    pm('providers', 'writer', o, flat(0.1), 10, { distinctProviderFrom: ['acme'] });
    pm('providers', 'writer', o, flat(0.1), 10, { distinctProviderFrom: ['acme', 'beta', 'gamma'] });
    pm('providers', 'writer', o, flat(0.1), 10, { exclude: ['acme/a1', 'acme/a2'] });
    pm('providers', 'fast', o, priced({ 'acme/a1': 5, 'acme/a2': 0.05, 'beta/b1': 0.04, 'gamma/c1': 0.5, 'beta/b2': 0.01 }), 0.1, { json: true });
  }
  pm('dup', 'video', opts(), flat(0.2));
  // the fixture catalog with real estimators, pins, free-only and local modes
  const fixOpts = [opts(), opts({ sliders: { cost: 100, complexity: 50 } }), opts({ sliders: { cost: 0, complexity: 50 } }), opts({ allowFree: true }), opts({ pins: { writer: 'zeta/lite', image: 'beta/img-free', tts: 'acme/audio-gen' } }),
    opts({ pins: { writer: 'acme/img-1' } }), opts({ exclude: ['acme'] }), opts({ tiers: { text: 0, image: 0, video: 0, audio: 0 } }), opts({ tiers: { text: 100, image: 100, video: 100, audio: 100 } }),
    opts({ local: 'off' }), opts({ local: 'only' }), opts({ local: 'prefer' }), opts({ local: 'prefer', sliders: { cost: 0, complexity: 50 } })];
  const fixEst = { writer: { kind: 'text', in: 3000, out: 1500 }, judge: { kind: 'text', in: 20000, out: 2000, opts: { effort: 'high' } }, fast: { kind: 'text', in: 500, out: 200 },
    research: { kind: 'text', in: 2000, out: 1000, opts: { searches: 3 } }, image: { kind: 'image', n: 2, quality: 'high' }, imageEdit: { kind: 'image', n: 1, quality: 'auto' },
    video: { kind: 'video', seconds: 8, resolution: '720p', audio: true }, videoI2V: { kind: 'video', seconds: 4, resolution: '480p', audio: false }, tts: { kind: 'tts', chars: 2000 },
    ttsClone: { kind: 'tts', chars: 2000 }, stt: { kind: 'stt', seconds: 600 }, embed: { kind: 'embed', tokens: 5000 }, rerank: { kind: 'rerank' }, vision: { kind: 'text', in: 1000, out: 300 } };
  for (const name of ['fixture', 'fixtureLocal']) for (const o of fixOpts) for (const [role, e] of Object.entries(fixEst)) {
    const fo = o.sliders.cost === 0;
    for (const allowance of [10, 0.001]) pm(name, role, o, e, allowance, {}, fo);
    pm(name, role, o, e, 10, { secondsLeft: 20 }, fo);
    sc('cheapestModel', name, o, fo, [role, e, {}], pickOut(S.cheapestModel(role, ctxOf(name, o, fo), est(e), {})));
    sc('cheapestModel', name, o, fo, [role, e, { secondsLeft: 20 }], pickOut(S.cheapestModel(role, ctxOf(name, o, fo), est(e), { secondsLeft: 20 })));
    sc('cheapestModel', name, o, fo, [role, e, { json: true }], pickOut(S.cheapestModel(role, ctxOf(name, o, fo), est(e), { json: true })));
  }
  // pinnedPick / pinIneligible
  const pinCases = [['image', 'acme/img-1', ['beta/img-free', 'acme/img-1', 'acme/vid-1', 'nope/x', 'beta/img-free']], ['image', 'nope/x', []], ['image', 'acme/vid-1', []], ['video', 'acme/vid-1', ['acme/img-1']],
    ['writer', 'delta/free-model:free', ['acme/writer-pro']], ['tts', 'acme/audio-gen', ['acme/tts-1']], ['ttsClone', 'acme/tts-1', []], ['ttsClone', 'acme/tts-clone', ['acme/tts-1', 'beta/tts-free:free']],
    ['embed', 'acme/embed-1', []], ['rerank', 'acme/rerank-1', []], ['videoEdit', 'acme/vid-1', []], ['writer', 'local/qwen', ['acme/writer-pro']]];
  for (const name of ['fixture', 'fixtureLocal', 'eligibility']) for (const [o, fo] of [[opts(), false], [opts(), true], [opts({ denyDataCollection: true }), false], [opts({ exclude: ['img-1', 'tts'] }), false], [opts({ local: 'off' }), false]])
    for (const [role, id, fb] of [...pinCases, ['video', 'openai/sora-2-pro', []], ['video', 'beta/vid-ultra', ['gamma/vid-soon', 'acme/vid-turbo']], ['video', 'delta/vid-free:free', []], ['image', 'acme/vid-pro-edit', []], ['videoEdit', 'acme/vid-pro-edit', []]]) {
      const ctx = ctxOf(name, o, fo);
      const r = S.pinnedPick(role, ctx, id, est(fixEst[role] ?? { kind: 'flat', usd: 0.2 }), fb);
      sc('pinnedPick', name, o, fo, [role, id, fixEst[role] ?? { kind: 'flat', usd: 0.2 }, fb], { pick: pickOut(r.pick), reason: r.reason ?? null });
      sc('pinIneligible', name, o, fo, [role, id], S.pinIneligible(role, ctx, ctx.catalog.byId.get(id), id) ?? null);
    }

  const golden = {
    _comment: 'Generated by tools/studio/scripts/golden-catalog.mjs from the StarMedia TypeScript (catalog / pricing / selector) against its test mock; do not edit by hand. test_studio_catalog holds the C++ port to these values.',
    source: { repo: 'starmedia', commit: head, files: FILES.map((p) => ({ path: p, sha256: sha(p) })) },
    reference_now: REFERENCE_NOW,
    catalog: { fetchedAt: cat.fetchedAt, models: plain(cat.models), summary },
    hand: plain(M),
    catalogs: plain(Object.fromEntries(Object.entries(catalogs).filter(([k]) => k !== 'fixture' && k !== 'fixtureLocal'))),
    fixtureLocalExtra: plain(catalogs.fixtureLocal.slice(cat.models.length)),
    pricing,
    misc,
    selector,
  };
  fs.mkdirSync(path.dirname(outFile), { recursive: true });
  fs.writeFileSync(outFile, JSON.stringify(golden, null, 1) + '\n');
  console.error(`wrote ${outFile}: ${cat.models.length} models, ${pricing.length} pricing cases, ${selector.length} selector cases`);
} finally {
  await mock.close();
}
