// MobileGL - MobileGL/MG_Remote/Transport/RoleMemory.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// Peak-RSS accounting for the two roles. Owner: package s1; CONSUMER: package t1,
// which puts the numbers in MEASUREMENTS.
//
// WHY BOTH HALVES ARE NEEDED, AND WHY NEITHER ALONE IS THE ANSWER.
//
//   VmHWM is the kernel's own high-water mark of resident set size, in
//   /proc/self/status. It is the only number that cannot be argued with - it
//   counts what the process actually touched, including the pages the allocator
//   never gave back. But under `inproc` BOTH ROLES ARE ONE PROCESS, so a single
//   VmHWM cannot be split between them and reporting it as "the client's" would
//   be a lie that only becomes visible in P6.
//
//   The segment ledger is the other half: every ShmSegment this process mapped,
//   by kind and by role. It is exact, it IS separable by role, and under `spawn`
//   it is the part that appears in both processes at once (one mapping, two
//   address spaces, one set of physical pages) - which is precisely the number a
//   naive "sum the two VmHWMs" double-counts.
//
// So the pair is the measurement: VmHWM for what the process really cost, the
// ledger for how much of it is shared mapping that a second process will not pay
// for again. t1 reports both, per role, and the split's memory claim is
// (client VmHWM + server VmHWM - shared ledger), never either half on its own.
//
// A SAMPLE IS A SYSCALL AND A PARSE. Take it at phase boundaries - after the
// handshake, after the first frame, at teardown - never per record.

#pragma once

#include <atomic>
#include <cstdint>

namespace MobileGL::MG_Remote::Transport {

    enum class MemoryRole : std::uint32_t {
        Client = 0,
        Server = 1,
        kMemoryRoleCount = 2,
    };

    // Resident-set high-water mark of THIS PROCESS in bytes, from
    // /proc/self/status's VmHWM line. 0 when the platform has no such file
    // (Windows, and Android's /proc is readable but the caller should still
    // treat 0 as "not measured" rather than "measured zero").
    std::uint64_t ProcessPeakRssBytes();

    // Current resident set (VmRSS), same source and same 0 convention. Sampled
    // beside the peak so a phase that never grew the peak is distinguishable
    // from one that was not sampled.
    std::uint64_t ProcessCurrentRssBytes();

    // BOTH, FROM ONE PASS OVER /proc/self/status. The kernel does not keep
    // hiwater_rss up to date on growth - it stores it only when RSS is about to
    // DROP, and task_mem() reports max(stored hiwater, rss-at-this-read) - so
    // two separate reads are not comparable: the fopen of the second read can
    // itself grow RSS past the VmHWM the first read reported. That is exactly
    // what GitHub run 35079459114 caught (VmHWM 4,784,128 < VmRSS 4,849,664 on
    // ubuntu-24.04) and what ~/w7/p5-s1-probe reproduced locally: two reads
    // disagree by 128 KiB with nothing allocated between them, one read never
    // does (0/1000 with 64 KiB of growth per iteration). One pass is therefore
    // the only way to read a pair, and even then the pair is a snapshot, not a
    // bound - see SampleRoleMemoryInto.
    void ProcessRssBytes(std::uint64_t* outPeakBytes, std::uint64_t* outCurrentBytes);

    // The ledger. ShmSegment does NOT update it itself: a segment is also created
    // by tests and by P6's adopt path, and a ledger that counted those would stop
    // meaning "this session's footprint". The SESSION books its own segments.
    void LedgerAddSegment(MemoryRole role, std::uint64_t bytes);
    void LedgerRemoveSegment(MemoryRole role, std::uint64_t bytes);
    std::uint64_t LedgerMappedBytes(MemoryRole role);
    // Every role's mapped bytes. Under inproc the two roles map THE SAME pages,
    // so this over-counts on purpose: the two per-role numbers are what t1
    // subtracts with, and a single total that silently deduplicated them would
    // hide exactly the spawn-vs-inproc difference the measurement is for.
    std::uint64_t LedgerMappedBytesAllRoles();

    // One sample, both halves, for one role.
    //
    // PeakRssBytes IS THE LEDGER'S OWN RUNNING MAXIMUM, not the kernel's VmHWM.
    // It is the largest of every kernel peak and every kernel current this
    // process has folded in through SampleRoleMemoryInto, so it is >= this
    // sample's CurrentRssBytes BY CONSTRUCTION and never decreases. The kernel's
    // VmHWM feeds it (it is a lower bound on the true peak that this process's
    // own samples may have missed) but is never reported on its own: as
    // ProcessRssBytes explains, the kernel's peak is not a monotone bound on the
    // kernel's current at read time, so a sample that reported VmHWM verbatim
    // could show a peak below its own current - which is a number t1 cannot put
    // in MEASUREMENTS and a test cannot assert against. GitHub run 35079459114.
    struct RoleMemorySample {
        std::uint64_t PeakRssBytes = 0;
        std::uint64_t CurrentRssBytes = 0;
        std::uint64_t MappedSegmentBytes = 0; // this role's ledger
        MemoryRole Role = MemoryRole::Client;
    };

    // One pass over /proc/self/status folded into the PROCESS's running peak.
    RoleMemorySample SampleRoleMemory(MemoryRole role);

    // The fold itself, over a running peak the CALLER owns: `runningPeak`
    // becomes max(runningPeak, kernelPeakRssBytes, kernelCurrentRssBytes) and
    // the sample reports that as PeakRssBytes beside `kernelCurrentRssBytes`.
    // SampleRoleMemory calls this with the process-wide running peak and the
    // numbers it just read; SessionTest calls it with a running peak of its own
    // and a stubbed reader whose current EXCEEDS its peak, which must not fail
    // (R-16's control on this rule: revert PeakRssBytes to the kernel's peak and
    // it does).
    RoleMemorySample SampleRoleMemoryInto(std::atomic<std::uint64_t>& runningPeak, MemoryRole role,
                                          std::uint64_t kernelPeakRssBytes,
                                          std::uint64_t kernelCurrentRssBytes);

    // Emits one line at INFO level, which is what every P5 lane builds at, so
    // t1's harness can grep it out of a lane log without a new log sink. Not
    // DEBUG, which the INFO build compiles out; not ERROR, which this is not.
    // The grep tag is `MG_Remote memory[`.
    // `phase` is a short tag: "handshake", "first-frame", "teardown".
    void LogRoleMemory(const char* phase, const RoleMemorySample& sample);

} // namespace MobileGL::MG_Remote::Transport
