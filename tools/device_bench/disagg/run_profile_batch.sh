#!/usr/bin/env bash
# MobileGL - tools/device_bench/disagg/run_profile_batch.sh
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

# Batch profile runs for the MobileGL split-mode attribution report.
# Sequential: the device is exclusive. Each run = one arm with PROF=1.
set -u
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'
cd "$(dirname "$0")"

run() {
  local wl="$1" arm="$2" rep="$3" spin="${4:-}"
  echo "===== RUN wl=$wl arm=$arm rep=$rep spin=$spin $(date +%T) ====="
  if [ -n "$spin" ]; then
    PROF=1 ONE="$arm" WL="$wl" REP="$rep" \
      EXTRA_ENV=";MOBILEGL_IPC_SPIN_US=$spin" \
      EXTRA_SERVER_ENV="MOBILEGL_IPC_SPIN_US=$spin" \
      bash profile_matrix.sh one || echo "RUN FAILED $wl/$arm/$rep"
  else
    PROF=1 ONE="$arm" WL="$wl" REP="$rep" \
      bash profile_matrix.sh one || echo "RUN FAILED $wl/$arm/$rep"
  fi
}

run rd12 monolith 92
run rd12 inproc   93
run rd12 shm      94
run rd12 tcp      95 2000
run openra monolith 96
run openra inproc   97
echo "===== ALL DONE $(date +%T) ====="
