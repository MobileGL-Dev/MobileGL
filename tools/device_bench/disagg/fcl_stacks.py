# fcl_stacks.py <rundir> <tid> <frames> [depth] [filter-substring]: on-CPU inclusive function time (ms/frame) and top stacks
import os, sys
from collections import Counter
sys.path.insert(0, os.environ.get("SIMPLEPERF_DIR", ""))
from simpleperf_report_lib import ReportLib
d, tid, frames = sys.argv[1], int(sys.argv[2]), float(sys.argv[3]); depth = int(sys.argv[4]) if len(sys.argv) > 4 else 8
flt = sys.argv[5] if len(sys.argv) > 5 else None
l = ReportLib(); l.SetRecordFile(os.path.join(d, "perf.data")); l.SetSymfs(os.path.join(d, "binary_cache"))
incl = Counter(); st = Counter(); offst = Counter()
sh = lambda n: n.replace("(anonymous namespace)", "{anon}").split("(")[0][-55:]
while True:
    s = l.GetNextSample()
    if s is None: break
    if s.tid != tid: continue
    leaf = l.GetSymbolOfCurrentSample(); cc = l.GetCallChainOfCurrentSample()
    fr = [(leaf.symbol_name, leaf.dso_name)] + [(cc.entries[i].symbol.symbol_name, cc.entries[i].symbol.dso_name) for i in range(cc.nr)]
    names = [sh(n) for n, _ in fr]
    if flt and not any(flt in n for n in names): continue
    if any(n in ("__schedule", "schedule") or n.startswith("__switch_to") for n, _ in fr[:10]):
        u = [sh(n) for n, dso in fr if "kallsyms" not in dso and not dso.startswith("[")][:depth]
        offst[" <- ".join(u)] += s.period; continue
    for n in set(names): incl[n] += s.period
    st[" <- ".join(names[:depth])] += s.period
print("inclusive on-CPU:"); [print(f"  {v/1e6/frames:7.3f} {k}") for k, v in incl.most_common(40)]
print("top on stacks:"); [print(f"  {v/1e6/frames:7.3f} {k[:300]}") for k, v in st.most_common(12)]
print("top off stacks:"); [print(f"  {v/1e6/frames:7.3f} {k[:300]}") for k, v in offst.most_common(8)]
