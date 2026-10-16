# Device window 2 (P7 closeout) - shared settings and helpers. Sourced, never run.
#
# Every setting is an overridable W2_* environment variable; the defaults are the Redmi
# 2f7cbe2e / p7w1-side-by-side-package facts recorded in docs/Disaggregated/notes/p7/
# device-window-1/00-session/README.md and HANDOFF-2026-09-22.md.

W2_HERE=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
W2_ARGV=("$@")   # the sourcing script's own arguments: w2_shared_lock re-runs it with them
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
# The device lock every agent of the wave holds while it touches the phone
# ('flock -w 3600 /home/swung/w7/locks/2f7cbe2e.lock <cmd>'); empty = do not take it (host tests).
W2_SHARED_LOCK=${W2_SHARED_LOCK-$HOME/w7/locks/${W2_SERIAL}.lock}
W2_SHARED_LOCK_WAIT=${W2_SHARED_LOCK_WAIT:-3600}
W2_SHARED_LOCK_RC=75   # a step's exit status when the shared lock never came free
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
    w2_shared_lock   # the first adb call of every device step already runs under the shared lock
    local state; state=$(timeout 20 adb -s "$W2_SERIAL" get-state 2>/dev/null | tr -d '\r')
    [ "$state" = device ] || die "adb does not see $W2_SERIAL (state '$state'). WSL's adb is a client of the WINDOWS adb server (mirrored networking); if it is gone, run 'adb start-server' on Windows - never start one from WSL"
}

# The wave's shared device lock ($W2_SHARED_LOCK): other agents run their device work under it, so
# the window must hold it too or it does not exclude them. window.sh takes it once, after the build,
# for every device step to the end (no other agent gets the phone between two steps); a step started
# on its own takes it for itself. Taken as `flock -o` does: this script is RE-RUN (same arguments) as
# the child of a flock process that keeps the lock, and the child does not carry the lock's fd - so
# nothing a step leaves running (an adb server, a supervisor launcher) keeps the phone locked after
# it. The re-run sees W2_SHARED_LOCK_HELD and goes on; everything before this call runs twice and
# must stay idempotent. Waits up to $W2_SHARED_LOCK_WAIT s, else exits $W2_SHARED_LOCK_RC.
w2_shared_lock() {
    [ -n "$W2_SHARED_LOCK" ] || return 0
    [ "${W2_SHARED_LOCK_HELD:-}" = "$W2_SHARED_LOCK" ] && return 0
    mkdir -p "$(dirname "$W2_SHARED_LOCK")"
    log "taking the shared device lock $W2_SHARED_LOCK (waits up to ${W2_SHARED_LOCK_WAIT}s while another agent holds $W2_SERIAL; exit $W2_SHARED_LOCK_RC if it never frees)"
    export W2_SHARED_LOCK_HELD=$W2_SHARED_LOCK
    exec flock -o -w "$W2_SHARED_LOCK_WAIT" -E "$W2_SHARED_LOCK_RC" "$W2_SHARED_LOCK" bash "$0" "${W2_ARGV[@]}"
}

# One device user at a time: the runner's result root and the device are both single-tenant.
# The shared lock first (it re-runs the step), then this window's own step lock.
w2_lock() {
    w2_shared_lock
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

# ---- one boot session (CONTRACT-P7 7.2; ruling: integrator 2026-09-23) ------------------------
# The phone's current boot_id ('' when adb cannot read it).
w2_boot_id() { timeout 20 adb -s "$W2_SERIAL" shell cat /proc/sys/kernel/random/boot_id 2>/dev/null | tr -d '\r\n '; }
# The window's session boot_id, as 21-preflight.sh recorded it.
w2_session_boot_id() {
    local f; f="$(w2_out "$1")/session/boot-id.txt"
    if [ -f "$f" ]; then tr -d '\r\n ' < "$f"; fi
}
# <session-boot-id> <where> [<current>]: die unless the phone is still in that boot session.
w2_same_session() {
    local now=${3-$(w2_boot_id)}
    [ -n "$now" ] || die "$2: cannot read the phone's boot_id (adb)"
    [ "$now" = "$1" ] || die "$2: the phone left this window's session (boot_id $1 -> $now; it rebooted). Gate 3 is a one-session reading: its finished pairs are void. RUNBOOK section 1 'phone rebooted' says what to delete before a new --reboot session"
}

# ---- the $BASE library back into mgcts ---------------------------------------------------------
# 50-cts-after.sh leaves the AFTER library in $W2_CTS_DEV; whoever uses mgcts next expects $BASE
# (device-window-1/CTS-base, the p7w1 monolith). Push the bundle's lib unless the device already has
# it, verify by sha256 on the device, print one status line. 0 = the device carries $BASE.
w2_restore_base_lib() {
    local want have got
    want=$(awk '/^libMobileGL.so  *sha256:/ {print $3}' "$W2_CTS_BUNDLE/IDENTITY.txt" 2>/dev/null)
    [ -n "$want" ] || { echo "mgcts lib : NOT restored - no \$BASE sha256 in $W2_CTS_BUNDLE/IDENTITY.txt"; return 1; }
    [ "$(Ash "test -d $W2_CTS_DEV && echo ok")" = ok ] || { echo "mgcts lib : no $W2_CTS_DEV on the device, nothing to restore"; return 0; }
    got=$(Ash "sha256sum $W2_CTS_DEV/libMobileGL.so 2>/dev/null" | cut -d' ' -f1)
    if [ "$got" = "$want" ]; then echo "mgcts lib : \$BASE $want (already)"; return 0; fi
    have=$(sha256sum "$W2_CTS_BUNDLE/libMobileGL.so" 2>/dev/null | cut -d' ' -f1)
    [ "$have" = "$want" ] || { echo "mgcts lib : NOT restored - bundle lib ${have:-missing} != \$BASE $want"; return 1; }
    A push "$W2_CTS_BUNDLE/libMobileGL.so" "$W2_CTS_DEV/libMobileGL.so" > /dev/null 2>&1
    local now; now=$(Ash "sha256sum $W2_CTS_DEV/libMobileGL.so 2>/dev/null" | cut -d' ' -f1)
    if [ "$now" = "$want" ]; then echo "mgcts lib : \$BASE $want restored (was ${got:-absent})"; return 0; fi
    echo "mgcts lib : RESTORE FAILED (${now:-absent} != \$BASE $want)"; return 1
}

w2_timing() {  # <out> <step> <start-epoch> <end-epoch> [note]
    printf '%s\t%s\t%s\t%s\t%s\n' "$2" "$(date -d @"$3" +%FT%T)" "$(date -d @"$4" +%FT%T)" \
        "$(( $4 - $3 ))" "${5:-}" >> "$1/timing.tsv"
}
