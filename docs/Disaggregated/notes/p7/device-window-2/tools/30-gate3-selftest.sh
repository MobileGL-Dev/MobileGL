#!/usr/bin/env bash
# 30-gate3-selftest.sh [--script PATH]   (host only: no phone, no adb server, no device lock)
#
# Runs 30-gate3.sh (or PATH: red-once of an older driver) against a stub adb and a stub runner in a
# temp dir and checks its repeat bookkeeping (ID-P7-62: the monolith arm runs x --repeat, recorded
# as arms.txt monolith_repeat=) and its resume semantics:
#   fresh       --cases a,b: arms.txt says repeat=3 monolith_repeat=3; the runner is asked for the
#               monolith x3; every monolith pair archives 3 repeats and writes its .done; exit 0
#   resume      one monolith .done deleted: only that pair re-runs, again x3
#   incomplete  a monolith repeat without its PNG: no .done, progress.tsv INCOMPLETE, exit 3; the
#               same command again completes it
#   frozen      an arms.txt with monolith_repeat=3, resumed with --repeat 1: every arm keeps 3
#   old-format  an arms.txt without monolith_repeat= (a run the pre-ID-P7-62 script started): a
#               resume keeps that run's monolith x1
# Exit 0 when every check holds.
set -uo pipefail
HERE=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
SCRIPT="$HERE/30-gate3.sh"
[ "${1:-}" = --script ] && SCRIPT=$2
TMP=$(mktemp -d "${TMPDIR:-/tmp}/w2-gate3-selftest-XXXXXX")
trap 'rm -rf "$TMP"' EXIT
BOOT=11111111-2222-3333-4444-555555555555

# The driver under test beside this tree's lib.sh (it sources "$(dirname "$0")/lib.sh").
mkdir -p "$TMP/under-test" "$TMP/bin" "$TMP/tree/tools/trace_replay" "$TMP/tree/tools/device_bench" "$TMP/logs"
cp "$SCRIPT" "$TMP/under-test/30-gate3.sh"
cp "$HERE/lib.sh" "$TMP/under-test/lib.sh"
cat > "$TMP/bin/adb" <<'EOF'
#!/usr/bin/env bash
[ "${1:-}" = -s ] && shift 2
case "$*" in
    get-state) echo device;;
    "shell cat /proc/sys/kernel/random/boot_id") echo "$FAKE_BOOT";;
esac
exit 0
EOF
chmod +x "$TMP/bin/adb"
printf '#!/usr/bin/env bash\necho "  VERDICT: PINNED"\n' > "$TMP/tree/tools/device_bench/pin_device.sh"
cat > "$TMP/tree/tools/trace_replay/run_android_retrace_local.py" <<'EOF'
# Stub runner (lib.sh looks for: def bash_executable / def clear_repeat_outputs).
import argparse, json, os, pathlib
ap = argparse.ArgumentParser()
for flag in ("--case", "--backend", "--transport", "--archive-dir"):
    ap.add_argument(flag)
ap.add_argument("--repeat", type=int, default=1)
ap.add_argument("--env", action="append", default=[])
ap.add_argument("--use-pbuffer", action="store_true")
a = ap.parse_args()
with open(os.environ["FAKE_RUNLOG"], "a") as log:
    log.write("transport=%s repeat=%d case=%s env=%s\n" % (a.transport, a.repeat, a.case, ",".join(a.env)))
drop = os.environ.get("FAKE_DROP", "")  # <transport>:<case>:<repeat> archived without its PNG
arm = pathlib.Path(a.archive_dir) / (a.case + "-DirectVulkan")
for i in range(1, a.repeat + 1):
    rep = arm / ("repeat-%02d" % i)
    rep.mkdir(parents=True, exist_ok=True)
    (rep / "result.json").write_text(json.dumps({"passed": True, "ssim": 0.998}))
    if drop != "%s:%s:%d" % (a.transport, a.case, i):
        (rep / (a.case + "-DirectVulkan-actual.png")).write_bytes(b"png")
    print('"passed": true')
(pathlib.Path(a.archive_dir) / "run.json").write_text("{}")
EOF
: > "$TMP/fake.apk"
export PATH="$TMP/bin:$PATH" FAKE_BOOT=$BOOT FAKE_RUNLOG="$TMP/runner.log" W2_LOGROOT="$TMP/logs" W2_SHARED_LOCK= \
       W2_LOCK="$TMP/w2.lock" W2_TOOLS_OVERRIDE="$TMP/tree" W2_APK_OVERRIDE="$TMP/fake.apk" W2_SERIAL=stub0000
: > "$FAKE_RUNLOG"
# Never the real adb (it would reach the Windows adb server, or start one in WSL).
[ "$(command -v adb)" = "$TMP/bin/adb" ] || { echo "the stub adb is not first on PATH: $(command -v adb)" >&2; exit 2; }

TOTAL=0; FAILED=()
expect() {  # <name> <condition-rc> [detail]
    TOTAL=$((TOTAL + 1))
    if [ "$2" -eq 0 ]; then echo "  ok   $1"; else echo "  FAIL $1   <-- ${3:-}"; FAILED+=("$1"); fi
}
gate() {  # <stamp> [args...] -> the driver's exit status; its log in $TMP/<stamp>.<n>.log
    local stamp=$1; shift
    mkdir -p "$W2_LOGROOT/$stamp/session"
    echo "$BOOT" > "$W2_LOGROOT/$stamp/session/boot-id.txt"
    RUNS=$((RUNS + 1))
    bash "$TMP/under-test/30-gate3.sh" "$stamp" "$@" > "$TMP/$stamp.$RUNS.log" 2>&1
}
calls() { grep -c -- "$1" "$FAKE_RUNLOG" || true; }
reps() { find "$W2_LOGROOT/$1/gate3/archive/$2/$3-DirectVulkan" -maxdepth 1 -name 'repeat-*' 2>/dev/null | wc -l; }
RUNS=0
echo "30-gate3 self-test: driver $SCRIPT, stubs in $TMP"

echo "[fresh] a new run records monolith_repeat and runs the monolith x --repeat"
gate fresh --cases case-a,case-b; rc=$?
G="$W2_LOGROOT/fresh/gate3"
expect "exit 0" $rc "rc=$rc; $(tail -3 "$TMP/fresh.$RUNS.log")"
grep -qx 'monolith_repeat=3' "$G/arms.txt" && grep -qx 'repeat=3' "$G/arms.txt"
expect "arms.txt: repeat=3 and monolith_repeat=3" $? "$(cat "$G/arms.txt")"
[ "$(calls 'transport=monolith repeat=3 ')" = 2 ] && [ "$(calls 'transport=monolith repeat=1 ')" = 0 ]
expect "the runner is asked for the monolith x3 on both cases" $? "$(grep monolith "$FAKE_RUNLOG")"
[ "$(reps fresh monolith case-a)" = 3 ] && [ "$(reps fresh monolith case-b)" = 3 ]
expect "each monolith pair archives 3 repeats" $? "$(reps fresh monolith case-a)/$(reps fresh monolith case-b)"
[ "$(ls "$G"/state/*/*.done | wc -l)" = 8 ] && [ -f "$G/state/monolith/case-a.done" ]
expect "every (arm, case) pair has its .done (8)" $? "$(ls "$G"/state/*/)"
[ "$(calls 'transport=inproc repeat=3 ')" = 2 ] && [ "$(calls 'transport=spawn repeat=3 ')" = 2 ] \
    && [ "$(calls 'transport=inproc repeat=1 case=case-.* env=MOBILEGL_IPC_RUN_AHEAD=0')" = 2 ]
expect "the split arms keep x3 and ra0 x1" $? "$(cat "$FAKE_RUNLOG")"

echo "[resume] a deleted monolith .done re-runs that one pair, x3"
: > "$FAKE_RUNLOG"; rm "$G/state/monolith/case-a.done"
gate fresh; rc=$?
[ $rc -eq 0 ] && [ "$(wc -l < "$FAKE_RUNLOG")" = 1 ] && [ "$(calls 'transport=monolith repeat=3 case=case-a ')" = 1 ] \
    && [ -f "$G/state/monolith/case-a.done" ] && [ "$(reps fresh monolith case-a)" = 3 ]
expect "only monolith case-a re-ran, x3, and wrote its .done" $? "rc=$rc; $(cat "$FAKE_RUNLOG")"

echo "[incomplete] a monolith repeat without its PNG leaves the pair open"
: > "$FAKE_RUNLOG"
FAKE_DROP=monolith:case-b:2 gate incomplete --cases case-a,case-b; rc=$?
G="$W2_LOGROOT/incomplete/gate3"
[ $rc -eq 3 ] && [ ! -f "$G/state/monolith/case-b.done" ] && [ -f "$G/state/monolith/case-a.done" ] \
    && grep -q "^monolith	case-b	.*	INCOMPLETE$" "$G/progress.tsv"
expect "monolith case-b repeat-02 without a PNG -> no .done, progress INCOMPLETE, exit 3" $? "rc=$rc; $(cat "$G/progress.tsv" 2>/dev/null)"
: > "$FAKE_RUNLOG"
gate incomplete; rc=$?
[ $rc -eq 0 ] && [ -f "$G/state/monolith/case-b.done" ] && [ "$(wc -l < "$FAKE_RUNLOG")" = 1 ] \
    && [ "$(calls 'transport=monolith repeat=3 case=case-b ')" = 1 ]
expect "...the same command again re-runs that pair only (x3) and completes, exit 0" $? "rc=$rc; $(cat "$FAKE_RUNLOG")"

echo "[frozen] a resume keeps the first run's counts, whatever --repeat it is given"
G="$W2_LOGROOT/frozen/gate3"; mkdir -p "$G"
printf 'case-a\n' > "$G/cases.txt"
printf 'arms=monolith inproc spawn inproc-ra0\nrepeat=3\nmonolith_repeat=3\n' > "$G/arms.txt"
: > "$FAKE_RUNLOG"
gate frozen --repeat 1; rc=$?
[ $rc -eq 0 ] && [ "$(calls 'transport=monolith repeat=3 ')" = 1 ] && [ "$(calls 'transport=inproc repeat=3 ')" = 1 ] \
    && [ "$(calls 'transport=spawn repeat=3 ')" = 1 ]
expect "arms.txt repeat=3 monolith_repeat=3 resumed with --repeat 1 -> monolith, inproc, spawn x3" $? "rc=$rc; $(cat "$FAKE_RUNLOG")"

echo "[old-format] an arms.txt the pre-ID-P7-62 script froze (no monolith_repeat=) keeps its monolith x1"
G="$W2_LOGROOT/old/gate3"; mkdir -p "$G"
printf 'case-a\n' > "$G/cases.txt"
printf 'arms=monolith inproc spawn inproc-ra0\nrepeat=3\napk=x\ntools=y\n' > "$G/arms.txt"
: > "$FAKE_RUNLOG"
gate old; rc=$?
[ $rc -eq 0 ] && [ "$(calls 'transport=monolith repeat=1 ')" = 1 ] && [ "$(reps old monolith case-a)" = 1 ] \
    && [ -f "$G/state/monolith/case-a.done" ] && [ "$(calls 'transport=inproc repeat=3 ')" = 1 ] \
    && grep -q 'monolith repeat frozen at 1' "$TMP/old.$RUNS.log"
expect "monolith x1 with its .done, the split arms x3, and the log says why" $? "rc=$rc; $(cat "$FAKE_RUNLOG")"

echo "30-gate3 self-test: $((TOTAL - ${#FAILED[@]}))/$TOTAL checks passed${FAILED:+; FAILED: ${FAILED[*]}}"
[ ${#FAILED[@]} -eq 0 ]
