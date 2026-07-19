#!/usr/bin/env python3
import argparse
import subprocess
from pathlib import Path


REMOTE_ROOT = "/sdcard/FCL/mobilegl-trace"
SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_WRAPPER = SCRIPT_DIR / "out" / "egltrace.so"


def adb(serial, args):
    cmd = ["adb"]
    if serial:
        cmd += ["-s", serial]
    cmd += args
    print("+", " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--serial")
    sub = parser.add_subparsers(dest="command", required=True)

    install = sub.add_parser("install-wrapper")
    install.add_argument("--wrapper", default=str(DEFAULT_WRAPPER))

    sub.add_parser("enable")
    sub.add_parser("disable")
    sub.add_parser("capture-once")

    pull = sub.add_parser("pull-latest")
    pull.add_argument("--output", default=".trace-work/pulled-mobilegl-captures")

    args = parser.parse_args()
    serial = args.serial

    if args.command == "install-wrapper":
        wrapper = Path(args.wrapper)
        if not wrapper.exists():
            raise SystemExit(f"missing wrapper: {wrapper}")
        adb(serial, ["shell", "mkdir", "-p", REMOTE_ROOT])
        adb(serial, ["push", str(wrapper), f"{REMOTE_ROOT}/egltrace.so"])
    elif args.command == "enable":
        adb(serial, ["shell", "mkdir", "-p", REMOTE_ROOT])
        adb(serial, ["shell", f"printf enabled > {REMOTE_ROOT}/enable"])
    elif args.command == "disable":
        adb(serial, ["shell", "rm", "-f", f"{REMOTE_ROOT}/enable"])
    elif args.command == "capture-once":
        adb(serial, ["shell", "mkdir", "-p", REMOTE_ROOT])
        adb(serial, ["shell", f"date +%s%3N > {REMOTE_ROOT}/capture-once.request"])
    elif args.command == "pull-latest":
        tmp = subprocess.check_output((["adb"] + (["-s", serial] if serial else []) +
                                      ["shell", "cat", f"{REMOTE_ROOT}/latest-session.txt"]),
                                      text=True, encoding="utf-8", errors="replace").strip()
        if not tmp:
            raise SystemExit("no latest-session.txt on device")
        output = Path(args.output)
        output.mkdir(parents=True, exist_ok=True)
        adb(serial, ["pull", tmp, str(output / Path(tmp).name)])


if __name__ == "__main__":
    main()
