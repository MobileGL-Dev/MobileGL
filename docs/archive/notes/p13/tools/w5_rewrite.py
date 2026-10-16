#!/usr/bin/env python3
"""P13 W5 S2: move the record arm's guards from MOBILEGL_BUILD_DISAGGREGATED to
MOBILEGL_BUILD_RECORD_ARM, by w5_classify.py's classes.

  w5_rewrite.py [--root DIR] [--dir PREFIX ...] [--dry-run]

Rewritten (the macro on the opening line, and on its #else / #endif comment if it names it):
  * a plain `#if MOBILEGL_BUILD_DISAGGREGATED` group of class A or F (record terms or nothing, no
    transport word, no MG_Remote name);
  * every plain `#if !MOBILEGL_BUILD_DISAGGREGATED` (the frontend arm of a build without the record arm);
  * `#if MOBILEGL_PIPE_VERIFY && !MOBILEGL_BUILD_DISAGGREGATED` (the verify tree's frontend pins).
Left for the hand pass (S3): B, C, D, E and the compound guards; the MOBILEGL_PIPE_POISON arm in
PipeInputs.h stays on DISAGG on purpose (the FCL library must not pay a per-read poison check).
Tests (MG_Test, MG_IntegrationTest) are S6's.

While MOBILEGL_BUILD_RECORD_ARM equals MOBILEGL_BUILD_DISAGGREGATED (until S7) every rewrite is a
no-op for the compiler: both trees keep byte-identical code.
"""
import argparse
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import w5_classify as wc  # noqa: E402

OLD = 'MOBILEGL_BUILD_DISAGGREGATED'
NEW = 'MOBILEGL_BUILD_RECORD_ARM'
SKIP_TOP = ('MobileGL/MG_Test/', 'MobileGL/MG_IntegrationTest/')
# S2 touches only the trees the record core lives in. The EGL layer, the GL context's device-loss
# state, the logger's role files and the stats dump paths are session machinery whose guards the
# token classes cannot tell apart from record code; they are S3's hand pass.
RECORD_DIRS = ('MobileGL/MG_Backend/DirectGLES/', 'MobileGL/MG_Backend/DirectVulkan/', 'MobileGL/MG_Backend/MGPipe/',
               'MobileGL/MG_Pipe/', 'MobileGL/MG_Impl/Pipe/', 'MobileGL/MG_Impl/GLImpl/',
               'MobileGL/MG_State/GLState/BufferState/', 'MobileGL/MG_State/GLState/ProgramState/',
               'MobileGL/MG_State/GLState/FramebufferState/', 'MobileGL/MG_Util/SelfTest/')


def rewrite_file(path, rel, lines):
    rows = wc.classify(rel, lines)
    changed = []
    for g in wc.groups(lines):
        cond = g['cond']
        plain = re.fullmatch(r'if\s+' + OLD, cond) is not None
        negated = re.fullmatch(r'if\s+!\s*' + OLD, cond) is not None
        verify_pin = re.fullmatch(r'if\s+MOBILEGL_PIPE_VERIFY\s*&&\s*!\s*' + OLD, cond) is not None
        if plain:
            cls = next(r['class'] for r in rows if r['open'] == g['open'])
            if cls not in ('A', 'F'):
                continue
        elif not (negated or verify_pin):
            continue
        targets = [g['open']] + [no for no, kind in g['branches'][1:]] + [g['close']]
        for no in targets:
            line = lines[no - 1]
            if no == g['open'] or OLD in line:
                lines[no - 1] = line.replace(OLD, NEW)
        changed.append((g['open'], 'plain' if plain else 'negated' if negated else 'verify-pin'))
    return changed


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--dir', action='append', default=[])
    ap.add_argument('--dry-run', action='store_true')
    args = ap.parse_args()
    root = pathlib.Path(args.root)
    total = 0
    for p in sorted((root / 'MobileGL').rglob('*')):
        if p.suffix not in wc.EXTS or not p.is_file():
            continue
        rel = p.relative_to(root).as_posix()
        if wc.SKIP_PARTS & set(pathlib.PurePosixPath(rel).parts) or rel.startswith(SKIP_TOP) or not rel.startswith(RECORD_DIRS):
            continue
        if args.dir and not any(rel.startswith(d) for d in args.dir):
            continue
        raw = p.read_bytes().decode('utf-8')
        crlf = '\r\n' in raw
        lines = raw.replace('\r\n', '\n').splitlines(keepends=True)
        changed = rewrite_file(p, rel, lines)
        if not changed:
            continue
        total += len(changed)
        print('%-70s %3d' % (rel, len(changed)))
        if not args.dry_run:
            text = ''.join(lines)
            p.write_bytes((text.replace('\n', '\r\n') if crlf else text).encode('utf-8'))
    print('groups rewritten:', total)
    return 0


if __name__ == '__main__':
    sys.exit(main())
