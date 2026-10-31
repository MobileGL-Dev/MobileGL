#!/usr/bin/env bash
# MobileGL - tools/anland-mobilegl-plasma/scripts/sync-src.sh
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

# sync-src.sh - FALLBACK path only (the WSL cross build, xbuild.sh, replaced it).  Pushes the files the
# worktree changed since the container's source stamp into the container's MobileGL source tree
# (/root/mobilegl-unified-src), keeping mtimes so the in-container ninja build stays incremental;
# then build/install inside the container with anland's anland-build-client.sh
# (producers/kde/Arch_v5/mobilegl-tools/, gcc, -j4, slow on the phone).
# A full re-sync of the tree is anland's anland-sync-source.sh (MOBILEGL_REPO=<worktree>).
set -euo pipefail
. "$(dirname "$0")/env.sh"
SRC=$CT_ROOT/root/mobilegl-unified-src
STAMP=$(adb_ shell "su -c 'cat $SRC/.anland-build-stamp'" | tr -d '\r\n')
cd "$MGL_WORKTREE"
FILES=$( (git diff --name-only --diff-filter=d "$STAMP"; git ls-files --others --exclude-standard) \
    | grep -v -E '^(3rdparty/|build-android|\.anland-build-stamp|\.claude/)' | sort -u)
echo "$(echo "$FILES" | wc -l) files since $STAMP"
tar --force-local -czf "$ANL_DIR/src-delta.tgz" $FILES
adb_ push "$ANL_DIR/src-delta.tgz" /data/local/tmp/src-delta.tgz >/dev/null
HEAD=$(git rev-parse HEAD)
adb_ shell "su -c 'tar xzf /data/local/tmp/src-delta.tgz -C $SRC && echo $HEAD > $SRC/.anland-build-stamp && echo synced'"
