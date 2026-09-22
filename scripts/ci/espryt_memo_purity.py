#!/usr/bin/env python3
"""P3b/P4b Espryt memo re-keying, with explicit compatibility limits.

P4a/P5f re-keyed the resolved-binding, sampler-pass, image-sweep and twin-registry
memos onto {slot, gen} handles and applier serials. The legacy pointer-keyed arms
were not deleted: they are the PULL build's only arms and G1 pins that build's
.text, so they stay behind MOBILEGL_PIPE_LEGACY_MEMOS or !MOBILEGL_BUILD_DISAGGREGATED
and retire with the pull path itself (Config.h:395-399). This gate does not pretend
the pointer types disappeared; it rejects a frontend-pointer KEY in the arm a split
build compiles, inside the named memo families, and nothing else. It does not
evaluate the preprocessor - it recognises the four conditional spellings the tree
actually uses - and it does not look at DirectVulkan, whose own key inventory is
P7 wave 2's.
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
FRONTEND = (r"(?:ITextureObject|TextureObject|SamplerObject|ProgramObject|FramebufferObject"
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
    ("MobileGL/MG_Backend/DirectGLES/Managers.h", "BackendMap m_entries",
     "the member behind the line above, same reason"),
    ("MobileGL/MG_Backend/DirectGLES/Managers.h", "Array<SamplerImpl::BackendSamplerObject*",
     "SamplerPassMemo::rows holds BACKEND pointers as the memo's VALUE, compared against "
     "g_boundSamplersCache; it is not a key and not a frontend type"),
    ("MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp", "SamplerImpl::BackendSamplerObject* backend",
     "UnitSamplerLookupMemo's resolved twin - the memo's value, not its key"),
    ("MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp", "const void* program",
     "ResolvedTextureBindingMemo's monolith-arm key, type-erased and set to nullptr under "
     "byHandle (DirectGLES.cpp:7369-7373); the split arm keys on drawProgram {slot,gen}. "
     "It is compiled into the pull build, so G1 keeps it until the pull path retires (P13)"),
    ("MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp", "Uint64 programLifetimeId",
     "the ABA companion of the line above, same arm and same reason"),
]


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


def allowed(path: str, line_text: str) -> str:
    for allow_path, needle, reason in ALLOW:
        if path == allow_path and needle in line_text:
            return reason
    return ""


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
        "the allow-listed type-erased monolith key must stay green"
    assert not violations(gles, "void Unrelated(ProgramObject* program) { Use(program); }\n"), \
        "a frontend pointer outside every memo family is not this gate's business"
    print(f"espryt memo purity negative controls: {len(controls)} named failures observed, "
          f"{len(ALLOW)} allow-list entries carried")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
    failures = [failure for name in SOURCES
                for failure in violations(name, (ROOT / name).read_text(encoding="utf-8"))]
    if failures:
        print("\n".join(failures))
        return 1
    print("espryt memo key purity gate: PASS (the legacy pointer-keyed arms behind "
          "MOBILEGL_PIPE_LEGACY_MEMOS / !MOBILEGL_BUILD_DISAGGREGATED are retained on purpose)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
