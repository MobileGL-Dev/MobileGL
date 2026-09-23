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
    def check_tcp_proof(self, backend):
        tally = module('junit_tally')
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            prefix = backend + '.Tcp.'
            name = prefix + 'TriangleScenario.Draw'
            junit, discovery, base = root / 'junit.xml', root / 'lane.json', root / 'case.log'
            junit.write_text(f'<testsuite><testcase name="{name}" /></testsuite>')
            discovery.write_text(json.dumps({'tests': [{'name': name, 'properties': [
                {'name': 'ENVIRONMENT', 'value': ['MOBILEGL_LOG_FILE_PATH=' + str(base)]}]}]}))
            role = root / 'case.client.log'
            role.write_text('control=tcp data=stream server=127.0.0.1:40613 pid=42\n')
            tally.require_tcp_proof(junit, prefix, discovery)
            with self.assertRaises(ValueError):
                tally.require_tcp_proof(junit, prefix, discovery, require_run_ahead=True)
            armed = 'control=tcp data=stream server=127.0.0.1:40613 pid=42\nrun-ahead ARMED\n'
            role.write_text(armed)
            tally.require_tcp_proof(junit, prefix, discovery, require_run_ahead=True)
            for fallback in ('running lockstep', 'run-ahead DISARMED'):
                role.write_text(armed + fallback)
                with self.assertRaises(ValueError):
                    tally.require_tcp_proof(junit, prefix, discovery, require_run_ahead=True)
            for bad in ('', 'control=tcp data=shm server=127.0.0.1:40613 pid=42',
                        'control=tcp data=stream server=127.0.0.1:40613 pid=0',
                        'spawn ARMED - the server role runs in pid 42'):
                role.write_text(bad)
                with self.assertRaises(ValueError):
                    tally.require_tcp_proof(junit, prefix, discovery)

    def test_each_tcp_case_needs_a_real_endpoint_and_pid(self):
        self.check_tcp_proof('DirectGLES')

    def test_the_directvulkan_tcp_lane_is_held_to_the_same_proof(self):
        """P7 exit gate 3 runs this lane on Magma, and the proof is prefix-driven.

        `require_tcp_proof` selects by test-name prefix, so a DirectVulkan sweep asks it for
        `DirectVulkan.Tcp.` - a prefix nothing in the tree had ever passed it. An empty
        selection raises nothing at all, so a whole-backend typo would have read as a clean
        pass; running the identical positive and negative battery under the other prefix is
        what makes that impossible.
        """
        self.check_tcp_proof('DirectVulkan')

    def test_a_prefix_that_matches_nothing_is_not_evidence(self):
        """The shape the test above guards against, stated on its own.

        require_tcp_proof iterates the testcases whose name starts with the prefix and asserts
        per case. With no match it iterates nothing and returns, which is indistinguishable from
        "every case proved its arm" - so the CALLER has to count, and this records that the
        function itself will not.
        """
        tally = module('junit_tally')
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            junit, discovery = root / 'junit.xml', root / 'lane.json'
            junit.write_text('<testsuite><testcase name="DirectGLES.Tcp.A" /></testsuite>')
            discovery.write_text(json.dumps({'tests': []}))
            tally.require_tcp_proof(junit, 'DirectVulkan.Tcp.', discovery)
            self.assertEqual(tally.tally(junit, 'DirectVulkan.Tcp.'), (0, 0, 0))
            # And the same junit under the prefix that DOES match has no private log path, so
            # the proof refuses rather than passing on the strength of the name alone.
            with self.assertRaises(ValueError):
                tally.require_tcp_proof(junit, 'DirectGLES.Tcp.', discovery)

    def test_a_skipped_required_entry_is_not_a_pass(self):
        """P7 wave 2-F, PH-7 (1). The four states of a NAMED entry, and only one of them passes.

        `TcpLane.SupervisorProtocolControls` skips itself (exit 77) when flatc is absent, which
        is correct for a developer and wrong for CI - the workflow now builds flatc and names the
        entry, so `skipped` has to red here exactly as `failed` and `absent` do. Written as four
        cases rather than one because the one that matters is `skipped`, and a check that only
        looked for <failure> would have let it through, which is the whole defect.
        """
        tally = module('junit_tally')
        name = 'TcpLane.SupervisorProtocolControls'
        bodies = {
            'passed': f'<testcase name="{name}" />',
            'skipped': f'<testcase name="{name}"><skipped /></testcase>',
            'notrun': f'<testcase name="{name}" status="notrun" />',
            'failed': f'<testcase name="{name}"><failure /></testcase>',
            'absent': '<testcase name="TcpLane.AccountingControls" />',
        }
        with tempfile.TemporaryDirectory() as directory:
            junit = Path(directory) / 'junit.xml'
            for state, body in bodies.items():
                junit.write_text(f'<testsuite>{body}</testsuite>')
                if state == 'passed':
                    tally.require_entries_passed(junit, [name])
                    continue
                with self.assertRaises(ValueError, msg=state) as raised:
                    tally.require_entries_passed(junit, [name])
                self.assertIn(name, str(raised.exception))

    def test_equal_counts_do_not_hide_a_missing_case(self):
        parity = module('spawn_lane_parity')
        a, b = 'TriangleScenario.Draw', 'TriangleScenario.Read'
        # The Magma tiers (P7 package L) read through lane_names, not lane_cases; an empty set
        # on every arm makes compare_arms report "nothing to compare" and stay out of this
        # assertion, instead of running a real ctest in the placeholder build dir.
        with patch.object(parity, 'lane_cases', side_effect=[{a}, {a}, {b}]), \
                patch.object(parity, 'lane_names', return_value=set()), \
                patch.object(sys, 'argv', ['spawn_lane_parity.py', 'unused']):
            self.assertEqual(parity.main(), 1)
        with patch.object(parity, 'lane_cases', return_value={a}), \
                patch.object(parity, 'lane_names', return_value=set()), \
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

    def started_server_environment(self, **outer):
        """Start the fixture under `outer` (None unsets a key) and return the environment its
        server process actually saw."""
        fixture = module('tcp_server_fixture')
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            seen = root / 'seen.json'
            server = root / 'server'
            server.write_text('#!' + sys.executable + '\n'
                              'import json,os,socket,sys\n'
                              f'open({str(seen)!r},"w").write(json.dumps(dict(os.environ)))\n'
                              'port=int(sys.argv[1].rsplit(":",1)[1])\n'
                              's=socket.socket();s.bind(("127.0.0.1",port));s.listen()\n'
                              'while True: s.accept()[0].close()\n')
            server.chmod(0o755)
            with socket.socket() as reservation:
                reservation.bind(('127.0.0.1', 0))
                port = reservation.getsockname()[1]
            state = root / 'state.json'
            env = dict(os.environ)
            for key, value in outer.items():
                if value is None:
                    env.pop(key, None)
                else:
                    env[key] = value
            subprocess.run([sys.executable, str(Path(fixture.__file__)), 'start',
                            '--server', str(server), '--endpoint', f'tcp://127.0.0.1:{port}',
                            '--state', str(state)], check=True, env=env)
            try:
                return json.loads(seen.read_text())
            finally:
                fixture.stop(state)

    def test_the_supervisor_is_headless_like_every_other_server_the_harness_reaches(self):
        """CI red since P6.5: every DirectGLES.Tcp. case failed "remote TCP bring-up failed".

        Under inproc and spawn the server inherits the CLIENT's environment after
        HeadlessGL.cpp's EnsureHeadlessPlatform pinned EGL_PLATFORM=surfaceless and cleared
        DISPLAY/WAYLAND_DISPLAY. This supervisor is started by ctest from the job environment
        instead, so on a WSLg workstation it bound Mesa's build-time default x11 platform and went
        green, and on a runner with no window system eglInitialize failed with "xcb_connect
        failed" for every session."""
        seen = self.started_server_environment(DISPLAY=':0', WAYLAND_DISPLAY='wayland-0', EGL_PLATFORM=None)
        self.assertEqual(seen.get('EGL_PLATFORM'), 'surfaceless')
        self.assertNotIn('DISPLAY', seen)
        self.assertNotIn('WAYLAND_DISPLAY', seen)
        # An operator's explicit platform still wins, as it does in the harness.
        seen = self.started_server_environment(EGL_PLATFORM='x11')
        self.assertEqual(seen.get('EGL_PLATFORM'), 'x11')


if __name__ == '__main__':
    unittest.main()
