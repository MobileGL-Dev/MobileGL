# fcl_split.py <rundir> <tid> <ms_per_frame> [depth] [--detail=layer]
# Period-weighted on/off-CPU split of one thread of an FCL simpleperf --trace-offcpu profile.
# On-CPU goes to a layer (driver / backend / applier / client half / jvm+game / other), off-CPU is
# grouped by its first user frames.
import os, sys
from collections import Counter
sys.path.insert(0, os.environ.get("SIMPLEPERF_DIR", ""))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from simpleperf_report_lib import ReportLib
from layers import layer_of
args = [a for a in sys.argv[1:] if not a.startswith("--")]
d, tid, mspf = args[0], int(args[1]), float(args[2]); depth = int(args[3]) if len(args) > 3 else 6
detail = next((a.split("=", 1)[1] for a in sys.argv[1:] if a.startswith("--detail=")), None)
l = ReportLib(); l.SetRecordFile(os.path.join(d, "perf.data")); l.SetSymfs(os.path.join(d, "binary_cache"))
on = Counter(); off = Counter(); det = Counter(); t0 = t1 = None; ON = OFF = 0
sh = lambda n: n.replace("(anonymous namespace)", "{anon}").split("(")[0][-60:]
def lay(fr):
    for n, dso in fr:
        x = layer_of(n, dso)
        if x == "harness": x = None
        if x: return x
    if any("libjvm" in dso or "jit" in dso.lower() or dso.endswith(".jar") or "dalvik" in dso or "[anon" in dso for _, dso in fr): return "jvm+game"
    return "other"
while True:
    s = l.GetNextSample()
    if s is None: break
    if s.tid != tid: continue
    t0 = s.time if t0 is None else t0; t1 = s.time
    leaf = l.GetSymbolOfCurrentSample(); cc = l.GetCallChainOfCurrentSample()
    fr = [(leaf.symbol_name, leaf.dso_name)] + [(cc.entries[i].symbol.symbol_name, cc.entries[i].symbol.dso_name) for i in range(cc.nr)]
    if any(n in ("__schedule", "schedule") or n.startswith("__switch_to") for n, _ in fr[:10]):
        OFF += s.period
        user = [sh(n) for n, dso in fr if "kallsyms" not in dso and not dso.startswith("[")][:depth]
        off[" <- ".join(user)] += s.period
    else:
        ON += s.period; L = lay(fr); on[L] += s.period
        if detail and L == detail:
            # first frame of that layer
            for n, dso in fr:
                if layer_of(n, dso) == detail: det[sh(n)] += s.period; break
frames = (t1 - t0) / 1e6 / mspf
print(f"tid {tid}: {frames:.0f} frames ({(t1-t0)/1e9:.2f}s at {mspf} ms/frame); on-CPU {ON/1e6/frames:.3f} ms/frame, off-CPU {OFF/1e6/frames:.3f}")
for k, v in on.most_common(): print(f"  on  {v/1e6/frames:7.3f}  {k}")
for k, v in off.most_common(16): print(f"  off {v/1e6/frames:7.3f}  {k[:230]}")
for k, v in det.most_common(30): print(f"  det {v/1e6/frames:7.3f}  {k}")
