# symagg.py <report.txt> <libname-substring> <so-path> <base-hex> [top]
# report.txt = simpleperf report --sort dso,vaddr_in_file output; aggregates samples per function.
import sys, re, subprocess, collections, os
rep, sub, so, base = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4], 16)
top = int(sys.argv[5]) if len(sys.argv) > 5 else 30
SYM = os.path.expandvars(r'$LOCALAPPDATA/Android/Sdk/ndk/27.2.12479018/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-symbolizer.exe')
rows = []
for line in open(rep, encoding='utf-8', errors='replace'):
    m = re.match(r'\s*([\d.]+)%\s+(\S+)\s+(0x[0-9a-f]+|\S+)', line)
    if not m: continue
    rows.append((float(m.group(1)), m.group(2), m.group(3)))
addrs = [int(a, 16) + base for p, d, a in rows if sub in d and a.startswith('0x')]
names = {}
if addrs:
    out = subprocess.run([SYM, '--obj=' + so, '-C', '-f', '--inlining=false'] + ['0x%x' % a for a in addrs], capture_output=True, text=True).stdout
    blocks = [b for b in out.strip().split('\n\n')]
    for a, b in zip(addrs, blocks):
        names[a] = b.split('\n')[0][:140]
agg = collections.Counter()
for p, d, a in rows:
    if sub in d and a.startswith('0x'):
        agg[names.get(int(a, 16) + base, a)] += p
    else:
        key = os.path.basename(d) + (' ' + a if d.startswith('[kernel') or 'vdso' in d else '')
        agg[key if not d.startswith('[kernel') else '[kernel]'] += p
for k, v in agg.most_common(top):
    print('%6.2f%%  %s' % (v, k))
