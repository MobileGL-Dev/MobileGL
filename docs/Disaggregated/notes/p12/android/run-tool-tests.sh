#!/usr/bin/env bash
# Runs the host-side tool tests the Android stage touches, in the package tree.
set -uo pipefail
cd /home/swung/w7/p12-onscreen || exit 1
export JAVA_HOME=/usr/lib/jvm/java-21-openjdk PATH=/usr/lib/jvm/java-21-openjdk/bin:$PATH
echo "--- test_android_lifecycle.py"
python3 tools/trace_replay/test_android_lifecycle.py; echo "rc=$?"
echo "--- tcp_lane_tools_test.py DeviceServerSurface + DeviceIdleExemption"
python3 -m pytest -q scripts/ci/tcp_lane_tools_test.py 2>/dev/null | tail -3 || true
python3 scripts/ci/tcp_lane_tools_test.py 2>&1 | tail -4; echo "rc=${PIPESTATUS[0]}"
