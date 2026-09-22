#!/usr/bin/env python3
"""Summarize P65LinkMetrics frame logs without confusing traffic with link throughput.

Example: p65_link_metrics.py --arm spawn:/tmp/spawn.client.log \
    --arm tcp-credit1:/tmp/tcp.client.log --warmup-frames 5 --output /tmp/p65-metrics.json

Enable MOBILEGL_PIPE_STATS=1 in each measured arm. RTT percentiles are upper
bounds of fixed power-of-two microsecond buckets, not exact sample quantiles.
The separately measured 64 MiB Stage burst throughput can be supplied with
--link-mib-per-second; ordinary frame traffic is never called link throughput.
"""
from __future__ import annotations
import argparse
import json
import math
import re
from pathlib import Path

FIELDS = re.compile(r"(\w+)=([^\s]+)")


def summarize(path: Path, warmup: int, throughput: float | None = None) -> dict:
    frames = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if "P65LinkMetrics " not in line:
            continue
        record = dict(FIELDS.findall(line.split("P65LinkMetrics ", 1)[1]))
        if record.get("kind") != "frame" or int(record["frame"]) <= warmup:
            continue
        frames.append(record)
    if not frames:
        raise ValueError(f"{path}: no measured frames after warmup={warmup}")
    histogram = [0] * 32
    count = samples = stage = wall = cpu = 0
    weighted_rtt_us = 0.0
    for frame in frames:
        buckets = [int(n) for n in frame["rtt_hist_us_pow2"].split(",")]
        if len(buckets) != 32 or sum(buckets) != int(frame["rtt_samples"]):
            raise ValueError(f"{path}: malformed RTT histogram in frame {frame['frame']}")
        histogram = [a + b for a, b in zip(histogram, buckets)]
        count += int(frame["wait_replies"])
        n = int(frame["rtt_samples"])
        samples += n
        weighted_rtt_us += float(frame["rtt_mean_us"]) * n
        stage += int(frame["stage_bytes"])
        wall += int(frame["wall_ns"])
        cpu += int(frame["client_thread_cpu_ns"])
    def percentile(q: float) -> int:
        target = math.ceil(samples * q)
        if target == 0:
            return 0
        seen = 0
        for i, bucket in enumerate(histogram):
            seen += bucket
            if seen >= target:
                return 1 << i
        raise AssertionError("histogram count is inconsistent")
    result = {
        "log": str(path), "frames": len(frames), "warmup_frames": warmup,
        "wait_replies": count, "wait_replies_per_frame": count / len(frames),
        "rtt_samples": samples,
        "rtt_mean_us": weighted_rtt_us / samples if samples else None,
        "rtt_p50_upper_us": percentile(0.5), "rtt_p99_upper_us": percentile(0.99),
        "rtt_histogram_upper_us": [1 << i for i in range(32)],
        "rtt_histogram_counts": histogram,
        "stage_bytes": stage, "stage_bytes_per_frame": stage / len(frames),
        "frame_traffic_mib_per_second": stage * 1e9 / wall / (1024 ** 2) if wall else None,
        "fps": len(frames) * 1e9 / wall if wall else None,
        "client_thread_cpu_ms_per_frame": cpu / len(frames) / 1e6,
    }
    if throughput is not None:
        traffic = result["frame_traffic_mib_per_second"]
        result["measured_link_mib_per_second"] = throughput
        result["link_bandwidth_fraction"] = traffic / throughput if traffic is not None else None
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arm", action="append", required=True, metavar="LABEL:LOG")
    parser.add_argument("--warmup-frames", type=int, default=0)
    parser.add_argument("--link-mib-per-second", type=float)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.warmup_frames < 0 or (args.link_mib_per_second is not None and args.link_mib_per_second <= 0):
        parser.error("warmup must be nonnegative and measured throughput must be positive")
    report = {}
    for arm in args.arm:
        label, sep, filename = arm.partition(":")
        if not sep or not label or label in report:
            parser.error("each --arm must have a unique LABEL:LOG")
        try:
            report[label] = summarize(Path(filename), args.warmup_frames, args.link_mib_per_second)
        except (OSError, ValueError, KeyError) as error:
            parser.error(str(error))
    if "spawn" in report:
        baseline = report["spawn"]["client_thread_cpu_ms_per_frame"]
        for label, arm in report.items():
            if label != "spawn":
                arm["client_thread_cpu_delta_vs_spawn_ms"] = arm["client_thread_cpu_ms_per_frame"] - baseline
    text = json.dumps(report, ensure_ascii=False, indent=2) + "\n"
    if args.output:
        args.output.write_text(text, encoding="utf-8")
    else:
        print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
