#!/usr/bin/env python3
"""P6.5 ownership and dispatch boundary, with explicit compatibility limits.

The old raw constructors and Session::Control/Shm test/bootstrap accessors remain
source compatible. Ring endpoints are cached, non-owning setup handles, shared
by both data planes. This gate does not pretend that their type names disappeared;
it rejects ownership and concrete-link dispatch outside Transport instead.
"""
from __future__ import annotations
import argparse
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
SOURCES = [
    "MobileGL/MG_Remote/Client/ClientSession.h",
    "MobileGL/MG_Remote/Client/ClientSession.cpp",
    "MobileGL/MG_Remote/Server/ServerSession.h",
    "MobileGL/MG_Remote/Server/ServerSession.cpp",
    "MobileGL/MG_Remote/Server/ServerLoop.cpp",
    "MobileGL/MG_Remote/Wire/PipeWireCodec.h",
    "MobileGL/MG_Remote/Wire/PipeWireCodec.cpp",
    "MobileGL/MG_Impl/Pipe/ResourceTracker.h",
]
PHYSICAL = r"(?:SessionSegments|ReplySlotPool|RingProducer|RingConsumer|EventRingProducer|EventRingConsumer)"

def code_only(source: str) -> str:
    return re.sub(r"/\*.*?\*/|//[^\n]*", "", source, flags=re.S)

def violations(path: str, text: str) -> list[str]:
    code = code_only(text)
    found: list[str] = []
    rules = [
        (rf"\b{PHYSICAL}\s+m_\w+\s*(?:[;{{=])", "physical data-plane ownership outside Transport"),
        (r"\bm_shm\s*\.", "session bypasses its link for segment access"),
        (r"(?:new\s+(?:Transport::)?|make_unique\s*<\s*(?:Transport::)?)(?:ShmLink|StreamLink)\b", "concrete link selected outside Transport factory"),
        (r'#\s*include\s*[<"][^>"\n]*(?:ShmLink|StreamLink)\.h', "concrete link included above Transport"),
    ]
    if path.endswith("ServerLoop.cpp"):
        rules.append((r"\bRingControl\b|(?:\.|->)Control\(\)", "apply loop reads a physical control page"))
    if path.endswith("PipeWireCodec.h"):
        rules.append((r"\bRingControl\s*\*\s*m_", "codec stores physical page instead of setup progress/signals"))
    if path.endswith("ResourceTracker.h"):
        rules.append((r"Segments\(\)\.Resolve", "writeback bypasses ILink span resolution"))
    for pattern, message in rules:
        for match in re.finditer(pattern, code):
            line = code.count("\n", 0, match.start()) + 1
            found.append(f"{path}:{line}: {message}")
    return found

def self_test() -> None:
    controls = [
        (SOURCES[0], "Transport::SessionSegments m_segments;"),
        (SOURCES[0], "Transport::ReplySlotPool m_replies;"),
        (SOURCES[1], "m_shm.Close();"),
        (SOURCES[1], "auto link = std::make_unique<Transport::StreamLink>();"),
        (SOURCES[4], "auto& page = session.Control();"),
        (SOURCES[5], "Transport::RingControl* m_control = nullptr;"),
        (SOURCES[-1], "session->Segments().Resolve(4, offset, size);"),
    ]
    for path, source in controls:
        assert violations(path, source), f"negative control did not turn red: {source}"
    assert not violations(SOURCES[0], "Transport::RingProducer* m_cmd = nullptr;")
    print(f"link seam negative controls: {len(controls)} named failures observed")

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
    failures = [failure for name in SOURCES for failure in violations(name, (ROOT / name).read_text(encoding="utf-8"))]
    if failures:
        print("\n".join(failures))
        return 1
    print("link seam ownership/dispatch gate: PASS (cached endpoints and legacy test constructors retained)")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
