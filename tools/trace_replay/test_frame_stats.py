#!/usr/bin/env python3
"""Small fixture checks for frame ID pairing and additive frame attribution."""

import tempfile
import unittest
from pathlib import Path

from frame_stats import summarize


class FrameStatsTest(unittest.TestCase):
    def logs(self, root, *, missing_server=False, overlapping=False):
        client = root / "client.log"
        server = root / "server.log"
        client.write_text("\n".join(
            f"P65LinkMetrics kind=frame frame={n} wall_ns={wall} "
            f"client_thread_cpu_ns={cpu} transport_wait_ns={wait}"
            for n, wall, cpu, wait in ((1, 10000000, 2000000, 1000000),
                                       (2, 20000000, 5000000, 4000000),
                                       (3, 40000000, 10000000, 7000000))) + "\n")
        server.write_text("\n".join(
            f"P65ServerMetrics frame={n} valid=1 wall_ns={wall} "
            f"apply_thread_cpu_ns={cpu}"
            for n, wall, cpu in ((2, 18000000, 17000000 if overlapping else 3000000),
                                 (3, 35000000, 31000000 if overlapping else 6000000))
            if not missing_server or n != 3) + "\n")
        return client, server

    def test_paired_percentiles_and_shares(self):
        with tempfile.TemporaryDirectory() as directory:
            result = summarize(*self.logs(Path(directory)), warmup=1)
        self.assertEqual((result["first_frame"], result["last_frame"], result["frames"]),
                         (2, 3, 2))
        self.assertEqual(result["stats"]["transport_wait_ms"],
                         {"avg": 5.5, "p50": 4.0, "p99": 7.0})
        self.assertEqual(result["share_percent_of_client_present"]["transport_wait_ms"],
                         100 * 11000000 / 60000000)

    def test_missing_server_frame_is_error(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError, "missing matched frames"):
                summarize(*self.logs(Path(directory), missing_server=True), warmup=1)

    def test_parallel_cpu_is_allowed(self):
        with tempfile.TemporaryDirectory() as directory:
            result = summarize(*self.logs(Path(directory), overlapping=True), warmup=1)
        self.assertGreater(sum(result["share_percent_of_client_present"].values()), 100)


if __name__ == "__main__":
    unittest.main()
