# pbshown.sh - frames SurfaceFlinger latched for the anland SurfaceView in the last second (BLAST layer)
L=$(dumpsys SurfaceFlinger --list | grep -E "^SurfaceView\[com\.anland\.consumer.*\(BLAST\)#" | head -1)
[ -z "$L" ] && L=$(dumpsys SurfaceFlinger --list | grep -oE "SurfaceView\[com\.anland\.consumer[^]]*\]\(BLAST\)#[0-9]+" | head -1)
dumpsys SurfaceFlinger --latency "$L" | awk -v l="$L" 'NR>1 && $2>0 && $2<9e18 {t[n++]=$2; if($2>m)m=$2} END{c=0; for(i=0;i<n;i++) if(t[i]>m-1e9) c++; print "shown_last_1s=" c " layer=" l}'
