#!/usr/bin/env python3
import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / "tools" / "trace_replay" / "fixtures"
RESULT_ROOT = ROOT / ".trace-work" / "android-retrace-result"
FIXTURE_ROOT = ROOT / ".trace-work" / "android-retrace-fixture"
SUMMARY_DIR = ROOT / ".trace-work" / "android-retrace-summary"
SUMMARY_HTML = "mobilegl-android-retrace-overview.html"

BACKENDS = {
    "DirectGLES": {
        "apk": ROOT / "android-plugin" / "app" / "build" / "outputs" / "apk" / "esprytTrace" / "debug" / "MobileGL-EsprytTrace-debug.apk",
        "package": "top.mobilegl.plugin.espryt.trace",
        "use_angle": False,
        "use_pbuffer": False,
    },
    "DirectVulkan": {
        "apk": ROOT / "android-plugin" / "app" / "build" / "outputs" / "apk" / "magmaTrace" / "debug" / "MobileGL-MagmaTrace-debug.apk",
        "package": "top.mobilegl.plugin.magma.trace",
        "use_angle": False,
        "use_pbuffer": False,
    },
}

CASES = [
    {
        "name": "OpenRA",
        "trace_archive": "openra.tgz",
        "trace_file": "openra.trace",
        "golden": "openra.0000031249.png",
        "target_call": 31249,
        "width": 640,
        "height": 480,
        "crop_x": 1,
        "crop_y": 1,
        "crop_width": 638,
        "crop_height": 478,
        "timeout_seconds": 180,
    },
    {"name": "minecraft-1.21.4-startup", "golden": "minecraft-1.21.4-startup.0000092195.png", "target_call": 92195, "timeout_seconds": 180},
    {"name": "minecraft-1.21.4-main-menu", "golden": "minecraft-1.21.4-main-menu.0000481787.png", "alternate_golden": "minecraft-1.21.4-main-menu.0000481787-mali.png", "target_call": 481787, "timeout_seconds": 180},
    {"name": "minecraft-1.21.4-in-world", "golden": "minecraft-1.21.4-in-world.0000280000.png", "target_call": 280000, "timeout_seconds": 900},
    {"name": "minecraft-1.21.4-fabric-sodium-in-world", "golden": "minecraft-1.21.4-fabric-sodium-in-world.0000923340.png", "target_call": 923340, "timeout_seconds": 1800},
    {"name": "minecraft-26.2-main-menu", "golden": "minecraft-26.2-main-menu.0000101926.png", "target_call": 101926, "timeout_seconds": 180},
    {"name": "minecraft-26.2-in-world", "golden": "minecraft-26.2-in-world.0000519370.png", "target_call": 519370, "timeout_seconds": 900},
    {"name": "minecraft-1.21.4-fabric-common-mods-in-world", "golden": "minecraft-1.21.4-fabric-common-mods-in-world.0000522084.png", "target_call": 522084, "timeout_seconds": 1800},
    {"name": "minecraft-1.21.4-fabric-common-mods-inventory", "golden": "minecraft-1.21.4-fabric-common-mods-inventory.0000728558.png", "target_call": 728558, "timeout_seconds": 1800},
    {"name": "minecraft-1.21.4-fabric-rei-inventory", "golden": "minecraft-1.21.4-fabric-rei-inventory.0005431826.png", "target_call": 5431826, "timeout_seconds": 1800},
    {"name": "minecraft-1.21.4-fabric-xaero-minimap-in-world", "golden": "minecraft-1.21.4-fabric-xaero-minimap-in-world.0002553500.png", "target_call": 2553500, "crop_y": 10, "crop_width": 140, "crop_height": 158, "timeout_seconds": 1800},
    {"name": "minecraft-1.21.4-fabric-xaero-world-map-in-world", "golden": "minecraft-1.21.4-fabric-xaero-world-map-in-world.0001598209.png", "target_call": 1598209, "timeout_seconds": 1800},
    {"name": "minecraft-1.21.4-fabric-journeymap-in-world", "golden": "minecraft-1.21.4-fabric-journeymap-in-world.0002632392.png", "target_call": 2632392, "timeout_seconds": 1800},
    {"name": "minecraft-1.21.4-fabric-modernui-inventory", "golden": "minecraft-1.21.4-fabric-modernui-inventory.0004907381.png", "target_call": 4907381, "timeout_seconds": 1800},
    {"name": "minecraft-1.21.4-fabric-rei-inventory-normal-world", "golden": "minecraft-1.21.4-fabric-rei-inventory-normal-world.0000734465.png", "target_call": 734465, "timeout_seconds": 1800},
    {"name": "minecraft-1.21.4-fabric-xaero-minimap-in-world-normal-world", "golden": "minecraft-1.21.4-fabric-xaero-minimap-in-world-normal-world.0000457190.png", "target_call": 457190, "crop_y": 10, "crop_width": 140, "crop_height": 158, "timeout_seconds": 1800},
    {"name": "minecraft-1.21.4-fabric-xaero-world-map-in-world-normal-world", "golden": "minecraft-1.21.4-fabric-xaero-world-map-in-world-normal-world.0000573061.png", "target_call": 573061, "timeout_seconds": 1800},
    {"name": "minecraft-1.21.4-fabric-journeymap-in-world-normal-world", "golden": "minecraft-1.21.4-fabric-journeymap-in-world-normal-world.0000641975.png", "target_call": 641975, "timeout_seconds": 1800},
    {"name": "minecraft-1.21.4-fabric-modernui-inventory-normal-world", "golden": "minecraft-1.21.4-fabric-modernui-inventory-normal-world.0000727926.png", "target_call": 727926, "timeout_seconds": 1800},
    {"name": "minecraft-1.21.4-fabric-iris-bsl-in-world", "golden": "minecraft-1.21.4-fabric-iris-bsl-in-world.0000110725.png", "alternate_golden": "minecraft-1.21.4-fabric-iris-bsl-in-world.0000110725-mali.png", "target_call": 110725, "timeout_seconds": 900},
    {"name": "minecraft-1.21.4-fabric-iris-makeup-ultrafast-in-world", "golden": "minecraft-1.21.4-fabric-iris-makeup-ultrafast-in-world.0000095322.png", "target_call": 95322, "timeout_seconds": 900},
    {"name": "minecraft-1.21.4-fabric-iris-super-duper-vanilla-in-world", "golden": "minecraft-1.21.4-fabric-iris-super-duper-vanilla-in-world.0000141559.png", "target_call": 141559, "timeout_seconds": 900},
    {"name": "minecraft-1.21.4-fabric-iris-sundial-lite-in-world", "golden": "minecraft-1.21.4-fabric-iris-sundial-lite-in-world.0000150023.png", "target_call": 150023, "timeout_seconds": 900},
    {"name": "minecraft-1.21.4-fabric-iris-complementary-reimagined-in-world", "golden": "minecraft-1.21.4-fabric-iris-complementary-reimagined-in-world.0000151297.png", "target_call": 151297, "timeout_seconds": 900},
    {"name": "minecraft-1.21.4-fabric-iris-complementary-unbound-in-world", "golden": "minecraft-1.21.4-fabric-iris-complementary-unbound-in-world.0000146559.png", "target_call": 146559, "timeout_seconds": 900},
    {"name": "minecraft-1.21.4-fabric-iris-mellow-in-world", "golden": "minecraft-1.21.4-fabric-iris-mellow-in-world.0000096143.png", "target_call": 96143, "timeout_seconds": 900},
    {"name": "minecraft-1.21.4-fabric-iris-nostalgia-in-world", "golden": "minecraft-1.21.4-fabric-iris-nostalgia-in-world.0000153808-linux-mesa.png", "alternate_golden": "minecraft-1.21.4-fabric-iris-nostalgia-in-world.0000153808.png", "target_call": 153808, "timeout_seconds": 900},
    {"name": "minecraft-1.21.4-fabric-iris-bliss-in-world", "golden": "minecraft-1.21.4-fabric-iris-bliss-in-world.0000113511.png", "target_call": 113511, "timeout_seconds": 900},
    {"name": "minecraft-1.21.4-fabric-iris-chocapic-v6-lite-in-world", "golden": "minecraft-1.21.4-fabric-iris-chocapic-v6-lite-in-world.0000125124-linux-mesa.png", "target_call": 125124, "timeout_seconds": 900},
    {"name": "minecraft-1.21.4-fabric-iris-iterationt-in-world", "golden": "minecraft-1.21.4-fabric-iris-iterationt-in-world.0000110538.png", "target_call": 110538, "timeout_seconds": 900},
    {"name": "minecraft-1.21.4-fabric-iris-iterationt-nodsa-in-world", "golden": "minecraft-1.21.4-fabric-iris-iterationt-nodsa-in-world.0000115019.png", "target_call": 115019, "timeout_seconds": 900},
    {"name": "minecraft-1.21.4-fabric-iris-photon-v1.1-in-world", "golden": "minecraft-1.21.4-fabric-iris-photon-v1.1-in-world.0000159866.png", "target_call": 159866, "timeout_seconds": 900},
    {"name": "minecraft-1.21.4-fabric-iris-photon-v1.3b-in-world", "golden": "minecraft-1.21.4-fabric-iris-photon-v1.3b-in-world.0000172128.png", "target_call": 172128, "timeout_seconds": 900},
    {"name": "minecraft-1.21.4-fabric-iris-derivative-main-d24.4.14-in-world", "golden": "minecraft-1.21.4-fabric-iris-derivative-main-d24.4.14-in-world.0000145353.png", "target_call": 145353, "timeout_seconds": 900},
]


def safe_case(name):
    return "".join(ch if ch.isalnum() or ch in "._-" else "_" for ch in name)


def is_lfs_pointer(path):
    return path.exists() and path.read_bytes()[:80].startswith(b"version https://git-lfs.github.com/spec/v1")


def bash_path(path):
    path = Path(path).resolve()
    drive = path.drive.rstrip(":").lower()
    parts = path.parts[1:]
    return "/" + drive + "/" + "/".join(parts)


def case_with_defaults(case):
    merged = {
        "trace_archive": f"{case['name']}.tgz",
        "trace_file": "trace.trace",
        "width": 854,
        "height": 480,
        "ssim_threshold": "0.99",
        "crop_x": 0,
        "crop_y": 0,
        "crop_width": 0,
        "crop_height": 0,
        "timeout_seconds": 900,
    }
    merged.update(case)
    return merged


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


def run_case(case, backend):
    backend_info = BACKENDS[backend]
    trace_archive = FIXTURES / case["trace_archive"]
    golden = FIXTURES / case["golden"]
    alternate = FIXTURES / case["alternate_golden"] if case.get("alternate_golden") else None
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
        bash_path(backend_info["apk"]),
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
        str(case["timeout_seconds"]),
    ]
    if alternate is not None:
        command[command.index("--target-call"):command.index("--target-call")] = ["--alternate-golden", bash_path(alternate)]
    if backend_info["use_pbuffer"]:
        command.append("--use-pbuffer")
    env = dict(**__import__("os").environ)
    env["PYTHON"] = "python"
    env["MSYS2_ARG_CONV_EXCL"] = "/data/*"
    if backend_info["use_angle"]:
        env["MOBILEGL_RETRACE_USE_ANGLE"] = "1"
    else:
        env.pop("MOBILEGL_RETRACE_USE_ANGLE", None)
    result = subprocess.run(command, cwd=ROOT, env=env)
    copy_goldens(case, backend)
    return result.returncode


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--case", action="append", dest="cases", help="Case name to run; may be repeated.")
    parser.add_argument("--backend", action="append", choices=sorted(BACKENDS), help="Backend to run; may be repeated.")
    parser.add_argument("--all", action="store_true", help="Run every case in the APK workflow matrix.")
    parser.add_argument("--keep-results", action="store_true", help="Do not clear the previous result root.")
    return parser.parse_args()


def main():
    args = parse_args()
    selected_backends = args.backend or list(BACKENDS)
    selected_names = set(args.cases or [])
    selected_cases = [case_with_defaults(case) for case in CASES if args.all or case["name"] in selected_names]
    if not selected_cases:
        print("No cases selected. Use --all or --case NAME.", file=sys.stderr)
        return 2
    if not args.keep_results and RESULT_ROOT.exists():
        shutil.rmtree(RESULT_ROOT)
    RESULT_ROOT.mkdir(parents=True, exist_ok=True)
    failures = 0
    for case in selected_cases:
        for backend in selected_backends:
            print(f"=== Android retrace: {case['name']} / {backend} ===", flush=True)
            rc = run_case(case, backend)
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
