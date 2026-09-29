#!/bin/bash
# P12 exit gate (a): FCL (the game, its own process) renders into the render server's on-screen
# window on the same phone; then the server is killed and the client must latch DEVICE LOST.
#
#   fcl_accept.sh [DirectGLES|DirectVulkan] [kill]
#
# Server = the trace APK's Render Server (MobileGLDisplayActivity, in-process display server, tcp
# loopback), started through the app's own screen (extras = the same prefill the screen takes).
# FCL     = com.tungsten.fcl.mgdebug.debug built from a tree that has the version setting
#           "keepRunningInBackground" (FCL pauses the game the moment its Activity leaves the
#           foreground, and the server window must be the one on screen). The setting must be ON in
#           FCL's config.json (versions' "global" block: "keepRunningInBackground": true).
#           MOBILEGL_* for the game come from /sdcard/FCL/mg_env.txt (FCLauncher reads it for the
#           built-in Espryt renderer, so the backend is picked THERE, not by the renderer picker).
# FCL's launcher auto-launches the selected instance after a countdown (no tap needed).
set -u
export MSYS_NO_PATHCONV=1
S="${SER:-2f7cbe2e}"; F=com.tungsten.fcl.mgdebug.debug; T=top.mobilegl.plugin.trace
BACKEND="${1:-DirectGLES}"; DOKILL="${2:-}"
A() { adb -s "$S" "$@"; }; Ash() { adb -s "$S" shell "$@" | tr -d '\r'; }
log() { echo "[$(date +%H:%M:%S)] $*"; }

A shell "am force-stop $F; am force-stop $T; input keyevent KEYCODE_WAKEUP; input keyevent 82; svc power stayon true"
A shell "printf 'MOBILEGL_BACKEND_TYPE=$BACKEND\nMOBILEGL_IPC_CONTROL=tcp://127.0.0.1:40613\nMOBILEGL_IPC_DATA=stream\nMOBILEGL_IPC_SURFACE=server\n' > /sdcard/FCL/mg_env.txt; echo spawn > /sdcard/FCL/mg_transport.txt"
A shell "run-as $F grep -c keepRunningInBackground files/config.json" | grep -q '^[1-9]' || { log "FATAL: keepRunningInBackground is not set in FCL's config.json"; exit 1; }
A logcat -c
# the server, through the Render Server screen's documented extras (same fields, same code path as the buttons)
A shell "am start -n $T/top.mobilegl.plugin.ServerControlActivity --es listen tcp://127.0.0.1:40613 --es token '' --es env 'MOBILEGL_BACKEND_TYPE=$BACKEND;MOBILEGL_PIPE_STATS=1' --ez onscreen true --ez start true" >/dev/null
for i in $(seq 1 30); do A logcat -d -s MobileGL:* | tr -d '\r' | grep -q 'in-process display server, display installed' && break; sleep 1; done
log "server $(Ash "pidof $T:mglwin") listening"
A shell "am start -n $F/com.tungsten.fcl.activity.SplashActivity" >/dev/null
for i in $(seq 1 60); do Ash "dumpsys activity activities | grep topResumedActivity | head -1" | grep -q JVMActivity && break; sleep 1; done
sleep 10   # let the game's Activity settle; then the server window goes to the front
A shell "am start -n $T/top.mobilegl.plugin.MobileGLDisplayActivity" >/dev/null
P=$(Ash "pidof $F")
for i in $(seq 1 90); do A logcat -d --pid="$P" | tr -d '\r' | grep -q 'surface=window' && break; sleep 2; done
log "client pid $P attached to the server window; letting it run 45 s"
sleep 45
log "$(A logcat -d --pid="$P" | tr -d '\r' | grep 'P65ClientPace' | tail -1 | grep -o 'present=[0-9]*')"
A exec-out screencap -p > "${OUT:-/tmp}/fcl-onscreen-$BACKEND.png" 2>/dev/null
if [ "$DOKILL" = kill ]; then
  SP=$(Ash "pidof $T:mglwin"); log "killing the server (pid $SP)"
  A shell "su -c 'kill -9 $SP'"; sleep 10
  log "FCL alive: $(Ash "pidof $F")   $(A logcat -d --pid="$P" | tr -d '\r' | grep -m1 'DEVICE LOST' | cut -c1-140)"
  log "DEVICE LOST lines: $(A logcat -d --pid="$P" | tr -d '\r' | grep -c 'DEVICE LOST')   dead-doorbell lines: $(A logcat -d --pid="$P" | tr -d '\r' | grep -c 'dead doorbell')"
fi
