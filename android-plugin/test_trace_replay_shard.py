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
with, that every case ends with a verdict, what the job log says about it, and what a
failure, an infrastructure retry, a timeout or a dead emulator does to the rest of the shard.

The last two classes run the transport proofs of trace-replay-ci.sh itself: the monolith
check on log files, and the whole script against an adb that serves a host directory.
"""

import contextlib
import importlib.util
import io
import json
import os
import pathlib
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tarfile
import tempfile
import time
import unittest
from unittest import mock

HERE = pathlib.Path(__file__).resolve().parent
REPLAY_SCRIPT = HERE / "trace-replay-ci.sh"


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
elif args[:2] == ["exec-out", "run-as"] and args[3:4] == ["cat"] and "device_files" in plan:
    name = args[4].rsplit("/", 1)[-1]
    if name not in plan["device_files"]:
        print(f"cat: {args[4]}: No such file or directory")
        sys.exit(1)
    sys.stdout.write(plan["device_files"][name])
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
transport = next((name for name in ("monolith", "inproc", "spawn") if f"--require-{name}" in args), "")
if transport and code == 0 and step.get("proof", True):
    (directory / "transport-proof.json").write_text(json.dumps({"passed": True, "required_transport": transport}))
for name, text in step.get("files", {}).items():
    (directory / name).write_text(text, encoding="utf-8")
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

# Library log lines as the emulator jobs write them.
CLIENT = "[02:55:51] [Android MobileGLTraceRe/INFO]: "
SERVER = "[02:55:51] [Android mgl-srv-apply/INFO]: "
BACKEND_LINE = CLIENT + "Config: Active backend type set to DirectVulkan"
LOADED_LINE = CLIENT + "Config loaded"
INPROC_LINE = CLIENT + ("Config: MOBILEGL_TRANSPORT=inproc - the MGPipe record stream crosses a real ring to an "
                        "apply thread")
SPAWN_LINE = CLIENT + ("Config: MOBILEGL_TRANSPORT=spawn - the MGPipe record stream crosses a real ring to an "
                       "apply thread in ANOTHER PROCESS")
IPC_LINE = CLIENT + ("Config: IPC ring=8MiB stage=32MiB wire-deferred=64MiB spin=50us event-wait=2000ms "
                     "create-window=10 pbo-readback-sync=0 poll-escalate=64 adopt-tier=unset(T0) allow-t0=1 "
                     "run-ahead=1 present-credit=1 control-timeout=5000ms cold-start=20000ms strict=1 audit=0 "
                     "role-split-state=1 affinity='auto' surface=offscreen")
ARMED_LINE = CLIENT + ('MG_Remote client: spawn ARMED - the server role runs in pid 2252, reached at '
                       '"@mgl-2134-124880560801840"')
SERVER_LINE = SERVER + "MG_Remote server: mgl-srv-apply started."
REST_OF_LOG = [CLIENT + "MobileGL Backend Info:", CLIENT + "  Renderer Name: Magma"]


def log_text(*lines):
    return "\n".join(lines) + "\n"


def library_logs(transport):
    """The files a replay on the transport leaves in the app's output directory, by name."""
    start = [CLIENT + "Initializing MobileGL...", BACKEND_LINE]
    if transport == "monolith":
        return {"mobilegl.client.log": log_text(*start, LOADED_LINE, *REST_OF_LOG)}
    if transport == "inproc":
        return {"mobilegl.client.log": log_text(*start, INPROC_LINE, IPC_LINE, LOADED_LINE, *REST_OF_LOG),
                "mobilegl.server.log": log_text(SERVER_LINE)}
    return {"mobilegl.client.log": log_text(*start, SPAWN_LINE, IPC_LINE, LOADED_LINE, *REST_OF_LOG, ARMED_LINE),
            "mobilegl.server.log": log_text(SERVER_LINE)}


def said(output, kind, name):
    """What the runner's `kind` lines say about the case."""
    prefix = f"trace-replay-shard: {kind} {name}: "
    return [line[len(prefix):] for line in output.splitlines() if line.startswith(prefix)]


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
            "monolith": ["--env", "MOBILEGL_TRANSPORT=monolith;MOBILEGL_GPU_HANG_BUDGET_MS=0", "--require-monolith"],
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
            # What follows the transport's own arguments, which end with its proof.
            return plan["arguments"][plan["arguments"].index("--require-monolith") + 1:]

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

    def test_the_monolith_lane_needs_its_proof_too(self):
        status, _ = self.run_shard([case("a"), case("b")], scenario={"replay": {"a": [{"exit": 0, "proof": False}]}})
        self.assertEqual(status, 1)
        self.assertEqual(self.status("a")["reason"], "the replay exited 0 without a passed monolith transport proof")
        self.assertEqual(self.status("b")["status"], "passed")

    # ---- what the job log says ---------------------------------------------------------

    def test_the_log_says_what_each_case_is_run_with(self):
        cases = [case("a", coherent_as_flush=True), case(BLISS)]
        shared = {"MOBILEGL_TRACE_SKIP_INSTALL": "1", "MOBILEGL_TRACE_POLL_INTERVAL": "0.5"}
        planned = self.planned(cases, "DirectGLES", "spawn")
        # The test decides what the runner inherits, not the job the test runs in.
        with mock.patch.dict(os.environ, {"MOBILEGL_RETRACE_USE_PBUFFER": "1"}):
            for name in ("PYTHON", "MOBILEGL_TRACE_ENV"):
                os.environ.pop(name, None)
            status, output = self.run_shard(cases, backend="DirectGLES", transport="spawn")
        self.assertEqual(status, 0, output)
        lines = output.splitlines()
        for index, plan in enumerate(planned):
            name = plan["case"]
            run, = said(output, "run", name)
            # The line is the case's --dry-run line, and it opens the case's group.
            self.assertEqual(run, json.dumps(plan, sort_keys=True))
            self.assertEqual(lines[lines.index(f"::group::{name} ({index + 1}/2)") + 1],
                             f"trace-replay-shard: run {name}: {run}")
            # It is also what the replay script was started with.
            started = self.log("replay.log")[index]
            self.assertEqual(started["arguments"], plan["arguments"])
            self.assertEqual(started["environment"], {**plan["environment"], **shared})
        every, = [line for line in lines if line.startswith("trace-replay-shard: every run: ")]
        self.assertEqual(json.loads(every.split(": ", 2)[2]), {
            "command": shlex.split(self.environment["MOBILEGL_SHARD_REPLAY_COMMAND"]), "environment": shared,
            # What the replay script reads from the runner's own environment, set or not.
            "inherited": {"ADB": self.environment["ADB"], "PYTHON": None, "MOBILEGL_RETRACE_USE_PBUFFER": "1",
                          "MOBILEGL_TRACE_ENV": None}})

    def test_the_log_names_every_setting_the_replay_script_reads(self):
        # Each variable trace-replay-ci.sh takes from its environment is in the case's run line
        # or in the every-run line. The two it assigns itself are not settings.
        script = REPLAY_SCRIPT.read_text(encoding="utf-8")
        read = set(re.findall(r"\$\{([A-Z][A-Z0-9_]*)", script)) - {"IFS", "INFRASTRUCTURE_FAILURE_EXIT_CODE"}
        plan, = self.planned([case("a")], "DirectVulkan", "monolith")
        named = set(plan["environment"]) | {"MOBILEGL_TRACE_SKIP_INSTALL", "MOBILEGL_TRACE_POLL_INTERVAL"}
        self.assertEqual(read - named, set(shard.INHERITED_REPLAY_SETTINGS))

    def test_the_log_holds_every_record_and_the_shard_status(self):
        phases = "script-started 1000\nfixture-pushed 3500\napk-installed 3500\napp-started 4700\nwait-ended 52700\n"
        cases = [case("a"), case("b"), case("c")]
        status, output = self.run_shard(cases, scenario={"replay": {
            "a": [{"exit": 75}, {"files": {"phase-timings.txt": phases}}], "b": [{"exit": 1, "message": "ssim=0.5"}]}})
        self.assertEqual(status, 1)
        lines = output.splitlines()
        # a was retried on a second boot, and c starts on a third because b did not pass.
        verdicts = {"a": ("trace-replay-shard: a: passed", 2),
                    "b": ("trace-replay-shard: b: failed (exit 1: ssim=0.5)", 2),
                    "c": ("trace-replay-shard: c: passed", 3)}
        for name, (verdict, boots) in verdicts.items():
            record, = said(output, "record", name)
            self.assertEqual(json.loads(record), self.status(name))
            # The record and the boots come before the verdict line, which still ends the group.
            at = lines.index(verdict)
            self.assertEqual(lines[at - 2:at + 2], [
                f"trace-replay-shard: record {name}: {record}",
                f"trace-replay-shard: emulator boots after {name}: {boots}", verdict, "::endgroup::"])
        self.assertEqual(json.loads(said(output, "record", "a")[0])["exit_codes"], [75, 0])
        # The phases are those of the attempt that counts; a replay that left no timings has none.
        self.assertEqual(said(output, "phases", "a"),
                         ['{"fixture-pushed": 2.5, "apk-installed": 0.0, "app-started": 1.2, "wait-ended": 48.0}'])
        self.assertEqual(said(output, "phases", "b"), [])
        self.assertEqual(lines[-1], "trace-replay-shard: 2 passed, 1 not passed, 3 verdict(s) for 3 case(s)")
        self.assertTrue(lines[-2].startswith("trace-replay-shard: shard-status: "), lines[-2])
        self.assertEqual(json.loads(lines[-2].split(": ", 2)[2]), self.shard_status())
        # The step that runs afterwards prints the file as it leaves it.
        status, output = self.run_shard(cases, "--finalize")
        self.assertEqual(status, 0)
        self.assertEqual(json.loads(output.splitlines()[-1].split(": ", 2)[2]), self.shard_status())

    def test_the_log_quotes_the_transport_lines_of_each_library_log(self):
        refusals = [CLIENT + f"MGPipe: Fatal{{Refused, {index}}}" for index in range(40)]
        long_line = SERVER + "MGPipe: Fatal{" + "x" * 3000 + "}"
        inproc_log = log_text(BACKEND_LINE, INPROC_LINE, IPC_LINE, LOADED_LINE, *REST_OF_LOG, SERVER_LINE)
        status, output = self.run_shard([case(name) for name in "abcde"], transport="inproc", scenario={
            "replay": {
                "a": [{"files": {"mobilegl.log": inproc_log}}],
                # A failed case is quoted too, and a log full of such lines does not flood the job log.
                "b": [{"exit": 1, "files": {"mobilegl.log": log_text(BACKEND_LINE, LOADED_LINE, *refusals)}}],
                # Only the role logs: what the runner pulls itself when the replay script was killed.
                "c": [{"files": {"mobilegl.client.log": log_text(SPAWN_LINE, *REST_OF_LOG, ARMED_LINE),
                                 "mobilegl.server.log": log_text(SERVER_LINE, long_line)}}],
                # Outside ASCII: the quote is escaped, whatever the encoding of the job log.
                "d": [{"files": {"mobilegl.log": log_text(CLIENT + "MGPipe: Fatal{Refused, \u00a72.3}")}}],
                "e": [{"files": {"mobilegl.log": log_text(*REST_OF_LOG)}}]}})
        self.assertEqual(status, 1)
        self.assertEqual(said(output, "library-log", "a"),
                         [f"mobilegl.log: {line}" for line in (BACKEND_LINE, INPROC_LINE, IPC_LINE, LOADED_LINE)])
        quoted = said(output, "library-log", "b")
        self.assertEqual(quoted[:-1], [f"mobilegl.log: {line}" for line in [BACKEND_LINE, LOADED_LINE] + refusals[:10]])
        self.assertEqual(quoted[-1], "30 more such line(s) are not shown")
        self.assertEqual(said(output, "library-log", "c"), [
            f"mobilegl.client.log: {SPAWN_LINE}", f"mobilegl.client.log: {ARMED_LINE}",
            f"mobilegl.server.log: {long_line[:1000]}"])
        self.assertEqual(said(output, "library-log", "d"),
                         [f"mobilegl.log: {CLIENT}MGPipe: Fatal{{Refused, \\xa72.3}}"])
        self.assertEqual(said(output, "library-log", "e"), ["the library log has none of the lines looked for"])
        # The lines are in the case's group, before its record.
        lines = output.splitlines()
        quoted = [f"trace-replay-shard: library-log a: mobilegl.log: {line}" for line in (BACKEND_LINE, LOADED_LINE)]
        record = next(line for line in lines if line.startswith("trace-replay-shard: record a: "))
        self.assertLess(lines.index("::group::a (1/5)"), lines.index(quoted[0]))
        self.assertLess(lines.index(quoted[1]), lines.index(record))

    def test_the_quoted_library_log_is_that_of_the_attempt_that_counts(self):
        first = log_text(BACKEND_LINE, INPROC_LINE, IPC_LINE, LOADED_LINE)
        status, output = self.run_shard([case("a"), case("b")], scenario={"replay": {
            "a": [{"exit": 75, "files": {"mobilegl.log": first}},
                  {"files": {"mobilegl.log": log_text(BACKEND_LINE, LOADED_LINE)}}],
            "b": [{"exit": 75, "files": {"mobilegl.log": first}}, {}]}})
        self.assertEqual(status, 0, output)
        self.assertEqual(said(output, "library-log", "a"),
                         [f"mobilegl.log: {line}" for line in (BACKEND_LINE, LOADED_LINE)])
        # A retry that left no log is not quoted from the attempt it replaced.
        self.assertEqual(said(output, "library-log", "b"), ["no library log was pulled"])
        for name in "ab":
            kept = self.results / f"{name}-DirectVulkan" / "first-attempt" / "mobilegl.log"
            self.assertEqual(kept.read_text(), first)

    def test_a_replay_that_pulled_nothing_is_quoted_from_what_the_runner_pulls(self):
        # The replay script ended before its own pulls. A split build has its two role logs
        # on the device; a build without the split transports has the one log.
        devices = {
            "a": ({"mobilegl.client.log": log_text(BACKEND_LINE, INPROC_LINE, LOADED_LINE),
                   "mobilegl.server.log": log_text(SERVER_LINE)},
                  [f"mobilegl.client.log: {BACKEND_LINE}", f"mobilegl.client.log: {INPROC_LINE}",
                   f"mobilegl.client.log: {LOADED_LINE}"]),
            "b": ({"mobilegl.log": log_text(BACKEND_LINE, LOADED_LINE)},
                  [f"mobilegl.log: {BACKEND_LINE}", f"mobilegl.log: {LOADED_LINE}"])}
        for name, (files, quoted) in devices.items():
            with self.subTest(name):
                status, output = self.run_shard([case(name)], scenario={
                    "device_files": files, "replay": {name: [{"exit": 1, "result": False}]}})
                self.assertEqual(status, 1)
                self.assertEqual(said(output, "library-log", name), quoted)
                pulled = sorted(path.name for path in (self.results / f"{name}-DirectVulkan").glob("mobilegl*"))
                self.assertEqual(pulled, sorted(files))

    def test_a_case_without_a_library_log_says_so(self):
        status, output = self.run_shard([case("a")])
        self.assertEqual(status, 0)
        self.assertEqual(said(output, "library-log", "a"), ["no library log was pulled"])

    def test_the_quoted_lines_are_the_ones_the_proofs_read(self):
        # Every sentence a --require-* check of trace-replay-ci.sh looks for is one the runner quotes.
        script = REPLAY_SCRIPT.read_text(encoding="utf-8")
        for sentence in ("Config: MOBILEGL_TRANSPORT=inproc - the MGPipe record stream",
                         "Config: MOBILEGL_TRANSPORT=spawn - the MGPipe record stream",
                         "Config: MOBILEGL_TRANSPORT={transport} - the MGPipe record stream",
                         "spawn ARMED - the server role runs in pid", "Config: IPC", "Fatal\\{", "Config loaded"):
            self.assertIn(sentence, script)
            self.assertTrue(any(mark in sentence.replace("\\", "") for mark in shard.LIBRARY_LOG_EVIDENCE), sentence)

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


MONOLITH_CONFIRMED = ("Android retrace: monolith confirmed: the library loaded its configuration and logged no "
                      "inproc or spawn transport, no IPC configuration, no spawned server and no server role")
NO_LOG = "the library log is missing or empty"
NO_LOAD = "the library log does not show the configuration load"


def monolith_not_confirmed(reason):
    return (f"Android retrace: monolith not confirmed ({reason}): nothing a split transport writes was found, "
            "so the run is not failed")


class MonolithProofTest(unittest.TestCase):
    """The --require-monolith check of trace-replay-ci.sh, run on the log files of a result directory."""

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = pathlib.Path(self.temporary.name)
        script = REPLAY_SCRIPT.read_text(encoding="utf-8")
        start = script.index("<<'PY'\n", script.index('if [ "${require_monolith}" -eq 1 ]; then\n    "${PYTHON}"'))
        self.program = script[start + len("<<'PY'\n"):script.index("\nPY\n", start)]

    def check(self, **logs):
        """Returns (exit status, stdout, stderr, the proof) for a result directory holding the logs."""
        for name, text in logs.items():
            (self.directory / name.replace("_", ".")).write_text(text, encoding="utf-8")
        proof = self.directory / "transport-proof.json"
        done = subprocess.run([sys.executable, "-", str(self.directory / "mobilegl.log"), str(proof)],
                              input=self.program, capture_output=True, text=True)
        return done.returncode, done.stdout.strip(), done.stderr.strip(), json.loads(proof.read_text())

    def test_a_monolith_log_passes(self):
        # The split build names its only log after the client role; a build without the split
        # transports writes the same lines. Either way mobilegl.log is what the check reads.
        status, said_line, _, proof = self.check(mobilegl_log=library_logs("monolith")["mobilegl.client.log"])
        self.assertEqual((status, said_line), (0, MONOLITH_CONFIRMED))
        self.assertEqual({key: proof[key] for key in ("required_transport", "passed", "confirmed",
                                                      "configuration_loaded", "split_transport_markers",
                                                      "problems")},
                         {"required_transport": "monolith", "passed": True, "confirmed": True,
                          "configuration_loaded": True, "split_transport_markers": {"inproc": False, "spawn": False},
                          "problems": []})

    def test_anything_a_split_run_logs_fails_it(self):
        monolith = library_logs("monolith")["mobilegl.client.log"]
        inproc, spawn = (log_text(*library_logs(transport).values()) for transport in ("inproc", "spawn"))
        server_role = "a server role wrote a log of its own"
        failures = [
            ("the library log has the inproc transport marker", {"mobilegl_log": inproc}),
            ("the library log has the spawn transport marker", {"mobilegl_log": spawn}),
            ("the library log names a spawned server process", {"mobilegl_log": monolith + ARMED_LINE + "\n"}),
            ("the library log has an IPC configuration line", {"mobilegl_log": monolith + IPC_LINE + "\n"}),
            # The three headers a server role's line can have: the apply thread of an inproc
            # run, a thread of a spawned server, and a line a peer forwarded.
            (server_role, {"mobilegl_log": monolith, "mobilegl_server_log": SERVER_LINE}),
            (server_role, {"mobilegl_log": monolith, "mobilegl_server_log":
                           "[08:25:29] [Android libMobileGLServ/ERROR]: MG_Remote server: pid=2252 listening\n"}),
            (server_role, {"mobilegl_log": monolith, "mobilegl_server_log": "[peer/WARN]: refused\n"}),
            # Without the client's log, where the transport sentence is.
            (server_role, {"mobilegl_server_log": SERVER_LINE}),
        ]
        for problem, logs in failures:
            with self.subTest(problem, logs=sorted(logs.items())):
                for leftover in self.directory.iterdir():
                    leftover.unlink()
                status, said_line, error, proof = self.check(**logs)
                self.assertEqual((status, said_line), (1, ""))
                self.assertTrue(error.startswith("trace replay failed actual monolith/no-split-transport proof ("),
                                error)
                self.assertIn(problem, error)
                self.assertIn(problem, proof["problems"])
                self.assertEqual((proof["required_transport"], proof["passed"], proof["confirmed"]),
                                 ("monolith", False, False))

    def test_a_log_without_the_configuration_load_passes_as_not_confirmed(self):
        # A lost or cut log says nothing about the transport, so it does not fail the run; the
        # line and the proof say that monolith was not confirmed.
        for reason, logs in ((NO_LOG, {}), (NO_LOG, {"mobilegl_log": "\n"}),
                             (NO_LOAD, {"mobilegl_log": log_text(BACKEND_LINE)})):
            with self.subTest(reason, logs=logs):
                for leftover in self.directory.iterdir():
                    leftover.unlink()
                status, said_line, error, proof = self.check(**logs)
                self.assertEqual((status, said_line, error), (0, monolith_not_confirmed(reason), ""))
                self.assertEqual({key: proof[key] for key in ("required_transport", "passed", "confirmed",
                                                              "not_confirmed_because", "problems")},
                                 {"required_transport": "monolith", "passed": True, "confirmed": False,
                                  "not_confirmed_because": reason, "problems": []})

    def test_what_a_device_answers_for_a_missing_server_log_is_not_a_server_role(self):
        monolith = library_logs("monolith")["mobilegl.client.log"]
        for answer in ("cat: /data/user/0/top.mobilegl.plugin.trace/files/mobilegl.server.log: Permission denied\n",
                       "run-as: unknown package: top.mobilegl.plugin.trace\n", "\n"):
            with self.subTest(answer):
                # trace-replay-ci.sh appends what it pulled for the server role to mobilegl.log.
                status, said_line, _, proof = self.check(mobilegl_log=monolith + answer, mobilegl_server_log=answer)
                self.assertEqual((status, said_line), (0, MONOLITH_CONFIRMED))
                self.assertEqual((proof["server_role_log_lines"], proof["confirmed"]), (0, True))


# An adb whose device is the host directory FAKE_DEVICE: `exec-out run-as <package> cat <path>`
# prints the file of that name there, or cat's own complaint as a device folds it into stdout.
FAKE_DEVICE_ADB = r'''#!/bin/sh
case "$1 ${2:-}" in
  "get-state "*) echo "${FAKE_ADB_STATE:-device}" ;;
  "install "*) echo Success ;;
  "exec-out run-as")
    if [ -f "${FAKE_DEVICE}/${5##*/}" ]; then
      cat "${FAKE_DEVICE}/${5##*/}"
    else
      echo "cat: $5: No such file or directory"
      exit 1
    fi
    ;;
  "shell getprop") echo 1 ;;
  "shell pm") if [ "$3" = path ]; then echo "package:/data/app/trace/base.apk"; else echo Success; fi ;;
  "shell run-as") if [ "${4:-}" = id ]; then echo u0_a77; fi ;;
  "shell ps") echo "USER PID NAME" ;;
  shell*) case "$*" in *mobilegl-poll-result*) echo mobilegl-poll-result ;; esac ;;
esac
exit 0
'''

# The shard runner takes ADB as a command line; trace-replay-ci.sh runs it as one word.
FAKE_REPLAY_WRAPPER = r'''#!/bin/sh
ADB="${FAKE_DEVICE_ADB}"
export ADB
exec "${FAKE_SH}" "${FAKE_REPLAY_SCRIPT}" "$@"
'''

# BASH_EXE names a POSIX shell where `sh` is not one, for example Git Bash on Windows.
POSIX_SH = os.environ.get("BASH_EXE") or shutil.which("sh")


@unittest.skipUnless(POSIX_SH, "needs a POSIX sh (BASH_EXE) to run trace-replay-ci.sh")
class ReplayScriptTest(unittest.TestCase):
    """trace-replay-ci.sh itself, alone and under the shard runner, against the fake device."""

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name)
        self.device = self.root / "device"
        for directory in (self.device, self.root / "fixtures"):
            directory.mkdir()
        # A checkout with CRLF line endings does not run under sh.
        scripts = {"trace-replay-ci.sh": REPLAY_SCRIPT.read_bytes().replace(b"\r\n", b"\n"),
                   "adb": FAKE_DEVICE_ADB.encode(), "replay.sh": FAKE_REPLAY_WRAPPER.encode()}
        for name, content in scripts.items():
            (self.root / name).write_bytes(content)
            (self.root / name).chmod(0o755)
        (self.root / "trace.apk").write_bytes(b"apk")
        (self.root / "fixtures" / "a.png").write_bytes(b"png")
        (self.root / "trace.trace").write_bytes(b"trace")
        with tarfile.open(self.root / "fixtures" / "a.tgz", "w:gz") as archive:
            archive.add(self.root / "trace.trace", arcname="trace.trace")
        (self.device / "result.json").write_text('{"passed": true, "ssim": 0.999, "message": "retrace completed"}\n')
        sh = pathlib.Path(POSIX_SH)
        # Nothing of the job that runs the tests: its replay settings, its emulator, its summary.
        own = ("AVD_NAME", "EMULATOR_LOG", "EMULATOR_PID_FILE", "GITHUB_STEP_SUMMARY")
        self.environment = {key: value for key, value in os.environ.items()
                            if not key.startswith("MOBILEGL_") and key not in own}
        self.environment.update({
            "GITHUB_STEP_SUMMARY": str(self.root / "summary.md"),
            "PATH": str(sh.parent) + os.pathsep + os.environ.get("PATH", ""),
            "PYTHON": pathlib.Path(sys.executable).as_posix(), "PYTHONDONTWRITEBYTECODE": "1",
            "FAKE_DEVICE": self.device.as_posix(), "FAKE_DEVICE_ADB": (self.root / "adb").as_posix(),
            "FAKE_SH": sh.as_posix(), "FAKE_REPLAY_SCRIPT": (self.root / "trace-replay-ci.sh").as_posix()})

    def device_has(self, files):
        for name, text in files.items():
            (self.device / name).write_bytes(text.encode())

    # Paths are relative to the temporary directory: tar reads a drive letter as a host name.
    CASE = ["--apk-file", "trace.apk", "--package", "top.mobilegl.plugin.trace", "--backend", "DirectVulkan",
            "--result-root", "results", "--fixture-root", "work", "--case", "a",
            "--trace-archive", "fixtures/a.tgz", "--trace-file", "trace.trace", "--golden", "fixtures/a.png",
            "--target-call", "1234", "--width", "854", "--height", "480", "--ssim-threshold", "0.99",
            "--crop-x", "0", "--crop-y", "0", "--crop-width", "0", "--crop-height", "0", "--timeout-seconds", "5"]

    def replay(self, *flags, adb_state="device"):
        """Returns (exit status, stdout, stderr, the proof or None) of one run of the script."""
        environment = {**self.environment, "ADB": self.environment["FAKE_DEVICE_ADB"], "FAKE_ADB_STATE": adb_state}
        done = subprocess.run([POSIX_SH, "trace-replay-ci.sh", *self.CASE, "--reuse-fixture", *flags],
                              cwd=self.root, env=environment, capture_output=True, text=True, timeout=600)
        proof = self.root / "results" / "a-DirectVulkan" / "transport-proof.json"
        return done.returncode, done.stdout, done.stderr, json.loads(proof.read_text()) if proof.exists() else None

    def test_require_monolith_passes_on_a_monolith_run_of_the_split_build(self):
        self.device_has(library_logs("monolith"))
        status, output, error, proof = self.replay("--require-monolith")
        self.assertEqual(status, 0, error)
        self.assertIn(MONOLITH_CONFIRMED, output.splitlines())
        self.assertEqual((proof["required_transport"], proof["passed"]), ("monolith", True))

    def test_require_monolith_passes_on_a_build_without_the_split_transports(self):
        # One role, one log, under the name the role logs are derived from.
        self.device_has({"mobilegl.log": library_logs("monolith")["mobilegl.client.log"]})
        status, output, error, proof = self.replay("--require-monolith")
        self.assertEqual(status, 0, error)
        self.assertIn(MONOLITH_CONFIRMED, output.splitlines())
        self.assertTrue(proof["passed"])

    def test_require_monolith_fails_a_run_that_was_not_monolith(self):
        for transport in ("inproc", "spawn"):
            with self.subTest(transport):
                self.device_has(library_logs(transport))
                status, output, error, proof = self.replay("--require-monolith")
                self.assertEqual(status, 1)
                self.assertNotIn(MONOLITH_CONFIRMED, output)
                self.assertIn("trace replay failed actual monolith/no-split-transport proof", error)
                self.assertIn(f"the library log has the {transport} transport marker", error)
                self.assertFalse(proof["passed"])

    def test_a_passed_monolith_replay_without_a_library_log_stays_passed(self):
        # Whether the device still answers or went away before the log was pulled: a passed
        # replay is not failed or retried for a log that says nothing about the transport.
        for adb_state in ("device", "offline"):
            with self.subTest(adb_state):
                status, output, error, proof = self.replay("--require-monolith", adb_state=adb_state)
                self.assertEqual(status, 0, error)
                self.assertIn(monolith_not_confirmed(NO_LOG), output.splitlines())
                self.assertEqual((proof["required_transport"], proof["passed"], proof["confirmed"]),
                                 ("monolith", True, False))

    def test_a_split_log_lost_with_the_device_asks_for_the_infrastructure_retry(self):
        # The inproc and spawn proofs need the log, so a device that went away before it was
        # pulled is the emulator's failure and not the proof's.
        status, _, error, proof = self.replay("--require-inproc", adb_state="offline")
        self.assertEqual(status, shard.INFRASTRUCTURE_FAILURE_EXIT_CODE, error)
        self.assertIsNone(proof)

    def test_a_failed_replay_of_a_build_without_the_split_transports_keeps_its_library_log(self):
        log = library_logs("monolith")["mobilegl.client.log"]
        self.device_has({"mobilegl.log": log,
                         "result.json": '{"passed": false, "ssim": 0.5, "message": "retrace completed"}\n'})
        status, _, error, proof = self.replay("--require-monolith")
        self.assertEqual(status, 1)
        self.assertIsNone(proof)
        self.assertEqual((self.root / "results" / "a-DirectVulkan" / "mobilegl.log").read_bytes(), log.encode())
        self.assertIn("trace-replay-ci.sh: tail of mobilegl.log:", error)
        self.assertIn(LOADED_LINE, error)

    def shard_through_the_script(self, transport, logs=None):
        """Runs one case through the runner and the real script; returns (exit status, the job log)."""
        self.device_has(library_logs(transport) if logs is None else logs)
        sh, adb = self.environment["FAKE_SH"], self.environment["FAKE_DEVICE_ADB"]
        environment = {**self.environment, "ADB": f'"{sh}" "{adb}"',
                       "MOBILEGL_SHARD_REPLAY_COMMAND": f'"{sh}" "{(self.root / "replay.sh").as_posix()}"',
                       "MOBILEGL_GPU_HANG_BUDGET_MS": "0", "MOBILEGL_SHARD_JOB_ID": "1234"}
        one_case = case("a", golden="fixtures/a.png", timeout_seconds=5)
        done = subprocess.run(
            [sys.executable, str(HERE / "trace_replay_shard.py"), "--apk-file", "trace.apk",
             "--backend", "DirectVulkan", "--transport", transport, "--lane", f"{transport}-lane",
             "--cases-json", json.dumps([one_case]), "--result-root", "results", "--fixture-root", "work"],
            cwd=self.root, env=environment, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=900)
        return done.returncode, done.stdout

    def test_a_shard_reaches_the_proof_of_its_transport_through_the_script(self):
        proofs = {"monolith": MONOLITH_CONFIRMED,
                  "inproc": "Android retrace: real inproc transport, strict role state and zero Fatal records "
                            "confirmed",
                  "spawn": "Android retrace: real spawn transport, server role in pid 2252, and zero Fatal records "
                           "confirmed"}
        marks = {"monolith": LOADED_LINE, "inproc": INPROC_LINE, "spawn": ARMED_LINE}
        for transport, proof in proofs.items():
            with self.subTest(transport):
                shutil.rmtree(self.root / "results", ignore_errors=True)
                for leftover in self.device.glob("mobilegl*"):
                    leftover.unlink()
                status, output = self.shard_through_the_script(transport)
                self.assertEqual(status, 0, output)
                lines = output.splitlines()
                # The script took the runner's arguments, its own proof line is in the log, and
                # the runner quoted the library's lines from the log the script pulled.
                self.assertIn(proof, lines)
                self.assertIn(f"mobilegl.log: {marks[transport]}", said(output, "library-log", "a"))
                self.assertEqual(json.loads(said(output, "record", "a")[0])["status"], "passed")
                self.assertIn("artifacts-copied", json.loads(said(output, "phases", "a")[0]))
                self.assertEqual(lines[-1], "trace-replay-shard: 1 passed, 0 not passed, 1 verdict(s) for 1 case(s)")
                written = json.loads((self.root / "results" / "a-DirectVulkan" / "transport-proof.json").read_text())
                self.assertEqual((written["required_transport"], written["passed"]), (transport, True))

    def test_a_monolith_shard_stays_green_without_a_library_log(self):
        status, output = self.shard_through_the_script("monolith", logs={})
        self.assertEqual(status, 0, output)
        self.assertIn(monolith_not_confirmed(NO_LOG), output.splitlines())
        self.assertEqual(said(output, "library-log", "a"), ["no library log was pulled"])
        self.assertEqual(json.loads(said(output, "record", "a")[0])["status"], "passed")


if __name__ == "__main__":
    unittest.main()
