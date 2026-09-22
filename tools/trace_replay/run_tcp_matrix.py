#!/usr/bin/env python3
"""Run manifest-selected TCP retraces from a CTest JSON catalog, with resumable evidence.

Requires Linux/WSL process groups. The old local CTest/manifest TIMEOUT is not a
TCP deadline: log inactivity and an explicit absolute ceiling are separate.
"""
from __future__ import annotations
import argparse
import contextlib
import ctypes
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path
import re
import signal
import shutil
import subprocess
import sys
import time
import uuid

from trace_cases import load_trace_case_manifest, ci_backends

ROOT = Path(__file__).resolve().parents[2]
OWNED_ENV = {"MOBILEGL_TRANSPORT", "MOBILEGL_IPC_CONTROL", "MOBILEGL_IPC_DATA",
             "MOBILEGL_IPC_TOKEN", "MOBILEGL_IPC_DIAL", "MOBILEGL_IPC_ROLE",
             "MOBILEGL_IPC_SERVER_PATH", "MOBILEGL_LOG_FILE_PATH"}


def read_json(path):
    def invalid(value):
        raise ValueError("non-finite JSON number: " + value)
    return json.loads(Path(path).read_text(encoding="utf-8"), parse_constant=invalid)


def digest(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def fingerprint(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def atomic_json(path, value):
    temporary = path.with_name(path.name + "." + uuid.uuid4().hex + ".tmp")
    try:
        with temporary.open("w", encoding="utf-8") as stream:
            json.dump(value, stream, ensure_ascii=False, indent=2, allow_nan=False)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def select_cases(manifest_path, requested):
    manifest = load_trace_case_manifest(manifest_path)
    raw = read_json(manifest_path)
    explicit = {case["name"]: case.get("split", raw.get("defaults", {}).get("split"))
                for case in raw["cases"]}
    cases = {case["name"]: case for case in manifest["cases"]}
    names = requested or [case["name"] for case in cases.values() if case["split"]]
    if len(names) != len(set(names)):
        raise ValueError("duplicate --case")
    selected = []
    for name in names:
        if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*", name) or name not in cases:
            raise ValueError("unknown/unsafe case: " + name)
        case = cases[name]
        # An explicit non-CI workload, such as rd12, is useful for extra golden/perf runs.
        # An explicit split:false remains a refusal, including in this explicit selection.
        if explicit[name] is False or (not case["split"] and case.get("ci", True)):
            raise ValueError("case opts out of split: " + name)
        selected.append(case)
    if not selected:
        raise ValueError("the manifest selected no split cases")
    return selected


def properties(test):
    return {entry["name"]: entry["value"] for entry in test.get("properties", [])}


def case_environment(test, base, args, output=None):
    env = dict(base)  # A fresh copy for every case; never leak the previous case's quirks.
    for pair in properties(test).get("ENVIRONMENT", []):
        key, sep, value = pair.partition("=")
        if not sep:
            raise ValueError("malformed CTest environment entry: " + pair)
        if key not in OWNED_ENV:
            env[key] = value
    for key in OWNED_ENV:
        env.pop(key, None)
    env.update(MOBILEGL_TRANSPORT="spawn", MOBILEGL_IPC_CONTROL=args.endpoint,
               MOBILEGL_IPC_DATA="stream", MOBILEGL_IPC_TOKEN=args.token,
               MOBILEGL_IPC_REQUIRE_SAME_BUILD="1", MOBILEGL_IPC_LOG_FORWARD="1",
               MOBILEGL_PIPE_STATS="1", MOBILEGL_PIPE_STATS_PERIOD="1",
               MOBILEGL_IPC_PRESENT_CREDIT=str(args.credit))
    if getattr(args, "require_run_ahead", False):
        env["MOBILEGL_IPC_RUN_AHEAD"] = "1"
        env["MOBILEGL_IPC_VERB_BARRIER"] = "1"
    if output is not None:
        env["MOBILEGL_LOG_FILE_PATH"] = str(output / "output/mobilegl.log")
    return env


def definitions(command):
    result = {}
    for arg in command:
        if arg.startswith("-D") and "=" in arg:
            key, value = arg[2:].split("=", 1)
            result[key.split(":", 1)[0]] = value
    return result


def plan_case(case, backend, catalog, args, hashes):
    name = f"MobileGLTraceReplay.{case['name']}.{backend}"
    test = next((catalog[key] for key in (name, name + ".TCP", name + ".SPAWN", name + ".SPLIT")
                 if key in catalog), None)
    if test is None:
        raise ValueError("CTest catalog has no entry for " + name)
    command = list(test["command"])
    defs = definitions(command)
    if defs.get("TRACE_CASE_NAME") != case["name"] or defs.get("TRACE_BACKEND") != backend:
        raise ValueError("CTest name and command disagree: " + name)
    if "-P" not in command or not defs.get("TRACE_ARCHIVE"):
        raise ValueError("expected a run_trace_case.cmake catalog entry: " + name)
    fixtures = args.fixtures or Path(defs["TRACE_ARCHIVE"]).parent
    runner = (args.runner or Path(defs["TRACE_REPLAY_EXE"])).resolve()
    script = args.source / "tools/trace_replay/run_trace_case.cmake"
    values = {"TRACE_REPLAY_EXE": str(runner), "MOBILEGL_LIBRARY": str(args.library),
              "TRACE_CASE_NAME": case["name"], "TRACE_BACKEND": backend,
              "TRACE_TRANSPORT": "spawn", "TRACE_IPC_SERVER_PATH": "",
              "TRACE_IPC_CONTROL": "", "TRACE_IPC_TOKEN": ""}
    # Read values from the CURRENT manifest, not stale -D values in the catalog.
    keys = {"TRACE_FILE": "trace_file", "TRACE_TARGET_CALL": "target_call", "TRACE_WIDTH": "width",
            "TRACE_HEIGHT": "height", "TRACE_SSIM_THRESHOLD": "ssim_threshold", "TRACE_CROP_X": "crop_x",
            "TRACE_CROP_Y": "crop_y", "TRACE_CROP_WIDTH": "crop_width", "TRACE_CROP_HEIGHT": "crop_height",
            "TRACE_COHERENT_AS_FLUSH": "coherent_as_flush"}
    for key, field in keys.items():
        value = case.get(field, False if field == "coherent_as_flush" else 0.99 if field == "ssim_threshold" else 0)
        values[key] = ("ON" if value else "OFF") if isinstance(value, bool) else str(value)
    inputs = []
    for key, field in (("TRACE_ARCHIVE", "trace_archive"), ("TRACE_GOLDEN", "golden"),
                       ("TRACE_ALTERNATE_GOLDEN", "alternate_golden")):
        path = (fixtures / case[field]).resolve() if case.get(field) else None
        values[key] = str(path) if path else ""
        if path:
            stat = path.stat()
            inputs.append({"path": str(path), "size": stat.st_size, "mtime_ns": stat.st_mtime_ns,
                           **({"sha256": digest(path)} if key != "TRACE_ARCHIVE" else {})})
    trace_file = Path(case["trace_file"])
    if trace_file.is_absolute() or ".." in trace_file.parts:
        raise ValueError("trace_file must remain inside the extracted input directory")
    for path in (runner, args.library, script):
        if str(path) not in hashes:
            hashes[str(path)] = digest(path)
    env = case_environment(test, args.base_env, args)
    # Ignore shell/session bookkeeping when resuming, but retain renderer knobs and
    # every case-specific catalog variable. The digest stores no token/plaintext env.
    case_keys = {pair.partition("=")[0] for pair in properties(test).get("ENVIRONMENT", [])}
    relevant = {key: value for key, value in env.items() if key in case_keys or
                key.startswith(("MOBILEGL_", "MGITEST_", "EGL_", "LIBGL_", "MESA_", "VK_", "__GL", "__EGL", "LD_")) or
                key in ("DISPLAY", "WAYLAND_DISPLAY", "XDG_RUNTIME_DIR", "DRI_PRIME", "GALLIUM_DRIVER")}
    identity = fingerprint({"case": case, "backend": backend, "credit": args.credit,
                            "endpoint": args.endpoint, "token": args.token,
                            "require_run_ahead": args.require_run_ahead,
                            "artifacts": {str(p): hashes[str(p)] for p in (runner, args.library, script)},
                            "inputs": inputs, "environment": relevant, "command": command})
    return {"case": case, "backend": backend, "test": test, "command": command, "values": values,
            "script": script, "runner": runner, "identity": identity,
            "key": f"{backend}/credit-{args.credit}/{case['name']}"}


def command_for(plan, work):
    values = {**plan["values"], "TRACE_OUTPUT_DIR": str(work), "TRACE_ARTIFACT_DIR": str(work.parent / "artifacts")}
    command, found = [], set()
    original = plan["command"]
    for index, arg in enumerate(original):
        if index and original[index - 1] == "-P":
            command.append(str(plan["script"]))
        elif arg.startswith("-D") and arg[2:].split("=", 1)[0].split(":", 1)[0] in values:
            key = arg[2:].split("=", 1)[0].split(":", 1)[0]
            command.append(f"-D{key}={values[key]}")
            found.add(key)
        else:
            command.append(arg)
    position = command.index("-P")
    command[position:position] = [f"-D{k}={v}" for k, v in values.items() if k not in found]
    return command


def group_members(pgid):
    members = []
    for path in Path("/proc").glob("[0-9]*/stat"):
        try:
            fields = path.read_text().rsplit(")", 1)[1].split()
            if int(fields[2]) == pgid:
                members.append((int(path.parent.name), fields[0]))
        except (OSError, ValueError, IndexError):
            pass
    return members


def reap_group(pgid):
    while True:
        try:
            pid, _ = os.waitpid(-pgid, os.WNOHANG)
            if pid == 0:
                break
        except ChildProcessError:
            break


@contextlib.contextmanager
def subreaper():
    libc = ctypes.CDLL(None, use_errno=True)
    previous = ctypes.c_int()
    if libc.prctl(37, ctypes.byref(previous), 0, 0, 0) or libc.prctl(36, 1, 0, 0, 0):
        raise OSError(ctypes.get_errno(), "cannot enable Linux child subreaper")
    try:
        yield
    finally:
        libc.prctl(36, previous.value, 0, 0, 0)


def terminate_group(process, grace=2.0):
    """Reap descendants even when the wrapper exits before a TERM-ignoring child."""
    pgid = process.pid
    for sig, duration in ((signal.SIGTERM, grace), (signal.SIGKILL, 2.0)):
        try:
            os.killpg(pgid, sig)
        except ProcessLookupError:
            pass
        until = time.monotonic() + duration
        while time.monotonic() < until:
            process.poll()  # Let Popen reap its own child before reaping adopted descendants.
            if process.returncode is not None:
                reap_group(pgid)
            if not group_members(pgid):
                return []
            time.sleep(.02)
    process.wait(timeout=2)
    reap_group(pgid)
    return group_members(pgid)


def progress_signature(work, console):
    signature = []
    for path in [console, *work.rglob("*.log")]:
        try:
            stat = path.stat()
            signature.append((str(path), stat.st_size, stat.st_mtime_ns))
        except FileNotFoundError:
            pass
    return tuple(sorted(signature))


def supervise(command, env, cwd, work, console, idle_seconds=300, max_seconds=7200,
              poll_seconds=.2, terminate_grace=2.0, wake=None):
    started = time.monotonic()
    last_progress = started
    previous = progress_signature(work, console)
    timeout_kind = None
    next_wake = started
    with subreaper(), console.open("wb") as output:
        process = subprocess.Popen(command, env=env, cwd=cwd, stdout=output, stderr=output,
                                   start_new_session=True)
        try:
            while process.poll() is None:
                now = time.monotonic()
                if wake is not None and now >= next_wake:
                    wake()
                    next_wake = time.monotonic() + 15
                signature = progress_signature(work, console)
                if signature != previous:
                    previous, last_progress = signature, time.monotonic()
                now = time.monotonic()
                if now - started >= max_seconds:
                    timeout_kind = "absolute"
                    break
                if now - last_progress >= idle_seconds:
                    timeout_kind = "idle"
                    break
                time.sleep(poll_seconds)
            leftovers = group_members(process.pid) if process.poll() is not None else []
            returncode = process.returncode
        finally:
            remaining = terminate_group(process, terminate_grace)
    return {"returncode": 124 if timeout_kind else returncode, "timeout_kind": timeout_kind,
            "seconds": round(time.monotonic() - started, 3), "pid": process.pid,
            "leftovers_after_parent_exit": [pid for pid, state in leftovers if state != "Z"],
            "remaining_processes": remaining}


def actual_arm_from_text(client_text):
    return {"armed": "run-ahead ARMED" in client_text,
            "lockstep": "running lockstep" in client_text,
            "disarmed": bool(re.search(r"\bDISARMED\b", client_text))}


def validate_result(plan, work, endpoint, require_run_ahead=False):
    path = work / "output/result.json"
    result = read_json(path)
    if result.get("passed") is not True or result.get("statusCode") != 0:
        raise ValueError("result.json does not report success")
    if result.get("backend") != plan["backend"] or result.get("targetCall") != int(plan["values"]["TRACE_TARGET_CALL"]):
        raise ValueError("result identifies a different backend/call")
    expected_trace = (work / "input" / plan["case"]["trace_file"]).resolve()
    if Path(result.get("tracePath", "")).resolve() != expected_trace:
        raise ValueError("result tracePath belongs to a different attempt")
    actual = work / "output/actual.png"
    if Path(result.get("actualPath", "")).resolve() != actual.resolve() or not actual.is_file():
        raise ValueError("result has no current-attempt actual image")
    goldens = {str(Path(plan["values"][key]).resolve()) for key in ("TRACE_GOLDEN", "TRACE_ALTERNATE_GOLDEN") if plan["values"][key]}
    if str(Path(result.get("matchedGoldenPath", "")).resolve()) not in goldens:
        raise ValueError("result matched a different golden")
    score = result.get("ssim")
    if isinstance(score, bool) or not isinstance(score, (float, int)) or not math.isfinite(score) or score < float(plan["values"]["TRACE_SSIM_THRESHOLD"]):
        raise ValueError("result SSIM does not meet the current manifest threshold")
    client_log = work / "output/mobilegl.client.log"
    client_text = client_log.read_text(errors="replace")
    marker = re.compile(r"control=tcp data=stream server=(\S+) pid=([1-9][0-9]*)")
    if not any(match[0] == endpoint.removeprefix("tcp://") for match in marker.findall(client_text)):
        raise ValueError("current-attempt log lacks the requested TCP arm proof")
    actual_arm = actual_arm_from_text(client_text)
    if require_run_ahead and (not actual_arm["armed"] or actual_arm["lockstep"] or actual_arm["disarmed"]):
        raise ValueError("required run-ahead arm missing or contradicted in private client log: " + str(actual_arm))
    return {"result": result, "result_path": str(path), "result_sha256": digest(path),
            "client_log": str(client_log), "client_log_sha256": digest(client_log), "actual_arm": actual_arm}


def reusable(row, plan, out, endpoint, require_run_ahead=False):
    if row is None or row.get("status") != "passed" or row.get("returncode") != 0 or row.get("timeout_kind") is not None or row.get("identity") != plan["identity"]:
        return False
    if require_run_ahead and row.get("require_run_ahead") is not True:
        return False
    try:
        work = Path(row["work"])
        if not work.resolve().is_relative_to(out.resolve()):
            return False
        proof = validate_result(plan, work, endpoint, require_run_ahead)
        return all(proof[key] == row.get(key) for key in ("result_sha256", "client_log_sha256", "actual_arm"))
    except (OSError, ValueError, KeyError):
        return False


def run_case(plan, args, row):
    work = Path(row["work"])
    console = work.parent / "runner.log"
    env = case_environment(plan["test"], args.base_env, args, work)
    row["requested_arm"] = {key: env.get(key) for key in ("MOBILEGL_IPC_RUN_AHEAD", "MOBILEGL_IPC_VERB_BARRIER")}
    props = properties(plan["test"])
    cwd = props.get("WORKING_DIRECTORY", str(args.source))
    def wake():
        with (work.parent / "wake-adb.log").open("ab") as log:
            try:
                subprocess.run([args.adb, "-s", args.wake_adb_serial, "shell", "input", "keyevent", "KEYCODE_WAKEUP"],
                               stdout=log, stderr=log, timeout=5, check=False)
            except (OSError, subprocess.TimeoutExpired) as error:
                log.write((str(error) + "\n").encode())
    outcome = supervise(command_for(plan, work), env, cwd, work, console, args.idle_seconds,
                        args.max_seconds, wake=wake if args.wake_adb_serial else None)
    row.update(outcome, status="failed", console=str(console))
    client_log = work / "output/mobilegl.client.log"
    row["actual_arm"] = actual_arm_from_text(client_log.read_text(errors="replace")) if client_log.is_file() else None
    if outcome["timeout_kind"]:
        row["status"] = "timeout"
    elif outcome["returncode"] == 0 and not outcome["remaining_processes"] and not outcome["leftovers_after_parent_exit"]:
        try:
            row.update(validate_result(plan, work, args.endpoint, args.require_run_ahead), status="passed")
        except (OSError, ValueError, KeyError) as error:
            row["error"] = str(error)
    if row["status"] != "passed" and row["returncode"] == 0:
        row["process_returncode"] = 0
        row["returncode"] = 1  # A zero wrapper exit cannot turn stale/missing proof green.
    return row


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    catalog_source = parser.add_mutually_exclusive_group(required=True)
    catalog_source.add_argument("--catalog", type=Path, help="ctest --show-only=json-v1 output")
    catalog_source.add_argument("--build-dir", type=Path, help="read a catalog using ctest; does not build")
    parser.add_argument("--source", type=Path, default=ROOT)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--fixtures", type=Path, help="default: the fixture directory recorded in the catalog")
    parser.add_argument("--runner", type=Path, help="override TRACE_REPLAY_EXE from the catalog")
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--endpoint", required=True)
    parser.add_argument("--token", default=os.environ.get("MOBILEGL_IPC_TOKEN", ""))
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--case", action="append")
    parser.add_argument("--backend", action="append", choices=("DirectGLES", "DirectVulkan"))
    parser.add_argument("--credit", type=int, choices=(1, 2, 3), default=2)
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--require-run-ahead", action="store_true",
                        help="require private-client ARMED proof, no lockstep fallback/DISARMED; force RUN_AHEAD=1 and VERB_BARRIER=1")
    parser.add_argument("--idle-seconds", type=float, default=300)
    parser.add_argument("--max-seconds", type=float, default=7200)
    parser.add_argument("--wake-adb-serial", help="explicit opt-in: KEYCODE_WAKEUP every 15 seconds while a case runs")
    parser.add_argument("--adb", default="adb")
    args = parser.parse_args(argv)
    if sys.platform != "linux":
        parser.error("run this process-group driver in Linux/WSL")
    if not args.endpoint.startswith("tcp://") or any(not math.isfinite(value) or value <= 0
                                                     for value in (args.idle_seconds, args.max_seconds)):
        parser.error("a tcp:// endpoint and positive timeouts are required")
    args.source, args.library, args.out = args.source.resolve(), args.library.resolve(), args.out.resolve()
    args.base_env = dict(os.environ)
    args.manifest = args.manifest or args.source / "tools/trace_replay/trace_cases.json"
    backends = args.backend or ["DirectGLES"]
    if len(backends) != len(set(backends)):
        parser.error("duplicate --backend")
    try:
        cases = select_cases(args.manifest, args.case)
        document = read_json(args.catalog) if args.catalog else json.loads(subprocess.check_output(
            ["ctest", "--test-dir", str(args.build_dir), "--show-only=json-v1"], text=True))
        catalog = {test["name"]: test for test in document["tests"]}
        hashes = {}
        plans = [plan_case(case, backend, catalog, args, hashes) for case in cases
                 for backend in backends if backend in ci_backends(case)]
        if not plans:
            raise ValueError("no selected case supports the requested backend")
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        parser.error(str(error))
    args.out.mkdir(parents=True, exist_ok=True)
    checkpoint_path = args.out / "checkpoint.json"
    with (args.out / ".matrix.lock").open("w") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            parser.error("another matrix driver owns this output directory")
        if not checkpoint_path.exists() and (args.out / "results.json").is_file():
            legacy = args.out / ("results.legacy-" + uuid.uuid4().hex + ".json")
            shutil.copy2(args.out / "results.json", legacy)
            print(json.dumps({"legacy_results_preserved": str(legacy)}), flush=True)
        checkpoint = read_json(checkpoint_path) if checkpoint_path.exists() else {"schema_version": 1, "runs": []}
        if checkpoint.get("schema_version") != 1 or not isinstance(checkpoint.get("runs"), list):
            parser.error("unsupported/corrupt checkpoint; old scratch results are not a checkpoint")
        selected = []
        def interrupted(signum, frame):
            raise KeyboardInterrupt
        handlers = {sig: signal.signal(sig, interrupted) for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)}
        try:
            for plan in plans:
                prior = next((r for r in reversed(checkpoint["runs"]) if r.get("key") == plan["key"]), None)
                if args.resume and reusable(prior, plan, args.out, args.endpoint, args.require_run_ahead):
                    selected.append({**prior, "resumed": True})
                    print(json.dumps({"case": prior["case"], "backend": prior["backend"], "status": "passed", "resumed": True}), flush=True)
                    continue
                attempt = args.out / plan["key"] / ("attempt-" + uuid.uuid4().hex)
                attempt.mkdir(parents=True)
                row = {"key": plan["key"], "case": plan["case"]["name"], "backend": plan["backend"],
                       "credit": args.credit, "ci": plan["case"].get("ci", True), "identity": plan["identity"],
                       "require_run_ahead": args.require_run_ahead,
                       "work": str(attempt / "work"), "status": "running", "started_at_ns": time.time_ns()}
                checkpoint["runs"].append(row)
                atomic_json(checkpoint_path, checkpoint)
                print(json.dumps({"case": row["case"], "backend": row["backend"], "status": "running"}), flush=True)
                try:
                    run_case(plan, args, row)
                except KeyboardInterrupt:
                    row.update(status="cancelled", returncode=130)
                    atomic_json(checkpoint_path, checkpoint)
                    return 130
                except Exception as error:
                    row.update(status="failed", returncode=1, error=str(error))
                row["completed_at_ns"] = time.time_ns()
                atomic_json(checkpoint_path, checkpoint)
                selected.append({**row, "resumed": False})
                atomic_json(args.out / "results.json", selected)
                print(json.dumps({k: row.get(k) for k in ("case", "backend", "status", "returncode", "seconds", "timeout_kind", "error")}), flush=True)
                if row["status"] == "timeout" or row.get("remaining_processes"):
                    # Do not convert an unhealthy peer into a cascade of Busy outcomes.
                    # The failed checkpoint is retained; resume after checking the peer.
                    break
                time.sleep(.3)
        finally:
            for sig, handler in handlers.items():
                signal.signal(sig, handler)
        atomic_json(args.out / "results.json", selected)
    return 0 if all(row["status"] == "passed" for row in selected) else 1


if __name__ == "__main__":
    raise SystemExit(main())
