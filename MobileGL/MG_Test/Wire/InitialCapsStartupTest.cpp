// SPDX-License-Identifier: LGPL-3.0-only
// Starts at the post-Welcome boundary with real TCP endpoints and ControlInbox.
// The peer publishes real CapsSnapshot bytes; no private run-ahead flag is set.
#include <MG_Remote/Client/ClientSession.h>
#include <MG_Remote/CapsCodec.h>
#include <MG_Remote/Transport/ControlInbox.h>
#include <MG_Remote/Transport/SocketTransport.h>
#include <MG_Remote/Protocol/generated/protocol_generated.h>
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace MobileGL;
using namespace MobileGL::MG_Remote;
namespace {
    template <class Tag, typename Tag::Type member>
    struct StartupPeerAccess {
        friend typename Tag::Type StartupPeerMember(Tag) { return member; }
    };
    struct StartupControlTag {
        using Type = Transport::ITransport* Client::ClientSession::*;
        friend Type StartupPeerMember(StartupControlTag);
    };
    template struct StartupPeerAccess<StartupControlTag, &Client::ClientSession::m_transport>;
    struct StartupInboxTag {
        using Type = std::unique_ptr<Transport::ControlInbox> Client::ClientSession::*;
        friend Type StartupPeerMember(StartupInboxTag);
    };
    template struct StartupPeerAccess<StartupInboxTag, &Client::ClientSession::m_controlInbox>;

    class InitialCapsStartup : public testing::Test {
    protected:
        Client::ClientSession client;
        std::unique_ptr<Transport::SocketTransport> controlClient, controlPeer;
        std::unique_ptr<Transport::ILink> dataPeer;
        const decltype(MG_Config::Ipc) savedIpc = MG_Config::Ipc;
        const MG_Config::TransportMode savedTransport = MG_Config::Transport;

        void SetUp() override {
            ::alarm(10);
            MG_Config::Transport = MG_Config::TransportMode::Spawn;
            MG_Config::Ipc.RunAhead = 1;
            MG_Config::Ipc.VerbBarrier = 1;
            int reserve = ::socket(AF_INET, SOCK_STREAM, 0);
            ASSERT_GE(reserve, 0);
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            ASSERT_EQ(::bind(reserve, reinterpret_cast<sockaddr*>(&address), sizeof address), 0);
            socklen_t length = sizeof address;
            ASSERT_EQ(::getsockname(reserve, reinterpret_cast<sockaddr*>(&address), &length), 0);
            const std::string endpoint = "tcp://127.0.0.1:" + std::to_string(ntohs(address.sin_port));
            ::close(reserve);
            int listener = -1;
            ASSERT_EQ(Transport::SocketTransport::Listen(endpoint, &listener), MOBILEGL_OK);
            MobileGLResult accepted = MOBILEGL_ERR_TIMEOUT;
            std::thread accepting(
                [&] { accepted = Transport::SocketTransport::AcceptPair(listener, 2000, controlPeer); });
            const auto connected = Transport::SocketTransport::ConnectTo(endpoint, 2000, controlClient);
            accepting.join();
            ::close(listener);
            ASSERT_EQ(connected, MOBILEGL_OK);
            ASSERT_EQ(accepted, MOBILEGL_OK);
            ASSERT_TRUE(controlClient->IsTcp());
            ASSERT_TRUE(controlPeer->IsTcp());

            Transport::SessionSegmentSizes sizes;
            sizes.CmdRingBytes = 4096;
            sizes.StageBytes = 4096;
            sizes.EventRingBytes = 4096;
            sizes.ReplyBytes = 4096;
            ASSERT_EQ(client.AttachStreamLink(controlClient->TakeDataFd(), sizes), MOBILEGL_OK);
            ASSERT_EQ(Transport::CreateStreamLink(controlPeer->TakeDataFd(), sizes,
                                                  Transport::TransportRoleTag::ServerConsumer, dataPeer),
                      MOBILEGL_OK);
            dataPeer->InitializeEndpoints();
            client.*StartupPeerMember(StartupControlTag{}) = controlClient.get();
            client.*StartupPeerMember(StartupInboxTag{}) = std::make_unique<Transport::ControlInbox>(*controlClient);
        }
        void TearDown() override {
            if (client.DataLink()) client.DataLink()->Detach();
            if (dataPeer) dataPeer->Detach();
            client.Stop();
            MG_Config::Ipc = savedIpc;
            MG_Config::Transport = savedTransport;
            ::alarm(0);
        }
        MobileGLResult Start() {
            return client.FinishStartup(&client.DataLink()->ConsumerBell(), &client.DataLink()->ProducerBell(), false);
        }
        MobileGLResult SendCaps(Uint64 bits) {
            MG_Backend::DynamicBackendParameters dynamic{};
            MG_Backend::FormatCapabilityCache formats{};
            RendererInfo renderer{};
            Vector<Uint8> encodedFormats, encodedRenderer;
            if (!EncodeFormatCapabilities(formats, encodedFormats) || !EncodeRendererInfo(renderer, encodedRenderer))
                return MOBILEGL_ERR_PROTOCOL_MISMATCH;
            flatbuffers::FlatBufferBuilder builder;
            const auto parameters = builder.CreateVector(reinterpret_cast<const Uint8*>(&dynamic), sizeof dynamic);
            const auto rendererBytes = builder.CreateVector(encodedRenderer.data(), encodedRenderer.size());
            const auto formatBytes = builder.CreateVector(encodedFormats.data(), encodedFormats.size());
            const auto version = builder.CreateString("4.6");
            const auto caps =
                ::MobileGL::Wire::CreateCapsSnapshot(builder, parameters, rendererBytes, formatBytes, 0, version, bits,
                                                     static_cast<Uint32>(BackendType::DirectGLES));
            const auto envelope =
                ::MobileGL::Wire::CreateCtrlEnvelope(builder, ::MobileGL::Wire::CtrlMsg::CapsSnapshot, caps.Union());
            ::MobileGL::Wire::FinishCtrlEnvelopeBuffer(builder, envelope);
            return controlPeer->SendFrame({builder.GetBufferPointer(), builder.GetSize()});
        }
        MobileGLResult SendLog() {
            flatbuffers::FlatBufferBuilder builder;
            const auto line = ::MobileGL::Wire::CreateLogLine(
                builder, ::MobileGL::Wire::LogLevel::Info,
                builder.CreateString("initial caps intentionally delayed by test peer"));
            const auto envelope =
                ::MobileGL::Wire::CreateCtrlEnvelope(builder, ::MobileGL::Wire::CtrlMsg::LogLine, line.Union());
            ::MobileGL::Wire::FinishCtrlEnvelopeBuffer(builder, envelope);
            return controlPeer->SendFrame({builder.GetBufferPointer(), builder.GetSize()});
        }
        bool PumpSnapshot() {
            const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            do {
                if (client.PumpControlPlane()) return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } while (std::chrono::steady_clock::now() < end);
            return false;
        }
    };

    TEST_F(InitialCapsStartup, DelayedTcpSnapshotArmsRunAheadAndLaterSnapshotsCanOnlyDemote) {
        std::atomic<bool> startupReturned{false};
        bool returnedBeforeCaps = false;
        MobileGLResult logResult = MOBILEGL_ERR_TIMEOUT, capsResult = MOBILEGL_ERR_TIMEOUT;
        std::thread sender([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            returnedBeforeCaps = startupReturned.load(std::memory_order_acquire);
            logResult = SendLog();
            capsResult = SendCaps(MG_Pipe::kCapRunAheadApply);
        });
        const auto result = Start();
        startupReturned.store(true, std::memory_order_release);
        sender.join();
        ASSERT_EQ(logResult, MOBILEGL_OK);
        ASSERT_EQ(capsResult, MOBILEGL_OK);
        EXPECT_FALSE(returnedBeforeCaps) << "startup returned without a real initial snapshot";
        ASSERT_EQ(result, MOBILEGL_OK);
        EXPECT_TRUE(client.Started());
        EXPECT_TRUE(client.RunAheadArmed());
        ASSERT_EQ(SendCaps(0), MOBILEGL_OK);
        ASSERT_TRUE(PumpSnapshot());
        EXPECT_FALSE(client.RunAheadArmed());
        ASSERT_EQ(SendCaps(MG_Pipe::kCapRunAheadApply), MOBILEGL_OK);
        ASSERT_TRUE(PumpSnapshot());
        EXPECT_FALSE(client.RunAheadArmed()) << "a later cap must not re-promote this session";
    }

    TEST_F(InitialCapsStartup, AHealthyPeerWithoutInitialCapsFailsStartupWithoutDeviceLoss) {
        const auto before = std::chrono::steady_clock::now();
        EXPECT_EQ(Start(), MOBILEGL_ERR_TIMEOUT);
        const auto elapsed = std::chrono::steady_clock::now() - before;
        EXPECT_GE(elapsed, std::chrono::seconds(4));
        EXPECT_LT(elapsed, std::chrono::seconds(8));
        EXPECT_FALSE(client.Started());
        EXPECT_FALSE(client.RunAheadArmed());
        EXPECT_FALSE(Client::ClientSession::DeviceLost());
    }

    TEST_F(InitialCapsStartup, ANullInitialSnapshotFailsInsteadOfStartingWithAPlaceholder) {
        flatbuffers::FlatBufferBuilder builder;
        const auto envelope = ::MobileGL::Wire::CreateCtrlEnvelope(builder, ::MobileGL::Wire::CtrlMsg::CapsSnapshot,
                                                                   flatbuffers::Offset<void>{});
        ::MobileGL::Wire::FinishCtrlEnvelopeBuffer(builder, envelope);
        ASSERT_EQ(controlPeer->SendFrame({builder.GetBufferPointer(), builder.GetSize()}), MOBILEGL_OK);
        EXPECT_EQ(Start(), MOBILEGL_ERR_PROTOCOL_MISMATCH);
        EXPECT_FALSE(client.Started());
        EXPECT_FALSE(Client::ClientSession::DeviceLost());
    }
} // namespace
