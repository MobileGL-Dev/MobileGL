#!/usr/bin/env python3
"""Reboot-clean, pinned Redmi TCP credit measurements with original-state restore.

This explicitly mutating session is separate from the ordinary benchmark tool.
Run only while this device has no other client. The service/idle exemption is
left available to its owner; CPU/GPU/fan settings are always restored here.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import time

import benchmark_tcp_credits as benchmark
from measure_transport_cpu import CASES, digest
from run_tcp_matrix import atomic_json, supervise


CPU = '/sys/devices/system/cpu/cpufreq'
GPU = '/sys/class/kgsl/kgsl-3d0'
FAN = '/sys/class/xm_power/hw_monitor/pwm_fan'
POLICIES = ('policy0', 'policy6')
RESTORE_NODES = [f'{CPU}/{policy}/{field}' for policy in POLICIES
                 for field in ('scaling_min_freq', 'scaling_max_freq', 'scaling_governor')]
RESTORE_NODES += [f'{GPU}/min_pwrlevel', f'{GPU}/max_pwrlevel', f'{FAN}/target_level']
READ_NODES = RESTORE_NODES + [f'{CPU}/{policy}/scaling_cur_freq' for policy in POLICIES]
READ_NODES += [f'{GPU}/gpuclk', f'{FAN}/real_speed', f'{FAN}/pwm_duty']
ORDERS = ((1, 2, 3), (2, 3, 1), (3, 1, 2))


class Device:
    def __init__(self, serial):
        self.adb = ['adb', '-s', serial]

    def shell(self, argv, timeout=30):
        return subprocess.run([*self.adb, 'shell', shlex.join(argv)], check=True,
                              capture_output=True, text=True, timeout=timeout).stdout.strip()

    def su(self, script):
        return self.shell(['su', '-c', script])

    def boot_id(self):
        return self.shell(['cat', '/proc/sys/kernel/random/boot_id'])

    def observe(self):
        statements = [f'printf "%s=%s\\n" {shlex.quote(node)} "$(cat {shlex.quote(node)})"'
                      for node in READ_NODES]
        statements.append('for node in /sys/class/thermal/thermal_zone*; do '
                          'if [ "$(cat "$node/type")" = cpuss-0-0 ]; then '
                          'printf "thermal_path=%s\\nthermal_mc=%s\\n" "$node" "$(cat "$node/temp")"; '
                          'break; fi; done')
        values = dict(line.split('=', 1) for line in self.su('; '.join(statements)).splitlines() if '=' in line)
        if any(not values.get(node) for node in READ_NODES) or not values.get('thermal_mc'):
            raise RuntimeError('incomplete device state; cannot establish or restore the thermal window')
        for node, value in values.items():
            if node.endswith('scaling_governor'):
                if not re.fullmatch(r'[A-Za-z0-9_-]+', value):
                    raise ValueError('invalid governor observation')
            elif node != 'thermal_path':
                int(value)
        return {'time_ns': time.time_ns(), **values}

    def restore(self, original):
        statements = []
        for policy in POLICIES:
            base = f'{CPU}/{policy}'
            statements += [f'cat {base}/cpuinfo_min_freq > {base}/scaling_min_freq',
                           f'echo {int(original[base + "/scaling_max_freq"])} > {base}/scaling_max_freq',
                           f'echo {int(original[base + "/scaling_min_freq"])} > {base}/scaling_min_freq',
                           f'echo {shlex.quote(original[base + "/scaling_governor"])} > {base}/scaling_governor']
        statements += [f'echo {int(original[node])} > {node}' for node in RESTORE_NODES if node.startswith((GPU, FAN))]
        self.su('; '.join(statements))
        after = self.observe()
        differences = {node: {'expected': original[node], 'actual': after[node]}
                       for node in RESTORE_NODES if original[node] != after[node]}
        return {'after': after, 'differences': differences, 'restored': not differences}


def wait_for_boot(device, old_boot_id):
    subprocess.run([*device.adb, 'reboot'], check=True, timeout=30)
    deadline = time.monotonic() + 300
    while time.monotonic() < deadline:
        try:
            if device.shell(['getprop', 'sys.boot_completed']) == '1' and device.boot_id() != old_boot_id:
                time.sleep(20)
                return device.boot_id()
        except (OSError, subprocess.SubprocessError):
            pass
        time.sleep(5)
    raise RuntimeError('reboot did not establish a changed, fully booted device')


def pin(args, action, output):
    result = subprocess.run(['bash', str(args.pin_copy), args.serial, action],
                            capture_output=True, text=True, timeout=30)
    output.write_text(result.stdout + result.stderr, encoding='utf-8')
    return result.returncode


def prepare_arm(args, device, directory):
    directory.mkdir(parents=True)
    if pin(args, 'pin', directory / 'pin-request.txt') != 0:
        raise RuntimeError('pin did not take')
    readings, consecutive = [], 0
    deadline = time.monotonic() + args.cool_timeout
    while time.monotonic() < deadline:
        observation = device.observe()
        readings.append(observation)
        atomic_json(directory / 'cooldown.json', readings)
        if int(observation['thermal_mc']) <= args.cool_max_mc:
            consecutive += 1
            if consecutive == 2:
                break
        else:
            consecutive = 0
        time.sleep(5)
    if consecutive < 2:
        raise RuntimeError('cooldown expired; no valid measurement window')
    if pin(args, 'check', directory / 'pin-before.txt') != 0:
        raise RuntimeError('frequency drift before measurement')
    if int(readings[-1][FAN + '/target_level']) != 2 or int(readings[-1][FAN + '/real_speed']) <= 0:
        raise RuntimeError('fan setting was not actually established')
    return readings[-1]


def start_server(args, device):
    script = Path(__file__).with_name('tcp_device_server.py')
    command = [sys.executable, str(script), 'start', '--serial', args.serial,
               '--package', args.package, '--listen', args.listen, '--token', args.token,
               '--allow-idle', '--state-file', str(args.server_idle_state), '--env',
               'MOBILEGL_TEST_SERVER_COUNTERS=1;MOBILEGL_PIPE_STATS=1;MOBILEGL_PIPE_STATS_PERIOD=1;'
               'MOBILEGL_LOG_FILE_PATH=/data/data/' + args.package + '/files/mgl.log']
    result = subprocess.run(command, check=True, capture_output=True, text=True, timeout=30)
    (args.out / 'server-start.txt').write_text(result.stdout + result.stderr, encoding='utf-8')
    port = args.endpoint.rsplit(':', 1)[-1]
    for _ in range(20):
        sockets = device.shell(['ss', '-ltn'])
        if re.search(r':' + re.escape(port) + r'\s', sockets):
            return
        time.sleep(1)
    raise RuntimeError('restarted supervisor is not listening')


def run_session(args):
    device = Device(args.serial)
    original = device.observe()
    session = {'status': 'prepared', 'serial': args.serial, 'endpoint': args.endpoint,
               'library_sha256': digest(args.library), 'runner_sha256': digest(args.runner),
               'pin_script_sha256': digest(args.pin_script), 'original_state': original,
               'boot_id_before': device.boot_id(), 'credit_orders': ORDERS,
               'cool_max_mc': args.cool_max_mc, 'arms': [], 'started_at_ns': time.time_ns()}
    state_path = args.out / 'session.json'
    atomic_json(state_path, session)  # Durable before the first mutation, including reboot.
    args.pin_copy = args.out / 'pin_device.sh'
    args.pin_copy.write_text(args.pin_script.read_text(), encoding='utf-8', newline='\n')
    try:
        session['boot_id'] = wait_for_boot(device, session['boot_id_before'])
        session['rebooted_clean'] = True
        device.su(f'echo 2 > {FAN}/target_level')
        time.sleep(3)
        host = args.endpoint.removeprefix('tcp://').rsplit(':', 1)[0]
        wlan = device.shell(['ip', '-f', 'inet', 'addr', 'show', 'wlan0'])
        session['wlan_after_reboot'] = wlan
        if host not in wlan:
            raise RuntimeError('device address changed; resolve host routing before a new performance session')
        for _ in range(60):
            names = device.shell(['ps', '-A', '-o', 'NAME'])
            if not any('dex2oat' in name for name in names.splitlines()):
                break
            time.sleep(5)
        else:
            raise RuntimeError('dex2oat did not become idle')
        start_server(args, device)
        session['status'] = 'running'
        atomic_json(state_path, session)
        index = 0
        for case in args.case or CASES:
            for repeat, order in enumerate(ORDERS, 1):
                for credit in order:
                    index += 1
                    directory = args.out / f'{index:02d}-{case}-credit{credit}-r{repeat}'
                    row = {'index': index, 'case': case, 'credit': credit, 'repeat': repeat, 'status': 'preparing'}
                    session['arms'].append(row)
                    atomic_json(state_path, session)
                    print(json.dumps(row), flush=True)
                    row['before'] = prepare_arm(args, device, directory)
                    try:
                        row.update(benchmark.measure(args, case, credit, directory / 'measurement'))
                    finally:
                        row['pin_after_rc'] = pin(args, 'check', directory / 'pin-after.txt')
                        row['after'] = device.observe()
                        atomic_json(state_path, session)
                    if row['status'] != 'passed' or row['pin_after_rc'] != 0:
                        row['status'] = 'invalid'
                        raise RuntimeError(f'arm {index} is invalid; preserve it and stop')
                    print(json.dumps({key: row.get(key) for key in ('index', 'case', 'credit', 'status', 'fps')}), flush=True)
        directory = args.out / 'stage-burst'
        burst = {'before': prepare_arm(args, device, directory)}
        session['stage_burst'] = burst
        script = Path(__file__).with_name('measure_tcp_stage_burst.py')
        command = [sys.executable, str(script), '--library', str(args.library), '--endpoint', args.endpoint,
                   '--token', args.token, '--out', str(directory / 'measurement')]
        def wake():
            device.shell(['input', 'keyevent', 'KEYCODE_WAKEUP'])
        try:
            burst.update(supervise(command, dict(os.environ), str(args.inputs), directory,
                                   directory / 'runner.log', wake=wake))
            if burst['returncode'] != 0 or burst['remaining_processes'] or burst['leftovers_after_parent_exit']:
                raise RuntimeError('Stage burst failed')
            burst['measurement'] = json.loads((directory / 'measurement/result.json').read_text())
        finally:
            burst['pin_after_rc'] = pin(args, 'check', directory / 'pin-after.txt')
            burst['after'] = device.observe()
        if burst['pin_after_rc'] != 0:
            raise RuntimeError('Stage burst pin drifted')
        session['status'] = 'passed'
    except BaseException as error:
        session.update(status='failed', error=repr(error))
        raise
    finally:
        try:
            session['restore'] = device.restore(original)
            if not session['restore']['restored']:
                session['status'] = 'restore-failed'
        except BaseException as error:
            session.update(status='restore-failed', restore_error=repr(error))
        session['completed_at_ns'] = time.time_ns()
        atomic_json(state_path, session)
    return 0 if session['status'] == 'passed' else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('runner', 'library', 'inputs', 'out', 'pin-script', 'server-idle-state'):
        parser.add_argument('--' + name, required=True, type=Path)
    parser.add_argument('--serial', required=True, choices=('2f7cbe2e',), help='profile validated for this Redmi only')
    parser.add_argument('--endpoint', required=True)
    parser.add_argument('--listen', required=True)
    parser.add_argument('--token', default=os.environ.get('MOBILEGL_IPC_TOKEN', ''))
    parser.add_argument('--package', default='top.mobilegl.plugin.trace')
    parser.add_argument('--cool-max-mc', type=int, default=36000)
    parser.add_argument('--cool-timeout', type=int, default=300)
    parser.add_argument('--case', action='append', choices=CASES)
    parser.add_argument('--reboot', action='store_true', required=True)
    args = parser.parse_args()
    args.adb = 'adb'
    if args.out.exists():
        parser.error('choose a new output directory; paired sessions never resume across boots')
    if not args.endpoint.startswith('tcp://') or args.cool_timeout <= 0:
        parser.error('TCP endpoint and a positive cooldown timeout are required')
    args.out.mkdir(parents=True)
    return run_session(args)


if __name__ == '__main__':
    raise SystemExit(main())
