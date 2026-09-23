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


class VerifySplitArmProofTest(unittest.TestCase):
    """The per-entry arm proof, BOTH SIDES (P7 wave 3, V1 fix round).

    The thing it replaced was a count, and a count cannot be tested for the failure that
    matters - "fifty arms became fifty skips" leaves it unmoved. These four cases are the four
    ways the census can be wrong, and each one has to raise."""

    ARMED = "DirectGLES.VerifySplit.TriangleScenario.DrawsATriangle"
    QUIET = "DirectGLES.VerifySplit.AdvertisedLimitsScenario.EveryGL45CoreMinimumIsMet"
    SKIPPED = "DirectVulkan.VerifySplit.F1WireScenario.SomethingLavapipeCannotDo"

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.logs = self.root / "verify-split-logs"
        self.logs.mkdir()
        self.xml = self.root / "run.xml"
        self.expected = self.root / "expected.txt"
        self.expected.write_text("# the quiet one\n" + self.QUIET + "\n")
        self.write_log(self.ARMED, helper.VERIFY_ARMED_LINE)
        self.write_log(self.QUIET, "no fill here")
        self.write_log(self.SKIPPED, "no fill here either")
        self.results([(self.ARMED, "run", None), (self.QUIET, "run", None),
                      (self.SKIPPED, "notrun", "skipped")])

    def tearDown(self):
        self.temp.cleanup()

    def write_log(self, entry, text):
        (self.logs / (entry + ".client.log")).write_text(text + "\n")

    def results(self, rows):
        suite = ET.Element("testsuite")
        for name, status, kind in rows:
            case = ET.SubElement(suite, "testcase", name=name, status=status)
            if kind:
                ET.SubElement(case, kind)
        ET.ElementTree(suite).write(self.xml)

    def proof(self):
        with contextlib.redirect_stdout(io.StringIO()):
            helper.verify_split_arming(self.logs, self.xml, self.expected)

    def test_armed_skipped_or_named_is_the_whole_rule(self):
        self.proof()

    def test_an_unlisted_quiet_entry_fails(self):
        self.expected.write_text("# nobody\n")
        with self.assertRaisesRegex(ValueError, "never armed"):
            self.proof()

    def test_a_listed_entry_that_armed_fails(self):
        self.write_log(self.QUIET, helper.VERIFY_ARMED_LINE)
        with self.assertRaisesRegex(ValueError, "DID arm"):
            self.proof()

    def test_a_listed_name_with_no_log_fails(self):
        (self.logs / (self.QUIET + ".client.log")).unlink()
        self.results([(self.ARMED, "run", None), (self.SKIPPED, "notrun", "skipped")])
        with self.assertRaisesRegex(ValueError, "match no per-entry client log"):
            self.proof()

    def test_a_log_with_no_result_fails(self):
        self.results([(self.ARMED, "run", None), (self.QUIET, "run", None)])
        with self.assertRaisesRegex(ValueError, "no entry in the JUnit result"):
            self.proof()

    def test_an_empty_log_directory_is_not_a_green(self):
        for path in self.logs.glob("*.client.log"):
            path.unlink()
        with self.assertRaisesRegex(ValueError, "did not take"):
            self.proof()


if __name__ == "__main__":
    unittest.main()
