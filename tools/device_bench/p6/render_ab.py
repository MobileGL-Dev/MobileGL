#!/usr/bin/env python3
"""Renders an ab_session.py session into the markdown tables the report needs.

Separate from the runner on purpose: the runner's job is to COLLECT, and a renderer that is reading
its own session-summary.json cannot influence what was collected. Every number below is a lookup or
an arithmetic step over that file - nothing here re-derives anything from the device.

Usage:
    python render_ab.py <session-dir>/session-summary.json [--title T]
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


def fmt(value, digits=3):
    if value is None:
        return "-"
    if isinstance(value, float):
        return f"{value:.{digits}f}"
    return str(value)


def steady_state_cpu(arm):
    """Per-thread CPU ms/frame over the arm's steady-state frames, from the DEVICE's own two
    independent series.

    WHY NOT the sampler's totals divided by the frame count, which is what this used to report:
    those totals are correct and the per-frame ratio is wrong, because a replay's frames are not
    interchangeable. Measured on the rd12 fixture with this build, frame 1 costs 7332 ms of CPU on
    the client thread - one shader/pipeline bring-up - and the other 250 frames cost 7.7-8.5 ms
    each. Dividing a whole replay's CPU by 251 frames charges every frame 1/251st of a compile that
    happened once, which is why that column read ~32 ms/frame beside a device median of 7.9. It is
    a real number and not a real per-frame cost.

    So the per-frame figures come from the two series the DEVICE already computes over the trailing
    `tailFrames`:

      - `medianFrameCpuMs` / `p95FrameCpuMs`: the client thread's own `clock_gettime(CLOCK_THREAD_
        CPUTIME_ID)` deltas per frame, summarised over the tail. Host-side per-frame sampling cannot
        reproduce this - a 0.5 s poll interval cannot resolve an 8 ms frame - and it is the primary
        metric (MEASUREMENTS.md section 9).
      - `medianFrameMs` / `p95FrameMs`: wall time per frame, which is what the frame budget is.

    The sampler's contribution is the ROLES: which thread carried the work, on which core, and how
    much CPU each role spent in total, which is what the doorbell comparison needs and what the
    device's single client-thread series cannot express.
    """
    best = arm.get("best_repeat") or {}
    return {
        "median_frame_ms": best.get("medianFrameMs"),
        "p95_frame_ms": best.get("p95FrameMs"),
        "median_frame_cpu_ms": best.get("medianFrameCpuMs"),
        "p95_frame_cpu_ms": best.get("p95FrameCpuMs"),
        "mean_frame_cpu_ms": best.get("meanFrameCpuMs"),
        "fps": best.get("fps"),
    }


def role_totals(arm):
    """(client cpu_ms, server cpu_ms) summed over the best repeat's window, and the window length.

    TOTALS, not rates. They answer "which role spent the CPU and how much core-time did it burn",
    and they are directly comparable across arms because every arm here replays the same fixture
    with the same frame count - the one thing that would break the comparison is a different frame
    count, which `repeat_count_matches_windows` and the shared `totalFrames` both guard.
    """
    index = (arm.get("best_repeat") or {}).get("index", 1) - 1
    repeats = arm.get("sampler", {}).get("per_repeat", [])
    if not (0 <= index < len(repeats)):
        return None, None, None
    repeat = repeats[index]
    client = server = 0.0
    seen_c = seen_s = False
    for thread in repeat.get("cpu", {}).get("threads", []):
        if not thread.get("measurable") or thread.get("cpu_ms", 0) <= 0:
            continue
        role = thread.get("role")
        if role == "client-gl":
            client += thread["cpu_ms"]
            seen_c = True
        elif role in ("apply", "server-process"):
            server += thread["cpu_ms"]
            seen_s = True
    return (client if seen_c else None, server if seen_s else None,
            repeat.get("cpu", {}).get("window_seconds"))


def per_repeat_rows(arm):
    """One row per REPLAY, with the frame count taken from the matching benchmark run.

    The CPU windows and the benchmark.json files are two independent records of the same repeats, in
    the same order, and they are joined BY ORDINAL - window N is repeat N. The ordinal is asserted
    rather than assumed (`repeat_count_matches_windows` in the arm summary): a mismatch means one
    record has a replay the other does not, and silently pairing them would attribute one replay's
    CPU to another replay's frames.
    """
    runs = arm.get("benchmark_runs") or []
    rows = []
    for repeat in arm.get("sampler", {}).get("per_repeat", []):
        client = server = 0.0
        seen_c = seen_s = False
        for thread in repeat.get("cpu", {}).get("threads", []):
            if not thread.get("measurable") or thread.get("cpu_ms", 0) <= 0:
                continue
            role = thread.get("role")
            if role == "client-gl":
                client += thread["cpu_ms"]
                seen_c = True
            elif role in ("apply", "server-process"):
                server += thread["cpu_ms"]
                seen_s = True
        index = repeat.get("repeat", 0) - 1
        report = runs[index] if 0 <= index < len(runs) else {}
        rows.append({
            "repeat": repeat.get("repeat"),
            "frames": report.get("totalFrames"),
            "mean_frame_ms": report.get("meanFrameMs"),
            "median_frame_ms": report.get("medianFrameMs"),
            "median_frame_cpu_ms": report.get("medianFrameCpuMs"),
            "fps": report.get("fps"),
            "seconds": repeat.get("cpu", {}).get("window_seconds"),
            "client_total_ms": client if seen_c else None,
            "server_total_ms": server if seen_s else None,
        })
    return rows


def best_frames(arm):
    return arm.get("best_repeat_frames") or (arm.get("best_repeat") or {}).get("totalFrames")


def wait_delta(arm):
    """The wait ledger for the arm, as run totals plus a per-frame rate.

    THE FOUR NUMBERS COME FROM TWO DIFFERENT LOGS ON A SPAWN ARM, and that is structural rather than
    a collection gap. `srv`/`srvpark` are published by the server's own apply loop into the PipeStats
    of the process it runs in, so:

      - under `inproc` both roles are threads of one process, and the client log's line carries all
        four;
      - under `spawn` the apply thread lives in the SERVER process, so its pair is written to
        `mobilegl.server.log` and the client's line prints `srv=0 srvpark=0`. PipeStats.h documents
        that shape: a zero that means "another process" printed like a zero that means "never
        waited". So the pair is read from the server log here and the zeros are NOT reported as
        evidence that the server never waited.

    `cli`/`clipark` are the client's own producer and are always in the client log.

    The per-frame rate divides by the RUN frame count (`frames=`, the third field on that line) and
    NOT by `window=`.

    THOSE ARE TWO DIFFERENT DENOMINATORS AND ONLY ONE OF THEM IS RIGHT HERE. Everything in the
    `bytes/f[...]`, `tex[...]` and `emit[...]` brackets is a WINDOW value (the last `window=` frames),
    but the `wait[...]` bracket and the ring gauges above it are RUN TOTALS - PipeStats.h says so in
    as many words beside the Gauge enum, because "a windowed difference of two counters published by
    two threads at two different moments is not a quantity either of them ever held". Dividing a run
    total by a window length inflates the rate by (run/window) - measured on a 128-frame OpenRA
    replay that is 16x, which looks like a plausible number and is not one.

    WHICH LOG SUPPLIES THE RUN FRAME COUNT DEPENDS ON THE TRANSPORT, and this is the second
    structural asymmetry of the spawn shape. The frame counter is bumped by `PipeStats::OnPresent`,
    which the backend calls on Present, and under `spawn` Present is applied in the SERVER process:

      - `inproc`: one process. The client log carries the session's final line, emitted by
        `PipeStats::Shutdown` at teardown, `frames=251`. The server log's own last line is at its
        previous period boundary (`frames=240`) because the apply thread does not emit the
        session-final one. So the client's number is the run.
      - `spawn`: the client never presents, so its `frames=0 window=0`. That zero is structural - it
        is the same shape as the `srv=0` zero - and dividing by it is undefined. The run's frame
        count is the SERVER's `frames=251`.

    So the denominator is the client's when it is non-zero, and the server's when it is not, and
    `run_frames_source` records which one was used. Using one log's frames with the other log's
    counters - the obvious thing to write - is the mistake this paragraph exists to prevent.
    """
    stats = arm.get("stats") or {}
    client = stats.get("client") or {}
    server = stats.get("server") or {}
    wait = dict(client.get("wait") or {})
    source = "client-log"
    if arm.get("transport") == "spawn":
        server_wait = server.get("wait")
        if server_wait:
            # Replace the client log's structurally-zero server half with the server's own.
            wait["srv"] = server_wait["srv"]
            wait["srvpark"] = server_wait["srvpark"]
            source = "client-log(cli) + server-log(srv)"
        else:
            source = "client-log only (no server-log summary line: srv NOT available)"
    run_frames = client.get("run_frames")
    run_frames_source = "client-log"
    if not run_frames:
        # The spawn shape: this process never presented, so it counted no frames.
        run_frames = server.get("run_frames")
        run_frames_source = "server-log (client frames=0: it never presents under spawn)"
    out = {"source": source, "wait": wait or None, "run_frames": run_frames,
           "run_frames_source": run_frames_source,
           "client_run_frames": client.get("run_frames"),
           "client_window_frames": client.get("window_frames"),
           "server_run_frames": server.get("run_frames"),
           "server_window_frames": server.get("window_frames")}
    if wait and run_frames:
        out["per_frame"] = {key: round(value / run_frames, 4) for key, value in wait.items()}
    return out


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("session")
    parser.add_argument("--title", default=None)
    args = parser.parse_args()

    session = json.loads(Path(args.session).read_text(encoding="utf-8"))
    arms = session.get("arms", [])
    lines = []

    title = args.title or f"A/B session {session.get('label')}"
    lines.append(f"## {title}")
    lines.append("")
    lines.append(f"- session directory: `{session.get('label')}`")
    lines.append(f"- boot id: `{session.get('boot_id')}` "
                 f"(reboot-clean: {session.get('rebooted_clean')})")
    lines.append(f"- package: `{session.get('package')}`")
    lines.append(f"- APK sha256: `{session.get('apk_sha256')}`")
    lines.append("")

    # --- headline: frame time and per-thread CPU ------------------------------------------------
    lines.append("### Frame time and per-thread CPU (best of N)")
    lines.append("")
    lines.append("The `frame cpu` column is the CLIENT thread's own per-frame CPU clock, summarised by the")
    lines.append("device over the trailing `tailFrames` - the primary metric (MEASUREMENTS.md section 9).")
    lines.append("`client total` / `server total` are the sampler's per-role CPU TOTALS over that repeat,")
    lines.append("which includes the one-off shader bring-up frame; they are not per-frame rates. See")
    lines.append("steady_state_cpu in this file.")
    lines.append("")
    lines.append("| arm | transport | status | frames | tail | mean ms | p50 ms | p95 ms | fps | frame cpu p50 ms | cpu p95 ms | client total ms | server total ms |")
    lines.append("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for arm in arms:
        best = arm.get("best_repeat") or {}
        client_total, server_total, _window = role_totals(arm)
        lines.append(
            f"| {arm.get('label')} | {arm.get('transport')} | {arm.get('status')} "
            f"| {fmt(best.get('totalFrames'))} | {fmt(best.get('tailFrames'))} "
            f"| {fmt(best.get('meanFrameMs'))} | {fmt(best.get('medianFrameMs'))} "
            f"| {fmt(best.get('p95FrameMs'))} | {fmt(best.get('fps'), 2)} "
            f"| {fmt(best.get('medianFrameCpuMs'))} | {fmt(best.get('p95FrameCpuMs'))} "
            f"| {fmt(client_total, 1)} | {fmt(server_total, 1)} |")
    lines.append("")

    # --- the pair deltas ------------------------------------------------------------------------
    lines.append("### Paired deltas (inproc = A, spawn = B)")
    lines.append("")
    lines.append("| pair | metric | A | B | B - A | B/A - 1 |")
    lines.append("|---|---|---|---|---|---|")
    by_key = {}
    for arm in arms:
        by_key.setdefault((arm.get("case"), arm.get("backend"), arm.get("transport")), arm)
    seen_pairs = set()
    for (case, backend, _transport) in by_key:
        if (case, backend) in seen_pairs:
            continue
        seen_pairs.add((case, backend))
        a = by_key.get((case, backend, "inproc"))
        b = by_key.get((case, backend, "spawn"))
        if not a or not b:
            continue
        for metric, getter in (
            ("mean frame ms", lambda x: (x.get("best_repeat") or {}).get("meanFrameMs")),
            ("p50 frame ms", lambda x: (x.get("best_repeat") or {}).get("medianFrameMs")),
            ("p95 frame ms", lambda x: (x.get("best_repeat") or {}).get("p95FrameMs")),
            ("client frame cpu p50 ms", lambda x: (x.get("best_repeat") or {}).get("medianFrameCpuMs")),
            ("client frame cpu p95 ms", lambda x: (x.get("best_repeat") or {}).get("p95FrameCpuMs")),
            ("fps", lambda x: (x.get("best_repeat") or {}).get("fps")),
        ):
            va, vb = getter(a), getter(b)
            if va and vb:
                lines.append(f"| {case}/{backend} | {metric} | {fmt(va)} | {fmt(vb)} "
                             f"| {fmt(vb - va)} | {fmt(vb / va - 1, 4)} |")
        for label, index in (("client cpu total ms", 0), ("server cpu total ms", 1)):
            va, vb = role_totals(a)[index], role_totals(b)[index]
            if va and vb:
                lines.append(f"| {case}/{backend} | {label} | {fmt(va, 1)} | {fmt(vb, 1)} "
                             f"| {fmt(vb - va, 1)} | {fmt(vb / va - 1, 4)} |")
    lines.append("")

    # --- the doorbell ledger --------------------------------------------------------------------
    lines.append("### The wait ledger")
    lines.append("")
    lines.append("The `wait[...]` bracket and the ring gauges are RUN TOTALS, so these rates divide by the run's")
    lines.append("frame count (`frames=`), not by the `window=` of the same line. See wait_delta. The `used`")
    lines.append("column names the log the frame count came from: under `spawn` the client's own `frames=` is")
    lines.append("**structurally 0**, because it never presents - present is applied in the server process.")
    lines.append("")
    lines.append("| arm | counters from | frames from | used frames | cli frames= | srv frames= | srv | srvpark | cli | clipark | srv/f | srvpark/f | cli/f | clipark/f |")
    lines.append("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for arm in arms:
        entry = wait_delta(arm)
        wait = entry["wait"] or {}
        per = entry.get("per_frame") or {}
        lines.append(
            f"| {arm.get('label')} | {entry['source']} | {entry['run_frames_source']} "
            f"| {fmt(entry.get('run_frames'))} | {fmt(entry.get('client_run_frames'))} "
            f"| {fmt(entry.get('server_run_frames'))} "
            f"| {fmt(wait.get('srv'))} | {fmt(wait.get('srvpark'))} | {fmt(wait.get('cli'))} "
            f"| {fmt(wait.get('clipark'))} | {fmt(per.get('srv'))} | {fmt(per.get('srvpark'))} "
            f"| {fmt(per.get('cli'))} | {fmt(per.get('clipark'))} |")
    lines.append("")

    # --- per repeat -----------------------------------------------------------------------------
    lines.append("### Per-repeat windows")
    lines.append("")
    for arm in arms:
        lines.append(f"**{arm.get('label')}** (windows={arm.get('sampler', {}).get('windows')}, "
                     f"repeats={len(arm.get('benchmark_runs') or [])}, "
                     f"replay={arm.get('best_repeat', {}).get('meanFrameMs')})")
        lines.append("")
        lines.append("| repeat | frames | window s | p50 frame ms | frame cpu p50 ms | client total ms | server total ms |")
        lines.append("|---|---|---|---|---|---|---|")
        for row in per_repeat_rows(arm):
            lines.append(f"| {fmt(row['repeat'])} | {fmt(row['frames'])} | {fmt(row['seconds'], 1)} "
                         f"| {fmt(row['median_frame_ms'])} | {fmt(row['median_frame_cpu_ms'])} "
                         f"| {fmt(row['client_total_ms'], 1)} | {fmt(row['server_total_ms'], 1)} |")
        lines.append("")

    # --- byte classes and records ---------------------------------------------------------------
    lines.append("### Wire byte classes and record size (gate 8 items 2-3 input)")
    lines.append("")
    lines.append("| arm | role | window | frames | buf/f | tex/f | ubog/f | ubon/f | csob-blob/f | resid/f | vtxc/f | idxc/f | icmd/f | pmap/f | maxrec | maxcap |")
    lines.append("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for arm in arms:
        for role in ("client", "server"):
            stats = (arm.get("stats") or {}).get(role)
            if not stats:
                continue
            bytes_per_frame = stats.get("bytes_per_frame") or {}
            lines.append(
                f"| {arm.get('label')} | {role} | {fmt(stats.get('window_frames'))} "
                f"| {fmt(stats.get('run_frames'))} "
                f"| {fmt(bytes_per_frame.get('buf'))} | {fmt(bytes_per_frame.get('tex'))} "
                f"| {fmt(bytes_per_frame.get('ubog'))} | {fmt(bytes_per_frame.get('ubon'))} "
                f"| {fmt(bytes_per_frame.get('csob-blob'))} | {fmt(bytes_per_frame.get('resid'))} "
                f"| {fmt(bytes_per_frame.get('vtxc'))} | {fmt(bytes_per_frame.get('idxc'))} "
                f"| {fmt(bytes_per_frame.get('icmd'))} | {fmt(bytes_per_frame.get('pmap'))} "
                f"| {fmt(stats.get('max_record_bytes'))} | {fmt(stats.get('max_record_cap'))} |")
    lines.append("")

    # --- arm proof ------------------------------------------------------------------------------
    lines.append("### Arm proof and pin state")
    lines.append("")
    lines.append("| arm | pin before | pin after | server pid (spawn) | problems |")
    lines.append("|---|---|---|---|---|")
    for arm in arms:
        problems = "; ".join(arm.get("problems") or []) or "none"
        lines.append(f"| {arm.get('label')} | {fmt(arm.get('pin_before_rc'))} "
                     f"| {fmt(arm.get('pin_after_rc'))} | {fmt(arm.get('server_pid'))} "
                     f"| {problems} |")
    lines.append("")

    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
