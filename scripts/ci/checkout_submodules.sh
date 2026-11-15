#!/bin/bash
# MobileGL - scripts/ci/checkout_submodules.sh
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

# The submodules a CI build reads, checked out shallow and in parallel, in place of a
# recursive checkout of every submodule in the tree.
#   library  what the root CMakeLists.txt configures for the library, its tests and benchmarks.
#   retrace  the library set plus the trace replay tool's: apitrace and the eight directories
#            its thirdparty/CMakeLists.txt requires.
# Each row names a submodule and one file the build reads from it. A row whose file is missing
# after the update fails the script: the configure would otherwise go on without it, and for
# 3rdparty/flatbuffers the root CMakeLists.txt then builds a library without MG_Remote instead
# of stopping. A submodule the build starts to use needs a row here.
#
# usage: checkout_submodules.sh <library|retrace>      (from the repository root)
#        checkout_submodules.sh --list <library|retrace>   prints the rows, checks out nothing
set -euo pipefail

usage="usage: checkout_submodules.sh [--list] <library|retrace>"

list_only=false
if [ "${1:-}" = --list ]; then
  list_only=true
  shift
fi
set_name="${1:?${usage}}"

# <submodule path> <file the build reads from it>
library=(
  "3rdparty/glslang CMakeLists.txt"
  "3rdparty/SPIRV-Cross CMakeLists.txt"
  "3rdparty/SPIRV-Reflect CMakeLists.txt"
  "3rdparty/VulkanMemoryAllocator CMakeLists.txt"
  "3rdparty/Vulkan-Headers CMakeLists.txt"
  "3rdparty/Vulkan-Utility-Libraries CMakeLists.txt"
  "3rdparty/xxHash build/cmake/CMakeLists.txt"
  "3rdparty/asio include/asio.hpp"
  "3rdparty/flatbuffers include/flatbuffers/flatbuffers.h"
  "include/ska flat_hash_map.hpp"
)
retrace=(
  "3rdparty/apitrace thirdparty/CMakeLists.txt"
)
# Inside 3rdparty/apitrace: the files its thirdparty/CMakeLists.txt tests before it configures.
apitrace=(
  "thirdparty/brotli LICENSE"
  "thirdparty/gtest LICENSE"
  "thirdparty/libbacktrace LICENSE"
  "thirdparty/libpng LICENSE"
  "thirdparty/snappy COPYING"
  "thirdparty/zlib README"
  "thirdparty/zstd LICENSE"
  "thirdparty/directxmath LICENSE"
)

case "${set_name}" in
  library) top=("${library[@]}"); nested=() ;;
  retrace) top=("${library[@]}" "${retrace[@]}"); nested=("${apitrace[@]}") ;;
  *)
    echo "checkout_submodules.sh: unknown set '${set_name}' (library or retrace)" >&2
    echo "${usage}" >&2
    exit 2
    ;;
esac

if [ "${list_only}" = true ]; then
  printf '%s\n' "${top[@]}"
  if [ "${#nested[@]}" -gt 0 ]; then
    printf '3rdparty/apitrace/%s\n' "${nested[@]}"
  fi
  exit 0
fi

# update <directory> <row>...: one shallow, parallel update of the rows' submodules. The update
# is idempotent, so a fetch that failed on the network is tried again, three times in all.
update() {
  local dir="$1"
  shift
  local paths=() row attempt
  for row in "$@"; do
    paths+=("${row%% *}")
  done
  for attempt in 1 2 3; do
    if git -C "${dir}" submodule update --init --depth=1 --jobs 8 -- "${paths[@]}"; then
      return 0
    fi
    echo "checkout_submodules.sh: the submodule update in ${dir} failed (attempt ${attempt} of 3)" >&2
    if [ "${attempt}" -lt 3 ]; then
      sleep $((attempt * 5))
    fi
  done
  return 1
}

# verify <directory> <row>...: every row's file exists; prints each missing one and returns 1.
verify() {
  local dir="$1" missing=0 row path file
  shift
  for row in "$@"; do
    path="${row%% *}"
    file="${row#* }"
    if [ ! -f "${dir}/${path}/${file}" ]; then
      echo "::error::checkout_submodules.sh: ${dir}/${path}/${file} is missing after the submodule update, so the '${set_name}' build cannot use ${path}"
      missing=1
    fi
  done
  return "${missing}"
}

update . "${top[@]}"
verify . "${top[@]}"
if [ "${#nested[@]}" -gt 0 ]; then
  update 3rdparty/apitrace "${nested[@]}"
  verify 3rdparty/apitrace "${nested[@]}"
fi
echo "checkout_submodules.sh: the '${set_name}' set is checked out ($(( ${#top[@]} + ${#nested[@]} )) submodules)"
