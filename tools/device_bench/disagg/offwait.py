# offwait.py <trace.txt> <tid> [present_marker]
# Off-CPU accounting for one thread from an atrace/ftrace text dump:
#  - sleep (S/D) and runnable (preempted) time per frame
#  - sleep time keyed by the atrace slice stack open at the switch-out and the waker's comm
#  - GPU busy from adreno_cmdbatch_retired (start/retire are GPU ticks: 19.2 MHz)
import re, sys
from collections import Counter, defaultdict
path, tid = sys.argv[1], sys.argv[2]
present = sys.argv[3] if len(sys.argv) > 3 else "QueuePresentKHR|eglSwapBuffers"
pres_re = re.compile(present)
line_re = re.compile(r'^\s*(.+?)-(\d+)\s+\(\s*([\d-]+)\)\s+\[(\d+)\]\s+\S+\s+([\d.]+):\s+(\w+):\s*(.*)$')
stack = []
state = None; t_state = None  # 'run' | 'sleep' | 'runq'
sleep_by = Counter(); runq_total = 0.0; sleep_total = 0.0; run_total = 0.0
pending_sleep = None  # (t0, stackkey, prev_state)
last_waker = None
presents = []
gpu = []  # (start_tick, retire_tick, ctx)
t_first = t_last = None
slice_time = Counter(); slice_open = []
for line in open(path, errors="replace"):
    m = line_re.match(line)
    if not m: continue
    comm, pid, tgid, cpu, ts, ev, rest = m.groups(); ts = float(ts)
    if t_first is None: t_first = ts
    t_last = ts
    if ev == "tracing_mark_write" and pid == tid:
        if rest.startswith("B|"):
            name = rest.split("|", 2)[2]
            stack.append((name, ts))
            if pres_re.search(name): presents.append(ts)
        elif rest.startswith("E|"):
            if stack:
                name, t0 = stack.pop(); slice_time[name[:60]] += ts - t0
    elif ev == "sched_waking":
        mm = re.search(r'pid=(\d+)', rest)
        if mm and mm.group(1) == tid: last_waker = f"{comm}"
    elif ev == "sched_switch":
        mp = re.search(r'prev_pid=(\d+) prev_prio=\d+ prev_state=(\S+) ==> next_comm=.* next_pid=(\d+)', rest)
        if not mp: continue
        ppid, pst, npid = mp.groups()
        if ppid == tid:
            key = " > ".join(n[:40] for n, _ in stack[-3:]) or "(no slice)"
            pending_sleep = (ts, key, pst)
            last_waker = None
        if npid == tid and pending_sleep:
            t0, key, pst = pending_sleep
            d = ts - t0
            if pst.startswith("R"): runq_total += d; sleep_by[("RUNNABLE(preempted)", key, "-")] += d
            else: sleep_total += d; sleep_by[("sleep " + pst, key, last_waker or "?")] += d
            pending_sleep = None
    elif ev == "adreno_cmdbatch_retired":
        ms = re.search(r'ctx=(\d+).*start=(\d+) retire=(\d+)', rest)
        if ms: gpu.append((int(ms.group(2)), int(ms.group(3)), int(ms.group(1))))
win = t_last - t_first
# only steady presents
nfr = max(1, len(presents) - 1); span = presents[-1] - presents[0] if len(presents) > 1 else win
print(f"window {win:.3f}s, presents {len(presents)} -> {len(presents)/win:.1f} fps, frame {1e3*span/nfr:.3f} ms")
print(f"per frame: sleep {1e3*sleep_total/nfr:.3f} ms, runnable(preempted) {1e3*runq_total/nfr:.3f} ms")
print("top off-CPU (ms/frame): state | open slices at switch-out | waker")
for (st, key, wk), d in sleep_by.most_common(18):
    print(f"  {1e3*d/nfr:6.3f}  {st:<22} | {key[:110]:<110} | {wk}")
print("top slices on thread (ms/frame, wall incl. children):")
for k, v in slice_time.most_common(14): print(f"  {1e3*v/nfr:6.3f}  {k}")
if gpu:
    gpu.sort(); busy = 0; cs, ce = gpu[0][0], gpu[0][1]
    for s, e, c in gpu[1:]:
        if s > ce: busy += ce - cs; cs, ce = s, e
        else: ce = max(ce, e)
    busy += ce - cs
    spanticks = gpu[-1][1] - gpu[0][0]
    ctxs = Counter(c for _, _, c in gpu)
    print(f"GPU: {len(gpu)} cmdbatches, busy {100*busy/spanticks:.1f}% of {spanticks/19.2e6:.2f}s, {1e3*busy/19.2e6/nfr:.3f} ms/frame; ctxs {ctxs.most_common(5)}")
