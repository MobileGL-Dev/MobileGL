#!/usr/bin/env python3
"""60-reduce.py <stamp> [--logroot DIR] [--tools TREE] [--json OUT]

Device window 2 verdicts, read from the window's own output directory (<logroot>/<stamp>):

GATE 3 (CONTRACT-P7 7.2), per case of gate3/cases.txt:
  * monolith x1 is the same-session control; it must itself pass;
  * inproc and spawn: every expected repeat present and error-free, each repeat's arm proof
    (transport-proof.json passed), the device's own pass verdict (ssim >= case threshold),
    ALL repeats bit-identical (actual PNG SHA-256), and |ssim - monolith ssim| <= 0.0005 on every
    repeat; a repeat whose logcat is byte-identical to the previous repeat's is flagged STALE;
  * OpenRA: 1.000000 and mismatch 0 on every repeat of both split arms AND on monolith;
  * inproc-ra0 (RUN_AHEAD=0): recorded (ssim, sha, run-ahead=0 proof), never gating.
BSL STATS: per arm, maps-line / VmRSS / VmHWM peaks and the wbuf[] gauges.
CTS AFTER (CONTRACT-P7 7.3), per block vs device-window-1/CTS-base/report-<block>.json, over the
  cases the AFTER run was asked to run (so a --limit subset is compared like for like):
  rate = Pass / (results - NotSupported)  [every non-NS outcome - Fail, Crash, warnings - is in the
  denominator; NS excluded], delta in percentage points, PASS iff delta >= -0.5 pp, no new crash
  (Crash/Timeout/InternalError/ResourceError/DeviceHang/Incomplete not already so in BASE) and the
  block is complete (unrun.txt empty).
Exit status: 0 when every section that has data is PASS, 1 otherwise, 2 on bad input.
"""
import argparse
import hashlib
import json
import os
import re
import sys
from pathlib import Path

TOL = 0.0005
CTS_TOL_PP = 0.5
HARD = {"Crash", "Timeout", "InternalError", "ResourceError", "DeviceHang", "Incomplete"}
SPLIT_ARMS = ("inproc", "spawn")
OPENRA = "OpenRA"


def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def read_kv(path):
    out = {}
    if path.is_file():
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            if "=" in line:
                k, v = line.split("=", 1)
                out[k.strip()] = v.strip()
    return out


# ------------------------------------------------------------------------------------------------
# gate 3
# ------------------------------------------------------------------------------------------------
def gate3(out, ca):
    g = out / "gate3"
    cases_file = g / "cases.txt"
    if not cases_file.is_file():
        return None
    cases = [c for c in cases_file.read_text().split() if c]
    excluded = (g / "excluded.txt").read_text().split() if (g / "excluded.txt").is_file() else []
    meta = read_kv(g / "arms.txt")
    repeat = int(meta.get("repeat", "3"))
    arms_present = [a for a in ("monolith", "inproc", "spawn", "inproc-ra0") if (g / "archive" / a).is_dir()]
    summaries = {a: ca.summarize(g / "archive" / a) for a in arms_present}

    def rows(arm, case):
        s = summaries.get(arm)
        if not s:
            return []
        return [r for r in s["rows"] if r["arm"] == case + "-DirectVulkan"]

    def proof(row):
        p = Path(row["dir"]) / "transport-proof.json"
        try:
            return json.loads(p.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return None

    def logcat_sha(row):
        p = Path(row["dir"]) / "logcat.txt"
        return sha(p) if p.is_file() else None

    def ra0_proof(row):
        p = Path(row["dir"]) / "mobilegl.log"
        if not p.is_file():
            return "no-log"
        lines = re.findall(r"Config: IPC[^\r\n]*", p.read_text(encoding="utf-8", errors="replace"))
        if lines and all(re.search(r"\brun-ahead=0\b", line) for line in lines):
            return "run-ahead=0"
        return "NOT-PROVEN(" + ";".join(lines)[:120] + ")"

    results = []
    for case in cases:
        reasons, incomplete = [], []
        entry = {"case": case, "arms": {}}
        mono = rows("monolith", case)
        mono_ssim = None
        if len(mono) != 1 or mono[0].get("error"):
            incomplete.append("monolith: %s" % (mono[0].get("error") if mono else "not run"))
        else:
            m = mono[0]
            mono_ssim = m.get("ssim_vs_golden")
            entry["arms"]["monolith"] = {"ssim": mono_ssim, "sha": m.get("actual_sha256"),
                                         "mismatch": m.get("mismatch_pixels_vs_golden"), "passed": m.get("passed")}
            if m.get("passed") is not True:
                reasons.append("monolith control did not pass (ssim %s)" % mono_ssim)
            if case == OPENRA and not (m.get("mismatch_pixels_vs_golden") == 0 and (mono_ssim or 0) >= 0.9999995):
                reasons.append("OpenRA monolith not 1.000000/0 (%s/%s)" % (mono_ssim, m.get("mismatch_pixels_vs_golden")))
        for arm in SPLIT_ARMS:
            rs = rows(arm, case)
            a = {"repeats": len(rs), "ssim": [r.get("ssim_vs_golden") for r in rs],
                 "sha": sorted({r.get("actual_sha256") for r in rs if r.get("actual_sha256")})}
            entry["arms"][arm] = a
            if arm not in summaries:
                incomplete.append(arm + ": not run")
                continue
            if len(rs) != repeat:
                incomplete.append("%s: %d/%d repeats" % (arm, len(rs), repeat))
            for r in rs:
                if r.get("error"):
                    incomplete.append("%s rep%d: %s" % (arm, r["repeat"], r["error"]))
                    continue
                if r.get("passed") is not True:
                    reasons.append("%s rep%d below threshold (ssim %s)" % (arm, r["repeat"], r.get("ssim_vs_golden")))
                p = proof(r)
                if not p or p.get("passed") is not True:
                    reasons.append("%s rep%d arm proof not passed" % (arm, r["repeat"]))
                if mono_ssim is not None and r.get("ssim_vs_golden") is not None:
                    d = abs(r["ssim_vs_golden"] - mono_ssim)
                    if d > TOL:
                        reasons.append("%s rep%d |ssim-monolith|=%.6f > %.4f" % (arm, r["repeat"], d, TOL))
                if case == OPENRA and not (r.get("mismatch_pixels_vs_golden") == 0 and (r.get("ssim_vs_golden") or 0) >= 0.9999995):
                    reasons.append("OpenRA %s rep%d not 1.000000/0 (%s/%s)" % (arm, r["repeat"], r.get("ssim_vs_golden"), r.get("mismatch_pixels_vs_golden")))
            shas = [r.get("actual_sha256") for r in rs if r.get("actual_sha256")]
            if len(set(shas)) > 1:
                reasons.append("%s repeats NOT bit-identical (%d distinct pictures)" % (arm, len(set(shas))))
            lc = [logcat_sha(r) for r in rs]
            for i in range(1, len(lc)):
                if lc[i] and lc[i] == lc[i - 1]:
                    reasons.append("%s rep%d STALE (logcat identical to rep%d)" % (arm, rs[i]["repeat"], rs[i - 1]["repeat"]))
        ra0 = rows("inproc-ra0", case)
        if ra0:
            r = ra0[0]
            entry["arms"]["inproc-ra0"] = {"ssim": r.get("ssim_vs_golden"), "sha": r.get("actual_sha256"),
                                           "passed": r.get("passed"), "proof": ra0_proof(r), "error": r.get("error")}
        ip = set(entry["arms"].get("inproc", {}).get("sha", []))
        sp = set(entry["arms"].get("spawn", {}).get("sha", []))
        entry["inproc_eq_spawn"] = bool(ip) and ip == sp
        entry["split_eq_monolith"] = bool(ip) and ip == sp == {entry["arms"].get("monolith", {}).get("sha")}
        entry["verdict"] = "INCOMPLETE" if incomplete else ("FAIL" if reasons else "PASS")
        entry["reasons"] = incomplete + reasons
        results.append(entry)
    verdicts = [e["verdict"] for e in results]
    overall = "PASS" if verdicts and all(v == "PASS" for v in verdicts) else (
        "INCOMPLETE" if "INCOMPLETE" in verdicts and "FAIL" not in verdicts else "FAIL")
    return {"cases": results, "excluded": excluded, "repeat": repeat, "denominator": len(cases),
            "overall": overall, "arms_present": arms_present}


def print_gate3(g3):
    print("== GATE 3 (DirectVulkan x pbuffer; split repeat=%d; tol |ssim-monolith| <= %.4f) ==" % (g3["repeat"], TOL))
    w = max([len(e["case"]) for e in g3["cases"]] + [4])
    print("%-*s  %-10s  %-11s  %-24s  %-24s  %-11s  %s" % (w, "case", "verdict", "monolith", "inproc(min..max) #sha", "spawn(min..max) #sha", "ra0", "i==s==m"))
    for e in g3["cases"]:
        def cell(arm):
            a = e["arms"].get(arm)
            if not a or not a.get("ssim"):
                return "-"
            vals = [v for v in a["ssim"] if isinstance(v, (int, float))]
            if not vals:
                return "err"
            return "%.6f..%.6f #%d" % (min(vals), max(vals), len(a["sha"]))
        mono = e["arms"].get("monolith", {}).get("ssim")
        ra0 = e["arms"].get("inproc-ra0", {}).get("ssim")
        print("%-*s  %-10s  %-11s  %-24s  %-24s  %-11s  %s" % (
            w, e["case"], e["verdict"], "%.6f" % mono if isinstance(mono, (int, float)) else "-",
            cell("inproc"), cell("spawn"), "%.6f" % ra0 if isinstance(ra0, (int, float)) else "-",
            "yes" if e["split_eq_monolith"] else ("i==s" if e["inproc_eq_spawn"] else "no")))
        for reason in e["reasons"]:
            print("%-*s    ! %s" % (w, "", reason))
    ra0s = [(e["case"], e["arms"]["inproc-ra0"]) for e in g3["cases"] if "inproc-ra0" in e["arms"]]
    if ra0s:
        print("-- RUN_AHEAD=0 control (recorded, not gating): %d case(s); proofs: %s" % (
            len(ra0s), ", ".join(sorted({r["proof"] for _, r in ra0s}))))
    print("-- excluded (red on monolith, CONTRACT-P7 7.1): " + (", ".join(g3["excluded"]) or "none"))
    n = len(g3["cases"])
    npass = sum(e["verdict"] == "PASS" for e in g3["cases"])
    note = "" if n == 36 else "  (denominator is %d, not the gate's 36: a subset run is NOT a gate verdict)" % n
    print("GATE3 %s: %d/%d cases PASS%s" % (g3["overall"], npass, n, note))


# ------------------------------------------------------------------------------------------------
# bsl stats
# ------------------------------------------------------------------------------------------------
def bsl(out, ca):
    b = out / "bsl"
    if not b.is_dir():
        return None
    arms = {}
    for arm_dir in sorted(p for p in b.iterdir() if p.is_dir()):
        a = {"done": read_kv(arm_dir / ".done")}
        a["peaks"] = (arm_dir / "peaks.txt").read_text() if (arm_dir / "peaks.txt").is_file() else ""
        a["wbuf"] = (arm_dir / "wbuf-gauges.txt").read_text() if (arm_dir / "wbuf-gauges.txt").is_file() else ""
        if (arm_dir / "archive").is_dir():
            s = ca.summarize(arm_dir / "archive")
            a["replay"] = [{k: r.get(k) for k in ("passed", "ssim_vs_golden", "actual_sha256", "error")} for r in s["rows"]]
        arms[arm_dir.name] = a
    return arms


def print_bsl(arms):
    print("== BSL-ESC-MENU MOBILEGL_PIPE_STATS (maps lines / RSS peaks, wbuf[] gauges) ==")
    for arm, a in arms.items():
        print("[%s] %s" % (arm, " ".join("%s=%s" % kv for kv in a["done"].items())))
        for r in a.get("replay", []):
            print("  replay: passed=%s ssim=%s sha=%s %s" % (r["passed"], r["ssim_vs_golden"],
                                                            (r["actual_sha256"] or "-")[:16], r.get("error") or ""))
        for line in a["peaks"].splitlines():
            print("  " + line)
        for line in a["wbuf"].splitlines() or ["(no wbuf[] line in the archived logs)"]:
            print("  " + line.strip())


# ------------------------------------------------------------------------------------------------
# CTS AFTER vs BASE
# ------------------------------------------------------------------------------------------------
def read_list(path):
    if not path.is_file():
        return []
    return [l.strip() for l in path.read_text(encoding="utf-8").splitlines() if l.strip() and not l.startswith("#")]


def rate(statuses):
    p = sum(1 for s in statuses if s == "Pass")
    ns = sum(1 for s in statuses if s == "NotSupported")
    f = sum(1 for s in statuses if s == "Fail")
    d = len(statuses) - ns
    return {"pass": p, "ns": ns, "fail": f, "other": d - p - f, "denominator": d,
            "rate": (p / d) if d else None, "literal_p_over_p_plus_f": (p / (p + f)) if (p + f) else None}


def cts(out, base_dir):
    c = out / "cts"
    if not c.is_dir():
        return None
    blocks = []
    for report in sorted(c.glob("report-*.json")):
        b = report.stem[len("report-"):]
        run = c / "runs" / b
        after = json.loads(report.read_text(encoding="utf-8"))["results"]
        base_path = base_dir / ("report-%s.json" % b)
        if not base_path.is_file():
            blocks.append({"block": b, "verdict": "NO-BASE", "reasons": ["no " + str(base_path)]})
            continue
        base = json.loads(base_path.read_text(encoding="utf-8"))["results"]
        caselist = Path((run / "caselist.path").read_text().strip()) if (run / "caselist.path").is_file() else None
        universe = read_list(caselist) if caselist else sorted(after)
        skipped = set(read_list(run / "skipped.txt"))
        universe = [x for x in universe if x not in skipped]
        unrun = read_list(run / "unrun.txt")
        hung = read_list(run / "hung.txt")
        a_st = [after[x] for x in universe if x in after]
        b_st = [base[x] for x in universe if x in base]
        ra, rb = rate(a_st), rate(b_st)
        delta = None if ra["rate"] is None or rb["rate"] is None else (ra["rate"] - rb["rate"]) * 100.0
        new_crash = sorted(x for x in universe if after.get(x) in HARD and base.get(x) not in HARD)
        new_crash += sorted(x for x in hung if x not in new_crash)
        regress = sorted(x for x in universe if base.get(x) == "Pass" and after.get(x) not in (None, "Pass"))
        fixed = sorted(x for x in universe if base.get(x) not in (None, "Pass") and after.get(x) == "Pass")
        missing_base = sorted(x for x in universe if x not in base)
        reasons = []
        verdict = "PASS"
        if unrun or len(a_st) < len(universe):
            verdict = "INCOMPLETE"
            reasons.append("%d case(s) without an AFTER result (unrun.txt %d)" % (len(universe) - len(a_st), len(unrun)))
        if new_crash:
            verdict = "FAIL"
            reasons.append("%d NEW crash(es)" % len(new_crash))
        if delta is not None and delta < -CTS_TOL_PP:
            verdict = "FAIL"
            reasons.append("delta %.3f pp < -%.1f pp" % (delta, CTS_TOL_PP))
        if delta is not None and delta > CTS_TOL_PP:
            reasons.append("note: AFTER is %.3f pp ABOVE BASE (not a regression; review the fixed list)" % delta)
        blocks.append({"block": b, "cases": len(universe), "after": ra, "base": rb, "delta_pp": delta,
                       "new_crash": new_crash, "regressed": regress, "fixed": fixed, "missing_in_base": missing_base,
                       "unrun": len(unrun), "hung": hung, "verdict": verdict, "reasons": reasons,
                       "subset": bool(caselist and "first" in caselist.name)})
    verdicts = [b["verdict"] for b in blocks]
    overall = "PASS" if blocks and all(v == "PASS" for v in verdicts) else ("FAIL" if "FAIL" in verdicts else "INCOMPLETE")
    return {"blocks": blocks, "overall": overall}


def print_cts(c):
    print("== CTS AFTER (inproc x DirectVulkan) vs BASE (monolith x DirectVulkan, p7w1) ==")
    print("rate = Pass/(results-NotSupported); gate: delta >= -%.1f pp and 0 new crash per block" % CTS_TOL_PP)
    print("%-14s %6s  %-26s  %-26s  %9s  %5s  %5s  %5s  %s" % ("block", "cases", "BASE P/NS/F/X  rate", "AFTER P/NS/F/X rate", "delta pp", "crash", "regr", "fixed", "verdict"))
    for b in c["blocks"]:
        if "after" not in b:
            print("%-14s  %s %s" % (b["block"], b["verdict"], "; ".join(b["reasons"])))
            continue
        def cell(r):
            return "%d/%d/%d/%d %s" % (r["pass"], r["ns"], r["fail"], r["other"],
                                       "%.3f%%" % (100 * r["rate"]) if r["rate"] is not None else "n/a")
        print("%-14s %6d  %-26s  %-26s  %9s  %5d  %5d  %5d  %s%s" % (
            b["block"], b["cases"], cell(b["base"]), cell(b["after"]),
            "%+.3f" % b["delta_pp"] if b["delta_pp"] is not None else "n/a",
            len(b["new_crash"]), len(b["regressed"]), len(b["fixed"]), b["verdict"],
            " (SUBSET)" if b["subset"] else ""))
        for reason in b["reasons"]:
            print("    ! " + reason)
        for x in b["new_crash"]:
            print("    NEW CRASH  " + x)
        for x in b["regressed"][:40]:
            print("    Pass->not  " + x)
        if len(b["regressed"]) > 40:
            print("    ... %d more regressions in the JSON" % (len(b["regressed"]) - 40))
        for x in b["fixed"][:20]:
            print("    not->Pass  " + x)
    print("CTS %s" % c["overall"] + (" (SUBSET run: not a gate verdict)" if any(b.get("subset") for b in c["blocks"]) else ""))


def print_timing(out):
    t = out / "timing.tsv"
    if not t.is_file():
        return
    print("== TIMING ==")
    for line in t.read_text().splitlines():
        step, start, end, secs, note = (line.split("\t") + [""] * 5)[:5]
        print("  %-18s %6ss  %s -> %s  %s" % (step, secs, start, end, note))
    prog = out / "gate3" / "progress.tsv"
    if prog.is_file():
        per = {}
        for line in prog.read_text().splitlines():
            arm, case, rc, secs, _ = (line.split("\t") + [""] * 5)[:5]
            per.setdefault(arm, []).append(int(secs))
        for arm, v in per.items():
            print("  gate3 %-11s %2d case invocations, mean %.0fs, total %ds" % (arm, len(v), sum(v) / len(v), sum(v)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("stamp")
    ap.add_argument("--logroot", default=os.environ.get("W2_LOGROOT", str(Path.home() / "w7/logs/p7w7")))
    ap.add_argument("--tools", help="MobileGL tree holding compare_actuals.py and CTS-base (default: build.env)")
    ap.add_argument("--json", help="write every section as JSON here (default <out>/verdict.json)")
    args = ap.parse_args()
    out = Path(args.logroot) / args.stamp
    if not out.is_dir():
        print("no such window output: %s" % out, file=sys.stderr)
        return 2
    tools = args.tools or os.environ.get("W2_TOOLS_OVERRIDE") or read_kv(out / "build.env").get("BUILD_TREE")
    if not tools:
        print("no tools tree (--tools)", file=sys.stderr)
        return 2
    sys.path.insert(0, str(Path(tools) / "tools" / "trace_replay"))
    import compare_actuals as ca
    base_dir = Path(tools) / "docs/Disaggregated/notes/p7/device-window-1/CTS-base"
    report = {"stamp": args.stamp, "out": str(out)}
    ok = True
    g3 = gate3(out, ca)
    if g3:
        print_gate3(g3); print()
        report["gate3"] = g3; ok &= g3["overall"] == "PASS"
    b = bsl(out, ca)
    if b:
        print_bsl(b); print()
        report["bsl"] = b
    c = cts(out, base_dir)
    if c:
        print_cts(c); print()
        report["cts"] = c; ok &= c["overall"] == "PASS"
    print_timing(out)
    Path(args.json or out / "verdict.json").write_text(json.dumps(report, indent=1) + "\n", encoding="utf-8")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
