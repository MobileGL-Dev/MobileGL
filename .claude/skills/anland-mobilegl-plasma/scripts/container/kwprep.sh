#!/bin/bash
# kwprep.sh - (container, as root) bring the live KWin tree /root/mobilegl-build/kwin-6.7.4 in line with
# anland's kwin.patch + anland backend sources, touching ONLY files whose content changed (keeps the
# ninja build incremental).  It re-applies the whole patch to a pristine copy of KWin's src/
# (/root/gbm-dev/kwin-a/src) in /root/kp and copies every patched file that differs into the live tree,
# then copies changed backend files.  Build + install afterwards with anland's sync-build-kwin.sh.
#
# Input: /root/kwsync.tgz, a tar made in the anland worktree's producers/kde directory:
#   tar czf kwsync.tgz Arch_v5/kwin.patch Arch_v5/mobilegl-startup.sh anland_backend_Arch_v5/src/backends/anland/
# and pushed to the container's /root (e.g. via /mnt/Droidspaces/arch-kde-mgl/root/ on the Android side).
# The pristine tree /root/gbm-dev/kwin-a/src was set up by hand earlier (unverified that it is exactly
# upstream 6.7.4); patch hunks that fail are reported by `patch`.
set -e
rm -rf /root/kwsync && mkdir -p /root/kwsync && tar xzf /root/kwsync.tgz -C /root/kwsync
P=/root/kwsync/Arch_v5/kwin.patch
rm -rf /root/kp && mkdir -p /root/kp && cp -a /root/gbm-dev/kwin-a/src /root/kp/
cd /root/kp && patch -p1 -s < $P
LIVE=/root/mobilegl-build/kwin-6.7.4
FILES=$(grep -E '^\+\+\+ b/' $P | sed 's|^+++ b/||' | awk '{print $1}' | sort -u)
for f in $FILES; do
  if [ -f "/root/kp/$f" ] && ! cmp -s "/root/kp/$f" "$LIVE/$f"; then cp "/root/kp/$f" "$LIVE/$f"; echo "updated $f"; fi
done
for f in /root/kwsync/anland_backend_Arch_v5/src/backends/anland/*; do
  # libdisplay_producer is a symlink in git; a Windows checkout turns it into a small text file. The live
  # tree has the real directory (repo-root libdisplay_producer/), which neither script syncs.
  [ -f "$f" ] && [ "$(basename $f)" != libdisplay_producer ] || continue
  b=$(basename $f); if ! cmp -s "$f" "$LIVE/src/backends/anland/$b"; then cp "$f" "$LIVE/src/backends/anland/$b"; echo "updated anland/$b"; fi
done
echo PREP_DONE
