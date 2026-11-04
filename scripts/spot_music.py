#!/usr/bin/env python3
"""spot_music.py: MIDI-style background music for generated spots (no samples, no network).

    python -I spot_music.py --out bed.wav --mid bed.mid --seconds 45 --mood news --energy 7 --seed 12345

A small composer writes a General MIDI file (conductor + pad, bass, arpeggio, lead and drums tracks) and a built-in
numpy synthesizer renders it to a 48 kHz stereo WAV of exactly --seconds. The tune is fitted to the spot:

  mood    news urgent sports upbeat warm bright calm night solemn cinematic steady quirky
  energy  1..10: below 4 no hats, from 5 kick and snare and the lead line join, high energy raises tempo and velocities
  seed    the same seed gives the same tune (key, tempo, progression, melody), a new spot gets a new one

The last bar resolves to the tonic so a fade-out at the end of the spot sounds finished. The bed is levelled to about
-12 dBFS RMS with a soft limiter; the spot mixer lowers it under the voice.

Stdout: one JSON object {ok, wav, mid, bpm, key, mood, seconds, tracks, notes}. Exit 0 / 2.
"""
from __future__ import annotations

import argparse
import json
import math
import random
import struct
import sys
import wave

import numpy as np

SR = 48000
PPQ = 480
NOTE_NAMES = ["C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"]
SCALES = {
    "major": [0, 2, 4, 5, 7, 9, 11], "minor": [0, 2, 3, 5, 7, 8, 10], "dorian": [0, 2, 3, 5, 7, 9, 10],
    "lydian": [0, 2, 4, 6, 7, 9, 11], "mixolydian": [0, 2, 4, 5, 7, 9, 10], "pent": [0, 2, 4, 7, 9],
}
# scale-degree chord roots per bar; the quality comes from the scale (triads built on the degree)
MOODS = {
    #            scale(s)                    bpm        progressions                              bass       arp (stride, seq)         drums     lead  gm programs: pad bass arp lead
    "calm":     dict(scale=["major"],        bpm=(68, 78),  progs=[[0, 5, 3, 4], [0, 3, 0, 4]],     bass="whole",  arp=(4, [0, 1, 2, 1]),   drums=None,      lead=True,  gm=(89, 32, 4, 73)),
    "warm":     dict(scale=["major"],        bpm=(84, 96),  progs=[[0, 4, 5, 3], [0, 3, 4, 4]],     bass="half",   arp=(2, [0, 1, 2, 1]),   drums="soft",    lead=True,  gm=(88, 33, 4, 73)),
    "bright":   dict(scale=["major", "lydian"], bpm=(100, 112), progs=[[0, 4, 5, 3], [0, 3, 4, 3]], bass="eighth", arp=(2, [0, 2, 1, 2]),   drums="pop",     lead=True,  gm=(88, 33, 11, 80)),
    "news":     dict(scale=["dorian", "minor"], bpm=(104, 116), progs=[[0, 5, 3, 4], [0, 2, 3, 4]], bass="eighth", arp=(1, [0, 2, 1, 2, 0, 2, 1, 2]), drums="drive", lead=False, gm=(89, 38, 5, 81)),
    "urgent":   dict(scale=["minor"],        bpm=(122, 134), progs=[[0, 0, 5, 4], [0, 5, 0, 4]],     bass="sixteenth", arp=(1, [0, 1, 2, 1]), drums="drive", lead=False, gm=(89, 38, 5, 81)),
    "sports":   dict(scale=["major", "mixolydian"], bpm=(118, 130), progs=[[0, 3, 4, 3], [0, 4, 5, 4]], bass="eighth", arp=(2, [0, 1, 2, 1]), drums="big",   lead=True,  gm=(61, 34, 29, 80)),
    "upbeat":   dict(scale=["major"],        bpm=(108, 120), progs=[[0, 4, 5, 3], [0, 3, 0, 4]],     bass="eighth", arp=(2, [0, 2, 1, 2]),   drums="pop",     lead=True,  gm=(88, 33, 4, 73)),
    "solemn":   dict(scale=["minor"],        bpm=(54, 62),  progs=[[0, 5, 3, 4], [0, 3, 5, 4]],     bass="whole",  arp=(4, [0, 1, 2, 1]),   drums=None,      lead=True,  gm=(48, 32, 0, 71)),
    "cinematic": dict(scale=["minor"],       bpm=(72, 84),  progs=[[0, 5, 2, 4], [0, 3, 5, 4]],     bass="pulse",  arp=(2, [0, 1, 2, 1]),   drums="soft",    lead=True,  gm=(48, 33, 46, 70)),
    "steady":   dict(scale=["dorian"],       bpm=(98, 108), progs=[[0, 3, 0, 4], [0, 2, 3, 2]],     bass="eighth", arp=(2, [0, 1, 2, 1]),   drums="soft",    lead=False, gm=(89, 38, 4, 73)),
    "quirky":   dict(scale=["pent"],         bpm=(112, 124), progs=[[0, 2, 3, 1], [0, 3, 2, 1]],    bass="bounce", arp=(1, [0, 2, 1, 3]),   drums="pop",     lead=True,  gm=(88, 33, 12, 80)),
    "night":    dict(scale=["lydian"],       bpm=(58, 66),  progs=[[0, 4, 5, 3], [0, 1, 4, 0]],     bass="whole",  arp=(4, [2, 1, 0, 1]),   drums=None,      lead=True,  gm=(89, 32, 98, 88)),
}
ALIASES = {"driving": "news", "tense": "urgent", "energetic": "sports", "happy": "upbeat", "gentle": "calm", "sad": "solemn",
           "dramatic": "cinematic", "light": "bright", "friendly": "warm", "money": "steady", "fun": "quirky"}

DRUMS = {   # 16th-step patterns per bar
    "soft":  dict(kick=[0, 8], snare=[], hat=[4, 12], vel=0.7),
    "pop":   dict(kick=[0, 8], snare=[4, 12], hat=list(range(0, 16, 2)), vel=0.85),
    "drive": dict(kick=[0, 6, 8, 14], snare=[4, 12], hat=list(range(0, 16, 2)), vel=0.95),
    "big":   dict(kick=[0, 4, 8, 12], snare=[4, 12], hat=list(range(0, 16, 2)), vel=1.0),
}
BASS = {    # (16th step, semitones above the chord root, length in steps)
    "whole":  [(0, 0, 16)],
    "half":   [(0, 0, 8), (8, 0, 8)],
    "pulse":  [(s, 0, 2) for s in range(0, 16, 2)],
    "eighth": [(0, 0, 2), (2, 0, 2), (4, 12, 2), (6, 0, 2), (8, 0, 2), (10, 0, 2), (12, 7, 2), (14, 0, 2)],
    "sixteenth": [(s, 12 if s % 8 == 6 else 0, 1) for s in range(16)],
    "bounce": [(0, 0, 2), (3, 12, 1), (6, 7, 2), (8, 0, 2), (11, 12, 1), (14, 7, 2)],
}


# ----------------------------------------------------------------------------------------------- composition
def pick_mood(name: str) -> str:
    n = (name or "").strip().lower()
    n = ALIASES.get(n, n)
    return n if n in MOODS else "warm"


class Piece:
    """The composed tune as note events: (start_step, length_steps, midi_note, velocity) per track; steps are 16ths."""
    def __init__(self, seconds: float, mood: str, energy: int, seed: int):
        self.mood_name = pick_mood(mood)
        m = MOODS[self.mood_name]
        self.rng = random.Random(seed)
        r = self.rng
        self.energy = max(1, min(10, int(energy)))
        lo, hi = m["bpm"]
        self.bpm = round(lo + (hi - lo) * (0.25 + 0.07 * self.energy) * r.uniform(0.85, 1.0))
        self.bpm = max(lo - 4, min(hi + 6, self.bpm))
        self.scale_name = r.choice(m["scale"])
        self.scale = SCALES[self.scale_name]
        self.root = r.choice([48, 50, 51, 53, 55, 57])      # C3 .. A3
        self.key = f"{NOTE_NAMES[self.root % 12]} {self.scale_name}"
        self.step = 60.0 / self.bpm / 4.0
        self.bars = max(2, int(math.ceil(seconds / (self.step * 16))) + 1)
        self.prog = r.choice(m["progs"])
        self.m = m
        self.tracks = {"pad": [], "bass": [], "arp": [], "lead": [], "drums": []}
        self._compose()

    def deg(self, d: int, octave: int = 0) -> int:
        n = len(self.scale)
        return self.root + 12 * (octave + d // n) + self.scale[d % n]

    def chord(self, d: int):
        return [self.deg(d), self.deg(d + 2), self.deg(d + 4)]

    def _compose(self):
        m, r, e = self.m, self.rng, self.energy
        vscale = 0.7 + 0.03 * e
        # chords: the progression loops; the last bar resolves to the tonic
        degs = [self.prog[b % len(self.prog)] for b in range(self.bars)]
        degs[-1] = 0
        for b, d in enumerate(degs):
            s0 = b * 16
            ch = self.chord(d)
            last = b == self.bars - 1
            # pad: the whole chord for the bar, below the arpeggio so the two do not share a register
            for n in ch:
                self.tracks["pad"].append((s0, 16, n, int(52 * vscale)))
            # bass
            for st, off, ln in (BASS["whole"] if last else BASS[m["bass"]]):
                self.tracks["bass"].append((s0 + st, ln, ch[0] - 12 + off, int(86 * vscale)))
            # arpeggio over the chord tones (skipped in the resolving bar)
            stride, seq = m["arp"]
            if not last:
                tones = [ch[0] + 12, ch[1] + 12, ch[2] + 12, ch[1] + 24]
                for i, st in enumerate(range(0, 16, stride)):
                    k = seq[i % len(seq)]
                    accent = 1.0 if st % 8 == 0 else 0.8
                    self.tracks["arp"].append((s0 + st, max(1, stride), tones[k % len(tones)], int(64 * vscale * accent)))
        # drums from energy 4 (hats), 5 (kick and snare)
        if m["drums"] and e >= 4:
            pat = DRUMS[m["drums"]]
            for b in range(self.bars - 1):
                s0 = b * 16
                if e >= 5:
                    for st in pat["kick"]: self.tracks["drums"].append((s0 + st, 1, 36, int(105 * pat["vel"])))
                    for st in pat["snare"]: self.tracks["drums"].append((s0 + st, 1, 38, int(95 * pat["vel"])))
                for st in pat["hat"]: self.tracks["drums"].append((s0 + st, 1, 42, int((70 if st % 4 == 0 else 52) * pat["vel"])))
                if e >= 7 and (b % 4 == 3):         # a snare fill at the end of every fourth bar
                    for st in (12, 13, 14, 15): self.tracks["drums"].append((s0 + st, 1, 38, int(70 + 8 * (st - 12))))
            self.tracks["drums"].append(((self.bars - 1) * 16, 1, 36, 100))     # a last kick on the resolving bar
        # lead: a two-bar motif (random walk on the scale, chord tones on the strong beats), varied every 4 bars
        if m["lead"] and e >= 3:
            rhythms = [[4, 4, 8], [6, 2, 4, 4], [4, 2, 2, 8], [8, 4, 4], [2, 2, 4, 8]]
            motif = []
            pos = r.choice([4, 5, 6])
            for _ in range(2):
                for ln in r.choice(rhythms):
                    motif.append((ln, pos)); pos = max(2, min(11, pos + r.choice([-2, -1, -1, 0, 1, 1, 2])))
            for b in range(0, self.bars - 1, 2):
                if (b // 2) % 2 == 1 and e < 7:
                    continue                      # every other phrase is left open
                st = b * 16
                for k, (ln, dg) in enumerate(motif):
                    if st >= (self.bars - 1) * 16: break
                    bar = st // 16
                    d = degs[bar]
                    if st % 16 == 0: dg = d + [0, 2, 4][k % 3] + 7        # a chord tone on the downbeat
                    self.tracks["lead"].append((st, max(1, ln - 1), self.deg(dg, 0) if dg > 6 else self.deg(dg + 7), int(74 * vscale)))
                    st += ln
            tail = (self.bars - 1) * 16
            self.tracks["lead"].append((tail, 12, self.deg(7), int(72 * vscale)))     # the tune ends on the tonic


# ----------------------------------------------------------------------------------------------- MIDI file
def vlq(n: int) -> bytes:
    out = [n & 0x7F]
    n >>= 7
    while n:
        out.append((n & 0x7F) | 0x80); n >>= 7
    return bytes(reversed(out))


def midi_track(events, channel: int, program: int | None, name: str) -> bytes:
    ev = []
    for st, ln, note, vel in events:
        t0 = st * PPQ // 4; t1 = (st + ln) * PPQ // 4
        ev.append((t0, 1, bytes([0x90 | channel, note & 0x7F, max(1, min(127, vel))])))
        ev.append((t1, 0, bytes([0x80 | channel, note & 0x7F, 0])))
    ev.sort(key=lambda x: (x[0], x[1]))
    data = bytes([0x00, 0xFF, 0x03, len(name)]) + name.encode("ascii")
    if program is not None:
        data += b"\x00" + bytes([0xC0 | channel, program])
    last = 0
    for t, _o, msg in ev:
        data += vlq(t - last) + msg; last = t
    data += b"\x00\xFF\x2F\x00"
    return b"MTrk" + struct.pack(">I", len(data)) + data


def write_midi(piece: Piece, path: str) -> None:
    mpqn = int(60_000_000 / piece.bpm)
    cond = (b"\x00\xFF\x03\x09conductor" + b"\x00\xFF\x51\x03" + mpqn.to_bytes(3, "big") + b"\x00\xFF\x58\x04\x04\x02\x18\x08"
            + b"\x00\xFF\x59\x02\x00" + (b"\x01" if piece.scale_name in ("minor", "dorian") else b"\x00") + b"\x00\xFF\x2F\x00")
    gm = piece.m["gm"]
    chunks = [b"MTrk" + struct.pack(">I", len(cond)) + cond]
    for ch, (nm, prog) in enumerate([("pad", gm[0]), ("bass", gm[1]), ("arp", gm[2]), ("lead", gm[3])]):
        if piece.tracks[nm]: chunks.append(midi_track(piece.tracks[nm], ch, prog, nm))
    if piece.tracks["drums"]: chunks.append(midi_track(piece.tracks["drums"], 9, None, "drums"))
    with open(path, "wb") as f:
        f.write(b"MThd" + struct.pack(">IHHH", 6, 1, len(chunks), PPQ))
        for c in chunks: f.write(c)


# ----------------------------------------------------------------------------------------------- synthesizer
def hz(note: int) -> float:
    return 440.0 * 2 ** ((note - 69) / 12.0)


def adsr(n: int, a: float, d: float, s: float, rel: float, held: int) -> np.ndarray:
    """Amplitude envelope over n samples: attack, decay to sustain, held for `held` samples, then release."""
    t = np.arange(n, dtype=np.float32) / SR
    env = np.minimum(1.0, t / max(a, 1e-4))
    env = np.where(t > a, 1.0 - (1.0 - s) * np.minimum(1.0, (t - a) / max(d, 1e-4)), env)
    rel_n = int(rel * SR)
    if held < n:
        tail = np.clip(1.0 - (np.arange(n - held, dtype=np.float32) / max(rel_n, 1)), 0.0, 1.0)
        env[held:] *= tail
    return env.astype(np.float32)


def voice(kind: str, note: int, length_s: float, rng: np.random.Generator) -> np.ndarray:
    rel = {"pad": 0.7, "bass": 0.10, "arp": 0.35, "lead": 0.22}[kind]
    n = int((length_s + rel) * SR)
    t = np.arange(n, dtype=np.float32) / SR
    f = hz(note)
    if kind == "pad":      # three detuned band-limited saws: a warm string-like pad
        w = np.zeros(n, dtype=np.float32)
        for det in (-0.07, 0.0, 0.07):
            fd = f * 2 ** (det / 12)
            for k in range(1, 9):
                if fd * k < 5200: w += np.sin(2 * np.pi * fd * k * t + k) / k
        w *= 0.22
        env = adsr(n, 0.35, 0.4, 0.8, rel, int(length_s * SR))
    elif kind == "bass":   # sine + octave + a little square: round and punchy
        w = np.sin(2 * np.pi * f * t) + 0.45 * np.sin(2 * np.pi * 2 * f * t) + 0.18 * np.sign(np.sin(2 * np.pi * f * t))
        w *= 0.55
        env = adsr(n, 0.004, 0.18, 0.55, rel, int(length_s * SR))
    elif kind == "arp":    # electric-piano style FM: bright attack, soft decay
        dec = np.exp(-t * 4.0)
        w = np.sin(2 * np.pi * f * t + 1.6 * dec * np.sin(2 * np.pi * 2 * f * t)) + 0.3 * np.sin(2 * np.pi * 4 * f * t) * dec
        w *= 0.5
        env = adsr(n, 0.003, 0.25, 0.35, rel, int(length_s * SR))
    else:                  # lead: triangle + a little vibrato, soft attack
        vib = 1.0 + 0.004 * np.sin(2 * np.pi * 5.2 * t) * np.minimum(1.0, t / 0.4)
        ph = 2 * np.pi * f * np.cumsum(vib) / SR
        w = (2 / np.pi) * np.arcsin(np.sin(ph)) * 0.8 + 0.25 * np.sin(2 * ph)
        w *= 0.5
        env = adsr(n, 0.02, 0.2, 0.7, rel, int(length_s * SR))
    return (w * env).astype(np.float32)


def drum_hit(note: int, rng: np.random.Generator) -> np.ndarray:
    if note == 36:    # kick: sine sweep down
        n = int(0.32 * SR); t = np.arange(n, dtype=np.float32) / SR
        ph = 2 * np.pi * (45 * t + 75 * (1 - np.exp(-t * 28)) / 28)
        return (np.sin(ph) * np.exp(-t * 11) * 1.1).astype(np.float32)
    if note == 38:    # snare: noise + a body tone
        n = int(0.22 * SR); t = np.arange(n, dtype=np.float32) / SR
        noise = rng.standard_normal(n).astype(np.float32)
        noise = noise - np.concatenate([[0], noise[:-1]]) * 0.6
        return ((noise * np.exp(-t * 20) * 0.5) + np.sin(2 * np.pi * 190 * t) * np.exp(-t * 28) * 0.45).astype(np.float32)
    n = int(0.07 * SR); t = np.arange(n, dtype=np.float32) / SR     # hat: high-passed noise
    noise = rng.standard_normal(n).astype(np.float32)
    noise = noise - np.concatenate([[0], noise[:-1]])
    return (noise * np.exp(-t * 70) * 0.22).astype(np.float32)


def render(piece: Piece, seconds: float) -> np.ndarray:
    n = int(math.ceil(seconds * SR))
    pad_len = int(piece.bars * 16 * piece.step * SR) + SR * 2
    L = np.zeros(pad_len, dtype=np.float32); R = np.zeros(pad_len, dtype=np.float32)
    rng = np.random.default_rng(12345)
    cache: dict = {}
    pan = {"pad": 0.0, "bass": 0.0, "arp": 0.25, "lead": -0.2, "drums": 0.0}
    gain = {"pad": 0.55, "bass": 0.9, "arp": 0.55, "lead": 0.5, "drums": 0.8}
    dry = {k: np.zeros(pad_len, dtype=np.float32) for k in ("pad", "bass", "arp", "lead", "drums")}
    for kind, evs in piece.tracks.items():
        buf = dry[kind]
        for st, ln, note, vel in evs:
            pos = int(st * piece.step * SR)
            if kind == "drums":
                key = (kind, note)
                if key not in cache: cache[key] = drum_hit(note, rng)
                w = cache[key]
            else:
                dur = round(ln * piece.step, 3)
                key = (kind, note, dur)
                if key not in cache: cache[key] = voice(kind, note, dur, rng)
                w = cache[key]
            end = min(len(buf), pos + len(w))
            if pos < len(buf): buf[pos:end] += w[: end - pos] * (vel / 100.0)
    # the arpeggio gets a dotted-eighth echo; the pad a slight stereo widening (a few ms Haas delay)
    d = int(piece.step * 3 * SR)
    arp = dry["arp"].copy(); arp[d:] += 0.38 * dry["arp"][:-d]; arp[2 * d:] += 0.16 * dry["arp"][:-2 * d]; dry["arp"] = arp
    haas = int(0.012 * SR)
    for kind, buf in dry.items():
        g = gain[kind] * 0.5
        lp = (1 - max(0.0, pan[kind])) * g; rp = (1 - max(0.0, -pan[kind])) * g
        L += buf * lp
        if kind == "pad": R[haas:] += buf[:-haas] * rp
        else: R += buf * rp
    out = np.stack([L[:n], R[:n]], axis=1) if len(L) >= n else np.stack([np.pad(L, (0, n - len(L))), np.pad(R, (0, n - len(R)))], axis=1)
    # level to about -12 dBFS RMS over the active part, soft limit, fade in / out
    act = out[: max(1, min(len(out), int(seconds * SR)))]
    rms = float(np.sqrt(np.mean(act.astype(np.float64) ** 2))) or 1e-6
    out = out * (10 ** (-12 / 20) / rms)
    out = np.tanh(out * 0.9) / 0.9 * 0.95
    fi = int(0.05 * SR); fo = int(min(1.2, seconds / 3) * SR)
    out[:fi] *= np.linspace(0, 1, fi)[:, None]; out[-fo:] *= np.linspace(1, 0, fo)[:, None]
    return out.astype(np.float32)


def write_wav(path: str, audio: np.ndarray) -> None:
    pcm = (np.clip(audio, -1, 1) * 32767.0).astype("<i2")
    with wave.open(path, "wb") as w:
        w.setnchannels(2); w.setsampwidth(2); w.setframerate(SR); w.writeframes(pcm.tobytes())


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True, help="rendered WAV")
    ap.add_argument("--mid", default="", help="also write the MIDI file")
    ap.add_argument("--seconds", type=float, required=True)
    ap.add_argument("--mood", default="warm")
    ap.add_argument("--energy", type=int, default=5)
    ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args()
    try:
        secs = max(3.0, min(900.0, a.seconds))
        piece = Piece(secs, a.mood, a.energy, a.seed)
        if a.mid: write_midi(piece, a.mid)
        write_wav(a.out, render(piece, secs))
        print(json.dumps({"ok": True, "wav": a.out, "mid": a.mid, "bpm": piece.bpm, "key": piece.key, "mood": piece.mood_name,
                          "seconds": secs, "tracks": [k for k, v in piece.tracks.items() if v], "notes": sum(len(v) for v in piece.tracks.values())}))
        return 0
    except Exception as e:  # noqa: BLE001
        sys.stderr.write(repr(e) + "\n")
        print(json.dumps({"ok": False, "error": str(e)}))
        return 2


if __name__ == "__main__":
    sys.exit(main())
