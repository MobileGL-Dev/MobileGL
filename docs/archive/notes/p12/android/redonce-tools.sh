#!/usr/bin/env bash
# Red-once for the Android stage's host-side tool tests: each mutation must turn its test red, and
# the working file is restored from a backup copy (never git checkout) afterwards.
set -uo pipefail
cd /home/swung/w7/p12-onscreen || exit 1
export JAVA_HOME=/usr/lib/jvm/java-21-openjdk PATH=/usr/lib/jvm/java-21-openjdk/bin:$PATH
B=$(mktemp -d /tmp/p12-redonce-XXXX)
run() { "$@" > "$B/out.txt" 2>&1; local rc=$?; tail -3 "$B/out.txt" | sed 's/^/      /'; echo "    rc=$rc"; }

echo "=== 1. tcp_device_server.py at base (no --surface): DeviceServerSurface must be red"
cp tools/trace_replay/tcp_device_server.py "$B/tds.py"
git show HEAD:tools/trace_replay/tcp_device_server.py > tools/trace_replay/tcp_device_server.py
run python3 scripts/ci/tcp_lane_tools_test.py DeviceServerSurface
cp "$B/tds.py" tools/trace_replay/tcp_device_server.py
echo "    restored; green again:"; run python3 scripts/ci/tcp_lane_tools_test.py DeviceServerSurface

echo "=== 2. ServerEnvironment.changes without unsets: ServerEnvironmentTest must be red"
F=android-plugin/app/src/trace/java/top/mobilegl/plugin/ServerEnvironment.java
cp "$F" "$B/se.java"
sed -i 's/if (!after.containsKey(name)) edits.put(name, null);/if (false) edits.put(name, null);/' "$F"
grep -c 'if (false) edits.put' "$F"
run python3 tools/trace_replay/test_android_lifecycle.py
cp "$B/se.java" "$F"

echo "=== 3. applyServerRole keeping the scrub list: ServerEnvironmentTest must be red"
sed -i 's/for (String name : SCRUBBED) env.remove(name);/for (String name : SCRUBBED) { }/' "$F"
grep -c 'SCRUBBED) { }' "$F"
run python3 tools/trace_replay/test_android_lifecycle.py
cp "$B/se.java" "$F"

echo "=== 4. the display Activity without android:process: the manifest check must be red"
M=android-plugin/app/src/trace/AndroidManifest.xml
cp "$M" "$B/manifest.xml"
sed -i '/android:process=":mglwin"/d' "$M"
run python3 tools/trace_replay/test_android_lifecycle.py
cp "$B/manifest.xml" "$M"

echo "=== restored: all green"
run python3 tools/trace_replay/test_android_lifecycle.py
git status --short
rm -rf "$B"
