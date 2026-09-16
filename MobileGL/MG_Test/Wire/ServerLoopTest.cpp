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
#include <MG_Backend/DirectGLES/Managers.h>
#include <MG_Backend/DirectGLES/Utils.h>
#include <MG_Backend/MGPipe/PipeInputs.h>
#include <MG_Pipe/MGPipe.h>
#include <MG_Remote/CapsCodec.h>
#include <MG_Remote/Protocol/generated/protocol_generated.h>
#include <MG_Remote/Server/PipeApplier.h>
#include <MG_Remote/Server/ServerLoop.h>
#include <MG_Remote/Server/ServerSession.h>
#include <MG_Remote/Server/StagedShadow.h>
#include <MG_Remote/Transport/InProcessTransport.h>
#include <MG_Remote/Transport/Ring.h>
#include <MG_Remote/Transport/SessionRings.h>
#include <MG_Remote/Wire/PipeWireCodec.h>
#include <MG_State/GLState/TextureState/TextureEnum.h>
#include <MGGitHash.h>

#include <csignal>

#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
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
        sizes.EventRingBytes = 16ull * 1024;
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
    const MobileGLResult rc = loop.RunOnApplyThread(
        +[](void* user) -> MobileGLResult {
            auto* p = static_cast<Probe*>(user);
            p->ranOn = std::this_thread::get_id();
            p->onApplyThread = Server::ServerLoop::OnApplyThread();
            return MOBILEGL_OK;
        },
        &probe);

    EXPECT_EQ(rc, MOBILEGL_OK);
    EXPECT_NE(probe.ranOn, std::this_thread::get_id())
        << "the control request ran on the CALLER, which means the EGL lifecycle calls would "
           "reach the driver from the app thread and the context would never migrate";
    EXPECT_TRUE(probe.onApplyThread);
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

// M-4 / codex 11, the other half: an EXPLICIT mask must resolve to itself and be LOGGED as the
// EFFECTIVE mask the kernel took, not the requested one. `off` -> 0 and `0x3` -> 0x3 are two
// answers that must DIFFER, so the resolver cannot be a constant; and reading the mask back with
// sched_getaffinity is what makes a cpuset that trimmed the request visible (codex 11), which the
// requested-mask log hid. Red once by making ApplyAffinity return `requested`: the effective read
// is what a trimmed request diverges from, and the explicit-mask assert is what catches a broken
// resolver.
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
    EXPECT_EQ(Server::ServerLoopInstance().DrainedRecords(), 8u);

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

// C7 / ID-54: the make-current decision. A new tuple binds; an identical repeat is a no-op; a
// release request (the three NO_* markers) is recorded, not forwarded. The native-bind COUNT is a
// joint-lane control (it needs a real EGL context); this is the pure decision, which is what
// keeps "the context is bound once and held for life" from being a per-call storm. Red once by
// deleting the RepeatNoOp arm - an identical repeat then classifies as NativeBind.
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
