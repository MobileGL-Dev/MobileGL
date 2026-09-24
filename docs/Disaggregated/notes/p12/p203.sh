#!/usr/bin/env bash
unset DISPLAY WAYLAND_DISPLAY
for t in pipe p12-onscreen; do
  echo "== $t"
  cd /home/swung/w7/$t/build-split || continue
  ctest -R "DirectVulkan.Split.Full.IterationRPProgram203Scenario" -V --timeout 300 2>&1 \
    | grep -E "SKIPPED|PASSED|FAILED|OK \]|Test command|MOBILEGL_|tests passed|tests failed" | head -14
done
