#!/usr/bin/env bash
# xbuild.sh [--reconfigure] [extra ninja targets...] - cross-build the container CLIENT: libMobileGL.so
# (glibc aarch64, glvnd EGL/GLX vendor) + mobilegl_gbm.so (GBM backend), in WSL archlinux with
# clang + lld against a sysroot exported from the container (xsysroot/refresh-sysroot.sh).
# Full build ~150 s, incremental seconds.  Deploy with xdeploy.sh.
# Env: MGL_WORKTREE (source, Windows form), MGL_XBUILD_DIR (WSL build dir, default ~/mgl-xbuild-a64),
#      MGL_JOBS (16), WSL_DISTRO (archlinux).
# Prereq in WSL: clang, lld, llvm (lld major MUST equal clang/llvm-libs major - lld 23 on llvm 22
# broke ld.lld), cmake, ninja; ~/sysroots/aarch64-arch.cmake + ~/sysroots/arch-kde-mgl.
set -euo pipefail
. "$(dirname "$0")/env.sh"
STAMP=$(git -C "$MGL_WORKTREE" rev-parse HEAD)
RECONF=0
if [ "${1:-}" = "--reconfigure" ]; then RECONF=1; shift; fi
EXTRA="$*"
SRC_WSL=$(win2wsl "$MGL_WORKTREE")
wsl -d "${WSL_DISTRO:-archlinux}" -- env SRC="$SRC_WSL" STAMP="$STAMP" RECONF="$RECONF" EXTRA="$EXTRA" \
    OUT="${MGL_XBUILD_DIR:-}" JOBS="${MGL_JOBS:-16}" bash -s <<'IN' 2>&1 | tr -d '\0' | grep -av '^wsl:'
set -euo pipefail
OUT=${OUT:-$HOME/mgl-xbuild-a64}
TC=$HOME/sysroots/aarch64-arch.cmake
[ -f "$TC" ] || { echo "missing toolchain $TC (run xsysroot/refresh-sysroot.sh)"; exit 1; }
ld.lld --version >/dev/null || { echo "ld.lld is not runnable in WSL (lld/llvm version mismatch?)"; exit 1; }
t0=$(date +%s)
if [ "$RECONF" = 1 ] || [ ! -f "$OUT/build.ninja" ]; then
    rm -rf "$OUT/CMakeCache.txt" "$OUT/CMakeFiles"
    cmake -S "$SRC" -B "$OUT" -G Ninja --toolchain "$TC" -DMOBILEGL_BUILD_STAMP="$STAMP" \
        -DCMAKE_BUILD_TYPE=Release -DMOBILEGL_BUILD_TEST=OFF -DMOBILEGL_BUILD_BENCHMARK=OFF \
        -DMOBILEGL_BUILD_DISAGGREGATED=ON -DMOBILEGL_ENABLE_LTO=OFF
elif ! grep -qx "MOBILEGL_BUILD_STAMP:STRING=$STAMP" "$OUT/CMakeCache.txt"; then
    cmake -S "$SRC" -B "$OUT" -DMOBILEGL_BUILD_STAMP="$STAMP" >/dev/null
fi
t1=$(date +%s)
cmake --build "$OUT" --target MobileGL mobilegl_gbm $EXTRA -j"$JOBS"
t2=$(date +%s)
echo "stamp:     $STAMP"
echo "configure: $((t1-t0))s   build: $((t2-t1))s"
for f in "$OUT/libMobileGL.so" "$OUT/MobileGL/MG_Gbm/mobilegl_gbm.so"; do
    ls -la "$f"; echo "  windows: $(wslpath -w "$f")"
done
IN
exit "${PIPESTATUS[0]}"
