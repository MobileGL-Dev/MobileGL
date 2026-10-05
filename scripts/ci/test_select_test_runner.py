#!/usr/bin/env python3
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import urllib.error

spec = importlib.util.spec_from_file_location("selector", Path(__file__).with_name("select_test_runner.py"))
selector = importlib.util.module_from_spec(spec)
spec.loader.exec_module(selector)


def runner(status="online", busy=False):
    return {"name": selector.NAME, "status": status, "busy": busy,
            "labels": [{"name": label} for label in selector.LABELS]}


class SelectRunnerTest(unittest.TestCase):
    def test_online_busy_and_idle_both_use_preferred(self):
        for busy in (False, True):
            labels, pool, _ = selector.choose([runner(busy=busy)])
            self.assertEqual(labels, selector.LABELS)
            self.assertEqual(pool, "arch-wsl")

    def test_offline_or_absent_falls_back(self):
        for rows in ([], [runner("offline")]):
            self.assertEqual(selector.choose(rows)[:2], (["ubuntu-latest"], "github"))

    def test_wrong_runner_or_missing_labels_cannot_queue_forever(self):
        wrong = runner()
        wrong["name"] = "another-machine"
        missing = runner()
        missing["labels"] = missing["labels"][:-1]
        for rows in ([wrong], [missing]):
            self.assertEqual(selector.choose(rows)[1], "github")

    def test_inventory_reads_later_pages(self):
        pages = [io.StringIO(json.dumps({"runners": [{}] * 100})),
                 io.StringIO(json.dumps({"runners": [runner()]}))]
        with patch.object(selector.urllib.request, "urlopen", side_effect=pages) as request:
            rows = selector.runner_inventory("owner/repo", "not-a-real-token")
        self.assertEqual(len(rows), 101)
        self.assertTrue(request.call_args.args[0].full_url.endswith("page=2"))

    def test_auth_failure_does_not_masquerade_as_offline(self):
        for error in (ValueError("missing token"), urllib.error.HTTPError("url", 403, "Forbidden", {}, None)):
            with patch.dict(os.environ, {"GITHUB_REPOSITORY": "owner/repo"}), \
                 patch.object(selector, "runner_inventory", side_effect=error), \
                 contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(selector.main(), 1)

    def test_outputs_are_valid_runs_on_json(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / "outputs"
            with patch.dict(os.environ, {"GITHUB_REPOSITORY": "owner/repo", "GITHUB_OUTPUT": str(path)}), \
                 patch.object(selector, "runner_inventory", return_value=[runner()]), \
                 contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(selector.main(), 0)
            values = dict(line.split("=", 1) for line in path.read_text().splitlines())
            self.assertEqual(json.loads(values["runner"]), selector.LABELS)


if __name__ == "__main__":
    unittest.main()
