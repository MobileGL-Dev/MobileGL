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
#include <unistd.h>
#endif

using namespace MobileGL::MG_Remote::Transport;

#if !defined(_WIN32)

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
