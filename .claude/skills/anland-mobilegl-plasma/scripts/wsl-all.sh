#!/bin/bash
# wsl-all.sh - run INSIDE WSL archlinux after wsl-test.sh configured $B: build every *Test target except
# DirectVulkan*/Benchmark* and run the unit label.  NEVER run the DirectVulkan integration tests
# (they need a live device + manual intervention); never run two ctest suites at once.
B=${B:-$HOME/mgl-anl-bld}
cd $B
T=$(ninja -t targets all 2>/dev/null | grep -oE "^[A-Za-z]+Test:" | tr -d : | sort -u | grep -v -E "^(DirectVulkan|Benchmark)" | tr "\n" " ")
ninja -j14 MobileGLServer $T 2>&1 | grep -E "error:|FAILED" | head -10
ctest -L unit -j10 --timeout 300 -E "DirectVulkan" 2>&1 | tail -25
