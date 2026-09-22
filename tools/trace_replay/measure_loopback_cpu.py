#!/usr/bin/env python3
"""Compare current unix+shm and loopback TCP+stream using per-frame role CPU clocks.

Start a stats-enabled supervisor separately at --endpoint. This runner neither
contacts a device nor changes production code. It refuses partial or mismatched
client/server frame windows instead of treating missing CPU observations as zero.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import statistics
import subprocess

from measure_transport_cpu import CASES, digest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('p65_metrics', ROOT / 'scripts/ci/p65_link_metrics.py')
METRICS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(METRICS)


def frame_rows(path, marker, server=False):
    rows = {}
    for line in path.read_text(encoding='utf-8', errors='replace').splitlines():
        if marker not in line:
            continue
        row = dict(METRICS.FIELDS.findall(line.split(marker, 1)[1]))
        if server:
            if row.get('valid') != '1':
                continue
        elif row.get('kind') != 'frame':
            continue
        number = int(row['frame'])
        if number in rows:
            raise ValueError(f'{path}: duplicate frame {number}')
        rows[number] = row
    return rows


def run(args, case, transport, repeat):
    _, trace_name, width, height, tail = CASES[case]
    trace = args.inputs / case / trace_name
    if not trace.is_file():
        raise ValueError(f'{trace}: extract the unchanged fixture before measuring')
    out = args.out / case / transport / f'run-{repeat}'
    if out.exists():
        raise ValueError(f'{out}: choose a new output directory; evidence is never overwritten')
    out.mkdir(parents=True)
    env = dict(os.environ, MOBILEGL_TRANSPORT='spawn', MOBILEGL_BACKEND_TYPE='DirectGLES',
               MOBILEGL_IPC_SERVER_PATH=str(args.server), MOBILEGL_PIPE_STATS='1', MOBILEGL_PIPE_STATS_PERIOD='1',
               MOBILEGL_IPC_PRESENT_CREDIT='1', MOBILEGL_IPC_RUN_AHEAD='1', MOBILEGL_IPC_VERB_BARRIER='1',
               MOBILEGL_IPC_BATCH_WAITS='1', MOBILEGL_IPC_SPIN_US='50', MOBILEGL_IPC_STRICT_ERRORS='0',
               MOBILEGL_IPC_ROLE_SPLIT_STATE='0', MOBILEGL_IPC_AUDIT='0',
               LIBGL_ALWAYS_SOFTWARE='1', EGL_PLATFORM='surfaceless',
               MESA_GL_VERSION_OVERRIDE='3.3', MESA_GLSL_VERSION_OVERRIDE='330')
    for key in ('MOBILEGL_IPC_CONTROL', 'MOBILEGL_IPC_DATA', 'MOBILEGL_IPC_TOKEN', 'MOBILEGL_IPC_DIAL',
                'MOBILEGL_IPC_REQUIRE_SAME_BUILD', 'MOBILEGL_TEST_SERVER_COUNTERS'):
        env.pop(key, None)
    if transport == 'tcp':
        env.update(MOBILEGL_IPC_CONTROL=args.endpoint, MOBILEGL_IPC_DATA='stream', MOBILEGL_IPC_TOKEN='',
                   MOBILEGL_IPC_REQUIRE_SAME_BUILD='1', MOBILEGL_IPC_LOG_FORWARD='1')
    if Path('/usr/share/glvnd/egl_vendor.d/50_mesa.json').exists():
        env['__EGL_VENDOR_LIBRARY_FILENAMES'] = '/usr/share/glvnd/egl_vendor.d/50_mesa.json'
    command = [str(args.runner), '--trace', str(trace), '--mobilegl-library', str(args.library),
               '--backend', 'DirectGLES', '--width', str(width), '--height', str(height), '--output', str(out),
               '--benchmark', '--benchmark-finish=0', '--benchmark-tail-frames=' + str(tail)]
    with (out / 'runner.log').open('w') as log:
        result = subprocess.run(command, env=env, stdout=log, stderr=log, timeout=900)
    if result.returncode:
        raise RuntimeError(f'{out}: benchmark exited {result.returncode}')
    bench = json.loads((out / 'benchmark.json').read_text())
    client_path, server_path = out / 'mobilegl.client.log', out / 'mobilegl.server.log'
    client = frame_rows(client_path, 'P65LinkMetrics ')
    server = frame_rows(server_path, 'P65ServerMetrics ', server=True)
    ids = sorted(client)
    if len(ids) != bench['totalFrames'] or len(ids) <= tail:
        raise ValueError(f'{out}: incomplete frame metrics or tail includes setup')
    selected = ids[-tail:]
    if selected != list(range(selected[0], selected[-1] + 1)) or any(frame not in server for frame in selected):
        raise ValueError(f'{out}: client/server tail frame IDs disagree')
    first, last = selected[0], selected[-1]
    # The existing reducer now sees precisely the proven common tail range.
    values = METRICS.summarize(client_path, first - 1)
    values.update(METRICS.summarize_server(server_path, first - 1))
    if values['frames'] != tail or values['server_frames'] != tail:
        raise ValueError(f'{out}: unexpected frames outside the selected tail')
    for frame in selected:
        if int(client[frame]['client_thread_cpu_ns']) <= 0 or int(server[frame]['apply_thread_cpu_ns']) <= 0:
            raise ValueError(f'{out}: nonpositive per-thread CPU in frame {frame}')
    proof = client_path.read_text(errors='replace')
    expected = 'control=tcp data=stream' if transport == 'tcp' else 'spawn ARMED - the server role runs in pid '
    if expected not in proof:
        raise ValueError(f'{out}: no proof of the requested transport')
    if ('run-ahead ARMED' not in proof or 'running lockstep' in proof or 'run-ahead DISARMED' in proof):
        raise ValueError(f'{out}: requested run-ahead was not continuously armed; not a pure transport CPU comparison')
    row = {'case': case, 'transport': transport, 'repeat': repeat,
           'first_frame': first, 'last_frame': last, 'tail_frames': tail,
           'run_ahead_armed': True,
           'benchmark_client_cpu_ms': statistics.mean(bench['frameCpuTimesMs'][-tail:]),
           'runner_sha256': args.runner_hash, 'library_sha256': args.library_hash,
           'server_sha256': args.server_hash, 'trace_sha256': digest(trace),
           'command': command, 'output': str(out), **values}
    (out / 'role-cpu.json').write_text(json.dumps(row, indent=2), encoding='utf-8')
    return row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runner', type=Path, required=True)
    parser.add_argument('--library', type=Path, required=True)
    parser.add_argument('--server', type=Path, required=True)
    parser.add_argument('--inputs', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--endpoint', default='tcp://127.0.0.1:40616')
    parser.add_argument('--transport', action='append', choices=('spawn', 'tcp'),
                        help='Measure only the selected arm; default measures both')
    parser.add_argument('--case', action='append', choices=CASES,
                        help='Measure only selected cases; default measures OpenRA and rd12')
    args = parser.parse_args()
    if not args.endpoint.startswith('tcp://127.0.0.1:'):
        parser.error('this measurement is specifically loopback, not a device benchmark')
    args.runner_hash, args.library_hash, args.server_hash = map(digest, (args.runner, args.library, args.server))
    rows = []
    for case, repeats in (('OpenRA', 3), ('rd12', 1)):
        if args.case and case not in args.case:
            continue
        for repeat in range(repeats):
            for transport in args.transport or ('spawn', 'tcp'):
                row = run(args, case, transport, repeat)
                rows.append(row)
                (args.out / 'summary.json').write_text(json.dumps(rows, indent=2), encoding='utf-8')
                print(json.dumps({key: row[key] for key in ('case', 'transport', 'repeat', 'first_frame', 'last_frame',
                                                           'client_thread_cpu_ms_per_frame', 'apply_thread_cpu_ms_per_frame')}), flush=True)


if __name__ == '__main__':
    main()
