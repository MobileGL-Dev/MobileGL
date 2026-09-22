#!/usr/bin/env python3
"""P3b/P4b Espryt memo re-keying, with explicit compatibility limits.

P4a/P5f re-keyed the resolved-binding, sampler-pass, image-sweep and twin-registry
memos onto {slot, gen} handles and applier serials. The legacy pointer-keyed arms
were not deleted: they are the PULL build's only arms and G1 pins that build's
.text, so they retire with the pull path itself (Config.h:395-399).

WHAT THE TWO GUARDS MEAN HERE IS A POLICY EXEMPTION, NOT AN ABSENCE, and the first
version of this docstring got that wrong. MOBILEGL_PIPE_LEGACY_MEMOS defaults ON
(CMakeLists.txt:43) and nothing in test.yml clears it, so a CI split build DOES
compile the #if MOBILEGL_PIPE_LEGACY_MEMOS arms - TwinLookupMemo (DirectGLES.cpp:114)
sits inside one and is compiled into every lane this repo runs. The rule is therefore
"the legacy arm is ALLOWED to keep pointer keys", not "the split build never sees
them". !MOBILEGL_BUILD_DISAGGREGATED is the genuinely-absent one.

This gate does not pretend the pointer types disappeared; it rejects a frontend-pointer
KEY outside those two exempted arms, inside the named memo families, and nothing else.
It does not evaluate the preprocessor - it recognises the conditional spellings the
tree actually uses - and it does not look at DirectVulkan, whose own key inventory is
P7 wave 2's.

Two structural self-checks keep the gate honest, because both failure modes are silent:
a renamed struct would drop a whole family and still pass, and an allow-list entry no
rule can consult would sit there documenting a carve-out that is not happening. So every
FAMILIES anchor must match at least one span in the real sources, and every ALLOW entry
must actually suppress something.
"""
from __future__ import annotations
import argparse
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
SOURCES = [
    "MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp",
    "MobileGL/MG_Backend/DirectGLES/Managers.h",
]
# The frontend object types a memo may not be keyed on. StateObject is the twin
# registry's template parameter and is the ONLY spelling its map key ever has, so a
# gate written in terms of the concrete names alone would read Managers.h and find
# nothing.
#
# THE LEADING (?<!\w) IS LOAD-BEARING. Without it every name here also matches as a
# SUFFIX: `BackendSamplerObject*` matched `SamplerObject`, `BackendTextureObject*`
# matched `TextureObject`, and the gate reported the backend twins - the memos' VALUES -
# as frontend keys. The first version of this file answered that with two allow-list
# entries, i.e. it documented a false positive instead of fixing it. A `::` before the
# name is not a word character, so qualified spellings still match.
FRONTEND = (r"(?<!\w)"
            r"(?:ITextureObject|TextureObject|SamplerObject|ProgramObject|FramebufferObject"
            r"|RenderbufferObject|BufferObject|VertexArrayObject|StateObject)")
# An associative container's FIRST template argument, i.e. its key.
CONTAINER = (r"(?:UnorderedMap|UnorderedSet|Map|Set|unordered_map|unordered_set|flat_hash_map"
             r"|flat_hash_set|std::unordered_map|std::unordered_set|std::map|std::set)")
KEYED_ON_POINTER = rf"{CONTAINER}\s*<\s*(?:const\s+)?(?:\w+::)*{FRONTEND}\s*\*"
# A raw frontend pointer held as a memo FIELD. Inside a memo family's own braces that
# is a key by construction: these structs hold nothing but the comparison inputs.
POINTER_FIELD = rf"(?:const\s+)?(?:\w+::)*{FRONTEND}\s*\*\s*\w+\s*(?:\[[^\]]*\])?\s*(?:=|;|\{{)"
# GetLifetimeId() read into a comparison, which is the pointer key's companion half.
LIFETIME_KEY = r"GetLifetimeId\s*\(\s*\)"

# THE MEMO FAMILIES THE PLAN ROW NAMES, each as the brace-matched body of its own
# declaration. Scoping to the body rather than to the file is what keeps the gate from
# firing on the hundreds of ordinary frontend pointers these two files legitimately
# pass around.
FAMILIES = [
    ("ResolvedTextureBindingMemo", r"struct\s+ResolvedTextureBindingMemo\s*\{"),
    ("UnitSamplerLookupMemo", r"struct\s+UnitSamplerLookupMemo\s*\{"),
    ("TwinLookupMemo", r"class\s+TwinLookupMemo\s*\{"),
    ("SamplerPassMemo", r"struct\s+SamplerPassMemo\s*\{"),
    ("StateBackendObjectRegistry", r"class\s+StateBackendObjectRegistry\s*\{"),
]
# The image sweep is seven file statics rather than a struct, so it is matched by name.
IMAGE_SWEEP = r"^\s*static\s+.*\bg_imageSweep\w*"

# ONE LINE PER ENTRY AND THE REASON IS THE ENTRY. A carve-out with no reason is how a
# gate becomes a list of things that are allowed to be wrong.
ALLOW = [
    ("MobileGL/MG_Backend/DirectGLES/Managers.h", "using BackendMap",
     "the legacy arm's map IS the pull build's only arm; deleting it moves pull .text and "
     "fails G1's 0/0/0/0, so it retires with the pull path (P13), not here"),
]
# WHAT USED TO BE HERE, AND WHY IT IS NOT. Five more entries, and every one of them was a
# carve-out for something no rule could have flagged in the first place:
#   * two for `Array<SamplerImpl::BackendSamplerObject*...> rows` and
#     `SamplerImpl::BackendSamplerObject* backend` - false positives from FRONTEND's missing
#     word boundary, fixed above rather than exempted;
#   * `BackendMap m_entries`, which names a typedef and no frontend type at all;
#   * `const void* program` and `Uint64 programLifetimeId`, which no rule can consult -
#     `void` is not in FRONTEND and neither line calls GetLifetimeId().
# ResolvedTextureBindingMemo's type-erased monolith key is still there and still deliberate
# (it is nulled under `byHandle`, DirectGLES.cpp:7369-7373, and the pull build needs it for
# G1) - it simply never needed an exemption, and saying it did was misleading. The
# allow-liveness check below is what stops this list growing that way again.


def code_only(source: str) -> str:
    return re.sub(r"/\*.*?\*/|//[^\n]*", "", source, flags=re.S)


def legacy_lines(code: str) -> set[int]:
    """Line numbers (1-based) sitting in an arm a split build does not compile.

    NOT a preprocessor. It recognises `#if MOBILEGL_PIPE_LEGACY_MEMOS`,
    `#if !MOBILEGL_BUILD_DISAGGREGATED`, and the `#else` of
    `#if MOBILEGL_BUILD_DISAGGREGATED` - which is every spelling these two files use -
    and treats every other conditional as transparent."""
    legacy_if = re.compile(r"^\s*#\s*if\s+(?:!\s*MOBILEGL_BUILD_DISAGGREGATED\b|MOBILEGL_PIPE_LEGACY_MEMOS\b)")
    disagg_if = re.compile(r"^\s*#\s*if\s+MOBILEGL_BUILD_DISAGGREGATED\b")
    any_if = re.compile(r"^\s*#\s*if(?:def|ndef)?\b")
    else_if = re.compile(r"^\s*#\s*(?:else|elif)\b")
    end_if = re.compile(r"^\s*#\s*endif\b")
    protected: set[int] = set()
    stack: list[list[bool]] = []  # [this arm is legacy, the other arm is legacy]
    for number, line in enumerate(code.splitlines(), start=1):
        if any_if.match(line):
            if legacy_if.match(line):
                stack.append([True, False])
            elif disagg_if.match(line):
                stack.append([False, True])
            else:
                stack.append([False, False])
        elif else_if.match(line) and stack:
            stack[-1] = [stack[-1][1], stack[-1][0]]
        elif end_if.match(line) and stack:
            stack.pop()
        if any(frame[0] for frame in stack):
            protected.add(number)
    return protected


def family_spans(code: str) -> list[tuple[str, int, int]]:
    """(family, first line, last line) for each named memo family's braces."""
    spans: list[tuple[str, int, int]] = []
    for name, anchor in FAMILIES:
        for match in re.finditer(anchor, code):
            start = code.count("\n", 0, match.start()) + 1
            depth = 0
            end = start
            for offset in range(match.end() - 1, len(code)):
                if code[offset] == "{":
                    depth += 1
                elif code[offset] == "}":
                    depth -= 1
                    if depth == 0:
                        end = code.count("\n", 0, offset) + 1
                        break
            spans.append((name, start, end))
    return spans


ALLOW_HITS: set[int] = set()


def allowed(path: str, line_text: str) -> str:
    for index, (allow_path, needle, reason) in enumerate(ALLOW):
        if path == allow_path and needle in line_text:
            ALLOW_HITS.add(index)
            return reason
    return ""


def missing_families(path: str, text: str) -> list[str]:
    """Anchors that matched NO span in this file's sources, when they should have.

    A renamed struct is the silent failure this exists for: family_spans() simply
    returns fewer spans, every rule keeps passing, and the family stops being checked
    without anything saying so."""
    code = code_only(text)
    seen = {name for name, _, _ in family_spans(code)}
    return [name for name, _ in FAMILIES if name not in seen]


def violations(path: str, text: str) -> list[str]:
    code = code_only(text)
    lines = code.splitlines()
    protected = legacy_lines(code)
    spans = family_spans(code)
    found: list[str] = []
    rules = [
        (KEYED_ON_POINTER, "memo family keyed on a frontend object pointer"),
        (POINTER_FIELD, "memo family holds a frontend object pointer as a key field"),
        (LIFETIME_KEY, "memo family uses GetLifetimeId() as a key"),
    ]
    for name, start, end in spans:
        for number in range(start, min(end, len(lines)) + 1):
            if number in protected:
                continue
            line_text = lines[number - 1]
            for pattern, message in rules:
                if not re.search(pattern, line_text):
                    continue
                if allowed(path, line_text):
                    continue
                found.append(f"{path}:{number}: {name}: {message}")
    # The image sweep carries no braces of its own; its statics are matched by name.
    for number, line_text in enumerate(lines, start=1):
        if number in protected or not re.match(IMAGE_SWEEP, line_text):
            continue
        for pattern, message in ((POINTER_FIELD, "keyed on a frontend object pointer"),
                                 (LIFETIME_KEY, "uses GetLifetimeId() as a key")):
            if re.search(pattern, line_text) and not allowed(path, line_text):
                found.append(f"{path}:{number}: the image sweep memo: {message}")
    return found


def self_test() -> None:
    gles = SOURCES[0]
    managers = SOURCES[1]
    controls = [
        (gles, "struct ResolvedTextureBindingMemo {\n"
               "    MG_State::GLState::ProgramObject* key = nullptr;\n};\n"),
        (gles, "struct UnitSamplerLookupMemo {\n"
               "    SamplerObject* frontend = nullptr;\n};\n"),
        (gles, "class TwinLookupMemo {\n"
               "    UnorderedMap<StateObject*, Slot> m_slots;\n};\n"),
        (gles, "struct ResolvedTextureBindingMemo {\n"
               "    Uint64 id = program->GetLifetimeId();\n};\n"),
        (gles, "        static ITextureObject* g_imageSweepOwner = nullptr;\n"),
        (managers, "struct SamplerPassMemo {\n"
                   "    MG_State::GLState::SamplerObject* rows[16];\n};\n"),
        (managers, "class StateBackendObjectRegistry {\n"
                   "    std::unordered_map<ProgramObject*, Entry> m_byPointer;\n};\n"),
    ]
    for path, source in controls:
        assert violations(path, source), f"negative control did not turn red: {source!r}"
    # POSITIVE CONTROLS: each is a shape the tree deliberately keeps, and a gate that
    # reddened on them would be reverted within a day rather than fixed.
    assert not violations(gles, "struct UnitSamplerLookupMemo {\n"
                                "#if MOBILEGL_PIPE_LEGACY_MEMOS\n"
                                "    SamplerObject* frontend = nullptr;\n"
                                "#endif\n};\n"), "a LEGACY_MEMOS arm must stay green"
    assert not violations(gles, "struct ResolvedTextureBindingMemo {\n"
                                "#if MOBILEGL_BUILD_DISAGGREGATED\n"
                                "    MG_Pipe::MGPipeHandle drawProgram{};\n"
                                "#else\n"
                                "    ProgramObject* program = nullptr;\n"
                                "#endif\n};\n"), "the #else of a DISAGGREGATED arm must stay green"
    assert not violations(gles, "struct ResolvedTextureBindingMemo {\n"
                                "    const void* program = nullptr;\n};\n"), \
        "the type-erased monolith key must stay green without needing an exemption"
    assert not violations(gles, "void Unrelated(ProgramObject* program) { Use(program); }\n"), \
        "a frontend pointer outside every memo family is not this gate's business"
    # THE WORD-BOUNDARY CONTROLS. Both of these are memo VALUES holding BACKEND twins, and
    # both used to be reported as frontend keys because FRONTEND matched as a suffix.
    assert not violations(gles, "struct UnitSamplerLookupMemo {\n"
                                "    SamplerImpl::BackendSamplerObject* backend = nullptr;\n};\n"), \
        "a BackendSamplerObject value must not match SamplerObject"
    assert not violations(managers, "struct SamplerPassMemo {\n"
                                    "    Array<SamplerImpl::BackendSamplerObject*, 16> rows{};\n};\n"), \
        "a BackendSamplerObject row must not match SamplerObject"
    # ...and the qualified spelling still must, or the boundary went in too tight.
    assert violations(gles, "struct ResolvedTextureBindingMemo {\n"
                            "    MG_State::GLState::ProgramObject* key = nullptr;\n};\n"), \
        "a :: before the name is not a word character and must still match"
    # THE FAMILY ANCHORS. Renaming a struct must be loud, not silent.
    assert missing_families(gles, "struct SomethingElse {\n};\n"), \
        "a source with none of the anchors must report every family as missing"
    print(f"espryt memo purity negative controls: {len(controls)} named failures observed, "
          f"{len(ALLOW)} allow-list entr(y/ies) carried")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
    ALLOW_HITS.clear()
    sources = {name: (ROOT / name).read_text(encoding="utf-8") for name in SOURCES}
    failures = [failure for name, text in sources.items() for failure in violations(name, text)]

    # STRUCTURAL CHECK 1: every family anchor found its declaration somewhere. Each family
    # lives in exactly one of the two sources, so the test is over their union.
    found_anywhere: set[str] = set()
    for text in sources.values():
        found_anywhere |= {name for name, _, _ in family_spans(code_only(text))}
    for name, anchor in FAMILIES:
        if name not in found_anywhere:
            failures.append(
                f"{SOURCES[0]}:0: the anchor for {name} matched nothing in either source. It was "
                f"renamed, moved or reformatted, and this gate has silently stopped checking that "
                f"family - every rule below still passes. Fix the anchor ({anchor!r}) or drop the "
                f"family from FAMILIES with a reason.")

    # STRUCTURAL CHECK 2: every allow-list entry actually suppressed something. An entry no
    # rule can consult documents a carve-out that is not happening, which is worse than no
    # entry at all - it is a claim about the tree that nothing verifies.
    for index, (allow_path, needle, reason) in enumerate(ALLOW):
        if index not in ALLOW_HITS:
            failures.append(
                f"{allow_path}:0: the allow-list entry for {needle!r} suppressed nothing on this "
                f"tree. Either the code it exempted is gone - delete the entry - or a rule stopped "
                f"matching it, in which case the exemption is hiding a gap. Its reason was: {reason}")

    if failures:
        print("\n".join(failures))
        return 1
    print(f"espryt memo key purity gate: PASS ({len(FAMILIES)} families anchored, "
          f"{len(ALLOW)} allow-list entr(y/ies), all consulted; the legacy pointer-keyed arm "
          f"behind MOBILEGL_PIPE_LEGACY_MEMOS is a policy exemption, not an absence)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
