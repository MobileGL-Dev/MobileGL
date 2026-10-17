#!/usr/bin/env python3
# MobileGL - tools/trace_replay/test_run_trace_case.py
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

"""Canned-log tests of run_trace_case.cmake's split and verify assertions.

Each case runs the script exactly as ctest does (`cmake -D... -P run_trace_case.cmake`) with a
stub replay that copies canned library logs into the output directory. A bad log must turn the
case red with the script's own sentence and leave no split-evidence.txt; a good one must pass and
leave evidence that scripts/ci/retrace_split_evidence.py accepts. No build, GPU or trace needed.
"""
import contextlib
import io
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
SCRIPT = HERE / 'run_trace_case.cmake'
CHECKER = HERE.parents[1] / 'scripts' / 'ci' / 'retrace_split_evidence.py'
sys.path.insert(0, str(CHECKER.parent))
import retrace_split_evidence as checker  # noqa: E402

# The stub replay. It refuses to run when the wire counters disagree with what the leg asked for,
# so removing the script's MOBILEGL_PIPE_STATS export turns every wire case red, and exporting it
# on a leg that did not ask turns that leg red too.
STUB = r'''#!/usr/bin/env python3
import json, os, shutil, sys
args = sys.argv[1:]
out = args[args.index('--output') + 1]
split = os.environ.get('MOBILEGL_TRANSPORT', '') not in ('', 'monolith')
wire = os.environ.get('MGTRACE_WIRE_EVIDENCE', '') not in ('', '0')
stats = os.environ.get('MOBILEGL_PIPE_STATS', '')
if wire and split and (stats != '1' or os.environ.get('MOBILEGL_PIPE_STATS_PERIOD') != '1'):
    sys.stderr.write('stub: MOBILEGL_PIPE_STATS did not reach the replay\n')
    sys.exit(3)
if not wire and stats:
    sys.stderr.write('stub: MOBILEGL_PIPE_STATS reached a leg that did not ask for it\n')
    sys.exit(4)
canned = os.environ.get('STUB_CANNED_DIR', '')
if canned:
    for name in os.listdir(canned):
        shutil.copy(os.path.join(canned, name), os.path.join(out, name))
with open(os.path.join(out, 'retrace.log'), 'w') as log:
    log.write('stub replay\n')
with open(os.path.join(out, 'result.json'), 'w') as result:
    json.dump({'passed': True, 'ssim': 1.0}, result)
'''

PREFIX = '[12:00:00] [Linux mobilegl_trace_/INFO]: '
MARKER = {
    'inproc': 'Config: MOBILEGL_TRANSPORT=inproc - the MGPipe record stream crosses a real ring to an apply thread',
    'spawn': 'Config: MOBILEGL_TRANSPORT=spawn - the MGPipe record stream crosses a real ring to an apply '
             'thread in ANOTHER PROCESS',
}
SPAWN_ARMED = 'MG_Remote client: spawn ARMED - the server role runs in pid 4242, reached at /tmp/x'
TCP_ARMED = 'MG_Remote client: control=tcp data=stream server=127.0.0.1:1 pid=4243 dial=connect'
SENT = ['P65LinkMetrics kind=frame-sent frame=1 records=40 by_op=2=4,57=1,59=30,67=1',
        'P65LinkMetrics kind=frame-sent frame=2 records=31 by_op=57=1,59=28,67=1']
STATS_ON = 'MGPipe stats: counters ON (MOBILEGL_PIPE_STATS), summary every 1 frames'
STATS = ['MGPipe stats: frames=1 window=1 draws=30 draws/f=30.00 acc=12 acc/draw=0.40 bytes/f[a=1]',
         'MGPipe stats: frames=2 window=1 draws=28 draws/f=28.00 acc=10 acc/draw=0.36 bytes/f[a=1]']
SERVER_START = 'MG_Remote server: apply thread started'


def quiet_check(argv):
    """The checker's exit status, with its ::error:: lines kept off a green run's annotations."""
    with contextlib.redirect_stdout(io.StringIO()):
        return checker.main(argv)


def lines(*rows):
    return ''.join(PREFIX + row + '\n' for row in rows)


def good_logs(transport):
    client = [MARKER['spawn' if transport == 'tcp' else transport]]
    if transport == 'spawn':
        client.append(SPAWN_ARMED)
    if transport == 'tcp':
        client.append(TCP_ARMED)
    client += SENT
    server = [SERVER_START, STATS_ON] + ([] if transport == 'tcp' else STATS)
    return {'mobilegl.client.log': lines(*client), 'mobilegl.server.log': lines(*server)}


def squash(text):
    return re.sub(r'\s+', ' ', text)


@unittest.skipIf(os.name == 'nt', 'the stub replay is a POSIX executable script')
class RunTraceCaseTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='run-trace-case-')
        self.root = Path(self.temp.name)
        payload = self.root / 't.trace'
        payload.write_text('not a real trace\n')
        self.archive = self.root / 't.tar.gz'
        with tarfile.open(self.archive, 'w:gz') as tar:
            tar.add(payload, arcname='t.trace')
        self.stub = self.root / 'stub_replay'
        self.stub.write_text(STUB)
        self.stub.chmod(0o755)
        self.out = self.root / 'case'

    def tearDown(self):
        self.temp.cleanup()

    def run_case(self, env, logs, tcp=False):
        canned = self.root / 'canned'
        shutil.rmtree(canned, ignore_errors=True)
        canned.mkdir()
        for name, text in logs.items():
            (canned / name).write_text(text)
        environment = {key: value for key, value in os.environ.items()
                       if not key.startswith(('MOBILEGL_', 'MGTRACE_'))}
        environment.update(env)
        environment['STUB_CANNED_DIR'] = str(canned)
        command = ['cmake', f'-DTRACE_REPLAY_EXE={self.stub}', f'-DMOBILEGL_LIBRARY={self.root}/libMobileGL.so',
                   f'-DTRACE_ARCHIVE={self.archive}', '-DTRACE_FILE=t.trace',
                   f'-DTRACE_GOLDEN={self.root}/golden.png', f'-DTRACE_OUTPUT_DIR={self.out}',
                   f'-DTRACE_ARTIFACT_DIR={self.root}/artifacts', '-DTRACE_BACKEND=DirectGLES',
                   '-DTRACE_CASE_NAME=Canned', '-DTRACE_TARGET_CALL=1', '-DTRACE_WIDTH=1', '-DTRACE_HEIGHT=1']
        if tcp:
            command += ['-DTRACE_IPC_CONTROL=tcp://127.0.0.1:1', '-DTRACE_IPC_TOKEN=x']
        command += ['-P', str(SCRIPT)]
        result = subprocess.run(command, env=environment, capture_output=True, text=True)
        return result.returncode, squash(result.stdout + result.stderr)

    def split_env(self, transport, wire=False):
        env = {'MOBILEGL_TRANSPORT': 'spawn' if transport == 'tcp' else transport,
               'MOBILEGL_IPC_SERVER_PATH': str(self.root / 'libMobileGLServer.so')}
        if wire:
            env['MGTRACE_WIRE_EVIDENCE'] = '1'
        return env

    @property
    def evidence(self):
        return self.out / 'output' / 'split-evidence.txt'

    def expect_red(self, env, logs, sentence, tcp=False):
        rc, output = self.run_case(env, logs, tcp=tcp)
        self.assertNotEqual(rc, 0, output)
        self.assertIn(squash(sentence), output)
        self.assertFalse(self.evidence.exists(), 'a red leg must leave no evidence file')

    def expect_green(self, env, logs, transport, tcp=False, wire=False):
        rc, output = self.run_case(env, logs, tcp=tcp)
        self.assertEqual(rc, 0, output)
        self.assertEqual(quiet_check([str(self.evidence), '--transport', transport, '--case', 'Canned',
                                       '--backend', 'DirectGLES'] + (['--require-wire'] if wire else [])), 0)
        return output

    # Red: each assertion of the transport block, fed the log that must trip it.
    def test_r1_a_library_without_a_transport_writes_no_client_log(self):
        self.expect_red(self.split_env('inproc'), {'mobilegl.log': lines('Config: loaded')},
                        'so there is no evidence the transport ever resolved')

    def test_r2_the_env_dump_line_is_not_the_marker(self):
        logs = {'mobilegl.client.log': lines('Config: Accepted env variable: MOBILEGL_TRANSPORT=inproc'),
                'mobilegl.server.log': lines(SERVER_START)}
        self.expect_red(self.split_env('inproc'), logs, 'never reported resolving it')

    def test_r3_spawn_without_a_separate_server_pid(self):
        logs = good_logs('spawn')
        logs['mobilegl.client.log'] = lines(MARKER['spawn'], *SENT)
        self.expect_red(self.split_env('spawn'), logs, 'spawn ARMED - the server role runs in pid')

    def test_r4_tcp_without_endpoint_and_pid(self):
        logs = good_logs('tcp')
        logs['mobilegl.client.log'] = lines(MARKER['spawn'], *SENT)
        self.expect_red(self.split_env('tcp'), logs, 'TCP session has no endpoint and server pid arm proof',
                        tcp=True)

    def test_r5_no_server_log(self):
        logs = good_logs('inproc')
        del logs['mobilegl.server.log']
        self.expect_red(self.split_env('inproc'), logs, 'but the run wrote no')

    def test_r6_a_server_fatal(self):
        logs = good_logs('inproc')
        logs['mobilegl.server.log'] += lines('MGPipe: Fatal{UnmigratedVerb, "X"}')
        self.expect_red(self.split_env('inproc'), logs, 'MGPipe Fatal(s) under MOBILEGL_TRANSPORT')

    def test_r7_a_floor_line(self):
        logs = good_logs('inproc')
        logs['mobilegl.server.log'] += lines('MGWIRE-FLOOR unsound-serial-complete serial=3')
        self.expect_red(self.split_env('inproc'), logs, 'MGWIRE-FLOOR unsound-serial-complete line(s)')

    def test_r8_no_frame_sent_lines(self):
        logs = good_logs('inproc')
        logs['mobilegl.client.log'] = lines(MARKER['inproc'])
        self.expect_red(self.split_env('inproc', wire=True), logs, 'committed no DrawVbo record to the wire')

    def test_r9_frames_sent_without_a_draw(self):
        logs = good_logs('inproc')
        logs['mobilegl.client.log'] = lines(MARKER['inproc'], 'P65LinkMetrics kind=frame-sent frame=1 '
                                                              'records=2 by_op=57=1,67=1,159=4,590=2')
        self.expect_red(self.split_env('inproc', wire=True), logs, 'committed no DrawVbo record to the wire')

    def test_r10_the_server_drew_nothing(self):
        logs = good_logs('inproc')
        logs['mobilegl.server.log'] = lines(SERVER_START, STATS_ON,
                                            'MGPipe stats: frames=1 window=1 draws=0 draws/f=0.00 acc=0')
        self.expect_red(self.split_env('inproc', wire=True), logs, 'backend executed no draw')

    def test_r11_an_unknown_transport(self):
        env = self.split_env('inproc')
        env['MOBILEGL_TRANSPORT'] = 'unix:/x'
        self.expect_red(env, good_logs('inproc'), 'this runner has no marker for it')

    def test_r12_wire_evidence_without_a_split_transport(self):
        self.expect_red({'MGTRACE_WIRE_EVIDENCE': '1', 'MOBILEGL_TRANSPORT': 'monolith'}, {},
                        'a run with no wire has no wire volume to count')

    def test_r13_the_stub_refuses_counters_on_a_leg_that_did_not_ask(self):
        env = self.split_env('inproc')
        env['MOBILEGL_PIPE_STATS'] = '1'
        self.expect_red(env, good_logs('inproc'), 'stub: MOBILEGL_PIPE_STATS reached a leg that did not ask for it')

    # Green: the same logs as a real leg writes, on every arm.
    def test_g1_inproc_with_wire(self):
        output = self.expect_green(self.split_env('inproc', wire=True), good_logs('inproc'), 'inproc', wire=True)
        self.assertIn('wire_draw_records=58, server_draws=58', output)

    def test_g1b_inproc_marker_only(self):
        self.expect_green(self.split_env('inproc'), good_logs('inproc'), 'inproc')
        values = checker.parse(self.evidence.read_text())
        self.assertEqual((values['wire'], values['wire_draw_records']), ('OFF', '-'))
        self.assertNotEqual(quiet_check([str(self.evidence), '--transport', 'inproc', '--require-wire']), 0)

    def test_g2_spawn_with_wire(self):
        self.expect_green(self.split_env('spawn', wire=True), good_logs('spawn'), 'spawn', wire=True)

    def test_g3_tcp_with_wire(self):
        self.expect_green(self.split_env('tcp', wire=True), good_logs('tcp'), 'spawn', tcp=True, wire=True)
        self.assertEqual(checker.parse(self.evidence.read_text())['server_draws'], 'n/a')

    def test_g4_monolith_leaves_no_evidence(self):
        rc, output = self.run_case({'MOBILEGL_TRANSPORT': 'monolith'}, {'mobilegl.log': lines('Config: loaded')})
        self.assertEqual(rc, 0, output)
        self.assertFalse(self.evidence.exists())
        self.assertNotEqual(quiet_check([str(self.evidence), '--transport', 'inproc']), 0,
                            'a leg whose transport block was skipped must fail the checker')

    # The verify block, which shares the script.
    def test_v1_verify_without_the_armed_line(self):
        self.expect_red({'MOBILEGL_PIPE_VERIFY': '1'}, {'mobilegl.log': lines('Config: loaded')},
                        'never reported "MGPipe: verify armed"')

    def test_v2_verify_with_a_divergence(self):
        logs = {'mobilegl.log': lines('MGPipe: verify armed',
                                      'MGPipe: Fatal{PipeVerifyDiffer, "GetRenderStateParameters@Clear"}')}
        self.expect_red({'MOBILEGL_PIPE_VERIFY': '1'}, logs, 'MGPipe Fatal(s) under MOBILEGL_PIPE_VERIFY')

    def test_v3_verify_green(self):
        rc, output = self.run_case({'MOBILEGL_PIPE_VERIFY': '1'}, {'mobilegl.log': lines('MGPipe: verify armed')})
        self.assertEqual(rc, 0, output)
        self.assertIn('armed, zero divergences', output)


class EvidenceCheckerTests(unittest.TestCase):
    GOOD = ('case=Canned\nbackend=DirectGLES\ntransport=spawn\ncontrol=-\nmarker=m\n'
            'second=spawn ARMED - the server role runs in pid \nserver_log=1\nfatal_lines=0\nfloor_lines=0\n'
            'wire=ON\ndraw_opcode=59\nwire_draw_records=58\nserver_draws=58\n')

    def check(self, text, *args):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'split-evidence.txt'
            if text is not None:
                path.write_text(text)
            return quiet_check([str(path)] + list(args))

    def test_good_file_passes(self):
        self.assertEqual(self.check(self.GOOD, '--transport', 'spawn', '--require-wire'), 0)

    def test_missing_file_fails(self):
        self.assertEqual(self.check(None, '--transport', 'spawn'), 1)

    def test_transport_mismatch_fails(self):
        self.assertEqual(self.check(self.GOOD.replace('transport=spawn', 'transport=inproc'),
                                    '--transport', 'spawn'), 1)

    def test_case_mismatch_fails(self):
        self.assertEqual(self.check(self.GOOD, '--transport', 'spawn', '--case', 'OpenRA'), 1)

    def test_zero_wire_records_fail(self):
        self.assertEqual(self.check(self.GOOD.replace('wire_draw_records=58', 'wire_draw_records=0'),
                                    '--transport', 'spawn'), 1)

    def test_zero_server_draws_fail(self):
        self.assertEqual(self.check(self.GOOD.replace('server_draws=58', 'server_draws=0'),
                                    '--transport', 'spawn'), 1)

    def test_spawn_without_second_sentence_fails(self):
        self.assertEqual(self.check(self.GOOD.replace('second=spawn ARMED - the server role runs in pid ',
                                                      'second=-'), '--transport', 'spawn'), 1)

    def test_a_fatal_count_fails(self):
        self.assertEqual(self.check(self.GOOD.replace('fatal_lines=0', 'fatal_lines=2'),
                                    '--transport', 'spawn'), 1)


if __name__ == '__main__':
    unittest.main()
