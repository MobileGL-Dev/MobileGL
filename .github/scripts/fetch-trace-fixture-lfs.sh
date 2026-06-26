#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
  echo "usage: $0 <trace-case> [fixture-dir]" >&2
  exit 2
fi

case_name="$1"
fixture_dir="${2:-tools/trace_replay/fixtures}"

files=()

add_standard_case() {
  local golden_suffix="$1"
  files+=(
    "${fixture_dir}/${case_name}.tgz"
    "${fixture_dir}/${case_name}.${golden_suffix}.png"
  )
}

case "${case_name}" in
  OpenRA)
    files+=(
      "${fixture_dir}/openra.tgz"
      "${fixture_dir}/openra.0000031249.png"
    )
    ;;
  minecraft-1.21.4-startup)
    add_standard_case "0000092195"
    ;;
  minecraft-1.21.4-main-menu)
    add_standard_case "0000481787"
    ;;
  minecraft-1.21.4-in-world)
    add_standard_case "0000280000"
    ;;
  minecraft-1.21.4-fabric-sodium-in-world)
    add_standard_case "0000923340"
    ;;
  minecraft-1.21.4-fabric-iris-bsl-in-world)
    add_standard_case "0000110725"
    ;;
  minecraft-1.21.4-fabric-iris-makeup-ultrafast-in-world)
    add_standard_case "0000095322"
    ;;
  minecraft-1.21.4-fabric-iris-super-duper-vanilla-in-world)
    add_standard_case "0000141559"
    ;;
  minecraft-1.21.4-fabric-iris-sundial-lite-in-world)
    add_standard_case "0000150023"
    ;;
  minecraft-1.21.4-fabric-iris-complementary-reimagined-in-world)
    add_standard_case "0000151297"
    ;;
  minecraft-1.21.4-fabric-iris-complementary-unbound-in-world)
    add_standard_case "0000146559"
    ;;
  minecraft-1.21.4-fabric-iris-mellow-in-world)
    add_standard_case "0000096143"
    ;;
  minecraft-1.21.4-fabric-iris-nostalgia-in-world)
    files+=(
      "${fixture_dir}/${case_name}.tgz"
      "${fixture_dir}/${case_name}.0000153808-linux-mesa.png"
      "${fixture_dir}/${case_name}.0000153808.png"
    )
    ;;
  minecraft-1.21.4-fabric-iris-bliss-in-world)
    add_standard_case "0000113511"
    ;;
  minecraft-1.21.4-fabric-iris-chocapic-v6-lite-in-world)
    add_standard_case "0000125124-linux-mesa"
    ;;
  minecraft-1.21.4-fabric-iris-iterationt-in-world)
    add_standard_case "0000110538"
    ;;
  minecraft-1.21.4-fabric-iris-iterationt-nodsa-in-world)
    add_standard_case "0000115019"
    ;;
  minecraft-1.21.4-fabric-iris-photon-v1.1-in-world)
    add_standard_case "0000159866"
    ;;
  *)
    echo "unknown trace case: ${case_name}" >&2
    exit 2
    ;;
esac

include="$(IFS=,; echo "${files[*]}")"
echo "Fetching LFS fixture files for ${case_name}: ${include}"

git lfs install --local
git lfs pull --include="${include}" --exclude=""

for file in "${files[@]}"; do
  test -s "${file}"
  if head -n 1 "${file}" | grep -q "version https://git-lfs.github.com/spec/v1"; then
    echo "failed to hydrate LFS fixture: ${file}" >&2
    exit 1
  fi
done
