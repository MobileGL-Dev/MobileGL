// MobileGL - MobileGL/MG_IntegrationTest/Harness/WireLedgerChecks.h
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// THE TWO ASSERTIONS THE WIRE PRODUCER'S LEDGER MAKES POSSIBLE, in one place so the scenarios
// that carry them cannot drift apart on what the numbers mean.
//
// R-10 (ExpectMaxRecordBytesUnderCap). P5 does no chunking and must instead PROVE it never
// needs any: no record may exceed RingProducer::MaxRecordBytes() == MOBILEGL_IPC_RING_MB / 2.
// Half of that proof is already a Fatal - PipeWireCodec.cpp aborts Fatal{RingOverrun} on a
// record ABOVE the cap - and it is the loud half. The quiet half is the one this assertion
// covers: the phase has to publish the MAXIMUM ACTUALLY SEEN on a real workload, so that a
// record creeping towards the cap is visible before the day it crosses. Until this landed the
// only readers of PipeWireEncoder::MaxRecordBytesSeen() were codec unit cases over synthetic
// records, and the joint gate recorded BRIEF 8 item 3 as "Maximum record bytes: NO MEASUREMENT"
// (joint-v1.md 5).
//
// R-9 / exit gate E3(e) (ExpectSmallRingWrappedAtLeastOnce). The SmallRing lane exists to run
// the reduced path over a ring small enough to force the wrap path - the kRecPad filler that
// both sides must SKIP WITHOUT ADVANCING seq, which is R-9's last clause and the one piece of
// ring behaviour no other lane reaches. It ran green from the day it was registered and proved
// nothing, because nobody counted: measured on the joint head, a whole split scenario writes
// about 40 KiB into SEG_CMD (emitseq 22-71, maxrec 784 bytes), so a 1 MiB "small" ring is 25
// times larger than the traffic and the head never comes near its wrap boundary. The lane and
// the default lane were the same run under two names - which is exactly the shape ID-53's own
// comment warned about and t1-v1.md carried as a debt ("WHAT THIS LANE DOES NOT YET ASSERT").
//
// So the lane now DRIVES ENOUGH WORK to overrun its own ring before it asserts, and the
// assertion prints the denominator with the count: "0 wraps" is a defect after 1.25 MiB through
// a 1 MiB ring and a tautology after 40 KiB, and a bare `EXPECT_GE(wraps, 1)` cannot tell those
// apart. The driving is ordinary GL - clears and draws through the scenario's own objects - and
// not a hand-built record: R-16 forbids an assertion that constructs the state it observes, and
// a test that called Reserve directly would be asserting that the ring wraps, not that the
// WORKLOAD makes it wrap.
//
// WHAT IS ASSERTED IS THE HEAD GOING ROUND, NOT THE kRecPad FILLER, and that distinction was
// forced by a measurement rather than chosen: the first cut of this assertion read the pad
// count, and 1310824 bytes of clears and draws through a 1 MiB SEG_CMD produced one and a half
// trips round the ring and ZERO pads. The reason is arithmetic, not a defect - a workload whose
// records repeat at a uniform stride that divides a power-of-two capacity lands on the boundary
// exactly, every time - so a gate written against the pad count would have been red for the
// sizes in the record catalogue and green the day one of them changed. The pad count is
// RECORDED beside the wrap count (ringpads= on the stats line, in the JUnit properties, and in
// the session's teardown ledger) so the number is available without being load-bearing.

#pragma once

#include <string>

#include <gtest/gtest.h>

#include "SplitLane.h"
#include "SplitRuntimePeek.h"

namespace MGITest::WireLedger {

    // The SmallRing lane declares MOBILEGL_IPC_RING_MB=1 (MG_IntegrationTest/CMakeLists.txt;
    // 1 MiB is ConfigLoader's floor for it). The byte target below is that size plus a
    // quarter: enough that the head MUST have crossed the wrap boundary, and far enough under
    // the DEFAULT 8 MiB ring that the same workload cannot wrap there - which is what makes
    // "raise the ring back to the default and this assertion goes red" a real control rather
    // than a description.
    inline constexpr unsigned long long kSmallRingLaneCmdByteTarget = (5ull << 20) / 4; // 1.25 MiB

    // A bound on the drive loop, so a lane whose records shrank cannot spin forever. It is
    // generous on purpose: the loop's exit condition is the BYTE COUNT, and this only turns an
    // infinite loop into a named failure.
    inline constexpr unsigned int kSmallRingLaneMaxIterations = 200000u;

    // R-10's reading, for any split lane. `where` names the case, because the number is a
    // MEASUREMENT this phase has to publish and a reader needs to know which workload produced
    // it.
    inline void ExpectMaxRecordBytesUnderCap(const char* where) {
        const SplitRuntimeState state = PeekSplitRuntime();
        ASSERT_TRUE(state.sessionActive)
            << "the wire ledger was read in a process with no client session; the caller must "
               "pass SplitLane::SkipReasonForSplitOnlyAssertions() first, because every field of "
               "this ledger is 0 there and 0 is also a legal measurement";
        // Not merely "under the cap": a maximum of ZERO means the case emitted no record at all,
        // which satisfies `< cap` perfectly and is the exact shape of an emit table that
        // resolved the transport and then fell through to the driver.
        EXPECT_GT(state.maxRecordBytes, 0u)
            << where << ": the largest record this session wrote is 0 bytes, so nothing crossed "
                        "SEG_CMD. R-10's proof obligation has no subject and the lane did not go "
                        "through the wire";
        EXPECT_LT(state.maxRecordBytes, state.maxRecordBytesCap)
            << where << ": R-10 - the largest record this session wrote is " << state.maxRecordBytes
            << " bytes and RingProducer::MaxRecordBytes() is " << state.maxRecordBytesCap
            << " (half of a " << (state.maxRecordBytesCap * 2)
            << " byte SEG_CMD, i.e. MOBILEGL_IPC_RING_MB). P5 does NOT chunk: a record at or "
               "above the cap is Fatal{RingOverrun} at the encoder, and a maximum that has "
               "climbed to it is the proof obligation failing. Report it to the integrator, who "
               "decides between early chunking (P8) and a bigger default ring";
        ::testing::Test::RecordProperty("max_record_bytes",
                                        static_cast<int>(state.maxRecordBytes));
        ::testing::Test::RecordProperty("max_record_bytes_cap",
                                        static_cast<int>(state.maxRecordBytesCap));
    }

    // E3(e)'s reading. The CALLER drives the workload; this only reads the result, so that the
    // thing being asserted about is the workload and not this header.
    inline void ExpectSmallRingWrappedAtLeastOnce(const char* where,
                                                  unsigned long long bytesDriven) {
        const SplitRuntimeState state = PeekSplitRuntime();
        ASSERT_TRUE(state.sessionActive)
            << "the wire ledger was read in a process with no client session";
        const unsigned long long capacity = state.maxRecordBytesCap * 2; // MaxRecordBytes == cap/2
        // THE WRAP FIRST, AND THE DENOMINATOR RIGHT BEHIND IT, both as EXPECT so that a red
        // carries both sentences. Order matters for what the failure SAYS: the thing this gate
        // is about is the missing wrap, and "the loop pushed fewer bytes than the ring holds"
        // is the EXPLANATION for it, not a different failure. An ASSERT on the denominator
        // would print only the explanation and the reader would have to infer the gate - which
        // is how the red-once line for this control was measured, and why it is written this
        // way round.
        EXPECT_GE(state.cmdWraps, 1u)
            << where << ": exit gate E3(e) - " << bytesDriven << " bytes were written into a "
            << capacity
            << " byte SEG_CMD and the producer's head NEVER WENT ROUND: no wrap, so this entry "
               "exercised exactly what the default lane exercises and the word SmallRing in its "
               "name asserts nothing. That is what the joint gate recorded as 'SmallRing entries "
               "ran, but no back-pressure wait count was measured' (joint-v1.md 6). The usual "
               "cause is the ring: MOBILEGL_IPC_RING_MB did not reach this process, or the lane "
               "was given the DEFAULT 8 MiB ring - which is exactly how this assertion was "
               "proved to be load-bearing (R-16), by re-running this entry's own command with "
               "MOBILEGL_IPC_RING_MB=8 and nothing else changed";
        EXPECT_GT(bytesDriven, capacity)
            << where << ": and the reason is the denominator - the drive loop pushed only "
            << bytesDriven << " bytes through a " << capacity
            << " byte SEG_CMD, which cannot reach a wrap boundary at all. The loop stops at "
               "kSmallRingLaneCmdByteTarget, which is sized for the 1 MiB ring this lane "
               "declares (MGL_ITEST_GLES_SPLIT_SMALL_RING_ENVIRONMENT); a larger ring needs a "
               "larger workload and is not what this lane is for";
        EXPECT_GE(state.stageReclaimWaits, 1u)
            << where << ": exit gate E3(e) - producer NEVER WAITED for staging retirement; "
               "lazy reclamation of already-retired bytes is not back-pressure";
        ::testing::Test::RecordProperty("ring_wraps", static_cast<int>(state.cmdWraps));
        ::testing::Test::RecordProperty("ring_wrap_pads", static_cast<int>(state.cmdWrapPads));
        ::testing::Test::RecordProperty("ring_waits", static_cast<int>(state.stageReclaimWaits));
        ::testing::Test::RecordProperty("cmd_bytes_written", static_cast<int>(bytesDriven));
    }

    // Bytes the producer has written so far, or 0 outside a split process.
    inline unsigned long long CmdBytesWritten() { return PeekSplitRuntime().cmdBytesWritten; }

} // namespace MGITest::WireLedger
