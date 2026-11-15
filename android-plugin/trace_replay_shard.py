#!/usr/bin/env python3
# MobileGL - android-plugin/trace_replay_shard.py
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

"""Replay an ordered list of trace cases on one emulator boot.

One shard is one (lane, transport, backend). Each case runs through trace-replay-ci.sh with
the per-case and per-transport settings of the APK workflow, in a fresh app process with
cleared app data. A failing case does not stop the shard: the case after it starts on a cold
boot, every case gets a verdict file, and the exit status is non-zero when a case failed or a
verdict is missing.

Layout under --result-root (the directory the workflow uploads):

  <case>-<backend>/           what trace-replay-ci.sh writes, plus lane.json, the golden
                              copies the summary renders, and case-status.json
  <case>-<backend>/first-attempt/   files of an attempt that was retried
  shard-status.json           every case of the shard with its verdict
  diagnostics/                emulator logs kept across restarts

The job log repeats what those files say, since the workflow deletes them after a green run:
what each case is run with, the transport lines of its library log, its phase timings and its
record, and shard-status.json at the end.
"""

import argparse
import json
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

INFRASTRUCTURE_FAILURE_EXIT_CODE = 75
TIMEOUT_EXIT_CODE = 124
BACKENDS = ("DirectGLES", "DirectVulkan")
TRANSPORTS = ("monolith", "inproc", "spawn")
PASSED, FAILED, NOT_RUN = "passed", "failed", "not-run"

# Cases with settings of their own. Bliss needs the newer packaged ANGLE build and, on
# DirectGLES, sampler mipmap min-filter downgrading on ANGLE llvmpipe; iterationRP needs its
# SPIR-V repairs forced on.
BLISS_CASE = "minecraft-1.21.4-fabric-iris-bliss-in-world"
ITERATIONRP_CASE = "minecraft-1.21.4-fabric-iris-iterationrp-in-world"
DEFAULT_ANGLE_VARIANT = "ec889e6ea831"
BLISS_ANGLE_VARIANT = "90a62123d794"

# The inproc and spawn lanes pass the same client settings, so they differ only in which
# process runs the server role.
SPLIT_CLIENT_SETTINGS = "MOBILEGL_IPC_ROLE_SPLIT_STATE=1;MOBILEGL_IPC_STRICT_ERRORS=1;MOBILEGL_IPC_RUN_AHEAD=1"

REQUIRED_CASE_KEYS = ("name", "trace_archive", "trace_file", "golden", "target_call", "width", "height",
                      "crop_x", "crop_y", "crop_width", "crop_height", "timeout_seconds")
# Seconds trace-replay-ci.sh gets beyond the case's own budget for install, push and log pulls.
REPLAY_GRACE_SECONDS = 300
BOOT_TIMEOUT_SECONDS = 300
# Seconds a case may take around its replays: the app reset, the failure diagnostics and the
# cold boot that follows a case that did not pass.
CASE_OVERHEAD_SECONDS = 180

# What marks the lines of a library log that say which transport ran: the configuration load
# every run logs, and what the --require-* checks of trace-replay-ci.sh read. The job log
# quotes at most LIBRARY_LOG_LINE_LIMIT of them per case, cut to LIBRARY_LOG_LINE_LENGTH.
LIBRARY_LOG_EVIDENCE = ("Config: Active backend type set to", "Config loaded", "MOBILEGL_TRANSPORT",
                        "Config: IPC", "spawn ARMED", "Fatal{")
LIBRARY_LOG_LINE_LIMIT = 12
LIBRARY_LOG_LINE_LENGTH = 1000

# What trace-replay-ci.sh reads from its environment and the runner does not set. A replay
# inherits the runner's environment, so the job log names the value each one has there.
INHERITED_REPLAY_SETTINGS = ("ADB", "PYTHON", "MOBILEGL_RETRACE_USE_PBUFFER", "MOBILEGL_TRACE_ENV")


def say(message):
    print(message, flush=True)


def pause(seconds):
    time.sleep(seconds)


def safe_case(name):
    return re.sub(r"[^A-Za-z0-9._-]", "_", name)


def as_text(value):
    if value is True:
        return "true"
    if value is False:
        return "false"
    return "" if value is None else str(value)


def is_true(value):
    return value is True or value == "true"


def load_cases(text, backend):
    """The case objects of the APK matrix, as text ready for the command line."""
    try:
        # Numbers stay the literal text of the matrix, so 0.985 is passed on as 0.985.
        cases = json.loads(text, parse_float=str, parse_int=str)
    except json.JSONDecodeError as error:
        raise ValueError(f"the case list is not JSON: {error}") from error
    if not isinstance(cases, list) or not cases:
        raise ValueError("the case list must be a non-empty JSON array of case objects")
    names = set()
    for index, case in enumerate(cases):
        if not isinstance(case, dict):
            raise ValueError(f"case {index} is not an object; pass the case objects of the APK matrix")
        missing = [key for key in REQUIRED_CASE_KEYS if case.get(key) in (None, "")]
        if missing:
            raise ValueError(f"case {case.get('name', index)} lacks {', '.join(missing)}")
        name = case["name"]
        if name in names:
            raise ValueError(f"case {name} is listed twice")
        names.add(name)
        backends = case.get("ci_backends")
        if backends is not None and backend not in backends:
            raise ValueError(f"case {name} does not run on {backend} (ci_backends: {backends})")
    return cases


def needed_seconds(cases):
    """Wall time in which every case of a shard can use its whole time budget.

    It covers each case running until its replay script is stopped, one infrastructure retry
    that does so again for the longest budget, and CASE_OVERHEAD_SECONDS per case. A shard
    deadline at or above this never stops a case that is within its own budget, unless more
    than one case needs a retry as long as its first attempt.
    """
    budgets = [int(as_text(case["timeout_seconds"])) + REPLAY_GRACE_SECONDS for case in cases]
    return sum(budgets) + max(budgets) + CASE_OVERHEAD_SECONDS * len(budgets)


def replay_environment(backend, case_name):
    return {
        "MOBILEGL_ESPRYT_USE_ANGLE": "1" if backend == "DirectGLES" else "0",
        "MOBILEGL_TRACE_ANGLE_VARIANT": BLISS_ANGLE_VARIANT if case_name == BLISS_CASE else DEFAULT_ANGLE_VARIANT,
        "MOBILEGL_MAGMA_R11G11B10F_FALLBACK": "1" if backend == "DirectVulkan" else "0",
        "MOBILEGL_ITRP_FIX": "ForceOn" if case_name == ITERATIONRP_CASE else "Auto",
    }


def transport_arguments(transport, hang_budget_ms):
    hang_budget = f"MOBILEGL_GPU_HANG_BUDGET_MS={hang_budget_ms}"
    if transport == "inproc":
        return ["--env", f"MOBILEGL_TRANSPORT=inproc;{SPLIT_CLIENT_SETTINGS};{hang_budget}", "--require-inproc"]
    if transport == "spawn":
        # No MOBILEGL_IPC_SERVER_PATH: only the app can spell nativeLibraryDir, and
        # TraceReplayActivity resolves it when the environment asks for spawn. The pbuffer is
        # required: a spawned server cannot be handed the client's window.
        return ["--env", f"MOBILEGL_TRANSPORT=spawn;{SPLIT_CLIENT_SETTINGS};{hang_budget}",
                "--use-pbuffer", "--require-spawn"]
    return ["--env", f"MOBILEGL_TRANSPORT=monolith;{hang_budget}", "--require-monolith"]


def replay_arguments(options, case, hang_budget_ms):
    name = case["name"]
    arguments = [
        "--apk-file", options.apk_file,
        "--package", options.package,
        "--backend", options.backend,
        "--result-root", options.result_root,
        "--fixture-root", options.fixture_root,
        "--case", name,
        "--trace-archive", as_text(case["trace_archive"]),
        "--trace-file", as_text(case["trace_file"]),
        "--golden", as_text(case["golden"]),
        "--alternate-golden", as_text(case.get("alternate_golden") or ""),
        "--target-call", as_text(case["target_call"]),
        "--width", as_text(case["width"]),
        "--height", as_text(case["height"]),
        "--ssim-threshold", as_text(case.get("ssim_threshold") or "0.99"),
        "--crop-x", as_text(case["crop_x"]),
        "--crop-y", as_text(case["crop_y"]),
        "--crop-width", as_text(case["crop_width"]),
        "--crop-height", as_text(case["crop_height"]),
        "--timeout-seconds", as_text(case["timeout_seconds"]),
    ]
    arguments += transport_arguments(options.transport, hang_budget_ms)
    if options.backend == "DirectGLES" and name == BLISS_CASE:
        arguments.append("--avoid-angle-llvmpipe-sampler-mipmap-min-filter")
    if options.backend == "DirectGLES" and is_true(case.get("avoid_angle_llvmpipe_explicit_lod_bias")):
        arguments.append("--avoid-angle-llvmpipe-explicit-lod-bias")
    if is_true(case.get("coherent_as_flush")):
        arguments.append("--coherent-as-flush")
    return arguments


def read_json(path):
    try:
        return json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None


def write_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def one_line(text):
    return " ".join(str(text).split())


class Shard:
    def __init__(self, options, cases, environ):
        self.options = options
        self.cases = cases
        self.environ = dict(environ)
        self.adb_command = shlex.split(environ.get("ADB", "adb"))
        directory = Path(__file__).resolve().parent
        self.replay_command = shlex.split(environ.get("MOBILEGL_SHARD_REPLAY_COMMAND") or
                                          f"sh {shlex.quote(str(directory / 'trace-replay-ci.sh'))}")
        self.avd_command = shlex.split(environ.get("MOBILEGL_SHARD_AVD_COMMAND") or
                                       f"sh {shlex.quote(str(directory / 'run-avd-ci.sh'))}")
        self.hang_budget_ms = environ.get("MOBILEGL_GPU_HANG_BUDGET_MS")
        self.restart_each_case = environ.get("MOBILEGL_SHARD_RESTART_EMULATOR_EACH_CASE", "0") == "1"
        self.poll_interval = environ.get("MOBILEGL_SHARD_POLL_INTERVAL", "0.5")
        self.avd_name = environ.get("AVD_NAME", "")
        self.emulator_log = environ.get("EMULATOR_LOG", "")
        self.emulator_pid_file = environ.get("EMULATOR_PID_FILE", "")
        self.result_root = Path(options.result_root)
        self.diagnostics = self.result_root / "diagnostics"
        self.started = time.monotonic()
        self.boots = 1
        self.install_seconds = []
        self.records = {}
        self.aborted = ""
        # The case before the current one when it did not pass; the next case gets a cold boot.
        self.unclean_case = ""

    # ---- bookkeeping -------------------------------------------------------------------

    def result_dir(self, case):
        return self.result_root / f"{safe_case(case['name'])}-{self.options.backend}"

    def record_for(self, case, status, reason, **extra):
        record = {"case": case["name"], "backend": self.options.backend, "transport": self.options.transport,
                  "lane": self.options.lane, "status": status, "reason": reason}
        record.update(extra)
        return record

    def write_summary_inputs(self, case):
        """lane.json and the golden copies the retrace summary reads next to each result."""
        directory = self.result_dir(case)
        directory.mkdir(parents=True, exist_ok=True)
        write_json(directory / "lane.json", {"lane": self.options.lane, "transport": self.options.transport})
        stem = f"{safe_case(case['name'])}-{self.options.backend}"
        for key, suffix in (("golden", "golden"), ("alternate_golden", "alternate-golden")):
            source = as_text(case.get(key) or "")
            if source and os.path.isfile(source) and os.path.getsize(source) > 0:
                shutil.copyfile(source, directory / f"{stem}-{suffix}.png")

    def save_record(self, case, record):
        self.records[case["name"]] = record
        self.write_summary_inputs(case)
        write_json(self.result_dir(case) / "case-status.json", record)
        self.write_shard_status()

    def plan(self, case):
        """What a replay of the case is run with. --dry-run prints it; a run prints it and uses it."""
        return {"case": case["name"],
                "environment": replay_environment(self.options.backend, case["name"]),
                "arguments": replay_arguments(self.options, case, self.hang_budget_ms),
                "timeout": int(as_text(case["timeout_seconds"])) + REPLAY_GRACE_SECONDS}

    def shared_environment(self):
        """What every replay of the shard gets on top of its case's environment."""
        # The shard installs the APK once per boot.
        return {"MOBILEGL_TRACE_SKIP_INSTALL": "1", "MOBILEGL_TRACE_POLL_INTERVAL": self.poll_interval}

    def say_shard_status(self):
        status = read_json(self.result_root / "shard-status.json")
        say(f"trace-replay-shard: shard-status: {json.dumps(status, sort_keys=True)}")

    def write_shard_status(self, complete=False):
        self.result_root.mkdir(parents=True, exist_ok=True)
        records = [self.records.get(case["name"]) or
                   self.record_for(case, NOT_RUN, "the shard has not reached this case") for case in self.cases]
        write_json(self.result_root / "shard-status.json", {
            "schema": 1, "lane": self.options.lane, "transport": self.options.transport,
            "backend": self.options.backend, "complete": complete,
            # The Actions job that ran the shard, so the artifact cleanup can tell whose job failed.
            "job_id": self.environ.get("MOBILEGL_SHARD_JOB_ID", ""),
            "expected_cases": [case["name"] for case in self.cases], "cases": records,
            "emulator_boots": self.boots, "install_seconds": self.install_seconds})

    def remaining_seconds(self):
        if self.options.deadline_seconds <= 0:
            return None
        return self.options.deadline_seconds - (time.monotonic() - self.started)

    def deadline_passed(self):
        remaining = self.remaining_seconds()
        return remaining is not None and remaining <= 0

    # ---- processes ---------------------------------------------------------------------

    def run(self, command, timeout=None, capture=False, extra_env=None):
        """Returns (exit status, output). A timeout kills the whole process group and reads as 124."""
        environment = dict(self.environ)
        environment.update(extra_env or {})
        own_group = os.name == "posix"
        try:
            process = subprocess.Popen(command, env=environment, start_new_session=own_group,
                                       stdout=subprocess.PIPE if capture else None,
                                       stderr=subprocess.STDOUT if capture else None)
        except OSError as error:
            return 127, str(error)
        def kill():
            if own_group:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except OSError:
                    pass
            process.kill()

        try:
            output, _ = process.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            kill()
            output, _ = process.communicate()
            return TIMEOUT_EXIT_CODE, (output or b"").decode("utf-8", "replace")
        except BaseException:
            # The job was canceled or timed out. The child is in a session of its own, so it
            # would otherwise keep driving the device behind the steps that still run.
            kill()
            raise
        return process.returncode, (output or b"").decode("utf-8", "replace")

    def adb(self, *arguments, timeout=60):
        status, output = self.run(self.adb_command + list(arguments), timeout=timeout, capture=True)
        return status, output.replace("\r", "")

    # ---- emulator ----------------------------------------------------------------------

    def can_restart(self):
        return bool(self.avd_name and self.options.gpu and self.emulator_log and self.emulator_pid_file)

    def emulator_pid(self):
        try:
            return int(Path(self.emulator_pid_file).read_text().strip())
        except (OSError, ValueError):
            return None

    def emulator_process_alive(self):
        pid = self.emulator_pid()
        if pid is None:
            return True  # nothing to check against, for example a real device
        try:
            os.kill(pid, 0)
        except OSError:
            return False
        return True

    def health_problem(self):
        """Empty when the device answers; otherwise what did not."""
        if not self.emulator_process_alive():
            return "the emulator process is gone"
        status, output = self.adb("get-state", timeout=15)
        if status != 0 or output.strip() != "device":
            return f"adb state is {output.strip() or 'unknown'}"
        status, output = self.adb("shell", "getprop", "sys.boot_completed", timeout=15)
        if status != 0 or output.strip() != "1":
            return "sys.boot_completed is not 1"
        # A framework call: boot_completed stays 1 across a system_server restart.
        status, output = self.adb("shell", "pm", "path", self.options.package, timeout=30)
        if status != 0 or "package:" not in output:
            return "the package manager does not list the trace APK"
        return ""

    def keep_emulator_log(self, label):
        if self.emulator_log and os.path.isfile(self.emulator_log):
            self.diagnostics.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(self.emulator_log, self.diagnostics / f"emulator-boot-{self.boots}-{label}.log")

    def prepare_booted_device(self):
        # A shard outlives the screen timeout a single replay never reached.
        self.adb("shell", "svc", "power", "stayon", "true", timeout=30)
        return self.install()

    def install(self):
        started = time.monotonic()
        status, output = self.adb("install", "-r", self.options.apk_file, timeout=300)
        self.install_seconds.append(round(time.monotonic() - started, 1))
        say(output.strip())
        return status == 0

    def restart_emulator(self, reason):
        """Stop, cold boot and reinstall. False when the emulator did not come back."""
        if not self.can_restart():
            say(f"trace-replay-shard: cannot restart the emulator ({reason}): no AVD is configured")
            return False
        say(f"trace-replay-shard: restarting the emulator: {reason}")
        common = ["--avd-name", self.avd_name, "--emulator-log", self.emulator_log,
                  "--pid-file", self.emulator_pid_file]
        for attempt in (1, 2):
            # The start truncates the log, and the boot that failed is the one worth reading.
            self.keep_emulator_log(f"attempt-{attempt}" if attempt > 1 else "ended")
            old_pid = self.emulator_pid()
            self.run(self.avd_command + ["stop"] + common, timeout=120)
            self.wait_for_exit(old_pid)
            self.adb("kill-server", timeout=30)
            pause(2)
            self.boots += 1
            status, _ = self.run(self.avd_command + ["start"] + common +
                                 ["--gpu", self.options.gpu, "--boot-timeout", str(BOOT_TIMEOUT_SECONDS)],
                                 timeout=2 * BOOT_TIMEOUT_SECONDS + 60)
            if status == 0 and self.prepare_booted_device():
                return True
            say(f"trace-replay-shard: emulator restart attempt {attempt} failed")
        return False

    def wait_for_exit(self, pid):
        if pid is None or os.name != "posix":
            return
        for _ in range(60):
            try:
                os.kill(pid, 0)
            except OSError:
                return
            pause(0.5)
        try:
            os.kill(pid, signal.SIGKILL)
        except OSError:
            pass

    # ---- app state ---------------------------------------------------------------------

    def leftover_processes(self):
        """Processes still running under the app's uid, as 'pid name' strings."""
        status, user = self.adb("shell", "run-as", self.options.package, "id", "-un", timeout=30)
        user = user.strip()
        if status != 0 or not re.fullmatch(r"[A-Za-z0-9_]+", user):
            return None
        status, listing = self.adb("shell", "ps", "-A", "-o", "USER,PID,NAME", timeout=30)
        if status != 0:
            return None
        found = []
        for line in listing.splitlines():
            fields = line.split()
            if len(fields) >= 3 and fields[0] == user and fields[1].isdigit():
                found.append(f"{fields[1]} {fields[2]}")
        return found

    def settled_leftover_processes(self, polls):
        """leftover_processes once the app's uid is empty, or as it still is after `polls` looks.

        A killed process stays listed for a moment, so one look right after the kill would
        report every force-stop as incomplete.
        """
        found = self.leftover_processes()
        for _ in range(polls - 1):
            if not found:
                break
            pause(0.5)
            found = self.leftover_processes()
        return found

    def reset_app_state(self):
        """Fresh process and fresh app data, as a new install gives. Returns notes for the record."""
        notes = []
        package = self.options.package
        self.adb("shell", "am", "force-stop", package, timeout=60)
        leftovers = self.settled_leftover_processes(polls=4)
        if leftovers is None:
            notes.append("the app's processes could not be listed after force-stop")
        if leftovers:
            # A force-stop is expected to take a spawned server with it; say so when it did not.
            notes.append("processes left after force-stop: " + ", ".join(leftovers))
            self.adb("shell", "run-as", package, "kill", "-9", *[entry.split()[0] for entry in leftovers],
                     timeout=30)
            if self.settled_leftover_processes(polls=6):
                notes.append("the leftover processes could not be killed")
                if self.restart_emulator("app processes survived force-stop and kill"):
                    notes.append("the emulator was restarted to remove them")
        status, output = self.adb("shell", "pm", "clear", package, timeout=60)
        probe_status, _ = self.adb("shell", "run-as", package, "ls", f"/data/user/0/{package}", timeout=30)
        if status != 0 or "Success" not in output or probe_status != 0:
            # The state a fresh install leaves, reached the other way.
            notes.append("pm clear did not leave a usable app data directory; reinstalled the APK")
            self.adb("uninstall", package, timeout=120)
            self.install()
        return notes

    def remove_case_leftovers(self, case):
        name = safe_case(case["name"])
        base = f"/data/local/tmp/mobilegl-{name}"
        self.adb("shell", "rm", "-f", f"{base}.trace", f"{base}.golden.png", f"{base}.alternate-golden.png",
                 timeout=60)
        self.adb("shell", "am", "force-stop", self.options.package, timeout=60)
        shutil.rmtree(Path(self.options.fixture_root) / name, ignore_errors=True)

    # ---- one case ----------------------------------------------------------------------

    def reason_lines(self):
        try:
            return (self.result_root / "infrastructure-failure-reason.txt").read_text().splitlines()
        except OSError:
            return []

    def run_attempt(self, plan):
        """Returns (exit status, seconds, how the shard deadline cut the attempt short or "")."""
        case_budget = plan["timeout"]
        budget = case_budget
        remaining = self.remaining_seconds()
        if remaining is not None and remaining < case_budget:
            budget = max(remaining, 1.0)
        started = time.monotonic()
        status, _ = self.run(self.replay_command + plan["arguments"], timeout=budget,
                             extra_env={**plan["environment"], **self.shared_environment()})
        elapsed = round(time.monotonic() - started, 1)
        cut = ""
        if status == TIMEOUT_EXIT_CODE and budget < case_budget:
            # The case never had its own budget, so the timeout says nothing about the case.
            cut = f"cut by the shard deadline after {elapsed:.0f} s of a {case_budget} s budget"
        return status, elapsed, cut

    def keep_first_attempt(self, case):
        """Move the retried attempt's files aside so the retry starts from an empty directory.

        result.json is renamed: the summary keys rows on any result.json below a directory
        that names the case, and the retry's is the one that counts.
        """
        directory = self.result_dir(case)
        if not directory.is_dir():
            return
        kept = directory / "first-attempt"
        kept.mkdir(exist_ok=True)
        for entry in directory.iterdir():
            if entry.is_file():
                name = "result.first-attempt.json" if entry.name == "result.json" else entry.name
                entry.replace(kept / name)

    def verdict(self, case, status):
        directory = self.result_dir(case)
        if status == 0:
            result = read_json(directory / "result.json")
            if not isinstance(result, dict) or result.get("passed") is not True:
                return FAILED, "the replay exited 0 without a passed result.json"
            proof = read_json(directory / "transport-proof.json")
            if not (isinstance(proof, dict) and proof.get("passed") is True and
                    proof.get("required_transport") == self.options.transport):
                return FAILED, f"the replay exited 0 without a passed {self.options.transport} transport proof"
            return PASSED, ""
        if status == INFRASTRUCTURE_FAILURE_EXIT_CODE:
            return FAILED, "the emulator infrastructure failed again on the retry"
        if status == TIMEOUT_EXIT_CODE:
            return FAILED, "the replay script exceeded its time budget"
        result = read_json(directory / "result.json")
        if not isinstance(result, dict):
            return FAILED, f"exit {status} without a result.json"
        if result.get("passed") is True:
            # The picture matched, so what failed is a check that runs after it.
            return FAILED, f"exit {status} after a passed result.json: the transport proof or a log check failed"
        return FAILED, f"exit {status}: {one_line(result.get('message', ''))}"

    def collect_failure_diagnostics(self, case):
        directory = self.result_dir(case)
        directory.mkdir(parents=True, exist_ok=True)
        logcat = directory / "logcat.txt"
        if not logcat.is_file() or logcat.stat().st_size == 0:
            # trace-replay-ci.sh pulls this itself unless it was killed first.
            _, output = self.adb("logcat", "-d", "-t", "20000", timeout=30)
            logcat.write_text(output, encoding="utf-8", errors="replace")
            app_output = f"/data/user/0/{self.options.package}/files/trace-replay/output"
            # The two role logs of a split build, and the one log of a build without the split
            # transports; the device has one kind or the other.
            for name in ("retrace.log", "mobilegl.client.log", "mobilegl.server.log", "mobilegl.log"):
                status, output = self.adb("exec-out", "run-as", self.options.package, "cat",
                                          f"{app_output}/{name}", timeout=30)
                if status == 0 and output and not output.startswith("cat: "):
                    (directory / name).write_text(output, encoding="utf-8", errors="replace")
        if self.emulator_log and os.path.isfile(self.emulator_log):
            shutil.copyfile(self.emulator_log, directory / "emulator.log")
        # A vanished emulator looks the same whether the host OOM killer took it or the
        # renderer faulted. These two say which.
        _, memory = self.run(["free", "-h"], timeout=30, capture=True)
        (directory / "host-memory.txt").write_text(memory, encoding="utf-8")
        _, dmesg = self.run(["sudo", "-n", "dmesg", "-T"], timeout=30, capture=True)
        (directory / "host-dmesg.txt").write_text("\n".join(dmesg.splitlines()[-300:]) + "\n", encoding="utf-8")

    def say_library_log(self, case):
        """Quote the lines of the case's pulled library log that say which transport ran."""
        directory = self.result_dir(case)
        label = f"trace-replay-shard: library-log {case['name']}:"
        # mobilegl.log is what the proofs of trace-replay-ci.sh read: the client role's log and
        # then the server role's, or the one log of a build without the split transports. A
        # split build has only its role logs when the runner pulled them itself.
        sources = [directory / "mobilegl.log"]
        if not sources[0].is_file():
            sources = [directory / "mobilegl.client.log", directory / "mobilegl.server.log"]
        read = shown = matched = 0
        for source in sources:
            try:
                lines = source.read_text(encoding="utf-8", errors="replace").splitlines()
            except OSError:
                continue
            read += 1
            for line in lines:
                if any(mark in line for mark in LIBRARY_LOG_EVIDENCE):
                    matched += 1
                    if shown < LIBRARY_LOG_LINE_LIMIT:
                        shown += 1
                        # Escaped, so that no character of a device log can fail the job log's encoding.
                        quoted = line[:LIBRARY_LOG_LINE_LENGTH].encode("ascii", "backslashreplace").decode("ascii")
                        say(f"{label} {source.name}: {quoted}")
        if not read:
            say(f"{label} no library log was pulled")
        elif not matched:
            say(f"{label} the library log has none of the lines looked for")
        elif matched > shown:
            say(f"{label} {matched - shown} more such line(s) are not shown")

    def say_phases(self, case):
        """Seconds from each mark of the last attempt's phase-timings.txt to the next, named by the later."""
        try:
            text = (self.result_dir(case) / "phase-timings.txt").read_text(encoding="utf-8", errors="replace")
        except OSError:
            return
        marks = [line.split() for line in text.splitlines()]
        marks = [(mark[0], int(mark[1])) for mark in marks if len(mark) == 2 and mark[1].isdecimal()]
        phases = {later[0]: round((later[1] - earlier[1]) / 1000, 1) for earlier, later in zip(marks, marks[1:])}
        if phases:
            say(f"trace-replay-shard: phases {case['name']}: {json.dumps(phases)}")

    def run_case(self, index, case):
        name = case["name"]
        notes = []
        seconds = {}
        case_started = time.monotonic()

        if self.deadline_passed():
            return self.record_for(case, NOT_RUN, "the shard deadline passed before this case started")
        plan = self.plan(case)
        say(f"trace-replay-shard: run {name}: {json.dumps(plan, sort_keys=True)}")

        restarted_for = ""
        if self.restart_each_case and index > 0:
            restarted_for = "MOBILEGL_SHARD_RESTART_EMULATOR_EACH_CASE=1"
        elif self.unclean_case and self.can_restart():
            # A crashed, hung or killed replay can leave the guest or the host renderer in a
            # state no adb query shows, and this case's verdict must not depend on it.
            restarted_for = f"the case before it, {self.unclean_case}, did not pass"
        else:
            restarted_for = self.health_problem()
        if restarted_for:
            notes.append(f"emulator restarted before the case: {restarted_for}")
            if not self.restart_emulator(restarted_for):
                self.aborted = "the emulator could not be restarted"
                return self.record_for(case, NOT_RUN, self.aborted, notes=notes)

        notes += self.reset_app_state()
        seconds["reset"] = round(time.monotonic() - case_started, 1)

        reasons_before = len(self.reason_lines())
        exit_codes = []
        replay_seconds = []
        status, elapsed, cut = self.run_attempt(plan)
        exit_codes.append(status)
        replay_seconds.append(elapsed)
        surface_lost = 0
        if status == INFRASTRUCTURE_FAILURE_EXIT_CODE:
            say(f"::warning::Android emulator infrastructure failed; restarting it and retrying {name} once.")
            new_reasons = self.reason_lines()[reasons_before:]
            # A lost surface is retried, so it is counted where a rate spike shows.
            surface_lost = sum(1 for line in new_reasons if "angle-surface-lost" in line)
            if surface_lost:
                self.append_step_summary(f"surface-lost retries: {surface_lost} ({self.options.backend}, {name})\n")
            notes.append("infrastructure retry: " + (", ".join(new_reasons) or "no reason recorded"))
            self.keep_first_attempt(case)
            if not self.restart_emulator(f"infrastructure failure in {name}"):
                self.aborted = "the emulator could not be restarted"
            elif self.deadline_passed():
                cut = "the shard deadline passed before the infrastructure retry could start"
            else:
                notes += self.reset_app_state()
                status, elapsed, cut = self.run_attempt(plan)
                exit_codes.append(status)
                replay_seconds.append(elapsed)
        seconds["replay"] = replay_seconds

        finish_started = time.monotonic()
        if self.aborted:
            verdict, reason = FAILED, "the emulator did not come back for the infrastructure retry"
        elif cut:
            verdict, reason = NOT_RUN, cut
        else:
            verdict, reason = self.verdict(case, status)
        if verdict != PASSED:
            self.collect_failure_diagnostics(case)
        self.say_library_log(case)
        self.say_phases(case)
        if not self.aborted:
            self.remove_case_leftovers(case)
        seconds["finish"] = round(time.monotonic() - finish_started, 1)
        seconds["total"] = round(time.monotonic() - case_started, 1)
        return self.record_for(case, verdict, reason, attempts=len(exit_codes), exit_codes=exit_codes,
                               surface_lost_retries=surface_lost, seconds=seconds, notes=notes)

    # ---- the shard ---------------------------------------------------------------------

    def append_step_summary(self, text):
        path = self.environ.get("GITHUB_STEP_SUMMARY")
        if path:
            with open(path, "a", encoding="utf-8") as summary:
                summary.write(text)

    def run_all(self):
        self.write_shard_status()
        say(f"trace-replay-shard: {len(self.cases)} case(s) on {self.options.backend} / "
            f"{self.options.transport} ({self.options.lane})")
        say("trace-replay-shard: every run: " + json.dumps(
            {"command": self.replay_command, "environment": self.shared_environment(),
             "inherited": {name: self.environ.get(name) for name in INHERITED_REPLAY_SETTINGS}}, sort_keys=True))
        if not self.prepare_booted_device():
            if not self.restart_emulator("the trace APK did not install"):
                self.aborted = "the trace APK could not be installed"
        for index, case in enumerate(self.cases):
            say(f"::group::{case['name']} ({index + 1}/{len(self.cases)})")
            if self.aborted:
                record = self.record_for(case, NOT_RUN, self.aborted)
            else:
                try:
                    record = self.run_case(index, case)
                except Exception as error:  # one case's bookkeeping must not cost the others their verdicts
                    record = self.record_for(case, FAILED, f"the shard runner failed: {one_line(error)}")
            self.unclean_case = "" if record["status"] == PASSED else case["name"]
            self.save_record(case, record)
            say(f"trace-replay-shard: record {case['name']}: {json.dumps(record, sort_keys=True)}")
            say(f"trace-replay-shard: emulator boots after {case['name']}: {self.boots}")
            say(f"trace-replay-shard: {case['name']}: {record['status']}"
                + (f" ({record['reason']})" if record["reason"] else ""))
            say("::endgroup::")
        return self.finish()

    def finish(self):
        """Tally from the files on disk, so a verdict the loop never wrote counts as missing."""
        label = f"{self.options.backend} / {self.options.transport}"
        rows = []
        verdicts = 0
        failed = []
        for case in self.cases:
            record = read_json(self.result_dir(case) / "case-status.json")
            if not isinstance(record, dict) or record.get("case") != case["name"]:
                record = self.record_for(case, NOT_RUN, "no verdict file was written")
            if record["status"] in (PASSED, FAILED):
                verdicts += 1
            if record["status"] == PASSED:
                result = read_json(self.result_dir(case) / "result.json")
                if not isinstance(result, dict) or result.get("passed") is not True:
                    record = dict(record, status=FAILED, reason="the verdict is passed but result.json is not")
            if record["status"] != PASSED:
                failed.append(record)
            result = read_json(self.result_dir(case) / "result.json")
            ssim = result.get("ssim", "") if isinstance(result, dict) else ""
            seconds = record.get("seconds", {}).get("total", "")
            note = one_line("; ".join([record.get("reason", "")] + record.get("notes", [])).strip("; "))
            rows.append(f"| {case['name']} | {record['status']} | {record.get('attempts', 0)} | {seconds} | "
                        f"{ssim} | {note.replace('|', '/')} |")
        self.write_shard_status(complete=verdicts == len(self.cases))

        summary = [f"### Android retrace: {label} ({self.options.lane})", "",
                   f"{len(self.cases) - len(failed)} of {len(self.cases)} passed, {self.boots} emulator boot(s).", "",
                   "| case | status | attempts | seconds | SSIM | note |", "| --- | --- | --- | --- | --- | --- |"]
        self.append_step_summary("\n".join(summary + rows) + "\n\n")
        for record in failed:
            say(f"::error title=Android retrace {record['status']} ({label})::"
                f"{record['case']}: {record.get('reason') or record['status']}")
        if verdicts != len(self.cases):
            say(f"::error title=Android retrace shard incomplete ({label})::"
                f"{verdicts} verdict(s) for {len(self.cases)} case(s)")
        self.say_shard_status()
        say(f"trace-replay-shard: {len(self.cases) - len(failed)} passed, {len(failed)} not passed, "
            f"{verdicts} verdict(s) for {len(self.cases)} case(s)")
        return 0 if not failed and verdicts == len(self.cases) else 1

    def finalize(self):
        """Give every case its summary inputs and a verdict file without touching the device.

        Runs after the replay step whatever its outcome, so a shard that was killed or never
        started still uploads one named row per case.
        """
        for case in self.cases:
            record = read_json(self.result_dir(case) / "case-status.json")
            if isinstance(record, dict) and record.get("case") == case["name"]:
                self.records[case["name"]] = record
                self.write_summary_inputs(case)
            else:
                self.save_record(case, self.record_for(case, NOT_RUN, "the shard did not reach this case"))
        previous = read_json(self.result_root / "shard-status.json")
        if isinstance(previous, dict):
            self.boots = previous.get("emulator_boots", self.boots)
            self.install_seconds = previous.get("install_seconds", self.install_seconds)
        self.write_shard_status(
            complete=all(self.records[case["name"]]["status"] in (PASSED, FAILED) for case in self.cases))
        self.say_shard_status()
        return 0

    def dry_run(self):
        for case in self.cases:
            say(json.dumps(self.plan(case), sort_keys=True))
        return 0


def parse_arguments(argv):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--apk-file", required=True, help="Trace APK to install once per emulator boot.")
    parser.add_argument("--package", default="top.mobilegl.plugin.trace")
    parser.add_argument("--backend", required=True, choices=BACKENDS)
    parser.add_argument("--transport", required=True, choices=TRANSPORTS)
    parser.add_argument("--lane", required=True, help="Lane name written to lane.json, e.g. inproc-acceptance.")
    parser.add_argument("--gpu", default="", help="Emulator -gpu mode, needed to restart the emulator.")
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--cases-json", help="JSON array of the APK matrix's case objects.")
    source.add_argument("--cases-file", help="File holding that JSON array.")
    source.add_argument("--cases-env", help="Name of the environment variable holding that JSON array.")
    parser.add_argument("--result-root", default="android-retrace-result")
    parser.add_argument("--fixture-root", default="android-retrace-fixture")
    parser.add_argument("--deadline-seconds", type=int, default=0,
                        help="Stop starting cases, and cut a running one, this long after the start; both are "
                             "recorded as not-run. 0: no limit. retrace_shard_budget.py says what a shard needs.")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--dry-run", action="store_true",
                      help="Print each case's environment and trace-replay-ci.sh arguments, run nothing.")
    mode.add_argument("--finalize", action="store_true",
                      help="Only write the summary inputs and a not-run verdict for cases without one.")
    return parser.parse_args(argv)


def main(argv=None):
    options = parse_arguments(argv)
    if options.cases_json is not None:
        text = options.cases_json
    elif options.cases_file is not None:
        text = Path(options.cases_file).read_text(encoding="utf-8")
    else:
        text = os.environ.get(options.cases_env, "")
    try:
        cases = load_cases(text, options.backend)
    except ValueError as error:
        say(f"::error title=Android retrace shard misconfigured::{error}")
        return 2
    shard = Shard(options, cases, os.environ)
    if shard.hang_budget_ms is None:
        say("::error title=Android retrace shard misconfigured::MOBILEGL_GPU_HANG_BUDGET_MS is not set")
        return 2
    if options.dry_run:
        return shard.dry_run()
    if options.finalize:
        return shard.finalize()
    if not os.path.isfile(options.apk_file):
        say(f"::error title=Android retrace shard misconfigured::APK does not exist: {options.apk_file}")
        return 2
    # A termination request unwinds through Shard.run, which kills the running child first.
    signal.signal(signal.SIGTERM, lambda number, frame: sys.exit(128 + number))
    return shard.run_all()


if __name__ == "__main__":
    sys.exit(main())
