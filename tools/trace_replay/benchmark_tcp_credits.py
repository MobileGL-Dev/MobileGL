#!/usr/bin/env python3
"""Record complete TCP benchmark runs for credit 1/2/3 with exact common tail frames."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import time
import uuid

from measure_transport_cpu import CASES, digest
from measure_loopback_cpu import frame_rows, METRICS
from run_tcp_matrix import supervise, atomic_json, fingerprint, actual_arm_from_text


def validate_measurement(work, endpoint, credit, tail):
    bench = json.loads((work / 'benchmark.json').read_text())
    client_path, server_path = work / 'mobilegl.client.log', work / 'mobilegl.server.log'
    text = client_path.read_text(errors='replace')
    actual_arm = actual_arm_from_text(text)
    if (actual_arm != {'armed': True, 'lockstep': False, 'disarmed': False}
            or not re.search(r'paced by a present credit of ' + str(credit) + r'\b', text)
            or 'control=tcp data=stream server=' + endpoint.removeprefix('tcp://') not in text):
        raise ValueError('runtime does not prove TCP and the requested armed credit')
    client = frame_rows(client_path, 'P65LinkMetrics ')
    server = frame_rows(server_path, 'P65ServerMetrics ', server=True)
    ids = sorted(client)
    if len(ids) != bench['totalFrames'] or len(ids) <= tail:
        raise ValueError('benchmark and frame metrics differ, or tail contains setup')
    selected = ids[-tail:]
    if selected != list(range(selected[0], selected[-1] + 1)) or any(n not in server for n in selected):
        raise ValueError('no matching complete client/server frame tail')
    for number in selected:
        if int(client[number]['client_thread_cpu_ns']) <= 0 or int(server[number]['apply_thread_cpu_ns']) <= 0:
            raise ValueError('nonpositive CPU measurement in selected frame ' + str(number))
    summary = METRICS.summarize(client_path, selected[0] - 1)
    summary.update(METRICS.summarize_server(server_path, selected[0] - 1))
    if summary['frames'] != tail or summary['server_frames'] != tail:
        raise ValueError('unexpected frames beyond the chosen tail')
    return {'run_ahead_armed': True, 'actual_arm': actual_arm,
            'first_frame': selected[0], 'last_frame': selected[-1], 'benchmark': bench,
            'benchmark_sha256': digest(work / 'benchmark.json'),
            'client_log_sha256': digest(client_path), 'server_log_sha256': digest(server_path), **summary}


def measure(args, case, credit, work):
    _, trace_name, width, height, tail = CASES[case]
    trace = args.inputs / case / trace_name
    work.mkdir(parents=True)
    env = dict(os.environ, MOBILEGL_TRANSPORT='spawn', MOBILEGL_IPC_CONTROL=args.endpoint,
               MOBILEGL_IPC_DATA='stream', MOBILEGL_IPC_TOKEN=args.token, MOBILEGL_IPC_REQUIRE_SAME_BUILD='1',
               MOBILEGL_IPC_LOG_FORWARD='1', MOBILEGL_PIPE_STATS='1', MOBILEGL_PIPE_STATS_PERIOD='1',
               MOBILEGL_IPC_PRESENT_CREDIT=str(credit), MOBILEGL_IPC_RUN_AHEAD='1', MOBILEGL_IPC_VERB_BARRIER='1',
               MOBILEGL_IPC_BATCH_WAITS='1', MOBILEGL_BACKEND_TYPE='DirectGLES',
               MOBILEGL_IPC_STRICT_ERRORS='0', MOBILEGL_IPC_ROLE_SPLIT_STATE='0', MOBILEGL_IPC_AUDIT='0')
    for name in ('MOBILEGL_IPC_DIAL', 'MOBILEGL_IPC_SERVER_PATH'):
        env.pop(name, None)
    command = [str(args.runner), '--trace', str(trace), '--mobilegl-library', str(args.library),
               '--backend', 'DirectGLES', '--width', str(width), '--height', str(height),
               '--output', str(work), '--benchmark', '--benchmark-finish=0', '--benchmark-tail-frames=' + str(tail)]
    def wake():
        # Keep maintenance output outside the progress scan: a failed adb wake
        # must not make a stalled replay look active.
        with (work.parent / 'wake-adb.log').open('ab') as log:
            try:
                subprocess.run([args.adb, '-s', args.serial, 'shell', 'input', 'keyevent', 'KEYCODE_WAKEUP'],
                               stdout=log, stderr=log, timeout=5, check=False)
            except (OSError, subprocess.TimeoutExpired) as error:
                log.write((str(error) + '\n').encode())
    result = supervise(command, env, str(args.inputs), work, work / 'runner.log',
                       idle_seconds=300, max_seconds=7200, wake=wake)
    if result['returncode'] or result['timeout_kind'] or result['remaining_processes'] or result['leftovers_after_parent_exit']:
        return {**result, 'status': 'failed', 'output': str(work), 'command': command}
    actual_arm = actual_arm_from_text((work / 'mobilegl.client.log').read_text(errors='replace'))
    try:
        values = validate_measurement(work, args.endpoint, credit, tail)
    except (OSError, ValueError, KeyError) as error:
        return {**result, 'status': 'failed', 'actual_arm': actual_arm,
                'error': str(error), 'output': str(work), 'command': command}
    return {**result, 'status': 'passed', 'output': str(work), 'command': command, **values}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runner', type=Path, required=True)
    parser.add_argument('--library', type=Path, required=True)
    parser.add_argument('--inputs', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--endpoint', required=True)
    parser.add_argument('--token', default=os.environ.get('MOBILEGL_IPC_TOKEN', ''))
    parser.add_argument('--serial', required=True, help='explicit adb serial to keep awake during each run')
    parser.add_argument('--adb', default='adb')
    parser.add_argument('--case', action='append', choices=CASES)
    parser.add_argument('--credit', action='append', type=int, choices=(1, 2, 3))
    parser.add_argument('--resume', action='store_true')
    args = parser.parse_args()
    if not args.endpoint.startswith('tcp://'):
        parser.error('a TCP endpoint is required')
    args.out.mkdir(parents=True, exist_ok=True)
    checkpoint = args.out / 'checkpoint.json'
    rows = json.loads(checkpoint.read_text()) if checkpoint.exists() else []
    if rows and not args.resume:
        parser.error('output already contains evidence; use --resume or a new output directory')
    for case in args.case or CASES:
        for credit in args.credit or (1, 2, 3):
            provenance = {'case': case, 'credit': credit, 'endpoint': args.endpoint,
                          'token_hash': fingerprint(args.token), 'runner_sha256': digest(args.runner),
                          'library_sha256': digest(args.library),
                          'trace_sha256': digest(args.inputs / case / CASES[case][1]),
                          'script_sha256': digest(Path(__file__)),
                          'helper_sha256': {name: digest(Path(__file__).with_name(name)) for name in (
                              'measure_transport_cpu.py', 'measure_loopback_cpu.py', 'run_tcp_matrix.py')},
                          'metrics_sha256': digest(Path(METRICS.__file__))}
            identity = fingerprint(provenance)
            prior = next((row for row in reversed(rows) if row.get('identity') == identity and row.get('status') == 'passed'), None)
            if args.resume and prior:
                directory = Path(prior['output'])
                if all((directory / filename).is_file() and digest(directory / filename) == prior[field]
                       for filename, field in (('benchmark.json', 'benchmark_sha256'), ('mobilegl.client.log', 'client_log_sha256'),
                                               ('mobilegl.server.log', 'server_log_sha256'))):
                    print(json.dumps({'case': case, 'credit': credit, 'status': 'passed', 'resumed': True}), flush=True)
                    continue
            work = args.out / case / f'credit-{credit}' / ('attempt-' + uuid.uuid4().hex)
            row = {'case': case, 'credit': credit, 'identity': identity,
                   'provenance': provenance, 'started_at_ns': time.time_ns()}
            print(json.dumps({**row, 'status': 'running'}), flush=True)
            try:
                row.update(measure(args, case, credit, work))
            except Exception as error:
                row.update(status='failed', error=str(error), output=str(work))
            rows.append(row)
            atomic_json(checkpoint, rows)
            print(json.dumps({key: row.get(key) for key in ('case', 'credit', 'status', 'seconds', 'fps', 'wait_replies_per_frame',
                                                          'rtt_mean_us', 'client_thread_cpu_ms_per_frame', 'error')}), flush=True)
            if row['status'] != 'passed':
                return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
