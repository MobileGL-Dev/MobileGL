// MobileGL - MobileGL/MG_Test/Wire/SessionTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The ring-owning session pair: four ShmSegment-backed segments, two real threads over the
// SEG_CMD ring, the five watermarks with real writers, the SEG_REPLY slot pool, the SEG_EVENT
// reverse channel, and a shutdown that a lost wakeup turns RED rather than hanging.
//
// WHY THIS SUITE EXISTS ALONGSIDE RingTest. RingTest pins the ring's own mechanics against a
// fixture whose control page is on the stack and whose byte area is a std::vector, and it pins
// the five watermark RULES against nobody, because until P5 nothing in the tree wrote one
// (every watermark was declared, zeroed by InitRingControl and written by no code at all).
// This suite pins the WRITERS: SessionProducer, SessionConsumer and the Watermark namespace are
// the only things that advance them, and they do it over memory that came from ShmSegment.
//
// AND THAT LAST PART IS THE POINT. `inproc` allocating its rings with new[] would work, would
// be shorter, and would move every question about mapping, alignment, size rounding and
// lifetime into P6 - onto the day the second process appears. So the session uses ShmSegment in
// both delivery modes and SessionSegmentsAreRealSharedMemory below is the mechanical check that
// it still does.

#include <MG_Remote/Protocol/generated/protocol_generated.h>
#include <MG_Remote/Transport/Doorbell.h>
#include <MG_Remote/Transport/EventRing.h>
#include <MG_Remote/Transport/InProcessTransport.h>
#include <MG_Remote/Transport/ReplySlot.h>
#include <MG_Remote/Transport/Ring.h>
#include <MG_Remote/Transport/RoleMemory.h>
#include <MG_Remote/Transport/SessionRings.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace MobileGL::MG_Remote::Transport;

namespace {

    // Small enough to be cheap in CI, large enough that 20 000 sixteen-byte records wrap the
    // command ring many times over - which is what puts the kRecPad rule under load rather
    // than under a contrived single wrap.
    SessionSegmentSizes TestSizes() {
        // RING sizes. The SEG_CMD and SEG_EVENT segments are each one control page bigger.
        SessionSegmentSizes sizes;
        sizes.CmdRingBytes = 32ull * 1024;
        sizes.StageBytes = 48ull * 1024;
        sizes.ReplyBytes = 64ull * 1024; // -> 8 slots of 8 KiB
        sizes.EventRingBytes = 16ull * 1024;
        sizes.ReplySlotCount = 8;
        return sizes;
    }

    // One session's worth of everything, wired the way ClientSession and ServerSession wire it:
    // the server owns the segments, the client attaches to the same mapping, and the two
    // doorbells come from the transport while the RINGS come from here.
    struct SessionFixture {
        std::unique_ptr<InProcessTransport> clientTransport;
        std::unique_ptr<InProcessTransport> serverTransport;
        SessionSegments serverSegments;
        SessionSegments clientSegments;
        RingProducer cmdProducer;
        // NO stageProducer and NO stage consumer: SEG_STAGE is w1's encoder-local linear
        // allocator and RingCursorSet::Stage is driven by nobody (Ring.h's stage triple).
        RingConsumer cmdConsumer;
        SessionProducer producer;
        SessionConsumer consumer;
        ReplySlotPool replies;
        EventRingProducer eventOut;
        EventRingConsumer eventIn;

        bool Build(const SessionSegmentSizes& sizes) {
            InProcessTransport::CreatePair(clientTransport, serverTransport);
            if (serverSegments.Create(sizes, MemoryRole::Server) != MOBILEGL_OK) {
                return false;
            }
            if (clientSegments.AttachInProcess(serverSegments, MemoryRole::Client) != MOBILEGL_OK) {
                return false;
            }
            // EACH ROLE DRIVES ITS OWN MAPPING. Under inproc AttachInProcess dups the
            // owner's descriptors and maps them again, so the client's RingControl is a
            // different VIRTUAL address over the same physical page - which is exactly the
            // shape spawn has, and the reason the fixture does not share one pointer.
            RingControl* clientControl = clientSegments.CmdControl();
            RingControl* serverControl = serverSegments.CmdControl();
            cmdProducer = RingProducer(clientControl, clientSegments.CmdRingBase(),
                                       clientSegments.CmdRingCapacity(), RingCursorSet::Cmd);
            cmdConsumer = RingConsumer(serverControl, serverSegments.CmdRingBase(),
                                       serverSegments.CmdRingCapacity(), RingCursorSet::Cmd);
            if (!cmdProducer.Valid() || !cmdConsumer.Valid()) {
                return false;
            }
            // PeerDoorbell is the bell the OTHER end parks on; SelfDoorbell is this end's own.
            // Which is which is the session's knowledge, never ITransport's (contract §3.9).
            producer.Attach(clientControl, &cmdProducer, &clientTransport->PeerDoorbell(),
                            &clientTransport->SelfDoorbell(), kDefaultSpinUs);
            consumer.Attach(serverControl, &cmdConsumer, &serverTransport->PeerDoorbell(),
                            &serverTransport->SelfDoorbell(), kDefaultSpinUs);
            replies = ReplySlotPool(serverSegments.ReplyBase(), serverSegments.ReplyBytes(),
                                    serverSegments.ReplySlotCount());
            replies.Clear();
            eventOut = EventRingProducer(serverSegments.EventControl(), serverControl,
                                         serverSegments.EventRingBase(),
                                         serverSegments.EventRingCapacity());
            eventIn = EventRingConsumer(clientSegments.EventControl(), clientControl,
                                        clientSegments.EventRingBase(),
                                        clientSegments.EventRingCapacity(),
                                        clientSegments.EventSegmentBase());
            return replies.Valid() && eventOut.Valid() && eventIn.Valid();
        }

        RingControl& Control() { return *serverSegments.CmdControl(); }
    };

} // namespace

// ---------------------------------------------------------------------------
// The segments themselves
// ---------------------------------------------------------------------------

// `inproc` must not quietly become new[]. A descriptor (POSIX) or a native handle (Windows) is
// the mechanical difference between a session whose spawn path is the same code and one whose
// spawn path is written for the first time in P6.
TEST(SessionTest, SessionSegmentsAreRealSharedMemoryAndNotAHeapAllocation) {
    SessionSegments segments;
    ASSERT_EQ(segments.Create(TestSizes(), MemoryRole::Server), MOBILEGL_OK);
    EXPECT_TRUE(segments.Valid());
#if !defined(_WIN32)
    EXPECT_GE(segments.DescriptorFor(SessionSegmentSlot::Cmd), 0);
    EXPECT_GE(segments.DescriptorFor(SessionSegmentSlot::Stage), 0);
    EXPECT_GE(segments.DescriptorFor(SessionSegmentSlot::Reply), 0);
    EXPECT_GE(segments.DescriptorFor(SessionSegmentSlot::Event), 0);
#endif
    // Both control pages start initialised, with the two generations at 1 and every watermark
    // at 0 - the two conventions are opposite on purpose (Ring.h).
    ASSERT_NE(segments.CmdControl(), nullptr);
    ASSERT_NE(segments.EventControl(), nullptr);
    EXPECT_EQ(segments.CmdControl()->ringGeneration.load(), 1u);
    EXPECT_EQ(segments.EventControl()->ringGeneration.load(), 1u);
    EXPECT_EQ(segments.CmdControl()->appliedSeq.load(), 0u);
    segments.Close();
    EXPECT_FALSE(segments.Valid());
}

// The knob names the RING and the segment carries one control page on top, so CONTRACT-P5 §5
// and Config.h's "A RECORD MAY BE AT MOST HALF OF THIS, so 8 MiB caps one record at 4 MiB" are
// true as written. Getting this the other way round - an 8 MiB segment with a 4 MiB ring -
// made both of those documents false and left half of SEG_CMD mapped and unreachable.
TEST(SessionTest, TheKnobNamesTheRingAndTheSegmentAddsOneControlPage) {
    EXPECT_EQ(LargestPowerOfTwoAtMost(0u), 0u);
    EXPECT_EQ(LargestPowerOfTwoAtMost(1u), 1u);
    EXPECT_EQ(LargestPowerOfTwoAtMost(4095u), 2048u);
    EXPECT_EQ(LargestPowerOfTwoAtMost(4096u), 4096u);
    EXPECT_EQ(LargestPowerOfTwoAtMost(4097u), 4096u);

    constexpr std::uint64_t kCmdRing = 8ull * 1024 * 1024;
    constexpr std::uint64_t kEventRing = 256ull * 1024;
    EXPECT_EQ(SegmentBytesForRing(kCmdRing), kCmdRing + sizeof(RingControl));
    EXPECT_EQ(SegmentBytesForRing(kEventRing), kEventRing + sizeof(RingControl));
    // Round trip: the capacity inside a segment SegmentBytesForRing produced is the ring
    // that was asked for, exactly.
    EXPECT_EQ(RingCapacityForSegment(SegmentBytesForRing(kCmdRing)), kCmdRing);
    EXPECT_EQ(RingCapacityForSegment(SegmentBytesForRing(kEventRing)), kEventRing);
    // A ring size that is not a power of two is rounded DOWN rather than silently producing a
    // segment whose tail can never be addressed.
    EXPECT_EQ(SegmentBytesForRing(6ull * 1024 * 1024), 4ull * 1024 * 1024 + sizeof(RingControl));

    // ... and therefore the real cap on one record, which R-10 obliges the codec to prove it
    // never approaches: half of MOBILEGL_IPC_RING_MB, which is what §5 says.
    RingControl control{};
    InitRingControl(control);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(kCmdRing));
    RingProducer producer(&control, bytes.data(), kCmdRing, RingCursorSet::Cmd);
    ASSERT_TRUE(producer.Valid());
    EXPECT_EQ(producer.MaxRecordBytes(), 4ull * 1024 * 1024);

    // A segment that cannot hold the control page plus the smallest ring has NO ring, rather
    // than a ring of some rounded-down nonsense.
    EXPECT_EQ(RingCapacityForSegment(sizeof(RingControl)), 0u);
    EXPECT_EQ(RingCapacityForSegment(sizeof(RingControl) + 8), 0u);
    EXPECT_EQ(SegmentBytesForRing(8u), 0u);
}

// The default geometry really allocates. The four RINGS are the four contract numbers; the two
// segments that carry a control page announce that much more, because sizeBytes is what a spawn
// peer must mmap.
TEST(SessionTest, TheDefaultGeometryIsTheFourContractRingSizes) {
    SessionSegments segments;
    ASSERT_EQ(segments.Create(SessionSegmentSizes{}, MemoryRole::Server), MOBILEGL_OK);
    EXPECT_EQ(segments.CmdRingCapacity(), 8ull * 1024 * 1024);
    EXPECT_EQ(segments.StageBytes(), 32ull * 1024 * 1024);
    // ID-47: 16 MiB, eight slots of 2 MiB. ProtocolSmokeTest pins the same number on the wire.
    EXPECT_EQ(segments.ReplyBytes(), 16ull * 1024 * 1024);
    EXPECT_EQ(segments.EventRingCapacity(), 256ull * 1024);

    EXPECT_EQ(segments.AnnouncedSize(SessionSegmentSlot::Cmd),
              8ull * 1024 * 1024 + sizeof(RingControl));
    // SEG_STAGE is not a ring at all - no control page, no cursor triple, no power-of-two
    // rounding - and neither is SEG_REPLY, so both announce exactly what was asked for.
    EXPECT_EQ(segments.AnnouncedSize(SessionSegmentSlot::Stage), 32ull * 1024 * 1024);
    EXPECT_EQ(segments.AnnouncedSize(SessionSegmentSlot::Reply), 16ull * 1024 * 1024);
    EXPECT_EQ(segments.AnnouncedSize(SessionSegmentSlot::Event),
              256ull * 1024 + sizeof(RingControl));
    segments.Close();
}

// ID-47 (ID-46 finding 2). The DEFAULT reply pool must hold the largest P5 read: the E2 retrace
// harness snapshots OpenRA's whole 640x480 surface as GL_RGBA/GL_UNSIGNED_BYTE through the
// interposer, 1,228,800 bytes, and the previous geometry (8 MiB / 8 slots, 1 MiB minus a 16-byte
// header) refused it by 180,240 bytes - and refused the 512x512 RGBA8 read s1-v1.md:134 claimed
// it covered, by sixteen. This is the verifier's case, committed: it posts exactly 640*480*4
// bytes into a pool built from SessionSegmentSizes{} and reads them back. RED ONCE by reverting
// SessionSegmentSizes::ReplyBytes to 8 MiB: Post then takes its oversize-abort branch and the
// case dies, which is the verifier's original outcome. That perturbation was run.
TEST(SessionTest, TheDefaultReplyGeometryHoldsTheLargestP5Read) {
    SessionSegments segments;
    ASSERT_EQ(segments.Create(SessionSegmentSizes{}, MemoryRole::Server), MOBILEGL_OK);
    ReplySlotPool pool(segments.ReplyBase(), segments.ReplyBytes(), segments.ReplySlotCount());
    ASSERT_TRUE(pool.Valid());
    EXPECT_EQ(pool.SlotCount(), 8u);
    EXPECT_EQ(pool.SlotBytes(), 2u * 1024 * 1024);
    EXPECT_EQ(pool.MaxReplyBytes(), 2u * 1024 * 1024 - 16u);

    const std::uint64_t kOpenRaSnapshot = 640ull * 480 * 4; // E2's read, the largest in P5
    const std::uint64_t kHalfKSquare = 512ull * 512 * 4;    // the read s1-v1.md:134 got wrong
    EXPECT_TRUE(pool.CanHold(kOpenRaSnapshot));
    EXPECT_TRUE(pool.CanHold(kHalfKSquare));

    std::vector<std::uint8_t> answer(static_cast<std::size_t>(kOpenRaSnapshot));
    for (std::size_t i = 0; i < answer.size(); ++i) {
        answer[i] = static_cast<std::uint8_t>(i * 7 + (i >> 12));
    }
    pool.Post(1, kReplyStatusOk, answer.data(), answer.size());

    std::vector<std::uint8_t> back(answer.size(), 0);
    std::int32_t status = -1;
    std::uint64_t size = 0;
    ASSERT_TRUE(pool.Read(1, back.data(), back.size(), &status, &size))
        << "the E2 snapshot does not fit the default reply pool";
    EXPECT_EQ(status, kReplyStatusOk);
    EXPECT_EQ(size, kOpenRaSnapshot);
    EXPECT_EQ(std::memcmp(back.data(), answer.data(), answer.size()), 0);

    // And the 512x512 read, in the next slot, so a geometry that only just clears 640x480 by
    // some accident of rounding cannot pass this case either.
    answer.resize(static_cast<std::size_t>(kHalfKSquare));
    pool.Post(2, kReplyStatusOk, answer.data(), answer.size());
    back.assign(answer.size(), 0);
    ASSERT_TRUE(pool.Read(2, back.data(), back.size(), &status, &size));
    EXPECT_EQ(size, kHalfKSquare);
    EXPECT_EQ(std::memcmp(back.data(), answer.data(), answer.size()), 0);
    segments.Close();
}

// The ledger is per role and DOES double-count under inproc, deliberately: the two roles map
// the same pages here and will not under spawn, so the per-role numbers are what t1 subtracts
// with and a silently deduplicated total would hide exactly that difference.
TEST(SessionTest, TheMemoryLedgerIsPerRoleAndIsReleasedOnClose) {
    const std::uint64_t clientBefore = LedgerMappedBytes(MemoryRole::Client);
    const std::uint64_t serverBefore = LedgerMappedBytes(MemoryRole::Server);
    {
        SessionFixture session;
        ASSERT_TRUE(session.Build(TestSizes()));
        const std::uint64_t mapped = session.serverSegments.MappedBytes();
        EXPECT_GT(mapped, 0u);
        EXPECT_EQ(LedgerMappedBytes(MemoryRole::Server), serverBefore + mapped);
        EXPECT_EQ(LedgerMappedBytes(MemoryRole::Client), clientBefore + mapped);

        const RoleMemorySample sample = SampleRoleMemory(MemoryRole::Client);
        EXPECT_EQ(sample.MappedSegmentBytes, clientBefore + mapped);
#if defined(__linux__) || defined(__ANDROID__)
        // The peak is the PROCESS's, so it is the same number for both roles and is only
        // meaningful beside the ledger - which is why RoleMemorySample carries both.
        EXPECT_GT(sample.PeakRssBytes, 0u);
        // Holds BY CONSTRUCTION now (the ledger's own running max, RoleMemory.h), not by
        // the kernel's grace: GitHub run 35079459114 failed exactly this line with VmHWM
        // 4,784,128 < VmRSS 4,849,664. The control on the construction is the next case.
        EXPECT_GE(sample.PeakRssBytes, sample.CurrentRssBytes);
#endif
    }
    EXPECT_EQ(LedgerMappedBytes(MemoryRole::Client), clientBefore);
    EXPECT_EQ(LedgerMappedBytes(MemoryRole::Server), serverBefore);
}

// The kernel's VmHWM is NOT a monotone bound on the kernel's VmRSS at read time: hiwater_rss
// is stored only when RSS is about to drop, task_mem() reports max(stored, rss-now), and the
// wave-1 sampler read the two keys in two passes, so the second fopen could grow RSS past the
// peak the first pass reported. GitHub run 35079459114 (ubuntu-24.04) caught it; the probe under
// ~/w7/p5-s1-probe reproduced it locally. The rule is therefore that the ledger keeps ITS OWN
// running peak and folds the kernel's two numbers into it, so a stubbed reader whose current
// exceeds its peak must not fail. RED ONCE by reverting SampleRoleMemoryInto to
// `sample.PeakRssBytes = kernelPeakRssBytes;` - the first EXPECT_EQ below then reads 100 against
// 200 and the EXPECT_GE beside it is the CI line again. That perturbation was run.
TEST(SessionTest, TheLedgersPeakIsItsOwnRunningMaxAndNeverTheKernelsHighWaterMarkVerbatim) {
    std::atomic<std::uint64_t> runningPeak{0};

    // The CI shape: the kernel says peak 100, current 200.
    RoleMemorySample sample = SampleRoleMemoryInto(runningPeak, MemoryRole::Client, 100, 200);
    EXPECT_EQ(sample.PeakRssBytes, 200u)
        << "the kernel's peak was reported verbatim although its current exceeded it";
    EXPECT_GE(sample.PeakRssBytes, sample.CurrentRssBytes);
    EXPECT_EQ(sample.CurrentRssBytes, 200u);
    EXPECT_EQ(runningPeak.load(), 200u);

    // A later, smaller sample does not lower it: a running max never decreases.
    sample = SampleRoleMemoryInto(runningPeak, MemoryRole::Server, 150, 120);
    EXPECT_EQ(sample.PeakRssBytes, 200u);
    EXPECT_EQ(sample.CurrentRssBytes, 120u);
    EXPECT_EQ(sample.Role, MemoryRole::Server);

    // The kernel's peak still counts when it IS the larger number: it is a lower bound on
    // the true peak that this process's own samples may have missed.
    sample = SampleRoleMemoryInto(runningPeak, MemoryRole::Client, 300, 100);
    EXPECT_EQ(sample.PeakRssBytes, 300u);
    EXPECT_EQ(runningPeak.load(), 300u);

    // And the production sampler reads BOTH keys in ONE pass, so the pair it folds is one
    // snapshot: on Linux neither number is 0 and the pair is self-consistent.
#if defined(__linux__) || defined(__ANDROID__)
    std::uint64_t kernelPeak = 0;
    std::uint64_t kernelCurrent = 0;
    ProcessRssBytes(&kernelPeak, &kernelCurrent);
    EXPECT_GT(kernelPeak, 0u);
    EXPECT_GT(kernelCurrent, 0u);
    EXPECT_GE(kernelPeak, kernelCurrent) << "one pass over /proc/self/status disagreed with itself";
#endif
}

// ---------------------------------------------------------------------------
// Two real threads, twenty thousand records
// ---------------------------------------------------------------------------

// The acceptance run. A 32 KiB command ring and 16-byte records means this wraps about ten
// times, so the wrap filler is exercised under load rather than in one contrived case - and
// the invariant checked at the end is that appliedSeq counted the RECORDS and not the fillers.
TEST(SessionTest, TwoThreadsMoveTwentyThousandRecordsAndAgreeOnEveryOne) {
    SessionFixture session;
    ASSERT_TRUE(session.Build(TestSizes()));

    constexpr std::uint32_t kRecords = 20000;
    std::atomic<bool> ok{true};
    std::atomic<std::uint32_t> consumed{0};

    std::thread apply([&] {
        std::uint32_t seen = 0;
        while (seen < kRecords && ok.load()) {
            const SessionWait woke = session.consumer.WaitForWork(5000);
            if (woke == SessionWait::ShutDown) {
                return;
            }
            if (woke == SessionWait::TimedOut) {
                ok.store(false); // a lost wakeup, or the producer stalled: RED, never a hang
                return;
            }
            bool corrupt = false;
            while (session.consumer.ApplyOne(
                [&](const RingRecordView& view) {
                    std::uint32_t value = 0;
                    std::memcpy(&value, view.payload, sizeof(value));
                    if (value != seen) {
                        ok.store(false);
                    }
                    // appliedSeq is advanced AFTER this returns, once, by the session - which
                    // is what the barrier's waiter is entitled to assume.
                    ++seen;
                    consumed.store(seen);
                },
                &corrupt)) {
                if (!ok.load()) {
                    return;
                }
            }
            if (corrupt) {
                ok.store(false);
                return;
            }
            session.consumer.RetireThrough(session.consumer.AppliedSeq());
        }
    });

    std::uint64_t emitted = 0;
    for (std::uint32_t index = 0; index < kRecords && ok.load(); ++index) {
        void* payload = nullptr;
        while ((payload = session.cmdProducer.Reserve(1, kRecNone, sizeof(std::uint32_t))) ==
               nullptr) {
            // Never "wait" on a nullptr with enough free bytes - Ring.h:226-233 says that can
            // only mean "too big, chunk", and a producer that waited there would stall for ever.
            ASSERT_LT(session.cmdProducer.FreeBytes(), 16u);
            if (session.producer.WaitForCmdSpace(16, 5000) != SessionWait::Reached) {
                ok.store(false);
                break;
            }
        }
        if (payload == nullptr) {
            break;
        }
        std::memcpy(payload, &index, sizeof(index));
        ++emitted;
        session.producer.PublishAndNotify(emitted);
    }

    apply.join();
    EXPECT_TRUE(ok.load());
    EXPECT_EQ(consumed.load(), kRecords);
    EXPECT_EQ(emitted, static_cast<std::uint64_t>(kRecords));
    EXPECT_EQ(session.Control().submittedSeq.load(), static_cast<std::uint64_t>(kRecords));
    // The whole point: the two sides' sequence spaces are identical after ten wraps' worth of
    // fillers. A side that counted a kRecPad would land here off by the number of wraps.
    EXPECT_EQ(session.Control().appliedSeq.load(), static_cast<std::uint64_t>(kRecords));
    EXPECT_EQ(session.consumer.AppliedSeq(), static_cast<std::uint64_t>(kRecords));
    EXPECT_TRUE(RingCursorsValid(session.Control(), RingCursorSet::Cmd,
                                 session.serverSegments.CmdRingCapacity()));
}

// ---------------------------------------------------------------------------
// One case per watermark rule (R-9), against the writers rather than the rules
// ---------------------------------------------------------------------------

// submittedSeq: advanced by the PRODUCER after it publishes, and NOBODY WAITS ON IT. The order
// inside PublishAndNotify is publish -> watermark -> ring, and never any other: the doorbell's
// fence only orders what precedes it, so ringing first reopens the lost-wakeup window.
TEST(SessionTest, SubmittedSeqIsThePublishersAndIsPurelyDiagnostic) {
    SessionFixture session;
    ASSERT_TRUE(session.Build(TestSizes()));

    for (std::uint64_t seq = 1; seq <= 4; ++seq) {
        void* payload = session.cmdProducer.Reserve(1, kRecNone, sizeof(std::uint64_t));
        ASSERT_NE(payload, nullptr);
        std::memcpy(payload, &seq, sizeof(seq));
        session.producer.PublishAndNotify(seq);
        EXPECT_EQ(session.Control().submittedSeq.load(), seq);
        // It says nothing about what has been APPLIED, which is the distinction a waiter that
        // picked the wrong watermark would lose.
        EXPECT_EQ(session.Control().appliedSeq.load(), 0u);
    }
}

// appliedSeq: advanced by the CONSUMER for EVERY SINGLE RECORD. P5 forbids the sixty-four
// record batching this ring was designed for, because the verb barrier and every reply wait
// read it - a batched watermark makes a waiter block on work that already ran or, far worse,
// resume on work that has not. Checked after every record, not at the end.
TEST(SessionTest, AppliedSeqAdvancesExactlyOncePerRecordAndIsNeverBatched) {
    SessionFixture session;
    ASSERT_TRUE(session.Build(TestSizes()));

    constexpr std::uint64_t kRecords = 12;
    for (std::uint64_t seq = 1; seq <= kRecords; ++seq) {
        void* payload = session.cmdProducer.Reserve(1, kRecNone, sizeof(std::uint64_t));
        ASSERT_NE(payload, nullptr);
        std::memcpy(payload, &seq, sizeof(seq));
    }
    session.producer.PublishAndNotify(kRecords);

    std::uint64_t applied = 0;
    while (session.consumer.ApplyOne([&](const RingRecordView&) { ++applied; })) {
        EXPECT_EQ(session.Control().appliedSeq.load(), applied)
            << "appliedSeq did not move with the record; a barrier waiter would be blocked on "
               "work that already ran";
        EXPECT_EQ(session.consumer.AppliedSeq(), applied);
    }
    EXPECT_EQ(applied, kRecords);
}

// retiredSeq: advanced once the SEG_STAGE bytes a record referenced are finished with, and the
// staging allocator reclaims behind it. Late is merely slow; EARLY hands live bytes back to the
// producer, so the advance clamps to appliedSeq rather than believing its caller.
TEST(SessionTest, RetiredSeqMayTrailTheApplyButCanNeverOvertakeIt) {
    SessionFixture session;
    ASSERT_TRUE(session.Build(TestSizes()));

    for (std::uint64_t seq = 1; seq <= 3; ++seq) {
        ASSERT_NE(session.cmdProducer.Reserve(1, kRecNone, 8), nullptr);
    }
    session.producer.PublishAndNotify(3);
    while (session.consumer.ApplyOne([](const RingRecordView&) {})) {
    }
    ASSERT_EQ(session.Control().appliedSeq.load(), 3u);

    // Trailing is legal and is what "late" means.
    Watermark::AdvanceRetired(session.Control(), 1);
    EXPECT_EQ(session.Control().retiredSeq.load(), 1u);
    // Running ahead is not: clamped to what has actually been applied.
    Watermark::AdvanceRetired(session.Control(), 99);
    EXPECT_EQ(session.Control().retiredSeq.load(), 3u);
    // Going BACKWARDS is Fatal, not clamped - see AWatermarkThatMovesBackwardsIsFatal below.
}

// completedFrameSerial: the SERVER's, advanced when a present completes. It trails appliedSeq
// by the GPU's own depth and must never be conflated with it - recycling and ageing wait on
// this one and would free a resource the GPU is still reading if they waited on the other.
TEST(SessionTest, CompletedFrameSerialIsTheServersAndIsIndependentOfAppliedSeq) {
    SessionFixture session;
    ASSERT_TRUE(session.Build(TestSizes()));

    for (std::uint64_t seq = 1; seq <= 5; ++seq) {
        ASSERT_NE(session.cmdProducer.Reserve(1, kRecNone, 8), nullptr);
    }
    session.producer.PublishAndNotify(5);
    while (session.consumer.ApplyOne([](const RingRecordView&) {})) {
    }
    EXPECT_EQ(session.Control().appliedSeq.load(), 5u);
    // Five records applied, no frame completed: the two are not the same number and nothing
    // may derive one from the other.
    EXPECT_EQ(session.Control().completedFrameSerial.load(), 0u);

    Watermark::AdvanceCompletedFrame(session.Control(), 2);
    EXPECT_EQ(session.Control().completedFrameSerial.load(), 2u);
    // Republishing the SAME serial is legal and is what a lazy publisher does.
    Watermark::AdvanceCompletedFrame(session.Control(), 2);
    EXPECT_EQ(session.Control().completedFrameSerial.load(), 2u);
}

// presentAckSerial: the only back-pressure that bounds LATENCY rather than bytes. A client
// throttled on it parks, and the server's advance plus the reverse doorbell is what releases
// it - which is the half of the doorbell design that exists so a client wait is not a
// cross-process spin on one shared cache line for a whole frame of a big core.
TEST(SessionTest, PresentAckSerialIsWaitedOnWithGreaterOrEqualAndWakesThroughTheReverseBell) {
    auto session = std::make_shared<SessionFixture>();
    ASSERT_TRUE(session->Build(TestSizes()));

    std::atomic<bool> released{false};
    std::atomic<SessionWait> result{SessionWait::TimedOut};
    std::thread throttled([session, &released, &result] {
        result.store(session->producer.WaitForPresentAck(4, 5000));
        released.store(true, std::memory_order_release);
    });

    // Let it get past the spin and announce itself parked, so the wakeup really travels.
    while (session->Control().producerParked.load() == 0 && !released.load()) {
        std::this_thread::yield();
    }
    // The server jumps STRAIGHT PAST the value the waiter asked for. An equality waiter would
    // still be asleep here; the >= waiter this contract mandates is released.
    Watermark::AdvancePresentAck(session->Control(), 7);
    session->consumer.NotifyClient();

    throttled.join();
    EXPECT_TRUE(released.load());
    EXPECT_EQ(result.load(), SessionWait::Reached);
    EXPECT_EQ(session->Control().presentAckSerial.load(), 7u);
    EXPECT_EQ(session->Control().producerParked.load(), 0u);
}

// ---------------------------------------------------------------------------
// kRecPad (R-9's last sentence, and the one with no other detector)
// ---------------------------------------------------------------------------

// A wrap filler is FRAMING, not a record: no opcode, no payload meaning, no reply slot. If one
// side counts it and the other does not, the two sequence spaces drift by one per wrap, for
// ever - and because seq IS the reply-slot id (R-3), a drifted seq silently reads ANOTHER
// CALL'S ANSWER rather than failing. Nothing on this ring checksums that.
//
// Here the ring is driven right across the wrap boundary with a record size that cannot divide
// it, so fillers are certain; the session's own appliedSeq must count the records and not them.
TEST(SessionTest, AWrapFillerDoesNotAdvanceTheSessionsAppliedSeq) {
    SessionSegmentSizes sizes = TestSizes();
    sizes.CmdRingBytes = 4096; // a handful of records wraps it several times
    SessionFixture session;
    ASSERT_TRUE(session.Build(sizes));
    ASSERT_EQ(session.serverSegments.CmdRingCapacity(), 4096u);

    // 104 bytes + the 8-byte header = 112, and 4096 / 112 is not an integer, so the boundary
    // falls inside a record and the producer must emit a filler on every lap.
    constexpr std::uint64_t kPayload = 104;
    constexpr std::uint64_t kRecords = 200; // ~5 laps
    std::uint64_t emitted = 0;
    std::uint64_t applied = 0;
    std::uint64_t fillerBytes = 0;
    std::uint64_t headBefore = 0;

    while (emitted < kRecords) {
        headBefore = session.cmdProducer.LocalHead();
        void* payload = session.cmdProducer.Reserve(1, kRecNone, kPayload);
        if (payload == nullptr) {
            // Drain and try again; no doorbell needed, this is one thread.
            session.producer.PublishAndNotify(emitted);
            while (session.consumer.ApplyOne([&](const RingRecordView& view) {
                // A filler must NEVER reach the thing that is about to number it.
                EXPECT_EQ(view.flags & kRecPad, 0u) << "a wrap filler reached the record counter";
                EXPECT_NE(view.kind, kRingPadRecordKind);
                ++applied;
            })) {
            }
            session.consumer.RetireThrough(session.consumer.AppliedSeq());
            continue;
        }
        std::memset(payload, static_cast<int>(emitted & 0xFF), static_cast<std::size_t>(kPayload));
        const std::uint64_t grew = session.cmdProducer.LocalHead() - headBefore;
        if (grew > kPayload + sizeof(RingRecordHeader)) {
            fillerBytes += grew - (kPayload + sizeof(RingRecordHeader));
        }
        ++emitted;
    }
    session.producer.PublishAndNotify(emitted);
    while (session.consumer.ApplyOne([&](const RingRecordView& view) {
        EXPECT_EQ(view.flags & kRecPad, 0u) << "a wrap filler reached the record counter";
        ++applied;
    })) {
    }

    EXPECT_GT(fillerBytes, 0u) << "the ring never wrapped, so this case proved nothing";
    EXPECT_EQ(applied, emitted);
    // The session's watermark - the number the barrier's waiter and every reply read use - has
    // to be the record count, with the fillers' bytes invisible to it.
    EXPECT_EQ(session.Control().appliedSeq.load(), emitted);
}

// ---------------------------------------------------------------------------
// SEG_REPLY: the slot pool
// ---------------------------------------------------------------------------

TEST(SessionTest, AReplyIsAddressedBySeqAndCarriesItsSeqBackForSelfCheck) {
    SessionFixture session;
    ASSERT_TRUE(session.Build(TestSizes()));
    ASSERT_EQ(session.replies.SlotCount(), 8u);
    EXPECT_EQ(session.replies.SlotBytes(), 64ull * 1024 / 8);
    EXPECT_EQ(session.replies.MaxReplyBytes(),
              session.replies.SlotBytes() - sizeof(ReplySlotHeader));

    const std::uint32_t pixels[4] = {1, 2, 3, 4};
    session.replies.Post(5, kReplyStatusOk, pixels, sizeof(pixels));

    std::uint32_t out[4] = {};
    std::int32_t status = -1;
    std::uint64_t size = 0;
    ASSERT_TRUE(session.replies.Read(5, out, sizeof(out), &status, &size));
    EXPECT_EQ(status, kReplyStatusOk);
    EXPECT_EQ(size, sizeof(pixels));
    EXPECT_EQ(std::memcmp(out, pixels, sizeof(pixels)), 0);

    // The stamp is the self-check. Seq 13 addresses the SAME slot (13 % 8 == 5), and reading it
    // as seq 13 must FAIL rather than hand back seq 5's answer - which is exactly what a
    // sequence space drifted by a counted kRecPad would do.
    EXPECT_FALSE(session.replies.Read(13, out, sizeof(out), &status, &size));
}

// DECLINED IS A REAL ANSWER, not a failure: it is how MapPersistent says nullptr (R-6) and how
// the four Bool acceptance entry points say false (R-5). A client that folds it into "error"
// re-creates ID-39's 66 lost uploads from the other side.
TEST(SessionTest, DeclinedIsARealAnswerWithNoPayload) {
    SessionFixture session;
    ASSERT_TRUE(session.Build(TestSizes()));

    session.replies.Post(1, kReplyStatusDeclined, nullptr, 0);
    std::int32_t status = -1;
    std::uint64_t size = 99;
    EXPECT_TRUE(session.replies.Read(1, nullptr, 0, &status, &size));
    EXPECT_EQ(status, kReplyStatusDeclined);
    EXPECT_EQ(size, 0u);

    session.replies.Post(2, kReplyStatusError, nullptr, 0);
    EXPECT_TRUE(session.replies.Read(2, nullptr, 0, &status, &size));
    EXPECT_EQ(status, kReplyStatusError);
    // Seq 0 is "no record" and can never name a slot: seq is 1-based (R-3).
    EXPECT_FALSE(session.replies.Read(0, nullptr, 0, &status, &size));
}

#if defined(GTEST_HAS_DEATH_TEST) && GTEST_HAS_DEATH_TEST
// A reply larger than a slot is FATAL, not chunked and not truncated: P5's only large answer is
// a blocking ReadPixels whose size the client knows before it emits, so an overflow means the
// two sides disagree about the frame. A gate that cannot go red is not a gate - and a death
// control with an empty regex is not a gate either (ID-46 finding 10: a bare std::abort(), or a
// segfault in a broken refusal path, satisfied the previous `""`). The regex below is the
// diagnostic's own wording, which WireLogFatal echoes to stderr for exactly this reader. RED ONCE
// by replacing the oversize branch's WireLogFatal with a bare std::abort(): the process still
// dies, the regex finds nothing, and the case fails on "died but not with the expected error".
// That perturbation was run. The boundary is a pair: exactly MaxReplyBytes() posts and reads
// back, one more byte is the named refusal.
TEST(SessionTestDeath, AReplyLargerThanItsSlotIsFatalRatherThanTruncated) {
    SessionSegments segments;
    ASSERT_EQ(segments.Create(TestSizes(), MemoryRole::Server), MOBILEGL_OK);
    ReplySlotPool pool(segments.ReplyBase(), segments.ReplyBytes(), segments.ReplySlotCount());
    ASSERT_TRUE(pool.Valid());
    ASSERT_EQ(pool.SlotBytes(), 8192u);
    ASSERT_EQ(pool.MaxReplyBytes(), 8176u);

    std::vector<std::uint8_t> exact(pool.MaxReplyBytes(), 0xAB);
    EXPECT_TRUE(pool.CanHold(exact.size()));
    pool.Post(1, kReplyStatusOk, exact.data(), exact.size());
    std::vector<std::uint8_t> back(exact.size(), 0);
    std::uint64_t size = 0;
    ASSERT_TRUE(pool.Read(1, back.data(), back.size(), nullptr, &size));
    EXPECT_EQ(size, exact.size());

    std::vector<std::uint8_t> oversize(pool.MaxReplyBytes() + 1, 0xAB);
    EXPECT_FALSE(pool.CanHold(oversize.size()));
    EXPECT_DEATH(pool.Post(2, kReplyStatusOk, oversize.data(), oversize.size()),
                 "reply pool: Fatal\\{ProtocolCorruption\\} - a 8177 byte answer for seq 2 does not "
                 "fit a 8192 byte slot \\(payload cap 8176\\)");
}

// ID-47's client half, on the DEFAULT geometry so the numbers are the ruling's: exactly
// MaxReplyBytes() = 2,097,136 passes, one more byte is refused BY NAME before emission, and the
// 1024x512 RGBA8 read - exactly 2 MiB, sixteen bytes over the cap, the same sixteen-byte shape
// that sank the previous geometry's 512x512 claim - is refused with its own dimensions in the
// line. The message is the contract's verbatim: `ReadPixels <w>x<h> <format> <bytes> > <cap>`.
// RED ONCE by replacing RequireReadPixelsFits's WireLogFatal with a bare std::abort() (the two
// death regexes then match nothing) and, separately, by making CanHold `<` instead of `<=` (the
// exact-cap call then dies). Both perturbations were run.
TEST(SessionTestDeath, AReadPixelsLargerThanAReplySlotIsRefusedAtTheClientByName) {
    SessionSegments segments;
    ASSERT_EQ(segments.Create(SessionSegmentSizes{}, MemoryRole::Server), MOBILEGL_OK);
    ReplySlotPool pool(segments.ReplyBase(), segments.ReplyBytes(), segments.ReplySlotCount());
    ASSERT_TRUE(pool.Valid());
    const std::uint64_t cap = pool.MaxReplyBytes();
    ASSERT_EQ(cap, 2097136u);
    constexpr std::uint32_t kGlRgba = 0x1908;
    constexpr std::uint32_t kGlUnsignedByte = 0x1401;

    // Exactly the cap: 524,284 RGBA8 pixels in one row. Returns, no death.
    ASSERT_EQ(524284ull * 1 * 4, cap);
    EXPECT_TRUE(pool.CanHold(cap));
    pool.RequireReadPixelsFits(524284, 1, kGlRgba, kGlUnsignedByte, cap);

    // One more byte.
    EXPECT_FALSE(pool.CanHold(cap + 1));
    EXPECT_DEATH(pool.RequireReadPixelsFits(524284, 1, kGlRgba, kGlUnsignedByte, cap + 1),
                 "Fatal\\{ReplyTooLarge, \"ReadPixels 524284x1 0x1908/0x1401 2097137 > 2097136\"\\}");

    // The read a caller would actually make: 1024x512 RGBA8 = 2 MiB, 16 over.
    EXPECT_DEATH(pool.RequireReadPixelsFits(1024, 512, kGlRgba, kGlUnsignedByte, 1024ull * 512 * 4),
                 "Fatal\\{ReplyTooLarge, \"ReadPixels 1024x512 0x1908/0x1401 2097152 > 2097136\"\\}");

    // And E2's read, the reason for the geometry, is not refused.
    pool.RequireReadPixelsFits(640, 480, kGlRgba, kGlUnsignedByte, 640ull * 480 * 4);
    segments.Close();
}
#endif

// ---------------------------------------------------------------------------
// SEG_EVENT: the reverse channel
// ---------------------------------------------------------------------------

// P5 owes exactly this: the event ring can CARRY the three callbacks the reduced path needs.
// The overflow policy is P9's, so what is pinned here is the mechanism - a full ring latches
// eventRingFull and a dropped lossy event counts in eventDropped - and not a decision between
// them.
TEST(SessionTest, TheEventRingCarriesTheThreeReverseCallbacks) {
    SessionFixture session;
    ASSERT_TRUE(session.Build(TestSizes()));

    // OnBufferWriteback: the bytes ride INSIDE the record, and the blobref the client hands the
    // frontend names SEG_EVENT plus the in-segment offset of those bytes - never a host
    // pointer (R-2's rule B).
    const std::uint8_t written[8] = {9, 8, 7, 6, 5, 4, 3, 2};
    {
        void* slot = session.eventOut.Reserve(kEventBufferWriteback,
                                              sizeof(EventBufferWritebackHead) + sizeof(written));
        ASSERT_NE(slot, nullptr);
        EventBufferWritebackHead head{};
        head.Resource = EventHandle{7, 1};
        head.Offset = 64;
        head.Size = sizeof(written);
        std::memcpy(slot, &head, sizeof(head));
        std::memcpy(static_cast<std::uint8_t*>(slot) + sizeof(head), written, sizeof(written));
    }
    // OnGpuWritten: a count and a tail of ranges.
    {
        void* slot = session.eventOut.Reserve(kEventGpuWritten,
                                              sizeof(EventGpuWrittenHead) + 2 * sizeof(EventRange));
        ASSERT_NE(slot, nullptr);
        EventGpuWrittenHead head{};
        head.Resource = EventHandle{9, 2};
        head.RangeCount = 2;
        std::memcpy(slot, &head, sizeof(head));
        const EventRange ranges[2] = {{0, 16}, {128, 32}};
        std::memcpy(static_cast<std::uint8_t*>(slot) + sizeof(head), ranges, sizeof(ranges));
    }
    // OnSurfaceChanged: a fixed head, the MGPSurfaceInfo image.
    {
        void* slot = session.eventOut.Reserve(kEventSurfaceChanged, sizeof(EventSurfaceChangedHead));
        ASSERT_NE(slot, nullptr);
        EventSurfaceChangedHead head{};
        head.Width = 1280;
        head.Height = 720;
        head.IsDefault = 1;
        std::memcpy(slot, &head, sizeof(head));
    }
    session.eventOut.PublishAndNotify(session.clientTransport->SelfDoorbell(),
                                      session.Control().producerParked);

    RingRecordView view{};
    ASSERT_TRUE(session.eventIn.Pop(view));
    EXPECT_EQ(view.kind, kEventBufferWriteback);
    EventBufferWritebackHead writeback{};
    std::memcpy(&writeback, view.payload, sizeof(writeback));
    EXPECT_EQ(writeback.Resource.Slot, 7u);
    EXPECT_EQ(writeback.Size, sizeof(written));
    const auto* inlineBytes = static_cast<const std::uint8_t*>(view.payload) + sizeof(writeback);
    EXPECT_EQ(std::memcmp(inlineBytes, written, sizeof(written)), 0);
    // The offset a blobref would carry: inside SEG_EVENT and past its control page, never a
    // host address.
    const std::uint64_t offset = session.eventIn.OffsetInSegment(inlineBytes);
    EXPECT_GE(offset, sizeof(RingControl));
    EXPECT_LT(offset, session.clientSegments.AnnouncedSize(SessionSegmentSlot::Event));

    ASSERT_TRUE(session.eventIn.Pop(view));
    EXPECT_EQ(view.kind, kEventGpuWritten);
    EventGpuWrittenHead gpuWritten{};
    std::memcpy(&gpuWritten, view.payload, sizeof(gpuWritten));
    EXPECT_EQ(gpuWritten.RangeCount, 2u);

    ASSERT_TRUE(session.eventIn.Pop(view));
    EXPECT_EQ(view.kind, kEventSurfaceChanged);
    EventSurfaceChangedHead surface{};
    std::memcpy(&surface, view.payload, sizeof(surface));
    EXPECT_EQ(surface.Width, 1280u);
    EXPECT_EQ(surface.IsDefault, 1u);

    EXPECT_FALSE(session.eventIn.Pop(view));
    session.eventIn.Drained();
    EXPECT_FALSE(session.eventIn.RingIsFull());
    EXPECT_EQ(session.eventIn.DroppedEvents(), 0u);
}

TEST(SessionTest, AFullEventRingLatchesTheFlagRatherThanDecidingWhatToDoAboutIt) {
    SessionFixture session;
    ASSERT_TRUE(session.Build(TestSizes()));

    // Fill it. Reserve refuses at half the ring per record, so this terminates.
    const std::uint64_t capacity = session.serverSegments.EventRingCapacity();
    std::uint64_t posted = 0;
    while (session.eventOut.Reserve(kEventGpuWritten, 256) != nullptr) {
        ++posted;
        ASSERT_LT(posted, capacity); // a producer that never fills is a broken case
    }
    EXPECT_TRUE(session.eventIn.RingIsFull());

    session.eventOut.CountDrop();
    EXPECT_EQ(session.eventIn.DroppedEvents(), 1u);

    session.eventOut.PublishAndNotify(session.clientTransport->SelfDoorbell(),
                                      session.Control().producerParked);
    RingRecordView view{};
    std::uint64_t drained = 0;
    while (session.eventIn.Pop(view)) {
        ++drained;
    }
    EXPECT_EQ(drained, posted);
    session.eventIn.Drained();
    EXPECT_FALSE(session.eventIn.RingIsFull());
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

// The design's own steady state: the apply thread spun, set consumerParked and blocked with NO
// DEADLINE. Only CondVarDoorbell::Kill() can bring it back - a single Notify is consumed by one
// Park, after which Doorbell::Wait re-tests a condition nothing published, finds the bell alive
// and parks again, forever. InProcessChannel::Close kills both bells, and SessionConsumer turns
// `Wait == false && Dead()` into SessionWait::ShutDown.
//
// A REGRESSION HERE IS A HANG, so the join is bounded at five seconds and the waiter is
// detached on timeout: the test goes red instead of wedging the CI job. That is
// InProcessTransportTest.cpp:344's shape, and it is copied on purpose.
TEST(SessionTest, ShutdownUnparksTheApplyThreadAndTheJoinIsBounded) {
    struct Shared {
        SessionFixture session;
        std::atomic<bool> returned{false};
        std::atomic<SessionWait> verdict{SessionWait::Reached};
        std::atomic<bool> started{false};
    };
    auto shared = std::make_shared<Shared>();
    ASSERT_TRUE(shared->session.Build(TestSizes()));

    std::thread apply([shared] {
        shared->started.store(true, std::memory_order_release);
        // kWaitForever, exactly as the real apply loop parks.
        shared->verdict.store(shared->session.consumer.WaitForWork(kWaitForever));
        shared->returned.store(true, std::memory_order_release);
    });

    while (shared->session.Control().consumerParked.load() == 0 &&
           !shared->returned.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ASSERT_FALSE(shared->returned.load(std::memory_order_acquire))
        << "the apply thread returned before anything shut the session down";

    shared->session.clientTransport->Shutdown();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!shared->returned.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!shared->returned.load(std::memory_order_acquire)) {
        apply.detach();
        FAIL() << "Shutdown did not unpark the apply thread within 5 s: without a doorbell death "
                  "state a waiter consumes the ring and parks again, and teardown can never join";
    }
    apply.join();

    EXPECT_EQ(shared->verdict.load(), SessionWait::ShutDown);
    EXPECT_TRUE(shared->session.serverTransport->SelfDoorbell().Dead());
    EXPECT_EQ(shared->session.Control().consumerParked.load(), 0u);

    // Sticky: a wait that ARRIVES after the shutdown returns at once rather than parking, so a
    // late thread cannot hang either.
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(shared->session.consumer.WaitForWork(kWaitForever), SessionWait::ShutDown);
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - start)
                  .count(),
              1000);
    // And so does a producer blocked in the verb barrier: without this, a client waiting for
    // appliedSeq when the server died would sit in the barrier for ever.
    EXPECT_EQ(shared->session.producer.WaitForApplied(1, kWaitForever), SessionWait::ShutDown);
}

// ---------------------------------------------------------------------------
// The ABI fingerprint's mixer: MOVED to SessionHandshakeTest (ID-46 finding 6).
//
// The case that lived here drove MixAbiFingerprint with made-up sizes and never touched
// CapsAbiFingerprint(), the value the two handshakes actually compare - which had its own
// second FNV loop and no caller of the mixer at all, so `return 1;` in production left the
// whole unit lane green. The sensitivity case now starts from CapsAbiFingerprint(), which
// needs CapsCodec.h and therefore the GL frontend's umbrella header that this suite keeps
// out; it lives in SessionHandshakeTest.cpp beside the two handshake-guard controls.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Fix round 1 - the cases the adversarial review's findings earned
// ---------------------------------------------------------------------------

// M-8. A watermark moving BACKWARDS is Fatal, not logged-and-ignored. Logging it and returning
// was strictly worse than an abort: SessionConsumer keeps its own counter, so once the shared
// appliedSeq is behind, every later advance is a no-op for ever and every WaitForApplied on
// kWaitForever - the verb barrier and every reply wait - blocks permanently. A hang whose only
// evidence is one ERROR line is not a diagnosis.
//
// The regex names the diagnostic (ID-46 finding 10; see the reply-pool death control for why an
// empty one was not a gate). RED ONCE by replacing AdvanceMonotonic's WireLogFatal block with a
// bare std::abort(): the case then fails on "died but not with the expected error". That
// perturbation was run.
#if defined(GTEST_HAS_DEATH_TEST) && GTEST_HAS_DEATH_TEST
TEST(SessionTestDeath, AWatermarkThatMovesBackwardsIsFatalRatherThanIgnored) {
    alignas(4096) RingControl control{};
    InitRingControl(control);
    Watermark::AdvanceApplied(control, 10);
    ASSERT_EQ(control.appliedSeq.load(), 10u);
    EXPECT_DEATH(Watermark::AdvanceApplied(control, 9),
                 "Fatal\\{ProtocolCorruption, \"watermark\"\\} appliedSeq moved backwards, 10 -> 9");
}
#endif

// M-6. `inproc`'s ATTACH half is a real second mapping, not an alias of the owner's ShmSegment
// objects. The attach side is the side P6 replaces with an SCM_RIGHTS Adopt, so leaving it
// aliased would mean dup/Adopt/Map/the fstat size check were first exercised on the day the
// second process appears - the same criticism this package levels at allocating with new[].
TEST(SessionTest, TheInprocPeerGetsItsOwnMappingOfTheSameSharedMemory) {
    SessionSegments owner;
    ASSERT_EQ(owner.Create(TestSizes(), MemoryRole::Server), MOBILEGL_OK);
    SessionSegments peer;
    ASSERT_EQ(peer.AttachInProcess(owner, MemoryRole::Client), MOBILEGL_OK);
    ASSERT_TRUE(peer.Valid());

#if !defined(_WIN32)
    // Different descriptors, different virtual addresses...
    EXPECT_NE(peer.DescriptorFor(SessionSegmentSlot::Cmd),
              owner.DescriptorFor(SessionSegmentSlot::Cmd));
    EXPECT_GE(peer.DescriptorFor(SessionSegmentSlot::Cmd), 0);
    EXPECT_NE(static_cast<void*>(peer.CmdControl()), static_cast<void*>(owner.CmdControl()));
    EXPECT_NE(peer.CmdRingBase(), owner.CmdRingBase());
#endif
    // ... over the SAME physical page, which is the whole point: a store through one mapping is
    // visible through the other, exactly as it is across two processes under spawn.
    EXPECT_EQ(peer.AnnouncedSize(SessionSegmentSlot::Cmd),
              owner.AnnouncedSize(SessionSegmentSlot::Cmd));
    owner.CmdControl()->appliedSeq.store(4242, std::memory_order_release);
    EXPECT_EQ(peer.CmdControl()->appliedSeq.load(std::memory_order_acquire), 4242u);
    peer.CmdControl()->presentAckSerial.store(77, std::memory_order_release);
    EXPECT_EQ(owner.CmdControl()->presentAckSerial.load(std::memory_order_acquire), 77u);

    peer.Close();
    // The owner is untouched by the peer's teardown - there is no shared ownership left whose
    // Close order has to be got right by hand.
    EXPECT_TRUE(owner.Valid());
    EXPECT_EQ(owner.CmdControl()->appliedSeq.load(), 4242u);
    owner.Close();
}

// M-5. RetireThrough must NOT hand back a slot borrowed into the GPU timeline. The first
// version called RingConsumer::PublishRetired(), which stores the local tail into BOTH tails
// and therefore frees every popped byte regardless of kRecBorrowSlot - defeating the reason the
// ring carries two tails at all (Ring.h:24-27) and letting the producer overwrite a slot the
// GPU is still reading. Nothing sets kRecBorrowSlot yet; this is the case that has to be true
// on the day something does.
TEST(SessionTest, ABorrowedSlotIsNotHandedBackUntilItIsReleased) {
    SessionFixture session;
    ASSERT_TRUE(session.Build(TestSizes()));

    constexpr std::uint64_t kPayload = 64;
    for (int index = 0; index < 3; ++index) {
        const std::uint16_t flags = index == 1 ? kRecBorrowSlot : kRecNone;
        ASSERT_NE(session.cmdProducer.Reserve(1, flags, kPayload), nullptr);
    }
    session.producer.PublishAndNotify(3);

    std::uint64_t firstCursor = 0;
    bool sawBorrow = false;
    int seen = 0;
    while (session.consumer.ApplyOne([&](const RingRecordView& view) {
        if (seen == 0) {
            firstCursor = view.cursor;
        }
        if ((view.flags & kRecBorrowSlot) != 0) {
            sawBorrow = true;
        }
        ++seen;
    })) {
    }
    ASSERT_EQ(seen, 3);
    ASSERT_TRUE(sawBorrow) << "the producer dropped kRecBorrowSlot, so this case proved nothing";

    EXPECT_EQ(session.Control().appliedSeq.load(), 3u);
    session.consumer.RetireThrough(session.consumer.AppliedSeq());

    // The reclaim point stops AT the borrowed record: only the first record's bytes came back.
    const std::uint64_t firstRecordEnd = firstCursor + sizeof(RingRecordHeader) + kPayload;
    EXPECT_EQ(session.consumer.RetirableCursor(), firstRecordEnd);
    EXPECT_EQ(session.Control().cmdRetiredTail.load(), firstRecordEnd);
    EXPECT_LT(session.Control().cmdRetiredTail.load(), session.Control().cmdAppliedTail.load())
        << "a borrowed slot was handed back to the producer while the GPU may still read it";
    EXPECT_TRUE(RingCursorsValid(session.Control(), RingCursorSet::Cmd,
                                 session.serverSegments.CmdRingCapacity()));

    // completedFrameSerial passed it: now the rest comes back.
    session.consumer.RetireBorrowedUpTo(session.cmdConsumer.LocalTail());
    EXPECT_EQ(session.Control().cmdRetiredTail.load(), session.Control().cmdAppliedTail.load());
    EXPECT_TRUE(RingCursorsValid(session.Control(), RingCursorSet::Cmd,
                                 session.serverSegments.CmdRingCapacity()));
}

// q-1. Teardown's drain keys on the PRODUCER's own last-published seq, not on
// RingControl::submittedSeq: Ring.h:72-77 permits that watermark to be published lazily, so a
// drain that trusted it would under-wait and free an emitter-owned var-tail while a record
// still names it. With the verb barrier armed the two are equal; under
// MOBILEGL_IPC_VERB_BARRIER=0 - R-1's negative control, which the phase must run once - they
// are not.
TEST(SessionTest, TheProducerRemembersWhatItPublishedEvenIfTheWatermarkLags) {
    SessionFixture session;
    ASSERT_TRUE(session.Build(TestSizes()));

    for (int index = 0; index < 4; ++index) {
        ASSERT_NE(session.cmdProducer.Reserve(1, kRecNone, 8), nullptr);
    }
    // A lazy publisher: four records are visible, the shared watermark says two.
    session.producer.PublishAndNotify(2);
    EXPECT_EQ(session.Control().submittedSeq.load(), 2u);
    EXPECT_EQ(session.producer.LastPublishedSeq(), 2u);

    session.producer.PublishAndNotify(4);
    EXPECT_EQ(session.producer.LastPublishedSeq(), 4u);
    EXPECT_EQ(session.Control().submittedSeq.load(), 4u);
    // The drain's bound may only grow, whatever a caller passes. Republishing an older bound is
    // publishing LATE, which R-9 permits - it is clamped up rather than treated as a watermark
    // moving backwards, which is Fatal. Teardown does exactly this.
    session.producer.PublishAndNotify(3);
    EXPECT_EQ(session.producer.LastPublishedSeq(), 4u);
    EXPECT_EQ(session.Control().submittedSeq.load(), 4u);
}

// M-3. FlatBuffers' Verifier::VerifyTable is `return !table || table->Verify(*this)`, so a NULL
// union member PASSES verification: a 24-byte frame verifies, carries the file identifier,
// reports msg_type() == Hello, and returns nullptr from msg_as_Hello(). Both handshakes fold
// that into their guard instead of dereferencing it.
//
// THIS CASE IS THE PREMISE, NOT THE CONTROL. It proves the shape is reachable - a FlatBuffers
// property - and nothing about MobileGL: the wave-1 review (ID-46 finding 7) deleted both
// `msg_as_*() == nullptr` clauses and this case stayed green, because it never calls Accept or
// Start. The controls on the two guards are SessionHandshakeTest's, which drive this exact
// frame THROUGH ServerSession::Accept and ClientSession::StartOverTransportPair and require
// each guard's own refusal line. s1-v1.md:338 claimed this case meant "the guard cannot be
// simplified away"; it did not, and s1-v3.md says so.
TEST(SessionTest, ANullUnionFrameVerifiesWhichIsThePremiseOfBothHandshakeGuards) {
    ::flatbuffers::FlatBufferBuilder builder(256);
    auto envelope = ::MobileGL::Wire::CreateCtrlEnvelope(builder, ::MobileGL::Wire::CtrlMsg::Hello,
                                                         ::flatbuffers::Offset<void>());
    ::MobileGL::Wire::FinishCtrlEnvelopeBuffer(builder, envelope);

    ::flatbuffers::Verifier verifier(builder.GetBufferPointer(), builder.GetSize());
    ASSERT_TRUE(::MobileGL::Wire::VerifyCtrlEnvelopeBuffer(verifier))
        << "if this ever starts failing, the guard in both handshakes may be relaxed";
    ASSERT_TRUE(::MobileGL::Wire::CtrlEnvelopeBufferHasIdentifier(builder.GetBufferPointer()));

    const ::MobileGL::Wire::CtrlEnvelope* parsed =
        ::MobileGL::Wire::GetCtrlEnvelope(builder.GetBufferPointer());
    ASSERT_NE(parsed, nullptr);
    EXPECT_EQ(parsed->msg_type(), ::MobileGL::Wire::CtrlMsg::Hello);
    // The whole finding, in one line: the tag says Hello and there is no Hello.
    EXPECT_EQ(parsed->msg_as_Hello(), nullptr);
}

// M-1. The four retired CapsSnapshot fields are `(deprecated)`, so their vtable slots stay
// burned and the two new fields sit past them. Plainly deleting them handed slot 14 - which
// used to carry an `[int]` vector, a 4-byte uoffset - to `callMask`, an 8-byte inline ulong,
// with no ABI-major bump and an ABI fingerprint that mixes struct sizes rather than the schema.
// THIS IS THE FIRST TEST IN THE TREE THAT PINS A TABLE FIELD ID: ProtocolSmokeTest's
// UnionTagsAreFrozenWireValues pins union tags and enum values only and says nothing about one.
TEST(SessionTest, CapsSnapshotFieldIdsAreFrozenAndTheRetiredSlotsStayBurned) {
    using CapsSnapshot = ::MobileGL::Wire::CapsSnapshot;
    EXPECT_EQ(static_cast<int>(CapsSnapshot::VT_DYNAMICPARAMETERS), 4);
    EXPECT_EQ(static_cast<int>(CapsSnapshot::VT_RENDERERINFO), 6);
    EXPECT_EQ(static_cast<int>(CapsSnapshot::VT_FORMATCAPS), 8);
    EXPECT_EQ(static_cast<int>(CapsSnapshot::VT_EXTENSIONS), 10);
    EXPECT_EQ(static_cast<int>(CapsSnapshot::VT_APIVERSION), 12);
    // 14, 16, 18 and 20 are the four retired fields and must stay unreachable for ever.
    EXPECT_EQ(static_cast<int>(CapsSnapshot::VT_CALLMASK), 22);
    EXPECT_EQ(static_cast<int>(CapsSnapshot::VT_BACKENDTYPE), 24);

    // The appended Hello / Welcome fields really are tail appends onto slots nothing occupied.
    EXPECT_EQ(static_cast<int>(::MobileGL::Wire::Hello::VT_ABIFINGERPRINT), 16);
    EXPECT_EQ(static_cast<int>(::MobileGL::Wire::Welcome::VT_EVENTRING), 16);
    EXPECT_EQ(static_cast<int>(::MobileGL::Wire::Welcome::VT_BUILDFINGERPRINT), 18);
    EXPECT_EQ(static_cast<int>(::MobileGL::Wire::Welcome::VT_ABIFINGERPRINT), 20);
}

// ---------------------------------------------------------------------------
// Fix round 2 - SEG_STAGE stopped being a ring, and the flag-space collisions
// ---------------------------------------------------------------------------

// Package w1's encoder owns SEG_STAGE as a LINEAR ALLOCATOR that reclaims on retiredSeq: a staged
// byte run carries no RingRecordHeader and nothing walks SEG_STAGE. So RingCursorSet::Stage has no
// producer and no consumer, and all three of its cursors stay ZERO for the whole of a session.
//
// This case exists because "declared and written by nobody" is exactly the state the five sequence
// watermarks were in for a whole phase before this one. The stage triple is in that state ON
// PURPOSE now, and the difference between "on purpose" and "forgotten" is that one of them is
// pinned. A half-wiring - a producer publishing stageHead with nothing advancing the two tails -
// would make FreeBytes() fall to zero on the first lap and never recover: a hang, not a slow path.
TEST(SessionTest, TheStageCursorTripleStaysDeadAcrossAWholeSession) {
    SessionFixture session;
    ASSERT_TRUE(session.Build(TestSizes()));

    // SEG_STAGE is still mapped and still usable bytes - w1's allocator needs them, and the
    // decoder resolves blobrefs against the same view - it is just not a ring.
    ASSERT_NE(session.serverSegments.StageBase(), nullptr);
    EXPECT_EQ(session.serverSegments.StageBytes(), 48ull * 1024)
        << "SEG_STAGE is no longer rounded down to a power of two; that was a ring requirement "
           "and keeping it would silently turn MOBILEGL_IPC_STAGE_MB=24 into 16";

    // Drive a full session's worth of traffic through SEG_CMD.
    constexpr int kRecords = 500;
    int applied = 0;
    for (int index = 0; index < kRecords; ++index) {
        void* payload = nullptr;
        while ((payload = session.cmdProducer.Reserve(1, kRecNone, 32)) == nullptr) {
            while (session.consumer.ApplyOne([&](const RingRecordView&) { ++applied; })) {
            }
            session.consumer.RetireThrough(session.consumer.AppliedSeq());
        }
        std::memset(payload, index & 0xFF, 32);
        session.producer.PublishAndNotify(static_cast<std::uint64_t>(index + 1));
    }
    while (session.consumer.ApplyOne([&](const RingRecordView&) { ++applied; })) {
    }
    session.consumer.RetireThrough(session.consumer.AppliedSeq());
    ASSERT_EQ(applied, kRecords);

    // The command triple moved. The stage triple did not, and nothing in the session touches it.
    EXPECT_GT(session.Control().cmdHead.load(), 0u);
    EXPECT_EQ(session.Control().stageHead.load(), 0u);
    EXPECT_EQ(session.Control().stageAppliedTail.load(), 0u);
    EXPECT_EQ(session.Control().stageRetiredTail.load(), 0u);
    // retiredSeq is the watermark w1's allocator reclaims behind, and it is a SEQUENCE - not one
    // of the three byte cursors above.
    EXPECT_GT(session.Control().retiredSeq.load(), 0u);
}

// The kVarTail/kRecPad collision, made harmless. Those two are bit 2 of two different flag spaces,
// so an encoder that copied MGPipeCallFlagsFor(op) into RingRecordHeader::flags verbatim would
// have every var-tail record SKIPPED by Pop, silently, with the record lost and nothing logged on
// either side. Pop now requires a filler to carry both the flag AND kind == kRingPadRecordKind, so
// a real record wearing that bit is delivered instead of eaten.
//
// THE BIT HAS TO BE ON THE WIRE. The first version of this case asked Reserve(7, kRecPad, 16),
// and RingProducer::Reserve MASKS kRecPad OUT of whatever the caller passes (Ring.cpp: `flags &
// ~kRecPad`), so the header it stored had flags == 0, Pop's pad arm was never entered, and the
// case stayed green with the kind check deleted (ID-46 finding 5, executed by the verifier). So
// this producer writes the header ITSELF after Reserve, the way a codec with its own header
// struct does - MGPWireRecHeader and RingRecordHeader are the same eight bytes - which is the one
// path Reserve's mask cannot cover. RingTest.TheTwoFlagSpacesAreDisjointByTranslation (ID-43) is
// the same control at the ring; this one is at the SESSION, where what is pinned in addition is
// that appliedSeq counts the delivered record and does NOT count the genuine filler beside it.
// RED ONCE by deleting `&& header.kind == kRingPadRecordKind` from RingConsumer::Pop: the record
// vanishes into the wrap-filler skip, `seen` stays 0 and appliedSeq stays 0, and the case fails
// on the first EXPECT's message. That perturbation was run.
TEST(SessionTest, ARecordWearingThePadBitIsDeliveredRatherThanEatenAsAFiller) {
    SessionFixture session;
    ASSERT_TRUE(session.Build(TestSizes()));

    // kRecPad is MGPipeCallFlags::kVarTail's bit. Kind 7 is a real opcode, not a filler.
    void* payload = session.cmdProducer.Reserve(7, kRecNone, 16);
    ASSERT_NE(payload, nullptr);
    std::memset(payload, 0xAB, 16);
    // Stamp the bit past Reserve's mask, exactly where the codec's header struct would put it.
    auto* headerBytes = static_cast<std::uint8_t*>(payload) - sizeof(RingRecordHeader);
    RingRecordHeader stamped{};
    std::memcpy(&stamped, headerBytes, sizeof(stamped));
    ASSERT_EQ(stamped.kind, 7u);
    ASSERT_EQ(stamped.flags & kRecPad, 0u) << "Reserve stopped masking kRecPad";
    stamped.flags = static_cast<std::uint16_t>(stamped.flags | kRecPad);
    std::memcpy(headerBytes, &stamped, sizeof(stamped));
    session.producer.PublishAndNotify(1);

    int seen = 0;
    std::uint16_t seenKind = 0;
    std::uint16_t seenFlags = 0;
    while (session.consumer.ApplyOne([&](const RingRecordView& view) {
        seenKind = view.kind;
        seenFlags = view.flags;
        ++seen;
    })) {
    }
    EXPECT_EQ(seen, 1) << "the record was skipped as a wrap filler because it wore bit 2 - Pop's "
                          "kind check (a filler is kRecPad AND kind == kRingPadRecordKind) is the "
                          "only thing between a stamped header and a silently deleted record";
    EXPECT_EQ(seenKind, 7);
    EXPECT_NE(seenFlags & kRecPad, 0u)
        << "the bit arrives intact: the kind check narrows the SKIP, it does not scrub the bit";
    EXPECT_EQ(session.Control().appliedSeq.load(), 1u);

    // The opposite control, so that "delivered" cannot be satisfied by not skipping anything:
    // a header wearing kRecPad whose kind IS kRingPadRecordKind is a genuine wrap filler, still
    // vanishes, and is NOT counted by appliedSeq (R-9: a filler does not advance seq).
    void* filler = session.cmdProducer.Reserve(kRingPadRecordKind, kRecNone, 16);
    ASSERT_NE(filler, nullptr);
    headerBytes = static_cast<std::uint8_t*>(filler) - sizeof(RingRecordHeader);
    std::memcpy(&stamped, headerBytes, sizeof(stamped));
    stamped.flags = static_cast<std::uint16_t>(stamped.flags | kRecPad);
    std::memcpy(headerBytes, &stamped, sizeof(stamped));
    session.producer.PublishAndNotify(2);
    while (session.consumer.ApplyOne([&](const RingRecordView&) { ++seen; })) {
    }
    EXPECT_EQ(seen, 1) << "a genuine filler was delivered as a record: Pop's skip was removed, "
                          "not narrowed";
    EXPECT_EQ(session.Control().appliedSeq.load(), 1u) << "a filler advanced appliedSeq (R-9)";
}

// The kHostSpan/kRecBorrowSlot collision, made loud. P5 implements no borrowed slots and is ruled
// to produce no host spans either, so the bit arriving means the collision did - and the
// conservative arm it takes (stop reclaiming) would otherwise wedge the producer on the first full
// ring with nothing in any log saying why.
TEST(SessionTest, ABorrowSlotSightingIsCountedRatherThanJustActedOn) {
    SessionFixture session;
    ASSERT_TRUE(session.Build(TestSizes()));
    EXPECT_EQ(session.consumer.BorrowedRecordsSeen(), 0u);

    ASSERT_NE(session.cmdProducer.Reserve(3, kRecBorrowSlot, 16), nullptr);
    session.producer.PublishAndNotify(1);
    while (session.consumer.ApplyOne([](const RingRecordView&) {})) {
    }
    EXPECT_EQ(session.consumer.BorrowedRecordsSeen(), 1u)
        << "a kRecBorrowSlot record went by unnamed; in P5 that bit can only be kHostSpan";
}

// m-4. The reply pool refuses a geometry whose slots would not be 8-aligned: the fences order
// the payload against the stamp, but the stamp's own 8-byte Seq has to be untorn for the
// wrong-slot self-check to mean anything, and that is only true while it is naturally aligned.
TEST(SessionTest, AReplyGeometryThatWouldMisalignASlotHeaderIsRefused) {
    std::vector<std::uint64_t> aligned(1024);
    void* base = aligned.data();
    // 4100 / 4 = 1025 -> every slot after the first lands on an odd boundary.
    EXPECT_FALSE(ReplySlotPool(base, 4100, 4).Valid());
    // A base that is not itself 8-aligned is refused too, whatever the slot size.
    EXPECT_FALSE(ReplySlotPool(static_cast<std::uint8_t*>(base) + 1, 4096, 8).Valid());
    // Not a power of two: the addressing is a mask.
    EXPECT_FALSE(ReplySlotPool(base, 4096, 6).Valid());
    // No room for a payload past the 16-byte header.
    EXPECT_FALSE(ReplySlotPool(base, 64, 8).Valid());
    // And the shape that is legal.
    EXPECT_TRUE(ReplySlotPool(base, 4096, 8).Valid());
}
