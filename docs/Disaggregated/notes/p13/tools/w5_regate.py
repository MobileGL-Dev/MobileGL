#!/usr/bin/env python3
"""P13 W5 S3: hand-directed guard edits, by content rather than line number.

  w5_regate.py <ops-file>

Each non-comment line of the ops file is one operation (fields separated by ' | '):
  open   | <path> | <text>              the `#if MOBILEGL_BUILD_DISAGGREGATED` group whose first body line
                                        contains <text> becomes `#if MOBILEGL_BUILD_RECORD_ARM`
                                        (with its #else / #endif comments)
  nest   | <path> | <first> | <last>    wrap the lines from the one containing <first> through the one
                                        containing <last> (searched after <first>) in
                                        `#if MOBILEGL_BUILD_DISAGGREGATED` / `#endif`
  line   | <path> | <text> | <new>      replace the one line containing <text> with <new> verbatim
Every <text> must match exactly one place (for `open`: exactly one group).
"""
import pathlib
import re
import sys

OLD = 'MOBILEGL_BUILD_DISAGGREGATED'
NEW = 'MOBILEGL_BUILD_RECORD_ARM'
DIRECTIVE = re.compile(r'^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)$')


def load(path):
    raw = pathlib.Path(path).read_bytes().decode('utf-8')
    return '\r\n' in raw, raw.replace('\r\n', '\n').split('\n')


def save(path, crlf, lines):
    text = '\n'.join(lines)
    pathlib.Path(path).write_bytes((text.replace('\n', '\r\n') if crlf else text).encode('utf-8'))


def groups(lines):
    stack, out = [], []
    for i, line in enumerate(lines):
        m = DIRECTIVE.match(line)
        if not m:
            continue
        kind = m.group(1)
        if kind in ('if', 'ifdef', 'ifndef'):
            stack.append({'open': i, 'branches': []})
        elif kind in ('elif', 'else'):
            if stack:
                stack[-1]['branches'].append(i)
        elif stack:
            g = stack.pop()
            g['close'] = i
            out.append(g)
    return out


def op_open(lines, text):
    hits = []
    for g in groups(lines):
        if not re.match(r'^\s*#\s*if\s+' + OLD + r'\s*(//.*)?$', lines[g['open']]):
            continue
        end = g['branches'][0] if g['branches'] else g['close']
        if any(text in l for l in lines[g['open'] + 1:end]):
            hits.append(g)
    # The innermost group that contains the text: drop any group that contains another hit.
    hits = [g for g in hits if not any(h is not g and g['open'] < h['open'] and h['close'] < g['close'] for h in hits)]
    if len(hits) != 1:
        raise SystemExit('open: %d groups contain %r' % (len(hits), text))
    g = hits[0]
    for i in [g['open']] + g['branches'] + [g['close']]:
        lines[i] = lines[i].replace(OLD, NEW)


def find_one(lines, text, start=0):
    idx = [i for i in range(start, len(lines)) if text in lines[i]]
    if not idx:
        raise SystemExit('no line contains %r' % text)
    if start == 0 and len(idx) != 1:
        raise SystemExit('%d lines contain %r' % (len(idx), text))
    return idx[0]


def main():
    ops = [l for l in pathlib.Path(sys.argv[1]).read_text(encoding='utf-8').splitlines()
           if l.strip() and not l.lstrip().startswith('#')]
    files = {}
    for op in ops:
        parts = [p.strip() for p in op.split(' | ')]
        kind, path = parts[0], parts[1]
        if path not in files:
            files[path] = load(path)
        crlf, lines = files[path]
        if kind == 'open':
            op_open(lines, parts[2])
        elif kind == 'nest':
            a = find_one(lines, parts[2])
            b = find_one(lines, parts[3], a)
            lines[b + 1:b + 1] = ['#endif']
            lines[a:a] = ['#if ' + OLD]
        elif kind == 'line':
            a = find_one(lines, parts[2])
            lines[a] = parts[3]
        else:
            raise SystemExit('unknown op ' + kind)
    for path, (crlf, lines) in files.items():
        save(path, crlf, lines)
    print('applied', len(ops), 'op(s) to', len(files), 'file(s)')


if __name__ == '__main__':
    main()
