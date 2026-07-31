#!/usr/bin/env python
"""Run a local Windows glcts executable and resume across process failures.

The desktop CTS normally runs a complete caselist in one process.  That is a
poor fit for testing a developing OpenGL implementation: one access violation
or GPU hang prevents every later case from running.  This driver gives each
invocation the cases which have not produced a result yet, preserves one QPA
and stdout/stderr pair per invocation, and starts another process after a
crash.

Existing ``chunkNNNN.qpa`` files and ``crashed.txt``/``hung.txt`` sidecars are
read on startup, so invoking the same command and output directory resumes an
interrupted run.  A timeout is based on *idle QPA time*, not total process wall
time: a healthy invocation may legitimately run thousands of cases for hours.

Example (values beginning with ``--`` use argparse's ``=`` spelling)::

    py run_cts_windows.py \
      --exe D:\\glcts\\glcts.exe --workdir D:\\glcts \
      --caselist D:\\glcts\\mustpass\\gl30.txt --outdir D:\\results\\gl30 \
      --backend DirectVulkan \
      --deqp-arg=--deqp-surface-type=window
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from typing import Iterable, Optional, Sequence


CASE_START = re.compile(r"^#beginTestCaseResult\s+(\S+)")
CASE_END = re.compile(r"^#endTestCaseResult(?:\s|$)")
CASE_TERM = re.compile(r"^#terminateTestCaseResult(?:\s|$)")
CASE_RESULT = re.compile(r'<Result\s+StatusCode="[^"]+"')
CHUNK_ARTIFACT = re.compile(r"^chunk(\d+)(?:\.|$)", re.IGNORECASE)
CHUNK_META = re.compile(r"^chunk(\d+)\.meta\.json$", re.IGNORECASE)
RECOVERY_SIDECAR_NAMES = frozenset(
    {"crashed.txt", "hung.txt", "unrun.txt", "skipped.txt", "remaining.txt"}
)
CONTROLLED_DEQP_OPTIONS = {
    "--deqp-caselist-file",
    "--deqp-log-filename",
}
ATOMIC_REPLACE_ATTEMPTS = 8
ATOMIC_REPLACE_INITIAL_BACKOFF_SECONDS = 0.025
ATOMIC_REPLACE_MAX_BACKOFF_SECONDS = 0.2


class RunnerError(Exception):
    """A user/configuration error which should not be attributed to a case."""


@dataclass
class QpaProgress:
    """Cases recorded by a QPA and its unterminated tail, if any."""

    recorded: list[str]
    in_flight: Optional[str]
    begin_count: int


@dataclass
class ProcessOutcome:
    returncode: Optional[int]
    duration_seconds: float
    timed_out: bool = False
    timeout_reason: Optional[str] = None
    interrupted: bool = False
    launch_error: Optional[str] = None


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def read_caselist(path: Path) -> list[str]:
    """Read a dEQP text caselist, preserving order and removing duplicates."""

    try:
        lines = path.read_text(encoding="utf-8-sig", errors="strict").splitlines()
    except (OSError, UnicodeError) as exc:
        raise RunnerError(f"cannot read caselist {path}: {exc}") from exc

    cases: list[str] = []
    seen: set[str] = set()
    for raw in lines:
        case = raw.strip()
        if not case or case.startswith("#") or case in seen:
            continue
        cases.append(case)
        seen.add(case)
    if not cases:
        raise RunnerError(f"caselist contains no test cases: {path}")
    return cases


def read_name_set(path: Path) -> set[str]:
    if not path.is_file():
        return set()
    try:
        return {
            line.strip()
            for line in path.read_text(encoding="utf-8-sig", errors="replace").splitlines()
            if line.strip() and not line.lstrip().startswith("#")
        }
    except OSError as exc:
        raise RunnerError(f"cannot read recovery file {path}: {exc}") from exc


def _replace_with_retry(source: Path, destination: Path) -> None:
    """Replace a state file, tolerating brief Windows access-denied races.

    Antivirus/indexing tools can momentarily open ``remaining.txt`` without
    delete sharing.  Windows then reports either ``PermissionError`` or a
    generic ``OSError`` carrying ``winerror == 5``.  Retry only those cases;
    disk, path, and programming errors remain immediately visible.
    """

    for attempt in range(ATOMIC_REPLACE_ATTEMPTS):
        try:
            os.replace(source, destination)
            return
        except OSError as exc:
            retryable = isinstance(exc, PermissionError) or getattr(exc, "winerror", None) == 5
            if not retryable or attempt + 1 >= ATOMIC_REPLACE_ATTEMPTS:
                raise
            delay = min(
                ATOMIC_REPLACE_INITIAL_BACKOFF_SECONDS * (2**attempt),
                ATOMIC_REPLACE_MAX_BACKOFF_SECONDS,
            )
            time.sleep(delay)


def atomic_write_text(path: Path, text: str) -> None:
    """Replace a small state file without exposing a partially-written copy."""

    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=f".{path.name}.", suffix=".tmp", dir=str(path.parent))
    temporary_path = Path(temporary)
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
            handle.flush()
            os.fsync(handle.fileno())
        _replace_with_retry(temporary_path, path)
    finally:
        try:
            temporary_path.unlink()
        except FileNotFoundError:
            pass


def atomic_write_json(path: Path, value: object) -> None:
    atomic_write_text(path, json.dumps(value, indent=2, sort_keys=True) + "\n")


def write_case_file(path: Path, cases: Iterable[str]) -> None:
    values = list(cases)
    atomic_write_text(path, "\n".join(values) + ("\n" if values else ""))


def scan_qpa(path: Path) -> QpaProgress:
    """Return cases with a final result and the unfinished tail, if any.

    ``#terminateTestCaseResult`` is a completed result (usually Crash or
    Timeout). ``#endTestCaseResult`` only completes a case when its XML carried
    a ``<Result StatusCode=...>``. A truncated case that already wrote Result is
    also recoverable; a case with no Result remains eligible for a retry.
    """

    if not path.is_file():
        return QpaProgress([], None, 0)

    recorded: list[str] = []
    current: Optional[str] = None
    has_result = False
    begin_count = 0
    try:
        with path.open("r", encoding="utf-8", errors="replace") as handle:
            for raw_line in handle:
                line = raw_line.lstrip("\ufeff")
                match = CASE_START.match(line)
                if match:
                    if current is not None and has_result:
                        recorded.append(current)
                    current = match.group(1)
                    has_result = False
                    begin_count += 1
                    continue
                if current is not None and CASE_RESULT.search(line):
                    has_result = True
                    continue
                if current is not None and CASE_TERM.match(line):
                    recorded.append(current)
                    current = None
                    has_result = False
                    continue
                if current is not None and CASE_END.match(line):
                    if has_result:
                        recorded.append(current)
                    current = None
                    has_result = False
    except OSError as exc:
        raise RunnerError(f"cannot read QPA {path}: {exc}") from exc
    if current is not None and has_result:
        recorded.append(current)
        current = None
    return QpaProgress(recorded, current, begin_count)


def numbered_files(outdir: Path, pattern: re.Pattern[str]) -> list[tuple[int, Path]]:
    found: list[tuple[int, Path]] = []
    try:
        children = list(outdir.iterdir())
    except OSError as exc:
        raise RunnerError(f"cannot list output directory {outdir}: {exc}") from exc
    for path in children:
        match = pattern.match(path.name)
        if match:
            found.append((int(match.group(1)), path))
    found.sort(key=lambda item: item[0])
    return found


def next_chunk_number(outdir: Path) -> int:
    numbers = [number for number, _path in numbered_files(outdir, CHUNK_ARTIFACT)]
    return max(numbers, default=-1) + 1


def load_meta_classifications(outdir: Path, expected: set[str]) -> tuple[set[str], set[str]]:
    """Recover an atomic classification written just before sidecar updates."""

    crashed: set[str] = set()
    hung: set[str] = set()
    for _number, path in numbered_files(outdir, CHUNK_META):
        try:
            value = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, UnicodeError, json.JSONDecodeError):
            # A damaged metadata file is diagnostic only.  QPA and sidecars are
            # authoritative and must still allow recovery.
            continue
        if not isinstance(value, dict):
            continue
        case = value.get("classified_case")
        classification = value.get("classification")
        if not isinstance(case, str) or case not in expected:
            continue
        if classification == "DeviceHang":
            hung.add(case)
        elif classification == "Crash":
            crashed.add(case)
    crashed.difference_update(hung)
    return crashed, hung


def result_qpa_files(outdir: Path) -> list[Path]:
    """Return every QPA a directory-based report would consume."""

    found: list[Path] = []
    try:
        for root, directories, names in os.walk(outdir):
            directories.sort(key=str.casefold)
            for name in sorted(names, key=str.casefold):
                if name.casefold().endswith(".qpa"):
                    found.append(Path(root) / name)
    except OSError as exc:
        raise RunnerError(f"cannot scan output directory {outdir}: {exc}") from exc
    return found


def recover_results(outdir: Path, expected: set[str]) -> tuple[set[str], set[str], set[str]]:
    recorded: set[str] = set()
    for path in result_qpa_files(outdir):
        progress = scan_qpa(path)
        recorded.update(case for case in progress.recorded if case in expected)

    crashed = read_name_set(outdir / "crashed.txt") & expected
    hung = read_name_set(outdir / "hung.txt") & expected
    meta_crashed, meta_hung = load_meta_classifications(outdir, expected)
    crashed.update(meta_crashed)
    hung.update(meta_hung)
    crashed.difference_update(hung)
    return recorded, crashed, hung


def caselist_fingerprint(cases: Sequence[str]) -> str:
    payload = "\n".join(cases).encode("utf-8") + b"\n"
    return hashlib.sha256(payload).hexdigest()


def recovery_artifacts(outdir: Path) -> list[Path]:
    """Return prior-run evidence which must not be adopted implicitly."""

    found = set(result_qpa_files(outdir))
    try:
        children = list(outdir.iterdir())
    except OSError as exc:
        raise RunnerError(f"cannot list output directory {outdir}: {exc}") from exc
    found.update(
        path
        for path in children
        if CHUNK_ARTIFACT.match(path.name)
        or path.name.casefold() in RECOVERY_SIDECAR_NAMES
    )
    return sorted(
        found,
        key=lambda path: str(path.relative_to(outdir)).casefold(),
    )


def check_run_identity(
    outdir: Path,
    backend: str,
    cases: Sequence[str],
    invocation_identity: Optional[str] = None,
    adopt_legacy: bool = False,
) -> None:
    """Refuse to silently mix different suites/backends in one result dir."""

    path = outdir / "run_state.json"
    fingerprint = caselist_fingerprint(cases)
    if path.is_file():
        try:
            state = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, UnicodeError, json.JSONDecodeError) as exc:
            raise RunnerError(f"cannot read run identity {path}: {exc}") from exc
        if not isinstance(state, dict):
            raise RunnerError(f"run identity must be a JSON object: {path}")
        if state.get("backend") != backend:
            raise RunnerError(
                f"output directory belongs to backend {state.get('backend')!r}, not {backend!r}: {outdir}"
            )
        if state.get("caselist_sha256") != fingerprint:
            raise RunnerError(f"output directory belongs to a different caselist: {outdir}")
        stored_invocation_identity = state.get("invocation_identity")
        if (
            stored_invocation_identity is not None
            or invocation_identity is not None
        ) and stored_invocation_identity != invocation_identity:
            raise RunnerError(f"output directory belongs to a different CTS invocation: {outdir}")
        return

    legacy_artifacts = recovery_artifacts(outdir)
    if legacy_artifacts and not adopt_legacy:
        examples = ", ".join(path.name for path in legacy_artifacts[:3])
        raise RunnerError(
            "output directory contains CTS recovery artifacts but no run_state.json; "
            f"refusing to adopt unverified legacy results ({examples}). Re-run with "
            "--adopt-legacy only after verifying the backend, caselist, and invocation."
        )

    atomic_write_json(
        path,
        {
            "version": 1,
            "backend": backend,
            "case_count": len(cases),
            "caselist_sha256": fingerprint,
            "invocation_identity": invocation_identity,
            "adopted_legacy": bool(legacy_artifacts),
            "created_utc": utc_now(),
        },
    )


def persist_sidecars(
    outdir: Path,
    ordered_cases: Sequence[str],
    crashed: set[str],
    hung: set[str],
    remaining: Sequence[str],
) -> None:
    write_case_file(outdir / "crashed.txt", (case for case in ordered_cases if case in crashed))
    write_case_file(outdir / "hung.txt", (case for case in ordered_cases if case in hung))
    write_case_file(outdir / "unrun.txt", remaining)
    write_case_file(outdir / "remaining.txt", remaining)


def parse_environment(values: Sequence[str]) -> dict[str, str]:
    result: dict[str, str] = {}
    for value in values:
        if "=" not in value:
            raise RunnerError(f"--env expects NAME=VALUE, got {value!r}")
        name, contents = value.split("=", 1)
        if not name or "\x00" in name or "=" in name:
            raise RunnerError(f"invalid environment variable name in {value!r}")
        result[name] = contents
    return result


def validate_deqp_args(values: Sequence[str]) -> None:
    for value in values:
        option = value.split("=", 1)[0].lower()
        if option in CONTROLLED_DEQP_OPTIONS:
            raise RunnerError(f"{option} is controlled by this runner and cannot be supplied via --deqp-arg")


def resolve_paths(
    exe_value: str,
    workdir_value: Optional[str],
    caselist_value: str,
    outdir_value: str,
) -> tuple[Path, Path, Path, Path]:
    launch_dir = Path.cwd()
    requested_exe = Path(exe_value).expanduser()

    if workdir_value:
        workdir = Path(workdir_value).expanduser().resolve()
    elif requested_exe.is_absolute():
        workdir = requested_exe.resolve().parent
    else:
        workdir = launch_dir

    if requested_exe.is_absolute():
        exe = requested_exe.resolve()
    else:
        in_workdir = (workdir / requested_exe).resolve()
        in_launch_dir = (launch_dir / requested_exe).resolve()
        exe = in_workdir if in_workdir.is_file() else in_launch_dir

    caselist = Path(caselist_value).expanduser().resolve()
    outdir = Path(outdir_value).expanduser().resolve()
    if not exe.is_file():
        raise RunnerError(f"glcts executable does not exist: {exe}")
    if not workdir.is_dir():
        raise RunnerError(f"working directory does not exist: {workdir}")
    if not caselist.is_file():
        raise RunnerError(f"caselist does not exist: {caselist}")
    return exe, workdir, caselist, outdir


def qpa_signature(path: Path) -> Optional[tuple[int, int]]:
    try:
        stat = path.stat()
    except FileNotFoundError:
        return None
    except OSError:
        # A transient sharing violation must not kill a healthy process.  The
        # next poll will retry and the idle clock retains its previous value.
        return None
    return stat.st_size, stat.st_mtime_ns


def kill_process_tree(process: subprocess.Popen[bytes]) -> None:
    """Force-stop the process and descendants, with a parent-only fallback."""

    if process.poll() is not None:
        return

    if os.name == "nt":
        # /T is essential: CTS/platform helpers can outlive the top-level
        # process, retain the QPA/DLL, and poison the next continuation round.
        taskkill = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32" / "taskkill.exe"
        command = [str(taskkill), "/PID", str(process.pid), "/T", "/F"]
        try:
            subprocess.run(
                command,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=20,
                check=False,
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
            )
        except (OSError, subprocess.TimeoutExpired):
            pass
    else:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except (ProcessLookupError, PermissionError, OSError):
            pass

    try:
        process.wait(timeout=10)
        return
    except subprocess.TimeoutExpired:
        pass

    try:
        process.kill()
    except OSError:
        pass
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        pass


def run_process(
    command: Sequence[str],
    workdir: Path,
    environment: dict[str, str],
    qpa_path: Path,
    stdout_path: Path,
    stderr_path: Path,
    idle_timeout: float,
    max_round_seconds: float,
    poll_seconds: float,
) -> ProcessOutcome:
    """Run one CTS chunk, killing its tree only after QPA progress stalls."""

    started = time.monotonic()
    with stdout_path.open("wb") as stdout_handle, stderr_path.open("wb") as stderr_handle:
        popen_options: dict[str, object] = {
            "cwd": str(workdir),
            "env": environment,
            "stdin": subprocess.DEVNULL,
            "stdout": stdout_handle,
            "stderr": stderr_handle,
        }
        if os.name == "nt":
            popen_options["creationflags"] = getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
        else:
            popen_options["start_new_session"] = True

        try:
            process = subprocess.Popen(list(command), **popen_options)  # type: ignore[arg-type]
        except OSError as exc:
            message = f"failed to launch {command[0]}: {exc}\n"
            stderr_handle.write(message.encode("utf-8", errors="replace"))
            stderr_handle.flush()
            return ProcessOutcome(None, time.monotonic() - started, launch_error=str(exc))

        last_signature = qpa_signature(qpa_path)
        last_progress = time.monotonic()
        timed_out = False
        timeout_reason: Optional[str] = None
        interrupted = False

        try:
            while True:
                try:
                    returncode = process.wait(timeout=poll_seconds)
                    break
                except subprocess.TimeoutExpired:
                    pass

                now = time.monotonic()
                signature = qpa_signature(qpa_path)
                if signature is not None and signature != last_signature:
                    last_signature = signature
                    last_progress = now

                if idle_timeout > 0 and now - last_progress >= idle_timeout:
                    timed_out = True
                    timeout_reason = "qpa-idle"
                    kill_process_tree(process)
                    returncode = process.poll()
                    break
                if max_round_seconds > 0 and now - started >= max_round_seconds:
                    timed_out = True
                    timeout_reason = "max-round"
                    kill_process_tree(process)
                    returncode = process.poll()
                    break
        except KeyboardInterrupt:
            interrupted = True
            kill_process_tree(process)
            returncode = process.poll()

    return ProcessOutcome(
        returncode=returncode,
        duration_seconds=time.monotonic() - started,
        timed_out=timed_out,
        timeout_reason=timeout_reason,
        interrupted=interrupted,
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Run Windows glcts against MobileGL, resuming across crashes and GPU hangs."
    )
    parser.add_argument("--exe", required=True, help="path to glcts.exe")
    parser.add_argument(
        "--workdir",
        help="glcts working directory (default: executable directory, or current directory for a relative exe)",
    )
    parser.add_argument("--caselist", required=True, help="mustpass/caselist text file")
    parser.add_argument("--outdir", required=True, help="persistent result directory")
    parser.add_argument("--backend", required=True, choices=("DirectGLES", "DirectVulkan"))
    parser.add_argument(
        "--run-identity",
        help="controller fingerprint for executable, data, arguments, and environment",
    )
    parser.add_argument(
        "--adopt-legacy",
        action="store_true",
        help=(
            "adopt existing chunk/sidecar results which predate run_state.json; "
            "disabled by default because their provenance cannot be verified"
        ),
    )
    parser.add_argument(
        "--env",
        action="append",
        default=[],
        metavar="NAME=VALUE",
        help="extra child environment variable (repeatable)",
    )
    parser.add_argument(
        "--deqp-arg",
        action="append",
        default=[],
        metavar="ARG",
        help="extra glcts argument; repeat and use --deqp-arg=--option=value for leading dashes",
    )
    parser.add_argument(
        "--idle-timeout",
        type=float,
        default=300.0,
        metavar="SECONDS",
        help="kill a chunk after this many seconds with no QPA size/mtime change (0 disables; default: 300)",
    )
    parser.add_argument(
        "--max-round-seconds",
        type=float,
        default=0.0,
        metavar="SECONDS",
        help="optional total wall limit for one invocation (0 disables; default: 0)",
    )
    parser.add_argument("--poll-seconds", type=float, default=1.0, help=argparse.SUPPRESS)
    parser.add_argument(
        "--max-rounds",
        type=int,
        default=10000,
        help="maximum glcts invocations in this runner process (default: 10000)",
    )
    parser.add_argument(
        "--max-empty-streak",
        type=int,
        default=3,
        help="abort after this many invocations record no case at all; no case is blamed (default: 3)",
    )
    return parser


def execute(args: argparse.Namespace) -> int:
    if args.idle_timeout < 0 or args.max_round_seconds < 0:
        raise RunnerError("timeout values must be non-negative")
    if args.poll_seconds <= 0:
        raise RunnerError("--poll-seconds must be greater than zero")
    if args.max_rounds <= 0 or args.max_empty_streak <= 0:
        raise RunnerError("--max-rounds and --max-empty-streak must be greater than zero")
    validate_deqp_args(args.deqp_arg)
    extra_environment = parse_environment(args.env)
    exe, workdir, caselist_path, outdir = resolve_paths(
        args.exe, args.workdir, args.caselist, args.outdir
    )
    outdir.mkdir(parents=True, exist_ok=True)

    cases = read_caselist(caselist_path)
    expected = set(cases)
    check_run_identity(
        outdir,
        args.backend,
        cases,
        args.run_identity,
        adopt_legacy=args.adopt_legacy,
    )
    recorded, crashed, hung = recover_results(outdir, expected)
    accounted = recorded | crashed | hung
    remaining = [case for case in cases if case not in accounted]
    persist_sidecars(outdir, cases, crashed, hung, remaining)

    existing_qpas = len(result_qpa_files(outdir))
    print(
        f"[run_cts_windows] {args.backend}: expected {len(cases)}, recovered {len(accounted)} "
        f"({existing_qpas} QPA chunk(s), {len(crashed)} crash, {len(hung)} hang)"
    )
    if not remaining:
        print(f"[run_cts_windows] complete: all {len(cases)} expected cases are accounted")
        return 0

    environment = os.environ.copy()
    environment.update(extra_environment)
    # --backend is authoritative even if the inherited or extra environment
    # already contains a different value.
    environment["MOBILEGL_BACKEND_TYPE"] = args.backend

    common_arguments = [
        "--deqp-terminate-on-device-lost=disable",
        "--deqp-log-images=disable",
        "--deqp-log-shader-sources=disable",
    ]

    chunk_number = next_chunk_number(outdir)
    rounds = 0
    empty_streak = 0
    interrupted = False
    fatal_launch_error = False
    started_all = time.monotonic()

    while remaining and rounds < args.max_rounds:
        prefix = f"chunk{chunk_number:04d}"
        remaining_path = outdir / "remaining.txt"
        qpa_path = outdir / f"{prefix}.qpa"
        stdout_path = outdir / f"{prefix}.stdout.log"
        stderr_path = outdir / f"{prefix}.stderr.log"
        meta_path = outdir / f"{prefix}.meta.json"

        # The number allocator considers every chunk artifact, so these should
        # be new.  Refuse to truncate evidence if a foreign file races us.
        for artifact in (qpa_path, stdout_path, stderr_path, meta_path):
            if artifact.exists():
                raise RunnerError(f"refusing to overwrite existing chunk artifact: {artifact}")
        write_case_file(remaining_path, remaining)

        command = [
            str(exe),
            f"--deqp-caselist-file={remaining_path}",
            f"--deqp-log-filename={qpa_path}",
            *common_arguments,
            *args.deqp_arg,
        ]
        print(
            f"[run_cts_windows] {prefix}: launching {len(remaining)} remaining case(s); "
            f"idle timeout {args.idle_timeout:g}s"
        )
        chunk_started_utc = utc_now()
        outcome = run_process(
            command,
            workdir,
            environment,
            qpa_path,
            stdout_path,
            stderr_path,
            args.idle_timeout,
            args.max_round_seconds,
            args.poll_seconds,
        )
        progress = scan_qpa(qpa_path)

        before = set(accounted)
        for case in progress.recorded:
            if case in expected:
                recorded.add(case)
                accounted.add(case)

        classification: Optional[str] = None
        classified_case: Optional[str] = None
        in_flight = progress.in_flight if progress.in_flight in expected else None
        if not outcome.interrupted and in_flight is not None and in_flight not in accounted:
            classified_case = in_flight
            if outcome.timed_out:
                classification = "DeviceHang"
                hung.add(in_flight)
                crashed.discard(in_flight)
            else:
                classification = "Crash"
                crashed.add(in_flight)
            accounted.add(in_flight)

        new_accounted = len(accounted - before)
        if new_accounted:
            empty_streak = 0
        elif progress.begin_count == 0:
            # No #begin marker means there is no evidence that the first
            # remaining case was reached.  Retry the identical caselist, then
            # abort rather than manufacturing a string of false Crash results.
            empty_streak += 1
        else:
            # A log containing only already-accounted cases is also no forward
            # progress, but it is a different failure mode.  Bound it with the
            # same guard while retaining the QPA evidence.
            empty_streak += 1

        remaining = [case for case in cases if case not in accounted]
        metadata = {
            "version": 1,
            "chunk": chunk_number,
            "started_utc": chunk_started_utc,
            "finished_utc": utc_now(),
            "duration_seconds": round(outcome.duration_seconds, 3),
            "returncode": outcome.returncode,
            "timed_out": outcome.timed_out,
            "timeout_reason": outcome.timeout_reason,
            "interrupted": outcome.interrupted,
            "launch_error": outcome.launch_error,
            "qpa_begin_count": progress.begin_count,
            "qpa_recorded_count": len(progress.recorded),
            "in_flight": progress.in_flight,
            "classification": classification,
            "classified_case": classified_case,
            "new_accounted": new_accounted,
            "remaining": len(remaining),
        }
        # Metadata is committed first.  If the runner itself dies between this
        # write and the sidecars, recovery can reconstruct the classification.
        atomic_write_json(meta_path, metadata)
        persist_sidecars(outdir, cases, crashed, hung, remaining)

        rounds += 1
        elapsed_minutes = (time.monotonic() - started_all) / 60.0
        detail = ""
        if classification:
            detail = f", {classification}={classified_case}"
        if outcome.timed_out:
            detail += f", timeout={outcome.timeout_reason}"
        print(
            f"[run_cts_windows] {prefix}: +{new_accounted}, accounted "
            f"{len(accounted)}/{len(cases)}, remaining {len(remaining)}{detail} "
            f"({elapsed_minutes:.1f} min)"
        )

        chunk_number += 1
        if outcome.interrupted:
            interrupted = True
            print("[run_cts_windows] interrupted; process tree stopped and state preserved", file=sys.stderr)
            break
        if outcome.launch_error:
            fatal_launch_error = True
            print(
                f"[run_cts_windows] launch failed; see {stderr_path.name}: {outcome.launch_error}",
                file=sys.stderr,
            )
            break
        if empty_streak >= args.max_empty_streak:
            print(
                f"[run_cts_windows] aborting after {empty_streak} consecutive chunks made no "
                "case progress; no unobserved case was labelled Crash/Hang",
                file=sys.stderr,
            )
            break

    # Recompute from the persisted evidence so the final completeness claim is
    # subject to the exact same recovery path as a later invocation.
    final_recorded, final_crashed, final_hung = recover_results(outdir, expected)
    final_accounted = final_recorded | final_crashed | final_hung
    final_remaining = [case for case in cases if case not in final_accounted]
    persist_sidecars(outdir, cases, final_crashed, final_hung, final_remaining)

    if not final_remaining and final_accounted == expected:
        print(
            f"[run_cts_windows] complete: all {len(cases)} expected cases are accounted "
            f"({len(final_crashed)} crash, {len(final_hung)} hang, {rounds} new invocation(s))"
        )
        return 0

    print(
        f"[run_cts_windows] INCOMPLETE: {len(final_accounted)}/{len(cases)} accounted; "
        f"{len(final_remaining)} listed in {outdir / 'unrun.txt'}",
        file=sys.stderr,
    )
    if interrupted:
        return 130
    if fatal_launch_error:
        return 3
    return 4


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        return execute(args)
    except RunnerError as exc:
        print(f"[run_cts_windows] ERROR: {exc}", file=sys.stderr)
        return 2
    except OSError as exc:
        print(f"[run_cts_windows] ERROR: filesystem/process operation failed: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
