#!/usr/bin/env bash
# launch-build.sh [<sha>]: starts build.sh for the package tree's HEAD (or <sha>) in the background.
set -euo pipefail
SHA=$(git -C /home/swung/w7/p12-onscreen rev-parse "${1:-HEAD}")
mkdir -p /home/swung/w7/logs/p12/android
LOG=/home/swung/w7/logs/p12/android/build-${SHA:0:8}.log
setsid nohup bash /home/swung/w7/notes/p12/android/build.sh "$SHA" > "$LOG" 2>&1 < /dev/null &
echo "started build of ${SHA:0:8}, log $LOG"
