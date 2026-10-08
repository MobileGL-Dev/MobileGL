# slices.py <trace> <comm-regex> : atrace slice wall time per slice name for threads whose comm-pid matches
import re, sys
from collections import Counter, defaultdict
line_re = re.compile(r'^\s*(.+?)-(\d+)\s+\(\s*([\d-]+)\)\s+\[(\d+)\]\s+\S+\s+([\d.]+):\s+tracing_mark_write:\s*(.*)$')
rx = re.compile(sys.argv[2]); stacks = defaultdict(list); tot = Counter(); cnt = Counter(); t0 = t1 = None
for line in open(sys.argv[1], errors="replace"):
    m = line_re.match(line)
    if not m: continue
    comm, pid, tgid, cpu, ts, rest = m.groups(); ts = float(ts)
    t0 = ts if t0 is None else t0; t1 = ts
    if not rx.search(comm + "-" + pid): continue
    if rest.startswith("B|"):
        stacks[pid].append((rest.split("|", 2)[2][:70], ts))
    elif rest.startswith("E|") and stacks[pid]:
        n, s = stacks[pid].pop(); tot[n] += ts - s; cnt[n] += 1
for k, v in tot.most_common(25): print(f"{1e3*v:9.1f} ms  n={cnt[k]:6d}  avg {1e6*v/cnt[k]:8.1f} us  {k}")
