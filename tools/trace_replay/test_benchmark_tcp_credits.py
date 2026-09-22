#!/usr/bin/env python3
"""Offline controls for selecting truthful TCP benchmark windows."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import benchmark_tcp_credits as benchmark
from benchmark_tcp_credits import validate_measurement


class BenchmarkEvidenceTest(unittest.TestCase):
    endpoint = 'tcp://127.0.0.1:40613'

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.work = Path(self.temporary.name)
        self.client = ['run-ahead ARMED', 'paced by a present credit of 1',
                       'control=tcp data=stream server=127.0.0.1:40613']
        self.server = []
        for frame in range(1, 4):
            self.client.append('P65LinkMetrics kind=frame frame=' + str(frame) +
                               ' wait_replies=2 rtt_samples=1 rtt_mean_us=8 stage_bytes=1024'
                               ' wall_ns=1000000 client_thread_cpu_ns=500000 rtt_hist_us_pow2=' +
                               ','.join('1' if bucket == 3 else '0' for bucket in range(32)))
            self.server.append('P65ServerMetrics valid=1 frame=' + str(frame) +
                               ' wall_ns=1000000 apply_thread_cpu_ns=250000')

    def validate(self, credit=1, total=3):
        (self.work / 'benchmark.json').write_text(json.dumps({'totalFrames': total}))
        (self.work / 'mobilegl.client.log').write_text('\n'.join(self.client))
        (self.work / 'mobilegl.server.log').write_text('\n'.join(self.server))
        return validate_measurement(self.work, self.endpoint, credit, 2)

    def test_valid_matching_tail(self):
        result = self.validate()
        self.assertEqual((result['first_frame'], result['last_frame']), (2, 3))
        self.assertEqual(result['wait_replies_per_frame'], 2)
        self.assertEqual(result['rtt_mean_us'], 8)
        self.assertEqual(result['fps'], 1000)

    def test_server_marker_cannot_arm_client(self):
        self.client.remove('run-ahead ARMED')
        self.server.append('run-ahead ARMED')
        with self.assertRaisesRegex(ValueError, 'requested armed credit'):
            self.validate()

    def test_demotion_or_lockstep_rejected(self):
        for marker in ('running lockstep', 'run-ahead DISARMED'):
            with self.subTest(marker=marker):
                self.client.append(marker)
                with self.assertRaisesRegex(ValueError, 'requested armed credit'):
                    self.validate()
                self.client.pop()

    def test_credit_proof_is_exact(self):
        self.client[1] = 'paced by a present credit of 10'
        with self.assertRaisesRegex(ValueError, 'requested armed credit'):
            self.validate()

    def test_missing_server_tail_frame_rejected(self):
        self.server.pop()
        with self.assertRaisesRegex(ValueError, 'matching complete'):
            self.validate()

    def test_duplicate_client_frame_rejected(self):
        self.client.append(self.client[-1])
        with self.assertRaisesRegex(ValueError, 'duplicate frame'):
            self.validate()

    def test_partial_metrics_rejected(self):
        with self.assertRaisesRegex(ValueError, 'benchmark and frame metrics differ'):
            self.validate(total=4)

    def test_missing_cpu_is_not_zero(self):
        self.server[-1] = self.server[-1].replace('apply_thread_cpu_ns=250000', 'apply_thread_cpu_ns=0')
        with self.assertRaisesRegex(ValueError, 'nonpositive CPU'):
            self.validate()

    def test_resume_requires_unchanged_artifacts_and_library(self):
        valid = self.validate()
        inputs = self.work / 'inputs' / 'OpenRA'
        inputs.mkdir(parents=True)
        (inputs / 'openra.trace').write_bytes(b'trace fixture')
        runner, library = self.work / 'runner', self.work / 'library'
        runner.write_bytes(b'runner fixture')
        library.write_bytes(b'library fixture')
        output = self.work / 'results'
        argv = ['benchmark_tcp_credits.py', '--runner', str(runner), '--library', str(library),
                '--inputs', str(inputs.parent), '--out', str(output), '--endpoint', self.endpoint,
                '--case', 'OpenRA', '--credit', '1', '--serial', 'offline-fixture']

        def completed_measurement(args, case, credit, work):
            work.mkdir(parents=True)
            for filename in ('benchmark.json', 'mobilegl.client.log', 'mobilegl.server.log'):
                (work / filename).write_bytes((self.work / filename).read_bytes())
            return {**valid, 'status': 'passed', 'output': str(work)}

        with mock.patch.object(benchmark, 'measure', side_effect=completed_measurement) as measure:
            with mock.patch('sys.argv', argv), mock.patch('builtins.print'):
                self.assertEqual(benchmark.main(), 0)
            with mock.patch('sys.argv', [*argv, '--resume']), mock.patch('builtins.print'):
                self.assertEqual(benchmark.main(), 0)
            self.assertEqual(measure.call_count, 1)
            log = next(output.rglob('mobilegl.client.log'))
            log.write_text('tampered')
            with mock.patch('sys.argv', [*argv, '--resume']), mock.patch('builtins.print'):
                self.assertEqual(benchmark.main(), 0)
            self.assertEqual(measure.call_count, 2)
            library.write_bytes(b'changed library')
            with mock.patch('sys.argv', [*argv, '--resume']), mock.patch('builtins.print'):
                self.assertEqual(benchmark.main(), 0)
            self.assertEqual(measure.call_count, 3)


if __name__ == '__main__':
    unittest.main()
