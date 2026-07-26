#!/usr/bin/env python
"""Drive a glcts run on a device, resuming across crashes.

MobileGL crashes on some cases, and glcts takes the whole process down with it.
A single invocation would therefore stop at the first crash and leave most of
the suite unmeasured. This runner re-invokes glcts with only the cases that have
not produced a result yet, records each crashed case as "Crash", and repeats
until the list is exhausted, so one bad case costs one case rather than the run.

Usage:
  python run_cts.py --serial <adb-serial> --backend DirectGLES|DirectVulkan \\
      --caselist <host-path-to-mustpass.txt> --outdir <host-dir> [--device-dir /data/local/tmp/mgcts]
"""

import argparse
import os
import re
import subprocess
import sys
import time

CASE_START = re.compile(r"^#beginTestCaseResult\s+(\S+)")
CASE_END = re.compile(r"^#endTestCaseResult")
CASE_TERM = re.compile(r"^#terminateTestCaseResult\s+(.*)")


def adb(serial, *args, timeout=None):
    try:
        return subprocess.run(["adb", "-s", serial, *args], capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return subprocess.CompletedProcess(args, returncode=124, stdout="", stderr="adb timeout")


def device_alive(serial, timeout=30):
    """True only if the device answers a trivial shell command.

    Distinguishes "glcts crashed" from "the device fell over". Without this a
    dead device looks like every remaining case crashing, which silently turns a
    broken run into a plausible-looking conformance number.
    """
    r = adb(serial, "shell", "echo alive", timeout=timeout)
    return r.returncode == 0 and "alive" in (r.stdout or "")


def wait_for_device(serial, attempts=20, delay=15):
    for i in range(attempts):
        if device_alive(serial):
            return True
        print(f"[run_cts] device {serial} unresponsive, waiting ({i + 1}/{attempts})")
        time.sleep(delay)
    return False


def mem_available_kb(serial):
    r = adb(serial, "shell", "grep MemAvailable /proc/meminfo", timeout=30)
    m = re.search(r"(\d+)", r.stdout or "")
    return int(m.group(1)) if m else None


def completed_cases(qpa_path):
    """Return (finished_case_names, last_started_case_or_None).

    A case that was started but never closed is the one the process died in.
    """
    finished = []
    current = None
    if not os.path.exists(qpa_path):
        return finished, None
    with open(qpa_path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = CASE_START.match(line)
            if m:
                current = m.group(1)
                continue
            if current is not None and (CASE_END.match(line) or CASE_TERM.match(line)):
                finished.append(current)
                current = None
    return finished, current


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--serial", required=True)
    ap.add_argument("--backend", required=True, choices=["DirectGLES", "DirectVulkan"])
    ap.add_argument("--caselist", required=True)
    ap.add_argument("--outdir", required=True)
    ap.add_argument("--device-dir", default="/data/local/tmp/mgcts")
    ap.add_argument("--surface", default="fbo", help="--deqp-surface-type value")
    ap.add_argument("--max-rounds", type=int, default=4000)
    ap.add_argument("--max-empty-streak", type=int, default=64,
                    help="abort after this many consecutive chunks that produce no log at all")
    ap.add_argument("--min-mem-kb", type=int, default=400000,
                    help="pause when the device drops below this much available memory")
    ap.add_argument("--chunk-timeout", type=int, default=900,
                    help="seconds before giving up on one glcts invocation (a GPU hang never returns)")
    ap.add_argument("--skip-file", default=None,
                    help="file of case names to exclude, e.g. cases known to hang the device")
    ap.add_argument("--env", action="append", default=[], metavar="K=V",
                    help="extra environment variable for glcts (repeatable)")
    args = ap.parse_args()

    os.makedirs(args.outdir, exist_ok=True)

    with open(args.caselist, "r", encoding="utf-8") as fh:
        remaining = [l.strip() for l in fh if l.strip() and not l.strip().startswith("#")]

    skipped = []
    if args.skip_file and os.path.isfile(args.skip_file):
        with open(args.skip_file, "r", encoding="utf-8") as fh:
            skip = {l.strip() for l in fh if l.strip() and not l.strip().startswith("#")}
        skipped = [c for c in remaining if c in skip]
        remaining = [c for c in remaining if c not in skip]
        print(f"[run_cts] skipping {len(skipped)} case(s) from {args.skip_file}")

    total = len(remaining)
    print(f"[run_cts] {args.backend} on {args.serial}: {total} cases")

    crashed = []
    hung = []
    done = set()
    chunk = 0
    started = time.time()
    empty_streak = 0

    if not wait_for_device(args.serial):
        print("[run_cts] device not responding before start; aborting", file=sys.stderr)
        return 3

    while remaining and chunk < args.max_rounds:
        listfile = os.path.join(args.outdir, "remaining.txt")
        with open(listfile, "w", encoding="utf-8", newline="\n") as fh:
            fh.write("\n".join(remaining) + "\n")

        # Repeated process launches plus crash tombstones can drive the device
        # into memory pressure; give it room rather than pushing it over.
        mem = mem_available_kb(args.serial)
        if mem is not None and mem < args.min_mem_kb:
            print(f"[run_cts] low memory ({mem} kB available); pausing 30 s")
            time.sleep(30)

        dev_list = f"{args.device_dir}/remaining.txt"
        dev_qpa = f"{args.device_dir}/chunk.qpa"
        push = adb(args.serial, "push", listfile, dev_list, timeout=120)
        if push.returncode != 0:
            print(f"[run_cts] push failed ({push.stderr.strip()}); treating as device trouble",
                  file=sys.stderr)
            if not wait_for_device(args.serial):
                print("[run_cts] ABORTING: device unreachable.", file=sys.stderr)
                break
            continue
        adb(args.serial, "shell", f"rm -f {dev_qpa}", timeout=60)

        extra_env = "".join(f"{kv} " for kv in args.env)
        cmd = (
            f"cd {args.device_dir} && "
            f"MOBILEGL_BACKEND_TYPE={args.backend} LD_LIBRARY_PATH=. {extra_env}"
            f"./glcts --deqp-caselist-file={dev_list} "
            f"--deqp-surface-type={args.surface} "
            f"--deqp-terminate-on-device-lost=disable "
            f"--deqp-log-images=disable --deqp-log-shader-sources=disable "
            f"--deqp-log-filename={dev_qpa} > /dev/null 2>&1; echo RC=$?"
        )
        run = adb(args.serial, "shell", cmd, timeout=args.chunk_timeout)
        if run.returncode == 124:
            print(f"[run_cts] chunk {chunk:04d} timed out after {args.chunk_timeout}s "
                  f"(likely a GPU hang)", file=sys.stderr)

        # Some cases hang the GPU hard enough to reboot the device. The log on
        # /data/local/tmp survives that, so wait for the device to come back and
        # pull it anyway rather than losing the whole chunk.
        rebooted = False
        if not device_alive(args.serial, timeout=30):
            print(f"[run_cts] device went away during chunk {chunk:04d}; waiting for it",
                  file=sys.stderr)
            if not wait_for_device(args.serial, attempts=40, delay=15):
                print("[run_cts] ABORTING: device never came back. Results are incomplete; "
                      "do NOT treat the remaining cases as failures.", file=sys.stderr)
                break
            rebooted = True
            print("[run_cts] device is back")

        local_qpa = os.path.join(args.outdir, f"chunk{chunk:04d}.qpa")
        pull = adb(args.serial, "pull", dev_qpa, local_qpa, timeout=300)
        if pull.returncode != 0 and rebooted:
            time.sleep(10)
            adb(args.serial, "pull", dev_qpa, local_qpa, timeout=300)

        finished, in_flight = completed_cases(local_qpa)
        for c in finished:
            done.add(c)

        progressed = len(finished)
        if progressed > 0:
            empty_streak = 0
        if in_flight is not None:
            # The case that was open when the process (or the device) died.
            if rebooted:
                # It took the whole device down: quarantine it, or the next
                # invocation walks straight back into it.
                print(f"[run_cts] DEVICE HANG in {in_flight} - quarantining it")
                hung.append(in_flight)
            else:
                crashed.append(in_flight)
            done.add(in_flight)
            progressed += 1
        elif progressed == 0:
            # Nothing at all came back. Either the first remaining case takes
            # the process down before the log is flushed, or the device died.
            # Those look identical from here, so confirm the device is alive
            # before blaming the test.
            if not device_alive(args.serial):
                print(f"[run_cts] device went away during chunk {chunk:04d}", file=sys.stderr)
                if not wait_for_device(args.serial):
                    print("[run_cts] ABORTING: device never came back. Results are "
                          "incomplete; do NOT treat the remaining cases as crashes.", file=sys.stderr)
                    break
                print("[run_cts] device recovered; retrying the same chunk")
                continue

            empty_streak += 1
            if empty_streak >= args.max_empty_streak:
                print(f"[run_cts] ABORTING: {empty_streak} consecutive chunks produced no output "
                      f"while the device stayed reachable. Something systemic is wrong; refusing "
                      f"to label the rest of the suite as crashes.", file=sys.stderr)
                break

            victim = remaining[0]
            print(f"[run_cts] no output at all; recording {victim} as Crash")
            crashed.append(victim)
            done.add(victim)
            progressed = 1

        remaining = [c for c in remaining if c not in done]
        elapsed = time.time() - started
        print(
            f"[run_cts] chunk {chunk:04d}: +{progressed} (done {len(done)}/{total}, "
            f"crashes {len(crashed)}, {elapsed / 60:.1f} min)"
        )
        chunk += 1

    with open(os.path.join(args.outdir, "crashed.txt"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(crashed) + ("\n" if crashed else ""))

    # Cases that rebooted the device. Feed this back in via --skip-file to avoid
    # paying for the same reboot on the next run.
    with open(os.path.join(args.outdir, "hung.txt"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(hung) + ("\n" if hung else ""))
    if hung:
        print(f"[run_cts] {len(hung)} case(s) hung the device (see hung.txt):")
        for c in hung:
            print(f"    {c}")

    # Anything still in `remaining` was never measured. Record it so the report
    # cannot quietly present a partial run as a complete one.
    with open(os.path.join(args.outdir, "unrun.txt"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(remaining) + ("\n" if remaining else ""))
    if skipped:
        with open(os.path.join(args.outdir, "skipped.txt"), "w", encoding="utf-8", newline="\n") as fh:
            fh.write("\n".join(skipped) + "\n")

    if remaining:
        print(f"[run_cts] WARNING: {len(remaining)} cases were never run (see unrun.txt)", file=sys.stderr)
    print(f"[run_cts] finished: {len(done)}/{total} cases, {len(crashed)} crashes, {chunk} invocations")
    print(f"[run_cts] qpa chunks in {args.outdir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
