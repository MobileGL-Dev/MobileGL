#!/usr/bin/env python3
"""Regenerate MobileGL/MG_Remote/Protocol/generated/protocol_generated.h from protocol.fbs.

flatc is a developer/CI tool ONLY: it is never part of the default build graph
(the earlier branch's Protocol/CMakeLists.txt:22-38 did add_subdirectory the
FlatBuffers tree and turned FLATBUFFERS_BUILD_FLATC ON when
MOBILEGL_FLATC_EXECUTABLE was unset, which is exactly the NDK trap it claimed
to avoid: cross-compiling an arm64 flatc and then trying to run it on the
host). The FlatBuffers runtime is header-only, so a build only needs
3rdparty/flatbuffers/include on the include path.

flatc resolution order:
  1. --flatc / MOBILEGL_FLATC_EXECUTABLE
  2. a flatc built once from the pinned 3rdparty/flatbuffers submodule into a
     directory OUTSIDE the repository (default: <repo>/../flatc-build,
     override with MOBILEGL_FLATC_BUILD_DIR)

A flatc found on PATH is deliberately NOT used: the generated header carries a
FLATBUFFERS_VERSION static_assert against the runtime headers, so a stray flatc
of another version produces a header that either fails to compile or churns the
committed file on every machine.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SCHEMA = REPO_ROOT / "MobileGL" / "MG_Remote" / "Protocol" / "protocol.fbs"
OUT_DIR = REPO_ROOT / "MobileGL" / "MG_Remote" / "Protocol" / "generated"
OUT_FILE = OUT_DIR / "protocol_generated.h"
SUBMODULE = REPO_ROOT / "3rdparty" / "flatbuffers"

LICENSE_HEADER = """\
// MobileGL - MobileGL/MG_Remote/Protocol/generated/protocol_generated.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// GENERATED FILE - DO NOT EDIT.
// Regenerate with `python3 scripts/gen_protocol.py` after changing
// MobileGL/MG_Remote/Protocol/protocol.fbs. CI's flatc-check step regenerates
// this file and fails on `git diff --exit-code`.
"""


def submodule_version() -> str | None:
    base = SUBMODULE / "include" / "flatbuffers" / "base.h"
    if not base.is_file():
        return None
    text = base.read_text(encoding="utf-8", errors="replace")
    parts = []
    for macro in ("FLATBUFFERS_VERSION_MAJOR", "FLATBUFFERS_VERSION_MINOR",
                  "FLATBUFFERS_VERSION_REVISION"):
        match = re.search(r"#\s*define\s+" + macro + r"\s+(\d+)", text)
        if not match:
            return None
        parts.append(match.group(1))
    return ".".join(parts)


def flatc_version(flatc: Path) -> str | None:
    try:
        out = subprocess.run([str(flatc), "--version"], check=True,
                             capture_output=True, text=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return None
    match = re.search(r"(\d+\.\d+\.\d+)", out)
    return match.group(1) if match else None


def build_flatc(build_dir: Path, jobs: int) -> Path:
    if not (SUBMODULE / "CMakeLists.txt").is_file():
        sys.exit(f"error: {SUBMODULE} is empty - run "
                 f"`git submodule update --init 3rdparty/flatbuffers`")
    exe_name = "flatc.exe" if os.name == "nt" else "flatc"
    for candidate in (build_dir / exe_name, build_dir / "Release" / exe_name):
        if candidate.is_file():
            return candidate

    build_dir.mkdir(parents=True, exist_ok=True)
    cmake = shutil.which("cmake")
    if cmake is None:
        sys.exit("error: cmake not found; needed to build flatc from the submodule")
    configure = [
        cmake, "-S", str(SUBMODULE), "-B", str(build_dir),
        "-DCMAKE_BUILD_TYPE=Release",
        "-DFLATBUFFERS_BUILD_FLATC=ON",
        "-DFLATBUFFERS_BUILD_FLATLIB=OFF",
        "-DFLATBUFFERS_BUILD_FLATHASH=OFF",
        "-DFLATBUFFERS_BUILD_TESTS=OFF",
        "-DFLATBUFFERS_INSTALL=OFF",
    ]
    if shutil.which("ninja"):
        configure += ["-G", "Ninja"]
    print("[gen_protocol] configuring flatc:", " ".join(configure), flush=True)
    subprocess.run(configure, check=True)
    print("[gen_protocol] building flatc", flush=True)
    subprocess.run([cmake, "--build", str(build_dir), "--target", "flatc",
                    "--config", "Release", "--parallel", str(jobs)], check=True)

    for candidate in (build_dir / exe_name, build_dir / "Release" / exe_name):
        if candidate.is_file():
            return candidate
    sys.exit(f"error: flatc was not produced under {build_dir}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--flatc", default=os.environ.get("MOBILEGL_FLATC_EXECUTABLE", ""),
                        help="path to a flatc binary (default: $MOBILEGL_FLATC_EXECUTABLE)")
    parser.add_argument("--build-dir",
                        default=os.environ.get("MOBILEGL_FLATC_BUILD_DIR", ""),
                        help="where to build flatc from the submodule "
                             "(default: <repo>/../flatc-build, kept out of the repo)")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--check", action="store_true",
                        help="fail if the committed header is not what flatc produces")
    parser.add_argument("--allow-version-mismatch", action="store_true",
                        help="proceed when flatc's version differs from the submodule's")
    args = parser.parse_args()

    if not SCHEMA.is_file():
        sys.exit(f"error: schema not found: {SCHEMA}")

    if args.flatc:
        flatc = Path(args.flatc)
        if not flatc.is_file():
            sys.exit(f"error: --flatc/{'MOBILEGL_FLATC_EXECUTABLE'} points at a "
                     f"missing file: {flatc}")
    else:
        build_dir = Path(args.build_dir) if args.build_dir else REPO_ROOT.parent / "flatc-build"
        flatc = build_flatc(build_dir.resolve(), args.jobs)

    have, want = flatc_version(flatc), submodule_version()
    print(f"[gen_protocol] flatc={flatc} version={have} submodule={want}", flush=True)
    if have and want and have != want and not args.allow_version_mismatch:
        sys.exit(f"error: flatc {have} does not match the pinned FlatBuffers runtime "
                 f"{want}; the generated header's version static_assert would fail. "
                 f"Unset MOBILEGL_FLATC_EXECUTABLE to build the pinned flatc, or pass "
                 f"--allow-version-mismatch.")

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    previous = OUT_FILE.read_bytes() if OUT_FILE.is_file() else None

    cmd = [str(flatc), "--cpp", "--cpp-std", "c++17", "-o", str(OUT_DIR), str(SCHEMA)]
    print("[gen_protocol]", " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True, cwd=str(REPO_ROOT))

    if not OUT_FILE.is_file():
        sys.exit(f"error: flatc did not produce {OUT_FILE}")

    body = OUT_FILE.read_text(encoding="utf-8")
    OUT_FILE.write_text(LICENSE_HEADER + "\n" + body, encoding="utf-8", newline="\n")

    if args.check and previous is not None and previous != OUT_FILE.read_bytes():
        sys.exit("error: committed protocol_generated.h is stale; rerun "
                 "scripts/gen_protocol.py and commit the result")

    print(f"[gen_protocol] wrote {OUT_FILE.relative_to(REPO_ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
