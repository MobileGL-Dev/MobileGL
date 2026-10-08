# fcl_incl.py <rundir> <tid|thread comm> <frames> [N]: inclusive on-CPU ms/frame of libMobileGL functions
import os, sys
from collections import Counter
sys.path.insert(0, os.environ.get("SIMPLEPERF_DIR", ""))
from simpleperf_report_lib import ReportLib
d, tid, frames = sys.argv[1], (int(sys.argv[2]) if sys.argv[2].isdigit() else sys.argv[2]), float(sys.argv[3]); N = int(sys.argv[4]) if len(sys.argv) > 4 else 60
l = ReportLib(); l.SetRecordFile(os.path.join(d, "perf.data")); l.SetSymfs(os.path.join(d, "binary_cache"))
incl = Counter(); selfc = Counter(); tot = 0
sh = lambda n: n.replace("(anonymous namespace)", "{anon}").replace("MobileGL::", "").split("(")[0][-75:]
while True:
    s = l.GetNextSample()
    if s is None: break
    if (s.tid != tid) if isinstance(tid, int) else (tid not in s.thread_comm): continue
    leaf = l.GetSymbolOfCurrentSample(); cc = l.GetCallChainOfCurrentSample()
    fr = [(leaf.symbol_name, leaf.dso_name)] + [(cc.entries[i].symbol.symbol_name, cc.entries[i].symbol.dso_name) for i in range(cc.nr)]
    if any(n in ("__schedule", "schedule") or n.startswith("__switch_to") for n, _ in fr[:10]): continue
    tot += s.period
    seen = set()
    for n, dso in fr:
        if "libMobileGL" in dso:
            k = sh(n)
            if k not in seen: incl[k] += s.period; seen.add(k)
    for n, dso in fr:
        if "libMobileGL" in dso: selfc[sh(n)] += s.period; break
print(f"on-CPU {tot/1e6/frames:.3f} ms/frame")
for k, v in incl.most_common(N): print(f"  {v/1e6/frames:7.3f} {k}")
print("self (first MobileGL frame):")
for k, v in selfc.most_common(25): print(f"  {v/1e6/frames:7.3f} {k}")
