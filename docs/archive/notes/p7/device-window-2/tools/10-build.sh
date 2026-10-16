#!/usr/bin/env bash
# 10-build.sh <pipe-sha> [<stamp>] [--apk-only|--host-only]
#
# Builds the window's two artefacts from ONE source commit and ONE explicit stamp:
#   <out>/apk/trace-<stamp>.apk   signed, debuggable, side-by-side id top.mobilegl.plugin.p7w1.trace
#   <out>/host/{libMobileGL.so,libMobileGLServer.so,mobilegl_trace_replay} + catalog.json
# (<out> = ~/w7/logs/p7w7/<stamp>; stamp defaults to p7w7-<sha8>). The same stamp on both ends is
# what MOBILEGL_IPC_REQUIRE_SAME_BUILD=1 checks, and the device server must be THIS build (F changed
# the wire fingerprint: an old server + new client only says `no Welcome (rc=6)`).
#
# It never builds in ~/w7/pipe (read-only for agents, and a gate needs its status clean): the source
# is a DETACHED worktree ~/w7/p7w7-tree-<sha8> at <pipe-sha>, submodules copied from the pipe
# checkout when their recorded commit matches, the glslang External tree copied, LFS fixtures
# smudged by the worktree checkout. That tree is also the tools tree the device steps run from.
# Recipe = ~/w7/logs/p7w6-apk-inner.sh + ~/w7/logs/p7w6host-build.sh, parameterised.
set -euo pipefail
. "$(dirname "$0")/lib.sh"

[ $# -ge 1 ] || die "usage: $0 <pipe-sha> [<stamp>] [--apk-only|--host-only]"
SHA=$(git -C "$W2_PIPE" rev-parse --verify "$1^{commit}") || die "unknown commit $1 in $W2_PIPE"
SHA8=${SHA:0:8}
STAMP=${2:-p7w7-$SHA8}
case "${2:-}" in --*) STAMP=p7w7-$SHA8;; esac
MODE=all
for a in "$@"; do case "$a" in --apk-only) MODE=apk;; --host-only) MODE=host;; esac; done
TREE=${W2_BUILD_TREE:-$HOME/w7/p7w7-tree-$SHA8}
OUT=$(w2_out "$STAMP")
mkdir -p "$OUT/apk" "$OUT/host"
t0=$(date +%s)
log "build sha=$SHA stamp=$STAMP tree=$TREE out=$OUT mode=$MODE"

# ---- source tree ---------------------------------------------------------------------------
if [ ! -e "$TREE/.git" ]; then
    log "creating detached worktree $TREE @ $SHA8"
    git -C "$W2_PIPE" worktree add --detach "$TREE" "$SHA" > "$OUT/worktree.log" 2>&1
fi
[ "$(git -C "$TREE" rev-parse HEAD)" = "$SHA" ] || die "$TREE is not at $SHA (refusing to move a tree someone may be using)"
# Submodules: copy the pipe checkout's copy when it is at the recorded commit (fast, offline);
# otherwise fall back to a real submodule update for that path.
git -C "$TREE" submodule status | while read -r st path _; do
    want=${st#[-+U ]}
    [ -z "$(ls -A "$TREE/$path" 2>/dev/null)" ] || continue
    have=$(git -C "$W2_PIPE/$path" rev-parse HEAD 2>/dev/null || true)
    if [ "$have" = "$want" ]; then
        rm -rf "$TREE/$path"
        cp -a "$W2_PIPE/$path" "$TREE/$path"
        rm -f "$TREE/$path/.git"      # the gitlink points into pipe's module store: never share it
        log "submodule $path: copied from pipe ($want)"
    else
        log "submodule $path: pipe has ${have:-nothing}, want $want - git submodule update"
        git -C "$TREE" submodule update --init --recursive -- "$path"
    fi
done
[ -d "$TREE/3rdparty/glslang/External/spirv-tools" ] \
    || cp -a "$W2_PIPE/3rdparty/glslang/External" "$TREE/3rdparty/glslang/"
if head -c 40 "$TREE/tools/trace_replay/fixtures/openra.tgz" | grep -q 'git-lfs'; then
    log "fixtures are LFS pointers - git lfs pull"
    git -C "$TREE" lfs pull
fi

# ---- APK -----------------------------------------------------------------------------------
if [ "$MODE" != host ]; then
    ta=$(date +%s)
    export JAVA_HOME=/usr/lib/jvm/java-21-openjdk ANDROID_HOME=$HOME/android-sdk ANDROID_SDK_ROOT=$HOME/android-sdk
    export PATH=$JAVA_HOME/bin:$PATH
    BT=$(ls -d "$HOME"/android-sdk/build-tools/* | sort -V | tail -1)
    SUFFIX=${STAMP%%-*}
    export MOBILEGL_BUILD_STAMP=$STAMP
    ( cd "$TREE"
      rm -rf android-plugin/app/.cxx .cxx build/intermediates/cxx android-plugin/app/build/outputs/apk/trace
      "$HOME/gradle-8.10.2/bin/gradle" --no-daemon -p android-plugin :app:assembleTraceRelease \
        -Pmobilegl.apkSuffix="$SUFFIX" -Pmobilegl.applicationIdSuffix=.p7w1 -Pmobilegl.debuggableRelease=true \
        -Pmobilegl.logLevel=MOBILEGL_LOG_LEVEL_INFO -Pmobilegl.pipePush=ON \
        -Pmobilegl.buildDisaggregated=ON -Pmobilegl.buildDisaggregatedInproc=ON \
        --parallel --max-workers 8 ) > "$OUT/apk/gradle.log" 2>&1 || { tail -40 "$OUT/apk/gradle.log"; die "gradle failed"; }
    unsigned="$TREE/android-plugin/app/build/outputs/apk/trace/release/MobileGL-plugin-trace-release-$SUFFIX.apk"
    [ -f "$unsigned" ] || die "gradle produced no $unsigned"
    APK="$OUT/apk/trace-$STAMP.apk"
    "$BT/apksigner" sign --ks "$HOME/.android/debug.keystore" --ks-pass pass:android --key-pass pass:android \
        --ks-key-alias androiddebugkey --out "$APK" "$unsigned"
    rm -rf "$TREE/android-plugin/app/.cxx"
    ( cd "$OUT/apk" && sha256sum "trace-$STAMP.apk" > apk.sha256 )
    rm -rf "$OUT/apk/lib" && mkdir -p "$OUT/apk/lib"
    unzip -o -q -j "$APK" lib/arm64-v8a/libMobileGL.so lib/arm64-v8a/libMobileGLServer.so -d "$OUT/apk/lib"
    n=$(strings "$OUT/apk/lib/libMobileGL.so" | grep -c -F "$STAMP" || true)
    [ "$n" -ge 1 ] || die "stamp $STAMP not found in the APK's libMobileGL.so"
    ( cd "$OUT/apk/lib" && sha256sum libMobileGL.so libMobileGLServer.so > ../lib.sha256 )
    unzip -l "$APK" | grep -E 'libMobileGL(Server)?\.so' > "$OUT/apk/lib-listing.txt"
    log "APK $(stat -c %s "$APK") bytes sha256 $(cut -c1-16 "$OUT/apk/apk.sha256"), stamp strings in lib: $n"
    w2_timing "$OUT" build-apk "$ta" "$(date +%s)"
fi

# ---- host artefacts (the TCP client side; same stamp) ---------------------------------------
if [ "$MODE" != apk ]; then
    th=$(date +%s)
    cmake -S "$TREE" -B "$TREE/build-host" -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
      -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
      -DMOBILEGL_BUILD_DISAGGREGATED=ON -DMOBILEGL_BUILD_DISAGGREGATED_INPROC=ON \
      -DMOBILEGL_PIPE_PUSH=ON -DMOBILEGL_BUILD_TRACE_REPLAY=ON \
      -DMOBILEGL_BUILD_INTEGRATION_TEST=ON -DMOBILEGL_BUILD_TEST=ON \
      -DMOBILEGL_BUILD_STAMP="$STAMP" -DMOBILEGL_LOG_ACTIVE_LEVEL=MOBILEGL_LOG_LEVEL_INFO \
      -DMOBILEGL_ITEST_EGL_VENDOR=/usr/share/glvnd/egl_vendor.d/50_mesa.json \
      -DMOBILEGL_ITEST_VK_ICD=/usr/share/vulkan/icd.d/lvp_icd.json \
      -DMOBILEGL_ITEST_TCP_ENDPOINT=tcp://127.0.0.1:41317 \
      > "$OUT/host/configure.log" 2>&1 || { tail -30 "$OUT/host/configure.log"; die "host configure failed"; }
    cmake --build "$TREE/build-host" -j 8 --target MobileGL MobileGLServer mobilegl_trace_replay \
      > "$OUT/host/build.log" 2>&1 || { tail -30 "$OUT/host/build.log"; die "host build failed"; }
    cp "$TREE/build-host/libMobileGL.so" "$TREE/build-host/libMobileGLServer.so" \
       "$TREE/build-host/tools/trace_replay/mobilegl_trace_replay" "$OUT/host/"
    n=$(strings "$OUT/host/libMobileGL.so" | grep -c -F "$STAMP" || true)
    [ "$n" -ge 1 ] || die "stamp $STAMP not found in the host libMobileGL.so"
    ( cd "$OUT/host" && sha256sum libMobileGL.so libMobileGLServer.so mobilegl_trace_replay > ../host.sha256 )
    ( cd "$TREE/build-host" && ctest --show-only=json-v1 > "$OUT/host/catalog.json" )
    log "host artefacts in $OUT/host (stamp strings: $n)"
    w2_timing "$OUT" build-host "$th" "$(date +%s)"
fi

cat > "$OUT/build.env" <<EOF
BUILD_SHA=$SHA
BUILD_SHA8=$SHA8
BUILD_STAMP=$STAMP
BUILD_TREE=$TREE
BUILD_APK=$OUT/apk/trace-$STAMP.apk
EOF
w2_timing "$OUT" build "$t0" "$(date +%s)" "$MODE"
log "=== BUILD DONE $STAMP ==="
