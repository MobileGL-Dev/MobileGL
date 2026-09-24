#!/usr/bin/env bash
# pidlines.sh <logcat> <pid> <from HH:MM:SS> <to HH:MM:SS> [exclude-regex]: that pid's lines in a window.
f=$1; pid=$2; from=$3; to=$4; ex=${5:-'^$'}
awk -v p="$pid" -v a="$from" -v b="$to" '$3==p && substr($2,1,8)>=a && substr($2,1,8)<=b' "$f" | grep -vE "$ex" | cut -c1-240
