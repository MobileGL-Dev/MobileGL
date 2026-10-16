#!/usr/bin/env bash
# Pin the lavapipe ICD like pipe's build-split (it forces the three iterationRP repairs into the Vulkan
# test env), rebuild, rerun the one lane that differed.
set -e
T=/home/swung/w7/p12-onscreen
export MOBILEGL_FLATC_EXECUTABLE=/home/swung/w7/flatc-build/flatc
unset DISPLAY WAYLAND_DISPLAY
cmake -S "$T" -B "$T/build-split" -DMOBILEGL_ITEST_VK_ICD=/usr/share/vulkan/icd.d/lvp_icd.json > /dev/null
cmake -S "$T" -B "$T/build-linux" -DMOBILEGL_ITEST_VK_ICD=/usr/share/vulkan/icd.d/lvp_icd.json > /dev/null
ninja -C "$T/build-split" -j 24 | tail -1
cd "$T/build-split"
ctest -L '^integration-magma-full-split$' -j 8 2>&1 | grep -E 'tests passed|tests failed'
