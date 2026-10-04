#!/bin/bash
# wtest.sh build|run [ctest -R regex] - host unit tests for worktree $W in $B
W=${W:?set W to the MobileGL worktree as a WSL path}
B=${B:-$HOME/mgl-test-perf}
if [ "$1" = build ]; then
  if [ ! -f $B/build.ninja ]; then
    cmake -S $W -B $B -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
      -DMOBILEGL_BUILD_TEST=ON -DMOBILEGL_BUILD_DISAGGREGATED=ON -DMOBILEGL_BUILD_DISAGGREGATED_INPROC=ON -DMOBILEGL_BUILD_STAMP=wsl > $B.cfg.log 2>&1 || { tail -20 $B.cfg.log; exit 1; }
  fi
  cd $B
  T=$(ninja -t targets all 2>/dev/null | grep -oE "^[A-Za-z]+Test:" | tr -d : | sort -u | grep -v -E "^(DirectVulkan|Benchmark)" | tr "\n" " ")
  ninja -j12 MobileGLServer $T > $B.build.log 2>&1; rc=$?
  echo "build rc=$rc"; grep -E "error:|FAILED" $B.build.log | head -20
elif [ "$1" = run ]; then
  cd $B; shift
  ctest -L unit -j10 --timeout 300 -E "DirectVulkan" "$@" 2>&1 | tail -30
fi
