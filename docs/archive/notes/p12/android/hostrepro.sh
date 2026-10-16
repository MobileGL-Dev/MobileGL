#!/usr/bin/env bash
# hostrepro.sh <libdir> <outdir> [case] [backend] [runs]: the in-process server on the host (no display),
# then <runs> sequential offscreen replays of <case> against it. Reproduces cross-session state leaks.
set -uo pipefail
LIBDIR=$1; OUT=$2; CASE=${3:-OpenRA}; BACKEND=${4:-DirectGLES}; RUNS=${5:-2}
TREE=/home/swung/w7/p12-onscreen
FIX=$TREE/tools/trace_replay/fixtures
REPLAY=${REPLAY:-/home/swung/w7/logs/p7w7/p12w1-984db455/host/mobilegl_trace_replay}
EP=tcp://127.0.0.1:41999
TOKEN=p12-host-repro-token-0001
rm -rf "$OUT"; mkdir -p "$OUT"
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json
MOBILEGL_IPC_TOKEN=$TOKEN MOBILEGL_LOG_FILE_PATH=$OUT/server.log ${SERVER_ENV:-} \
    python3 /home/swung/w7/notes/p12/android/host-inproc-server.py "$LIBDIR/libMobileGL.so" "$EP" > "$OUT/server.out" 2>&1 &
SERVER=$!
sleep 1.5
CASES=${CASES:-$(for _ in $(seq 1 "$RUNS"); do printf '%s ' "$CASE"; done)}
n=0
for CASE in $CASES; do
    n=$((n + 1))
read -r archive trace golden target w h cx cy cw ch ssim < <(python3 - "$TREE/tools/trace_replay/trace_cases.json" "$CASE" <<'EOF'
import json, sys
m = json.load(open(sys.argv[1])); d = m["defaults"]
c = next(c for c in m["cases"] if c["name"] == sys.argv[2])
g = lambda k: c.get(k, d.get(k))
print(g("trace_archive"), g("trace_file"), g("golden"), g("target_call"), g("width"), g("height"),
      g("crop_x"), g("crop_y"), g("crop_width"), g("crop_height"), g("ssim_threshold"))
EOF
)
    echo "run$n: $CASE"
    d=$OUT/run$n; mkdir -p "$d/input" "$d/output"; tar xzf "$FIX/$archive" -C "$d/input"
    env -u DISPLAY -u WAYLAND_DISPLAY MOBILEGL_TRANSPORT=spawn MOBILEGL_IPC_CONTROL=$EP MOBILEGL_IPC_DATA=stream \
        MOBILEGL_IPC_TOKEN=$TOKEN MOBILEGL_IPC_LOG_FORWARD=1 MOBILEGL_LOG_FILE_PATH=$d/output/mobilegl.log ${CLIENT_ENV:-} \
        timeout 600 "$REPLAY" --trace "$d/input/$trace" --golden "$FIX/$golden" --diff "$d/output/diff.png" \
        --output "$d/output" --backend "$BACKEND" --mobilegl-library "$LIBDIR/libMobileGL.so" --target-call "$target" \
        --width "$w" --height "$h" --ssim-threshold "$ssim" --crop-x "$cx" --crop-y "$cy" --crop-width "$cw" \
        --crop-height "$ch" ${SURFACE_FLAG:---pbuffer-surface} > "$d/replay.log" 2>&1
    echo "run$n rc=$? $(python3 -c "import json;r=json.load(open('$d/output/result.json'));print(r['passed'],r.get('ssim'))" 2>/dev/null)"
    grep -hcE "Program linking failed" "$d/output/mobilegl.server.log" 2>/dev/null | sed "s/^/  link failures: /"
    rm -rf "$d/input"
done
kill -TERM $SERVER; wait $SERVER
tail -3 "$OUT/server.out"
