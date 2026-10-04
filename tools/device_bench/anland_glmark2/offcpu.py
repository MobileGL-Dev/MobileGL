# offcpu.py <samples.txt> <tid> <so> <base-hex> <dso-substr> [depth]
# Aggregates report-sample output (trace-offcpu) for one thread: on/off CPU split, and the
# off-CPU (leaf __schedule) time by kernel wait path + first N libMobileGL frames.
import sys, re, subprocess, collections, os
f, tid, so, base, sub = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4], 16), sys.argv[5]
depth = int(sys.argv[6]) if len(sys.argv) > 6 else 3
SYM = os.path.expandvars(r'$LOCALAPPDATA/Android/Sdk/ndk/27.2.12479018/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-symbolizer.exe')
samples = []
cur = None
mode = None
for line in open(f, encoding='utf-8', errors='replace'):
    s = line.strip()
    if s == 'sample:':
        if cur: samples.append(cur)
        cur = {'frames': [], 'count': 0, 'tid': None, 'time': 0, 'ev': ''}; mode = 'top'; continue
    if cur is None: continue
    if s.startswith('event_count:'): cur['count'] = int(s.split()[1])
    elif s.startswith('time:'): cur['time'] = int(s.split()[1])
    elif s.startswith('event_type:'): cur['ev'] = s.split()[1]
    elif s.startswith('thread_id:'): cur['tid'] = s.split()[1]
    elif s.startswith('vaddr_in_file:'): cur['frames'].append([s.split()[1], None, None])
    elif s.startswith('file:') and cur['frames']: cur['frames'][-1][1] = s.split(None, 1)[1]
    elif s.startswith('symbol:') and cur['frames']: cur['frames'][-1][2] = s.split(None, 1)[1]
if cur: samples.append(cur)
samples = sorted([x for x in samples if x['tid'] == tid], key=lambda x: x['time'])
for i, x in enumerate(samples):
    if x['ev'].startswith('sched'):
        x['count'] = (samples[i+1]['time'] - x['time']) if i + 1 < len(samples) else 0
        x['off'] = True
    else:
        x['off'] = False
addrs = set()
for x in samples:
    for a, d, sym in x['frames']:
        if d and sub in d: addrs.add(int(a, 16) + base)
addrs = sorted(addrs)
names = {}
CH = 4000
for i in range(0, len(addrs), CH):
    part = addrs[i:i+CH]
    out = subprocess.run([SYM, '--obj=' + so, '-C', '-f', '--inlining=false'] + ['0x%x' % a for a in part], capture_output=True, text=True).stdout
    for a, b in zip(part, out.strip().split('\n\n')):
        n = b.split('\n')[0]
        n = re.sub(r'\(.*', '', n)  # drop args
        n = n.replace('MobileGL::', '')
        names[a] = n[:90]
def short(fr):
    a, d, sym = fr
    if d and sub in d: return names.get(int(a, 16) + base, a)
    if sym and not sym.startswith('*'): return sym
    return os.path.basename(d or '?')
tot = sum(x['count'] for x in samples)
off = [x for x in samples if x['off']]
offt = sum(x['count'] for x in off)
print('tid %s: total %.3fs, off-CPU %.3fs (%.0f%%)' % (tid, tot/1e9, offt/1e9, 100*offt/max(tot,1)))
agg = collections.Counter()
for x in off:
    kern = [fr[2] for fr in x['frames'] if fr[1] and 'kernel' in fr[1]]
    kpath = next((k for k in kern if any(w in k for w in ('futex','ep_poll','do_sys_poll','kgsl','unix_stream','sock','sync','fence','wait','nanosleep','read','sendmsg','recv'))), kern[1] if len(kern)>1 else '?')
    user = [short(fr) for fr in x['frames'] if fr[1] and sub in fr[1]][:depth]
    agg[(kpath, ' <- '.join(user))] += x['count']
for (k, u), v in agg.most_common(18):
    print('%5.1f%%  %-28s %s' % (100*v/max(tot,1), k[:28], u[:230]))
on = collections.Counter()
for x in samples:
    if x['off']: continue
    fr = x['frames'][0] if x['frames'] else ['?', '?', '?']
    on[short(fr)] += x['count']
print('-- on-CPU top')
for k, v in on.most_common(15):
    print('%5.1f%%  %s' % (100*v/max(tot,1), k[:150]))
