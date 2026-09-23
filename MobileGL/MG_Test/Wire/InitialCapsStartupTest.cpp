// SPDX-License-Identifier: LGPL-3.0-only
// Starts at the post-Welcome boundary with real TCP endpoints and ControlInbox.
// The peer publishes real CapsSnapshot bytes; no private run-ahead flag is set.
#include <MG_Remote/Client/ClientSession.h>
#include <MG_Remote/CapsCodec.h>
#include <MG_Remote/Handshake.h>
#include <MG_Remote/Transport/AuthToken.h>
#include <MG_Remote/Transport/ControlInbox.h>
#include <MG_Remote/Transport/SocketTransport.h>
#include <MG_Remote/Protocol/generated/protocol_generated.h>
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
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

            // X2 (P7): THE PRODUCT'S TCP RENDEZVOUS, NOT ConnectTo/AcceptPair. Two loopback TCP
            // connections opened back to back reach the listener's accept queue out of connect
            // order under load (measured: 5 of 3000 with 20 CPU hogs on the host), and AcceptPair
            // pairs them BY ORDER - the peer's control socket was then the client's data socket,
            // the first CapsSnapshot landed on the StreamLink reader and the process died of
            // Fatal{ProtocolCorruption, "StreamLink"} invalid data frame header. PH-7 (4) took
            // exactly this pairing out of the product on TCP (SocketTransport.h: "never paired
            // by arrival order"); the fixture now does what ServerMain's ListenerSource does -
            // the control connection is accepted before the data connection is opened, and the
            // data connection names itself with a DataBind nonce rather than by arrival.
            const auto connected = Transport::SocketTransport::ConnectControl(endpoint, 2000, controlClient);
            int peerControlFd = -1;
            const auto accepted = Transport::SocketTransport::AcceptOne(listener, 2000, &peerControlFd);
            if (accepted == MOBILEGL_OK)
                controlPeer = std::make_unique<Transport::SocketTransport>(peerControlFd, -1,
                                                                           Transport::TransportRole::Server);
            ASSERT_EQ(connected, MOBILEGL_OK);
            ASSERT_EQ(accepted, MOBILEGL_OK);
            ASSERT_TRUE(controlClient->IsTcp());
            ASSERT_TRUE(controlPeer->IsTcp());

            Uint8 nonce[Transport::kDataNonceBytes];
            ASSERT_EQ(Transport::SocketTransport::MintNonce(nonce, sizeof nonce), MOBILEGL_OK);
            const auto bind = EncodeDataBind(nonce, sizeof nonce);
            int clientDataFd = -1, peerDataFd = -1;
            ASSERT_EQ(Transport::SocketTransport::ConnectDataConnection(
                          endpoint, 2000, MobileGLByteSpan{bind.data(), bind.size()}, &clientDataFd),
                      MOBILEGL_OK);
            const auto dataAccepted = Transport::SocketTransport::AcceptOne(listener, 2000, &peerDataFd);
            ::close(listener);
            ASSERT_EQ(dataAccepted, MOBILEGL_OK);
            std::vector<Uint8> first;
            ASSERT_EQ(Transport::SocketTransport::ReceiveOneFrame(peerDataFd, 2000, 1024, &first), MOBILEGL_OK);
            Uint8 presented[Transport::kDataNonceBytes] = {};
            ASSERT_TRUE(DecodeDataBind(first, presented));
            ASSERT_TRUE(Transport::ConstantTimeNonceMatch(nonce, presented));

            Transport::SessionSegmentSizes sizes;
            sizes.CmdRingBytes = 4096;
            sizes.StageBytes = 4096;
            sizes.EventRingBytes = 4096;
            sizes.ReplyBytes = 4096;
            ASSERT_EQ(client.AttachStreamLink(clientDataFd, sizes), MOBILEGL_OK);
            ASSERT_EQ(Transport::CreateStreamLink(peerDataFd, sizes, Transport::TransportRoleTag::ServerConsumer,
                                                  dataPeer),
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
