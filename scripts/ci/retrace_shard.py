#!/usr/bin/env python3
# MobileGL - scripts/ci/retrace_shard.py
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

"""Replay the cases of one retrace shard, one after another, and give each its own verdict.

A shard is some of the cases of one (lane, transport, backend) cell, replayed by one CI job
(tools/trace_replay/trace_cases.py, shard_matrix). Each case runs through its own ctest entry
with the environment and the checks of its lane (case_plan), in a process group and a driver
cache directory of its own, so no case sees what another left behind. A failing case does not
stop the shard. The shard fails when a case failed, or when the verdicts it recorded are not
exactly the cases it was given.

For every case it prints a log group, appends a row to the step summary and records a verdict
in <result-dir>/results/shard-results.json; a failed case also gets an error annotation.
The replay output the lane uploads is moved to <result-dir>/results/<case>/, the JUnit files
are in <result-dir>/junit/, and the extracted trace is deleted.

Usage:
  retrace_shard.py run --profile {retrace,split,verify} --backend B [--transport T] [--lane L]
                       --build-dir DIR --result-dir DIR [--names-env VAR | --names JSON]
                       [--split-reuse] [--server-path PATH] [--shard N/M]
  retrace_shard.py --self-test
"""
import argparse
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parent
ITERATIONRP_CASE = "minecraft-1.21.4-fabric-iris-iterationrp-in-world"
BLENDED_DEPTH_WRITE_CASE = "improved-transparency-minecraft-26.3"
WIRE_EVIDENCE_CASE = "OpenRA"
CONTROL_CASE = "OpenRA"
CONTROL_BACKEND = "DirectGLES"
CONTROL_NAME = "OpenRA (corrupted-snapshot control)"
# The comparator runs the long cases several times slower than a plain replay, and a spawned
# server can take as long, so the verify and split lanes raise ctest's 1500 s default.
LONG_TIMEOUT = ("--timeout", "10800")
SPLIT_REUSE_EXPECT_IPC = "strict=0 role-split-state=1 run-ahead=1"
SPLIT_REUSE_EXPECT_IPC_REASON = (
    "retrace-split dropped its inproc rows because the inproc-acceptance retrace rows replay the "
    "same library with strict=1 role-split-state=1 run-ahead=1, which differs from this leg's "
    "defaults only in strict while these hold. A default changed: restore the inproc rows (the "
    "trace-cases job's split-spawn-shards without --transports spawn) or update the expectation.")
# GitHub shows about ten error annotations of a step. The first failures are annotated as they
# happen, so a shard killed later still shows them; the rest wait for the roll-up that names
# every failed case.
IMMEDIATE_ANNOTATIONS = 8


def test_regex(case, backend):
    return rf"^MobileGLTraceReplay\.{case}\.{backend}$"


def test_name(case, backend):
    return f"MobileGLTraceReplay.{case}.{backend}"


def case_plan(profile, transport, backend, case, *, split_reuse=False, server_path=""):
    """What one case of a lane runs with: the environment it adds, ctest's arguments, its checks.

    retrace  the monolith-control and inproc-acceptance lanes of the retrace job. The job itself
             exports MOBILEGL_TRANSPORT and the three IPC knobs for every case alike.
    split    the retrace-split job, inproc and spawn.
    verify   the retrace-verify job; its library has no transport.
    """
    if profile not in ("retrace", "split", "verify"):
        raise ValueError(f"unknown profile: {profile}")
    env = {}
    if backend == "DirectVulkan":
        env["MOBILEGL_MAGMA_R11G11B10F_FALLBACK"] = "1"
        # The blended depth-write quirk enables itself only on Qualcomm, which no runner has,
        # so the case it exists for forces it on; ForceOn bypasses only the vendor gate.
        if case == BLENDED_DEPTH_WRITE_CASE:
            env["MOBILEGL_MAGMA_DISABLE_BLENDED_DEPTH_WRITE"] = "1"
        # The iterationRP repairs default to device detection and the software driver is on no
        # list, so the one case that needs them forces them. The verify lane does not set it.
        if case == ITERATIONRP_CASE and profile != "verify":
            env["MOBILEGL_ITRP_FIX"] = "ForceOn"
    ctest = ["-V", "--no-tests=error"]
    evidence = None
    census = False
    if profile == "retrace":
        if transport == "inproc":
            # These rows stand in for retrace-split's dropped inproc rows, so they prove the
            # configuration they ran with from the client's own log.
            env["MGTRACE_EXPECT_IPC"] = "strict=1 role-split-state=1 run-ahead=1"
            census = True
        if transport != "monolith":
            evidence = []
    elif profile == "split":
        if transport not in ("inproc", "spawn"):
            raise ValueError(f"the split profile runs inproc or spawn, not {transport!r}")
        if not server_path.startswith("/"):
            # The replay's working directory is not the workspace and the launcher resolves the
            # image relative to nothing. The inproc arm reads it only to decide it needs none.
            raise ValueError("the split profile needs an absolute --server-path")
        env["MOBILEGL_TRANSPORT"] = transport
        env["MOBILEGL_IPC_SERVER_PATH"] = server_path
        evidence = []
        if case == WIRE_EVIDENCE_CASE:
            # This case also counts the wire's draw records and the server's draws. That turns
            # on the library's per-frame counters, so the others keep the plain configuration.
            env["MGTRACE_WIRE_EVIDENCE"] = "1"
            evidence = ["--require-wire"]
        if split_reuse:
            env["MGTRACE_EXPECT_IPC"] = SPLIT_REUSE_EXPECT_IPC
            env["MGTRACE_EXPECT_IPC_REASON"] = SPLIT_REUSE_EXPECT_IPC_REASON
        ctest += LONG_TIMEOUT
        census = True
    else:
        env["MOBILEGL_PIPE_VERIFY"] = "1"
        ctest += LONG_TIMEOUT
    ctest += ["-R", test_regex(case, backend)]
    return {"env": env, "ctest": ctest, "evidence": evidence, "census": census,
            "control": has_control(profile, backend, case)}


def control_plan():
    """The verify lane's negative control: the same replay with one snapshot field corrupted.

    It must fail, and for the comparator's reason. It adds only these two variables and raises
    no core limit, since the failure is the expected outcome.
    """
    return {"env": {"MOBILEGL_PIPE_VERIFY": "1",
                    "MOBILEGL_PIPE_VERIFY_CORRUPT": "GetRenderStateParameters"},
            "ctest": ["-V", "--no-tests=error", *LONG_TIMEOUT,
                      "-R", test_regex(CONTROL_CASE, CONTROL_BACKEND)]}


def has_control(profile, backend, case):
    return profile == "verify" and case == CONTROL_CASE and backend == CONTROL_BACKEND


def expected_results(profile, backend, names):
    """The verdicts a shard owes, in order."""
    out = []
    for name in names:
        out.append(name)
        if has_control(profile, backend, name):
            out.append(CONTROL_NAME)
    return out


def step_ignores_sigpipe():
    """Whether the process that started this script, the step's shell, ignores SIGPIPE."""
    try:
        with open(f"/proc/{os.getppid()}/status", encoding="ascii") as status:
            for line in status:
                if line.startswith("SigIgn:"):
                    return bool(int(line.split()[1], 16) & (1 << (signal.SIGPIPE - 1)))
    except (OSError, ValueError, IndexError):
        pass
    return False


def raise_core_limit():
    # What `ulimit -c unlimited` does; the runner's core handler honors each process's limit.
    import resource
    resource.setrlimit(resource.RLIMIT_CORE, (resource.RLIM_INFINITY, resource.RLIM_INFINITY))


class Shard:
    def __init__(self, args, names, base_env):
        self.args = args
        self.names = names
        self.base_env = dict(base_env)
        self.build_dir = Path(args.build_dir).resolve()
        self.result_dir = Path(args.result_dir).resolve()
        self.results_root = self.result_dir / "results"
        self.junit_dir = self.result_dir / "junit"
        self.log_dir = self.result_dir / "logs"
        self.cache_root = self.result_dir / "driver-cache"
        self.records = []
        self.failures = 0
        self.child = None
        self.lane = args.lane or "-"
        self.transport = args.transport or "-"
        self.where = f"{self.lane} / {self.transport} / {args.backend}"

    # --- output -------------------------------------------------------------------------------

    def say(self, text=""):
        print(text, flush=True)

    def summary(self, text):
        path = os.environ.get("GITHUB_STEP_SUMMARY")
        if path:
            with open(path, "a", encoding="utf-8") as file:
                file.write(text + "\n")

    def annotate(self, title, message):
        escaped = message.replace("%", "%25").replace("\r", "").replace("\n", "%0A")
        # A property value also escapes the two characters that delimit properties.
        title = title.replace("%", "%25").replace(":", "%3A").replace(",", "%2C")
        self.say(f"::error title={title}::{escaped}")

    def write_results(self):
        self.results_root.mkdir(parents=True, exist_ok=True)
        payload = {"profile": self.args.profile, "lane": self.args.lane, "transport": self.args.transport,
                   "backend": self.args.backend, "shard": self.args.shard, "cases": self.names,
                   "results": self.records}
        target = self.results_root / "shard-results.json"
        scratch = target.with_suffix(".tmp")
        scratch.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
        scratch.replace(target)

    def record(self, name, status, seconds, reasons, *, ssim="", cores=()):
        """One verdict: "passed", "failed", or "skipped" for a control whose case already failed."""
        entry = {"case": name, "status": status, "seconds": round(seconds, 1), "ssim": ssim,
                 "reasons": list(reasons), "cores": list(cores), "annotated": False}
        if status == "failed":
            self.failures += 1
            entry["annotated"] = self.failures <= IMMEDIATE_ANNOTATIONS
        self.records.append(entry)
        self.write_results()
        note = "; ".join(reasons)
        if cores:
            note = (note + "; " if note else "") + "core: " + ", ".join(cores)
        shown = "**failed**" if status == "failed" else status
        self.summary(f"| {name} | {shown} | {seconds:.0f} | {ssim} | {note.replace('|', '/')[:300]} |")
        if entry["annotated"]:
            self.annotate(f"retrace failed: {self.where} / {name}", note or "failed")

    # --- one process ----------------------------------------------------------------------------

    def run_ctest(self, plan, *, junit=None, transcript=None, core_limit=True, cache_home=None):
        env = dict(self.base_env)
        env.update(plan["env"])
        if cache_home is not None:
            env["XDG_CACHE_HOME"] = str(cache_home)
        command = [self.args.ctest, *plan["ctest"]]
        if junit is not None:
            command += ["--output-junit", str(junit)]
        self.say("+ " + " ".join(f"{key}={value!r}" for key, value in sorted(plan["env"].items())))
        self.say("+ " + " ".join(command))
        options = {"cwd": self.build_dir, "env": env, "start_new_session": True}
        if core_limit:
            options["preexec_fn"] = raise_core_limit
        # Python ignores SIGPIPE itself and by default hands a child the default action. The
        # replay gets what a step's own child would get instead, so a write to a closed pipe
        # or socket ends the same way as when the step ran ctest directly.
        options["restore_signals"] = not step_ignores_sigpipe()
        sys.stdout.flush()
        if transcript is None:
            self.child = subprocess.Popen(command, **options)
            status = self.child.wait()
        else:
            # The transcript is both shown and kept, as `2>&1 | tee` would.
            self.child = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, **options)
            with open(transcript, "wb") as file:
                for chunk in iter(lambda: self.child.stdout.read1(65536), b""):
                    file.write(chunk)
                    sys.stdout.buffer.write(chunk)
                    sys.stdout.buffer.flush()
            status = self.child.wait()
        self.reap()
        return status

    def reap(self, grace=5.0):
        """End whatever the finished replay left running, so it cannot run under the next case.

        A spawned server exits by itself when its client is gone, so the group gets a moment
        first. True when something had to be killed.
        """
        child, self.child = self.child, None
        if child is None:
            return False
        deadline = time.monotonic() + grace
        try:
            while True:
                os.killpg(child.pid, 0)
                if time.monotonic() >= deadline:
                    break
                time.sleep(0.25)
            os.killpg(child.pid, signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            return False
        self.say(f"killed the processes ctest (pid {child.pid}) left running")
        return True

    # --- one case -------------------------------------------------------------------------------

    def fresh_cache_home(self):
        """An empty driver cache directory, as on a runner that has replayed nothing yet.

        The software drivers keep compiled shaders on disk under the user cache directory. Each
        case gets a new one, so a case never starts from shaders another case compiled. On a
        hosted runner the default location is emptied as well, in case a process ignores the
        redirect; a self-hosted machine's own cache is left alone.
        """
        self.cache_root.mkdir(parents=True, exist_ok=True)
        cache_home = Path(tempfile.mkdtemp(prefix="case-", dir=self.cache_root))
        if self.base_env.get("RUNNER_ENVIRONMENT") != "self-hosted":
            default_home = Path(self.base_env.get("HOME", "/nonexistent")) / ".cache"
            for stale in default_home.glob("mesa_shader_cache*"):
                self.say(f"removing {stale} left by an earlier process")
                shutil.rmtree(stale, ignore_errors=True)
        return cache_home

    def drop_cache_home(self, cache_home):
        entries = sum(1 for path in cache_home.rglob("*") if path.is_file())
        self.say(f"driver cache: {entries} file(s) written under {cache_home}, removed")
        shutil.rmtree(cache_home, ignore_errors=True)

    def junit_problem(self, junit, name):
        """Why this JUnit file does not show exactly the one entry that was asked for, or ''."""
        if not junit.is_file():
            return f"ctest wrote no {junit.name}"
        try:
            entries = [case.get("name") for case in ET.parse(junit).getroot().iter("testcase")]
        except ET.ParseError as error:
            return f"{junit.name} does not parse: {error}"
        if entries != [name]:
            return f"ctest ran {entries or 'no entry'} where exactly {name} was asked for"
        return ""

    def check_evidence(self, plan, case):
        """Run the lane's evidence check; its error lines are folded into the case's annotation."""
        evidence = self.build_dir / case / self.args.backend / "output" / "split-evidence.txt"
        command = [sys.executable, str(SCRIPTS / "retrace_split_evidence.py"), str(evidence),
                   "--transport", self.args.transport, "--case", case, "--backend", self.args.backend,
                   *plan["evidence"]]
        done = subprocess.run(command, cwd=self.build_dir, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, text=True)
        text = done.stdout.strip()
        self.say(re.sub(r"^::error::", "split evidence: ", text, flags=re.M))
        if done.returncode == 0:
            return ""
        return "split evidence: " + re.sub(r"^::error::", "", text, flags=re.M).replace("\n", " ")[:600]

    def census(self, case):
        """The refusal census from the library's own logs, recorded and never a verdict.

        Both roles are read: under spawn the applier's refusals are raised in the server's
        process. ctest's transcript cannot stand in, since the console sink is compiled out.
        """
        output = self.build_dir / case / self.args.backend / "output"
        log = output / "mobilegl.client.log"
        server_log = output / "mobilegl.server.log"
        evidence = output / "split-evidence.txt"
        self.say(evidence.read_text(errors="replace").rstrip() if evidence.is_file() else "no split-evidence.txt")
        if not log.is_file():
            self.say(f"no {log} - the replay wrote no library log")
            return
        files = [log] + ([server_log] if server_log.is_file() else [])
        self.say("census over: " + " ".join(str(path) for path in files))
        lines = [line for path in files for line in path.read_text(errors="replace").splitlines()]
        self.say(f"Fatal{{ lines: {sum(1 for line in lines if 'Fatal{' in line)}")
        counts = {}
        for line in lines:
            for match in re.findall(r'Fatal\{[A-Za-z]*, "[^"]*"', line):
                counts[match] = counts.get(match, 0) + 1
        for match, count in sorted(counts.items(), key=lambda item: (-item[1], item[0]))[:20]:
            self.say(f"{count:7d} {match}")
        transport_line = next((line for line in log.read_text(errors="replace").splitlines()
                               if "MOBILEGL_TRANSPORT=" in line), None)
        self.say(transport_line or f"no transport line in {log}")

    def collect(self, case):
        """Move what the lane uploads into the result directory and delete the extracted trace."""
        source = self.build_dir / case
        target = self.results_root / case
        shutil.rmtree(source / self.args.backend / "input", ignore_errors=True)
        for relative in ("actual-images", f"{self.args.backend}/output"):
            if (source / relative).is_dir():
                (target / relative).parent.mkdir(parents=True, exist_ok=True)
                shutil.rmtree(target / relative, ignore_errors=True)
                shutil.move(str(source / relative), str(target / relative))
        shutil.rmtree(source, ignore_errors=True)

    def ssim_of(self, case):
        try:
            result = json.loads((self.build_dir / case / self.args.backend / "output" / "result.json")
                                .read_text(errors="replace"))
        except (OSError, ValueError):
            return ""
        value = result.get("ssim")
        return "" if value is None else str(value)

    def run_case(self, case):
        plan = case_plan(self.args.profile, self.args.transport, self.args.backend, case,
                         split_reuse=self.args.split_reuse, server_path=self.args.server_path)
        self.say(f"::group::{case} ({self.where})")
        started = time.monotonic()
        cores_before = set(Path("/tmp").glob("core.*"))
        junit = self.junit_dir / f"{case}.xml"
        reasons = []
        cache_home = self.fresh_cache_home()
        try:
            try:
                status = self.run_ctest(plan, junit=junit, cache_home=cache_home)
            except (OSError, subprocess.SubprocessError) as error:
                # The case still gets its verdict, and the cases after it still run.
                self.child = None
                status = None
                reasons.append(f"ctest could not be run: {error}")
            if status is not None and status > 0:
                reasons.append(f"ctest exit {status}")
            elif status is not None and status < 0:
                reasons.append(f"ctest ended by signal {-status}")
            problem = self.junit_problem(junit, test_name(case, self.args.backend))
            if problem:
                reasons.append(problem)
            if plan["evidence"] is not None:
                # The transport assertions ran on this case and found their evidence. A failed
                # replay is already red, and its missing evidence file would only repeat that.
                if status == 0:
                    problem = self.check_evidence(plan, case)
                    if problem:
                        reasons.append(problem)
            if plan["census"]:
                try:
                    self.census(case)
                except OSError as error:
                    self.say(f"refusal census skipped: {error}")
        finally:
            self.drop_cache_home(cache_home)
        ssim = self.ssim_of(case)
        self.collect(case)
        cores = sorted(path.name for path in set(Path("/tmp").glob("core.*")) - cores_before)
        seconds = time.monotonic() - started
        self.say(f"{case}: {'passed' if not reasons else 'FAILED: ' + '; '.join(reasons)} in {seconds:.0f} s")
        self.say("::endgroup::")
        self.record(case, "failed" if reasons else "passed", seconds, reasons, ssim=ssim, cores=cores)
        return not reasons

    def run_control(self, verified_passed):
        """With a snapshot field corrupted the same replay must fail, or the lane proves nothing.

        It replays into the case directory the verified run used. That run's output has already
        been moved to the result directory, and the corrupted run's output is deleted, so only
        the verified run is uploaded under the case's name.
        """
        self.say(f"::group::{CONTROL_NAME}")
        if not verified_passed:
            self.say("not run: the verified replay it is compared with failed")
            self.say("::endgroup::")
            self.record(CONTROL_NAME, "skipped", 0, ["not run: the verified replay failed"])
            return
        started = time.monotonic()
        reasons = []
        transcript = self.log_dir / "verify-control.txt"
        cache_home = self.fresh_cache_home()
        status = None
        try:
            status = self.run_ctest(control_plan(), transcript=transcript, core_limit=False,
                                    cache_home=cache_home)
        except (OSError, subprocess.SubprocessError) as error:
            self.child = None
            reasons.append(f"ctest could not be run: {error}")
        finally:
            self.drop_cache_home(cache_home)
        shutil.rmtree(self.build_dir / CONTROL_CASE, ignore_errors=True)
        if status is None:
            pass
        elif status == 0:
            reasons.append(
                "MOBILEGL_PIPE_VERIFY_CORRUPT=GetRenderStateParameters left the OpenRA retrace "
                "green, so the comparator is not comparing and the verify retrace lane proves nothing")
        else:
            # Its red must carry the comparator's divergence on the corrupted field.
            done = subprocess.run([sys.executable, str(SCRIPTS / "retrace_verify_reason.py"), str(transcript)],
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            text = re.sub(r"^::error::", "", done.stdout.strip(), flags=re.M)
            self.say(text)
            if done.returncode != 0:
                reasons.append(text.replace("\n", " ")[:600])
            else:
                self.say(f"the corrupted field turned the retrace red, as it must (ctest exit {status})")
        seconds = time.monotonic() - started
        self.say("::endgroup::")
        self.record(CONTROL_NAME, "failed" if reasons else "passed", seconds, reasons)

    # --- the shard --------------------------------------------------------------------------------

    def run(self):
        for directory in (self.results_root, self.junit_dir, self.log_dir):
            directory.mkdir(parents=True, exist_ok=True)
        expected = expected_results(self.args.profile, self.args.backend, self.names)
        self.say(f"retrace shard {self.args.shard or ''} ({self.where}): {len(self.names)} case(s), "
                 f"{len(expected)} verdict(s) owed")
        self.summary(f"### retrace shard {self.args.shard or ''} ({self.where})\n\n"
                     "| case | result | seconds | SSIM | note |\n|---|---|---|---|---|")
        self.write_results()
        for case in self.names:
            passed = self.run_case(case)
            if case_plan(self.args.profile, self.args.transport, self.args.backend, case,
                         split_reuse=self.args.split_reuse, server_path=self.args.server_path)["control"]:
                self.run_control(passed)
        return self.finish(expected)

    def finish(self, expected):
        problems = verdict_problems(expected, self.records)
        # Counted again from what ctest wrote, apart from the loop's own bookkeeping.
        ran = sum(1 for case in self.names
                  if not self.junit_problem(self.junit_dir / f"{case}.xml", test_name(case, self.args.backend)))
        passed_cases = sum(1 for entry in self.records
                           if entry["status"] == "passed" and entry["case"] in self.names)
        if ran < passed_cases:
            problems.append(f"{passed_cases} case(s) recorded as passed but only {ran} JUnit file(s) "
                            f"show their entry ran")
        failed = [entry for entry in self.records if entry["status"] == "failed"]
        if failed:
            names = ", ".join(entry["case"] for entry in failed)
            self.annotate(f"retrace shard {self.args.shard or ''}: {len(failed)} of {len(expected)} failed",
                          f"{self.where}: {names}")
            for entry in failed:
                if not entry["annotated"]:
                    self.annotate(f"retrace failed: {self.where} / {entry['case']}",
                                  "; ".join(entry["reasons"]) or "failed")
        for problem in problems:
            self.annotate(f"retrace shard {self.args.shard or ''}: verdicts do not match the cases", problem)
        passed = sum(1 for entry in self.records if entry["status"] == "passed")
        closing = f"{passed} passed, {len(failed)} failed, {len(expected)} owed"
        if problems:
            closing += "; " + "; ".join(problems)
        self.summary(f"\n{closing}\n")
        self.say(f"retrace shard ({self.where}): {closing}")
        return 1 if failed or problems else 0


def verdict_problems(expected, records):
    """Why the recorded verdicts are not exactly the ones owed; empty when they are."""
    recorded = [entry["case"] for entry in records]
    problems = []
    if len(recorded) != len(expected):
        problems.append(f"{len(recorded)} verdict(s) recorded for {len(expected)} owed")
    if recorded != expected:
        missing = [name for name in expected if name not in recorded]
        extra = [name for name in recorded if name not in expected]
        if missing:
            problems.append("no verdict for " + ", ".join(missing))
        if extra:
            problems.append("verdict for a case that was not assigned: " + ", ".join(extra))
        if not missing and not extra and len(recorded) == len(expected):
            problems.append("verdicts are out of order or repeated")
    return problems


def run(args):
    if args.names is not None:
        names = json.loads(args.names)
    else:
        names = json.loads(os.environ.get(args.names_env) or "null")
    if not isinstance(names, list) or not names or not all(isinstance(name, str) and name for name in names):
        print(f"::error::retrace shard: no case list (looked in --names and ${args.names_env})")
        return 2
    if len(set(names)) != len(names):
        print(f"::error::retrace shard: the case list repeats a case: {names}")
        return 2
    if args.profile == "retrace":
        # The job exports the transport for this lane; the plan and the library must agree on it.
        exported = os.environ.get("MOBILEGL_TRANSPORT", "")
        if exported != args.transport:
            print(f"::error::retrace shard: --transport is {args.transport!r} but the job exports "
                  f"MOBILEGL_TRANSPORT={exported!r}")
            return 2
    try:
        case_plan(args.profile, args.transport, args.backend, names[0],
                  split_reuse=args.split_reuse, server_path=args.server_path)
    except ValueError as error:
        print(f"::error::retrace shard: {error}")
        return 2
    shard = Shard(args, names, os.environ)

    def stop(signum, _frame):
        shard.reap(grace=0)
        shard.say(f"retrace shard: stopped by signal {signum} after {len(shard.records)} verdict(s)")
        sys.exit(128 + signum)

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    return shard.run()


def self_test():
    """The plan of every lane against the values the lanes are defined by, and the verdict check."""
    oit, itrp, other = BLENDED_DEPTH_WRITE_CASE, ITERATIONRP_CASE, "minecraft-1.21.4-in-world"
    server = "/w/build-linux/libMobileGLServer.so"
    regex = lambda case, backend: ["-R", rf"^MobileGLTraceReplay\.{case}\.{backend}$"]
    plain, long = ["-V", "--no-tests=error"], ["-V", "--no-tests=error", "--timeout", "10800"]
    vk = {"MOBILEGL_MAGMA_R11G11B10F_FALLBACK": "1"}
    expect_strict = {"MGTRACE_EXPECT_IPC": "strict=1 role-split-state=1 run-ahead=1"}
    reuse = {"MGTRACE_EXPECT_IPC": SPLIT_REUSE_EXPECT_IPC, "MGTRACE_EXPECT_IPC_REASON": SPLIT_REUSE_EXPECT_IPC_REASON}
    split = lambda transport: {"MOBILEGL_TRANSPORT": transport, "MOBILEGL_IPC_SERVER_PATH": server}
    table = [
        # profile, transport, backend, case, reuse -> env, ctest, evidence, census, control
        ("retrace", "monolith", "DirectGLES", other, False, {}, plain, None, False, False),
        ("retrace", "monolith", "DirectVulkan", other, False, vk, plain, None, False, False),
        ("retrace", "monolith", "DirectVulkan", oit, False,
         {**vk, "MOBILEGL_MAGMA_DISABLE_BLENDED_DEPTH_WRITE": "1"}, plain, None, False, False),
        ("retrace", "monolith", "DirectGLES", oit, False, {}, plain, None, False, False),
        ("retrace", "monolith", "DirectVulkan", itrp, False, {**vk, "MOBILEGL_ITRP_FIX": "ForceOn"},
         plain, None, False, False),
        ("retrace", "inproc", "DirectGLES", other, False, expect_strict, plain, [], True, False),
        ("retrace", "inproc", "DirectVulkan", itrp, False,
         {**vk, "MOBILEGL_ITRP_FIX": "ForceOn", **expect_strict}, plain, [], True, False),
        ("retrace", "inproc", "DirectGLES", "OpenRA", False, expect_strict, plain, [], True, False),
        ("split", "spawn", "DirectGLES", other, True, {**split("spawn"), **reuse}, long, [], True, False),
        ("split", "spawn", "DirectGLES", other, False, split("spawn"), long, [], True, False),
        ("split", "spawn", "DirectVulkan", oit, True,
         {**vk, "MOBILEGL_MAGMA_DISABLE_BLENDED_DEPTH_WRITE": "1", **split("spawn"), **reuse},
         long, [], True, False),
        ("split", "spawn", "DirectVulkan", itrp, True,
         {**vk, "MOBILEGL_ITRP_FIX": "ForceOn", **split("spawn"), **reuse}, long, [], True, False),
        ("split", "spawn", "DirectGLES", "OpenRA", True,
         {**split("spawn"), "MGTRACE_WIRE_EVIDENCE": "1", **reuse}, long, ["--require-wire"], True, False),
        ("split", "inproc", "DirectVulkan", "OpenRA", True,
         {**vk, **split("inproc"), "MGTRACE_WIRE_EVIDENCE": "1", **reuse}, long, ["--require-wire"], True, False),
        ("split", "inproc", "DirectGLES", other, False, split("inproc"), long, [], True, False),
        ("verify", "", "DirectGLES", other, False, {"MOBILEGL_PIPE_VERIFY": "1"}, long, None, False, False),
        ("verify", "", "DirectGLES", "OpenRA", False, {"MOBILEGL_PIPE_VERIFY": "1"}, long, None, False, True),
        ("verify", "", "DirectVulkan", "OpenRA", False, {"MOBILEGL_PIPE_VERIFY": "1", **vk}, long, None, False, False),
        ("verify", "", "DirectVulkan", oit, False,
         {"MOBILEGL_PIPE_VERIFY": "1", **vk, "MOBILEGL_MAGMA_DISABLE_BLENDED_DEPTH_WRITE": "1"},
         long, None, False, False),
        ("verify", "", "DirectVulkan", itrp, False, {"MOBILEGL_PIPE_VERIFY": "1", **vk}, long, None, False, False),
    ]
    for profile, transport, backend, case, split_reuse, env, ctest, evidence, census, control in table:
        plan = case_plan(profile, transport, backend, case, split_reuse=split_reuse, server_path=server)
        wanted = {"env": env, "ctest": ctest + regex(case, backend), "evidence": evidence,
                  "census": census, "control": control}
        assert plan == wanted, f"{profile} {transport} {backend} {case}: {plan} != {wanted}"
    control = control_plan()
    assert control["env"] == {"MOBILEGL_PIPE_VERIFY": "1", "MOBILEGL_PIPE_VERIFY_CORRUPT": "GetRenderStateParameters"}
    assert control["ctest"] == long + regex("OpenRA", "DirectGLES")
    for bad in (("split", "spawn", ""), ("split", "spawn", "relative/server"), ("split", "monolith", server),
                ("pull", "monolith", server)):
        try:
            case_plan(bad[0], bad[1], "DirectGLES", other, server_path=bad[2])
        except ValueError:
            continue
        raise AssertionError(f"case_plan accepted {bad}")
    assert expected_results("verify", "DirectGLES", ["a", "OpenRA", "b"]) == ["a", "OpenRA", CONTROL_NAME, "b"]
    assert expected_results("verify", "DirectVulkan", ["OpenRA"]) == ["OpenRA"]
    assert expected_results("split", "DirectGLES", ["OpenRA"]) == ["OpenRA"]
    entry = lambda name: {"case": name, "status": "passed"}
    assert verdict_problems(["a", "b"], [entry("a"), entry("b")]) == []
    assert verdict_problems(["a", "b"], [entry("a")]), "a case without a verdict passed the check"
    assert verdict_problems(["a", "b"], []), "a shard that ran nothing passed the check"
    assert verdict_problems(["a"], [entry("a"), entry("a")]), "a repeated verdict passed the check"
    assert verdict_problems(["a", "b"], [entry("a"), entry("c")]), "an unassigned case passed the check"
    assert verdict_problems(["a", "b"], [entry("b"), entry("a")]), "out-of-order verdicts passed the check"
    print(f"retrace_shard: {len(table)} plans, the control plan and the verdict check OK")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", nargs="?", choices=("run",))
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--profile", choices=("retrace", "split", "verify"))
    parser.add_argument("--backend")
    parser.add_argument("--transport", default="")
    parser.add_argument("--lane", default="")
    parser.add_argument("--shard", default="", help="N/M, for the report only")
    parser.add_argument("--build-dir", help="the directory ctest runs in")
    parser.add_argument("--result-dir", help="where verdicts, replay output and JUnit files go")
    parser.add_argument("--names", help="the cases, as a JSON list")
    parser.add_argument("--names-env", default="SHARD_NAMES", help="the variable holding the JSON list")
    parser.add_argument("--split-reuse", action="store_true",
                        help="split profile: the library is the retrace job's, so prove the IPC defaults")
    parser.add_argument("--server-path", default="", help="split profile: absolute path of the server image")
    parser.add_argument("--ctest", default="ctest")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.command != "run" or not (args.profile and args.backend and args.build_dir and args.result_dir):
        parser.error("run needs --profile, --backend, --build-dir and --result-dir")
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
