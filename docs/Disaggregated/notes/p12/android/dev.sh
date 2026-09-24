# P12 android stage - device check helpers. Sourced, never run. Needs STAMP (p12w1-<sha8>).
# Every script that sources this re-runs itself under the shared device lock (flock -o, like
# window-2's lib.sh), so nothing it leaves running keeps the phone locked.
SERIAL=2f7cbe2e
PKG=top.mobilegl.plugin.p7w1.trace
TOKEN=p7w1b-lan-token-20260922      # >= 16 bytes
LISTEN=tcp://127.0.0.1:40613        # loopback on the phone, reached through adb forward
ENDPOINT=tcp://127.0.0.1:40613
: "${STAMP:?set STAMP=p12w1-<sha8>}"
BUILD_OUT=$HOME/w7/logs/p7w7/$STAMP
HOST=$BUILD_OUT/host
BUILD_TREE=$(. "$BUILD_OUT/build.env"; echo "$BUILD_TREE")
FIX=$BUILD_TREE/tools/trace_replay/fixtures
DEV=$HOME/w7/logs/p12/device
LOCK=$HOME/w7/locks/$SERIAL.lock

if [ "${P12_LOCK_HELD:-}" != 1 ]; then
    export P12_LOCK_HELD=1
    exec flock -o -w 3600 "$LOCK" bash "$0" "$@"
fi

A() { adb -s "$SERIAL" "$@"; }
Ash() { adb -s "$SERIAL" shell "$@" | tr -d '\r'; }
log() { printf '[%s] %s\n' "$(date +%T)" "$*"; }
devtime() { Ash "date +'%m-%d %H:%M:%S.000'"; }
wake() { Ash input keyevent KEYCODE_WAKEUP; Ash wm dismiss-keyguard > /dev/null 2>&1 || true; }
tds() { python3 "$BUILD_TREE/tools/trace_replay/tcp_device_server.py" "$@" --serial "$SERIAL" --package "$PKG" \
            --listen "$LISTEN" --token "$TOKEN"; }
force_stop() { Ash am force-stop "$PKG"; }
pid_of() { Ash "pidof $1" | awk '{print $1}'; }
ps_mobilegl() { Ash "ps -A -o PID,PPID,NAME,ARGS" | grep -E "mobilegl|MobileGL" | grep -v grep; }
logcat_since() { A logcat -d -v threadtime -T "$1"; }   # <devtime>
# mglines <logcat file>: only this package's own tags (MobileGL native lines, the Activity, the service).
mglines() { grep -E " [VDIWEF] (MobileGL|MobileGLDisplay|MobileGLServer) *:" "$1" | cut -c1-300; }

# wait_logcat <devtime> <extended-regex> <seconds>: 0 once logcat since <devtime> matches.
wait_logcat() {
    local since=$1 pattern=$2 limit=$3 n=0
    while [ $n -lt $((limit * 2)) ]; do
        if logcat_since "$since" 2>/dev/null | grep -Eq -- "$pattern"; then return 0; fi
        sleep 0.5; n=$((n + 1))
    done
    return 1
}

# case_args <case>: archive trace golden target width height crop_x crop_y crop_w crop_h
case_args() {
    python3 - "$BUILD_TREE/tools/trace_replay/trace_cases.json" "$1" <<'EOF'
import json, sys
m = json.load(open(sys.argv[1])); d = m["defaults"]
c = next(c for c in m["cases"] if c["name"] == sys.argv[2])
g = lambda k: c.get(k, d.get(k))
print(g("trace_archive"), g("trace_file"), g("golden"), g("target_call"), g("width"), g("height"),
      g("crop_x"), g("crop_y"), g("crop_width"), g("crop_height"), g("ssim_threshold"))
EOF
}

# replay <dir> <case> <backend> <window|pbuffer> [K=V ...]: one host retrace against ENDPOINT, the
# run_trace_case.cmake invocation plus the surface flag. Writes <dir>/{replay.log,rc,t0,t1,output/}.
replay() {
    local dir=$1 case=$2 backend=$3 surface=$4; shift 4
    local archive trace golden target w h cx cy cw ch ssim
    read -r archive trace golden target w h cx cy cw ch ssim < <(case_args "$case")
    rm -rf "$dir/input" "$dir/output"; mkdir -p "$dir/input" "$dir/output"
    tar xzf "$FIX/$archive" -C "$dir/input"
    local flag=--pbuffer-surface; [ "$surface" = window ] && flag=--window-surface
    date +%s.%N > "$dir/t0"
    env -u DISPLAY -u WAYLAND_DISPLAY -u MOBILEGL_IPC_SURFACE \
        MOBILEGL_TRANSPORT=spawn MOBILEGL_IPC_CONTROL="$ENDPOINT" MOBILEGL_IPC_DATA=stream \
        MOBILEGL_IPC_TOKEN="$TOKEN" MOBILEGL_IPC_REQUIRE_SAME_BUILD=1 MOBILEGL_IPC_LOG_FORWARD=1 \
        MOBILEGL_LOG_FILE_PATH="$dir/output/mobilegl.log" "$@" \
        timeout 900 "$HOST/mobilegl_trace_replay" --trace "$dir/input/$trace" --golden "$FIX/$golden" \
            --diff "$dir/output/$case-diff.png" --output "$dir/output" --backend "$backend" \
            --mobilegl-library "$HOST/libMobileGL.so" --target-call "$target" --width "$w" --height "$h" \
            --ssim-threshold "$ssim" --crop-x "$cx" --crop-y "$cy" --crop-width "$cw" --crop-height "$ch" \
            "$flag" > "$dir/replay.log" 2>&1
    local rc=$?
    date +%s.%N > "$dir/t1"
    echo "$rc" > "$dir/rc"
    rm -rf "$dir/input"
    return $rc
}

# shots <dir> <pid>: adb screencaps every ~1 s while <pid> lives -> <dir>/shots/NN.png + shots.tsv
shots() {
    local dir=$1 pid=$2 n=0
    mkdir -p "$dir/shots"
    while kill -0 "$pid" 2> /dev/null; do
        local t0 t1 f
        f=$(printf '%s/shots/%02d.png' "$dir" "$n")
        t0=$(date +%s.%N)
        A exec-out screencap -p > "$f" 2> /dev/null
        t1=$(date +%s.%N)
        printf '%s\t%s\t%s\n' "$n" "$t0" "$t1" >> "$dir/shots.tsv"
        n=$((n + 1))
        sleep 0.7
    done
}

# replay_with_shots <dir> <case> <backend> <surface> [K=V ...]
replay_with_shots() {
    local dir=$1
    mkdir -p "$dir"; : > "$dir/shots.tsv"
    replay "$@" &
    local pid=$!
    shots "$dir" "$pid" &
    local shooter=$!
    wait "$pid"; local rc=$?
    wait "$shooter" 2> /dev/null
    return $rc
}

# shot_report <dir>: per screencap, whether it was taken DURING the replay and how much of the
# screen is non-black; the best during-replay shot is copied (and a 540p copy made) as evidence.
shot_report() {
    python3 - "$1" <<'EOF'
import sys, pathlib
from PIL import Image
d = pathlib.Path(sys.argv[1])
t0 = float((d / "t0").read_text()); t1 = float((d / "t1").read_text())
best = None
rows = []
for line in (d / "shots.tsv").read_text().splitlines():
    n, a, b = line.split("\t"); a = float(a); b = float(b)
    f = d / "shots" / f"{int(n):02d}.png"
    try:
        im = Image.open(f).convert("L")
    except Exception as e:
        rows.append(f"{n} unreadable {e}"); continue
    small = im.resize((im.width // 4, im.height // 4))
    px = list(small.getdata())
    lit = sum(1 for p in px if p > 24) / len(px)
    mean = sum(px) / len(px)
    during = a >= t0 and b <= t1
    rows.append(f"{n} t=+{a - t0:6.2f}s..+{b - t0:6.2f}s during={during} nonblack={lit:.3f} mean={mean:.1f}")
    if during and (best is None or lit > best[1]):
        best = (f, lit, mean, a - t0)
print(f"replay ran {t1 - t0:.2f}s; {len(rows)} screencaps")
print("\n".join(rows))
if best:
    f, lit, mean, at = best
    ev = d / "evidence-during-replay.png"
    ev.write_bytes(f.read_bytes())
    im = Image.open(f); h = 540; w = max(1, im.width * h // im.height)
    im.resize((w, h)).save(d / "evidence-during-replay-540p.png")
    print(f"BEST during-replay screencap: {f.name} at +{at:.2f}s nonblack={lit:.3f} mean={mean:.1f} -> {ev}")
else:
    print("NO screencap was taken during the replay")
EOF
}

# result_summary <dir>: the runner's own verdict (result.json) and the arm proofs from the logs.
result_summary() {
    local dir=$1
    echo "rc=$(cat "$dir/rc" 2>/dev/null)"
    python3 - "$dir/output/result.json" <<'EOF' 2>/dev/null || echo "result.json: missing"
import json, sys
r = json.load(open(sys.argv[1]))
print("result.json:", {k: r.get(k) for k in ("passed", "statusCode", "ssim", "message", "matchedGoldenPath")})
EOF
    grep -hE "surface=(window|pbuffer)|control=tcp data=stream server=|owner=server|Refuse ServerOwned|NoServerDisplay|SurfaceModeMismatch|ServerWindowLost|device lost|DEVICE_LOST|Welcome" \
        "$dir"/output/mobilegl*.log 2>/dev/null | cut -c1-300 | sort | uniq -c | sort -rn | head -20
}
