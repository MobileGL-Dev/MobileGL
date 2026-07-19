#!/usr/bin/env python3
import argparse
import json
import re
import shutil
import subprocess
import tarfile
from pathlib import Path

DEFAULT_MAX_ARCHIVE_BYTES = 20 * 1024 * 1024
# Bundled under the skill at .../skills/trace-fixture-authoring-on-android-fcl/scripts/.
SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_REPO = SCRIPT_DIR.parents[5]  # -> FoldCraftLauncher repo root


def run(cmd, cwd=None, capture=False):
    print("+", " ".join(str(part) for part in cmd), flush=True)
    if capture:
        return subprocess.check_output(cmd, cwd=cwd, text=True, encoding="utf-8", errors="replace")
    subprocess.run(cmd, cwd=cwd, check=True)
    return ""


def adb(args, serial=None):
    cmd = ["adb"]
    if serial:
        cmd += ["-s", serial]
    cmd += args
    return run(cmd, capture=True)


def pull_latest(serial, dest):
    latest = adb(["shell", "cat", "/sdcard/FCL/mobilegl-trace/latest-session.txt"], serial).strip()
    if not latest:
        raise SystemExit("device has no /sdcard/FCL/mobilegl-trace/latest-session.txt")
    dest.mkdir(parents=True, exist_ok=True)
    local = dest / Path(latest).name
    if local.exists():
        shutil.rmtree(local)
    adb(["pull", latest, str(local)], serial)
    return local


def choose_target_frame(capture_dir, explicit_frame):
    if explicit_frame is not None:
        return explicit_frame
    result = capture_dir / "capture-result.json"
    if not result.exists():
        raise SystemExit(f"missing {result}; press the FCL capture button or create capture-once.request first")
    data = json.loads(result.read_text(encoding="utf-8"))
    if "targetFrame" not in data:
        raise SystemExit(f"{result} has no targetFrame")
    return int(data["targetFrame"])


def choose_snapshot(golden_dir):
    pngs = sorted(golden_dir.glob("*.png"))
    if not pngs:
        raise SystemExit(f"no snapshots produced in {golden_dir}")
    def call_no(path):
        match = re.search(r"\.(\d+)\.png$", path.name)
        return int(match.group(1)) if match else -1
    return max(pngs, key=call_no)


def choose_target_call(explicit_call, golden):
    if explicit_call is not None:
        return explicit_call
    if golden is None:
        return None
    match = re.search(r"\.(\d+)\.png$", golden.name)
    return int(match.group(1)) if match else None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", default=str(DEFAULT_REPO), help="FoldCraftLauncher repo root")
    parser.add_argument("--serial", help="adb serial; when set, pull latest capture from device")
    parser.add_argument("--capture-dir", help="local capture directory; defaults to pulled latest")
    parser.add_argument("--pull-root", default=".trace-work/pulled-mobilegl-captures")
    parser.add_argument("--case", required=True)
    parser.add_argument("--target-frame", type=int)
    parser.add_argument("--target-call", type=int)
    parser.add_argument("--golden", help="existing golden PNG, normally produced by Android replay")
    parser.add_argument("--skip-desktop-golden", action="store_true",
                        help="skip apitrace replay --headless; requires --golden and --target-call")
    parser.add_argument("--apitrace", default="apitrace")
    parser.add_argument("--fixtures-dir", default="MobileGL/tools/trace_replay/fixtures")
    parser.add_argument("--width", type=int, default=854)
    parser.add_argument("--height", type=int, default=480)
    parser.add_argument("--ssim-threshold", default="0.99")
    parser.add_argument("--max-archive-bytes", type=int, default=DEFAULT_MAX_ARCHIVE_BYTES)
    args = parser.parse_args()

    repo = Path(args.repo).resolve()
    if args.capture_dir:
        capture_dir = Path(args.capture_dir).resolve()
    elif args.serial:
        capture_dir = pull_latest(args.serial, repo / args.pull_root)
    else:
        raise SystemExit("pass --capture-dir or --serial")

    full_trace = capture_dir / "full.trace"
    if not full_trace.exists():
        raise SystemExit(f"missing trace: {full_trace}")
    target_frame = choose_target_frame(capture_dir, args.target_frame)
    work = capture_dir / "fixture-work"
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)

    frames_txt = work / "frames.txt"
    frames_txt.write_text(run([args.apitrace, "dump", "--calls=frame", str(full_trace)], capture=True), encoding="utf-8")

    trimmed = work / "trace.trace"
    run([args.apitrace, "gltrim", "-f", str(target_frame), "--output", str(trimmed), str(full_trace)])

    supplied_golden = Path(args.golden).resolve() if args.golden else None
    target_call = choose_target_call(args.target_call, supplied_golden)
    if args.skip_desktop_golden:
        if supplied_golden is None or target_call is None:
            raise SystemExit("--skip-desktop-golden requires --golden and --target-call")
        golden = supplied_golden
    else:
        golden_dir = work / "golden"
        golden_dir.mkdir()
        prefix = golden_dir / f"{args.case}."
        run([args.apitrace, "replay", "--headless", "--snapshot-prefix", str(prefix), "--call-nos", str(trimmed)])
        golden = choose_snapshot(golden_dir)
        target_call = choose_target_call(args.target_call, golden)
        if target_call is None:
            raise SystemExit(f"cannot infer target call from {golden}")

    fixtures = (repo / args.fixtures_dir).resolve()
    fixtures.mkdir(parents=True, exist_ok=True)
    archive_root = work / "archive"
    archive_root.mkdir()
    shutil.copy2(trimmed, archive_root / "trace.trace")
    tgz = fixtures / f"{args.case}.tgz"
    with tarfile.open(tgz, "w:gz") as tar:
        tar.add(archive_root / "trace.trace", arcname="trace.trace")
    archive_size = tgz.stat().st_size
    if archive_size > args.max_archive_bytes:
        raise SystemExit(
            f"{tgz} is {archive_size} bytes, over the {args.max_archive_bytes} byte fixture limit; "
            "choose an earlier/smaller frame and re-run gltrim"
        )
    golden_out = fixtures / f"{args.case}.{target_call:010d}.png"
    shutil.copy2(golden, golden_out)

    manifest = {
        "name": args.case,
        "trace_archive": tgz.name,
        "trace_file": "trace.trace",
        "golden": golden_out.name,
        "target_call": target_call,
        "width": args.width,
        "height": args.height,
        "ssim_threshold": float(args.ssim_threshold),
        "archive_size": archive_size,
    }
    manifest_path = capture_dir / f"{args.case}.fixture.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
