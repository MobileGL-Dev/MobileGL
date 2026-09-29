#!/usr/bin/env python3
"""Reduce crosshost_accept.sh output (.trace-work/perf-compare/crosshost/<run>/) to a table."""
import json, os, re, statistics, sys

root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.path.dirname(__file__), "..", "..", "..", ".trace-work", "perf-compare", "crosshost")

def load(path):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return None

def kv(line):
    return dict(re.findall(r"(\w+)=([^\s]+)", line))

rows = []
for name in sorted(os.listdir(root)):
    d = os.path.join(root, name)
    if not os.path.isdir(d):
        continue
    rc = open(os.path.join(d, "rc.txt")).read().strip() if os.path.exists(os.path.join(d, "rc.txt")) else "?"
    res = load(os.path.join(d, "result.json")) or {}
    bench = load(os.path.join(d, "benchmark.json")) or {}
    summary = {}
    frame_bytes = {}
    client = os.path.join(d, "mobilegl.client.log")
    if os.path.exists(client):
        for line in open(client, encoding="utf-8", errors="replace"):
            if "P65LinkMetrics kind=summary" in line:
                summary = kv(line)
            elif "P65LinkMetrics kind=frame " in line:
                f = kv(line)
                frame_bytes[int(f["frame"])] = int(f["stage_bytes"])
    ft = bench.get("frameTimesMs") or []
    # Steady state = every frame after the first (the load frame carries the whole texture upload).
    steady = [k for k in sorted(frame_bytes) if 2 <= k <= len(ft)]
    steady_s = sum(ft[k - 1] for k in steady) / 1000 if steady else 0
    steady_mb = sum(frame_bytes[k] for k in steady) / 1e6 if steady else 0
    rows.append({
        "run": name, "rc": rc,
        "ssim": res.get("ssim") if "ssim" in name else None,
        "frames": bench.get("totalFrames"),
        "fps": bench.get("fps"), "mean_ms": bench.get("meanFrameMs"),
        "median_ms": bench.get("medianFrameMs"), "p95_ms": bench.get("p95FrameMs"),
        "first_frame_s": round(ft[0] / 1000, 1) if ft else None,
        "steady_fps": round(1000 / statistics.median(ft[1:]), 1) if len(ft) > 2 else None,
        "steady_MB_per_frame": round(steady_mb / len(steady), 2) if steady else None,
        "steady_MBps": round(steady_mb / steady_s, 1) if steady_s else None,
        "wait_replies": summary.get("wait_replies"),
        "rtt_mean_ms": round(float(summary["rtt_mean_us"]) / 1000, 1) if "rtt_mean_us" in summary else None,
        "rtt_p50_ub_ms": round(int(summary["rtt_p50_upper_us"]) / 1000, 1) if "rtt_p50_upper_us" in summary else None,
        "rtt_p99_ub_ms": round(int(summary["rtt_p99_upper_us"]) / 1000, 1) if "rtt_p99_upper_us" in summary else None,
        "stage_MB": round(int(summary["stage_bytes"]) / 1e6) if "stage_bytes" in summary else None,
        "client_cpu_s": round(int(summary["client_thread_cpu_ns"]) / 1e9, 1) if "client_thread_cpu_ns" in summary else None,
        "wall_s": round(int(summary["wall_ns"]) / 1e9, 1) if "wall_ns" in summary else None,
    })

cols = ["run", "rc", "ssim", "frames", "fps", "steady_fps", "median_ms", "p95_ms", "first_frame_s",
        "steady_MB_per_frame", "steady_MBps", "wait_replies", "rtt_mean_ms", "rtt_p50_ub_ms", "rtt_p99_ub_ms", "stage_MB", "client_cpu_s", "wall_s"]
print("| " + " | ".join(cols) + " |")
print("|" + "---|" * len(cols))
for r in rows:
    print("| " + " | ".join("" if r[c] is None else str(r[c]) for c in cols) + " |")
