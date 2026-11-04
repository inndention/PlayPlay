#!/usr/bin/env python
"""
PLAY PLAY transcription worker: faster-whisper (CTranslate2) with word timestamps.

    python -I scripts/transcribe.py --input <media> --output <json> [--level fast|balanced|accurate|best]
                                    [--model <name>] [--language en] [--device auto|cpu|cuda] [--compute int8|float16|...]
                                    [--vad 0|1] [--beam N] [--initial-prompt "..."]

Prints progress lines "PROGRESS <seconds_done> <total_seconds>" to stderr; writes the transcript JSON:
  { "language", "languageProbability", "duration", "model", "level", "device", "computeType",
    "segments": [ { "id", "start", "end", "text", "avgLogprob", "noSpeechProb",
                    "words": [ { "word", "start", "end", "probability" } ] } ],
    "text" }
Accuracy levels (model / beam / VAD):
  fast      base.en|base      beam 1   no VAD    (seconds per minute of audio on a CPU)
  balanced  small             beam 2   VAD
  accurate  medium            beam 5   VAD
  best      large-v3          beam 5   VAD       (needs a GPU or patience)
Models download from Hugging Face on first use into ~/.cache/huggingface.
"""
import argparse, json, os, sys, time

LEVELS = {
    "fast":     {"model": "base",     "beam": 1, "vad": False, "best_of": 1},
    "balanced": {"model": "small",    "beam": 2, "vad": True,  "best_of": 2},
    "accurate": {"model": "medium",   "beam": 5, "vad": True,  "best_of": 5},
    "best":     {"model": "large-v3", "beam": 5, "vad": True,  "best_of": 5},
}

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--input", required=True); ap.add_argument("--output", required=True)
    ap.add_argument("--level", default="balanced", choices=sorted(LEVELS)); ap.add_argument("--model", default="")
    ap.add_argument("--language", default=""); ap.add_argument("--device", default="auto"); ap.add_argument("--compute", default="")
    ap.add_argument("--vad", default=""); ap.add_argument("--beam", type=int, default=0); ap.add_argument("--initial-prompt", default="")
    ap.add_argument("--threads", type=int, default=0)
    a = ap.parse_args()
    try:
        from faster_whisper import WhisperModel
    except Exception as e:  # pragma: no cover
        print(f"ERROR faster-whisper is not installed for {sys.executable}: {e}", file=sys.stderr); return 2
    lv = LEVELS[a.level]
    model_name = a.model or lv["model"]
    if a.language == "en" and model_name in ("tiny", "base", "small", "medium"): model_name += ".en"
    device = a.device
    if device == "auto":
        device = "cpu"
        try:
            import ctranslate2
            if ctranslate2.get_cuda_device_count() > 0: device = "cuda"
        except Exception: pass
    compute = a.compute or ("float16" if device == "cuda" else "int8")
    t0 = time.time()
    print(f"LOAD model={model_name} device={device} compute={compute}", file=sys.stderr, flush=True)
    kw = {"device": device, "compute_type": compute}
    if a.threads > 0: kw["cpu_threads"] = a.threads
    model = WhisperModel(model_name, **kw)
    vad = lv["vad"] if a.vad == "" else a.vad in ("1", "true", "yes")
    beam = a.beam or lv["beam"]
    opts = dict(beam_size=beam, best_of=lv["best_of"], word_timestamps=True, vad_filter=vad, condition_on_previous_text=a.level != "fast")
    if vad: opts["vad_parameters"] = {"min_silence_duration_ms": 500}
    if a.language: opts["language"] = a.language
    if a.initial_prompt: opts["initial_prompt"] = a.initial_prompt
    segments_iter, info = model.transcribe(a.input, **opts)
    total = float(info.duration or 0.0)
    out = {"language": info.language, "languageProbability": round(float(info.language_probability or 0.0), 3), "duration": total,
           "model": model_name, "level": a.level, "device": device, "computeType": compute, "segments": [], "text": ""}
    texts = []; last = 0.0
    for i, s in enumerate(segments_iter):
        words = [{"word": w.word, "start": round(w.start, 3), "end": round(w.end, 3), "probability": round(float(w.probability), 3)} for w in (s.words or [])]
        out["segments"].append({"id": i, "start": round(s.start, 3), "end": round(s.end, 3), "text": s.text.strip(), "avgLogprob": round(float(s.avg_logprob), 3), "noSpeechProb": round(float(s.no_speech_prob), 3), "words": words})
        texts.append(s.text.strip())
        if s.end - last >= 5.0 or i % 10 == 0:
            last = s.end; print(f"PROGRESS {s.end:.1f} {total:.1f}", file=sys.stderr, flush=True)
    out["text"] = " ".join(texts)
    out["elapsedSec"] = round(time.time() - t0, 1)
    tmp = a.output + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f: json.dump(out, f, ensure_ascii=False)
    os.replace(tmp, a.output)
    print(f"DONE segments={len(out['segments'])} elapsed={out['elapsedSec']}s", file=sys.stderr, flush=True)
    return 0

if __name__ == "__main__":
    sys.exit(main())
