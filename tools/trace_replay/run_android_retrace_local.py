#!/usr/bin/env python3
import argparse
import datetime
import hashlib
import json
import math
import shutil
import subprocess
import sys
from pathlib import Path

from trace_cases import (case_for_backend, case_with_defaults, ci_backends, ci_trace_cases,
                         load_trace_cases, split_trace_cases)


ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / "tools" / "trace_replay" / "fixtures"
RESULT_ROOT = ROOT / ".trace-work" / "android-retrace-result"
FIXTURE_ROOT = ROOT / ".trace-work" / "android-retrace-fixture"
SUMMARY_DIR = ROOT / ".trace-work" / "android-retrace-summary"
SUMMARY_HTML = "mobilegl-android-retrace-overview.html"
DEFAULT_ANGLE_VARIANT = "ec889e6ea831"
BLISS_ANGLE_VARIANT = "90a62123d794"
BLISS_CASE = "minecraft-1.21.4-fabric-iris-bliss-in-world"
# BOTH OUTPUT DIRECTORIES, newest wins. The trace variant is assembled as a RELEASE build
# (`:app:assembleTraceRelease`) and Gradle puts it under apk/trace/release, but CI has historically
# staged the same artifact under apk/trace/debug - so a tree that has built locally has two, and
# looking in one of them silently retraces the other one's APK. That is the "ran the wrong binary
# and went green" shape, on the platform where it is hardest to notice: nothing about the result
# says which APK produced it.
TRACE_APK_DIRS = (
    ROOT / "android-plugin" / "app" / "build" / "outputs" / "apk" / "trace" / "release",
    ROOT / "android-plugin" / "app" / "build" / "outputs" / "apk" / "trace" / "debug",
)
TRACE_APK_DIR = TRACE_APK_DIRS[1]

BACKENDS = {
    "DirectGLES": {
        "package": "top.mobilegl.plugin.trace",
        "use_angle": False,
        "use_pbuffer": False,
    },
    "DirectVulkan": {
        "package": "top.mobilegl.plugin.trace",
        "use_angle": False,
        "use_pbuffer": False,
    },
}

# The trace APK's application id, overridable so a development build can be installed BESIDE an
# existing trace install instead of replacing it.
#
# WHY IT IS NEEDED: `-Pmobilegl.applicationIdSuffix=<x>` is the only way to put two trace APKs on one
# device, and two APKs signed by different keys cannot share an id at all (`adb install -r` fails
# with INSTALL_FAILED_UPDATE_INCOMPATIBLE). A machine that already carries a trace install under the
# canonical id - signed by whichever key was current when it was made - otherwise has to have that
# install destroyed before an A/B can run, which is destructive and, on a shared bench, rude.
#
# ABSENT MEANS THE CANONICAL ID, so every existing caller keeps the behaviour it had. What the
# override does NOT change: the Activity class, the intent action, the extras, the on-device app dir
# and the arm-proof markers all stay identical, so the lane under test is the real lane. The one
# thing that must move with it is `--package` in the same command, because trace-replay-ci.sh drives
# `am start`, `run-as` and `force-stop` off it. tools/device_bench/p6/ab_session.py sets both.
_trace_package_override = __import__("os").environ.get("MOBILEGL_TRACE_PACKAGE", "").strip()
if _trace_package_override:
    for _backend in BACKENDS.values():
        _backend["package"] = _trace_package_override

CASES = load_trace_cases()


def safe_case(name):
    return "".join(ch if ch.isalnum() or ch in "._-" else "_" for ch in name)


def is_lfs_pointer(path):
    return path.exists() and path.read_bytes()[:80].startswith(b"version https://git-lfs.github.com/spec/v1")


def find_trace_apk():
    # A preinstalled APK need not be the newest Gradle output in this tree.
    # Let the caller pin the exact signed file used for the session so run.json
    # does not present an unrelated local build's SHA as device provenance.
    override = __import__("os").environ.get("MOBILEGL_TRACE_APK", "").strip()
    if override:
        path = Path(override).expanduser().resolve()
        if not path.is_file():
            raise FileNotFoundError(f"MOBILEGL_TRACE_APK does not exist: {path}")
        return path
    candidates = [
        path
        for directory in TRACE_APK_DIRS
        for path in directory.glob("MobileGL-plugin-trace-release-*.apk")
    ]
    return max(candidates, key=lambda path: path.stat().st_mtime) if candidates else None


def bash_path(path):
    path = Path(path).resolve()
    drive = path.drive.rstrip(":").lower()
    parts = path.parts[1:]
    return "/" + drive + "/" + "/".join(parts)


def mark_skipped(case, backend, reason):
    result_dir = RESULT_ROOT / f"{safe_case(case['name'])}-{backend}"
    result_dir.mkdir(parents=True, exist_ok=True)
    result = {
        "passed": False,
        "statusCode": 2,
        "message": reason,
        "tracePath": str(FIXTURES / case["trace_archive"]),
        "goldenPath": str(FIXTURES / case["golden"]),
        "alternateGoldenPaths": [],
        "matchedGoldenPath": "",
        "actualPath": "",
        "diffPath": "",
        "backend": backend,
        "angleVariant": (
            BLISS_ANGLE_VARIANT if case["name"] == BLISS_CASE else DEFAULT_ANGLE_VARIANT
        ) if BACKENDS[backend]["use_angle"] else "",
        "targetCall": case["target_call"],
        "width": case["width"],
        "height": case["height"],
        "cropX": case["crop_x"],
        "cropY": case["crop_y"],
        "cropWidth": case["crop_width"],
        "cropHeight": case["crop_height"],
        "ssim": -1,
        "ssimThreshold": float(case["ssim_threshold"]),
        "mismatchPixels": -1,
    }
    (result_dir / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")


def copy_goldens(case, backend):
    result_dir = RESULT_ROOT / f"{safe_case(case['name'])}-{backend}"
    result_dir.mkdir(parents=True, exist_ok=True)
    for key, suffix in (("golden", "golden"), ("alternate_golden", "alternate-golden")):
        value = case.get(key)
        if not value:
            continue
        source = FIXTURES / value
        if source.exists() and source.stat().st_size > 0:
            shutil.copyfile(source, result_dir / f"{safe_case(case['name'])}-{backend}-{suffix}.png")


def render_summary():
    SUMMARY_DIR.mkdir(parents=True, exist_ok=True)
    command = [
        "node",
        str(ROOT / "tools" / "trace_replay" / "render_retrace_summary.mjs"),
        "--input",
        str(RESULT_ROOT),
        "--output-dir",
        str(SUMMARY_DIR),
        "--title",
        "MobileGL Android retrace overview",
        "--group-label",
        "Android Device",
        "--html",
        SUMMARY_HTML,
    ]
    subprocess.run(command, cwd=ROOT, check=True)
    shutil.copyfile(SUMMARY_DIR / SUMMARY_HTML, SUMMARY_DIR / "index.html")


def run_case(case, backend, extra_args=None, timeout_seconds=None, env_overrides=None,
             use_pbuffer=False):
    backend_info = BACKENDS[backend]
    apk = find_trace_apk()
    trace_archive = FIXTURES / case["trace_archive"]
    golden = FIXTURES / case["golden"]
    alternate = FIXTURES / case["alternate_golden"] if case.get("alternate_golden") else None
    if apk is None:
        mark_skipped(case, backend, "SKIPPED_MISSING_APK: no trace APK found under "
                     + " or ".join(str(directory) for directory in TRACE_APK_DIRS))
        return 2
    if not trace_archive.exists() or is_lfs_pointer(trace_archive):
        mark_skipped(case, backend, "SKIPPED_LFS_POINTER: trace archive is missing or still an LFS pointer")
        copy_goldens(case, backend)
        return 2
    if not golden.exists() or is_lfs_pointer(golden):
        mark_skipped(case, backend, "SKIPPED_LFS_POINTER: golden image is missing or still an LFS pointer")
        return 2
    if alternate is not None and (not alternate.exists() or is_lfs_pointer(alternate)):
        alternate = None

    command = [
        "C:/Program Files/Git/bin/bash.exe",
        "android-plugin/trace-replay-ci.sh",
        "--apk-file",
        bash_path(apk),
        "--package",
        backend_info["package"],
        "--backend",
        backend,
        "--result-root",
        bash_path(RESULT_ROOT),
        "--fixture-root",
        bash_path(FIXTURE_ROOT),
        "--case",
        case["name"],
        "--trace-archive",
        bash_path(trace_archive),
        "--trace-file",
        case["trace_file"],
        "--golden",
        bash_path(golden),
        "--target-call",
        str(case["target_call"]),
        "--width",
        str(case["width"]),
        "--height",
        str(case["height"]),
        "--ssim-threshold",
        str(case["ssim_threshold"]),
        "--crop-x",
        str(case["crop_x"]),
        "--crop-y",
        str(case["crop_y"]),
        "--crop-width",
        str(case["crop_width"]),
        "--crop-height",
        str(case["crop_height"]),
        "--timeout-seconds",
        str(timeout_seconds if timeout_seconds is not None else case["timeout_seconds"]),
    ]
    command.extend(extra_args or [])
    if alternate is not None:
        command[command.index("--target-call"):command.index("--target-call")] = ["--alternate-golden", bash_path(alternate)]
    if backend_info["use_pbuffer"] or use_pbuffer:
        command.append("--use-pbuffer")
    if backend_info["use_angle"] and case["name"] == BLISS_CASE:
        command.append("--avoid-angle-llvmpipe-sampler-mipmap-min-filter")
    if backend_info["use_angle"] and case.get("avoid_angle_llvmpipe_explicit_lod_bias"):
        command.append("--avoid-angle-llvmpipe-explicit-lod-bias")
    if case.get("coherent_as_flush"):
        command.append("--coherent-as-flush")
    # Generic environment passthrough: --env MOBILEGL_FOO=1 needs no per-knob plumbing in
    # this script, in trace-replay-ci.sh, in the Activity, in the JNI marshalling or in the
    # runner - one extra carries them all.
    if env_overrides:
        command.extend(["--env", ";".join(env_overrides)])
    env = dict(**__import__("os").environ)
    env["PYTHON"] = "python"
    env["MSYS2_ARG_CONV_EXCL"] = "/data/*"
    if backend_info["use_angle"]:
        env["MOBILEGL_ESPRYT_USE_ANGLE"] = "1"
        env["MOBILEGL_TRACE_ANGLE_VARIANT"] = (
            BLISS_ANGLE_VARIANT if case["name"] == BLISS_CASE else DEFAULT_ANGLE_VARIANT
        )
    else:
        env.pop("MOBILEGL_ESPRYT_USE_ANGLE", None)
        env.pop("MOBILEGL_TRACE_ANGLE_VARIANT", None)
    result = subprocess.run(command, cwd=ROOT, env=env)
    copy_goldens(case, backend)
    return result.returncode


# -------------------------------------------------------------------------------------------
# P7-7 repeat archiving.
#
# WHY IT IS NOT OPTIONAL FOR E0. `.trace-work/android-retrace-result/<case>-<backend>/` is keyed
# by case and backend and by nothing else, so repeat 2 overwrites repeat 1 and the only artefact
# that survives a three-repeat run is the last one. That makes "is this case's divergence
# REPRODUCIBLE, and are the three repeats the same picture?" - the first question P7-7 E0 asks -
# unanswerable after the fact, and the device window is the one place where re-running to find
# out is expensive. --archive-dir copies each repeat out before the next one starts.
#
# WHAT IS ARCHIVED, and why each piece: result.json (the SSIM the gate scored and which golden
# it matched), the actual PNG (the only thing an actual-vs-actual comparison can be computed
# from), both role logs and transport-proof.json (which arm actually ran - a divergence measured
# on an arm that silently fell back to monolith is not a finding), and logcat.txt (a crash that
# still wrote a result). The golden is copied ONCE per arm, not per repeat.
# -------------------------------------------------------------------------------------------
ARCHIVED_PER_REPEAT = ("result.json", "mobilegl.log", "mobilegl.client.log", "mobilegl.server.log",
                       "transport-proof.json", "logcat.txt", "benchmark.json")


def file_sha256(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def archive_repeat(archive_dir, case, backend, repeat_index):
    """Copy one repeat's evidence out of the overwritten result root. Returns the destination."""
    arm = f"{safe_case(case['name'])}-{backend}"
    source = RESULT_ROOT / arm
    destination = Path(archive_dir) / arm / ("repeat-%02d" % repeat_index)
    destination.mkdir(parents=True, exist_ok=True)
    for name in ARCHIVED_PER_REPEAT:
        candidate = source / name
        if candidate.is_file():
            shutil.copyfile(candidate, destination / name)
    for actual in sorted(source.glob("*-actual.png")):
        shutil.copyfile(actual, destination / actual.name)
    for diff in sorted(source.glob("*-diff.png")):
        shutil.copyfile(diff, destination / diff.name)
    # The goldens sit beside the repeats, once, so the archive is self-contained without
    # carrying the same megabyte three times.
    for golden in sorted(source.glob("*-golden.png")) + sorted(source.glob("*-alternate-golden.png")):
        target = Path(archive_dir) / arm / golden.name
        if not target.exists():
            shutil.copyfile(golden, target)
    return destination


def write_archive_manifest(archive_dir, args, backends, cases):
    """What this archive IS, recorded beside it rather than in the operator's shell history.

    The APK hash in particular: `find_trace_apk` takes the newest of two output directories, so
    "which binary produced these pictures" is a question the archive has to answer itself.
    """
    apk = find_trace_apk()
    manifest = {
        "tool": "run_android_retrace_local.py",
        "written_at": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
        "argv": sys.argv[1:],
        "backends": list(backends),
        "transport": args.transport,
        "use_pbuffer": bool(args.use_pbuffer),
        "repeat": args.repeat,
        "env": list(args.env),
        "transport_env": transport_env(args),
        "transport_proof_args": transport_proof_args(args),
        "package": BACKENDS["DirectGLES"]["package"],
        "cases": [case["name"] for case in cases],
        "apk": str(apk) if apk else None,
        "apk_sha256": file_sha256(apk) if apk else None,
    }
    path = Path(archive_dir) / "run.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return manifest


def read_benchmark(case, backend, run_index):
    """Reads the benchmark.json the run just pulled and files it under the run number."""
    result_dir = RESULT_ROOT / f"{safe_case(case['name'])}-{backend}"
    source = result_dir / "benchmark.json"
    if not source.exists():
        return None
    try:
        report = json.loads(source.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        print(f"failed to read {source}: {error}", file=sys.stderr)
        return None
    shutil.copyfile(source, result_dir / f"benchmark-run{run_index}.json")
    return report


def series_median(values):
    """The median, by the rule SummarizeSeries uses on the device.

    trace_replay_core.cpp's SeriesSummary takes the middle element of an odd window and the
    AVERAGE of the two middle elements of an even one, so p50 has to be computed the same way or
    the line would print a p50 next to a medianFrameCpuMs that disagreed with it for a reason
    nobody could see. (It is the only one of the three that is not a nearest rank: the device's
    p95 is.)
    """
    if not values:
        return -1.0
    ordered = sorted(values)
    middle = len(ordered) // 2
    if len(ordered) % 2 == 1:
        return ordered[middle]
    return 0.5 * (ordered[middle - 1] + ordered[middle])


def nearest_rank_percentile(values, fraction):
    """Nearest-rank percentile, the rule SummarizeSeries uses on the device for p95.

    Nearest rank rather than an interpolating percentile so that every number printed here is a
    frame that was actually observed, and so that a p95 computed on this side agrees exactly with
    the p95 the device reported for the same window. The device computes no p99 at all - that is
    the whole reason benchmark.json carries the full series - so p99 is this rule extended, and
    p50 is NOT computed here (see series_median).
    """
    if not values:
        return -1.0
    ordered = sorted(values)
    rank = math.ceil(fraction * len(ordered))
    if rank < 1:
        rank = 1
    return ordered[rank - 1]


def cpu_tail(report):
    """The trailing window of the per-frame CPU series, or [] when the run collected none.

    benchmark.json carries the WHOLE frameCpuTimesMs[] array precisely so that p99 - which the
    device does not compute, and which the paired A/B publishes beside p50 - is a host-side
    reduction over an artefact that already exists. The window is the same trailing tailFrames the
    device summarised, so the numbers below sit beside the device's own without being about a
    different set of frames; p50 is recomputed here by the device's own median rule, so it agrees
    with medianFrameCpuMs on the same run rather than merely sitting next to it.
    """
    series = report.get("frameCpuTimesMs") or []
    if not series:
        return []
    tail = report.get("tailFrames", 0)
    if not isinstance(tail, int) or tail <= 0 or tail > len(series):
        tail = len(series)
    return series[-tail:]


def format_benchmark(report):
    line = (
        f"frames={report.get('totalFrames', -1)}"
        f" total={report.get('totalSeconds', -1):.1f}s"
        f" tail={report.get('tailFrames', -1)}"
        f" mean={report.get('meanFrameMs', -1):.3f}ms"
        f" median={report.get('medianFrameMs', -1):.3f}ms"
        f" p95={report.get('p95FrameMs', -1):.3f}ms"
        f" fps={report.get('fps', -1):.1f}"
    )
    # The CPU half. It is what the disaggregation A/B is actually read on - wall time under
    # --benchmark-no-finish still contains everything the retrace thread waited for - so it is
    # printed on the same line rather than left to whoever remembers to open the JSON.
    window = cpu_tail(report)
    if window:
        line += (
            f" | cpu mean={report.get('meanFrameCpuMs', -1):.3f}ms"
            f" p50={series_median(window):.3f}ms"
            f" p95={report.get('p95FrameCpuMs', -1):.3f}ms"
            f" p99={nearest_rank_percentile(window, 0.99):.3f}ms"
        )
    else:
        # Not "cpu=0": a run with no per-thread CPU clock and a run that burned no CPU are
        # different claims, and only one of them is possible.
        line += " | cpu unavailable (no per-thread CPU clock in this run)"
    return line


def run_benchmark_case(case, backend, args):
    """Runs the case as a frame-timing benchmark `--benchmark-repeats` times.

    Only the first run installs the APK and pushes the trace; the repeats reuse what is
    already on the device, so the numbers are not paying for an adb push each time.
    """
    label = f"{case['name']} / {backend}"
    reports = []
    failures = 0
    for run_index in range(1, args.benchmark_repeats + 1):
        extra_args = [
            "--benchmark",
            "--benchmark-tail-frames",
            str(args.benchmark_tail_frames),
            "--benchmark-finish",
            "0" if args.benchmark_no_finish else "1",
        ]
        if run_index > 1:
            extra_args.append("--reuse-fixture")
        # The previous repeat's file would otherwise be read back as this run's result.
        stale = RESULT_ROOT / f"{safe_case(case['name'])}-{backend}" / "benchmark.json"
        if stale.exists():
            stale.unlink()
        rc = run_case(
            case,
            backend,
            extra_args=extra_args,
            timeout_seconds=args.benchmark_timeout_seconds,
            env_overrides=args.env,
        )
        report = read_benchmark(case, backend, run_index)
        if rc != 0 or report is None:
            print(f"{label} run {run_index}/{args.benchmark_repeats}: FAILED (exit {rc})", flush=True)
            failures += 1
            continue
        reports.append((run_index, report))
        print(
            f"{label} run {run_index}/{args.benchmark_repeats}: {format_benchmark(report)}",
            flush=True,
        )

    def mean_frame_ms(entry):
        # A run that recorded no frames reports -1; it must not win "best" by being smallest.
        mean = entry[1].get("meanFrameMs", -1)
        return mean if mean > 0 else float("inf")

    if reports:
        best_index, best = min(reports, key=mean_frame_ms)
        print(
            f"{label} best of {args.benchmark_repeats} (run {best_index}): {format_benchmark(best)}",
            flush=True,
        )
    return 1 if failures else 0


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--case", action="append", dest="cases", help="Case name to run; may be repeated.")
    parser.add_argument("--backend", action="append", choices=sorted(BACKENDS), help="Backend to run; may be repeated.")
    parser.add_argument("--all", action="store_true", help="Run every case in the APK workflow matrix.")
    parser.add_argument(
        "--matrix",
        action="store_true",
        help="Run the CI SPLIT SUBSET - the cases the retrace-split lane runs - rather than "
             "every case in the manifest. This is the set P7 exit gate 3 is scored on: --all "
             "additionally includes the non-CI workloads (rd12), which ID-P7-4 excludes from "
             "that denominator. Each case runs only the backends its own ci_backends names.",
    )
    parser.add_argument(
        "--repeat",
        type=int,
        default=1,
        metavar="N",
        help="Replay each case/backend N times. With --archive-dir each repeat's result.json "
             "and actual PNG are kept, so compare_actuals.py can compute actual-vs-actual SSIM "
             "between repeats; without it the repeats overwrite one another and only the last "
             "one survives.",
    )
    parser.add_argument(
        "--archive-dir",
        type=Path,
        metavar="DIR",
        help="Copy each repeat's evidence to DIR/<case>-<backend>/repeat-NN/ before the next "
             "run overwrites the result root, and write DIR/run.json recording the APK hash, "
             "the arm and the environment. Read it with "
             "`python3 tools/trace_replay/compare_actuals.py summary DIR`.",
    )
    parser.add_argument("--keep-results", action="store_true", help="Do not clear the previous result root.")
    parser.add_argument(
        "--env",
        action="append",
        default=[],
        metavar="KEY=VALUE",
        help="Environment variable to set in the replay process, applied just before "
             "libMobileGL.so is loaded; may be repeated. A KEY with no '=' unsets it. This "
             "is the generic passthrough for MOBILEGL_* knobs that have no flag of their own.",
    )
    parser.add_argument(
        "--benchmark",
        action="store_true",
        help="Replay each case end to end as a frame-timing benchmark instead of comparing "
             "one frame against its golden.",
    )
    parser.add_argument(
        "--benchmark-repeats",
        type=int,
        default=3,
        help="Benchmark runs per case/backend; the best (lowest mean frame time) is reported.",
    )
    parser.add_argument(
        "--benchmark-tail-frames",
        type=int,
        default=200,
        help="Frames at the end of the run the statistics are computed over.",
    )
    parser.add_argument(
        "--benchmark-no-finish",
        action="store_true",
        help="Do not glFinish at every frame boundary, so frame times measure CPU submission "
             "only instead of GPU completion.",
    )
    parser.add_argument(
        "--use-pbuffer",
        action="store_true",
        help="Render into a pbuffer instead of the Activity's window surface. REQUIRED with "
             "--transport spawn until P12: an ANativeWindow* is a pointer into the CLIENT's "
             "process and means nothing in the server's, so SetWindowHandle is refused by name "
             "with Fatal{UnmigratedSurface, \"AndroidNativeWindow@P12\"} (Rule H). The desktop "
             "retrace has always run pbuffer and matches the same goldens.",
    )
    parser.add_argument(
        "--transport",
        choices=("monolith", "inproc", "spawn"),
        default="monolith",
        help="MOBILEGL_TRANSPORT for the replay. `spawn` runs the server role in a SECOND PROCESS "
             "on the device, launched out of the APK's lib/<abi>/ - which requires an APK built "
             "with -Pmobilegl.buildDisaggregated=ON, or ConfigLoader has no parser, accepts the "
             "value and ignores it (CONTRACT-P5 rule 5) and the run is monolith under a name that "
             "says otherwise. MOBILEGL_IPC_SERVER_PATH is NOT passed from here: nativeLibraryDir "
             "carries an install-time hash, so only the app can spell it, and "
             "TraceReplayActivity.resolveSpawnServerPath does.",
    )
    parser.add_argument(
        "--benchmark-timeout-seconds",
        type=int,
        default=900,
        help="Per-run timeout; a benchmark replays the whole trace, not just up to target_call.",
    )
    return parser.parse_args()


def transport_proof_args(args):
    """The same arm-proof flag CI passes, so a local run cannot be greener than the lane.

    trace-replay-ci.sh's --require-inproc / --require-spawn read the LIBRARY's own log and
    refuse a run whose transport did not actually resolve - and for spawn, one whose server
    role never left the process. Passing them here too is what stops "it works locally" from
    meaning "it ran monolith locally".
    """
    return {"inproc": ["--require-inproc"], "spawn": ["--require-spawn"]}.get(args.transport, [])


# The knobs .github/workflows/apk.yml hands BOTH acceptance lanes. They are kept here verbatim so
# a local run is the same run: the arm proof below reads the library's own `Config: IPC` line and
# requires strict/role-split-state/run-ahead to be 1, so a local invocation that quietly omitted
# them would fail a gate CI passes - or worse, pass a weaker one.
SPLIT_ACCEPTANCE_KNOBS = (
    "MOBILEGL_IPC_ROLE_SPLIT_STATE=1",
    "MOBILEGL_IPC_STRICT_ERRORS=1",
    "MOBILEGL_IPC_RUN_AHEAD=1",
)


def transport_env(args):
    """The transport knob as env overrides, or nothing at all for monolith.

    An EMPTY list on monolith rather than MOBILEGL_TRANSPORT=monolith, so the default arm's
    environment is byte-identical to what it was before this option existed - the control arm has
    to stay a control.
    """
    if args.transport == "monolith":
        return []
    return [f"MOBILEGL_TRANSPORT={args.transport}", *SPLIT_ACCEPTANCE_KNOBS]


def main():
    args = parse_args()
    selected_backends = args.backend or list(BACKENDS)
    selected_names = set(args.cases or [])
    if args.matrix:
        # The CI split subset, resolved by the manifest's own loader so that a case added later
        # is in it unless it says otherwise (trace_cases.py explains why the key is an opt-out).
        pool = split_trace_cases(ci_trace_cases(CASES))
    else:
        pool = CASES
    selected_cases = [case_with_defaults(case) for case in pool
                      if args.all or args.matrix or case["name"] in selected_names]
    if not selected_cases:
        print("No cases selected. Use --matrix, --all or --case NAME.", file=sys.stderr)
        return 2
    if args.benchmark and args.benchmark_repeats < 1:
        print("--benchmark-repeats must be at least 1.", file=sys.stderr)
        return 2
    if args.repeat < 1:
        print("--repeat must be at least 1.", file=sys.stderr)
        return 2
    if args.benchmark and args.repeat != 1:
        print("--repeat is the correctness lane's repeat count; use --benchmark-repeats.",
              file=sys.stderr)
        return 2
    if args.benchmark and args.transport != "monolith":
        # REFUSED RATHER THAN RUN UNDER A NAME THAT LIES. run_benchmark_case passes args.env
        # alone, so --benchmark --transport spawn has always produced a MONOLITH run labelled
        # spawn (tools/device_bench/p6/README.md records the same pitfall). The knob the
        # benchmark path does honour is --env, so say that instead of silently ignoring this.
        print("--benchmark ignores --transport; pass the arm explicitly, e.g. "
              f"--env MOBILEGL_TRANSPORT={args.transport} "
              "--env MOBILEGL_IPC_ROLE_SPLIT_STATE=1 --env MOBILEGL_IPC_STRICT_ERRORS=1 "
              "--env MOBILEGL_IPC_RUN_AHEAD=1", file=sys.stderr)
        return 2
    if not args.keep_results and RESULT_ROOT.exists():
        shutil.rmtree(RESULT_ROOT)
    RESULT_ROOT.mkdir(parents=True, exist_ok=True)
    if args.archive_dir:
        write_archive_manifest(args.archive_dir, args, selected_backends, selected_cases)
    failures = 0
    for case in selected_cases:
        for backend in selected_backends:
            if backend not in ci_backends(case):
                # A case restricted to one backend stays restricted, and the two ways of asking
                # for it are different questions. A SWEEP skips it and says so - the
                # DirectVulkan-only iris case is simply not part of a DirectGLES sweep. A case
                # NAMED on the command line is a request, and silently doing nothing with a
                # request is how an operator concludes an arm was measured when it was not.
                message = (f"{case['name']} does not run {backend}: the case declares "
                           f"ci_backends = {', '.join(ci_backends(case))}")
                if case["name"] in selected_names:
                    print(message, file=sys.stderr)
                    return 2
                print("--- skipping " + message, flush=True)
                continue
            # The golden and threshold this backend is scored against (trace_cases.py's
            # backend_overrides); the case unchanged when it declares none.
            resolved = case_for_backend(case, backend)
            if args.benchmark:
                print(f"=== Android benchmark: {case['name']} / {backend} ===", flush=True)
                # No SSIM verdicts to render here; the summary page is for the correctness lane.
                failures += run_benchmark_case(resolved, backend, args)
                continue
            for repeat in range(1, args.repeat + 1):
                print(f"=== Android retrace: {case['name']} / {backend} "
                      f"(repeat {repeat}/{args.repeat}) ===", flush=True)
                extra = list(transport_proof_args(args))
                if repeat > 1:
                    # The repeats measure the REPLAY, not the push: re-extracting and
                    # re-installing between them would put an installer inside the window and
                    # would not change a byte of the input.
                    extra.append("--reuse-fixture")
                rc = run_case(resolved, backend, extra_args=extra,
                              env_overrides=transport_env(args) + list(args.env),
                              use_pbuffer=args.use_pbuffer)
                if args.archive_dir:
                    try:
                        kept = archive_repeat(args.archive_dir, resolved, backend, repeat)
                        print(f"archived repeat {repeat} to {kept}", flush=True)
                    except OSError as error:
                        print(f"failed to archive repeat {repeat}: {error}", file=sys.stderr)
                        failures += 1
                if rc not in (0, 2):
                    failures += 1
            try:
                render_summary()
            except Exception as error:
                print(f"failed to render summary: {error}", file=sys.stderr)
                failures += 1
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
