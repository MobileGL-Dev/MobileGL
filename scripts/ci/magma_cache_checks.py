#!/usr/bin/env python3
"""Run exactly seven CTest-discovered Magma cache cases under Vulkan sync validation.

Preparation is inert: --execute is required before discovery, log deletion or tests.
Run only while other host GPU lanes that share these private logs are idle.
This script does not build, modify source, or operate a device.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import xml.etree.ElementTree as ET


CASES = (
    "SamplerSwizzleAtoBtoAKeepsEachQueuedPixel",
    "SamplerRespecifyKeepsOldAndNewNativeImages",
    "SamplerViewsDistinguishMipLayerAndViewType",
    "FramebuffersAtoBtoAKeepDistinctAttachments",
    "FramebufferReattachmentDistinguishesMipAndLayer",
    "FramebufferSrgbPolicyAtoBtoAUsesDistinctFormats",
    "FramebufferRespecifyRebindsTheNewNativeImage",
)
PREFIX = "DirectVulkan.Split.Caches."
REQUIRED = {PREFIX + "MagmaWireCacheScenario." + name for name in CASES}
SETTINGS = {
    "VK_INSTANCE_LAYERS": "VK_LAYER_KHRONOS_validation",
    "VK_LAYER_ENABLES": "VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT",
    "VK_LOADER_DEBUG": "layer",
}
REQUIRED_ENV = {
    "MOBILEGL_BACKEND_TYPE": "DirectVulkan",
    "MOBILEGL_TRANSPORT": "inproc",
    "MOBILEGL_IPC_ROLE_SPLIT_STATE": "1",
    "MOBILEGL_IPC_STRICT_ERRORS": "1",
    "MOBILEGL_IPC_RUN_AHEAD": "1",
    "MOBILEGL_IPC_PRESENT_CREDIT": "3",
    "MGITEST_MAGMA_CACHES_LANE": "1",
    "MGITEST_SPLIT_LANE": "1",
    "MOBILEGL_ITEST_REQUIRE_GPU": "1",
}


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding="utf-8")


def load_checker(source):
    path = source / "scripts/ci/magma_runahead_checks.py"
    spec = importlib.util.spec_from_file_location("magma_shared_checker", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot import repository checker: {path}")
    checker = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(checker)
    return checker, path


def discover(build, out, checker):
    result = subprocess.run(
        ["ctest", "--test-dir", str(build), "-L", "^integration-magma-caches$",
         "--show-only=json-v1"], capture_output=True, text=True, timeout=60)
    (out / "discovery.json").write_text(result.stdout, encoding="utf-8")
    (out / "discovery.stderr.txt").write_text(result.stderr, encoding="utf-8")
    if result.returncode:
        raise RuntimeError(f"CTest discovery returned {result.returncode}")
    rows = json.loads(result.stdout)["tests"]
    tests = {test["name"]: test for test in rows}
    if len(rows) != 7 or len(tests) != 7 or set(tests) != REQUIRED:
        raise RuntimeError(f"exact-seven discovery mismatch: missing={sorted(REQUIRED-set(tests))}, "
                           f"extra={sorted(set(tests)-REQUIRED)}, rows={len(rows)}")
    executables = {}
    private_logs = set()
    for name, test in tests.items():
        command, declared, cwd, _ = checker.recipe(test)
        wrong = {key: declared.get(key) for key, expected in REQUIRED_ENV.items()
                 if declared.get(key) != expected}
        if wrong:
            raise RuntimeError(f"{name}: required strict cache environment changed: {wrong}")
        private = Path(declared["MOBILEGL_LOG_FILE_PATH"])
        private = (cwd / private).resolve() if not private.is_absolute() else private.resolve()
        if not private.is_relative_to(build) or private in private_logs:
            raise RuntimeError(f"private library log is not unique and inside the build: {private}")
        private_logs.add(private)
        executable = Path(command[0]).resolve(strict=True)
        executables[str(executable)] = sha256(executable)
    if len(executables) != 1:
        raise RuntimeError(f"cache cases do not share one executable: {list(executables)}")
    return tests, executables


def inspect_exact_case(directory, expected):
    # The repository's inspect() intentionally admits only its run-ahead cases.
    # Preserve the same no-skip/execution checks with this lane's exact identity.
    document = ET.parse(directory / "gtest.xml").getroot()
    cases = document.findall(".//testcase")
    if len(cases) != 1:
        raise RuntimeError(f"expected exactly one testcase, found {len(cases)}")
    case = cases[0]
    actual = f"{case.get('classname')}.{case.get('name')}"
    if actual != expected.removeprefix(PREFIX):
        raise RuntimeError(f"executed testcase {actual!r} does not match {expected!r}")
    return case


def validate(tests, build, out, checker, executable_hashes):
    outcomes = []
    junit = ET.Element("testsuite", name="MagmaWireCacheSynchronizationValidation")
    for name in sorted(REQUIRED):
        directory = out / name
        result = {"ctest_name": name, "verdict": "FAILED_CHECK"}
        copied = None
        try:
            # Shared code uses the discovered argv/ENVIRONMENT/WORKING_DIRECTORY,
            # applies overrides AFTER CTest's values, and saves executable SHA,
            # effective recipe, raw XML, merged stdout/stderr, and private log.
            result.update(checker.run_case(tests[name], build, directory, SETTINGS))
            stdout = (directory / "stdout.txt").read_text(encoding="utf-8", errors="replace")
            private = (directory / "private.log").read_text(encoding="utf-8", errors="replace")
            validation, application = checker.split_diagnostics(stdout, private)
            (directory / "validation-errors.txt").write_text("\n".join(validation), encoding="utf-8")
            (directory / "application-errors.txt").write_text("\n".join(application), encoding="utf-8")
            result.update(validation_error_lines=len(validation), application_error_lines=len(application),
                          layer_inserted=bool(checker.LAYER.search(stdout + "\n" + private)))
            # Application ERROR diagnostics (including the existing stencil caps
            # messages) are retained separately by the repository classifier.
            # Embedded validation/VUID/SYNC-HAZARD and loader ERROR remain fatal.
            case = inspect_exact_case(directory, name)
            copied = copy.deepcopy(case)
            copied.set("name", name)
            junit.append(copied)
            skipped = (case.find("skipped") is not None or case.get("status") != "run" or
                       case.get("result") != "completed")
            result["testcase_completed_without_skip"] = not skipped
            if skipped:
                raise RuntimeError("selected cache testcase skipped or did not complete")
            if result["returncode"] or case.find("failure") is not None or case.find("error") is not None:
                raise RuntimeError("cache testcase did not pass with process exit zero")
            if not result["private_log_present"]:
                raise RuntimeError("actual private library log is missing")
            if not result["layer_inserted"]:
                raise RuntimeError("loader output does not prove Khronos validation-layer insertion")
            if validation:
                raise RuntimeError("VUID / synchronization hazard / validation or loader error found")
            recipe = json.loads((directory / "recipe.json").read_text(encoding="utf-8"))
            executable = str(Path(recipe["command"][0]).resolve(strict=True))
            if recipe["executable_sha256"] != executable_hashes[executable]:
                raise RuntimeError("test executable changed since discovery")
            result["verdict"] = "GREEN"
        except (RuntimeError, OSError, ValueError, KeyError, ET.ParseError) as exc:
            result.update(verdict="FAILED_CHECK", error=str(exc))
            if copied is None:
                copied = ET.SubElement(junit, "testcase", name=name)
            ET.SubElement(copied, "failure", message=str(exc))
        outcomes.append(result)
        junit.set("tests", str(len(outcomes)))
        junit.set("failures", str(sum(row["verdict"] != "GREEN" for row in outcomes)))
        junit.set("skipped", str(sum(case.find("skipped") is not None for case in junit)))
        write_json(out / "summary.json", outcomes)
        ET.ElementTree(junit).write(out / "aggregate-gtest.xml", encoding="utf-8", xml_declaration=True)
        print(f"{name}: process_rc={result.get('returncode')} verdict={result['verdict']}", flush=True)
    unchanged = all(sha256(Path(path)) == digest for path, digest in executable_hashes.items())
    complete = len(outcomes) == 7 and {row["ctest_name"] for row in outcomes} == REQUIRED
    green = complete and unchanged and all(row["verdict"] == "GREEN" for row in outcomes)
    write_json(out / "totals.json", {
        "verdict": "GREEN" if green else "FAILED_CHECK", "complete_exact_seven": complete,
        "passed": sum(row["verdict"] == "GREEN" for row in outcomes),
        "failed": sum(row["verdict"] != "GREEN" for row in outcomes),
        "layer_inserted_cases": sum(row.get("layer_inserted", False) for row in outcomes),
        "validation_error_lines": sum(row.get("validation_error_lines", 0) for row in outcomes),
        "application_error_lines": sum(row.get("application_error_lines", 0) for row in outcomes),
        "executables_unchanged_after_all_cases": unchanged,
    })
    return 0 if green else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--build-dir", type=Path, help="defaults to source-root/build-split")
    parser.add_argument("--out", type=Path, required=True, help="new evidence directory; existing paths are refused")
    parser.add_argument("--execute", action="store_true", help="run serially after other GPU lanes are idle")
    args = parser.parse_args()
    if not args.execute:
        print("Prepared only. Add --execute after other host GPU lanes are idle; nothing was discovered or run.")
        return 0
    source = args.source_root.resolve(strict=True)
    build = (args.build_dir or source / "build-split").resolve(strict=True)
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    try:
        checker, checker_path = load_checker(source)
        tests, executable_hashes = discover(build, out, checker)
        head = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
        write_json(out / "runner-identity.json", {
            "source_root": str(source), "source_head_at_start": head, "build_dir": str(build),
            "shared_checker": str(checker_path), "shared_checker_sha256": sha256(checker_path),
            "runner": str(Path(__file__).resolve()), "runner_sha256": sha256(Path(__file__)),
            "executable_sha256_by_path": executable_hashes, "validation_overrides": SETTINGS,
            "required_cases": sorted(REQUIRED),
        })
        return validate(tests, build, out, checker, executable_hashes)
    except (RuntimeError, OSError, ValueError, KeyError, ET.ParseError, subprocess.TimeoutExpired,
            subprocess.CalledProcessError) as exc:
        (out / "runner-error.txt").write_text(str(exc), encoding="utf-8")
        print(f"FAILED: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
