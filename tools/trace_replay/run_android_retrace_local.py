#!/usr/bin/env python3
import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

from trace_cases import case_with_defaults, load_trace_cases


ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / "tools" / "trace_replay" / "fixtures"
RESULT_ROOT = ROOT / ".trace-work" / "android-retrace-result"
FIXTURE_ROOT = ROOT / ".trace-work" / "android-retrace-fixture"
SUMMARY_DIR = ROOT / ".trace-work" / "android-retrace-summary"
SUMMARY_HTML = "mobilegl-android-retrace-overview.html"
DEFAULT_ANGLE_VARIANT = "ec889e6ea831"
BLISS_ANGLE_VARIANT = "90a62123d794"
BLISS_CASE = "minecraft-1.21.4-fabric-iris-bliss-in-world"
TRACE_APK_DIR = ROOT / "android-plugin" / "app" / "build" / "outputs" / "apk" / "trace" / "debug"

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

CASES = load_trace_cases()


def safe_case(name):
    return "".join(ch if ch.isalnum() or ch in "._-" else "_" for ch in name)


def is_lfs_pointer(path):
    return path.exists() and path.read_bytes()[:80].startswith(b"version https://git-lfs.github.com/spec/v1")


def find_trace_apk():
    candidates = list(TRACE_APK_DIR.glob("MobileGL-plugin-trace-release-*.apk"))
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


def run_case(case, backend, extra_args=None, timeout_seconds=None, env_overrides=None):
    backend_info = BACKENDS[backend]
    apk = find_trace_apk()
    trace_archive = FIXTURES / case["trace_archive"]
    golden = FIXTURES / case["golden"]
    alternate = FIXTURES / case["alternate_golden"] if case.get("alternate_golden") else None
    if apk is None:
        mark_skipped(case, backend, f"SKIPPED_MISSING_APK: no trace APK found under {TRACE_APK_DIR}")
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
    if backend_info["use_pbuffer"]:
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


def format_benchmark(report):
    return (
        f"frames={report.get('totalFrames', -1)}"
        f" total={report.get('totalSeconds', -1):.1f}s"
        f" tail={report.get('tailFrames', -1)}"
        f" mean={report.get('meanFrameMs', -1):.3f}ms"
        f" median={report.get('medianFrameMs', -1):.3f}ms"
        f" p95={report.get('p95FrameMs', -1):.3f}ms"
        f" fps={report.get('fps', -1):.1f}"
    )


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
        "--benchmark-timeout-seconds",
        type=int,
        default=900,
        help="Per-run timeout; a benchmark replays the whole trace, not just up to target_call.",
    )
    return parser.parse_args()


def main():
    args = parse_args()
    selected_backends = args.backend or list(BACKENDS)
    selected_names = set(args.cases or [])
    selected_cases = [case_with_defaults(case) for case in CASES if args.all or case["name"] in selected_names]
    if not selected_cases:
        print("No cases selected. Use --all or --case NAME.", file=sys.stderr)
        return 2
    if args.benchmark and args.benchmark_repeats < 1:
        print("--benchmark-repeats must be at least 1.", file=sys.stderr)
        return 2
    if not args.keep_results and RESULT_ROOT.exists():
        shutil.rmtree(RESULT_ROOT)
    RESULT_ROOT.mkdir(parents=True, exist_ok=True)
    failures = 0
    for case in selected_cases:
        for backend in selected_backends:
            if args.benchmark:
                print(f"=== Android benchmark: {case['name']} / {backend} ===", flush=True)
                # No SSIM verdicts to render here; the summary page is for the correctness lane.
                failures += run_benchmark_case(case, backend, args)
                continue
            print(f"=== Android retrace: {case['name']} / {backend} ===", flush=True)
            rc = run_case(case, backend, env_overrides=args.env)
            try:
                render_summary()
            except Exception as error:
                print(f"failed to render summary: {error}", file=sys.stderr)
                failures += 1
            if rc not in (0, 2):
                failures += 1
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
