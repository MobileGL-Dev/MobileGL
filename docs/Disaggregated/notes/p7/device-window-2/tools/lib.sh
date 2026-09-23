# Device window 2 (P7 closeout) - shared settings and helpers. Sourced, never run.
#
# Every setting is an overridable W2_* environment variable; the defaults are the Redmi
# 2f7cbe2e / p7w1-side-by-side-package facts recorded in docs/Disaggregated/notes/p7/
# device-window-1/00-session/README.md and HANDOFF-2026-09-22.md.

W2_HERE=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
W2_SERIAL=${W2_SERIAL:-2f7cbe2e}
W2_PKG=${W2_PKG:-top.mobilegl.plugin.p7w1.trace}
W2_TOKEN=${W2_TOKEN:-p7w1b-lan-token-20260922}          # >= 16 bytes (ServerMain token rule)
W2_LISTEN=${W2_LISTEN:-tcp://0.0.0.0:40613}
W2_PIPE=${W2_PIPE:-$HOME/w7/pipe}                      # integration tree: READ-ONLY here
export W2_LOGROOT=${W2_LOGROOT:-$HOME/w7/logs/p7w7}    # one directory per <stamp> below this
W2_CTS_BUNDLE=${W2_CTS_BUNDLE:-$HOME/w7/logs/cts-a64/deploy}   # glcts + mgprobe + BASE lib
W2_CTS_DEV=${W2_CTS_DEV:-/data/local/tmp/mgcts}
W2_IDLE_STATE=${W2_IDLE_STATE:-$HOME/.cache/mobilegl/tcp-server/${W2_SERIAL}-${W2_PKG}.json}
W2_LOCK=${W2_LOCK:-$HOME/w7/logs/p7w7-device-${W2_SERIAL}.lock}
export MOBILEGL_FLATC_EXECUTABLE=${MOBILEGL_FLATC_EXECUTABLE:-/home/swung/w7/flatc-build/flatc}

# The three gate-3 exclusions (CONTRACT-P7 7.1, E0-attribution/exclusions.md). They are red on
# the MONOLITH arm too, so they are not split defects; they are listed, never silently dropped.
W2_EXCLUDED="minecraft-1.21.1-neoforge-create-indirect-in-world minecraft-1.21.11-main-menu minecraft-1.21.4-fabric-iris-photon-v1.3b-in-world"
W2_ITERATIONRP=minecraft-1.21.4-fabric-iris-iterationrp-in-world
W2_BSL=minecraft-1.21.4-fabric-iris-bsl-esc-menu-854
# The split acceptance knobs run_android_retrace_local.py hands every split arm
# (SPLIT_ACCEPTANCE_KNOBS); CTS AFTER uses the same arm.
W2_SPLIT_KNOBS="MOBILEGL_IPC_ROLE_SPLIT_STATE=1 MOBILEGL_IPC_STRICT_ERRORS=1 MOBILEGL_IPC_RUN_AHEAD=1"

A() { adb -s "$W2_SERIAL" "$@"; }
Ash() { adb -s "$W2_SERIAL" shell "$@" | tr -d '\r'; }
log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }
die() { printf '[%s] FATAL: %s\n' "$(date +%H:%M:%S)" "$*" >&2; exit 1; }

# <stamp> -> the window's output directory, and the per-stamp facts 10-build.sh wrote.
w2_out() { printf '%s/%s\n' "$W2_LOGROOT" "$1"; }
w2_load_build() {
    local out; out=$(w2_out "$1")
    [ -f "$out/build.env" ] && . "$out/build.env"
    # Overrides for a run against an APK this window did not build (the dry run: p7w6).
    W2_APK=${W2_APK_OVERRIDE:-${BUILD_APK:-$out/apk/trace-$1.apk}}
    W2_TOOLS=${W2_TOOLS_OVERRIDE:-${BUILD_TREE:-}}
    [ -n "$W2_TOOLS" ] || die "no tools tree: run 10-build.sh first or set W2_TOOLS_OVERRIDE"
    [ -f "$W2_TOOLS/tools/trace_replay/run_android_retrace_local.py" ] || die "not a MobileGL tree: $W2_TOOLS"
    # The runner must be able to start from this (POSIX) host: the fix is p7/devprep fe4c8745.
    grep -q 'def bash_executable' "$W2_TOOLS/tools/trace_replay/run_android_retrace_local.py" \
        || die "$W2_TOOLS lacks the POSIX-host runner fix (cherry-pick p7/devprep fe4c8745)"
    grep -q 'def clear_repeat_outputs' "$W2_TOOLS/tools/trace_replay/run_android_retrace_local.py" \
        || die "$W2_TOOLS lacks the per-repeat output clear (cherry-pick p7/devprep fe4c8745)"
    [ -f "$W2_APK" ] || die "APK not found: $W2_APK"
}

w2_require_device() {
    local state; state=$(timeout 20 adb -s "$W2_SERIAL" get-state 2>/dev/null | tr -d '\r')
    [ "$state" = device ] || die "adb does not see $W2_SERIAL (state '$state'). WSL's adb is a client of the WINDOWS adb server (mirrored networking); if it is gone, run 'adb start-server' on Windows - never start one from WSL"
}

# One device user at a time: the runner's result root and the device are both single-tenant.
w2_lock() {
    mkdir -p "$(dirname "$W2_LOCK")"
    exec 9>"$W2_LOCK"
    flock -n 9 || die "another window-2 step holds $W2_LOCK"
}

w2_wake() {
    timeout 15 adb -s "$W2_SERIAL" shell input keyevent KEYCODE_WAKEUP >/dev/null 2>&1 || true
    timeout 15 adb -s "$W2_SERIAL" shell wm dismiss-keyguard >/dev/null 2>&1 || true
}

# pin_device.sh check, classified. PINNED = exit 0. The one tolerated DRIFT is the documented
# board fact (00-session/README.md): this unit's GPU really runs pwrlevel 0 at 1100 MHz, while
# pin_device.sh still expects 1050 - that is PINNED-1100, not a slipped pin. Anything else is
# recorded as-is and the arm's pin status is UNPROVEN.
w2_pin_check() {  # <tools-tree> <outfile>
    local out=$2 rc
    bash "$1/tools/device_bench/pin_device.sh" "$W2_SERIAL" check > "$out" 2>&1; rc=$?
    if [ $rc -eq 0 ]; then echo PINNED; return 0; fi
    if grep -q 'VERDICT: DRIFT' "$out" \
        && [ "$(grep -c '^    - ' "$out")" = 1 ] \
        && grep -q '^    - gpu pinned to pwrlevel 0 but gpuclk=1100000000 (expected 1050000000)' "$out"; then
        echo PINNED-1100; return 0
    fi
    grep -q 'VERDICT: UNPINNED' "$out" && { echo UNPINNED; return 1; }
    echo DRIFT; return 1
}

# No `| grep -q` anywhere under `set -o pipefail`: grep -q exits on the first match, the writer
# takes SIGPIPE and the pipeline reports 141 - a MATCH read as "not running" (the dry run's first
# preflight recorded a live supervisor as absent exactly this way).
w2_supervisor_running() {
    local ps; ps=$(Ash "ps -A -o ARGS" 2>/dev/null)
    case "$ps" in *"libMobileGLServer.so ${W2_LISTEN} --serve"*) return 0;; esac
    return 1
}

w2_timing() {  # <out> <step> <start-epoch> <end-epoch> [note]
    printf '%s\t%s\t%s\t%s\t%s\n' "$2" "$(date -d @"$3" +%FT%T)" "$(date -d @"$4" +%FT%T)" \
        "$(( $4 - $3 ))" "${5:-}" >> "$1/timing.tsv"
}
