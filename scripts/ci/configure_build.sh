#!/bin/bash
# MobileGL - scripts/ci/configure_build.sh
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

# The configure lines of the two Linux library builds, in one place, so build-linux and
# build-linux-split run exactly these and build_fingerprint.py can configure either one as a probe.
#   primary  build-linux's: CI_BUILD_TYPE (default Release), MOBILEGL_CI_DISAGGREGATED (ON or OFF,
#            required) for the transport, the benchmarks, compile_commands.json.
#   split    build-linux-split's: Release, the transport and its in-process arm always on.
# Both default to Release at the INFO log level, like the shipped build. INFO is load-bearing: the
# library logs the resolved transport at INFO, and run_trace_case.cmake and the split lanes read
# that line back as proof the transport resolved in the process under test.
#
# usage: configure_build.sh <primary|split> <build dir>
#   with MOBILEGL_CI_CC, MOBILEGL_CI_CXX and VK_ICD_FILENAMES from setup-test-runner.
set -euo pipefail

variant="${1:?usage: configure_build.sh <primary|split> <build dir>}"
build_dir="${2:?usage: configure_build.sh <primary|split> <build dir>}"

common=(
  -S . -B "${build_dir}" -G Ninja
  -DCMAKE_C_COMPILER="${MOBILEGL_CI_CC}"
  -DCMAKE_CXX_COMPILER="${MOBILEGL_CI_CXX}"
  -DCMAKE_C_COMPILER_LAUNCHER=ccache
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
)

case "${variant}" in
  primary)
    disaggregated="${MOBILEGL_CI_DISAGGREGATED:?the primary build needs MOBILEGL_CI_DISAGGREGATED (ON or OFF)}"
    flags=(
      -DCMAKE_BUILD_TYPE="${CI_BUILD_TYPE:-Release}"
      -DMOBILEGL_LOG_ACTIVE_LEVEL=MOBILEGL_LOG_LEVEL_INFO
      -DMOBILEGL_BUILD_TEST=ON
      -DMOBILEGL_BUILD_BENCHMARK=ON
      -DMOBILEGL_BUILD_INTEGRATION_TEST=ON
      -DMOBILEGL_ITEST_VK_ICD="${VK_ICD_FILENAMES}"
      -DMOBILEGL_BUILD_TRACE_REPLAY=OFF
      -DMOBILEGL_BUILD_DISAGGREGATED="${disaggregated}"
      -DMOBILEGL_BUILD_DISAGGREGATED_INPROC="${disaggregated}"
      -DMOBILEGL_PIPE_VERIFY=OFF
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
      -DBENCHMARK_DOWNLOAD_DEPENDENCIES=ON
      -DBENCHMARK_ENABLE_TESTING=OFF
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5
    )
    ;;
  split)
    # _INPROC implies _DISAGGREGATED; both are passed, so neither depends on remembering that.
    flags=(
      -DCMAKE_BUILD_TYPE=Release
      -DMOBILEGL_LOG_ACTIVE_LEVEL=MOBILEGL_LOG_LEVEL_INFO
      -DMOBILEGL_BUILD_TEST=ON
      -DMOBILEGL_BUILD_BENCHMARK=OFF
      -DMOBILEGL_BUILD_INTEGRATION_TEST=ON
      -DMOBILEGL_ITEST_VK_ICD="${VK_ICD_FILENAMES}"
      -DMOBILEGL_BUILD_TRACE_REPLAY=OFF
      -DMOBILEGL_BUILD_DISAGGREGATED=ON
      -DMOBILEGL_BUILD_DISAGGREGATED_INPROC=ON
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5
    )
    ;;
  *)
    echo "configure_build.sh: unknown variant '${variant}' (primary or split)" >&2
    exit 2
    ;;
esac

cmake "${common[@]}" "${flags[@]}"
