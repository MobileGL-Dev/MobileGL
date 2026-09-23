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

P7 PACKAGE L ADDS THE SAME QUESTION FOR MAGMA, IN TWO TIERS, AND THE NORMALISATION IS
DIFFERENT ON PURPOSE. The Espryt half above strips the sub-lane tails `F1.` and `Ct.` because
they name neither the case nor the arm. The Magma half must NOT: `RunAhead.Credit1.` and
`RunAhead.Credit3.` are two entries of the same case, differing in the credit the lane pins, and
a normaliser that dropped the tail would fold them together and then report an equal-sized set
that had silently lost one. So there the arm - and only the arm, the SECOND dotted segment - is
what comes off.
"""
import argparse
import re
import subprocess
import sys

# Registered only on the push-monolith arm, which has no spawn counterpart by construction.
MONOLITH_ONLY = {"MonolithAttachmentClearScenario"}

CASE = re.compile(r"DirectGLES\.[A-Za-z0-9]+\.(?:F1\.|Ct\.)?([A-Za-z0-9_]+Scenario\.[A-Za-z0-9_]+)")

# `<Backend>.<Arm>.<everything else>` -> `<Backend>.<everything else>`. The arm is the only
# segment two arms of one lane are allowed to differ in.
ARM = re.compile(r"^(DirectGLES|DirectVulkan)\.(Split|Spawn|Tcp|TcpDevice)\.(.+)$")
TEST_LINE = re.compile(r"^\s*Test\s+#\d+:\s*(\S+)\s*$", re.MULTILINE)

# `ctest -N -L <tcp label>` lists the FIXTURE entries too, because every tcp entry declares
# FIXTURES_REQUIRED mobilegl-tcp and ctest pulls the setup/cleanup in with them. They are the
# supervisor, not cases, and they exist on exactly one arm by construction. Named here so that
# any OTHER unparseable name is still an error rather than a silent drop.
FIXTURE_ENTRIES = {"TcpServer.Start", "TcpServer.Stop"}

# THE MAGMA TIER'S SANCTIONED ASYMMETRIES, LISTED HERE AND NOWHERE ELSE - the same discipline
# MONOLITH_ONLY above gets. Both are inproc-only BY CONSTRUCTION, not by omission:
#   MagmaRunAheadScenario  arms its hold through ServerLoopInstance().SetBeforeRetireHookForTesting,
#                          a hook on the CALLING process's server loop. With the server in another
#                          process the hold never activates and the wait times out.
#   MagmaWireCacheScenario SetUp asserts runtime.transportName == "inproc" outright: the lane reads
#                          the server's own view/attachment identity out of this process.
# A cross-process hold and a cross-process cache peek are server-side knobs neither exists yet;
# until they do, naming the two scenarios here keeps the tolerance from widening for anything else.
MAGMA_INPROC_ONLY = ("MagmaRunAheadScenario.", "MagmaWireCacheScenario.")

# P7 wave 2-B2: THE SANCTIONED ASYMMETRY THAT IS NOT INPROC-ONLY - these exist on split AND
# spawn and cannot exist on tcp. Every MGITEST_MAGMA_FORCE_* knob is read BY THE SERVER
# (WireFramebuffer.inc's getenv readers run on the apply thread), and a ctest ENVIRONMENT
# property only reaches the process ctest starts, the CLIENT. Under inproc the server is a
# thread of that process and under spawn ServerSpawn.cpp copies ::environ into the child, so
# both see it; under tcp the server is the LANE-WIDE TcpServer.Start fixture, started once from
# the fixture's own os.environ before any case runs, so nothing a per-test property says can
# reach it. A tcp copy would run the DEFAULT arm under a name claiming otherwise.
#
# MEASURED (a temporary fatal in each of the three readers): all five knob=1 entries died on
# split and on spawn and ALL FIVE PASSED on tcp.
#
# These are TAILS, not scenarios - the normaliser above keeps the sub-lane tail for the Magma
# tier on purpose, so `.MsResolve1.` names the knob entry and `.MsResolve0.` (the default, which
# does keep its tcp entry) is untouched.
#
# P7 wave 2-B3 (ID-P7-34) adds `.StaleSerial.`: MGITEST_MAGMA_FORCE_STALE_BUFFER_SERIAL is read by
# the server too (VkBufferManager.cpp), so the streamed subdata-then-draw red-once has the same
# two arms and the same tcp absence. One list, one mechanism, for every server-side knob.
# `.MsFlip1.` (B2 review round 2) is the shader-resolve knob twin of the knob-free `.MsFlip.`,
# which keeps its tcp entry.
# P7 wave 4 M2: MagmaWireReclaimScenario reads the SERVER's wbuf[] PipeStats gauges off the server's
# private log with the stats channel and MOBILEGL_IPC_WIRE_DEFERRED_MB=8 in the entry's environment -
# server-process environment again, so the same two arms and the same tcp absence. Its three cases
# are three tails (one key each) under the `.Reclaim.` prefix.
MAGMA_SERVER_ENV_KNOB_NO_TCP = (".ShaderMip1.", ".ShaderMip2.", ".DepthMip.",
                                ".DefaultBlitShape1.", ".MsResolve1.", ".StaleSerial.", ".MsFlip1.",
                                ".Reclaim.MagmaWireReclaimScenario.RespecifiesWithNoDrawBetweenKeepTheLiveStoreCountBounded",
                                ".Reclaim.MagmaWireReclaimScenario.RespecifyAndDrawEachStoreInOneFrameStaysWithinTheDeferredBudget",
                                ".Reclaim.MagmaWireReclaimScenario.ManySmallRespecifyAndDrawRoundsStayUnderTheStoreCountCeiling")


def lane_names(build_dir, label):
    """Every ctest entry name under an ANCHORED label (`ctest -L` is a regex, not a name)."""
    out = subprocess.run(["ctest", "-N", "-L", "^" + label + "$"], cwd=build_dir,
                         capture_output=True, text=True, check=True).stdout
    return {m.group(1) for m in TEST_LINE.finditer(out)}


def arm_keys(names):
    return {f"{m.group(1)}.{m.group(3)}" for m in (ARM.match(n) for n in names) if m}


def lane_cases(build_dir, label):
    out = subprocess.run(["ctest", "-N", "-L", "^" + label + "$"], cwd=build_dir,
                         capture_output=True, text=True, check=True).stdout
    return {m.group(1) for m in CASE.finditer(out)}


def compare_arms(build_dir, tier, labels, inproc_only=(), no_tcp=()):
    """The three arms of one tier must name the same set after the arm segment comes off.

    `inproc_only` drops a key from the comparison for EVERY non-split arm; `no_tcp` drops it for
    the tcp arm alone, which is a different shape and needs to be: a server-side test knob does
    reach a spawned server and does not reach the tcp lane's shared fixture, so the entry is
    required on spawn and forbidden on tcp. Folding it into `inproc_only` would stop requiring
    it on spawn, which is where it does most of its work.

    Returns True on failure, the way main() below counts them."""
    sets = {}
    for arm, label in labels.items():
        names = lane_names(build_dir, label) - FIXTURE_ENTRIES
        sets[arm] = arm_keys(names)
        print(f"{tier}: {label} has {len(names)} case entrie(s), "
              f"{len(sets[arm])} after normalisation")
        if len(names) != len(sets[arm]):
            print(f"::error::{label}: {len(names) - len(sets[arm])} entry name(s) do not parse "
                  f"as <Backend>.<Arm>.<case>, so this comparison silently drops them: "
                  + ", ".join(sorted(n for n in names if not ARM.match(n))), file=sys.stderr)
            return True
    if not any(sets.values()):
        print(f"::note::{tier}: no entries under {sorted(labels.values())} - nothing to compare")
        return False
    failed = False
    reference = "split"
    comparable = {k for k in sets[reference]
                  if not any(only in k for only in inproc_only)}
    if len(comparable) != len(sets[reference]):
        print(f"{tier}: {len(sets[reference]) - len(comparable)} inproc-only entrie(s) excluded "
              f"from the comparison by name ({', '.join(inproc_only)})")
    if no_tcp:
        dropped = {k for k in comparable if any(only in k for only in no_tcp)}
        # EVERY TAIL MUST NAME SOMETHING (review round 2). A tail that matches no split key is
        # silently inert - a renamed prefix would leave the exception in the list and the
        # entry it was about riding under some other name - so an unmatched tail is an error,
        # and so is a tail that suddenly matches a different number of entries than the one
        # knob case it was written for.
        for tail in no_tcp:
            hits = sorted(k for k in comparable if tail in k)
            if len(hits) != 1:
                print(f"::error::{tier}: the server-env-knob tail {tail!r} matches {len(hits)} "
                      f"split entrie(s) {hits}, not exactly one. Each tail names ONE knob case; a "
                      f"tail that names none is an exception about nothing, and one that names "
                      f"more has let a second case ride off the tcp arm unexamined.",
                      file=sys.stderr)
                return True
        if dropped:
            print(f"{tier}: {len(dropped)} server-env-knob entrie(s) excluded from the TCP "
                  f"comparison only ({', '.join(no_tcp)}) - the knob is read on the server and "
                  f"the tcp server is the lane fixture, not a per-case process")
    for arm, keys in sets.items():
        if arm == reference:
            continue
        expected = comparable
        if arm == "tcp" and no_tcp:
            expected = {k for k in comparable if not any(only in k for only in no_tcp)}
        missing, extra = sorted(expected - keys), sorted(keys - expected)
        if missing or extra:
            print(f"::error::{tier}: the {arm} arm does not match the {reference} arm: "
                  f"missing={missing}, extra={extra}. Every arm of a tier is emitted from one "
                  f"loop over the same case list, so a difference here is a registration bug, "
                  f"not a coverage decision.", file=sys.stderr)
            failed = True
    return failed


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

    # P7 package L, exit gate 1's half of the same question: the Magma arms.
    # THE EXCLUSION IS PER TIER, not global. In the GATED tier the two scenarios are registered
    # on the inproc arm ALONE (the CMakeLists says why), so they are absent from the other arms
    # and have to come off the reference set. In the FULL-SUITE census they are registered on
    # every arm - it is a whole-binary filter, and there they simply SKIP for want of their
    # lane marker - so excluding them there would turn a present-on-all-arms case into a
    # spurious "extra".
    # The server-env knob entries are gated-tier only for the same per-tier reason: the
    # full-suite census is a whole-binary filter with no knob in its environment at all, so
    # there is nothing there to exclude.
    failed |= compare_arms(args.build_dir, "magma gated tier",
                           {"split": "integration-magma-split",
                            "spawn": "integration-magma-spawn",
                            "tcp": "integration-magma-tcp"},
                           inproc_only=MAGMA_INPROC_ONLY,
                           no_tcp=MAGMA_SERVER_ENV_KNOB_NO_TCP)
    failed |= compare_arms(args.build_dir, "magma informational tier",
                           {"split": "integration-magma-all-split",
                            "spawn": "integration-magma-all-spawn",
                            "tcp": "integration-magma-all-tcp"})
    # Tier 3, the whole-suite census. It is expected to be RED and is in no gating step - but
    # its three arms must still name the same set, because the census's whole value is that a
    # case red on spawn and green on inproc is a two-process fact, and that comparison needs
    # both arms to have run the same case.
    failed |= compare_arms(args.build_dir, "magma full-suite tier",
                           {"split": "integration-magma-full-split",
                            "spawn": "integration-magma-full-spawn",
                            "tcp": "integration-magma-full-tcp"})
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
