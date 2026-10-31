#!/bin/bash
# MobileGL - scripts/ci/split_reuse_build_type.sh
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

# The split consumers' build-type check. A consumer that reuses build-linux's build (SPLIT_REUSE is
# true: build-guard found the two configurations identical) takes it only when build-linux built
# Release, the only type build-guard can find identical to the split line. A re-run of failed jobs
# with debug logging rebuilds build-linux as Debug but keeps build-guard's verdict, while
# build-linux-split built nothing, so the consumer stops here and says so.
#
# usage: split_reuse_build_type.sh               (SPLIT_REUSE and PRIMARY_BUILD_TYPE from the job)
#        split_reuse_build_type.sh --self-test
set -euo pipefail

if [ "${1:-}" = --self-test ]; then
  failures=0
  expect() {
    local want="$1" reuse="$2" type="$3" got=0
    SPLIT_REUSE="${reuse}" PRIMARY_BUILD_TYPE="${type}" bash "$0" > /dev/null || got=$?
    if [ "${got}" -ne "${want}" ]; then
      echo "NOT OK  reuse=${reuse} build type '${type}': exit ${got}, expected ${want}"
      failures=$((failures + 1))
    fi
  }
  expect 0 true Release
  expect 1 true Debug
  expect 1 true ''
  expect 0 false Debug
  expect 0 '' ''
  if [ "${failures}" -ne 0 ]; then
    echo "split_reuse_build_type self-test: FAILED"
    exit 1
  fi
  echo "split_reuse_build_type self-test: ok"
  exit 0
fi

if [ "${SPLIT_REUSE:-false}" = true ] && [ "${PRIMARY_BUILD_TYPE:-}" != Release ]; then
  echo "::error::build-guard found the two configurations identical as Release builds, but build-linux built '${PRIMARY_BUILD_TYPE:-an unknown type}' (a re-run of failed jobs with debug logging keeps the guard's verdict) and build-linux-split built nothing; re-run all jobs"
  exit 1
fi
