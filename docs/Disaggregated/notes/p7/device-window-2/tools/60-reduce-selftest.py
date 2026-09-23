#!/usr/bin/env python3
"""60-reduce-selftest.py [--tools TREE] [--dry-run-dir DIR] [--keep]

Self-test of 60-reduce.py: builds canned window output directories in a temp dir and runs the
reducer on each exactly as window.sh does (a subprocess, <stamp> --logroot --tools), then checks
the printed verdict, verdict.json and the exit status. The scenarios:

  control         gate 3 (2 cases x monolith/inproc x3/spawn x3/ra0, one boot_id) and all five CTS
                  blocks equal to $BASE                           -> GATE3 PASS, CTS PASS, exit 0
  missing-block   ssbo never finished, dsa's report is empty, texture's has no results; and a
                  window with no cts/ at all                      -> CTS INCOMPLETE, exit != 0
  boot-id-change  a spawn pair's .done / one repeat's boot_id.txt names another boot; a .done
                  in the pre-ruling format (no boot_id); no session/boot-id.txt
                                                                  -> GATE3 INVALID-SESSION
  monolith-arm    the monolith repeat's log carries Config: IPC / has no log; a split repeat's
                  transport-proof did not pass                    -> that case FAIL
  cross-arm       spawn's pictures differ from inproc's and monolith's, each arm self-identical
                                                                  -> WARN line, GATE3 still PASS
                  one inproc repeat differs from its siblings     -> FAIL (within-arm identity)
  unfinished      a pair without .done; a repeat without an actual PNG -> GATE3 INCOMPLETE
  dry-run         the pre-14e1c8b9 CTS dry run rebuilt from $BASE + its recorded differences
                  (60-reduce-selftest-pre14e1c8b9.json): contract rate Pass/(Pass+Fail) gives
                  dsa -0.272 pp (the pre-ruling Pass/(results-NS) -0.539 pp is printed as info
                  only), shader-image +4.284, texture +0.960, ssbo/packed-pixels 0; dsa and
                  shader-image FAIL on one NEW crash each, not on the rate -> CTS FAIL
When --dry-run-dir (default ~/w7/logs/devprep/w2/pre-14e1c8b9) exists, the reducer also runs on
that real output (verdict JSON to the temp dir) and must give the same CTS numbers.
TREE (compare_actuals.py + device-window-1/CTS-base + tools/cts/caselists) defaults to the
MobileGL tree this file sits in, else $W2_TOOLS_OVERRIDE, else $W2_PIPE / ~/w7/pipe (only read).
Exit 0 when every check holds.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from collections import Counter
from pathlib import Path

HERE = Path(__file__).resolve().parent
REDUCER = HERE / "60-reduce.py"
FIXTURE = HERE / "60-reduce-selftest-pre14e1c8b9.json"
CTS_BASE_REL = "docs/Disaggregated/notes/p7/device-window-1/CTS-base"
BLOCKS = ("shader-image", "ssbo", "dsa", "texture", "packed-pixels")
BOOT = "11111111-2222-3333-4444-555555555555"
OTHER_BOOT = "99999999-8888-7777-6666-555555555555"
CASES = ("caseA-in-world", "caseB-main-menu")
REPEAT = 3
ARMS = (("monolith", 1), ("inproc", REPEAT), ("spawn", REPEAT), ("inproc-ra0", 1))
DSA_STORAGE = "KHR-GL46.direct_state_access.renderbuffers_storage"
SI_INCOMPLETE = "KHR-GL46.shader_image_load_store.incomplete_textures"
IPC = ("Config: IPC ring=8MiB stage=32MiB wire-deferred=64MiB spin=50us persistent-block=64KiB adopt-tier=2 "
       "verb-barrier=1 run-ahead=%d present-credit=1 strict=1 audit=0 role-split-state=1 affinity='auto'")
TAG = "[12:00:00] [Android MobileGLTraceRe/INFO]: "
LOGS = {
    "monolith": TAG + "Config: Active backend type set to DirectVulkan\n" + TAG + "Config loaded\n",
    "inproc": TAG + "Config: Active backend type set to DirectVulkan\n" + TAG + "Config: MOBILEGL_TRANSPORT=inproc"
              " - the MGPipe record stream crosses a real ring to an apply thread\n" + TAG + IPC % 1 + "\n",
    "spawn": TAG + "Config: Active backend type set to DirectVulkan\n" + TAG + "Config: MOBILEGL_TRANSPORT=spawn"
             " - the MGPipe record stream crosses a real process boundary\n" + TAG + IPC % 1 + "\n",
    "inproc-ra0": TAG + "Config: MOBILEGL_TRANSPORT=inproc - the MGPipe record stream crosses a real ring to an"
                  " apply thread\n" + TAG + IPC % 0 + "\n",
}


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    if isinstance(data, bytes):
        path.write_bytes(data)
    else:
        path.write_text(data, encoding="utf-8")


def read_list(path):
    if not path.is_file():
        return []
    return [l.strip() for l in path.read_text(encoding="utf-8").splitlines() if l.strip() and not l.startswith("#")]


def find_tree(arg):
    # The repo copy finds its own tree; the working copy (~/w7/notes/p7/window2) falls back to the
    # tools override, then the integration tree lib.sh names (read only).
    candidates = [arg] + [str(p) for p in HERE.parents] + [
        os.environ.get("W2_TOOLS_OVERRIDE"), os.environ.get("W2_PIPE"), str(Path.home() / "w7/pipe")]
    for c in candidates:
        if c and (Path(c) / "tools/trace_replay/compare_actuals.py").is_file() and (Path(c) / CTS_BASE_REL).is_dir():
            return Path(c)
    return None


# ------------------------------------------------------------------------------------------------
# canned window directories
# ------------------------------------------------------------------------------------------------
class Canned:
    def __init__(self, root, tree, ca):
        self.root, self.tree, self.ca = root, tree, ca

    def png(self, value):
        return self.ca._png_bytes(8, 8, self.ca._solid(8, 8, value))

    def gate3(self, out, picture=None, boot=BOOT):
        picture = picture or {}
        g = out / "gate3"
        write(g / "cases.txt", "\n".join(CASES) + "\n")
        write(g / "excluded.txt", "x-red-on-monolith-case\n")
        write(g / "arms.txt", "arms=monolith inproc spawn inproc-ra0\nrepeat=%d\n" % REPEAT)
        write(out / "session/boot-id.txt", boot + "\n")
        write(out / "session/reboot-clean.txt", "reboot-clean: 00000000-0000-0000-0000-000000000000 -> %s\n" % boot)
        for arm, n in ARMS:
            for case in CASES:
                name = case + "-DirectVulkan"
                for i in range(1, n + 1):
                    rep = g / "archive" / arm / name / ("repeat-%02d" % i)
                    write(rep / "result.json", json.dumps({
                        "passed": True, "statusCode": 0, "backend": "DirectVulkan", "ssim": 0.998,
                        "ssimThreshold": 0.99, "mismatchPixels": 17, "matchedGoldenPath": "/in/golden.png",
                        "cropX": 0, "cropY": 0, "cropWidth": 0, "cropHeight": 0}))
                    write(rep / (name + "-actual.png"), self.png(picture.get(arm, 128)))
                    write(rep / "logcat.txt", "canned logcat %s %s repeat %d\n" % (arm, case, i))
                    write(rep / "mobilegl.log", LOGS[arm])
                    if arm in ("inproc", "spawn"):
                        write(rep / "transport-proof.json", json.dumps({"required_transport": arm, "passed": True}))
                    write(rep / "boot_id.txt", "boot_id=%s boot_id_before=%s stamped=2026-09-23T12:00:00-04:00\n" % (boot, boot))
                write(g / "state" / arm / (case + ".done"),
                      "rc=0 seconds=7 attempts=1 passed_lines=%d boot_id=%s boot_id_before=%s "
                      "finished=2026-09-23T12:00:00-04:00\n" % (n, boot, boot))
        return g

    def cts(self, out, differences=None, drop=(), empty=(), no_results=()):
        differences = differences or {}
        base_dir = self.tree / CTS_BASE_REL
        skip = set(read_list(self.tree / "tools/cts/caselists/p7-skip.txt"))
        c = out / "cts"
        c.mkdir(parents=True, exist_ok=True)
        rebuilt = {}
        for b in BLOCKS:
            if b in drop:
                continue
            caselist = self.tree / "tools/cts/caselists" / ("p7-%s-gl46.txt" % b)
            run = c / "runs" / b
            write(run / "caselist.path", str(caselist) + "\n")
            skipped = [x for x in read_list(caselist) if x in skip]
            if skipped:
                write(run / "skipped.txt", "\n".join(skipped) + "\n")
            write(run / "unrun.txt", "")
            write(run / "hung.txt", "")
            results = dict(json.loads((base_dir / ("report-%s.json" % b)).read_text(encoding="utf-8"))["results"])
            results.update(differences.get(b, {}))
            rebuilt[b] = results
            write(run / "crashed.txt", "".join(k + "\n" for k, v in sorted(results.items()) if v == "Crash"))
            write(run / ".done", "rc=0 seconds=1 pin_after=PINNED finished=2026-09-23T12:00:00-04:00\n")
            report = c / ("report-%s.json" % b)
            if b in empty:
                write(report, "")
            elif b in no_results:
                write(report, json.dumps({"label": "DirectVulkan-inproc-" + b, "results": {}}))
            else:
                write(report, json.dumps({"label": "DirectVulkan-inproc-" + b, "total": len(results),
                                          "counts": dict(Counter(results.values())), "results": results}, indent=1))
        return rebuilt

    def window(self, stamp, gate3=True, cts=True, **cts_args):
        out = self.root / stamp
        out.mkdir(parents=True)
        if gate3:
            self.gate3(out)
        else:
            write(out / "session/boot-id.txt", BOOT + "\n")
        if cts:
            self.cts(out, **cts_args)
        return out


def run_reducer(logroot, stamp, tree, json_out=None):
    cmd = [sys.executable, str(REDUCER), stamp, "--logroot", str(logroot), "--tools", str(tree)]
    if json_out:
        cmd += ["--json", str(json_out)]
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
    vpath = Path(json_out) if json_out else Path(logroot) / stamp / "verdict.json"
    verdict = json.loads(vpath.read_text(encoding="utf-8")) if vpath.is_file() else {}
    return p.returncode, p.stdout, verdict


class Checks:
    def __init__(self):
        self.total, self.failed = 0, []

    def expect(self, name, cond, detail=""):
        self.total += 1
        print("  %s %s%s" % ("ok  " if cond else "FAIL", name, "" if cond else "   <-- " + str(detail)[:300]))
        if not cond:
            self.failed.append(name)


def block(verdict, name):
    for b in verdict.get("cts", {}).get("blocks", []):
        if b["block"] == name:
            return b
    return {}


def case(verdict, name):
    for e in verdict.get("gate3", {}).get("cases", []):
        if e["case"] == name:
            return e
    return {}


def r3(v):
    return None if v is None else round(v, 3)


# ------------------------------------------------------------------------------------------------
# scenarios
# ------------------------------------------------------------------------------------------------
def scenario_control(k, cn, tree):
    print("[control] complete clean window")
    cn.window("control")
    rc, text, v = run_reducer(cn.root, "control", tree)
    k.expect("exit 0", rc == 0, "rc=%d\n%s" % (rc, text))
    k.expect("GATE3 PASS", v.get("gate3", {}).get("overall") == "PASS", text)
    k.expect("CTS PASS, all five blocks", v.get("cts", {}).get("overall") == "PASS"
             and [b["block"] for b in v["cts"]["blocks"]] == list(BLOCKS), text)
    k.expect("every block delta 0.000 pp", all(r3(block(v, b).get("delta_pp")) == 0.0 for b in BLOCKS))
    k.expect("UBO recorded 'unrun (not in this glcts)'", v.get("cts", {}).get("ubo", {}).get("verdict") == "UNRUN"
             and "unrun (not in this glcts)" in text)
    s = v.get("gate3", {}).get("session", {})
    records = 2 * len(CASES) * (len(ARMS) + sum(n for _, n in ARMS))  # (.done + repeats) x (boot_id, before)
    k.expect("session valid: one boot_id over %d records" % records,
             s.get("valid") is True and s.get("boot_ids") == [BOOT] and s.get("records") == records, s)
    k.expect("verdict line", "WINDOW2 GATE3 PASS | CTS PASS -> exit 0" in text, text[-400:])


def scenario_missing_block(k, cn, tree):
    print("[missing-block] a block without a report makes CTS INCOMPLETE")
    cn.window("missing", drop=("ssbo",), empty=("dsa",), no_results=("texture",))
    rc, text, v = run_reducer(cn.root, "missing", tree)
    k.expect("exit != 0", rc != 0, "rc=%d" % rc)
    k.expect("CTS INCOMPLETE", v.get("cts", {}).get("overall") == "INCOMPLETE" and "CTS INCOMPLETE" in text, text)
    k.expect("ssbo MISSING (never finished)", block(v, "ssbo").get("verdict") == "MISSING"
             and "no runs/ssbo/.done" in " ".join(block(v, "ssbo").get("reasons", [])), block(v, "ssbo"))
    k.expect("dsa MISSING (.done but empty report)", block(v, "dsa").get("verdict") == "MISSING"
             and "report-dsa.json is empty" in " ".join(block(v, "dsa").get("reasons", [])), block(v, "dsa"))
    k.expect("texture MISSING (report without results)", block(v, "texture").get("verdict") == "MISSING"
             and "has no results" in " ".join(block(v, "texture").get("reasons", [])), block(v, "texture"))
    k.expect("the two complete blocks still PASS", block(v, "shader-image").get("verdict") == "PASS"
             and block(v, "packed-pixels").get("verdict") == "PASS")
    k.expect("GATE3 unaffected (PASS)", v.get("gate3", {}).get("overall") == "PASS")
    cn.window("no-cts", cts=False)
    rc, text, v = run_reducer(cn.root, "no-cts", tree)
    k.expect("no cts/ at all -> CTS INCOMPLETE, exit != 0", rc != 0 and v.get("cts", {}).get("overall") == "INCOMPLETE"
             and all(b["verdict"] == "MISSING" for b in v["cts"]["blocks"]), text)


def scenario_boot_id(k, cn, tree):
    print("[boot-id-change] every gate-3 record must carry the session's one boot_id")
    out = cn.window("boot-done")
    done = out / "gate3/state/spawn" / (CASES[1] + ".done")
    done.write_text(done.read_text().replace(BOOT, OTHER_BOOT))
    rc, text, v = run_reducer(cn.root, "boot-done", tree)
    s = v.get("gate3", {}).get("session", {})
    k.expect("spawn .done on another boot -> GATE3 INVALID-SESSION, exit != 0",
             rc != 0 and v.get("gate3", {}).get("overall") == "INVALID-SESSION" and "GATE3 INVALID-SESSION" in text, text)
    k.expect("the other boot_id is named", any(OTHER_BOOT in p for p in s.get("problems", [])), s)
    k.expect("...although every case's pictures PASS", all(e["verdict"] == "PASS" for e in v["gate3"]["cases"]))

    out = cn.window("boot-repeat")
    rep = out / "gate3/archive/inproc" / (CASES[0] + "-DirectVulkan") / "repeat-02" / "boot_id.txt"
    rep.write_text("boot_id=%s boot_id_before=%s\n" % (OTHER_BOOT, BOOT))
    rc, text, v = run_reducer(cn.root, "boot-repeat", tree)
    k.expect("one repeat's boot_id.txt on another boot -> INVALID-SESSION",
             rc != 0 and v.get("gate3", {}).get("overall") == "INVALID-SESSION", text)

    out = cn.window("boot-old-format")
    done = out / "gate3/state/monolith" / (CASES[0] + ".done")
    done.write_text("rc=0 seconds=8 attempts=1 passed_lines=1 finished=2026-09-23T09:40:14-04:00\n")
    rc, text, v = run_reducer(cn.root, "boot-old-format", tree)
    k.expect(".done without boot_id (pre-ruling format) -> INVALID-SESSION",
             rc != 0 and v.get("gate3", {}).get("overall") == "INVALID-SESSION"
             and any("carry no boot_id" in p for p in v["gate3"]["session"]["problems"]), text)

    out = cn.window("boot-no-session")
    (out / "session/boot-id.txt").unlink()
    rc, text, v = run_reducer(cn.root, "boot-no-session", tree)
    k.expect("no session/boot-id.txt -> INVALID-SESSION",
             rc != 0 and v.get("gate3", {}).get("overall") == "INVALID-SESSION", text)


def scenario_monolith_arm(k, cn, tree):
    print("[monolith-arm] the monolith arm must be the monolith; split arms must prove theirs")
    out = cn.window("mono-ipc")
    log = out / "gate3/archive/monolith" / (CASES[0] + "-DirectVulkan") / "repeat-01" / "mobilegl.log"
    log.write_text(LOGS["monolith"] + TAG + IPC % 1 + "\n")
    rc, text, v = run_reducer(cn.root, "mono-ipc", tree)
    e = case(v, CASES[0])
    k.expect("monolith log with Config: IPC -> case FAIL, GATE3 FAIL",
             e.get("verdict") == "FAIL" and v["gate3"]["overall"] == "FAIL" and rc != 0
             and any("NOT proven monolith" in r for r in e.get("reasons", [])), e)
    k.expect("the other case still PASS", case(v, CASES[1]).get("verdict") == "PASS")
    k.expect("arm identity line counts 1/2 monolith", "monolith 1/2 repeat(s)" in text, text)

    out = cn.window("mono-nolog")
    (out / "gate3/archive/monolith" / (CASES[1] + "-DirectVulkan") / "repeat-01" / "mobilegl.log").unlink()
    rc, text, v = run_reducer(cn.root, "mono-nolog", tree)
    e = case(v, CASES[1])
    k.expect("monolith repeat without mobilegl.log -> FAIL (identity unproven)",
             e.get("verdict") == "FAIL" and any("unproven" in r for r in e.get("reasons", [])), e)

    out = cn.window("split-proof")
    p = out / "gate3/archive/spawn" / (CASES[0] + "-DirectVulkan") / "repeat-02" / "transport-proof.json"
    p.write_text(json.dumps({"required_transport": "spawn", "passed": False}))
    rc, text, v = run_reducer(cn.root, "split-proof", tree)
    e = case(v, CASES[0])
    k.expect("spawn transport-proof not passed -> FAIL",
             e.get("verdict") == "FAIL" and any("spawn rep2 arm proof" in r for r in e.get("reasons", [])), e)


def scenario_cross_arm(k, cn, tree):
    print("[cross-arm] cross-arm identity is reported (WARN), within-arm identity is the gate")
    out = cn.root / "cross"
    out.mkdir()
    cn.gate3(out, picture={"spawn": 129})
    cn.cts(out)
    rc, text, v = run_reducer(cn.root, "cross", tree)
    k.expect("spawn != inproc != monolith pictures -> GATE3 still PASS, exit 0",
             rc == 0 and v.get("gate3", {}).get("overall") == "PASS", text)
    k.expect("a WARN cross-arm line per case", all(("WARN cross-arm %s:" % c) in text for c in CASES), text)
    k.expect("cross_arm.identical false in the JSON", all(case(v, c).get("cross_arm", {}).get("identical") is False for c in CASES))
    k.expect("the ruling is named", "ruling: integrator 2026-09-23" in text)

    out = cn.window("within")
    rep = out / "gate3/archive/inproc" / (CASES[1] + "-DirectVulkan") / "repeat-03"
    (rep / (CASES[1] + "-DirectVulkan-actual.png")).write_bytes(cn.png(127))
    rc, text, v = run_reducer(cn.root, "within", tree)
    e = case(v, CASES[1])
    k.expect("one inproc repeat differs from its siblings -> FAIL",
             e.get("verdict") == "FAIL" and any("inproc repeats NOT bit-identical" in r for r in e.get("reasons", [])), e)


def scenario_unfinished(k, cn, tree):
    print("[unfinished] a pair without .done or with a repeat lacking its picture is INCOMPLETE")
    out = cn.window("nodone")
    (out / "gate3/state/inproc" / (CASES[1] + ".done")).unlink()
    rc, text, v = run_reducer(cn.root, "nodone", tree)
    e = case(v, CASES[1])
    k.expect("pair without .done -> case INCOMPLETE, GATE3 INCOMPLETE",
             e.get("verdict") == "INCOMPLETE" and v["gate3"]["overall"] == "INCOMPLETE" and rc != 0
             and any("no state .done" in r for r in e.get("reasons", [])), e)
    out = cn.window("nopng")
    (out / "gate3/archive/spawn" / (CASES[0] + "-DirectVulkan") / "repeat-03" / (CASES[0] + "-DirectVulkan-actual.png")).unlink()
    rc, text, v = run_reducer(cn.root, "nopng", tree)
    e = case(v, CASES[0])
    k.expect("repeat without actual PNG -> INCOMPLETE", e.get("verdict") == "INCOMPLETE"
             and any("no *-actual.png" in r for r in e.get("reasons", [])), e)


def check_dryrun_numbers(k, v, text, label):
    dsa, si, tex = block(v, "dsa"), block(v, "shader-image"), block(v, "texture")
    k.expect("%s: dsa contract delta -0.272 pp" % label, r3(dsa.get("delta_pp")) == -0.272, dsa.get("delta_pp"))
    k.expect("%s: dsa BASE 368/(368+2) = 99.459%%, AFTER 366/(366+3) = 99.187%%" % label,
             r3(100 * dsa["base"]["rate"]) == 99.459 and r3(100 * dsa["after"]["rate"]) == 99.187
             if dsa.get("base") else False, dsa)
    k.expect("%s: dsa pre-ruling delta -0.539 pp kept as info" % label,
             r3(dsa.get("delta_pp_results_info")) == -0.539 and "(-0.539)" in text, dsa.get("delta_pp_results_info"))
    k.expect("%s: dsa FAIL on the new crash only, not on the rate" % label,
             dsa.get("verdict") == "FAIL" and dsa.get("reasons") == ["1 NEW crash(es)"]
             and dsa.get("new_crash") == [DSA_STORAGE], dsa.get("reasons"))
    k.expect("%s: dsa counts P/F/NS/W/X 366/3/0/1/1" % label,
             [dsa.get("after", {}).get(x) for x in ("pass", "fail", "ns", "warn", "crash")] == [366, 3, 0, 1, 1])
    k.expect("%s: shader-image +4.284 pp, FAIL on new crash incomplete_textures" % label,
             r3(si.get("delta_pp")) == 4.284 and si.get("verdict") == "FAIL" and si.get("new_crash") == [SI_INCOMPLETE],
             (si.get("delta_pp"), si.get("verdict"), si.get("new_crash")))
    k.expect("%s: texture +0.960 pp PASS" % label, r3(tex.get("delta_pp")) == 0.96 and tex.get("verdict") == "PASS",
             (tex.get("delta_pp"), tex.get("verdict")))
    k.expect("%s: ssbo, packed-pixels 0.000 pp PASS" % label,
             all(r3(block(v, b).get("delta_pp")) == 0.0 and block(v, b).get("verdict") == "PASS"
                 for b in ("ssbo", "packed-pixels")))
    k.expect("%s: CTS FAIL" % label, v.get("cts", {}).get("overall") == "FAIL" and "\nCTS FAIL" in text)
    k.expect("%s: printed '-0.272'" % label, "-0.272" in text)


def scenario_dryrun(k, cn, tree, dry_dir):
    print("[dry-run] contract formula on the pre-14e1c8b9 dry-run data")
    fixture = json.loads(FIXTURE.read_text(encoding="utf-8"))
    diffs = {b: fixture["blocks"][b]["after_differences"] for b in BLOCKS}
    out = cn.root / "dryrun"
    out.mkdir()
    rebuilt = cn.cts(out, differences=diffs)
    for b in BLOCKS:
        want = fixture["blocks"][b]
        got_sha = hashlib.sha256(json.dumps(rebuilt[b], sort_keys=True).encode()).hexdigest()
        k.expect("rebuild of %s is the recorded report (counts + sha256)" % b,
                 dict(Counter(rebuilt[b].values())) == want["after_counts"]
                 and got_sha == want["after_sha256_of_sorted_results"], (Counter(rebuilt[b].values()), got_sha))
    rc, text, v = run_reducer(cn.root, "dryrun", tree)
    print("    --- reducer output (canned rebuild), CTS section ---")
    start = text.find("== CTS AFTER")
    for line in text[start:].splitlines():
        if line.strip():
            print("    | " + line)
    check_dryrun_numbers(k, v, text, "canned")
    k.expect("canned: no gate3/ -> GATE3 NOT-RUN, exit != 0",
             rc != 0 and v.get("gate3", {}).get("overall") == "NOT-RUN")
    if dry_dir and (Path(dry_dir) / "cts").is_dir():
        dry = Path(dry_dir)
        rc2, text2, v2 = run_reducer(dry.parent, dry.name, tree, json_out=cn.root / "real-dry-run-verdict.json")
        check_dryrun_numbers(k, v2, text2, "real %s" % dry.name)
    else:
        print("  (skip) real dry-run output not found at %s" % dry_dir)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tools", help="MobileGL tree (default: the tree holding this file, else $W2_TOOLS_OVERRIDE)")
    ap.add_argument("--dry-run-dir", default=str(Path.home() / "w7/logs/devprep/w2/pre-14e1c8b9"))
    ap.add_argument("--keep", action="store_true", help="keep the canned directories")
    args = ap.parse_args()
    tree = find_tree(args.tools)
    if tree is None:
        print("no MobileGL tree with tools/trace_replay/compare_actuals.py and %s: pass --tools" % CTS_BASE_REL,
              file=sys.stderr)
        return 2
    sys.path.insert(0, str(tree / "tools" / "trace_replay"))
    import compare_actuals as ca
    root = Path(tempfile.mkdtemp(prefix="w2-reduce-selftest-"))
    print("60-reduce self-test: reducer %s, tree %s, canned windows in %s" % (REDUCER, tree, root))
    k = Checks()
    cn = Canned(root, tree, ca)
    try:
        scenario_control(k, cn, tree)
        scenario_missing_block(k, cn, tree)
        scenario_boot_id(k, cn, tree)
        scenario_monolith_arm(k, cn, tree)
        scenario_cross_arm(k, cn, tree)
        scenario_unfinished(k, cn, tree)
        scenario_dryrun(k, cn, tree, args.dry_run_dir)
    finally:
        if not args.keep:
            shutil.rmtree(root, ignore_errors=True)
    print("60-reduce self-test: %d/%d checks passed%s" % (
        k.total - len(k.failed), k.total, "" if not k.failed else "; FAILED: " + "; ".join(k.failed)))
    return 0 if not k.failed else 1


if __name__ == "__main__":
    sys.exit(main())
