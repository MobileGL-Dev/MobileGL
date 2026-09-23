// MobileGL - MobileGL/MG_Test/Wire/SocketTransportTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The SAME nine ITransport-contract cases InProcessTransportTest runs, driven
// over a real socketpair() with real threads. Package `so`.
//
// THIS FILE IS THE PACKAGE'S WHOLE POINT. CONTRACT-P6.md §8 and the spawn plan
// both require SocketTransport green over a plain socketpair BEFORE a second
// process exists, for the reason SessionRings.h gives about the attach half:
// a transport first exercised on the day the child appears is a transport
// whose bugs all arrive at once, on the hardest day to debug them.
//
// WHY THE BODIES ARE COPIED RATHER THAN SHARED. The two suites assert the same
// contract but not through the same handle: InProcessTransport hands out
// condvar doorbells its tests reach into, SocketTransport does not have them
// (the spawn design pairs it with SocketDoorbell, which FdPassingTest already
// covers). Folding both into one parameterised fixture would rename the
// sixteen existing cases, and G14 says test names only ever grow. Nine new
// names, zero removed.
//
// The seven Doorbell* cases in InProcessTransportTest are deliberately NOT
// here: they exercise CondVarDoorbell through a transport that happens to own
// one, not ITransport. SocketDoorbell's equivalents live in FdPassingTest.
//
// WHAT A SOCKET MAKES REAL THAT A MESSAGE QUEUE DID NOT. InProcessTransport
// hands the peer whole messages by construction; a stream hands you arbitrary
// fragments, so every one of these cases now also exercises Framing.h's
// reassembler - which is why `PreservesOrder` sends 64 messages without
// reading, forcing coalescing in the socket buffer, and why
// `BufferTooSmallKeepsTheMessage` matters more here: the message it must keep
// is one this side already reassembled out of bytes nobody else can re-deliver.

#include <MG_Remote/Handshake.h> // PH-7 (4): EncodeDataBind / DecodeDataBind
#include <MG_Remote/Protocol/generated/protocol_generated.h> // RefuseCode, for the word below
#include <MG_Remote/Transport/AuthToken.h> // PH-7 (3): the minimum and the comparison
#include <MG_Remote/Transport/Doorbell.h> // kWaitForever
#include <MG_Remote/Transport/FdPassing.h>
#include <MG_Remote/Transport/SocketTransport.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#endif

using namespace MobileGL::MG_Remote::Transport;

#if !defined(_WIN32)

TEST(SocketTransportTest, TcpCarriesControlAndIndependentDataWithKeepalive) {
    // Find an available loopback port; Listen deliberately refuses ambiguous port 0 URIs.
    int reservation = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(reservation, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(::bind(reservation, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    socklen_t addressSize = sizeof(address);
    ASSERT_EQ(::getsockname(reservation, reinterpret_cast<sockaddr*>(&address), &addressSize), 0);
    const auto endpoint = std::string("tcp://127.0.0.1:") + std::to_string(ntohs(address.sin_port));
    ::close(reservation);
    int listener = -1;
    ASSERT_EQ(SocketTransport::Listen(endpoint, &listener), MOBILEGL_OK);
    std::unique_ptr<SocketTransport> client, server;
    ASSERT_EQ(SocketTransport::ConnectTo(endpoint, 1000, client), MOBILEGL_OK);
    ASSERT_EQ(SocketTransport::AcceptPair(listener, 1000, server), MOBILEGL_OK);
    ::close(listener);
    ASSERT_TRUE(client->IsTcp());
    ASSERT_TRUE(server->IsTcp());
    for (auto* transport : {client.get(), server.get()}) {
        int option = 0;
        socklen_t size = sizeof(option);
        ASSERT_EQ(::getsockopt(transport->StreamFd(), IPPROTO_TCP, TCP_NODELAY, &option, &size), 0);
        EXPECT_EQ(option, 1);
        ASSERT_EQ(::getsockopt(transport->StreamFd(), SOL_SOCKET, SO_KEEPALIVE, &option, &size), 0);
        EXPECT_EQ(option, 1);
        EXPECT_EQ(transport->ShareFd(0, {nullptr, 0}), MOBILEGL_ERR_UNSUPPORTED);
    }
    const int clientData = client->TakeDataFd(), serverData = server->TakeDataFd();
    ASSERT_GE(clientData, 0);
    ASSERT_GE(serverData, 0);
    EXPECT_EQ(client->TakeDataFd(), -1);
    const char message[] = "independent";
    ASSERT_EQ(::send(clientData, message, sizeof(message), MSG_NOSIGNAL), sizeof(message));
    char received[sizeof(message)]{};
    ASSERT_EQ(::recv(serverData, received, sizeof(received), MSG_WAITALL), sizeof(received));
    EXPECT_EQ(std::memcmp(message, received, sizeof(message)), 0);
    ASSERT_EQ(server->SendFrame({message, sizeof(message)}), MOBILEGL_OK);
    std::uint64_t size = 0;
    ASSERT_EQ(client->ReceiveFrame({received, sizeof(received)}, &size, 1000), MOBILEGL_OK);
    EXPECT_EQ(size, sizeof(message));
    ::close(clientData);
    ::close(serverData);
}

TEST(SocketTransportTest, TcpRejectsMalformedNamesAndUnauthenticatedWildcard) {
    int listener = -1;
    for (const char* endpoint : {"tcp://", "tcp://localhost", "tcp://localhost:0", "tcp://localhost:65536",
                                 "tcp://localhost:-1", "tcp://[::1]broken", "tcp://::1:40000"}) {
        EXPECT_EQ(SocketTransport::Listen(endpoint, &listener), MOBILEGL_ERR_INVALID_ARGUMENT) << endpoint;
        EXPECT_EQ(listener, -1);
    }
    const char* prior = std::getenv("MOBILEGL_IPC_TOKEN");
    const std::string saved = prior ? prior : "";
    const bool wasSet = prior != nullptr;
    ::unsetenv("MOBILEGL_IPC_TOKEN");
    EXPECT_EQ(SocketTransport::Listen("tcp://0.0.0.0:40613", &listener), MOBILEGL_ERR_PROTOCOL_MISMATCH);
    EXPECT_EQ(listener, -1);
    // PH-7 (2). The refusal that path logs is `Refuse{Authentication}`, and `Authentication` has
    // to be RefuseCode's own spelling: for a whole phase it read `Refuse{AuthenticationRequired}`,
    // a word the wire enum has never had, so anyone grepping the refusal vocabulary found a name
    // no peer could ever receive. fatal_census.py's rule 4 refuses an invented word statically;
    // this pins the word that gate checks against to the enumerator the peer actually reads, so
    // renaming the enumerator without the log line fails here rather than silently.
    EXPECT_STREQ(MobileGL::Wire::EnumNameRefuseCode(MobileGL::Wire::RefuseCode::Authentication),
                 "Authentication");
    if (wasSet) ::setenv("MOBILEGL_IPC_TOKEN", saved.c_str(), 1);
}

// PH-7 (3), ID-P7-3. A CONFIGURED TOKEN IS EITHER A CREDENTIAL OR A REFUSAL.
//
// The listen policy used to ask only "non-empty", so a one-byte MOBILEGL_IPC_TOKEN opened a
// wildcard listen. The end-to-end half of this - the supervisor PROCESS dying with a named
// reason rather than degrading to a loopback server that looks like success - is
// TcpLane.SupervisorProtocolControls; this is the cheap half, and it also pins that the refusal
// applies on LOOPBACK, where a short token would otherwise be harmless. Silently accepting it
// there is what would let a short token reach a deployment.
TEST(SocketTransportTest, TcpRefusesToListenWithATokenShorterThanTheMinimum) {
    const char* prior = std::getenv("MOBILEGL_IPC_TOKEN");
    const std::string saved = prior ? prior : "";
    const bool wasSet = prior != nullptr;
    const std::string tooShort(kMinimumAuthTokenBytes - 1, 'x');
    ::setenv("MOBILEGL_IPC_TOKEN", tooShort.c_str(), 1);
    int listener = -1;
    EXPECT_EQ(SocketTransport::Listen("tcp://127.0.0.1:40697", &listener),
              MOBILEGL_ERR_PROTOCOL_MISMATCH);
    EXPECT_EQ(listener, -1);
    // One more byte and the same endpoint comes up, so the refusal above is the LENGTH and not
    // the port, the address family or the environment variable's mere presence.
    const std::string longEnough(kMinimumAuthTokenBytes, 'x');
    ::setenv("MOBILEGL_IPC_TOKEN", longEnough.c_str(), 1);
    EXPECT_EQ(SocketTransport::Listen("tcp://127.0.0.1:40697", &listener), MOBILEGL_OK);
    EXPECT_GE(listener, 0);
    if (listener >= 0) ::close(listener);
    if (wasSet) ::setenv("MOBILEGL_IPC_TOKEN", saved.c_str(), 1);
    else ::unsetenv("MOBILEGL_IPC_TOKEN");
}

// The comparison itself. `std::strcmp` and `std::string::operator==` - the two this replaced -
// both stop at the first differing byte; these cases pin the answers, and the loop in
// AuthToken.h is what makes the time not depend on WHERE the difference is.
TEST(SocketTransportTest, TheTokenComparisonAnswersLengthAndPrefixTheSameWay) {
    const char* expected = "0123456789abcdef";
    EXPECT_TRUE(ConstantTimeTokenMatch(expected, "0123456789abcdef", 16));
    EXPECT_FALSE(ConstantTimeTokenMatch(expected, "0123456789abcdeF", 16));
    EXPECT_FALSE(ConstantTimeTokenMatch(expected, "Z123456789abcdef", 16));
    // A correct PREFIX is not a match, and neither is a correct token with anything after it.
    EXPECT_FALSE(ConstantTimeTokenMatch(expected, "0123456789abcde", 15));
    EXPECT_FALSE(ConstantTimeTokenMatch(expected, "0123456789abcdefg", 17));
    EXPECT_FALSE(ConstantTimeTokenMatch(expected, "", 0));
    // An embedded NUL is compared, not truncated at: `presented` carries its own length because
    // a flatbuffers string can hold one and c_str()-plus-strcmp would have stopped there.
    const char embedded[] = {'0', '\0', '2', '3', '4', '5', '6', '7',
                             '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    EXPECT_FALSE(ConstantTimeTokenMatch(expected, embedded, sizeof(embedded)));
    EXPECT_FALSE(ConstantTimeTokenMatch(nullptr, "anything", 8));
}

// PH-7 (4), ID-P7-3. THE DATA CONNECTION'S FIRST FRAME IS READ EXACTLY, AND ONLY A DataBind
// OF THE MINTED WIDTH IS ONE.
//
// A client writes its DataBind and then, on the same socket, StreamLink's own frames - possibly
// in the same segment. The server hands that descriptor to a StreamLink reader after reading
// the DataBind, so a reader that buffered ahead would eat the first command bytes and the
// session would die of "invalid data frame header" one frame later. ReceiveOneFrame reads the
// header and then exactly the payload; the bytes behind it are still on the socket afterwards.
// End to end, over a real loopback TCP listener, through the two helpers ServerMain and the
// client use. The two-process half (reordered arrival, stale nonce, refused by name) is
// TcpLane.SupervisorProtocolControls.
TEST(SocketTransportTest, TheDataBindIsReadExactlyAndTheStreamBehindItIsLeftOnTheSocket) {
    int reservation = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(reservation, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(::bind(reservation, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    socklen_t addressSize = sizeof(address);
    ASSERT_EQ(::getsockname(reservation, reinterpret_cast<sockaddr*>(&address), &addressSize), 0);
    const auto endpoint = std::string("tcp://127.0.0.1:") + std::to_string(ntohs(address.sin_port));
    ::close(reservation);
    int listener = -1;
    ASSERT_EQ(SocketTransport::Listen(endpoint, &listener), MOBILEGL_OK);

    std::uint8_t nonce[kDataNonceBytes];
    ASSERT_EQ(SocketTransport::MintNonce(nonce, sizeof(nonce)), MOBILEGL_OK);
    const auto bind = MobileGL::MG_Remote::EncodeDataBind(nonce, sizeof(nonce));
    int client = -1;
    ASSERT_EQ(SocketTransport::ConnectDataConnection(endpoint, 1000, MobileGLByteSpan{bind.data(), bind.size()},
                                                     &client), MOBILEGL_OK);
    const char tail[] = "MGLD-the-first-stream-bytes";
    ASSERT_EQ(::send(client, tail, sizeof(tail), MSG_NOSIGNAL), static_cast<ssize_t>(sizeof(tail)));

    int accepted = -1;
    ASSERT_EQ(SocketTransport::AcceptOne(listener, 1000, &accepted), MOBILEGL_OK);
    std::vector<std::uint8_t> frame;
    ASSERT_EQ(SocketTransport::ReceiveOneFrame(accepted, 1000, 1024, &frame), MOBILEGL_OK);
    EXPECT_EQ(frame, bind);
    std::uint8_t presented[kDataNonceBytes] = {};
    ASSERT_TRUE(MobileGL::MG_Remote::DecodeDataBind(frame, presented));
    EXPECT_TRUE(ConstantTimeNonceMatch(nonce, presented));
    char after[sizeof(tail)] = {};
    EXPECT_EQ(::recv(accepted, after, sizeof(after), MSG_WAITALL), static_cast<ssize_t>(sizeof(tail)));
    EXPECT_EQ(std::memcmp(after, tail, sizeof(tail)), 0);

    // Only a DataBind of the minted width is one: a shorter nonce, and a frame that is not a
    // DataBind at all, both decode as "no", which the callers refuse by name.
    const auto shortBind = MobileGL::MG_Remote::EncodeDataBind(nonce, sizeof(nonce) - 1);
    EXPECT_FALSE(MobileGL::MG_Remote::DecodeDataBind(shortBind, presented));
    std::vector<std::uint8_t> notABind(bind.size(), 0xA5);
    EXPECT_FALSE(MobileGL::MG_Remote::DecodeDataBind(notABind, presented));
    // One byte of difference anywhere is a mismatch.
    presented[kDataNonceBytes - 1] ^= 1;
    EXPECT_FALSE(ConstantTimeNonceMatch(nonce, presented));

    // A frame over the caller's bound is refused before its payload is read.
    int second = -1;
    ASSERT_EQ(SocketTransport::ConnectDataConnection(endpoint, 1000, MobileGLByteSpan{bind.data(), bind.size()},
                                                     &second), MOBILEGL_OK);
    int big = -1;
    ASSERT_EQ(SocketTransport::AcceptOne(listener, 1000, &big), MOBILEGL_OK);
    EXPECT_EQ(SocketTransport::ReceiveOneFrame(big, 1000, bind.size() - 1, &frame), MOBILEGL_ERR_PROTOCOL_MISMATCH);
    // And a connection that says nothing times out rather than blocking.
    EXPECT_EQ(SocketTransport::AcceptOne(listener, 50, &big), MOBILEGL_ERR_TIMEOUT);
    for (const int fd : {client, accepted, second, big, listener})
        if (fd >= 0) ::close(fd);
}

namespace {

    MobileGLByteSpan Span(const std::string& text) {
        return MobileGLByteSpan{text.data(), text.size()};
    }

    std::string Receive(ITransport& transport, std::uint32_t timeoutMs = 1000) {
        std::vector<std::uint8_t> buffer(4096);
        std::uint64_t size = 0;
        MobileGLMutableByteSpan span{buffer.data(), buffer.size()};
        const MobileGLResult result = transport.ReceiveFrame(span, &size, timeoutMs);
        if (result != MOBILEGL_OK) {
            return std::string("<result=") + std::to_string(static_cast<int>(result)) + ">";
        }
        return std::string(reinterpret_cast<const char*>(buffer.data()),
                           static_cast<std::size_t>(size));
    }

    struct Pair {
        std::unique_ptr<SocketTransport> client;
        std::unique_ptr<SocketTransport> server;
    };

    Pair MakePair() {
        Pair pair;
        const MobileGLResult result = SocketTransport::CreatePair(pair.client, pair.server);
        EXPECT_EQ(result, MOBILEGL_OK);
        return pair;
    }

} // namespace

TEST(SocketTransportTest, CarriesFramesInBothDirections) {
    Pair pair = MakePair();
    ASSERT_TRUE(pair.client && pair.server);
    EXPECT_EQ(pair.client->Role(), TransportRole::Client);
    EXPECT_EQ(pair.server->Role(), TransportRole::Server);

    const std::string hello = "Hello{abiMajor=1}";
    const std::string welcome = "Welcome{serverPid=42}";
    ASSERT_EQ(pair.client->SendFrame(Span(hello)), MOBILEGL_OK);
    // PeekFrameSize pumps, so it is allowed to need the bytes to have arrived;
    // on a socketpair a completed send is already in the peer's buffer.
    EXPECT_EQ(pair.server->PeekFrameSize(), hello.size());
    // A message goes to the PEER, never back to the sender.
    EXPECT_EQ(pair.client->PeekFrameSize(), 0u);
    EXPECT_EQ(Receive(*pair.server), hello);

    ASSERT_EQ(pair.server->SendFrame(Span(welcome)), MOBILEGL_OK);
    EXPECT_EQ(Receive(*pair.client), welcome);
}

TEST(SocketTransportTest, RequestHalfCloseStillDeliversTheFinalReply) {
    std::unique_ptr<SocketTransport> client, server;
    ASSERT_EQ(SocketTransport::CreatePair(client, server), MOBILEGL_OK);
    ASSERT_EQ(client->SendFrame(Span("last request")), MOBILEGL_OK);
    EXPECT_EQ(Receive(*server), "last request");
    ASSERT_EQ(client->ShutdownSend(), MOBILEGL_OK);
    std::uint64_t bytes = 0;
    char buffer[32]{};
    EXPECT_EQ(server->ReceiveFrame({buffer, sizeof buffer}, &bytes, 1000), MOBILEGL_ERR_TRANSPORT_CLOSED);
    ASSERT_EQ(server->SendFrame(Span("cleanup complete")), MOBILEGL_OK);
    EXPECT_EQ(Receive(*client), "cleanup complete");
    server->Shutdown();
    EXPECT_EQ(client->ReceiveFrame({buffer, sizeof buffer}, &bytes, 1000), MOBILEGL_ERR_TRANSPORT_CLOSED);
}

TEST(SocketTransportTest, PreservesOrder) {
    Pair pair = MakePair();

    // 64 unread messages coalesce in the socket buffer, so the reassembler has
    // to find every boundary itself. That is the case InProcessTransport could
    // not have: its queue kept the boundaries for it.
    for (int i = 0; i < 64; ++i) {
        const std::string message = "msg-" + std::to_string(i);
        ASSERT_EQ(pair.client->SendFrame(Span(message)), MOBILEGL_OK);
    }
    for (int i = 0; i < 64; ++i) {
        EXPECT_EQ(Receive(*pair.server), "msg-" + std::to_string(i));
    }
}

TEST(SocketTransportTest, BufferTooSmallKeepsTheMessage) {
    Pair pair = MakePair();

    const std::string message(300, 'x');
    ASSERT_EQ(pair.client->SendFrame(Span(message)), MOBILEGL_OK);

    std::vector<std::uint8_t> small(16);
    std::uint64_t required = 0;
    MobileGLMutableByteSpan smallSpan{small.data(), small.size()};
    // Zero timeout: the bytes may still be in flight, so give the reassembler a
    // real chance first. PeekFrameSize is the documented way to do that.
    while (pair.server->PeekFrameSize() == 0) {
        std::this_thread::yield();
    }
    EXPECT_EQ(pair.server->ReceiveFrame(smallSpan, &required, 0), MOBILEGL_ERR_BUFFER_TOO_SMALL);
    EXPECT_EQ(required, message.size());
    // Still there. On a stream this is the load-bearing half: the bytes have
    // already been consumed from the socket, so if the reassembler dropped the
    // message nothing could re-deliver it and the stream would be wedged.
    EXPECT_EQ(pair.server->PeekFrameSize(), message.size());
    EXPECT_EQ(Receive(*pair.server), message);
}

TEST(SocketTransportTest, PollAndTimeoutDoNotBlockForever) {
    Pair pair = MakePair();

    std::vector<std::uint8_t> buffer(64);
    std::uint64_t size = 0;
    MobileGLMutableByteSpan span{buffer.data(), buffer.size()};
    EXPECT_EQ(pair.server->ReceiveFrame(span, &size, 0), MOBILEGL_ERR_TIMEOUT);

    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(pair.server->ReceiveFrame(span, &size, 30), MOBILEGL_ERR_TIMEOUT);
    EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - start)
                  .count(),
              20);
}

TEST(SocketTransportTest, ShutdownDrainsBeforeItCloses) {
    Pair pair = MakePair();

    const std::string last = "Fatal{code=DeviceLost}";
    ASSERT_EQ(pair.client->SendFrame(Span(last)), MOBILEGL_OK);
    pair.client->Shutdown();

    // A peer that shuts down right after sending must not lose its last
    // message - that is usually the one that says why it is going away. On a
    // socket the bytes are already in the peer's receive buffer when close()
    // runs, and the peer must reassemble them before it reports EOF.
    EXPECT_EQ(Receive(*pair.server), last);

    std::vector<std::uint8_t> buffer(64);
    std::uint64_t size = 0;
    MobileGLMutableByteSpan span{buffer.data(), buffer.size()};
    EXPECT_EQ(pair.server->ReceiveFrame(span, &size, 100), MOBILEGL_ERR_TRANSPORT_CLOSED);
    EXPECT_EQ(pair.server->SendFrame(Span(last)), MOBILEGL_ERR_TRANSPORT_CLOSED);
}

TEST(SocketTransportTest, BlockedReceiverWakesOnSendAndOnShutdown) {
    Pair pair = MakePair();

    std::atomic<bool> got{false};
    std::thread reader([&] { got.store(Receive(*pair.server, kWaitForever) == "wake"); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ASSERT_EQ(pair.client->SendFrame(Span(std::string("wake"))), MOBILEGL_OK);
    reader.join();
    EXPECT_TRUE(got.load());

    std::thread closer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        pair.client->Shutdown();
    });
    std::vector<std::uint8_t> buffer(64);
    std::uint64_t size = 0;
    MobileGLMutableByteSpan span{buffer.data(), buffer.size()};
    // The wake is a real POLLHUP here, not a condvar notify: this is the
    // mechanism Doorbell.h:390-398 chose a stream socket for, and the reason
    // death is an event rather than a hang.
    EXPECT_EQ(pair.server->ReceiveFrame(span, &size, kWaitForever), MOBILEGL_ERR_TRANSPORT_CLOSED);
    closer.join();
}

TEST(SocketTransportTest, RefusesAPayloadOverTheFrameCap) {
    Pair pair = MakePair();

    // Not allocated: the cap is checked before the bytes are touched, and it is
    // the SAME cap InProcessTransport enforces, so nothing passes CI on inproc
    // and then fails after the switch to spawn.
    const std::uint8_t dummy = 0;
    MobileGLByteSpan huge{&dummy, 64ull * 1024 * 1024 + 1};
    EXPECT_EQ(pair.client->SendFrame(huge), MOBILEGL_ERR_INVALID_ARGUMENT);
}

TEST(SocketTransportTest, HandsOverADescriptorAndItsSideband) {
    Pair pair = MakePair();

    int pipeFds[2] = {-1, -1};
    ASSERT_EQ(::pipe(pipeFds), 0);

    const std::string sideband = "SegmentRef{id=1,kind=Cmd}";
    ASSERT_EQ(pair.client->ShareFd(pipeFds[0], Span(sideband)), MOBILEGL_OK);

    // A short sideband buffer is refused before anything is consumed, so the
    // descriptor is never dropped. This is the real SCM_RIGHTS path, not an
    // in-process hand-off: the fd the peer gets is renumbered by the kernel.
    std::vector<std::uint8_t> small(8);
    int fd = -1;
    std::uint64_t required = 0;
    MobileGLMutableByteSpan smallSpan{small.data(), small.size()};
    EXPECT_EQ(pair.server->ReceiveFd(&fd, smallSpan, &required, 0), MOBILEGL_ERR_BUFFER_TOO_SMALL);
    EXPECT_EQ(required, FdPassing::kMaxSidebandBytes);
    EXPECT_EQ(fd, -1);

    std::vector<std::uint8_t> big(FdPassing::kMaxSidebandBytes);
    std::uint64_t sidebandSize = 0;
    MobileGLMutableByteSpan bigSpan{big.data(), big.size()};
    ASSERT_EQ(pair.server->ReceiveFd(&fd, bigSpan, &sidebandSize, 100), MOBILEGL_OK);
    ASSERT_GE(fd, 0);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(big.data()),
                          static_cast<std::size_t>(sidebandSize)),
              sideband);

    // Same open file description, independent descriptor - which is exactly
    // what the four segments will ride on in `sm`.
    const char payload[] = "bytes";
    ASSERT_EQ(::write(pipeFds[1], payload, sizeof(payload)), static_cast<ssize_t>(sizeof(payload)));
    char readBack[sizeof(payload)] = {};
    ASSERT_EQ(::read(fd, readBack, sizeof(readBack)), static_cast<ssize_t>(sizeof(payload)));
    EXPECT_STREQ(readBack, payload);

    ::close(fd);
    ::close(pipeFds[0]);
    ::close(pipeFds[1]);
}

TEST(SocketTransportTest, AFrameWakeupIsNotEatenByAWaiterOnDescriptors) {
    Pair pair = MakePair();

    // Two readers on the SAME endpoint, blocked on two different predicates.
    // The socket transport keeps them on two different descriptors - the stream
    // and the aux - so neither can eat the other's wakeup by construction. That
    // is a stronger property than the inproc transport's, and asserting it here
    // is what would catch a future version that folded them onto one fd.
    std::atomic<bool> fdWaiterStarted{false};
    std::thread fdWaiter([&] {
        std::vector<std::uint8_t> sideband(FdPassing::kMaxSidebandBytes);
        MobileGLMutableByteSpan span{sideband.data(), sideband.size()};
        int fd = -1;
        std::uint64_t size = 0;
        fdWaiterStarted.store(true);
        // Never offered a descriptor: this one ends on its own deadline. NOT on
        // Shutdown, the way the inproc suite's does - closing an fd out from
        // under a thread already blocked in poll() does not reliably wake it,
        // and a test that depended on that would be asserting a platform
        // accident. The deadline is short because all this waiter has to do is
        // BE BLOCKED while the frame below arrives.
        const MobileGLResult result = pair.client->ReceiveFd(&fd, span, &size, 600);
        EXPECT_NE(result, MOBILEGL_OK);
        EXPECT_EQ(fd, -1);
    });
    while (!fdWaiterStarted.load()) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::atomic<bool> frameWaiterStarted{false};
    std::string got;
    std::thread frameWaiter([&] {
        frameWaiterStarted.store(true);
        got = Receive(*pair.client, 4000);
    });
    while (!frameWaiterStarted.load()) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const std::string message = "wake the right waiter";
    const auto start = std::chrono::steady_clock::now();
    ASSERT_EQ(pair.server->SendFrame(Span(message)), MOBILEGL_OK);
    frameWaiter.join();
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - start)
                               .count();

    EXPECT_EQ(got, message);
    // Not "eventually, when the receive timed out and re-checked".
    EXPECT_LT(elapsedMs, 2000);

    fdWaiter.join();
}

// ---- cases a socket has and a message queue does not ------------------------

TEST(SocketTransportTest, ReassemblesAMessageSplitAcrossReads) {
    Pair pair = MakePair();

    // 256 KiB is comfortably past a default socketpair buffer, so the sender
    // blocks and the payload provably crosses in several reads. If PumpOnce
    // returned a partial frame as a whole one, or if it dropped the tail, this
    // is where it shows.
    const std::string big(256 * 1024, 'q');
    std::thread sender([&] { EXPECT_EQ(pair.client->SendFrame(Span(big)), MOBILEGL_OK); });

    std::vector<std::uint8_t> buffer(big.size());
    std::uint64_t size = 0;
    MobileGLMutableByteSpan span{buffer.data(), buffer.size()};
    ASSERT_EQ(pair.server->ReceiveFrame(span, &size, kWaitForever), MOBILEGL_OK);
    sender.join();

    EXPECT_EQ(size, big.size());
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(buffer.data()),
                          static_cast<std::size_t>(size)),
              big);
}

TEST(SocketTransportTest, AGarbageStreamIsLatchedFailedAndNeverRecovers) {
    Pair pair = MakePair();

    // Write bytes that are not a frame directly onto the peer's stream fd,
    // bypassing SendFrame. A corrupt control stream must latch, by name, and
    // must not be retry-able past: the alternative is a reader that resyncs
    // onto a byte boundary it invented, which is how one bad frame becomes a
    // silently misparsed session.
    const std::uint8_t garbage[16] = {0xDE, 0xAD, 0xBE, 0xEF, 0, 0, 0, 0,
                                      0xFF, 0xFF, 0xFF, 0xFF, 1, 2, 3, 4};
    ASSERT_EQ(::write(pair.client->StreamFd(), garbage, sizeof(garbage)),
              static_cast<ssize_t>(sizeof(garbage)));

    std::vector<std::uint8_t> buffer(64);
    std::uint64_t size = 0;
    MobileGLMutableByteSpan span{buffer.data(), buffer.size()};
    EXPECT_EQ(pair.server->ReceiveFrame(span, &size, 200), MOBILEGL_ERR_PROTOCOL_MISMATCH);
    // Latched: a second call says the same thing rather than resyncing.
    EXPECT_EQ(pair.server->ReceiveFrame(span, &size, 200), MOBILEGL_ERR_PROTOCOL_MISMATCH);
    EXPECT_EQ(pair.server->PeekFrameSize(), 0u);
}

TEST(SocketTransportTest, AdoptsAnAlreadyConnectedFdAndRefusesFdPassingWithoutAnAux) {
    // What `sm` does on each side of the fork: adopt fd 3, and - for a link
    // that has no descriptor channel - answer UNSUPPORTED to ShareFd rather
    // than pretending. Rule G: a transport declares what it supports.
    int fds[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    SocketTransport a(fds[0], -1, TransportRole::Client);
    SocketTransport b(fds[1], -1, TransportRole::Server);

    const std::string message = "Hello{adopted}";
    ASSERT_EQ(a.SendFrame(Span(message)), MOBILEGL_OK);
    EXPECT_EQ(Receive(b), message);

    const std::uint8_t dummy = 0;
    EXPECT_EQ(a.ShareFd(1, MobileGLByteSpan{&dummy, 1}), MOBILEGL_ERR_UNSUPPORTED);
    int fd = -1;
    std::uint64_t size = 0;
    std::vector<std::uint8_t> sideband(FdPassing::kMaxSidebandBytes);
    MobileGLMutableByteSpan span{sideband.data(), sideband.size()};
    EXPECT_EQ(b.ReceiveFd(&fd, span, &size, 0), MOBILEGL_ERR_UNSUPPORTED);
    EXPECT_EQ(fd, -1);
}

#endif // !_WIN32
