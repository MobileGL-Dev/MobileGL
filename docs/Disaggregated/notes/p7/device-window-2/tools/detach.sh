#!/usr/bin/env bash
# detach.sh <logfile> <script> [args...]
#
# Starts one window-2 step so that it survives the terminal / the agent's 10-minute tool limit:
# a new session (setsid), immune to hangup (nohup), stdin from /dev/null, all output to <logfile>.
# Prints the pid and the log path, and returns at once. Poll with:  tail -f <logfile>
# Every step is resumable (per-case / per-block done markers), so re-running the same command
# after an interruption continues where it stopped.
set -euo pipefail
[ $# -ge 2 ] || { echo "usage: $0 <logfile> <script> [args...]" >&2; exit 64; }
logfile=$1; shift
mkdir -p "$(dirname "$logfile")"
setsid nohup bash "$@" > "$logfile" 2>&1 < /dev/null &
pid=$!
disown "$pid" 2>/dev/null || true
# Linger briefly: when the caller is `wsl.exe ... bash launcher.sh`, returning at once can tear the
# WSL session down before the child has left the caller's session (observed: a step that never
# wrote its first line). Two seconds, then say whether it is really running.
sleep 2
if kill -0 "$pid" 2>/dev/null; then
    echo "detached pid=$pid log=$logfile"
else
    echo "WARNING: pid $pid already exited; see $logfile" >&2
fi
