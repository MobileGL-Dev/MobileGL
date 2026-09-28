#!/usr/bin/env python3
"""Render the perf-compare report tables from summary.json + per-run artefacts.

Usage: make_report.py [results_dir]
Defaults to <repo>/.trace-work/perf-compare (run_matrix.sh's output root).
"""
import json
import re
import statistics
import sys
from pathlib import Path

ROOT = (Path(sys.argv[1]).resolve() if len(sys.argv) > 1
        else Path(__file__).resolve().parents[3] / ".trace-work" / "perf-compare")
ARMS = ["monolith", "inproc", "shm", "tcp"]
ARM_LABEL = {
    "monolith": "monolith",
    "inproc": "inproc",
    "shm": "spawn+shm",
    "tcp": "spawn+tcp-local",
}


def fmt(v, nd=3):
    if v is None:
        return "-"
    if isinstance(v, float):
        return f"{v:.{nd}f}"
    return str(v)


def load_json(path):
    try:
        return json.loads(Path(path).read_text(encoding="utf-8"))
    except Exception:
        return None


def load():
    return json.loads((ROOT / "summary.json").read_text(encoding="utf-8"))


def perf_table(data, wl):
    runs = [r for r in data["runs"] if r["workload"] == wl and r["mode"] == "benchmark"]
    reps = sorted({r["rep"] for r in runs}, key=lambda x: int(x))
    lines = [
        f"### {wl} — raw perf runs (benchmark lane)",
        "| rep | arm | status | fps | mean ms | median ms | p95 ms | frames | tail s | dur s | temp d°C |",
        "|---|---|---|---|---|---|---|---|---|---|---|",
    ]
    for rep in reps:
        for arm in ARMS:
            r = next((x for x in runs if x["rep"] == rep and x["arm"] == arm), None)
            if r is None:
                lines.append(f"| {rep} | {ARM_LABEL[arm]} | MISSING | - | - | - | - | - | - | - | - |")
                continue
            lines.append(
                f"| {rep} | {ARM_LABEL[arm]} | {r['status']} | {fmt(r['fps'],1)} | {fmt(r['mean_ms'])} "
                f"| {fmt(r['median_ms'])} | {fmt(r['p95_ms'])} | {fmt(r['total_frames'],0)} "
                f"| {fmt(r['total_seconds'],2)} | {fmt(r['duration_s'],0)} | {fmt(r['battery_temp_dc'],0)} |"
            )
    return lines


def agg_table(data, wl):
    aggs = [a for a in data["aggregates"] if a["workload"] == wl]
    by_arm = {a["arm"]: a for a in aggs}
    ns = ", ".join(str(by_arm[a]["n_ok"]) for a in ARMS if a in by_arm)
    lines = [
        f"### {wl} — per-arm aggregates (best-of-N by mean; N={ns})",
        "| arm | best fps | best mean ms | best median ms | best p95 ms | median-of-means ms | spread (min..max mean) | spread % |",
        "|---|---|---|---|---|---|---|---|",
    ]
    for arm in ARMS:
        a = by_arm.get(arm)
        if not a:
            lines.append(f"| {ARM_LABEL[arm]} | - | - | - | - | - | - | - |")
            continue
        lo, hi = a["spread_mean_ms"]
        lines.append(
            f"| {ARM_LABEL[arm]} | {fmt(a['best_fps'],1)} | {fmt(a['best_mean_ms'])} | {fmt(a['best_median_ms'])} "
            f"| {fmt(a['best_p95_ms'])} | {fmt(a['median_of_mean_ms'])} | {fmt(lo)}..{fmt(hi)} | {fmt(a['spread_pct'],1)} |"
        )
    return lines


def ssim_table(data, wl):
    runs = [r for r in data["runs"] if r["workload"] == wl and r["mode"] == "ssim"]
    lines = [
        f"### {wl} — ssim lane (non-benchmark run to target_call, compared to golden)",
        "| arm | status | passed | ssim | message |",
        "|---|---|---|---|---|",
    ]
    for arm in ARMS:
        r = next((x for x in runs if x["arm"] == arm), None)
        if r is None:
            lines.append(f"| {ARM_LABEL[arm]} | MISSING | - | - | - |")
            continue
        msg = (r.get("message") or "")[:90]
        lines.append(f"| {ARM_LABEL[arm]} | {r['status']} | {r['passed']} | {fmt(r.get('ssim'),6)} | {msg} |")
    return lines


def marker_short(v):
    tm = v.get("TRANSPORT_MARKER", v.get("SPLIT_MARKER", "?"))
    if tm.endswith(":OK"):
        return tm.rsplit(":", 1)[0] + ":OK"
    if tm.endswith(":MISSING"):
        return tm.rsplit(":", 1)[0] + ":MISSING"
    return tm


def verify_table(data, wl):
    runs = [r for r in data["runs"] if r["workload"] == wl and r["mode"] == "benchmark"]
    lines = [
        f"### {wl} — arm verification (per run)",
        "| rep | arm | transport marker | 2nd-process proof | run-ahead | lockstep/DISARMED | PipeStats lines | Fatal |",
        "|---|---|---|---|---|---|---|---|",
    ]
    reps = sorted({r["rep"] for r in runs}, key=lambda x: int(x))
    for rep in reps:
        for arm in ARMS:
            r = next((x for x in runs if x["rep"] == rep and x["arm"] == arm), None)
            if r is None:
                continue
            v = r.get("verify", {})
            second = v.get("SECOND_PROCESS_SPAWN_ARMED", v.get("TCP_SESSION", "-"))
            second = "OK" if second.startswith("OK") else second
            ra = "OK" if v.get("RUN_AHEAD_ARMED", "").startswith("OK") else v.get("RUN_AHEAD_ARMED", "?")
            lock = "lockstep!" if v.get("LOCKSTEP_WARNING", "").startswith("PRESENT") else (
                "DISARMED!" if v.get("DISARMED", "").startswith("PRESENT") else "none")
            lines.append(
                f"| {rep} | {ARM_LABEL[arm]} | {marker_short(v)} | {second} | {ra} | {lock} "
                f"| {v.get('PIPESTATS_LINES','-')} | {v.get('FATAL_LINES','-')} |"
            )
    return lines


def linkmetrics_rollup(path):
    """Sum wait_replies / stage_bytes / transport_wait_ns over kind=frame lines."""
    if not path.exists():
        return None
    tot = {"frames": 0, "wait_replies": 0, "stage_bytes": 0, "transport_wait_ns": 0}
    seen = set()
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if "P65LinkMetrics kind=frame " not in line:
            continue
        m_frame = re.search(r"frame=(\d+)", line)
        if not m_frame:
            continue
        fid = int(m_frame.group(1))
        if fid in seen:
            continue
        seen.add(fid)
        tot["frames"] += 1
        for k in ("wait_replies", "stage_bytes", "transport_wait_ns"):
            m = re.search(k + r"=(\d+)", line)
            if m:
                tot[k] += int(m.group(1))
    return tot if seen else None


def pipestats_rollup(wl_dir, arm, rep):
    """Merge client/server pipestats JSON dumps for one run into one counter dict."""
    d = wl_dir / f"{arm}-r{rep}"
    if not d.is_dir():
        return None
    dumps = [load_json(d / f) for f in ("pipestats.client.json", "pipestats.server.json", "tcp-server-pipestats.json")]
    dumps = [x for x in dumps if x]
    if not dumps:
        return None
    frames = max((x.get("frames", 0) or 0) for x in dumps)
    by = lambda key, sub: sum((x.get(key, {}) or {}).get(sub, 0) or 0 for x in dumps)
    wire = {}
    for x in dumps:
        for k, v in (x.get("wire", {}) or {}).items():
            wire[k] = wire.get(k, 0) + (v or 0)
    lm = linkmetrics_rollup(d / "linkmetrics-lines.txt")
    return {
        "frames": frames,
        "stage_seg": by("bytes", "stage-segment-bytes"),
        "stage_buf": by("bytes", "stage-buffer"),
        "stage_tex": by("bytes", "stage-texture"),
        "stage_pmap": by("bytes", "persistent-map-push"),
        "wire_records": by("calls", "wire-records"),
        "draws": by("calls", "draws"),
        "srv": wire.get("server-waits", 0),
        "srvpark": wire.get("server-parks", 0),
        "cli": wire.get("client-waits", 0),
        "clipark": wire.get("client-parks", 0),
        "maxrec": wire.get("max-record-bytes", 0),
        "maxcap": wire.get("max-record-bytes-cap", 0),
        "ringwraps": wire.get("ring-wraps", 0),
        "ringwaits": wire.get("ring-waits", 0),
        "lm": lm,
    }


def pipiestats_table(wl):
    lines = [
        f"### {wl} — PipeStats run totals (client+server JSON dumps merged) & LinkMetrics",
        "| arm | rep | frames | srv | srvpark | cli | clipark | stage seg B | stage buf B | stage tex B | wire-records | maxrec/cap | "
        "LM frames | LM wait_replies | LM stage B | LM transport-wait ms |",
        "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|",
    ]
    wl_dir = ROOT / wl
    if not wl_dir.is_dir():
        return ["(no data)"]
    for arm in ARMS:
        for rep in ("1", "2", "3"):
            s = pipestats_rollup(wl_dir, arm, rep)
            if s is None:
                continue
            lm = s["lm"] or {}
            tw = lm.get("transport_wait_ns")
            lines.append(
                f"| {ARM_LABEL[arm]} | {rep} | {s['frames']} | {s['srv']} | {s['srvpark']} | {s['cli']} | {s['clipark']} "
                f"| {s['stage_seg']} | {s['stage_buf']} | {s['stage_tex']} | {s['wire_records']} "
                f"| {s['maxrec']}/{s['maxcap']} "
                f"| {lm.get('frames','-')} | {lm.get('wait_replies','-')} | {lm.get('stage_bytes','-')} "
                f"| {fmt(tw/1e6,1) if tw is not None else '-'} |"
            )
    return lines


def main():
    data = load()
    out = [
        "# MobileGL transport perf comparison — 2f7cbe2e (Qualcomm Adreno, Android 16, aarch64, no root)",
        "",
        "Arms: `monolith` / `inproc` / `spawn+shm` (fork control, shm data) / `spawn+tcp-local` "
        "(tcp://127.0.0.1:40613 control, stream data). Interleaved A-B-C-D rounds, no root/no frequency "
        "pinning — spread columns show run-to-run scatter.",
        "",
        "Perf numbers are the device's own benchmark summary over the trailing `tailFrames` window. "
        "openra's 128-frame trace fits entirely in the default tail of 200 (so its `mean` carries the "
        "one-off setup spikes of frames 1 and 5); rd12's 251 frames are summarized over the last 200, "
        "which excludes the 6.2 s first-frame shader/asset setup.",
        "",
    ]
    for wl in ("openra", "rd12"):
        if not any(r["workload"] == wl for r in data["runs"]):
            continue
        out.append(f"## Workload: {wl}")
        out.append("")
        out += perf_table(data, wl)
        out.append("")
        out += agg_table(data, wl)
        out.append("")
        out += ssim_table(data, wl)
        out.append("")
        out += verify_table(data, wl)
        out.append("")
        out += pipiestats_table(wl)
        out.append("")
    (ROOT / "REPORT.md").write_text("\n".join(out) + "\n", encoding="utf-8")
    print(f"wrote {ROOT / 'REPORT.md'}")


if __name__ == "__main__":
    main()
