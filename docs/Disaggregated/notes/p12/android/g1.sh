#!/usr/bin/env bash
# g1.sh: the pull build's libMobileGL against the P12 base - .text size and full nm lines (the
# base file holds full `nm` lines, so both sides are compared whole and as names).
set -uo pipefail
cd /home/swung/w7/p12-onscreen || exit 1
export MOBILEGL_FLATC_EXECUTABLE=/home/swung/w7/flatc-build/flatc
ninja -C build-linux -j 24 MobileGL 2>&1 | tail -1
readelf -S -W build-linux/libMobileGL.so | awk '$2==".text"{print "G1 text size (hex):", $6, "(base a52203)"}'
T=$(mktemp -d)
nm --defined-only build-linux/libMobileGL.so | sort > "$T/now.txt"
sort /home/swung/w7/logs/p12/pull-syms-base.txt > "$T/base.txt"
echo "G1 full nm lines: added=$(comm -13 "$T/base.txt" "$T/now.txt" | wc -l) removed=$(comm -23 "$T/base.txt" "$T/now.txt" | wc -l)"
awk '{print $3}' "$T/now.txt" | sort > "$T/now-n.txt"; awk '{print $3}' "$T/base.txt" | sort > "$T/base-n.txt"
echo "G1 names: added=$(comm -13 "$T/base-n.txt" "$T/now-n.txt" | wc -l) removed=$(comm -23 "$T/base-n.txt" "$T/now-n.txt" | wc -l)"
rm -rf "$T"
