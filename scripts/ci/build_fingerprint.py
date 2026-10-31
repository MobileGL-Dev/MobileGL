#!/usr/bin/env python3
# MobileGL - scripts/ci/build_fingerprint.py
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

"""Decide whether the two Linux library configurations are identical apart from the benchmarks.

build-linux and build-linux-split configure the same sources twice. On the disaggregation
branches the only intended difference is MOBILEGL_BUILD_BENCHMARK, which adds MG_Benchmark and the
google benchmark dependency and touches nothing else. When the two configurations are identical,
the split consumers can take build-linux's artifacts and build-linux-split need not compile again.

The fingerprint is read from what configure writes, so it can be taken before the build:
  cache      MOBILEGL_* entries (except MOBILEGL_BUILD_BENCHMARK), the build type, compilers,
             launchers, flags and the link tools from CMakeCache.txt;
  compile    every compile_commands.json entry: file, output and arguments;
  link       every link statement of build.ninja: outputs, rule, inputs and variables;
  tests      the text of every CTestTestfile.cmake and gtest *_include.cmake.
The build directory's path becomes @BUILD@ and a shared FetchContent directory @DEPS@; everything
under MobileGL/MG_Benchmark or the benchmark dependency is left out. Both fingerprints come from
one runner, so its toolchain and image are recorded beside the verdict and never compared.

  build_fingerprint.py fingerprint --build-dir D [--deps-dir F] [--out F]
  build_fingerprint.py compare A.json B.json
  build_fingerprint.py guard --primary-dir P --split-dir S --deps-dir F [--github-output KEY] [--report R]
  build_fingerprint.py --self-test

guard configures both lines with scripts/ci/configure_build.sh (configure only) into P and S, with
F as their one FetchContent directory, so only the first configure fetches anything; a configure
that fails, or runs past 10 minutes and is killed, is retried once. It writes
KEY=identical|different|unknown, image= and compiler= to $GITHUB_OUTPUT and always exits 0.
'different' is a proven difference. A configure that failed twice, any error, or a tree with fewer
than 200 compile entries, 3 link statements (libMobileGL.so among them) or 2 CTest files is
'unknown': only 'identical' lets the workflow reuse a build. Any other verdict is annotated with
its first reasons, a warning for 'different' and a notice for 'unknown'.
"""
import argparse
import contextlib
import io
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
CONFIGURE = ROOT / 'scripts' / 'ci' / 'configure_build.sh'
# Each configure attempt is killed after CONFIGURE_TIMEOUT seconds. GUARD_STEP_MINUTES must equal the
# guard step's timeout-minutes in test.yml, and leave room for four killed attempts.
CONFIGURE_TIMEOUT = 600
GUARD_STEP_MINUTES = 45

MIN_COMPILE = 200
MIN_LINK = 3
MIN_TESTS = 2
VERDICTS = ('identical', 'different', 'unknown')

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
BENCHMARK_PATH = re.compile(r'(^|/)(MobileGL/MG_Benchmark|(_deps|@DEPS@)/benchmark-[a-z]+)(/|$)')
BENCHMARK_SUBDIRS = re.compile(
    r'^\s*subdirs\("[^"]*(MobileGL/MG_Benchmark|(_deps|@DEPS@)/benchmark-build)"\)\s*$')
PACKAGES = ('clang-20', 'lld-20', 'libc++-20-dev', 'libc++abi-20-dev', 'libvulkan-dev',
            'libegl1-mesa-dev', 'libgles2-mesa-dev')


def normalizer(build_dir, deps_dir=None):
    roots = [(str(Path(build_dir)), '@BUILD@'), (str(Path(build_dir).resolve()), '@BUILD@')]
    if deps_dir:
        roots += [(str(Path(deps_dir)), '@DEPS@'), (str(Path(deps_dir).resolve()), '@DEPS@')]
    roots = sorted(set(roots), key=lambda pair: len(pair[0]), reverse=True)

    def norm(text):
        for root, token in roots:
            text = text.replace(root, token)
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
        lines = [norm(line) for line in path.read_text(errors='replace').splitlines()]
        kept = [line for line in lines if not BENCHMARK_SUBDIRS.match(line) and not line.startswith('# ')]
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


def read_image():
    return {key: os.environ.get(key, '') for key in ('ImageOS', 'ImageVersion', 'RUNNER_NAME')}


def fingerprint(build_dir, deps_dir=None):
    build = Path(build_dir)
    norm = normalizer(build, deps_dir)
    result = {
        'cache': read_cache(build, norm),
        'compile': read_compile(build, norm),
        'link': read_links(build, norm),
        'tests': read_tests(build, norm),
    }
    result['counts'] = {'compile': len(result['compile']), 'link': len(result['link']),
                        'tests': len(result['tests'])}
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
    return {field: rows[:limit] + ([f'... {len(rows) - limit} more'] if len(rows) > limit else [])
            for field, rows in diffs.items()}


def verdict(a, b):
    """(verdict, reasons). Too little evidence on either side is 'unknown', never 'identical'."""
    thin = [f'first: {p}' for p in unproven(a)] + [f'second: {p}' for p in unproven(b)]
    if thin:
        return 'unknown', thin
    reasons = [f'{field}: {row}' for field, rows in compare(a, b).items() for row in rows]
    return ('different' if reasons else 'identical'), reasons


def run_attempt(argv, label, timeout=CONFIGURE_TIMEOUT):
    """One configure attempt; True on success. One that runs past the timeout (a stalled fetch) is
    killed with its whole process group, cmake and its git clones included, and counts as failed."""
    proc = subprocess.Popen(argv, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                            start_new_session=True)
    try:
        output, _ = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGKILL)
        try:
            output, _ = proc.communicate(timeout=60)
        except subprocess.TimeoutExpired:
            output = ''
        print(output[-6000:])
        print(f'build_fingerprint: {label} ran past {timeout} s and was killed')
        return False
    if proc.returncode != 0:
        print(output[-6000:])
        print(f'build_fingerprint: {label} failed with exit {proc.returncode}')
    return proc.returncode == 0


def run_configure(variant, build_dir, deps_dir, timeout=None):
    """configure_build.sh, configure only, with the shared FetchContent directory; True on success."""
    return run_attempt(['bash', str(CONFIGURE), variant, str(build_dir), f'-DFETCHCONTENT_BASE_DIR={deps_dir}'],
                       f'configure_build.sh {variant}', CONFIGURE_TIMEOUT if timeout is None else timeout)


def configure_twice(configure, variant, build_dir, deps_dir):
    """One retry: a transient fetch failure must not decide anything. Returns the attempts used."""
    for attempt in (1, 2):
        if build_dir.exists():
            shutil.rmtree(build_dir)
        if configure(variant, build_dir, deps_dir):
            return attempt
    return 0


def decide(primary_dir, split_dir, deps_dir, configure=run_configure):
    """(verdict, reasons, counts). The primary line runs first: it alone fetches the benchmark."""
    primary_dir, split_dir, deps_dir = Path(primary_dir), Path(split_dir), Path(deps_dir)
    counts = {}
    for variant, build_dir in (('primary', primary_dir), ('split', split_dir)):
        attempts = configure_twice(configure, variant, build_dir, deps_dir)
        if not attempts:
            return 'unknown', [f'the {variant} configure failed twice'], counts
        counts[f'{variant}_attempts'] = attempts
    primary = fingerprint(primary_dir, deps_dir)
    split = fingerprint(split_dir, deps_dir)
    counts.update(primary=primary['counts'], split=split['counts'])
    result, reasons = verdict(primary, split)
    return result, reasons, counts


def one_line(text, limit=200):
    return ' '.join(str(text).split())[:limit]


def annotation(result, reasons):
    """The workflow annotation for a verdict other than 'identical': a 'different' that persists is
    configuration drift that brings the second build back on every run, so it is a warning."""
    if result == 'identical':
        return None
    level = 'warning' if result == 'different' else 'notice'
    text = one_line('; '.join(reasons[:3]) or 'no reason recorded', 600).replace('%', '%25')
    return f'::{level}::build-guard verdict {result}, so build-linux-split builds: {text}'


def write_outputs(key, result, image, compiler):
    lines = [f'{key}={result}', f'image={one_line(image)}', f'compiler={one_line(compiler)}']
    for line in lines:
        print(line)
    path = os.environ.get('GITHUB_OUTPUT')
    if path:
        with open(path, 'a') as out:
            out.write(''.join(line + '\n' for line in lines))


def guard(args, configure=run_configure, toolchain_reader=read_toolchain):
    result, reasons, counts = 'unknown', [], {}
    image = read_image()
    toolchain = {}
    try:
        toolchain = toolchain_reader()
        result, reasons, counts = decide(args.primary_dir, args.split_dir, args.deps_dir, configure)
    except Exception as error:  # a guard that cannot decide says so, and nothing is reused
        result, reasons = 'unknown', [f'the guard could not compare the configurations: {error!r}']
    compiler = (toolchain.get('MOBILEGL_CI_CXX') or 'unset').splitlines()[0] if toolchain else 'unread'
    image_text = f"{image['ImageOS']} {image['ImageVersion']}".strip() or 'unknown image'
    print(f'build_fingerprint: {counts}')
    print(f'build_fingerprint: this runner: {image_text}, {compiler}')
    for reason in reasons:
        print(f'build_fingerprint: {reason}')
    note = annotation(result, reasons)
    if note:
        print(note)
    if args.report:
        Path(args.report).write_text(json.dumps({'verdict': result, 'reasons': reasons, 'counts': counts,
                                                 'toolchain': toolchain, 'image': image}, indent=1) + '\n')
    write_outputs(args.github_output, result, image_text, compiler)
    return 0


# ------------------------------------------------------------------------------------- self-test

def canned_tree(root, *, name='build', mobilegl_flag='ON', build_type='Release', define='-DMG=1',
                link_flag='-Wl,--as-needed', test_env='VK_ICD_FILENAMES=/icd.json', benchmark=False,
                empty=False, compile_count=MIN_COMPILE + 5, deps=None, library='libMobileGL.so'):
    build = root / name
    build.mkdir(parents=True)
    if empty:
        return build
    abs_build = str(build)
    deps = str(deps) if deps else abs_build + '/_deps'
    (build / 'CMakeCache.txt').write_text(
        f'CMAKE_BUILD_TYPE:STRING={build_type}\n'
        'CMAKE_CXX_COMPILER:FILEPATH=/usr/bin/clang++-20\n'
        'CMAKE_CXX_COMPILER_LAUNCHER:STRING=ccache\n'
        'CMAKE_CXX_FLAGS:STRING=\n'
        f'MOBILEGL_BUILD_DISAGGREGATED:BOOL={mobilegl_flag}\n'
        f"MOBILEGL_BUILD_BENCHMARK:BOOL={'ON' if benchmark else 'OFF'}\n"
        f'FETCHCONTENT_BASE_DIR:PATH={deps}\n'
        f'CMAKE_CACHEFILE_DIR:INTERNAL={abs_build}\n')
    entries = [{'directory': abs_build, 'file': f'/src/MobileGL/f{i}.cpp',
                'output': f'CMakeFiles/MobileGL.dir/f{i}.cpp.o',
                'command': f'ccache clang++ {define if i == 7 else "-DMG=1"} -I{abs_build}/gen '
                           f'-I{deps}/googletest-src/googletest/include '
                           f'-o CMakeFiles/MobileGL.dir/f{i}.cpp.o -c /src/MobileGL/f{i}.cpp'}
               for i in range(compile_count)]
    entries.append({'directory': deps + '/googletest-build/googletest',
                    'file': deps + '/googletest-src/googletest/src/gtest-all.cc',
                    'output': 'CMakeFiles/gtest.dir/src/gtest-all.cc.o', 'command': 'clang++ -c gtest-all.cc'})
    if benchmark:
        entries.append({'directory': abs_build + '/MobileGL/MG_Benchmark', 'file': '/src/MobileGL/MG_Benchmark/b.cpp',
                        'output': 'CMakeFiles/SanityBench.dir/b.cpp.o', 'command': 'clang++ -c b.cpp'})
        entries.append({'directory': deps + '/benchmark-build/src', 'file': deps + '/benchmark-src/src/benchmark.cc',
                        'output': 'CMakeFiles/benchmark.dir/benchmark.cc.o', 'command': 'clang++ -c benchmark.cc'})
    (build / 'compile_commands.json').write_text(json.dumps(entries))
    links = [
        (library, 'CXX_SHARED_LIBRARY_LINKER__MobileGL_Release', 'CMakeFiles/MobileGL.dir/f1.cpp.o'),
        ('libMobileGL_s.a', 'CXX_STATIC_LIBRARY_LINKER__MobileGL_s_Release', 'CMakeFiles/MobileGL.dir/f2.cpp.o'),
        ('MobileGL/MG_Test/SanityTest', 'CXX_EXECUTABLE_LINKER__SanityTest_Release', 'libMobileGL_s.a'),
    ]
    if benchmark:
        links.append(('MobileGL/MG_Benchmark/SanityBench', 'CXX_EXECUTABLE_LINKER__SanityBench_Release',
                      'libMobileGL_s.a'))
        links.append((deps + '/benchmark-build/src/libbenchmark.a', 'CXX_STATIC_LIBRARY_LINKER__benchmark_Release',
                      deps + '/benchmark-build/src/CMakeFiles/benchmark.dir/benchmark.cc.o'))
    ninja = ['ninja_required_version = 1.5', '']
    for out, rule, inp in links:
        ninja += [f'build {out}: {rule} {inp} | /usr/lib/libc.so || cmake_object_order_depends_target_x',
                  f'  LINK_FLAGS = {link_flag}', '  LINK_LIBRARIES = -lEGL $', '      -lvulkan',
                  f'  POST_BUILD = cd {abs_build}/MobileGL && :', '']
    ninja.append('build CMakeFiles/x.o: CXX_COMPILER__MobileGL_Release /src/x.cpp')
    (build / 'build.ninja').write_text('\n'.join(ninja) + '\n')
    subdirs = (f'subdirs("MobileGL/MG_Test")\nsubdirs("{deps}/googletest-build")\n'
               + (f'subdirs("MobileGL/MG_Benchmark")\nsubdirs("{deps}/benchmark-build")\n' if benchmark else ''))
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
    cases = [
        ('none', {}, 'identical'),
        ('a MOBILEGL_* cache line', {'mobilegl_flag': 'OFF'}, 'different'),
        ("one TU's -D", {'define': '-DMG=2'}, 'different'),
        ('the build type', {'build_type': 'Debug'}, 'different'),
        ('a link flag', {'link_flag': '-Wl,--no-as-needed'}, 'different'),
        ('a test ENVIRONMENT', {'test_env': 'VK_ICD_FILENAMES=/other.json'}, 'different'),
        ('only the benchmark TUs, link lines, subdirs lines and dependency', {'benchmark': True}, 'identical'),
        ('only the build directory name', {'name': 'build-other'}, 'identical'),
        ('only where the shared FetchContent directory is', {'deps_name': 'deps-elsewhere'}, 'identical'),
        ('an empty directory', {'empty': True}, 'unknown'),
    ]
    failures = 0

    def report(ok, label, got):
        nonlocal failures
        failures += not ok
        print(f"{'ok' if ok else 'NOT OK':7} {label}: {got}")

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        base_deps = tmp / 'deps'
        base = fingerprint(canned_tree(tmp / 'base', deps=base_deps), base_deps)
        assert not unproven(base), unproven(base)
        for label, change, expected in cases:
            change = dict(change)
            deps = tmp / change.pop('deps_name', 'deps')
            root = tmp / re.sub(r'\W+', '_', label)
            other = fingerprint(canned_tree(root, deps=deps, **change), deps)
            got, reasons = verdict(base, other)
            report(got == expected, label, got + ('' if got == expected or not reasons else f' ({reasons[:2]})'))
        # Two trees that agree on everything but are too thin to prove anything are never identical.
        thin_a = fingerprint(canned_tree(tmp / 'thin-a', compile_count=10, deps=base_deps), base_deps)
        thin_b = fingerprint(canned_tree(tmp / 'thin-b', compile_count=10, deps=base_deps), base_deps)
        report(verdict(thin_a, thin_b)[0] == 'unknown', 'both sides thin', verdict(thin_a, thin_b)[0])
        empty = verdict(fingerprint(canned_tree(tmp / 'empty-a', empty=True)),
                        fingerprint(canned_tree(tmp / 'empty-b', empty=True)))[0]
        report(empty == 'unknown', 'both sides empty', empty)
        # Full-sized trees that agree but link no libMobileGL.so prove nothing about the library.
        no_lib = [fingerprint(canned_tree(tmp / f'no-lib-{side}', deps=base_deps, library='libMobileGLServer.so'),
                              base_deps) for side in 'ab']
        report(verdict(*no_lib)[0] == 'unknown', 'both sides without a libMobileGL.so link', verdict(*no_lib)[0])

        # A configure attempt that hangs is killed with its children, so it cannot hold the step.
        started = time.monotonic()
        with contextlib.redirect_stdout(io.StringIO()):
            hung = run_attempt(['bash', '-c', 'sleep 60 & wait'], 'a hung configure', timeout=1)
        elapsed = time.monotonic() - started
        report(not hung and elapsed < 30, 'a hung configure is killed and fails', f'{hung} after {elapsed:.1f} s')
        # The production path carries the timeout too, and four killed attempts fit the step.
        stub = tmp / 'hanging-configure.sh'
        stub.write_text('sleep 60 & wait\n')
        real_configure = CONFIGURE
        globals()['CONFIGURE'] = stub
        try:
            started = time.monotonic()
            with contextlib.redirect_stdout(io.StringIO()):
                hung = run_configure('primary', tmp / 'hung-build', tmp / 'hung-deps', timeout=1)
            elapsed = time.monotonic() - started
        finally:
            globals()['CONFIGURE'] = real_configure
        report(not hung and elapsed < 30, 'run_configure passes its timeout on', f'{hung} after {elapsed:.1f} s')
        report(isinstance(CONFIGURE_TIMEOUT, int) and CONFIGURE_TIMEOUT > 0
               and 4 * (CONFIGURE_TIMEOUT + 60) <= GUARD_STEP_MINUTES * 60,
               'four killed configure attempts fit the guard step', CONFIGURE_TIMEOUT)
        escaped = annotation('different', ['100% of the flags'])
        report(escaped is not None and escaped.endswith('100%25 of the flags'), "an annotation escapes '%'", escaped)

        # The guard itself, with a stand-in for configure_build.sh.
        def run_guard(configure, label):
            output = tmp / f'github_output-{label}'
            old = os.environ.get('GITHUB_OUTPUT')
            os.environ['GITHUB_OUTPUT'] = str(output)
            try:
                args = argparse.Namespace(primary_dir=str(tmp / f'{label}-primary'),
                                          split_dir=str(tmp / f'{label}-split'),
                                          deps_dir=str(tmp / f'{label}-deps'), github_output='verdict', report=None)
                printed = io.StringIO()
                with contextlib.redirect_stdout(printed):
                    guard(args, configure, lambda: {'MOBILEGL_CI_CXX': 'clang version 20.1.8'})
            finally:
                if old is None:
                    os.environ.pop('GITHUB_OUTPUT', None)
                else:
                    os.environ['GITHUB_OUTPUT'] = old
            outputs = dict(line.split('=', 1) for line in output.read_text().splitlines())
            outputs['annotations'] = [line for line in printed.getvalue().splitlines() if line.startswith('::')]
            return outputs

        calls = []

        def flaky(variant, build_dir, deps_dir):
            calls.append(variant)
            if variant == 'primary' and calls.count('primary') == 1:
                return False
            canned_tree(build_dir.parent, name=build_dir.name, benchmark=variant == 'primary', deps=deps_dir)
            return True
        out = run_guard(flaky, 'flaky')
        report(out.get('verdict') == 'identical' and calls == ['primary', 'primary', 'split'],
               'a configure that fails once is retried', f"{out.get('verdict')} after {calls}")
        report(set(out) == {'verdict', 'image', 'compiler', 'annotations'},
               'the guard writes verdict, image and compiler', sorted(out))
        report(out['annotations'] == [], "'identical' is not annotated", out['annotations'])

        hangs = []

        def hangs_once(variant, build_dir, deps_dir):
            hangs.append(variant)
            if len(hangs) == 1:
                return run_attempt(['bash', '-c', 'sleep 60 & wait'], 'a hung configure', timeout=1)
            canned_tree(build_dir.parent, name=build_dir.name, benchmark=variant == 'primary', deps=deps_dir)
            return True
        out = run_guard(hangs_once, 'hangs')
        report(out.get('verdict') == 'identical' and hangs == ['primary', 'primary', 'split'],
               'a configure killed on its timeout is retried', f"{out.get('verdict')} after {hangs}")

        def broken(variant, build_dir, deps_dir):
            return variant != 'split'
        out = run_guard(broken, 'broken')
        report(out.get('verdict') == 'unknown', 'a configure that fails twice', out.get('verdict'))
        report(len(out['annotations']) == 1 and out['annotations'][0].startswith('::notice::')
               and 'failed twice' in out['annotations'][0], "'unknown' is a notice with its reason",
               out['annotations'])

        def debug_split(variant, build_dir, deps_dir):
            canned_tree(build_dir.parent, name=build_dir.name, deps=deps_dir,
                        build_type='Debug' if variant == 'split' else 'Release')
            return True
        out = run_guard(debug_split, 'differ')
        report(out.get('verdict') == 'different', 'a proven difference', out.get('verdict'))
        report(len(out['annotations']) == 1 and out['annotations'][0].startswith('::warning::')
               and 'CMAKE_BUILD_TYPE' in out['annotations'][0], "'different' is a warning with its reason",
               out['annotations'])

        def raising(variant, build_dir, deps_dir):
            raise OSError('no bash')
        out = run_guard(raising, 'raising')
        report(out.get('verdict') == 'unknown', 'an error inside the guard', out.get('verdict'))
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
    p.add_argument('--deps-dir')
    p.add_argument('--out')
    p = sub.add_parser('compare')
    p.add_argument('first')
    p.add_argument('second')
    p = sub.add_parser('guard')
    p.add_argument('--primary-dir', required=True)
    p.add_argument('--split-dir', required=True)
    p.add_argument('--deps-dir', required=True)
    p.add_argument('--github-output', default='verdict')
    p.add_argument('--report')
    args = parser.parse_args(argv)
    if args.command == 'fingerprint':
        fp = fingerprint(args.build_dir, args.deps_dir)
        text = json.dumps(fp, indent=1, sort_keys=True) + '\n'
        if args.out:
            Path(args.out).write_text(text)
        print(f"build_fingerprint: {args.build_dir}: {fp['counts']}; unproven: {unproven(fp) or 'no'}")
        return 0
    if args.command == 'compare':
        a = json.loads(Path(args.first).read_text())
        b = json.loads(Path(args.second).read_text())
        result, reasons = verdict(a, b)
        for reason in reasons:
            print(reason)
        print(f'verdict={result}')
        return 0 if result == 'identical' else 1
    return guard(args)


if __name__ == '__main__':
    sys.exit(main())
