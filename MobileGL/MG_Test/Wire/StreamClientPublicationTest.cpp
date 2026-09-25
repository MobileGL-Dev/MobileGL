// SPDX-License-Identifier: LGPL-3.0-only
// Production session submission with a real stream reader and a deliberately
// non-applying peer. A passing test cannot be supplied by a later client wait.
#include <MG_Remote/Client/ClientSession.h>
#include <MG_Remote/CapsCodec.h>
#include <MG_Remote/Transport/InProcessTransport.h>
#include <MG_Remote/Transport/StreamLink.h>
#include <MG_Remote/Protocol/generated/protocol_generated.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <MG_Pipe/PipeRoute.h>
// P12: the create row's observables and the wire tables it is installed as.
#include <MG_Remote/Client/WireTables.h>
#include <MG_Impl/GLImpl/Getter/GL_Getter.h>
#include <chrono>
#include <thread>
#include <csignal>
#include <sys/wait.h>
#include <vector>
#include <cstring>

using namespace MobileGL;
using namespace MobileGL::MG_Remote;
namespace {
    // Only the test's control endpoint is injected. A queued real CapsSnapshot
    // arms the production latch without a backend, EGL context, or private toggle.
    template <class Tag, typename Tag::Type member>
    struct PublicationPeerAccess {
        friend typename Tag::Type PublicationPeerMember(Tag) { return member; }
    };
    struct ControlEndpointTag {
        using Type = Transport::ITransport* Client::ClientSession::*;
        friend Type PublicationPeerMember(ControlEndpointTag);
    };
    template struct PublicationPeerAccess<ControlEndpointTag, &Client::ClientSession::m_transport>;

    class StreamClientPublication : public testing::Test {
    protected:
        Client::ClientSession client;
        std::unique_ptr<Transport::ILink> peer;
        std::unique_ptr<Transport::InProcessTransport> controlClient, controlPeer;
        const decltype(MG_Config::Ipc) savedIpc = MG_Config::Ipc;
        const MG_Config::TransportMode savedTransport = MG_Config::Transport;

        void SetUp() override {
            // A regression that turns submission into an applied wait fails
            // promptly instead of consuming the production 120-second budget.
            ::alarm(5);
            MG_Config::Transport = MG_Config::TransportMode::Spawn;
            MG_Config::Ipc.RunAhead = 1;
            MG_Config::Ipc.VerbBarrier = 1;
            MG_Config::Ipc.BatchWaits = 1;
            MG_Config::Ipc.PresentCredit = 3;
            Transport::SessionSegmentSizes sizes;
            sizes.CmdRingBytes = 4096;
            sizes.StageBytes = 4096;
            sizes.EventRingBytes = 4096;
            sizes.ReplyBytes = 4096;
            int sockets[2];
            ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);
            ASSERT_EQ(AttachData(sockets, sizes), MOBILEGL_OK);
            Transport::InProcessTransport::CreatePair(controlClient, controlPeer);
            client.*PublicationPeerMember(ControlEndpointTag{}) = controlClient.get();

            MG_Backend::DynamicBackendParameters dynamic{};
            MG_Backend::FormatCapabilityCache formats{};
            RendererInfo renderer{};
            Vector<Uint8> encodedFormats, encodedRenderer;
            ASSERT_TRUE(EncodeFormatCapabilities(formats, encodedFormats));
            ASSERT_TRUE(EncodeRendererInfo(renderer, encodedRenderer));
            flatbuffers::FlatBufferBuilder builder;
            const auto dynamicBytes = builder.CreateVector(reinterpret_cast<const Uint8*>(&dynamic), sizeof dynamic);
            const auto rendererBytes = builder.CreateVector(encodedRenderer.data(), encodedRenderer.size());
            const auto formatBytes = builder.CreateVector(encodedFormats.data(), encodedFormats.size());
            const auto version = builder.CreateString("4.6");
            const auto caps = ::MobileGL::Wire::CreateCapsSnapshot(builder, dynamicBytes, rendererBytes, formatBytes, 0,
                                                                   version, MG_Pipe::kCapRunAheadApply,
                                                                   static_cast<Uint32>(BackendType::DirectGLES));
            const auto envelope =
                ::MobileGL::Wire::CreateCtrlEnvelope(builder, ::MobileGL::Wire::CtrlMsg::CapsSnapshot, caps.Union());
            ::MobileGL::Wire::FinishCtrlEnvelopeBuffer(builder, envelope);
            ASSERT_EQ(controlPeer->SendFrame({builder.GetBufferPointer(), builder.GetSize()}), MOBILEGL_OK);
            ASSERT_EQ(
                client.FinishStartup(&client.DataLink()->ConsumerBell(), &client.DataLink()->ProducerBell(), false),
                MOBILEGL_OK);
            ASSERT_TRUE(client.RunAheadArmed());
        }
        void TearDown() override {
            // The peer intentionally never applies. Closing delivery before
            // teardown prevents its drain from supplying the assertion.
            if (client.DataLink()) client.DataLink()->Detach();
            if (peer) peer->Detach();
            client.Stop();
            MG_Config::Ipc = savedIpc;
            MG_Config::Transport = savedTransport;
            ::alarm(0);
        }
        virtual MobileGLResult AttachData(int sockets[2], const Transport::SessionSegmentSizes& sizes) {
            const auto result = client.AttachStreamLink(sockets[0], sizes);
            if (result != MOBILEGL_OK) {
                ::close(sockets[1]);
                return result;
            }
            const auto attached =
                Transport::CreateStreamLink(sockets[1], sizes, Transport::TransportRoleTag::ServerConsumer, peer);
            if (attached == MOBILEGL_OK) peer->InitializeEndpoints();
            return attached;
        }
        bool Upload(std::size_t count, unsigned char fill) {
            std::vector<Uint8> bytes(count, fill);
            MG_Pipe::MGPSubData record{};
            record.Res = {1, 1};
            record.Target = MG_Pipe::kMGPipeResourceTargetBuffer;
            record.UnionBox.W = static_cast<Uint32>(count);
            record.UnionBox.H = record.UnionBox.D = 1;
            return MG_Pipe::MGPipeRouteResourceSubData(record, bytes.data(), bytes.size());
        }
        bool PeerGone() {
            auto& bell = client.DataLink()->ProducerBell();
            const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            while (!bell.PeerHungUp() && std::chrono::steady_clock::now() < end)
                (void)bell.Park(1);
            return bell.PeerHungUp();
        }
        bool StageIs(unsigned char fill) {
            const auto& memory = client.DataLink()->Memory();
            const auto* bytes = static_cast<const Uint8*>(memory.StageBase());
            for (Uint64 i = 0; i < memory.StageBytes(); ++i)
                if (bytes[i] != fill) return false;
            return true;
        }
        bool ReceivedPublishedPrefix() {
            const auto head = client.DataLink()->Signals().CmdHead->load(std::memory_order_acquire);
            const auto signals = peer->Signals();
            return peer->ConsumerBell().Wait(
                *signals.ConsumerParked, [&] { return signals.CmdHead->load(std::memory_order_acquire) >= head; }, 0,
                1000);
        }
    };

    TEST_F(StreamClientPublication, AllThreePresentCreditsReachAnIdlePeerWithoutAnyClientWait) {
        for (Uint64 serial = 1; serial <= 3; ++serial) {
            MG_Pipe::MGPPresent present{};
            present.FrameSerial = client.AcquirePresentCredit();
            ASSERT_EQ(present.FrameSerial, serial);
            client.EmitAndWait(MG_Pipe::MGPWireOp::Present, &present, sizeof present, nullptr, 0, nullptr, 0, nullptr);
            ASSERT_TRUE(ReceivedPublishedPrefix()) << "present " << serial << " remained in the local batch";
        }
        EXPECT_EQ(client.Producer().Waits(), 0u);
        EXPECT_EQ(client.PresentCreditWaits(), 0u);
        EXPECT_EQ(client.DataLink()->Progress()->appliedSeq.load(), 0u);
        for (int record = 0; record != 3; ++record) {
            Transport::RingRecordView view{};
            ASSERT_TRUE(peer->CommandsIn().Pop(view));
            EXPECT_EQ(view.kind, static_cast<std::uint16_t>(MG_Pipe::MGPWireOp::Present));
        }
    }

    TEST_F(StreamClientPublication, ExportedGlFlushDeliversPublishedCommandsWithoutWaitingForApplied) {
        MG_Pipe::MGPMemoryBarrier barrier{};
        client.EmitAndWait(MG_Pipe::MGPWireOp::MemoryBarrier, &barrier, sizeof barrier, nullptr, 0, nullptr, 0,
                           nullptr);
        ASSERT_EQ(client.LastPublishedSeq(), 1u);
        EXPECT_EQ(peer->Signals().CmdHead->load(), 0u);
        ::glFlush();
        ASSERT_TRUE(ReceivedPublishedPrefix());
        EXPECT_EQ(client.Producer().Waits(), 0u);
        EXPECT_EQ(client.DataLink()->Progress()->appliedSeq.load(), 0u);
        Transport::RingRecordView view{};
        ASSERT_TRUE(peer->CommandsIn().Pop(view));
        EXPECT_EQ(view.kind, static_cast<std::uint16_t>(MG_Pipe::MGPWireOp::MemoryBarrier));
    }

    TEST_F(StreamClientPublication, ADisconnectedPeerDeclinesALargeUploadWithoutCopyOrWireOrdinal) {
        std::memset(client.DataLink()->Memory().StageBase(), 0x33, 4096);
        peer->Detach();
        ASSERT_TRUE(PeerGone());
        // Larger than the whole window: cancellation must precede allocation,
        // capacity checks and memcpy. The live path would refuse this request.
        EXPECT_FALSE(Upload(32768, 0x99));
        EXPECT_TRUE(Client::ClientSession::DeviceLost());
        EXPECT_EQ(MG_Impl::GLImpl::GetGraphicsResetStatus(), GL_UNKNOWN_CONTEXT_RESET);
        EXPECT_TRUE(client.Encoder().Cancelled());
        EXPECT_EQ(client.Encoder().EmitSeq(), 0u);
        EXPECT_EQ(client.LastPublishedSeq(), 0u);
        EXPECT_TRUE(StageIs(0x33));
        // A second local decline still consumes a nonzero MintReplySlot ticket.
        EXPECT_FALSE(Upload(16, 0x88));
        EXPECT_EQ(client.Encoder().EmitSeq(), 0u);
        MG_Pipe::MGPMemoryBarrier record{};
        EXPECT_EQ(client.Encoder().EncodeRecord(MG_Pipe::MGPWireOp::MemoryBarrier, &record, sizeof record,
                                                static_cast<const void*>(nullptr), Uint64{0}),
                  MG_Remote::Wire::kInvalidSeq);
        EXPECT_TRUE(StageIs(0x33));
    }

    // =====================================================================================
    // P12: THE CREATE WINDOW, ON THE ARM IT SHIPS ON
    // =====================================================================================
    //
    // WHY THIS CASE EXISTS AT ALL, and it is not symmetry. The window's own gates run in
    // RemoteClientControls, whose session is `TransportMode::InProcess` - ShmLink - where the window
    // is now OFF BY DESIGN, because a shared pool's slot is `seq % slotCount` and is reused, so a
    // deferred answer may be gone with no way to recover it. That means EVERY window-level gate in
    // the tree runs on the arm the feature does not ship on: three rounds of this feature were
    // green in the lanes while being inert on the device for exactly that reason. This case runs on
    // a real StreamLink with a real ClientSession and a peer that answers, which is the arm the
    // device runs, and it asserts the one property the device measured as broken: every deferred
    // answer comes BACK.
    //
    // THE PEER IS A SERVO, not a server: it pops each record, answers its seq, and publishes the
    // watermark, which is all the row needs (nothing here applies a record, and the row does not
    // care). What it must NOT do is answer late or out of order - the reply must precede the
    // progress frame that publishes its seq, because the drain's `WaitForApplied` oracle and the
    // store's invariant both rest on that ordering (StreamLink::PostReply sends Reply then
    // SendProgress under one lock).
    class StreamClientWindow : public StreamClientPublication {
    protected:
        std::atomic<bool> servoStop{false};
        std::thread servo;
        std::atomic<std::uint64_t> answered{0};
        void SetUp() override {
            StreamClientPublication::SetUp();
            // `Start()` does this on the real path (ClientSession.cpp:1615); this harness enters
            // through FinishStartup, so the routed rows have to be installed by hand or
            // MGPipeRouteResourceCreate would reach the monolith screen.
            Client::InstallClientWireTables();
        }
        void TearDown() override {
            servoStop.store(true);
            if (servo.joinable()) servo.join();
            Client::UninstallClientWireTables();
            StreamClientPublication::TearDown();
        }
        void StartServo() {
            servo = std::thread([this] {
                std::uint64_t applied = 0;
                while (!servoStop.load(std::memory_order_acquire)) {
                    Transport::RingRecordView view{};
                    if (!peer->CommandsIn().Pop(view)) {
                        std::this_thread::yield();
                        continue;
                    }
                    ++applied;
                    const auto posted = peer->PostReply(applied, 0, nullptr, 0);
                    Transport::Watermark::AdvanceApplied(*peer->Memory().CmdControl(), applied);
                    const auto flushed = peer->FlushProgress();
                    if (posted != MOBILEGL_OK || flushed != MOBILEGL_OK) {
                        std::printf("servo stopped: record=%llu post=%d flush=%d\n",
                                    static_cast<unsigned long long>(applied),
                                    static_cast<int>(posted), static_cast<int>(flushed));
                        std::fflush(nullptr);
                        return;
                    }
                    answered.store(applied, std::memory_order_release);
                }
            });
        }
    };

    TEST_F(StreamClientWindow, EveryDeferredCreateAnswerComesBackOnTheShippingArm) {
        const Uint32 savedWindow = MG_Config::Ipc.CreateWindow;
        MG_Config::Ipc.CreateWindow = 4;
        StartServo();
        const std::uint64_t takenBefore = Client::ClientCreateWindowTaken();
        constexpr Int32 kCreates = 24;
        const auto create = [](Int32 i) {
            MG_Pipe::MGPResourceDesc desc{};
            desc.Resource = MG_Pipe::MGPipeHandle{static_cast<Uint32>(i + 1), 1};
            desc.Target = static_cast<Uint8>(MG_Pipe::MGPipeResourceTarget::Tex2D);
            desc.InternalFormat = 1;
            desc.Width = 4;
            desc.Height = 4;
            desc.Depth = 1;
            desc.ArrayLayers = 1;
            desc.Levels = 1;
            desc.Samples = 1;
            (void)MG_Pipe::MGPipeRouteResourceCreate(desc);
        };
        create(0);
        // ---- AND THEN THE TRAFFIC THAT USED TO CROWD THE ANSWER OUT ----
        // THIS IS THE CASE THE GATE IS FOR, and without it the case passes against a store that
        // keeps only the last 8 answers (measured: it did). What killed the device was not the
        // creates themselves but the OTHER answers arriving between a create being deferred and
        // the drain coming back for it - the server is behind, and its late answers to the earlier
        // records fill a bounded buffer first. So twelve more records whose answers are WANTED are
        // emitted here, all of them declared and therefore all of them retained: a store bounded by
        // the last N answers loses the create's here, and the store under test does not.
        for (Int32 i = 0; i < 12; ++i) {
            MG_Pipe::MGPTextureParams params{};
            params.Res = MG_Pipe::MGPipeHandle{static_cast<Uint32>(100 + i), 1};
            Int32 status = 0;
            (void)client.EmitAndWaitTails(MG_Pipe::MGPWireOp::SetTextureParams, &params,
                                          sizeof params, nullptr, 0, nullptr, 0, &status, nullptr,
                                          /*wantReply=*/false, /*willReadReply=*/true);
        }
        // DELIVER THEM. A published record reaches the peer on a flush, not on PublishAndNotify -
        // the case above this one is built on exactly that (`glFlush` is what exports the batch),
        // and without this the servo sees nothing and the crowding never happens.
        ::glFlush();
        // WAIT UNTIL THOSE ANSWERS HAVE ACTUALLY ARRIVED, or the crowding is a race the store can
        // win by luck: the case would pass against the ring whenever the reader thread happened to
        // be behind. The servo posts them synchronously, and the reader is a thread on a socketpair,
        // so the settlement is a wait on the servo's own count plus one scheduling slice.
        const auto settled = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (answered.load(std::memory_order_acquire) < 13 &&
               std::chrono::steady_clock::now() < settled)
            std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        ASSERT_GE(answered.load(), 13u) << "the peer did not answer the crowding records";
        for (Int32 i = 1; i < kCreates; ++i) create(i);
        const std::uint64_t taken = Client::ClientCreateWindowTaken() - takenBefore;
        const Uint32 pending = Client::ClientCreateWindowPending();
        // THE PROPERTY THE DEVICE MEASURED AS BROKEN: `Taken` counts the answers the drain has
        // read OUT OF THE LINK'S STORE, so it can only move if retention works end to end - the
        // wire declared each seq before publishing it, the peer answered it, the store kept it
        // across however much traffic followed, and the drain found it again. On the device this
        // number was FROZEN AT 63 for a whole load frame while the ring was 8 deep (`taken=63
        // pushed=65 readfail=8,872`), which is exactly what a bounded buffer of the last N does.
        //
        // AND THE TAIL IS THE WINDOW'S OWN DEPTH, not a loss: nothing drains the window when no
        // further create arrives, so the last `pending` answers stay held - which is what
        // `pending` is. The conservation law is the assertion; `taken == kCreates` would be
        // asserting that the row drains on a path it does not have.
        EXPECT_GE(taken, static_cast<std::uint64_t>(kCreates) - Client::ClientCreateWindowEffective())
            << "the window collected " << taken << " of " << kCreates << " answers";
        EXPECT_EQ(taken + pending, static_cast<std::uint64_t>(kCreates))
            << "answers were lost or invented: taken " << taken << " + pending " << pending;
        EXPECT_LE(pending, Client::ClientCreateWindowEffective());
        // AND THE ROW REALLY IS DEFERRING HERE, so the case cannot pass by taking the blocking
        // path on every create (which is what a link the window refuses to run on looks like).
        EXPECT_EQ(Client::ClientCreateWindowEffective(), 4u);
        EXPECT_TRUE(client.LinkRetainsReplies());
        MG_Config::Ipc.CreateWindow = savedWindow;
    }

    class StreamClientStagingProcess : public StreamClientPublication {
    protected:
        pid_t peerPid = -1;
        MobileGLResult AttachData(int sockets[2], const Transport::SessionSegmentSizes& sizes) override {
            int ready[2];
            if (::pipe(ready) != 0) {
                ::close(sockets[0]);
                ::close(sockets[1]);
                return MOBILEGL_ERR_NOT_INITIALIZED;
            }
            // Fork before attaching either stream, so the child starts its own
            // real reader and inherits no running thread or live link mutex.
            peerPid = ::fork();
            if (peerPid == 0) {
                ::close(ready[0]);
                ::close(sockets[0]);
                std::unique_ptr<Transport::ILink> link;
                const auto result =
                    Transport::CreateStreamLink(sockets[1], sizes, Transport::TransportRoleTag::ServerConsumer, link);
                if (result == MOBILEGL_OK) link->InitializeEndpoints();
                const unsigned char success = result == MOBILEGL_OK;
                (void)::write(ready[1], &success, 1);
                ::close(ready[1]);
                if (!success) ::_exit(81);
                for (;;)
                    ::pause(); // Reader receives; the peer deliberately never applies.
            }
            ::close(ready[1]);
            ::close(sockets[1]);
            unsigned char success = 0;
            const auto received = peerPid > 0 ? ::read(ready[0], &success, 1) : -1;
            ::close(ready[0]);
            if (received != 1 || !success) {
                ::close(sockets[0]);
                return MOBILEGL_ERR_NOT_INITIALIZED;
            }
            return client.AttachStreamLink(sockets[0], sizes);
        }
        void TearDown() override {
            if (peerPid > 0) {
                (void)::kill(peerPid, SIGKILL);
                (void)::waitpid(peerPid, nullptr, 0);
                peerPid = -1;
            }
            StreamClientPublication::TearDown();
        }
    };

    TEST_F(StreamClientStagingProcess, APeerKilledDuringAFullStageWaitDeclinesWithoutCopyingTheNextBlob) {
        ASSERT_TRUE(Upload(4096, 0x5a));
        ASSERT_EQ(client.Encoder().EmitSeq(), 1u);
        client.Flush();
        auto* parked = client.DataLink()->Signals().ProducerParked;
        std::atomic<bool> killedWhileParked{false};
        std::thread killer([&] {
            const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!parked->load(std::memory_order_acquire) && std::chrono::steady_clock::now() < end)
                std::this_thread::yield();
            killedWhileParked.store(parked->load(std::memory_order_acquire) != 0, std::memory_order_release);
            (void)::kill(peerPid, SIGKILL);
        });
        const bool accepted = Upload(64, 0xa7);
        killer.join();
        EXPECT_TRUE(killedWhileParked.load());
        EXPECT_FALSE(accepted);
        EXPECT_TRUE(Client::ClientSession::DeviceLost());
        EXPECT_EQ(MG_Impl::GLImpl::GetGraphicsResetStatus(), GL_UNKNOWN_CONTEXT_RESET);
        EXPECT_GT(client.Encoder().StageReclaimWaits(), 0u);
        EXPECT_EQ(client.Encoder().EmitSeq(), 1u);
        EXPECT_EQ(client.LastPublishedSeq(), 1u);
        EXPECT_TRUE(StageIs(0x5a));
    }

    class HealthyStageTimeoutHarness : public StreamClientPublication {
    public:
        void TestBody() override {}
        void Run() {
            SetUp();
            client.Encoder().SetStageWaitTimeoutMs(5);
            if (!Upload(4096, 0x5a)) ::_exit(82);
            (void)Upload(64, 0xa7);
            ::_exit(83);
        }
    };

    TEST(StreamClientCancellationDeath, AHealthyPeerStageTimeoutDoesNotInventDeviceLoss) {
        EXPECT_EXIT(
            {
                ::signal(
                    SIGABRT, +[](int) {
                        const bool lost = Client::ClientSession::DeviceLost();
                        const char* text = lost ? "stage-timeout-device-lost=1\n" : "stage-timeout-device-lost=0\n";
                        (void)::write(STDERR_FILENO, text, sizeof("stage-timeout-device-lost=0\n") - 1);
                        ::_exit(lost ? 91 : 72);
                    });
                HealthyStageTimeoutHarness harness;
                harness.Run();
            },
            testing::ExitedWithCode(72), "RetirementWaitFailed(.|\n)*stage-timeout-device-lost=0");
    }
} // namespace
