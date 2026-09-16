// MobileGL - MobileGL/MG_Test/Wire/ServerLoopTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// Package v1's suite: the apply thread, the blocking control mailbox, the verb stamp, the
// bounded shutdown, and R-11's server-owned staging copy.
//
// WHAT THIS SUITE REFUSES TO DO, and why it is written the way it is (R-16).
//
// Every case here drives the REAL path: a real ServerSession over four real ShmSegment-backed
// segments, a real apply thread parked on a real Doorbell, and records that go in through w1's
// encoder and come out through w1's decoder. None of them writes a record field by hand, none
// of them calls PipeApplier::ApplyOne directly, and none of them sets a flag the case then
// observes. That matters because the three defects the first P5 wave shipped were all of that
// shape - a persistent-map case that wrote the record field itself still passed with the
// producer deleted, and a lane probe armed against stub SOURCE TEXT went green having run
// monolith on 8 of 11 lanes.
//
// The two things this suite deliberately CANNOT reach are named rather than faked:
//   * there is no GL context here, so ServerLoop::CreateBackend is not called and the five
//     class-B verbs DECLINE. That is asserted as a decline, not skipped - a Clear that was
//     APPLIED with no backend would mean the sink found a table it should not have.
//   * R-11's end-to-end control is MOBILEGL_IPC_AUDIT=1 over the reduced path, which needs the
//     client's emit table (package c1). What IS reachable here is the property that control
//     exists to test: bytes that were copied survive the source being overwritten with 0xDD.

#include <Config.h>
#include <MG_Backend/BackendObject.h>
#include <MG_Backend/BackendObjects.h>
#include <MG_Backend/DirectGLES/BackendObject_DirectGLES.h>
#include <MG_Backend/DirectGLES/DirectGLES.h>
#include <MG_Backend/DirectGLES/Managers.h>
#include <MG_Backend/DirectGLES/Utils.h>
#include <MG_Backend/MGPipe/PipeInputs.h>
#include <MG_Impl/Pipe/PipeFill.h>
#include <MG_Pipe/MGPipe.h>
#include <MG_Pipe/PipeApply.h>
#include <MG_Remote/CapsCodec.h>
#include <MG_Remote/Client/ClientSession.h>
#include <MG_Remote/Protocol/generated/protocol_generated.h>
#include <MG_Remote/Server/PipeApplier.h>
#include <MG_Remote/Server/ServerLoop.h>
#include <MG_Remote/Server/ServerSession.h>
#include <MG_Remote/Server/StagedShadow.h>
#include <MG_Remote/Transport/InProcessTransport.h>
#include <MG_Remote/Transport/ReplySlot.h>
#include <MG_Remote/Transport/Ring.h>
#include <MG_Remote/Transport/SessionRings.h>
#include <MG_Remote/Wire/PipeWireCodec.h>
#include <MG_State/GLState/BufferState/BufferObject.h>
#include <MG_State/GLState/Core.h>
#include <MG_State/GLState/TextureState/TextureEnum.h>
#include <MGGitHash.h>

#include <csignal>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif
#if defined(__linux__) || defined(__ANDROID__)
#include <sched.h>
#endif

using namespace MobileGL;

// NOT `using namespace MobileGL::MG_Remote`. MobileGL::Wire (the flatbuffers control-plane
// schema) and MobileGL::MG_Remote::Wire (the G3 codec) are two different namespaces with the
// same last name, and a using-directive over the second makes every mention of `Wire`
// ambiguous - including the ones in the generated header.
namespace Transport = MobileGL::MG_Remote::Transport;
namespace Server = MobileGL::MG_Remote::Server;
namespace Codec = MobileGL::MG_Remote::Wire;
using MobileGL::MG_Remote::CapsAbiFingerprint;

namespace {

    std::string g_logPath;

    std::string ReadLog() {
        std::ifstream in(g_logPath, std::ios::binary);
        if (!in) return {};
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    unsigned ProcessId() {
#if defined(_WIN32)
        return static_cast<unsigned>(_getpid());
#else
        return static_cast<unsigned>(::getpid());
#endif
    }

    Transport::SessionSegmentSizes TestSizes() {
        Transport::SessionSegmentSizes sizes;
        sizes.CmdRingBytes = 64ull * 1024;
        sizes.StageBytes = 64ull * 1024;
        sizes.ReplyBytes = 64ull * 1024;
        // 1 MiB, not 16 KiB: the EGL cases have the server republish its caps snapshot into the
        // event ring several times with no client draining it, and a ring that fills would turn a
        // "no republish" assertion into a ring-full one.
        sizes.EventRingBytes = 1024ull * 1024;
        sizes.ReplySlotCount = 8;
        return sizes;
    }

    // The client half of a session, wired exactly the way ClientSession::Start wires it: the
    // transport pair, a real Hello, the server's Accept, the client's attach to the SAME
    // mapping, and w1's encoder over the shared SEG_CMD ring. It is NOT ClientSession itself,
    // because ClientSession::Start finishes by installing package c1's BackendObject_Remote,
    // which does not exist yet - and a fixture that skipped the handshake to get around that
    // would be testing a session this tree does not build.
    struct ServerFixture {
        std::unique_ptr<Transport::InProcessTransport> clientTransport;
        std::unique_ptr<Transport::InProcessTransport> serverTransport;
        Transport::SessionSegments clientSegments;
        Transport::RingProducer cmd;
        Transport::SessionProducer producer;
        Codec::SegmentTable clientTable;
        Codec::PipeWireEncoder encoder;
        Server::ServerSession* session = nullptr;

        bool Handshake() {
            Transport::InProcessTransport::CreatePair(clientTransport, serverTransport);
            {
                ::flatbuffers::FlatBufferBuilder builder(512);
                auto stamp = builder.CreateString(GIT_COMMIT_HASH_SHORT);
                auto hello = ::MobileGL::Wire::CreateHello(
                    builder, MOBILEGL_PROTOCOL_ABI_MAJOR, MOBILEGL_PROTOCOL_ABI_MINOR, stamp,
                    /*backendType=*/0u, /*pid=*/0u, /*configBlob=*/0, CapsAbiFingerprint());
                auto root = ::MobileGL::Wire::CreateCtrlEnvelope(
                    builder, ::MobileGL::Wire::CtrlMsg::Hello, hello.Union());
                ::MobileGL::Wire::FinishCtrlEnvelopeBuffer(builder, root);
                if (clientTransport->SendFrame(MobileGLByteSpan{builder.GetBufferPointer(),
                                                                builder.GetSize()}) != MOBILEGL_OK) {
                    return false;
                }
            }
            session = &Server::ServerSessionInstance();
            session->SetSegmentSizes(TestSizes());
            // The two halves v1 owns. Neither has a default and CallMask() Fatals on an unset
            // one, which is s1's BLOCKER fix and the reason this is stated rather than derived.
            session->SetCapabilityBits(0);
            session->SetConsumedSubsystems(MG_Pipe::kMGPipeSubsystemsMigratedAtP4a);
            if (session->Accept(*serverTransport) != MOBILEGL_OK) return false;
            if (clientSegments.AttachInProcess(session->Shm(), Transport::MemoryRole::Client) !=
                MOBILEGL_OK) {
                return false;
            }
            Transport::RingControl* control = clientSegments.CmdControl();
            cmd = Transport::RingProducer(control, clientSegments.CmdRingBase(),
                                          clientSegments.CmdRingCapacity(), Transport::RingCursorSet::Cmd);
            if (!cmd.Valid()) return false;
            producer.Attach(control, &cmd, &clientTransport->PeerDoorbell(),
                            &clientTransport->SelfDoorbell(), MG_Config::Ipc.SpinUs);
            clientTable.Install(Codec::kSegCmd, Codec::SegmentView{clientSegments.CmdRingBase(),
                                                                 clientSegments.CmdRingCapacity()});
            clientTable.Install(Codec::kSegStage,
                                Codec::SegmentView{clientSegments.StageBase(), clientSegments.StageBytes()});
            clientTable.Install(Codec::kSegReply,
                                Codec::SegmentView{clientSegments.ReplyBase(), clientSegments.ReplyBytes()});
            encoder = Codec::PipeWireEncoder(control, &cmd, nullptr, &clientTable);
            return true;
        }

        bool StartLoop() {
            return Server::ServerLoopInstance().Start(*session) == MOBILEGL_OK;
        }

        // Emit one record and wait for the server to apply it. This is the verb barrier's own
        // wait (R-3/R-5) - appliedSeq, not a sleep - so a case that goes green here has really
        // seen the apply thread move the watermark.
        bool EmitAndWait(MG_Pipe::MGPWireOp op, const void* payload, Uint64 payloadBytes,
                         Uint32 timeoutMs = 5000) {
            const Uint64 seq = encoder.EncodeRecord(op, payload, payloadBytes);
            if (seq == Codec::kInvalidSeq) return false;
            encoder.Publish();
            producer.PublishAndNotify(seq);
            return producer.WaitForApplied(seq, timeoutMs) == Transport::SessionWait::Reached;
        }

        bool EmitAndWaitWithTail(MG_Pipe::MGPWireOp op, const void* payload, Uint64 payloadBytes,
                                 const void* tail, Uint64 tailBytes, Uint32 timeoutMs = 5000) {
            const Uint64 seq = encoder.EncodeRecord(op, payload, payloadBytes, tail, tailBytes);
            if (seq == Codec::kInvalidSeq) return false;
            encoder.Publish();
            producer.PublishAndNotify(seq);
            return producer.WaitForApplied(seq, timeoutMs) == Transport::SessionWait::Reached;
        }

        // C10: poll the flag the Doorbell sets ONLY while it is actually blocked in Park
        // (RingControl::consumerParked, stored inside Doorbell::Wait's blocking section and
        // cleared on wake), NOT ServerLoop::ParkCount(), which increments on the way TOWARD a park
        // and stays set even if Wait returns without ever blocking. A loop whose wait was replaced
        // by `true` never sets this, so a poll for it TIMES OUT - which is what makes "the apply
        // thread really parked" a claim that can go red for its own reason.
        bool WaitUntilTrulyParked(Uint32 timeoutMs = 5000) {
            Transport::RingControl* control = clientSegments.CmdControl();
            if (control == nullptr) return false;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
            while (std::chrono::steady_clock::now() < deadline) {
                if (control->consumerParked.load(std::memory_order_acquire) == 1u) return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return false;
        }

        void Stop() {
            // Table 3's order: the client publishes and lets the server drain (EmitAndWait
            // already did), then Doorbell::Kill through the transport's Shutdown, THEN the
            // bounded join, and only then is anything an emitter owns released.
            if (clientTransport) clientTransport->Shutdown();
            Server::ServerLoopInstance().Stop();
            producer.Detach();
            encoder = Codec::PipeWireEncoder();
            cmd = Transport::RingProducer();
            clientSegments.Close();
            if (session != nullptr) session->Close();
            clientTransport.reset();
            serverTransport.reset();
        }
    };

    MG_Pipe::MGPClear WholeFramebufferClear() {
        MG_Pipe::MGPClear clear{};
        clear.Kind = Server::kMGPClearKindWhole;
        clear.BufferMask = 0x00004000; // GL_COLOR_BUFFER_BIT
        clear.DrawBufferIndex = -1;
        clear.ValueClass = Server::kMGPClearValueClassFloat;
        return clear;
    }

} // namespace

// =====================================================================================
// The thread
// =====================================================================================

// A control request must be able to un-park a thread waiting on kWaitForever. A Notify alone
// cannot do that - Doorbell::Wait consumes it with one Park, re-tests a condition nothing
// published, finds the bell alive and parks again - so the park predicate has to carry the
// control flag too. The case asserts the thread REALLY PARKED first, because a loop that
// spun instead would pass this without the predicate ever mattering.
TEST(ServerLoopTest, AControlRequestRunsOnTheApplyThreadAndUnparksIt) {
    ServerFixture fixture;
    ASSERT_TRUE(fixture.Handshake());
    ASSERT_TRUE(fixture.StartLoop());

    Server::ServerLoop& loop = Server::ServerLoopInstance();
    // C10: arm on the ACTUAL blocked park (consumerParked), not ParkCount() - a loop that spun
    // instead of blocking would pass a ParkCount() check without the wait ever mattering, which
    // was the whole finding.
    ASSERT_TRUE(fixture.WaitUntilTrulyParked())
        << "the apply thread never entered the blocking park, so this case would prove nothing "
           "about waking it (ParkCount() counts an intention, not a park)";

    struct Probe {
        std::thread::id ranOn{};
        Bool onApplyThread = false;
    } probe;
    // POSTED FROM A HELPER THREAD AND WAITED FOR WITH A DEADLINE (review v2 N-9). A park predicate
    // that lost the control flag never un-parks the apply thread and the post blocks for ever; the
    // case has to SAY that, in its own words, rather than hit ctest's timeout - a timeout is what an
    // unrelated hang produces too. Stop() afterwards is what frees the helper: m_stopRequested is in
    // the predicate, the thread exits, and its exit block answers the still-posted request
    // NOT_INITIALIZED (C2's block), so the join below cannot wedge either.
    MobileGLResult rc = MOBILEGL_ERR_INVALID_ARGUMENT;
    std::atomic<Bool> answered{false};
    std::thread::id posterId{};
    std::thread poster([&] {
        posterId = std::this_thread::get_id();
        rc = loop.RunOnApplyThread(
            +[](void* user) -> MobileGLResult {
                auto* p = static_cast<Probe*>(user);
                p->ranOn = std::this_thread::get_id();
                p->onApplyThread = Server::ServerLoop::OnApplyThread();
                return MOBILEGL_OK;
            },
            &probe);
        answered.store(true, std::memory_order_release);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!answered.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!answered.load(std::memory_order_acquire)) {
        ADD_FAILURE() << "a posted control request did not un-park the apply thread within 5 s: the park "
                         "predicate no longer carries the control flag, so a Notify alone cannot break a "
                         "kWaitForever park (Doorbell::Wait consumes it, re-tests a condition nothing "
                         "published, and parks again)";
        fixture.Stop();
        poster.join();
        return;
    }
    poster.join();

    EXPECT_EQ(rc, MOBILEGL_OK);
    // The CALLER is the poster thread, not the test thread (N-9 moved the post onto a helper).
    EXPECT_NE(probe.ranOn, posterId)
        << "the control request ran on the CALLER, which means the EGL lifecycle calls would "
           "reach the driver from the app thread and the context would never migrate";
    EXPECT_NE(probe.ranOn, std::this_thread::get_id());
    EXPECT_TRUE(probe.onApplyThread)
        << "the control request ran on the CALLER (OnApplyThread() answered false inside it)";
    EXPECT_FALSE(Server::ServerLoop::OnApplyThread());

    fixture.Stop();
}

// Re-entrancy is not a deadlock: ~BackendObject_DirectGLES reaches ReleaseEGLResources FROM the
// apply thread, so a post from there must run inline.
TEST(ServerLoopTest, APostFromTheApplyThreadItselfRunsInlineRatherThanDeadlocking) {
    ServerFixture fixture;
    ASSERT_TRUE(fixture.Handshake());
    ASSERT_TRUE(fixture.StartLoop());

    struct Outer {
        Bool innerRan = false;
        MobileGLResult innerRc = MOBILEGL_ERR_INVALID_ARGUMENT;
    } outer;
    const MobileGLResult rc = Server::ServerLoopInstance().RunOnApplyThread(
        +[](void* user) -> MobileGLResult {
            auto* o = static_cast<Outer*>(user);
            o->innerRc = Server::ServerLoopInstance().RunOnApplyThread(
                +[](void* inner) -> MobileGLResult {
                    *static_cast<Bool*>(inner) = true;
                    return MOBILEGL_OK;
                },
                &o->innerRan);
            return MOBILEGL_OK;
        },
        &outer);

    EXPECT_EQ(rc, MOBILEGL_OK);
    EXPECT_EQ(outer.innerRc, MOBILEGL_OK);
    EXPECT_TRUE(outer.innerRan);

    fixture.Stop();
}

// The bounded join. The thread is parked on kWaitForever when Stop() is called, so this is the
// exact lost-wakeup shape table 3 step 2 is about - and the bound is what turns a regression
// into a red test rather than a wedged CI job.
TEST(ServerLoopTest, StopKillsTheDoorbellJoinsAndTheThreadReallyExits) {
    ServerFixture fixture;
    ASSERT_TRUE(fixture.Handshake());
    ASSERT_TRUE(fixture.StartLoop());
    Server::ServerLoop& loop = Server::ServerLoopInstance();

    // C10: the same actual-park arming - a shutdown test whose thread never blocked would not be
    // exercising the lost-wakeup path table 3 step 2 is about.
    ASSERT_TRUE(fixture.WaitUntilTrulyParked())
        << "the thread must be BLOCKED in the park for this to be a shutdown test";
    ASSERT_TRUE(loop.Running());

    const auto began = std::chrono::steady_clock::now();
    fixture.Stop();
    const auto took = std::chrono::steady_clock::now() - began;

    EXPECT_FALSE(loop.Running());
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(took).count(), 5000)
        << "Stop() took the whole bound, which means it hit the join timeout rather than a "
           "wakeup";
}

// MOBILEGL_IPC_SERVER_AFFINITY is kept as the raw string BECAUSE the resolved mask is what gets
// logged: an affinity that silently did nothing looks exactly like one that worked. So the
// resolved mask has to be readable, and `off` has to resolve to zero rather than to "auto".
TEST(ServerLoopTest, TheAffinityStringResolvesToAMaskAndTheMaskIsLogged) {
    const String saved = MG_Config::Ipc.ServerAffinity;
    MG_Config::Ipc.ServerAffinity = "off";

    ServerFixture fixture;
    ASSERT_TRUE(fixture.Handshake());
    ASSERT_TRUE(fixture.StartLoop());
    ASSERT_TRUE(fixture.WaitUntilTrulyParked());
    EXPECT_EQ(Server::ServerLoopInstance().ResolvedAffinityMask(), 0u)
        << "`off` did not resolve to no-affinity";

    const std::string log = ReadLog();
    EXPECT_NE(log.find("RESOLVED mask"), std::string::npos)
        << "the apply thread did not log its resolved affinity mask; an affinity that silently "
           "did nothing is indistinguishable from one that worked";
    EXPECT_NE(log.find("mgl-srv-apply started"), std::string::npos);
    // M-4: `off` must be a RECOGNISED string, not fall through to the numeric-parse "not
    // recognised" arm. Deleting the `off` branch in RequestedAffinityMask makes off unrecognised;
    // this asserts it did not, so that branch's deletion goes red here (its own reason).
    EXPECT_EQ(log.find("is not `auto`, `off` or a number"), std::string::npos)
        << "`off` was treated as an unrecognised affinity string; its own branch is gone";

    fixture.Stop();
    MG_Config::Ipc.ServerAffinity = saved;
}

// M-4, the explicit-mask half: an EXPLICIT mask the box can honour must resolve to itself and be
// LOGGED. `off` -> 0 and `0x3` -> 0x3 are two answers that must DIFFER, so the resolver cannot be a
// constant. This case does NOT gate codex 11's effective-mask read-back - on an unrestricted box the
// request and the effective set are the same and `return requested` stays green here (review v2
// item 6, which struck the claim the first version of this comment made); the read-back's own gate
// is TheResolvedAffinityMaskIsTheKernelsEffectiveSetNotTheRequest below. Red once by making the
// resolver return a constant 0: the explicit-mask assert reads 0.
TEST(ServerLoopTest, TheExplicitAffinityMaskResolvesToItselfAndIsLogged) {
#if defined(__linux__) || defined(__ANDROID__)
    if (std::thread::hardware_concurrency() < 2) {
        GTEST_SKIP() << "needs at least 2 online cpus to honour 0x3";
    }
    const String saved = MG_Config::Ipc.ServerAffinity;
    MG_Config::Ipc.ServerAffinity = "0x3"; // cpu 0 and cpu 1, both online on any 2+-cpu box

    ServerFixture fixture;
    ASSERT_TRUE(fixture.Handshake());
    ASSERT_TRUE(fixture.StartLoop());
    ASSERT_TRUE(fixture.WaitUntilTrulyParked());

    EXPECT_EQ(Server::ServerLoopInstance().ResolvedAffinityMask(), 0x3u)
        << "an explicit mask the box can honour did not resolve to itself (0x3); the effective "
           "mask read back from the kernel differs from the request, or the resolver is broken";
    const std::string log = ReadLog();
    EXPECT_NE(log.find("RESOLVED mask 0x3"), std::string::npos)
        << "the logged RESOLVED mask is not the effective 0x3";

    fixture.Stop();
    MG_Config::Ipc.ServerAffinity = saved;
#else
    GTEST_SKIP() << "affinity is a Linux/Android facility";
#endif
}

// =====================================================================================
// The record path: stamp, apply, watermark
// =====================================================================================

// The whole phase's prerequisite. MGPipeApplyAccess deliberately does not stamp the poison
// generations, so under split NOTHING stamps unless the applier does - every FilledGen[] stays
// 0, MGPipeInputFieldIsFresh answers false for everything, and a server-side read aborts on the
// FIRST field inside SyncRenderState. This case asserts the stamp happened by reading the verb
// the stamp SET, which the case itself never writes.
TEST(ServerLoopTest, AClearRecordCrossesAndIsStampedAsAVerbBoundary) {
    ServerFixture fixture;
    ASSERT_TRUE(fixture.Handshake());
    ASSERT_TRUE(fixture.StartLoop());

    // The pre-state is the honest one: nothing has stamped yet in this process.
    ASSERT_NE(MG_Pipe::gPipeInputs.CurrentVerb(), MG_Pipe::MGPipeVerb::Clear);

    const MG_Pipe::MGPClear clear = WholeFramebufferClear();
    ASSERT_TRUE(fixture.EmitAndWait(MG_Pipe::MGPWireOp::Clear, &clear, sizeof(clear)));

    Server::ServerLoop& loop = Server::ServerLoopInstance();
    EXPECT_EQ(loop.DrainedRecords(), 1u);
    EXPECT_EQ(MG_Pipe::gPipeInputs.CurrentVerb(), MG_Pipe::MGPipeVerb::Clear)
        << "PipeApplier::StampVerbBoundary did not run, so every server-side PipeInputs read "
           "would abort on the first field inside SyncRenderState";

    // MANDATORY on leaving the applier (p1's M-5): without it a SPAWNED server latches the flag
    // for its whole life, every later read is judged against the last verb's mask, and the
    // sticky forwards start aborting under strict on the very case their exemption exists for.
    EXPECT_FALSE(MG_Pipe::gPipeInputs.ServerStampedVerb())
        << "MGPipeServerClearVerbBoundary was not called when the apply thread left the applier";

    // No backend object in this process, so the five class-B verbs DECLINE. Asserted rather
    // than skipped: a Clear that was APPLIED here would mean the sink found a function table
    // it had no business finding.
    EXPECT_EQ(fixture.session->Applier().Verbs().Clears(), 0u);

    fixture.Stop();
}

// R-9's batching ban, made checkable rather than merely stated. RingControl::appliedSeq has ONE
// writer (SessionConsumer::ApplyOne, +1 per record) and PipeWireDecoder keeps its own tally; a
// batched publish moves one and not the other, which a single counter could never tell apart.
TEST(ServerLoopTest, TheSessionWatermarkAndTheDecoderTallyAgreeAfterEveryRecord) {
    ServerFixture fixture;
    ASSERT_TRUE(fixture.Handshake());
    ASSERT_TRUE(fixture.StartLoop());

    const MG_Pipe::MGPClear clear = WholeFramebufferClear();
    for (int i = 0; i < 8; ++i) {
        ASSERT_TRUE(fixture.EmitAndWait(MG_Pipe::MGPWireOp::Clear, &clear, sizeof(clear))) << i;
        EXPECT_EQ(fixture.session->Consumer().AppliedSeq(), static_cast<Uint64>(i + 1));
        EXPECT_EQ(fixture.session->Applier().DecoderAppliedSeq(), static_cast<Uint64>(i + 1));
    }
    EXPECT_EQ(Server::ServerLoopInstance().DrainedRecords(), 8u)
        << "the loop's own record tally (DrainedRecords) did not move with the eight records it "
           "applied, so the ordering-defect control has nothing of its own to hold against appliedSeq";

    // retiredSeq is the watermark w1's SEG_STAGE allocator reclaims against; a loop that
    // applies and never retires ends the first MOBILEGL_IPC_STAGE_MB in Fatal{RingOverrun}.
    //
    // IT IS POLLED, NOT READ ONCE, and that is R-9's own rule rather than a papered-over race:
    // appliedSeq is the ONLY watermark P5 forbids batching, and every other one "may be
    // published LATE but never EARLY". The apply thread retires after the drain batch and
    // before it parks, so the barrier can release the client between the two - reading it
    // instantly would be asserting a promise R-9 deliberately does not make. The BOUND is what
    // keeps this a check: a loop that never retires times out here instead of passing.
    const auto retireDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (fixture.clientSegments.CmdControl()->retiredSeq.load() < 8u &&
           std::chrono::steady_clock::now() < retireDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_GE(fixture.clientSegments.CmdControl()->retiredSeq.load(), 8u)
        << "the apply loop advanced appliedSeq but never retired within 2 s, so SEG_STAGE would "
           "never be reclaimed and the first MOBILEGL_IPC_STAGE_MB would end in "
           "Fatal{RingOverrun}";

    fixture.Stop();
}

// =====================================================================================
// R-2.5 / rule C's mechanical control, running for real
// =====================================================================================

// MOBILEGL_IPC_AUDIT=1 fills a retired SEG_STAGE run with 0xDD once the applier has RETURNED,
// so an applier that kept the pointer reads the poison on the next frame instead of bytes that
// happen to still be there. Without this, an inproc implementation that kept a pointer is
// INDISTINGUISHABLE from one that copied - which is R-2's entire argument.
//
// The case asserts three separate things, because each one alone can be true for the wrong
// reason: that the record's bytes really crossed (memcmp before the poison), that the poison
// really ran (PoisonedStageBytes moved), and that it landed on the run the record named (the
// bytes read 0xDD afterwards, through the CLIENT's mapping of the same segment).
TEST(ServerLoopTest, TheAuditPoisonFillsExactlyTheStagedRunAfterTheApplierReturns) {
    const Bool savedAudit = MG_Config::Ipc.Audit;
    // Set BEFORE the loop starts: PipeWireDecoder reads it in its constructor, and its
    // constructor runs on the apply thread inside ServerLoop::Start.
    MG_Config::Ipc.Audit = true;

    ServerFixture fixture;
    ASSERT_TRUE(fixture.Handshake());
    ASSERT_TRUE(fixture.StartLoop());

    MG_Pipe::MGPResourceDesc create{};
    create.Resource.Slot = 61;
    create.Resource.Gen = 1;
    create.Target = static_cast<Uint8>(MG_Pipe::MGPipeResourceTarget::Tex2D);
    create.InternalFormat = 1;
    create.Width = 4;
    create.Height = 4;
    create.Depth = 1;
    create.ArrayLayers = 1;
    create.Levels = 1;
    create.Samples = 1;
    ASSERT_TRUE(fixture.EmitAndWait(MG_Pipe::MGPWireOp::ResourceCreate, &create, sizeof(create)));

    Vector<Uint8> texels(4 * 4 * 4, 0x5A);
    MG_Pipe::MGPSubData upload{};
    upload.Res = create.Resource;
    upload.Target = MG_Pipe::MGPipePackSubDataTarget(
        static_cast<Uint32>(MG_Pipe::MGPipeResourceTarget::Tex2D), 0u);
    upload.Level = 0;
    upload.UnionBox = MG_Pipe::MGPBox{0, 0, 0, 4, 4, 1};
    upload.RegionCount = 1;
    upload.Blob = fixture.encoder.StageBytes(texels.data(), texels.size());
    ASSERT_NE(upload.Blob.Size, 0u) << "rule A: a content record must declare non-zero bytes";

    // The bytes are in SEG_STAGE and readable through the CLIENT's own mapping right now - the
    // record has not been published yet, so nothing has retired it.
    const void* stagedBefore =
        fixture.clientTable.Resolve(upload.Blob.Seg, upload.Blob.Offset, upload.Blob.Size);
    ASSERT_NE(stagedBefore, nullptr);
    ASSERT_EQ(std::memcmp(stagedBefore, texels.data(), texels.size()), 0);

    MG_Pipe::MGPSubRegion region{};
    region.W = 4;
    region.H = 4;
    region.D = 1;
    ASSERT_TRUE(fixture.EmitAndWaitWithTail(MG_Pipe::MGPWireOp::ResourceSubData, &upload,
                                            sizeof(upload), &region, sizeof(region)));

    EXPECT_GT(fixture.session->Applier().PoisonedStageBytes(), 0u)
        << "MOBILEGL_IPC_AUDIT=1 poisoned nothing, so R-2.5's only mechanical control against a "
           "retained SEG_STAGE pointer never ran";

    const auto* poisoned = static_cast<const Uint8*>(
        fixture.clientTable.Resolve(upload.Blob.Seg, upload.Blob.Offset, upload.Blob.Size));
    ASSERT_NE(poisoned, nullptr);
    for (Uint64 i = 0; i < upload.Blob.Size; ++i) {
        ASSERT_EQ(poisoned[i], 0xDD) << "staged byte " << i << " was not poisoned; an applier "
                                        "that kept this pointer would still read real bytes and "
                                        "the control would prove nothing";
    }

    fixture.Stop();
    MG_Config::Ipc.Audit = savedAudit;
}

// =====================================================================================
// R-11 - the server's own copy of the staged bytes
// =====================================================================================

// THIS IS THE PROPERTY MOBILEGL_IPC_AUDIT=1's 0xDD FILL EXISTS TO TEST, at unit scope: after
// the source bytes are overwritten - which is exactly what the decoder does to a retired
// SEG_STAGE run - the server's copy still reads the original. An implementation that returned
// `raw - offset` cannot pass this, and the monolith arm below is the control that says so: it
// is the SAME call with the same inputs, and it must see the 0xDD.
TEST(StagedShadowTest, TheSplitArmCopiesAndSurvivesTheSourceBeingPoisoned) {
    Server::StagedShadowStore splitStore(/*copies=*/true);
    Server::StagedShadowStore monolithStore(/*copies=*/false);
    const int key = 0;

    Vector<Uint8> staged(32, 0xAB);
    const Uint8* splitBase = splitStore.Adopt(&key, 64, staged.data(), 16, staged.size());
    const Uint8* monolithBase = monolithStore.Adopt(&key, 64, staged.data(), 16, staged.size());

    ASSERT_NE(splitBase, nullptr);
    EXPECT_EQ(monolithBase, staged.data() - 16)
        << "the monolith arm must be the ORIGINAL expression, character for character, or every "
           "push and verify lane is running new code it was never measured against";
    EXPECT_NE(splitBase, monolithBase);

    // w1's retired-stage poison, by hand and at the right moment: the record has retired, so
    // the staging run is dead.
    std::fill(staged.begin(), staged.end(), Uint8{0xDD});

    for (SizeT i = 0; i < 32; ++i) {
        EXPECT_EQ(splitBase[16 + i], 0xAB) << "byte " << i << " of the server's copy is the "
                                              "poison, so the copy never happened";
    }
    // And the control: the monolith arm reads the poison, which is what makes the assertion
    // above a statement about copying rather than about the test's own buffer.
    EXPECT_EQ(monolithBase[16], 0xDD);
}

// The extent is EXACTLY what the record declared. Adjacent runs merge because there is no gap
// between them; runs with a gap do not, and that is the whole mechanism - it is what makes a
// missing record detectable instead of papered over by a widened INVALIDATE_RANGE.
TEST(StagedShadowTest, CoverageIsExactAndAGapIsNotCovered) {
    Server::StagedShadowStore store(/*copies=*/true);
    const int key = 0;
    Vector<Uint8> bytes(16, 0x11);

    store.Adopt(&key, 64, bytes.data(), 0, 16);
    store.Adopt(&key, 64, bytes.data(), 32, 16);
    EXPECT_EQ(store.CoveredRunCount(&key), 2u) << "two runs with a 16-byte gap merged into one";
    EXPECT_TRUE(store.IsCovered(&key, 0, 16));
    EXPECT_TRUE(store.IsCovered(&key, 32, 48));
    EXPECT_FALSE(store.IsCovered(&key, 16, 32)) << "the gap reads as covered";
    EXPECT_FALSE(store.IsCovered(&key, 0, 48)) << "a span across the gap reads as covered";
    EXPECT_FALSE(store.IsCovered(&key, 8, 40));

    // Filling the gap merges all three into one run, which is the proof that adjacency (not
    // proximity) is the merge rule.
    store.Adopt(&key, 64, bytes.data(), 16, 16);
    EXPECT_EQ(store.CoveredRunCount(&key), 1u);
    EXPECT_TRUE(store.IsCovered(&key, 0, 48));
}

// m-5's discriminator, at unit scope: HasShadow is TRUE for a key that was Adopted and FALSE once
// DropAll has run - which is exactly what tells the generation-reset block whether a twin's
// hostBytes names a live server shadow (keep it) or a freed one (null it). A version that answered
// "always live" would let a freed base reach the driver; "always gone" would drop a base a subdata
// just staged in the same generation (that regression really happened - TriangleScenario read a
// shifted VBO). Both directions are asserted here.
TEST(StagedShadowTest, HasShadowIsTrueAfterAdoptAndFalseAfterTheShadowIsDropped) {
    Server::StagedShadowStore store(/*copies=*/true);
    const int key = 0;
    Vector<Uint8> bytes(16, Uint8{0x44});
    EXPECT_FALSE(store.HasShadow(&key)) << "nothing staged yet";
    store.Adopt(&key, 16, bytes.data(), 0, 16);
    EXPECT_TRUE(store.HasShadow(&key)) << "a staged key must read live, or the reset block nulls a "
                                          "base a subdata just filled";
    store.DropAll();
    EXPECT_FALSE(store.HasShadow(&key)) << "after DropAll the base is freed and must read gone, or "
                                           "a surviving twin hands the driver a dangling pointer";
}

TEST(StagedShadowTest, DropForgetsOneResourceAndDropAllForgetsEveryOne) {
    Server::StagedShadowStore store(/*copies=*/true);
    const int a = 0;
    const int b = 0;
    Vector<Uint8> bytes(8, 0x22);
    store.Adopt(&a, 8, bytes.data(), 0, 8);
    store.Adopt(&b, 8, bytes.data(), 0, 8);
    ASSERT_EQ(store.TrackedResources(), 2u);
    store.Drop(&a);
    EXPECT_EQ(store.TrackedResources(), 1u);
    EXPECT_FALSE(store.IsCovered(&a, 0, 8));
    EXPECT_TRUE(store.IsCovered(&b, 0, 8));
    store.DropAll();
    EXPECT_EQ(store.TrackedResources(), 0u);
}

// The Fatal, and it asserts ITS OWN failure string rather than "the process died". A death
// test that only checks for a crash goes green on any other abort in the same body, which is
// exactly the shape this wave shipped three of.
#if !defined(_WIN32)
TEST(StagedShadowTest, ADrainOutsideTheStagedCoverageIsFatalByName) {
    Server::StagedShadowStore store(/*copies=*/true);
    const int key = 0;
    Vector<Uint8> bytes(16, 0x33);
    const Uint8* base = store.Adopt(&key, 64, bytes.data(), 0, 16);

    // In coverage: no Fatal, and this is asserted first so that the death below cannot be a
    // function that aborts on everything.
    store.RequireCoverage(&key, base, 0, 16, "unit");
    // A base that is not this resource's shadow is not this rule's subject - that is the
    // legacy arm, whose MappedData() is valid for the whole store.
    store.RequireCoverage(&key, bytes.data(), 0, 64, "unit");

    // m-2: name the death MODE and the diagnostic, not just "the process died". The empty regex
    // accepted any death, including a SIGSEGV inside RequireCoverage; KilledBySignal(SIGABRT) pins
    // it to the Fatal's abort() (a segfault is SIGSEGV and fails this), and the log grep names the
    // exact wording. The log flush is pinned: Log.cpp's WriteToFile fflushes after every write and
    // MGLOG_F logs before abort(), so the line is on disk in the forked child before it dies.
    EXPECT_EXIT(store.RequireCoverage(&key, base, 0, 64, "unit_out_of_range"),
                ::testing::KilledBySignal(SIGABRT), ".*");
    const std::string log = ReadLog();
    EXPECT_NE(log.find("Fatal{StageSnapshotTooNarrow, \"unit_out_of_range\"}"), std::string::npos)
        << "the abort happened but not for this rule's reason; the log says: " << log;
}
#endif

// =====================================================================================
// R-11's PRODUCTION wiring (M-1 / codex 8), the EGL lifecycle seams (C2/M-6/M-7/C6/C7)
// =====================================================================================

// M-1 / codex 8: the R-11 gate that drives the PRODUCTION path - the real resource op table
// installed by RegisterBufferBackendOps, dispatched through MGPipeGetResourceOps()->SubData, i.e.
// the exact Managers.cpp:2118 call site R-11 changed - and NOT StagedShadowStore in isolation.
// StagedShadowTest stays as the container test; this is the gate that goes red under the two
// perturbations the reviewer used (restore `hostBytes = raw - offset`, neuter the coverage clamp).
#if !defined(_WIN32)
TEST(StagedShadowProductionTest, SubDataThroughTheRealOpsTableCopiesAndSurvivesTheSourcePoison) {
    // MG_Config::Transport is InProcess (main), so ServerStaged() latches its copying arm on.
    MG_Backend::DirectGLES::BufferImpl::RegisterBufferBackendOps();
    const MG_Pipe::MGPipeResourceOps* ops = MG_Pipe::MGPipeGetResourceOps();
    ASSERT_NE(ops, nullptr) << "RegisterBufferBackendOps did not install the resource op table";
    ASSERT_NE(ops->SubData, nullptr);

    MG_Pipe::MGPipeHandle res{};
    res.Slot = 7;
    res.Gen = 1;
    auto* twin = MG_Backend::DirectGLES::BufferImpl::GetOrCreateBufferResourceForHandle(res);
    ASSERT_NE(twin, nullptr);

    Vector<Uint8> src(32, Uint8{0xAB});
    MG_Pipe::MGPSubData rec{};
    rec.Res = res;
    rec.Target = MG_Pipe::MGPipePackSubDataTarget(MG_Pipe::kMGPipeResourceTargetBuffer, 0u);
    ASSERT_TRUE(MG_Pipe::MGPipeSetSubDataBufferRange(rec, 0, src.size()));
    // THE PRODUCTION CALL. Not StagedShadowStore::Adopt directly - the whole point of M-1.
    ops->SubData(res, rec, src.data());

    auto* found = MG_Backend::DirectGLES::BufferImpl::FindBufferResourceForHandle(res);
    ASSERT_NE(found, nullptr);
    ASSERT_NE(found->hostBytes, nullptr) << "Ops_H_SubData recorded no base at all";
    EXPECT_NE(found->hostBytes, static_cast<const Uint8*>(src.data()))
        << "hostBytes points into the CLIENT's staging bytes (raw - offset), the exact rule-C "
           "violation R-11 exists to fix - restore that line and this goes red";

    // w1's retired-stage poison, by hand and at the right moment: the record has 'retired', so the
    // client's staging run is dead. A server that copied still reads the original bytes.
    std::fill(src.begin(), src.end(), Uint8{0xDD});
    for (SizeT i = 0; i < src.size(); ++i) {
        ASSERT_EQ(found->hostBytes[i], 0xAB)
            << "byte " << i << " of the server base is the poison: Ops_H_SubData kept a pointer "
               "into SEG_STAGE instead of copying";
    }
    MG_Backend::DirectGLES::BufferImpl::UnregisterBufferBackendOps();
}
#endif

// C2: after the loop has stopped, a forwarder call must return NOT_INITIALIZED and must NOT run
// inline on the caller. The pre-fix code read m_running outside the control mutex and ran work
// inline for !m_running - which in the race the verifier's latch harness reproduced
// (v1-codex-verify.md C2) hung forever posting into a mailbox no thread pumps. The fix clears
// m_running UNDER the mutex and re-checks it there; this is the deterministic half of it (no
// latch needed: after Stop() m_running is false for certain).
TEST(ServerLoopTest, AForwarderCallAfterTheLoopStoppedReturnsNotInitializedAndDoesNotRunInline) {
    ServerFixture fixture;
    ASSERT_TRUE(fixture.Handshake());
    ASSERT_TRUE(fixture.StartLoop());
    ASSERT_TRUE(fixture.WaitUntilTrulyParked());
    fixture.Stop();
    ASSERT_FALSE(Server::ServerLoopInstance().Running());

    struct Probe {
        Bool ran = false;
    } probe;
    const MobileGLResult rc = Server::ServerLoopInstance().RunOnApplyThread(
        +[](void* user) -> MobileGLResult {
            static_cast<Probe*>(user)->ran = true;
            return MOBILEGL_OK;
        },
        &probe);

    EXPECT_EQ(rc, MOBILEGL_ERR_NOT_INITIALIZED)
        << "a forwarder call after Stop() did not return NOT_INITIALIZED (old code ran it inline)";
    EXPECT_FALSE(probe.ran) << "the work ran on the caller after the loop stopped";
}

// M-7: the OTHER !m_running arm - a backend built but no thread (Start refused / std::thread
// threw) - must be a NAMED Fatal, never a silent inline EGL-on-the-app-thread fallback. A split
// lane that ran eglMakeCurrent on the app thread would render correctly with the context on the
// wrong thread, which is the one outcome R-1 exists to make impossible.
#if !defined(_WIN32)
TEST(ServerLoopTest, AForwarderWithABackendButNoThreadIsFatalNotAnAppThreadFallback) {
    Server::ServerLoop& loop = Server::ServerLoopInstance();
    ASSERT_EQ(loop.CreateBackend(BackendType::DirectGLES), MOBILEGL_OK);
    ASSERT_FALSE(loop.Running());
    // A backend exists, no apply thread runs. A forwarder must abort by name.
    EXPECT_EXIT(Server::ServerReleaseEGLResources(), ::testing::KilledBySignal(SIGABRT), ".*");
    const std::string log = ReadLog();
    EXPECT_NE(log.find("Fatal{ApplyThreadNotRunning"), std::string::npos)
        << "the abort was not the M-7 named Fatal; the log says: " << log;
    loop.Stop(); // drop the backend in the parent
}
#endif

// M-6: ServerLoop::Stop() with no thread ever started must still DROP the private backend, which
// ShutdownSplitRoles relies on for the early-Start-failure path. If it does not, the server's
// BackendObject stays alive with g_resourceOps pointing into it and every later CreateBackend in
// the process is refused - which is exactly the leak M-6 names.
TEST(ServerLoopTest, StopWithoutAStartedThreadStillDropsThePrivateBackend) {
    Server::ServerLoop& loop = Server::ServerLoopInstance();
    ASSERT_EQ(loop.CreateBackend(BackendType::DirectGLES), MOBILEGL_OK);
    ASSERT_NE(loop.Backend(), nullptr);
    loop.Stop(); // the !joinable arm
    EXPECT_EQ(loop.Backend(), nullptr) << "Stop() left the private backend alive with no thread";
    EXPECT_FALSE(loop.Running());
    // A second bring-up must now succeed; CreateBackend refuses if m_backend != nullptr.
    EXPECT_EQ(loop.CreateBackend(BackendType::DirectGLES), MOBILEGL_OK)
        << "the leaked backend blocks the next split bring-up (CreateBackend's m_backend guard)";
    loop.Stop();
}

// C6: the server's format-capability accessor reads the SERVER's own backend cache, not the
// process global (which under split is the CLIENT's BackendObject_Remote). Pointer-identity: no
// mask injection needed - ActiveBackendFormatCaps() must return the server's cache address, not
// the global's. Red once by making ActiveBackendFormatCaps return pActiveBackendObject's.
TEST(ServerLoopTest, TheServerFormatCapsAccessorReadsTheServersOwnBackendNotTheGlobal) {
    Server::ServerLoop& loop = Server::ServerLoopInstance();
    ASSERT_EQ(loop.CreateBackend(BackendType::DirectGLES), MOBILEGL_OK);
    MG_Backend::BackendObject* server = loop.Backend();
    ASSERT_NE(server, nullptr);

    // A DIFFERENT object in the global the seven C6 reads used to follow.
    auto client = MakeUnique<MG_Backend::DirectGLES::BackendObject_DirectGLES>();
    MG_Backend::BackendObject* clientRaw = client.get();
    MG_Backend::pActiveBackendObject = std::move(client);

    const MG_Backend::FormatCapabilityCache* caps = MG_Backend::DirectGLES::ActiveBackendFormatCaps();
    EXPECT_EQ(caps, &server->GetFormatCapabilities())
        << "ActiveBackendFormatCaps returned the process global's cache; under split that is the "
           "CLIENT's BackendObject_Remote, not the server's private backend";
    EXPECT_NE(caps, &clientRaw->GetFormatCapabilities());

    loop.Stop();
    MG_Backend::pActiveBackendObject.reset();
}

// C7 / ID-54: the make-current DECISION, pure. A new tuple binds; an identical repeat is a no-op; a
// release request (the three NO_* markers) is recorded, not forwarded. The native-bind COUNT - what
// the driver actually saw - is measured by ServerLoopEglTest's C7 control on a real context below,
// which is the control round 2 said needed the joint lane. Red once by deleting the RepeatNoOp arm:
// an identical repeat then classifies as NativeBind.
TEST(ServerLoopTest, MakeCurrentClassifiesRepeatAndReleaseWithoutRebinding) {
    const EGLDisplay dpy = reinterpret_cast<EGLDisplay>(0x1);
    const EGLSurface draw = reinterpret_cast<EGLSurface>(0x2);
    const EGLSurface read = reinterpret_cast<EGLSurface>(0x2);
    const EGLContext ctx = reinterpret_cast<EGLContext>(0x3);

    // Nothing current yet: a genuine bind.
    EXPECT_EQ(Server::ClassifyEglMakeCurrent(false, EGL_NO_DISPLAY, EGL_NO_SURFACE, EGL_NO_SURFACE,
                                             EGL_NO_CONTEXT, dpy, draw, read, ctx),
              Server::EglBindAction::NativeBind);
    // The SAME tuple already held: a no-op, no native bind, no owner write.
    EXPECT_EQ(Server::ClassifyEglMakeCurrent(true, dpy, draw, read, ctx, dpy, draw, read, ctx),
              Server::EglBindAction::RepeatNoOp)
        << "an identical make-current was classified as a native rebind; the owner would be "
           "written twice and the caches invalidated for nothing";
    // A DIFFERENT tuple: a real rebind.
    const EGLContext otherCtx = reinterpret_cast<EGLContext>(0x9);
    EXPECT_EQ(Server::ClassifyEglMakeCurrent(true, dpy, draw, read, ctx, dpy, draw, read, otherCtx),
              Server::EglBindAction::NativeBind);
    // A release request, whatever is current: recorded, not forwarded (the context is held).
    EXPECT_EQ(Server::ClassifyEglMakeCurrent(true, dpy, draw, read, ctx, dpy, EGL_NO_SURFACE,
                                             EGL_NO_SURFACE, EGL_NO_CONTEXT),
              Server::EglBindAction::ClientRelease);
}

// =====================================================================================
// Round 3: the gates the v2 review found missing (N-5), each driving the PRODUCTION wiring
// =====================================================================================

// M-6 (review v2 item 7): the fix is the ServerLoop::Stop() call INSIDE ShutdownSplitRoles, so the
// gate calls ShutdownSplitRoles - not Stop - after the shape M-6 names: InitSplitRoles step 1 built
// the server's private backend and step 3 (ClientSession::Start) failed EARLY. The failure is a real
// one, not a skipped step: P5's client refuses every transport but InProcess by name, before Accept
// and before any ring exists. Red once by deleting Init.cpp's `ServerLoopInstance().Stop()` line:
// the backend outlives the shutdown and the next CreateBackend is refused.
TEST(ServerLoopTest, ShutdownSplitRolesAfterAnEarlyStartFailureDropsTheServersPrivateBackend) {
    Server::ServerLoop& loop = Server::ServerLoopInstance();
    ASSERT_EQ(loop.CreateBackend(BackendType::DirectGLES), MOBILEGL_OK);
    ASSERT_NE(MobileGL::MG_Remote::Client::ClientSessionInstance().Start(MG_Config::TransportMode::Spawn, ""),
              MOBILEGL_OK)
        << "the forced early Start failure did not fail; the case has nothing to shut down after";
    ASSERT_NE(loop.Backend(), nullptr);
    ASSERT_FALSE(loop.Running());

    MG_Backend::ShutdownSplitRoles();

    EXPECT_EQ(loop.Backend(), nullptr)
        << "ShutdownSplitRoles left the server's private backend alive after an early "
           "ClientSession::Start failure (M-6): every later split bring-up in this process is refused "
           "at CreateBackend's m_backend guard";
    EXPECT_FALSE(loop.Running());
    EXPECT_EQ(loop.CreateBackend(BackendType::DirectGLES), MOBILEGL_OK)
        << "the leaked backend blocks the next split bring-up";
    loop.Stop();
}

// codex 11 (review v2 item 6): the resolved mask must be the EFFECTIVE set the kernel took, read
// back with sched_getaffinity, not the request. The review's point was that on an unrestricted box
// the two never differ, so `return requested` stays green; this makes them differ without a cpuset:
// the request names one cpu this thread may use (taken from the kernel's own answer, so the case
// constructs nothing it observes) PLUS cpu 63, which does not exist on any box with fewer than 64
// cpus. sched_setaffinity accepts the mask with the absent cpu dropped, so the effective set is the
// one real cpu - and a resolver that echoes the request reports a cpu the thread can never run on.
// Red once by making ApplyAffinity `return requested;`: the resolved mask reads 0x8000000000000001-
// shaped instead of 0x1-shaped, and the log line names the wrong mask.
#if defined(__linux__) || defined(__ANDROID__)
TEST(ServerLoopTest, TheResolvedAffinityMaskIsTheKernelsEffectiveSetNotTheRequest) {
    const unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0 || hw >= 64) {
        GTEST_SKIP() << "needs a box with fewer than 64 cpus so that cpu 63 is absent (has " << hw << ")";
    }
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    ASSERT_EQ(sched_getaffinity(0, sizeof(allowed), &allowed), 0);
    int usable = -1;
    for (int cpu = 0; cpu < 63; ++cpu) {
        if (CPU_ISSET(cpu, &allowed)) {
            usable = cpu;
            break;
        }
    }
    if (usable < 0 || CPU_ISSET(63, &allowed)) {
        GTEST_SKIP() << "this thread's allowed set is not the shape the case needs";
    }
    const Uint64 requested = (1ull << usable) | (1ull << 63);
    const Uint64 effective = 1ull << usable;
    char requestedText[40];
    std::snprintf(requestedText, sizeof(requestedText), "0x%llx", static_cast<unsigned long long>(requested));
    char effectiveLine[64];
    std::snprintf(effectiveLine, sizeof(effectiveLine), "RESOLVED mask 0x%llx (0 = no affinity applied)",
                  static_cast<unsigned long long>(effective));

    const String saved = MG_Config::Ipc.ServerAffinity;
    MG_Config::Ipc.ServerAffinity = requestedText;

    ServerFixture fixture;
    ASSERT_TRUE(fixture.Handshake());
    ASSERT_TRUE(fixture.StartLoop());
    ASSERT_TRUE(fixture.WaitUntilTrulyParked());

    EXPECT_EQ(Server::ServerLoopInstance().ResolvedAffinityMask(), effective)
        << "the resolved affinity mask reports the REQUESTED set (" << requestedText
        << ") rather than the effective one the kernel took: cpu 63 does not exist on this " << hw
        << "-cpu box, sched_setaffinity dropped it, and codex 11's sched_getaffinity read-back is what "
           "makes that visible";
    const std::string log = ReadLog();
    EXPECT_NE(log.find(effectiveLine), std::string::npos)
        << "the logged RESOLVED mask is not the effective one (" << effectiveLine << ")";

    fixture.Stop();
    MG_Config::Ipc.ServerAffinity = saved;
}
#endif

namespace {
    // A second DirectGLES object whose format cache the case can FILL, so that the server's (empty,
    // never InitCapabilities'd) cache and the global's answer differently. Filling an INPUT is not
    // constructing the observed state (R-16): what is observed is which object each read follows.
    struct CapsProbeBackend final : MG_Backend::DirectGLES::BackendObject_DirectGLES {
        MG_Backend::FormatCapabilityCache& Caps() { return MutableFormatCapabilities(); }
    };
} // namespace

// C6 (review v2 item 9): the seven table-3 reads, asserted by their ANSWERS with two objects
// installed - not by the accessor's address, which is what round 2 gated and what reverting one
// call site at a time never moved. The server's cache is empty; the global's is not.
//
// Four of the seven are VALUE reads and the two objects' caches disagree for them: the
// HasCachedFormatCapability pair behind the caveat decision (Utils.cpp) and the
// ClampSamplesToBackendSupport pair (BackendObject_DirectGLES.cpp). Two are NULL-CHECK reads -
// GenerateFormatInfo's and BackendFormatAddsAlpha's "is there a backend at all" - which two live
// objects cannot tell apart, so for those the global is NULL and the server is present, which is
// exactly the window between InitSplitRoles step 1 (the server backend) and step 4 (the client
// object) that the reads used to answer "no backend" in. The seventh, UsesWidenedPacked16NormStorage's
// gate in front of a memoised driver probe, is a null check whose two arms give the same answer on
// the reference rasterizer (the probe says "not mirrored", the no-backend arm says false); it has
// no answer-level control on this box and the report says so.
//
// Red once, one site at a time: revert the HasCachedFormatCapability pair -> the caveat decision
// follows the global; revert ClampSamples -> the clamp answers 6; revert GenerateFormatInfo's null
// check -> RGB8 widens to RGBA8; revert BackendFormatAddsAlpha's -> adds-alpha answers true.
TEST(ServerLoopTest, TheSevenFormatCapabilityReadsAnswerFromTheServersBackendNotTheGlobal) {
    namespace TextureImpl = MG_Backend::DirectGLES::TextureImpl;
    Server::ServerLoop& loop = Server::ServerLoopInstance();
    ASSERT_EQ(loop.CreateBackend(BackendType::DirectGLES), MOBILEGL_OK);
    const SizeT tex2d = MG_Backend::GetFormatCapabilityTargetIndex(TextureTarget::Texture2D);
    const SizeT rb = MG_Backend::GetRenderbufferFormatCapabilityTargetIndex();
    const SizeT rgb8 = static_cast<SizeT>(TextureInternalFormat::RGB8);
    const SizeT rgba8 = static_cast<SizeT>(TextureInternalFormat::RGBA8);

    // (A) the global holds a DIFFERENT object whose cache says the opposite of "empty": RGB8 is
    // caveat-only there, and RGBA8 was "probed" at six samples - a count no driver's own maximum
    // is, so it cannot collide with the fallback the server's empty cache falls to.
    auto client = MakeUnique<CapsProbeBackend>();
    for (const SizeT target : {tex2d, rb}) {
        client->Caps().CaveatCaps[target][rgb8] |= MG_Backend::FormatCapability::Creatable;
        client->Caps().CaveatCaps[target][rgb8] |= MG_Backend::FormatCapability::FramebufferRenderable;
        client->Caps().SampleCounts[target][rgba8] = {6, 2};
    }
    MG_Backend::pActiveBackendObject = std::move(client);

    EXPECT_FALSE(TextureImpl::ShouldUseCaveatTextureFormat(TextureInternalFormat::RGB8, TextureTarget::Texture2D))
        << "the caveat decision followed the global's cache (caveat-only RGB8), not the server's "
           "(empty): the HasCachedFormatCapability pair reads pActiveBackendObject again";
    EXPECT_FALSE(TextureImpl::ShouldUseCaveatRenderbufferFormat(TextureInternalFormat::RGB8))
        << "the renderbuffer caveat decision followed the global's cache, not the server's";
    EXPECT_NE(MG_Backend::DirectGLES::ClampSamplesToBackendSupport(tex2d, TextureInternalFormat::RGBA8, GL_RGBA8, 8), 6)
        << "the sample clamp answered from the global's probed counts (6), not the server's";
    EXPECT_NE(MG_Backend::DirectGLES::ClampSamplesToBackendSupport(rb, TextureInternalFormat::RGBA8, GL_RGBA8, 8), 6)
        << "the renderbuffer sample clamp answered from the global's probed counts (6), not the server's";

    // (B) the null-check reads: global NULL, server present. RGB8_SNORM, not RGB8: the fallback
    // normalisation's three-channel widening is APPLICABLE to GL_RGB8_SNORM and not to GL_RGB8
    // (TextureFormatProcessor.cpp's applicability table), so it is the format whose answer the
    // "no backend" arm actually changes - the first cut of this case used RGB8 and both arms agreed.
    MG_Backend::pActiveBackendObject.reset();
    constexpr GLenum kGlRgb8Snorm = 0x8F96;
    GLenum internalFormat = 0;
    GLenum format = 0;
    GLenum type = 0;
    TextureImpl::GenerateTextureFormatInfo(TextureInternalFormat::RGB8Snorm, &internalFormat, &format, &type,
                                           TextureTarget::Texture2D);
    EXPECT_EQ(internalFormat, kGlRgb8Snorm)
        << "the format info followed the global (null, so the fallback normalisation widened RGB8_SNORM) "
           "instead of the server's backend (present, so native RGB8_SNORM); got 0x" << std::hex
        << internalFormat;
    EXPECT_FALSE(TextureImpl::BackendTextureFormatAddsAlpha(TextureInternalFormat::RGB8Snorm, TextureTarget::Texture2D))
        << "the adds-alpha answer followed the global (null, so the fallback adds alpha) instead of "
           "the server's backend";
    EXPECT_FALSE(TextureImpl::BackendRenderbufferFormatAddsAlpha(TextureInternalFormat::RGB8Snorm))
        << "the renderbuffer adds-alpha answer followed the global (null) instead of the server's backend";

    loop.Stop();
}

// =====================================================================================
// The EGL fixture: the SERVER's real DirectGLES backend, a real headless context (mesa
// surfaceless, llvmpipe) on the apply thread, driven only through v1's twelve forwarders
// =====================================================================================
//
// Round 2 shipped its C7, M-3 and m-5 claims with the caveat "the native count is a joint-lane
// control (needs a real EGL context)", and the review answered that nothing then gates them. This
// fixture is the answer to that: the server role's own backend, brought up exactly the way
// InitSplitRoles and the client's EGL virtuals bring it up - CreateBackend on the app thread, then
// InitializeEGLDisplay / CreateEGLPbufferSurface / MakeEGLCurrent as BLOCKING control requests on
// mgl-srv-apply - with no client object, no emit table and no scenario. What the joint lane adds is
// c1's client; what it does not add is anything these cases observe: the driver's own eglMakeCurrent
// count, the applier's read_pixels reply, and the backend's buffer twins.
//
// THE NATIVE COUNT IS READ WHERE THE DRIVER READS IT. DirectGLES dispatches every EGL call through
// its function table (g_EGLFuncs, filled by BackendObject_DirectGLES::Initialize); the fixture puts
// a counting trampoline in front of that table's eglMakeCurrent and forwards to the driver's entry
// point. Nothing in the production tree is edited or told, and no production counter is trusted for
// this number - ServerLoop::NativeBindCount() is the forwarder's view, and the whole point of the C7
// control is that the two can disagree (they did: 2 native binds per process with the forwarder
// counting 1, review v2 item 10).
//
// NO USABLE EGL IS A SKIP, UNLESS MOBILEGL_ITEST_REQUIRE_GPU SAYS OTHERWISE - the integration
// harness's own rule (ScenarioFixture.h): a developer box without mesa should not fail a run it could
// not perform, but a lane that relies on these cases sets the variable, and then an unusable EGL is
// a FAILURE naming the step, never a green that ran nothing (review v2 N-1's lesson).
#if !defined(_WIN32)
namespace {

    std::atomic<int> g_nativeEglBinds{0};
    std::atomic<int> g_nativeEglReleases{0};
    decltype(MG_Backend::DirectGLES::g_EGLFuncs.eglMakeCurrent) g_driverEglMakeCurrent = nullptr;

    EGLBoolean CountingEglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) {
        if (ctx == EGL_NO_CONTEXT) {
            g_nativeEglReleases.fetch_add(1, std::memory_order_acq_rel);
        } else {
            g_nativeEglBinds.fetch_add(1, std::memory_order_acq_rel);
        }
        return g_driverEglMakeCurrent(dpy, draw, read, ctx);
    }

    Bool RequireGpuFromEnvironment() {
        const char* value = std::getenv("MOBILEGL_ITEST_REQUIRE_GPU");
        return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
    }

    // The integration harness's headless pin (HeadlessGL.cpp, EnsureHeadlessPlatform), for the same
    // reason: mesa's surfaceless platform needs no display, and glvnd has to be told which vendor to
    // load. The vendor json is the one MG_IntegrationTest/CMakeLists.txt pins by default; an operator
    // who set __EGL_VENDOR_LIBRARY_FILENAMES keeps theirs.
    void PinHeadlessEglEnvironment() {
        if (std::getenv("EGL_PLATFORM") == nullptr) setenv("EGL_PLATFORM", "surfaceless", 1);
        unsetenv("DISPLAY");
        unsetenv("WAYLAND_DISPLAY");
        if (std::getenv("__EGL_VENDOR_LIBRARY_FILENAMES") == nullptr) {
            const char* mesa = "/usr/share/glvnd/egl_vendor.d/50_mesa.json";
            std::error_code ec;
            if (std::filesystem::exists(mesa, ec)) setenv("__EGL_VENDOR_LIBRARY_FILENAMES", mesa, 1);
        }
    }

    // A control request that runs an arbitrary callable on the apply thread. A std::function is
    // fine in a test; production's ControlWork is a raw pointer for the teardown path's sake.
    MobileGLResult OnApply(const std::function<void()>& body) {
        std::function<void()> copy = body;
        return Server::ServerLoopInstance().RunOnApplyThread(
            +[](void* user) -> MobileGLResult {
                (*static_cast<std::function<void()>*>(user))();
                return MOBILEGL_OK;
            },
            &copy);
    }

    struct EglServerFixture : ServerFixture {
        // The client's VIRTUAL handles. EGLImpl mints small integers, and every one of them is 0x1
        // on this host (the N-3 finding's whole point), so the cases use exactly that shape.
        static EGLDisplay Dpy() { return reinterpret_cast<EGLDisplay>(static_cast<std::uintptr_t>(0x1)); }
        static EGLSurface Surf() { return reinterpret_cast<EGLSurface>(static_cast<std::uintptr_t>(0x1)); }
        static EGLContext Ctx() { return reinterpret_cast<EGLContext>(static_cast<std::uintptr_t>(0x1)); }

        // The backend's draw-shaped paths (ReadPixels syncs the bound textures and the read
        // framebuffer) walk the process's frontend GLContext, which a bare unit process does not
        // have; installed for the case's life and put back afterwards (SplitBufferTest's shape).
        UniquePtr<MG_State::GLState::GLContext> savedContext;

        // Empty on success, else the step that failed - the harness's SkipReason shape.
        std::string BringUp() {
            PinHeadlessEglEnvironment();
            savedContext = Move(MG_State::pGLContext);
            MG_State::pGLContext = MakeUnique<MG_State::GLState::GLContext>();
            Server::ServerLoop& loop = Server::ServerLoopInstance();
            // InitSplitRoles step 1: the server's private backend, no GL and no EGL yet; its
            // Initialize() loads the driver's EGL/GLES entry points into the two tables.
            if (loop.CreateBackend(BackendType::DirectGLES) != MOBILEGL_OK) return "CreateBackend refused";
            const MG_External::EGLFunctionsTable& egl = MG_Backend::DirectGLES::g_EGLFuncs;
            if (egl.eglMakeCurrent == nullptr) return "libEGL did not load (no eglMakeCurrent entry point)";
            if (egl.eglMakeCurrent != &CountingEglMakeCurrent) g_driverEglMakeCurrent = egl.eglMakeCurrent;
            g_nativeEglBinds.store(0);
            g_nativeEglReleases.store(0);
            MG_External::EGLFunctionsTable counted = egl;
            counted.eglMakeCurrent = &CountingEglMakeCurrent;
            MG_Backend::DirectGLES::SetEGLFuncsTable(counted);
            // InitSplitRoles step 2: the session answers its caps snapshots from this backend, so
            // ServerMakeEGLCurrent's R-12 republish has something to publish (ID-67's control).
            Server::ServerSessionInstance().SetBackend(loop.Backend());
            if (!Handshake()) return "the session handshake";
            if (!StartLoop()) return "ServerLoop::Start";
            EGLint major = 0;
            EGLint minor = 0;
            if (!Server::ServerInitializeEGLDisplay(Dpy(), &major, &minor)) return "ServerInitializeEGLDisplay";
            // THE SURFACE'S OWN CREATION is what binds natively (InitPbufferSurface -> MakeCurrent).
            if (!Server::ServerCreateEGLPbufferSurface(Surf(), 64, 64)) {
                return "ServerCreateEGLPbufferSurface - no usable headless EGL (EGL_PLATFORM=surfaceless, "
                       "__EGL_VENDOR_LIBRARY_FILENAMES -> mesa/llvmpipe) on this box";
            }
            return {};
        }

        Bool MakeCurrent() { return Server::ServerMakeEGLCurrent(Dpy(), Surf(), Surf(), Ctx()); }
        Bool ReleaseCurrent() {
            return Server::ServerMakeEGLCurrent(Dpy(), EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        }

        void TearDown() {
            Server::ServerReleaseEGLResources();
            Stop();
            // The backend Stop() just destroyed must not be the session's answer for the next case.
            Server::ServerSessionInstance().SetBackend(nullptr);
            MG_State::pGLContext = Move(savedContext);
        }
    };

// FAIL() and GTEST_SKIP() both return from the enclosing void test body.
#define MGL_EGL_BRING_UP_OR_BAIL(fixture)                                                             \
    do {                                                                                              \
        const std::string why = (fixture).BringUp();                                                  \
        if (!why.empty()) {                                                                           \
            (fixture).Stop();                                                                         \
            MG_State::pGLContext = Move((fixture).savedContext);                                      \
            if (RequireGpuFromEnvironment()) {                                                        \
                FAIL() << "MOBILEGL_ITEST_REQUIRE_GPU is set, so the server's EGL bring-up failing is " \
                          "a failure, not a skip. It failed at: "                                     \
                       << why;                                                                        \
            }                                                                                         \
            GTEST_SKIP() << "no usable headless EGL for the server backend: " << why;                 \
        }                                                                                             \
    } while (0)

    // The production shape of a buffer under split, step by step through the applier's own entry
    // points and the REAL op table (Initialize registered it): resource_create mints the record,
    // resource_respecify stores the descriptor - its bytes never cross under split (table 1 row 19),
    // they arrive as resource_subdata right behind it - and resource_subdata stages bytes into the
    // server shadow, minting the twin if no draw has (ID-52 item 3). `definedContent` is the
    // descriptor's HasDefinedContent: what glBufferData(size, data) sets and glBufferData(size, NULL)
    // clears.
    MG_Pipe::MGPResourceDesc BufferDesc(Uint32 slot, Uint32 width, Bool definedContent) {
        MG_Pipe::MGPResourceDesc desc{};
        desc.Resource = MG_Pipe::MGPipeHandle{slot, 1};
        desc.Target = MG_Pipe::kMGPipeResourceTargetBuffer;
        desc.Width = width;
        desc.Height = 1;
        desc.Depth = 1;
        desc.ArrayLayers = 1;
        desc.Levels = 1;
        desc.Samples = 1;
        desc.Usage = static_cast<Uint32>(BufferUsage::StaticDraw);
        desc.HasDefinedContent = definedContent ? 1 : 0;
        return desc;
    }

    MG_Pipe::MGPipeHandle DeclareBuffer(const MG_Pipe::MGPResourceDesc& desc) {
        EXPECT_TRUE(MG_Pipe::MGPipeApplyResourceCreate(desc)) << "resource_create refused slot " << desc.Resource.Slot;
        EXPECT_TRUE(MG_Pipe::MGPipeApplyResourceRespecify(desc, nullptr)) << "resource_respecify refused";
        return desc.Resource;
    }

    void StageBytes(MG_Pipe::MGPipeHandle res, SizeT offset, SizeT size, Uint8 fill) {
        const MG_Pipe::MGPipeResourceOps* ops = MG_Pipe::MGPipeGetResourceOps();
        ASSERT_NE(ops, nullptr) << "no resource op table is registered";
        ASSERT_NE(ops->SubData, nullptr);
        Vector<Uint8> bytes(size, fill);
        MG_Pipe::MGPSubData rec{};
        rec.Res = res;
        rec.Target = MG_Pipe::MGPipePackSubDataTarget(MG_Pipe::kMGPipeResourceTargetBuffer, 0u);
        ASSERT_TRUE(MG_Pipe::MGPipeSetSubDataBufferRange(rec, offset, size));
        ops->SubData(res, rec, bytes.data());
    }

    MG_Backend::DirectGLES::BufferImpl::GLESBufferResource* Ensure(MG_Pipe::MGPipeHandle res) {
        return MG_Backend::DirectGLES::BufferImpl::EnsureBufferResourceForHandle({}, res);
    }

    MG_Backend::DirectGLES::BufferImpl::GLESBufferResource* Twin(MG_Pipe::MGPipeHandle res) {
        return MG_Backend::DirectGLES::BufferImpl::FindBufferResourceForHandle(res);
    }

} // namespace

// C7 / ID-54, THE NATIVE HALF, measured where the review said it was not: at the driver. Surface
// creation binds the context natively on the apply thread (that is bind #1 of the "2 per process"
// the review counted); the client's first eglMakeCurrent onto that surface used to be bind #2 and is
// now deduped by BackendObject_DirectGLES's ID-54 arm; an identical repeat is a forwarder-level
// RepeatNoOp; a client release-current is recorded and never reaches the driver; a bind after the
// release is a RepeatNoOp again because the driver never lost the context. ONE native bind, ZERO
// native releases, and the apply thread is still the owner at the end. Red once, three ways: (a) make
// ApplyMakeCurrent ignore the classification (the repeat is forwarded and the release reaches the
// driver), (b) make the backend's native call unconditional again (2 native binds), (c) forward the
// ClientRelease arm to the backend (1 native release).
TEST(ServerLoopEglTest, MakeCurrentBindsNativelyOncePerContextAndNeverForwardsAClientRelease) {
    EglServerFixture fixture;
    MGL_EGL_BRING_UP_OR_BAIL(fixture);
    Server::ServerLoop& loop = Server::ServerLoopInstance();

    ASSERT_EQ(g_nativeEglBinds.load(), 1) << "the surface's own creation did not bind natively exactly once";
    ASSERT_EQ(loop.NativeBindCount(), 0u);

    ASSERT_TRUE(fixture.MakeCurrent());
    EXPECT_EQ(g_nativeEglBinds.load(), 1)
        << "the client's first make-current cost a second native eglMakeCurrent on a surface its own "
           "creation had already made current on this thread (ID-54: bind once per tuple)";
    EXPECT_EQ(loop.NativeBindCount(), 1u) << "the first make-current was not forwarded at all";

    ASSERT_TRUE(fixture.MakeCurrent());
    EXPECT_EQ(loop.NativeBindCount(), 1u)
        << "an identical repeat make-current was forwarded as a native bind; the owner slot would be "
           "written again and the caches invalidated for nothing";
    EXPECT_EQ(g_nativeEglBinds.load(), 1);

    ASSERT_TRUE(fixture.ReleaseCurrent());
    EXPECT_EQ(loop.ClientReleaseCount(), 1u) << "the client release-current was not recorded";
    EXPECT_EQ(g_nativeEglReleases.load(), 0)
        << "a client release-current reached the driver as eglMakeCurrent(NO_CONTEXT): the apply thread "
           "lost the context it holds for life (ID-54: never unbind on a client release)";

    ASSERT_TRUE(fixture.MakeCurrent());
    EXPECT_EQ(loop.NativeBindCount(), 1u) << "a bind after the recorded, unforwarded release was forwarded again";
    EXPECT_EQ(g_nativeEglBinds.load(), 1);

    // The owner is still the apply thread by EGL ground truth (IsBackendContextCurrentOnThisThread
    // re-verifies against eglGetCurrentContext), and not this thread.
    Bool ownerIsApplyThread = false;
    ASSERT_EQ(OnApply([&] { ownerIsApplyThread = MG_Backend::DirectGLES::IsBackendContextCurrentOnThisThread(); }),
              MOBILEGL_OK);
    EXPECT_TRUE(ownerIsApplyThread) << "after the sequence the apply thread no longer owns the context";
    EXPECT_FALSE(MG_Backend::DirectGLES::IsBackendContextCurrentOnThisThread()) << "the app thread owns the context";

    fixture.TearDown();
}

// ID-67: an IDENTICAL tuple is a native no-op AND publishes no caps snapshot (the client's mirror
// generation must not move, nothing accumulates); a DIFFERENT tuple - a second MobileGL context onto
// the same surface, the same driver triple - is a real native bind AND a republish (R-12 arm (a)),
// so the client adopts it without a pump or a Present. Both halves measured: native binds at the EGL
// table, republishes at ServerMakeEGLCurrent's own tally. Red once, three ways: make the backend's
// skip answer "same tuple" for any tuple (the different tuple stays at 1 native bind), republish on
// a repeat (the repeat reads 2 republishes), republish only on the first bind (the different tuple
// reads 1).
TEST(ServerLoopEglTest, ADifferentTupleBindsNativelyAndRepublishesAnIdenticalRepeatDoesNeither) {
    EglServerFixture fixture;
    MGL_EGL_BRING_UP_OR_BAIL(fixture);
    Server::ServerLoop& loop = Server::ServerLoopInstance();
    const EGLDisplay dpy = EglServerFixture::Dpy();
    const EGLSurface surf = EglServerFixture::Surf();
    const EGLContext ctxA = EglServerFixture::Ctx();
    const EGLContext ctxB = reinterpret_cast<EGLContext>(static_cast<std::uintptr_t>(0x2));
    ASSERT_EQ(g_nativeEglBinds.load(), 1) << "the surface's own creation did not bind natively exactly once";
    ASSERT_EQ(loop.MakeCurrentRepublishCount(), 0u);

    // The first tuple adopts the creation's bind, and republishes (InitCapabilities has now run).
    ASSERT_TRUE(Server::ServerMakeEGLCurrent(dpy, surf, surf, ctxA));
    EXPECT_EQ(g_nativeEglBinds.load(), 1);
    EXPECT_EQ(loop.MakeCurrentRepublishCount(), 1u)
        << "the first make-current did not republish the caps snapshot (R-12 arm (a))";

    // Identical: 0 native binds, 0 republishes.
    ASSERT_TRUE(Server::ServerMakeEGLCurrent(dpy, surf, surf, ctxA));
    EXPECT_EQ(g_nativeEglBinds.load(), 1) << "an identical repeat bound natively";
    EXPECT_EQ(loop.MakeCurrentRepublishCount(), 1u)
        << "an identical repeat republished the caps snapshot: the client's mirror generation moved "
           "for nothing and unpumped snapshots accumulate (ID-67)";

    // Different (a second context onto the same surface): 1 native bind, 1 republish.
    ASSERT_TRUE(Server::ServerMakeEGLCurrent(dpy, surf, surf, ctxB));
    EXPECT_EQ(g_nativeEglBinds.load(), 2)
        << "a make-current with a DIFFERENT tuple (a second context onto the same surface) did not "
           "bind natively: DirectGLES's invalidations for the new frontend context never ran (ID-67)";
    EXPECT_EQ(loop.NativeBindCount(), 2u);
    EXPECT_EQ(loop.MakeCurrentRepublishCount(), 2u)
        << "a make-current with a different tuple did not republish the caps snapshot; the client "
           "would adopt nothing without a pump or a Present (R-12 arm (a), ID-67)";

    // Identical again, then back to the first: 0 + 0, then 1 + 1.
    ASSERT_TRUE(Server::ServerMakeEGLCurrent(dpy, surf, surf, ctxB));
    EXPECT_EQ(g_nativeEglBinds.load(), 2);
    EXPECT_EQ(loop.MakeCurrentRepublishCount(), 2u);
    ASSERT_TRUE(Server::ServerMakeEGLCurrent(dpy, surf, surf, ctxA));
    EXPECT_EQ(g_nativeEglBinds.load(), 3);
    EXPECT_EQ(loop.NativeBindCount(), 3u);
    EXPECT_EQ(loop.MakeCurrentRepublishCount(), 3u);

    fixture.TearDown();
}

// N-3: a destroy-recreate with the SAME handle values must be a real bind again. Before the fix the
// tuple survived ServerReleaseEGLResources, the recreated context's first make-current classified as
// a RepeatNoOp, nothing ran in the base class (no InitCapabilities, no current-thread record) and no
// caps were republished - a silent no-context-current from the forwarder's point of view, visible
// only as the next swap failing. Red once by deleting the ForgetCurrentTuple() calls the forwarders
// make: NativeBindCount() stays 1 and the swap after the re-create fails.
TEST(ServerLoopEglTest, ADestroyedContextForgetsTheTupleSoTheSameHandleValuesBindAgain) {
    EglServerFixture fixture;
    MGL_EGL_BRING_UP_OR_BAIL(fixture);
    Server::ServerLoop& loop = Server::ServerLoopInstance();
    ASSERT_TRUE(fixture.MakeCurrent());
    ASSERT_EQ(loop.NativeBindCount(), 1u);
    ASSERT_TRUE(Server::ServerSwapEGLBuffers(EglServerFixture::Dpy(), EglServerFixture::Surf()))
        << "the swap BEFORE the destroy failed, so the case could not tell lost bookkeeping from a swap "
           "that never worked";
    ASSERT_EQ(g_nativeEglBinds.load(), 1);

    // The context goes (DestroyEGLContext on the apply thread) and comes back under the same
    // handle values.
    Server::ServerReleaseEGLResources();
    EGLint major = 0;
    EGLint minor = 0;
    ASSERT_TRUE(Server::ServerInitializeEGLDisplay(EglServerFixture::Dpy(), &major, &minor));
    ASSERT_TRUE(Server::ServerCreateEGLPbufferSurface(EglServerFixture::Surf(), 64, 64));
    ASSERT_EQ(g_nativeEglBinds.load(), 2) << "the recreated surface did not bind natively";

    ASSERT_TRUE(fixture.MakeCurrent());
    EXPECT_EQ(loop.NativeBindCount(), 2u)
        << "the make-current after a destroy-recreate with the same handle values was classified as a "
           "RepeatNoOp: the tuple outlived the context it named (N-3), so no base-class bookkeeping "
           "ran and the caps snapshot was not republished";
    EXPECT_TRUE(Server::ServerSwapEGLBuffers(EglServerFixture::Dpy(), EglServerFixture::Surf()))
        << "the swap after the re-create failed: BackendObject::SwapEGLBuffers found no current-thread "
           "record, because the make-current that should have made one was treated as a repeat";
    EXPECT_EQ(g_nativeEglBinds.load(), 2) << "one native bind per context lifetime, not more";

    fixture.TearDown();
}

// ID-49's tight-size half, gated (review v2 N-5: "nothing, unit and joint"). The client's DstSize is
// deliberately WRONG - 80, the size a ROW_LENGTH=8 / SKIP_* client would compute for a 4x3 RGBA8
// read whose tight extent is 48 - and the reply must still be the tight 48 bytes: posted at 48,
// counted at 48, read into a scratch that grew to 48 and never to the client's 80, and stamped 48 in
// the slot header the client reads. Red once, three ways: post at info.DstSize, size the scratch from
// info.DstSize, compute `tight` from info.DstSize.
TEST(ServerLoopEglTest, AReadPixelsReplyIsTheTightExtentWhateverDstSizeTheClientSent) {
    EglServerFixture fixture;
    MGL_EGL_BRING_UP_OR_BAIL(fixture);
    ASSERT_TRUE(fixture.MakeCurrent());

    // THE CLIENT'S HALF OF THE VERB, exactly as glReadPixels runs it before it reaches a backend
    // (MGP_FILL(ReadPixels) in MG_Impl): the validate point fills gPipeInputs' residual fields
    // from the frontend context - the texture-unit base, the bound framebuffers, the pack state
    // - which the server's ReadPixels reads BARRIER-PULLED under R-1 while the client is parked
    // in the barrier (PipeInputs.h's class table). Without it the apply thread reads a block
    // nobody filled and walks a null texture-unit base; with it the case drives the same two
    // halves the inproc lane drives, in the same order, on the same process-wide block.
    MG_Pipe::MGPipeValidateForVerb(MG_Pipe::MGPipeVerb::ReadPixels);

    MG_Pipe::MGPReadbackInfo info{};
    info.Res = MG_Pipe::kMGPipeNullHandle; // read_pixels: the bound read surface answers
    info.Box = MG_Pipe::MGPBox{0, 0, 0, 4, 3, 1};
    info.Format = 0x1908; // GL_RGBA
    info.Type = 0x1401;   // GL_UNSIGNED_BYTE
    info.DstSize = 80;    // wrong on purpose; tight is 4 * 3 * 4 = 48
    const Uint64 seq = fixture.encoder.EncodeRecord(MG_Pipe::MGPWireOp::ReadPixels, &info, sizeof(info));
    ASSERT_NE(seq, Codec::kInvalidSeq);
    fixture.encoder.Publish();
    fixture.producer.PublishAndNotify(seq);
    ASSERT_EQ(fixture.producer.WaitForApplied(seq, 5000), Transport::SessionWait::Reached);

    const Server::ServerVerbSink& verbs = fixture.session->Applier().Verbs();
    ASSERT_EQ(verbs.Readbacks(), 1u) << "the read_pixels record was declined or errored rather than applied";
    EXPECT_EQ(verbs.ReadbackBytes(), 48u)
        << "the reply was posted at the client's DstSize (80), not the tight w*h*bpp extent (48) (ID-49)";
    EXPECT_EQ(verbs.ReadbackScratchBytes(), 48u)
        << "the scratch was sized from the client's DstSize rather than the tight extent; a DstSize "
           "smaller than tight would then be the heap overflow codex 1 found";

    // And through the client's own view of the slot: the seq stamped back, OK, 48 bytes.
    Transport::ReplySlotPool replies(fixture.clientSegments.ReplyBase(), fixture.clientSegments.ReplyBytes(),
                                     fixture.clientSegments.ReplySlotCount());
    ASSERT_TRUE(replies.Valid());
    Vector<Uint8> pixels(128, 0);
    Int32 status = -1;
    Uint64 size = 0;
    EXPECT_TRUE(replies.Read(seq, pixels.data(), pixels.size(), &status, &size));
    EXPECT_EQ(status, static_cast<Int32>(Transport::kReplyStatusOk));
    EXPECT_EQ(size, 48u) << "the slot header carries the client's DstSize, not the tight extent";

    fixture.TearDown();
}

// M-3 / codex 4, the POOL-REUSE reader (review v2 item 4: "nothing can notice it going away"). The
// production sequence, on the real op table and the real backend: A - glBufferData(64, data), drawn
// (ensured), then deleted on the apply thread, so its id retires into the buffer pool; two presents
// with a finish between them move the frame-completion watermark past the id's retire serial, which
// is the pool's hand-out rule. B - glBufferData(64, data2), whose 64 bytes cross as resource_subdata
// behind the respecify (table 1 row 19) - and only 16 of them arrive, the MISSING-RECORD shape. B's
// first ensure finds A's id in the pool and would seed the driver's whole 64-byte store from a shadow
// 48 bytes of which nothing staged. The refusal fires BEFORE the glBufferSubData, so the forked
// child - whose only thread is a copy of this one and holds no context - reaches it with no GL call
// that matters (a no-context call dispatches to a no-op). Red once by deleting the pool-reuse
// MGL_SERVER_STAGED_REQUIRE: the child seeds the store and does not die. (The ORPHANED shape of the
// same sequence, glBufferData(64, NULL) + 16 bytes, must NOT die: TheStreamingIdiom... below.)
TEST(StagedShadowProductionTest, ASparseShadowForcedThroughAPoolReuseUploadIsFatalByName) {
    EglServerFixture fixture;
    MGL_EGL_BRING_UP_OR_BAIL(fixture);
    ASSERT_TRUE(fixture.MakeCurrent());

    const MG_Pipe::MGPipeHandle a = DeclareBuffer(BufferDesc(21, 64, true));
    StageBytes(a, 0, 64, 0x11);
    Bool aEnsured = false;
    ASSERT_EQ(OnApply([&] {
                  aEnsured = Ensure(a) != nullptr;
                  MG_Pipe::MGPipeGetResourceOps()->Destroy(a);
                  for (int frame = 0; frame < 2; ++frame) {
                      if (MG_Backend::DirectGLES::g_GLESFuncs.glFinish) MG_Backend::DirectGLES::g_GLESFuncs.glFinish();
                      MG_Backend::DirectGLES::Present();
                  }
              }),
              MOBILEGL_OK);
    ASSERT_TRUE(aEnsured) << "A's ensure minted no storage, so nothing retired into the pool";

    const MG_Pipe::MGPipeHandle b = DeclareBuffer(BufferDesc(22, 64, true));
    StageBytes(b, 0, 16, 0x22);
    ASSERT_NE(Twin(b), nullptr) << "the subdata did not mint B's twin (ID-52 item 3)";
    ASSERT_EQ(Twin(b)->id, 0u) << "B already has a store; the pool-reuse arm cannot be reached";

    EXPECT_EXIT(Ensure(b), ::testing::KilledBySignal(SIGABRT), ".*");
    const std::string log = ReadLog();
    EXPECT_NE(log.find("Fatal{StageSnapshotTooNarrow, \"pool_reuse_whole_store\"}"), std::string::npos)
        << "the abort was not the pool-reuse reader's coverage refusal (M-3); the log says: " << log;

    fixture.TearDown();
}

// M-3 / codex 4, the RESPECIFY reader. A - glBufferData(64, data), drawn (ensured: id, storage,
// serial all current). Then glBufferData(64, data2) again, whose bytes do not cross (table 1 row 19):
// the record says "defined content", the old shadow is dropped, the store is pending a respecify -
// and only 16 of the 64 bytes then arrive, staged from the app thread the way the applier queues
// them off the context thread. The next ensure would RespecifyStorageWith(initialData = the sparse
// shadow) over the whole store. The refusal fires before that glBufferData. Red once by deleting the
// respecify MGL_SERVER_STAGED_REQUIRE.
TEST(StagedShadowProductionTest, ASparseShadowForcedThroughAStorageRespecifyIsFatalByName) {
    EglServerFixture fixture;
    MGL_EGL_BRING_UP_OR_BAIL(fixture);
    ASSERT_TRUE(fixture.MakeCurrent());

    const MG_Pipe::MGPResourceDesc desc = BufferDesc(23, 64, true);
    const MG_Pipe::MGPipeHandle res = DeclareBuffer(desc);
    StageBytes(res, 0, 64, 0x33);
    Bool ensured = false;
    ASSERT_EQ(OnApply([&] { ensured = Ensure(res) != nullptr; }), MOBILEGL_OK);
    ASSERT_TRUE(ensured);
    ASSERT_NE(Twin(res)->id, 0u);

    // Off the context thread on purpose (the apply thread is parked): the respecify takes the
    // "cannot touch GL now" arm and leaves the store PENDING, exactly as an emitted record does.
    ASSERT_TRUE(MG_Pipe::MGPipeApplyResourceRespecify(desc, nullptr));
    ASSERT_TRUE(Twin(res)->pendingRespecify) << "the respecify did not leave the store pending";
    StageBytes(res, 0, 16, 0x44);

    EXPECT_EXIT(Ensure(res), ::testing::KilledBySignal(SIGABRT), ".*");
    const std::string log = ReadLog();
    EXPECT_NE(log.find("Fatal{StageSnapshotTooNarrow, \"respecify_whole_store\"}"), std::string::npos)
        << "the abort was not the respecify reader's coverage refusal (M-3); the log says: " << log;

    fixture.TearDown();
}

// M-1 / codex 8, THE COVERAGE HALF (review v2 item 2: "gated nowhere"). glBufferData(64, NULL) +
// glBufferSubData(0, 16) + a draw: the shadow covers [0, 16) and the NULL respecify seeds nothing, so
// that is legal. Then a resource_flush_range over [16, 48) with no bytes (that is what the row
// carries under split, item 12) queues a range NOTHING STAGED, and the next draw-time drain would
// take it through the three-tier ladder - whose tier 1 is a range-invalidating map that declares
// the old bytes dead. RequireStagedCoverageForPendingRanges refuses before the drain, by name. Red
// once by neutering its loop body: the child then dies one tier later, inside UploadRangeFrom's own
// check, with a DIFFERENT site name - which this assertion refuses - or, on the tier-1 map, not at
// all.
TEST(StagedShadowProductionTest, AQueuedRangeOutsideTheStagedCoverageIsFatalAtTheDrainByName) {
    EglServerFixture fixture;
    MGL_EGL_BRING_UP_OR_BAIL(fixture);
    ASSERT_TRUE(fixture.MakeCurrent());

    const MG_Pipe::MGPipeHandle res = DeclareBuffer(BufferDesc(24, 64, false));
    StageBytes(res, 0, 16, 0x55);
    Bool ensured = false;
    ASSERT_EQ(OnApply([&] { ensured = Ensure(res) != nullptr; }), MOBILEGL_OK);
    ASSERT_TRUE(ensured);
    ASSERT_NE(Twin(res)->id, 0u);
    ASSERT_FALSE(Twin(res)->pendingRespecify);

    MG_Pipe::MGPFlushRange flush{};
    flush.Res = res;
    flush.Offset = 16;
    flush.Size = 32;
    MG_Pipe::MGPipeGetResourceOps()->FlushRange(res, flush, nullptr); // off the context thread: queued
    ASSERT_FALSE(Twin(res)->pendingRanges.empty()) << "the flush queued nothing, so no drain is owed";

    EXPECT_EXIT(Ensure(res), ::testing::KilledBySignal(SIGABRT), ".*");
    const std::string log = ReadLog();
    EXPECT_NE(log.find("Fatal{StageSnapshotTooNarrow, \"ensure_flush_pending\"}"), std::string::npos)
        << "the abort was not the pending-coverage clamp's (M-1, RequireStagedCoverageForPendingRanges); "
           "the log says: " << log;

    fixture.TearDown();
}

// m-5 / codex 5 (review v2 item 5: "the shipped control cannot reach the freed base"). This one can:
// glBufferData(64, data) drawn (the twin's hostBytes names the server shadow), then the context is
// lost (ServerReleaseEGLResources -> DestroyEGLContext -> OnBackendContextDestroyed -> DropAll frees
// every shadow; the twin table survives), then the context comes back and the twin is re-armed at
// its next ensure. The re-armed twin's base must be NULL. Without the null it still names the freed
// allocation and the whole-store respecify that follows reads it - the use-after-free ASan would
// name, asserted here as the dangling pointer itself. Red once by deleting the null.
TEST(StagedShadowProductionTest, ATwinSurvivingContextLossDropsItsFreedShadowBase) {
    EglServerFixture fixture;
    MGL_EGL_BRING_UP_OR_BAIL(fixture);
    ASSERT_TRUE(fixture.MakeCurrent());

    const MG_Pipe::MGPipeHandle res = DeclareBuffer(BufferDesc(25, 64, true));
    StageBytes(res, 0, 64, 0x66);
    Bool ensured = false;
    ASSERT_EQ(OnApply([&] { ensured = Ensure(res) != nullptr; }), MOBILEGL_OK);
    ASSERT_TRUE(ensured);
    ASSERT_NE(Twin(res)->hostBytes, nullptr) << "the drawn twin names no shadow, so there is nothing to free";

    Server::ServerReleaseEGLResources();
    ASSERT_NE(Twin(res), nullptr) << "the twin did not survive context loss; the m-5 hazard needs a survivor";

    EGLint major = 0;
    EGLint minor = 0;
    ASSERT_TRUE(Server::ServerInitializeEGLDisplay(EglServerFixture::Dpy(), &major, &minor));
    ASSERT_TRUE(Server::ServerCreateEGLPbufferSurface(EglServerFixture::Surf(), 64, 64));
    ASSERT_TRUE(fixture.MakeCurrent());
    ASSERT_EQ(OnApply([&] { ensured = Ensure(res) != nullptr; }), MOBILEGL_OK);
    ASSERT_TRUE(ensured);

    EXPECT_EQ(Twin(res)->hostBytes, nullptr)
        << "a twin that survived context loss still names the freed server shadow (m-5): the "
           "whole-store respecify that re-armed it read freed memory (ASan: heap-use-after-free at "
           "RespecifyStorageWith)";

    fixture.TearDown();
}

// M-2 / ID-52 item 3, SHIPPED AND EXECUTED (joint report joint-v1.md 3, "Audit and its R-16
// limit": the corrupt-staged-bytes perturbation existed only by hand). Under an active transport
// the server's staged copy is the draw's ONLY base; the frontend object's MappedData() - which under
// inproc is the same process's memory and always non-null - may not be read. So: a real frontend
// BufferObject whose shadow holds pattern A, a server shadow staged with pattern B through the real
// op table (what resource_subdata delivers), and the production ensure path given BOTH. The driver
// store must hold B. That is the executable form of "corrupt only the staged bytes and the draw
// shows it": B is the corruption, the upload is the draw, the mapped read-back is the picture. Red
// once by restoring the MappedData() fallback in liveHostBase(): the store then holds A, the
// client's bytes, and the R-2.5 audit could never reach a draw again.
TEST(ServerLoopEglTest, FrontendFramebufferDeathDeletesOnTheContextOwner) {
    MG_Config::Features.PipePush |= MG_Pipe::kMGPipeSubsystemEsprytSlots;
    EglServerFixture fixture;
    MGL_EGL_BRING_UP_OR_BAIL(fixture);
    ASSERT_TRUE(fixture.MakeCurrent());
    namespace GLES = MG_Backend::DirectGLES;
    static auto nativeDelete = GLES::g_GLESFuncs.glDeleteFramebuffers;
    static std::atomic<Uint32> deleted{0};
    static std::atomic<Bool> wrongThread{false};
    nativeDelete = GLES::g_GLESFuncs.glDeleteFramebuffers;
    deleted.store(0);
    wrongThread.store(false);
    auto framebuffer = MakeShared<MG_State::GLState::FramebufferObject>(901u);
    GLuint driverId = 0;
    ASSERT_EQ(OnApply([&] {
        auto& twin = GLES::FramebufferImpl::g_backendFramebufferObjects.GetOrCreate(framebuffer);
        twin = MakeShared<GLES::FramebufferImpl::BackendFramebufferObject>();
        driverId = twin->GetBackendFramebufferId();
        twin->Bind(FramebufferTarget::Draw);
        GLES::g_GLESFuncs.glDeleteFramebuffers = +[](GLsizei count, const GLuint* names) {
            if (!Server::ServerLoop::OnApplyThread()) wrongThread.store(true);
            deleted.fetch_add(count);
            nativeDelete(count, names);
        };
    }), MOBILEGL_OK);
    ASSERT_NE(driverId, 0u);
    framebuffer.reset(); // Frontend destructor runs on this client thread.
    EXPECT_EQ(deleted.load(), 1u);
    EXPECT_FALSE(wrongThread.load());
    ASSERT_EQ(OnApply([&] {
        GLES::g_GLESFuncs.glDeleteFramebuffers = nativeDelete;
        GLint bound = -1;
        GLES::g_GLESFuncs.glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &bound);
        EXPECT_EQ(bound, 0) << "native deletion must unbind the framebuffer in the owner context";
    }), MOBILEGL_OK);
    fixture.TearDown();
}

TEST(StagedShadowProductionTest, TheEnsurePathUploadsTheServerShadowNotTheClientObjectsBytes) {
    EglServerFixture fixture;
    MGL_EGL_BRING_UP_OR_BAIL(fixture);
    ASSERT_TRUE(fixture.MakeCurrent());

    // The frontend object: glBufferData(64, A). Its shadow is A and it declares defined content.
    Vector<Uint8> clientBytes(64, Uint8{0xA5});
    auto buffer = MakeShared<MG_State::GLState::BufferObject>(31u);
    buffer->Respecify(clientBytes.size(), clientBytes.data());
    ASSERT_NE(buffer->MappedData(), nullptr);
    ASSERT_EQ(buffer->MappedData()[0], 0xA5);
    ASSERT_TRUE(buffer->HasDefinedContent());

    // Keep this object coherently mapped: Ensure's retained backend sync site must not
    // re-enter the client producer and replace the staged 0x5B bytes with this 0xA5 map.
    ASSERT_NE(buffer->AcquireMemoryRange({0, 64}, BufferMappingAccessBit::Write |
                         BufferMappingAccessBit::Persistent | BufferMappingAccessBit::Coherent), nullptr);
    ASSERT_TRUE(buffer->IsMapped());

    // The server shadow for the twin the draw will use: B, staged the way the wire delivers it.
    const MG_Pipe::MGPipeHandle res = DeclareBuffer(BufferDesc(26, 64, true));
    StageBytes(res, 0, 64, 0x5B);

    // The production ensure, WITH the frontend object (the draw-time shape), on the apply thread;
    // then the driver's store read back through a READ map on the same thread.
    Vector<Uint8> store(64, 0);
    Bool ensured = false;
    Bool mapped = false;
    ASSERT_EQ(OnApply([&] {
                  ensured = MG_Backend::DirectGLES::BufferImpl::EnsureBufferResourceForHandle(buffer, res) != nullptr;
                  auto* twin = Twin(res);
                  if (!ensured || twin == nullptr || twin->id == 0) return;
                  const auto& gl = MG_Backend::DirectGLES::g_GLESFuncs;
                  gl.glBindBuffer(GL_ARRAY_BUFFER, twin->id);
                  void* view = gl.glMapBufferRange(GL_ARRAY_BUFFER, 0, 64, GL_MAP_READ_BIT);
                  if (view != nullptr) {
                      std::memcpy(store.data(), view, 64);
                      gl.glUnmapBuffer(GL_ARRAY_BUFFER);
                      mapped = true;
                  }
              }),
              MOBILEGL_OK);
    ASSERT_TRUE(ensured);
    ASSERT_TRUE(mapped) << "the driver store could not be mapped for reading, so the case cannot see the upload";

    EXPECT_EQ(store[0], 0x5B)
        << "the ensure path uploaded the CLIENT object's bytes (MappedData(), pattern A = 0xA5) rather "
           "than the server shadow (pattern B = 0x5B): under an active transport the staged copy must "
           "be the draw's only base (M-2 / ID-52 item 3), or a corrupt staged upload renders the "
           "frontend's correct bytes and the R-2.5 audit cannot reach a draw";
    EXPECT_EQ(store[63], 0x5B);
    // And the frontend object was not written through: its shadow is still A.
    EXPECT_EQ(buffer->MappedData()[0], 0xA5);

    fixture.TearDown();
}

namespace {
    std::string ReadLogFrom(SizeT offset) {
        const std::string whole = ReadLog();
        return offset < whole.size() ? whole.substr(offset) : std::string();
    }

    // THE STREAMING IDIOM, end to end, in a FORKED CHILD that brings the server up itself (the
    // integration harness's pre-flight shape): a Fatal on the apply thread ends the child, not the
    // case, so the case can name it. glBufferData(64, NULL) then glBufferSubData(0, 16) then a draw:
    // the frontend object ORPHANS its store and writes 16 bytes - which flips ITS HasDefinedContent
    // to true - while on the wire that is a resource_respecify with HasDefinedContent CLEAR and one
    // 16-byte resource_subdata, so the server shadow covers [0, 16) of 64 and the descriptor says
    // the rest is undefined by the application's own declaration. The draw's whole-store upload
    // (the respecify reader, or the pool-reuse reader when a 64-byte id is waiting in the pool)
    // must go through and leave 16 bytes of pattern and 48 of zero in the driver's store. Exit 0
    // when it did, 7 when the wrong bytes landed, 8 when the child's bring-up failed.
    enum class IdiomArm { Respecify, PoolReuse };

    [[noreturn]] void RunTheStreamingIdiomAndExit(IdiomArm arm) {
        EglServerFixture fixture;
        if (!fixture.BringUp().empty() || !fixture.MakeCurrent()) ::_exit(8);
        Vector<Uint8> sixteen(16, Uint8{0x77});
        auto buffer = MakeShared<MG_State::GLState::BufferObject>(arm == IdiomArm::PoolReuse ? 33u : 32u);
        buffer->Respecify(64, nullptr);
        buffer->UploadSubData(DataPtr{sixteen.data(), sixteen.size()}, 0);
        Bool ok = false;
        (void)OnApply([&] {
            if (arm == IdiomArm::PoolReuse) {
                const MG_Pipe::MGPipeHandle a = DeclareBuffer(BufferDesc(29, 64, true));
                StageBytes(a, 0, 64, 0x11);
                if (Ensure(a) == nullptr) return;
                MG_Pipe::MGPipeGetResourceOps()->Destroy(a);
                for (int frame = 0; frame < 2; ++frame) {
                    if (MG_Backend::DirectGLES::g_GLESFuncs.glFinish) MG_Backend::DirectGLES::g_GLESFuncs.glFinish();
                    MG_Backend::DirectGLES::Present();
                }
            }
            const Uint32 slot = arm == IdiomArm::PoolReuse ? 30u : 28u;
            const MG_Pipe::MGPipeHandle b = DeclareBuffer(BufferDesc(slot, 64, false));
            StageBytes(b, 0, 16, 0x77);
            auto* twin = MG_Backend::DirectGLES::BufferImpl::EnsureBufferResourceForHandle(buffer, b); // the draw
            if (twin == nullptr || twin->id == 0) return;
            const auto& gl = MG_Backend::DirectGLES::g_GLESFuncs;
            gl.glBindBuffer(GL_ARRAY_BUFFER, twin->id);
            const auto* view = static_cast<const Uint8*>(gl.glMapBufferRange(GL_ARRAY_BUFFER, 0, 64, GL_MAP_READ_BIT));
            if (view == nullptr) return;
            ok = view[0] == 0x77 && view[15] == 0x77 && view[16] == 0 && view[63] == 0;
            gl.glUnmapBuffer(GL_ARRAY_BUFFER);
        });
        ::_exit(ok ? 0 : 7);
    }
} // namespace

// M-3, ROUND 3's RULE, positive direction (the round-2 refusal aborted six LargeArenaAdoption /
// ResourceSubsystemControl entries on the joint by this exact name): the ordinary streaming idiom
// through the RESPECIFY reader is uploaded, not refused. Red once by making the refusal
// unconditional again (the descriptor's HasDefinedContent ignored): the child dies of
// Fatal{StageSnapshotTooNarrow, "respecify_whole_store"} and the appended log names it.
TEST(StagedShadowProductionTest, TheStreamingIdiomOrphanThenPartialSubDataIsUploadedNotRefused) {
    {
        EglServerFixture probe;
        MGL_EGL_BRING_UP_OR_BAIL(probe);
        probe.TearDown();
    }
    const SizeT mark = ReadLog().size();
    EXPECT_EXIT(RunTheStreamingIdiomAndExit(IdiomArm::Respecify), ::testing::ExitedWithCode(0), ".*")
        << "the streaming idiom - glBufferData(64, NULL), glBufferSubData(0, 16), draw - did not put "
           "its 16 bytes and 48 undefined (zero) bytes into the driver's store through the respecify "
           "reader: exit 7 is the wrong bytes, a signal is the whole-store refusal firing on a store "
           "the application itself orphaned";
    const std::string appended = ReadLogFrom(mark);
    EXPECT_EQ(appended.find("Fatal{StageSnapshotTooNarrow"), std::string::npos)
        << "the orphan-then-partial-subdata idiom was refused by name (M-3's whole-store refusal must "
           "key on the descriptor's HasDefinedContent, not on the frontend flag a partial subdata "
           "flips); the log says: " << appended;
}

// The same idiom through the POOL-REUSE reader: a 64-byte id retired to the pool by a previous
// buffer's delete, handed to the orphaned store's first draw, seeded from the sparse shadow.
TEST(StagedShadowProductionTest, TheStreamingIdiomThroughAPoolReuseIsUploadedNotRefused) {
    {
        EglServerFixture probe;
        MGL_EGL_BRING_UP_OR_BAIL(probe);
        probe.TearDown();
    }
    const SizeT mark = ReadLog().size();
    EXPECT_EXIT(RunTheStreamingIdiomAndExit(IdiomArm::PoolReuse), ::testing::ExitedWithCode(0), ".*")
        << "the streaming idiom through a recycled pool id did not put its 16 bytes and 48 undefined "
           "(zero) bytes into the driver's store: exit 7 is the wrong bytes, a signal is the "
           "whole-store refusal firing on a store the application itself orphaned";
    const std::string appended = ReadLogFrom(mark);
    EXPECT_EQ(appended.find("Fatal{StageSnapshotTooNarrow"), std::string::npos)
        << "the orphan-then-partial-subdata idiom through a pool reuse was refused by name (M-3's "
           "whole-store refusal must key on the descriptor's HasDefinedContent); the log says: "
        << appended;
}
#endif // !_WIN32

int main(int argc, char** argv) {
    // Before anything logs: MG_Util::Debug::InitFile() reads the variable once, on the first
    // write, and caches the FILE*. The name carries this process's pid, because
    // gtest_discover_tests runs every case as its own process, in parallel under ctest -j.
    namespace fs = std::filesystem;
    const fs::path path =
        fs::temp_directory_path() / ("mobilegl-serverloop-test-" + std::to_string(ProcessId()) + ".log");
    std::error_code ec;
    fs::remove(path, ec);
    g_logPath = path.string();
#if defined(_WIN32)
    _putenv_s("MOBILEGL_LOG_FILE_PATH", g_logPath.c_str());
#else
    setenv("MOBILEGL_LOG_FILE_PATH", g_logPath.c_str(), 1);
#endif
    // THIS PROCESS IS A SPLIT ONE. Everything under test reads MG_Config::Transport - the
    // staged-shadow arm, the applier's stamp path, ConfigLoader's own knobs - and a suite that
    // left it at Monolith would be a server test running the monolith answers, which is the
    // failure this phase is built to make impossible.
    MG_Config::Transport = MG_Config::TransportMode::InProcess;
    ::testing::InitGoogleTest(&argc, argv);
    const int rc = RUN_ALL_TESTS();
    fs::remove(path, ec);
    return rc;
}
