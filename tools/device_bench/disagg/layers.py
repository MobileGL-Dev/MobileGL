#!/usr/bin/env python3
"""Split one thread of a simpleperf profile by LAYER, in ms per frame.

    python layers.py <perf.data> <binary_cache> <thread comm> <cpu_ms_per_frame> [--tail-ms=N]

Each sample goes to the layer of the first frame, walking from the leaf toward the root, that
belongs to one: the GPU driver (vendor GLES/Vulkan libraries, kgsl), the backend (MG_Backend),
the applier (MGPipeApply*, the record sink, the wire decoder), the client half (the frontend,
the validate point and its emitters, the verb port, the wire encoder), or the replay harness
(apitrace parsing and dispatch). Library code with no layer of its own (malloc, memcpy, atomics)
lands in the layer that called it. Kernel frames count as the driver only below a driver frame.
"""
import os
import sys
from collections import Counter

sys.path.insert(0, os.environ.get("SIMPLEPERF_DIR", ""))
from simpleperf_report_lib import ReportLib  # noqa: E402

LAYERS = ["driver", "backend", "applier", "client half", "harness", "other"]


def layer_of(symbol, dso):
    if any(k in dso for k in ("adreno", "libGLESv2", "libEGL_", "vulkan.", "libgsl", "libllvm-glnext", "msm_kgsl")):
        return "driver"
    if "MG_Backend::" in symbol:
        return "backend"
    if ("MGPipeApply" in symbol or "RecordVerbSink" in symbol or "PipeWireDecoder" in symbol
            or "PipeApplier::" in symbol or "ServerLoop::" in symbol):
        return "applier"
    if any(k in symbol for k in ("MG_Pipe::", "MG_Impl::", "MG_Record::", "MG_State::", "MG_Remote::Client",
                                  "PipeWireEncoder", "MG_Util::")) or "libMobileGL" in dso and symbol.startswith("gl"):
        return "client half"
    if any(k in symbol for k in ("trace::", "retrace", "Brotli", "mobilegl_apitrace", "mobilegl_trace")):
        return "harness"
    return None


def tail_start(data, symfs, tail_ms):
    if not tail_ms:
        return 0
    lib = ReportLib()
    lib.SetRecordFile(data)
    lib.SetSymfs(symfs)
    last = 0
    while True:
        sample = lib.GetNextSample()
        if sample is None:
            break
        chain = lib.GetCallChainOfCurrentSample()
        if any("retrace::retraceCall" in chain.entries[i].symbol.symbol_name for i in range(chain.nr)):
            last = max(last, sample.time)
    return last - int(tail_ms * 1e6)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    tail = next((float(a.split("=", 1)[1]) for a in sys.argv[1:] if a.startswith("--tail-ms=")), 0.0)
    data, symfs, comm, cpu = args[0], args[1], args[2], float(args[3])
    start = tail_start(data, symfs, tail)
    lib = ReportLib()
    lib.SetRecordFile(data)
    lib.SetSymfs(symfs)
    counts = Counter()
    detail = Counter()
    detail_layer = next((a.split("=", 1)[1] for a in sys.argv[1:] if a.startswith("--detail=")), None)
    total = 0
    while True:
        sample = lib.GetNextSample()
        if sample is None:
            break
        if comm not in sample.thread_comm or sample.time < start:
            continue
        total += 1
        leaf = lib.GetSymbolOfCurrentSample()
        frames = [(leaf.symbol_name, leaf.dso_name)]
        chain = lib.GetCallChainOfCurrentSample()
        frames += [(chain.entries[i].symbol.symbol_name, chain.entries[i].symbol.dso_name) for i in range(chain.nr)]
        found = None
        owner = None
        for symbol, dso in frames:
            if "kallsyms" in dso:
                continue
            found = layer_of(symbol, dso)
            if found:
                owner = symbol
                break
        counts[found or "other"] += 1
        if detail_layer and found == detail_layer:
            # the outermost frame of the same layer below the first non-layer caller: the entry
            # into that layer, e.g. MGPipeValidateForVerb or EmitDrawRecord for the client half
            names = [f for f, d in frames if layer_of(f, d) == found]
            detail[owner.split("(")[0][-90:] + "  <-  " + names[-1].split("(")[0][-60:]] += 1
    print(f"{comm}: {total} samples, {cpu:.2f} ms/frame")
    for name in LAYERS:
        share = counts[name] / max(1, total)
        print(f"  {name:12} {100 * share:5.1f}%  {cpu * share:6.2f} ms/frame")
    for key, n in detail.most_common(30):
        print(f"    {cpu * n / max(1, total):6.3f} ms  {key}")


if __name__ == "__main__":
    main()
