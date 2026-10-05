#!/usr/bin/env bash
set -euo pipefail
# CPU runners may be GitHub-hosted or self-hosted; the artifact ABI stays Ubuntu.
test -f /.dockerenv
. /etc/os-release
test "$ID" = ubuntu && test "$VERSION_ID" = 24.04
apt-get update
apt-get install -y curl gnupg tar xz-utils zstd build-essential gh jq unzip zip wget pkg-config cmake
curl --fail --silent --show-error https://apt.llvm.org/llvm-snapshot.gpg.key \
  | gpg --dearmor -o /usr/share/keyrings/llvm-archive-keyring.gpg
echo 'deb [signed-by=/usr/share/keyrings/llvm-archive-keyring.gpg] https://apt.llvm.org/noble/ llvm-toolchain-noble-20 main' \
  > /etc/apt/sources.list.d/llvm.list
apt-get update
apt-get install -y libc++1-20 libc++abi1-20 libunwind-20
