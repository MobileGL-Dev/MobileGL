#!/usr/bin/env python3
"""60-reduce-selftest.py [--tools TREE] [--dry-run-dir DIR] [--reducer PY] [--keep]

Self-test of 60-reduce.py: builds canned window output directories in a temp dir and runs the
reducer on each exactly as window.sh does (a subprocess, <stamp> --logroot --tools), then checks
the printed verdict, verdict.json and the exit status. Gate 3 is canned at the gate's own
denominator - the canonical 36, derived here as 30-gate3.sh derives gate3/cases.txt (the tree's
trace_cases.json CI split DirectVulkan cases minus CONTRACT-P7 7.1's three exclusions) - with
monolith x3 (ID-P7-62) / inproc x3 / spawn x3 / ra0, one boot_id, reboot-clean, unless a scenario
says otherwise. The scenarios:

  control         gate 3 and all five CTS blocks equal to $BASE    -> GATE3 PASS, CTS PASS, exit 0
  missing-block   ssbo never finished, dsa's report is empty, texture's has no results; and a
                  window with no cts/ at all                      -> CTS INCOMPLETE, exit != 0
  boot-id-change  a spawn pair's .done / one repeat's boot_id.txt names another boot; a .done
                  in the pre-ruling format (no boot_id); no session/boot-id.txt
                                                                  -> GATE3 INVALID-SESSION
  monolith-arm    the monolith repeat's log carries Config: IPC / has no log; a split repeat's
                  transport-proof did not pass                    -> that case FAIL
  repeats         the contract's 3, not the runner's record: a --repeat 1 run -> GATE3 FAIL-REPEATS;
                  the same archive under an arms.txt that claims repeat=3 -> FAIL-REPEATS; one
                  finished spawn pair of 2 repeats -> FAIL-REPEATS; the same pair unfinished
                  (no .done) -> INCOMPLETE (a resume re-runs it); each exit != 0
  arms            all four arms on every case: a run started without inproc-ra0 (or spawn) ->
                  GATE3 FAIL-ARMS; ra0 named in arms.txt but not reached / one case without its
                  ra0 -> INCOMPLETE; a ra0 repeat whose log says run-ahead=1 -> that case FAIL
  case-set        cases.txt must EQUAL the canonical 36: 35 + create-indirect, 35 + a name outside
                  the manifest, 36 + an exclusion, a case listed twice -> GATE3 FAIL-CASESET
  cross-arm       cross-arm identity is not a per-case gate, but every mismatch needs an
                  adjudication: spawn's pictures differ on every case -> WARN + px counts,
                  GATE3 PASS-NEEDS-ADJUDICATION, exit 1; every case in gate3/adjudication.tsv ->
                  GATE3 PASS, exit 0; one case left out (a line without a reason) -> still
                  NEEDS-ADJUDICATION naming it; one case differing, adjudicated (+ a stale line) ->
                  PASS; one inproc repeat differs from its siblings -> FAIL (within-arm identity)
  unfinished      a pair without .done; a repeat without an actual PNG -> GATE3 INCOMPLETE
  monolith-determinism  ID-P7-62, on synthetic 8x8 pictures (base grey 128, the first n px at
                  another level): a bit-identical monolith x3 + a nondeterministic inproc -> FAIL
                  (strict clause); a nondeterministic monolith (3 distinct / 3 passes, 10 px / delta
                  4 apart) + split pictures inside 1.25x (10 px / delta 5) -> PASS, exit 0, no
                  adjudication line needed, the numbers printed; the same with the stdlib decoder
                  (W2_REDUCE_PNG_DECODER=pure) -> the same numbers; split beyond 1.25x in delta
                  (6 > 5) or in pixels (13 > 12.5) -> FAIL; one split ssim 0.0005+ from ONE
                  monolith reading -> FAIL; the pre-ID-P7-62 monolith x1 + a nondeterministic
                  split -> FAIL naming the >= 3 passes it lacks; a finished monolith pair short of
                  arms.txt's monolith_repeat -> FAIL; --extra-monolith DIR@BOOT adding two passes
                  to that x1 archive -> the rule applies, GATE3 PASS-WITH-EXTRA-READINGS (exit 1),
                  without @BOOT or with another boot -> INVALID-SESSION
  rate-gate       the gate rate alone decides (no crash anywhere): k $BASE-Pass dsa cases
                  turn Fail. k=1: 367/370, -0.270 pp -> PASS, exit 0; k=2: -0.541 pp -> FAIL on
                  the rate; k=4: 364/6/0/1/0, -1.081 pp -> FAIL, CTS FAIL
  cts-lost        a $BASE-Pass case AFTER turns NotSupported / another non-Pass, non-Fail status
                  (L): dsa Pass->NS -> NON-PASS in the rate (367/370, -0.270 pp; the literal
                  Pass/(Pass+Fail) -0.001 pp printed as info), listed, dsa FAIL, CTS FAIL; with a
                  cts/adjudication.tsv line -> dsa PASS, exit 0, printed ADJUDICATED and OUT of the
                  block's rate on both sides (367/369 vs 367/369, +0.000 pp); one adjudicated + one
                  not -> L 1, -0.271 pp, FAIL on the unadjudicated one; 2 adjudicated NS + 2
                  Pass->Fail -> -0.543 pp, FAIL on the rate; a line without a reason adjudicates
                  nothing; ssbo Pass->NS adjudicated -> ssbo PASS (+0.000 pp, not -0.806); codex's
                  example, 123 of 124 ssbo Passes -> NS: literal 100% but gate rate 0.806% -> FAIL;
                  Pass->CompatibilityWarning -> L; Pass->Crash adjudicated -> still FAIL (a NEW crash
                  is never adjudicated)
  vacuous         no verdict from nothing: the caselist a run names is gone (the dry run's AFTER
                  data, two new crashes) -> every block MISSING, CTS INCOMPLETE - and with the
                  caselist copy the window keeps, the real reading (CTS FAIL); no caselist at all /
                  an empty one -> MISSING; every AFTER result NotSupported -> every block FAIL
                  (gate rate 0, every $BASE Pass an L case)
  not-a-gate      a pass that is not the gate's is never PASS / exit 0: 2 of the canonical 36 ->
                  GATE3 PASS-SUBSET; not reboot-clean -> GATE3 PASS-NOT-REBOOT-CLEAN; a --limit CTS
                  block -> CTS PASS-SUBSET; each exit != 0
  warnings        information lines: a case $BASE lacks (AFTER Fail) stays out of both rates
                  (texture delta 0) with a WARN
  check-block     60-reduce.py --check-block (50-cts-after.sh's test before a block's .done):
                  complete -> 0; unrun.txt non-empty / a case without result / caselist gone /
                  empty report -> 3
  dry-run         the pre-14e1c8b9 CTS dry run rebuilt from $BASE + its recorded differences
                  (60-reduce-selftest-pre14e1c8b9.json): gate rate Pass/(Pass+Fail+L) gives dsa
                  -0.541 pp (renderbuffers_storage Pass->Crash is an L case; the literal
                  Pass/(Pass+Fail) -0.272 pp and the pre-ruling Pass/(results-NS) -0.539 pp, rates
                  99.191% -> 98.652%, are printed as info only), shader-image +4.284, texture
                  +0.960, ssbo/packed-pixels 0; dsa FAIL on its NEW crash, the rate and the
                  unadjudicated loss, shader-image on one NEW crash -> CTS FAIL
When --dry-run-dir (default ~/w7/logs/devprep/w2/pre-14e1c8b9) exists, the reducer also runs on
that real output (verdict JSON to the temp dir) and must give the same CTS numbers.
TREE (compare_actuals.py + trace_cases.json + device-window-1/CTS-base + tools/cts/caselists)
defaults to the MobileGL tree this file sits in, else $W2_TOOLS_OVERRIDE, else $W2_PIPE /
~/w7/pipe (only read).
--reducer runs the checks against another 60-reduce.py (red-once of an older reducer).
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
# CONTRACT-P7 7.1: the three cases red on monolith, out of the 39 CI split DirectVulkan cases.
EXCLUDED = ("minecraft-1.21.1-neoforge-create-indirect-in-world", "minecraft-1.21.11-main-menu",
            "minecraft-1.21.4-fabric-iris-photon-v1.3b-in-world")
# Filled by main() from the tree's trace_cases.json: the canonical 36 (manifest order) and the two
# non-OpenRA cases the scenarios poke.
CASES, A, B = (), None, None
OPENRA = "OpenRA"
REPEAT = 3
GONE = "/nonexistent/p7w7-tree-deadbeef/tools/cts/caselists/p7-%s-gl46.txt"
ARMS = (("monolith", REPEAT), ("inproc", REPEAT), ("spawn", REPEAT), ("inproc-ra0", 1))
# The pre-ID-P7-62 30-gate3.sh: monolith x1, and no monolith_repeat= in gate3/arms.txt.
ARMS_MONO1 = (("monolith", 1),) + ARMS[1:]
ARMS_TXT_MONO1 = "arms=monolith inproc spawn inproc-ra0\nrepeat=3\n"
DSA_STORAGE = "KHR-GL46.direct_state_access.renderbuffers_storage"
SI_INCOMPLETE = "KHR-GL46.shader_image_load_store.incomplete_textures"
CODEX_RULING = "ruling: integrator 2026-09-23 after codex closeout review"
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


def lookup(table, arm, case, repeat, default):
    """A canned per-repeat value: (arm, case, repeat), else (arm, case), else arm, else default."""
    for key in ((arm, case, repeat), (arm, case), arm):
        if key in table:
            return table[key]
    return default


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
        if (c and (Path(c) / "tools/trace_replay/compare_actuals.py").is_file()
                and (Path(c) / "tools/trace_replay/trace_cases.json").is_file() and (Path(c) / CTS_BASE_REL).is_dir()):
            return Path(c)
    return None


def canonical_cases():
    """CONTRACT-P7 7.1's denominator, derived here independently of the reducer, the way 30-gate3.sh
    writes gate3/cases.txt: -> (the 39 CI split DirectVulkan cases, those minus EXCLUDED)."""
    from trace_cases import ci_backends, ci_trace_cases, load_trace_cases, split_trace_cases
    split_dv = [c["name"] for c in split_trace_cases(ci_trace_cases(load_trace_cases()))
                if "DirectVulkan" in ci_backends(c)]
    return split_dv, tuple(c for c in split_dv if c not in EXCLUDED)


# ------------------------------------------------------------------------------------------------
# canned window directories
# ------------------------------------------------------------------------------------------------
class Canned:
    def __init__(self, root, tree, ca):
        self.root, self.tree, self.ca = root, tree, ca

    def png(self, value):
        """value: a grey level (a solid 8x8 picture), or (n, level): grey 128 with its first n pixels
        (row-major) at `level` - the synthetic 'speckle' the ID-P7-62 scenarios move around."""
        if isinstance(value, tuple):
            n, level = value
            rgba = bytearray(self.ca._solid(8, 8, 128))
            for i in range(n):
                rgba[i * 4:i * 4 + 3] = bytes([level] * 3)
            return self.ca._png_bytes(8, 8, bytes(rgba))
        return self.ca._png_bytes(8, 8, self.ca._solid(8, 8, value))

    def gate3(self, out, picture=None, boot=BOOT, cases=None, reboot_clean=True, arms=ARMS, arms_txt=None,
              excluded=EXCLUDED, ssim=None):
        """30-gate3.sh's output. picture: {arm, (arm, case) or (arm, case, repeat): Canned.png value}
        (default 128 everywhere); ssim: the same keys -> result.json's ssim (default 0.998, OpenRA
        1.0); arms: ((arm, repeats), ...) actually run; arms_txt: gate3/arms.txt's text (default:
        what 30-gate3.sh writes for `arms`)."""
        picture, ssim = picture or {}, ssim or {}
        cases = CASES if cases is None else cases
        g = out / "gate3"
        write(g / "cases.txt", "\n".join(cases) + "\n")
        write(g / "excluded.txt", "\n".join(excluded) + "\n")
        repeat = max([n for a, n in arms if a in ("inproc", "spawn")] or [REPEAT])
        write(g / "arms.txt", arms_txt if arms_txt is not None
              else "arms=%s\nrepeat=%d\nmonolith_repeat=%d\n" % (" ".join(a for a, _ in arms), repeat,
                                                                   dict(arms).get("monolith", repeat)))
        write(out / "session/boot-id.txt", boot + "\n")
        write(out / "session/reboot-clean.txt",
              ("reboot-clean: 00000000-0000-0000-0000-000000000000 -> %s\n" % boot) if reboot_clean
              else ("NOT reboot-clean (no --reboot): boot_id %s, up 5000.00s\n" % boot))
        for arm, n in arms:
            for case in cases:
                name = case + "-DirectVulkan"
                openra = case == OPENRA
                for i in range(1, n + 1):
                    rep = g / "archive" / arm / name / ("repeat-%02d" % i)
                    write(rep / "result.json", json.dumps({
                        "passed": True, "statusCode": 0, "backend": "DirectVulkan",
                        "ssim": lookup(ssim, arm, case, i, 1.0 if openra else 0.998),
                        "ssimThreshold": 0.99, "mismatchPixels": 0 if openra else 17,
                        "matchedGoldenPath": "/in/golden.png", "cropX": 0, "cropY": 0, "cropWidth": 0, "cropHeight": 0}))
                    write(rep / (name + "-actual.png"), self.png(lookup(picture, arm, case, i, 128)))
                    write(rep / "logcat.txt", "canned logcat %s %s repeat %d\n" % (arm, case, i))
                    write(rep / "mobilegl.log", LOGS[arm])
                    if arm in ("inproc", "spawn"):
                        write(rep / "transport-proof.json", json.dumps({"required_transport": arm, "passed": True}))
                    write(rep / "boot_id.txt", "boot_id=%s boot_id_before=%s stamped=2026-09-23T12:00:00-04:00\n" % (boot, boot))
                write(g / "state" / arm / (case + ".done"),
                      "rc=0 seconds=7 attempts=1 passed_lines=%d boot_id=%s boot_id_before=%s "
                      "finished=2026-09-23T12:00:00-04:00\n" % (n, boot, boot))
        return g

    def extra_monolith(self, d, case, pictures, boot=None):
        """A run_android_retrace_local.py --archive-dir tree of monolith passes outside gate3/ (the
        g3det E1-mono shape): one repeat per `pictures` value; boot: stamp repeat-NN/boot_id.txt."""
        name = case + "-DirectVulkan"
        for i, value in enumerate(pictures, 1):
            rep = d / name / ("repeat-%02d" % i)
            write(rep / "result.json", json.dumps({
                "passed": True, "statusCode": 0, "backend": "DirectVulkan", "ssim": 0.998, "ssimThreshold": 0.99,
                "mismatchPixels": 17, "matchedGoldenPath": "/in/golden.png", "cropX": 0, "cropY": 0,
                "cropWidth": 0, "cropHeight": 0}))
            write(rep / (name + "-actual.png"), self.png(value))
            write(rep / "logcat.txt", "canned extra logcat %s repeat %d\n" % (case, i))
            write(rep / "mobilegl.log", LOGS["monolith"])
            if boot:
                write(rep / "boot_id.txt", "boot_id=%s boot_id_before=%s\n" % (boot, boot))
        return d

    def base(self, b):
        return dict(json.loads((self.tree / CTS_BASE_REL / ("report-%s.json" % b)).read_text(encoding="utf-8"))["results"])

    def caselist(self, b):
        return self.tree / "tools/cts/caselists" / ("p7-%s-gl46.txt" % b)

    def cts(self, out, differences=None, drop=(), empty=(), no_results=(), copy=True, named=None, lists=None,
            adjudication=None):
        """50-cts-after.sh's output for the five blocks: AFTER = $BASE + `differences` per block.
        copy: keep runs/<b>/caselist.txt as 50-cts-after.sh does; named: b -> what caselist.path
        says (None = the tools tree's list, '' = no caselist.path); lists: b -> the cases the run
        was asked to run (a --limit subset, or a list with cases $BASE lacks); adjudication: the
        text of cts/adjudication.tsv."""
        differences, lists = differences or {}, lists or {}
        skip = set(read_list(self.tree / "tools/cts/caselists/p7-skip.txt"))
        c = out / "cts"
        c.mkdir(parents=True, exist_ok=True)
        if adjudication is not None:
            write(c / "adjudication.tsv", adjudication)
        rebuilt = {}
        for b in BLOCKS:
            if b in drop:
                continue
            caselist = self.caselist(b)
            run = c / "runs" / b
            results = self.base(b)
            if b in lists:
                caselist = c / "caselists" / ("p7-%s-gl46.canned.txt" % b)
                write(caselist, "\n".join(lists[b]) + "\n")
                results = {k: v for k, v in results.items() if k in set(lists[b])}
            path = named(b) if named else str(caselist)
            if path:
                write(run / "caselist.path", path + "\n")
            if copy:
                write(run / "caselist.txt", caselist.read_bytes())
            skipped = [x for x in read_list(caselist) if x in skip]
            if skipped:
                write(run / "skipped.txt", "\n".join(skipped) + "\n")
            write(run / "unrun.txt", "")
            write(run / "hung.txt", "")
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

    def window(self, stamp, gate3=True, cts=True, gate3_args=None, **cts_args):
        out = self.root / stamp
        out.mkdir(parents=True)
        if gate3:
            self.gate3(out, **(gate3_args or {}))
        else:
            write(out / "session/boot-id.txt", BOOT + "\n")
        if cts:
            self.cts(out, **cts_args)
        return out


def run_reducer(logroot, stamp, tree, json_out=None, extra=(), env=None):
    cmd = [sys.executable, str(REDUCER), stamp, "--logroot", str(logroot), "--tools", str(tree)]
    if json_out:
        cmd += ["--json", str(json_out)]
    for spec in extra:
        cmd += ["--extra-monolith", str(spec)]
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True,
                       env=dict(os.environ, **(env or {})))
    vpath = Path(json_out) if json_out else Path(logroot) / stamp / "verdict.json"
    verdict = json.loads(vpath.read_text(encoding="utf-8")) if vpath.is_file() else {}
    return p.returncode, p.stdout, verdict


def run_check_block(run, report):
    p = subprocess.run([sys.executable, str(REDUCER), "--check-block", str(run), str(report)],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
    return p.returncode, p.stdout


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


def overall(verdict):
    return verdict.get("gate3", {}).get("overall")


def g3(verdict, key, default=None):
    """verdict.json's gate3[key], or `default` (an older reducer, --reducer, may lack the key)."""
    return verdict.get("gate3", {}).get(key, default)


def contract(verdict, key):
    return (g3(verdict, "contract") or {}).get(key)


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
    k.expect("GATE3 PASS 36/36", overall(v) == "PASS" and "GATE3 PASS: 36/36 cases PASS\n" in text, text)
    k.expect("the contract constants held (repeats, four arms, canonical case set)",
             g3(v, "contract") == {"repeats": [], "arms": [], "cases": []}
             and "case set == the canonical 36 (39 CI split DirectVulkan - 3 exclusions): held" in text,
             g3(v, "contract"))
    k.expect("CTS PASS, all five blocks", v.get("cts", {}).get("overall") == "PASS"
             and [b["block"] for b in v["cts"]["blocks"]] == list(BLOCKS), text)
    k.expect("every block delta 0.000 pp, no L case", all(r3(block(v, b).get("delta_pp")) == 0.0
                                                         and block(v, b).get("lost") == [] for b in BLOCKS))
    k.expect("UBO recorded 'unrun (not in this glcts)'", v.get("cts", {}).get("ubo", {}).get("verdict") == "UNRUN"
             and "unrun (not in this glcts)" in text)
    s = v.get("gate3", {}).get("session", {})
    records = 2 * len(CASES) * (len(ARMS) + sum(n for _, n in ARMS))  # (.done + repeats) x (boot_id, before)
    k.expect("session valid: one boot_id over %d records" % records,
             s.get("valid") is True and s.get("boot_ids") == [BOOT] and s.get("records") == records, s)
    k.expect("RUN_AHEAD=0 control on 36/36 cases, proven", "RUN_AHEAD=0 control (required on every case, picture "
             "recorded, not gating): 36/36 case(s); proofs: run-ahead=0" in text, text)
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
    k.expect("GATE3 unaffected (PASS)", overall(v) == "PASS")
    cn.window("no-cts", cts=False)
    rc, text, v = run_reducer(cn.root, "no-cts", tree)
    k.expect("no cts/ at all -> CTS INCOMPLETE, exit != 0", rc != 0 and v.get("cts", {}).get("overall") == "INCOMPLETE"
             and all(b["verdict"] == "MISSING" for b in v["cts"]["blocks"]), text)


def scenario_boot_id(k, cn, tree):
    print("[boot-id-change] every gate-3 record must carry the session's one boot_id")
    out = cn.window("boot-done")
    done = out / "gate3/state/spawn" / (B + ".done")
    done.write_text(done.read_text().replace(BOOT, OTHER_BOOT))
    rc, text, v = run_reducer(cn.root, "boot-done", tree)
    s = v.get("gate3", {}).get("session", {})
    k.expect("spawn .done on another boot -> GATE3 INVALID-SESSION, exit != 0",
             rc != 0 and overall(v) == "INVALID-SESSION" and "GATE3 INVALID-SESSION" in text, text)
    k.expect("the other boot_id is named", any(OTHER_BOOT in p for p in s.get("problems", [])), s)
    k.expect("...although every case's pictures PASS", all(e["verdict"] == "PASS" for e in g3(v, "cases", [])))

    out = cn.window("boot-repeat")
    rep = out / "gate3/archive/inproc" / (A + "-DirectVulkan") / "repeat-02" / "boot_id.txt"
    rep.write_text("boot_id=%s boot_id_before=%s\n" % (OTHER_BOOT, BOOT))
    rc, text, v = run_reducer(cn.root, "boot-repeat", tree)
    k.expect("one repeat's boot_id.txt on another boot -> INVALID-SESSION",
             rc != 0 and overall(v) == "INVALID-SESSION", text)

    out = cn.window("boot-old-format")
    done = out / "gate3/state/monolith" / (A + ".done")
    done.write_text("rc=0 seconds=8 attempts=1 passed_lines=1 finished=2026-09-23T09:40:14-04:00\n")
    rc, text, v = run_reducer(cn.root, "boot-old-format", tree)
    k.expect(".done without boot_id (pre-ruling format) -> INVALID-SESSION",
             rc != 0 and overall(v) == "INVALID-SESSION"
             and any("carry no boot_id" in p for p in v["gate3"]["session"]["problems"]), text)

    out = cn.window("boot-no-session")
    (out / "session/boot-id.txt").unlink()
    rc, text, v = run_reducer(cn.root, "boot-no-session", tree)
    k.expect("no session/boot-id.txt -> INVALID-SESSION", rc != 0 and overall(v) == "INVALID-SESSION", text)


def scenario_monolith_arm(k, cn, tree):
    print("[monolith-arm] the monolith arm must be the monolith; split arms must prove theirs")
    out = cn.window("mono-ipc")
    log = out / "gate3/archive/monolith" / (A + "-DirectVulkan") / "repeat-01" / "mobilegl.log"
    log.write_text(LOGS["monolith"] + TAG + IPC % 1 + "\n")
    rc, text, v = run_reducer(cn.root, "mono-ipc", tree)
    e = case(v, A)
    k.expect("monolith log with Config: IPC -> case FAIL, GATE3 FAIL",
             e.get("verdict") == "FAIL" and overall(v) == "FAIL" and rc != 0
             and any("NOT proven monolith" in r for r in e.get("reasons", [])), e)
    k.expect("the other case still PASS", case(v, B).get("verdict") == "PASS")
    k.expect("arm identity line counts 107/108 monolith passes (x3)", "monolith 107/108 repeat(s)" in text, text)

    out = cn.window("mono-nolog")
    (out / "gate3/archive/monolith" / (B + "-DirectVulkan") / "repeat-01" / "mobilegl.log").unlink()
    rc, text, v = run_reducer(cn.root, "mono-nolog", tree)
    e = case(v, B)
    k.expect("monolith repeat without mobilegl.log -> FAIL (identity unproven)",
             e.get("verdict") == "FAIL" and any("unproven" in r for r in e.get("reasons", [])), e)

    out = cn.window("split-proof")
    p = out / "gate3/archive/spawn" / (A + "-DirectVulkan") / "repeat-02" / "transport-proof.json"
    p.write_text(json.dumps({"required_transport": "spawn", "passed": False}))
    rc, text, v = run_reducer(cn.root, "split-proof", tree)
    e = case(v, A)
    k.expect("spawn transport-proof not passed -> FAIL",
             e.get("verdict") == "FAIL" and any("spawn rep2 arm proof" in r for r in e.get("reasons", [])), e)


def scenario_repeats(k, cn, tree):
    print("[repeats] inproc and spawn need CONTRACT-P7 7.2's 3 repeats, whatever gate3/arms.txt says")
    one = (("monolith", 1), ("inproc", 1), ("spawn", 1), ("inproc-ra0", 1))
    cn.window("rep1", gate3_args={"arms": one})
    rc, text, v = run_reducer(cn.root, "rep1", tree)
    k.expect("a --repeat 1 run (arms.txt repeat=1), every picture passing -> GATE3 FAIL-REPEATS, exit != 0",
             overall(v) == "FAIL-REPEATS" and "\nGATE3 FAIL-REPEATS: " in text and rc != 0
             and "WINDOW2 GATE3 FAIL-REPEATS | CTS PASS -> exit 1" in text, text[-600:])
    k.expect("...naming arms.txt's repeat=1 and the 72 short finished pairs",
             any("repeat=1 < CONTRACT-P7 7.2's 3" in p for p in (contract(v, "repeats") or []))
             and any(p.startswith("72 finished inproc/spawn pair(s)") for p in contract(v, "repeats")),
             g3(v, "contract"))
    k.expect("...each case FAIL on '1 repeat(s) in a finished pair'",
             all(e["verdict"] == "FAIL" and any("inproc: 1 repeat(s) in a finished pair" in r for r in e["reasons"])
                 for e in g3(v, "cases", [])), case(v, A))

    cn.window("rep1-forged", gate3_args={"arms": one, "arms_txt": "arms=monolith inproc spawn inproc-ra0\nrepeat=3\n"})
    rc, text, v = run_reducer(cn.root, "rep1-forged", tree)
    k.expect("the same one-repeat archive under an arms.txt that says repeat=3 -> still GATE3 FAIL-REPEATS",
             overall(v) == "FAIL-REPEATS" and rc != 0 and g3(v, "repeat") == 3
             and any(p.startswith("72 finished inproc/spawn pair(s)") for p in contract(v, "repeats")),
             (overall(v), g3(v, "contract")))

    out = cn.window("rep-short")
    shutil.rmtree(out / "gate3/archive/spawn" / (A + "-DirectVulkan") / "repeat-03")
    rc, text, v = run_reducer(cn.root, "rep-short", tree)
    k.expect("one finished spawn pair with 2 repeats -> GATE3 FAIL-REPEATS naming spawn/%s (2)" % A,
             overall(v) == "FAIL-REPEATS" and rc != 0
             and any(("spawn/%s (2)" % A) in p for p in contract(v, "repeats")), g3(v, "contract"))

    out = cn.window("rep-unfinished")
    shutil.rmtree(out / "gate3/archive/spawn" / (A + "-DirectVulkan") / "repeat-03")
    (out / "gate3/state/spawn" / (A + ".done")).unlink()
    rc, text, v = run_reducer(cn.root, "rep-unfinished", tree)
    k.expect("...the same pair without its .done (a resume re-runs it) -> INCOMPLETE, not FAIL-REPEATS",
             overall(v) == "INCOMPLETE" and case(v, A).get("verdict") == "INCOMPLETE" and rc != 0
             and contract(v, "repeats") == [], (overall(v), case(v, A).get("reasons")))


def scenario_arms(k, cn, tree):
    print("[arms] monolith, inproc, spawn and inproc-ra0 on every case")
    three = ARMS[:3]
    cn.window("no-ra0", gate3_args={"arms": three})
    rc, text, v = run_reducer(cn.root, "no-ra0", tree)
    k.expect("a run started with --arms monolith,inproc,spawn -> GATE3 FAIL-ARMS, exit != 0",
             overall(v) == "FAIL-ARMS" and "\nGATE3 FAIL-ARMS: " in text and rc != 0
             and any("arm inproc-ra0 has no archive" in p for p in contract(v, "arms")), text[-600:])
    k.expect("...each case INCOMPLETE on 'inproc-ra0: not run'",
             all(any("inproc-ra0: not run" in r for r in e["reasons"]) for e in g3(v, "cases", [])), case(v, A))

    cn.window("no-spawn", gate3_args={"arms": (ARMS[0], ARMS[1], ARMS[3])})
    rc, text, v = run_reducer(cn.root, "no-spawn", tree)
    k.expect("a run started without spawn -> GATE3 FAIL-ARMS", overall(v) == "FAIL-ARMS" and rc != 0
             and any("arm spawn has no archive" in p for p in contract(v, "arms")), overall(v))

    cn.window("ra0-planned", gate3_args={"arms": three, "arms_txt": "arms=monolith inproc spawn inproc-ra0\nrepeat=3\n"
                                                                     "monolith_repeat=3\n"})
    rc, text, v = run_reducer(cn.root, "ra0-planned", tree)
    k.expect("inproc-ra0 named in arms.txt but not reached yet -> GATE3 INCOMPLETE (not PASS), exit != 0",
             overall(v) == "INCOMPLETE" and rc != 0 and contract(v, "arms") == []
             and all(e["verdict"] == "INCOMPLETE" for e in g3(v, "cases", [])), overall(v))

    out = cn.window("ra0-one-case")
    shutil.rmtree(out / "gate3/archive/inproc-ra0" / (B + "-DirectVulkan"))
    (out / "gate3/state/inproc-ra0" / (B + ".done")).unlink()
    rc, text, v = run_reducer(cn.root, "ra0-one-case", tree)
    k.expect("one case without its inproc-ra0 -> that case INCOMPLETE, GATE3 INCOMPLETE",
             case(v, B).get("verdict") == "INCOMPLETE" and overall(v) == "INCOMPLETE" and rc != 0
             and case(v, A).get("verdict") == "PASS", (case(v, B).get("reasons"), overall(v)))

    out = cn.window("ra0-not-proven")
    (out / "gate3/archive/inproc-ra0" / (A + "-DirectVulkan") / "repeat-01" / "mobilegl.log").write_text(LOGS["inproc"])
    rc, text, v = run_reducer(cn.root, "ra0-not-proven", tree)
    k.expect("an inproc-ra0 repeat whose log says run-ahead=1 -> that case FAIL (not the control arm)",
             case(v, A).get("verdict") == "FAIL" and overall(v) == "FAIL" and rc != 0
             and any("not proven the RUN_AHEAD=0 arm" in r for r in case(v, A).get("reasons", [])), case(v, A))


def scenario_case_set(k, cn, tree):
    print("[case-set] gate3/cases.txt must EQUAL the canonical denominator, not merely count 36")
    swapped = tuple(c for c in CASES if c != B) + (EXCLUDED[0],)
    cn.window("set-swap", gate3_args={"cases": swapped})
    rc, text, v = run_reducer(cn.root, "set-swap", tree)
    k.expect("36 cases = 35 canonical + create-indirect, all passing -> GATE3 FAIL-CASESET, exit != 0",
             len(swapped) == 36 and overall(v) == "FAIL-CASESET" and "\nGATE3 FAIL-CASESET: 36/36 cases PASS" in text
             and rc != 0, text[-600:])
    k.expect("...naming the case outside and the canonical case missing",
             any(EXCLUDED[0] in p and ("missing: " + B) in p for p in (contract(v, "cases") or [])),
             g3(v, "contract"))
    bogus = tuple(c for c in CASES if c != B) + ("minecraft-9.9-not-a-trace-case",)
    cn.window("set-bogus", gate3_args={"cases": bogus})
    rc, text, v = run_reducer(cn.root, "set-bogus", tree)
    k.expect("36 cases = 35 canonical + a name outside the manifest -> FAIL-CASESET",
             overall(v) == "FAIL-CASESET" and rc != 0, overall(v))
    cn.window("set-37", gate3_args={"cases": CASES + (EXCLUDED[2],)})
    rc, text, v = run_reducer(cn.root, "set-37", tree)
    k.expect("the canonical 36 + photon-v1.3b (37) -> FAIL-CASESET", overall(v) == "FAIL-CASESET" and rc != 0, overall(v))
    out = cn.window("set-dup")
    write(out / "gate3/cases.txt", "\n".join(CASES + (B,)) + "\n")
    rc, text, v = run_reducer(cn.root, "set-dup", tree)
    k.expect("the canonical 36 with one listed twice -> FAIL-CASESET ('more than once')",
             overall(v) == "FAIL-CASESET" and rc != 0
             and any("more than once" in p and B in p for p in contract(v, "cases")), overall(v))


def scenario_cross_arm(k, cn, tree):
    print("[cross-arm] cross-arm identity is not a per-case gate, but every mismatch needs an adjudication")
    cn.window("cross", gate3_args={"picture": {"spawn": 129}})
    rc, text, v = run_reducer(cn.root, "cross", tree)
    k.expect("spawn != inproc == monolith on every case -> GATE3 PASS-NEEDS-ADJUDICATION, exit 1",
             rc == 1 and overall(v) == "PASS-NEEDS-ADJUDICATION" and "\nGATE3 PASS-NEEDS-ADJUDICATION: 36/36 cases PASS" in text
             and "WINDOW2 GATE3 PASS-NEEDS-ADJUDICATION | CTS PASS -> exit 1" in text, text[-600:])
    k.expect("...every case still PASS (not a per-case gate)", all(e["verdict"] == "PASS" for e in g3(v, "cases", [])))
    k.expect("a WARN cross-arm line per case", all(("WARN cross-arm %s:" % c) in text for c in CASES), text)
    k.expect("cross_arm.identical false and 36 unadjudicated in the JSON",
             all(case(v, c).get("cross_arm", {}).get("identical") is False for c in CASES)
             and g3(v, "cross_arm_unadjudicated") == list(CASES))
    x = case(v, B).get("cross_arm", {})
    k.expect("the list carries the archived px!=golden and the direct px counts (8x8: 64 between 128 and 129)",
             x.get("px_vs_golden") == {"monolith": [17, 17, 17], "inproc": [17, 17, 17], "spawn": [17, 17, 17]}
             and x.get("px_direct") == {"inproc-monolith": 0, "spawn-monolith": 64, "inproc-spawn": 64}
             and "px!=golden (archived mismatchPixels) monolith 17..17, inproc 17..17, spawn 17..17; px between the "
                 "arms' first repeats: inproc-monolith 0, spawn-monolith 64, inproc-spawn 64" in text, x)
    k.expect("both rulings are named", "ruling: integrator 2026-09-23;" in text and CODEX_RULING in text)

    out = cn.window("cross-adj", gate3_args={"picture": {"spawn": 129}})
    write(out / "gate3/adjudication.tsv", "# case\treason\n" + "".join(
        "%s\tspawn +1 grey level everywhere, diffed by hand: driver rounding (canned)\n" % c for c in CASES))
    rc, text, v = run_reducer(cn.root, "cross-adj", tree)
    k.expect("...every mismatching case adjudicated in gate3/adjudication.tsv -> GATE3 PASS, exit 0",
             rc == 0 and overall(v) == "PASS" and "GATE3 PASS: 36/36 cases PASS\n" in text
             and text.count("adjudicated: spawn +1 grey level") == 36, text[-600:])

    out = cn.window("cross-adj-partial", gate3_args={"picture": {"spawn": 129}})
    write(out / "gate3/adjudication.tsv", "".join("%s\tcanned reason\n" % c for c in CASES if c != B) + B + "\n")
    rc, text, v = run_reducer(cn.root, "cross-adj-partial", tree)
    k.expect("...35 adjudicated + %s on a line without a reason -> still PASS-NEEDS-ADJUDICATION naming it" % B,
             rc == 1 and overall(v) == "PASS-NEEDS-ADJUDICATION" and g3(v, "cross_arm_unadjudicated") == [B]
             and "adjudication.tsv:36: %s has no reason" % B in text, (overall(v), g3(v, "cross_arm_unadjudicated")))

    out = cn.window("cross-one", gate3_args={"picture": {("spawn", A): 129}})
    rc, text, v = run_reducer(cn.root, "cross-one", tree)
    k.expect("only %s's spawn differs -> PASS-NEEDS-ADJUDICATION listing that one case" % A,
             rc == 1 and overall(v) == "PASS-NEEDS-ADJUDICATION" and g3(v, "cross_arm_mismatched") == [A]
             and "UNADJUDICATED: no gate3/adjudication.tsv line" in text, (overall(v), g3(v, "cross_arm_mismatched")))
    write(out / "gate3/adjudication.tsv", "%s\tone LSB on spawn, canned\n%s\tstale line\n" % (A, B))
    rc, text, v = run_reducer(cn.root, "cross-one", tree)
    k.expect("...adjudicated (plus a stale line for a case whose arms agree) -> GATE3 PASS, exit 0, stale noted",
             rc == 0 and overall(v) == "PASS" and (g3(v, "adjudication") or {}).get("stale") == [B]
             and "names case(s) whose arms agree (nothing to adjudicate): " + B in text, text[-600:])

    out = cn.window("within")
    rep = out / "gate3/archive/inproc" / (B + "-DirectVulkan") / "repeat-03"
    (rep / (B + "-DirectVulkan-actual.png")).write_bytes(cn.png(127))
    rc, text, v = run_reducer(cn.root, "within", tree)
    e = case(v, B)
    k.expect("one inproc repeat differs from its siblings -> FAIL",
             e.get("verdict") == "FAIL" and any("inproc repeats NOT bit-identical" in r for r in e.get("reasons", [])), e)


def scenario_unfinished(k, cn, tree):
    print("[unfinished] a pair without .done or with a repeat lacking its picture is INCOMPLETE")
    out = cn.window("nodone")
    (out / "gate3/state/inproc" / (B + ".done")).unlink()
    rc, text, v = run_reducer(cn.root, "nodone", tree)
    e = case(v, B)
    k.expect("pair without .done -> case INCOMPLETE, GATE3 INCOMPLETE",
             e.get("verdict") == "INCOMPLETE" and overall(v) == "INCOMPLETE" and rc != 0
             and any("no state .done" in r for r in e.get("reasons", [])), e)
    out = cn.window("nopng")
    (out / "gate3/archive/spawn" / (A + "-DirectVulkan") / "repeat-03" / (A + "-DirectVulkan-actual.png")).unlink()
    rc, text, v = run_reducer(cn.root, "nopng", tree)
    e = case(v, A)
    k.expect("repeat without actual PNG -> INCOMPLETE", e.get("verdict") == "INCOMPLETE"
             and any("no *-actual.png" in r for r in e.get("reasons", [])), e)


def scenario_determinism(k, cn, tree):
    print("[monolith-determinism] ID-P7-62: a monolith not bit-identical over >= 3 passes swaps the split arms' "
          "bit-identity clause for the distributional check")
    # A nondeterministic monolith: 3 distinct pictures, pairwise 10 px apart, largest delta 4 -> the
    # split bound is 12.5 px / delta 5.0.
    mono = {("monolith", A, 1): (10, 128), ("monolith", A, 2): (10, 132), ("monolith", A, 3): (10, 130)}
    inside = {("inproc", A, 1): (10, 129), ("inproc", A, 2): (10, 131), ("inproc", A, 3): (10, 133),
              ("spawn", A, 1): (10, 130), ("spawn", A, 2): (10, 132), ("spawn", A, 3): (10, 128)}
    rule_line = "nondeterministic monolith (3 distinct / 3 passes) -> distributional check (ID-P7-62)"
    numbers = {"passes": 3, "distinct": 3, "rule": "distributional", "mono_pairs": 3, "mono_px_max": 10,
               "mono_delta_max": 4, "split_pairs": 18, "split_px_max": 10, "split_delta_max": 5, "within": True}

    def det(v):
        return case(v, A).get("monolith_determinism", {})

    def has(v, needle):
        return any(needle in r for r in case(v, A).get("reasons", []))

    # (a) the strict clause stands when the monolith is bit-identical over its 3 passes.
    cn.window("det-strict", gate3_args={"picture": {("inproc", A, 2): (10, 131)}})
    rc, text, v = run_reducer(cn.root, "det-strict", tree)
    k.expect("bit-identical monolith x3 + a nondeterministic inproc -> %s FAIL (strict clause), GATE3 FAIL" % A,
             case(v, A).get("verdict") == "FAIL" and overall(v) == "FAIL" and rc != 0 and det(v).get("rule") == "strict"
             and has(v, "inproc repeats NOT bit-identical (2 distinct pictures); the same-session monolith is "
                        "bit-identical over 3 passes") and case(v, B).get("verdict") == "PASS",
             (case(v, A).get("reasons"), det(v)))

    # (b) a nondeterministic monolith, the split pictures inside its spread -> PASS, no adjudication.
    cn.window("det-inside", gate3_args={"picture": {**mono, **inside}})
    rc, text, v = run_reducer(cn.root, "det-inside", tree)
    k.expect("nondeterministic monolith (3 distinct / 3 passes) + split inside 1.25x -> %s PASS, GATE3 PASS, exit 0" % A,
             case(v, A).get("verdict") == "PASS" and overall(v) == "PASS" and rc == 0
             and "GATE3 PASS: 36/36 cases PASS\n" in text, (case(v, A).get("reasons"), overall(v), text[-400:]))
    k.expect("...the rule line and the numbers: 10 px / delta 4 over 3 monolith pairs, 10 px / delta 5 over 18 "
             "split pairs, bound 12.50 px / 5.00",
             {key: det(v).get(key) for key in numbers} == numbers and rule_line in text
             and "monolith-vs-monolith max 10 px / delta 4 (3 pairs); split-vs-monolith max 10 px / delta 5 (18 pairs); "
                 "bound 1.25x = 12.50 px / 5.00: within" in text
             and g3(v, "determinism", {}).get("distributional") == [A], (det(v), text[:1500]))
    k.expect("...its cross-arm difference needs no gate3/adjudication.tsv line (decided by the rule)",
             case(v, A).get("cross_arm", {}).get("identical") is False
             and case(v, A).get("cross_arm", {}).get("distributional") is True
             and g3(v, "cross_arm_unadjudicated") == [] and "compared by the ID-P7-62 distributional check" in text,
             case(v, A).get("cross_arm"))
    decoder = det(v).get("decoder", "")
    rc, text, v = run_reducer(cn.root, "det-inside", tree, env={"W2_REDUCE_PNG_DECODER": "pure"})
    k.expect("...the stdlib PNG decoder (W2_REDUCE_PNG_DECODER=pure) gives the same numbers (default decoder: %s)"
             % decoder, {key: det(v).get(key) for key in numbers} == numbers
             and det(v).get("decoder", "").startswith("pure-python") and rc == 0, det(v))

    # (c) beyond 1.25x: in the per-channel delta, then in the pixel count.
    cn.window("det-delta", gate3_args={"picture": {**mono, **inside, ("inproc", A, 3): (10, 134)}})
    rc, text, v = run_reducer(cn.root, "det-delta", tree)
    k.expect("...one split picture at delta 6 > 1.25 x 4 -> %s FAIL, GATE3 FAIL" % A,
             case(v, A).get("verdict") == "FAIL" and overall(v) == "FAIL" and rc != 0
             and det(v).get("split_delta_max") == 6 and det(v).get("within") is False
             and has(v, "split-vs-monolith max per-channel delta 6 > 1.25 x the monolith-vs-monolith max 4 (= 5.00)")
             and not has(v, "differing px"), case(v, A).get("reasons"))
    cn.window("det-px", gate3_args={"picture": {**mono, **inside, ("inproc", A, 3): (13, 131)}})
    rc, text, v = run_reducer(cn.root, "det-px", tree)
    k.expect("...one split picture 13 px off > 1.25 x 10 (delta inside) -> %s FAIL" % A,
             case(v, A).get("verdict") == "FAIL" and rc != 0 and det(v).get("split_px_max") == 13
             and det(v).get("split_delta_max") == 4
             and has(v, "split-vs-monolith max 13 differing px > 1.25 x the monolith-vs-monolith max 10 (= 12.50)")
             and not has(v, "per-channel delta"), (det(v), case(v, A).get("reasons")))

    # (d) clause (1) holds against EVERY monolith reading, not one of them.
    ssim = {("monolith", A, 1): 0.9978, ("monolith", A, 2): 0.998, ("monolith", A, 3): 0.9982, ("spawn", A, 2): 0.99831}
    cn.window("det-ssim", gate3_args={"picture": {**mono, **inside}, "ssim": ssim})
    rc, text, v = run_reducer(cn.root, "det-ssim", tree)
    k.expect("...a spawn ssim 0.00051 from ONE monolith reading (0.00011 from another) -> %s FAIL" % A,
             case(v, A).get("verdict") == "FAIL" and rc != 0 and det(v).get("within") is True
             and case(v, A).get("reasons") == ["spawn rep2 |ssim-monolith|=0.000510 > 0.0005 (max over 3 monolith readings)"],
             case(v, A).get("reasons"))

    # (e) the pre-ID-P7-62 archives (monolith x1, no monolith_repeat= in arms.txt): the rule cannot apply.
    cn.window("det-mono1", gate3_args={"arms": ARMS_MONO1, "arms_txt": ARMS_TXT_MONO1, "picture": inside})
    rc, text, v = run_reducer(cn.root, "det-mono1", tree)
    k.expect("monolith x1 (the earlier 30-gate3.sh) + a nondeterministic split -> %s FAIL naming the >= 3 monolith "
             "passes the rule needs" % A,
             case(v, A).get("verdict") == "FAIL" and overall(v) == "FAIL" and rc != 0 and g3(v, "monolith_repeat") == 1
             and det(v).get("rule") == "strict (< 3 monolith passes)"
             and has(v, "inproc repeats NOT bit-identical (3 distinct pictures); the ID-P7-62 nondeterministic-monolith "
                        "rule needs >= 3 same-session monolith passes, this session has 1")
             and case(v, B).get("verdict") == "PASS", (det(v), case(v, A).get("reasons")))

    # (f) the monolith pair's own bookkeeping: arms.txt monolith_repeat=3 holds a finished pair to 3.
    out = cn.window("det-short")
    shutil.rmtree(out / "gate3/archive/monolith" / (A + "-DirectVulkan") / "repeat-03")
    rc, text, v = run_reducer(cn.root, "det-short", tree)
    k.expect("a finished monolith pair with 2 of arms.txt's monolith_repeat=3 -> %s FAIL" % A,
             case(v, A).get("verdict") == "FAIL" and rc != 0
             and has(v, "monolith: 2 repeat(s) in a finished pair < gate3/arms.txt monolith_repeat=3"), case(v, A))
    (out / "gate3/state/monolith" / (A + ".done")).unlink()
    rc, text, v = run_reducer(cn.root, "det-short", tree)
    k.expect("...the same pair without its .done -> INCOMPLETE (a resume re-runs it)",
             case(v, A).get("verdict") == "INCOMPLETE" and overall(v) == "INCOMPLETE"
             and "monolith: 2/3 repeats" in case(v, A).get("reasons", []), case(v, A).get("reasons"))

    # (g) --extra-monolith: the g3det E1-mono re-reduction shape (x1 archive + passes outside gate3/).
    cn.window("det-extra", gate3_args={"arms": ARMS_MONO1, "arms_txt": ARMS_TXT_MONO1, "picture": inside})
    extra = cn.extra_monolith(cn.root / "extra-unstamped", A, [(10, 132), (10, 130)])
    rc, text, v = run_reducer(cn.root, "det-extra", tree, extra=["%s@%s" % (extra, BOOT)])
    k.expect("x1 archive + 2 extra monolith passes attested @session boot -> the rule applies (3 distinct / 3), "
             "%s PASS, GATE3 PASS-WITH-EXTRA-READINGS, exit 1" % A,
             case(v, A).get("verdict") == "PASS" and det(v).get("rule") == "distributional"
             and overall(v) == "PASS-WITH-EXTRA-READINGS" and rc == 1
             and "GATE3 PASS-WITH-EXTRA-READINGS: 36/36 cases PASS" in text and "NOT a gate verdict" in text
             and "monolith 38/38 repeat(s) (2 of them --extra-monolith)" in text,
             (overall(v), case(v, A).get("reasons"), det(v), text[-600:]))
    rc, text, v = run_reducer(cn.root, "det-extra", tree, extra=[extra])
    s = g3(v, "session", {})
    k.expect("...the same extra passes with no boot_id.txt and no @BOOT_ID -> GATE3 INVALID-SESSION",
             overall(v) == "INVALID-SESSION" and rc != 0
             and any("carry no boot_id" in p and "--extra-monolith" in p for p in s.get("problems", [])), s)
    rc, text, v = run_reducer(cn.root, "det-extra", tree, extra=["%s@%s" % (extra, OTHER_BOOT)])
    k.expect("...attested @another boot -> GATE3 INVALID-SESSION naming it",
             overall(v) == "INVALID-SESSION" and rc != 0
             and any(OTHER_BOOT in p for p in g3(v, "session", {}).get("problems", [])), g3(v, "session"))
    stamped = cn.extra_monolith(cn.root / "extra-stamped", A, [(10, 132), (10, 130)], boot=BOOT)
    rc, text, v = run_reducer(cn.root, "det-extra", tree, extra=[stamped])
    k.expect("...extra passes whose own boot_id.txt is the session's (no @BOOT_ID) -> PASS-WITH-EXTRA-READINGS",
             overall(v) == "PASS-WITH-EXTRA-READINGS" and rc == 1 and case(v, A).get("verdict") == "PASS", overall(v))


def pass_cases(cn, b):
    """$BASE-Pass cases of block b's caselist (not skipped), in caselist order."""
    base = cn.base(b)
    skip = set(read_list(cn.tree / "tools/cts/caselists/p7-skip.txt"))
    return [x for x in read_list(cn.caselist(b)) if x not in skip and base.get(x) == "Pass"]


def dryrun_differences():
    fixture = json.loads(FIXTURE.read_text(encoding="utf-8"))
    return fixture, {b: fixture["blocks"][b]["after_differences"] for b in BLOCKS}


def scenario_rate_gate(k, cn, tree):
    print("[rate-gate] the gate rate alone decides a block (no crash anywhere)")
    flips = pass_cases(cn, "dsa")
    cn.window("rate-1", differences={"dsa": {x: "Fail" for x in flips[:1]}})
    rc, text, v = run_reducer(cn.root, "rate-1", tree)
    d = block(v, "dsa")
    k.expect("1 dsa Pass->Fail: 367/370 = -0.270 pp -> dsa PASS, CTS PASS, window exit 0",
             r3(d.get("delta_pp")) == -0.27 and d.get("verdict") == "PASS" and v.get("cts", {}).get("overall") == "PASS"
             and rc == 0, (d.get("delta_pp"), d.get("verdict"), rc, text[-300:]))
    cn.window("rate-2", gate3=False, differences={"dsa": {x: "Fail" for x in flips[:2]}})
    rc, text, v = run_reducer(cn.root, "rate-2", tree)
    d = block(v, "dsa")
    k.expect("2 dsa Pass->Fail: -0.541 pp < -0.5 pp -> dsa FAIL on the rate alone (0 new crash, 0 L)",
             r3(d.get("delta_pp")) == -0.541 and d.get("verdict") == "FAIL" and d.get("new_crash") == []
             and d.get("lost") == [] and d.get("reasons") == ["delta -0.541 pp < -0.5 pp (Pass/(Pass+Fail+L))"],
             (d.get("delta_pp"), d.get("verdict"), d.get("reasons")))
    cn.window("rate-4", gate3=False, differences={"dsa": {x: "Fail" for x in flips[:4]}})
    rc, text, v = run_reducer(cn.root, "rate-4", tree)
    d = block(v, "dsa")
    k.expect("4 dsa Pass->Fail: AFTER 364/6/0/1/0, -1.081 pp -> dsa FAIL, CTS FAIL, exit != 0",
             [d.get("after", {}).get(x) for x in ("pass", "fail", "ns", "warn", "crash")] == [364, 6, 0, 1, 0]
             and r3(d.get("delta_pp")) == -1.081 and d.get("verdict") == "FAIL" and d.get("new_crash") == []
             and v.get("cts", {}).get("overall") == "FAIL" and "\nCTS FAIL" in text and rc != 0,
             (d.get("after"), d.get("delta_pp"), d.get("verdict"), rc))


def scenario_cts_lost(k, cn, tree):
    print("[cts-lost] a $BASE Pass that AFTER turns NotSupported / another non-Pass, non-Fail status is NON-PASS")
    dsa, ssbo = pass_cases(cn, "dsa"), pass_cases(cn, "ssbo")
    ns = dsa[0]
    cn.window("lost-ns", differences={"dsa": {ns: "NotSupported"}})
    rc, text, v = run_reducer(cn.root, "lost-ns", tree)
    d = block(v, "dsa")
    k.expect("dsa Pass->NS: gate rate 367/(367+2+1) = -0.270 pp, the literal Pass/(Pass+Fail) -0.001 pp as info",
             r3(d.get("delta_pp")) == -0.27 and r3(d.get("delta_pp_literal_info")) == -0.001
             and d.get("after", {}).get("lost") == 1 and r3(100 * d["after"]["rate"]) == 99.189
             and "info  literal Pass/(Pass+Fail): BASE 99.459%  AFTER 99.458%  delta -0.001 pp (not gating)" in text,
             (d.get("delta_pp"), d.get("delta_pp_literal_info"), d.get("after")))
    k.expect("...listed (LOST ... Pass -> NotSupported, UNADJUDICATED), dsa FAIL, CTS FAIL, exit != 0",
             d.get("lost") == [{"case": ns, "after": "NotSupported", "adjudication": None}] and d.get("verdict") == "FAIL"
             and ("LOST       %s  (Pass -> NotSupported)  UNADJUDICATED" % ns) in text
             and any("lost to NotSupported / another non-Pass, non-Fail status with no cts/adjudication.tsv line" in r
                     for r in d.get("reasons", [])) and v["cts"]["overall"] == "FAIL" and rc != 0
             and "WARN dsa: NotSupported $BASE 0 -> AFTER 1" in text, (d.get("lost"), d.get("reasons"), rc))

    reason = "CTS NotSupported: extension probe differs by design (canned)"
    cn.window("lost-ns-adj", differences={"dsa": {ns: "NotSupported"}}, adjudication="%s\t%s\n" % (ns, reason))
    rc, text, v = run_reducer(cn.root, "lost-ns-adj", tree)
    d = block(v, "dsa")
    k.expect("...with a cts/adjudication.tsv line: dsa PASS, CTS PASS, window exit 0",
             d.get("verdict") == "PASS" and v["cts"]["overall"] == "PASS" and rc == 0
             and d.get("lost") == [{"case": ns, "after": "NotSupported", "adjudication": reason}],
             (d.get("verdict"), d.get("reasons"), rc))
    k.expect("...and OUT of the block's rate on both sides: BASE 367/369, AFTER 367/(367+2+0), +0.000 pp, L 0, "
             "printed ADJUDICATED (the literal info still counts it: -0.001 pp; the NS WARN stays)",
             r3(d.get("delta_pp")) == 0.0 and d.get("after", {}).get("lost") == 0
             and r3(100 * d.get("base", {}).get("rate", 0)) == 99.458 and r3(100 * d.get("after", {}).get("rate", 0)) == 99.458
             and d.get("lost_adjudicated") == [ns] and d.get("lost_unadjudicated") == []
             and ("ADJUDICATED %s  (Pass -> NotSupported)  out of the rate; adjudicated: %s" % (ns, reason)) in text
             and r3(d.get("delta_pp_literal_info")) == -0.001 and "WARN dsa: NotSupported $BASE 0 -> AFTER 1" in text,
             (d.get("delta_pp"), d.get("base"), d.get("after"), d.get("lost_adjudicated")))

    # One adjudicated, one not, in one block: only the adjudicated one leaves the rate.
    cn.window("lost-mixed", gate3=False, differences={"dsa": {dsa[0]: "NotSupported", dsa[1]: "NotSupported"}},
              adjudication="%s\t%s\n" % (dsa[0], reason))
    rc, text, v = run_reducer(cn.root, "lost-mixed", tree)
    d = block(v, "dsa")
    k.expect("dsa 2 Pass->NS, 1 adjudicated: L 1, BASE 367/369 vs AFTER 366/(366+2+1) = -0.271 pp; FAIL on the "
             "unadjudicated one, one ADJUDICATED and one LOST line",
             r3(d.get("delta_pp")) == -0.271 and d.get("after", {}).get("lost") == 1 and d.get("verdict") == "FAIL"
             and d.get("lost_unadjudicated") == [dsa[1]] and d.get("lost_adjudicated") == [dsa[0]]
             and not any(r.startswith("delta ") for r in d.get("reasons", []))
             and ("LOST       %s  (Pass -> NotSupported)  UNADJUDICATED" % dsa[1]) in text
             and ("ADJUDICATED %s  (Pass -> NotSupported)" % dsa[0]) in text,
             (d.get("delta_pp"), d.get("after"), d.get("reasons")))

    # An adjudication takes its own case out and nothing else: real Pass->Fail losses still gate.
    cn.window("lost-adj-fails", gate3=False, adjudication="".join("%s\t%s\n" % (x, reason) for x in dsa[:2]),
              differences={"dsa": {dsa[0]: "NotSupported", dsa[1]: "NotSupported", dsa[2]: "Fail", dsa[3]: "Fail"}})
    rc, text, v = run_reducer(cn.root, "lost-adj-fails", tree)
    d = block(v, "dsa")
    k.expect("dsa 2 adjudicated Pass->NS + 2 Pass->Fail: BASE 366/368 vs AFTER 364/368 = -0.543 pp -> FAIL on the rate",
             r3(d.get("delta_pp")) == -0.543 and d.get("verdict") == "FAIL" and d.get("lost_unadjudicated") == []
             and "delta -0.543 pp < -0.5 pp (Pass/(Pass+Fail+L))" in d.get("reasons", []),
             (d.get("delta_pp"), d.get("reasons")))

    cn.window("lost-ns-noreason", gate3=False, differences={"dsa": {ns: "NotSupported"}}, adjudication=ns + "\n")
    rc, text, v = run_reducer(cn.root, "lost-ns-noreason", tree)
    d = block(v, "dsa")
    k.expect("...an adjudication line without a reason adjudicates nothing -> dsa FAIL, WARN on the line",
             d.get("verdict") == "FAIL" and d.get("lost_unadjudicated") == [ns]
             and ("cts/adjudication.tsv:1: %s has no reason" % ns) in text, (d.get("verdict"), text[-300:]))

    cn.window("lost-ssbo-adj", gate3=False, differences={"ssbo": {ssbo[0]: "NotSupported"}},
              adjudication="%s\t%s\n" % (ssbo[0], reason))
    rc, text, v = run_reducer(cn.root, "lost-ssbo-adj", tree)
    s = block(v, "ssbo")
    k.expect("ssbo Pass->NS adjudicated: out of the rate, 123/123 vs 123/123 = +0.000 pp -> ssbo PASS (it was "
             "-0.806 pp FAIL for ever while an adjudicated case stayed NON-PASS)",
             s.get("verdict") == "PASS" and r3(s.get("delta_pp")) == 0.0 and s.get("lost_unadjudicated") == []
             and s.get("lost_adjudicated") == [ssbo[0]] and not any(r.startswith("delta ") for r in s.get("reasons", [])),
             (s.get("delta_pp"), s.get("reasons")))

    cn.window("lost-codex", gate3=False, differences={"ssbo": {x: "NotSupported" for x in ssbo[1:]}})
    rc, text, v = run_reducer(cn.root, "lost-codex", tree)
    s = block(v, "ssbo")
    k.expect("codex's example: 123 of 124 ssbo Passes -> NS, one still passes: literal 100% (+0.000 pp) but gate "
             "rate 1/124 -> FAIL, all 123 listed",
             r3(s.get("delta_pp_literal_info")) == 0.0 and r3(100 * s.get("after", {}).get("literal_rate", 0)) == 100.0
             and r3(100 * s.get("after", {}).get("rate", 1)) == 0.806 and s.get("verdict") == "FAIL"
             and len(s.get("lost", [])) == 123 and text.count("(Pass -> NotSupported)  UNADJUDICATED") == 123,
             (s.get("delta_pp_literal_info"), s.get("after"), s.get("verdict")))

    cn.window("lost-warn", gate3=False, differences={"dsa": {ns: "CompatibilityWarning"}})
    rc, text, v = run_reducer(cn.root, "lost-warn", tree)
    d = block(v, "dsa")
    k.expect("dsa Pass->CompatibilityWarning (any non-Pass, non-Fail status) -> an L case, dsa FAIL",
             d.get("lost") == [{"case": ns, "after": "CompatibilityWarning", "adjudication": None}]
             and d.get("verdict") == "FAIL" and r3(d.get("delta_pp")) == -0.27, (d.get("lost"), d.get("verdict")))

    cn.window("lost-crash-adj", gate3=False, differences={"dsa": {ns: "Crash"}}, adjudication="%s\t%s\n" % (ns, reason))
    rc, text, v = run_reducer(cn.root, "lost-crash-adj", tree)
    d = block(v, "dsa")
    k.expect("dsa Pass->Crash adjudicated -> the L case is adjudicated (out of the rate, +0.000 pp), but the NEW "
             "crash still makes dsa FAIL",
             d.get("verdict") == "FAIL" and d.get("new_crash") == [ns] and d.get("lost_unadjudicated") == []
             and r3(d.get("delta_pp")) == 0.0 and d.get("reasons", [None])[0] == "1 NEW crash(es)",
             (d.get("verdict"), d.get("delta_pp"), d.get("reasons")))


def scenario_vacuous(k, cn, tree):
    print("[vacuous] a block with nothing to compare is never PASS")
    _, diffs = dryrun_differences()
    cn.window("gone", differences=diffs, copy=False, named=lambda b: GONE % b)
    rc, text, v = run_reducer(cn.root, "gone", tree)
    k.expect("caselist.path names a caselist that is gone (no copy), dry-run AFTER data -> every block MISSING, "
             "CTS INCOMPLETE, exit != 0",
             rc != 0 and v.get("cts", {}).get("overall") == "INCOMPLETE" and "CTS PASS" not in text
             and all(block(v, b).get("verdict") == "MISSING" and "is gone" in " ".join(block(v, b).get("reasons", []))
                     for b in BLOCKS), text)
    cn.window("gone-copy", gate3=False, differences=diffs, named=lambda b: GONE % b)
    rc, text, v = run_reducer(cn.root, "gone-copy", tree)
    d = block(v, "dsa")
    k.expect("...with the window's caselist.txt copy: the real reading (dsa -0.541 pp, 2 new crashes, CTS FAIL)",
             r3(d.get("delta_pp")) == -0.541 and d.get("caselist", "").endswith("runs/dsa/caselist.txt")
             and v.get("cts", {}).get("overall") == "FAIL", (d.get("delta_pp"), d.get("caselist"), v.get("cts", {}).get("overall")))
    cn.window("no-caselist", gate3=False, copy=False, named=lambda b: "")
    rc, text, v = run_reducer(cn.root, "no-caselist", tree)
    k.expect("no caselist.txt and no caselist.path -> every block MISSING ('unknown'), CTS INCOMPLETE",
             rc != 0 and v.get("cts", {}).get("overall") == "INCOMPLETE"
             and all("unknown" in " ".join(block(v, b).get("reasons", [])) for b in BLOCKS), text)
    out = cn.window("empty-caselist", gate3=False)
    write(out / "cts/runs/ssbo/caselist.txt", "# nothing asked\n")
    rc, text, v = run_reducer(cn.root, "empty-caselist", tree)
    k.expect("an empty caselist -> ssbo MISSING ('holds no case'), CTS INCOMPLETE",
             block(v, "ssbo").get("verdict") == "MISSING" and "holds no case" in " ".join(block(v, "ssbo").get("reasons", []))
             and v.get("cts", {}).get("overall") == "INCOMPLETE", block(v, "ssbo"))
    ns = {b: {x: "NotSupported" for x in cn.base(b)} for b in BLOCKS}
    cn.window("all-ns", differences=ns)
    rc, text, v = run_reducer(cn.root, "all-ns", tree)
    k.expect("every AFTER result NotSupported -> every block FAIL ('AFTER has no Pass/Fail'), gate rate 0, every "
             "$BASE Pass an L case, CTS FAIL, exit != 0",
             rc != 0 and v.get("cts", {}).get("overall") == "FAIL" and "CTS PASS" not in text
             and all(block(v, b).get("verdict") == "FAIL" and block(v, b)["after"]["rate"] == 0.0
                     and block(v, b)["after"]["lost"] == block(v, b)["base"]["pass"]
                     and any("AFTER has no Pass/Fail" in r for r in block(v, b).get("reasons", [])) for b in BLOCKS), text)
    k.expect("...and a WARN on each block's NotSupported count", all(("WARN %s: NotSupported $BASE" % b) in text for b in BLOCKS), text)


def scenario_not_a_gate(k, cn, tree):
    print("[not-a-gate] a pass that is not the gate's own reading never exits 0")
    cn.window("subset-g3", gate3_args={"cases": CASES[:2]})
    rc, text, v = run_reducer(cn.root, "subset-g3", tree)
    k.expect("2 of the canonical 36, all passing -> GATE3 PASS-SUBSET, exit != 0",
             overall(v) == "PASS-SUBSET" and "GATE3 PASS-SUBSET: 2/2 cases PASS" in text
             and "2 of the canonical 36 case(s)" in text
             and "WINDOW2 GATE3 PASS-SUBSET | CTS PASS -> exit 1" in text and rc != 0, text[-500:])
    cn.window("not-clean", gate3_args={"reboot_clean": False})
    rc, text, v = run_reducer(cn.root, "not-clean", tree)
    k.expect("36/36 in a session that is not reboot-clean -> GATE3 PASS-NOT-REBOOT-CLEAN, exit != 0",
             overall(v) == "PASS-NOT-REBOOT-CLEAN" and rc != 0 and "session not reboot-clean" in text, text[-500:])
    first20 = read_list(cn.caselist("dsa"))[:20]
    cn.window("subset-cts", lists={"dsa": first20})
    rc, text, v = run_reducer(cn.root, "subset-cts", tree)
    k.expect("a --limit 20 dsa block, all passing -> CTS PASS-SUBSET (dsa marked), GATE3 PASS, exit != 0",
             v.get("cts", {}).get("overall") == "PASS-SUBSET" and block(v, "dsa").get("subset") is True
             and block(v, "dsa").get("cases") == 20 and overall(v) == "PASS" and rc != 0
             and "SUBSET run: dsa" in text, text[-500:])


def scenario_warnings(k, cn, tree):
    print("[warnings] like-for-like rates are reported, not gated")
    extra = "KHR-GL46.texture_selftest.not_in_base"
    tex = read_list(cn.caselist("texture")) + [extra]
    cn.window("warn", gate3=False, lists={"texture": tex}, differences={"texture": {extra: "Fail"}})
    rc, text, v = run_reducer(cn.root, "warn", tree)
    t = block(v, "texture")
    k.expect("a case $BASE lacks (AFTER Fail) stays out of both rates: texture delta 0.000, PASS, compared = cases - 1",
             r3(t.get("delta_pp")) == 0.0 and t.get("verdict") == "PASS" and t.get("compared") == t.get("cases", 0) - 1
             and t.get("missing_in_base") == [extra], (t.get("delta_pp"), t.get("verdict"), t.get("compared"), t.get("cases")))
    k.expect("...with a WARN line naming it", "WARN texture: 1 case(s) have no $BASE result (e.g. %s)" % extra in text, text)


def scenario_check_block(k, cn, tree):
    print("[check-block] 50-cts-after.sh's completeness test before a block's .done")
    out = cn.window("cb", gate3=False)
    c = out / "cts"
    rc, text = run_check_block(c / "runs/dsa", c / "report-dsa.json")
    k.expect("complete dsa block -> 0", rc == 0 and "complete: 371/371" in text, (rc, text))
    write(c / "runs/ssbo/unrun.txt", read_list(cn.caselist("ssbo"))[-1] + "\n")
    rc, text = run_check_block(c / "runs/ssbo", c / "report-ssbo.json")
    k.expect("unrun.txt non-empty -> 3", rc == 3 and "unrun.txt 1" in text, (rc, text))
    report = json.loads((c / "report-texture.json").read_text(encoding="utf-8"))
    report["results"].pop(pass_cases(cn, "texture")[0])
    write(c / "report-texture.json", json.dumps(report))
    rc, text = run_check_block(c / "runs/texture", c / "report-texture.json")
    k.expect("a case without a result -> 3", rc == 3 and "1 case(s) without an AFTER result" in text, (rc, text))
    (c / "runs/shader-image/caselist.txt").unlink()
    write(c / "runs/shader-image/caselist.path", GONE % "shader-image" + "\n")
    rc, text = run_check_block(c / "runs/shader-image", c / "report-shader-image.json")
    k.expect("the caselist gone -> 3", rc == 3 and "is gone" in text, (rc, text))
    write(c / "report-packed-pixels.json", "")
    rc, text = run_check_block(c / "runs/packed-pixels", c / "report-packed-pixels.json")
    k.expect("an empty report -> 3", rc == 3 and "is empty" in text, (rc, text))


def check_dryrun_numbers(k, v, text, label):
    dsa, si, tex = block(v, "dsa"), block(v, "shader-image"), block(v, "texture")
    k.expect("%s: dsa gate delta -0.541 pp (renderbuffers_storage Pass->Crash is an L case)" % label,
             r3(dsa.get("delta_pp")) == -0.541, dsa.get("delta_pp"))
    k.expect("%s: dsa BASE 368/(368+2) = 99.459%%, AFTER 366/(366+3+1) = 98.919%%, literal 366/(366+3) = 99.187%%" % label,
             r3(100 * dsa["base"]["rate"]) == 99.459 and r3(100 * dsa["after"]["rate"]) == 98.919
             and r3(100 * dsa["after"]["literal_rate"]) == 99.187 if dsa.get("base") else False, dsa)
    k.expect("%s: dsa literal Pass/(Pass+Fail) -0.272 pp kept as info" % label,
             r3(dsa.get("delta_pp_literal_info")) == -0.272 and "(-0.272)" in text
             and "info  literal Pass/(Pass+Fail): BASE 99.459%  AFTER 99.187%  delta -0.272 pp (not gating)" in text,
             dsa.get("delta_pp_literal_info"))
    k.expect("%s: dsa pre-ruling delta -0.539 pp kept as info (info line 99.191%% -> 98.652%%)" % label,
             r3(dsa.get("delta_pp_results_info")) == -0.539
             and "info  pre-ruling Pass/(results-NS): BASE 99.191%  AFTER 98.652%  delta -0.539 pp (not gating)" in text,
             dsa.get("delta_pp_results_info"))
    k.expect("%s: dsa FAIL on the new crash, the rate and the unadjudicated loss" % label,
             dsa.get("verdict") == "FAIL" and dsa.get("reasons") == [
                 "1 NEW crash(es)", "delta -0.541 pp < -0.5 pp (Pass/(Pass+Fail+L))",
                 "1 $BASE-Pass case(s) lost to NotSupported / another non-Pass, non-Fail status with no "
                 "cts/adjudication.tsv line (%s)" % CODEX_RULING]
             and dsa.get("new_crash") == [DSA_STORAGE]
             and dsa.get("lost") == [{"case": DSA_STORAGE, "after": "Crash", "adjudication": None}], dsa.get("reasons"))
    k.expect("%s: dsa counts P/F/NS/W/X 366/3/0/1/1, L 1" % label,
             [dsa.get("after", {}).get(x) for x in ("pass", "fail", "ns", "warn", "crash", "lost")] == [366, 3, 0, 1, 1, 1])
    k.expect("%s: shader-image +4.284 pp (L 0), FAIL on new crash incomplete_textures" % label,
             r3(si.get("delta_pp")) == 4.284 and si.get("verdict") == "FAIL" and si.get("new_crash") == [SI_INCOMPLETE]
             and si.get("lost") == [], (si.get("delta_pp"), si.get("verdict"), si.get("new_crash")))
    k.expect("%s: texture +0.960 pp PASS" % label, r3(tex.get("delta_pp")) == 0.96 and tex.get("verdict") == "PASS",
             (tex.get("delta_pp"), tex.get("verdict")))
    k.expect("%s: ssbo, packed-pixels 0.000 pp PASS" % label,
             all(r3(block(v, b).get("delta_pp")) == 0.0 and block(v, b).get("verdict") == "PASS"
                 for b in ("ssbo", "packed-pixels")))
    k.expect("%s: CTS FAIL" % label, v.get("cts", {}).get("overall") == "FAIL" and "\nCTS FAIL" in text)
    k.expect("%s: printed '-0.541' and the LOST line" % label, "-0.541" in text
             and ("LOST       %s  (Pass -> Crash)  UNADJUDICATED" % DSA_STORAGE) in text)


def scenario_dryrun(k, cn, tree, dry_dir):
    print("[dry-run] the gate rate on the pre-14e1c8b9 dry-run data")
    fixture, diffs = dryrun_differences()
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
    k.expect("canned: no gate3/ -> GATE3 NOT-RUN, exit != 0", rc != 0 and overall(v) == "NOT-RUN")
    if dry_dir and (Path(dry_dir) / "cts").is_dir():
        dry = Path(dry_dir)
        rc2, text2, v2 = run_reducer(dry.parent, dry.name, tree, json_out=cn.root / "real-dry-run-verdict.json")
        check_dryrun_numbers(k, v2, text2, "real %s" % dry.name)
    else:
        print("  (skip) real dry-run output not found at %s" % dry_dir)


def main():
    global REDUCER, CASES, A, B
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tools", help="MobileGL tree (default: the tree holding this file, else $W2_TOOLS_OVERRIDE)")
    ap.add_argument("--dry-run-dir", default=str(Path.home() / "w7/logs/devprep/w2/pre-14e1c8b9"))
    ap.add_argument("--reducer", default=str(REDUCER), help="the 60-reduce.py under test (default: beside this file)")
    ap.add_argument("--keep", action="store_true", help="keep the canned directories")
    args = ap.parse_args()
    REDUCER = Path(args.reducer).resolve()
    tree = find_tree(args.tools)
    if tree is None:
        print("no MobileGL tree with tools/trace_replay/{compare_actuals.py,trace_cases.json} and %s: pass --tools"
              % CTS_BASE_REL, file=sys.stderr)
        return 2
    sys.path.insert(0, str(tree / "tools" / "trace_replay"))
    sys.dont_write_bytecode = True  # the tree is only read (the working copy falls back to ~/w7/pipe)
    import compare_actuals as ca
    split_dv, CASES = canonical_cases()
    if len(split_dv) != 39 or len(CASES) != 36 or not all(x in split_dv for x in EXCLUDED) or OPENRA not in CASES:
        print("%s's trace_cases.json no longer gives CONTRACT-P7 7.1's 39 - 3 = 36 (got %d, %d)"
              % (tree, len(split_dv), len(CASES)), file=sys.stderr)
        return 2
    A, B = [c for c in CASES if c != OPENRA][:2]
    root = Path(tempfile.mkdtemp(prefix="w2-reduce-selftest-"))
    print("60-reduce self-test: reducer %s, tree %s, canned windows in %s" % (REDUCER, tree, root))
    k = Checks()
    cn = Canned(root, tree, ca)
    try:
        scenario_control(k, cn, tree)
        scenario_missing_block(k, cn, tree)
        scenario_boot_id(k, cn, tree)
        scenario_monolith_arm(k, cn, tree)
        scenario_repeats(k, cn, tree)
        scenario_arms(k, cn, tree)
        scenario_case_set(k, cn, tree)
        scenario_cross_arm(k, cn, tree)
        scenario_unfinished(k, cn, tree)
        scenario_determinism(k, cn, tree)
        scenario_rate_gate(k, cn, tree)
        scenario_cts_lost(k, cn, tree)
        scenario_vacuous(k, cn, tree)
        scenario_not_a_gate(k, cn, tree)
        scenario_warnings(k, cn, tree)
        scenario_check_block(k, cn, tree)
        scenario_dryrun(k, cn, tree, args.dry_run_dir)
    finally:
        if not args.keep:
            shutil.rmtree(root, ignore_errors=True)
    print("60-reduce self-test: %d/%d checks passed%s" % (
        k.total - len(k.failed), k.total, "" if not k.failed else "; FAILED: " + "; ".join(k.failed)))
    return 0 if not k.failed else 1


if __name__ == "__main__":
    sys.exit(main())
