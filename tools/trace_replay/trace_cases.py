#!/usr/bin/env python3
import argparse
import json
import sys
from pathlib import Path


TRACE_CASES_JSON = Path(__file__).with_name("trace_cases.json")
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
    return {"defaults": defaults, "cases": cases}


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


def github_split_matrix(cases):
    """{backend, case, transport} for the split arm.

    THREE DIMENSIONS, NOT TWO. The job used to take {backend, case} and hard-code
    MOBILEGL_TRANSPORT=inproc in its own run block, so `spawn` had no way into CI at all. The
    backend list is the case's own ci_backends - the same list the monolith matrix uses - so a
    case restricted to one backend stays restricted here instead of being registered with a
    backend it does not run.
    """
    return {
        "include": [
            {"backend": backend, "case": case["name"], "transport": transport}
            for case in split_trace_cases(cases)
            for backend in ci_backends(case)
            for transport in SPLIT_TRANSPORTS
        ]
    }


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


def github_transport_matrix(cases, *, apk=False):
    """Run the entire original matrix as control and as acceptance, once per transport.

    The transports use the same D/P artifact and each case's existing backend
    list. The legacy split:true subset does not limit this main acceptance matrix.
    Nested APK case/backend metadata is deliberately left untouched.

    THE SPAWN LANE IS APK-ONLY, and that is a statement about where the evidence is missing
    rather than about what spawn can do. On Linux the spawn arm already has a job of its own -
    test.yml's `retrace-split`, which is {backend, case, transport} over inproc AND spawn and
    carries the two negative controls that make it mean anything (the pull library must red it,
    dropping the draws must red it). Adding spawn to THIS matrix as well would pay for a second
    full Linux sweep that proves nothing the first one does not, and test.yml's `retrace` job
    has neither the server image staged nor MOBILEGL_IPC_SERVER_PATH set - so the lane would
    not merely be redundant, it would be red.

    On Android there is no such job. Until this lane existed, nothing in CI ever started a
    second MobileGL process on a device.
    """
    build = github_apk_matrix if apk else github_test_matrix
    if not cases:
        raise ValueError("transport matrix needs at least one trace case")
    lanes = [
        (cases, "monolith-control", "monolith"),
        (cases, "inproc-acceptance", "inproc"),
    ]
    if apk:
        lanes.append((cases, "spawn-acceptance", "spawn"))
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
    parser.add_argument("--fixture-root", default="tools/trace_replay/fixtures")
    parser.add_argument(
        "--format",
        choices=(
            "names",
            "github-test-matrix",
            "github-test-transport-matrix",
            "github-verify-matrix",
            "github-split-matrix",
            "github-apk",
            "github-apk-matrix",
            "github-apk-transport-matrix",
            "fixture-files",
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
    cases = load_trace_cases()
    if args.ci:
        cases = ci_trace_cases(cases)
    if args.format == "names":
        print(json.dumps([case["name"] for case in cases], separators=(",", ":")))
    elif args.format == "github-test-matrix":
        print(json.dumps(github_test_matrix(cases), separators=(",", ":")))
    elif args.format == "github-test-transport-matrix":
        print(json.dumps(github_transport_matrix(cases), separators=(",", ":")))
    elif args.format == "github-verify-matrix":
        print(json.dumps(github_verify_matrix(cases), separators=(",", ":")))
    elif args.format == "github-split-matrix":
        print(json.dumps(github_split_matrix(cases), separators=(",", ":")))
    elif args.format == "github-apk":
        print(json.dumps([github_apk_case(case) for case in cases], separators=(",", ":")))
    elif args.format == "github-apk-matrix":
        print(json.dumps(github_apk_matrix(cases), separators=(",", ":")))
    elif args.format == "github-apk-transport-matrix":
        print(json.dumps(github_transport_matrix(cases, apk=True), separators=(",", ":")))
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
