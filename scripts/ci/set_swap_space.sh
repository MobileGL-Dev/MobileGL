#!/bin/bash
# MobileGL - scripts/ci/set_swap_space.sh
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

# Replaces the runner's swap with one swap file of SWAP_SIZE_GB GiB (.github/actions/set-swap-space).
# Hosted images differ: some ship a /swapfile, some no swap, and a swap partition or zram device is
# possible. Every active swap area is turned off, but only a swap file is replaced in place; with
# none the file is /swapfile, or /mnt/swapfile when that file system has more room. The file is
# sized down to what fits with SWAP_MARGIN_MIB to spare, and the step fails only below 1 GiB.
# df only informs the size: when it reports nothing usable, the requested size is tried as is.
#
# usage: set_swap_space.sh    (SWAP_SIZE_GB from the action input; SWAP_MARGIN_MIB and
#                              SWAP_ALT_DIR, the /mnt fallback directory, are optional)
set -euo pipefail

size_gb="${SWAP_SIZE_GB:-}"
margin_mib="${SWAP_MARGIN_MIB:-2048}"
alt_dir="${SWAP_ALT_DIR:-/mnt}"
floor_gb=1
case "${size_gb}" in
  '' | *[!0-9]* | 0)
    echo "::error::swap-size-gb must be a positive whole number of GiB, got '${size_gb}'"
    exit 1
    ;;
esac

# Prints the MiB available on the file system that holds $1, or nothing when df fails or prints
# something else.
avail_mib() {
  local value
  value="$(df --output=avail -B1M "$1" 2> /dev/null | tail -n 1 | tr -d ' ' || true)"
  case "${value}" in
    '' | *[!0-9]*) ;;
    *) printf '%s\n' "${value}" ;;
  esac
}

show_state() {
  echo "Swap $1:"
  swapon --show || true
  free -h || true
}

show_state before

swap_file=""
swap_names=()
while read -r name type _; do
  [ -n "${name}" ] || continue
  swap_names+=("${name}")
  if [ "${type}" = file ]; then
    swap_file="${name}"
  fi
done < <(swapon --show=NAME,TYPE --noheadings)
swap_file="${swap_file:-/swapfile}"

for name in "${swap_names[@]}"; do
  sudo swapoff "${name}"
done
sudo rm -f "${swap_file}"

want_mib=$((size_gb * 1024))
swap_dir="$(dirname "${swap_file}")"
avail="$(avail_mib "${swap_dir}")"
if [ -z "${avail}" ]; then
  echo "df did not report the space available on ${swap_dir}; trying the requested ${size_gb} GiB"
elif [ "${avail}" -lt $((want_mib + margin_mib)) ] && [ "${swap_dir}" != "${alt_dir}" ] &&
  [ -d "${alt_dir}" ]; then
  alt_avail="$(avail_mib "${alt_dir}")"
  if [ -n "${alt_avail}" ] && [ "${alt_avail}" -gt "${avail}" ]; then
    echo "${swap_dir} has ${avail} MiB available and ${alt_dir} has ${alt_avail} MiB; using ${alt_dir}/swapfile"
    swap_file="${alt_dir}/swapfile"
    swap_dir="${alt_dir}"
    avail="${alt_avail}"
    sudo rm -f "${swap_file}"
  fi
fi
df -h "${swap_dir}" || true

if [ -n "${avail}" ] && [ "${avail}" -lt $((want_mib + margin_mib)) ]; then
  # df only shrinks the file; whether even the floor fits is left to fallocate, which may use
  # blocks df does not count as available.
  fit_gb=$(((avail - margin_mib) / 1024))
  if [ "${fit_gb}" -lt "${floor_gb}" ]; then
    fit_gb="${floor_gb}"
  fi
  echo "::warning::${swap_dir} has ${avail} MiB available; the swap file is ${fit_gb} GiB instead of the requested ${size_gb} GiB"
  size_gb="${fit_gb}"
fi

# A refusal that df did not predict halves the size down to the floor, printing what the file
# system looked like each time.
df_note="df reported ${avail} MiB available"
if [ -z "${avail}" ]; then
  df_note="df did not report the available space"
fi
while ! sudo fallocate -l "${size_gb}G" "${swap_file}"; do
  sudo rm -f "${swap_file}"
  df -h "${swap_dir}" || true
  findmnt -T "${swap_dir}" || true
  lsblk || true
  if [ "${size_gb}" -le "${floor_gb}" ]; then
    echo "::error::fallocate of ${size_gb} GiB at ${swap_file} failed; ${df_note}"
    exit 1
  fi
  next_gb=$((size_gb / 2 > floor_gb ? size_gb / 2 : floor_gb))
  echo "::warning::fallocate of ${size_gb} GiB at ${swap_file} failed; ${df_note}; trying ${next_gb} GiB"
  size_gb="${next_gb}"
done
sudo chmod 600 "${swap_file}"
sudo mkswap "${swap_file}"
sudo swapon "${swap_file}"

show_state after
df -h "${swap_dir}" || true
