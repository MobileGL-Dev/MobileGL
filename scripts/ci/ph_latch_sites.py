#!/usr/bin/env python3
"""PH-1 (3) / Ph fuzz arm 2: the site -> raw-peer control map, checked (P7 package F2 latch).

WHAT IT CHECKS. PH-1 (3) (ID-P7-1) converts the named faults whose input bytes the PEER wrote into a
per-session latch (MG_Remote/FatalFunnel.h SessionLatch). Fuzz arm 2 wants one raw-peer negative
control per converted site. This script keeps those two lists the same list:

  * it finds every latch site in the four PH-1 (3) files - each `SessionLatch(` call and each
    `WireProtocolLatch(` / `WireProtocolLatchAt(` call - and rebuilds the line it logs,
    `Fatal{Family, "word"} ...` with every printf conversion a wildcard;
  * it reads the row table (`kRows`) of MobileGL/MG_Test/Wire/PeerLatchTest.cpp, whose Latched rows
    each carry a marker the test requires to be the FIRST `Fatal{` line of the server log;
  * a site is COVERED when a Latched row of the same file has a marker that is a prefix of a line
    the site can log. Otherwise it must be argued in UNREACHABLE: which earlier check refuses the
    same bytes first. Anything else is red.

Sites whose formats are IDENTICAL are one group (a log-reading control cannot tell them apart, and
the group is covered once): the three "opcode" copies (the decoder's pre-gate is the reachable one;
ApplyChecked's two are behind it) and SetVertexAttribDefaults.Count's two (the count > 32 arm is
behind popcount(Mask) == Count, so only the mismatch is reachable).

`--table` prints the map as Markdown. `--self-test` proves the check can go red: it drops each
covering row in turn and requires its group to turn up unmapped.

THE DECLINE-AND-CLOSE HALF IS NOT A SITE, so it is not a row: MECHANICS below names each check
that makes a latched session decline the rest of its work and close (DrainRing's pre-pop latch check,
the apply thread's own exit, the control pump's and SurfaceOpCodec's latched answers, RunSession's
sliced wait, the pre-gate ahead of the applier's stamp, SessionLatch's own bookkeeping), the code
that IS the check, and the cases that go red without it. The script requires both to exist.
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TEST = ROOT / "MobileGL" / "MG_Test" / "Wire" / "PeerLatchTest.cpp"
FILES = {
    "PipeWireCodec.cpp": ROOT / "MobileGL" / "MG_Remote" / "Wire" / "PipeWireCodec.cpp",
    "PipeApplier.cpp": ROOT / "MobileGL" / "MG_Remote" / "Server" / "PipeApplier.cpp",
    "ServerLoop.cpp": ROOT / "MobileGL" / "MG_Remote" / "Server" / "ServerLoop.cpp",
    "SurfaceOpCodec.cpp": ROOT / "MobileGL" / "MG_Remote" / "Protocol" / "SurfaceOpCodec.cpp",
}

# (file, a substring of the site's line) -> why no peer byte reaches it. The site is converted
# anyway - it is on the decode path and returns like its neighbours - and the reason names the
# earlier check that refuses the same bytes first.
UNREACHABLE = [
    ("PipeWireCodec.cpp", '"segment-resolve"',
     "ResolveOrFatal runs RequireDeclaredBlob first, which has already refused a run the segment "
     "table cannot resolve (R2BlobOutsideItsSegment); reaching it means the table moved"),
    ("PipeWireCodec.cpp", "the audit was asked to record",
     "NoteResolvedRun's one caller is ResolveOrFatal after RequireDeclaredBlob: an empty or "
     "non-SEG_STAGE run never arrives here"),
    ("PipeWireCodec.cpp", "SEG_STAGE runs in one record",
     "no record resolves more runs than the audit array holds (CreateShaderState's seven blobs "
     "are the most, and six of them must be undeclared)"),
    ("PipeWireCodec.cpp", '"DrawVbo.userIndices"',
     "MGPipeWireRecordLayout gives a user-index draw exactly one 32-byte second tail and the tail "
     "cross-check refuses any record that differs (CodecTailCrossCheck)"),
    ("PipeWireCodec.cpp", '"DrawVbo.indirect"',
     "as DrawVbo.userIndices: the layout fixes the indirect block's size"),
    ("ServerLoop.cpp", '"appliedSeq batched"',
     "both tallies move inside the same ApplyOne callback; no record bytes can separate them"),
    ("ServerLoop.cpp", '"SurfaceOp.windowBackend"',
     "SurfaceOpCodec maps the wire WindowKind onto a legal tag or refuses it (the three SurfaceOp "
     "rows) before a frame reaches the dispatch"),
    ("ServerLoop.cpp", '"SurfaceOp.kind"',
     "SurfaceOpCodec refuses an unknown wire kind (SurfaceOpUnknownKind) before the dispatch"),
    # P12 (D6): not a peer's byte at all - the server's OWN display window was destroyed
    # (ServerDisplay::Detach from the display Activity's surfaceDestroyed). Its control is
    # ServerLoopTest's ALostServerWindowIsReleasedOnTheApplyThreadBeforeDetachReturnsAndLatchesByName.
    ("ServerLoop.cpp", '"surfaceDestroyed"',
     "raised by the server's own display (surfaceDestroyed -> ServerDisplay::Detach), never by the "
     "peer's bytes; ServerLoopTest's lost-window case is its negative control"),
]

REMOTE = ROOT / "MobileGL" / "MG_Remote"
WIRE_TESTS = ROOT / "MobileGL" / "MG_Test" / "Wire"

# (source, what the check does, a regex that IS the check, [(test file, case name)]) - each case goes
# red with the check deleted (the red-once is recorded in the package's NOTE).
MECHANICS = [
    # ONE check, the first statement of DrainRing's loop, immediately before every pop. It replaced
    # the two the F2 latch package shipped (one at the function's top, one under `++applied;`):
    # codex closeout finding 6 showed a latch that another thread stores after the per-record check
    # still let one more record be popped and applied. The one check does all three jobs - a drain
    # ENTERED latched pops nothing (the exit-path drain), a record that latched is the last one its
    # batch applies, and a latch from RunSession's control thread between two records stops the
    # next pop when stored before this check (it NARROWS the window to check-to-pop, not closes it;
    # the session ends at the next check) - and each case below goes red with it deleted. The
    # pattern may not leave DrainRing's body (`\n    }\n` closes a member function in ServerLoop.cpp).
    (REMOTE / "Server" / "ServerLoop.cpp",
     "DrainRing: no record is popped once the session latched (checked immediately before every pop)",
     r"Uint64 ServerLoop::DrainRing\(\) \{(?:(?!\n    \}\n).)*?for \(;;\) \{\s*if \(SessionLatched\(\)\) break;",
     [("ServerLoopTest.cpp", "ALatchedRecordEndsItsBatchAndTheApplyThreadLeavesWithoutAStop"),
      ("PeerLatchTest.cpp", "ALatchedRecordIsTheLastRecordItsBatchApplies"),
      ("ServerLoopTest.cpp", "ALatchFromAnotherThreadBetweenTwoRecordsStopsTheNextPop")]),
    (REMOTE / "Server" / "ServerLoop.cpp", "ApplyThreadMain: the apply thread leaves its loop on the latch, no Stop()",
     r"if \(SessionLatched\(\)\) \{\s*MGLOG_E\(\"MG_Remote server: mgl-srv-apply leaves its loop",
     [("ServerLoopTest.cpp", "ALatchedRecordEndsItsBatchAndTheApplyThreadLeavesWithoutAStop")]),
    (REMOTE / "Server" / "ServerLoop.cpp", "PumpControlRequest: a frame taken after the latch is answered, not run",
     r"if \(SessionLatched\(\)\) \{\s*m_controlResult = MOBILEGL_ERR_PROTOCOL_MISMATCH;",
     [("ServerLoopTest.cpp", "AControlFrameTakenAfterTheLatchIsAnsweredWithoutRunning")]),
    (REMOTE / "Protocol" / "SurfaceOpCodec.cpp", "ServerApplyWireSurfaceOp: a latched session declines a well-formed op",
     r"if \(SessionLatched\(\)\) return MOBILEGL_ERR_PROTOCOL_MISMATCH;",
     [("SurfaceControlFrameTest.cpp", "ALatchedSessionAnswersAWellFormedSurfaceOpWithoutRunningIt")]),
    (REMOTE / "Server" / "ServerMain.cpp", "RunSession: the control wait is sliced, so a latch closes a silent peer's session",
     r"ReceiveFrame\(\{buffer\.data\(\), buffer\.size\(\)\}, &size, kControlSliceMs\)",
     [("PeerLatchTest.cpp", "ALatchOnTheApplyThreadClosesTheSessionWhileAUnixPeerHoldsItOpen"),
      ("PeerLatchTest.cpp", "ALatchOnTheApplyThreadClosesTheSessionWhileATcpPeerHoldsItOpen")]),
    (REMOTE / "Server" / "PipeApplier.cpp", "ApplyOne: the pre-gate runs before the verb stamp and the barrier read",
     r"if \(!m_decoder\.AdmitOrDecline\(record\)\) return false;\s*const Bool wireSaysBarriered",
     [("ServerLoopTest.cpp", "ARecordShorterThanItsTypeIsLatchedBeforeTheApplierStampsIt")]),
    (REMOTE / "FatalFunnel.cpp", "SessionLatch: first fault wins, every fault counted, one SessionFault; unarmed it dies",
     r"if \(!g_latchArmed\.load\(std::memory_order_acquire\)\) \{\s*SessionFail\(",
     [("ServerLoopTest.cpp", "AnArmedSessionLatchKeepsTheFirstFaultCountsEveryOneAndPublishesOnce"),
      ("ServerLoopTest.cpp", "AnUnarmedSessionLatchDiesWithItsLineLikeSessionFail")]),
]


def case_suite(test_file, case):
    """The gtest suite that owns `case` in `test_file`, or None when the file has no such case."""
    text = (WIRE_TESTS / test_file).read_text(encoding="utf-8")
    found = re.search(r"\bTEST(?:_F|_P)?\(\s*(\w+)\s*,\s*" + case + r"\s*\)", text)
    return found.group(1) if found else None


def mechanics_failures():
    failures = []
    for source, what, check, cases in MECHANICS:
        if not re.search(check, source.read_text(encoding="utf-8"), re.DOTALL):
            failures.append(f"MECHANICS: {source.name} no longer has the check for `{what}`")
        for test_file, case in cases:
            if case_suite(test_file, case) is None:
                failures.append(f"MECHANICS: {test_file} has no case {case} (the control for `{what}`)")
    return failures


def print_mechanics():
    print()
    print("| decline / close check | source | negative control(s) |")
    print("|---|---|---|")
    for source, what, _check, cases in MECHANICS:
        print(f"| {what} | {source.name} | " +
              ", ".join(f"`{case_suite(f, c)}.{c}` ({f})" for f, c in cases) + " |")


LATCH_CALL = re.compile(r"(?<![A-Za-z0-9_])(SessionLatch|WireProtocolLatchAt|WireProtocolLatch)\s*\(")
STRING = re.compile(r'"((?:[^"\\]|\\.)*)"')
CONVERSION = re.compile(r"%[-+ #0-9.]*(?:hh|h|ll|l|z|j|t)?[diouxXcsp]")


def strip_comments(text):
    out, i, n, quote = [], 0, len(text), None
    while i < n:
        c = text[i]
        if quote:
            out.append(c)
            if c == "\\" and i + 1 < n:
                out.append(text[i + 1])
                i += 2
                continue
            if c == quote:
                quote = None
            i += 1
            continue
        if c in "\"'":
            quote = c
            out.append(c)
            i += 1
            continue
        if text.startswith("//", i):
            while i < n and text[i] != "\n":
                i += 1
            continue
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("\n" * text.count("\n", i, j))
            i = j
            continue
        out.append(c)
        i += 1
    return "".join(out)


def c_unescape(literal):
    return literal.replace('\\"', '"').replace("\\\\", "\\")


def call_text(lines, index, start):
    """The call's text from `start` on line `index` to its closing parenthesis."""
    depth, text, i = 0, [], index
    col = start
    while i < len(lines):
        line = lines[i]
        j, quote = col, None
        while j < len(line):
            c = line[j]
            text.append(c)
            if quote:
                if c == "\\" and j + 1 < len(line):
                    text.append(line[j + 1])
                    j += 2
                    continue
                if c == quote:
                    quote = None
            elif c in "\"'":
                quote = c
            elif c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
                if depth == 0:
                    return "".join(text)
            j += 1
        text.append("\n")
        i += 1
        col = 0
    return "".join(text)


def string_args(call):
    """The call's arguments that are string literals (adjacent literals concatenated), in order."""
    args, depth, current, quote, i = [], 0, [], None, 0
    body = call[call.find("(") + 1:]
    pieces = []
    while i < len(body):
        c = body[i]
        if quote:
            current.append(c)
            if c == "\\" and i + 1 < len(body):
                current.append(body[i + 1])
                i += 2
                continue
            if c == quote:
                quote = None
        elif c in "\"'":
            quote = c
            current.append(c)
        elif c in "([{":
            depth += 1
            current.append(c)
        elif c in ")]}":
            if depth == 0:
                pieces.append("".join(current))
                break
            depth -= 1
            current.append(c)
        elif c == "," and depth == 0:
            pieces.append("".join(current))
            current = []
        else:
            current.append(c)
        i += 1
    for piece in pieces:
        literals = STRING.findall(piece)
        stripped = STRING.sub("", piece).strip()
        args.append(c_unescape("".join(literals)) if literals and not stripped else None)
    return args


def latch_sites():
    """[(file, line, format)] - the line each latch call logs, from `Fatal{` on."""
    sites = []
    for name, path in FILES.items():
        lines = strip_comments(path.read_text(encoding="utf-8")).split("\n")
        for index, line in enumerate(lines):
            for match in LATCH_CALL.finditer(line):
                if re.search(r"\b(Bool|bool)\s+" + match.group(1) + r"\s*\(", line):
                    continue  # the helper's own definition
                previous = "\n".join(lines[max(0, index - 3):index])
                if re.search(r"\bBool\s+WireProtocolLatch(At)?\s*\(", previous):
                    continue  # the SessionLatch inside the helper: its callers are the sites
                args = string_args(call_text(lines, index, match.start()))
                if match.group(1) == "SessionLatch":
                    fmt = next((a for a in args[1:] if a is not None), None)
                elif match.group(1) == "WireProtocolLatch":
                    fmt = None if args[0] is None else (
                        'Fatal{ProtocolCorruption, "' + args[0] + '"} ' + (args[1] or "%s"))
                else:
                    fmt = None if args[0] is None else (
                        'Fatal{ProtocolCorruption, "' + args[0] + '"} got=%llu expected=%llu')
                if fmt is None or "Fatal{" not in fmt:
                    raise SystemExit(f"{name}:{index + 1}: a {match.group(1)} call whose line names no Fatal{{Family")
                sites.append((name, index + 1, fmt[fmt.find("Fatal{"):]))
    return sites


ROW = re.compile(r'\{"(\w+)",\s*"([^"]+)",\s*"([^"]*)",\s*Outcome::(\w+),\s*((?:"(?:[^"\\]|\\.)*"\s*)+|nullptr)',
                 re.DOTALL)


def rows():
    text = TEST.read_text(encoding="utf-8")
    start = text.find("const Row kRows[] = {")
    end = text.find("\n    };", start)
    if start < 0 or end < 0:
        raise SystemExit("PeerLatchTest.cpp has no kRows table")
    found = []
    for match in ROW.finditer(strip_comments(text[start:end])):
        raw = match.group(5)
        marker = None if raw.strip() == "nullptr" else c_unescape("".join(STRING.findall(raw)))
        found.append({"name": match.group(1), "file": match.group(2), "site": match.group(3),
                      "outcome": match.group(4), "marker": marker})
    return found


def prefix_regex(fmt):
    """A regex accepting exactly the PREFIXES of lines `fmt` can log (conversions are wildcards)."""
    tokens, i = [], 0
    for conversion in CONVERSION.finditer(fmt):
        tokens.extend(re.escape(c) for c in fmt[i:conversion.start()])
        # A conversion stands for what printf can put there and nothing wider: a %s never
        # crosses a quote or a line (the fault word sits between quotes) and a number or a
        # pointer is its own digits. A bare `.*?` let "%s.blob" accept every marker in the file.
        kind = conversion.group(0)[-1]
        if kind == "s":
            tokens.append('[^"\\n]*')
        elif kind == "c":
            tokens.append(".")
        else:
            tokens.append(r"(?:\(nil\)|[-+0-9a-fA-FxX.])*")
        i = conversion.end()
    tokens.extend(re.escape(c) for c in fmt[i:])
    pattern = ""
    for token in reversed(tokens):
        pattern = "(?:" + token + pattern + ")?"
    return re.compile(pattern, re.DOTALL)


def groups(sites):
    """Identical formats in one file are one group: [(file, fmt, [lines])]."""
    merged = {}
    for name, line, fmt in sites:
        merged.setdefault((name, fmt), []).append(line)
    return [(name, fmt, lines) for (name, fmt), lines in merged.items()]


def literal_word(fmt):
    """True when the fault word between the first quotes is spelled out, not a %s."""
    word = re.match(r'Fatal\{\w+, "([^"]*)"', fmt)
    return bool(word) and "%" not in word.group(1)


def build_map(sites, table):
    merged = groups(sites)
    accepts = {(name, fmt): prefix_regex(fmt) for name, fmt, _ in merged}
    # A row belongs to the sites its marker can be a prefix of - and when one of those spells the
    # fault word out, only to those: a marker that stops at `"DrawVbo.Flags"}` is also a prefix of
    # the `"%s"} ...` helpers' lines, and crediting the helpers with it would hide that nothing
    # drives them.
    owners = {}
    for row in table:
        if row["outcome"] != "Latched" or not row["marker"] or not row["marker"].startswith("Fatal{"):
            continue
        hits = [(name, fmt) for name, fmt, _ in merged
                if name == row["file"] and accepts[(name, fmt)].fullmatch(row["marker"])]
        if any(literal_word(fmt) for _, fmt in hits):
            hits = [(name, fmt) for name, fmt in hits if literal_word(fmt)]
        for key in hits:
            owners.setdefault(key, []).append(row["name"])
    result = []
    for name, fmt, lines in merged:
        reason = next((why for file, needle, why in UNREACHABLE if file == name and needle in fmt), None)
        result.append({"file": name, "lines": lines, "fmt": fmt, "rows": owners.get((name, fmt), []),
                       "unreachable": reason})
    return result


def unmapped(mapping):
    return [m for m in mapping if not m["rows"] and not m["unreachable"]]


def short(fmt, width=72):
    return fmt if len(fmt) <= width else fmt[:width - 3] + "..."


def print_table(mapping, table):
    print("| file:line | site line (`%` = wildcard) | raw-peer control(s) |")
    print("|---|---|---|")
    for m in mapping:
        where = m["file"] + ":" + ",".join(str(line) for line in m["lines"])
        cell = ", ".join(f"`{r}`" for r in m["rows"]) if m["rows"] else f"unreachable - {m['unreachable']}"
        print(f"| {where} | `{short(m['fmt'])}` | {cell} |")
    print()
    print("| D11 / PH bound row | file | site | outcome |")
    print("|---|---|---|---|")
    for row in table:
        if row["name"].startswith("D11"):
            print(f"| `{row['name']}` | {row['file']} | `{row['site']}` | {row['outcome']} |")


def self_test(sites, table):
    covered = [m for m in build_map(sites, table) if m["rows"]]
    if not covered:
        return "no covered site to perturb"
    for m in covered:
        pruned = [row for row in table if row["name"] not in m["rows"]]
        if not any(x["fmt"] == m["fmt"] and x["file"] == m["file"] for x in unmapped(build_map(sites, pruned))):
            return f"dropping {m['rows']} did not leave {m['file']} `{short(m['fmt'])}` unmapped"
    return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--table", action="store_true", help="print the site -> control map as Markdown")
    parser.add_argument("--self-test", action="store_true", help="prove every covered site can go red")
    args = parser.parse_args()
    sites = latch_sites()
    table = rows()
    mapping = build_map(sites, table)
    if args.table:
        print_table(mapping, table)
        print_mechanics()
    failures = mechanics_failures()
    if args.self_test:
        failure = self_test(sites, table)
        if failure:
            failures.append(f"self-test did not go red: {failure}")
    for m in unmapped(mapping):
        failures.append(f"{m['file']}:{m['lines'][0]} latches `{short(m['fmt'])}` and no PeerLatchTest row's "
                        f"marker names it; add a raw-peer row, or argue it into UNREACHABLE")
    for file, needle, _ in UNREACHABLE:
        if not any(m["file"] == file and needle in m["fmt"] for m in mapping):
            failures.append(f"UNREACHABLE names {file} `{needle}`, which is no longer a latch site")
    for row in table:
        if row["outcome"] == "Latched" and row["name"] not in {r for m in mapping for r in m["rows"]} \
                and not row["name"].startswith("D11"):
            failures.append(f"row {row['name']} is Latched but its marker names no latch site of {row['file']}")
    covered = sum(1 for m in mapping if m["rows"])
    argued = sum(1 for m in mapping if not m["rows"] and m["unreachable"])
    print(f"ph latch sites: {len(sites)} latch calls = {len(mapping)} distinct lines in {len(FILES)} files; "
          f"{covered} covered by PeerLatchTest rows ({len(table)} rows), {argued} argued unreachable, "
          f"{len(unmapped(mapping))} unmapped; {len(MECHANICS)} decline/close checks, each with its case(s)" +
          ("; self-test red on every drop" if args.self_test and not
           any("self-test" in f for f in failures) else ""))
    for message in failures:
        print(f"::error::{message}", file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
