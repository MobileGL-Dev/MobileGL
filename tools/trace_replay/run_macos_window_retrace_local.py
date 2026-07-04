#!/usr/bin/env python3
import argparse
import concurrent.futures
import json
import os
import signal
import shutil
import subprocess
import sys
import time
from pathlib import Path

from trace_cases import case_with_defaults, ci_trace_cases, load_trace_cases


ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / "tools" / "trace_replay" / "fixtures"
RESULT_ROOT = ROOT / ".trace-work" / "macos-window-retrace-result"
WORK_ROOT = ROOT / ".trace-work" / "macos-window-retrace-work"
SUMMARY_DIR = ROOT / ".trace-work" / "macos-window-retrace-summary"
SUMMARY_HTML = "mobilegl-macos-window-vulkan-retrace-overview.html"
DEFAULT_MIRROR_BASE = "https://repo.miawa.cn/mgl/tools/trace_replay/fixtures"
BACKENDS = ("DirectVulkan",)
CASES = load_trace_cases()


def host_machine():
    try:
        result = subprocess.run(["/usr/sbin/sysctl", "-n", "hw.optional.arm64"], text=True, capture_output=True, check=False)
        if result.stdout.strip() == "1":
            return "arm64"
    except OSError:
        pass
    try:
        result = subprocess.run(["/usr/bin/uname", "-m"], text=True, capture_output=True, check=False)
        if result.stdout.strip():
            return result.stdout.strip()
    except OSError:
        pass
    return os.uname().machine


DEFAULT_BUILD_DIR = ROOT / (
    "cmake-build-macos-trace-arm64" if host_machine() == "arm64" else "cmake-build-macos-trace"
)


def safe_case(name):
    return "".join(ch if ch.isalnum() or ch in "._-" else "_" for ch in name)


def is_lfs_pointer(path):
    return path.exists() and path.read_bytes()[:80].startswith(b"version https://git-lfs.github.com/spec/v1")


def is_bad_fixture(path):
    return not path.exists() or path.stat().st_size <= 1024 or is_lfs_pointer(path)


def fixture_paths(case):
    paths = [FIXTURES / case["trace_archive"], FIXTURES / case["golden"]]
    if case.get("alternate_golden"):
        paths.append(FIXTURES / case["alternate_golden"])
    return paths


def file_arch(path):
    if not path.exists():
        return ""
    try:
        result = subprocess.run(["file", str(path)], text=True, capture_output=True, check=False)
    except OSError:
        return ""
    text = result.stdout
    if "x86_64" in text and "arm64" not in text:
        return "x86_64"
    if "arm64" in text and "x86_64" not in text:
        return "arm64"
    if "universal" in text or ("x86_64" in text and "arm64" in text):
        return "universal"
    return ""


def default_vulkan_icd(mobilegl_library):
    env_value = os.environ.get("VK_ICD_FILENAMES")
    if env_value:
        return env_value

    candidates = [
        Path("/usr/local/share/vulkan/icd.d/MoltenVK_icd.json"),
        Path("/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json"),
        Path("/opt/homebrew/Cellar/molten-vk/1.4.1/etc/vulkan/icd.d/MoltenVK_icd.json"),
    ]
    arch = file_arch(mobilegl_library)
    if arch == "arm64":
        candidates = candidates[1:] + candidates[:1]
    for candidate in candidates:
        if candidate.exists():
            return str(candidate)
    return ""


def download_fixture(path, mirror_base):
    path.parent.mkdir(parents=True, exist_ok=True)
    url = f"{mirror_base.rstrip('/')}/{path.name}"
    tmp = path.with_suffix(path.suffix + ".tmp")
    command = [
        "curl",
        "-L",
        "--fail",
        "--retry",
        "3",
        "--retry-delay",
        "2",
        "--continue-at",
        "-",
        "-o",
        str(tmp),
        url,
    ]
    result = subprocess.run(command, text=True, capture_output=True)
    if result.returncode != 0:
        return path, False, result.stderr.strip() or result.stdout.strip()
    tmp.replace(path)
    if is_bad_fixture(path):
        return path, False, "downloaded fixture is empty or still an LFS pointer"
    return path, True, ""


def hydrate_fixtures(cases, fetch, mirror_base, download_jobs):
    required = []
    seen = set()
    for case in cases:
        for path in fixture_paths(case):
            if path in seen:
                continue
            seen.add(path)
            if is_bad_fixture(path):
                required.append(path)

    if not required:
        print("fixtures: all required files are present", flush=True)
        return True
    if not fetch:
        for path in required:
            print(f"missing fixture: {path}", file=sys.stderr)
        return False

    print(f"fixtures: downloading {len(required)} file(s) with {download_jobs} worker(s)", flush=True)
    failures = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=download_jobs) as executor:
        futures = [executor.submit(download_fixture, path, mirror_base) for path in required]
        for future in concurrent.futures.as_completed(futures):
            path, ok, message = future.result()
            if ok:
                print(f"  fetched {path.name}", flush=True)
            else:
                print(f"  mirror fetch failed for {path.name}: {message}", file=sys.stderr, flush=True)
                failures.append(path)

    remaining = [path for path in required if is_bad_fixture(path)]
    if remaining:
        include = ",".join(str(path.relative_to(ROOT)) for path in remaining)
        print(f"fixtures: falling back to git lfs for {include}", flush=True)
        subprocess.run(["git", "lfs", "install", "--local"], cwd=ROOT, check=False)
        subprocess.run(["git", "lfs", "pull", f"--include={include}", "--exclude="], cwd=ROOT, check=False)

    remaining = [path for path in required if is_bad_fixture(path)]
    if remaining:
        for path in remaining:
            print(f"failed to hydrate fixture: {path}", file=sys.stderr)
        return False
    return True


def copy_goldens(case, backend, result_dir):
    for key, suffix in (("golden", "golden"), ("alternate_golden", "alternate-golden")):
        value = case.get(key)
        if not value:
            continue
        source = FIXTURES / value
        if source.exists() and not is_lfs_pointer(source):
            shutil.copyfile(source, result_dir / f"{safe_case(case['name'])}-{backend}-{suffix}.png")


def synthesize_result(case, backend, result_dir, status_code, message, elapsed_seconds):
    result = {
        "passed": False,
        "statusCode": status_code,
        "message": message,
        "tracePath": str(WORK_ROOT / safe_case(case["name"]) / "input" / case["trace_file"]),
        "goldenPath": str(FIXTURES / case["golden"]),
        "alternateGoldenPaths": [
            str(FIXTURES / case["alternate_golden"])
        ] if case.get("alternate_golden") else [],
        "matchedGoldenPath": "",
        "actualPath": str(result_dir / "actual.png"),
        "diffPath": str(result_dir / f"{safe_case(case['name'])}-{backend}-diff.png"),
        "backend": backend,
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
        "elapsedSeconds": elapsed_seconds,
    }
    (result_dir / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")


def recent_log_text(paths, max_bytes=16000):
    chunks = []
    for path in paths:
        if not path.exists():
            continue
        try:
            with path.open("rb") as file:
                file.seek(0, os.SEEK_END)
                size = file.tell()
                file.seek(max(0, size - max_bytes), os.SEEK_SET)
                text = file.read().decode("utf-8", errors="replace")
            chunks.append(f"==> {path.name} <==\n{text}")
        except OSError:
            pass
    return "\n".join(chunks)


def fatal_replay_message(result_dir):
    text = recent_log_text([result_dir / "mobilegl.log", result_dir / "retrace.log"])
    fatal_markers = ("[FATAL]", "Assertion failed:", "caught signal", "caught an unhandled exception")
    if any(marker in text for marker in fatal_markers):
        lines = [line for line in text.splitlines() if any(marker in line for marker in fatal_markers)]
        return "\n".join(lines[-8:]) or "trace replay hit a fatal error"
    return ""


def terminate_process_group(process):
    if process.poll() is not None:
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    except OSError:
        pass
    try:
        process.wait(timeout=1)
        return
    except subprocess.TimeoutExpired:
        pass
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        return
    except OSError:
        pass
    try:
        process.wait(timeout=1)
    except subprocess.TimeoutExpired:
        pass


def render_summary(result_root, summary_dir, html_path):
    summary_dir.mkdir(parents=True, exist_ok=True)
    html_path = Path(html_path)
    if html_path.is_absolute() or html_path.parent != Path("."):
        html_output = html_path if html_path.is_absolute() else (ROOT / html_path).resolve()
        html_output.parent.mkdir(parents=True, exist_ok=True)
        html_name = html_output.name
    else:
        html_output = summary_dir / html_path
        html_name = html_path.name
    command = [
        "node",
        str(ROOT / "tools" / "trace_replay" / "render_retrace_summary.mjs"),
        "--input",
        str(result_root),
        "--output-dir",
        str(summary_dir),
        "--title",
        "MobileGL macOS window retrace overview",
        "--group-label",
        "macOS Window",
        "--html",
        html_name,
    ]
    subprocess.run(command, cwd=ROOT, check=True)
    rendered_html = summary_dir / html_name
    if html_output != rendered_html:
        shutil.copyfile(rendered_html, html_output)
    if rendered_html != summary_dir / "index.html" and html_output != summary_dir / "index.html":
        shutil.copyfile(rendered_html, summary_dir / "index.html")
    return html_output


def run_case(case, backend, replay_exe, mobilegl_library, vulkan_icd):
    result_dir = RESULT_ROOT / f"{safe_case(case['name'])}-{backend}"
    case_work = WORK_ROOT / safe_case(case["name"])
    input_dir = case_work / "input"
    if result_dir.exists():
        shutil.rmtree(result_dir)
    if case_work.exists():
        shutil.rmtree(case_work)
    result_dir.mkdir(parents=True, exist_ok=True)
    input_dir.mkdir(parents=True, exist_ok=True)
    copy_goldens(case, backend, result_dir)

    start = time.monotonic()
    extract = subprocess.run(
        ["cmake", "-E", "tar", "xzf", str(FIXTURES / case["trace_archive"])],
        cwd=input_dir,
        text=True,
        capture_output=True,
    )
    if extract.returncode != 0:
        elapsed = time.monotonic() - start
        (result_dir / "runner-stdout.log").write_text(extract.stdout, encoding="utf-8", errors="replace")
        (result_dir / "runner-stderr.log").write_text(extract.stderr, encoding="utf-8", errors="replace")
        synthesize_result(case, backend, result_dir, extract.returncode, "failed to extract trace archive", elapsed)
        return extract.returncode, elapsed, "", False

    alternate_args = []
    if case.get("alternate_golden"):
        alternate_args = ["--alternate-golden", str(FIXTURES / case["alternate_golden"])]

    diff = result_dir / f"{safe_case(case['name'])}-{backend}-diff.png"
    command = [
        str(replay_exe),
        "--trace",
        str(input_dir / case["trace_file"]),
        "--golden",
        str(FIXTURES / case["golden"]),
        *alternate_args,
        "--diff",
        str(diff),
        "--output",
        str(result_dir),
        "--backend",
        backend,
        "--mobilegl-library",
        str(mobilegl_library),
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
        "--window-surface",
    ]
    env = os.environ.copy()
    env["MOBILEGL_BACKEND_TYPE"] = backend
    env["MOBILEGL_VULKAN_R11G11B10F_FALLBACK"] = "1"
    if vulkan_icd:
        env["VK_ICD_FILENAMES"] = vulkan_icd

    stdout_path = result_dir / "runner-stdout.log"
    stderr_path = result_dir / "runner-stderr.log"
    with stdout_path.open("w", encoding="utf-8", errors="replace") as stdout_file, \
            stderr_path.open("w", encoding="utf-8", errors="replace") as stderr_file:
        process = subprocess.Popen(
                command,
                cwd=ROOT,
                env=env,
                stdout=stdout_file,
                stderr=stderr_file,
                start_new_session=True,
        )
        deadline = start + int(case["timeout_seconds"])
        while process.poll() is None:
            elapsed = time.monotonic() - start
            fatal_message = fatal_replay_message(result_dir)
            if fatal_message:
                terminate_process_group(process)
                synthesize_result(case, backend, result_dir, 133, fatal_message, elapsed)
                return 133, elapsed, "", True
            if time.monotonic() >= deadline:
                terminate_process_group(process)
                elapsed = time.monotonic() - start
                synthesize_result(case, backend, result_dir, 124, f"timed out after {case['timeout_seconds']}s", elapsed)
                return 124, elapsed, "", False
            time.sleep(0.5)
    elapsed = time.monotonic() - start

    if not (result_dir / "result.json").exists():
        stdout_text = stdout_path.read_text(encoding="utf-8", errors="replace") if stdout_path.exists() else ""
        stderr_text = stderr_path.read_text(encoding="utf-8", errors="replace") if stderr_path.exists() else ""
        message = (stdout_text + "\n" + stderr_text).strip()[-4000:] or f"process exited {process.returncode}"
        synthesize_result(case, backend, result_dir, process.returncode, message, elapsed)

    actual = result_dir / "actual.png"
    if actual.exists():
        shutil.copyfile(actual, result_dir / f"{safe_case(case['name'])}-{backend}-actual.png")

    ssim = ""
    try:
        result = json.loads((result_dir / "result.json").read_text(encoding="utf-8"))
        ssim = result.get("ssim", "")
    except (OSError, json.JSONDecodeError):
        pass
    return process.returncode, elapsed, ssim, False


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--case", action="append", dest="cases", help="Case name to run; may be repeated.")
    parser.add_argument("--all", action="store_true", help="Run every selected trace case.")
    parser.add_argument("--ci", action="store_true", help="Select only cases enabled for CI.")
    parser.add_argument("--skip-case", action="append", default=[], help="Mark a selected case skipped without launching it; may be repeated.")
    parser.add_argument("--backend", action="append", choices=BACKENDS, help="Backend to run; default: DirectVulkan.")
    parser.add_argument("--build-dir", type=Path, default=DEFAULT_BUILD_DIR, help="CMake build directory with mobilegl_trace_replay.")
    parser.add_argument("--trace-replay-exe", type=Path, help="Path to mobilegl_trace_replay.")
    parser.add_argument("--mobilegl-library", type=Path, help="Path to libMobileGL.dylib.")
    parser.add_argument("--vulkan-icd", help="MoltenVK ICD JSON path; defaults to VK_ICD_FILENAMES or a detected local install.")
    parser.add_argument("--result-root", type=Path, default=RESULT_ROOT)
    parser.add_argument("--work-root", type=Path, default=WORK_ROOT)
    parser.add_argument("--summary-dir", type=Path, default=SUMMARY_DIR)
    parser.add_argument("--html", default=SUMMARY_HTML)
    parser.add_argument("--keep-results", action="store_true", help="Do not clear previous result/work roots.")
    parser.add_argument("--no-render", action="store_true", help="Do not render the HTML summary.")
    parser.add_argument("--fetch-fixtures", action=argparse.BooleanOptionalAction, default=True, help="Hydrate missing fixtures before running.")
    parser.add_argument("--fixture-mirror-base", default=os.environ.get("MOBILEGL_TRACE_FIXTURE_MIRROR_BASE", DEFAULT_MIRROR_BASE))
    parser.add_argument("--download-jobs", type=int, default=4, help="Parallel fixture download count.")
    parser.add_argument("--continue-after-fatal", action="store_true", help="Continue launching later cases after a fatal native-window replay.")
    return parser.parse_args()


def main():
    global RESULT_ROOT, WORK_ROOT
    args = parse_args()
    RESULT_ROOT = args.result_root.resolve()
    WORK_ROOT = args.work_root.resolve()
    replay_exe = (args.trace_replay_exe or (args.build_dir / "tools" / "trace_replay" / "mobilegl_trace_replay")).resolve()
    mobilegl_library = (args.mobilegl_library or (args.build_dir / "libMobileGL.dylib")).resolve()
    vulkan_icd = args.vulkan_icd or default_vulkan_icd(mobilegl_library)
    selected_backends = args.backend or ["DirectVulkan"]

    cases = ci_trace_cases(CASES) if args.ci else CASES
    selected_names = set(args.cases or [])
    selected_cases = [case_with_defaults(case) for case in cases if args.all or case["name"] in selected_names]
    if not selected_cases:
        print("No cases selected. Use --all or --case NAME.", file=sys.stderr)
        return 2
    if not replay_exe.exists():
        print(f"missing trace replay executable: {replay_exe}", file=sys.stderr)
        return 2
    if not mobilegl_library.exists():
        print(f"missing MobileGL library: {mobilegl_library}", file=sys.stderr)
        return 2

    if not hydrate_fixtures(selected_cases, args.fetch_fixtures, args.fixture_mirror_base, max(1, args.download_jobs)):
        return 2

    if not args.keep_results:
        shutil.rmtree(RESULT_ROOT, ignore_errors=True)
        shutil.rmtree(WORK_ROOT, ignore_errors=True)
    RESULT_ROOT.mkdir(parents=True, exist_ok=True)
    WORK_ROOT.mkdir(parents=True, exist_ok=True)

    print(f"trace replay: {replay_exe} ({file_arch(replay_exe) or 'unknown arch'})", flush=True)
    print(f"MobileGL: {mobilegl_library} ({file_arch(mobilegl_library) or 'unknown arch'})", flush=True)
    if vulkan_icd:
        print(f"Vulkan ICD: {vulkan_icd}", flush=True)
    else:
        print("Vulkan ICD: using loader defaults", flush=True)

    results = []
    stop_matrix = False
    skipped_names = set(args.skip_case)
    for case in selected_cases:
        for backend in selected_backends:
            if case["name"] in skipped_names:
                result_dir = RESULT_ROOT / f"{safe_case(case['name'])}-{backend}"
                result_dir.mkdir(parents=True, exist_ok=True)
                copy_goldens(case, backend, result_dir)
                synthesize_result(case, backend, result_dir, 125, "skipped by --skip-case", 0)
                print(f"=== macOS window retrace: {case['name']} / {backend} ===", flush=True)
                print("  SKIP rc=125 elapsed=0.0s ssim=", flush=True)
                results.append({
                    "case": case["name"],
                    "backend": backend,
                    "status": "SKIP",
                    "returncode": 125,
                    "elapsedSeconds": 0,
                    "ssim": "",
                })
                continue
            print(f"=== macOS window retrace: {case['name']} / {backend} ===", flush=True)
            rc, elapsed, ssim, fatal = run_case(case, backend, replay_exe, mobilegl_library, vulkan_icd)
            status = "PASS" if rc == 0 else "FAIL"
            print(f"  {status} rc={rc} elapsed={elapsed:.1f}s ssim={ssim}", flush=True)
            results.append({
                "case": case["name"],
                "backend": backend,
                "status": status,
                "returncode": rc,
                "elapsedSeconds": round(elapsed, 3),
                "ssim": ssim,
            })
            if fatal and not args.continue_after_fatal:
                print("stopping after fatal native-window replay; use --continue-after-fatal to keep launching later cases", file=sys.stderr)
                stop_matrix = True
                break
        if stop_matrix:
            break

    summary_json = RESULT_ROOT / "macos-window-retrace-summary.json"
    summary_json.write_text(json.dumps(results, indent=2), encoding="utf-8")
    if not args.no_render:
        html_output = render_summary(RESULT_ROOT, args.summary_dir.resolve(), args.html)
        print(f"summary: {html_output}", flush=True)

    failures = sum(1 for result in results if result["returncode"] != 0)
    print(f"completed cases={len(results)} pass={len(results) - failures} fail={failures}", flush=True)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
