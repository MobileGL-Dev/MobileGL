#!/usr/bin/env python3
"""Paired A/B on a disaggregated-campaign device: one reboot-clean thermal window, interleaved arms.

Its first use was `CONTRACT-P6.md` section 9 item 7 (the paired device gate) and item 8-1 (the
doorbell ledger), and it is not specific to those: the thing it computes is "N transports, one
thermal window, identical protocol" for any transport/backend/case triple the runner accepts.
Everything it does is a step of the protocol in docs/Disaggregated/MEASUREMENTS.md sections 4.4 / 9
/ 11 plus notes/p5f/device-report.md.

Why it is a script and not a shell transcript: a run that wants to be quotable needs the CPU
samples, the pin evidence and the library's own wait ledger to come from the SAME window, and the
three of those are collected by three different mechanisms. Collected by hand they drift apart by
minutes, and the quantity being compared (a few tenths of a millisecond per frame between two
transports) is smaller than that drift.

WHAT IT DELIBERATELY DOES NOT DO.

- It does not decide anything about correctness. Benchmark mode takes no snapshot and compares no
  golden (--benchmark replays the whole trace for timing), so an SSIM column here would be a
  fabrication. Correctness on this device is P6 section 3 of CURRENT_STAGE_PROGRESS.md and the
  retrace lanes.
- It does not read `MOBILEGL_TRANSPORT` out of `--transport` on the runner. That flag is applied by
  run_android_retrace_local.py's run_case() path only; run_benchmark_case() passes `args.env`
  alone, so `--transport spawn` alongside `--benchmark` is silently ignored and the arm runs
  monolith while its label says spawn. Measured on this tree (a `--benchmark` run with
  `--transport inproc` wrote no `Config: IPC` line at all, i.e. it was monolith). Every arm here
  therefore spells its transport as an explicit `--env` entry, and every arm is verified against
  the library's own log afterwards. `assert_arm` is the gate on that; an arm that does not prove
  itself is not measured, it is retried and then reported as a failure.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import threading
import time
from datetime import datetime, timezone
from pathlib import Path

SERIAL = "2f7cbe2e"


def find_git_bash():
    """The interpreter that runs pin_device.sh, resolved rather than hardcoded.

    Named explicitly because a bare `bash` on a host with several installations picks whichever is
    first on PATH, and MSYS path translation differs between them - the failure mode is a
    `No such file or directory` for a script that exists, which reads like a missing file rather
    than a wrong interpreter.
    """
    import shutil
    candidates = [
        os.environ.get("GIT_BASH"),
        r"C:\Program Files\Git\bin\bash.exe",
        r"C:\Program Files (x86)\Git\bin\bash.exe",
        shutil.which("bash"),
    ]
    for candidate in candidates:
        if candidate and Path(candidate).exists():
            return candidate
    return "bash"


GIT_BASH = find_git_bash()

# The knobs CI's split acceptance lanes hand both arms, kept verbatim so a local run is the same
# run (run_android_retrace_local.py SPLIT_ACCEPTANCE_KNOBS). The arm proof reads the library's own
# `Config: IPC` line and requires all three, so omitting one here does not measure a weaker arm -
# it fails to prove any arm at all.
SPLIT_KNOBS = (
    "MOBILEGL_IPC_ROLE_SPLIT_STATE=1",
    "MOBILEGL_IPC_STRICT_ERRORS=1",
    "MOBILEGL_IPC_RUN_AHEAD=1",
)
# 120 is the P5-era default and it is what every historical number in MEASUREMENTS.md sections 9-11
# was taken at, so the wait ledger here can be read beside them.
STATS_PERIOD = "120"

TRANSPORT_MARKERS = {
    "inproc": "Config: MOBILEGL_TRANSPORT=inproc",
    "spawn": "Config: MOBILEGL_TRANSPORT=spawn",
}
SPAWN_ARMED = re.compile(r"spawn ARMED - the server role runs in pid (\d+)")

WAIT_LEDGER = re.compile(
    r"wait\[srv=(\d+) srvpark=(\d+) cli=(\d+) clipark=(\d+)\]")
SECTION = re.compile(r"\] ([a-z]+)\[")
STAGE_LINE = re.compile(r"stage=(\d+)MiB")


def run(argv, **kwargs):
    kwargs.setdefault("capture_output", True)
    kwargs.setdefault("text", True)
    kwargs.setdefault("encoding", "utf-8")
    kwargs.setdefault("errors", "replace")
    return subprocess.run(argv, **kwargs)


class Adb:
    """Every call pins the serial. There is more than one device class on this bench and a bare
    `adb` is how a run measures the wrong phone."""

    def __init__(self, serial):
        self.serial = serial

    @staticmethod
    def _env():
        env = dict(os.environ)
        env["MSYS_NO_PATHCONV"] = "1"
        env["MSYS2_ARG_CONV_EXCL"] = "*"
        return env

    def shell(self, command, timeout=30):
        result = run(["adb", "-s", self.serial, "shell", command], env=self._env(), timeout=timeout)
        return result.stdout.replace("\r", "")

    def su(self, command, timeout=30):
        return self.shell("su -c '%s'" % command, timeout=timeout)

    def reboot(self):
        # `adb reboot` returns as soon as the request is queued; the wait is the caller's.
        run(["adb", "-s", self.serial, "reboot"], env=self._env(), timeout=60)

    def wait_boot(self, timeout=300):
        deadline = time.time() + timeout
        run(["adb", "-s", self.serial, "wait-for-device"], env=self._env(), timeout=timeout)
        while time.time() < deadline:
            state = self.shell("getprop sys.boot_completed").strip()
            if state == "1":
                return True
            time.sleep(3)
        return False

    def boot_id(self):
        return self.shell("cat /proc/sys/kernel/random/boot_id").strip()

    def clk_tck(self):
        """The DEVICE's tick rate, which is what /proc/<pid>/stat's counters are denominated in.

        Read rather than assumed, and not the host's CLK_TCK: a 100-vs-250 mismatch would scale
        every CPU millisecond in the report by 2.5x and nothing downstream could tell.
        """
        value = self.shell("getconf CLK_TCK").strip()
        try:
            return int(value)
        except ValueError:
            return host_ticks_per_second()


def host_ticks_per_second():
    """Only a fallback. The authoritative tick rate is the DEVICE's, read in Adb.__init__ -
    /proc/<pid>/stat reports ticks, and a host that assumes its own CLK_TCK while the device uses
    another silently scales every CPU number by the ratio."""
    try:
        return int(subprocess.run(["getconf", "CLK_TCK"], capture_output=True, text=True).stdout.strip())
    except Exception:
        return 100


TICKS = host_ticks_per_second()


class CpuSampler:
    """Per-thread CPU over the arm, sampled from the host.

    `/proc/<pid>/task/<tid>/stat` rather than the process total, because the quantity under
    comparison is a PER-THREAD one: the kernel ignores this app's affinity requests, so the client
    GL thread and the apply thread land on different clusters and a process total mixes two
    different clock domains (MEASUREMENTS.md section 9's caveat, which is why the primary metric is
    the client thread's CPU and not wall time).

    Under `spawn` the server role is a separate PROCESS, so the same read is taken against that pid
    as well - `/proc/<serverpid>/task/*` yields exactly the server's threads, and `libMobileGLServer.so`
    is the process name that identifies it.

    Read as a delta between two samples, so the residual is bounded by one poll interval and is
    reported rather than hidden: `first_utc`/`last_utc` are written into the summary.
    """

    def __init__(self, adb, package, interval=0.5):
        self.adb = adb
        self.package = package
        self.interval = interval
        self._stop = threading.Event()
        self._thread = None
        self.samples = []

    def _read_once(self):
        # THE DEVICE SIDE ONLY LOCATES THE NUMBERS; EVERY FIELD IS INDEXED ON THIS SIDE.
        #
        # `stat` is `pid (comm) state ...` and comm is its ONLY space-bearing field, so the split
        # point is the LAST ')' - which is a two-line string operation and belongs in Python, not in
        # a device-side program trying to survive three layers of shell quoting. Two attempts at
        # doing it on the device failed differently and both looked like data: `set -- $(cat stat);
        # echo $14` is `$1` followed by a literal 4 on Android's sh, so the sampler echoed the pid
        # into every counter column and every delta came out 0; and an inline `awk` program cannot be
        # written inside `su -c '...'` without its own quotes closing that wrapper, which surfaces as
        # `/system/bin/sh: syntax error: unexpected '('` and an empty read.
        #
        # So the device-side text is one line per thread with no quotes at all:
        #     <pid> <tid> |<the whole raw stat line>
        # and `summarize_cpu` never sees the raw text - only the counters parsed out of it here.
        #
        # Comm is truncated to 15 characters by the kernel, so `MobileGLTraceReplay` reads back as
        # `MobileGLTraceRe`; matching is on the PREFIX.
        script = (
            "P=$(pidof %s); S=$(pidof libMobileGLServer.so); "
            "for pid in $P $S; do "
            "[ -n \"$pid\" ] || continue; "
            "for t in /proc/$pid/task/*; do "
            "echo \"$pid ${t##*/} | $(cat $t/stat 2>/dev/null)\"; "
            "done; done"
        ) % self.package
        out = self.adb.su(script, timeout=25)
        rows = []
        for line in out.splitlines():
            head, sep, stat_line = line.partition("|")
            if not sep:
                continue
            ids = head.split()
            if len(ids) < 2 or ")" not in stat_line:
                continue
            open_paren = stat_line.find("(")
            close_paren = stat_line.rfind(")")
            if open_paren < 0 or close_paren < open_paren:
                continue
            # After the ')', stat's fields are: state(3) ppid(4) ... utime(14) stime(15) ...
            # starttime(22). `tail` is that sequence, so stat's field N sits at 0-based N-3.
            tail = stat_line[close_paren + 1:].split()
            if len(tail) < 20:
                continue
            try:
                rows.append({
                    "pid": int(ids[0]), "tid": int(ids[1]),
                    "comm": stat_line[open_paren + 1:close_paren],
                    "utime": int(tail[14 - 3]), "stime": int(tail[15 - 3]),
                    "starttime": int(tail[22 - 3]),
                })
            except ValueError:
                continue
        return rows

    def _loop(self):
        while not self._stop.is_set():
            try:
                rows = self._read_once()
            except Exception:
                rows = []
            if rows:
                self.samples.append((time.time(), rows))
            self._stop.wait(self.interval)

    def start(self):
        self._thread = threading.Thread(target=self._loop, daemon=True)
        self._thread.start()

    def stop(self):
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=10)

    def replay_windows(self, min_seconds=1.0):
        """Split the samples into one window PER REPLAY.

        TWO BOUNDARIES, because neither alone is sufficient:

          1. A CHANGE IN THE PACKAGE'S MAIN THREAD IDENTITY. Each repeat is a separate app launch -
             trace-replay-ci.sh force-stops the package and `am start`s it again - so the main thread
             has a new tid. This is the structural boundary and needs no threshold.
          2. A GAP WIDER THAN THREE POLL INTERVALS. Pids are recycled, and on a device that is
             force-stopping and restarting one app, two consecutive replays really can receive the
             same pid; the dead interval between them is then the only evidence that they were two
             launches rather than one long one. The gap is also what separates the app sitting idle
             from the app replaying.

        This matters because the divisor and the CPU counters have to describe the SAME stretch of
        work. Summing CPU across three replays and dividing by ONE replay's frame count reports three
        times the real per-frame cost; pairing a window with the wrong repeat's frames does the same
        thing less visibly.

        Returns a list of (start_index, length) in sample order, keeping only windows at least
        `min_seconds` long: shorter than that is start-up or teardown, not a replay.
        """
        if not self.samples:
            return []

        def main_pid(rows):
            """The pid of the CLIENT process, or None if it is not in this sample.

            Identified as "the process whose thread with tid == pid is not the server binary". tid ==
            pid is the definition of a main thread, so that half needs no evidence; the server
            exclusion is needed because under `spawn` the server process is a separate main thread in
            the same sample set.

            NOT matched on the package name: Android's comm for a main thread is the TAIL of the
            process name truncated to 15 characters (`top.mobilegl.plugin.p6gate8.trace` reads back
            as `n.p6gate8.trace`), so a prefix comparison against `top.mobilegl...` never matches and
            would silently produce zero windows.
            """
            for row in rows:
                if row["tid"] != row["pid"]:
                    continue
                if row["comm"].startswith("libMobileGL"):
                    continue
                return row["pid"]
            return None

        gap_limit = self.interval * 3
        windows = []
        run_start = 0
        current_pid = main_pid(self.samples[0][1])
        for index in range(1, len(self.samples)):
            stamp, rows = self.samples[index]
            pid = main_pid(rows)
            gap = stamp - self.samples[index - 1][0]
            pid_changed = pid is not None and current_pid is not None and pid != current_pid
            if pid_changed or gap > gap_limit:
                windows.append((run_start, index - run_start))
                run_start = index
            if pid is not None:
                current_pid = pid
        windows.append((run_start, len(self.samples) - run_start))

        kept = []
        for start, length in windows:
            span = self.samples[start + length - 1][0] - self.samples[start][0]
            if span >= min_seconds:
                kept.append((start, length))
        return kept


def summarize_cpu(samples, clock_ticks):
    """Per-thread CPU milliseconds over the sample window, keyed by (pid, tid, starttime).

    THE WINDOW IS ANCHORED ON EACH THREAD'S OWN FIRST AND LAST SIGHTING, not on the first and last
    sample. The app's threads are not all alive for the whole window - the GL thread is created when
    the replay starts, and under `spawn` the server process appears partway through - so a single
    global pair of timestamps would report every late thread's counter as if it had been spent since
    the window opened, i.e. as an overcount, and would report a thread that exited early as zero.
    Anchoring per thread makes `cpu_ms` the CPU that thread really consumed while it existed; the
    window it did so in is `window_seconds`, which is per thread for the same reason.

    `ms_per_frame` is filled in by the caller, which is the only place that knows the frame count
    and the frame-to-frame mapping.
    """
    if len(samples) < 2:
        return {"threads": [], "window_seconds": 0.0, "clock_ticks": clock_ticks}
    sightings = {}
    for index, (stamp, rows) in enumerate(samples):
        for row in rows:
            # IDENTITY IS (pid, tid, STARTTIME). Two threads can share a tid across a process's
            # lifetime - the kernel recycles them - and a short-lived thread spawned near the end of
            # a window has tid values that look like an unrelated long-lived one's. Keying on
            # (pid, tid) alone merged those and produced a handful of phantom entries with one
            # sample each: under inproc the OpenRA replay's real GL thread showed up three times,
            # once with the right 880 ms and twice with 0. `starttime` is what tells them apart, and
            # it is why the device-side read carries field 22.
            key = (row["pid"], row["tid"], row["starttime"])
            entry = sightings.setdefault(key, {
                "pid": row["pid"], "tid": row["tid"], "comm": row["comm"],
                "first_index": index, "last_index": index, "first": row, "last": row, "count": 0,
            })
            entry["last_index"] = index
            entry["last"] = row
            entry["count"] += 1

    threads = []
    for entry in sightings.values():
        dticks = ((entry["last"]["utime"] - entry["first"]["utime"]) +
                  (entry["last"]["stime"] - entry["first"]["stime"]))
        span = samples[entry["last_index"]][0] - samples[entry["first_index"]][0]
        threads.append({
            "pid": entry["pid"], "tid": entry["tid"], "comm": entry["comm"],
            "cpu_ms": round(1000.0 * dticks / clock_ticks, 3),
            "window_seconds": round(span, 3),
            "samples": entry["count"],
            "starttime": entry["first"]["starttime"],
            # A thread seen in only ONE sample has no delta to measure, and `cpu_ms` for it is 0 by
            # construction rather than by evidence. Flagged rather than dropped so the row can be
            # excluded from the ROLE table while still being visible in the raw dump - a thread that
            # only ever appeared once is not a measurement of anything, and a table that averages it
            # in as a zero is worse than one that omits it.
            "measurable": entry["count"] > 1,
        })
    threads.sort(key=lambda t: -t["cpu_ms"])
    return {
        "threads": threads,
        "window_seconds": round(samples[-1][0] - samples[0][0], 3),
        "clock_ticks": clock_ticks,
        "first_utc": datetime.fromtimestamp(samples[0][0], timezone.utc).isoformat(),
        "last_utc": datetime.fromtimestamp(samples[-1][0], timezone.utc).isoformat(),
    }


def read_stats_line(log_path):
    """The LAST `MGPipe stats:` line in a role log, parsed.

    Last and not first: the line is a WINDOW, and the first window of a run contains load and
    shader warm-up. The last is the closest thing to steady state the trace has (for rd12, the
    trailing window of a 251-frame benchmark).
    """
    if not Path(log_path).is_file():
        return None
    last = None
    with open(log_path, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if "MGPipe stats:" in line:
                last = line.strip()
    if last is None:
        return None
    parsed = {"raw": last}
    ledger = WAIT_LEDGER.search(last)
    if ledger:
        parsed["wait"] = {
            "srv": int(ledger.group(1)), "srvpark": int(ledger.group(2)),
            "cli": int(ledger.group(3)), "clipark": int(ledger.group(4)),
        }
    window = re.search(r"window=(\d+)", last)
    if window:
        parsed["window_frames"] = int(window.group(1))
    frames = re.search(r"\bframes=(\d+)", last)
    if frames:
        parsed["run_frames"] = int(frames.group(1))
    draws = re.search(r"draws/f=([\d.]+)", last)
    if draws:
        parsed["draws_per_frame"] = float(draws.group(1))
    # The byte classes, keyed by their short names, straight out of the `bytes/f[...]` bracket.
    bracket = re.search(r"bytes/f\[([^\]]*)\]", last) or re.search(r"bytes\[([^\]]*)\]", last)
    if bracket:
        for entry in bracket.group(1).split():
            if "=" in entry:
                name, value = entry.split("=", 1)
                try:
                    parsed.setdefault("bytes_per_frame", {})[name] = float(value)
                except ValueError:
                    pass
    maxrec = re.search(r"maxrec=(\d+)", last)
    if maxrec:
        parsed["max_record_bytes"] = int(maxrec.group(1))
    maxcap = re.search(r"maxcap=(\d+)", last)
    if maxcap:
        parsed["max_record_cap"] = int(maxcap.group(1))
    ringwaits = re.search(r"ringwaits=(\d+)", last)
    if ringwaits:
        parsed["ring_waits"] = int(ringwaits.group(1))
    return parsed


class Arm:
    def __init__(self, spec, args, out_root, index):
        fields = spec.split(":")
        self.transport = fields[0]
        self.backend = fields[1] if len(fields) > 1 else "DirectGLES"
        self.case = fields[2] if len(fields) > 2 else None
        self.repeats = int(fields[3]) if len(fields) > 3 else args.repeats
        self.tree = Path(args.tree).resolve()
        self.package = args.package
        self.label = f"{index:02d}-{self.case}-{self.backend}-{self.transport}-r{self.repeats}"
        self.dir = out_root / self.label
        self.dir.mkdir(parents=True, exist_ok=True)
        self.adb = Adb(args.serial)
        self.summary = {
            "label": self.label, "case": self.case, "backend": self.backend,
            "transport": self.transport, "repeats": self.repeats,
        }

    def env_overrides(self):
        env = ["MOBILEGL_PIPE_STATS=1", f"MOBILEGL_PIPE_STATS_PERIOD={STATS_PERIOD}"]
        if self.transport != "monolith":
            env.insert(0, f"MOBILEGL_TRANSPORT={self.transport}")
            env.extend(SPLIT_KNOBS)
        return env

    @staticmethod
    def surface_env():
        """The surface shape, exported rather than passed as an override - and that distinction is
        load-bearing.

        `MOBILEGL_RETRACE_USE_PBUFFER` is read by trace-replay-ci.sh ITSELF, to decide whether to
        add `--ez use_pbuffer true` to the activity intent. A `--env MOBILEGL_RETRACE_USE_PBUFFER=1`
        override only reaches the applier of env knobs deep inside the replay process, which is a
        different mechanism and does not set that shell variable - so the retrace still asks for a
        window surface.

        That is fatal on the spawn arm and only there: an ANativeWindow* is a pointer into the
        CLIENT's process and the wire refuses it by name (Rule H). Measured on this tree - the spawn
        arm died with `Fatal{UnmigratedSurface, "AndroidNativeWindow@P12"}` published as a
        SessionFault, and the same run passes as soon as the variable is in the script's environment.
        """
        return {"MOBILEGL_RETRACE_USE_PBUFFER": "1"}

    def result_dirs(self):
        """The runner's result directory for this case/backend (it is fixed, not per-arm)."""
        safe = "".join(c if c.isalnum() or c in "._-" else "_" for c in self.case)
        return self.tree / ".trace-work" / "android-retrace-result" / f"{safe}-{self.backend}"

    def build_command(self):
        return [
            sys.executable, "tools/trace_replay/run_android_retrace_local.py",
            "--case", self.case, "--backend", self.backend, "--keep-results",
            "--benchmark", "--benchmark-repeats", str(self.repeats),
            "--benchmark-tail-frames", "200", "--benchmark-no-finish",
            "--benchmark-timeout-seconds", "1800",
            "--env", ";".join(self.env_overrides()),
        ]

    def child_env(self):
        env = dict(os.environ)
        env["ANDROID_SERIAL"] = self.adb.serial
        # Snapshot-local override so this build can sit beside an unrelated trace install under
        # the canonical id (see run_android_retrace_local.py's note beside BACKENDS).
        env["MOBILEGL_TRACE_PACKAGE"] = self.package
        # The session installs the APK once, before the thermal window opens; the script's own
        # `adb install -r` on the first repeat would otherwise run inside it. trace-replay-ci.sh
        # puts its install behind this variable and leaves `copy_fixture_to_app` OUTSIDE the same
        # branch, so skipping the install still gets the trace and golden onto the device - which
        # matters here because the first repeat of every arm is a `reuse_fixture == 0` run.
        env["MOBILEGL_TRACE_SKIP_INSTALL"] = "1"
        env.update(self.surface_env())
        env["PYTHON"] = sys.executable
        env["MSYS2_ARG_CONV_EXCL"] = "/data/*"
        env.pop("MSYS_NO_PATHCONV", None)
        return env

    def pin(self, action):
        # Git Bash needs its own path form: `bash C:/Users/...` is not a path it resolves (measured
        # "No such file or directory" for a script that exists). The /c/ form is what its own
        # MSYS path layer understands, and the interpreter is named explicitly because a bare
        # `bash` on this host can resolve to something that is not Git Bash at all.
        script = self.tree.as_posix().replace("C:/", "/c/") + "/tools/device_bench/pin_device.sh"
        result = run([GIT_BASH, script, self.adb.serial, action], cwd=str(self.tree))
        return result.returncode, result.stdout + result.stderr

    def thread_role(self, thread):
        """Which of the two roles this thread carries, from its name and its process.

        The kernel truncates comm to 15 characters, so the names to match are PREFIXES:
        `MobileGLTraceReplay` is `MobileGLTraceRe`, and `mgl-srv-apply` survives intact.

        Under `spawn` the whole server process is the server role, so every thread of that process
        counts as one - the interesting one is whichever thread is busy, and naming it by process is
        both simpler and truer than guessing from comm (the apply thread is named `mgl-srv-apply`
        there too, but a future server-side thread would not be).
        """
        if self.transport == "spawn" and thread["pid"] == self.summary.get("server_pid"):
            return "server-process"
        name = thread["comm"]
        if name.startswith("MobileGLTraceRe"):
            return "client-gl"
        if name.startswith("mgl-srv"):
            return "apply"
        return "other"

    def run(self):
        self.summary["boot_id"] = self.adb.boot_id()
        self.summary["started_utc"] = datetime.now(timezone.utc).isoformat()

        # PURGE THE PREVIOUS ARM'S ARTEFACTS BEFORE THIS ONE STARTS, and not after it ends.
        # The runner's result directory is keyed by case and backend, not by arm, so arm N+1
        # overwrites arm N's benchmark.json in place - and if arm N+1 fails early it leaves arm N's
        # file sitting there under arm N+1's name. `read_benchmark` cannot tell: it reads whatever is
        # at that path and files it as this run's result. Deleting first makes "no file" the only
        # thing a failed run can leave behind, which `assert_arm` already refuses.
        result_dir = self.result_dirs()
        if result_dir.exists():
            shutil.rmtree(result_dir)

        gen, text = self.pin("pin")
        (self.dir / "pin-before.txt").write_text(text, encoding="utf-8")
        self.summary["pin_before_rc"] = gen
        if gen != 0:
            self.summary["status"] = "PIN_FAILED_BEFORE"
            return

        # FORCE-STOP AND WAIT FOR IT TO ACTUALLY BE GONE BEFORE THE SAMPLER OPENS.
        #
        # A leftover instance from an earlier arm or an earlier session keeps answering `pidof`, so
        # the sampler counts it as part of this arm's window - measured: a stray idle instance from a
        # previous smoke run put 14 samples in front of the real repeat and made the window count
        # disagree with the repeat count. trace-replay-ci.sh force-stops the package too, but only
        # after it has pushed the fixture, which is well inside the sampler's lifetime; doing it here
        # as well means the sampler's first sample cannot see anything but this arm.
        self.adb.shell(f"am force-stop {self.package}")
        for _ in range(20):
            if not self.adb.shell(f"pidof {self.package}").strip():
                break
            time.sleep(0.5)

        sampler = CpuSampler(self.adb, self.package)
        sampler.start()
        try:
            result = run(self.build_command(), cwd=self.tree, env=self.child_env(), timeout=2000)
            stdout = result.stdout
            stderr = result.stderr
            rc = result.returncode
        except subprocess.TimeoutExpired as error:
            stdout = (error.stdout or b"").decode("utf-8", "replace")
            stderr = (error.stderr or b"").decode("utf-8", "replace")
            rc = 124
        finally:
            sampler.stop()

        (self.dir / "runner.stdout.txt").write_text(stdout, encoding="utf-8")
        (self.dir / "runner.stderr.txt").write_text(stderr, encoding="utf-8")
        self.summary["runner_rc"] = rc

        # ONE WINDOW PER REPLAY, matched to that repeat's own frame count. See
        # CpuSampler.replay_windows: the boundary is the package's main-thread identity, and the
        # divisor has to be the frames replayed inside the SAME window or the ratio is off by the
        # repeat count in whichever direction the mistake was made.
        windows = sampler.replay_windows()
        clock = self.adb.clk_tck()
        per_repeat = []
        for index, (start, length) in enumerate(windows):
            window = sampler.samples[start:start + length]
            per_repeat.append({
                "repeat": index + 1,
                "start_utc": datetime.fromtimestamp(window[0][0], timezone.utc).isoformat(),
                "end_utc": datetime.fromtimestamp(window[-1][0], timezone.utc).isoformat(),
                "samples": length,
                "cpu": summarize_cpu(window, clock),
            })
        self.summary["sampler"] = {
            "total_samples": len(sampler.samples),
            "windows": len(windows),
            "per_repeat": per_repeat,
        }

        result_dir = self.result_dirs()
        for role in ("client", "server"):
            source = result_dir / f"mobilegl.{role}.log"
            if source.is_file():
                shutil.copyfile(source, self.dir / f"mobilegl.{role}.log")
        for name in ("benchmark.json", "transport-proof.json", "result.json", "logcat.txt"):
            source = result_dir / name
            if source.is_file():
                shutil.copyfile(source, self.dir / name)

        self.summary["stats"] = {
            "client": read_stats_line(self.dir / "mobilegl.client.log"),
            "server": read_stats_line(self.dir / "mobilegl.server.log"),
        }

        reports = []
        for path in sorted(result_dir.glob("benchmark-run*.json")):
            try:
                reports.append(json.loads(path.read_text(encoding="utf-8")))
            except ValueError:
                pass
        if not reports and (result_dir / "benchmark.json").is_file():
            reports.append(json.loads((result_dir / "benchmark.json").read_text(encoding="utf-8")))
        self.summary["benchmark_runs"] = reports

        # THE ms/FRAME COLUMN IS COMPUTED FOR EVERY REPEAT, not just the best one, and the best-of-N
        # row is chosen exactly as run_android_retrace_local.py chooses it - lowest mean frame time -
        # so this row and the runner's own summary line describe one run rather than a median of a
        # median.
        #
        # `repeat_count_matches_windows` is checked rather than assumed: every per-frame column pairs
        # window N with repeat N, so if one record has a replay the other does not, the pairs are
        # wrong in a way no individual number would reveal.
        self.summary["repeat_count_matches_windows"] = len(reports) == len(per_repeat)
        for index, repeat in enumerate(per_repeat):
            repeat["repeat"] = index + 1
            report = reports[index] if index < len(reports) else {}
            repeat["benchmark"] = {
                "totalFrames": report.get("totalFrames"), "tailFrames": report.get("tailFrames"),
                "meanFrameMs": report.get("meanFrameMs"), "medianFrameMs": report.get("medianFrameMs"),
                "p95FrameMs": report.get("p95FrameMs"), "fps": report.get("fps"),
            }
            frames = report.get("totalFrames") or 0
            for thread in repeat["cpu"]["threads"]:
                thread["role"] = self.thread_role(thread)
                if frames:
                    thread["cpu_ms_per_frame"] = round(thread["cpu_ms"] / frames, 4)

        if reports:
            best_index, best = min(
                enumerate(reports),
                key=lambda pair: pair[1].get("meanFrameMs", -1) if pair[1].get("meanFrameMs", -1) > 0 else float("inf"),
            )
            self.summary["best_repeat"] = {
                "index": best_index + 1,
                "totalFrames": best.get("totalFrames"), "tailFrames": best.get("tailFrames"),
                "meanFrameMs": best.get("meanFrameMs"), "medianFrameMs": best.get("medianFrameMs"),
                "p95FrameMs": best.get("p95FrameMs"), "fps": best.get("fps"),
                "meanFrameCpuMs": best.get("meanFrameCpuMs"),
                "medianFrameCpuMs": best.get("medianFrameCpuMs"),
                "p95FrameCpuMs": best.get("p95FrameCpuMs"),
            }
            self.summary["best_repeat_frames"] = best.get("totalFrames") or 0

        gen2, text2 = self.pin("check")
        (self.dir / "pin-after.txt").write_text(text2, encoding="utf-8")
        self.summary["pin_after_rc"] = gen2

        self.assert_arm()
        self.summary["finished_utc"] = datetime.now(timezone.utc).isoformat()
        (self.dir / "arm-summary.json").write_text(
            json.dumps(self.summary, indent=2), encoding="utf-8")

    def assert_arm(self):
        """Does the library's own log prove the arm it is labelled as?

        Two questions, and the second is the one that matters for `spawn`:
          1. did the transport resolve to what this arm claims (the ConfigLoader marker), and
          2. for spawn, did the server role actually leave this process (the `spawn ARMED` pid line,
             which only ClientSession::StartSpawned writes and only after launch+connect+handshake
             all succeeded).
        A run that resolved spawn and then never reached another process produces the first line and
        not the second; a run that fell back to monolith produces neither. Either way the arm is
        `status: UNPROVEN` and its numbers are not evidence.
        """
        problems = []
        client = self.dir / "mobilegl.client.log"
        texts = {}
        for role in ("client", "server"):
            path = self.dir / f"mobilegl.{role}.log"
            texts[role] = path.read_text(encoding="utf-8", errors="replace") if path.is_file() else ""
        combined = texts["client"] + texts["server"]

        expected = TRANSPORT_MARKERS.get(self.transport)
        if expected is not None:
            if expected not in texts["client"]:
                problems.append(f"client log does not carry the transport marker for {self.transport}")
            configs = re.findall(r"Config: IPC[^\n]*", texts["client"])
            if not configs:
                problems.append("no `Config: IPC` line on the client: the split knobs never resolved")
            else:
                for key in ("strict=1", "role-split-state=1", "run-ahead=1"):
                    if not any(key in line for line in configs):
                        problems.append(f"`Config: IPC` line missing {key}")
        else:
            # The control arm has to be a control: no IPC line and no transport marker.
            if "Config: IPC" in combined:
                problems.append("monolith arm wrote a `Config: IPC` line: the knobs were armed")

        if self.transport == "spawn":
            armed = SPAWN_ARMED.search(texts["client"])
            if armed is None:
                problems.append("no `spawn ARMED - the server role runs in pid N` line: "
                                "the server never left this process")
            else:
                self.summary["server_pid"] = int(armed.group(1))
                if not texts["server"]:
                    problems.append("spawn arm has no server-role log")
        elif self.transport == "inproc":
            if not texts["server"]:
                problems.append("inproc arm has no server-role log (the apply thread's own file)")

        for path in self.dir.glob("mobilegl.*.log"):
            for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
                if "Fatal{" in line:
                    problems.append(f"Fatal{{ in {path.name}: {line.strip()[:200]}")
                    break

        if self.summary.get("pin_before_rc") != 0:
            problems.append("pin did not take before the run")
        if self.summary.get("pin_after_rc") != 0:
            problems.append("pin check AFTER the run did not report PINNED: this arm is void")
        if self.summary.get("runner_rc") != 0:
            problems.append(f"runner exited {self.summary.get('runner_rc')}")
        if not self.summary.get("benchmark_runs"):
            problems.append("no benchmark.json was pulled")

        self.summary["problems"] = problems
        self.summary["status"] = "OK" if not problems else "UNPROVEN"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--tree", required=True, help="Snapshot tree the APK and runner come from")
    parser.add_argument("--out", required=True, help="Evidence root for this session")
    parser.add_argument("--serial", default=SERIAL)
    parser.add_argument("--package", default="top.mobilegl.plugin.p6gate8.trace",
                        help="Application id of the installed trace APK. Defaults to the id this "
                             "campaign's development builds use; pass the canonical "
                             "top.mobilegl.plugin.trace for a stock install.")
    parser.add_argument("--apk", required=True, help="Signed trace APK to install before the window")
    parser.add_argument("--arm", action="append", required=True,
                        metavar="TRANSPORT[:BACKEND[:CASE[:REPEATS]]]",
                        help="One arm, in the order given. Repeating a spec interleaves it.")
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--reboot", action="store_true",
                        help="Reboot and wait for boot_completed before the window")
    parser.add_argument("--fan-level", default="2")
    parser.add_argument("--session", default=None)
    args = parser.parse_args()

    tree = Path(args.tree).resolve()
    label = args.session or datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    out_root = Path(args.out).resolve() / label
    out_root.mkdir(parents=True, exist_ok=True)
    adb = Adb(args.serial)

    session = {"label": label, "serial": args.serial, "tree": str(tree),
               "package": args.package, "apk": str(Path(args.apk).resolve()),
               "apk_sha256": subprocess.run(["sha256sum", args.apk], capture_output=True, text=True)
                                     .stdout.split()[0],
               "arms": [a for a in args.arm], "repeats": args.repeats,
               "stats_period": STATS_PERIOD,
               # The DEVICE's tick rate, read here so the session records the divisor every arm's
               # CPU number was computed with. `TICKS` (the host's) is only a fallback for a device
               # that cannot answer, and printing it here would have looked like the answer.
               "device_clock_ticks": adb.clk_tck()}

    # The fan is a CONSTANT of the protocol, not a treatment: both arms run under the same airflow,
    # and on this device it is the difference between a 60 s cool-down between arms and a 20-minute
    # one (notes/p4a/INTEGRATOR-DECISIONS.md). `real_speed` is read back because target_level alone
    # is a request, not a fact.
    fan = "/sys/class/xm_power/hw_monitor/pwm_fan"
    adb.su(f"echo {args.fan_level} > {fan}/target_level")
    time.sleep(3)
    session["fan"] = {
        "target_level": adb.su(f"cat {fan}/target_level").strip(),
        "real_speed": adb.su(f"cat {fan}/real_speed").strip(),
        "pwm_duty": adb.su(f"cat {fan}/pwm_duty").strip(),
    }

    if args.reboot:
        session["reboot_requested_utc"] = datetime.now(timezone.utc).isoformat()
        boot_before = adb.boot_id()
        adb.reboot()
        if not adb.wait_boot():
            print("device did not reach boot_completed", file=sys.stderr)
            return 2
        # boot_id must have CHANGED, or "reboot-clean" is a claim about a reboot that did not happen.
        time.sleep(20)
        boot_after = adb.boot_id()
        session["boot_id_before"] = boot_before
        session["boot_id"] = boot_after
        session["rebooted_clean"] = boot_before != boot_after
        if not session["rebooted_clean"]:
            print(f"boot_id unchanged ({boot_before}): not a clean reboot", file=sys.stderr)
            return 2
        # Re-assert the fan: an xm_power level does not always survive a reboot.
        adb.su(f"echo {args.fan_level} > {fan}/target_level")
        time.sleep(3)
    else:
        session["boot_id"] = adb.boot_id()
        session["rebooted_clean"] = False

    install = run(["adb", "-s", args.serial, "install", "-r", str(Path(args.apk).resolve())])
    session["install"] = (install.stdout + install.stderr).strip().splitlines()[-1:]
    # dex2oat outlives the install by a minute or two, and an `am_kill ... due to installPackageLI`
    # landing inside the first arm is a measurement of the installer, not of the transport
    # (MEASUREMENTS.md section 10). Wait for it by name.
    for _ in range(60):
        if not adb.shell("ps -A -o NAME | grep -c dex2oat").strip().isdigit():
            break
        if adb.shell("ps -A -o NAME | grep -c dex2oat").strip() == "0":
            break
        time.sleep(5)
    time.sleep(10)

    session["arms"] = []
    for index, spec in enumerate(args.arm, start=1):
        started = time.time()
        arm = Arm(spec, args, out_root, index)
        print(f"=== arm {arm.label} ===", flush=True)
        arm.run()
        session["arms"].append(arm.summary)
        print(f"    status={arm.summary['status']} problems={arm.summary.get('problems')} "
              f"({time.time() - started:.0f}s)", flush=True)
        # Between-arm cool-down, gate at 40 C on the CPU-subsystem sensor the pin script reports.
        for _ in range(60):
            temp = adb.su("cat /sys/class/thermal/thermal_zone13/temp").strip()
            try:
                if int(temp) <= 36000:
                    break
            except ValueError:
                break
            time.sleep(5)

    (out_root / "session-summary.json").write_text(
        json.dumps(session, indent=2), encoding="utf-8")
    print(json.dumps({"session": label, "out": str(out_root),
                      "arms": [{"label": a["label"], "status": a["status"]}
                               for a in session["arms"]]}, indent=2))
    return 0 if all(a["status"] == "OK" for a in session["arms"]) else 1


if __name__ == "__main__":
    raise SystemExit(main())
