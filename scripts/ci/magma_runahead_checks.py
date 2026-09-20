#!/usr/bin/env python3
"""Run CTest-discovered Magma negative controls or synchronization validation.

No build, source edit, commit, device operation or synthetic sequence change.
--execute is required because these tests share their CTest private log paths.
Each fresh process uses the discovered argv, ENVIRONMENT and WORKING_DIRECTORY.
Only RUN_AHEAD changes for the negative control (plus a gtest XML output argument).
"""
from __future__ import annotations
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import xml.etree.ElementTree as ET

CASES = (
    "QueuedBufferVersionsAndDeletedNameReuseKeepTheirPixels",
    "QueuedProgramRebindsKeepEachUniformSnapshot",
    "QueuedGpuWritesReadBackTheLastDispatchBytes",
    "MixedNativeAndConvertedGpuVertexStreamsKeepTheirReservation",
    "TextureUploadCannotOvertakeThePreviouslyQueuedDraw",
    "GpuClearSurvivesTheFirstStorageImageUpgrade",
    "PresentCreditActuallyParksTheClientAtItsConfiguredLimit",
)
PREFIX = "DirectVulkan.Split.RunAhead.Credit"
REQUIRED = {f"{PREFIX}{credit}.MagmaRunAheadScenario.{name}" for credit in (1, 3) for name in CASES}
NEGATIVE = f"{PREFIX}1.MagmaRunAheadScenario.QueuedProgramRebindsKeepEachUniformSnapshot"
CLEAR_ASSERTION = "the clear waited for apply instead of returning while its server batch was held"
ERROR = re.compile(r"VUID-[A-Za-z0-9_-]+|SYNC-HAZARD-[A-Z_]+|Validation Error|\bERROR\b", re.I)
LAYER = re.compile(r'Insert instance layer\s+["\']?VK_LAYER_KHRONOS_validation', re.I)


def write_json(path, data):
    path.write_text(json.dumps(data, indent=2, ensure_ascii=False), encoding="utf-8")


def discover(build, out):
    run = subprocess.run(["ctest", "--test-dir", str(build), "-L", "integration-magma-runahead",
                          "--show-only=json-v1"], capture_output=True, text=True, timeout=60)
    (out / "discovery.stderr.txt").write_text(run.stderr, encoding="utf-8")
    if run.returncode:
        raise RuntimeError(f"CTest discovery failed: {run.returncode}")
    (out / "discovery.json").write_text(run.stdout, encoding="utf-8")
    document = json.loads(run.stdout)
    tests = {test["name"]: test for test in document["tests"]}
    if len(document["tests"]) != len(tests) or set(tests) != REQUIRED:
        raise RuntimeError(f"discovery mismatch: missing={sorted(REQUIRED-set(tests))}, extra={sorted(set(tests)-REQUIRED)}")
    return tests


def recipe(test):
    props = {p["name"]: p["value"] for p in test["properties"]}
    if props.get("ENVIRONMENT_MODIFICATION"):
        raise RuntimeError("ENVIRONMENT_MODIFICATION must be interpreted explicitly before using this runner")
    declared = {}
    for entry in props["ENVIRONMENT"]:
        key, value = entry.split("=", 1)
        declared[key] = value
    cwd = Path(props["WORKING_DIRECTORY"]).resolve(strict=True)
    command = list(test["command"])
    if not command or not Path(command[0]).is_file():
        raise RuntimeError("discovered test executable does not exist")
    if declared.get("MOBILEGL_IPC_RUN_AHEAD") != "1":
        raise RuntimeError("the discovered lane no longer declares RUN_AHEAD=1")
    return command, declared, cwd, float(props.get("TIMEOUT", 120))


def run_case(test, build, directory, overrides):
    directory.mkdir()
    command, declared, cwd, timeout = recipe(test)
    env = os.environ.copy()
    env.update(declared)
    env.update(overrides)  # Deliberately AFTER CTest ENVIRONMENT, which otherwise wins.
    private = Path(env["MOBILEGL_LOG_FILE_PATH"])
    private = (cwd / private).resolve() if not private.is_absolute() else private.resolve()
    if not private.is_relative_to(build):
        raise RuntimeError(f"refusing to reset a private log outside the selected build: {private}")
    private.unlink(missing_ok=True)
    xml = directory / "gtest.xml"
    command = [arg for arg in command if not arg.startswith("--gtest_output=")]
    command.append(f"--gtest_output=xml:{xml}")
    visible_env = {key: value for key, value in env.items() if key.startswith(
        ("MOBILEGL_", "MGITEST_", "VK_", "EGL_", "__EGL_", "GTEST_")) or key == "LD_LIBRARY_PATH"}
    write_json(directory / "recipe.json", {
        "ctest_name": test["name"], "command": command, "working_directory": str(cwd),
        "ctest_environment": declared, "overrides": overrides, "effective_graphics_environment": visible_env,
        "private_log": str(private), "timeout_seconds": timeout,
        "executable_sha256": hashlib.sha256(Path(command[0]).read_bytes()).hexdigest(),
    })
    started = time.monotonic()
    with (directory / "stdout.txt").open("w", encoding="utf-8") as output:
        try:
            run = subprocess.run(command, cwd=cwd, env=env, stdout=output, stderr=subprocess.STDOUT,
                                 text=True, timeout=timeout)
            rc = run.returncode
        except subprocess.TimeoutExpired:
            rc = 124
            output.write("\nRUNNER_TIMEOUT: process killed after discovered CTest timeout\n")
    private_text = private.read_text(encoding="utf-8", errors="replace") if private.is_file() else ""
    (directory / "private.log").write_text(private_text, encoding="utf-8")
    result = {"ctest_name": test["name"], "returncode": rc,
              "seconds": round(time.monotonic()-started, 3), "private_log_present": private.is_file()}
    write_json(directory / "process.json", result)
    return result


def inspect(directory):
    root = ET.parse(directory / "gtest.xml").getroot()
    cases = root.findall(".//testcase")
    if len(cases) != 1:
        raise RuntimeError(f"expected exactly one executed testcase, observed {len(cases)}")
    case = cases[0]
    name = f"{case.get('classname')}.{case.get('name')}"
    if name not in {f"MagmaRunAheadScenario.{s}" for s in CASES}:
        raise RuntimeError(f"unexpected testcase {name}")
    if case.find("skipped") is not None or case.get("status") != "run" or case.get("result") != "completed":
        raise RuntimeError("selected testcase skipped or did not execute")
    failed = case.find("failure") is not None or case.find("error") is not None
    stdout = (directory / "stdout.txt").read_text(encoding="utf-8", errors="replace")
    private = (directory / "private.log").read_text(encoding="utf-8", errors="replace")
    return case, failed, stdout, private


def negative(tests, build, out):
    outcomes = []
    # Always restore the green arm even if the red assertion check does not match.
    for arm, value in (("run_ahead_0", "0"), ("restored_run_ahead_1", "1")):
        directory = out / arm
        result = run_case(tests[NEGATIVE], build, directory, {"MOBILEGL_IPC_RUN_AHEAD": value})
        try:
            case, failed, stdout, private = inspect(directory)
            if f"{case.get('classname')}.{case.get('name')}" != "MagmaRunAheadScenario.QueuedProgramRebindsKeepEachUniformSnapshot":
                raise RuntimeError("the named program-rebind case did not run")
            if not result["private_log_present"]:
                raise RuntimeError("missing actual private library log")
            if not re.search(r"Config: IPC [^\n]*run-ahead=" + value + r"\b", private):
                raise RuntimeError("private log does not prove the requested runtime knob")
            if value == "0":
                # Accept either scheduling point, but only from this exact failed
                # testcase's XML failures. A bare ARMED/cap failure is insufficient.
                failure_text = "\n".join(node.get("message", "") + " " + " ".join(node.itertext())
                                         for tag in ("failure", "error") for node in case.findall(tag))
                clear_wait = CLEAR_ASSERTION in failure_text
                queue_wait = ("client waited for apply instead of running ahead" in failure_text and
                              "no real client lead over the apply watermark" in failure_text)
                if result["returncode"] == 0 or not failed or not (clear_wait or queue_wait):
                    raise RuntimeError("negative arm did not fail on real held-apply waiting and lost queue lead")
                result["control_failure"] = "clear_wait" if clear_wait else "client_wait_and_no_apply_lead"
            elif result["returncode"] != 0 or failed:
                raise RuntimeError("restored arm did not pass")
            result["verdict"] = "EXPECTED_RED" if value == "0" else "GREEN"
        except (RuntimeError, OSError, ET.ParseError) as exc:
            result.update(verdict="FAILED_CHECK", error=str(exc))
        outcomes.append(result)
        write_json(out / "summary.json", outcomes)
        print(f"{arm}: process_rc={result['returncode']} verdict={result['verdict']}", flush=True)
    return 0 if [r["verdict"] for r in outcomes] == ["EXPECTED_RED", "GREEN"] else 1


def validation(tests, build, out):
    outcomes = []
    junit = ET.Element("testsuite", name="MagmaRunAheadSynchronizationValidation")
    settings = {
        "VK_INSTANCE_LAYERS": "VK_LAYER_KHRONOS_validation",
        "VK_LAYER_ENABLES": "VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT",
        # Loader diagnostics prove the layer was inserted, rather than merely installed.
        "VK_LOADER_DEBUG": "layer",
    }
    for name in sorted(tests):
        directory = out / name
        result = run_case(tests[name], build, directory, settings)
        copied = None
        try:
            case, failed, stdout, private = inspect(directory)
            if f"{case.get('classname')}.{case.get('name')}" != name.split(".", 4)[-1]:
                raise RuntimeError("executed testcase does not match its discovered entry")
            copied = copy.deepcopy(case)
            copied.set("name", name)  # Raw gtest.xml remains unchanged beside this aggregate.
            junit.append(copied)
            lines = [line for line in (stdout + "\n" + private).splitlines() if ERROR.search(line)]
            (directory / "validation-errors.txt").write_text("\n".join(lines), encoding="utf-8")
            result.update(validation_error_lines=len(lines), layer_inserted=bool(LAYER.search(stdout + private)))
            if result["returncode"] or failed or not result["private_log_present"]:
                raise RuntimeError("GPU case did not complete with a passing result and actual private log")
            if not result["layer_inserted"]:
                raise RuntimeError("loader output does not prove validation-layer insertion")
            if lines:
                raise RuntimeError("VUID / synchronization hazard / error output found; see validation-errors.txt")
            result["verdict"] = "GREEN"
        except (RuntimeError, OSError, ET.ParseError) as exc:
            result.update(verdict="FAILED_CHECK", error=str(exc))
            if copied is None:
                copied = ET.SubElement(junit, "testcase", name=name)
            ET.SubElement(copied, "failure", message=str(exc))
        outcomes.append(result)
        junit.set("tests", str(len(outcomes)))
        junit.set("failures", str(sum(r["verdict"] != "GREEN" for r in outcomes)))
        write_json(out / "summary.json", outcomes)
        ET.ElementTree(junit).write(out / "aggregate-gtest.xml", encoding="utf-8", xml_declaration=True)
        print(f"{name}: process_rc={result['returncode']} verdict={result['verdict']}", flush=True)
    return 0 if len(outcomes) == 14 and all(r["verdict"] == "GREEN" for r in outcomes) else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("negative", "validation"))
    parser.add_argument("--build-dir", type=Path, default=Path("build-split"))
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--execute", action="store_true", help="execute serially with other GPU lanes that share private logs")
    args = parser.parse_args()
    if not args.execute:
        print("Add --execute with other GPU lanes idle; no discovery or tests were run")
        return 0
    build = args.build_dir.resolve(strict=True)
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    try:
        tests = discover(build, out)
        return negative(tests, build, out) if args.mode == "negative" else validation(tests, build, out)
    except (RuntimeError, OSError, ValueError, subprocess.TimeoutExpired) as exc:
        (out / "runner-error.txt").write_text(str(exc), encoding="utf-8")
        print(f"FAILED: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
