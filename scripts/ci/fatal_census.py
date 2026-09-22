#!/usr/bin/env python3
"""The Fatal census gate (CONTRACT-P6 5.2, exit gate S7; perimeter widened by P7 wave 0).

WHAT IT REFUSES: a `std::abort()` inside the SPLIT SERVER IMAGE that carries no family word, a
SessionFail / WireLogFatal call whose string forgot its family, a family word with no row in
FatalFamilies.def, a `Refuse{Word}` that is not a value of the wire RefuseCode, and one more
abort site than the baseline records.

S7: "a bare abort() added outside Session::Fail -> the census gate goes red". Session::Fail
EXISTS - it is SessionFail() in FatalFunnel.cpp - so a bare std::abort() is the exception this
gate refuses rather than the rule. The property it enforces is the one every downstream consumer
depends on: every death names a FAMILY, in a `Fatal{Word...}` marker, on a line a grep can find.

That is not as weak as it sounds. a6's census found the vocabulary had drifted to 30 family words
against a 7-value wire FatalCode, and found two aborts carrying no marker at all - so "the log
stays verbatim" was untrue of them and no census could see them. Everything downstream reads these
markers: run_trace_case.cmake reds a split retrace on any `Fatal{` line, the retrace lane's refusal
census counts them by name, and MEASUREMENTS quotes the distinct words. A death with no word is
invisible to all of it.

THE PERIMETER IS THE SERVER IMAGE, NOT `MG_Remote/` ALONE (P7 wave 0, plan section 1.1 row
"gate: Session::Fail site census"). CONTRACT-P6 §12.1 settles what the server image is: "P6's
server image is the whole libMobileGL.so", one flat SOURCE_FILES list feeding both roles. Until
this widening the gate scanned `MG_Remote/` and saw FOUR abort sites; the server process was
already executing seventy-nine more - 15 in DirectGLES.cpp, 12 in Managers.cpp, 11 in
VkBufferManager.cpp, 7 in PipeRoute.cpp - and every one of them was outside the gate. SCAN_ROOTS
below is the set of roots the split server actually runs code from; `.inc` joins `.cpp`/`.h`
because the Magma wire arms live in Renderer/Wire*.inc and nothing else compiles them.

TEST TREES ARE OUT. A death test's deliberate abort is not a production death, and the benchmark
and integration harnesses drive the library rather than being it.

THE 83 SITES OF THE WIDENED PERIMETER, TRIAGED. Every one was read; the triage is the argument
for why the gate can be green with eighty-three deaths in it, and the two tables below are the
only places an unnamed one is tolerated.

  (i)   INSIDE A SANCTIONED FUNNEL - 4 sites. The abort is reached only through a function whose
        CALLERS carry the family word, so the death is named by the caller. FUNNEL_FILES exempts
        the two whole files that are nothing but funnel; FUNNEL_SITES exempts the individual
        funnels that live inside a file with ordinary named deaths around them.
  (ii)  NAMED, AND OWNED BY A Ph / P7 ROW - 20 sites. The Magma wire arms (WireFramebuffer.inc,
        WireDraw.inc, UniformManager.cpp, VulkanRenderer.cpp) hold the 15 `@P7` refusals due for
        retirement in P7 wave 2; StagedShadow.h / StagedTextureStore.h and the D11 rows are Ph's.
        They carry their family word today, which is the whole point of widening the perimeter:
        the census can now SEE them, and the ratchet below means retiring one is visible.
  (iii) NAMED, BUT NOT SESSION-SCOPED - the remainder. Startup and role self-checks that predate
        the split (Init.cpp's ConsumerMaskLie pair, MagmaPipeArms.h's PipeLegacyMemosDisabled,
        DirectGLES.cpp's UnnamedIdentity, the PipeVerify knob parser) plus the backend-internal
        resource failures in Managers.cpp / VkBufferManager.cpp. They die before or beside a
        session rather than inside one, so routing them through SessionFail would publish a
        SessionFault to a peer that does not exist yet. They are named, which is what this gate
        asks of them; converting them to latches is Ph's fuzz arm 2, not wave 0's.

THE FOUR RULES, spelled out rather than inferred:

  1. an abort's `Fatal{Word` marker is within kMarkerWindow lines above it, OR the abort is
     exempted by FUNNEL_FILES / FUNNEL_SITES;
  2. every WireLogFatal / SessionFail call carries a `Fatal{` in its format string - a funnel that
     accepted an unmarked string would launder exactly what rule 1 refuses;
  3. every family word found in the perimeter has a row in FatalFamilies.def. a6's finding was a
     30-word vocabulary against a 7-value wire enum; the .def is the projection that bounds the
     divergence, and a word with no row projects onto nothing at all;
  4. every `Refuse{Word}` names a value of protocol.fbs's RefuseCode enum. This is rule 3 for the
     handshake's vocabulary, and it exists because `Refuse{AuthenticationRequired}` was in the
     tree for a whole phase naming an enumerator that has never existed (PH-7 (2)).

THE BASELINE IS A DOWNWARD RATCHET ON ABORT SITES AND AN UPWARD ONE ON WORDS. More abort sites
than the baseline records is a failure that names the file(s) that grew; fewer is the direction
the P7 `@P7` retirements move and only asks to be re-baselined in the same commit. The family set
may grow, but never unrecorded: the wire's FatalCode has seven values while the internal
vocabulary is six times that, and the divergence is the thing 5.2's funnel exists to bound.
"""
import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# CONTRACT-P6 §12.1's server image, root by root. Not "everything under MobileGL/": MG_Impl's GL
# entry points and MG_State's object tables are the CLIENT's halves except for the two subtrees
# named here, which the server's applier and staging paths run directly.
SCAN_ROOTS = (
    ROOT / "MobileGL" / "MG_Remote",
    ROOT / "MobileGL" / "MG_Pipe",
    ROOT / "MobileGL" / "MG_Backend",
    ROOT / "MobileGL" / "MG_Impl" / "Pipe",
    ROOT / "MobileGL" / "MG_State",
)
# A death test's deliberate abort is not a production death; the benchmark and integration
# harnesses drive the library rather than being it.
EXCLUDED_PARTS = ("MG_Test", "MG_Benchmark", "MG_IntegrationTest")
# `.inc` is not decoration: the Magma wire arms and the generated wire records are .inc files
# included into a .cpp, and 20 of the 83 sites - every one of the `@P7` refusals among them -
# are reachable only through one.
SOURCE_SUFFIXES = (".cpp", ".h", ".inc")

BASELINE = Path(__file__).with_name("fatal_census_baseline.json")
FAMILIES_DEF = ROOT / "MobileGL" / "MG_Remote" / "FatalFamilies.def"
PROTOCOL_FBS = ROOT / "MobileGL" / "MG_Remote" / "Protocol" / "protocol.fbs"

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

# THE ALLOW-LIST FOR FUNNELS THAT DO NOT GET A WHOLE FILE TO THEMSELVES (P7 wave 0). An unmarked
# abort here is sanctioned only if one of its file's anchors appears in the same kMarkerWindow the
# marker search uses - so the exemption follows the funnel if it moves and expires if it is
# deleted, and an unrelated unmarked abort added elsewhere in the same file is still refused. One
# line of reason per entry, argued here rather than in a pull request nobody rereads.
FUNNEL_SITES = {
    "MobileGL/MG_Pipe/PipeApply.cpp": (
        ("#define MGP_TRIP_WIRE_REPORT",
         "the poison/verify trip-wire funnel: its 30-odd callers each pass MGP_TRIP_WIRE_TAG(name), "
         "which IS the `Fatal{name}` marker, so the death is named by the caller"),
    ),
    "MobileGL/MG_Pipe/generated/PipeWire.inc": (
        ("MGPipeWireProtocolFatal",
         "the generated bounds gate's one death, reached only through MGP_WIRE_CHECK_BOUNDS - but "
         "its line carries NO `Fatal{` marker at all (scripts/gen_pipe.py:982), so the retrace "
         "refusal census cannot name it; a generator fix owed to Ph, recorded here meanwhile"),
    ),
}

# Twelve lines: long enough for a wrapped MGLOG_F argument list (the longest in the tree runs to
# eight continuation lines), short enough that an unrelated marker further up cannot cover an
# abort it has nothing to do with.
kMarkerWindow = 12

MARKER = re.compile(r"Fatal\{([A-Za-z][A-Za-z0-9]*)")
REFUSAL = re.compile(r"Refuse\{([A-Za-z][A-Za-z0-9]*)")
ABORT = re.compile(r"\bstd::abort\(\)")
WIRE_LOG_FATAL_CALL = re.compile(r"WireLogFatal\s*\(")
# P6 dl: a SessionFail call must carry a Fatal{ word in its string, the same rule
# WireLogFatal has - the enum and the string both name the family, and a call whose string
# forgot it would let the two disagree. Checked everywhere SessionFail is called.
SESSION_FAIL_CALL = re.compile(r"SessionFail\s*\(")
FAMILY_ROW = re.compile(r"^\s*X\(([A-Za-z][A-Za-z0-9]*)\s*,", re.MULTILINE)
REFUSE_ENUM = re.compile(r"enum\s+RefuseCode\s*:[^{]*\{(.*?)\}", re.DOTALL)
ENUM_VALUE = re.compile(r"^\s*([A-Za-z][A-Za-z0-9]*)\s*=", re.MULTILINE)

# Two client-side declines that borrow the Refuse vocabulary for a refusal that never becomes a
# wire Refuse frame, so no peer ever reads the word and no RefuseCode can carry it. Listed rather
# than renamed: inventing enumerators for them would move `wireFingerprint`, which is a protocol
# change and not a wave-0 instrument's to make. Recorded for the integrator.
LOCAL_REFUSAL_WORDS = {
    "ProtocolMismatch":
        "ClientSession.cpp:700/:718 - the CLIENT refusing a control/data transport pair locally, "
        "before any Hello is sent; there is no peer to send a Refuse frame to",
    "InitialCapsSnapshot":
        "ClientSession.cpp:1292/:1302 - the CLIENT refusing a Welcome that carried no initial "
        "capabilities; the handshake has already been answered, so the decline is local",
}


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
    seen = set()
    for root in SCAN_ROOTS:
        for path in sorted(root.rglob("*")):
            if path.suffix not in SOURCE_SUFFIXES:
                continue
            if any(part in EXCLUDED_PARTS for part in path.parts):
                continue
            if path in seen:
                continue
            seen.add(path)
            yield path


def relative(path):
    return path.relative_to(ROOT).as_posix()


def declared_families():
    """The family words FatalFamilies.def has a row for - rule 3's right-hand side."""
    return set(FAMILY_ROW.findall(FAMILIES_DEF.read_text(encoding="utf-8")))


def declared_refusals():
    """The RefuseCode enumerators protocol.fbs defines - rule 4's right-hand side."""
    block = REFUSE_ENUM.search(PROTOCOL_FBS.read_text(encoding="utf-8"))
    if block is None:
        return set()
    return set(ENUM_VALUE.findall(block.group(1)))


def sanctioned(rel, window):
    """Is this unmarked abort inside a funnel the tables above sanction?"""
    if rel in FUNNEL_FILES:
        return True
    return any(anchor in window for anchor, _reason in FUNNEL_SITES.get(rel, ()))


def census():
    unmarked = []
    families = set()
    refusals = {}
    sites = []
    unnamed_funnel_calls = []

    for path in source_files():
        raw = path.read_text(encoding="utf-8", errors="replace")
        text = strip_comments(raw)
        lines = text.split("\n")
        rel = relative(path)

        for word in MARKER.findall(text):
            families.add(word)
        for index, line in enumerate(lines):
            for word in REFUSAL.findall(line):
                refusals.setdefault(word, []).append(f"{rel}:{index + 1}")

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
            sites.append({"file": rel, "line": index + 1})
            window = "\n".join(lines[max(0, index - kMarkerWindow):index + 1])
            if sanctioned(rel, window):
                continue  # rule 2, or an argued funnel entry, covers it
            if not MARKER.search(window):
                unmarked.append({"file": rel, "line": index + 1, "text": line.strip()})

    by_file = {}
    for site in sites:
        by_file[site["file"]] = by_file.get(site["file"], 0) + 1

    return {
        "abort_sites": len(sites),
        "abort_sites_by_file": dict(sorted(by_file.items())),
        "abort_site_lines": sites,
        "unmarked_aborts": unmarked,
        "unnamed_funnel_calls": unnamed_funnel_calls,
        "families": sorted(families),
        "refusal_words": dict(sorted(refusals.items())),
    }


def ratchet_aborts(result, baseline, failures, notes):
    """The DOWNWARD ratchet on abort sites (P7 wave 0).

    More sites than the baseline records is a failure that NAMES the file that grew and prints its
    sites, because "83 became 84" is not actionable and "VkBufferManager.cpp went from 11 to 12,
    here they are" is. Fewer is the direction `@P7` retirement moves the number, so it asks for a
    re-baseline in the same commit rather than blocking one.
    """
    recorded = baseline.get("abort_sites")
    if recorded is None:
        return
    current = result["abort_sites"]
    if current == recorded:
        return
    if current < recorded:
        notes.append(
            f"abort sites fell from {recorded} to {current} - re-baseline in this commit with "
            f"`python3 scripts/ci/fatal_census.py --write-baseline`, so the ratchet holds at the "
            f"new floor instead of leaving {recorded - current} sites of headroom behind.")
        return

    before = baseline.get("abort_sites_by_file", {})
    grew = [(name, count) for name, count in result["abort_sites_by_file"].items()
            if count > before.get(name, 0)]
    detail = []
    for name, count in grew:
        where = ", ".join(str(site["line"]) for site in result["abort_site_lines"]
                          if site["file"] == name)
        detail.append(f"{name} {before.get(name, 0)} -> {count} (lines {where})")
    if not detail:
        # Only reachable from a baseline written before the per-file map existed.
        detail.append("(no per-file baseline recorded; run --write-baseline once to gain naming)")
    failures.append(
        f"abort sites grew from {recorded} to {current}: " + "; ".join(detail) +
        ". A new death in the server image goes through SessionFail (FatalFunnel.h), not through "
        "its own std::abort(): the funnel is the one place a SessionFault reaches the peer, the "
        "one telemetry point, and the only reason this census can be sure a death was named. If "
        "the site genuinely cannot reach the funnel, argue it into FUNNEL_SITES with a reason.")


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
                                        "abort_sites_by_file": result["abort_sites_by_file"],
                                        "families": result["families"]}, indent=2) + "\n",
                            encoding="utf-8")
        print(f"fatal census baseline written: {result['abort_sites']} abort sites over "
              f"{len(result['abort_sites_by_file'])} files, {len(result['families'])} families")
        return 0

    failures = []
    notes = []
    for entry in result["unmarked_aborts"]:
        failures.append(
            f"{entry['file']}:{entry['line']} aborts with no Fatal{{Family}} marker within "
            f"{kMarkerWindow} lines. Every death in the split server image must name a family: "
            f"the split retrace reds on `Fatal{{`, the refusal census counts by name, and a death "
            f"with no word is invisible to both.")
    for entry in result["unnamed_funnel_calls"]:
        failures.append(
            f"{entry['file']}:{entry['line']} calls WireLogFatal with no Fatal{{Family}} in its "
            f"format string. The funnel's own abort is exempt from the rule above ONLY because "
            f"its callers carry the word; a call that does not would launder exactly what the "
            f"rule refuses.")

    # Rule 3: the vocabulary is the .def's, and only the .def's.
    rowless = sorted(set(result["families"]) - declared_families())
    if rowless:
        failures.append(
            "family word(s) with no row in MG_Remote/FatalFamilies.def: " + ", ".join(rowless) +
            ". A row is the family's projection onto the seven-value wire FatalCode (5.2's D2 "
            "ruling); a word with no row tells the peer nothing, and FatalCodeForFamily cannot "
            "compile a call that names it.")

    # Rule 4: the handshake's vocabulary is protocol.fbs's RefuseCode, and only that.
    enumerators = declared_refusals()
    for word, where in result["refusal_words"].items():
        if word in enumerators or word in LOCAL_REFUSAL_WORDS:
            continue
        failures.append(
            f"Refuse{{{word}}} at {', '.join(where)} names no RefuseCode enumerator "
            f"(protocol.fbs has {', '.join(sorted(enumerators))}). A refusal the peer reads must "
            f"spell the enum's own word, or the log and the frame disagree about why the session "
            f"was declined; a refusal the peer never reads goes in LOCAL_REFUSAL_WORDS with its "
            f"reason.")

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
        ratchet_aborts(result, baseline, failures, notes)

    print(f"fatal census: {result['abort_sites']} abort sites over "
          f"{len(result['abort_sites_by_file'])} files of the split server image, "
          f"{len(result['families'])} distinct family words, "
          f"{len(result['refusal_words'])} refusal words, "
          f"{len(result['unmarked_aborts'])} unmarked")
    for message in notes:
        print(f"::notice::{message}")
    if failures:
        for message in failures:
            print(f"::error::{message}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
