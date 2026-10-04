import json, sys, statistics as st
d = json.load(open(sys.argv[1]))
stacks = ['stock', 'espryt', 'magma']
cfgs = [('es2w', 'glmark2-es2-wayland, 800x600 window'), ('es2f', 'glmark2-es2-wayland, fullscreen'),
        ('glw', 'glmark2-wayland (desktop GL), 800x600 window'), ('glf', 'glmark2-wayland (desktop GL), fullscreen')]
def med(v):
    v = [x for x in v if x is not None]
    return st.median(v) if v else None
def spread(v):
    v = [x for x in v if x is not None]
    return (min(v), max(v)) if v else (None, None)
out = []
out.append('### Overall scores (median of runs; min-max in brackets; n = runs)\n')
out.append('| Configuration | stock | Espryt | Magma | Espryt/stock | Magma/stock | Espryt/stock (common scenes) | Magma/stock (common scenes) |')
out.append('|---|---|---|---|---|---|---|---|')
for c, label in cfgs:
    row = [label]
    meds = {}
    common = {}
    for s in stacks:
        k = '%s/%s' % (s, c)
        if k not in d: row.append('-'); continue
        sc = d[k]['scenes'].get('__score', [])
        m = med(sc); lo, hi = spread(sc); meds[s] = m
        row.append('%d [%d-%d] n=%d' % (m, lo, hi, len(sc)) if m else '-')
    # common-scene mean of medians
    keys = None
    for s in stacks:
        k = '%s/%s' % (s, c)
        if k not in d: continue
        ks = {n for n, v in d[k]['scenes'].items() if n != '__score' and med(v) is not None}
        keys = ks if keys is None else keys & ks
    for s in stacks:
        k = '%s/%s' % (s, c)
        if k in d and keys:
            common[s] = st.mean(med(d[k]['scenes'][n]) for n in keys)
    for s in ['espryt', 'magma']:
        row.append('%.2f' % (meds[s] / meds['stock']) if meds.get(s) and meds.get('stock') else '-')
    for s in ['espryt', 'magma']:
        row.append('%.2f' % (common[s] / common['stock']) if common.get(s) and common.get('stock') else '-')
    out.append('| ' + ' | '.join(row) + ' |')
out.append('')
for c, label in cfgs:
    out.append('### Per-scene FPS: %s (median of runs)\n' % label)
    out.append('| Scene | stock | Espryt | Magma | Espryt/stock | Magma/stock |')
    out.append('|---|---|---|---|---|---|')
    names = []
    for s in stacks:
        k = '%s/%s' % (s, c)
        if k in d:
            for n in d[k]['scenes']:
                if n != '__score' and n not in names: names.append(n)
    for n in names:
        vals = {}
        for s in stacks:
            k = '%s/%s' % (s, c)
            v = d.get(k, {}).get('scenes', {}).get(n)
            vals[s] = med(v) if v else None
            if v is not None and all(x is None for x in v): vals[s] = 'unsupported'
        def f(x): return x if isinstance(x, str) else ('-' if x is None else '%d' % x)
        def r(a, b):
            return '%.2f' % (a / b) if isinstance(a, (int, float)) and isinstance(b, (int, float)) and b else '-'
        out.append('| %s | %s | %s | %s | %s | %s |' % (n.replace('|', '/'), f(vals['stock']), f(vals['espryt']), f(vals['magma']),
                   r(vals['espryt'], vals['stock']), r(vals['magma'], vals['stock'])))
    out.append('')
print('\n'.join(out))
