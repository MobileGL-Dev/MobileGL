#!/usr/bin/env python3
"""P8-A (ID-P8-2): every monolith integration case runs on a split arm or is exempt BY NAME.

THE QUESTION. The monolith registrations (`DirectGLES.<case>`, `DirectVulkan.<case>`, plus their
knob-pinned lanes such as `DirectGLES.ForcedDepthStencilEmulation.<case>`) define what the suite
tests. A case that exists there and on no split arm is a case the disaggregated runtime was never
asked about - and nothing about a green split lane says so. This asserts, per backend:

    monolith cases  ⊆  split cases  ∪  exempt cases

after the arm and tail segments come off both sides: an entry name is `<Backend>.[<Arm>.][<tail>.]
<Suite>.<Case>`, and only the last two segments (the gtest name) are compared. `Split.F1.`,
`Spawn.ForcedDs.`, `Tcp.MultiDrawTierBaseVertex.` and the monolith's
`ForcedDepthStencilEmulation.` all normalise away.

WHAT COUNTS AS A SPLIT CASE differs by backend, on purpose:
  * DirectGLES - an entry with an arm segment (Split/Spawn/Tcp/TcpDevice) that carries a GATED
    label. The one informational spelling family is `integration-magma-{all,full}-<arm>`
    (MG_IntegrationTest/CMakeLists.txt, P7 package L tiers 2 and 3), and an entry carrying only
    those labels is not coverage.
  * DirectVulkan - any split-arm entry, the informational tiers included. Magma's split arms are
    informational by ruling (ID-P8-2): cases move from tier 2/3 into the gated Magma tier with the
    commit that retires their blocker. The report prints how many are gated and how many are
    informational-only, so the number is visible without being a gate.

WHAT COUNTS AS A MONOLITH CASE: an entry with no arm segment carrying `integration-gpu` - the label
the monolith registrations and the monolith-control mechanism entries share. That puts
`DirectGLES.PushMonolithArm.<case>` (labelled integration-split, but no arm) on the monolith side,
where it belongs, and leaves build-verify's `DirectGLES.VerifySplit.<case>` (no integration-gpu)
out of both sides.

THE EXEMPTION TABLE (scripts/data/split_coverage_exemptions.txt), one row per pattern:

    [<Backend>:]<gtest pattern> <reason-class> <one-line reason>

with reason classes mechanism / single-backend / no-records / inproc-peek / pending-fix. The table
keeps itself honest the way spawn_lane_parity.py's exception lists do - every one of these is an
error, not a warning:
  * a pattern that matches no monolith case (an exemption about nothing);
  * a pattern every case of which is already on a split arm (stale: a pending-fix that landed must
    take its row with it, or the next regression of that case is silently exempt);
  * a `pending-fix` row that names neither a package (`P8-B`) nor a DEBTS.md row;
  * a `single-backend` row with no `<Backend>:` scope (it exempts the backend the case does NOT
    belong to, and unscoped it would also hide a gap on the one it does);
  * an unknown reason class, a missing reason, a duplicated pattern.

This script does not compare the arms with each other - spawn_lane_parity.py owns that.
"""
import argparse
import fnmatch
import json
from pathlib import Path
import re
import subprocess
import sys

BACKENDS = ("DirectGLES", "DirectVulkan")
ARMS = ("Split", "Spawn", "Tcp", "TcpDevice")
REASON_CLASSES = ("mechanism", "single-backend", "no-records", "inproc-peek", "pending-fix")
MONOLITH_LABEL = "integration-gpu"
INFORMATIONAL_LABEL = re.compile(r"^integration-magma-(all|full)-(split|spawn|tcp)$")
GTEST_NAME = re.compile(r"^[A-Za-z0-9_]+\.[A-Za-z0-9_]+$")
PATTERN = re.compile(r"^(?:(DirectGLES|DirectVulkan):)?([A-Za-z0-9_*?]+\.[A-Za-z0-9_*?]+)$")
OWNER = re.compile(r"\bP\d+[a-z]?-[0-9A-Z]+\b|DEBTS\.md")
DEFAULT_EXEMPTIONS = Path(__file__).resolve().parents[2] / "scripts/data/split_coverage_exemptions.txt"


def labels_of(test):
    for prop in test.get("properties", []):
        if prop.get("name") == "LABELS":
            value = prop.get("value") or []
            return [value] if isinstance(value, str) else list(value)
    return []


def classify(document, errors):
    """{backend: {"mono": set, "gated": set, "info": set, "arms": {arm: set}}} from a
    `ctest --show-only=json-v1` document. Names under a backend that do not parse are errors."""
    out = {b: {"mono": set(), "gated": set(), "info": set(), "arms": {a: set() for a in ARMS}}
           for b in BACKENDS}
    for test in document.get("tests", []):
        name = test.get("name", "")
        parts = name.split(".")
        if parts[0] not in BACKENDS:
            continue
        backend = parts[0]
        labels = labels_of(test)
        arm = parts[1] if len(parts) > 1 and parts[1] in ARMS else None
        gtest = ".".join(parts[-2:])
        rest = parts[2:] if arm else parts[1:]
        if len(rest) < 2 or not GTEST_NAME.match(gtest):
            errors.append(f"{name}: does not parse as <Backend>.[<Arm>.][<tail>.]<Suite>.<Case>, so "
                          f"this gate would silently drop it")
            continue
        side = out[backend]
        if arm is None:
            if MONOLITH_LABEL in labels:
                side["mono"].add(gtest)
            continue
        if labels and not all(INFORMATIONAL_LABEL.match(label) for label in labels):
            side["gated"].add(gtest)
            side["arms"][arm].add(gtest)
        else:
            side["info"].add(gtest)
    return out


def parse_exemptions(text, errors):
    rows = []
    seen = set()
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        fields = line.split(None, 2)
        where = f"exemptions:{number}"
        if len(fields) < 3 or not fields[2].strip():
            errors.append(f"{where}: `{line}` needs <pattern> <reason-class> <reason>")
            continue
        pattern, reason_class, reason = fields
        match = PATTERN.match(pattern)
        if not match:
            errors.append(f"{where}: `{pattern}` is not [<Backend>:]<Suite>.<Case> (globs * and ? allowed)")
            continue
        if reason_class not in REASON_CLASSES:
            errors.append(f"{where}: reason class `{reason_class}` is not one of {', '.join(REASON_CLASSES)}")
            continue
        if reason_class == "pending-fix" and not OWNER.search(reason):
            errors.append(f"{where}: pending-fix `{pattern}` names no owner - a package (P8-B) or a "
                          f"DEBTS.md row must be in the reason, or the row outlives everyone's memory of it")
            continue
        if reason_class == "single-backend" and match.group(1) is None:
            errors.append(f"{where}: single-backend `{pattern}` must be scoped `<Backend>:` to the backend "
                          f"it exempts; unscoped it would also hide a gap on the backend the case belongs to")
            continue
        if pattern in seen:
            errors.append(f"{where}: `{pattern}` is listed twice")
            continue
        seen.add(pattern)
        backends = (match.group(1),) if match.group(1) else BACKENDS
        rows.append({"where": where, "pattern": pattern, "glob": match.group(2), "backends": backends,
                     "class": reason_class, "reason": reason.strip()})
    return rows


def check(document, exemption_text, out=print):
    """Returns the exit code; every finding goes through `out`."""
    errors = []
    sides = classify(document, errors)
    rows = parse_exemptions(exemption_text, errors)
    covered = {"DirectGLES": sides["DirectGLES"]["gated"],
               "DirectVulkan": sides["DirectVulkan"]["gated"] | sides["DirectVulkan"]["info"]}
    exempt = {b: {} for b in BACKENDS}
    for row in rows:
        matched = needed = 0
        for backend in row["backends"]:
            for case in sorted(sides[backend]["mono"]):
                if not fnmatch.fnmatchcase(case, row["glob"]):
                    continue
                matched += 1
                if case not in covered[backend]:
                    needed += 1
                    exempt[backend].setdefault(case, row)
        if matched == 0:
            errors.append(f"{row['where']}: `{row['pattern']}` matches no monolith case - an exemption "
                          f"about nothing (renamed or deleted case?)")
        elif needed == 0:
            errors.append(f"{row['where']}: `{row['pattern']}` is STALE - every monolith case it names is "
                          f"on a split arm now; delete the row so the next regression is not exempt")
    for backend in BACKENDS:
        side = sides[backend]
        mono = side["mono"]
        if not mono:
            errors.append(f"{backend}: no monolith cases in this build - an empty set is covered by "
                          f"anything, which is the one way this gate could stop meaning something")
            continue
        missing = sorted(mono - covered[backend] - set(exempt[backend]))
        by_class = {}
        for row in exempt[backend].values():
            by_class[row["class"]] = by_class.get(row["class"], 0) + 1
        per_arm = ", ".join(f"{arm.lower()} {len(mono & cases)}" for arm, cases in side["arms"].items() if cases)
        classes = ", ".join(f"{name} {count}" for name, count in sorted(by_class.items())) or "none"
        if backend == "DirectGLES":
            out(f"split coverage {backend}: monolith {len(mono)}, on a gated split arm "
                f"{len(mono & side['gated'])} ({per_arm}), exempt {len(exempt[backend])} ({classes}), "
                f"missing {len(missing)}")
        else:
            gated = mono & side["gated"]
            out(f"split coverage {backend}: monolith {len(mono)}, on a gated split arm {len(gated)} "
                f"({per_arm}), informational tiers only {len(mono & side['info'] - side['gated'])}, "
                f"exempt {len(exempt[backend])} ({classes}), missing {len(missing)}")
        for case in missing:
            errors.append(f"{backend}.{case} is registered on monolith and on no "
                          f"{'gated ' if backend == 'DirectGLES' else ''}split arm, and no exemption "
                          f"names it: register it (MG_IntegrationTest/P8Coverage.cmake) or add a row "
                          f"to scripts/data/split_coverage_exemptions.txt with its reason class")
    for message in errors:
        out(f"::error::{message}")
    return 1 if errors else 0


def ctest_document(build_dir):
    text = subprocess.run(["ctest", "--show-only=json-v1"], cwd=build_dir, capture_output=True,
                          text=True, check=True).stdout
    return json.loads(text)


# ---- self-test -------------------------------------------------------------------------------

def _entry(name, *labels):
    return {"name": name, "properties": [{"name": "LABELS", "value": list(labels)}]}


def _doc(*entries):
    return {"tests": list(entries)}


BASE = [
    _entry("DirectGLES.AScenario.One", "integration-gpu"),
    _entry("DirectGLES.AScenario.Two", "integration-gpu"),
    _entry("DirectGLES.ForcedDepthStencilEmulation.BScenario.Three", "integration-gpu"),
    _entry("DirectGLES.Split.AScenario.One", "integration-gpu", "integration-split"),
    _entry("DirectGLES.Spawn.F1.AScenario.Two", "integration-gpu", "integration-spawn"),
    _entry("DirectGLES.Tcp.ForcedDs.BScenario.Three", "integration-gpu", "integration-tcp"),
    _entry("DirectVulkan.AScenario.One", "integration-gpu"),
    _entry("DirectVulkan.MagmaOnlyScenario.Four", "integration-gpu"),
    _entry("DirectVulkan.Split.AScenario.One", "integration-magma-all-split"),
    _entry("DirectVulkan.Tcp.Fm.MagmaOnlyScenario.Four", "integration-magma-tcp"),
    _entry("TcpServer.Start"),
]


def _run(document, exemptions):
    lines = []
    rc = check(document, exemptions, out=lines.append)
    return rc, "\n".join(lines)


def self_test():
    failures = []
    ran = []

    def expect(label, document, exemptions, rc, *needles, absent=()):
        ran.append(label)
        got, text = _run(document, exemptions)
        if got != rc or any(n not in text for n in needles) or any(n in text for n in absent):
            failures.append(f"{label}: rc {got} (want {rc}); output:\n{text}")

    expect("tails normalise on both sides", _doc(*BASE), "", 0,
           "DirectGLES: monolith 3, on a gated split arm 3", "missing 0")
    expect("a monolith case on no arm reds BY NAME",
           _doc(*BASE, _entry("DirectGLES.CScenario.Five", "integration-gpu")), "", 1,
           "DirectGLES.CScenario.Five is registered on monolith")
    expect("an informational-only arm is not DirectGLES coverage",
           _doc(*BASE, _entry("DirectGLES.CScenario.Five", "integration-gpu"),
                _entry("DirectGLES.Split.CScenario.Five", "integration-magma-full-split")), "", 1,
           "DirectGLES.CScenario.Five is registered on monolith")
    expect("an informational tier IS DirectVulkan coverage, and is reported as such", _doc(*BASE), "", 0,
           "DirectVulkan: monolith 2, on a gated split arm 1", "informational tiers only 1")
    expect("an arm-less entry without integration-gpu is on neither side",
           _doc(*BASE, _entry("DirectGLES.VerifySplit.CScenario.Five", "integration-verify-split")), "", 0)
    expect("PushMonolithArm (integration-split, no arm) is a MONOLITH case",
           _doc(*BASE, _entry("DirectGLES.PushMonolithArm.CScenario.Five", "integration-gpu",
                              "integration-split")), "", 1,
           "DirectGLES.CScenario.Five is registered on monolith")
    expect("an exemption covers a missing case",
           _doc(*BASE, _entry("DirectGLES.CScenario.Five", "integration-gpu")),
           "CScenario.* mechanism toggles a process-local mask", 0, "exempt 1 (mechanism 1)")
    expect("a stale exemption reds",
           _doc(*BASE), "AScenario.One no-records query only", 1, "is STALE")
    expect("an exemption about nothing reds",
           _doc(*BASE), "ZScenario.* mechanism gone", 1, "matches no monolith case")
    expect("pending-fix without an owner reds",
           _doc(*BASE, _entry("DirectGLES.CScenario.Five", "integration-gpu")),
           "CScenario.Five pending-fix red on spawn", 1, "names no owner")
    expect("pending-fix owned by a package passes",
           _doc(*BASE, _entry("DirectGLES.CScenario.Five", "integration-gpu")),
           "CScenario.Five pending-fix red on spawn until P8-B lands", 0)
    expect("pending-fix owned by a DEBTS.md row passes",
           _doc(*BASE, _entry("DirectGLES.CScenario.Five", "integration-gpu")),
           "CScenario.Five pending-fix red on spawn (DEBTS.md row: default framebuffer)", 0)
    expect("an unknown reason class reds",
           _doc(*BASE, _entry("DirectGLES.CScenario.Five", "integration-gpu")),
           "CScenario.Five flaky it is slow", 1, "is not one of")
    expect("an unscoped single-backend row reds",
           _doc(*BASE, _entry("DirectGLES.MagmaOnlyScenario.Four", "integration-gpu")),
           "MagmaOnlyScenario.* single-backend Magma's lane", 1, "must be scoped")
    expect("a scoped single-backend row exempts only its backend",
           _doc(*BASE, _entry("DirectGLES.MagmaOnlyScenario.Four", "integration-gpu")),
           "DirectGLES:MagmaOnlyScenario.* single-backend Magma's lane", 0)
    expect("a duplicated pattern reds",
           _doc(*BASE, _entry("DirectGLES.CScenario.Five", "integration-gpu")),
           "CScenario.* mechanism a\nCScenario.* mechanism b", 1, "listed twice")
    expect("an unparseable backend entry reds", _doc(*BASE, _entry("DirectGLES.Split.Oops", "integration-split")),
           "", 1, "does not parse")
    expect("an empty monolith side reds", _doc(*[e for e in BASE if e["name"].startswith("DirectVulkan")]),
           "", 1, "DirectGLES: no monolith cases")
    expect("a row with no reason reds", _doc(*BASE), "AScenario.Two mechanism", 1, "needs <pattern>")
    # The real table must at least parse (its coverage needs a build; the gate run checks that).
    if DEFAULT_EXEMPTIONS.exists():
        errors = []
        parse_exemptions(DEFAULT_EXEMPTIONS.read_text(), errors)
        if errors:
            failures.append("the checked-in exemption table does not parse:\n" + "\n".join(errors))
    for failure in failures:
        print(f"::error::self-test: {failure}", file=sys.stderr)
    print(f"split_coverage self-test: {len(ran)} cases + the checked-in table's parse, "
          f"{len(failures)} failure(s), {'OK' if not failures else 'FAILED'}")
    return 1 if failures else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("build_dir", nargs="?", help="a configured build (runs ctest --show-only=json-v1)")
    parser.add_argument("--json", help="a saved `ctest --show-only=json-v1` document instead of a build")
    parser.add_argument("--exemptions", default=str(DEFAULT_EXEMPTIONS))
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if bool(args.build_dir) == bool(args.json):
        parser.error("give exactly one of build_dir or --json")
    document = json.loads(Path(args.json).read_text()) if args.json else ctest_document(args.build_dir)
    return check(document, Path(args.exemptions).read_text())


if __name__ == "__main__":
    sys.exit(main())
