#!/usr/bin/env python3
# MobileGL - android-plugin/test_trace_replay_shard.py
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

"""Drive the emulator shard runner with a fake adb, replay script and AVD script.

No device is needed. The fakes record every call, so the tests pin what each case is run
with, that every case ends with a verdict, and what a failure, an infrastructure retry, a
timeout or a dead emulator does to the rest of the shard.
"""

import contextlib
import importlib.util
import io
import json
import os
import pathlib
import re
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

HERE = pathlib.Path(__file__).resolve().parent


def load(name):
    spec = importlib.util.spec_from_file_location(name, HERE / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


shard = load("trace_replay_shard")
budget = load("retrace_shard_budget")

BLISS = "minecraft-1.21.4-fabric-iris-bliss-in-world"
ITERATIONRP = "minecraft-1.21.4-fabric-iris-iterationrp-in-world"
SPLIT = "MOBILEGL_IPC_ROLE_SPLIT_STATE=1;MOBILEGL_IPC_STRICT_ERRORS=1;MOBILEGL_IPC_RUN_AHEAD=1"

FAKE_COMMON = r'''
import json, os, pathlib, sys
state = pathlib.Path(os.environ["FAKE_STATE"])
def record(name, entry):
    with open(state / name, "a", encoding="utf-8") as log:
        log.write(json.dumps(entry) + "\n")
def scenario():
    path = state / "scenario.json"
    return json.loads(path.read_text()) if path.exists() else {}
def count(name):
    path = state / f"count-{name}"
    value = int(path.read_text()) if path.exists() else 0
    path.write_text(str(value + 1))
    return value
'''

FAKE_ADB = FAKE_COMMON + r'''
args = sys.argv[1:]
record("adb.log", args)
plan = scenario()
if args[:1] == ["get-state"]:
    print("offline" if count("get-state") < plan.get("offline_get_state_calls", 0) else "device")
elif args[:3] == ["shell", "getprop", "sys.boot_completed"]:
    print("1")
elif args[:3] == ["shell", "pm", "path"]:
    print("package:/data/app/trace/base.apk")
elif args[:3] == ["shell", "pm", "clear"]:
    print("Failed" if plan.get("pm_clear_fails") else "Success")
elif args[:1] == ["install"]:
    if count("install") < plan.get("failing_installs", 0):
        print("Failure [INSTALL_FAILED]")
        sys.exit(1)
    print("Success")
elif args[:2] == ["shell", "run-as"] and args[3:] == ["id", "-un"]:
    print("u0_a77")
elif args[:3] == ["shell", "ps", "-A"]:
    print("USER PID NAME")
    print("root 1 init")
    if count("ps") < plan.get("ps_calls_with_leftover", 0):
        print("u0_a77 4242 libMobileGLServer.so")
'''

FAKE_REPLAY = FAKE_COMMON + r'''
import time
args = sys.argv[1:]
def value(flag):
    return args[args.index(flag) + 1]
case, backend, root = value("--case"), value("--backend"), pathlib.Path(value("--result-root"))
record("replay.log", {"case": case, "arguments": args, "environment": {
    key: os.environ.get(key) for key in (
        "MOBILEGL_ESPRYT_USE_ANGLE", "MOBILEGL_TRACE_ANGLE_VARIANT", "MOBILEGL_MAGMA_R11G11B10F_FALLBACK",
        "MOBILEGL_ITRP_FIX", "MOBILEGL_TRACE_SKIP_INSTALL", "MOBILEGL_TRACE_POLL_INTERVAL")}})
(state / "replay.pid").write_text(str(os.getpid()))
steps = scenario().get("replay", {}).get(case, [])
attempt = count("replay-" + case)
step = steps[attempt] if attempt < len(steps) else {}
time.sleep(step.get("sleep", 0))
directory = root / f"{case}-{backend}"
directory.mkdir(parents=True, exist_ok=True)
code = step.get("exit", 0)
if code == 75:
    with open(root / "infrastructure-failure-reason.txt", "a") as reasons:
        reasons.write(step.get("reason", "device-unavailable") + "\n")
if step.get("result", code in (0, 1)):
    (directory / "result.json").write_text(json.dumps(
        {"passed": step.get("passed", code == 0), "ssim": 0.999, "message": step.get("message", "retrace completed")}))
transport = "inproc" if "--require-inproc" in args else "spawn" if "--require-spawn" in args else ""
if transport and code == 0 and step.get("proof", True):
    (directory / "transport-proof.json").write_text(json.dumps({"passed": True, "required_transport": transport}))
sys.exit(code)
'''

FAKE_AVD = FAKE_COMMON + r'''
args = sys.argv[1:]
record("avd.log", args)
if args[:1] == ["start"]:
    exits = scenario().get("avd_start_exits", [])
    index = count("avd-start")
    sys.exit(exits[index] if index < len(exits) else 0)
'''


def case(name, **extra):
    base = {"name": name, "trace_archive": f"fixtures/{name}.tgz", "trace_file": "trace.trace",
            "golden": f"fixtures/{name}.png", "target_call": 1234, "width": 854, "height": 480,
            "ssim_threshold": 0.99, "crop_x": 0, "crop_y": 0, "crop_width": 0, "crop_height": 0,
            "timeout_seconds": 900}
    base.update(extra)
    return base


class ShardTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name)
        self.state = self.root / "state"
        self.state.mkdir()
        self.results = self.root / "results"
        (self.root / "fixtures").mkdir()
        self.apk = self.root / "trace.apk"
        self.apk.write_bytes(b"apk")
        self.emulator_log = self.root / "emulator.log"
        self.emulator_log.write_text("boot\n")
        python = pathlib.Path(sys.executable).as_posix()
        commands = {}
        for name, source in (("adb", FAKE_ADB), ("replay", FAKE_REPLAY), ("avd", FAKE_AVD)):
            script = self.root / f"fake_{name}.py"
            script.write_text(source)
            commands[name] = f'"{python}" "{script.as_posix()}"'
        self.environment = {
            "FAKE_STATE": str(self.state), "ADB": commands["adb"],
            "MOBILEGL_SHARD_REPLAY_COMMAND": commands["replay"], "MOBILEGL_SHARD_AVD_COMMAND": commands["avd"],
            "MOBILEGL_GPU_HANG_BUDGET_MS": "0", "AVD_NAME": "mobilegl-ci",
            "EMULATOR_LOG": str(self.emulator_log), "EMULATOR_PID_FILE": str(self.root / "emulator.pid"),
            "GITHUB_STEP_SUMMARY": str(self.root / "summary.md"),
        }

    def fixture(self, name):
        path = self.root / "fixtures" / name
        path.write_bytes(b"png")
        return str(path)

    def run_shard(self, cases, *extra, backend="DirectVulkan", transport="monolith", scenario=None, env=None):
        if scenario is not None:
            (self.state / "scenario.json").write_text(json.dumps(scenario))
        arguments = ["--apk-file", str(self.apk), "--backend", backend, "--gpu", "lavapipe",
                     "--transport", transport, "--lane", f"{transport}-lane", "--cases-json", json.dumps(cases),
                     "--result-root", str(self.results), "--fixture-root", str(self.root / "work"), *extra]
        output = io.StringIO()
        with mock.patch.dict(os.environ, {**self.environment, **(env or {})}), \
                mock.patch.object(shard, "pause", lambda seconds: None), \
                contextlib.redirect_stdout(output):
            status = shard.main(arguments)
        return status, output.getvalue()

    def log(self, name):
        path = self.state / name
        return [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []

    def status(self, name, backend="DirectVulkan"):
        return json.loads((self.results / f"{name}-{backend}" / "case-status.json").read_text())

    def shard_status(self):
        return json.loads((self.results / "shard-status.json").read_text())

    # ---- what each case is run with ----------------------------------------------------

    def planned(self, cases, backend, transport):
        status, output = self.run_shard(cases, "--dry-run", backend=backend, transport=transport)
        self.assertEqual(status, 0)
        return [json.loads(line) for line in output.splitlines()]

    def test_plain_case_arguments_per_transport(self):
        cases = [case("plain", alternate_golden="fixtures/plain-mali.png")]
        head = ["--apk-file", str(self.apk), "--package", "top.mobilegl.plugin.trace", "--backend", "DirectVulkan",
                "--result-root", str(self.results), "--fixture-root", str(self.root / "work"), "--case", "plain",
                "--trace-archive", "fixtures/plain.tgz", "--trace-file", "trace.trace",
                "--golden", "fixtures/plain.png", "--alternate-golden", "fixtures/plain-mali.png",
                "--target-call", "1234", "--width", "854", "--height", "480", "--ssim-threshold", "0.99",
                "--crop-x", "0", "--crop-y", "0", "--crop-width", "0", "--crop-height", "0",
                "--timeout-seconds", "900"]
        tails = {
            "monolith": ["--env", "MOBILEGL_TRANSPORT=monolith;MOBILEGL_GPU_HANG_BUDGET_MS=0"],
            "inproc": ["--env", f"MOBILEGL_TRANSPORT=inproc;{SPLIT};MOBILEGL_GPU_HANG_BUDGET_MS=0", "--require-inproc"],
            "spawn": ["--env", f"MOBILEGL_TRANSPORT=spawn;{SPLIT};MOBILEGL_GPU_HANG_BUDGET_MS=0",
                      "--use-pbuffer", "--require-spawn"],
        }
        for transport, tail in tails.items():
            plan, = self.planned(cases, "DirectVulkan", transport)
            self.assertEqual(plan["arguments"], head + tail, transport)
            self.assertEqual(plan["timeout"], 1200)
            self.assertEqual(plan["environment"], {
                "MOBILEGL_ESPRYT_USE_ANGLE": "0", "MOBILEGL_TRACE_ANGLE_VARIANT": "ec889e6ea831",
                "MOBILEGL_MAGMA_R11G11B10F_FALLBACK": "1", "MOBILEGL_ITRP_FIX": "Auto"})

    def test_case_specific_settings(self):
        cases = [case(BLISS), case(ITERATIONRP), case("flush", coherent_as_flush=True),
                 case("lod", avoid_angle_llvmpipe_explicit_lod_bias=True), case("plain", ssim_threshold=0.985)]
        gles = {plan["case"]: plan for plan in self.planned(cases, "DirectGLES", "monolith")}
        vulkan = {plan["case"]: plan for plan in self.planned(cases, "DirectVulkan", "monolith")}

        def flags(plan):
            return [argument for argument in plan["arguments"][plan["arguments"].index("--env") + 2:]]

        self.assertEqual(gles[BLISS]["environment"]["MOBILEGL_TRACE_ANGLE_VARIANT"], "90a62123d794")
        self.assertEqual(vulkan[BLISS]["environment"]["MOBILEGL_TRACE_ANGLE_VARIANT"], "90a62123d794")
        self.assertEqual(gles["plain"]["environment"], {
            "MOBILEGL_ESPRYT_USE_ANGLE": "1", "MOBILEGL_TRACE_ANGLE_VARIANT": "ec889e6ea831",
            "MOBILEGL_MAGMA_R11G11B10F_FALLBACK": "0", "MOBILEGL_ITRP_FIX": "Auto"})
        self.assertEqual(gles[ITERATIONRP]["environment"]["MOBILEGL_ITRP_FIX"], "ForceOn")
        self.assertEqual(flags(gles[BLISS]), ["--avoid-angle-llvmpipe-sampler-mipmap-min-filter"])
        self.assertEqual(flags(vulkan[BLISS]), [])
        self.assertEqual(flags(gles["lod"]), ["--avoid-angle-llvmpipe-explicit-lod-bias"])
        self.assertEqual(flags(vulkan["lod"]), [])
        self.assertEqual(flags(gles["flush"]), ["--coherent-as-flush"])
        self.assertEqual(flags(vulkan["flush"]), ["--coherent-as-flush"])
        self.assertEqual(flags(gles["plain"]), [])
        arguments = gles["plain"]["arguments"]
        self.assertEqual(arguments[arguments.index("--ssim-threshold") + 1], "0.985")
        self.assertEqual(arguments[arguments.index("--alternate-golden") + 1], "")

    def test_misconfigured_case_lists_run_nothing(self):
        for cases in ([case("a"), case("a")], [case("a", ci_backends=["DirectGLES"])], [{"name": "a"}], []):
            status, output = self.run_shard(cases)
            self.assertEqual(status, 2, cases)
            self.assertIn("::error title=Android retrace shard misconfigured::", output)
        self.assertEqual(self.log("adb.log"), [])

    # ---- the loop ----------------------------------------------------------------------

    def test_every_case_runs_in_order_on_one_install(self):
        cases = [case("a", golden=self.fixture("a.png"), alternate_golden=self.fixture("a-mali.png")),
                 case("b", golden=self.fixture("b.png")), case("c")]
        status, output = self.run_shard(cases, transport="inproc")
        self.assertEqual(status, 0, output)
        self.assertEqual([entry["case"] for entry in self.log("replay.log")], ["a", "b", "c"])
        for entry in self.log("replay.log"):
            self.assertEqual(entry["environment"]["MOBILEGL_TRACE_SKIP_INSTALL"], "1")
            self.assertEqual(entry["environment"]["MOBILEGL_TRACE_POLL_INTERVAL"], "0.5")
        adb = self.log("adb.log")
        self.assertEqual(sum(1 for call in adb if call[:1] == ["install"]), 1)
        self.assertEqual(sum(1 for call in adb if call[:3] == ["shell", "pm", "clear"]), 3)
        self.assertEqual(sum(1 for call in adb if call[:3] == ["shell", "rm", "-f"]), 3)
        self.assertIn(["shell", "rm", "-f", "/data/local/tmp/mobilegl-b.trace", "/data/local/tmp/mobilegl-b.golden.png",
                       "/data/local/tmp/mobilegl-b.alternate-golden.png"], adb)
        self.assertEqual(self.log("avd.log"), [])
        directory = self.results / "a-DirectVulkan"
        self.assertEqual(json.loads((directory / "lane.json").read_text()),
                         {"lane": "inproc-lane", "transport": "inproc"})
        self.assertTrue((directory / "a-DirectVulkan-golden.png").is_file())
        self.assertTrue((directory / "a-DirectVulkan-alternate-golden.png").is_file())
        self.assertFalse((self.results / "c-DirectVulkan" / "c-DirectVulkan-golden.png").exists())
        whole = self.shard_status()
        self.assertTrue(whole["complete"])
        self.assertEqual([(entry["case"], entry["status"]) for entry in whole["cases"]],
                         [("a", "passed"), ("b", "passed"), ("c", "passed")])
        self.assertIn("| b | passed | 1 |", (self.root / "summary.md").read_text())

    def test_a_failed_case_does_not_stop_the_shard(self):
        status, output = self.run_shard([case("a"), case("b"), case("c")], scenario={
            "replay": {"b": [{"exit": 1, "message": "ssim=0.5"}]}})
        self.assertEqual(status, 1)
        self.assertEqual([entry["case"] for entry in self.log("replay.log")], ["a", "b", "c"])
        self.assertEqual([self.status(name)["status"] for name in "abc"], ["passed", "failed", "passed"])
        self.assertIn("::error title=Android retrace failed (DirectVulkan / monolith)::b: exit 1: ssim=0.5", output)
        self.assertEqual(output.count("::error"), 1)
        self.assertTrue(self.shard_status()["complete"])
        self.assertTrue((self.results / "b-DirectVulkan" / "emulator.log").is_file())

    def test_the_case_after_one_that_did_not_pass_starts_on_a_cold_boot(self):
        status, _ = self.run_shard([case("a"), case("b"), case("c"), case("d")], scenario={
            "replay": {"a": [{"exit": 75}], "b": [{"exit": 1}]}})
        self.assertEqual(status, 1)
        self.assertEqual([self.status(name)["status"] for name in "abcd"], ["passed", "failed", "passed", "passed"])
        # One restart for a's infrastructure retry and one before c. A case that passed on
        # its retry costs the next case nothing.
        self.assertEqual([call[0] for call in self.log("avd.log")], ["stop", "start", "stop", "start"])
        self.assertEqual(self.status("b")["notes"], [])
        self.assertEqual(self.status("c")["notes"], ["emulator restarted before the case: "
                                                    "the case before it, b, did not pass"])
        self.assertEqual(self.status("d")["notes"], [])
        self.assertEqual(sum(1 for call in self.log("adb.log") if call[:1] == ["install"]), 3)

    def test_a_failed_case_without_an_avd_to_restart_does_not_stop_the_shard(self):
        status, _ = self.run_shard([case("a"), case("b")], scenario={"replay": {"a": [{"exit": 1}]}},
                                   env={"AVD_NAME": ""})
        self.assertEqual(status, 1)
        self.assertEqual([self.status(name)["status"] for name in "ab"], ["failed", "passed"])
        self.assertEqual(self.log("avd.log"), [])

    def test_an_exit_0_without_a_passed_result_or_proof_fails(self):
        status, _ = self.run_shard([case("a"), case("b")], transport="spawn", scenario={
            "replay": {"a": [{"exit": 0, "result": False}], "b": [{"exit": 0, "proof": False}]}})
        self.assertEqual(status, 1)
        self.assertIn("without a passed result.json", self.status("a")["reason"])
        self.assertIn("without a passed spawn transport proof", self.status("b")["reason"])

    def test_an_infrastructure_failure_restarts_and_retries_once(self):
        status, output = self.run_shard([case("a"), case("b")], scenario={
            "replay": {"a": [{"exit": 75, "reason": "angle-surface-lost", "result": True, "passed": False}]}})
        self.assertEqual(status, 0, output)
        self.assertEqual([entry["case"] for entry in self.log("replay.log")], ["a", "a", "b"])
        self.assertEqual([call[0] for call in self.log("avd.log")], ["stop", "start"])
        self.assertEqual(self.log("avd.log")[1], [
            "start", "--avd-name", "mobilegl-ci", "--emulator-log", str(self.emulator_log),
            "--pid-file", str(self.root / "emulator.pid"), "--gpu", "lavapipe", "--boot-timeout", "300"])
        self.assertIn(["kill-server"], self.log("adb.log"))
        self.assertEqual(sum(1 for call in self.log("adb.log") if call[:1] == ["install"]), 2)
        record = self.status("a")
        self.assertEqual((record["status"], record["attempts"], record["exit_codes"]), ("passed", 2, [75, 0]))
        self.assertEqual(record["surface_lost_retries"], 1)
        kept = self.results / "a-DirectVulkan" / "first-attempt"
        self.assertTrue((kept / "result.first-attempt.json").is_file())
        self.assertFalse((kept / "result.json").exists())
        self.assertIn("::warning::Android emulator infrastructure failed", output)
        self.assertIn("surface-lost retries: 1 (DirectVulkan, a)", (self.root / "summary.md").read_text())
        self.assertTrue((self.results / "diagnostics" / "emulator-boot-1-ended.log").is_file())
        self.assertEqual(self.shard_status()["emulator_boots"], 2)

    def test_a_second_infrastructure_failure_is_the_verdict(self):
        status, _ = self.run_shard([case("a"), case("b")], scenario={
            "replay": {"a": [{"exit": 75}, {"exit": 75}]}})
        self.assertEqual(status, 1)
        self.assertEqual(self.status("a")["exit_codes"], [75, 75])
        self.assertEqual(self.status("a")["status"], "failed")
        self.assertEqual(self.status("b")["status"], "passed")
        # The surface-lost count is this case's own: the reasons of earlier cases do not leak in.
        self.assertEqual(self.status("b")["surface_lost_retries"], 0)

    def test_the_deadline_cuts_a_running_case_and_names_the_rest(self):
        status, output = self.run_shard([case("a"), case("b"), case("c")], "--deadline-seconds", "5", scenario={
            "replay": {"b": [{"sleep": 30}]}})
        self.assertEqual(status, 1)
        self.assertEqual([entry["case"] for entry in self.log("replay.log")], ["a", "b"])
        # b never had its own 1200 s, so the cut is not charged to it as a failure.
        self.assertEqual([self.status(name)["status"] for name in "abc"], ["passed", "not-run", "not-run"])
        self.assertRegex(self.status("b")["reason"], r"^cut by the shard deadline after \d+ s of a 1200 s budget$")
        self.assertEqual(self.status("b")["exit_codes"], [124])
        self.assertIn("deadline passed before this case started", self.status("c")["reason"])
        self.assertIn("::error title=Android retrace not-run (DirectVulkan / monolith)::b: cut by the shard deadline",
                      output)
        self.assertIn("::error title=Android retrace shard incomplete (DirectVulkan / monolith)::"
                      "1 verdict(s) for 3 case(s)", output)
        self.assertFalse(self.shard_status()["complete"])
        # Nothing is booted for a case the deadline keeps from starting.
        self.assertEqual(self.log("avd.log"), [])

    def test_a_case_that_runs_out_of_its_own_budget_fails(self):
        # The deadline is far away, so the one second a has is its own budget.
        with mock.patch.object(shard, "REPLAY_GRACE_SECONDS", 0):
            status, _ = self.run_shard([case("a", timeout_seconds=1), case("b")], "--deadline-seconds", "600",
                                       scenario={"replay": {"a": [{"sleep": 30}]}})
        self.assertEqual(status, 1)
        self.assertEqual([self.status(name)["status"] for name in "ab"], ["failed", "passed"])
        self.assertEqual(self.status("a")["reason"], "the replay script exceeded its time budget")
        self.assertTrue(self.shard_status()["complete"])

    def test_an_infrastructure_retry_is_not_started_after_the_deadline(self):
        def restart_past_the_deadline(runner, reason):
            runner.started -= 1000
            return True

        with mock.patch.object(shard.Shard, "restart_emulator", restart_past_the_deadline):
            status, _ = self.run_shard([case("a"), case("b")], "--deadline-seconds", "600",
                                       scenario={"replay": {"a": [{"exit": 75}]}})
        self.assertEqual(status, 1)
        self.assertEqual([entry["case"] for entry in self.log("replay.log")], ["a"])
        self.assertEqual([self.status(name)["status"] for name in "ab"], ["not-run", "not-run"])
        self.assertEqual(self.status("a")["reason"],
                         "the shard deadline passed before the infrastructure retry could start")
        self.assertEqual(self.status("a")["exit_codes"], [75])

    # ---- the time a shard needs --------------------------------------------------------

    def test_needed_seconds_lets_every_case_use_its_budget(self):
        cases = [case("a", timeout_seconds=1800), case("b", timeout_seconds=900), case("c", timeout_seconds="180")]
        # Every case to the end of its budget, the longest once more, and the overhead per case.
        self.assertEqual(shard.needed_seconds(cases), (2100 + 1200 + 480) + 2100 + 3 * 180)
        self.assertEqual(shard.needed_seconds(cases[2:]), 480 + 480 + 180)

    def test_a_deadline_of_needed_seconds_gives_every_hanging_case_its_verdict(self):
        cases = [case(name, timeout_seconds=1) for name in "abc"]
        # Scaled down: one second per case, and ten for what the fakes take around it.
        with mock.patch.object(shard, "REPLAY_GRACE_SECONDS", 0), \
                mock.patch.object(shard, "CASE_OVERHEAD_SECONDS", 10):
            deadline = shard.needed_seconds(cases)
            self.assertEqual(deadline, 3 + 1 + 30)
            status, _ = self.run_shard(cases, "--deadline-seconds", str(deadline), scenario={
                "replay": {name: [{"sleep": 60}] for name in "abc"}})
        self.assertEqual(status, 1)
        self.assertEqual([entry["case"] for entry in self.log("replay.log")], ["a", "b", "c"])
        for name in "abc":
            self.assertEqual((self.status(name)["status"], self.status(name)["reason"]),
                             ("failed", "the replay script exceeded its time budget"))
        self.assertTrue(self.shard_status()["complete"])
        self.assertEqual([call[0] for call in self.log("avd.log")], ["stop", "start", "stop", "start"])

    def test_every_shard_gets_its_own_deadline_and_job_timeout(self):
        matrix = {"include": [
            {"backend": {"name": "DirectGLES", "gpu": "software"}, "lane": "spawn-acceptance", "transport": "spawn",
             "shard": 1, "shards": 2, "names": ["a", "b"],
             "cases": [case("a", timeout_seconds=1800), case("b", timeout_seconds=1800)]},
            {"backend": {"name": "DirectVulkan", "gpu": "lavapipe"}, "shard": 2, "shards": 2, "names": ["c"],
             "cases": [case("c", timeout_seconds=180)]}]}
        matrix_file = self.root / "matrix.json"
        matrix_file.write_text(json.dumps(matrix))

        def limits(cap, path=matrix_file):
            output, report = io.StringIO(), io.StringIO()
            with contextlib.redirect_stdout(output), contextlib.redirect_stderr(report):
                status = budget.main(["--cap-seconds", str(cap), "--matrix-file", str(path)])
            return status, output.getvalue(), report.getvalue()

        self.assertEqual(budget.shard_needs(matrix), [("DirectGLES / spawn, shard 1/2", 2, 6660),
                                                      ("DirectVulkan / monolith, shard 2/2", 1, 1140)])
        # Under the cap every shard gets exactly what its cases need.
        status, output, report = limits(6660)
        self.assertEqual(status, 0, report)
        rows = json.loads(output)["include"]
        self.assertEqual([(row["deadline_seconds"], row["timeout_minutes"]) for row in rows],
                         [(6660, 111 + budget.JOB_OVERHEAD_MINUTES), (1140, 19 + budget.JOB_OVERHEAD_MINUTES)])
        # Nothing but the two fields is added, and no case is dropped or changed.
        self.assertEqual([{key: value for key, value in row.items()
                           if key not in ("deadline_seconds", "timeout_minutes")} for row in rows],
                         matrix["include"])
        self.assertIn("2 shard(s), 0 capped at 6660 s; the most a shard needs is 6660 s", report)
        # Over the cap a shard is cut to it and named; the other shard is untouched.
        status, output, report = limits(6000)
        self.assertEqual(status, 0, report)
        rows = json.loads(output)["include"]
        self.assertEqual([(row["deadline_seconds"], row["timeout_minutes"]) for row in rows],
                         [(6000, 100 + budget.JOB_OVERHEAD_MINUTES), (1140, 19 + budget.JOB_OVERHEAD_MINUTES)])
        self.assertIn("DirectGLES / spawn, shard 1/2 would need 6660 s for all of its 2 case(s)", report)
        self.assertNotIn("DirectVulkan / monolith, shard 2/2 would need", report)
        # A cap that could not fit in a hosted job, and a matrix that cannot be read.
        self.assertEqual(limits(0)[0], 2)
        self.assertEqual(limits(360 * 60)[0], 2)
        for broken in ({"include": []}, {"include": [{"names": ["a"]}]}, []):
            matrix_file.write_text(json.dumps(broken))
            status, output, _ = limits(7200)
            self.assertEqual((status, output), (2, ""), broken)

    def test_the_workflow_takes_its_limits_from_the_matrix(self):
        text = (HERE.parent / ".github" / "workflows" / "apk.yml").read_text(encoding="utf-8")
        retrace = text[text.index("\n  retrace:"):text.index("\n  retrace-summary:")]
        cap, = re.findall(r'^\s+RETRACE_SHARD_DEADLINE_CAP_SECONDS: "(\d+)"\s*$', text, flags=re.MULTILINE)
        # The runner's deadline and the job's limit are the two fields the budget script adds.
        self.assertIn('--deadline-seconds "${{ matrix.deadline_seconds }}"', retrace)
        self.assertEqual(re.findall(r"^    timeout-minutes: (.+?)\s*$", retrace, flags=re.MULTILINE),
                         ["${{ matrix.timeout_minutes }}"])
        self.assertIn('--cap-seconds "${RETRACE_SHARD_DEADLINE_CAP_SECONDS}"', text)
        # The capped deadline and the job's overhead stay inside the six hours a hosted job may run.
        self.assertLessEqual(-(-int(cap) // 60) + budget.JOB_OVERHEAD_MINUTES, budget.HOSTED_JOB_LIMIT_MINUTES)

    def test_an_unhealthy_emulator_is_restarted_before_the_next_case(self):
        status, output = self.run_shard([case("a"), case("b")], scenario={"offline_get_state_calls": 1})
        self.assertEqual(status, 0, output)
        self.assertEqual([call[0] for call in self.log("avd.log")], ["stop", "start"])
        self.assertIn("emulator restarted before the case: adb state is offline", self.status("a")["notes"])
        self.assertEqual(self.status("b")["notes"], [])
        self.assertEqual([entry["case"] for entry in self.log("replay.log")], ["a", "b"])

    def test_the_restart_switch_cold_boots_before_every_case_but_the_first(self):
        status, _ = self.run_shard([case("a"), case("b"), case("c")],
                                   env={"MOBILEGL_SHARD_RESTART_EMULATOR_EACH_CASE": "1"})
        self.assertEqual(status, 0)
        self.assertEqual([call[0] for call in self.log("avd.log")], ["stop", "start", "stop", "start"])
        self.assertEqual(sum(1 for call in self.log("adb.log") if call[:1] == ["install"]), 3)

    def test_an_emulator_that_does_not_come_back_leaves_named_rows(self):
        status, output = self.run_shard([case("a"), case("b"), case("c")], scenario={
            "replay": {"a": [{"exit": 75}]}, "avd_start_exits": [2, 2]})
        self.assertEqual(status, 1)
        self.assertEqual([entry["case"] for entry in self.log("replay.log")], ["a"])
        self.assertEqual([self.status(name)["status"] for name in "abc"], ["failed", "not-run", "not-run"])
        self.assertEqual(output.count("::error title=Android retrace"), 4)
        for name in "abc":
            self.assertTrue((self.results / f"{name}-DirectVulkan" / "lane.json").is_file())

    def test_an_apk_that_does_not_install_runs_nothing(self):
        status, _ = self.run_shard([case("a"), case("b")], scenario={"failing_installs": 9})
        self.assertEqual(status, 1)
        self.assertEqual(self.log("replay.log"), [])
        self.assertEqual([self.status(name)["status"] for name in "ab"], ["not-run", "not-run"])

    def test_processes_left_after_force_stop_are_killed_and_recorded(self):
        # Listed on all four looks after the force-stop, gone after the kill.
        status, _ = self.run_shard([case("a"), case("b")], scenario={"ps_calls_with_leftover": 4})
        self.assertEqual(status, 0)
        self.assertIn(["shell", "run-as", "top.mobilegl.plugin.trace", "kill", "-9", "4242"], self.log("adb.log"))
        self.assertIn("processes left after force-stop: 4242 libMobileGLServer.so", self.status("a")["notes"])
        self.assertEqual(self.status("b")["notes"], [])
        self.assertEqual(self.log("avd.log"), [])

    def test_a_process_that_is_still_exiting_is_not_reported(self):
        status, _ = self.run_shard([case("a")], scenario={"ps_calls_with_leftover": 2})
        self.assertEqual(status, 0)
        self.assertEqual(self.status("a")["notes"], [])
        self.assertNotIn("kill", [call[3] for call in self.log("adb.log") if call[:2] == ["shell", "run-as"]])

    def test_processes_that_survive_the_kill_cost_an_emulator_restart(self):
        status, _ = self.run_shard([case("a")], scenario={"ps_calls_with_leftover": 10})
        self.assertEqual(status, 0)
        self.assertEqual([call[0] for call in self.log("avd.log")], ["stop", "start"])
        self.assertIn("the emulator was restarted to remove them", self.status("a")["notes"])

    def test_a_failed_pm_clear_falls_back_to_a_reinstall(self):
        status, _ = self.run_shard([case("a")], scenario={"pm_clear_fails": True})
        self.assertEqual(status, 0)
        adb = self.log("adb.log")
        self.assertIn(["uninstall", "top.mobilegl.plugin.trace"], adb)
        self.assertEqual(sum(1 for call in adb if call[:1] == ["install"]), 2)

    @unittest.skipUnless(os.name == "posix", "signals and process groups")
    def test_a_terminated_runner_takes_the_running_replay_with_it(self):
        cases = [case("a"), case("b")]
        (self.state / "scenario.json").write_text(json.dumps({"replay": {"a": [{"sleep": 60}]}}))
        arguments = ["--apk-file", str(self.apk), "--backend", "DirectVulkan", "--gpu", "lavapipe",
                     "--transport", "monolith", "--lane", "monolith-lane", "--cases-json", json.dumps(cases),
                     "--result-root", str(self.results), "--fixture-root", str(self.root / "work")]
        runner = subprocess.Popen([sys.executable, str(HERE / "trace_replay_shard.py")] + arguments,
                                  env={**os.environ, **self.environment}, stdout=subprocess.DEVNULL)
        deadline = time.monotonic() + 30
        while not (self.state / "replay.pid").exists() and time.monotonic() < deadline:
            time.sleep(0.05)
        replay_pid = int((self.state / "replay.pid").read_text())
        runner.send_signal(signal.SIGTERM)
        self.assertEqual(runner.wait(timeout=30), 128 + signal.SIGTERM)
        for _ in range(100):
            try:
                os.kill(replay_pid, 0)
            except OSError:
                break
            time.sleep(0.05)
        else:
            self.fail("the replay outlived the runner")
        # The step that runs afterwards names both cases.
        status, _ = self.run_shard(cases, "--finalize")
        self.assertEqual(status, 0)
        self.assertEqual([self.status(name)["status"] for name in "ab"], ["not-run", "not-run"])

    def test_finalize_names_every_case_without_a_device(self):
        cases = [case("a", golden=self.fixture("a.png")), case("b")]
        status, _ = self.run_shard(cases)
        self.assertEqual(status, 0)
        (self.results / "b-DirectVulkan" / "case-status.json").unlink()
        calls_before = len(self.log("adb.log"))
        status, _ = self.run_shard(cases + [case("c")], "--finalize")
        self.assertEqual(status, 0)
        self.assertEqual(len(self.log("adb.log")), calls_before)
        self.assertEqual([self.status(name)["status"] for name in "abc"], ["passed", "not-run", "not-run"])
        self.assertTrue((self.results / "c-DirectVulkan" / "lane.json").is_file())
        self.assertFalse(self.shard_status()["complete"])
        self.assertEqual(self.shard_status()["expected_cases"], ["a", "b", "c"])


if __name__ == "__main__":
    unittest.main()
