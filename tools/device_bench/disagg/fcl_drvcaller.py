# fcl_drvcaller.py <rundir> <tid> <frames>: driver-layer on-CPU time keyed by (driver entry, MobileGL caller)
import os, sys
from collections import Counter
sys.path.insert(0, os.environ.get("SIMPLEPERF_DIR", ""))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from simpleperf_report_lib import ReportLib
from layers import layer_of
d, tid, frames = sys.argv[1], int(sys.argv[2]), float(sys.argv[3])
l = ReportLib(); l.SetRecordFile(os.path.join(d, "perf.data")); l.SetSymfs(os.path.join(d, "binary_cache"))
c = Counter(); entry = Counter(); leafc = Counter()
sh = lambda n: n.replace("(anonymous namespace)", "{anon}").split("(")[0][-60:]
while True:
    s = l.GetNextSample()
    if s is None: break
    if s.tid != tid: continue
    leaf = l.GetSymbolOfCurrentSample(); cc = l.GetCallChainOfCurrentSample()
    fr = [(leaf.symbol_name, leaf.dso_name)] + [(cc.entries[i].symbol.symbol_name, cc.entries[i].symbol.dso_name) for i in range(cc.nr)]
    if any(n in ("__schedule", "schedule") or n.startswith("__switch_to") for n, _ in fr[:10]): continue
    lay = None
    for n, dso in fr:
        lay = layer_of(n, dso)
        if lay: break
    if lay != "driver": continue
    # outermost driver frame and the first MobileGL frame above it
    last_drv = None; caller = "?"
    for i, (n, dso) in enumerate(fr):
        if layer_of(n, dso) == "driver": last_drv = i
    if last_drv is not None:
        e = sh(fr[last_drv][0]) + " [" + os.path.basename(fr[last_drv][1]) + "]"
        for n, dso in fr[last_drv + 1:]:
            if "libMobileGL" in dso: caller = sh(n); break
            if "libgui" in dso or "libvulkan" in dso: caller = sh(n) + " [" + os.path.basename(dso) + "]"; break
        c[(e, caller)] += s.period; entry[e] += s.period
        leafc[sh(fr[0][0]) + " [" + os.path.basename(fr[0][1]) + "]"] += s.period
print("driver entry (ms/frame):"); [print(f"  {v/1e6/frames:7.3f} {k}") for k, v in entry.most_common(15)]
print("entry x caller:"); [print(f"  {v/1e6/frames:7.3f} {k[0]}  <- {k[1]}") for k, v in c.most_common(20)]
print("leaf:"); [print(f"  {v/1e6/frames:7.3f} {k}") for k, v in leafc.most_common(12)]
