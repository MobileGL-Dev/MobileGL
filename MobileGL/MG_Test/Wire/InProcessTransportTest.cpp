// MobileGL - MobileGL/MG_Test/Wire/InProcessTransportTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The `inproc` transport: message queues in both directions, the
// buffer-too-small contract, shutdown semantics, descriptor hand-off, and the
// condvar doorbells the rings park on.

#include <MG_Remote/Transport/FdPassing.h>
#include <MG_Remote/Transport/InProcessTransport.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

using namespace MobileGL::MG_Remote::Transport;

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

} // namespace

TEST(InProcessTransportTest, CarriesFramesInBothDirections) {
    std::unique_ptr<InProcessTransport> client;
    std::unique_ptr<InProcessTransport> server;
    InProcessTransport::CreatePair(client, server);
    ASSERT_TRUE(client && server);
    EXPECT_EQ(client->Role(), TransportRole::InProcess);

    const std::string hello = "Hello{abiMajor=1}";
    const std::string welcome = "Welcome{serverPid=42}";
    ASSERT_EQ(client->SendFrame(Span(hello)), MOBILEGL_OK);
    EXPECT_EQ(server->PeekFrameSize(), hello.size());
    // A message goes to the PEER's inbox, never back to the sender.
    EXPECT_EQ(client->PeekFrameSize(), 0u);
    EXPECT_EQ(Receive(*server), hello);

    ASSERT_EQ(server->SendFrame(Span(welcome)), MOBILEGL_OK);
    EXPECT_EQ(Receive(*client), welcome);
}

TEST(InProcessTransportTest, PreservesOrder) {
    std::unique_ptr<InProcessTransport> client;
    std::unique_ptr<InProcessTransport> server;
    InProcessTransport::CreatePair(client, server);

    for (int i = 0; i < 64; ++i) {
        const std::string message = "msg-" + std::to_string(i);
        ASSERT_EQ(client->SendFrame(Span(message)), MOBILEGL_OK);
    }
    for (int i = 0; i < 64; ++i) {
        EXPECT_EQ(Receive(*server), "msg-" + std::to_string(i));
    }
}

TEST(InProcessTransportTest, BufferTooSmallKeepsTheMessage) {
    std::unique_ptr<InProcessTransport> client;
    std::unique_ptr<InProcessTransport> server;
    InProcessTransport::CreatePair(client, server);

    const std::string message(300, 'x');
    ASSERT_EQ(client->SendFrame(Span(message)), MOBILEGL_OK);

    std::vector<std::uint8_t> small(16);
    std::uint64_t required = 0;
    MobileGLMutableByteSpan smallSpan{small.data(), small.size()};
    EXPECT_EQ(server->ReceiveFrame(smallSpan, &required, 0), MOBILEGL_ERR_BUFFER_TOO_SMALL);
    EXPECT_EQ(required, message.size());
    // Still queued - the caller just retries with the size it was told.
    EXPECT_EQ(server->PeekFrameSize(), message.size());
    EXPECT_EQ(Receive(*server), message);
}

TEST(InProcessTransportTest, PollAndTimeoutDoNotBlockForever) {
    std::unique_ptr<InProcessTransport> client;
    std::unique_ptr<InProcessTransport> server;
    InProcessTransport::CreatePair(client, server);

    std::vector<std::uint8_t> buffer(64);
    std::uint64_t size = 0;
    MobileGLMutableByteSpan span{buffer.data(), buffer.size()};
    EXPECT_EQ(server->ReceiveFrame(span, &size, 0), MOBILEGL_ERR_TIMEOUT);

    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(server->ReceiveFrame(span, &size, 30), MOBILEGL_ERR_TIMEOUT);
    EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - start)
                  .count(),
              20);
}

TEST(InProcessTransportTest, ShutdownDrainsBeforeItCloses) {
    std::unique_ptr<InProcessTransport> client;
    std::unique_ptr<InProcessTransport> server;
    InProcessTransport::CreatePair(client, server);

    const std::string last = "Fatal{code=DeviceLost}";
    ASSERT_EQ(client->SendFrame(Span(last)), MOBILEGL_OK);
    client->Shutdown();

    // A peer that shuts down right after sending must not lose its last
    // message - that is usually the one that says why it is going away.
    EXPECT_EQ(Receive(*server), last);

    std::vector<std::uint8_t> buffer(64);
    std::uint64_t size = 0;
    MobileGLMutableByteSpan span{buffer.data(), buffer.size()};
    EXPECT_EQ(server->ReceiveFrame(span, &size, 100), MOBILEGL_ERR_TRANSPORT_CLOSED);
    EXPECT_EQ(server->SendFrame(Span(last)), MOBILEGL_ERR_TRANSPORT_CLOSED);
}

TEST(InProcessTransportTest, BlockedReceiverWakesOnSendAndOnShutdown) {
    std::unique_ptr<InProcessTransport> client;
    std::unique_ptr<InProcessTransport> server;
    InProcessTransport::CreatePair(client, server);

    std::atomic<bool> got{false};
    std::thread reader([&] {
        got.store(Receive(*server, kWaitForever) == "wake");
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ASSERT_EQ(client->SendFrame(Span(std::string("wake"))), MOBILEGL_OK);
    reader.join();
    EXPECT_TRUE(got.load());

    std::thread closer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        client->Shutdown();
    });
    std::vector<std::uint8_t> buffer(64);
    std::uint64_t size = 0;
    MobileGLMutableByteSpan span{buffer.data(), buffer.size()};
    EXPECT_EQ(server->ReceiveFrame(span, &size, kWaitForever), MOBILEGL_ERR_TRANSPORT_CLOSED);
    closer.join();
}

TEST(InProcessTransportTest, RefusesAPayloadOverTheFrameCap) {
    std::unique_ptr<InProcessTransport> client;
    std::unique_ptr<InProcessTransport> server;
    InProcessTransport::CreatePair(client, server);

    // Not allocated: the cap is checked before the bytes are touched. Keeping
    // the same limit as the socket transports means nothing passes CI here and
    // then fails after the switch to `spawn`.
    const std::uint8_t dummy = 0;
    MobileGLByteSpan huge{&dummy, 64ull * 1024 * 1024 + 1};
    EXPECT_EQ(client->SendFrame(huge), MOBILEGL_ERR_INVALID_ARGUMENT);
}

#if !defined(_WIN32)
TEST(InProcessTransportTest, HandsOverADescriptorAndItsSideband) {
    std::unique_ptr<InProcessTransport> client;
    std::unique_ptr<InProcessTransport> server;
    InProcessTransport::CreatePair(client, server);

    int pipeFds[2] = {-1, -1};
    ASSERT_EQ(::pipe(pipeFds), 0);

    const std::string sideband = "SegmentRef{id=1,kind=Cmd}";
    ASSERT_EQ(client->ShareFd(pipeFds[0], Span(sideband)), MOBILEGL_OK);

    // Symmetric with the SCM_RIGHTS path: a short sideband buffer is refused
    // before anything is consumed, so the descriptor is never dropped.
    std::vector<std::uint8_t> small(8);
    int fd = -1;
    std::uint64_t required = 0;
    MobileGLMutableByteSpan smallSpan{small.data(), small.size()};
    EXPECT_EQ(server->ReceiveFd(&fd, smallSpan, &required, 0), MOBILEGL_ERR_BUFFER_TOO_SMALL);
    EXPECT_EQ(required, FdPassing::kMaxSidebandBytes);
    EXPECT_EQ(fd, -1);

    std::vector<std::uint8_t> big(FdPassing::kMaxSidebandBytes);
    std::uint64_t sidebandSize = 0;
    MobileGLMutableByteSpan bigSpan{big.data(), big.size()};
    ASSERT_EQ(server->ReceiveFd(&fd, bigSpan, &sidebandSize, 100), MOBILEGL_OK);
    ASSERT_GE(fd, 0);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(big.data()),
                          static_cast<std::size_t>(sidebandSize)),
              sideband);

    // Same open file description, independent descriptor.
    const char payload[] = "bytes";
    ASSERT_EQ(::write(pipeFds[1], payload, sizeof(payload)), static_cast<ssize_t>(sizeof(payload)));
    char readBack[sizeof(payload)] = {};
    ASSERT_EQ(::read(fd, readBack, sizeof(readBack)), static_cast<ssize_t>(sizeof(payload)));
    EXPECT_STREQ(readBack, payload);

    ::close(fd);
    ::close(pipeFds[0]);
    ::close(pipeFds[1]);
}

TEST(InProcessTransportTest, AFrameWakeupIsNotEatenByAWaiterOnDescriptors) {
    std::unique_ptr<InProcessTransport> client;
    std::unique_ptr<InProcessTransport> server;
    InProcessTransport::CreatePair(client, server);

    // Two readers on the SAME endpoint, blocked on two different predicates.
    // With one condition_variable per direction and notify_one, the SendFrame
    // below could be delivered to the descriptor waiter, which re-tests its
    // own predicate and goes back to sleep - and the message then sits
    // undelivered until some unrelated later event. ITransport narrows the
    // contract to one dedicated reader thread, but that is a comment, and the
    // first caller that splits its reader should not have to discover this.
    std::atomic<bool> fdWaiterStarted{false};
    std::thread fdWaiter([&] {
        std::vector<std::uint8_t> sideband(FdPassing::kMaxSidebandBytes);
        MobileGLMutableByteSpan span{sideband.data(), sideband.size()};
        int fd = -1;
        std::uint64_t size = 0;
        fdWaiterStarted.store(true);
        // Never offered a descriptor: this one ends on the Shutdown below.
        EXPECT_EQ(client->ReceiveFd(&fd, span, &size, kWaitForever),
                  MOBILEGL_ERR_TRANSPORT_CLOSED);
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
        got = Receive(*client, 4000);
    });
    while (!frameWaiterStarted.load()) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const std::string message = "wake the right waiter";
    const auto start = std::chrono::steady_clock::now();
    ASSERT_EQ(server->SendFrame(Span(message)), MOBILEGL_OK);
    frameWaiter.join();
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - start)
                               .count();

    EXPECT_EQ(got, message);
    // Not "eventually, when the receive timed out and re-checked".
    EXPECT_LT(elapsedMs, 2000);

    client->Shutdown();
    fdWaiter.join();
}
#endif

TEST(InProcessTransportTest, DoorbellWakesAParkedWaiter) {
    std::unique_ptr<InProcessTransport> client;
    std::unique_ptr<InProcessTransport> server;
    InProcessTransport::CreatePair(client, server);

    // producerParked / consumerParked live in RingControl; here a standalone
    // flag stands in for one.
    std::atomic<std::uint32_t> parked{0};
    std::atomic<bool> ready{false};
    std::atomic<bool> woke{false};

    std::thread waiter([&] {
        woke.store(client->SelfDoorbell().Wait(
            parked, [&] { return ready.load(std::memory_order_acquire); }, kDefaultSpinUs, 5000));
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ready.store(true, std::memory_order_release);
    // The peer only rings when the waiter says it parked, which is what makes
    // the common (spin-only) case free.
    NotifyIfParked(server->PeerDoorbell(), parked);

    waiter.join();
    EXPECT_TRUE(woke.load());
    EXPECT_EQ(parked.load(), 0u);
}

TEST(InProcessTransportTest, DoorbellReturnsImmediatelyWhenAlreadyReady) {
    std::unique_ptr<InProcessTransport> client;
    std::unique_ptr<InProcessTransport> server;
    InProcessTransport::CreatePair(client, server);

    std::atomic<std::uint32_t> parked{0};
    // No notification is sent at all: a condition that is already true must
    // never park, or the lost-wakeup window would be reachable.
    EXPECT_TRUE(client->SelfDoorbell().Wait(
        parked, [] { return true; }, kDefaultSpinUs, 0));
    EXPECT_EQ(parked.load(), 0u);
}

TEST(InProcessTransportTest, DoorbellTimesOutWhenNothingHappens) {
    std::unique_ptr<InProcessTransport> client;
    std::unique_ptr<InProcessTransport> server;
    InProcessTransport::CreatePair(client, server);

    std::atomic<std::uint32_t> parked{0};
    const auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(client->SelfDoorbell().Wait(
        parked, [] { return false; }, kDefaultSpinUs, 30));
    EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - start)
                  .count(),
              20);
    EXPECT_EQ(parked.load(), 0u);
}
