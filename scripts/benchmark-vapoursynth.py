"""Evaluate optional spatial VS filters without changing any running channel.
Synthetic changing 1080p frames, two VS worker threads. Includes Python source-copy cost;
excludes decoder, GPU transfers, compositor, encoding and storage. This is not a live capacity test.
"""
import argparse
import importlib.util
import json
import pathlib
import statistics
import time

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--frames", type=int, default=90)
    p.add_argument("--output", type=pathlib.Path)
    args = p.parse_args()
    if not 10 <= args.frames <= 10000:
        p.error("--frames must be between 10 and 10000")
    import numpy as np
    import vapoursynth as vs
    core = vs.core
    core.num_threads = 2
    core.max_cache_size = 128
    spec = importlib.util.spec_from_file_location("lightweight", pathlib.Path(__file__).resolve().parents[1] / "filters/vapoursynth/lightweight.py")
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    rng = np.random.default_rng(47)
    planes = [rng.integers(16, 236, (1080, 1920), dtype=np.uint8), rng.integers(16, 241, (540, 960), dtype=np.uint8), rng.integers(16, 241, (540, 960), dtype=np.uint8)]
    rows = []
    for preset in ("baseline",) + module.PRESETS:
        blank = core.std.BlankClip(width=1920, height=1080, format=vs.YUV420P8, length=args.frames + 5, fpsnum=30000, fpsden=1001, keep=False)
        def source(n, f):
            frame = f.copy()
            for plane in range(3):
                np.bitwise_xor(planes[plane], n % 4, out=np.asarray(frame[plane]))
            return frame
        clip = core.std.ModifyFrame(blank, clips=blank, selector=source)
        if preset != "baseline":
            clip = module.apply(clip, preset)
        for n in range(5):
            with clip.get_frame(n):
                pass
        samples = []
        for n in range(5, args.frames + 5):
            start = time.perf_counter()
            with clip.get_frame(n):
                pass
            samples.append((time.perf_counter() - start) * 1000)
        rows.append({"preset": preset, "medianMs": round(statistics.median(samples), 3), "p95Ms": round(sorted(samples)[int(len(samples) * .95) - 1], 3), "fps": round(1000 / statistics.mean(samples), 1)})
    result = {"scope": "Synthetic 1920x1080 YUV420P8, 2 VS threads, sequential requests; no decode/encode or live I/O", "version": str(vs.__version__), "framesPerPreset": args.frames, "results": rows}
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2), encoding="utf8")
    print(json.dumps(result, indent=2))

if __name__ == "__main__":
    main()
