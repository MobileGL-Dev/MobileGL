#!/usr/bin/env python3
"""Split one function's inclusive samples by its direct callees (and by malloc/atomic leaves).

    python callee_split.py <perf.data> <binary_cache> <function substring> [--depth 1] [--thread NAME]

Uses the NDK's simpleperf_report_lib (set SIMPLEPERF_DIR to <ndk>/simpleperf). For every sample
whose call chain contains a frame matching <function substring>, attributes the sample to the
frame directly below it (its callee), or to "<self>" when it is the leaf. Prints the percentage
of all samples of the selected threads.
"""
import argparse
import os
import sys
from collections import Counter

sys.path.insert(0, os.environ.get("SIMPLEPERF_DIR", ""))
from simpleperf_report_lib import ReportLib  # noqa: E402


def short(name):
    name = name.split("(")[0]
    return name.replace("MobileGL::", "").replace("MG_Backend::DirectVulkan::", "DV::")[-110:]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("data")
    ap.add_argument("symfs")
    ap.add_argument("function")
    ap.add_argument("--depth", type=int, default=1)
    ap.add_argument("--thread", default=None)
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--leaf", action="store_true",
                    help="match <function> against the LEAF only and attribute to the nearest caller "
                         "outside the allocator/libc/atomics (who called malloc?)")
    args = ap.parse_args()
    lib = ReportLib()
    lib.SetRecordFile(args.data)
    lib.SetSymfs(args.symfs)
    total = 0
    hit = 0
    callees = Counter()
    while True:
        sample = lib.GetNextSample()
        if sample is None:
            break
        if args.thread and args.thread not in sample.thread_comm:
            continue
        total += 1
        # frames leaf first: the sample's own symbol, then callchain entries (callers)
        frames = [lib.GetSymbolOfCurrentSample().symbol_name]
        chain = lib.GetCallChainOfCurrentSample()
        for i in range(chain.nr):
            frames.append(chain.entries[i].symbol.symbol_name)
        if args.leaf:
            if args.function not in frames[0]:
                continue
            hit += 1
            noise = ("scudo", "malloc", "free", "operator new", "operator delete", "__aarch64_", "@plt",
                     "memmove", "memcpy", "memset", "__emutls", "pthread_", "shared_count", "__release_shared",
                     "!!!", "std::__ndk1::")
            owner = next((f for f in frames[1:] if not any(n in f for n in noise)), "<none>")
            callees[short(owner)] += 1
            continue
        # find the outermost (closest to root) matching frame so recursion counts once
        idx = None
        for i in range(len(frames) - 1, -1, -1):
            if args.function in frames[i]:
                idx = i
                break
        if idx is None:
            continue
        hit += 1
        if idx == 0:
            callees["<self>"] += 1
            continue
        key = " > ".join(short(frames[j]) for j in range(idx - 1, max(idx - 1 - args.depth, -1), -1))
        callees[key] += 1
    print(f"samples={total} in '{args.function}'={hit} ({100.0 * hit / max(1, total):.1f}%)")
    for key, n in callees.most_common(args.top):
        print(f"{100.0 * n / max(1, total):6.2f}%  {key}")


if __name__ == "__main__":
    main()
