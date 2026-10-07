#!/usr/bin/env python3
"""Reduce matrix_p14.sh results: steady-state fps, per-thread CPU ms/frame, GPU busy share.

    python p14_report.py <results_dir> [--md out.md] [--json out.json]

Steady state drops the first SKIP[wl] frames (the load frames: openra's 270/736 ms set-up
frames, rd12's single multi-second upload frame plus three) and averages the rest. The CPU window is the steady frames'
wall span ending at benchmark.json's mtime (cpusampler.c records that mtime and per-thread
schedstat on-CPU ns every 10 ms); per-thread CPU is interpolated at the window edges and divided
by the steady frame count. GPU busy = delta(kgsl gpu_clock_stats sum) / window.
"""
import argparse
import bisect
import json
import re
import statistics
import sys
from collections import defaultdict
from pathlib import Path

SKIP = {"openra": 8, "rd12": 4}


def load_cpu(path):
    meta = {}
    series = defaultdict(list)  # (pid, tid, comm) -> [(t, v)]
    if not path.exists():
        return meta, series
    with path.open(encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("#"):
                for k, v in re.findall(r"(\w+)=(-?\d+)", line):
                    meta[k] = int(v)
                continue
            if line.startswith("t_ns"):
                continue
            parts = line.rstrip("\n").split(",")
            if len(parts) != 5:
                continue
            t, pid, tid, comm, v = parts
            series[(int(pid), int(tid), comm)].append((int(t), int(v)))
    return meta, series


def at(points, t):
    ts = [p[0] for p in points]
    i = bisect.bisect_left(ts, t)
    if i <= 0:
        return points[0][1] if points and points[0][0] - t < 50_000_000 else None
    if i >= len(points):
        return points[-1][1] if t - points[-1][0] < 50_000_000 else None
    (t0, v0), (t1, v1) = points[i - 1], points[i]
    return v0 + (v1 - v0) * (t - t0) / max(1, t1 - t0)


def reduce_run(run_dir, wl):
    bench_path = run_dir / "benchmark.json"
    if not bench_path.exists():
        return None
    bench = json.loads(bench_path.read_text(encoding="utf-8", errors="replace"))
    frames = bench.get("frameTimesMs") or []
    skip = SKIP.get(wl, 0)
    steady = frames[skip:]
    if not steady:
        return None
    out = {
        "frames": len(frames),
        "steady_frames": len(steady),
        "steady_mean_ms": statistics.mean(steady),
        "steady_median_ms": statistics.median(steady),
        "steady_p95_ms": sorted(steady)[max(0, int(len(steady) * 0.95) - 1)],
        "harness_fps": bench.get("fps"),
    }
    out["steady_fps"] = 1000.0 / out["steady_mean_ms"]
    meta, series = load_cpu(run_dir / "cpu.csv")
    end = meta.get("watch_mtime_ns", -1)
    if end > 0 and series:
        span = sum(steady) * 1e6
        start = end - span
        threads = []
        procs = defaultdict(float)
        for (pid, tid, comm), pts in series.items():
            if pid < 0:
                continue
            a, b = at(pts, start), at(pts, end)
            if a is None or b is None:
                continue
            ms = (b - a) / 1e6 / len(steady)
            if ms <= 0.005:
                continue
            threads.append({"pid": pid, "tid": tid, "comm": comm, "ms_per_frame": ms,
                            "util": (b - a) / span})
            procs[pid] += ms
        threads.sort(key=lambda r: -r["ms_per_frame"])
        out["threads"] = threads[:8]
        out["cpu_ms_per_frame_total"] = sum(t["ms_per_frame"] for t in threads)
        out["process_ms_per_frame"] = dict(procs)
        gpu = [v for k, v in series.items() if k[2] == "gpubusy_us"]
        if gpu:
            a, b = at(gpu[0], start), at(gpu[0], end)
            if a is not None and b is not None:
                out["gpu_busy"] = (b - a) * 1e3 / span
        for name in ("gpufreq", "cpu7freq", "cpu2freq"):
            pts = [v for k, v in series.items() if k[2] == name]
            if pts:
                vals = [v for t, v in pts[0] if start <= t <= end]
                if vals:
                    out[name] = (min(vals), max(vals))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root", type=Path)
    ap.add_argument("--md", type=Path)
    ap.add_argument("--json", type=Path)
    ap.add_argument("--threads", action="store_true", help="print per-run top threads")
    args = ap.parse_args()
    runs = defaultdict(list)
    for bench in sorted(args.root.glob("*/*/*-r*/benchmark.json")):
        run_dir = bench.parent
        wl, backend = run_dir.parent.parent.name, run_dir.parent.name
        m = re.fullmatch(r"(.+)-r(\d+)", run_dir.name)
        if not m:
            continue
        verify = (run_dir / "verify.txt").read_text() if (run_dir / "verify.txt").exists() else ""
        if "MISSING" in verify or "UNEXPECTED" in verify:
            print(f"skip {run_dir}: {verify.strip()}", file=sys.stderr)
            continue
        r = reduce_run(run_dir, wl)
        if r:
            r["rep"] = int(m.group(2))
            r["dir"] = str(run_dir)
            runs[(wl, backend, m.group(1))].append(r)
            if args.threads:
                tops = ", ".join(f"{t['comm']}({t['pid']})={t['ms_per_frame']:.2f}" for t in r.get("threads", [])[:5])
                print(f"{wl:7} {backend:12} {m.group(1):9} r{r['rep']} fps={r['steady_fps']:.1f} "
                      f"cpu={r.get('cpu_ms_per_frame_total', float('nan')):.2f} gpu={r.get('gpu_busy', float('nan')):.2f} :: {tops}")
    order = {"dev": 0, "monolith": 1, "inproc": 2, "shm": 3, "tcp": 4}
    rows = []
    for (wl, backend, arm), rs in sorted(runs.items(), key=lambda kv: (kv[0][0], kv[0][1], order.get(kv[0][2], 9), kv[0][2])):
        fps = sorted(r["steady_fps"] for r in rs)
        best = max(rs, key=lambda r: r["steady_fps"])
        med = statistics.median(fps)
        main_thread = best.get("threads", [{}])[0] if best.get("threads") else {}
        rows.append({
            "wl": wl, "backend": backend, "arm": arm, "n": len(rs),
            "best_fps": fps[-1], "median_fps": med,
            "spread_pct": 100.0 * (fps[-1] - fps[0]) / med if len(fps) > 1 else 0.0,
            "best_mean_ms": best["steady_mean_ms"], "best_median_ms": best["steady_median_ms"],
            "cpu_total": best.get("cpu_ms_per_frame_total"),
            "top_thread": f"{main_thread.get('comm', '?')} {main_thread.get('ms_per_frame', float('nan')):.2f}" if main_thread else "-",
            "top_util": main_thread.get("util"),
            "second_thread": (f"{best['threads'][1]['comm']} {best['threads'][1]['ms_per_frame']:.2f}"
                              if len(best.get("threads", [])) > 1 else "-"),
            "gpu_busy": best.get("gpu_busy"),
            "procs": best.get("process_ms_per_frame"),
        })
    lines = ["| workload | backend | arm | n | best fps | median fps | spread | mean ms | CPU ms/frame (all threads) | top thread ms/frame (util) | 2nd thread | GPU busy |",
             "|---|---|---|---|---|---|---|---|---|---|---|---|"]
    fmt = lambda v, f="{:.1f}": "-" if v is None else f.format(v)
    for r in rows:
        lines.append(f"| {r['wl']} | {r['backend']} | {r['arm']} | {r['n']} | {r['best_fps']:.1f} | {r['median_fps']:.1f} | "
                     f"{r['spread_pct']:.1f}% | {r['best_mean_ms']:.2f} | {fmt(r['cpu_total'], '{:.2f}')} | {r['top_thread']} "
                     f"({fmt(r['top_util'] and 100 * r['top_util'], '{:.0f}')}%) | {r['second_thread']} | {fmt(r['gpu_busy'] and 100 * r['gpu_busy'], '{:.0f}')}% |")
    text = "\n".join(lines)
    print(text)
    if args.md:
        args.md.write_text(text + "\n", encoding="utf-8")
    if args.json:
        args.json.write_text(json.dumps({"rows": rows, "runs": {"/".join(k): v for k, v in runs.items()}}, indent=1), encoding="utf-8")


if __name__ == "__main__":
    main()
