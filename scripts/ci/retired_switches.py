#!/usr/bin/env python3
"""P13 W3b: the pull build's switches stay retired.

The pull build's macro (MOBILEGL_PIPE_PUSH as a preprocessor condition), the legacy-memo macro
(MOBILEGL_PIPE_LEGACY_MEMOS) and the MGB_CTX spelling of the backends' context were removed in
P13 W3b. Any of them coming back in source is a second arm nobody tests, so it is a red here.

The RUNTIME MOBILEGL_PIPE_PUSH environment bitmask (bit 63 is still a switch, bits 0-13 are fixed
on) is a different thing and is not matched: only a preprocessor conditional naming the macro is.

Usage: retired_switches.py [--self-test]
"""
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
CODE_GLOBS = ('*.cpp', '*.h', '*.hpp', '*.inc', '*.def', '*.c')
RULES = (
    (re.compile(r'^\s*#\s*(?:if|ifdef|ifndef|elif)\b.*\b(?:MOBILEGL_PIPE_PUSH|MOBILEGL_PIPE_LEGACY_MEMOS)\b'),
     'a preprocessor conditional names a macro P13 W3b retired (MOBILEGL_PIPE_PUSH / '
     'MOBILEGL_PIPE_LEGACY_MEMOS)'),
    (re.compile(r'\bMGB_CTX(?:_LIVE|_IDENTITY)?\b'),
     'MGB_CTX is back; backends read MG_Pipe::gPipeInputs directly since P13 W3b'),
)


def scan(files):
    findings = []
    for path, text in files:
        for number, line in enumerate(text.splitlines(), start=1):
            for pattern, message in RULES:
                if pattern.search(line):
                    findings.append(f'{path}:{number}: {message}: {line.strip()}')
    return findings


def tree_files():
    names = subprocess.check_output(['git', 'ls-files', '--', *CODE_GLOBS], cwd=ROOT, text=True).split()
    for name in names:
        try:
            yield name, (ROOT / name).read_text(encoding='utf-8', errors='replace')
        except FileNotFoundError:
            continue


def self_test():
    planted = [('planted.cpp', '#if MOBILEGL_PIPE_PUSH\nx();\n#endif\n'),
               ('planted.h', '  #  elif !MOBILEGL_PIPE_LEGACY_MEMOS && FOO\n'),
               ('planted.inc', 'auto v = MGB_CTX->GetX();\n'),
               ('planted2.cpp', 'if (MGB_CTX_LIVE) {}\n')]
    for item in planted:
        assert scan([item]), f'the gate missed a planted line: {item}'
    clean = [('clean.cpp', 'features.PipePush = QueryEnvUint64("MOBILEGL_PIPE_PUSH", 0x3fff);\n'),
             ('clean.h', '// MOBILEGL_PIPE_PUSH=0x7f was the P2 mask\n'),
             ('clean.inc', 'MG_Pipe::gPipeInputs.GetX();\n')]
    for item in clean:
        assert not scan([item]), f'the gate fired on a legitimate line: {item}'
    print(f'retired switches self-test: {len(planted)} planted lines caught, {len(clean)} clean lines left alone')


def main():
    if '--self-test' in sys.argv[1:]:
        self_test()
    findings = scan(tree_files())
    for line in findings:
        print(f'::error::{line}')
    if findings:
        return 1
    print('retired switches: none of MOBILEGL_PIPE_PUSH / MOBILEGL_PIPE_LEGACY_MEMOS (as #if) or MGB_CTX in source')
    return 0


if __name__ == '__main__':
    sys.exit(main())
