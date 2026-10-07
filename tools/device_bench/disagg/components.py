#!/usr/bin/env python3
"""Inclusive share of fixed components on one thread of a simpleperf profile, converted to
milliseconds per frame with the thread's measured CPU ms/frame.

    python components.py <perf.data> <binary_cache> <thread comm> [cpu_ms_per_frame]

A sample counts toward a component when any frame of its call chain matches the component's
pattern (first match in COMPONENTS order wins for the 'exclusive' column, so the rows of that
column add up to 100%).
"""
import os
import sys
from collections import Counter

sys.path.insert(0, os.environ.get("SIMPLEPERF_DIR", ""))
from simpleperf_report_lib import ReportLib  # noqa: E402

COMPONENTS = [
    ("trace parse (harness)", ("trace::Parser::", "BrotliFile", "trace::Call::~Call", "trace::File::")),
    ("validate point", ("MGPipeValidateForVerb",)),
    ("backend draw setup", ("SetupWireDraw", "VulkanRenderer::SetupDraw", "DirectGLES::PrepareForDraw")),
    ("backend draw other", ("DirectVulkan::DrawElements", "DirectGLES::DrawElements", "VulkanRenderer::DrawElements")),
    ("verb port / record sink", ("EmitDrawRecord", "VerbChannel::Port", "RecordVerbSink::")),
    ("wire encode (client)", ("ClientSession::Emit", "PipeWireEncoder::")),
    ("apply decode", ("PipeWireDecoder::", "PipeApplier::ApplyOne")),
    ("apply loop idle/poll", ("ServerLoop::ApplyThreadMain", "ServerLoop::DrainRing")),
    ("other GL entry points", ("retrace::retraceCall",)),
]


def main():
    data, symfs, comm = sys.argv[1], sys.argv[2], sys.argv[3]
    cpu = float(sys.argv[4]) if len(sys.argv) > 4 else None
    lib = ReportLib()
    lib.SetRecordFile(data)
    lib.SetSymfs(symfs)
    total = 0
    inclusive = Counter()
    exclusive = Counter()
    while True:
        sample = lib.GetNextSample()
        if sample is None:
            break
        if comm not in sample.thread_comm:
            continue
        total += 1
        frames = [lib.GetSymbolOfCurrentSample().symbol_name]
        chain = lib.GetCallChainOfCurrentSample()
        frames += [chain.entries[i].symbol.symbol_name for i in range(chain.nr)]
        first = None
        for name, patterns in COMPONENTS:
            if any(p in f for f in frames for p in patterns):
                inclusive[name] += 1
                if first is None:
                    first = name
        exclusive[first or "rest"] += 1
    print(f"{comm}: {total} samples" + (f", {cpu:.2f} ms/frame" if cpu else ""))
    for name, _ in COMPONENTS + [("rest", ())]:
        inc = 100.0 * inclusive[name] / max(1, total)
        exc = 100.0 * exclusive[name] / max(1, total)
        ms = f"  {cpu * exc / 100.0:6.2f} ms" if cpu else ""
        print(f"  {name:28} incl {inc:5.1f}%  excl {exc:5.1f}%{ms}")


if __name__ == "__main__":
    main()
