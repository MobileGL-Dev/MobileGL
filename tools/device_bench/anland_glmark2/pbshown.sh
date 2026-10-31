# MobileGL - tools/device_bench/anland_glmark2/pbshown.sh
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

# pbshown.sh - frames SurfaceFlinger latched for the anland SurfaceView in the last second (BLAST layer)
L=$(dumpsys SurfaceFlinger --list | grep -E "^SurfaceView\[com\.anland\.consumer.*\(BLAST\)#" | head -1)
[ -z "$L" ] && L=$(dumpsys SurfaceFlinger --list | grep -oE "SurfaceView\[com\.anland\.consumer[^]]*\]\(BLAST\)#[0-9]+" | head -1)
dumpsys SurfaceFlinger --latency "$L" | awk -v l="$L" 'NR>1 && $2>0 && $2<9e18 {t[n++]=$2; if($2>m)m=$2} END{c=0; for(i=0;i<n;i++) if(t[i]>m-1e9) c++; print "shown_last_1s=" c " layer=" l}'
