#!/usr/bin/env bash
# show.sh <check-dir>...: the non-logcat lines of each check.log, then its logcat lines (trimmed).
for c in "$@"; do
    f=/home/swung/w7/logs/p12/device/$c/check.log
    echo "######## $c"
    grep -vE "^09-24" "$f" | grep -v Deprecation | cut -c1-260
    echo "---- logcat lines"
    grep -E "^09-24" "$f" | cut -c1-250 | head -${LINES_MAX:-30}
done
