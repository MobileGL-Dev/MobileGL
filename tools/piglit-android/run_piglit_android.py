#!/usr/bin/env python3
# MobileGL - tools/piglit-android/run_piglit_android.py
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header
"""Run piglit GL tests on a connected Android device against MobileGL.

Pipeline:
  1. Read a test list produced by `piglit print-cmd` ("name ::: command" lines).
  2. Rewrite host paths to on-device paths and collect referenced data files.
  3. Push binaries/libs/data to the device (incremental, marker-based).
  4. Execute tests serially on-device in chunked shell scripts under `timeout`,
     with MobileGL served to waffle via WAFFLE_EGL_LIBRARY=libMobileGL.so.
  5. Parse "PIGLIT: {...}" result lines, classify, and write results + summary.

The device never needs python or an APK; everything runs as the adb shell user
from /data/local/tmp.
"""

import argparse
import json
import os
import re
import shlex
import subprocess
import sys
import tarfile
import tempfile
import time
from pathlib import Path

RESULT_LINE = re.compile(rb'PIGLIT: *({.*})')
MARK_START = re.compile(rb'@@@T (\d+) START')
MARK_EXIT = re.compile(rb'@@@T (\d+) EXIT (\d+)')

# toybox timeout exit codes: 124 = timed out (SIGTERM), 137 = SIGKILL after -k.
TIMEOUT_EXITS = {124, 137, 142}


def adb(args, serial=None, **kw):
    cmd = ['adb'] + (['-s', serial] if serial else []) + args
    return subprocess.run(cmd, **kw)


def adb_check(args, serial=None):
    r = adb(args, serial=serial, capture_output=True)
    if r.returncode != 0:
        raise RuntimeError(f"adb {' '.join(args)} failed: {r.stderr.decode(errors='replace')}")
    return r.stdout


def parse_list(path):
    tests = []
    for line in Path(path).read_text().splitlines():
        line = line.strip()
        if not line or ' ::: ' not in line:
            continue
        name, cmd = line.split(' ::: ', 1)
        tests.append((name.strip(), shlex.split(cmd)))
    return tests


def rewrite_cmd(argv, piglit_root, build_dir, device_dir):
    """Map host paths in a test command to device paths.

    Returns (device_argv, referenced_host_files).
    """
    build_abs = str((piglit_root / build_dir).resolve())
    src_abs = str(piglit_root.resolve())
    out = []
    refs = []
    for i, arg in enumerate(argv):
        a = arg
        if i == 0:
            # program: "build-android/bin/foo" or absolute
            prog = a if os.path.isabs(a) else str((piglit_root / a).resolve())
            refs.append(prog)
            out.append(f'{device_dir}/bin/{os.path.basename(prog)}')
            continue
        p = a if os.path.isabs(a) else None
        if p and p.startswith(build_abs + '/generated_tests/'):
            refs.append(p)
            out.append(p.replace(build_abs + '/generated_tests', device_dir + '/generated_tests', 1))
        elif p and p.startswith(build_abs + '/tests/'):
            # Serialized profiles record data paths under the build dir, but
            # only *generated* data lives there in an out-of-tree build;
            # plain test files stay in the source tests/ dir.
            if not os.path.exists(p):
                p = p.replace(build_abs + '/tests', src_abs + '/tests', 1)
                refs.append(p)
                out.append(p.replace(src_abs + '/tests', device_dir + '/tests', 1))
            else:
                refs.append(p)
                out.append(p.replace(build_abs + '/tests', device_dir + '/build-tests', 1))
        elif p and p.startswith(src_abs + '/tests/'):
            refs.append(p)
            out.append(p.replace(src_abs + '/tests', device_dir + '/tests', 1))
        else:
            out.append(a)
    return out, refs


def backend_env(backend, device_dir, use_angle=False):
    env = {
        'LD_LIBRARY_PATH': f'{device_dir}/lib',
        'WAFFLE_EGL_LIBRARY': 'libMobileGL.so',
        'WAFFLE_GL_LIBRARY': 'libMobileGL.so',
        # Upgrade low compat context requests to what MobileGL implements;
        # without this, piglit's supports_gl_compat_version=10 tests get the
        # raw backend version string and skip themselves.
        'WAFFLE_FORCE_GL_CONTEXT_VERSION': '33core',
        'PIGLIT_PLATFORM': 'surfaceless_egl',
        'PIGLIT_SOURCE_DIR': device_dir,
        'MOBILEGL_BACKEND_TYPE': backend,
        'MOBILEGL_LOG_FILE_PATH': f'{device_dir}/mobilegl.log',
    }
    if backend == 'DirectGLES':
        env['MOBILEGL_USE_ANGLE'] = '1' if use_angle else '0'
    if backend == 'DirectVulkan':
        # The device ICD usually lacks VK_EXT_headless_surface, which the
        # MobileGL pbuffer path needs; use a real ANativeWindow from
        # AImageReader instead (patched waffle surfaceless_egl platform).
        env['WAFFLE_ANDROID_WINDOW'] = 'imagereader'
    return env


def push_tree(host, dev, serial, marker_name, force=False):
    """Push a directory once, tracked by a marker file on the device."""
    marker = f'{dev}.pushed-{marker_name}'
    if not force:
        r = adb(['shell', f'test -e {shlex.quote(marker)} && echo yes'],
                serial=serial, capture_output=True)
        if r.stdout.strip() == b'yes':
            return False
    print(f'  pushing {host} -> {dev}')
    adb_check(['shell', f'rm -rf {shlex.quote(dev)}'], serial=serial)
    adb_check(['push', str(host), dev], serial=serial)
    adb_check(['shell', f'touch {shlex.quote(marker)}'], serial=serial)
    return True


def push_files(files, strip_prefix, dev_prefix, serial):
    """Tar up referenced data files (relative to strip_prefix) and unpack on device."""
    files = sorted(set(files))
    if not files:
        return
    with tempfile.NamedTemporaryFile(suffix='.tar', delete=False) as tf:
        tar_path = tf.name
    with tarfile.open(tar_path, 'w') as tar:
        for f in files:
            rel = os.path.relpath(f, strip_prefix)
            if rel.startswith('..'):
                raise RuntimeError(f'file {f} not under {strip_prefix}')
            tar.add(f, arcname=rel)
    dev_tar = dev_prefix + '/.data.tar'
    adb_check(['shell', f'mkdir -p {shlex.quote(dev_prefix)}'], serial=serial)
    adb_check(['push', tar_path, dev_tar], serial=serial)
    adb_check(['shell', f'cd {shlex.quote(dev_prefix)} && tar xf .data.tar && rm .data.tar'],
              serial=serial)
    os.unlink(tar_path)


def classify(exit_code, piglit_results, timed_out):
    if timed_out:
        return 'timeout'
    result = None
    for r in piglit_results:
        if 'result' in r:
            result = r['result']
    if result is None:
        return 'crash' if exit_code != 0 else 'notrun'
    if exit_code not in (0, 1):
        return 'crash'
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--piglit-root', required=True, type=Path,
                    help='piglit source checkout (with the Android build dir inside)')
    ap.add_argument('--build-dir', default='build-android',
                    help='Android build dir name inside piglit root')
    ap.add_argument('--list', required=True,
                    help='test list file from `piglit print-cmd` (name ::: cmd)')
    ap.add_argument('--backend', required=True, choices=['DirectGLES', 'DirectVulkan'])
    ap.add_argument('--use-angle', action='store_true',
                    help='DirectGLES only: let MobileGL load ANGLE instead of the system driver')
    ap.add_argument('--mobilegl-lib', required=True, type=Path,
                    help='stripped libMobileGL.so for the device')
    ap.add_argument('--waffle-lib', required=True, type=Path,
                    help='cross-built libwaffle-1.so')
    ap.add_argument('--device-dir', default='/data/local/tmp/piglit-mgl')
    ap.add_argument('--out', required=True, type=Path, help='host results directory')
    ap.add_argument('--serial', default=None, help='adb device serial')
    ap.add_argument('--timeout', type=int, default=60, help='per-test timeout (seconds)')
    ap.add_argument('--chunk', type=int, default=200, help='tests per device-side script')
    ap.add_argument('--repush', action='store_true', help='force re-push of bin/lib/tests trees')
    args = ap.parse_args()

    piglit_root = args.piglit_root.resolve()
    build = piglit_root / args.build_dir
    dev = args.device_dir.rstrip('/')
    args.out.mkdir(parents=True, exist_ok=True)

    tests = parse_list(args.list)
    if not tests:
        print('no tests in list', file=sys.stderr)
        return 2

    print(f'[1/4] {len(tests)} tests; rewriting paths')
    dev_cmds = []
    data_refs = []
    for name, argv in tests:
        dcmd, refs = rewrite_cmd(argv, piglit_root, args.build_dir, dev)
        dev_cmds.append((name, dcmd))
        data_refs.extend(r for r in refs if not r.split('/')[-2:][0] == 'bin')

    print('[2/4] pushing artifacts')
    adb_check(['shell', f'mkdir -p {dev} {dev}/lib {dev}/chunks {dev}/logs'], serial=args.serial)
    push_tree(build / 'bin', f'{dev}/bin', args.serial, 'bin', args.repush)
    adb_check(['shell', f'chmod -R 755 {dev}/bin'], serial=args.serial)
    # libs: piglit utils + waffle + MobileGL in one LD_LIBRARY_PATH dir
    for lib in sorted((build / 'lib').glob('*.so')):
        adb_check(['push', str(lib), f'{dev}/lib/'], serial=args.serial)
    adb_check(['push', str(args.waffle_lib), f'{dev}/lib/libwaffle-1.so'], serial=args.serial)
    adb_check(['push', str(args.mobilegl_lib), f'{dev}/lib/libMobileGL.so'], serial=args.serial)
    # data files referenced by this run (tests/, build-tests/, generated_tests/)
    src_abs = str(piglit_root)
    build_abs = str(build)
    groups = {
        (src_abs + '/tests', f'{dev}/tests'): [],
        (build_abs + '/tests', f'{dev}/build-tests'): [],
        (build_abs + '/generated_tests', f'{dev}/generated_tests'): [],
    }
    for r in set(data_refs):
        for (host_prefix, dev_prefix), bucket in groups.items():
            if r.startswith(host_prefix + '/'):
                bucket.append(r)
                break
    for (host_prefix, dev_prefix), bucket in groups.items():
        push_files(bucket, host_prefix, dev_prefix, args.serial)

    env = backend_env(args.backend, dev, args.use_angle)
    env_lines = '\n'.join(f'export {k}={shlex.quote(v)}' for k, v in env.items())

    print(f'[3/4] running {len(tests)} tests on {args.backend} '
          f'(timeout {args.timeout}s/test, chunks of {args.chunk})')
    raw_log = (args.out / 'raw.log').open('wb')
    t0 = time.time()
    outcomes = {}
    chunks = [dev_cmds[i:i + args.chunk] for i in range(0, len(dev_cmds), args.chunk)]
    idx_base = 0
    for ci, chunk in enumerate(chunks):
        lines = ['#!/system/bin/sh', env_lines, f'cd {dev}']
        for j, (name, dcmd) in enumerate(chunk):
            idx = idx_base + j
            quoted = ' '.join(shlex.quote(a) for a in dcmd)
            lines.append(f'echo "@@@T {idx} START"')
            lines.append(f'timeout -k 5 {args.timeout} {quoted} </dev/null 2>&1')
            lines.append(f'echo "@@@T {idx} EXIT $?"')
        script = '\n'.join(lines) + '\n'
        with tempfile.NamedTemporaryFile('w', suffix='.sh', delete=False) as tf:
            tf.write(script)
            host_script = tf.name
        dev_script = f'{dev}/chunks/chunk{ci}.sh'
        adb_check(['push', host_script, dev_script], serial=args.serial)
        os.unlink(host_script)
        r = adb(['shell', f'sh {dev_script}'], serial=args.serial, capture_output=True)
        raw_log.write(r.stdout)
        raw_log.flush()

        # parse this chunk
        cur = None
        cur_results = []
        cur_out = []
        for line in r.stdout.splitlines():
            m = MARK_START.search(line)
            if m:
                cur = int(m.group(1))
                cur_results = []
                cur_out = []
                continue
            m = MARK_EXIT.search(line)
            if m and cur is not None and int(m.group(1)) == cur:
                code = int(m.group(2))
                name = dev_cmds[cur][0]
                status = classify(code, cur_results, code in TIMEOUT_EXITS)
                outcomes[name] = {
                    'status': status,
                    'exit': code,
                    'subtests': {k: v for r_ in cur_results if 'subtest' in r_
                                 for k, v in r_['subtest'].items()},
                    'tail': b'\n'.join(cur_out[-8:]).decode(errors='replace')
                            if status in ('crash', 'timeout', 'fail', 'notrun') else '',
                }
                cur = None
                continue
            if cur is not None:
                cur_out.append(line)
                m = RESULT_LINE.search(line)
                if m:
                    try:
                        cur_results.append(json.loads(m.group(1)))
                    except json.JSONDecodeError:
                        pass
        # tests whose markers never appeared (adb drop, device reboot)
        for j, (name, _) in enumerate(chunk):
            outcomes.setdefault(dev_cmds[idx_base + j][0], {
                'status': 'missing', 'exit': -1, 'subtests': {}, 'tail': ''})
        idx_base += len(chunk)
        done = idx_base
        print(f'  chunk {ci + 1}/{len(chunks)} done ({done}/{len(dev_cmds)}, '
              f'{time.time() - t0:.0f}s elapsed)')
    raw_log.close()

    print('[4/4] writing results')
    counts = {}
    for name, o in outcomes.items():
        counts[o['status']] = counts.get(o['status'], 0) + 1
    result_doc = {
        'backend': args.backend,
        'use_angle': args.use_angle,
        'device_dir': dev,
        'timeout': args.timeout,
        'elapsed_sec': round(time.time() - t0, 1),
        'totals': counts,
        'tests': outcomes,
    }
    (args.out / 'results.json').write_text(json.dumps(result_doc, indent=1, sort_keys=True))
    lines = [f"backend: {args.backend}  tests: {len(outcomes)}  elapsed: {result_doc['elapsed_sec']}s"]
    lines.append('totals: ' + ', '.join(f'{k}={v}' for k, v in sorted(counts.items())))
    for status in ('crash', 'timeout', 'fail', 'missing', 'notrun', 'warn'):
        bad = sorted(n for n, o in outcomes.items() if o['status'] == status)
        if bad:
            lines.append(f'\n== {status} ({len(bad)}):')
            lines.extend(f'  {n}' for n in bad)
    (args.out / 'summary.txt').write_text('\n'.join(lines) + '\n')
    print('\n'.join(lines[:2]))
    print(f"results: {args.out / 'results.json'}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
