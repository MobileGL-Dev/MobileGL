#!/usr/bin/env bash
# The G1 base was saved as full nm lines; gate.sh compares names. Keep the full file, write the names list.
B=/home/swung/w7/logs/p12/pull-syms-base.txt
[ -f "$B.full" ] || cp "$B" "$B.full"
awk '{print $3}' "$B.full" | sort > "$B"
head -2 "$B"; wc -l < "$B"
