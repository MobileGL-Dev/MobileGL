#!/usr/bin/env python3
"""Regression controls for complete, non-vacuous P5f result accounting."""
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import tempfile
import unittest
import xml.etree.ElementTree as ET

module_path = Path(os.environ.get("P5F_TEST_HELPER", Path(__file__).with_name("split_log_paths.py")))
spec = importlib.util.spec_from_file_location("tested_split_logs", module_path)
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)
A = "DirectGLES.Split.P5fRsp.F1WireScenario.EachWireFrameHasZeroResidualPulls"
B = "DirectVulkan.Split.P5fRsp.F1WireScenario.EachWireFrameHasZeroResidualPulls"


class ResultAccountingTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.expected = self.root / "expected.txt"
        self.expected.write_text("# empty P5f census\n")
        self.xml = self.root / "run.xml"
        self.document = self.discovery([A, B])
        self.results([(A, "run", None), (B, "run", None)])

    def tearDown(self):
        self.temp.cleanup()

    def discovery(self, names):
        # P6: the ENV path is a BASE NAME and the library writes <base>.client.log /
        # <base>.server.log. The fixture writes the CLIENT role file - what a real run leaves on
        # disk - while the XML carries the base, exactly as the lane does.
        tests = []
        for i, name in enumerate(names):
            base = self.root / (str(i) + ".log")
            (self.root / (str(i) + ".client.log")).write_text("")
            tests.append({"name": name, "properties": [{"name": "ENVIRONMENT", "value": [
                "MOBILEGL_LOG_FILE_PATH=" + str(base)]}]})
        return {"tests": tests}

    def results(self, rows):
        suite = ET.Element("testsuite")
        for name, status, kind in rows:
            case = ET.SubElement(suite, "testcase", name=name, status=status)
            if kind:
                ET.SubElement(case, kind)
        ET.ElementTree(suite).write(self.xml)

    def census(self):
        with contextlib.redirect_stdout(io.StringIO()):
            helper.expect_fatal(self.document, self.xml, self.expected)

    def test_complete_green_census(self):
        self.census()

    def test_missing_junit_entry(self):
        self.results([(A, "run", None)])
        with self.assertRaisesRegex(ValueError, "missing"):
            self.census()

    def test_empty_discovery(self):
        self.document = {"tests": []}
        self.results([])
        with self.assertRaisesRegex(ValueError, "empty"):
            self.census()

    def test_extra_and_duplicate_results(self):
        for rows in [[(A, "run", None), (B, "run", None), ("extra", "run", None)],
                     [(A, "run", None), (B, "run", None), (B, "run", None)]]:
            self.results(rows)
            with self.assertRaises(ValueError):
                self.census()

    def test_notrun_or_disabled_is_not_pass(self):
        for status in ["notrun", "disabled"]:
            self.results([(A, "run", None), (B, status, None)])
            with self.assertRaisesRegex(ValueError, "did not execute"):
                self.census()

    def test_rsp_skip_is_forbidden(self):
        self.results([(A, "run", None), (B, "notrun", "skipped")])
        with self.assertRaisesRegex(ValueError, "unexpected skip"):
            self.census()

    def test_known_skip_requires_its_exact_reason(self):
        name, reason = next(iter(helper.DUALBLOCK_ALLOWED_SKIPS.items()))
        self.document = self.discovery([name])
        self.results([(name, "notrun", "skipped")])
        with self.assertRaisesRegex(ValueError, "unexpected skip"):
            self.census()
        tree = ET.parse(self.xml)
        ET.SubElement(tree.getroot().find("testcase"), "system-out").text = reason
        tree.write(self.xml)
        self.census()

    def test_admitted_or_fatal_on_passed_case_is_not_hidden(self):
        for marker in ['Admitted{UnmigratedPipeInput, "GetProgramObject@Clear"}',
                       'Fatal{UnmigratedPipeInput, "GetProgramObject@Clear"}']:
            (self.root / "0.client.log").write_text(marker)
            with self.assertRaises(ValueError):
                self.census()

    def test_required_pair_must_be_exact_and_both_pass(self):
        with contextlib.redirect_stdout(io.StringIO()):
            helper.require_green(self.document, self.xml, [A, B])
        for row in [(B, "notrun", "skipped"), (B, "fail", "failure")]:
            self.results([(A, "run", None), row])
            with self.assertRaises(ValueError):
                helper.require_green(self.document, self.xml, [A, B])
        self.document = self.discovery([A])
        self.results([(A, "run", None)])
        with self.assertRaisesRegex(ValueError, "entries differ"):
            helper.require_green(self.document, self.xml, [A, B])


if __name__ == "__main__":
    unittest.main()
