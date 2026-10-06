#!/usr/bin/env python3
"""P13 W5 inventory: every preprocessor group whose condition names MOBILEGL_BUILD_DISAGGREGATED,
classified by what its DISAGG-taken branch talks about.

  w5_classify.py [--root DIR] [--csv OUT] [--summary]

Classes (W5 design §3.1):
  A  record terms, no transport token, no MG_Remote name  -> becomes MOBILEGL_BUILD_RECORD_ARM
  B  names MG_Remote, no record term                      -> stays DISAGG
  C  names MG_Remote and record terms                     -> hand split
  D  transport token and record terms, no MG_Remote name  -> hand split
  E  transport token, no record term, no MG_Remote name   -> stays DISAGG
  F  none of the three                                    -> reviewed, biased to record
  N  negated (`#if !MOBILEGL_BUILD_DISAGGREGATED`) or compound condition -> listed for the hand pass

Comments are stripped before matching. Nested DISAGG groups are listed with their depth; a rewrite
touches the outermost of a run and leaves inner transport groups as they are.
"""
import argparse
import csv
import pathlib
import re
import sys

SCAN = ['MobileGL']
SKIP_PARTS = {'MG_Remote', 'generated', '3rdparty'}
EXTS = {'.h', '.hpp', '.cpp', '.cc', '.inc', '.def'}
MACRO = 'MOBILEGL_BUILD_DISAGGREGATED'

DIRECTIVE = re.compile(r'^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)$')

TRANSPORT = [r'ClientSession', r'ServerSession', r'ServerLoop', r'Transport::', r'MG_Config::Ipc\b', r'Ipc\.',
             r'SharedImage', r'AdoptT0', r'\bT0\b', r'externalAhb', r'RunsAsTheServerRole', r'CapsMirror',
             r'SessionLatch', r'ServerRole', r'SplitRoles', r'TransportMode::Spawn', r'TransportMode::InProcess',
             r'ServerOwnedWindow', r'ServerSetContextLive', r'DeviceLost', r'RoleSplit', r'ApplyThread',
             r'MGPipeServerArm', r'MGPipeSessionLive', r'ClientBlock', r'EndedServerSession', r'OnApplyThread',
             r'Transport\s*!=', r'Transport\s*==']
RECORD = [r'DataArmIsRecord', r'RecordArm', r'Staged', r'\bWire', r'MGPipe', r'Handle', r'Record\b', r'Twin',
          r'Respecif', r'Readback', r'Verb', r'PersistentMap', r'GpuWrite', r'Emit', r'Applier', r'Slot',
          r'Residual', r'ContextValues', r'Cso\b', r'CSO', r'Placeholder', r'Mipmap', r'Shadow']
TRANSPORT_RE = re.compile('|'.join(TRANSPORT))
RECORD_RE = re.compile('|'.join(RECORD))
REMOTE_RE = re.compile(r'MG_Remote')


def strip_comments(text):
    text = re.sub(r'/\*.*?\*/', ' ', text, flags=re.S)
    return re.sub(r'//[^\n]*', '', text)


def groups(lines):
    """Yield dicts for each #if group naming MACRO: open/else/close lines, condition, depth, body."""
    stack = []  # entries: {'open', 'cond', 'kind', 'names', 'branches': [(line, kind)], 'depth'}
    out = []
    for no, raw in enumerate(lines, 1):
        m = DIRECTIVE.match(raw)
        if not m:
            continue
        kind, rest = m.group(1), strip_comments(m.group(2)).strip()
        if kind in ('if', 'ifdef', 'ifndef'):
            names = MACRO in rest
            depth = sum(1 for s in stack if s['names'])
            stack.append({'open': no, 'cond': (kind + ' ' + rest).strip(), 'names': names,
                          'branches': [(no, kind)], 'depth': depth})
        elif kind in ('elif', 'else'):
            if stack:
                stack[-1]['branches'].append((no, kind))
        elif kind == 'endif':
            if not stack:
                continue
            g = stack.pop()
            g['close'] = no
            if g['names']:
                out.append(g)
    return out


def classify(path, lines):
    rows = []
    for g in groups(lines):
        cond = g['cond']
        plain = re.fullmatch(r'(if\s+' + MACRO + r'|ifdef\s+' + MACRO + r'|if\s+defined\s*\(?\s*' + MACRO +
                             r'\s*\)?)', cond) is not None
        # The DISAGG-taken branch: first branch for a plain condition.
        first_end = g['branches'][1][0] if len(g['branches']) > 1 else g['close']
        body = strip_comments(''.join(lines[g['open']:first_end - 1]))
        remote = bool(REMOTE_RE.search(body))
        transport = sorted(set(t.group(0) for t in TRANSPORT_RE.finditer(body)))
        record = bool(RECORD_RE.search(body))
        if not plain:
            cls = 'N'
        elif remote and record:
            cls = 'C'
        elif remote:
            cls = 'B'
        elif transport and record:
            cls = 'D'
        elif transport:
            cls = 'E'
        elif record:
            cls = 'A'
        else:
            cls = 'F'
        rows.append({'file': path, 'open': g['open'], 'close': g['close'], 'lines': g['close'] - g['open'] + 1,
                     'depth': g['depth'], 'class': cls, 'cond': cond, 'has_else': len(g['branches']) > 1,
                     'transport': ' '.join(transport)[:120]})
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--csv')
    ap.add_argument('--summary', action='store_true')
    args = ap.parse_args()
    root = pathlib.Path(args.root)
    rows = []
    for top in SCAN:
        for p in sorted((root / top).rglob('*')):
            if p.suffix not in EXTS or not p.is_file():
                continue
            rel = p.relative_to(root)
            if SKIP_PARTS & set(rel.parts):
                continue
            lines = p.read_text(encoding='utf-8', errors='replace').splitlines(keepends=True)
            rows.extend(classify(rel.as_posix(), lines))
    if args.csv:
        with open(args.csv, 'w', newline='', encoding='utf-8') as f:
            w = csv.DictWriter(f, fieldnames=list(rows[0].keys()) if rows else ['file'])
            w.writeheader()
            w.writerows(rows)
    if args.summary or not args.csv:
        by = {}
        for r in rows:
            top = r['file'].split('/')[1] if '/' in r['file'] else r['file']
            if top.startswith('MG_Backend') and len(r['file'].split('/')) > 2:
                top = '/'.join(r['file'].split('/')[1:3])
            key = (top, r['class'])
            n, l = by.get(key, (0, 0))
            by[key] = (n + 1, l + r['lines'])
        classes = 'ABCDEFN'
        print('%-28s' % 'dir' + ''.join('%14s' % c for c in classes))
        for top in sorted(set(k[0] for k in by)):
            print('%-28s' % top + ''.join('%14s' % ('%d/%d' % by.get((top, c), (0, 0))) for c in classes))
        tot = {c: (sum(by[k][0] for k in by if k[1] == c), sum(by[k][1] for k in by if k[1] == c)) for c in classes}
        print('%-28s' % 'TOTAL blocks/lines' + ''.join('%14s' % ('%d/%d' % tot[c]) for c in classes))
    return 0


if __name__ == '__main__':
    sys.exit(main())
