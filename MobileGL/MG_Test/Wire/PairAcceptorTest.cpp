// MobileGL - MobileGL/MG_Test/Wire/PairAcceptorTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P11 PAIR. Server::PairAcceptor against real listeners - an abstract unix name and a loopback TCP
// port, every case on both - with the product's client half (SocketTransport::ConnectTo) and, where
// a case needs its halves in an order ConnectTo never produces, raw connections presenting their own
// PairBind.
//
// EVERY CASE PROVES IDENTITY, NOT THAT A PAIR CAME OUT. A server transport handed out is asked
// which client it is connected to (ServesExactly): a frame down its control connection has to arrive
// on one client's control connection, a frame up that client's control connection has to arrive at
// the server, and bytes on that same client's aux connection have to be the first bytes on the
// server's aux - which also proves the aux PairBind was read exactly and nothing past it was.
// Pairing by arrival order (what SocketTransport::AcceptPair did) fails the first two cases here.

#include <MG_Remote/CapsCodec.h> // WireFingerprint
#include <MG_Remote/Protocol/generated/protocol_generated.h>
#include <MG_Remote/Server/PairAcceptor.h>
#include <MG_Remote/Transport/Framing.h>
#include <MG_Remote/Transport/PairBind.h>
#include <MG_Remote/Transport/SocketTransport.h>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#if !defined(_WIN32)

using namespace MobileGL::MG_Remote;
using Transport::SocketTransport;

namespace {

    enum class Kind { Unix, Tcp };

    std::string MakeEndpoint(Kind kind) {
        static int serial = 0;
        if (kind == Kind::Unix) return "@mgl-pair-" + std::to_string(::getpid()) + "-" + std::to_string(++serial);
        // Listen refuses an ambiguous port 0, so a free loopback port is found first.
        const int probe = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t size = sizeof(address);
        if (probe < 0 || ::bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            ::getsockname(probe, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
            if (probe >= 0) ::close(probe);
            return "tcp://127.0.0.1:1";
        }
        ::close(probe);
        return "tcp://127.0.0.1:" + std::to_string(ntohs(address.sin_port));
    }

    // One bare connection - what a readiness probe opens - and nothing written on it.
    int RawConnect(const std::string& endpoint) {
        if (endpoint.compare(0, 6, "tcp://") == 0) {
            const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = htons(static_cast<std::uint16_t>(std::stoi(endpoint.substr(endpoint.rfind(':') + 1))));
            if (fd >= 0 && ::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) return fd;
            if (fd >= 0) ::close(fd);
            return -1;
        }
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        // An abstract name: sun_path[0] stays NUL and the length carries the end (SocketTransport's FillAddress).
        std::memcpy(address.sun_path + 1, endpoint.data() + 1, endpoint.size() - 1);
        const auto length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + endpoint.size());
        if (fd >= 0 && ::connect(fd, reinterpret_cast<sockaddr*>(&address), length) == 0) return fd;
        if (fd >= 0) ::close(fd);
        return -1;
    }

    bool SendFramed(int fd, const void* payload, std::size_t size) {
        std::vector<std::uint8_t> framed;
        if (Transport::AppendFrame(framed, payload, size) != MOBILEGL_OK) return false;
        return ::send(fd, framed.data(), framed.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(framed.size());
    }

    struct Nonce {
        std::uint8_t bytes[Transport::kPairNonceBytes] = {};
    };

    Nonce FreshNonce() {
        Nonce nonce;
        EXPECT_EQ(SocketTransport::MintNonce(nonce.bytes, sizeof(nonce.bytes)), MOBILEGL_OK);
        return nonce;
    }

    // One half of a client, opened by hand: connects and presents its PairBind, as ConnectTo would.
    int Half(const std::string& endpoint, const Nonce& nonce, bool aux) {
        const int fd = RawConnect(endpoint);
        if (fd < 0) return -1;
        const auto bind = Transport::EncodePairBind(nonce.bytes, aux);
        if (!SendFramed(fd, bind.data(), bind.size())) {
            ::close(fd);
            return -1;
        }
        return fd;
    }

    struct ScopedUnset {
        std::string name, prior;
        bool had = false;
        explicit ScopedUnset(const char* key) : name(key) {
            if (const char* value = std::getenv(key)) {
                prior = value;
                had = true;
            }
            ::unsetenv(key);
        }
        ~ScopedUnset() {
            if (had) ::setenv(name.c_str(), prior.c_str(), 1);
        }
    };

    struct ClientEnds {
        int control;
        int aux;
        const char* name;
    };

    // Reads exactly `size` bytes off `fd` within `timeoutMs`.
    bool ReadExactly(int fd, std::uint8_t* into, std::size_t size, int timeoutMs) {
        while (size != 0) {
            pollfd pfd{fd, POLLIN, 0};
            if (::poll(&pfd, 1, timeoutMs) != 1) return false;
            const ssize_t n = ::recv(fd, into, size, 0);
            if (n <= 0) return false;
            into += n;
            size -= static_cast<std::size_t>(n);
        }
        return true;
    }

    // Which of `clients` the server transport is connected to, and that it is connected to ALL of
    // that one client - its control connection both ways and its aux connection.
    ::testing::AssertionResult ServesExactly(SocketTransport& server, const std::vector<ClientEnds>& clients,
                                             int* outWho) {
        *outWho = -1;
        const std::string down = "server-to-control";
        if (server.SendFrame({down.data(), down.size()}) != MOBILEGL_OK)
            return ::testing::AssertionFailure() << "the server could not write its control connection (its peer is gone)";
        std::vector<pollfd> fds;
        for (const auto& client : clients) fds.push_back({client.control, POLLIN, 0});
        if (::poll(fds.data(), static_cast<nfds_t>(fds.size()), 2000) <= 0)
            return ::testing::AssertionFailure() << "no client's control connection received the server's frame";
        int who = -1;
        for (std::size_t index = 0; index < fds.size(); ++index)
            if ((fds[index].revents & POLLIN) != 0) who = static_cast<int>(index);
        if (who < 0) return ::testing::AssertionFailure() << "no client's control connection became readable";
        const ClientEnds& client = clients[static_cast<std::size_t>(who)];
        std::vector<std::uint8_t> frame;
        if (SocketTransport::ReceiveOneFrame(client.control, 2000, 4096, &frame) != MOBILEGL_OK ||
            std::string(frame.begin(), frame.end()) != down)
            return ::testing::AssertionFailure() << "client " << client.name << "'s control connection read something else";
        const std::string up = std::string("control-to-server:") + client.name;
        if (!SendFramed(client.control, up.data(), up.size()))
            return ::testing::AssertionFailure() << "client " << client.name << " could not write its control connection";
        std::vector<std::uint8_t> buffer(4096);
        std::uint64_t size = 0;
        if (server.ReceiveFrame({buffer.data(), buffer.size()}, &size, 2000) != MOBILEGL_OK ||
            std::string(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(size)) != up)
            return ::testing::AssertionFailure() << "the server's control connection did not carry client " << client.name
                                                 << "'s frame";
        const std::string side = std::string("aux:") + client.name;
        if (::send(client.aux, side.data(), side.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(side.size()))
            return ::testing::AssertionFailure() << "client " << client.name << " could not write its aux connection";
        std::string received(side.size(), '\0');
        if (!ReadExactly(server.AuxFd(), reinterpret_cast<std::uint8_t*>(received.data()), received.size(), 2000) ||
            received != side)
            return ::testing::AssertionFailure() << "the server's aux connection is not client " << client.name
                                                 << "'s (read \"" << received << "\")";
        *outWho = who;
        return ::testing::AssertionSuccess();
    }

    class PairAcceptorTest : public ::testing::TestWithParam<Kind> {
    protected:
        void SetUp() override {
            endpoint = MakeEndpoint(GetParam());
            ASSERT_EQ(SocketTransport::Listen(endpoint, &listener), MOBILEGL_OK) << endpoint;
        }
        void TearDown() override {
            if (listener >= 0) ::close(listener);
        }

        std::string endpoint;
        int listener = -1;
    };

} // namespace

// (1) B0-CROSS-APP.md F2 as the device showed it: the server app's readiness probe - one connect()
// and close() - lands right before a real client. The probe is dropped and the client is the pair.
TEST_P(PairAcceptorTest, AStrayConnectAndCloseRightBeforeAClientDoesNotTakeItsPlace) {
    const int stray = RawConnect(endpoint);
    ASSERT_GE(stray, 0);
    ::close(stray);
    std::unique_ptr<SocketTransport> client;
    ASSERT_EQ(SocketTransport::ConnectTo(endpoint, 2000, client), MOBILEGL_OK);

    Server::PairAcceptor pairs(listener);
    std::unique_ptr<SocketTransport> server;
    ASSERT_EQ(pairs.Accept(3000, server), MOBILEGL_OK);
    int who = -1;
    EXPECT_TRUE(ServesExactly(*server, {{client->StreamFd(), client->AuxFd(), "client"}}, &who));
    EXPECT_EQ(pairs.Unpaired(), 0u) << "the probe is gone, not held";
}

// (2) Two clients whose four connections interleave - A-control, B-control, B-aux, A-aux. Each
// server transport is one client's control AND aux; B comes out first (its aux arrived first) and
// A-control, the oldest of the four, is still held for the call that hands out A.
TEST_P(PairAcceptorTest, TwoClientsWhoseFourConnectionsInterleaveArePairedEachWithItsOwn) {
    const Nonce a = FreshNonce(), b = FreshNonce();
    const int aControl = Half(endpoint, a, false);
    const int bControl = Half(endpoint, b, false);
    const int bAux = Half(endpoint, b, true);
    const int aAux = Half(endpoint, a, true);
    ASSERT_GE(aControl, 0);
    ASSERT_GE(bControl, 0);
    ASSERT_GE(bAux, 0);
    ASSERT_GE(aAux, 0);
    const std::vector<ClientEnds> clients = {{aControl, aAux, "A"}, {bControl, bAux, "B"}};

    Server::PairAcceptor pairs(listener);
    std::unique_ptr<SocketTransport> first, second;
    ASSERT_EQ(pairs.Accept(3000, first), MOBILEGL_OK);
    ASSERT_EQ(pairs.Accept(3000, second), MOBILEGL_OK);
    int firstWho = -1, secondWho = -1;
    EXPECT_TRUE(ServesExactly(*first, clients, &firstWho));
    EXPECT_TRUE(ServesExactly(*second, clients, &secondWho));
    EXPECT_NE(firstWho, secondWho) << "one client was handed out twice";
    EXPECT_EQ(pairs.Unpaired(), 0u);
    for (int fd : {aControl, bControl, bAux, aAux}) ::close(fd);
}

// (3) An aux connection whose control connection never arrives: held within the budget, dropped
// after it (its client sees the connection end), and the next client pairs as if it had never been.
TEST_P(PairAcceptorTest, AnAuxWhoseControlNeverArrivesIsDroppedAfterTheBudgetAndTheNextClientPairs) {
    constexpr std::uint32_t kBudgetMs = 300;
    Server::PairAcceptor pairs(listener, kBudgetMs);
    const int orphan = Half(endpoint, FreshNonce(), true);
    ASSERT_GE(orphan, 0);
    std::unique_ptr<SocketTransport> server;
    EXPECT_EQ(pairs.Accept(100, server), MOBILEGL_ERR_TIMEOUT);
    EXPECT_EQ(pairs.Unpaired(), 1u) << "inside its budget the aux connection is held";
    EXPECT_EQ(pairs.Accept(kBudgetMs + 300, server), MOBILEGL_ERR_TIMEOUT);
    EXPECT_EQ(pairs.Unpaired(), 0u) << "past its budget the aux connection is dropped";
    pollfd pfd{orphan, POLLIN, 0};
    ASSERT_EQ(::poll(&pfd, 1, 1000), 1) << "the dropped connection never ended";
    char byte = 0;
    EXPECT_EQ(::recv(orphan, &byte, 1, 0), 0) << "an aux connection is closed, never written to";
    ::close(orphan);

    std::unique_ptr<SocketTransport> client;
    ASSERT_EQ(SocketTransport::ConnectTo(endpoint, 2000, client), MOBILEGL_OK);
    ASSERT_EQ(pairs.Accept(3000, server), MOBILEGL_OK);
    int who = -1;
    EXPECT_TRUE(ServesExactly(*server, {{client->StreamFd(), client->AuxFd(), "next"}}, &who));
}

// A client from before control revision 4 writes its Hello first and presents no PairBind. It is
// refused by name and never paired: Refuse{WireFingerprint} for an older wire, as any session would
// answer it, and Refuse{MalformedHello} for a Hello on this wire that still cannot be paired.
TEST_P(PairAcceptorTest, AClientWhoseFirstFrameIsAHelloIsRefusedByNameAndNeverPaired) {
    // The token is asked first (Handshake.h); this case is about the wire, so none is configured.
    const ScopedUnset noToken("MOBILEGL_IPC_TOKEN");
    const ScopedUnset noSameBuild("MOBILEGL_IPC_REQUIRE_SAME_BUILD");
    Server::PairAcceptor pairs(listener);
    for (const bool currentWire : {false, true}) {
        SCOPED_TRACE(currentWire ? "current wire" : "older wire");
        const int control = RawConnect(endpoint);
        const int aux = RawConnect(endpoint);
        ASSERT_GE(control, 0);
        ASSERT_GE(aux, 0);
        ::flatbuffers::FlatBufferBuilder builder(512);
        const std::uint64_t fingerprint = currentWire ? WireFingerprint() : WireFingerprint() ^ 0x1ull;
        auto terms = ::MobileGL::Wire::CreateLinkTerms(builder);
        auto hello = ::MobileGL::Wire::CreateHelloDirect(builder, MOBILEGL_PROTOCOL_ABI_MAJOR, MOBILEGL_PROTOCOL_ABI_MINOR,
                                                         "an-older-client", 0, 1, nullptr, fingerprint, fingerprint, terms,
                                                         nullptr, ::MobileGL::Wire::DialMode::Connect);
        ::MobileGL::Wire::FinishCtrlEnvelopeBuffer(
            builder, ::MobileGL::Wire::CreateCtrlEnvelope(builder, ::MobileGL::Wire::CtrlMsg::Hello, hello.Union()));
        ASSERT_TRUE(SendFramed(control, builder.GetBufferPointer(), builder.GetSize()));

        std::unique_ptr<SocketTransport> server;
        EXPECT_EQ(pairs.Accept(300, server), MOBILEGL_ERR_TIMEOUT) << "a Hello-first client was paired";
        std::vector<std::uint8_t> frame;
        ASSERT_EQ(SocketTransport::ReceiveOneFrame(control, 2000, 4096, &frame), MOBILEGL_OK) << "no refusal came back";
        ::flatbuffers::Verifier verifier(frame.data(), frame.size());
        ASSERT_TRUE(::MobileGL::Wire::VerifyCtrlEnvelopeBuffer(verifier));
        const auto* refusal = ::MobileGL::Wire::GetCtrlEnvelope(frame.data())->msg_as_Refuse();
        ASSERT_NE(refusal, nullptr) << "the answer is not a Refuse";
        EXPECT_EQ(refusal->code(), currentWire ? ::MobileGL::Wire::RefuseCode::MalformedHello
                                               : ::MobileGL::Wire::RefuseCode::WireFingerprint);
        ::close(control);
        ::close(aux);
        EXPECT_EQ(pairs.Accept(100, server), MOBILEGL_ERR_TIMEOUT);
        EXPECT_EQ(pairs.Unpaired(), 0u) << "the client's silent aux connection left with it";
    }
}

INSTANTIATE_TEST_SUITE_P(Endpoints, PairAcceptorTest, ::testing::Values(Kind::Unix, Kind::Tcp),
                         [](const ::testing::TestParamInfo<Kind>& info) {
                             return info.param == Kind::Unix ? std::string("unix") : std::string("tcp");
                         });

#endif // !_WIN32
