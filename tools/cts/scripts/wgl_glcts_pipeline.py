#!/usr/bin/env python
"""Build MobileGL's Windows WGL shim and run Khronos OpenGL CTS suites.

The pipeline intentionally keeps the build, runtime, results, and reports in an
explicit work root.  Each run is keyed by the hashes of glcts.exe, opengl32.dll,
and (for DirectGLES) the ANGLE runtime, so resuming can never silently combine
results from different binaries.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
from datetime import datetime, timezone
from typing import Iterable, Mapping, Optional, Sequence


SUPPORTED_VERSIONS = ("30", "31", "32", "33", "40", "41", "42", "43", "44", "45", "46")
SUPPORTED_BACKENDS = ("DirectGLES", "DirectVulkan")
ANGLE_REQUIRED_DLLS = ("libEGL.dll", "libGLESv2.dll", "d3dcompiler_47.dll")
ANGLE_OPTIONAL_DLLS = ("dxcompiler.dll", "dxil.dll")
PE_MACHINE_AMD64 = 0x8664
TRACKED_ENVIRONMENT_NAMES = frozenset({"PATH"})
TRACKED_ENVIRONMENT_PREFIXES = (
    "MOBILEGL_",
    "LIBGL_",
    "VK_",
    "ANGLE_",
    "EGL_",
    "D3D_",
    "DXVK_",
)
CONTROLLED_ENVIRONMENT_NAMES = frozenset(
    {"MOBILEGL_BACKEND_TYPE", "MOBILEGL_LOG_FILE_PATH"}
)

DEFAULT_DEQP_ARGS = (
    "--deqp-gl-context-type=wgl",
    "--deqp-surface-type=fbo",
    "--deqp-gl-config-name=rgba8888d24s8",
    "--deqp-surface-width=64",
    "--deqp-surface-height=-1",
    "--deqp-base-seed=3",
    "--deqp-visibility=hidden",
    "--deqp-watchdog=enable",
    "--deqp-crashhandler=enable",
)

SESSION_VENDOR = re.compile(r'^#sessionInfo vendor "([^"]*)"', re.MULTILINE)
SESSION_RENDERER = re.compile(r'^#sessionInfo renderer "([^"]*)"', re.MULTILINE)
SESSION_COMMAND_LINE = re.compile(r'^#sessionInfo commandLineParameters "([^"]*)"', re.MULTILINE)
TARGET_GL_VERSION = re.compile(r"Target OpenGL Version:\s*(\d+)\.(\d+)")


class PipelineError(RuntimeError):
    """A configuration, build, or identity error."""


def repository_root() -> Path:
    return Path(__file__).resolve().parents[3]


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def normalize_version(value: str) -> str:
    normalized = value.strip().lower().removeprefix("gl").replace(".", "")
    if normalized not in SUPPORTED_VERSIONS:
        supported = ", ".join(f"gl{version}" for version in SUPPORTED_VERSIONS)
        raise argparse.ArgumentTypeError(f"unsupported GL suite {value!r}; choose one of: {supported}")
    return normalized


def gl_version_tuple(version: str) -> tuple[int, int]:
    return int(version[0]), int(version[1])


def parse_assignment(value: str) -> tuple[str, str]:
    name, separator, setting = value.partition("=")
    if not separator or not name or "\x00" in value:
        raise argparse.ArgumentTypeError(f"expected NAME=VALUE, got {value!r}")
    if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
        raise argparse.ArgumentTypeError(f"invalid environment variable name {name!r}")
    return name, setting


def canonicalize_windows_environment(
    items: Iterable[tuple[str, str]],
) -> dict[str, str]:
    """Canonicalize environment keys using Windows' case-insensitive rules."""

    result: dict[str, str] = {}
    for name, value in items:
        result[name.upper()] = value
    return result


def tracked_run_environment(
    inherited: Mapping[str, str], overrides: Mapping[str, str]
) -> tuple[dict[str, str], dict[str, str]]:
    """Return tracked ambient values and the effective values used for identity."""

    canonical_inherited = canonicalize_windows_environment(inherited.items())
    ambient = {
        name: value
        for name, value in canonical_inherited.items()
        if name in TRACKED_ENVIRONMENT_NAMES
        or name.startswith(TRACKED_ENVIRONMENT_PREFIXES)
    }
    effective = dict(ambient)
    effective.update(canonicalize_windows_environment(overrides.items()))
    return dict(sorted(ambient.items())), dict(sorted(effective.items()))


def command_text(command: Sequence[object]) -> str:
    return subprocess.list2cmdline([str(part) for part in command])


def run_command(
    command: Sequence[object],
    *,
    cwd: Optional[Path] = None,
    env: Optional[Mapping[str, str]] = None,
    check: bool = True,
    capture: bool = False,
) -> subprocess.CompletedProcess[str]:
    rendered = command_text(command)
    location = f" (cwd={cwd})" if cwd else ""
    print(f"[wgl_glcts_pipeline] $ {rendered}{location}", flush=True)
    completed = subprocess.run(
        [str(part) for part in command],
        cwd=str(cwd) if cwd else None,
        env=dict(env) if env else None,
        text=True,
        capture_output=capture,
        check=False,
    )
    if check and completed.returncode != 0:
        detail = ""
        if capture:
            detail = f"\nstdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        raise PipelineError(f"command failed with exit code {completed.returncode}: {rendered}{detail}")
    return completed


def require_file(path: Path, label: str) -> Path:
    resolved = path.expanduser().resolve()
    if not resolved.is_file():
        raise PipelineError(f"{label} does not exist or is not a file: {resolved}")
    return resolved


def require_directory(path: Path, label: str) -> Path:
    resolved = path.expanduser().resolve()
    if not resolved.is_dir():
        raise PipelineError(f"{label} does not exist or is not a directory: {resolved}")
    return resolved


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def sha256_directory(path: Path) -> str:
    digest = hashlib.sha256()
    files = sorted(
        (candidate for candidate in path.rglob("*") if candidate.is_file()),
        key=lambda candidate: candidate.relative_to(path).as_posix(),
    )
    if not files:
        raise PipelineError(f"directory contains no files to fingerprint: {path}")
    for candidate in files:
        relative = candidate.relative_to(path).as_posix()
        digest.update(relative.encode("utf-8"))
        digest.update(b"\0")
        digest.update(sha256_file(candidate).encode("ascii"))
        digest.update(b"\n")
    return digest.hexdigest()


def pe_machine(path: Path) -> int:
    with path.open("rb") as stream:
        if stream.read(2) != b"MZ":
            raise PipelineError(f"not a PE executable: {path}")
        stream.seek(0x3C)
        offset_bytes = stream.read(4)
        if len(offset_bytes) != 4:
            raise PipelineError(f"truncated PE header: {path}")
        pe_offset = struct.unpack("<I", offset_bytes)[0]
        stream.seek(pe_offset)
        if stream.read(4) != b"PE\0\0":
            raise PipelineError(f"invalid PE signature: {path}")
        machine_bytes = stream.read(2)
        if len(machine_bytes) != 2:
            raise PipelineError(f"truncated PE COFF header: {path}")
        return struct.unpack("<H", machine_bytes)[0]


def require_x64_pe(path: Path, label: str) -> None:
    machine = pe_machine(path)
    if machine != PE_MACHINE_AMD64:
        raise PipelineError(f"{label} must be an x64 PE (machine 0x8664), got 0x{machine:04x}: {path}")


def git_snapshot(path: Path) -> dict[str, object]:
    snapshot: dict[str, object] = {"path": str(path)}
    try:
        head = run_command(
            ["git", "-C", path, "rev-parse", "HEAD"], check=True, capture=True
        ).stdout.strip()
        status = run_command(
            ["git", "-C", path, "status", "--porcelain"], check=True, capture=True
        ).stdout
        snapshot.update({"head": head, "dirty": bool(status.strip())})
    except (OSError, PipelineError):
        snapshot.update({"head": None, "dirty": None})
    return snapshot


def generator_arguments(generator: str, architecture: str) -> list[str]:
    arguments = ["-G", generator]
    if generator.lower().startswith("visual studio"):
        arguments.extend(["-A", architecture])
    return arguments


def mobilegl_configure_command(
    repo_root: Path,
    build_dir: Path,
    generator: str,
    architecture: str,
    extra: Iterable[str],
) -> list[str]:
    return [
        "cmake",
        "-S",
        str(repo_root),
        "-B",
        str(build_dir),
        *generator_arguments(generator, architecture),
        "-DMOBILEGL_BUILD_TEST=OFF",
        "-DMOBILEGL_BUILD_BENCHMARK=OFF",
        "-DMOBILEGL_BUILD_TRACE_REPLAY=OFF",
        "-DMOBILEGL_ENABLE_TRACY=OFF",
        "-DMOBILEGL_FORCE_RELEASE_OPT=ON",
        *extra,
    ]


def cts_configure_command(
    cts_source: Path,
    build_dir: Path,
    generator: str,
    architecture: str,
    extra: Iterable[str],
) -> list[str]:
    return [
        "cmake",
        "-S",
        str(cts_source),
        "-B",
        str(build_dir),
        *generator_arguments(generator, architecture),
        "-DDEQP_TARGET=default",
        "-DDEQP_SUPPORT_DRM=OFF",
        *extra,
    ]


def build_command(build_dir: Path, configuration: str, target: str, jobs: int) -> list[str]:
    command = ["cmake", "--build", str(build_dir), "--config", configuration, "--target", target]
    if jobs > 0:
        command.extend(["--parallel", str(jobs)])
    return command


def verify_mobilegl_sources(repo_root: Path) -> None:
    required = (
        repo_root / "CMakeLists.txt",
        repo_root / "MobileGL" / "MG_Impl" / "WGLImpl" / "WGLImpl.cpp",
        repo_root / "3rdparty" / "glslang" / "CMakeLists.txt",
        repo_root / "3rdparty" / "SPIRV-Cross" / "CMakeLists.txt",
        repo_root / "3rdparty" / "Vulkan-Headers" / "CMakeLists.txt",
    )
    missing = [str(path) for path in required if not path.is_file()]
    if missing:
        raise PipelineError(
            "MobileGL source/submodules are incomplete:\n  "
            + "\n  ".join(missing)
            + f"\nRun: git -C {repo_root} submodule update --init --recursive"
        )


def verify_cts_sources(cts_source: Path) -> None:
    required = (
        cts_source / "CMakeLists.txt",
        cts_source / "external" / "openglcts" / "CMakeLists.txt",
    )
    missing = [str(path) for path in required if not path.is_file()]
    if missing:
        raise PipelineError(
            "VK-GL-CTS source/external packages are incomplete:\n  "
            + "\n  ".join(missing)
            + f"\nRun: {sys.executable} {cts_source / 'external' / 'fetch_sources.py'}"
        )


def discover_mobilegl_dll(build_dir: Path, configuration: str) -> Path:
    preferred = (
        build_dir / configuration / "opengl32.dll",
        build_dir / "MobileGL" / configuration / "opengl32.dll",
        build_dir / "opengl32.dll",
    )
    for candidate in preferred:
        if candidate.is_file():
            return candidate.resolve()
    candidates = sorted({path.resolve() for path in build_dir.rglob("opengl32.dll") if path.is_file()})
    if len(candidates) == 1:
        return candidates[0]
    if not candidates:
        raise PipelineError(f"MobileGL build produced no opengl32.dll under {build_dir}")
    raise PipelineError("multiple opengl32.dll candidates; pass --mobilegl-dll explicitly:\n  " + "\n  ".join(map(str, candidates)))


def discover_glcts_exe(build_dir: Path, configuration: str) -> Path:
    preferred = (
        build_dir / "external" / "openglcts" / "modules" / configuration / "glcts.exe",
        build_dir / "external" / "openglcts" / "modules" / "glcts.exe",
    )
    for candidate in preferred:
        if candidate.is_file():
            return candidate.resolve()
    candidates = sorted({path.resolve() for path in build_dir.rglob("glcts.exe") if path.is_file()})
    if len(candidates) == 1:
        return candidates[0]
    if not candidates:
        raise PipelineError(f"CTS build produced no glcts.exe under {build_dir}")
    raise PipelineError("multiple glcts.exe candidates; pass --glcts-exe explicitly:\n  " + "\n  ".join(map(str, candidates)))


def default_cts_modules_dir(cts_build_dir: Path) -> Path:
    return cts_build_dir / "external" / "openglcts" / "modules"


def find_caselist_root(cts_modules_dir: Path, cts_source: Path) -> Path:
    relative = Path("gl_cts/data/mustpass/gl/khronos_mustpass/main")
    candidates = (cts_modules_dir / relative, cts_source / "external" / "openglcts" / "modules" / relative)
    for candidate in candidates:
        if candidate.is_dir():
            return candidate.resolve()
    raise PipelineError("Khronos GL mustpass directory was not found; checked:\n  " + "\n  ".join(map(str, candidates)))


def caselist_for(caselist_root: Path, version: str) -> Path:
    return require_file(caselist_root / f"gl{version}-main.txt", f"GL{version} mustpass caselist")


def runtime_source_files(
    glcts_exe: Path,
    mobilegl_dll: Path,
    backends: Sequence[str],
    angle_dir: Optional[Path],
) -> dict[str, Path]:
    files = {"glcts.exe": glcts_exe, "opengl32.dll": mobilegl_dll}
    if "DirectGLES" in backends:
        if angle_dir is None:
            raise PipelineError("--angle-dir is required when DirectGLES is selected")
        angle_dir = require_directory(angle_dir, "ANGLE runtime directory")
        for name in ANGLE_REQUIRED_DLLS:
            files[name] = require_file(angle_dir / name, f"ANGLE {name}")
        for name in ANGLE_OPTIONAL_DLLS:
            candidate = angle_dir / name
            if candidate.is_file():
                files[name] = candidate.resolve()
    return files


def runtime_fingerprint(files: Mapping[str, Path]) -> tuple[str, dict[str, str]]:
    hashes = {name: sha256_file(path) for name, path in sorted(files.items())}
    digest = hashlib.sha256()
    for name, file_hash in hashes.items():
        digest.update(f"{name}\0{file_hash}\n".encode("utf-8"))
    return digest.hexdigest(), hashes


def run_fingerprint(
    runtime_hash: str,
    cts_data_hash: str,
    tool_hashes: Mapping[str, str],
    caselist_hashes: Mapping[str, str],
    deqp_args: Sequence[str],
    environment: Mapping[str, str],
    result_semantics: Optional[Mapping[str, object]] = None,
) -> str:
    identity = {
        "version": 2,
        "runtime_fingerprint": runtime_hash,
        "cts_data_sha256": cts_data_hash,
        "tool_hashes": dict(sorted(tool_hashes.items())),
        "caselist_hashes": dict(sorted(caselist_hashes.items())),
        "deqp_args": list(deqp_args),
        "environment": dict(sorted(environment.items())),
        "result_semantics": dict(sorted((result_semantics or {}).items())),
    }
    return hashlib.sha256(
        json.dumps(identity, sort_keys=True, separators=(",", ":")).encode("utf-8")
    ).hexdigest()


def assemble_runtime(
    work_root: Path,
    sources: Mapping[str, Path],
    fingerprint: str,
    hashes: Mapping[str, str],
) -> Path:
    runtime_dir = work_root / "runtime" / fingerprint[:16]
    runtime_dir.mkdir(parents=True, exist_ok=True)
    for name, source in sources.items():
        require_x64_pe(source, name)
        destination = runtime_dir / name
        if source.resolve() != destination.resolve():
            shutil.copy2(source, destination)
        if sha256_file(destination) != hashes[name]:
            raise PipelineError(f"runtime copy hash mismatch: {destination}")
    manifest = {
        "version": 1,
        "created_utc": utc_now(),
        "fingerprint": fingerprint,
        "files": {
            name: {"source": str(source), "sha256": hashes[name]}
            for name, source in sorted(sources.items())
        },
    }
    write_json(runtime_dir / "manifest.json", manifest)
    return runtime_dir


def write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(temporary, path)


def read_cases(caselist: Path) -> list[str]:
    cases: list[str] = []
    for raw_line in caselist.read_text(encoding="utf-8-sig").splitlines():
        line = raw_line.strip()
        if line and not line.startswith("#"):
            cases.append(line)
    if not cases:
        raise PipelineError(f"caselist contains no cases: {caselist}")
    return cases


def choose_preflight_case(caselist: Path) -> str:
    cases = read_cases(caselist)
    preferred_suffixes = (
        ".buffer_objects.gen_buffers",
        ".CommonBugs.CommonBug_GetProgramivActiveUniformBlockMaxNameLength",
    )
    for suffix in preferred_suffixes:
        for case in cases:
            if case.endswith(suffix):
                return case
    return cases[0]


def runner_command(
    runner: Path,
    runtime_exe: Path,
    workdir: Path,
    caselist: Path,
    outdir: Path,
    backend: str,
    idle_timeout: float,
    max_round_seconds: float,
    max_rounds: int,
    environment: Mapping[str, str],
    deqp_args: Sequence[str],
    run_identity: Optional[str] = None,
) -> list[str]:
    command = [
        sys.executable,
        str(runner),
        "--exe",
        str(runtime_exe),
        "--workdir",
        str(workdir),
        "--caselist",
        str(caselist),
        "--outdir",
        str(outdir),
        "--backend",
        backend,
        "--idle-timeout",
        str(idle_timeout),
        "--max-round-seconds",
        str(max_round_seconds),
        "--max-rounds",
        str(max_rounds),
    ]
    if run_identity is not None:
        command.extend(["--run-identity", run_identity])
    for name, value in sorted(environment.items()):
        command.extend(["--env", f"{name}={value}"])
    command.extend(f"--deqp-arg={argument}" for argument in deqp_args)
    return command


def parse_preflight_identity(
    qpa_files: Sequence[Path],
    mobilegl_log: Path,
    backend: str,
    minimum_version: tuple[int, int],
) -> dict[str, object]:
    if not qpa_files:
        raise PipelineError(f"{backend} preflight produced no QPA file")
    qpa_text = "\n".join(path.read_text(encoding="utf-8", errors="replace") for path in qpa_files)
    vendors = SESSION_VENDOR.findall(qpa_text)
    renderers = SESSION_RENDERER.findall(qpa_text)
    command_lines = SESSION_COMMAND_LINE.findall(qpa_text)
    if not vendors or "MobileGL" not in vendors[-1]:
        raise PipelineError(f"{backend} preflight did not load MobileGL (vendor={vendors[-1] if vendors else None!r})")
    expected_renderer = "Espryt" if backend == "DirectGLES" else "Magma"
    if not renderers or expected_renderer not in renderers[-1]:
        raise PipelineError(
            f"{backend} preflight renderer mismatch: expected {expected_renderer!r}, "
            f"got {renderers[-1] if renderers else None!r}"
        )
    if not command_lines or "--deqp-gl-context-type=wgl" not in command_lines[-1]:
        raise PipelineError(f"{backend} preflight did not record a WGL context")
    if not mobilegl_log.is_file():
        raise PipelineError(f"{backend} preflight did not create MobileGL log: {mobilegl_log}")
    log_text = mobilegl_log.read_text(encoding="utf-8", errors="replace")
    versions = [(int(major), int(minor)) for major, minor in TARGET_GL_VERSION.findall(log_text)]
    if not versions:
        raise PipelineError(f"{backend} preflight log contains no target OpenGL version")
    actual_version = versions[-1]
    if actual_version < minimum_version:
        raise PipelineError(
            f"{backend} reports GL {actual_version[0]}.{actual_version[1]}, "
            f"but selected suites require at least {minimum_version[0]}.{minimum_version[1]}"
        )
    return {
        "backend": backend,
        "vendor": vendors[-1],
        "renderer": renderers[-1],
        "target_gl_version": f"{actual_version[0]}.{actual_version[1]}",
        "qpa_files": [str(path) for path in qpa_files],
        "mobilegl_log": str(mobilegl_log),
    }


def run_preflight(
    *,
    runner: Path,
    runtime_exe: Path,
    cts_modules_dir: Path,
    caselist: Path,
    preflight_root: Path,
    backend: str,
    idle_timeout: float,
    environment: Mapping[str, str],
    deqp_args: Sequence[str],
    minimum_version: tuple[int, int],
    run_identity: str,
) -> dict[str, object]:
    outdir = preflight_root / backend.lower()
    outdir.mkdir(parents=True, exist_ok=True)
    case_file = outdir / "case.txt"
    case_file.write_text(choose_preflight_case(caselist) + "\n", encoding="utf-8")
    log_path = outdir / "mobilegl.log"
    child_environment = dict(environment)
    child_environment["MOBILEGL_LOG_FILE_PATH"] = str(log_path)
    command = runner_command(
        runner,
        runtime_exe,
        cts_modules_dir,
        case_file,
        outdir,
        backend,
        min(idle_timeout, 120.0) if idle_timeout > 0 else 120.0,
        180.0,
        1,
        child_environment,
        deqp_args,
        run_identity,
    )
    # A developing driver may fail the chosen case or terminate during deinit.
    # Identity is the gate: the QPA and MobileGL log must prove which WGL driver ran.
    run_command(command, cwd=repository_root(), check=False)
    identity = parse_preflight_identity(sorted(outdir.glob("chunk*.qpa")), log_path, backend, minimum_version)
    print(
        f"[wgl_glcts_pipeline] preflight {backend}: {identity['renderer']} | "
        f"GL {identity['target_gl_version']}"
    )
    return identity


def report_command(
    reporter: Path,
    suites: Sequence[tuple[str, str, Path, Path]],
    markdown: Path,
    json_out: Path,
    allow_incomplete: bool,
    expected_run_identity: Optional[str] = None,
) -> list[str]:
    command = [sys.executable, str(reporter)]
    for backend, label, caselist, results in suites:
        command.extend(["--suite", backend, label, str(caselist), str(results)])
    command.extend(["--markdown", str(markdown), "--json", str(json_out)])
    if expected_run_identity is not None:
        command.extend(["--expected-run-identity", expected_run_identity])
    if allow_incomplete:
        command.append("--allow-incomplete")
    return command


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Build MobileGL WGL, assemble a local glcts runtime, run GL30-GL46, and report results."
    )
    parser.add_argument("--repo-root", type=Path, default=repository_root(), help="MobileGL worktree root")
    parser.add_argument("--cts-source", type=Path, required=True, help="VK-GL-CTS source checkout")
    parser.add_argument("--work-root", type=Path, required=True, help="build/result root (kept outside source)")
    parser.add_argument("--angle-dir", type=Path, help="x64 ANGLE directory for DirectGLES")
    parser.add_argument("--backends", nargs="+", choices=SUPPORTED_BACKENDS, default=list(SUPPORTED_BACKENDS))
    parser.add_argument("--versions", nargs="+", type=normalize_version, default=list(SUPPORTED_VERSIONS))
    parser.add_argument("--configuration", default="Release")
    parser.add_argument("--generator", default="Visual Studio 17 2022")
    parser.add_argument("--architecture", default="x64")
    parser.add_argument("--jobs", type=int, default=max(1, os.cpu_count() or 1))
    parser.add_argument("--mobilegl-build-dir", type=Path)
    parser.add_argument("--cts-build-dir", type=Path)
    parser.add_argument("--mobilegl-dll", type=Path, help="reuse an existing MobileGL/opengl32 DLL")
    parser.add_argument("--glcts-exe", type=Path, help="reuse an existing glcts.exe")
    parser.add_argument("--cts-modules-dir", type=Path, help="glcts working directory containing gl_cts data")
    parser.add_argument("--skip-mobilegl-build", action="store_true")
    parser.add_argument("--skip-cts-build", action="store_true")
    parser.add_argument("--skip-preflight", action="store_true")
    parser.add_argument("--skip-run", action="store_true")
    parser.add_argument("--skip-report", action="store_true")
    parser.add_argument("--allow-incomplete-report", action="store_true")
    parser.add_argument("--continue-on-suite-error", action="store_true")
    parser.add_argument("--idle-timeout", type=float, default=300.0)
    parser.add_argument("--max-round-seconds", type=float, default=0.0)
    parser.add_argument("--max-rounds", type=int, default=10000)
    parser.add_argument("--env", action="append", type=parse_assignment, default=[], metavar="NAME=VALUE")
    parser.add_argument(
        "--deqp-arg", action="append", default=[], metavar="ARG", help="extra glcts option; use --deqp-arg=--x=y"
    )
    parser.add_argument(
        "--mobilegl-cmake-arg", action="append", default=[], metavar="ARG", help="extra MobileGL configure option"
    )
    parser.add_argument("--cts-cmake-arg", action="append", default=[], metavar="ARG", help="extra CTS configure option")
    return parser


def execute(args: argparse.Namespace) -> int:
    if os.name != "nt":
        raise PipelineError("this pipeline builds and exercises the Windows WGL target and must run on Windows")
    if shutil.which("cmake") is None and (not args.skip_mobilegl_build or not args.skip_cts_build):
        raise PipelineError("cmake was not found on PATH")
    if args.jobs < 0 or args.max_rounds <= 0:
        raise PipelineError("--jobs must be >= 0 and --max-rounds must be > 0")

    repo_root = require_directory(args.repo_root, "MobileGL worktree")
    cts_source = require_directory(args.cts_source, "VK-GL-CTS checkout")
    work_root = args.work_root.expanduser().resolve()
    work_root.mkdir(parents=True, exist_ok=True)
    versions = list(dict.fromkeys(args.versions))
    backends = list(dict.fromkeys(args.backends))
    minimum_version = max(gl_version_tuple(version) for version in versions)
    extra_environment = canonicalize_windows_environment(args.env)
    reserved_environment = CONTROLLED_ENVIRONMENT_NAMES & set(extra_environment)
    if reserved_environment:
        raise PipelineError("the pipeline controls these environment variables: " + ", ".join(sorted(reserved_environment)))

    configuration = args.configuration
    mobilegl_build_dir = (args.mobilegl_build_dir or work_root / f"mobilegl-build-{configuration.lower()}").resolve()
    cts_build_dir = (args.cts_build_dir or work_root / f"cts-build-wgl-{configuration.lower()}").resolve()

    if not args.skip_mobilegl_build:
        verify_mobilegl_sources(repo_root)
        mobilegl_build_dir.mkdir(parents=True, exist_ok=True)
        run_command(
            mobilegl_configure_command(
                repo_root, mobilegl_build_dir, args.generator, args.architecture, args.mobilegl_cmake_arg
            ),
            cwd=repo_root,
        )
        run_command(build_command(mobilegl_build_dir, configuration, "MobileGL", args.jobs), cwd=repo_root)
    mobilegl_dll = (
        require_file(args.mobilegl_dll, "MobileGL DLL")
        if args.mobilegl_dll
        else discover_mobilegl_dll(mobilegl_build_dir, configuration)
    )

    if not args.skip_cts_build:
        verify_cts_sources(cts_source)
        cts_build_dir.mkdir(parents=True, exist_ok=True)
        run_command(
            cts_configure_command(cts_source, cts_build_dir, args.generator, args.architecture, args.cts_cmake_arg),
            cwd=cts_source,
        )
        run_command(build_command(cts_build_dir, configuration, "glcts", args.jobs), cwd=cts_source)
    glcts_exe = (
        require_file(args.glcts_exe, "glcts executable")
        if args.glcts_exe
        else discover_glcts_exe(cts_build_dir, configuration)
    )
    cts_modules_dir = require_directory(
        args.cts_modules_dir or default_cts_modules_dir(cts_build_dir), "CTS modules/working directory"
    )
    cts_data_dir = require_directory(cts_modules_dir / "gl_cts" / "data", "CTS gl_cts data directory")
    cts_data_hash = sha256_directory(cts_data_dir)
    caselist_root = find_caselist_root(cts_modules_dir, cts_source)
    caselists = {version: caselist_for(caselist_root, version) for version in versions}

    runtime_sources = runtime_source_files(glcts_exe, mobilegl_dll, backends, args.angle_dir)
    fingerprint, hashes = runtime_fingerprint(runtime_sources)
    runtime_dir = assemble_runtime(work_root, runtime_sources, fingerprint, hashes)
    runtime_exe = runtime_dir / "glcts.exe"
    runner = require_file(repo_root / "tools" / "cts" / "scripts" / "run_cts_windows.py", "Windows CTS runner")
    reporter = require_file(repo_root / "tools" / "cts" / "scripts" / "cts_multi_report.py", "CTS reporter")
    matrix_reporter = require_file(
        repo_root / "tools" / "cts" / "scripts" / "cts_matrix_report.py", "CTS matrix reporter"
    )
    qpa_reporter = require_file(repo_root / "tools" / "cts" / "scripts" / "qpa_report.py", "QPA parser")
    tool_hashes = {
        "wgl_glcts_pipeline.py": sha256_file(Path(__file__).resolve()),
        "run_cts_windows.py": sha256_file(runner),
        "cts_multi_report.py": sha256_file(reporter),
        "cts_matrix_report.py": sha256_file(matrix_reporter),
        "qpa_report.py": sha256_file(qpa_reporter),
    }
    deqp_args = [*DEFAULT_DEQP_ARGS, *args.deqp_arg]
    caselist_hashes = {version: sha256_file(path) for version, path in caselists.items()}
    ambient_environment, identity_environment = tracked_run_environment(
        os.environ, extra_environment
    )
    result_semantics = {
        "idle_timeout_seconds": args.idle_timeout,
        "max_round_seconds": args.max_round_seconds,
    }
    execution_settings = {
        **result_semantics,
        "max_rounds": args.max_rounds,
        "continue_on_suite_error": args.continue_on_suite_error,
    }
    execution_fingerprint = run_fingerprint(
        fingerprint,
        cts_data_hash,
        tool_hashes,
        caselist_hashes,
        deqp_args,
        identity_environment,
        result_semantics,
    )
    run_root = work_root / "runs" / execution_fingerprint[:16]
    report_root = run_root / "reports"

    manifest = {
        "version": 1,
        "created_utc": utc_now(),
        "run_fingerprint": execution_fingerprint,
        "runtime_fingerprint": fingerprint,
        "runtime_hashes": hashes,
        "tool_hashes": tool_hashes,
        "cts_data": {"path": str(cts_data_dir), "sha256": cts_data_hash},
        "runtime_dir": str(runtime_dir),
        "mobilegl": git_snapshot(repo_root),
        "vk_gl_cts": git_snapshot(cts_source),
        "configuration": configuration,
        "generator": args.generator,
        "architecture": args.architecture,
        "backends": backends,
        "versions": versions,
        "caselists": {version: {"path": str(path), "sha256": caselist_hashes[version]} for version, path in caselists.items()},
        "deqp_args": deqp_args,
        "environment_overrides": extra_environment,
        "ambient_environment": ambient_environment,
        "identity_environment": identity_environment,
        "result_semantics": result_semantics,
        "execution_settings": execution_settings,
    }
    write_json(run_root / "manifest.json", manifest)
    print(f"[wgl_glcts_pipeline] runtime fingerprint: {fingerprint}")
    print(f"[wgl_glcts_pipeline] run fingerprint: {execution_fingerprint}")
    print(f"[wgl_glcts_pipeline] run root: {run_root}")

    identities: list[dict[str, object]] = []
    if not args.skip_preflight:
        first_caselist = caselists[versions[0]]
        preflight_root = run_root / "preflight"
        for backend in backends:
            identities.append(
                run_preflight(
                    runner=runner,
                    runtime_exe=runtime_exe,
                    cts_modules_dir=cts_modules_dir,
                    caselist=first_caselist,
                    preflight_root=preflight_root,
                    backend=backend,
                    idle_timeout=args.idle_timeout,
                    environment=extra_environment,
                    deqp_args=deqp_args,
                    minimum_version=minimum_version,
                    run_identity=execution_fingerprint,
                )
            )
        manifest["preflight"] = identities
        write_json(run_root / "manifest.json", manifest)

    suites: list[tuple[str, str, Path, Path]] = []
    suite_errors: list[dict[str, object]] = []
    for backend in backends:
        for version in versions:
            label = f"gl{version}"
            result_dir = run_root / "results" / backend.lower() / label
            suites.append((backend, label, caselists[version], result_dir))
            if args.skip_run:
                continue
            result_dir.mkdir(parents=True, exist_ok=True)
            child_environment = dict(extra_environment)
            child_environment["MOBILEGL_LOG_FILE_PATH"] = str(result_dir / "mobilegl.log")
            command = runner_command(
                runner,
                runtime_exe,
                cts_modules_dir,
                caselists[version],
                result_dir,
                backend,
                args.idle_timeout,
                args.max_round_seconds,
                args.max_rounds,
                child_environment,
                deqp_args,
                execution_fingerprint,
            )
            completed = run_command(command, cwd=repo_root, check=False)
            if completed.returncode != 0:
                suite_errors.append({"backend": backend, "suite": label, "returncode": completed.returncode})
                if not args.continue_on_suite_error:
                    break
        if suite_errors and not args.continue_on_suite_error:
            break

    report_returncode: Optional[int] = None
    if not args.skip_report:
        report_root.mkdir(parents=True, exist_ok=True)
        completed = run_command(
            report_command(
                reporter,
                suites,
                report_root / "gl-cts-summary.md",
                report_root / "gl-cts-summary.json",
                args.allow_incomplete_report or bool(suite_errors),
                execution_fingerprint,
            ),
            cwd=repo_root,
            check=False,
        )
        report_returncode = completed.returncode

    manifest["suite_errors"] = suite_errors
    manifest["report_returncode"] = report_returncode
    manifest["finished_utc"] = utc_now()
    write_json(run_root / "manifest.json", manifest)
    if suite_errors:
        print(f"[wgl_glcts_pipeline] {len(suite_errors)} suite runner(s) incomplete; see manifest/report", file=sys.stderr)
        returncodes = {int(item["returncode"]) for item in suite_errors}
        if 130 in returncodes:
            return 130
        if 2 in returncodes:
            return 2
        if 3 in returncodes:
            return 3
        return 4
    if report_returncode is not None and report_returncode != 0:
        print(f"[wgl_glcts_pipeline] report validation failed with exit code {report_returncode}", file=sys.stderr)
        return report_returncode
    return 0


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        return execute(args)
    except PipelineError as exc:
        print(f"[wgl_glcts_pipeline] ERROR: {exc}", file=sys.stderr)
        return 2
    except OSError as exc:
        print(f"[wgl_glcts_pipeline] ERROR: filesystem/process operation failed: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
