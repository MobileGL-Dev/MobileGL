#!/usr/bin/env python3
# MobileGL - scripts/ci/build_fingerprint.py
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

"""Prove that two CMake build trees were configured identically, apart from the benchmarks.

build-linux and build-linux-split configure the same sources twice. On the disaggregation
branches the only intended difference is MOBILEGL_BUILD_BENCHMARK, which adds MG_Benchmark and the
google benchmark dependency and touches nothing else. When the two configurations are equal, the
split consumers can take build-linux's artifacts and build-linux-split need not compile again.

The fingerprint is read from what configure writes, so it can be taken before the build:
  cache      MOBILEGL_* entries (except MOBILEGL_BUILD_BENCHMARK), the build type, compilers,
             launchers, flags and the link tools from CMakeCache.txt;
  compile    every compile_commands.json entry: file, output and arguments;
  link       every link statement of build.ninja: outputs, rule, inputs and variables;
  tests      the text of every CTestTestfile.cmake and gtest *_include.cmake;
  toolchain  compiler, linker, cmake, ninja and package versions on this runner;
  image      the runner image, recorded and never compared.
The build directory's own path is replaced by @BUILD@, and everything under MobileGL/MG_Benchmark
or the benchmark dependency is left out. A fingerprint with fewer than 200 compile entries, 3 link
statements (libMobileGL.so among them) or 2 CTest files proves nothing and never counts as equal.

  build_fingerprint.py fingerprint --build-dir D [--out F]
  build_fingerprint.py compare A.json B.json
  build_fingerprint.py guard --built D --probe primary|split --probe-dir P [--github-output KEY]
  build_fingerprint.py --self-test

guard configures the probe with scripts/ci/configure_build.sh (configure only), compares it with
the built tree and writes KEY=true|false to $GITHUB_OUTPUT. Any error makes it false, and it always
exits 0: a difference sends the workflow down the path that builds both, it is not a red.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
CONFIGURE = ROOT / 'scripts' / 'ci' / 'configure_build.sh'

MIN_COMPILE = 200
MIN_LINK = 3
MIN_TESTS = 2

CACHE_KEY = re.compile(
    r'^(MOBILEGL_\w+'
    r'|CMAKE_BUILD_TYPE'
    r'|CMAKE_(C|CXX)_COMPILER(_LAUNCHER)?'
    r'|CMAKE_(C|CXX)_FLAGS(_\w+)?'
    r'|CMAKE_(EXE|SHARED|MODULE|STATIC)_LINKER_FLAGS(_\w+)?'
    r'|CMAKE_INTERPROCEDURAL_OPTIMIZATION|CMAKE_POSITION_INDEPENDENT_CODE'
    r'|CMAKE_LINKER|CMAKE_AR|CMAKE_RANLIB)$')
CACHE_SKIP = {'MOBILEGL_BUILD_BENCHMARK'}
CACHE_LINE = re.compile(r'^([A-Za-z0-9_.+-]+):([A-Z]+)=(.*)$')
LINK_RULE = re.compile(r'^(C|CXX)_(SHARED_LIBRARY|EXECUTABLE|MODULE|STATIC_LIBRARY)_LINKER__')
BENCHMARK_PATH = re.compile(r'(^|/)(MobileGL/MG_Benchmark|_deps/benchmark-[a-z]+)(/|$)')
BENCHMARK_SUBDIRS = re.compile(r'^\s*subdirs\("[^"]*(MobileGL/MG_Benchmark|_deps/benchmark-build)"\)\s*$')
PACKAGES = ('clang-20', 'lld-20', 'libc++-20-dev', 'libc++abi-20-dev', 'libvulkan-dev',
            'libegl1-mesa-dev', 'libgles2-mesa-dev')


def normaliser(build_dir):
    roots = sorted({str(Path(build_dir)), str(Path(build_dir).resolve())}, key=len, reverse=True)

    def norm(text):
        for root in roots:
            text = text.replace(root, '@BUILD@')
        return text
    return norm


def is_benchmark(path):
    return bool(BENCHMARK_PATH.search(path.replace('\\', '/')))


def read_cache(build, norm):
    cache = {}
    path = build / 'CMakeCache.txt'
    if not path.is_file():
        return cache
    for line in path.read_text(errors='replace').splitlines():
        m = CACHE_LINE.match(line)
        if m and CACHE_KEY.match(m.group(1)) and m.group(1) not in CACHE_SKIP:
            cache[m.group(1)] = norm(m.group(3))
    return cache


def read_compile(build, norm):
    path = build / 'compile_commands.json'
    rows = []
    if not path.is_file():
        return rows
    for entry in json.loads(path.read_text(errors='replace')):
        directory = entry.get('directory', '')
        source = entry.get('file', '')
        if 'arguments' in entry:
            argv = list(entry['arguments'])
        else:
            argv = shlex.split(entry.get('command', ''))
        output = entry.get('output', '')
        rel_out = norm(os.path.join(directory, output)) if output else ''
        if is_benchmark(norm(source)) or is_benchmark(rel_out) or is_benchmark(norm(directory)):
            continue
        rows.append([norm(source), rel_out] + [norm(arg) for arg in argv])
    return sorted(rows)


def ninja_lines(text):
    """build.ninja's logical lines: a line ending in an unescaped $ continues on the next one."""
    out, pending = [], None
    for raw in text.splitlines():
        line = raw if pending is None else pending + raw.lstrip()
        trailing = len(line) - len(line.rstrip('$'))
        if trailing % 2 == 1:
            pending = line[:-1]
            continue
        pending = None
        out.append(line)
    if pending is not None:
        out.append(pending)
    return out


def split_unescaped(text):
    """Ninja tokens: split on spaces that are not escaped as '$ '."""
    tokens, current, i = [], '', 0
    while i < len(text):
        c = text[i]
        if c == '$' and i + 1 < len(text):
            current += text[i:i + 2]
            i += 2
            continue
        if c == ' ':
            if current:
                tokens.append(current)
            current = ''
        else:
            current += c
        i += 1
    if current:
        tokens.append(current)
    return tokens


def read_links(build, norm):
    links = {}
    if not (build / 'build.ninja').is_file():
        return links
    lines = ninja_lines((build / 'build.ninja').read_text(errors='replace'))
    i = 0
    while i < len(lines):
        line = lines[i]
        i += 1
        if not line.startswith('build '):
            continue
        head = line[len('build '):]
        colon = re.search(r'(?<!\$):', head)
        if colon is None:
            continue
        outputs = split_unescaped(head[:colon.start()])
        rest = split_unescaped(head[colon.end():])
        if not rest or not LINK_RULE.match(rest[0]):
            continue
        rule, inputs = rest[0], []
        for token in rest[1:]:
            if token in ('|', '||', '|@'):
                break
            inputs.append(norm(token))
        variables = {}
        while i < len(lines) and lines[i].startswith((' ', '\t')):
            key, _, value = lines[i].strip().partition(' = ')
            variables[key] = norm(value)
            i += 1
        names = [norm(o) for o in outputs if o != '|']
        if any(is_benchmark(name) for name in names):
            continue
        links[' '.join(names)] = {'rule': rule, 'inputs': sorted(inputs), 'vars': variables}
    return links


def read_tests(build, norm):
    tests = {}
    for path in sorted(build.rglob('*.cmake')):
        rel = path.relative_to(build).as_posix()
        if path.name != 'CTestTestfile.cmake' and not path.name.endswith('_include.cmake'):
            continue
        if is_benchmark(rel) or '/CMakeFiles/' in '/' + rel:
            continue
        text = path.read_text(errors='replace')
        kept = [norm(line) for line in text.splitlines()
                if not BENCHMARK_SUBDIRS.match(line) and not line.startswith('# ')]
        tests[rel] = '\n'.join(kept)
    return tests


def run_version(argv):
    try:
        out = subprocess.run(argv, capture_output=True, text=True, timeout=60)
        return (out.stdout + out.stderr).strip()
    except (OSError, subprocess.SubprocessError) as error:
        return f'unavailable: {error.__class__.__name__}'


def read_toolchain():
    tools = {}
    for name in ('MOBILEGL_CI_CC', 'MOBILEGL_CI_CXX', 'MOBILEGL_CI_LLD'):
        exe = os.environ.get(name)
        tools[name] = run_version([exe, '--version']) if exe else 'unset'
    tools['cmake'] = run_version(['cmake', '--version'])
    tools['ninja'] = run_version(['ninja', '--version'])
    tools['packages'] = run_version(['dpkg-query', '-W', '-f=${Package} ${Version}\\n', *PACKAGES])
    return tools


def fingerprint(build_dir, toolchain=None):
    build = Path(build_dir)
    norm = normaliser(build)
    print_counts = {}
    result = {
        'cache': read_cache(build, norm),
        'compile': read_compile(build, norm),
        'link': read_links(build, norm),
        'tests': read_tests(build, norm),
        'toolchain': read_toolchain() if toolchain is None else toolchain,
        'image': {key: os.environ.get(key, '') for key in ('ImageOS', 'ImageVersion', 'RUNNER_NAME')},
    }
    print_counts['compile'] = len(result['compile'])
    print_counts['link'] = len(result['link'])
    print_counts['tests'] = len(result['tests'])
    result['counts'] = print_counts
    return result


def unproven(fp):
    counts = fp['counts']
    problems = []
    if counts['compile'] < MIN_COMPILE:
        problems.append(f"{counts['compile']} compile entries (< {MIN_COMPILE})")
    if counts['link'] < MIN_LINK:
        problems.append(f"{counts['link']} link statements (< {MIN_LINK})")
    if not any(name.split()[0].endswith('libMobileGL.so') for name in fp['link']):
        problems.append('no link statement for libMobileGL.so')
    if counts['tests'] < MIN_TESTS:
        problems.append(f"{counts['tests']} CTest files (< {MIN_TESTS})")
    return problems


def compare(a, b, limit=8):
    """Per-field differences between two fingerprints; empty means equal."""
    diffs = {}
    keys = sorted(set(a['cache']) | set(b['cache']))
    cache = [f"{k}: {a['cache'].get(k, '<absent>')!r} vs {b['cache'].get(k, '<absent>')!r}"
             for k in keys if a['cache'].get(k) != b['cache'].get(k)]
    if cache:
        diffs['cache'] = cache
    ca = {tuple(row) for row in a['compile']}
    cb = {tuple(row) for row in b['compile']}
    if ca != cb:
        only_a = sorted(ca - cb)
        only_b = sorted(cb - ca)
        diffs['compile'] = ([f'only in first: {row[0]} -> {row[1]}' for row in only_a[:limit]]
                            + [f'only in second: {row[0]} -> {row[1]}' for row in only_b[:limit]]
                            + [f'{len(only_a)} vs {len(only_b)} differing entries'])
    link = []
    for name in sorted(set(a['link']) | set(b['link'])):
        la, lb = a['link'].get(name), b['link'].get(name)
        if la == lb:
            continue
        if la is None or lb is None:
            link.append(f"{name}: only in {'second' if la is None else 'first'}")
            continue
        for field in ('rule', 'inputs'):
            if la[field] != lb[field]:
                link.append(f'{name}: {field} differs')
        for var in sorted(set(la['vars']) | set(lb['vars'])):
            if la['vars'].get(var) != lb['vars'].get(var):
                link.append(f'{name}: {var} differs')
    if link:
        diffs['link'] = link
    tests = [f"{name}: {'differs' if name in a['tests'] and name in b['tests'] else 'only in one'}"
             for name in sorted(set(a['tests']) | set(b['tests']))
             if a['tests'].get(name) != b['tests'].get(name)]
    if tests:
        diffs['tests'] = tests
    toolchain = [k for k in sorted(set(a['toolchain']) | set(b['toolchain']))
                 if a['toolchain'].get(k) != b['toolchain'].get(k)]
    if toolchain:
        diffs['toolchain'] = [f'{k} differs' for k in toolchain]
    return {field: rows[:limit] + ([f'... {len(rows) - limit} more'] if len(rows) > limit else [])
            for field, rows in diffs.items()}


def verdict(a, b):
    """(identical, reasons). Unproven evidence on either side is never 'identical'."""
    reasons = [f'first: {p}' for p in unproven(a)] + [f'second: {p}' for p in unproven(b)]
    diffs = compare(a, b)
    for field, rows in diffs.items():
        reasons += [f'{field}: {row}' for row in rows]
    return not reasons, reasons


def write_output(key, identical):
    path = os.environ.get('GITHUB_OUTPUT')
    line = f"{key}={'true' if identical else 'false'}"
    print(line)
    if path:
        with open(path, 'a') as out:
            out.write(line + '\n')


def guard(args):
    identical = False
    try:
        probe = Path(args.probe_dir)
        if probe.exists():
            shutil.rmtree(probe)
        configured = subprocess.run(['bash', str(CONFIGURE), args.probe, str(probe)], cwd=ROOT,
                                    capture_output=True, text=True)
        if configured.returncode != 0:
            print(configured.stdout[-4000:] + configured.stderr[-4000:])
            raise RuntimeError(f'configure_build.sh {args.probe} failed with exit {configured.returncode}')
        toolchain = read_toolchain()
        built = fingerprint(args.built, toolchain)
        probed = fingerprint(probe, toolchain)
        print(f"build_fingerprint: built {args.built}: {built['counts']}; probe {args.probe} at {probe}: "
              f"{probed['counts']}")
        print(f"build_fingerprint: compiler {toolchain.get('MOBILEGL_CI_CXX', '').splitlines()[:1]}, "
              f"image {built['image']}")
        identical, reasons = verdict(built, probed)
        for reason in reasons:
            print(f'build_fingerprint: {reason}')
        if args.report:
            Path(args.report).write_text(json.dumps({'identical': identical, 'reasons': reasons,
                                                     'built': built['counts'], 'probe': probed['counts'],
                                                     'toolchain': toolchain, 'image': built['image']},
                                                    indent=1) + '\n')
    except Exception as error:  # a guard that cannot decide must not reuse anything
        print(f'build_fingerprint: guard could not compare the configurations: {error}')
        identical = False
    write_output(args.github_output, identical)
    return 0


# ------------------------------------------------------------------------------------- self-test

def canned_tree(root, *, name='build', mobilegl_flag='ON', build_type='Release', define='-DMG=1',
                link_flag='-Wl,--as-needed', test_env='VK_ICD_FILENAMES=/icd.json', benchmark=False,
                empty=False):
    build = root / name
    build.mkdir(parents=True)
    if empty:
        return build
    abs_build = str(build)
    (build / 'CMakeCache.txt').write_text(
        f'CMAKE_BUILD_TYPE:STRING={build_type}\n'
        'CMAKE_CXX_COMPILER:FILEPATH=/usr/bin/clang++-20\n'
        'CMAKE_CXX_COMPILER_LAUNCHER:STRING=ccache\n'
        'CMAKE_CXX_FLAGS:STRING=\n'
        f'MOBILEGL_BUILD_DISAGGREGATED:BOOL={mobilegl_flag}\n'
        f"MOBILEGL_BUILD_BENCHMARK:BOOL={'ON' if benchmark else 'OFF'}\n"
        f'CMAKE_CACHEFILE_DIR:INTERNAL={abs_build}\n')
    entries = [{'directory': abs_build, 'file': f'/src/MobileGL/f{i}.cpp',
                'output': f'CMakeFiles/MobileGL.dir/f{i}.cpp.o',
                'command': f'ccache clang++ {define if i == 7 else "-DMG=1"} -I{abs_build}/gen '
                           f'-o CMakeFiles/MobileGL.dir/f{i}.cpp.o -c /src/MobileGL/f{i}.cpp'}
               for i in range(MIN_COMPILE + 5)]
    if benchmark:
        entries.append({'directory': abs_build + '/MobileGL/MG_Benchmark', 'file': '/src/MobileGL/MG_Benchmark/b.cpp',
                        'output': 'CMakeFiles/SanityBench.dir/b.cpp.o', 'command': 'clang++ -c b.cpp'})
    (build / 'compile_commands.json').write_text(json.dumps(entries))
    links = [
        ('libMobileGL.so', 'CXX_SHARED_LIBRARY_LINKER__MobileGL_Release', 'CMakeFiles/MobileGL.dir/f1.cpp.o'),
        ('libMobileGL_s.a', 'CXX_STATIC_LIBRARY_LINKER__MobileGL_s_Release', 'CMakeFiles/MobileGL.dir/f2.cpp.o'),
        ('MobileGL/MG_Test/SanityTest', 'CXX_EXECUTABLE_LINKER__SanityTest_Release', 'libMobileGL_s.a'),
    ]
    if benchmark:
        links.append(('MobileGL/MG_Benchmark/SanityBench', 'CXX_EXECUTABLE_LINKER__SanityBench_Release',
                      'libMobileGL_s.a'))
    ninja = ['ninja_required_version = 1.5', '']
    for out, rule, inp in links:
        ninja += [f'build {out}: {rule} {inp} | /usr/lib/libc.so || cmake_object_order_depends_target_x',
                  f'  LINK_FLAGS = {link_flag}', '  LINK_LIBRARIES = -lEGL $', '      -lvulkan',
                  f'  POST_BUILD = cd {abs_build}/MobileGL && :', '']
    ninja.append('build CMakeFiles/x.o: CXX_COMPILER__MobileGL_Release /src/x.cpp')
    (build / 'build.ninja').write_text('\n'.join(ninja) + '\n')
    subdirs = 'subdirs("MobileGL/MG_Test")\n' + ('subdirs("MobileGL/MG_Benchmark")\n' if benchmark else '')
    (build / 'CTestTestfile.cmake').write_text(f'# CMake generated Testfile for {abs_build}\n{subdirs}')
    (build / 'MobileGL' / 'MG_Test').mkdir(parents=True)
    (build / 'MobileGL' / 'MG_Test' / 'CTestTestfile.cmake').write_text(
        f'add_test([=[T]=] "{abs_build}/MobileGL/MG_Test/SanityTest")\n'
        f'set_tests_properties([=[T]=] PROPERTIES ENVIRONMENT "{test_env}")\n')
    if benchmark:
        (build / 'MobileGL' / 'MG_Benchmark').mkdir(parents=True)
        (build / 'MobileGL' / 'MG_Benchmark' / 'CTestTestfile.cmake').write_text(
            'add_test([=[SanityBench]=] "SanityBench")\n')
    return build


def self_test():
    toolchain = {'MOBILEGL_CI_CXX': 'clang version 20.1.8', 'cmake': 'cmake version 3.31.10'}
    other_compiler = dict(toolchain, MOBILEGL_CI_CXX='clang version 20.1.2')
    cases = [
        ('none', {}, toolchain, True),
        ('a MOBILEGL_* cache line', {'mobilegl_flag': 'OFF'}, toolchain, False),
        ("one TU's -D", {'define': '-DMG=2'}, toolchain, False),
        ('the build type', {'build_type': 'Debug'}, toolchain, False),
        ("the compiler's --version text", {}, other_compiler, False),
        ('a link flag', {'link_flag': '-Wl,--no-as-needed'}, toolchain, False),
        ('a test ENVIRONMENT', {'test_env': 'VK_ICD_FILENAMES=/other.json'}, toolchain, False),
        ('only the benchmark TUs, link lines and subdirs line', {'benchmark': True}, toolchain, True),
        ('only the build directory name', {'name': 'build-other'}, toolchain, True),
        ('an empty directory', {'empty': True}, toolchain, False),
    ]
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        base = fingerprint(canned_tree(Path(tmp) / 'base'), toolchain)
        assert not unproven(base), unproven(base)
        for label, change, chain, expected in cases:
            other = fingerprint(canned_tree(Path(tmp) / label.replace(' ', '_').replace("'", ''), **change),
                                chain)
            identical, reasons = verdict(base, other)
            ok = identical == expected
            failures += not ok
            print(f"{'ok' if ok else 'NOT OK':7} {label}: identical={identical}"
                  + ('' if ok or identical else f' ({reasons[:2]})'))
        # The guard itself: a probe that cannot be configured is never 'identical'.
        output = Path(tmp) / 'github_output'
        old = os.environ.get('GITHUB_OUTPUT')
        os.environ['GITHUB_OUTPUT'] = str(output)
        try:
            args = argparse.Namespace(built=str(Path(tmp) / 'base' / 'build'), probe='no-such-variant',
                                      probe_dir=str(Path(tmp) / 'probe'), github_output='identical', report=None)
            import contextlib
            import io
            with contextlib.redirect_stdout(io.StringIO()):
                guard(args)
        finally:
            if old is None:
                os.environ.pop('GITHUB_OUTPUT', None)
            else:
                os.environ['GITHUB_OUTPUT'] = old
        ok = output.read_text().strip() == 'identical=false'
        failures += not ok
        print(f"{'ok' if ok else 'NOT OK':7} a probe that fails to configure: {output.read_text().strip()}")
    print(f'build_fingerprint self-test: {"FAILED" if failures else "ok"}')
    return 1 if failures else 0


def main(argv=None):
    if argv is None:
        argv = sys.argv[1:]
    if argv == ['--self-test']:
        return self_test()
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    p = sub.add_parser('fingerprint')
    p.add_argument('--build-dir', required=True)
    p.add_argument('--out')
    p = sub.add_parser('compare')
    p.add_argument('first')
    p.add_argument('second')
    p = sub.add_parser('guard')
    p.add_argument('--built', required=True)
    p.add_argument('--probe', required=True, choices=('primary', 'split'))
    p.add_argument('--probe-dir', required=True)
    p.add_argument('--github-output', default='identical')
    p.add_argument('--report')
    args = parser.parse_args(argv)
    if args.command == 'fingerprint':
        fp = fingerprint(args.build_dir)
        text = json.dumps(fp, indent=1, sort_keys=True) + '\n'
        if args.out:
            Path(args.out).write_text(text)
        print(f"build_fingerprint: {args.build_dir}: {fp['counts']}; unproven: {unproven(fp) or 'no'}")
        return 0
    if args.command == 'compare':
        a = json.loads(Path(args.first).read_text())
        b = json.loads(Path(args.second).read_text())
        identical, reasons = verdict(a, b)
        for reason in reasons:
            print(reason)
        print(f"identical={'true' if identical else 'false'}")
        return 0 if identical else 1
    return guard(args)


if __name__ == '__main__':
    sys.exit(main())
