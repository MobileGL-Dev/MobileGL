#!/usr/bin/env python3
"""P8-SE: PipeCatalogueTest's unmigrated-emulation list IS the set of call sites in the tree.

THE QUESTION. `MGPipeUnmigratedEmulation("<name>")` (MG_Pipe/PipeApply.h) names one emulation
that reaches into client memory; a split server aborts on it, a monolith arm runs past it. The
test `PipeCatalogue.EveryUnmigratedEmulationIsNamedOnce` (MG_Test/Pipe/PipeCatalogueTest.cpp)
pins the LIST of names, and nothing compared that list with the tree: it drifted twice without a
red (P8-B found a name the list never had; P8-SE a name whose only call site no transport could
reach). This asserts, every one an error:

  * every name in the test's `kNames[]` has EXACTLY ONE call site in the product sources;
  * every call site's name is in `kNames[]`;
  * `kNames[]` has no duplicate;
  * every call is spelled with a string literal (a computed name cannot be checked).

PRODUCT SOURCES are MobileGL/ minus MG_Test/ and MG_IntegrationTest/ (tests call the function
with literals of their own), in C/C++ files (.c .cc .cpp .h .hpp .inc .mm). Comments are masked
before matching, string literals are kept, so a name quoted in a comment is not a call site.

Usage:  unmigrated_emulation_sites.py [repo-root]      (default: this script's repository)
        unmigrated_emulation_sites.py --self-test
"""
import argparse
from pathlib import Path
import re
import sys
import tempfile

LIST_FILE = Path("MobileGL/MG_Test/Pipe/PipeCatalogueTest.cpp")
TEST_NAME = "EveryUnmigratedEmulationIsNamedOnce"
SOURCE_ROOT = Path("MobileGL")
EXCLUDED = (Path("MobileGL/MG_Test"), Path("MobileGL/MG_IntegrationTest"))
SUFFIXES = {".c", ".cc", ".cpp", ".h", ".hpp", ".inc", ".mm"}
CALL = re.compile(r"\bMGPipeUnmigratedEmulation\s*\(")
LITERAL_CALL = re.compile(r'\bMGPipeUnmigratedEmulation\s*\(\s*"((?:[^"\\]|\\.)*)"\s*\)')
# The declaration and the definition: the return type (and any qualifier) right before the name
# on its line.
DECLARATION = re.compile(r"\bvoid\s+(?:[A-Za-z_]\w*::)*$")


def mask_comments(text):
    """`text` with every comment character replaced by a space (newlines kept, so offsets and line
    numbers survive); string and character literals are left as they are."""
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                out[i] = " "
                i += 1
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            for k in range(i, end):
                if text[k] != "\n":
                    out[k] = " "
            i = end
        elif c in "\"'":
            # R"(...)" raw strings: skip to the matching delimiter.
            if c == '"' and i > 0 and text[i - 1] == "R":
                close = text.find("(", i)
                delim = text[i + 1:close]
                end = text.find(")" + delim + '"', close)
                i = n if end < 0 else end + len(delim) + 2
                continue
            i += 1
            while i < n and text[i] != c:
                i += 2 if text[i] == "\\" else 1
            i += 1
        else:
            i += 1
    return "".join(out)


def list_names(root, errors):
    path = root / LIST_FILE
    try:
        text = mask_comments(path.read_text(encoding="utf-8"))
    except OSError as exc:
        errors.append(f"{LIST_FILE}: cannot read ({exc})")
        return []
    test = text.find(TEST_NAME)
    block = re.search(r"kNames\s*\[\s*\]\s*=\s*\{(.*?)\};", text[test:], re.S) if test >= 0 else None
    if block is None:
        errors.append(f"{LIST_FILE}: no `kNames[] = {{ ... }};` in {TEST_NAME}")
        return []
    return re.findall(r'"((?:[^"\\]|\\.)*)"', block.group(1))


def call_sites(root, errors):
    """{name: [file:line, ...]} over the product sources."""
    sites = {}
    base = root / SOURCE_ROOT
    for path in sorted(base.rglob("*")):
        if path.suffix not in SUFFIXES or not path.is_file():
            continue
        rel = path.relative_to(root)
        if any(rel.is_relative_to(ex) for ex in EXCLUDED):
            continue
        text = mask_comments(path.read_text(encoding="utf-8", errors="replace"))
        if "MGPipeUnmigratedEmulation" not in text:
            continue
        literal_starts = set()
        for match in LITERAL_CALL.finditer(text):
            line = text.count("\n", 0, match.start()) + 1
            sites.setdefault(match.group(1), []).append(f"{rel.as_posix()}:{line}")
            literal_starts.add(match.start())
        for match in CALL.finditer(text):
            if match.start() in literal_starts:
                continue
            prefix = text[text.rfind("\n", 0, match.start()) + 1:match.start()]
            if DECLARATION.search(prefix):
                continue
            line = text.count("\n", 0, match.start()) + 1
            errors.append(f"{rel.as_posix()}:{line}: MGPipeUnmigratedEmulation called without a string "
                          f"literal - the name cannot be checked against {LIST_FILE.name}")
    return sites


def check(root):
    errors = []
    names = list_names(root, errors)
    sites = call_sites(root, errors)
    seen = set()
    for name in names:
        if name in seen:
            errors.append(f"{LIST_FILE}: \"{name}\" is listed twice")
        seen.add(name)
    for name in sorted(seen):
        where = sites.get(name, [])
        if not where:
            errors.append(f"\"{name}\" is in {LIST_FILE.name}'s list and has no call site in the tree "
                          f"(retired? take it out of the list)")
        elif len(where) > 1:
            errors.append(f"\"{name}\" has {len(where)} call sites ({', '.join(where)}); one site per name")
    for name in sorted(set(sites) - seen):
        errors.append(f"\"{name}\" is called at {', '.join(sites[name])} and is not in {LIST_FILE.name}'s "
                      f"list")
    return names, sites, errors


def run(root):
    names, sites, errors = check(root)
    for err in errors:
        print(f"error: {err}")
    total = sum(len(v) for v in sites.values())
    state = "OK" if not errors else f"{len(errors)} error(s)"
    print(f"unmigrated emulation sites: {len(names)} listed, {total} call sites - {state}")
    return 1 if errors else 0


def self_test():
    """Each rule reds on a synthetic tree and a matching tree is green."""
    listing = """
TEST(PipeCatalogue, EveryUnmigratedEmulationIsNamedOnce) {
    // "commented-out-name" is not in the list
    const char* const kNames[] = {
%s
    };
}
"""
    product = """
    void MGPipeUnmigratedEmulation(const char* name);
    void Site() {
        // MGPipeUnmigratedEmulation("in-a-comment");
        /* MGPipeUnmigratedEmulation("in-a-block-comment"); */
%s
    }
"""
    cases = [
        ("a matching tree", ['"a",', '"b",'], ['MGPipeUnmigratedEmulation("a");',
                                               'MG_Pipe::MGPipeUnmigratedEmulation( "b" );'], 0),
        ("a stray listed name", ['"a",', '"b",', '"stray",'], ['MGPipeUnmigratedEmulation("a");',
                                                               'MGPipeUnmigratedEmulation("b");'], 1),
        ("an unlisted call site", ['"a",'], ['MGPipeUnmigratedEmulation("a");',
                                             'MGPipeUnmigratedEmulation("b");'], 1),
        ("two sites for one name", ['"a",'], ['MGPipeUnmigratedEmulation("a");',
                                              'MGPipeUnmigratedEmulation("a");'], 1),
        ("a duplicated list entry", ['"a",', '"a",'], ['MGPipeUnmigratedEmulation("a");'], 1),
        ("a computed name", ['"a",'], ['MGPipeUnmigratedEmulation("a");',
                                       'MGPipeUnmigratedEmulation(name);'], 1),
    ]
    failures = 0
    for label, listed, calls, want in cases:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / LIST_FILE).parent.mkdir(parents=True)
            (root / LIST_FILE).write_text(listing % "\n".join("        " + s for s in listed))
            src = root / "MobileGL/MG_Backend/Site.cpp"
            src.parent.mkdir(parents=True)
            src.write_text(product % "\n".join("        " + c for c in calls))
            # A test's own literal call is not a product call site.
            other = root / "MobileGL/MG_Test/Other/OtherTest.cpp"
            other.parent.mkdir(parents=True)
            other.write_text('void T() { MGPipeUnmigratedEmulation("only-in-a-test"); }\n')
            _, _, errors = check(root)
            got = 1 if errors else 0
            if got != want:
                failures += 1
                print(f"self-test FAILED: {label}: rc {got} (want {want}); errors: {errors}")
    print("self-test " + ("OK" if failures == 0 else f"FAILED ({failures})") + f": {len(cases)} cases")
    return 1 if failures else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("root", nargs="?", default=str(Path(__file__).resolve().parents[2]))
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    return run(Path(args.root))


if __name__ == "__main__":
    sys.exit(main())
