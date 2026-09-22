#!/usr/bin/env python3
"""Executable negative controls for TCP lane accounting and supervisor lifecycle."""
import importlib.util
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]


def module(name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(name + '.py'))
    loaded = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(loaded)
    return loaded


class Accounting(unittest.TestCase):
    def test_each_tcp_case_needs_a_real_endpoint_and_pid(self):
        tally = module('junit_tally')
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            name = 'DirectGLES.Tcp.TriangleScenario.Draw'
            junit, discovery, base = root / 'junit.xml', root / 'lane.json', root / 'case.log'
            junit.write_text(f'<testsuite><testcase name="{name}" /></testsuite>')
            discovery.write_text(json.dumps({'tests': [{'name': name, 'properties': [
                {'name': 'ENVIRONMENT', 'value': ['MOBILEGL_LOG_FILE_PATH=' + str(base)]}]}]}))
            role = root / 'case.client.log'
            role.write_text('control=tcp data=stream server=127.0.0.1:40613 pid=42\n')
            tally.require_tcp_proof(junit, 'DirectGLES.Tcp.', discovery)
            with self.assertRaises(ValueError):
                tally.require_tcp_proof(junit, 'DirectGLES.Tcp.', discovery, require_run_ahead=True)
            armed = 'control=tcp data=stream server=127.0.0.1:40613 pid=42\nrun-ahead ARMED\n'
            role.write_text(armed)
            tally.require_tcp_proof(junit, 'DirectGLES.Tcp.', discovery, require_run_ahead=True)
            for fallback in ('running lockstep', 'run-ahead DISARMED'):
                role.write_text(armed + fallback)
                with self.assertRaises(ValueError):
                    tally.require_tcp_proof(junit, 'DirectGLES.Tcp.', discovery, require_run_ahead=True)
            for bad in ('', 'control=tcp data=shm server=127.0.0.1:40613 pid=42',
                        'control=tcp data=stream server=127.0.0.1:40613 pid=0',
                        'spawn ARMED - the server role runs in pid 42'):
                role.write_text(bad)
                with self.assertRaises(ValueError):
                    tally.require_tcp_proof(junit, 'DirectGLES.Tcp.', discovery)

    def test_equal_counts_do_not_hide_a_missing_case(self):
        parity = module('spawn_lane_parity')
        a, b = 'TriangleScenario.Draw', 'TriangleScenario.Read'
        with patch.object(parity, 'lane_cases', side_effect=[{a}, {a}, {b}]), \
                patch.object(sys, 'argv', ['spawn_lane_parity.py', 'unused']):
            self.assertEqual(parity.main(), 1)
        with patch.object(parity, 'lane_cases', return_value={a}), \
                patch.object(sys, 'argv', ['spawn_lane_parity.py', 'unused']):
            self.assertEqual(parity.main(), 0)


class DeviceIdleExemption(unittest.TestCase):
    def test_stop_restores_only_the_setting_this_task_changed(self):
        spec = importlib.util.spec_from_file_location('tcp_device_server', ROOT / 'tools/trace_replay/tcp_device_server.py')
        device = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(device)
        package = 'top.mobilegl.plugin.trace'
        for initially_allowed in (False, True):
            allowed = {package} if initially_allowed else set()
            def shell(_adb, command):
                if len(command) == 3:
                    return '\n'.join('user,' + name + ',10325' for name in sorted(allowed))
                change = command[3]
                if change.startswith('+'):
                    allowed.add(change[1:])
                else:
                    allowed.discard(change[1:])
                return ''
            with tempfile.TemporaryDirectory() as directory, patch.object(device, 'shell', side_effect=shell):
                state = Path(directory) / 'original.json'
                device.allow_idle([], 'phone', package, state)
                # A restarted supervisor must retain the first original state.
                device.allow_idle([], 'phone', package, state)
                self.assertIn(package, allowed)
                self.assertEqual(json.loads(state.read_text())['originally_whitelisted'], initially_allowed)
                with self.assertRaises(ValueError):
                    device.restore_idle([], 'different-phone', package, state)
                self.assertTrue(state.exists())
                device.restore_idle([], 'phone', package, state)
                self.assertEqual(package in allowed, initially_allowed)
                self.assertFalse(state.exists())


@unittest.skipUnless(sys.platform.startswith('linux'), 'supervisor fixture uses Linux /proc')
class Supervisor(unittest.TestCase):
    def test_readiness_does_not_consume_a_connection_and_stop_owns_only_its_pid(self):
        fixture = module('tcp_server_fixture')
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            server = root / 'server'
            server.write_text('#!' + sys.executable + '\n'
                              'import socket,sys\n'
                              'port=int(sys.argv[1].rsplit(":",1)[1])\n'
                              's=socket.socket();s.bind(("127.0.0.1",port));s.listen()\n'
                              'i=0\n'
                              'while True:\n'
                              ' c,_=s.accept();i+=1;c.sendall(str(i).encode());c.close()\n')
            server.chmod(0o755)
            with socket.socket() as reservation:
                reservation.bind(('127.0.0.1', 0))
                port = reservation.getsockname()[1]
            state = root / 'state.json'
            subprocess.run([sys.executable, str(Path(fixture.__file__)), 'start',
                            '--server', str(server), '--endpoint', f'tcp://127.0.0.1:{port}',
                            '--state', str(state)], check=True)
            try:
                with socket.create_connection(('127.0.0.1', port)) as peer:
                    self.assertEqual(peer.recv(10), b'1')
                # An unrelated live PID with a different start token is not ours.
                unrelated = root / 'unrelated.json'
                unrelated.write_text(json.dumps({'pid': os.getpid(), 'start': 'not-this-process'}))
                fixture.stop(unrelated)
                already_exited = root / 'already-exited.json'
                already_exited.write_text(json.dumps({'pid': 2147483647, 'start': None}))
                fixture.stop(already_exited)
                self.assertFalse(already_exited.exists())
            finally:
                fixture.stop(state)
            with self.assertRaises(OSError):
                socket.create_connection(('127.0.0.1', port), timeout=.2)


if __name__ == '__main__':
    unittest.main()
