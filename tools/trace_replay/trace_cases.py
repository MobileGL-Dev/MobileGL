#!/usr/bin/env python3
# MobileGL - tools/trace_replay/trace_cases.py
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

import argparse
import json
import sys
from pathlib import Path


TRACE_CASES_JSON = Path(__file__).with_name("trace_cases.json")

# The top-level keys of the manifest. An unknown one is refused, as an unknown case key is.
KNOWN_MANIFEST_KEYS = frozenset({"defaults", "cases", "android_smoke"})
CI_BACKENDS = ("DirectGLES", "DirectVulkan")

# Every key a case or the defaults block may carry. An UNKNOWN key is a hard error rather than a
# silent no-op, which is review finding N-4: `"split": true` mistyped as `"splitt": true` loaded
# clean, the split subset became [], the GitHub matrix became {"include":[]}, and `retrace-split`
# was skipped with no red anywhere. Every other way of getting `split` wrong already raised
# (`"ci": false`, a non-bool value) - the typo was the one hole, and it is the shape that makes a
# whole CI job quietly stop existing. P6 inverted the key's default (see load_trace_case_manifest),
# which closes the mirror-image hole: a case added later is IN the split subset unless it says
# otherwise, so forgetting the key can no longer drop a case out of the lane.
#
# Adding a key means adding it here, deliberately, in the same commit. That is the point.
KNOWN_CASE_KEYS = frozenset({
    "name",
    "trace_archive",
    "trace_file",
    "golden",
    "alternate_golden",
    "target_call",
    "width",
    "height",
    "ssim_threshold",
    "crop_x",
    "crop_y",
    "crop_width",
    "crop_height",
    "coherent_as_flush",
    "timeout_seconds",
    "ci",
    "ci_backends",
    "verify",
    "split",
    "avoid_angle_llvmpipe_explicit_lod_bias",
    "backend_overrides",
    # Documentation only - which trace a derived fixture was made from, and how. Nothing reads it.
    "derived_from",
})

# The only fields a per-backend block may restate. Deliberately three: they are the three the
# comparison is made of. Letting a backend restate `target_call` or `crop_*` would mean the two
# backends stopped retracing the same frame of the same trace through the same window, and the
# rates would no longer be comparable - which is the one thing the two-backend matrix is for.
BACKEND_OVERRIDE_KEYS = frozenset({"golden", "alternate_golden", "ssim_threshold"})


def load_trace_case_manifest(path=TRACE_CASES_JSON):
    with Path(path).open("r", encoding="utf-8") as file:
        manifest = json.load(file)
    defaults = manifest.get("defaults", {})
    unknown_defaults = sorted(set(defaults) - KNOWN_CASE_KEYS)
    if unknown_defaults:
        raise ValueError(
            f"unknown key(s) in the defaults block: {', '.join(unknown_defaults)}. "
            f"Known keys are {', '.join(sorted(KNOWN_CASE_KEYS))}"
        )
    cases = []
    seen = set()
    for case in manifest.get("cases", []):
        merged = {**defaults, **case}
        name = merged.get("name")
        if not name:
            raise ValueError("trace case is missing name")
        unknown = sorted(set(case) - KNOWN_CASE_KEYS)
        if unknown:
            raise ValueError(
                f"unknown key(s) for {name}: {', '.join(unknown)}. A mistyped flag loads clean and "
                f"turns its whole CI subset into an empty matrix, which GitHub skips with no red. "
                f"Known keys are {', '.join(sorted(KNOWN_CASE_KEYS))}"
            )
        if name in seen:
            raise ValueError(f"duplicate trace case: {name}")
        seen.add(name)
        for key in ("trace_archive", "trace_file", "golden", "target_call", "width", "height"):
            if key not in merged:
                raise ValueError(f"{key} is required for {name}")
        # "verify" opts a case into the MOBILEGL_PIPE_VERIFY retrace subset. It is checked here
        # rather than where the matrix is built so that a typo is a loud manifest error in every
        # consumer (the cmake emitter included) instead of a subset that is quietly one case short.
        verify = merged.get("verify", False)
        if not isinstance(verify, bool):
            raise ValueError(f"verify must be true or false for {name}")
        if verify and not merged.get("ci", True):
            raise ValueError(
                f"{name} is marked verify but excluded from CI, so the verify matrix would drop it"
            )
        # "split" IS AN OPT-OUT, NOT AN OPT-IN, and P6 inverted it deliberately.
        #
        # It began as an opt-in listing one case, because the split arm was one
        # case's worth of work. Now that the whole CI matrix runs split, 40
        # identical `"split": true` lines would carry no information AND would
        # reinstate the exact hole the key was written to close: a case added
        # later gets the monolith arms by default and is silently absent from
        # the split ones. Absent is the one state nothing reds on.
        #
        # So: a CI case is in the split subset unless it says `"split": false`,
        # and saying so is a reviewable claim that this trace cannot cross a
        # role boundary. `ci: false` removes a case from CI entirely and takes
        # its split arms with it - the subset is a SUBSET of the CI matrix, and
        # a case CI never runs cannot be one CI runs split.
        #
        # The arm runs the case's OWN ci_backends, both of them by default. The
        # DirectGLES-only requirement that used to live here is gone with the
        # DirectGLES-only arm (tools/trace_replay/CMakeLists.txt says why).
        split = merged.get("split", merged.get("ci", True))
        if not isinstance(split, bool):
            raise ValueError(f"split must be true or false for {name}")
        if split and not merged.get("ci", True):
            raise ValueError(
                f"{name} is marked split but excluded from CI, so the split matrix would drop it"
            )
        merged["split"] = split
        validate_backend_overrides(merged)
        cases.append(merged)
    unknown_top = sorted(set(manifest) - KNOWN_MANIFEST_KEYS)
    if unknown_top:
        raise ValueError(f"unknown top-level key(s) in the manifest: {', '.join(unknown_top)}")
    # "android_smoke" names the cases the emulator replays on the monolith and spawn lanes
    # when those lanes are reduced. A name that is not a CI case would shrink the subset with
    # no red, so it is refused here.
    smoke = manifest.get("android_smoke", [])
    if not isinstance(smoke, list) or not all(isinstance(name, str) for name in smoke):
        raise ValueError("android_smoke must be a list of case names")
    if len(set(smoke)) != len(smoke):
        raise ValueError("android_smoke repeats a case name")
    ci_names = {case["name"] for case in cases if case.get("ci", True)}
    not_ci = [name for name in smoke if name not in ci_names]
    if not_ci:
        raise ValueError(f"android_smoke names case(s) that are not CI cases: {', '.join(not_ci)}")
    return {"defaults": defaults, "cases": cases, "android_smoke": smoke}


def android_smoke_names(path=TRACE_CASES_JSON):
    return load_trace_case_manifest(path)["android_smoke"]


def validate_backend_overrides(case):
    """`backend_overrides` names the golden/threshold ONE backend uses instead of the shared one.

    P7 needs it because the device DirectVulkan matrix cannot be scored against the DirectGLES
    picture. Until now the two backends shared `golden` plus an optional `alternate_golden`, and
    `alternate_golden` is an OR, not a per-backend key: a DirectVulkan run that matched the
    DirectGLES golden passed, and a DirectGLES run that matched a Magma-only image passed too.
    That is exactly the "went green on the wrong arm" shape, so a per-backend golden REPLACES the
    shared pair for that backend rather than being added to it (see case_for_backend).

    Everything here is a load-time refusal rather than a runtime surprise, for the reason the
    KNOWN_CASE_KEYS comment above gives: the failure mode of a manifest key is a lane that
    quietly measures something else.
    """
    overrides = case.get("backend_overrides")
    if overrides is None:
        return
    name = case["name"]
    if not isinstance(overrides, dict) or not overrides:
        raise ValueError(f"backend_overrides must be a non-empty object for {name}")
    allowed = ci_backends(case)
    for backend, entry in overrides.items():
        if backend not in CI_BACKENDS:
            raise ValueError(f"unknown backend in backend_overrides for {name}: {backend}")
        if backend not in allowed:
            # An override for a backend the case never runs can only ever be dead text, and a
            # dead override reads exactly like a live one to whoever audits the golden next.
            raise ValueError(
                f"backend_overrides names {backend} but {name} does not run it "
                f"(ci_backends = {', '.join(allowed)})")
        if not isinstance(entry, dict) or not entry:
            raise ValueError(f"backend_overrides[{backend}] must be a non-empty object for {name}")
        unknown = sorted(set(entry) - BACKEND_OVERRIDE_KEYS)
        if unknown:
            raise ValueError(
                f"backend_overrides[{backend}] for {name} may only restate "
                f"{', '.join(sorted(BACKEND_OVERRIDE_KEYS))}; got {', '.join(unknown)}")
        for key in ("golden", "alternate_golden"):
            if key in entry and not (isinstance(entry[key], str) and entry[key]):
                raise ValueError(f"backend_overrides[{backend}].{key} must be a non-empty "
                                 f"path for {name}")
        threshold = entry.get("ssim_threshold")
        if "ssim_threshold" in entry and (isinstance(threshold, bool)
                                          or not isinstance(threshold, (int, float))):
            raise ValueError(f"backend_overrides[{backend}].ssim_threshold must be a number "
                             f"for {name}")
        if "alternate_golden" in entry and "golden" not in entry:
            # Otherwise the block would add a second acceptable image to the SHARED golden,
            # which is the OR this key exists to remove.
            raise ValueError(f"backend_overrides[{backend}] for {name} sets alternate_golden "
                             f"without golden; state the backend's own golden explicitly")


def case_for_backend(case, backend):
    """The case as ONE backend sees it: its own golden pair and threshold, or the shared ones.

    Returns the case UNCHANGED - the same object - when it declares no `backend_overrides` at
    all, so a manifest without the key produces byte-identical selections, environments and run
    fingerprints to the ones it produced before the key existed.

    THE KEY ITSELF IS DROPPED from the result even for a backend that overrides nothing, and
    that is deliberate: `run_tcp_matrix` fingerprints the resolved case, so leaving the block in
    would make "give DirectVulkan its own golden" invalidate every DirectGLES checkpoint in the
    same run - a full re-sweep of an arm whose golden, threshold and trace did not move.
    """
    if "backend_overrides" not in case:
        return case
    merged = {key: value for key, value in case.items() if key != "backend_overrides"}
    overrides = (case.get("backend_overrides") or {}).get(backend)
    if overrides:
        if "golden" in overrides:
            # REPLACE, do not extend: an override's golden is this backend's whole answer.
            merged["alternate_golden"] = ""
        merged.update(overrides)
    return merged


def backend_override_goldens(case, backend):
    """The goldens this case declares for OTHER backends - what `backend` must never match.

    Two sources, and the second is the one that is easy to miss: when THIS backend replaced the
    shared golden, the shared `golden`/`alternate_golden` stop being neutral and become the
    other backends' images. A DirectVulkan run that matched the shared (DirectGLES) picture is
    precisely the accident the per-backend key exists to make impossible, so it is named here
    rather than reported as "a different golden".
    """
    overrides = case.get("backend_overrides") or {}
    foreign = set()
    if "golden" in (overrides.get(backend) or {}):
        for key in ("golden", "alternate_golden"):
            if case.get(key):
                foreign.add(case[key])
    for other, entry in overrides.items():
        if other == backend:
            continue
        for key in ("golden", "alternate_golden"):
            if entry.get(key):
                foreign.add(entry[key])
    resolved = case_for_backend(case, backend)
    for key in ("golden", "alternate_golden"):
        foreign.discard(resolved.get(key))
    return foreign


def refuse_backend_overrides(cases, consumer):
    """Consumers that cannot express a per-backend golden must say so, not drop it silently.

    `add_trace_replay_test_for_backends` and the APK matrix both carry ONE golden path for both
    backends. Emitting a case that declares a per-backend golden through either of them would
    register the DirectVulkan arm against the DirectGLES image - the precise failure this key was
    added to close - so they refuse until they learn the key.
    """
    named = [case["name"] for case in cases if case.get("backend_overrides")]
    if named:
        raise ValueError(
            f"{consumer} cannot express backend_overrides yet, and the following case(s) declare "
            f"it: {', '.join(named)}. Teach {consumer} the key in the same commit that adds the "
            f"override, or the arm it emits runs against another backend's golden.")


def load_trace_cases(path=TRACE_CASES_JSON):
    return load_trace_case_manifest(path)["cases"]


def case_with_defaults(case):
    return dict(case)


def trace_case_names(path=TRACE_CASES_JSON):
    return [case["name"] for case in load_trace_cases(path)]


def find_trace_case(name, path=TRACE_CASES_JSON):
    for case in load_trace_cases(path):
        if case["name"] == name:
            return case
    raise KeyError(name)


def fixture_files(case, fixture_root):
    root = fixture_root.rstrip("/\\")
    files = [
        f"{root}/{case['trace_archive']}",
        f"{root}/{case['golden']}",
    ]
    if case.get("alternate_golden"):
        files.append(f"{root}/{case['alternate_golden']}")
    return files


def github_apk_case(case):
    refuse_backend_overrides([case], "the APK retrace matrix (github_apk_case)")
    result = dict(case)
    for key in ("trace_archive", "golden", "alternate_golden"):
        if result.get(key):
            result[key] = f"tools/trace_replay/fixtures/{result[key]}"
    return result


def ci_trace_cases(cases):
    return [case for case in cases if case.get("ci", True)]


def verify_trace_cases(cases):
    """The subset the third CI mode retraces.

    The verify build compares two state models at every verb boundary and again at every accessor
    read, which the design budgets at 5-10x, so the per-push lane runs a named subset and the full
    79-case sweep happens at the phase exit and on workflow_dispatch.
    """
    return [case for case in cases if case.get("verify", False)]


def split_trace_cases(cases):
    """The subset the split CI modes retrace - inproc AND spawn, both backends.

    P5's phase gate named exactly one case, OpenRA at SSIM >= 0.99, because one case was the
    work. P6 runs the whole CI matrix across a process boundary, so the subset is now every CI
    case that has not explicitly opted out. load_cases() has already resolved the default, so
    "split" is present and boolean on every case by the time this reads it.
    """
    return [case for case in cases if case.get("split", False)]


def ci_backends(case):
    backends = case.get("ci_backends")
    if backends is None:
        return CI_BACKENDS
    if not isinstance(backends, list) or not backends:
        raise ValueError(f"ci_backends must be a non-empty list for {case['name']}")
    unknown = [backend for backend in backends if backend not in CI_BACKENDS]
    if unknown:
        raise ValueError(
            f"unknown ci_backends for {case['name']}: {', '.join(unknown)}"
        )
    if len(set(backends)) != len(backends):
        raise ValueError(f"ci_backends contains duplicates for {case['name']}")
    return backends


def github_test_matrix(cases):
    return {
        "include": [
            {"backend": backend, "case": case["name"]}
            for case in cases
            for backend in ci_backends(case)
        ]
    }


def github_verify_matrix(cases):
    return github_test_matrix(verify_trace_cases(cases))


# The two transports the split retrace lane runs. inproc is the CONTROL - same library, same
# codec, same picture, server role on a thread HERE - and spawn is the claim. Both, always: a
# spawn failure that inproc also shows is a phase bug, and one inproc does not show is a
# transport bug, and collapsing the two makes that distinction unaskable.
SPLIT_TRANSPORTS = ("inproc", "spawn")


def github_split_matrix(cases, transports=SPLIT_TRANSPORTS, keep_inproc=()):
    """{backend, case, transport} for the split arm.

    THREE DIMENSIONS, NOT TWO. The job used to take {backend, case} and hard-code
    MOBILEGL_TRANSPORT=inproc in its own run block, so `spawn` had no way into CI at all. The
    backend list is the case's own ci_backends - the same list the monolith matrix uses - so a
    case restricted to one backend stays restricted here instead of being registered with a
    backend it does not run.

    `transports` narrows the arms (test.yml drops inproc when the inproc-acceptance retrace rows
    already replay the same library); `keep_inproc` names split cases whose inproc rows stay
    anyway - OpenRA's, which carry the wire-volume evidence. A narrowed matrix that comes out
    empty is an error; the full one is returned empty, and test.yml names that error itself.
    """
    unknown = [t for t in transports if t not in SPLIT_TRANSPORTS]
    if unknown or not transports:
        raise ValueError(f"split transports must be a non-empty subset of {SPLIT_TRANSPORTS}: {transports}")
    split_cases = split_trace_cases(cases)
    split_names = {case["name"] for case in split_cases}
    missing = [name for name in keep_inproc if name not in split_names]
    if missing:
        raise ValueError(f"--keep-inproc names cases that are not split CI cases: {missing}")
    rows = [
        {"backend": backend, "case": case["name"], "transport": transport}
        for case in split_cases
        for backend in ci_backends(case)
        for transport in SPLIT_TRANSPORTS
        if transport in transports or (transport == "inproc" and case["name"] in keep_inproc)
    ]
    narrowed = tuple(transports) != SPLIT_TRANSPORTS or bool(keep_inproc)
    if not rows and narrowed:
        raise ValueError("the narrowed split matrix is empty")
    return {"include": rows}


def github_apk_matrix(cases):
    backends = {
        "DirectGLES": {"name": "DirectGLES", "gpu": "software"},
        "DirectVulkan": {"name": "DirectVulkan", "gpu": "lavapipe"},
    }
    return {
        "include": [
            {"backend": backends[backend], "case": github_apk_case(case)}
            for case in cases
            for backend in ci_backends(case)
        ]
    }


# The Android lanes of the transport matrix. "full" replays every case on every transport.
# "inproc-full" replays every case on inproc, the transport the launcher ships, and only the
# manifest's android_smoke cases on monolith and spawn: the Linux retrace jobs replay every
# case on all three transports, so those two emulator lanes keep the cases that prove the
# Android side of each arm.
ANDROID_LANES = ("full", "inproc-full")


def github_transport_matrix(cases, *, apk=False, android_smoke=None):
    """Run the entire original matrix as control and as acceptance, once per transport.

    The transports use the same D/P artifact and each case's existing backend
    list. The legacy split:true subset does not limit this main acceptance matrix.
    With `android_smoke`, an APK matrix keeps every case on inproc and only the named
    cases on monolith and spawn.
    Nested APK case/backend metadata is deliberately left untouched.

    THE SPAWN LANE IS APK-ONLY, and that is a statement about where the evidence is missing
    rather than about what spawn can do. On Linux the spawn arm already has a job of its own -
    test.yml's `retrace-split`, which is {backend, case, transport} over spawn (and inproc too
    when it cannot reuse this matrix's inproc rows' library) and whose legs prove in-test that
    the transport resolved and, on OpenRA, that draw records crossed the wire
    (run_trace_case.cmake's evidence). Adding spawn to THIS matrix would pay
    for a second full Linux sweep that proves nothing the first one does not, and test.yml's
    `retrace` job has neither the server image staged nor MOBILEGL_IPC_SERVER_PATH set - so the
    lane would not merely be redundant, it would be red.

    On Android there is no such job. Until this lane existed, nothing in CI ever started a
    second MobileGL process on a device.
    """
    build = github_apk_matrix if apk else github_test_matrix
    if not cases:
        raise ValueError("transport matrix needs at least one trace case")
    reduced = cases
    if android_smoke is not None:
        if not apk:
            raise ValueError("only the APK transport matrix has reduced lanes")
        reduced = [case for case in cases if case["name"] in android_smoke]
        if not reduced:
            raise ValueError("the reduced Android lanes are empty: no case is in android_smoke")
    lanes = [
        (reduced, "monolith-control", "monolith"),
        (cases, "inproc-acceptance", "inproc"),
    ]
    if apk:
        lanes.append((reduced, "spawn-acceptance", "spawn"))
    return {"include": [
        {**row, "lane": lane, "transport": transport}
        for selected, lane, transport in lanes
        for row in build(selected)["include"]
    ]}


def self_test_transport_matrices():
    cases = ci_trace_cases(load_trace_cases())
    for apk in (False, True):
        build = github_apk_matrix if apk else github_test_matrix
        original = build(cases)["include"]
        mixed = github_transport_matrix(cases, apk=apk)["include"]
        controls = [row for row in mixed if row["transport"] == "monolith"]
        acceptance = [row for row in mixed if row["transport"] == "inproc"]
        strip_lane = lambda rows: [{key: value for key, value in row.items()
                                   if key not in ("lane", "transport")} for row in rows]
        spawned = [row for row in mixed if row["transport"] == "spawn"]
        assert strip_lane(controls) == original, "control lost original case/backend/parameters"
        assert strip_lane(acceptance) == original, "acceptance lost original case/backend/parameters"
        assert all(row["lane"] == "monolith-control" for row in controls)
        assert all(row["lane"] == "inproc-acceptance" for row in acceptance)
        # THE SPAWN LANE IS APK-ONLY AND MUST NOT DRIFT INTO THE LINUX MATRIX. test.yml's
        # `retrace` job exports matrix.transport straight into MOBILEGL_TRANSPORT and stages no
        # server image, so a spawn row reaching it would be red for a packaging reason and say
        # nothing about the transport. Asserted in both directions: present for apk, absent
        # otherwise - an accidental `if apk` removal has to be caught here rather than in CI.
        if apk:
            assert strip_lane(spawned) == original, "spawn lane lost original case/backend/parameters"
            assert all(row["lane"] == "spawn-acceptance" for row in spawned)
        else:
            assert not spawned, "the Linux transport matrix must carry no spawn lane"
        identities = [(row["case"]["name"] if apk else row["case"],
                       row["backend"]["name"] if apk else row["backend"], row["transport"])
                      for row in mixed]
        assert len(identities) == len(set(identities)), "duplicate lane identity"
        # A restricted case keeps exactly its original backend and golden data.
        restricted = [{**cases[0], "split": False, "ci_backends": ["DirectGLES"]}]
        rows = github_transport_matrix(restricted, apk=apk)["include"]
        expected_lanes = 3 if apk else 2
        assert len(rows) == expected_lanes
        assert all(strip_lane(rows[:1]) == strip_lane(rows[index:index + 1])
                   for index in range(1, expected_lanes))
        try:
            github_transport_matrix([], apk=apk)
        except ValueError:
            pass
        else:
            raise AssertionError("an empty acceptance lane silently passed")
        print(f"transport matrix {'APK' if apk else 'Linux'}: {len(controls)} unchanged controls, "
              f"{len(acceptance)} inproc acceptance entries, {len(spawned)} spawn acceptance "
              f"entries; exact metadata and negative controls OK")
    # The reduced Android lanes are the full matrix less the monolith and spawn rows of the
    # cases outside android_smoke: nothing else is dropped, changed or added.
    smoke = android_smoke_names()
    assert smoke, "android_smoke is empty, so the reduced monolith and spawn lanes would be too"
    full = github_transport_matrix(cases, apk=True)["include"]
    reduced = github_transport_matrix(cases, apk=True, android_smoke=smoke)["include"]
    kept = [row for row in full if row["transport"] == "inproc" or row["case"]["name"] in smoke]
    assert reduced == kept, "the reduced Android lanes are not the full matrix filtered"
    for transport in ("monolith", "spawn"):
        names = {row["case"]["name"] for row in reduced if row["transport"] == transport}
        assert names == set(smoke), f"the reduced {transport} lane is not the smoke cases: {names}"
    full_inproc = [row for row in full if row["transport"] == "inproc"]
    assert [row for row in reduced if row["transport"] == "inproc"] == full_inproc, \
        "the inproc lane was reduced"
    for bad in ({"apk": False, "android_smoke": smoke},
                {"apk": True, "android_smoke": ["no-such-case"]}):
        try:
            github_transport_matrix(cases, **bad)
        except ValueError:
            continue
        raise AssertionError(f"github_transport_matrix accepted {bad}")
    print(f"transport matrix APK reduced: {len(reduced)} of {len(full)} rows; inproc keeps all "
          f"{len(full_inproc)}, monolith and spawn keep the {len(smoke)} smoke cases")


def self_test_split_matrix(cases=None):
    """retrace-split may drop its inproc rows only because the inproc-acceptance rows replay them."""
    if cases is None:
        cases = ci_trace_cases(load_trace_cases())
    full = github_split_matrix(cases)["include"]
    spawn_only = github_split_matrix(cases, transports=("spawn",), keep_inproc=("OpenRA",))["include"]
    assert [r for r in spawn_only if r["transport"] == "spawn"] == [r for r in full if r["transport"] == "spawn"], \
        "the spawn-only matrix lost or changed a spawn row"
    assert any(r["transport"] == "spawn" for r in spawn_only), "the spawn-only matrix has no spawn row"
    kept = [r for r in spawn_only if r["transport"] == "inproc"]
    assert kept and all(r["case"] == "OpenRA" for r in kept), "the kept inproc rows are not OpenRA's"
    assert all(r in full for r in kept), "a kept inproc row is not a row of the full matrix"
    acceptance = {(r["backend"], r["case"]) for r in github_transport_matrix(cases)["include"]
                  if r["lane"] == "inproc-acceptance"}
    dropped = [(r["backend"], r["case"]) for r in full if r["transport"] == "inproc" and r not in kept]
    uncovered = [row for row in dropped if row not in acceptance]
    assert not uncovered, f"inproc rows dropped from retrace-split that no acceptance row replays: {uncovered}"
    for bad in ({"transports": ("unix",)}, {"transports": ()}, {"keep_inproc": ("no-such-case",)}):
        try:
            github_split_matrix(cases, **bad)
        except ValueError:
            continue
        raise AssertionError(f"github_split_matrix accepted {bad}")
    try:
        github_split_matrix([], transports=("spawn",))
    except ValueError:
        pass
    else:
        raise AssertionError("an empty narrowed split matrix silently passed")
    assert github_split_matrix([]) == {"include": []}, \
        "the full split matrix must come back empty, for the workflow's named error"
    print(f"split matrix: {len(full)} rows, {len(spawn_only)} with inproc dropped ({len(kept)} OpenRA inproc "
          f"kept); all {len(dropped)} dropped inproc rows are inproc-acceptance rows")


# --- Shards: several cases of one cell replayed by one CI job ---------------------------------
#
# A cell is everything a per-case matrix row carries except its case: the backend, and the lane
# and transport where the matrix has them. A shard row keeps the cell's keys unchanged and adds
# the cases it replays, so the job that runs it reads the same fields a per-case leg reads.
# Every shard matrix is built from the per-case matrix it replaces and is refused unless its
# rows, taken apart again, are exactly that matrix.

TRACE_DURATION_HINTS_JSON = Path(__file__).with_name("trace_duration_hints.json")

# Shards per cell on Linux, the one tunable of that layout. A cell with little to replay gets
# fewer: a shard costs about half a minute of setup, so it is not split below
# MIN_SHARD_SECONDS of replay.
DEFAULT_SHARDS_PER_CELL = 4
MIN_SHARD_SECONDS = 90

# Replay seconds one emulator shard aims for, the one tunable of the Android layout. A cell is
# split into as many shards as that takes.
DEFAULT_ANDROID_SHARD_SECONDS = 580

# The hint a row without a transport uses. The verify lane has timings of its own.
HINT_KIND_MONOLITH = "monolith"
HINT_KIND_VERIFY = "verify"

# A shard's case names reach a shell command line, so the characters are restricted.
SHARD_SAFE_NAME = frozenset("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-")


def load_duration_hints(platform, path=TRACE_DURATION_HINTS_JSON):
    """{case: {backend: {kind: seconds}}} for "linux" or "android"; empty when there is none."""
    path = Path(path)
    if not path.is_file():
        return {}
    with path.open("r", encoding="utf-8") as file:
        table = json.load(file).get(platform, {}).get("seconds", {})
    for case, backends in table.items():
        for backend, kinds in backends.items():
            for kind, seconds in kinds.items():
                if isinstance(seconds, bool) or not isinstance(seconds, (int, float)) or seconds <= 0:
                    raise ValueError(f"duration hint for {case} {backend} {kind} must be a positive "
                                     f"number: {seconds!r}")
    return table


def median(values):
    ordered = sorted(values)
    if not ordered:
        return None
    middle = len(ordered) // 2
    if len(ordered) % 2:
        return ordered[middle]
    return (ordered[middle - 1] + ordered[middle]) / 2


def hint_seconds(hints, case, backend, kind):
    """The hinted replay time of one row, or the median of its backend and kind, or of everything."""
    try:
        return hints[case][backend][kind]
    except KeyError:
        pass
    same_cell = [kinds[kind] for backends in hints.values()
                 for name, kinds in backends.items() if name == backend and kind in kinds]
    everything = [seconds for backends in hints.values() for kinds in backends.values()
                  for seconds in kinds.values()]
    return median(same_cell) or median(everything) or 30


def row_case_name(row):
    case = row["case"]
    return case["name"] if isinstance(case, dict) else case


def row_backend_name(row):
    backend = row["backend"]
    return backend["name"] if isinstance(backend, dict) else backend


def row_cell(row):
    return {key: value for key, value in row.items() if key != "case"}


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"))


def shard_rows_as_cases(shard_rows):
    """Take shard rows apart into the per-case rows they stand for."""
    added = ("shard", "shards", "names", "cases", "seconds")
    rows = []
    for shard in shard_rows:
        cell = {key: value for key, value in shard.items() if key not in added}
        for case in shard.get("cases", shard["names"]):
            rows.append({**cell, "case": case})
    return rows


def check_shard_union(rows, shard_rows):
    """Refuse a shard matrix that is not exactly the per-case matrix `rows`, regrouped."""
    expected = sorted(canonical(row) for row in rows)
    actual = sorted(canonical(row) for row in shard_rows_as_cases(shard_rows))
    if expected != actual:
        missing = sorted(set(expected) - set(actual))
        extra = sorted(set(actual) - set(expected))
        raise ValueError(
            f"the shard matrix is not the per-case matrix regrouped: {len(expected)} per-case rows, "
            f"{len(actual)} rows in the shards; missing {missing[:3]}, unexpected {extra[:3]}")
    if len(set(expected)) != len(expected):
        raise ValueError("the per-case matrix repeats a row, so a shard would replay it twice")
    cells = {}
    for shard in shard_rows:
        if not shard["names"]:
            raise ValueError(f"empty shard: {shard}")
        if "cases" in shard and [case["name"] for case in shard["cases"]] != shard["names"]:
            raise ValueError(f"a shard's names and cases disagree: {shard['names']}")
        cell = canonical({key: value for key, value in shard.items()
                          if key not in ("shard", "shards", "names", "cases", "seconds")})
        cells.setdefault(cell, []).append((shard["shard"], shard["shards"]))
    for cell, numbers in cells.items():
        count = len(numbers)
        if sorted(numbers) != [(index, count) for index in range(1, count + 1)]:
            raise ValueError(f"shards of {cell} are not numbered 1..{count} of {count}: {numbers}")


def shard_matrix(rows, hints, *, default_kind=HINT_KIND_MONOLITH, shards=None, shard_seconds=None):
    """Regroup a per-case matrix into shards of one cell each.

    Exactly one of `shards` (shards per cell, capped so that none holds less than
    MIN_SHARD_SECONDS of hinted replay) and `shard_seconds` (hinted replay per shard) sets the
    layout. Cases go longest first into the shard with the least hinted time so far, which is
    deterministic and keeps the shards of a cell even; inside a shard they run in matrix order.
    """
    if (shards is None) == (shard_seconds is None):
        raise ValueError("give exactly one of shards and shard_seconds")
    if shards is not None and shards < 1:
        raise ValueError(f"shards per cell must be at least 1: {shards}")
    if shard_seconds is not None and shard_seconds < 1:
        raise ValueError(f"seconds per shard must be at least 1: {shard_seconds}")
    cells = {}
    for index, row in enumerate(rows):
        name = row_case_name(row)
        if not name or not set(name) <= SHARD_SAFE_NAME:
            raise ValueError(f"case name {name!r} cannot be passed on a command line; allowed "
                             f"characters are letters, digits, '.', '_' and '-'")
        seconds = hint_seconds(hints, name, row_backend_name(row),
                               row.get("transport", default_kind))
        cells.setdefault(canonical(row_cell(row)), []).append((index, row, seconds))
    out = []
    for members in cells.values():
        total = sum(seconds for _, _, seconds in members)
        if shards is not None:
            count = min(shards, int(total // MIN_SHARD_SECONDS))
        else:
            count = -(-int(total) // shard_seconds)
        count = max(1, min(count, len(members)))
        bins = [[] for _ in range(count)]
        loads = [0] * count
        for member in sorted(members, key=lambda item: (-item[2], item[0])):
            target = loads.index(min(loads))
            bins[target].append(member)
            loads[target] += member[2]
        for number, members_of_shard in enumerate(bins, start=1):
            members_of_shard.sort(key=lambda item: item[0])
            shard = {**row_cell(members_of_shard[0][1]), "shard": number, "shards": count,
                     "names": [row_case_name(row) for _, row, _ in members_of_shard]}
            if isinstance(members_of_shard[0][1]["case"], dict):
                shard["cases"] = [row["case"] for _, row, _ in members_of_shard]
            shard["seconds"] = round(sum(seconds for _, _, seconds in members_of_shard))
            out.append(shard)
    check_shard_union(rows, out)
    return {"include": out}


def per_case_matrix(format_name, cases, *, transports=SPLIT_TRANSPORTS, keep_inproc=(),
                    android_smoke=None):
    """The per-case matrix a shard format regroups, and the hint kind of its transport-less rows."""
    if format_name == "github-test-shard-matrix":
        return github_test_matrix(cases), HINT_KIND_MONOLITH
    if format_name == "github-test-transport-shard-matrix":
        return github_transport_matrix(cases), HINT_KIND_MONOLITH
    if format_name == "github-verify-shard-matrix":
        return github_verify_matrix(cases), HINT_KIND_VERIFY
    if format_name == "github-split-shard-matrix":
        return github_split_matrix(cases, transports, keep_inproc), HINT_KIND_MONOLITH
    if format_name == "github-apk-shard-matrix":
        return github_apk_matrix(cases), HINT_KIND_MONOLITH
    if format_name == "github-apk-transport-shard-matrix":
        return (github_transport_matrix(cases, apk=True, android_smoke=android_smoke),
                HINT_KIND_MONOLITH)
    raise ValueError(f"not a shard format: {format_name}")


LINUX_SHARD_FORMATS = (
    "github-test-shard-matrix",
    "github-test-transport-shard-matrix",
    "github-verify-shard-matrix",
    "github-split-shard-matrix",
)
ANDROID_SHARD_FORMATS = (
    "github-apk-shard-matrix",
    "github-apk-transport-shard-matrix",
)


def github_shard_matrix(format_name, cases, *, shards=DEFAULT_SHARDS_PER_CELL,
                        shard_seconds=DEFAULT_ANDROID_SHARD_SECONDS,
                        transports=SPLIT_TRANSPORTS, keep_inproc=(), hints_path=TRACE_DURATION_HINTS_JSON,
                        android_smoke=None):
    """A shard format: the Linux ones lay out by shards per cell, the Android ones by seconds."""
    matrix, default_kind = per_case_matrix(format_name, cases, transports=transports,
                                           keep_inproc=keep_inproc, android_smoke=android_smoke)
    if format_name in ANDROID_SHARD_FORMATS:
        return shard_matrix(matrix["include"], load_duration_hints("android", hints_path),
                            default_kind=default_kind, shard_seconds=shard_seconds)
    return shard_matrix(matrix["include"], load_duration_hints("linux", hints_path),
                        default_kind=default_kind, shards=shards)


def self_test_shard_matrices():
    """Every shard format replays exactly the rows of the per-case matrix it replaces."""
    cases = ci_trace_cases(load_trace_cases())
    narrowed = {"transports": ("spawn",), "keep_inproc": ("OpenRA",)}
    layouts = [(name, {}, {"shards": count}) for name in LINUX_SHARD_FORMATS for count in (1, 4, 100)]
    layouts += [("github-split-shard-matrix", narrowed, {"shards": count}) for count in (1, 4, 100)]
    layouts += [(name, {}, {"shard_seconds": seconds}) for name in ANDROID_SHARD_FORMATS
                for seconds in (60, DEFAULT_ANDROID_SHARD_SECONDS, 10 ** 9)]
    layouts += [("github-apk-transport-shard-matrix", {"android_smoke": android_smoke_names()},
                 {"shard_seconds": seconds}) for seconds in (60, DEFAULT_ANDROID_SHARD_SECONDS, 10 ** 9)]
    for name, selection, layout in layouts:
        matrix, _ = per_case_matrix(name, cases, **selection)
        rows = matrix["include"]
        built = github_shard_matrix(name, cases, **selection, **layout)["include"]
        again = github_shard_matrix(name, cases, **selection, **layout)["include"]
        assert built == again, f"{name} {layout}: the layout is not deterministic"
        # Compared here as well as inside shard_matrix, with nothing shared but the JSON form.
        replayed = sorted(
            canonical({**{key: value for key, value in shard.items()
                          if key not in ("shard", "shards", "names", "cases", "seconds")},
                       "case": case})
            for shard in built for case in shard.get("cases", shard["names"]))
        assert replayed == sorted(canonical(row) for row in rows), \
            f"{name} {layout}: the shards do not replay the per-case matrix"
        assert sum(len(shard["names"]) for shard in built) == len(rows)
        cells = {canonical(row_cell(row)) for row in rows}
        assert len({canonical({key: value for key, value in shard.items()
                               if key not in ("shard", "shards", "names", "cases", "seconds")})
                    for shard in built}) == len(cells), f"{name} {layout}: a shard spans two cells"
        if layout.get("shards") == 1 or layout.get("shard_seconds") == 10 ** 9:
            assert len(built) == len(cells), f"{name} {layout}: expected one shard per cell"
        seconds = [shard["seconds"] for shard in built]
        shown = {key: (f"{len(value)} smoke cases" if key == "android_smoke" else value)
                 for key, value in selection.items()}
        print(f"shard matrix {name} {shown or ''} {layout}: {len(rows)} rows in {len(built)} "
              f"shards over {len(cells)} cells, {min(seconds)}-{max(seconds)} s hinted per shard")
        # The check itself must be able to fail: a dropped, a repeated and a moved case.
        if len(built) >= 2 and any(len(shard["names"]) >= 2 for shard in built):
            index = next(i for i, shard in enumerate(built) if len(shard["names"]) >= 2)
            for label, damage in (("dropped", lambda names: names[1:]),
                                  ("repeated", lambda names: names + names[:1])):
                broken = [dict(shard) for shard in built]
                broken[index]["names"] = damage(list(broken[index]["names"]))
                if "cases" in broken[index]:
                    broken[index]["cases"] = damage(list(broken[index]["cases"]))
                try:
                    check_shard_union(rows, broken)
                except ValueError:
                    pass
                else:
                    raise AssertionError(f"{name} {layout}: a {label} case passed the union check")
            other = next((i for i, shard in enumerate(built)
                          if row_backend_name(shard) != row_backend_name(built[index])), None)
            if other is not None:
                broken = [dict(shard) for shard in built]
                for key in ("names", "cases"):
                    if key in broken[index]:
                        moved = list(broken[index][key])
                        broken[other][key] = list(broken[other][key]) + moved[:1]
                        broken[index][key] = moved[1:]
                try:
                    check_shard_union(rows, broken)
                except ValueError:
                    pass
                else:
                    raise AssertionError(f"{name} {layout}: a case moved to another backend's "
                                         f"shard passed the union check")
    # Hints only weigh the packing. With none at all, and with a case no hint names, the rows
    # replayed are the same.
    rows = github_transport_matrix(cases)["include"]
    for hints in ({}, {"no-such-case": {"DirectGLES": {"monolith": 5}}}):
        check_shard_union(rows, shard_matrix(rows, hints, shards=4)["include"])
    assert hint_seconds({}, "x", "DirectGLES", "monolith") == 30
    assert hint_seconds({"a": {"DirectGLES": {"monolith": 10}}, "b": {"DirectGLES": {"monolith": 20}},
                         "c": {"DirectVulkan": {"monolith": 90}}}, "x", "DirectGLES", "monolith") == 15
    for bad in ({"shards": 0}, {"shard_seconds": 0}, {}, {"shards": 4, "shard_seconds": 60}):
        try:
            shard_matrix(rows, {}, **bad)
        except ValueError:
            continue
        raise AssertionError(f"shard_matrix accepted {bad}")
    try:
        shard_matrix([{"backend": "DirectGLES", "case": "bad name; rm"}], {}, shards=1)
    except ValueError:
        pass
    else:
        raise AssertionError("a case name with shell characters was accepted")
    # The verify lane's corrupted-snapshot control replays OpenRA on DirectGLES, so that row has
    # to be in the verify matrix for the control to have a shard to run in.
    verify_rows = github_verify_matrix(cases)["include"]
    assert {"backend": "DirectGLES", "case": "OpenRA"} in verify_rows, \
        "the verify matrix has no OpenRA DirectGLES row, so its negative control would not run"
    print("shard matrices: union, numbering, determinism and negative controls OK")


def cmake_quote(value):
    return '"' + str(value).replace("\\", "/").replace('"', '\\"') + '"'


def emit_cmake(cases, fixture_root):
    refuse_backend_overrides(cases, "the CTest catalog emitter (emit_cmake)")
    root = fixture_root.rstrip("/\\")
    lines = ["# Generated from tools/trace_replay/trace_cases.json.", ""]
    keys = [
        ("TRACE_ARCHIVE", "trace_archive", True),
        ("TRACE_FILE", "trace_file", False),
        ("GOLDEN", "golden", True),
        ("ALTERNATE_GOLDEN", "alternate_golden", True),
        ("TARGET_CALL", "target_call", False),
        ("WIDTH", "width", False),
        ("HEIGHT", "height", False),
        ("SSIM_THRESHOLD", "ssim_threshold", False),
        ("CROP_X", "crop_x", False),
        ("CROP_Y", "crop_y", False),
        ("CROP_WIDTH", "crop_width", False),
        ("CROP_HEIGHT", "crop_height", False),
        ("COHERENT_AS_FLUSH", "coherent_as_flush", False),
    ]
    def emit_one(function, case):
        lines.append(f"{function}({cmake_quote(case['name'])}")
        for cmake_key, json_key, fixture_path in keys:
            value = case.get(json_key)
            if value is None or value == "":
                continue
            if fixture_path:
                value = f"{root}/{value}"
            lines.append(f"        {cmake_key} {cmake_quote(value)}")
        lines.append(")")
        lines.append("")

    for case in cases:
        emit_one("add_trace_replay_test_for_backends", case)
        # P5's split arm, emitted beside the monolith pair rather than in a block of its own so
        # that a case and its variant always carry identical parameters. The CMake side is a
        # no-op unless MOBILEGL_BUILD_DISAGGREGATED is ON.
        if case.get("split", False):
            emit_one("add_trace_replay_split_test", case)
    return "\n".join(lines)


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--case", dest="case_name", help="Trace case name.")
    parser.add_argument("--ci", action="store_true", help="Only include cases enabled for CI.")
    parser.add_argument("--self-test-transport-matrices", action="store_true")
    parser.add_argument("--self-test-split-matrix", action="store_true",
                        help="prove the spawn-only split matrix drops only inproc-acceptance rows")
    parser.add_argument("--self-test-shard-matrices", action="store_true",
                        help="prove every shard matrix replays exactly its per-case matrix")
    parser.add_argument("--fixture-root", default="tools/trace_replay/fixtures")
    parser.add_argument("--transports", default=",".join(SPLIT_TRANSPORTS),
                        help="github-split-matrix: the comma-separated split transports to emit")
    parser.add_argument("--keep-inproc", default="",
                        help="github-split-matrix: comma-separated split cases whose inproc rows stay")
    parser.add_argument("--shards", type=int, default=DEFAULT_SHARDS_PER_CELL,
                        help="Linux shard formats: shards per (lane, transport, backend) cell")
    parser.add_argument("--shard-seconds", type=int, default=DEFAULT_ANDROID_SHARD_SECONDS,
                        help="Android shard formats: hinted replay seconds one shard aims for")
    parser.add_argument("--android-lanes", choices=ANDROID_LANES, default="full",
                        help="APK transport formats: inproc-full keeps every case on inproc and "
                             "the manifest's android_smoke cases on monolith and spawn")
    parser.add_argument("--duration-hints", default=str(TRACE_DURATION_HINTS_JSON),
                        help="shard formats: the JSON of replay seconds the packing weighs cases by")
    parser.add_argument(
        "--format",
        choices=(
            "names",
            "android-smoke-names",
            "github-test-matrix",
            "github-test-transport-matrix",
            "github-verify-matrix",
            "github-split-matrix",
            "github-apk",
            "github-apk-matrix",
            "github-apk-transport-matrix",
            *LINUX_SHARD_FORMATS,
            *ANDROID_SHARD_FORMATS,
            "fixture-files",
            "fixture-manifest",
            "cmake",
        ),
        default="names",
    )
    return parser.parse_args()


def main():
    args = parse_args()
    if args.self_test_transport_matrices:
        self_test_transport_matrices()
        return 0
    if args.self_test_split_matrix:
        self_test_split_matrix()
        return 0
    if args.self_test_shard_matrices:
        self_test_shard_matrices()
        return 0
    cases = load_trace_cases()
    if args.ci:
        cases = ci_trace_cases(cases)
    android_smoke = android_smoke_names() if args.android_lanes == "inproc-full" else None
    if args.format == "names":
        print(json.dumps([case["name"] for case in cases], separators=(",", ":")))
    elif args.format == "android-smoke-names":
        print(",".join(android_smoke_names()))
    elif args.format == "github-test-matrix":
        print(json.dumps(github_test_matrix(cases), separators=(",", ":")))
    elif args.format == "github-test-transport-matrix":
        print(json.dumps(github_transport_matrix(cases), separators=(",", ":")))
    elif args.format == "github-verify-matrix":
        print(json.dumps(github_verify_matrix(cases), separators=(",", ":")))
    elif args.format == "github-split-matrix":
        transports = tuple(t for t in args.transports.split(",") if t)
        keep = tuple(c for c in args.keep_inproc.split(",") if c)
        print(json.dumps(github_split_matrix(cases, transports, keep), separators=(",", ":")))
    elif args.format == "github-apk":
        print(json.dumps([github_apk_case(case) for case in cases], separators=(",", ":")))
    elif args.format == "github-apk-matrix":
        print(json.dumps(github_apk_matrix(cases), separators=(",", ":")))
    elif args.format == "github-apk-transport-matrix":
        print(json.dumps(github_transport_matrix(cases, apk=True, android_smoke=android_smoke),
                         separators=(",", ":")))
    elif args.format in LINUX_SHARD_FORMATS + ANDROID_SHARD_FORMATS:
        transports = tuple(t for t in args.transports.split(",") if t)
        keep = tuple(c for c in args.keep_inproc.split(",") if c)
        print(json.dumps(
            github_shard_matrix(args.format, cases, shards=args.shards,
                                shard_seconds=args.shard_seconds, transports=transports,
                                keep_inproc=keep, hints_path=args.duration_hints,
                                android_smoke=android_smoke),
            separators=(",", ":")))
    elif args.format == "fixture-manifest":
        # One "<case><TAB><file>" line per fixture file, for the fixture bundle script.
        for case in cases:
            for file in fixture_files(case, args.fixture_root):
                print(f"{case['name']}\t{file}")
    elif args.format == "fixture-files":
        if not args.case_name:
            print("--case is required for --format fixture-files", file=sys.stderr)
            return 2
        try:
            case = find_trace_case(args.case_name)
        except KeyError:
            print(f"unknown trace case: {args.case_name}", file=sys.stderr)
            return 2
        print("\n".join(fixture_files(case, args.fixture_root)))
    elif args.format == "cmake":
        print(emit_cmake(cases, args.fixture_root))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
