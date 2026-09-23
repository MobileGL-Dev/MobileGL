#!/usr/bin/env python3
"""60-reduce.py <stamp> [--logroot DIR] [--tools TREE] [--json OUT] [--extra-monolith DIR[@BOOT_ID] ...]
   60-reduce.py --check-block RUN_DIR REPORT_JSON

Device window 2 verdicts, read from the window's own output directory (<logroot>/<stamp>).

GATE 3 (CONTRACT-P7 7.1/7.2), per case of gate3/cases.txt:
  * THE CONTRACT'S CONSTANTS, NOT THE RUNNER'S OWN RECORDS (ruling: integrator 2026-09-23 after
    codex closeout review). gate3/arms.txt says what 30-gate3.sh was asked to run, not what the
    gate needs, so the reducer holds the run to CONTRACT-P7 7.1/7.2 itself:
      - REPEATS: inproc and spawn need >= 3 archived repeats on every case. arms.txt repeat < 3,
        or a finished pair (state .done) with fewer than 3 archived repeats, is GATE3 FAIL-REPEATS
        whatever arms.txt says;
      - ARMS: monolith, inproc, spawn AND inproc-ra0 on every case. An arm with no archive that
        arms.txt's arms= does not name (the run was started without it) is GATE3 FAIL-ARMS; a named
        arm not reached yet, or a case an arm lacks, makes that case INCOMPLETE;
      - CASE SET: gate3/cases.txt must EQUAL the canonical denominator, computed the way
        30-gate3.sh computes it - the tools tree's trace_cases.json CI split cases whose
        ci_backends carry DirectVulkan (39) - minus CONTRACT-P7 7.1's three exclusions, which are
        this file's constants (CONTRACT_EXCLUDED), not gate3/excluded.txt. A case outside it, a
        case listed twice, or a manifest that no longer gives 39 / 36 is GATE3 FAIL-CASESET; a
        proper subset (a --cases run) is PASS-SUBSET at best - never merely "count 36";
  * ONE SESSION: every gate3/state/<arm>/<case>.done and every archived repeat's boot_id.txt, of
    every arm, carries one boot_id, equal to session/boot-id.txt. A second boot_id, a record that
    carries none, or no session/boot-id.txt makes the whole gate GATE3 INVALID-SESSION, whatever
    the pictures say (the gate is a same-session reading);
  * the monolith arm (x gate3/arms.txt's monolith_repeat: 3 from ID-P7-62 on; an arms.txt without
    the key is the earlier script's x1) is the same-session control: every pass must itself pass,
    and its role logs must show no 'Config: IPC' / MOBILEGL_TRANSPORT line (the arm really was the
    monolith); a finished monolith pair with fewer passes than monolith_repeat is that case FAIL;
  * inproc and spawn: the pair finished (state .done), every expected repeat present and
    error-free, each repeat's arm proof (transport-proof.json passed), the device's own pass
    verdict (ssim >= case threshold), ALL repeats OF THE ARM bit-identical (actual PNG SHA-256;
    within-arm identity is the gate), and |ssim - m| <= 0.0005 on every repeat for EVERY monolith
    reading m of the session; a repeat whose logcat is byte-identical to the previous repeat's is
    flagged STALE;
  * NONDETERMINISTIC MONOLITH (ID-P7-62, g3det VERDICT.md section 2): the bit-identity clause
    presupposes a bit-reproducible reference. A case whose same-session monolith pictures are NOT
    all bit-identical over >= 3 passes has none, so for it the split arms' bit-identity clause is
    replaced by the distributional check: (1) the |ssim - m| clause above, for every monolith
    reading m; (2) NEAREST MONOLITH (integrator ruling on ID-P7-62, replacing the first form's
    1.25 x the max over all pairs): px(a, b) = the pixels where any RGBA channel differs, delta(a, b)
    = the largest per-channel difference; the nearest of a pass among candidate monolith passes is
    the one with the fewest differing px (a tie: the smaller delta), and its delta is measured to
    that same pass. D_mm / Delta_mm = the max over the monolith passes of px / delta to the nearest
    OTHER monolith pass (leave-one-out, over passes: a bit-identical twin is 0 / 0); every split
    pass s (inproc and spawn repeats) must hold px(s, nearest) <= 1.25 x D_mm + 32 AND
    delta(s, nearest) <= Delta_mm + 1. The pixels are read from the archived actual PNGs (numpy +
    PIL; without them, or with W2_REDUCE_PNG_DECODER=pure, compare_actuals' stdlib decoder - the
    same numbers, slower). Such a case prints 'nondeterministic monolith (N distinct / M passes)
    -> distributional check' and its numbers, and its cross-arm difference is decided by (2), not
    by gate3/adjudication.tsv. A case whose monolith is bit-identical, or that has fewer than 3
    monolith passes (the pre-ID-P7-62 x1 archives), keeps the strict clause;
  * --extra-monolith DIR[@BOOT_ID] (repeatable; an adjudication re-reduction, never the gate):
    adds the passes under DIR/<case>-DirectVulkan/repeat-NN (a run_android_retrace_local.py
    --archive-dir tree) to the case's monolith readings. Each must be the session's: a repeat's
    boot_id.txt, else the BOOT_ID the operator attests for DIR from the run's own boot_id checks;
    none, or another boot, is INVALID-SESSION. A reading that uses one is at best
    GATE3 PASS-WITH-EXTRA-READINGS, never PASS;
  * OpenRA: 1.000000 and mismatch 0 on every repeat of both split arms AND on monolith;
  * cross-arm picture identity (inproc vs spawn vs monolith) is not a per-case gate (ruling:
    integrator 2026-09-23, ID-P7-59): a mismatch prints a WARN line and leaves the case's verdict
    alone. But it is never waved through silently (ruling: integrator 2026-09-23 after codex
    closeout review): every mismatching case must be listed in gate3/adjudication.tsv
    ('<case><TAB><reason>'), else an otherwise PASS gate is GATE3 PASS-NEEDS-ADJUDICATION (exit 1).
    Each mismatch is listed with the archived mismatchPixels (vs golden) of every arm and the
    direct pixel counts between the arms' first repeats;
  * inproc-ra0 (RUN_AHEAD=0): required on every case (one archived repeat with its .done) and its
    role log must prove run-ahead=0 (else that case FAIL: it is not the control arm); its picture
    and ssim are recorded, never gating;
  * a reading that passes but is not the gate's - a proper subset of the canonical 36, a session
    that is not reboot-clean, or one that used --extra-monolith - is GATE3 PASS-SUBSET /
    PASS-NOT-REBOOT-CLEAN / PASS-WITH-EXTRA-READINGS, never PASS.
BSL STATS: per arm, maps-line / VmRSS / VmHWM peaks and the wbuf[] gauges (information).
CTS AFTER (CONTRACT-P7 7.3), per block vs device-window-1/CTS-base/report-<block>.json, over the
  cases the AFTER run was asked to run (so a --limit subset is compared like for like):
  * all five blocks (shader-image ssbo dsa texture packed-pixels) are REQUIRED, each with a
    finished run (cts/runs/<block>/.done), a non-empty cts/report-<block>.json with results, and
    a readable, non-empty caselist (runs/<block>/caselist.txt, the copy 50-cts-after.sh keeps;
    else the file caselist.path names); a missing one makes the block MISSING and CTS INCOMPLETE -
    a block of 0 cases is never a reading. The UBO block (GTF-GL46 uniform_buffer_object) is
    recorded as 'unrun (not in this glcts)': the device glcts carries no GTF module (ID-P7-16);
  * rate = Pass / (Pass + Fail + L), L = the cases BASE passed that AFTER turned NotSupported or
    any other non-Pass, non-Fail status (a warning, a crash): a lost supported case is NON-PASS, it
    does not leave the denominator (ruling: integrator 2026-09-23 after codex closeout review -
    under the contract's literal Pass / (Pass + Fail), 123 of 124 BASE-Pass cases turning
    NotSupported would still read 100%). NotSupported, warnings and crashes that were not a BASE
    Pass stay out of both rates. gate: delta >= -0.5 pp on this rate. Both rates are taken over the
    cases that have a BASE and an AFTER result (a case BASE lacks prints a WARN, stays out of both
    rates and still counts for new crashes). AFTER without a single Pass/Fail/L where BASE has some
    is FAIL (the rate cannot be formed); BASE without one is INCOMPLETE;
  * every L case is listed (Pass -> its AFTER status), and a block with one that
    cts/adjudication.tsv ('<case><TAB><reason>') does not list is FAIL. An ADJUDICATED one is
    printed 'ADJUDICATED' and leaves that block's rate on both sides - no longer an L case in AFTER's
    denominator, one Pass fewer in BASE's (ruling: integrator 2026-09-23 after the int3 critic: else
    a single ruled-legitimate loss in the 124-case ssbo block was -0.806 pp that nothing could clear);
    the literal and pre-ruling info rates still count it;
  * crashes (Crash/Timeout/InternalError/ResourceError/DeviceHang/Incomplete, plus hung.txt) are
    counted in their own column and are non-Pass; one that BASE does not already have is a NEW
    crash, a hard red of its own that no adjudication waives (a Fail that became a Crash is a new
    crash; a Pass that became a Crash is a new crash AND an L case);
  * a block whose cases are not the tools tree's full p7-<block>-gl46.txt (a --limit run) makes an
    all-PASS CTS reading 'PASS-SUBSET', never PASS;
  * information only, never gating: the contract's literal Pass / (Pass + Fail) of BASE and AFTER
    and its delta, the pre-ruling Pass / (results - NotSupported) and its delta, and a WARN when
    the NotSupported count moved.
Exit status: 0 only when GATE 3 is PASS (the canonical 36, >= 3 repeats, four arms, reboot-clean,
one session, every cross-arm mismatch adjudicated) and CTS is PASS (five full blocks); 1 otherwise
(a section that did not run, a PASS-SUBSET / PASS-NOT-REBOOT-CLEAN / PASS-NEEDS-ADJUDICATION /
PASS-WITH-EXTRA-READINGS reading, INCOMPLETE / INVALID-SESSION, FAIL-REPEATS / FAIL-ARMS /
FAIL-CASESET, or FAIL); 2 on bad input.
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
# ID-P7-62 (g3det VERDICT.md section 2): a case whose same-session monolith is not bit-identical
# over >= MONOLITH_RULE_PASSES passes is judged by the distributional check, whose clause (2) holds
# every split pass to its nearest monolith pass within SPREAD_FACTOR x D_mm + PX_HEADROOM px and
# Delta_mm + DELTA_HEADROOM per channel (D_mm / Delta_mm: the monolith's leave-one-out nearest
# spread; integrator ruling on ID-P7-62).
MONOLITH_RULE_PASSES = 3
SPREAD_FACTOR = 1.25
PX_HEADROOM = 32
DELTA_HEADROOM = 1
RULING_62 = "ID-P7-62"
UUID = re.compile(r"^(.+)@([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})$")
# CONTRACT-P7 7.1/7.2's constants: the gate is held to these, never to what gate3/arms.txt,
# gate3/cases.txt or gate3/excluded.txt record about the run (ruling: integrator 2026-09-23 after
# codex closeout review).
GATE3_DENOMINATOR = 36
CI_SPLIT_DIRECTVULKAN = 39
CONTRACT_EXCLUDED = ("minecraft-1.21.1-neoforge-create-indirect-in-world",   # OQ-17
                     "minecraft-1.21.11-main-menu",                          # UBO offset alignment
                     "minecraft-1.21.4-fabric-iris-photon-v1.3b-in-world")   # VK_ERROR_UNKNOWN
CONTRACT_REPEATS = 3
REQUIRED_ARMS = ("monolith", "inproc", "spawn", "inproc-ra0")
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
CODEX_RULING = "ruling: integrator 2026-09-23 after codex closeout review"


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


def read_adjudication(path):
    """gate3/adjudication.tsv, cts/adjudication.tsv: '<case><TAB><reason>' per line ('#' comments
    and blank lines skipped; a case name holds no whitespace) -> ({case: reason}, problems). A line
    without a reason adjudicates nothing and is reported."""
    found, problems = {}, []
    if not path.is_file():
        return found, problems
    for number, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
        text = line.strip()
        if not text or text.startswith("#"):
            continue
        parts = text.split(None, 1)
        reason = parts[1].strip() if len(parts) > 1 else ""
        if not reason:
            problems.append("%s/%s:%d: %s has no reason: it adjudicates nothing"
                            % (path.parent.name, path.name, number, parts[0]))
            continue
        found.setdefault(parts[0], reason)
    return found, problems


# ------------------------------------------------------------------------------------------------
# gate 3
# ------------------------------------------------------------------------------------------------
def gate3_session(out, g, extras=()):
    """One boot session for every record of every arm (ruling c), and of every --extra-monolith
    reading (its repeat's boot_id.txt, else the BOOT_ID attested for its directory)."""
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
    for extra in extras:
        for row in extra["rows"]:
            where = "--extra-monolith %s/%s/repeat-%02d" % (extra["dir"], row["arm"], row["repeat"])
            rec = read_record(Path(row["dir"]) / "boot_id.txt") or {}
            note(rec.get("boot_id") or extra["boot"], where)
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


def canonical_gate3_cases():
    """CONTRACT-P7 7.1's denominator, computed the way 30-gate3.sh computes gate3/cases.txt: the
    tools tree's trace_cases.json CI split cases whose ci_backends carry DirectVulkan, minus the
    contract's three exclusions (CONTRACT_EXCLUDED, never the run's own gate3/excluded.txt).
    -> (cases in manifest order, or None; problems). A problem means the manifest no longer gives
    the contract's 39 / 36, so no run can be scored against it."""
    try:
        from trace_cases import ci_backends, ci_trace_cases, load_trace_cases, split_trace_cases
        split_dv = [c["name"] for c in split_trace_cases(ci_trace_cases(load_trace_cases()))
                    if "DirectVulkan" in ci_backends(c)]
    except Exception as error:  # an unreadable manifest is a finding of the reading, not a crash
        return None, ["the tools tree's trace_cases.json cannot be read (%s: %s)" % (type(error).__name__, error)]
    problems = []
    if len(split_dv) != CI_SPLIT_DIRECTVULKAN:
        problems.append("trace_cases.json gives %d CI split DirectVulkan case(s), CONTRACT-P7 7.1 names %d"
                        % (len(split_dv), CI_SPLIT_DIRECTVULKAN))
    absent = [x for x in CONTRACT_EXCLUDED if x not in split_dv]
    if absent:
        problems.append("CONTRACT-P7 7.1 exclusion(s) not among them: " + ", ".join(absent))
    canonical = [c for c in split_dv if c not in CONTRACT_EXCLUDED]
    if len(set(canonical)) != GATE3_DENOMINATOR:
        problems.append("the canonical denominator is %d case(s), CONTRACT-P7 7.1 says %d"
                        % (len(set(canonical)), GATE3_DENOMINATOR))
    return canonical, problems


def px_between(ca, left, right):
    """Exact RGB pixel-inequality count between two archived pictures (compare_actuals'
    mismatch_pixels, whole frame), or why it cannot be counted; None when a picture is missing."""
    if not left or not right:
        return None
    try:
        a, b = ca.read_png_rgba(left), ca.read_png_rgba(right)
        return ca.mismatch_pixels(a, b, *ca.resolve_crop(a, b))
    except Exception as error:  # ImageError (a size mismatch), OSError, zlib: reported, not fatal
        return "n/a (%s)" % error


# ---- ID-P7-62: the pixel spread of a nondeterministic monolith, and the split arms' against it ----
class NumpyPictures:
    """Archived actual PNGs as int16 RGBA arrays (numpy + PIL)."""
    name = "numpy+PIL"

    def __init__(self):
        import numpy  # noqa: F401 - both imported here so a missing one selects the fallback
        from PIL import Image
        self.np, self.image = numpy, Image

    def load(self, path):
        with self.image.open(path) as picture:
            return self.np.asarray(picture.convert("RGBA"), dtype=self.np.int16)

    def diff(self, a, b):
        """-> (pixels where any RGBA channel differs, the largest per-channel delta)."""
        if a.shape != b.shape:
            raise ValueError("picture sizes differ: %s vs %s" % (a.shape, b.shape))
        d = self.np.abs(a - b)
        return int((d.max(axis=2) > 0).sum()), int(d.max())


class PurePictures:
    """The same numbers with compare_actuals' stdlib PNG decoder (no numpy / PIL)."""
    name = "pure-python (compare_actuals.read_png_rgba)"

    def __init__(self, ca):
        self.ca = ca

    def load(self, path):
        return self.ca.read_png_rgba(path)

    def diff(self, a, b):
        if (a.width, a.height) != (b.width, b.height):
            raise ValueError("picture sizes differ: %dx%d vs %dx%d" % (a.width, a.height, b.width, b.height))
        stride, px, delta = a.width * 4, 0, 0
        for y in range(a.height):
            ra, rb = a.pixels[y * stride:(y + 1) * stride], b.pixels[y * stride:(y + 1) * stride]
            if ra == rb:
                continue
            for i in range(0, stride, 4):
                if ra[i:i + 4] != rb[i:i + 4]:
                    px += 1
                    delta = max(delta, max(abs(ra[i + c] - rb[i + c]) for c in range(4)))
        return px, delta


def picture_reader(ca):
    """numpy + PIL when importable (and W2_REDUCE_PNG_DECODER != pure), else the stdlib decoder."""
    if os.environ.get("W2_REDUCE_PNG_DECODER", "") != "pure":
        try:
            return NumpyPictures()
        except ImportError:
            pass
    return PurePictures(ca)


def nearest_spread(reader, mono, split):
    """ID-P7-62 clause (2), nearest-monolith form. mono, split: [(label, sha, path)] over every
    reading (a picture repeated in several readings is decoded and compared once; an identical pair
    is 0 px / delta 0). The nearest of a pass among candidates is the one with the fewest differing
    px, a tie broken by the smaller delta (then the first listed); its delta is measured to that
    same pass. -> {mono_nearest: [{pass, nearest, px, delta}] (leave-one-out: the nearest OTHER
    monolith pass), mono_nearest_px_max (D_mm), mono_nearest_delta_max (Delta_mm), split_nearest:
    [...] (the nearest monolith pass), split_nearest_px_max, split_nearest_delta_max}."""
    cache, memo = {}, {}

    def load(sha, path):
        if sha not in cache:
            cache[sha] = reader.load(path)
        return cache[sha]

    def diff(a, b):
        (_, sa, pa), (_, sb, pb) = a, b
        if sa == sb:
            return 0, 0
        key = (sa, sb) if sa < sb else (sb, sa)
        if key not in memo:
            memo[key] = reader.diff(load(sa, pa), load(sb, pb))
        return memo[key]

    def nearest(item, candidates):
        px, delta, _, other = min((diff(item, c) + (i, c) for i, c in enumerate(candidates)),
                                  key=lambda t: t[:3])
        return {"pass": item[0], "nearest": other[0], "px": px, "delta": delta}
    mono_nearest = [nearest(m, mono[:i] + mono[i + 1:]) for i, m in enumerate(mono)]
    split_nearest = [nearest(s, mono) for s in split]
    return {"mono_nearest": mono_nearest,
            "mono_nearest_px_max": max(n["px"] for n in mono_nearest),
            "mono_nearest_delta_max": max(n["delta"] for n in mono_nearest),
            "split_nearest": split_nearest,
            "split_nearest_px_max": max((n["px"] for n in split_nearest), default=0),
            "split_nearest_delta_max": max((n["delta"] for n in split_nearest), default=0)}


def extra_monolith(ca, specs):
    """--extra-monolith DIR[@BOOT_ID] -> [{dir, boot, rows}] (rows: compare_actuals.summarize rows,
    arm = <case>-DirectVulkan), or raises ValueError on a directory that is not an archive."""
    extras = []
    for spec in specs or []:
        m = UUID.match(spec)
        path, boot = (m.group(1), m.group(2)) if m else (spec, None)
        d = Path(path).expanduser()
        if not d.is_dir():
            raise ValueError("--extra-monolith %s: not a directory" % path)
        rows = ca.summarize(d)["rows"]
        extras.append({"dir": str(d), "boot": boot, "rows": rows})
    return extras


def gate3(out, ca, extras=()):
    g = out / "gate3"
    cases_file = g / "cases.txt"
    if not cases_file.is_file():
        return None
    listed = [c for c in cases_file.read_text().split() if c]
    cases = list(dict.fromkeys(listed))
    excluded = (g / "excluded.txt").read_text().split() if (g / "excluded.txt").is_file() else []
    meta = read_kv(g / "arms.txt")
    try:
        repeat = int(meta.get("repeat", str(CONTRACT_REPEATS)))
    except ValueError:
        repeat = 0
    # 30-gate3.sh records monolith_repeat= from ID-P7-62 on (the monolith runs x --repeat); an
    # arms.txt without the key was written by the earlier script, which ran the monolith x1.
    try:
        mono_repeat = int(meta.get("monolith_repeat", "1"))
    except ValueError:
        mono_repeat = 0
    planned = meta.get("arms", "").split()
    arms_present = [a for a in REQUIRED_ARMS if (g / "archive" / a).is_dir()]
    summaries = {a: ca.summarize(g / "archive" / a) for a in arms_present}
    session = gate3_session(out, g, extras)
    identity = {"monolith_repeats": 0, "monolith_proven": 0, "monolith_extra": 0, "split_repeats": 0,
                "split_proofs_passed": 0}
    adjudicated, adjudication_problems = read_adjudication(g / "adjudication.tsv")
    reader = None  # the PNG reader of the distributional check, made on first use
    extra_rows = {}
    for extra in extras:
        for row in extra["rows"]:
            extra_rows.setdefault(row["arm"], []).append(dict(row, extra=extra["dir"]))

    # ---- the contract's constants (ruling: integrator 2026-09-23 after codex closeout review) ----
    contract = {"repeats": [], "arms": [], "cases": []}
    if repeat < CONTRACT_REPEATS:
        contract["repeats"].append("gate3/arms.txt repeat=%s < CONTRACT-P7 7.2's %d: 30-gate3.sh froze that count "
                                   "with the case list, a resume keeps it" % (meta.get("repeat", "?"), CONTRACT_REPEATS))
    for arm in REQUIRED_ARMS:
        if arm not in arms_present and arm not in planned:
            contract["arms"].append("arm %s has no archive and gate3/arms.txt's arms=[%s] does not name it: the run "
                                    "was started without it; CONTRACT-P7 7.2 needs %s on every case"
                                    % (arm, " ".join(planned), " + ".join(REQUIRED_ARMS)))
    canonical, canonical_problems = canonical_gate3_cases()
    missing = []
    if canonical is None or canonical_problems:
        contract["cases"].append("no canonical denominator to hold gate3/cases.txt to: " + "; ".join(canonical_problems))
    else:
        outside = [c for c in cases if c not in set(canonical)]
        missing = [c for c in canonical if c not in set(cases)]
        if outside:
            contract["cases"].append("%d case(s) outside CONTRACT-P7 7.1's denominator (%d CI split DirectVulkan - "
                                     "the %d exclusions): %s%s" % (
                                         len(outside), CI_SPLIT_DIRECTVULKAN, len(CONTRACT_EXCLUDED), ", ".join(outside),
                                         "; canonical case(s) missing: " + ", ".join(missing) if missing else ""))
    twice = sorted(c for c, n in Counter(listed).items() if n > 1)
    if twice:
        contract["cases"].append("case(s) listed more than once in gate3/cases.txt: " + ", ".join(twice))
    short = []

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

    def picture(row):
        return str(Path(row["dir"]) / row["actual"]) if row and row.get("actual") else None

    def mono_label(row):  # a monolith reading, short: "monolith rep2", "monolith +E1-mono rep3"
        return "monolith %srep%d" % ("+%s " % Path(row["extra"]).name if row.get("extra") else "", row["repeat"])

    def ra0_proof(row):
        p = Path(row["dir"]) / "mobilegl.log"
        if not p.is_file():
            return "no-log"
        lines = re.findall(r"Config: IPC[^\r\n]*", p.read_text(encoding="utf-8", errors="replace"))
        if lines and all(re.search(r"\brun-ahead=0\b", line) for line in lines):
            return "run-ahead=0"
        return "NOT-PROVEN(" + ";".join(lines)[:120] + ")"

    def not_run(arm):
        return "%s: not run%s" % (arm, "" if arm in planned else " (and gate3/arms.txt does not name the arm)")

    results = []
    for case in cases:
        reasons, incomplete = [], []
        entry = {"case": case, "arms": {}}
        # The monolith readings: the gate's own x monolith_repeat, then any --extra-monolith pass.
        gate_mono = rows("monolith", case)
        extra_mono = extra_rows.get(case + "-DirectVulkan", [])
        if "monolith" in summaries and not finished("monolith", case):
            incomplete.append("monolith: no state .done (pair unfinished; 30-gate3.sh retries it)")
        if not gate_mono:
            incomplete.append(not_run("monolith"))
        elif len(gate_mono) != mono_repeat:
            if finished("monolith", case) and len(gate_mono) < mono_repeat:
                reasons.append("monolith: %d repeat(s) in a finished pair < gate3/arms.txt monolith_repeat=%d"
                               % (len(gate_mono), mono_repeat))
            else:
                incomplete.append("monolith: %d/%d repeats" % (len(gate_mono), mono_repeat))
        mono_rows, not_monolith = [], []
        for m in gate_mono + extra_mono:
            where = "monolith rep%d" % m["repeat"] if not m.get("extra") else \
                "monolith (--extra-monolith %s) rep%d" % (m["extra"], m["repeat"])
            if m.get("error"):
                incomplete.append("%s: %s" % (where, m["error"]))
                continue
            mono_rows.append(m)
            not_mono = monolith_identity(m)
            if not_mono:
                not_monolith.append("%s: %s" % (where, not_mono))
            identity["monolith_repeats"] += 1
            identity["monolith_proven"] += not_mono is None
            identity["monolith_extra"] += bool(m.get("extra"))
            m_ssim = m.get("ssim_vs_golden")
            if not_mono:
                reasons.append("%s is NOT proven monolith (%s)" % (where, not_mono))
            if m.get("passed") is not True:
                reasons.append("%s: the monolith control did not pass (ssim %s)" % (where, m_ssim))
            if case == OPENRA and not (m.get("mismatch_pixels_vs_golden") == 0 and (m_ssim or 0) >= 0.9999995):
                reasons.append("OpenRA %s not 1.000000/0 (%s/%s)" % (where, m_ssim, m.get("mismatch_pixels_vs_golden")))
        mono_row = mono_rows[0] if mono_rows else None
        mono_ssims = [m["ssim_vs_golden"] for m in mono_rows if isinstance(m.get("ssim_vs_golden"), (int, float))]
        mono_shas = [m["actual_sha256"] for m in mono_rows if m.get("actual_sha256")]
        if mono_rows:
            entry["arms"]["monolith"] = {
                "repeats": len(gate_mono), "extra": len(extra_mono), "ssim": [m.get("ssim_vs_golden") for m in mono_rows],
                "sha": sorted(set(mono_shas)), "mismatch": [m.get("mismatch_pixels_vs_golden") for m in mono_rows],
                "passed": all(m.get("passed") is True for m in mono_rows), "identity": not_monolith or "monolith"}
        # ID-P7-62: is the same-session reference itself bit-reproducible?
        det = entry["monolith_determinism"] = {"passes": len(mono_shas), "distinct": len(set(mono_shas))}
        distributional = det["passes"] >= MONOLITH_RULE_PASSES and det["distinct"] > 1
        if distributional:
            det["rule"] = "distributional"
        elif det["passes"] >= MONOLITH_RULE_PASSES:
            det["rule"] = "strict"
        else:
            det["rule"] = "strict (< %d monolith passes)" % MONOLITH_RULE_PASSES
        split_rows = {}
        for arm in SPLIT_ARMS:
            rs = split_rows[arm] = rows(arm, case)
            a = {"repeats": len(rs), "ssim": [r.get("ssim_vs_golden") for r in rs],
                 "sha": sorted({r.get("actual_sha256") for r in rs if r.get("actual_sha256")}), "proofs_passed": 0}
            entry["arms"][arm] = a
            if arm not in summaries:
                incomplete.append(not_run(arm))
                continue
            if not finished(arm, case):
                incomplete.append("%s: no state .done (pair unfinished; 30-gate3.sh retries it)" % arm)
            if len(rs) != repeat:
                incomplete.append("%s: %d/%d repeats" % (arm, len(rs), repeat))
            if len(rs) < CONTRACT_REPEATS and finished(arm, case):
                # The pair is final (30-gate3.sh wrote its .done): a resume will not add a repeat.
                reasons.append("%s: %d repeat(s) in a finished pair < CONTRACT-P7 7.2's %d" % (arm, len(rs), CONTRACT_REPEATS))
                short.append("%s/%s (%d)" % (arm, case, len(rs)))
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
                if mono_ssims and r.get("ssim_vs_golden") is not None:
                    # Against EVERY monolith reading of the session (ID-P7-62 clause (1)); with one
                    # reading, or a bit-identical monolith, this is the contract's single difference.
                    d = max(abs(r["ssim_vs_golden"] - m) for m in mono_ssims)
                    if d > TOL:
                        reasons.append("%s rep%d |ssim-monolith|=%.6f > %.4f%s" % (
                            arm, r["repeat"], d, TOL,
                            "" if len(set(mono_ssims)) == 1 else " (max over %d monolith readings)" % len(mono_ssims)))
                if case == OPENRA and not (r.get("mismatch_pixels_vs_golden") == 0 and (r.get("ssim_vs_golden") or 0) >= 0.9999995):
                    reasons.append("OpenRA %s rep%d not 1.000000/0 (%s/%s)" % (arm, r["repeat"], r.get("ssim_vs_golden"), r.get("mismatch_pixels_vs_golden")))
            shas = [r.get("actual_sha256") for r in rs if r.get("actual_sha256")]
            if len(set(shas)) > 1 and not distributional:
                # The strict clause; under ID-P7-62 the distributional check below replaces it.
                if det["passes"] >= MONOLITH_RULE_PASSES:
                    why = "; the same-session monolith is bit-identical over %d passes" % det["passes"]
                else:
                    why = ("; the %s nondeterministic-monolith rule needs >= %d same-session monolith passes, this "
                           "session has %d" % (RULING_62, MONOLITH_RULE_PASSES, det["passes"]))
                reasons.append("%s repeats NOT bit-identical (%d distinct pictures)%s" % (arm, len(set(shas)), why))
            lc = [logcat_sha(r) for r in rs]
            for i in range(1, len(lc)):
                if lc[i] and lc[i] == lc[i - 1]:
                    reasons.append("%s rep%d STALE (logcat identical to rep%d)" % (arm, rs[i]["repeat"], rs[i - 1]["repeat"]))
        if distributional:
            # ID-P7-62 clause (2), nearest-monolith form: every split pass lies within the monolith's
            # own leave-one-out nearest spread of its nearest monolith pass.
            split_valid = [("%s rep%d" % (arm, r["repeat"]), r) for arm in SPLIT_ARMS for r in split_rows[arm]
                           if not r.get("error") and r.get("actual")]
            split_ssims = [r["ssim_vs_golden"] for _, r in split_valid if isinstance(r.get("ssim_vs_golden"), (int, float))]
            det["max_dssim"] = max((abs(s - m) for s in split_ssims for m in mono_ssims), default=None)
            reader = reader or picture_reader(ca)
            det["decoder"] = reader.name
            try:
                det.update(nearest_spread(
                    reader, [(mono_label(m), m["actual_sha256"], picture(m)) for m in mono_rows if m.get("actual")],
                    [(label, r["actual_sha256"], picture(r)) for label, r in split_valid]))
            except Exception as error:  # an unreadable / mis-sized picture: no measurement, no pass
                det["error"] = "%s: %s" % (type(error).__name__, error)
                reasons.append("%s distributional check cannot be measured (%s)" % (RULING_62, det["error"]))
            else:
                d_mm, delta_mm = det["mono_nearest_px_max"], det["mono_nearest_delta_max"]
                det["px_bound"] = SPREAD_FACTOR * d_mm + PX_HEADROOM
                det["delta_bound"] = delta_mm + DELTA_HEADROOM
                for n in det["split_nearest"]:
                    n["within"] = n["px"] <= det["px_bound"] and n["delta"] <= det["delta_bound"]
                    if n["px"] > det["px_bound"]:
                        reasons.append("%s: %s is %d differing px from its nearest monolith pass (%s) > %.2f x D_mm %d "
                                       "+ %d (= %.2f)" % (RULING_62, n["pass"], n["px"], n["nearest"], SPREAD_FACTOR,
                                                          d_mm, PX_HEADROOM, det["px_bound"]))
                    if n["delta"] > det["delta_bound"]:
                        reasons.append("%s: %s's per-channel delta to its nearest monolith pass (%s, %d px) is %d > "
                                       "Delta_mm %d + %d (= %d)" % (RULING_62, n["pass"], n["nearest"], n["px"],
                                                                    n["delta"], delta_mm, DELTA_HEADROOM,
                                                                    det["delta_bound"]))
                det["within"] = all(n["within"] for n in det["split_nearest"])
        # The RUN_AHEAD=0 control arm: required on every case (CONTRACT-P7 7.2 "RUN_AHEAD=0 对照臂一遍
        # 记录"), and it must be that arm; its picture is recorded, never gating.
        ra0 = rows("inproc-ra0", case)
        if "inproc-ra0" in summaries and not finished("inproc-ra0", case):
            incomplete.append("inproc-ra0: no state .done (pair unfinished; 30-gate3.sh retries it)")
        if not ra0:
            incomplete.append(not_run("inproc-ra0"))
        else:
            r = ra0[0]
            entry["arms"]["inproc-ra0"] = {"ssim": r.get("ssim_vs_golden"), "sha": r.get("actual_sha256"),
                                           "passed": r.get("passed"), "proof": ra0_proof(r), "error": r.get("error")}
            if r.get("error"):
                incomplete.append("inproc-ra0 rep%d: %s" % (r["repeat"], r["error"]))
            elif entry["arms"]["inproc-ra0"]["proof"] != "run-ahead=0":
                reasons.append("inproc-ra0 is not proven the RUN_AHEAD=0 arm (%s)" % entry["arms"]["inproc-ra0"]["proof"])
        # Cross-arm identity: not a per-case gate (ruling d), but every mismatch needs an adjudication -
        # except under ID-P7-62, where the monolith has no one picture to be identical to and clause
        # (2) above is the cross-arm judgment.
        ip = set(entry["arms"].get("inproc", {}).get("sha", []))
        sp = set(entry["arms"].get("spawn", {}).get("sha", []))
        mp = set(mono_shas)
        entry["inproc_eq_spawn"] = bool(ip) and ip == sp
        entry["split_eq_monolith"] = bool(ip) and ip == sp == mp
        x = entry["cross_arm"] = {"monolith": sorted(mp), "inproc": sorted(ip), "spawn": sorted(sp),
                                  "identical": (entry["split_eq_monolith"] if (ip and sp and mp) else None),
                                  "distributional": distributional}
        if x["identical"] is False and not distributional:
            first = {arm: picture(split_rows[arm][0]) if split_rows[arm] else None for arm in SPLIT_ARMS}
            x["px_vs_golden"] = {"monolith": [m.get("mismatch_pixels_vs_golden") for m in mono_rows],
                                 "inproc": [r.get("mismatch_pixels_vs_golden") for r in split_rows["inproc"]],
                                 "spawn": [r.get("mismatch_pixels_vs_golden") for r in split_rows["spawn"]]}
            x["px_direct"] = {"inproc-monolith": px_between(ca, first["inproc"], picture(mono_row)),
                              "spawn-monolith": px_between(ca, first["spawn"], picture(mono_row)),
                              "inproc-spawn": px_between(ca, first["inproc"], first["spawn"])}
            x["adjudication"] = adjudicated.get(case)
        entry["verdict"] = "INCOMPLETE" if incomplete else ("FAIL" if reasons else "PASS")
        entry["reasons"] = incomplete + reasons
        results.append(entry)
    if short:
        contract["repeats"].append("%d finished inproc/spawn pair(s) archived fewer than CONTRACT-P7 7.2's %d repeats "
                                   "(arms.txt repeat=%s): %s%s" % (len(short), CONTRACT_REPEATS, meta.get("repeat", "?"),
                                                                    ", ".join(short[:4]), ", ..." if len(short) > 4 else ""))
    mismatched = [e["case"] for e in results if e["cross_arm"]["identical"] is False
                  and not e["cross_arm"]["distributional"]]
    unadjudicated = [c for c in mismatched if c not in adjudicated]
    stale = sorted(c for c in adjudicated if c not in set(mismatched))
    verdicts = [e["verdict"] for e in results]
    # A reading that is not the gate's own (CONTRACT-P7 7.2: the canonical 36, one reboot-clean
    # session, every cross-arm mismatch adjudicated) may pass, but never as PASS: exit 0 is
    # reserved for the gate verdict.
    not_a_gate = []
    if missing and not contract["cases"]:
        not_a_gate.append("%d of the canonical %d case(s) (missing %d, e.g. %s): a subset run is NOT a gate verdict"
                          % (len(cases), GATE3_DENOMINATOR, len(missing), missing[0]))
    if not session["reboot_clean"].startswith("reboot-clean:"):
        not_a_gate.append("session not reboot-clean: NOT a gate verdict")
    if unadjudicated:
        not_a_gate.append("%d cross-arm mismatch(es) without a gate3/adjudication.tsv line (%s): %s%s" % (
            len(unadjudicated), CODEX_RULING, ", ".join(unadjudicated[:3]), ", ..." if len(unadjudicated) > 3 else ""))
    if extras:
        not_a_gate.append("%d extra monolith reading(s) from outside gate3/archive (--extra-monolith %s): an "
                          "adjudication re-reduction, NOT a gate verdict" % (
                              sum(len(x["rows"]) for x in extras), ", ".join(x["dir"] for x in extras)))
    if not session["valid"]:
        overall = "INVALID-SESSION"
    elif contract["repeats"]:
        overall = "FAIL-REPEATS"
    elif contract["arms"]:
        overall = "FAIL-ARMS"
    elif contract["cases"]:
        overall = "FAIL-CASESET"
    elif verdicts and all(v == "PASS" for v in verdicts):
        overall = "PASS"
        if missing:
            overall = "PASS-SUBSET"
        elif not session["reboot_clean"].startswith("reboot-clean:"):
            overall = "PASS-NOT-REBOOT-CLEAN"
        elif unadjudicated:
            overall = "PASS-NEEDS-ADJUDICATION"
        elif extras:
            overall = "PASS-WITH-EXTRA-READINGS"
    elif "FAIL" in verdicts:
        overall = "FAIL"
    else:
        overall = "INCOMPLETE"
    rule_of = [(e["case"], e["monolith_determinism"]["rule"]) for e in results]
    return {"cases": results, "excluded": excluded, "repeat": repeat, "monolith_repeat": mono_repeat,
            "determinism": {"distributional": [c for c, r in rule_of if r == "distributional"],
                            "strict": [c for c, r in rule_of if r == "strict"],
                            "strict_under_3_passes": [c for c, r in rule_of if r.startswith("strict (")]},
            "extra_monolith": [{"dir": x["dir"], "boot_attested": x["boot"], "repeats": len(x["rows"]),
                                "cases": sorted({r["arm"] for r in x["rows"]})} for x in extras],
            "denominator": len(cases),
            "overall": overall, "not_a_gate": not_a_gate, "arms_present": arms_present, "arms_planned": planned,
            "contract": contract, "canonical": canonical, "canonical_problems": canonical_problems,
            "cross_arm_mismatched": mismatched, "cross_arm_unadjudicated": unadjudicated,
            "adjudication": {"file": str(g / "adjudication.tsv"), "cases": adjudicated,
                             "problems": adjudication_problems, "stale": stale},
            "session": session, "identity": identity}


def print_gate3(g3):
    print("== GATE 3 (DirectVulkan x pbuffer; split repeat=%d, contract >= %d; monolith repeat=%d; "
          "tol |ssim-monolith| <= %.4f) ==" % (g3["repeat"], CONTRACT_REPEATS, g3["monolith_repeat"], TOL))
    s = g3["session"]
    print("-- session: boot_id %s; %d record(s) stamped; %s" % (s["session_boot_id"] or "?", s["records"],
                                                               s["reboot_clean"] or "no session/reboot-clean.txt"))
    for x in g3["extra_monolith"]:
        print("-- extra monolith readings (--extra-monolith, NOT the gate's own archive): %s: %d repeat(s) over %d "
              "case(s); unstamped repeats attested boot_id %s" % (x["dir"], x["repeats"], len(x["cases"]),
                                                                  x["boot_attested"] or "(none)"))
    for problem in s["problems"]:
        print("   ! INVALID-SESSION: " + problem)
    c = g3["contract"]
    held = not any(c.values())
    print("-- contract (CONTRACT-P7 7.1/7.2; %s): inproc/spawn >= %d repeats per case, arms %s on every case, "
          "case set == the canonical %d (%d CI split DirectVulkan - %d exclusions): %s"
          % (CODEX_RULING, CONTRACT_REPEATS, "+".join(REQUIRED_ARMS), GATE3_DENOMINATOR, CI_SPLIT_DIRECTVULKAN,
             len(CONTRACT_EXCLUDED), "held" if held else "VIOLATED"))
    for key, label in (("repeats", "FAIL-REPEATS"), ("arms", "FAIL-ARMS"), ("cases", "FAIL-CASESET")):
        for problem in c[key]:
            print("   ! %s: %s" % (label, problem))
    if sorted(g3["excluded"]) != sorted(CONTRACT_EXCLUDED):
        print("WARN gate3/excluded.txt [%s] is not CONTRACT-P7 7.1's [%s] (the case set above is held to the contract's)"
              % (" ".join(g3["excluded"]), " ".join(CONTRACT_EXCLUDED)))
    w = max([len(e["case"]) for e in g3["cases"]] + [4])
    print("%-*s  %-10s  %-24s  %-24s  %-24s  %-11s  %s" % (w, "case", "verdict", "monolith(min..max) #sha",
                                                          "inproc(min..max) #sha", "spawn(min..max) #sha", "ra0", "i==s==m"))
    for e in g3["cases"]:
        def cell(arm):
            a = e["arms"].get(arm)
            if not a or not a.get("ssim"):
                return "-"
            vals = [v for v in a["ssim"] if isinstance(v, (int, float))]
            if not vals:
                return "err"
            return "%.6f..%.6f #%d" % (min(vals), max(vals), len(a["sha"]))
        ra0 = e["arms"].get("inproc-ra0", {}).get("ssim")
        print("%-*s  %-10s  %-24s  %-24s  %-24s  %-11s  %s" % (
            w, e["case"], e["verdict"], cell("monolith"),
            cell("inproc"), cell("spawn"), "%.6f" % ra0 if isinstance(ra0, (int, float)) else "-",
            "yes" if e["split_eq_monolith"] else ("i==s" if e["inproc_eq_spawn"] else "no")))
        det = e["monolith_determinism"]
        if det["rule"] == "distributional":
            print("%-*s    nondeterministic monolith (%d distinct / %d passes) -> distributional check (%s)" % (
                w, "", det["distinct"], det["passes"], RULING_62))
            if "error" not in det:
                worst = max(det["split_nearest"], key=lambda n: (n["px"], n["delta"]), default=None)
                print("%-*s      monolith leave-one-out nearest: D_mm %d px / Delta_mm %d (%d passes); split-to-nearest-"
                      "monolith max %d px / delta %d (%d passes%s); bound %.2f x %d + %d = %.2f px / %d + %d = %d: %s; "
                      "max |ssim - monolith| %s; decoder %s" % (
                          w, "", det["mono_nearest_px_max"], det["mono_nearest_delta_max"], len(det["mono_nearest"]),
                          det["split_nearest_px_max"], det["split_nearest_delta_max"], len(det["split_nearest"]),
                          "; most px: %s -> %s %d px / delta %d" % (worst["pass"], worst["nearest"], worst["px"],
                                                                    worst["delta"]) if worst else "",
                          SPREAD_FACTOR, det["mono_nearest_px_max"], PX_HEADROOM, det["px_bound"],
                          det["mono_nearest_delta_max"], DELTA_HEADROOM, det["delta_bound"],
                          "within" if det["within"] else "OUTSIDE",
                          "-" if det["max_dssim"] is None else "%.1e" % det["max_dssim"], det["decoder"]))
        for reason in e["reasons"]:
            print("%-*s    ! %s" % (w, "", reason))
    dt = g3["determinism"]
    print("-- monolith determinism (%s; same-session monolith passes): %d case(s) bit-identical over >= %d passes "
          "(strict clause), %d nondeterministic -> distributional check%s, %d with < %d passes (strict clause; the "
          "rule cannot apply)" % (RULING_62, len(dt["strict"]), MONOLITH_RULE_PASSES, len(dt["distributional"]),
                                  " (%s)" % ", ".join(dt["distributional"]) if dt["distributional"] else "",
                                  len(dt["strict_under_3_passes"]), MONOLITH_RULE_PASSES))
    ra0s = [(e["case"], e["arms"]["inproc-ra0"]) for e in g3["cases"] if "inproc-ra0" in e["arms"]]
    print("-- RUN_AHEAD=0 control (required on every case, picture recorded, not gating): %d/%d case(s); proofs: %s" % (
        len(ra0s), len(g3["cases"]), ", ".join(sorted({r["proof"] for _, r in ra0s})) or "-"))
    i = g3["identity"]
    print("-- arm identity: monolith %d/%d repeat(s)%s with no Config: IPC / MOBILEGL_TRANSPORT line; "
          "inproc+spawn transport-proof passed %d/%d" % (
              i["monolith_proven"], i["monolith_repeats"],
              " (%d of them --extra-monolith)" % i["monolith_extra"] if i["monolith_extra"] else "",
              i["split_proofs_passed"], i["split_repeats"]))
    judged = [e for e in g3["cases"] if e["cross_arm"]["identical"] is not None]
    adj = g3["adjudication"]
    distributional = [e for e in judged if e["cross_arm"]["distributional"] and not e["cross_arm"]["identical"]]
    print("-- cross-arm picture identity (NOT a per-case gate, %s; every mismatch needs a gate3/adjudication.tsv "
          "line, %s): %d/%d case(s) inproc==spawn==monolith; %d mismatch(es), %d unadjudicated; %d nondeterministic-"
          "monolith case(s) compared by the %s distributional check instead" % (
              RULING, CODEX_RULING, sum(e["cross_arm"]["identical"] for e in judged), len(judged),
              len(g3["cross_arm_mismatched"]), len(g3["cross_arm_unadjudicated"]), len(distributional), RULING_62))

    def px(v):
        if isinstance(v, list):
            nums = [n for n in v if isinstance(n, int)]
            return "%d..%d" % (min(nums), max(nums)) if nums else "-"
        return "-" if v is None else str(v)
    for e in distributional:
        print("     note: cross-arm %s: nondeterministic monolith (%d distinct pictures): the arms are compared by the "
              "%s distributional check above, not by gate3/adjudication.tsv" % (
                  e["case"], e["monolith_determinism"]["distinct"], RULING_62))
    for e in judged:
        if not e["cross_arm"]["identical"] and not e["cross_arm"]["distributional"]:
            x = e["cross_arm"]
            g, d = x["px_vs_golden"], x["px_direct"]
            print("WARN cross-arm %s: monolith %s | inproc %s | spawn %s" % (
                e["case"], ",".join(v[:12] for v in x["monolith"]) or "-", ",".join(v[:12] for v in x["inproc"]) or "-",
                ",".join(v[:12] for v in x["spawn"]) or "-"))
            print("     px!=golden (archived mismatchPixels) monolith %s, inproc %s, spawn %s; px between the arms' "
                  "first repeats: inproc-monolith %s, spawn-monolith %s, inproc-spawn %s" % (
                      px(g["monolith"]), px(g["inproc"]), px(g["spawn"]), px(d["inproc-monolith"]),
                      px(d["spawn-monolith"]), px(d["inproc-spawn"])))
            print("     " + ("adjudicated: " + x["adjudication"] if x.get("adjudication")
                             else "UNADJUDICATED: no gate3/adjudication.tsv line"))
    for problem in adj["problems"]:
        print("WARN " + problem)
    by_rule = set(g3["determinism"]["distributional"])
    agree = [c for c in adj["stale"] if c not in by_rule]
    if agree:
        print("     note: gate3/adjudication.tsv names case(s) whose arms agree (nothing to adjudicate): "
              + ", ".join(agree))
    if len(agree) != len(adj["stale"]):
        print("     note: gate3/adjudication.tsv names nondeterministic-monolith case(s) the %s distributional check "
              "decides (the line is not used): %s" % (RULING_62, ", ".join(c for c in adj["stale"] if c in by_rule)))
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


def rate(statuses, lost=0, adjudicated=0):
    """The gate's block rate = Pass / (Pass + Fail + lost), `lost` = the UNADJUDICATED $BASE-Pass
    cases this side turned NotSupported or another non-Pass, non-Fail status (a warning, a crash): a
    lost supported case is NON-PASS, it does not leave the denominator (ruling: integrator 2026-09-23
    after codex closeout review). An ADJUDICATED one leaves the block's rate on both sides (ruling:
    integrator 2026-09-23 after the int3 critic): not an L case here, and on the $BASE side
    `adjudicated` of its Passes are taken out. Other NotSupported / warnings / crashes stay out of the
    denominator; crashes get their own count. Information only: CONTRACT-P7 7.3's literal Pass /
    (Pass + Fail) as `literal_rate` and the pre-ruling Pass / (results - NotSupported) as
    `results_rate`, both over every compared case."""
    c = Counter(statuses)
    p, f, ns = c["Pass"], c["Fail"], c["NotSupported"]
    x = sum(c[s] for s in HARD)
    n = len(statuses)
    g = p - adjudicated
    return {"pass": p, "fail": f, "ns": ns, "crash": x, "warn": n - p - f - ns - x, "results": n, "lost": lost,
            "adjudicated": adjudicated,
            "rate": (g / (g + f + lost)) if (g + f + lost) else None,
            "literal_rate": (p / (p + f)) if (p + f) else None,
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


def cts_block(c, b, base_dir, canon_dir, adjudicated=None):
    adjudicated = adjudicated or {}
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
    # A $BASE Pass that AFTER turned NotSupported / a warning / a crash: NON-PASS in the gate rate,
    # listed, and FAIL unless adjudicated (ruling: integrator 2026-09-23 after codex closeout review).
    # An adjudicated one is out of the block's rate on BOTH sides (ruling: integrator 2026-09-23 after
    # the int3 critic - else one ruled-legitimate loss in a 124-case block was -0.806 pp for ever).
    lost = sorted(x for x in both if base[x] == "Pass" and after[x] not in ("Pass", "Fail"))
    unadjudicated = [x for x in lost if x not in adjudicated]
    waived = len(lost) - len(unadjudicated)
    ra, rb = rate([after[x] for x in both], lost=len(unadjudicated)), rate([base[x] for x in both], adjudicated=waived)

    def pp(key):
        return None if ra[key] is None or rb[key] is None else (ra[key] - rb[key]) * 100.0
    delta, delta_literal, delta_results = pp("rate"), pp("literal_rate"), pp("results_rate")
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
    elif ra["literal_rate"] is None:
        fail.append("AFTER has no Pass/Fail result (NS %d, W %d, X %d of %d): the literal Pass/(Pass+Fail) cannot be "
                    "formed where $BASE's is %.3f%%; %d $BASE Pass(es) lost" % (
                        ra["ns"], ra["warn"], ra["crash"], ra["results"], 100 * rb["rate"], len(lost)))
    if delta is not None and delta < -CTS_TOL_PP:
        fail.append("delta %.3f pp < -%.1f pp (Pass/(Pass+Fail+L))" % (delta, CTS_TOL_PP))
    if delta is not None and delta > CTS_TOL_PP:
        notes.append("note: AFTER is %.3f pp ABOVE BASE (not a regression; review the fixed list)" % delta)
    if unadjudicated:
        fail.append("%d $BASE-Pass case(s) lost to NotSupported / another non-Pass, non-Fail status with no "
                    "cts/adjudication.tsv line (%s)" % (len(unadjudicated), CODEX_RULING))
    if waived:
        notes.append("note: %d $BASE-Pass case(s) lost to NotSupported / another non-Pass, non-Fail status and "
                     "adjudicated in cts/adjudication.tsv: out of the block's rate, BASE and AFTER" % waived)
    if missing_base:
        warnings.append("%d case(s) have no $BASE result (e.g. %s): left out of both rates; a crash among them "
                        "still counts as NEW" % (len(missing_base), missing_base[0]))
    if ra["ns"] != rb["ns"]:
        warnings.append("NotSupported $BASE %d -> AFTER %d (a $BASE Pass among them is an L case: NON-PASS in the "
                        "rate, listed below)" % (rb["ns"], ra["ns"]))
    canonical = canon_dir / ("p7-%s-gl46.txt" % b)
    subset = set(read_list(src)) != set(read_list(canonical))
    verdict = "FAIL" if fail else ("INCOMPLETE" if incomplete else "PASS")
    return {"block": b, "cases": len(universe), "compared": len(both), "caselist": str(src),
            "after": ra, "base": rb, "delta_pp": delta, "delta_pp_literal_info": delta_literal,
            "delta_pp_results_info": delta_results,
            "new_crash": new_crash, "regressed": regress, "fixed": fixed, "missing_in_base": missing_base,
            "lost": [{"case": x, "after": after[x], "adjudication": adjudicated.get(x)} for x in lost],
            "lost_unadjudicated": unadjudicated,
            "lost_adjudicated": [x for x in lost if x in adjudicated],
            "unrun": len(unrun), "hung": hung, "verdict": verdict, "reasons": incomplete + fail + notes,
            "warnings": warnings, "subset": subset}


def cts(out, base_dir, canon_dir):
    c = out / "cts"
    adjudicated, adjudication_problems = read_adjudication(c / "adjudication.tsv")
    blocks = [cts_block(c, b, base_dir, canon_dir, adjudicated) for b in CTS_BLOCKS]
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
    lost = {x["case"] for b in blocks for x in b.get("lost", [])}
    return {"blocks": blocks, "overall": overall, "ubo": dict(UBO_BLOCK), "ignored_reports": extra,
            "incomplete_blocks": [b["block"] for b in blocks if b["verdict"] not in ("PASS", "FAIL")],
            "adjudication": {"file": str(c / "adjudication.tsv"), "cases": adjudicated,
                             "problems": adjudication_problems, "stale": sorted(x for x in adjudicated if x not in lost)}}


def print_cts(c):
    print("== CTS AFTER (inproc x DirectVulkan) vs BASE (monolith x DirectVulkan, p7w1) ==")
    print("gate (CONTRACT-P7 7.3 + %s):" % CODEX_RULING)
    print("  rate = Pass/(Pass+Fail+L), L = the $BASE-Pass cases AFTER turned NotSupported or another non-Pass,")
    print("  non-Fail status (a lost supported case is NON-PASS); other NS, warnings W and crashes X are not in the")
    print("  denominator. Per block: delta >= -%.1f pp, 0 NEW crash, and every L case adjudicated in" % CTS_TOL_PP)
    print("  cts/adjudication.tsv (else FAIL); an adjudicated one (A) is out of the block's rate, BASE and AFTER.")
    print("  All five blocks required. '(literal)' = the contract's literal Pass/(Pass+Fail) delta and 'info' the")
    print("  pre-ruling Pass/(results-NS), both over every compared case: printed for comparison, never gating.")
    print("%-14s %6s  %-20s %8s  %-20s %4s %3s  %8s  %9s  %9s  %5s  %4s  %5s  %s" % (
        "block", "cases", "BASE P/F/NS/W/X", "rate", "AFTER P/F/NS/W/X", "L", "A", "rate", "delta pp", "(literal)",
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
        print("%-14s %6d  %-20s %8s  %-20s %4d %3d  %8s  %9s  %9s  %5d  %4d  %5d  %s%s" % (
            b["block"], b["cases"], counts(b["base"]), pct(b["base"]["rate"]), counts(b["after"]),
            b["after"]["lost"], len(b.get("lost_adjudicated", [])), pct(b["after"]["rate"]), pp(b["delta_pp"]),
            "(%s)" % pp(b["delta_pp_literal_info"]),
            len(b["new_crash"]), len(b["regressed"]), len(b["fixed"]), b["verdict"],
            " (SUBSET)" if b["subset"] else ""))
        print("    info  literal Pass/(Pass+Fail): BASE %s  AFTER %s  delta %s pp (not gating)" % (
            pct(b["base"]["literal_rate"]), pct(b["after"]["literal_rate"]), pp(b["delta_pp_literal_info"])))
        print("    info  pre-ruling Pass/(results-NS): BASE %s  AFTER %s  delta %s pp (not gating)" % (
            pct(b["base"]["results_rate"]), pct(b["after"]["results_rate"]), pp(b["delta_pp_results_info"])))
        for reason in b["reasons"]:
            print("    ! " + reason)
        for warning in b["warnings"]:
            print("WARN %s: %s" % (b["block"], warning))
        for x in b["new_crash"]:
            print("    NEW CRASH  " + x)
        for x in b["lost"]:
            if x["adjudication"]:
                print("    ADJUDICATED %s  (Pass -> %s)  out of the rate; adjudicated: %s" % (
                    x["case"], x["after"], x["adjudication"]))
            else:
                print("    LOST       %s  (Pass -> %s)  UNADJUDICATED" % (x["case"], x["after"]))
        lost = {x["case"] for x in b["lost"]}
        others = [x for x in b["regressed"] if x not in lost]
        for x in others[:40]:
            print("    Pass->not  " + x)
        if len(others) > 40:
            print("    ... %d more regressions in the JSON" % (len(others) - 40))
        for x in b["fixed"][:20]:
            print("    not->Pass  " + x)
    u = c["ubo"]
    print("%-14s  %s  (%s)" % (u["block"], u["reasons"][0], u["cases"]))
    if c["ignored_reports"]:
        print("    note: report(s) outside the five gate blocks ignored: " + ", ".join(c["ignored_reports"]))
    for problem in c["adjudication"]["problems"]:
        print("WARN " + problem)
    if c["adjudication"]["stale"]:
        print("    note: cts/adjudication.tsv names case(s) that are no $BASE-Pass loss (nothing to adjudicate): "
              + ", ".join(c["adjudication"]["stale"]))
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
    ap.add_argument("--extra-monolith", action="append", metavar="DIR[@BOOT_ID]",
                    help="add DIR/<case>-DirectVulkan/repeat-NN as extra same-session monolith readings (%s "
                         "adjudication re-reduction; never PASS); @BOOT_ID attests the session of the repeats that "
                         "carry no boot_id.txt" % RULING_62)
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
    try:
        extras = extra_monolith(ca, args.extra_monolith)
    except ValueError as error:  # (compare_actuals.ImageError is one)
        print(str(error), file=sys.stderr)
        return 2
    g3 = gate3(out, ca, extras)
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
