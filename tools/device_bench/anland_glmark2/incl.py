# incl.py <samples> <tid> <so> <base> <dsosub> [top] - inclusive on-CPU %, and leaf-dso -> first own frame
import sys, re, subprocess, collections, os
f, tid, so, base, sub = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4], 16), sys.argv[5]
top = int(sys.argv[6]) if len(sys.argv) > 6 else 40
SYM = os.path.expandvars(r'$LOCALAPPDATA/Android/Sdk/ndk/27.2.12479018/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-symbolizer.exe')
S = []; cur = None
for line in open(f, encoding='utf-8', errors='replace'):
    s = line.strip()
    if s == 'sample:':
        if cur: S.append(cur)
        cur = {'ev': '', 'tid': '', 'fr': []}; continue
    if cur is None: continue
    if s.startswith('event_type:'): cur['ev'] = s.split()[1]
    elif s.startswith('thread_id:'): cur['tid'] = s.split()[1]
    elif s.startswith('vaddr_in_file:'): cur['fr'].append([s.split()[1], '', ''])
    elif s.startswith('file:') and cur['fr']: cur['fr'][-1][1] = s.split(None, 1)[1]
    elif s.startswith('symbol:') and cur['fr']: cur['fr'][-1][2] = s.split(None, 1)[1]
if cur: S.append(cur)
S = [x for x in S if x['tid'] == tid and not x['ev'].startswith('sched')]
addrs = sorted({int(a, 16) + base for x in S for a, d, _ in x['fr'] if sub in d})
names = {}
for i in range(0, len(addrs), 3000):
    part = addrs[i:i+3000]
    out = subprocess.run([SYM, '--obj=' + so, '-C', '-f', '--inlining=false'] + ['0x%x' % a for a in part], capture_output=True, text=True).stdout
    for a, b in zip(part, out.strip().split('\n\n')):
        n = re.sub(r'\(.*', '', b.split('\n')[0]).replace('MobileGL::', '').replace('MG_Backend::DirectVulkan::', 'DV::').replace('MG_Backend::DirectGLES::', 'DG::')
        names[a] = n[:110]
def nm(fr):
    a, d, sy = fr
    if sub in d: return names.get(int(a, 16) + base, a)
    return (sy if sy and not sy.startswith('*') and not sy.startswith('!!!') else os.path.basename(d))[:60]
N = len(S)
inc = collections.Counter(); leafmap = collections.Counter()
for x in S:
    seen = set()
    for fr in x['fr']:
        if sub in fr[1]:
            n = nm(fr)
            if n not in seen: seen.add(n); inc[n] += 1
    if x['fr'] and sub not in x['fr'][0][1]:
        own = next((nm(fr) for fr in x['fr'] if sub in fr[1]), '?')
        leafmap[(os.path.basename(x['fr'][0][1]) or '?', own)] += 1
print('on-CPU samples', N)
for k, v in inc.most_common(top): print('%5.1f%%  %s' % (100.0*v/N, k))
print('-- foreign leaf -> first own frame')
for (d, o), v in leafmap.most_common(25): print('%5.1f%%  %-22s %s' % (100.0*v/N, d[:22], o))
