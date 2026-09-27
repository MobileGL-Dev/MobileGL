#!/usr/bin/env python3
"""Pair client/server present intervals and summarize their frame time.

MOBILEGL_PIPE_STATS=1 enables the per-present clocks. Frame IDs are matched
exactly; the first server interval is invalid because it has no preceding
present. Transport wait measures wall time inside the client's transport park
(flush, spin and sleep). It can overlap with server CPU and is not network latency.
"""

from __future__ import annotations

import argparse
import json
import math
import re
from pathlib import Path


FIELDS = re.compile(r"(\w+)=([^\s]+)")
CLIENT_MARKER = "P65LinkMetrics "
SERVER_MARKER = "P65ServerMetrics "


def read_frames(path: Path, marker: str, *, server: bool) -> dict[int, dict]:
    rows = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if marker not in line:
            continue
        values = dict(FIELDS.findall(line.split(marker, 1)[1]))
        if server and values.get("valid") != "1":
            continue
        if not server and values.get("kind") != "frame":
            continue
        try:
            number = int(values["frame"])
            wall = int(values["wall_ns"])
            cpu = int(values["apply_thread_cpu_ns" if server else "client_thread_cpu_ns"])
            transport = 0 if server else int(values["transport_wait_ns"])
        except (KeyError, ValueError) as error:
            raise ValueError(f"{path}: malformed {marker.strip()} row") from error
        if number in rows:
            raise ValueError(f"{path}: duplicate frame {number}")
        # The wall and thread-CPU clocks are sampled separately. Their nanosecond
        # readings can differ slightly at a boundary even for a fully busy thread.
        tolerance = max(100_000, wall // 100)
        if (number <= 0 or wall <= 0 or cpu <= 0 or cpu > wall + tolerance or
                transport < 0 or transport > wall + tolerance):
            raise ValueError(f"{path}: invalid clocks in frame {number}")
        rows[number] = {"wall_ns": wall, "cpu_ns": cpu, "transport_ns": transport}
    if not rows:
        raise ValueError(f"{path}: no usable {marker.strip()} frames")
    return rows


def quantile_ns(samples: list[int], percentile: int) -> int:
    ordered = sorted(samples)
    return ordered[max(0, math.ceil(len(ordered) * percentile / 100) - 1)]


def distribution(samples: list[int]) -> dict[str, float]:
    return {"avg": sum(samples) / len(samples) / 1e6,
            "p50": quantile_ns(samples, 50) / 1e6,
            "p99": quantile_ns(samples, 99) / 1e6}


def summarize(client_path: Path, server_path: Path, *, warmup: int = 0,
              tail: int | None = None) -> dict:
    client = read_frames(client_path, CLIENT_MARKER, server=False)
    server = read_frames(server_path, SERVER_MARKER, server=True)
    selected = sorted(number for number in client if number > warmup)
    if tail is not None:
        selected = selected[-tail:]
    if not selected:
        raise ValueError("no frames remain after warmup/tail selection")
    if selected != list(range(selected[0], selected[-1] + 1)):
        raise ValueError("selected client frame IDs are not contiguous")
    missing = [number for number in selected if number not in server]
    if missing:
        raise ValueError(f"server is missing matched frames: {missing[:10]}")

    columns = {name: [] for name in ("client_present_ms", "server_present_ms",
                                     "client_cpu_ms", "transport_wait_ms", "server_cpu_ms")}
    for number in selected:
        c, s = client[number], server[number]
        for key, value in (("client_present_ms", c["wall_ns"]),
                           ("server_present_ms", s["wall_ns"]),
                           ("client_cpu_ms", c["cpu_ns"]),
                           ("transport_wait_ms", c["transport_ns"]),
                           ("server_cpu_ms", s["cpu_ns"])):
            columns[key].append(value)

    duration = sum(columns["client_present_ms"])
    shares = {name: 100 * sum(columns[name]) / duration
              for name in ("client_cpu_ms", "transport_wait_ms", "server_cpu_ms")}
    return {"frames": len(selected), "first_frame": selected[0],
            "last_frame": selected[-1], "warmup_frames": warmup,
            "definition": "transport_wait is client wall time in SessionProducer::Park "
                          "(flush, spin and sleep); role CPU and transport wait can overlap",
            "percentile": "nearest rank", "unit": "ms",
            "stats": {key: distribution(values) for key, values in columns.items()},
            "share_percent_of_client_present": shares}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--client-log", type=Path, required=True)
    parser.add_argument("--server-log", type=Path, required=True)
    parser.add_argument("--warmup-frames", type=int, default=1)
    parser.add_argument("--tail-frames", type=int)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.warmup_frames < 0 or args.tail_frames is not None and args.tail_frames <= 0:
        parser.error("warmup must be nonnegative and tail must be positive")
    try:
        result = summarize(args.client_log, args.server_log, warmup=args.warmup_frames,
                           tail=args.tail_frames)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    rendered = json.dumps(result, ensure_ascii=False, indent=2) + "\n"
    if args.output:
        args.output.write_text(rendered, encoding="utf-8")
    else:
        print(rendered, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
