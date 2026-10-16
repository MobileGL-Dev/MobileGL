#!/usr/bin/env bash
# hostkill.sh <libdir> <outdir> [case] [kill-after-s]: a replay against the host in-process server,
# whose process is SIGKILLed mid-session. The client must latch DEVICE LOST and end without a
# SIGABRT (rc 134).
set -uo pipefail
LIBDIR=$1; OUT=$2; CASE=${3:-minecraft-1.21.4-fabric-iris-bsl-esc-menu-854}; AFTER=${4:-6}
TREE=/home/swung/w7/p12-onscreen
FIX=$TREE/tools/trace_replay/fixtures
REPLAY=${REPLAY:-/home/swung/w7/logs/p7w7/p12w1-a4a940db/host/mobilegl_trace_replay}
EP=tcp://127.0.0.1:41998
TOKEN=p12-host-repro-token-0001
rm -rf "$OUT"; mkdir -p "$OUT"
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json
MOBILEGL_IPC_TOKEN=$TOKEN MOBILEGL_LOG_FILE_PATH=$OUT/server.log \
    python3 /home/swung/w7/notes/p12/android/host-inproc-server.py "$LIBDIR/libMobileGL.so" "$EP" > "$OUT/server.out" 2>&1 &
SERVER=$!
sleep 1.5
read -r archive trace golden target w h cx cy cw ch ssim < <(python3 - "$TREE/tools/trace_replay/trace_cases.json" "$CASE" <<'EOF'
import json, sys
m = json.load(open(sys.argv[1])); d = m["defaults"]
c = next(c for c in m["cases"] if c["name"] == sys.argv[2])
g = lambda k: c.get(k, d.get(k))
print(g("trace_archive"), g("trace_file"), g("golden"), g("target_call"), g("width"), g("height"),
      g("crop_x"), g("crop_y"), g("crop_width"), g("crop_height"), g("ssim_threshold"))
EOF
)
d=$OUT/run; mkdir -p "$d/input" "$d/output"; tar xzf "$FIX/$archive" -C "$d/input"
env -u DISPLAY -u WAYLAND_DISPLAY MOBILEGL_TRANSPORT=spawn MOBILEGL_IPC_CONTROL=$EP MOBILEGL_IPC_DATA=stream \
    MOBILEGL_IPC_TOKEN=$TOKEN MOBILEGL_IPC_LOG_FORWARD=1 MOBILEGL_LOG_FILE_PATH=$d/output/mobilegl.log \
    timeout 600 "$REPLAY" --trace "$d/input/$trace" --golden "$FIX/$golden" --diff "$d/output/diff.png" \
    --output "$d/output" --backend DirectGLES --mobilegl-library "$LIBDIR/libMobileGL.so" --target-call "$target" \
    --width "$w" --height "$h" --ssim-threshold "$ssim" --crop-x "$cx" --crop-y "$cy" --crop-width "$cw" \
    --crop-height "$ch" --pbuffer-surface > "$d/replay.log" 2>&1 &
CLIENT=$!
sleep "$AFTER"
kill -0 $CLIENT 2>/dev/null && echo "client live after ${AFTER}s: SIGKILL the server" || echo "WARNING: client already ended"
kill -KILL $SERVER
wait $CLIENT; RC=$?
echo "client rc=$RC $( [ $RC = 134 ] && echo '(SIGABRT)')"
grep -hE "DEVICE LOST|FATAL" "$d/output/mobilegl.client.log" | cut -c1-200 | head -4
rm -rf "$d/input"
