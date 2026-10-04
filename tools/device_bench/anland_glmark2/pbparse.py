import re, sys, glob, os, statistics as st
# usage: pbparse.py <dir>  ; files <round>-<stack>-<cfg>.out
d = sys.argv[1]
data = {}  # (stack,cfg) -> list of dict scene->fps ; plus score
sizes = {}
for f in sorted(glob.glob(os.path.join(d, '*.out'))):
    b = os.path.basename(f)[:-4]
    m = re.match(r'(r\d+)-(\w+)-(\w+)$', b)
    if not m: continue
    rnd, stack, cfg = m.groups()
    txt = open(f, encoding='utf-8', errors='replace').read()
    sc = {}
    for line in txt.splitlines():
        mm = re.match(r'\[(\w+)\] (.*): FPS: (\d+)', line)
        if mm: sc[mm.group(1) + ' ' + mm.group(2)] = int(mm.group(3))
        mm = re.match(r'\[(\w+)\] (.*): Unsupported', line)
        if mm: sc[mm.group(1) + ' ' + mm.group(2)] = None
        mm = re.search(r'glmark2 Score: (\d+)', line)
        if mm: sc['__score'] = int(mm.group(1))
        mm = re.search(r'Surface Size:\s+(\S+ \S+)', line)
        if mm: sizes[(stack, cfg)] = mm.group(1)
    data.setdefault((stack, cfg), []).append((rnd, sc))
import json
out = {}
for k, runs in data.items():
    scenes = {}
    for rnd, sc in runs:
        for s, v in sc.items(): scenes.setdefault(s, []).append(v)
    out['%s/%s' % k] = {'size': sizes.get(k), 'runs': [r for r, _ in runs], 'scenes': scenes}
json.dump(out, open(os.path.join(d, 'all.json'), 'w'), indent=1)
for k in sorted(out):
    s = out[k]['scenes'].get('__score', [])
    print(k, out[k]['size'], 'scores', s, 'median', st.median(s) if s else None)
