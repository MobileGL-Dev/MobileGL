#!/usr/bin/env bash
# rec.sh <name> - record 3 s of the screen while opening Kickoff, extract frames inside the container
# with ffmpeg, pull them to $ANL_DIR/af_<name>/ and build a 3x4 contact sheet $ANL_DIR/grid_<name>.png
# (needs Python + Pillow on Windows).  Use it to catch flicker/black frames a single screenshot misses.
# Env: TAP="x y" Kickoff button position (default "69 1349" = bottom-left on the 2560x1600 tablet;
# device specific).  Note: anland grabs the touchscreen (EVIOCGRAB); if `input tap` does nothing, use
# `input mouse tap x y`.  Env CHANNEL (default rec).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
. "$here/env.sh"
N=${1:?usage: rec.sh <name>}; TAP=${TAP:-69 1349}
adb_ shell "su -c 'input keyevent 111; sleep 1.5; (screenrecord --time-limit 3 --bit-rate 20000000 $DEV_SHARE/$N.mp4 &); sleep 0.7; input tap $TAP; sleep 3; cp $DEV_SHARE/$N.mp4 $CT_ROOT/root/$N.mp4'"
bash "$here/ct.sh" "${CHANNEL:-rec}" <<IN
rm -rf /root/af_$N; mkdir -p /root/af_$N; ffmpeg -loglevel quiet -i /root/$N.mp4 -vf scale=1000:-1 -fps_mode passthrough /root/af_$N/f%03d.png; ls /root/af_$N | wc -l
IN
adb_ shell "su -c 'rm -rf $DEV_SHARE/af_$N; cp -r $CT_ROOT/root/af_$N $DEV_SHARE/ && chmod -R 777 $DEV_SHARE/af_$N'"
rm -rf "$ANL_DIR/af_$N"; adb_ pull "$DEV_SHARE/af_$N" "$ANL_DIR/" >/dev/null
python - "$ANL_DIR/af_$N" "$ANL_DIR/grid_$N.png" <<'P'
import sys, os
from PIL import Image
d, out = sys.argv[1], sys.argv[2]
fs = sorted(os.listdir(d)); n = len(fs)
pick = [fs[min(n-1, int(i * n / 12))] for i in range(12)]
ims = [Image.open(os.path.join(d, f)) for f in pick]
w, h = ims[0].size; tw, th = w // 2, h // 2
g = Image.new('RGB', (tw * 3, th * 4))
for i, im in enumerate(ims): g.paste(im.resize((tw, th)), ((i % 3) * tw, (i // 3) * th))
g.save(out); print(n, pick)
P
