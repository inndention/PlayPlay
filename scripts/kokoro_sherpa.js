#!/usr/bin/env node
// kokoro_sherpa.js — Kokoro-82M narration (int8, kokoro-en-v0_19) through sherpa-onnx-node, for spot_tts.py.
// The same engine StarMedia's motion-spots uses (prebuilt N-API addon, CPU, no Python packages, no network).
//
//   node kokoro_sherpa.js job.json
//
// job.json: {"modelDir": "D:/StarMedia/models/kokoro-int8-en-v0_19",
//            "sherpaDir": "D:/StarMedia/motion-spots/node_modules/sherpa-onnx-node",
//            "voice": "am_michael", "speed": 1.0, "threads": 8,
//            "items": [{"text": "...", "out": "voice_00.wav"}, ...]}
// Every item is voiced on its own (one model load), written as 16-bit mono PCM WAV at the model's rate.
// Stdout: one JSON line {"ok": true, "engine": "kokoro", "voiceUsed": "am_michael", "items": [{"path", "durationSec"}]}
//         or {"ok": false, "error": "..."} with exit code 2.
'use strict';
const fs = require('fs');
const path = require('path');

// Speaker ids of kokoro-en-v0_19 (voices.bin order)
const VOICES = { af: 0, af_bella: 1, af_nicole: 2, af_sarah: 3, af_sky: 4, am_adam: 5, am_michael: 6, bf_emma: 7, bf_isabella: 8, bm_george: 9, bm_lewis: 10 };

/** A v0_19 voice name, or the nearest one of the same accent and gender for newer Kokoro names (af_heart, am_echo, ...). */
function pickVoice(want) {
  const w = String(want || '').trim().toLowerCase();
  if (Object.prototype.hasOwnProperty.call(VOICES, w)) return w;
  if (w.startsWith('af')) return 'af_bella';
  if (w.startsWith('am')) return 'am_michael';
  if (w.startsWith('bf')) return 'bf_emma';
  if (w.startsWith('bm')) return 'bm_george';
  return 'am_michael';
}

function writeWav(file, samples, rate) {
  const n = samples.length;
  const buf = Buffer.alloc(44 + n * 2);
  buf.write('RIFF', 0); buf.writeUInt32LE(36 + n * 2, 4); buf.write('WAVE', 8);
  buf.write('fmt ', 12); buf.writeUInt32LE(16, 16); buf.writeUInt16LE(1, 20); buf.writeUInt16LE(1, 22);
  buf.writeUInt32LE(rate, 24); buf.writeUInt32LE(rate * 2, 28); buf.writeUInt16LE(2, 32); buf.writeUInt16LE(16, 34);
  buf.write('data', 36); buf.writeUInt32LE(n * 2, 40);
  let peak = 0;
  for (let i = 0; i < n; i++) { const a = Math.abs(samples[i]); if (a > peak) peak = a; }
  const k = peak > 1 ? 1 / peak : 1;
  for (let i = 0; i < n; i++) buf.writeInt16LE(Math.max(-32768, Math.min(32767, Math.round(samples[i] * k * 32767))), 44 + i * 2);
  fs.mkdirSync(path.dirname(file), { recursive: true });
  fs.writeFileSync(file, buf);
}

async function main() {
  const job = JSON.parse(fs.readFileSync(process.argv[2], 'utf8').replace(/^\uFEFF/, ''));
  const dir = job.modelDir;
  for (const f of ['model.int8.onnx', 'voices.bin', 'tokens.txt', 'espeak-ng-data']) {
    if (!fs.existsSync(path.join(dir, f))) throw new Error(`Kokoro model incomplete: ${f} missing in ${dir}`);
  }
  const sherpa = require(job.sherpaDir);
  const config = {
    model: {
      kokoro: { model: path.join(dir, 'model.int8.onnx'), voices: path.join(dir, 'voices.bin'), tokens: path.join(dir, 'tokens.txt'), dataDir: path.join(dir, 'espeak-ng-data') },
      numThreads: job.threads || 8, provider: 'cpu', debug: false,
    },
    maxNumSentences: 1,
  };
  const tts = typeof sherpa.OfflineTts.createAsync === 'function' ? await sherpa.OfflineTts.createAsync(config) : new sherpa.OfflineTts(config);
  const voice = pickVoice(job.voice);
  const speed = Math.max(0.5, Math.min(2, Number(job.speed) || 1));
  const items = [];
  for (const it of job.items || []) {
    const req = { text: String(it.text || ''), sid: VOICES[voice], speed };
    const a = typeof tts.generateAsync === 'function' ? await tts.generateAsync({ ...req, onProgress: () => 1 }) : tts.generate(req);
    if (!a || !a.samples || !a.samples.length) throw new Error(`no audio for: ${req.text.slice(0, 60)}`);
    writeWav(it.out, a.samples, a.sampleRate);
    items.push({ path: it.out, durationSec: +(a.samples.length / a.sampleRate).toFixed(3) });
  }
  process.stdout.write(JSON.stringify({ ok: true, engine: 'kokoro', voiceUsed: voice, items }) + '\n');
}

main().catch((e) => {
  process.stdout.write(JSON.stringify({ ok: false, error: String((e && e.message) || e).split('\n')[0] }) + '\n');
  process.exit(2);
});
