#!/usr/bin/env python3
"""Record client thread CPU and sampled apply-thread CPU for unchanged trace workloads.

The client clock comes from the existing benchmark runner. Apply CPU comes from
/proc/<pid>/task/<tid>/schedstat, never process-wide CPU. The sampled tail window
is approximately anchored to benchmark.json's mtime; raw samples and this
alignment limitation are retained so the result cannot claim cycle precision.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess
import tarfile
import time


CASES = {
    'OpenRA': ('openra.tgz', 'openra.trace', 640, 480, 100),
    'rd12': ('minecraft-1.21.4-rd12-odinlite-in-world.tgz', 'trace.trace', 854, 480, 200),
}


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(chunk)
    return value.hexdigest()


def thread_samples(pid):
    pids = [pid]
    try:
        pids += [int(value) for value in Path(f'/proc/{pid}/task/{pid}/children').read_text().split()]
    except (FileNotFoundError, ProcessLookupError):
        pass
    rows = []
    for process in pids:
        try:
            tasks = sorted(Path(f'/proc/{process}/task').iterdir(), key=lambda path: int(path.name))
        except (FileNotFoundError, ProcessLookupError):
            continue
        for task in tasks:
            try:
                name = (task / 'comm').read_text().strip()
                if name != 'mgl-srv-apply':
                    continue
                cpu_ns = int((task / 'schedstat').read_text().split()[0])
                rows.append({'time_ns': time.monotonic_ns(), 'pid': process, 'tid': int(task.name),
                             'name': name, 'cpu_ns': cpu_ns})
                # Driver helper threads inherit their creator's comm. The
                # ServerLoop thread exists before it calls the driver, so its
                # live TID precedes those helpers; do not sum them as apply CPU.
                break
            except (FileNotFoundError, ProcessLookupError):
                continue
    return rows


def at_time(samples, timestamp):
    for left, right in zip(samples, samples[1:]):
        if left['time_ns'] <= timestamp <= right['time_ns']:
            width = right['time_ns'] - left['time_ns']
            part = (timestamp - left['time_ns']) / width if width else 0
            return left['cpu_ns'] + part * (right['cpu_ns'] - left['cpu_ns'])
    return None


def reduce_apply(samples, start, end, frames):
    grouped = {}
    for row in samples:
        grouped.setdefault((row['pid'], row['tid']), []).append(row)
    contributions = []
    for (pid, tid), rows in grouped.items():
        first = at_time(rows, start)
        last = at_time(rows, end)
        if first is None or last is None:
            continue
        inside = [row for row in rows if start <= row['time_ns'] <= end]
        gaps = [(b['time_ns'] - a['time_ns']) / 1e6 for a, b in zip(inside, inside[1:])]
        contributions.append({'pid': pid, 'tid': tid, 'cpu_ms': (last - first) / 1e6,
                              'sample_count': len(inside), 'max_sample_gap_ms': max(gaps, default=None)})
    # One owning apply thread must cover the whole selected window. Refuse to
    # sum unrelated driver threads that inherited an indistinguishable name.
    if len(contributions) != 1:
        return {'valid': False, 'reason': 'no unique apply thread spans the complete tail window',
                'candidates': contributions}
    return {'valid': True, **contributions[0], 'mean_cpu_ms_per_frame': contributions[0]['cpu_ms'] / frames}


def run(args, transport, case, repeat):
    archive, trace_name, width, height, tail = CASES[case]
    trace_dir = args.inputs / case
    trace_dir.mkdir(parents=True, exist_ok=True)
    trace = trace_dir / trace_name
    if not trace.exists():
        archive_path = args.fixtures / archive
        if archive_path.stat().st_size < 1024:
            raise ValueError(f'{archive_path}: fixture is an LFS pointer, not a trace archive')
        with tarfile.open(archive_path, 'r:gz') as bundle:
            bundle.extractall(trace_dir, filter='data')
    out = args.out / transport / case / f'run-{repeat}'
    out.mkdir(parents=True, exist_ok=True)
    if (out / 'benchmark.json').exists():
        raise ValueError(f'{out}: evidence already exists; choose a new output directory')
    trace_digest = digest(trace)
    env = dict(os.environ, MOBILEGL_TRANSPORT=transport, MOBILEGL_IPC_SERVER_PATH=str(args.server),
               MOBILEGL_PIPE_STATS='0', MOBILEGL_IPC_PRESENT_CREDIT='1', MOBILEGL_IPC_RUN_AHEAD='1',
               MOBILEGL_IPC_STRICT_ERRORS='0', MOBILEGL_IPC_ROLE_SPLIT_STATE='0', MOBILEGL_IPC_AUDIT='0',
               MOBILEGL_IPC_VERB_BARRIER='1', MOBILEGL_IPC_BATCH_WAITS='1', MOBILEGL_IPC_SPIN_US='50',
               LIBGL_ALWAYS_SOFTWARE='1', EGL_PLATFORM='surfaceless',
               MESA_GL_VERSION_OVERRIDE='3.3', MESA_GLSL_VERSION_OVERRIDE='330')
    for key in ('MOBILEGL_IPC_CONTROL', 'MOBILEGL_IPC_DATA', 'MOBILEGL_IPC_TOKEN', 'MOBILEGL_IPC_DIAL',
                'MOBILEGL_IPC_REQUIRE_SAME_BUILD', 'MOBILEGL_TEST_SERVER_COUNTERS'):
        env.pop(key, None)
    if Path('/usr/share/glvnd/egl_vendor.d/50_mesa.json').exists():
        env['__EGL_VENDOR_LIBRARY_FILENAMES'] = '/usr/share/glvnd/egl_vendor.d/50_mesa.json'
    command = [str(args.runner), '--trace', str(trace), '--mobilegl-library', str(args.library),
               '--backend', 'DirectGLES', '--width', str(width), '--height', str(height),
               '--output', str(out), '--benchmark', '--benchmark-finish=0',
               '--benchmark-tail-frames=' + str(tail), '--hold-ms', '150']
    raw = []
    offset_before = time.monotonic_ns() - time.time_ns()
    started = time.monotonic()
    with (out / 'runner.log').open('w') as log:
        process = subprocess.Popen(command, env=env, stdout=log, stderr=log)
        try:
            while True:
                raw.extend(thread_samples(process.pid))
                if process.poll() is not None:
                    break
                if time.monotonic() - started > args.timeout:
                    process.kill()
                    raise TimeoutError('benchmark exceeded its run budget')
                time.sleep(args.sample_ms / 1000)
        finally:
            process.wait()
    offset_after = time.monotonic_ns() - time.time_ns()
    (out / 'apply-samples.json').write_text(json.dumps(raw))
    if process.returncode:
        raise RuntimeError(f'{out}: replay failed with {process.returncode}')
    report_path = out / 'benchmark.json'
    report = json.loads(report_path.read_text())
    wall, cpu = report['frameTimesMs'], report['frameCpuTimesMs']
    if len(wall) != len(cpu) or len(wall) <= tail:
        raise RuntimeError('no complete CPU series or no startup-excluding tail window')
    # End() precedes JSON serialization. Subtract the measured unframed suffix
    # after the last frame; do not accidentally include teardown in the tail.
    report_end = report_path.stat().st_mtime_ns + (offset_before + offset_after) // 2
    unframed_ns = int(report['totalSeconds'] * 1e9 - sum(wall) * 1e6)
    tail_end = report_end - unframed_ns
    tail_start = tail_end - int(sum(wall[-tail:]) * 1e6)
    apply = reduce_apply(raw, tail_start, tail_end, tail)
    result = {'label': args.label, 'transport': transport, 'case': case, 'repeat': repeat,
              'total_frames': len(wall), 'tail_frames': tail, 'tail_wall_ms': sum(wall[-tail:]),
              'client_mean_cpu_ms': statistics.mean(cpu[-tail:]),
              'client_median_cpu_ms': statistics.median(cpu[-tail:]),
              'apply': apply, 'sample_period_ms': args.sample_ms,
              'frame_window_start_ns': tail_start, 'frame_window_end_ns': tail_end,
              'clock_offset_drift_ns': offset_after - offset_before,
              'apply_clock': '/proc/<pid>/task/<tid>/schedstat execution runtime (nanoseconds)',
              'apply_thread_selection': 'oldest live TID named mgl-srv-apply; excludes driver helpers inheriting its name',
              'alignment_limit': 'mtime anchor includes unmeasured benchmark summary/JSON serialization latency; '
                                 'apply endpoints are interpolated between external samples, not exact frame markers',
              'runner_sha256': args.runner_digest, 'library_sha256': args.library_digest,
              'server_sha256': args.server_digest, 'trace_sha256': trace_digest,
              'runtime_environment': {key: value for key, value in env.items()
                                      if key.startswith(('MOBILEGL_', 'MESA_', 'LIBGL_', '__EGL_'))},
              'command': command, 'output': str(out)}
    (out / 'cpu-summary.json').write_text(json.dumps(result, indent=2))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runner', type=Path, required=True)
    parser.add_argument('--library', type=Path, required=True)
    parser.add_argument('--server', type=Path, required=True)
    parser.add_argument('--fixtures', type=Path, required=True)
    parser.add_argument('--inputs', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--label', required=True)
    parser.add_argument('--transport', action='append', choices=('inproc', 'spawn'))
    parser.add_argument('--case', action='append', choices=CASES)
    parser.add_argument('--repeats', type=int, default=3)
    parser.add_argument('--case-repeats', action='append', default=[], metavar='CASE=N',
                        help='Override repetitions for one case, e.g. rd12=2')
    parser.add_argument('--sample-ms', type=float, default=5)
    parser.add_argument('--timeout', type=float, default=900)
    args = parser.parse_args()
    if args.repeats < 1 or args.sample_ms <= 0:
        parser.error('repeats and sample-ms must be positive')
    repeats = {}
    for override in args.case_repeats:
        name, separator, count = override.partition('=')
        if not separator or name not in CASES or not count.isdigit() or int(count) < 1:
            parser.error('--case-repeats requires a known CASE and a positive count')
        repeats[name] = int(count)
    args.runner_digest, args.library_digest, args.server_digest = map(digest, (args.runner, args.library, args.server))
    results = []
    for case in args.case or list(CASES):
        for repeat in range(repeats.get(case, args.repeats)):
            for transport in args.transport or ('inproc', 'spawn'):
                result = run(args, transport, case, repeat)
                results.append(result)
                (args.out / 'summary.json').write_text(json.dumps(results, indent=2))
                print(json.dumps({key: result[key] for key in ('case', 'transport', 'repeat', 'client_mean_cpu_ms', 'apply')}), flush=True)
    return 0 if all(row['apply']['valid'] for row in results) else 1


if __name__ == '__main__':
    raise SystemExit(main())
