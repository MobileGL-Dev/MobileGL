#!/usr/bin/env python3
"""Exit gate 9.2: integration-spawn's case-name set equals integration-split's.

WHY A GATE AND NOT A COMMENT. The two lanes are registered from one macro, so today they agree
by construction - but a macro is one edit away from a second hand-written block, and a spawn lane
covering LESS than the split lane goes green while proving less. Nothing about a smaller green
lane looks wrong. This asserts the sets rather than the counts, because a count can agree while
the membership does not.

ARM PREFIXES ARE STRIPPED BEFORE COMPARING. The lanes deliberately differ in name -
`DirectGLES.Split.X` against `DirectGLES.Spawn.X` - and the sub-lane tails (`F1.`, `Ct.`) are
part of neither the case nor the arm, so both come off.

ONE SANCTIONED ASYMMETRY, LISTED HERE AND NOWHERE ELSE. `integration-split` also labels the
PUSH-MONOLITH arm (the CMakeLists explains why: the label means "the set the disaggregation gate
runs", not "runs inproc"), and that arm's scenario is monolith-only by construction. It cannot
have a spawn counterpart, so it is named as an exception rather than allowed to widen the
tolerance for everything else.
"""
import argparse
import re
import subprocess
import sys

# Registered only on the push-monolith arm, which has no spawn counterpart by construction.
MONOLITH_ONLY = {"MonolithAttachmentClearScenario"}

CASE = re.compile(r"DirectGLES\.[A-Za-z0-9]+\.(?:F1\.|Ct\.)?([A-Za-z0-9_]+Scenario\.[A-Za-z0-9_]+)")


def lane_cases(build_dir, label):
    out = subprocess.run(["ctest", "-N", "-L", "^" + label + "$"], cwd=build_dir,
                         capture_output=True, text=True, check=True).stdout
    return {m.group(1) for m in CASE.finditer(out)}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("build_dir")
    parser.add_argument("--require-device", action="store_true",
                        help="Also require the configured integration-tcp-device lane")
    args = parser.parse_args()

    split = lane_cases(args.build_dir, "integration-split")
    spawn = lane_cases(args.build_dir, "integration-spawn")
    if not split or not spawn:
        print(f"::error::a lane is EMPTY (split={len(split)}, spawn={len(spawn)}). An empty set "
              f"compares equal to nothing and would pass this gate silently, which is the one "
              f"way it could stop meaning anything.", file=sys.stderr)
        return 1

    split_comparable = {c for c in split if c.split(".", 1)[0] not in MONOLITH_ONLY}
    missing = sorted(split_comparable - spawn)
    extra = sorted(spawn - split_comparable)

    print(f"spawn-lane parity: split {len(split)} ({len(split_comparable)} comparable), "
          f"spawn {len(spawn)}")
    if missing:
        print("::error::cases in integration-split with no integration-spawn counterpart: " +
              ", ".join(missing) + ". Exit gate 9.2 requires the sets to be equal - a spawn lane "
              "that covers less goes green while proving less. Register through "
              "mgl_itest_register_split_arms, which emits both arms from one filter.",
              file=sys.stderr)
    if extra:
        print("::error::cases in integration-spawn that integration-split does not run: " +
              ", ".join(extra) + ". The split arm is the control; a case only the spawn arm runs "
              "has no baseline to be compared against.", file=sys.stderr)
    failed = bool(missing or extra)
    labels = ["integration-tcp"]
    if args.require_device:
        labels.append("integration-tcp-device")
    for label in labels:
        cases = lane_cases(args.build_dir, label)
        missing, extra = sorted(spawn - cases), sorted(cases - spawn)
        print(f"{label} parity: spawn {len(spawn)}, tcp {len(cases)}")
        if not cases or missing or extra:
            print(f"::error::{label}: missing={missing}, extra={extra}", file=sys.stderr)
            failed = True
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
