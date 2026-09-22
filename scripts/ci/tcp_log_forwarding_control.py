#!/usr/bin/env python3
"""Run the four real arming scenarios with server log forwarding enabled and disabled."""
import argparse
import errno
import json
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import time
import xml.etree.ElementTree as ET


CASES = {
    'unlocated': ('DirectGLES', 'UnlocatedIoBlockScenario.TheEmulationIsActuallyArmedWhenTheEnvironmentPinsItOn',
                  {'MOBILEGL_ESPRYT_UNLOCATED_IO_BLOCKS': '1'}, 'WITHOUT their layout(location) qualifier'),
    'primitives': ('DirectVulkan', 'PrimitivesGeneratedNoXfbScenario.TheRerouteIsActuallyArmedWhenTheEnvironmentPinsItOn',
                   {'MOBILEGL_MAGMA_PRIMGEN_QUERY_REROUTE': '1'}, 'PRIMITIVES_GENERATED reroute engaged'),
    'point-size': ('DirectGLES', 'PointSizeDemotionScenario.TheDemotionIsActuallyArmedWhenTheEnvironmentPinsItOn',
                   {'MOBILEGL_POINT_SIZE_DEMOTION': '1'}, 'demoted to an ordinary varying'),
    'verify': ('DirectGLES', 'PipeVerifyArmingScenario.Armed',
               {'MOBILEGL_PIPE_VERIFY': '1', 'MGITEST_PIPE_ARMING_LANE': '1'}, 'MGPipe: verify armed'),
}


def read(path):
    return path.read_text(errors='replace') if path.exists() else ''


def run_arm(build, out, case, forward):
    backend, test, knobs, marker = CASES[case]
    out.mkdir(parents=True, exist_ok=True)
    with socket.socket() as reserve:
        reserve.bind(('127.0.0.1', 0))
        port = reserve.getsockname()[1]
    endpoint = f'tcp://127.0.0.1:{port}'
    env = dict(os.environ, MOBILEGL_TRANSPORT='spawn', MOBILEGL_BACKEND_TYPE=backend,
               MOBILEGL_IPC_REQUIRE_SAME_BUILD='1', MOBILEGL_IPC_TOKEN='', EGL_PLATFORM='surfaceless',
               LIBGL_ALWAYS_SOFTWARE='1', MOBILEGL_ITEST_REQUIRE_GPU='1', **knobs)
    for name in ('MOBILEGL_PIPE_VERIFY_CORRUPT', 'MOBILEGL_IPC_CONTROL', 'MOBILEGL_IPC_DIAL'):
        env.pop(name, None)
    if Path('/usr/share/glvnd/egl_vendor.d/50_mesa.json').exists():
        env['__EGL_VENDOR_LIBRARY_FILENAMES'] = '/usr/share/glvnd/egl_vendor.d/50_mesa.json'
    if Path('/usr/share/vulkan/icd.d/lvp_icd.json').exists():
        env['VK_ICD_FILENAMES'] = '/usr/share/vulkan/icd.d/lvp_icd.json'
    peer_env = dict(env, MOBILEGL_IPC_ROLE='server', MOBILEGL_IPC_DIAL='no',
                    MOBILEGL_IPC_LOG_FORWARD=str(int(forward)), MOBILEGL_LOG_FILE_PATH=str(out / 'peer.log'))
    for name in ('MOBILEGL_TRANSPORT', 'MOBILEGL_IPC_SERVER_PATH', 'MOBILEGL_IPC_RING_MB', 'MOBILEGL_IPC_STAGE_MB'):
        peer_env.pop(name, None)
    with (out / 'supervisor.log').open('wb') as output:
        server = subprocess.Popen([str(build / 'libMobileGLServer.so'), endpoint, '--serve'],
                                  env=peer_env, stdout=output, stderr=output, start_new_session=True)
    try:
        for _ in range(100):
            if server.poll() is not None:
                raise RuntimeError('supervisor exited before listening: ' + read(out / 'supervisor.log'))
            with socket.socket() as probe:
                probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                try:
                    probe.bind(('127.0.0.1', port))
                except OSError as error:
                    if error.errno == errno.EADDRINUSE:
                        break
                    raise
            time.sleep(.05)
        else:
            raise RuntimeError('supervisor did not listen')
        client_env = dict(env, MOBILEGL_IPC_CONTROL=endpoint, MOBILEGL_IPC_DATA='stream',
                          MOBILEGL_IPC_LOG_FORWARD='1', MOBILEGL_LOG_FILE_PATH=str(out / 'client.log'),
                          MGITEST_SPLIT_LANE='1', MGITEST_TCP_LANE='1')
        result = subprocess.run([str(build / 'MobileGL/MG_IntegrationTest/MobileGLIntegrationTest'),
                                 '--gtest_filter=' + test, '--gtest_output=xml:' + str(out / 'result.xml')],
                                env=client_env, capture_output=True, text=True, timeout=120)
        (out / 'stdout.log').write_text(result.stdout + result.stderr)
    finally:
        try:
            os.killpg(server.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        server.wait(timeout=10)
    passed = failed = skipped = 0
    marker_assertion_failed = False
    if (out / 'result.xml').exists():
        for entry in ET.parse(out / 'result.xml').getroot().iter('testcase'):
            if entry.find('skipped') is not None or entry.get('status') == 'notrun':
                skipped += 1
            elif entry.find('failure') is not None:
                failed += 1
                marker_assertion_failed = any(marker in (failure.get('message', '') + (failure.text or ''))
                                              for failure in entry.findall('failure'))
            else:
                passed += 1
    client_log = read(out / 'client.client.log')
    return {'case': test, 'returncode': result.returncode, 'passed': passed, 'failed': failed, 'skipped': skipped,
            'tcp_armed': bool(re.search(r'control=tcp data=stream server=\S+ pid=[1-9][0-9]*', client_log)),
            'marker_in_client': marker in client_log,
            'marker_in_server': marker in read(out / 'peer.server.log'),
            'marker_forwarded': marker in read(out / 'client.server.log'),
            'marker_assertion_failed': marker_assertion_failed,
            'output': str(out)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--verify-build-dir', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--case', action='append', choices=CASES,
                        help='Explicit subset; the default runs all four and requires a verify build')
    args = parser.parse_args()
    selected = args.case or list(CASES)
    if 'verify' in selected and not args.verify_build_dir:
        parser.error('the verify scenario requires --verify-build-dir configured with MOBILEGL_PIPE_VERIFY=ON')
    args.out.mkdir(parents=True, exist_ok=True)
    results = {}
    for case in selected:
        build = args.verify_build_dir if case == 'verify' else args.build_dir
        arms = {}
        for forward in (True, False):
            name = 'forward-on' if forward else 'forward-off'
            try:
                arms[name] = run_arm(build, args.out / case / name, case, forward)
            except Exception as error:
                arms[name] = {'error': str(error)}
        on, off = arms['forward-on'], arms['forward-off']
        if case == 'verify':
            # PipeFill owns this marker in the client. Disabling peer forwarding
            # must leave it visible; requiring red here would test a false design.
            arms['expectation'] = 'client-owned marker remains green with peer forwarding disabled'
            arms['proved'] = all(arm.get('passed') == 1 and arm.get('returncode') == 0
                                 and arm.get('tcp_armed') and arm.get('marker_in_client')
                                 for arm in (on, off))
        else:
            arms['expectation'] = 'server-owned marker passes with forwarding and fails without it'
            arms['proved'] = bool(on.get('passed') == 1 and on.get('returncode') == 0
                                 and on.get('tcp_armed') and on.get('marker_in_server') and on.get('marker_forwarded')
                                 and off.get('failed') == 1 and not off.get('skipped') and off.get('tcp_armed')
                                 and off.get('marker_in_server') and not off.get('marker_forwarded')
                                 and off.get('marker_assertion_failed')
                                 and not off.get('marker_in_client'))
        results[case] = arms
        print(case + ': ' + ('PASS' if arms['proved'] else 'NOT PROVEN'), flush=True)
    (args.out / 'summary.json').write_text(json.dumps(results, indent=2))
    return 0 if all(result['proved'] for result in results.values()) else 1


if __name__ == '__main__':
    raise SystemExit(main())
