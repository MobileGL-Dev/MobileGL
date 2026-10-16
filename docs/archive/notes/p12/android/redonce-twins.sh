#!/usr/bin/env bash
# Red-once for the twin drop: the call removed from ~BackendObject_DirectGLES must turn the unit
# case red, and the host sequential repro red again; the file is restored from a backup copy.
set -uo pipefail
cd /home/swung/w7/p12-onscreen || exit 1
export MOBILEGL_FLATC_EXECUTABLE=/home/swung/w7/flatc-build/flatc
F=MobileGL/MG_Backend/DirectGLES/BackendObject_DirectGLES.cpp
B=$(mktemp /tmp/p12-bodg-XXXX.cpp); cp "$F" "$B"
sed -i 's/if (MG_Config::Transport != MG_Config::TransportMode::Monolith) DropEveryTwinForEndedServerSession();/(void)0;/' "$F"
echo "mutation applied: $(grep -c '(void)0;' "$F") site(s)"
ninja -C build-split -j 24 SanityTest MobileGL 2>&1 | tail -1
echo "--- unit case WITHOUT the drop"
./build-split/MobileGL/MG_Test/SanityTest --gtest_filter='EsprytServerSession.*' 2>&1 | grep -E "Failure|OK \]|FAILED \]|still live" | head -8
echo "--- host sequential sessions WITHOUT the drop"
bash /home/swung/w7/notes/p12/android/hostrepro.sh /home/swung/w7/p12-onscreen/build-split /tmp/p12-redonce-twins OpenRA DirectGLES 2 2>&1 | grep -E "^run"
cp "$B" "$F"; rm -f "$B"
touch "$F"
ninja -C build-split -j 24 SanityTest MobileGL 2>&1 | tail -1
echo "--- restored"
./build-split/MobileGL/MG_Test/SanityTest --gtest_filter='EsprytServerSession.*' 2>&1 | grep -E "OK \]|FAILED \]"
git diff --stat
