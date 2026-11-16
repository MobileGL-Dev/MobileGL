#!/bin/bash
# MobileGL - scripts/ci/test_set_swap_space.sh
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

# Runs scripts/ci/set_swap_space.sh against stubbed sudo (which logs swapoff, fallocate and the
# other privileged commands instead of running them), swapon, df, free, findmnt and lsblk, so no
# case touches the host's swap. STUB_SWAPS lists the active swap areas as "NAME TYPE" lines and
# STUB_FS the file systems as "mount=available-MiB" words; fallocate fails with ENOSPC when the
# size does not fit, as a small devtmpfs on /dev does, or exceeds STUB_FALLOCATE_MAX_GB, and
# STUB_DF=fail or STUB_DF=garbage makes df fail or print no number. The /mnt fallback is a
# temporary directory, so the result does not depend on the host's /mnt.
# Each case checks the exit status and the privileged commands the script ran.
#
# usage: test_set_swap_space.sh [script]    (defaults to set_swap_space.sh next to this file)
set -euo pipefail

script="${1:-$(cd "$(dirname "$0")" && pwd)/set_swap_space.sh}"
stubs="$(mktemp -d)"
trap 'rm -rf "${stubs}"' EXIT

cat > "${stubs}/stub-lib" << 'EOF'
mount_of() {
  local path="$1" best="" mount
  for entry in ${STUB_FS}; do
    mount="${entry%%=*}"
    case "${path}" in
      "${mount}" | "${mount%/}"/*)
        if [ "${#mount}" -gt "${#best}" ]; then
          best="${mount}"
        fi
        ;;
    esac
  done
  printf '%s\n' "${best}"
}
avail_of() {
  local mount entry
  mount="$(mount_of "$1")"
  for entry in ${STUB_FS}; do
    if [ "${entry%%=*}" = "${mount}" ]; then
      printf '%s\n' "${entry#*=}"
    fi
  done
}
EOF

stub() {
  printf '#!/bin/bash\n. "%s/stub-lib"\n%s\n' "${stubs}" "$2" > "${stubs}/$1"
  chmod +x "${stubs}/$1"
}
stub sudo 'echo "$*" >> "${STUB_LOG}"
if [ "$1" = fallocate ]; then
  want="${3%G}"
  if [ $((want * 1024)) -gt "$(avail_of "$(dirname "$4")")" ] ||
    [ "${want}" -gt "${STUB_FALLOCATE_MAX_GB:-${want}}" ]; then
    echo "fallocate: fallocate failed: No space left on device" >&2
    exit 1
  fi
fi'
stub swapon 'if [ "$1" = --show=NAME,TYPE ] && [ "${2:-}" = --noheadings ]; then
  printf "%s\n" "${STUB_SWAPS}" | sed "/^$/d"
elif [ "$1" = --show=NAME ] && [ "${2:-}" = --noheadings ]; then
  printf "%s\n" "${STUB_SWAPS}" | sed "/^$/d" | cut -d" " -f1
else
  printf "%s\n" "${STUB_SWAPS}"
fi'
stub df 'path="${*: -1}"
if [ "${STUB_DF:-}" = fail ]; then
  echo "df: stub failure" >&2
  exit 1
elif [ "${STUB_DF:-}" = garbage ]; then
  echo "Avail"
  echo "-"
elif [ "$1" = --output=avail ]; then
  echo "1M-blocks"
  avail_of "${path}"
else
  echo "Filesystem Avail Mounted on"
  echo "stub $(avail_of "${path}")M $(mount_of "${path}")"
fi'
stub free 'echo "Mem: stub"'
stub findmnt 'true'
stub lsblk 'true'

failures=0
# expect <case> <exit> <expected sudo log, ";"-separated> <swaps> <file systems> [size]
expect() {
  local name="$1" want_exit="$2" want_log="$3" got_exit=0 got_log
  : > "${stubs}/log"
  PATH="${stubs}:${PATH}" STUB_LOG="${stubs}/log" STUB_SWAPS="$4" STUB_FS="$5" \
    SWAP_SIZE_GB="${6:-8}" SWAP_ALT_DIR="${SWAP_ALT_DIR:-${mnt}}" \
    bash -euo pipefail "${script}" > "${stubs}/out" 2>&1 || got_exit=$?
  got_log="$(paste -sd ';' "${stubs}/log")"
  if [ "${got_exit}" -ne "${want_exit}" ] || [ "${got_log}" != "${want_log}" ]; then
    echo "NOT OK  ${name}: exit ${got_exit} (expected ${want_exit})"
    echo "        ran:      ${got_log}"
    echo "        expected: ${want_log}"
    sed 's/^/        | /' "${stubs}/out"
    failures=$((failures + 1))
  fi
}

# The /mnt fallback directory; ${mnt}/missing stands for a host without /mnt.
mnt="${stubs}/mnt"
mkdir "${mnt}"
roomy="/=93000 /dev=4 ${mnt}=70000"
replace() {
  echo "rm -f $1;fallocate -l $2G $1;chmod 600 $1;mkswap $1;swapon $1"
}

expect "image swap file" 0 "swapoff /swapfile;$(replace /swapfile 8)" "/swapfile file" "${roomy}"
expect "no swap" 0 "$(replace /swapfile 32)" "" "${roomy}" 32
expect "swap file on /mnt" 0 "swapoff ${mnt}/swapfile;$(replace "${mnt}/swapfile" 8)" \
  "${mnt}/swapfile file" "${roomy}"
expect "zram device" 0 "swapoff /dev/zram0;$(replace /swapfile 8)" "/dev/zram0 partition" "${roomy}"
expect "swap partition and file" 0 "swapoff /dev/sdb2;swapoff /swapfile;$(replace /swapfile 8)" \
  "/dev/sdb2 partition
/swapfile file" "${roomy}"
expect "short root, roomy /mnt" 0 "rm -f /swapfile;$(replace "${mnt}/swapfile" 16)" \
  "" "/=6000 /dev=4 ${mnt}=70000" 16
expect "short root, no /mnt file system" 0 "$(replace /swapfile 3)" "" "/=6000 /dev=4" 16
SWAP_ALT_DIR="${mnt}/missing" expect "short root, no /mnt directory" 0 "$(replace /swapfile 3)" \
  "" "/=6000 /dev=4 ${mnt}/missing=70000" 16
expect "room for the floor only" 0 "$(replace /swapfile 1)" "" "/=2500 /dev=4" 8
expect "no room anywhere" 1 "rm -f /swapfile;fallocate -l 1G /swapfile;rm -f /swapfile" "" \
  "/=900 /dev=4" 8
expect "size not a number" 1 "" "" "${roomy}" 8G
STUB_DF=fail expect "df fails" 0 "swapoff /swapfile;$(replace /swapfile 8)" "/swapfile file" "${roomy}"
STUB_DF=garbage expect "df prints no number" 0 "$(replace /swapfile 8)" "" "${roomy}"
STUB_DF=fail STUB_FALLOCATE_MAX_GB=4 expect "df fails and fallocate refuses" 0 \
  "rm -f /swapfile;fallocate -l 8G /swapfile;$(replace /swapfile 4)" "" "${roomy}"
STUB_FALLOCATE_MAX_GB=2 expect "fallocate refuses although df has room" 0 \
  "rm -f /swapfile;fallocate -l 8G /swapfile;rm -f /swapfile;fallocate -l 4G /swapfile;$(replace /swapfile 2)" \
  "" "${roomy}"
STUB_FALLOCATE_MAX_GB=0 expect "fallocate refuses every size" 1 \
  "rm -f /swapfile;fallocate -l 2G /swapfile;rm -f /swapfile;fallocate -l 1G /swapfile;rm -f /swapfile" \
  "" "${roomy}" 2

if [ "${failures}" -ne 0 ]; then
  echo "set_swap_space self-test: FAILED"
  exit 1
fi
echo "set_swap_space self-test: ok"
