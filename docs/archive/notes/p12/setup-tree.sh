#!/usr/bin/env bash
# setup-tree.sh <slug> <base-sha> <itest-port>: package worktree ~/w7/p12-<slug> on branch p12/<slug>,
# submodules copied from pipe, build-split (disaggregated) + build-linux (pull, for G1) configured and built.
set -euo pipefail
SLUG=$1; BASE=$2; PORT=$3
PIPE=$HOME/w7/pipe; TREE=$HOME/w7/p12-$SLUG
export MOBILEGL_FLATC_EXECUTABLE=/home/swung/w7/flatc-build/flatc
if [ ! -e "$TREE/.git" ]; then git -C "$PIPE" worktree add -b "p12/$SLUG" "$TREE" "$BASE"; fi
git -C "$TREE" submodule status | while read -r st path _; do
  want=${st#[-+U ]}
  [ -z "$(ls -A "$TREE/$path" 2>/dev/null)" ] || continue
  have=$(git -C "$PIPE/$path" rev-parse HEAD 2>/dev/null || true)
  if [ "$have" = "$want" ]; then
    rm -rf "$TREE/$path"; cp -a "$PIPE/$path" "$TREE/$path"; rm -f "$TREE/$path/.git"; echo "copied $path"
  else
    echo "submodule update $path"; git -C "$TREE" submodule update --init --recursive -- "$path"
  fi
done
[ -d "$TREE/3rdparty/glslang/External/spirv-tools" ] || cp -a "$PIPE/3rdparty/glslang/External" "$TREE/3rdparty/glslang/"
# LFS fixtures: copy the hydrated copies from pipe (never git lfs pull; see lfs-fixture-policy).
git -C "$TREE" ls-files -z tools/trace_replay/fixtures | while IFS= read -r -d "" f; do
  if head -c 40 "$TREE/$f" 2>/dev/null | grep -q git-lfs && ! head -c 40 "$PIPE/$f" | grep -q git-lfs; then
    cp "$PIPE/$f" "$TREE/$f"
  fi
done
git -C "$TREE" ls-files -z tools/trace_replay/fixtures | xargs -0 -r git -C "$TREE" update-index --assume-unchanged
COMMON=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
        -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
        -DMOBILEGL_ITEST_VK_ICD=/usr/share/vulkan/icd.d/lvp_icd.json
        -DMOBILEGL_BUILD_TEST=ON -DMOBILEGL_BUILD_INTEGRATION_TEST=ON -DMOBILEGL_FORCE_RELEASE_OPT=ON -DMOBILEGL_BUILD_BENCHMARK=OFF)
cmake -S "$TREE" -B "$TREE/build-split" "${COMMON[@]}" -DMOBILEGL_BUILD_DISAGGREGATED=ON \
      -DMOBILEGL_BUILD_DISAGGREGATED_INPROC=ON -DMOBILEGL_PIPE_PUSH=ON \
      -DMOBILEGL_ITEST_TCP_ENDPOINT="tcp://127.0.0.1:$PORT" > "$TREE/cfg-split.log" 2>&1
cmake -S "$TREE" -B "$TREE/build-linux" "${COMMON[@]}" -DMOBILEGL_BUILD_DISAGGREGATED=OFF \
      -DMOBILEGL_BUILD_DISAGGREGATED_INPROC=OFF -DMOBILEGL_PIPE_PUSH=OFF > "$TREE/cfg-linux.log" 2>&1
set +e
ninja -C "$TREE/build-split" -j 24 > "$TREE/build-split.log" 2>&1; echo "split rc=$?"
ninja -C "$TREE/build-linux" -j 24 > "$TREE/build-linux.log" 2>&1; echo "linux rc=$?"
readelf -S -W "$TREE/build-linux/libMobileGL.so" | awk '$2==".text"{print "G1 base text:", $6}'
echo "=== SETUP DONE $TREE ==="
