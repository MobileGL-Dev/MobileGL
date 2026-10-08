# sfalign.py <trace>: does SF's setTransactionState return right after SF main-thread flushTransactions?
import re, sys, bisect
from collections import defaultdict
line_re = re.compile(r'^\s*(.+?)-(\d+)\s+\(\s*([\d-]+)\)\s+\[(\d+)\]\s+\S+\s+([\d.]+):\s+tracing_mark_write:\s*(.*)$')
st = defaultdict(list); ends = []; flush = []; begins = []
for line in open(sys.argv[1], errors="replace"):
    m = line_re.match(line)
    if not m: continue
    comm, pid, tgid, cpu, ts, rest = m.groups(); ts = float(ts)
    if tgid.strip() != "1645": continue
    if rest.startswith("B|"):
        n = rest.split("|", 2)[2]; st[pid].append((n, ts))
        if n.startswith("TransactionHandler:flushTransactions"): flush.append(ts)
    elif rest.startswith("E|") and st[pid]:
        n, s = st[pid].pop()
        if n == "setTransactionState": ends.append(ts); begins.append(s)
flush.sort(); d = []; w = []
for b, e in zip(begins, ends):
    i = bisect.bisect_right(flush, e) - 1
    if i >= 0: d.append(1e3 * (e - flush[i]))
    j = bisect.bisect_right(flush, b)
    w.append(1e3 * (e - b))
d.sort(); w.sort()
q = lambda a, p: a[int(p * (len(a) - 1))]
print(f"setTransactionState n={len(d)} duration ms p10/50/90 {q(w,.1):.2f}/{q(w,.5):.2f}/{q(w,.9):.2f}")
print(f"end - last flushTransactions ms p10/50/90 {q(d,.1):.3f}/{q(d,.5):.3f}/{q(d,.9):.3f}; flush period {1e3*(flush[-1]-flush[0])/(len(flush)-1):.2f} ms")
