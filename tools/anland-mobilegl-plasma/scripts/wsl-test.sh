#!/bin/bash
# MobileGL - tools/anland-mobilegl-plasma/scripts/wsl-test.sh
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

# wsl-test.sh <targets...> - run INSIDE WSL archlinux: configure (once) and build MobileGL unit-test
# targets for the host (clang, disaggregated + in-process server ON).  From Git Bash:
#   MSYS2_ARG_CONV_EXCL='*' wsl -d archlinux -- bash /mnt/c/<repo>/tools/anland-mobilegl-plasma/scripts/wsl-test.sh MultiSessionTest
# then ctest in $B (see wsl-all.sh).  Env: W (worktree, WSL path; default: derived from this file),
# B (build dir, default ~/mgl-anl-bld).  The Windows MSVC build of feat/disaggregated is broken; use this.
set -e
W=${W:-$(cd "$(dirname "$0")/../../../.." && pwd)}
B=${B:-$HOME/mgl-anl-bld}
if [ ! -f $B/build.ninja ]; then
cmake -S $W -B $B -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DMOBILEGL_BUILD_TEST=ON -DMOBILEGL_BUILD_DISAGGREGATED=ON -DMOBILEGL_BUILD_DISAGGREGATED_INPROC=ON -DMOBILEGL_BUILD_STAMP=wsl
fi
cmake --build $B --target "$@" -j16
