#!/bin/sh
# MobileGL - android-plugin/trace-replay-shard.sh
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

# Replays an ordered list of trace cases for one (lane, transport, backend) on one emulator
# boot, each through trace-replay-ci.sh. The loop, the per-case settings and the verdict files
# are in trace_replay_shard.py; `--help` lists the arguments.
#
#   sh android-plugin/trace-replay-shard.sh \
#     --apk-file FILE --backend DirectGLES|DirectVulkan --gpu GPU \
#     --transport monolith|inproc|spawn --lane LANE \
#     --cases-env VARIABLE | --cases-file FILE | --cases-json JSON \
#     [--deadline-seconds N] [--dry-run | --finalize]
#
# Environment:
#   AVD_NAME, EMULATOR_LOG, EMULATOR_PID_FILE   the emulator to restart when it stops answering
#   MOBILEGL_GPU_HANG_BUDGET_MS                 forwarded to every replay (required)
#   MOBILEGL_SHARD_RESTART_EMULATOR_EACH_CASE   1 cold boots the emulator before every case
#   MOBILEGL_SHARD_POLL_INTERVAL                seconds between result polls (default 0.5);
#                                               empty selects trace-replay-ci.sh's 1 s loop
set -eu

exec "${PYTHON:-python3}" "$(dirname "$0")/trace_replay_shard.py" "$@"
