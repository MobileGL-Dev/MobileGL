#!/bin/bash
# wsl-test.sh <targets...> - run INSIDE WSL archlinux: configure (once) and build MobileGL unit-test
# targets for the host (clang, disaggregated + in-process server ON).  From Git Bash:
#   MSYS2_ARG_CONV_EXCL='*' wsl -d archlinux -- bash /mnt/c/<worktree>/.claude/skills/anland-mobilegl-plasma/scripts/wsl-test.sh MultiSessionTest
# then ctest in $B (see wsl-all.sh).  Env: W (worktree, WSL path; default: derived from this file),
# B (build dir, default ~/mgl-anl-bld).  The Windows MSVC build of feat/disaggregated is broken; use this.
set -e
W=${W:-$(cd "$(dirname "$0")/../../../.." && pwd)}
B=${B:-$HOME/mgl-anl-bld}
if [ ! -f $B/build.ninja ]; then
cmake -S $W -B $B -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DMOBILEGL_BUILD_TEST=ON -DMOBILEGL_BUILD_DISAGGREGATED=ON -DMOBILEGL_BUILD_DISAGGREGATED_INPROC=ON -DMOBILEGL_BUILD_STAMP=wsl
fi
cmake --build $B --target "$@" -j16
