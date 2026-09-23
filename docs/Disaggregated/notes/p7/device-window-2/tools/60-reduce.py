#!/usr/bin/env python3
"""60-reduce.py <stamp> [--logroot DIR] [--tools TREE] [--json OUT]
   60-reduce.py --check-block RUN_DIR REPORT_JSON

Device window 2 verdicts, read from the window's own output directory (<logroot>/<stamp>).

GATE 3 (CONTRACT-P7 7.2), per case of gate3/cases.txt:
  * ONE SESSION: every gate3/state/<arm>/<case>.done and every archived repeat's boot_id.txt, of
    every arm, carries one boot_id, equal to session/boot-id.txt. A second boot_id, a record that
    carries none, or no session/boot-id.txt makes the whole gate GATE3 INVALID-SESSION, whatever
    the pictures say (the gate is a same-session reading);
  * monolith x1 is the same-session control: it must itself pass, and its role logs must show no
    'Config: IPC' / MOBILEGL_TRANSPORT line (the arm really was the monolith);
  * inproc and spawn: the pair finished (state .done), every expected repeat present and
    error-free, each repeat's arm proof (transport-proof.json passed), the device's own pass
    verdict (ssim >= case threshold), ALL repeats OF THE ARM bit-identical (actual PNG SHA-256;
    within-arm identity is the gate), and |ssim - monolith ssim| <= 0.0005 on every repeat; a
    repeat whose logcat is byte-identical to the previous repeat's is flagged STALE;
  * OpenRA: 1.000000 and mismatch 0 on every repeat of both split arms AND on monolith;
  * cross-arm picture identity (inproc vs spawn vs monolith) is REPORTED per case and a mismatch
    prints a WARN line - information, not a gate (ruling: integrator 2026-09-23);
  * inproc-ra0 (RUN_AHEAD=0): recorded (ssim, sha, run-ahead=0 proof), never gating;
  * a reading that passes but is not the gate's - a denominator other than 36, or a session that
    is not reboot-clean - is GATE3 PASS-SUBSET / PASS-NOT-REBOOT-CLEAN, never PASS.
BSL STATS: per arm, maps-line / VmRSS / VmHWM peaks and the wbuf[] gauges (information).
CTS AFTER (CONTRACT-P7 7.3), per block vs device-window-1/CTS-base/report-<block>.json, over the
  cases the AFTER run was asked to run (so a --limit subset is compared like for like):
  * all five blocks (shader-image ssbo dsa texture packed-pixels) are REQUIRED, each with a
    finished run (cts/runs/<block>/.done), a non-empty cts/report-<block>.json with results, and
    a readable, non-empty caselist (runs/<block>/caselist.txt, the copy 50-cts-after.sh keeps;
    else the file caselist.path names); a missing one makes the block MISSING and CTS INCOMPLETE -
    a block of 0 cases is never a reading. The UBO block (GTF-GL46 uniform_buffer_object) is
    recorded as 'unrun (not in this glcts)': the device glcts carries no GTF module (ID-P7-16);
  * rate = Pass / (Pass + Fail), the contract formula: NotSupported, the warnings and the crashes
    are NOT in the denominator; gate: delta >= -0.5 pp on this rate. Both rates are taken over the
    cases that have a BASE and an AFTER result (a case BASE lacks prints a WARN, stays out of both
    rates and still counts for new crashes). AFTER without a single Pass/Fail where BASE has some
    is FAIL (the rate cannot be formed: every BASE Pass is gone); BASE without one is INCOMPLETE;
  * crashes (Crash/Timeout/InternalError/ResourceError/DeviceHang/Incomplete, plus hung.txt) are
    counted in their own column and are non-Pass; one that BASE does not already have is a NEW
    crash, a hard red of its own (a Fail that became a Crash is a new crash);
  * a block whose cases are not the tools tree's full p7-<block>-gl46.txt (a --limit run) makes an
    all-PASS CTS reading 'PASS-SUBSET', never PASS;
  * information only, never gating: the pre-ruling rate Pass / (results - NotSupported) of BASE
    and AFTER and its delta, and a WARN when the NotSupported count moved.
Exit status: 0 only when GATE 3 is PASS (36/36, reboot-clean, one session) and CTS is PASS (five
full blocks); 1 otherwise (a section that did not run, a PASS-SUBSET / PASS-NOT-REBOOT-CLEAN
reading, INCOMPLETE / INVALID-SESSION, or FAIL); 2 on bad input.
--check-block RUN_DIR REPORT_JSON: the completeness test 50-cts-after.sh applies before it writes
a block's .done (the same code the verdict uses): exit 0 when the report has a result for every
non-skipped case of the run's caselist and unrun.txt is empty, 3 when not (the reason is printed).
"""
import argparse
import hashlib
import json
import os
import re
import sys
from collections import Counter
from pathlib import Path

TOL = 0.0005
GATE3_DENOMINATOR = 36
CTS_TOL_PP = 0.5
HARD = ("Crash", "Timeout", "InternalError", "ResourceError", "DeviceHang", "Incomplete")
SPLIT_ARMS = ("inproc", "spawn")
OPENRA = "OpenRA"
CTS_BLOCKS = ("shader-image", "ssbo", "dsa", "texture", "packed-pixels")
UBO_BLOCK = {"block": "ubo", "caselist": "tools/cts/caselists/p7-ubo-gl46.txt",
             "cases": "GTF-GL46.gtf31.GL3Tests.uniform_buffer_object*", "verdict": "UNRUN",
             "reasons": ["unrun (not in this glcts): the device glcts has no GTF module (ID-P7-16);"
                         " recorded, not gated"]}
REPEAT_DIR = re.compile(r"^repeat-(\d+)$")
KV = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)=(\S*)")
# ConfigLoader.cpp: "Config: MOBILEGL_TRANSPORT=inproc|spawn - ..." (and its refusals) and
# "Config: IPC ring=..." are printed only when a transport resolves; the monolith prints neither.
NOT_MONOLITH = re.compile(r"Config: IPC|MOBILEGL_TRANSPORT=")
RULING = "ruling: integrator 2026-09-23"


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


def read_record(path):
    """A one-line 'k=v k=v ...' record (gate3 .done, boot_id.txt) -> dict, or None if absent."""
    try:
        return dict(KV.findall(path.read_text(encoding="utf-8", errors="replace")))
    except OSError:
        return None


def first_line(path):
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return ""
    return lines[0].strip() if lines else ""


# ------------------------------------------------------------------------------------------------
# gate 3
# ------------------------------------------------------------------------------------------------
def gate3_session(out, g):
    """One boot session for every record of every arm (ruling c)."""
    want = first_line(out / "session" / "boot-id.txt")
    problems, seen, unstamped = [], {}, []

    def note(value, where):
        if value:
            seen.setdefault(value, []).append(where)
        else:
            unstamped.append(where)

    state = g / "state"
    for arm_dir in sorted(p for p in state.iterdir() if p.is_dir()) if state.is_dir() else []:
        for done in sorted(arm_dir.glob("*.done")):
            rec = read_record(done) or {}
            where = "state/%s/%s" % (arm_dir.name, done.name)
            note(rec.get("boot_id"), where)
            if "boot_id_before" in rec:
                note(rec["boot_id_before"], where + " (boot_id_before)")
    archive = g / "archive"
    for arm_dir in sorted(p for p in archive.iterdir() if p.is_dir()) if archive.is_dir() else []:
        for case_dir in sorted(p for p in arm_dir.iterdir() if p.is_dir()):
            for rep in sorted(p for p in case_dir.iterdir() if p.is_dir() and REPEAT_DIR.match(p.name)):
                where = "archive/%s/%s/%s/boot_id.txt" % (arm_dir.name, case_dir.name, rep.name)
                rec = read_record(rep / "boot_id.txt") or {}
                note(rec.get("boot_id"), where)
                if "boot_id_before" in rec:
                    note(rec["boot_id_before"], where + " (boot_id_before)")
    if not want:
        problems.append("no session/boot-id.txt: 21-preflight.sh records the session's boot_id there")
    if unstamped:
        problems.append("%d record(s) carry no boot_id (pre-ruling 30-gate3.sh, or a repeat the driver did "
                        "not stamp), e.g. %s" % (len(unstamped), unstamped[0]))
    for boot in sorted(b for b in seen if b != want):
        problems.append("boot_id %s != session %s in %d record(s), e.g. %s"
                        % (boot, want or "?", len(seen[boot]), seen[boot][0]))
    return {"session_boot_id": want, "boot_ids": sorted(seen), "records": sum(len(v) for v in seen.values()),
            "unstamped": len(unstamped), "problems": problems, "valid": not problems,
            "reboot_clean": first_line(out / "session" / "reboot-clean.txt")}


def monolith_identity(row):
    """None when the repeat's role logs show a monolith, else what says otherwise (ruling f)."""
    d = Path(row["dir"])
    if not (d / "mobilegl.log").is_file():
        return "no mobilegl.log archived: the arm's identity is unproven"
    for name in ("mobilegl.log", "mobilegl.client.log", "mobilegl.server.log"):
        p = d / name
        if not p.is_file():
            continue
        for line in p.read_text(encoding="utf-8", errors="replace").splitlines():
            if NOT_MONOLITH.search(line):
                return "%s: %s" % (name, line.strip()[:140])
    return None


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
    session = gate3_session(out, g)
    identity = {"monolith_repeats": 0, "monolith_proven": 0, "split_repeats": 0, "split_proofs_passed": 0}

    def rows(arm, case):
        s = summaries.get(arm)
        if not s:
            return []
        return [r for r in s["rows"] if r["arm"] == case + "-DirectVulkan"]

    def finished(arm, case):
        return (g / "state" / arm / (case + ".done")).is_file()

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
        mono_ssim = mono_sha = None
        if "monolith" in summaries and not finished("monolith", case):
            incomplete.append("monolith: no state .done (pair unfinished; 30-gate3.sh retries it)")
        if len(mono) != 1 or mono[0].get("error"):
            incomplete.append("monolith: %s" % (mono[0].get("error") if mono else "not run"))
        else:
            m = mono[0]
            mono_ssim, mono_sha = m.get("ssim_vs_golden"), m.get("actual_sha256")
            not_mono = monolith_identity(m)
            identity["monolith_repeats"] += 1
            identity["monolith_proven"] += not_mono is None
            entry["arms"]["monolith"] = {"ssim": mono_ssim, "sha": mono_sha, "identity": not_mono or "monolith",
                                         "mismatch": m.get("mismatch_pixels_vs_golden"), "passed": m.get("passed")}
            if not_mono:
                reasons.append("monolith arm is NOT proven monolith (%s)" % not_mono)
            if m.get("passed") is not True:
                reasons.append("monolith control did not pass (ssim %s)" % mono_ssim)
            if case == OPENRA and not (m.get("mismatch_pixels_vs_golden") == 0 and (mono_ssim or 0) >= 0.9999995):
                reasons.append("OpenRA monolith not 1.000000/0 (%s/%s)" % (mono_ssim, m.get("mismatch_pixels_vs_golden")))
        for arm in SPLIT_ARMS:
            rs = rows(arm, case)
            a = {"repeats": len(rs), "ssim": [r.get("ssim_vs_golden") for r in rs],
                 "sha": sorted({r.get("actual_sha256") for r in rs if r.get("actual_sha256")}), "proofs_passed": 0}
            entry["arms"][arm] = a
            if arm not in summaries:
                incomplete.append(arm + ": not run")
                continue
            if not finished(arm, case):
                incomplete.append("%s: no state .done (pair unfinished; 30-gate3.sh retries it)" % arm)
            if len(rs) != repeat:
                incomplete.append("%s: %d/%d repeats" % (arm, len(rs), repeat))
            for r in rs:
                if r.get("error"):
                    incomplete.append("%s rep%d: %s" % (arm, r["repeat"], r["error"]))
                    continue
                if r.get("passed") is not True:
                    reasons.append("%s rep%d below threshold (ssim %s)" % (arm, r["repeat"], r.get("ssim_vs_golden")))
                p = proof(r)
                identity["split_repeats"] += 1
                if not p or p.get("passed") is not True:
                    reasons.append("%s rep%d arm proof (transport-proof.json) not passed" % (arm, r["repeat"]))
                else:
                    a["proofs_passed"] += 1
                    identity["split_proofs_passed"] += 1
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
        # Cross-arm identity: reported, never gating (ruling d).
        ip = set(entry["arms"].get("inproc", {}).get("sha", []))
        sp = set(entry["arms"].get("spawn", {}).get("sha", []))
        entry["inproc_eq_spawn"] = bool(ip) and ip == sp
        entry["split_eq_monolith"] = bool(ip) and ip == sp == {mono_sha}
        entry["cross_arm"] = {"monolith": mono_sha, "inproc": sorted(ip), "spawn": sorted(sp),
                              "identical": (entry["split_eq_monolith"] if (ip and sp and mono_sha) else None)}
        entry["verdict"] = "INCOMPLETE" if incomplete else ("FAIL" if reasons else "PASS")
        entry["reasons"] = incomplete + reasons
        results.append(entry)
    verdicts = [e["verdict"] for e in results]
    # A reading that is not the gate's own (CONTRACT-P7 7.2: the 36-case denominator, one
    # reboot-clean session) may pass, but never as PASS: exit 0 is reserved for the gate verdict.
    not_a_gate = []
    if len(cases) != GATE3_DENOMINATOR:
        not_a_gate.append("denominator is %d, not the gate's %d: a subset run is NOT a gate verdict"
                          % (len(cases), GATE3_DENOMINATOR))
    if not session["reboot_clean"].startswith("reboot-clean:"):
        not_a_gate.append("session not reboot-clean: NOT a gate verdict")
    if not session["valid"]:
        overall = "INVALID-SESSION"
    elif verdicts and all(v == "PASS" for v in verdicts):
        overall = "PASS"
        if len(cases) != GATE3_DENOMINATOR:
            overall = "PASS-SUBSET"
        elif not_a_gate:
            overall = "PASS-NOT-REBOOT-CLEAN"
    elif "FAIL" in verdicts:
        overall = "FAIL"
    else:
        overall = "INCOMPLETE"
    return {"cases": results, "excluded": excluded, "repeat": repeat, "denominator": len(cases),
            "overall": overall, "not_a_gate": not_a_gate, "arms_present": arms_present,
            "session": session, "identity": identity}


def print_gate3(g3):
    print("== GATE 3 (DirectVulkan x pbuffer; split repeat=%d; tol |ssim-monolith| <= %.4f) ==" % (g3["repeat"], TOL))
    s = g3["session"]
    print("-- session: boot_id %s; %d record(s) stamped; %s" % (s["session_boot_id"] or "?", s["records"],
                                                               s["reboot_clean"] or "no session/reboot-clean.txt"))
    for problem in s["problems"]:
        print("   ! INVALID-SESSION: " + problem)
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
    i = g3["identity"]
    print("-- arm identity: monolith %d/%d repeat(s) with no Config: IPC / MOBILEGL_TRANSPORT line; "
          "inproc+spawn transport-proof passed %d/%d" % (i["monolith_proven"], i["monolith_repeats"],
                                                        i["split_proofs_passed"], i["split_repeats"]))
    judged = [e for e in g3["cases"] if e["cross_arm"]["identical"] is not None]
    print("-- cross-arm picture identity (reported, NOT a gate; %s): %d/%d case(s) inproc==spawn==monolith" % (
        RULING, sum(e["cross_arm"]["identical"] for e in judged), len(judged)))
    for e in judged:
        if not e["cross_arm"]["identical"]:
            x = e["cross_arm"]
            print("WARN cross-arm %s: monolith %s | inproc %s | spawn %s" % (
                e["case"], (x["monolith"] or "-")[:12], ",".join(v[:12] for v in x["inproc"]) or "-",
                ",".join(v[:12] for v in x["spawn"]) or "-"))
    print("-- excluded (red on monolith, CONTRACT-P7 7.1): " + (", ".join(g3["excluded"]) or "none"))
    n = len(g3["cases"])
    npass = sum(e["verdict"] == "PASS" for e in g3["cases"])
    print("GATE3 %s: %d/%d cases PASS%s" % (g3["overall"], npass, n, "".join("  (%s)" % x for x in g3["not_a_gate"])))


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
    """CONTRACT-P7 7.3: rate = Pass / (Pass + Fail). NotSupported, the warnings (Compatibility /
    Quality) and the crashes stay out of the denominator; crashes get their own count. The
    pre-ruling Pass / (results - NotSupported) is kept as `results_rate`, information only."""
    c = Counter(statuses)
    p, f, ns = c["Pass"], c["Fail"], c["NotSupported"]
    x = sum(c[s] for s in HARD)
    n = len(statuses)
    return {"pass": p, "fail": f, "ns": ns, "crash": x, "warn": n - p - f - ns - x, "results": n,
            "rate": (p / (p + f)) if (p + f) else None,
            "results_rate": (p / (n - ns)) if (n - ns) else None}


def load_results(path):
    """-> (results dict, None) or (None, why the report does not count)."""
    if not path.is_file():
        return None, "no %s" % path.name
    if path.stat().st_size == 0:
        return None, "%s is empty" % path.name
    try:
        results = json.loads(path.read_text(encoding="utf-8"))["results"]
    except (OSError, ValueError, KeyError, TypeError) as error:
        return None, "%s unreadable (%s)" % (path.name, error)
    if not isinstance(results, dict) or not results:
        return None, "%s has no results" % path.name
    return results, None


def caselist_of(run):
    """-> (cases, source, None) for the cases a block's AFTER run was asked to run, or (None,
    source, why) when that cannot be read. 50-cts-after.sh keeps a copy (runs/<b>/caselist.txt)
    beside the path it ran (caselist.path), so the window's output reduces on its own after the
    build tree that path names is gone; a named list that is gone or empty is NOT replaced by
    the report's own cases (that would make a block of 0 - or of only what ran - look complete)."""
    copy = run / "caselist.txt"
    if copy.is_file():
        src = copy
    elif (run / "caselist.path").is_file():
        src = Path((run / "caselist.path").read_text(encoding="utf-8", errors="replace").strip() or "(empty caselist.path)")
    else:
        return None, None, ("no runs/%s/caselist.txt or caselist.path: which cases the run was asked "
                            "to run is unknown" % run.name)
    if not src.is_file():
        return None, src, "the caselist the run used is gone (%s) and runs/%s/caselist.txt holds no copy" % (src, run.name)
    cases = read_list(src)
    if not cases:
        return None, src, "the caselist the run used (%s) holds no case" % src
    return cases, src, None


def block_scope(run, after):
    """The one completeness test of a finished block (the verdict below, and 50-cts-after.sh through
    --check-block before it writes .done). -> (universe, source, missing, incomplete): universe =
    the caselist minus run_cts's skipped.txt; `missing` says why there is no reading at all,
    `incomplete` why the reading lacks cases."""
    cases, src, why = caselist_of(run)
    if why:
        return [], src, [why], []
    skipped = set(read_list(run / "skipped.txt"))
    universe = [x for x in cases if x not in skipped]
    if not universe:
        return [], src, ["every case of %s is skipped: a block of 0 cases is no reading" % src], []
    unrun = read_list(run / "unrun.txt")
    without = [x for x in universe if x not in after]
    incomplete = []
    if unrun or without:
        incomplete.append("%d case(s) without an AFTER result (unrun.txt %d)" % (len(without), len(unrun)))
    return universe, src, [], incomplete


def cts_block(c, b, base_dir, canon_dir):
    run = c / "runs" / b
    if not c.is_dir():
        return {"block": b, "verdict": "MISSING", "reasons": ["no cts/ directory: 50-cts-after.sh did not run"]}
    if not (run / ".done").is_file():
        return {"block": b, "verdict": "MISSING", "reasons": ["block not finished (no runs/%s/.done)" % b]}
    after, why = load_results(c / ("report-%s.json" % b))
    if why:
        return {"block": b, "verdict": "MISSING", "reasons": [why]}
    universe, src, missing, incomplete = block_scope(run, after)
    if missing:
        return {"block": b, "verdict": "MISSING", "reasons": missing}
    base, why = load_results(base_dir / ("report-%s.json" % b))
    if why:
        return {"block": b, "verdict": "NO-BASE", "reasons": ["$BASE: " + why + " in " + str(base_dir)]}
    unrun = read_list(run / "unrun.txt")
    hung = read_list(run / "hung.txt")
    # Like for like: both rates over the cases that have a BASE and an AFTER result.
    both = [x for x in universe if x in after and x in base]
    ra, rb = rate([after[x] for x in both]), rate([base[x] for x in both])
    delta = None if ra["rate"] is None or rb["rate"] is None else (ra["rate"] - rb["rate"]) * 100.0
    delta_results = (None if ra["results_rate"] is None or rb["results_rate"] is None
                     else (ra["results_rate"] - rb["results_rate"]) * 100.0)
    new_crash = sorted(x for x in universe if after.get(x) in HARD and base.get(x) not in HARD)
    new_crash += sorted(x for x in hung if x not in new_crash)
    regress = sorted(x for x in universe if base.get(x) == "Pass" and after.get(x) not in (None, "Pass"))
    fixed = sorted(x for x in universe if base.get(x) not in (None, "Pass") and after.get(x) == "Pass")
    missing_base = sorted(x for x in universe if x not in base)
    fail, notes, warnings = [], [], []
    if new_crash:
        fail.append("%d NEW crash(es)" % len(new_crash))
    if rb["rate"] is None:
        incomplete.append("$BASE has no Pass/Fail over this block's %d case(s): no contract rate to compare against"
                          % len(both))
    elif ra["rate"] is None:
        fail.append("AFTER has no Pass/Fail result (NS %d, W %d, X %d of %d): the contract rate cannot be formed "
                    "where $BASE's is %.3f%% - every $BASE Pass is lost" % (
                        ra["ns"], ra["warn"], ra["crash"], ra["results"], 100 * rb["rate"]))
    if delta is not None and delta < -CTS_TOL_PP:
        fail.append("delta %.3f pp < -%.1f pp (Pass/(Pass+Fail))" % (delta, CTS_TOL_PP))
    if delta is not None and delta > CTS_TOL_PP:
        notes.append("note: AFTER is %.3f pp ABOVE BASE (not a regression; review the fixed list)" % delta)
    if missing_base:
        warnings.append("%d case(s) have no $BASE result (e.g. %s): left out of both rates; a crash among them "
                        "still counts as NEW" % (len(missing_base), missing_base[0]))
    if ra["ns"] != rb["ns"]:
        warnings.append("NotSupported $BASE %d -> AFTER %d (not in the contract rate; a Pass that became NS is in "
                        "the Pass->not list)" % (rb["ns"], ra["ns"]))
    canonical = canon_dir / ("p7-%s-gl46.txt" % b)
    subset = set(read_list(src)) != set(read_list(canonical))
    verdict = "FAIL" if fail else ("INCOMPLETE" if incomplete else "PASS")
    return {"block": b, "cases": len(universe), "compared": len(both), "caselist": str(src),
            "after": ra, "base": rb, "delta_pp": delta, "delta_pp_results_info": delta_results,
            "new_crash": new_crash, "regressed": regress, "fixed": fixed, "missing_in_base": missing_base,
            "unrun": len(unrun), "hung": hung, "verdict": verdict, "reasons": incomplete + fail + notes,
            "warnings": warnings, "subset": subset}


def cts(out, base_dir, canon_dir):
    c = out / "cts"
    blocks = [cts_block(c, b, base_dir, canon_dir) for b in CTS_BLOCKS]
    extra = sorted(p.stem[len("report-"):] for p in c.glob("report-*.json")
                   if p.stem[len("report-"):] not in CTS_BLOCKS) if c.is_dir() else []
    verdicts = [b["verdict"] for b in blocks]
    if "FAIL" in verdicts:
        overall = "FAIL"
    elif all(v == "PASS" for v in verdicts):
        # Passing on a --limit subset (or on a list that is not the tools tree's) is not gate 5.
        overall = "PASS-SUBSET" if any(b.get("subset") for b in blocks) else "PASS"
    else:
        overall = "INCOMPLETE"
    return {"blocks": blocks, "overall": overall, "ubo": dict(UBO_BLOCK), "ignored_reports": extra,
            "incomplete_blocks": [b["block"] for b in blocks if b["verdict"] not in ("PASS", "FAIL")]}


def print_cts(c):
    print("== CTS AFTER (inproc x DirectVulkan) vs BASE (monolith x DirectVulkan, p7w1) ==")
    print("gate (CONTRACT-P7 7.3): rate = Pass/(Pass+Fail) [NS, warnings W and crashes X are not in the denominator];")
    print("  per block delta >= -%.1f pp and 0 NEW crash; all five blocks required. 'info' = the pre-ruling" % CTS_TOL_PP)
    print("  Pass/(results-NS) delta (its rates on each block's 'info' line), for comparison only, never gating.")
    print("%-14s %6s  %-20s %8s  %-20s %8s  %9s  %9s  %5s  %4s  %5s  %s" % (
        "block", "cases", "BASE P/F/NS/W/X", "rate", "AFTER P/F/NS/W/X", "rate", "delta pp", "(info)",
        "crash", "regr", "fixed", "verdict"))

    def counts(r):
        return "%d/%d/%d/%d/%d" % (r["pass"], r["fail"], r["ns"], r["warn"], r["crash"])

    def pct(v):
        return "%.3f%%" % (100 * v) if v is not None else "n/a"

    def pp(v):
        return "%+.3f" % v if v is not None else "n/a"
    for b in c["blocks"]:
        if "after" not in b:
            print("%-14s  %s: %s" % (b["block"], b["verdict"], "; ".join(b["reasons"])))
            continue
        print("%-14s %6d  %-20s %8s  %-20s %8s  %9s  %9s  %5d  %4d  %5d  %s%s" % (
            b["block"], b["cases"], counts(b["base"]), pct(b["base"]["rate"]), counts(b["after"]),
            pct(b["after"]["rate"]), pp(b["delta_pp"]), "(%s)" % pp(b["delta_pp_results_info"]),
            len(b["new_crash"]), len(b["regressed"]), len(b["fixed"]), b["verdict"],
            " (SUBSET)" if b["subset"] else ""))
        print("    info  pre-ruling Pass/(results-NS): BASE %s  AFTER %s  delta %s pp (not gating)" % (
            pct(b["base"]["results_rate"]), pct(b["after"]["results_rate"]), pp(b["delta_pp_results_info"])))
        for reason in b["reasons"]:
            print("    ! " + reason)
        for warning in b["warnings"]:
            print("WARN %s: %s" % (b["block"], warning))
        for x in b["new_crash"]:
            print("    NEW CRASH  " + x)
        for x in b["regressed"][:40]:
            print("    Pass->not  " + x)
        if len(b["regressed"]) > 40:
            print("    ... %d more regressions in the JSON" % (len(b["regressed"]) - 40))
        for x in b["fixed"][:20]:
            print("    not->Pass  " + x)
    u = c["ubo"]
    print("%-14s  %s  (%s)" % (u["block"], u["reasons"][0], u["cases"]))
    if c["ignored_reports"]:
        print("    note: report(s) outside the five gate blocks ignored: " + ", ".join(c["ignored_reports"]))
    tail = ""
    if c["incomplete_blocks"]:
        tail += "  (INCOMPLETE: %s)" % ", ".join("%s %s" % (b["block"], b["verdict"]) for b in c["blocks"]
                                                  if b["block"] in c["incomplete_blocks"])
    if any(b.get("subset") for b in c["blocks"]):
        tail += "  (SUBSET run: %s not the tools tree's full caselist; not a gate verdict)" % ", ".join(
            b["block"] for b in c["blocks"] if b.get("subset"))
    print("CTS %s%s" % (c["overall"], tail))


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


def check_block(run, report):
    """--check-block: 0 when the block is a complete AFTER reading, 3 when not (50-cts-after.sh)."""
    after, why = load_results(report)
    if why:
        print("INCOMPLETE: " + why)
        return 3
    universe, src, missing, incomplete = block_scope(run, after)
    if missing or incomplete:
        print("INCOMPLETE: " + "; ".join(missing + incomplete))
        return 3
    print("complete: %d/%d case(s) of %s have a result" % (len(universe), len(universe), src))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("stamp", nargs="?")
    ap.add_argument("--logroot", default=os.environ.get("W2_LOGROOT", str(Path.home() / "w7/logs/p7w7")))
    ap.add_argument("--tools", help="MobileGL tree holding compare_actuals.py and CTS-base (default: build.env)")
    ap.add_argument("--json", help="write every section as JSON here (default <out>/verdict.json)")
    ap.add_argument("--check-block", nargs=2, metavar=("RUN_DIR", "REPORT_JSON"),
                    help="only test one block's completeness (50-cts-after.sh, before it writes .done)")
    args = ap.parse_args()
    if args.check_block:
        return check_block(Path(args.check_block[0]), Path(args.check_block[1]))
    if not args.stamp:
        ap.print_usage(sys.stderr)
        return 2
    out = Path(args.logroot) / args.stamp
    if not out.is_dir():
        print("no such window output: %s" % out, file=sys.stderr)
        return 2
    tools = args.tools or os.environ.get("W2_TOOLS_OVERRIDE") or read_kv(out / "build.env").get("BUILD_TREE")
    if not tools:
        print("no tools tree (--tools)", file=sys.stderr)
        return 2
    sys.path.insert(0, str(Path(tools) / "tools" / "trace_replay"))
    sys.dont_write_bytecode = True  # the tools tree is only read (it may be the read-only integration tree)
    import compare_actuals as ca
    base_dir = Path(tools) / "docs/Disaggregated/notes/p7/device-window-1/CTS-base"
    report = {"stamp": args.stamp, "out": str(out)}
    g3 = gate3(out, ca)
    if g3:
        print_gate3(g3)
    else:
        g3 = {"overall": "NOT-RUN"}
        print("== GATE 3 ==\nGATE3 NOT-RUN (no gate3/cases.txt)")
    print()
    report["gate3"] = g3
    b = bsl(out, ca)
    if b:
        print_bsl(b); print()
        report["bsl"] = b
    c = cts(out, base_dir, Path(tools) / "tools/cts/caselists")
    print_cts(c); print()
    report["cts"] = c
    print_timing(out)
    ok = g3["overall"] == "PASS" and c["overall"] == "PASS"
    report["window"] = {"gate3": g3["overall"], "cts": c["overall"], "exit": 0 if ok else 1}
    print("\nWINDOW2 GATE3 %s | CTS %s -> exit %d" % (g3["overall"], c["overall"], 0 if ok else 1))
    Path(args.json or out / "verdict.json").write_text(json.dumps(report, indent=1) + "\n", encoding="utf-8")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
