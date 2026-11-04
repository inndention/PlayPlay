#!/usr/bin/env python3
"""spot_tts.py — narration synthesis for PLAY PLAY auto-generated spots.

    python -I spot_tts.py --text-file in.txt --out voice.wav [--voice af_heart] [--speed 1.0]
                          [--engine auto|kokoro|sapi] [--kokoro-dir C:\\PlayPlayOBS\\tools]

Engines, tried in order under `auto`:
  1. kokoro      — the `kokoro` pip package (KPipeline, 24 kHz) in THIS interpreter
  2. kokoro_onnx — the `kokoro-onnx` package with model files found under --kokoro-dir
                   (kokoro-v1.0.onnx + voices-v1.0.bin, searched in the dir, dir/kokoro_models,
                   dir/../kokoro_models)
  3. kokoro venv — a PlayPlay OBS style install under --kokoro-dir: ttsvenv/Scripts/python.exe +
                   kokoro_synth.py, called with a BOM-free JSON config {text, out, voice, speed}
  3b. kokoro sherpa — Kokoro-82M int8 (kokoro-en-v0_19) through sherpa-onnx-node with Node.js (kokoro_sherpa.js next
                   to this script): the install StarMedia's motion-spots uses. Model dir: --kokoro-model, a --kokoro-dir
                   holding model.int8.onnx, $STARMEDIA_KOKORO_DIR or D:\StarMedia\models\kokoro-int8-en-v0_19; addon:
                   --sherpa-node, $SHERPA_ONNX_NODE_DIR or D:\StarMedia\motion-spots\node_modules\sherpa-onnx-node.
                   Voices of that model: af af_bella af_nicole af_sarah af_sky am_adam am_michael bf_emma bf_isabella
                   bm_george bm_lewis; newer names map to the nearest (af_heart -> af_bella).
  4. sapi        — Windows SAPI 5 through System.Speech (PowerShell). Always present on Windows;
                   voice names: a Kokoro id (af_*/bf_* -> Zira, am_*/bm_* -> David) or any
                   installed SAPI voice name such as "Microsoft Zira Desktop".

The narration text is read from a FILE (never passed on a command line or interpolated into
PowerShell), and the output is always a PCM WAV. Stdout carries exactly one JSON object:
    {"ok": true, "engine": "sapi", "durationSec": 5.8, "path": "...", "sampleRate": 48000}
    {"ok": false, "error": "..."}
Progress and diagnostics go to stderr. Exit code 0 on success, 2 on failure.

Batch mode (Headline News: one WAV per scene, the engine loaded once):
    python -I spot_tts.py --batch voices.json [--voice ..] [--speed ..] [--engine ..] [--kokoro-dir ..]
voices.json = {"items": [{"text": "...", "out": "voice_00.wav"}, ...]}; every item uses the same engine.
Stdout: {"ok": true, "engine": "...", "voiceUsed": "...", "items": [{"path", "durationSec", "words"}], "tried": [...]}
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import wave
from pathlib import Path


def log(msg: str) -> None:
    sys.stderr.write(str(msg).rstrip() + "\n")
    sys.stderr.flush()


def wav_duration(path: str) -> tuple[float, int]:
    with wave.open(path, "rb") as w:
        frames, rate = w.getnframes(), w.getframerate()
        return (frames / float(rate) if rate else 0.0), rate


def write_wav(path: str, samples, rate: int) -> None:
    """Float32/-1..1 numpy samples -> 16-bit mono PCM WAV."""
    import numpy as np
    a = np.asarray(samples, dtype="float32").reshape(-1)
    peak = float(np.max(np.abs(a))) if a.size else 0.0
    if peak > 1.0:
        a = a / peak
    pcm = (a * 32767.0).astype("<i2")
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(int(rate))
        w.writeframes(pcm.tobytes())


# ----------------------------------------------------------------------------- kokoro (pip)
def try_kokoro_pkg(text: str, out: str, voice: str, speed: float) -> dict | None:
    try:
        from kokoro import KPipeline  # type: ignore
    except Exception as e:  # noqa: BLE001
        log(f"kokoro package not available: {e.__class__.__name__}")
        return None
    import numpy as np
    lang = "b" if voice.startswith("b") else "a"
    pipe = KPipeline(lang_code=lang)
    chunks = []
    for _gs, _ps, audio in pipe(text, voice=voice or "af_heart", speed=speed):
        if audio is None:
            continue
        try:
            audio = audio.numpy()  # torch tensor
        except AttributeError:
            pass
        chunks.append(np.asarray(audio, dtype="float32").reshape(-1))
        chunks.append(np.zeros(int(24000 * 0.25), dtype="float32"))  # breath between chunks
    if not chunks:
        return None
    write_wav(out, np.concatenate(chunks), 24000)
    return {"engine": "kokoro"}


# ----------------------------------------------------------------------------- kokoro-onnx
def find_kokoro_models(kdir: str) -> tuple[str, str] | None:
    if not kdir:
        return None
    base = Path(kdir)
    for cand in (base, base / "kokoro_models", base.parent / "kokoro_models", base / "models"):
        m = cand / "kokoro-v1.0.onnx"
        v = cand / "voices-v1.0.bin"
        if m.is_file() and v.is_file():
            return str(m), str(v)
    return None


def try_kokoro_onnx(text: str, out: str, voice: str, speed: float, kdir: str) -> dict | None:
    models = find_kokoro_models(kdir)
    if not models:
        return None
    try:
        from kokoro_onnx import Kokoro  # type: ignore
    except Exception as e:  # noqa: BLE001
        log(f"kokoro_onnx not importable: {e.__class__.__name__}")
        return None
    k = Kokoro(models[0], models[1])
    lang = "en-gb" if voice.startswith("b") else "en-us"
    samples, rate = k.create(text, voice=voice or "af_heart", speed=speed, lang=lang)
    write_wav(out, samples, int(rate))
    return {"engine": "kokoro_onnx"}


# ----------------------------------------------------------------------------- kokoro venv
def try_kokoro_venv(text: str, out: str, voice: str, speed: float, kdir: str) -> dict | None:
    if not kdir:
        return None
    base = Path(kdir)
    py = None
    for cand in (base / "ttsvenv" / "Scripts" / "python.exe", base / "Scripts" / "python.exe",
                 base.parent / "tools" / "ttsvenv" / "Scripts" / "python.exe"):
        if cand.is_file():
            py = cand
            break
    synth = None
    for cand in (base / "kokoro_synth.py", base / "tools" / "kokoro_synth.py", base.parent / "tools" / "kokoro_synth.py"):
        if cand.is_file():
            synth = cand
            break
    if not py or not synth:
        return None
    cfg = {"text": text, "out": out, "output": out, "voice": voice or "af_heart", "speed": speed}
    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False, encoding="utf-8") as f:
        json.dump(cfg, f)          # json.dump never writes a BOM (the worker rejects one)
        cfg_path = f.name
    try:
        r = subprocess.run([str(py), str(synth), cfg_path], capture_output=True, text=True, timeout=900)
        if r.returncode != 0 or not Path(out).is_file():
            log("kokoro venv worker failed: " + (r.stderr or r.stdout)[-400:])
            return None
        return {"engine": "kokoro_venv"}
    finally:
        try:
            os.unlink(cfg_path)
        except OSError:
            pass


# ----------------------------------------------------------------------------- kokoro through sherpa-onnx-node
SHERPA_FILES = ("model.int8.onnx", "voices.bin", "tokens.txt", "espeak-ng-data")
SHERPA_MODEL_DIRS = (os.environ.get("STARMEDIA_KOKORO_DIR", ""), r"D:\StarMedia\models\kokoro-int8-en-v0_19")
SHERPA_NODE_DIRS = (os.environ.get("SHERPA_ONNX_NODE_DIR", ""), r"D:\StarMedia\motion-spots\node_modules\sherpa-onnx-node")


def find_kokoro_sherpa(a) -> tuple[str, str, str] | None:
    """(model dir, sherpa-onnx-node dir, node.exe) when everything the Node engine needs is present, else None."""
    model = next((d for d in (getattr(a, "kokoro_model", ""), getattr(a, "kokoro_dir", ""), *SHERPA_MODEL_DIRS)
                  if d and all((Path(d) / f).exists() for f in SHERPA_FILES)), "")
    pkg = next((d for d in (getattr(a, "sherpa_node", ""), *SHERPA_NODE_DIRS) if d and (Path(d) / "package.json").is_file()), "")
    node = shutil.which("node") or shutil.which("node.exe") or next((p for p in (r"C:\Program Files\nodejs\node.exe",) if Path(p).is_file()), "")
    helper = Path(__file__).with_name("kokoro_sherpa.js")
    if not (model and pkg and node and helper.is_file()):
        log("kokoro (sherpa-onnx) not available: model %s, sherpa-onnx-node %s, node %s, helper %s"
            % tuple("ok" if x else "missing" for x in (model, pkg, node, helper.is_file())))
        return None
    return model, pkg, node


def batch_kokoro_sherpa(jobs, voice: str, speed: float, a) -> dict | None:
    found = find_kokoro_sherpa(a)
    if not found:
        return None
    model, pkg, node = found
    tmp = tempfile.mkdtemp(prefix="spot_kokoro_")
    try:
        job = {"modelDir": model, "sherpaDir": pkg, "voice": voice or "am_michael", "speed": speed, "threads": 8,
               "items": [{"text": t, "out": o} for t, o in jobs]}
        jf = os.path.join(tmp, "job.json")
        Path(jf).write_text(json.dumps(job), encoding="utf-8")
        r = subprocess.run([node, str(Path(__file__).with_name("kokoro_sherpa.js")), jf],
                           capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=1800)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    lines = [ln for ln in (r.stdout or "").splitlines() if ln.strip().startswith("{")]
    try:
        res = json.loads(lines[-1]) if lines else {}
    except ValueError:
        res = {}
    if r.returncode != 0 or not res.get("ok") or not all(Path(o).is_file() for _t, o in jobs):
        log("kokoro (sherpa-onnx) failed: " + str(res.get("error") or (r.stderr or r.stdout or "")[-400:]))
        return None
    return {"engine": "kokoro", "voiceUsed": "kokoro:" + str(res.get("voiceUsed") or voice)}


# ----------------------------------------------------------------------------- SAPI
_PS_SCRIPT = r"""
param([string]$TextFile, [string]$Out, [string]$Voice, [int]$Rate)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Speech
$text = [System.IO.File]::ReadAllText($TextFile, [System.Text.Encoding]::UTF8)
$sp = New-Object System.Speech.Synthesis.SpeechSynthesizer
try {
  if ($Voice) { try { $sp.SelectVoice($Voice) } catch { Write-Error ("voice not installed: " + $Voice) -ErrorAction Continue } }
  $sp.Rate = [Math]::Max(-10, [Math]::Min(10, $Rate))
  $fmt = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(48000, [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen, [System.Speech.AudioFormat.AudioChannel]::Mono)
  $sp.SetOutputToWaveFile($Out, $fmt)
  $sp.Speak($text)
} finally { $sp.Dispose() }
Write-Output ("VOICE " + $sp.Voice.Name)
"""


def sapi_voice_for(voice: str, installed: list[str]) -> str:
    """Map a Kokoro voice id onto an installed SAPI voice; pass real SAPI names through."""
    v = (voice or "").strip()
    if not v:
        return ""
    low = v.lower()
    for name in installed:
        if name.lower() == low:
            return name
    female = low.startswith(("af_", "bf_", "ef_", "ff_", "f")) and not low.startswith("fm")
    male = low.startswith(("am_", "bm_", "em_", "fm_", "m"))
    british = low.startswith(("bf_", "bm_"))
    def pick(pred):
        for name in installed:
            if pred(name.lower()):
                return name
        return ""
    if female:
        return pick(lambda n: british and "hazel" in n) or pick(lambda n: "zira" in n) or pick(lambda n: "eva" in n or "aria" in n or "jenny" in n) or ""
    if male:
        return pick(lambda n: british and "george" in n) or pick(lambda n: "david" in n) or pick(lambda n: "mark" in n or "guy" in n) or ""
    return ""


def installed_sapi_voices(ps: str) -> list[str]:
    try:
        r = subprocess.run([ps, "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-Command",
                            "Add-Type -AssemblyName System.Speech; (New-Object System.Speech.Synthesis.SpeechSynthesizer).GetInstalledVoices() | ForEach-Object { $_.VoiceInfo.Name }"],
                           capture_output=True, text=True, timeout=60)
        return [l.strip() for l in r.stdout.splitlines() if l.strip()]
    except Exception as e:  # noqa: BLE001
        log(f"could not list SAPI voices: {e}")
        return []


def try_sapi(text_file: str, out: str, voice: str, speed: float) -> dict | None:
    ps = shutil.which("powershell") or shutil.which("powershell.exe") or shutil.which("pwsh")
    if not ps:
        log("no PowerShell found for SAPI")
        return None
    voices = installed_sapi_voices(ps)
    sapi_voice = sapi_voice_for(voice, voices)
    rate = int(round((float(speed or 1.0) - 1.0) * 10.0))      # 1.0 -> 0, 1.2 -> +2, 0.8 -> -2
    with tempfile.NamedTemporaryFile("w", suffix=".ps1", delete=False, encoding="utf-8") as f:
        f.write(_PS_SCRIPT)
        script = f.name
    try:
        r = subprocess.run([ps, "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", script,
                            "-TextFile", text_file, "-Out", out, "-Voice", sapi_voice, "-Rate", str(rate)],
                           capture_output=True, text=True, timeout=900)
    finally:
        try:
            os.unlink(script)
        except OSError:
            pass
    if r.returncode != 0 or not Path(out).is_file():
        log("SAPI failed: " + (r.stderr or r.stdout)[-400:])
        return None
    used = ""
    for line in r.stdout.splitlines():
        if line.startswith("VOICE "):
            used = line[6:].strip()
    return {"engine": "sapi", "voiceUsed": used or sapi_voice}


# ----------------------------------------------------------------------------- batch
_PS_BATCH = r"""
param([string]$ListFile, [string]$Voice, [int]$Rate)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Speech
$doc = Get-Content -Raw -Encoding UTF8 $ListFile | ConvertFrom-Json      # {"items": [...]}: Windows PowerShell 5.1 hands a bare JSON array down the pipeline as ONE object
$list = $doc.items
$sp = New-Object System.Speech.Synthesis.SpeechSynthesizer
$used = ''
try {
  if ($Voice) { try { $sp.SelectVoice($Voice) } catch { Write-Error ("voice not installed: " + $Voice) -ErrorAction Continue } }
  $sp.Rate = [Math]::Max(-10, [Math]::Min(10, $Rate))
  $used = $sp.Voice.Name
  $fmt = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(48000, [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen, [System.Speech.AudioFormat.AudioChannel]::Mono)
  foreach ($it in $list) {
    $text = [System.IO.File]::ReadAllText($it.TextFile, [System.Text.Encoding]::UTF8)
    $sp.SetOutputToWaveFile($it.Out, $fmt)
    $sp.Speak($text)
    $sp.SetOutputToNull()
  }
} finally { $sp.Dispose() }
Write-Output ("VOICE " + $used)
"""


def batch_kokoro_pkg(jobs, voice: str, speed: float) -> dict | None:
    try:
        from kokoro import KPipeline  # type: ignore
    except Exception as e:  # noqa: BLE001
        log(f"kokoro package not available: {e.__class__.__name__}")
        return None
    import numpy as np
    pipe = KPipeline(lang_code="b" if voice.startswith("b") else "a")
    for text, out in jobs:
        chunks = []
        for _gs, _ps, audio in pipe(text, voice=voice or "af_heart", speed=speed):
            if audio is None:
                continue
            try:
                audio = audio.numpy()
            except AttributeError:
                pass
            chunks.append(np.asarray(audio, dtype="float32").reshape(-1))
            chunks.append(np.zeros(int(24000 * 0.25), dtype="float32"))
        if not chunks:
            return None
        write_wav(out, np.concatenate(chunks), 24000)
    return {"engine": "kokoro"}


def batch_kokoro_onnx(jobs, voice: str, speed: float, kdir: str) -> dict | None:
    models = find_kokoro_models(kdir)
    if not models:
        return None
    try:
        from kokoro_onnx import Kokoro  # type: ignore
    except Exception as e:  # noqa: BLE001
        log(f"kokoro_onnx not importable: {e.__class__.__name__}")
        return None
    k = Kokoro(models[0], models[1])
    lang = "en-gb" if voice.startswith("b") else "en-us"
    for text, out in jobs:
        samples, rate = k.create(text, voice=voice or "af_heart", speed=speed, lang=lang)
        write_wav(out, samples, int(rate))
    return {"engine": "kokoro_onnx"}


def batch_kokoro_venv(jobs, voice: str, speed: float, kdir: str) -> dict | None:
    for text, out in jobs:
        if not try_kokoro_venv(text, out, voice, speed, kdir):
            return None
    return {"engine": "kokoro_venv"}


def batch_sapi(jobs, voice: str, speed: float) -> dict | None:
    ps = shutil.which("powershell") or shutil.which("powershell.exe") or shutil.which("pwsh")
    if not ps:
        log("no PowerShell found for SAPI")
        return None
    sapi_voice = sapi_voice_for(voice, installed_sapi_voices(ps))
    rate = int(round((float(speed or 1.0) - 1.0) * 10.0))
    tmp = tempfile.mkdtemp(prefix="spot_tts_")
    try:
        listing = []
        for i, (text, out) in enumerate(jobs):
            tf = os.path.join(tmp, f"t{i:02d}.txt")
            Path(tf).write_text(text, encoding="utf-8")
            listing.append({"TextFile": tf, "Out": out})
        list_file = os.path.join(tmp, "list.json")
        Path(list_file).write_text(json.dumps({"items": listing}), encoding="utf-8")       # no BOM
        script = os.path.join(tmp, "batch.ps1")
        Path(script).write_text(_PS_BATCH, encoding="utf-8")
        r = subprocess.run([ps, "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", script,
                            "-ListFile", list_file, "-Voice", sapi_voice, "-Rate", str(rate)],
                           capture_output=True, text=True, timeout=1800)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    if r.returncode != 0 or not all(Path(out).is_file() for _t, out in jobs):
        log("SAPI batch failed: " + (r.stderr or r.stdout)[-400:])
        return None
    used = ""
    for line in r.stdout.splitlines():
        if line.startswith("VOICE "):
            used = line[6:].strip()
    return {"engine": "sapi", "voiceUsed": used or sapi_voice}


def trim_wav(path: str, threshold_db: float = -42.0, keep_ms: int = 80) -> None:
    """Drop the engine's leading / trailing silence (SAPI pads every clip by up to a second), keeping a short
    margin, so the scene timing follows the speech and stories do not sit apart by long gaps."""
    import numpy as np
    with wave.open(path, "rb") as w:
        n, rate, ch, sw = w.getnframes(), w.getframerate(), w.getnchannels(), w.getsampwidth()
        raw = w.readframes(n)
    if sw != 2 or n == 0:
        return
    a = np.frombuffer(raw, dtype="<i2").reshape(-1, ch)
    level = np.abs(a.astype("int32")).max(axis=1)
    loud = np.nonzero(level > int(32767 * (10 ** (threshold_db / 20.0))))[0]
    if loud.size == 0:
        return
    keep = int(rate * keep_ms / 1000)
    s0, s1 = max(0, int(loud[0]) - keep), min(len(level), int(loud[-1]) + keep + 1)
    if s0 == 0 and s1 == len(level):
        return
    with wave.open(path, "wb") as w:
        w.setnchannels(ch)
        w.setsampwidth(sw)
        w.setframerate(rate)
        w.writeframes(a[s0:s1].tobytes())


def limit_peaks(y, rate: int, ceiling: float):
    """A small peak limiter: per 2 ms block the gain that keeps the block under the ceiling, taken one block early
    (attack), released over ~60 ms, interpolated between blocks so the gain never steps audibly."""
    import numpy as np
    blk = max(1, int(rate * 0.002))
    m = (len(y) + blk - 1) // blk
    peaks = np.pad(np.abs(y), (0, m * blk - len(y))).reshape(m, blk).max(axis=1)
    need = np.minimum(1.0, ceiling / np.maximum(peaks, 1e-9))
    need = np.minimum(need, np.concatenate([need[1:], need[-1:]]))
    rel = float(np.exp(-blk / (rate * 0.06)))
    g = np.empty(m)
    cur = 1.0
    for i in range(m):
        cur = need[i] if need[i] < cur else need[i] + (cur - need[i]) * rel
        g[i] = cur
    centers = (np.arange(m) + 0.5) * blk
    return y * np.interp(np.arange(len(y)), centers, g)


def normalize_wav(path: str, target_db: float = -19.0, ceiling_db: float = -1.5) -> float:
    """Bring speech to one steady level whatever the engine (Kokoro speaks 5-7 dB under SAPI): the RMS of the voiced
    50 ms blocks goes to target_db, never pushing the peak past ceiling_db. Returns the gain applied in dB."""
    import numpy as np
    with wave.open(path, "rb") as w:
        n, rate, ch, sw = w.getnframes(), w.getframerate(), w.getnchannels(), w.getsampwidth()
        raw = w.readframes(n)
    if sw != 2 or n == 0:
        return 0.0
    x = np.frombuffer(raw, dtype="<i2").astype("float32") / 32768.0
    blk = max(1, int(rate * 0.05)) * ch
    m = len(x) // blk
    if m == 0:
        return 0.0
    e = (x[: m * blk].reshape(m, blk) ** 2).mean(axis=1)
    voiced = e[e > 1e-5]                                         # blocks above -50 dBFS
    if voiced.size == 0:
        return 0.0
    gain = min(target_db - 10.0 * float(np.log10(voiced.mean())), 24.0)
    if abs(gain) < 0.5:
        return 0.0
    y = x * (10.0 ** (gain / 20.0))
    ceiling = 10.0 ** (ceiling_db / 20.0)
    if float(np.abs(y).max()) > ceiling:
        y = limit_peaks(y, rate * ch, ceiling)                   # speech transients (sharper on male voices) limited, not clipped
    y = np.clip(y, -1.0, 1.0)
    with wave.open(path, "wb") as w:
        w.setnchannels(ch)
        w.setsampwidth(sw)
        w.setframerate(rate)
        w.writeframes((y * 32767.0).astype("<i2").tobytes())
    return gain


def run_batch(a) -> int:
    try:
        spec = json.loads(Path(a.batch).read_text(encoding="utf-8-sig"))
    except Exception as e:  # noqa: BLE001
        print(json.dumps({"ok": False, "error": f"cannot read batch: {e}"}))
        return 2
    jobs = []
    for i, it in enumerate(spec.get("items") or []):
        text = re.sub(r"\s+", " ", str(it.get("text") or "")).strip()[: a.max_chars]
        out = str(Path(str(it.get("out") or "")).resolve())
        if len(text) < 2 or not it.get("out"):
            print(json.dumps({"ok": False, "error": f"item {i}: empty text or no output path"}))
            return 2
        Path(out).parent.mkdir(parents=True, exist_ok=True)
        if Path(out).exists():
            try:
                os.unlink(out)
            except OSError:
                pass
        jobs.append((text, out))
    if not jobs:
        print(json.dumps({"ok": False, "error": "no items"}))
        return 2
    speed = max(0.5, min(2.0, float(a.speed or 1.0)))
    order = []
    if a.engine in ("auto", "kokoro"):
        order += [("kokoro", lambda: batch_kokoro_pkg(jobs, a.voice, speed)),
                  ("kokoro_onnx", lambda: batch_kokoro_onnx(jobs, a.voice, speed, a.kokoro_dir)),
                  ("kokoro_venv", lambda: batch_kokoro_venv(jobs, a.voice, speed, a.kokoro_dir)),
                  ("kokoro_sherpa", lambda: batch_kokoro_sherpa(jobs, a.voice, speed, a))]
    if a.engine in ("auto", "sapi"):
        order.append(("sapi", lambda: batch_sapi(jobs, a.voice, speed)))
    res, tried = None, []
    for name, fn in order:
        tried.append(name)
        try:
            res = fn()
        except Exception as e:  # noqa: BLE001
            log(f"{name} failed: {e}")
            res = None
        if res:
            break
    if not res:
        print(json.dumps({"ok": False, "error": "no TTS engine produced audio", "tried": tried}))
        return 2
    items = []
    for text, out in jobs:
        try:
            trim_wav(out)
            normalize_wav(out)
        except Exception as e:  # noqa: BLE001
            log(f"{Path(out).name}: trim / level skipped ({e})")
        try:
            dur, _rate = wav_duration(out)
        except Exception as e:  # noqa: BLE001
            print(json.dumps({"ok": False, "error": f"{Path(out).name} is not a WAV: {e}", "engine": res.get("engine")}))
            return 2
        if dur < 0.3:
            print(json.dumps({"ok": False, "error": f"{Path(out).name}: audio too short ({dur:.2f}s)", "engine": res.get("engine")}))
            return 2
        items.append({"path": out, "durationSec": round(dur, 3), "words": len(text.split())})
    print(json.dumps({"ok": True, "engine": res.get("engine"), "voiceUsed": res.get("voiceUsed", ""), "items": items, "tried": tried}))
    return 0


# ----------------------------------------------------------------------------- main
def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--text-file", default="")
    ap.add_argument("--out", default="")
    ap.add_argument("--batch", default="", help="voices.json: one WAV per item, one engine for all (see the module doc)")
    ap.add_argument("--voice", default="af_heart")
    ap.add_argument("--speed", type=float, default=1.0)
    ap.add_argument("--engine", default="auto", choices=["auto", "kokoro", "sapi"])
    ap.add_argument("--kokoro-dir", default="")
    ap.add_argument("--kokoro-model", default="", help="sherpa-onnx Kokoro model dir (model.int8.onnx, voices.bin, tokens.txt, espeak-ng-data)")
    ap.add_argument("--sherpa-node", default="", help="the sherpa-onnx-node package dir")
    ap.add_argument("--max-chars", type=int, default=12000)
    a = ap.parse_args()
    if a.batch:
        return run_batch(a)
    if not a.text_file or not a.out:
        print(json.dumps({"ok": False, "error": "--text-file and --out are required (or --batch)"}))
        return 2

    try:
        text = Path(a.text_file).read_text(encoding="utf-8", errors="replace")
    except OSError as e:
        print(json.dumps({"ok": False, "error": f"cannot read text: {e}"}))
        return 2
    text = re.sub(r"\s+", " ", text).strip()
    if len(text) > a.max_chars:
        cut = text[: a.max_chars]
        text = cut[: cut.rfind(". ") + 1] if ". " in cut[a.max_chars // 2:] else cut
    if len(text) < 2:
        print(json.dumps({"ok": False, "error": "empty narration"}))
        return 2
    out = str(Path(a.out).resolve())
    Path(out).parent.mkdir(parents=True, exist_ok=True)
    if Path(out).exists():
        try:
            os.unlink(out)
        except OSError:
            pass
    # SAPI must read the normalised text, not the raw file
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False, encoding="utf-8") as f:
        f.write(text)
        norm_file = f.name

    speed = max(0.5, min(2.0, float(a.speed or 1.0)))
    res = None
    tried = []
    try:
        if a.engine in ("auto", "kokoro"):
            for fn, name in ((lambda: try_kokoro_pkg(text, out, a.voice, speed), "kokoro"),
                             (lambda: try_kokoro_onnx(text, out, a.voice, speed, a.kokoro_dir), "kokoro_onnx"),
                             (lambda: try_kokoro_venv(text, out, a.voice, speed, a.kokoro_dir), "kokoro_venv"),
                             (lambda: batch_kokoro_sherpa([(text, out)], a.voice, speed, a), "kokoro_sherpa")):
                tried.append(name)
                try:
                    res = fn()
                except Exception as e:  # noqa: BLE001
                    log(f"{name} failed: {e}")
                    res = None
                if res:
                    break
        if not res and a.engine in ("auto", "sapi"):
            tried.append("sapi")
            res = try_sapi(norm_file, out, a.voice, speed)
    finally:
        try:
            os.unlink(norm_file)
        except OSError:
            pass
    if not res or not Path(out).is_file():
        print(json.dumps({"ok": False, "error": "no TTS engine produced audio", "tried": tried}))
        return 2
    try:
        normalize_wav(out)                                   # one speech level for every engine
    except Exception as e:  # noqa: BLE001
        log(f"level skipped ({e})")
    try:
        dur, rate = wav_duration(out)
    except Exception as e:  # noqa: BLE001
        print(json.dumps({"ok": False, "error": f"output is not a WAV: {e}", "engine": res.get("engine")}))
        return 2
    if dur < 0.3:
        print(json.dumps({"ok": False, "error": f"synthesised audio too short ({dur:.2f}s)", "engine": res.get("engine")}))
        return 2
    res.update({"ok": True, "durationSec": round(dur, 3), "path": out, "sampleRate": rate, "words": len(text.split()), "tried": tried})
    print(json.dumps(res))
    return 0


if __name__ == "__main__":
    sys.exit(main())
