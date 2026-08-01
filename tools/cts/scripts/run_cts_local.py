#!/usr/bin/env python
"""Drive a glcts run on the local host, resuming across crashes.

Local-host counterpart of run_cts.py: MobileGL crashes on some cases and glcts
takes the whole process down with it, so a single invocation stops at the first
crash. This runner re-invokes glcts with only the cases that have no result
yet, records the case that was open when the process died as "Crash" (or
"Hang" on a timeout), and repeats until the list is exhausted.

Usage:
  python run_cts_local.py --backend DirectVulkan \\
      --glcts <path-to-glcts-binary> --lib <path-to-libMobileGL.so> \\
      --caselist <mustpass.txt> --outdir <dir> [--env K=V ...]
"""

import argparse
import glob
import os
import re
import resource
import subprocess
import sys
import time

CASE_START = re.compile(r"^#beginTestCaseResult\s+(\S+)")
CASE_END = re.compile(r"^#endTestCaseResult")
CASE_TERM = re.compile(r"^#terminateTestCaseResult")


def completed_cases(qpa_path):
    """Return (finished_case_names, last_started_case_or_None)."""
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
    ap.add_argument("--backend", required=True, choices=["DirectGLES", "DirectVulkan"])
    ap.add_argument("--glcts", required=True, help="path to the glcts binary")
    ap.add_argument("--lib", required=True, help="path to libMobileGL.so")
    ap.add_argument("--caselist", required=True)
    ap.add_argument("--outdir", required=True)
    ap.add_argument("--surface", default="fbo", help="--deqp-surface-type value")
    # Without an explicit size, dEQP's FboRenderContext sizes the wrapper FBO to
    # GL_MAX_RENDERBUFFER_SIZE (16384^2 here) and size-derived test allocations
    # explode (a 4-sample 16K depth texture alone is 4 GiB).
    ap.add_argument("--surface-size", type=int, default=256,
                    help="--deqp-surface-width/height value")
    ap.add_argument("--max-rounds", type=int, default=4000)
    ap.add_argument("--max-empty-streak", type=int, default=64,
                    help="abort after this many consecutive chunks that produce no log at all")
    ap.add_argument("--chunk-timeout", type=int, default=1800,
                    help="seconds before killing one glcts invocation (a wedged case never returns)")
    ap.add_argument("--skip-file", default=None,
                    help="file of case names to exclude, e.g. cases known to wedge the host")
    ap.add_argument("--waiver-file", default=None,
                    help="--deqp-waiver-file value, e.g. tools/cts/waivers/mobilegl-fbo-harness.xml")
    ap.add_argument("--env", action="append", default=[], metavar="K=V",
                    help="extra environment variable for glcts (repeatable)")
    args = ap.parse_args()

    os.makedirs(args.outdir, exist_ok=True)
    glcts = os.path.abspath(args.glcts)
    lib = os.path.abspath(args.lib)
    # glcts resolves its gl_cts data tree relative to the binary's directory.
    workdir = os.path.dirname(glcts)

    with open(args.caselist, "r", encoding="utf-8") as fh:
        remaining = [l.strip() for l in fh if l.strip() and not l.strip().startswith("#")]

    skipped = []
    if args.skip_file and os.path.isfile(args.skip_file):
        with open(args.skip_file, "r", encoding="utf-8") as fh:
            skip = {l.strip() for l in fh if l.strip() and not l.strip().startswith("#")}
        skipped = [c for c in remaining if c in skip]
        remaining = [c for c in remaining if c not in skip]
        print(f"[run_cts_local] skipping {len(skipped)} case(s) from {args.skip_file}")

    total = len(remaining)
    print(f"[run_cts_local] {args.backend}: {total} cases")

    env = dict(os.environ)
    env["MOBILEGL_BACKEND_TYPE"] = args.backend
    env["MOBILEGL_CTS_LIB"] = lib
    for kv in args.env:
        k, _, v = kv.partition("=")
        env[k] = v

    crashed = []
    hung = []
    done = set()
    chunk = 0
    started = time.time()
    empty_streak = 0

    # Resume: results in chunk files from an interrupted run still count. The
    # case that was open when that run died is re-tried rather than assumed bad.
    prior_chunks = sorted(glob.glob(os.path.join(args.outdir, "chunk*.qpa")))
    for prior in prior_chunks:
        finished, _ = completed_cases(prior)
        done.update(finished)
    if prior_chunks:
        chunk = int(re.search(r"chunk(\d+)\.qpa$", prior_chunks[-1]).group(1)) + 1
        remaining = [c for c in remaining if c not in done]
        print(f"[run_cts_local] resuming: {len(done)} case(s) already measured, "
              f"{len(remaining)} to go")

    while remaining and chunk < args.max_rounds:
        listfile = os.path.abspath(os.path.join(args.outdir, "remaining.txt"))
        with open(listfile, "w", encoding="utf-8", newline="\n") as fh:
            fh.write("\n".join(remaining) + "\n")

        qpa = os.path.abspath(os.path.join(args.outdir, f"chunk{chunk:04d}.qpa"))
        cmd = [
            glcts,
            f"--deqp-caselist-file={listfile}",
            f"--deqp-surface-type={args.surface}",
            f"--deqp-surface-width={args.surface_size}",
            f"--deqp-surface-height={args.surface_size}",
            "--deqp-terminate-on-device-lost=disable",
            # A wedged case aborts the process instead of stalling the chunk;
            # the runner then records it as Crash and resumes past it.
            "--deqp-watchdog=enable",
            "--deqp-log-images=disable",
            "--deqp-log-shader-sources=disable",
            f"--deqp-log-filename={qpa}",
        ]
        if args.waiver_file:
            cmd.append(f"--deqp-waiver-file={os.path.abspath(args.waiver_file)}")
        timed_out = False
        try:
            # RLIMIT_CORE=0: MobileGL asserts abort with a core dump, and writing
            # a multi-GB glcts core image after every crash dominates wall time.
            subprocess.run(cmd, cwd=workdir, env=env, timeout=args.chunk_timeout,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                           start_new_session=True,
                           preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
        except subprocess.TimeoutExpired:
            timed_out = True
            print(f"[run_cts_local] chunk {chunk:04d} timed out after {args.chunk_timeout}s",
                  file=sys.stderr)

        finished, in_flight = completed_cases(qpa)
        for c in finished:
            done.add(c)

        progressed = len(finished)
        if progressed > 0:
            empty_streak = 0
        if in_flight is not None:
            if timed_out:
                print(f"[run_cts_local] HANG in {in_flight} - quarantining it")
                hung.append(in_flight)
            else:
                crashed.append(in_flight)
            done.add(in_flight)
            progressed += 1
        elif progressed == 0:
            empty_streak += 1
            if empty_streak >= args.max_empty_streak:
                print(f"[run_cts_local] ABORTING: {empty_streak} consecutive chunks produced no "
                      f"output. Something systemic is wrong; refusing to label the rest of the "
                      f"suite as crashes.", file=sys.stderr)
                break
            victim = remaining[0]
            label = "Hang" if timed_out else "Crash"
            print(f"[run_cts_local] no output at all; recording {victim} as {label}")
            (hung if timed_out else crashed).append(victim)
            done.add(victim)
            progressed = 1

        remaining = [c for c in remaining if c not in done]
        elapsed = time.time() - started
        print(
            f"[run_cts_local] chunk {chunk:04d}: +{progressed} (done {len(done)}/{total}, "
            f"crashes {len(crashed)}, hangs {len(hung)}, {elapsed / 60:.1f} min)"
        )
        chunk += 1

    with open(os.path.join(args.outdir, "crashed.txt"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(crashed) + ("\n" if crashed else ""))
    with open(os.path.join(args.outdir, "hung.txt"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(hung) + ("\n" if hung else ""))
    with open(os.path.join(args.outdir, "unrun.txt"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(remaining) + ("\n" if remaining else ""))
    if skipped:
        with open(os.path.join(args.outdir, "skipped.txt"), "w", encoding="utf-8", newline="\n") as fh:
            fh.write("\n".join(skipped) + "\n")

    if remaining:
        print(f"[run_cts_local] WARNING: {len(remaining)} cases were never run (see unrun.txt)",
              file=sys.stderr)
    print(f"[run_cts_local] finished: {len(done)}/{total} cases, {len(crashed)} crashes, "
          f"{len(hung)} hangs, {chunk} invocations")
    print(f"[run_cts_local] qpa chunks in {args.outdir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
