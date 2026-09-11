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
    struct RoleMemorySample {
        std::uint64_t PeakRssBytes = 0;
        std::uint64_t CurrentRssBytes = 0;
        std::uint64_t MappedSegmentBytes = 0; // this role's ledger
        MemoryRole Role = MemoryRole::Client;
    };

    RoleMemorySample SampleRoleMemory(MemoryRole role);

    // Emits one line at ERROR level (the wire layer's only level - WireLog.h) so
    // t1's harness can grep it out of a lane log without a new log sink.
    // `phase` is a short tag: "handshake", "first-frame", "teardown".
    void LogRoleMemory(const char* phase, const RoleMemorySample& sample);

} // namespace MobileGL::MG_Remote::Transport
