#!/usr/bin/env python3
"""The Fatal census gate (CONTRACT-P6 5.2, exit gate S7).

WHAT IT REFUSES: a `std::abort()` under MG_Remote/ that carries no family word, and a
SessionFail / WireLogFatal call whose string forgot its family.

S7: "a bare abort() added outside Session::Fail -> the census gate goes red". Session::Fail now
EXISTS - it is SessionFail() in FatalFunnel.cpp, and the ~90 scattered aborts route through it,
so a bare std::abort() under MG_Remote/ is now the exception this gate refuses rather than the
rule. Four aborts remain, all inside the two funnels (WireLog.cpp, FatalFunnel.cpp), and both
files are exempt because their callers carry the family word. The property this enforces is the
one every downstream consumer depends on: every death names a FAMILY, in a `Fatal{Word...}`
marker, on a line a grep can find.

That is not as weak as it sounds. a6's census found the vocabulary had drifted to 30 family
words against a 7-value wire FatalCode, and found two aborts carrying no marker at all - so "the
log stays verbatim" was untrue of them and no census could see them. Everything downstream reads
these markers: run_trace_case.cmake reds a split retrace on any `Fatal{` line, the retrace lane's
refusal census counts them by name, and MEASUREMENTS quotes the distinct words. A death with no
word is invisible to all of it.

TWO SANCTIONED SHAPES, both spelled out rather than inferred:

  1. the marker is within kMarkerWindow lines above the abort;
  2. the abort is inside the WireLogFatal funnel (Transport/WireLog.*), whose callers each pass
     a `Fatal{` format string - which this file also checks, because a funnel that accepted an
     unmarked string would launder exactly what rule 1 refuses.

THE BASELINE IS A CEILING ON SILENCE, NOT ON DEATHS. The count of abort sites may grow; the set
of family words may not grow unrecorded. Freezing the total would make every new guard a gate
change, which is how a gate stops being run.
"""
import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCAN_ROOT = ROOT / "MobileGL" / "MG_Remote"
BASELINE = Path(__file__).with_name("fatal_census_baseline.json")

# The funnel's own files. WireLog.cpp's abort is reached only through WireLogFatal, and rule 2
# checks that every CALLER passes a family word - so the death is named by the caller. Both files
# are exempt from rule 2 as well: a function's own declaration and definition match any
# call-shaped regex, and reporting them would be reporting the funnel for being one.
FUNNEL_FILES = {
    "MobileGL/MG_Remote/Transport/WireLog.cpp",
    "MobileGL/MG_Remote/Transport/WireLog.h",
    # P6 dl: Session::Fail's funnel. Its abort is the sanctioned one every SessionFail reaches,
    # exactly as WireLog.cpp's is for WireLogFatal; its own file also defines the SessionFail
    # call, so it is exempt from both rules for the same reason WireLog is.
    "MobileGL/MG_Remote/FatalFunnel.cpp",
    "MobileGL/MG_Remote/FatalFunnel.h",
}

# Twelve lines: long enough for a wrapped MGLOG_F argument list (the longest in the tree runs to
# eight continuation lines), short enough that an unrelated marker further up cannot cover an
# abort it has nothing to do with.
kMarkerWindow = 12

MARKER = re.compile(r"Fatal\{([A-Za-z][A-Za-z0-9]*)")
ABORT = re.compile(r"\bstd::abort\(\)")
WIRE_LOG_FATAL_CALL = re.compile(r"WireLogFatal\s*\(")
# P6 dl: a SessionFail call must carry a Fatal{ word in its string, the same rule
# WireLogFatal has - the enum and the string both name the family, and a call whose string
# forgot it would let the two disagree. Checked everywhere SessionFail is called.
SESSION_FAIL_CALL = re.compile(r"SessionFail\s*\(")


def strip_comments(text):
    """Blank out comments, keeping line numbering and STRING LITERALS intact.

    Both halves matter. Comments have to go because WireLog.h's own prose contains the words
    `std::abort()` while explaining why the stderr echo exists - the first run of this gate
    reported that sentence as an unmarked death. String literals have to STAY, because the family
    markers this file looks for live inside them.

    A character-level pass rather than a regex, because `"http://..."` inside a string is not a
    comment and a regex with no string state says it is.
    """
    out = []
    index = 0
    length = len(text)
    quote = None
    while index < length:
        char = text[index]
        if quote is not None:
            out.append(char)
            if char == "\\" and index + 1 < length:
                out.append(text[index + 1])
                index += 2
                continue
            if char == quote:
                quote = None
            index += 1
            continue
        if char == '"' or char == "'":
            quote = char
            out.append(char)
            index += 1
            continue
        if char == "/" and index + 1 < length and text[index + 1] == "/":
            while index < length and text[index] != "\n":
                index += 1
            continue
        if char == "/" and index + 1 < length and text[index + 1] == "*":
            index += 2
            while index + 1 < length and not (text[index] == "*" and text[index + 1] == "/"):
                # Newlines survive so every reported line number stays the file's own.
                if text[index] == "\n":
                    out.append("\n")
                index += 1
            index += 2
            continue
        out.append(char)
        index += 1
    return "".join(out)


def source_files():
    for path in sorted(SCAN_ROOT.rglob("*")):
        if path.suffix not in (".cpp", ".h"):
            continue
        # A death test's deliberate abort is not a production death.
        if "MG_Test" in path.parts:
            continue
        yield path


def relative(path):
    return path.relative_to(ROOT).as_posix()


def census():
    unmarked = []
    families = set()
    aborts = 0
    unnamed_funnel_calls = []

    for path in source_files():
        raw = path.read_text(encoding="utf-8", errors="replace")
        text = strip_comments(raw)
        lines = text.split("\n")
        rel = relative(path)

        for word in MARKER.findall(text):
            families.add(word)

        if rel not in FUNNEL_FILES:
            # Rule 2: every WireLogFatal / SessionFail call carries a family word in its string.
            # window covers its continuation lines.
            for index, line in enumerate(lines):
                if not (WIRE_LOG_FATAL_CALL.search(line) or SESSION_FAIL_CALL.search(line)):
                    continue
                window = "\n".join(lines[index:index + kMarkerWindow])
                if not MARKER.search(window):
                    unnamed_funnel_calls.append({"file": rel, "line": index + 1,
                                                 "text": line.strip()})

        for index, line in enumerate(lines):
            if not ABORT.search(line):
                continue
            aborts += 1
            if rel in FUNNEL_FILES:
                continue  # rule 2 covers it
            window = "\n".join(lines[max(0, index - kMarkerWindow):index + 1])
            if not MARKER.search(window):
                unmarked.append({"file": rel, "line": index + 1, "text": line.strip()})

    return {
        "abort_sites": aborts,
        "unmarked_aborts": unmarked,
        "unnamed_funnel_calls": unnamed_funnel_calls,
        "families": sorted(families),
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--write-baseline", action="store_true",
                        help="Record today's numbers. Run it when a family is deliberately added.")
    parser.add_argument("--json", action="store_true", help="Print the census and exit 0.")
    args = parser.parse_args()

    result = census()
    if args.json:
        print(json.dumps(result, indent=2))
        return 0

    if args.write_baseline:
        BASELINE.write_text(json.dumps({"abort_sites": result["abort_sites"],
                                        "families": result["families"]}, indent=2) + "\n",
                            encoding="utf-8")
        print(f"fatal census baseline written: {result['abort_sites']} abort sites, "
              f"{len(result['families'])} families")
        return 0

    failures = []
    for entry in result["unmarked_aborts"]:
        failures.append(
            f"{entry['file']}:{entry['line']} aborts with no Fatal{{Family}} marker within "
            f"{kMarkerWindow} lines. Every death under MG_Remote/ must name a family: the split "
            f"retrace reds on `Fatal{{`, the refusal census counts by name, and a death with no "
            f"word is invisible to both.")
    for entry in result["unnamed_funnel_calls"]:
        failures.append(
            f"{entry['file']}:{entry['line']} calls WireLogFatal with no Fatal{{Family}} in its "
            f"format string. The funnel's own abort is exempt from the rule above ONLY because "
            f"its callers carry the word; a call that does not would launder exactly what the "
            f"rule refuses.")

    if BASELINE.is_file():
        baseline = json.loads(BASELINE.read_text(encoding="utf-8"))
        # A NEW FAMILY WORD IS A DELIBERATE ACT and has to be recorded, because the wire's
        # FatalCode has seven values while the internal vocabulary is already five times that -
        # the divergence 5.2's funnel exists to bound. Growth is allowed; unrecorded growth is not.
        added = sorted(set(result["families"]) - set(baseline.get("families", [])))
        if added:
            failures.append(
                "new Fatal family word(s) with no baseline entry: " + ", ".join(added) +
                ". Add them with `python3 scripts/ci/fatal_census.py --write-baseline` in the "
                "same commit that introduces them - the point is that the vocabulary grows on "
                "purpose rather than by accident (CONTRACT-P6 5.2's D2 ruling).")

    print(f"fatal census: {result['abort_sites']} abort sites under MG_Remote/, "
          f"{len(result['families'])} distinct family words, "
          f"{len(result['unmarked_aborts'])} unmarked")
    if failures:
        for message in failures:
            print(f"::error::{message}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
