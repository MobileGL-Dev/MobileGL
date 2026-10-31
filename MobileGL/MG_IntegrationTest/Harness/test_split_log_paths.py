#!/usr/bin/env python3
# MobileGL - MobileGL/MG_IntegrationTest/Harness/test_split_log_paths.py
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

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
        for name, reason in helper.DUALBLOCK_ALLOWED_SKIPS.items():
            with self.subTest(name=name):
                self.assertTrue(reason.strip(), "an allowed skip needs a non-empty reason")
                self.document = self.discovery([name])
                self.results([(name, "notrun", "skipped")])
                with self.assertRaisesRegex(ValueError, "unexpected skip"):
                    self.census()
                tree = ET.parse(self.xml)
                ET.SubElement(tree.getroot().find("testcase"), "system-out").text = reason
                tree.write(self.xml)
                self.census()

    def test_an_allowed_entry_skipping_for_another_reason_is_refused(self):
        # The Glsl420 row names the DECLINE; a program that stopped building skips too, with a
        # different sentence, and must not ride the same row. The accepted half is asserted
        # FIRST: without it this case would pass on a helper that does not list the entry at all
        # (CI's dual-block census went red on exactly that, run 35785685692 onward).
        name = "DirectGLES.Split.Glsl420DeclarationScenario.AnArrayOfSamplerArraysIsHonoredOrDeclinedCleanly"
        self.assertIn(name, helper.DUALBLOCK_ALLOWED_SKIPS)
        for reason, accepted in [
                ("the frontend's binding-qualifier seeding does not walk an array of arrays, so "
                 "DirectGLES samples unit 0 for every element; the locations asserted above are "
                 "the half of this case it can answer", True),
                ("the frontend does not build an array of sampler arrays: link error", False)]:
            with self.subTest(accepted=accepted):
                self.document = self.discovery([name])
                self.results([(name, "notrun", "skipped")])
                tree = ET.parse(self.xml)
                ET.SubElement(tree.getroot().find("testcase"), "system-out").text = "Skipped\n" + reason
                tree.write(self.xml)
                if accepted:
                    self.census()
                else:
                    with self.assertRaisesRegex(ValueError, "unexpected skip"):
                        self.census()

    def test_all_unexpected_skips_are_reported_together(self):
        self.results([(A, "notrun", "skipped"), (B, "notrun", "skipped")])
        with self.assertRaises(ValueError) as error:
            self.census()
        self.assertIn(A, str(error.exception))
        self.assertIn(B, str(error.exception))

    def test_capability_and_known_limitation_reasons_are_not_interchangeable(self):
        for table in (helper.DUALBLOCK_CAPABILITY_SKIPS, helper.DUALBLOCK_KNOWN_LIMITATIONS):
            name, reason = next(iter(table.items()))
            for actual, accepted in ((reason, True), ("preflight failed: no EGL display", False)):
                with self.subTest(name=name, accepted=accepted):
                    self.document = self.discovery([name])
                    self.results([(name, "notrun", "skipped")])
                    tree = ET.parse(self.xml)
                    ET.SubElement(tree.getroot().find("testcase"), "system-out").text = actual
                    tree.write(self.xml)
                    if accepted:
                        self.census()
                    else:
                        with self.assertRaisesRegex(ValueError, "unexpected skip"):
                            self.census()

    def test_specialized_control_missing_or_skipped_is_rejected(self):
        names = sorted(helper.specialized_required())
        document = self.discovery(names)
        helper.select_specialized(document)
        with self.assertRaisesRegex(ValueError, "missing or duplicated"):
            helper.select_specialized({"tests": document["tests"][1:]})
        self.results([(n, "run", None) for n in names])
        with contextlib.redirect_stdout(io.StringIO()):
            helper.require_green(document, self.xml, names)
        self.results([(n, "notrun", "skipped") if i == 0 else (n, "run", None)
                      for i, n in enumerate(names)])
        with self.assertRaisesRegex(ValueError, "unexpected skip"):
            helper.require_green(document, self.xml, names)

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


class IpcConfigProofTest(unittest.TestCase):
    """ipc-config reads each passed entry's own client log for the knobs the run resolved."""

    LINE = "Config: IPC ring=8MiB run-ahead={ra} present-credit=2 strict={st} audit=0 role-split-state={rs} surface=x"

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.xml = self.root / "run.xml"

    def tearDown(self):
        self.temp.cleanup()

    def lane(self, rows):
        """rows: (name, status, pins, log line or None)."""
        tests, suite = [], ET.Element("testsuite")
        for i, (name, status, pins, line) in enumerate(rows):
            base = self.root / f"{i}.log"
            if line is not None:
                (self.root / f"{i}.client.log").write_text("Config: MOBILEGL_TRANSPORT=inproc\n" + line + "\n")
            env = ["MOBILEGL_LOG_FILE_PATH=" + str(base)] + pins
            tests.append({"name": name, "properties": [{"name": "ENVIRONMENT", "value": env}]})
            case = ET.SubElement(suite, "testcase", name=name, status="run")
            if status == "skip":
                ET.SubElement(case, "skipped")
            if status == "fail":
                ET.SubElement(case, "failure")
        ET.ElementTree(suite).write(self.xml)
        return {"tests": tests}

    def check(self, rows, expected=None, assumed=("role-split-state", "run-ahead")):
        expected = expected or {"strict": "0", "role-split-state": "1", "run-ahead": "1"}
        with contextlib.redirect_stdout(io.StringIO()):
            return helper.ipc_config(self.lane(rows), self.xml, expected, set(assumed))[0]

    def inproc(self, ra="1", st="0", rs="1"):
        return ["MOBILEGL_TRANSPORT=inproc"], self.LINE.format(ra=ra, st=st, rs=rs)

    def test_the_defaults_are_proven(self):
        pins, line = self.inproc()
        self.assertEqual(self.check([("A", "pass", pins, line), ("B", "skip", pins, None)]), 0)

    def test_a_changed_default_is_exit_three(self):
        pins, line = self.inproc(rs="0")
        self.assertEqual(self.check([("A", "pass", pins, line)]), 3)

    def test_an_unassumed_key_that_differs_is_red(self):
        pins, line = self.inproc(st="1")
        self.assertEqual(self.check([("A", "pass", pins, line)]), 1)

    def test_a_passed_inproc_entry_without_the_line_is_red(self):
        pins, line = self.inproc()
        self.assertEqual(self.check([("A", "pass", pins, line), ("B", "pass", pins, None)]), 1)

    def test_no_evidence_is_red(self):
        pins, _ = self.inproc()
        self.assertEqual(self.check([("A", "skip", pins, None)]), 1)

    def test_an_entry_pin_overrides_the_expectation(self):
        pins, line = self.inproc(ra="0")
        self.assertEqual(self.check([("A", "pass", pins + ["MOBILEGL_IPC_RUN_AHEAD=0"], line)]), 0)
        self.assertEqual(self.check([("A", "pass", pins + ["MOBILEGL_IPC_RUN_AHEAD=1"], line)]), 1,
                         "a pinned value the run did not honor is red, not a changed default")

    def test_the_value_is_matched_whole(self):
        pins, line = self.inproc(ra="10")
        self.assertEqual(self.check([("A", "pass", pins, line)]), 3)

    def test_monolith_pinned_entries_are_left_out(self):
        pins, line = self.inproc()
        rows = [("A", "pass", pins, line), ("M", "pass", ["MOBILEGL_TRANSPORT=monolith"], None)]
        self.assertEqual(self.check(rows), 0)


if __name__ == "__main__":
    unittest.main()
