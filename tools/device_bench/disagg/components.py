#!/usr/bin/env python3
"""Inclusive share of fixed components on one thread of a simpleperf profile, converted to
milliseconds per frame with the thread's measured CPU ms/frame.

    python components.py <perf.data> <binary_cache> <thread comm> [cpu_ms_per_frame] [--tail-ms=N]

A sample counts toward a component when any frame of its call chain matches the component's
pattern (first match in COMPONENTS order wins for the 'exclusive' column, so the rows of that
column add up to 100%).
"""
import os
import sys
from collections import Counter

sys.path.insert(0, os.environ.get("SIMPLEPERF_DIR", ""))
from simpleperf_report_lib import ReportLib  # noqa: E402


def tail_start(data, symfs, tail_ms):
    """Timestamp (ns) where the last `tail_ms` of the recording begins, or 0 for everything."""
    if not tail_ms:
        return 0
    lib = ReportLib()
    lib.SetRecordFile(data)
    lib.SetSymfs(symfs)
    # The end is the replay's last retraced call, not the recording's last sample: the app
    # tears its context down after the benchmark ends, and that must not land in the window.
    last = 0
    while True:
        sample = lib.GetNextSample()
        if sample is None:
            break
        chain = lib.GetCallChainOfCurrentSample()
        if any("retrace::retraceCall" in chain.entries[i].symbol.symbol_name for i in range(chain.nr)):
            last = max(last, sample.time)
    return last - int(tail_ms * 1e6)

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
    args = [a for a in sys.argv[1:] if not a.startswith("--tail-ms=")]
    tail = next((float(a.split("=", 1)[1]) for a in sys.argv[1:] if a.startswith("--tail-ms=")), 0.0)
    data, symfs, comm = args[0], args[1], args[2]
    cpu = float(args[3]) if len(args) > 3 else None
    start = tail_start(data, symfs, tail)
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
        if comm not in sample.thread_comm or sample.time < start:
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
