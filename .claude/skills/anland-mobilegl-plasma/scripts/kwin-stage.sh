#!/usr/bin/env bash
# kwin-stage.sh [build] - stage anland's KWin changes (kwin.patch, anland backend sources,
# mobilegl-startup.sh, sync-build-kwin.sh) for the in-container KWin rebuild; with "build", also run
# kwprep.sh (apply changed patch hunks / backend files to the live tree) and start sync-build-kwin.sh
# (ninja -j4 + atomic install into /opt/mobilegl/kwin, screencast plugin, startup script) in the
# background, log /root/kwin-build.log.  KWin is NOT restarted: run device/run-plasma.sh afterwards.
# Poll:  printf 'tail -5 /root/kwin-build.log\n' | bash ct.sh kw
#
# Two staging places, because the two container scripts read different ones:
#   /root/kwsync.tgz                            <- kwprep.sh (installed by push-tools.sh)
#   $DEV_SHARE/kwin-sync = /run/anland-mobilegl/kwin-sync  <- sync-build-kwin.sh (as kwin-sync/sync-build.sh)
# All files are CR-stripped: the Windows checkout of anland has CRLF backend sources and a CRLF
# sync-build-kwin.sh (bash then fails on $'\r').
# Assembled from kwprep.sh + sync-build-kwin.sh; not exercised as one unit yet.
# Env: ANLAND_WORKTREE (anland checkout on branch legacy-mobilegl-unified, Windows form),
#      CHANNEL (ct.sh channel, default kw).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
. "$here/env.sh"
: "${ANLAND_WORKTREE:=$(cygpath -m "$USERPROFILE" 2>/dev/null)/AndroidStudioProjects/anland/.claude/worktrees/mgl-unified}"
K=$ANLAND_WORKTREE/producers/kde
st=$ANL_DIR/kwstage
rm -rf "$st"; mkdir -p "$st/kwsync/Arch_v5" "$st/kwsync/anland_backend_Arch_v5/src/backends/anland" "$st/kwin-sync/anland" "$st/kwin-sync/misc"
lf() { tr -d '\r' < "$1" > "$2"; }
for f in "$K"/anland_backend_Arch_v5/src/backends/anland/*; do
    b=$(basename "$f"); [ "$b" = libdisplay_producer ] && continue
    lf "$f" "$st/kwsync/anland_backend_Arch_v5/src/backends/anland/$b"
    lf "$f" "$st/kwin-sync/anland/$b"
done
for f in kwin.patch mobilegl-startup.sh desktop-session-mobilegl.conf; do
    lf "$K/Arch_v5/$f" "$st/kwsync/Arch_v5/$f"
    lf "$K/Arch_v5/$f" "$st/kwin-sync/misc/$f"
done
lf "$K/Arch_v5/sync-build-kwin.sh" "$st/kwin-sync/sync-build.sh"
(cd "$st/kwsync" && tar --force-local -czf ../kwsync.tgz Arch_v5 anland_backend_Arch_v5)
tar --force-local -C "$st" -czf "$st/kwin-sync.tgz" kwin-sync
adb_ push "$st/kwsync.tgz" "$DEV_TOOLS/kwsync.tgz" >/dev/null
adb_ push "$st/kwin-sync.tgz" "$DEV_TOOLS/kwin-sync.tgz" >/dev/null
adb_ shell "su -c 'cp $DEV_TOOLS/kwsync.tgz $CT_ROOT/root/kwsync.tgz && tar xzf $DEV_TOOLS/kwin-sync.tgz -C $DEV_SHARE && chmod -R a+rX $DEV_SHARE/kwin-sync && rm -f $DEV_TOOLS/kwsync.tgz $DEV_TOOLS/kwin-sync.tgz && echo staged'"
if [ "${1:-}" = build ]; then
    bash "$here/ct.sh" "${CHANNEL:-kw}" <<'IN'
bash /root/kwprep.sh || exit 1
nohup bash /run/anland-mobilegl/kwin-sync/sync-build.sh > /root/kwin-build.log 2>&1 < /dev/null &
echo "kwin build started (pid $!), log /root/kwin-build.log; it ends with '== ALL DONE =='"
IN
fi
