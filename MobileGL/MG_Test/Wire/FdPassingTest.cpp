// MobileGL - MobileGL/MG_Test/Wire/FdPassingTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// SCM_RIGHTS across a real process boundary: a forked child creates a shared
// segment, fills it, and hands the descriptor over the aux socket; the parent
// adopts it, maps it read-only and compares every byte.
//
// This is the test the earlier branch never had. Its transport hardcoded
// `out->fd = -1` in the offer poll, so its data plane could not move a byte
// between processes - and nothing in its suite noticed, because everything ran
// in one process.

#include <MG_Remote/Transport/Doorbell.h>
#include <MG_Remote/Transport/FdPassing.h>
#include <MG_Remote/Transport/ShmSegment.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace MobileGL::MG_Remote::Transport;

namespace {

    constexpr std::uint64_t kSegmentSize = 64 * 1024;

    std::uint8_t ByteAt(std::uint64_t index) {
        return static_cast<std::uint8_t>((index * 31u + 7u) & 0xFFu);
    }

    // Child-side exit codes, so a failure says where it happened.
    enum ChildStatus : int {
        kChildOk = 0,
        kChildCreateFailed = 2,
        kChildMapFailed = 3,
        kChildSendFailed = 4,
    };

} // namespace

TEST(FdPassingTest, IsSupportedOnThisPlatform) { EXPECT_TRUE(FdPassing::Supported()); }

TEST(FdPassingTest, ChildSharesASegmentThatTheParentMapsAndVerifies) {
    int sockets[2] = {-1, -1};
    ASSERT_EQ(FdPassing::CreateSocketPair(sockets), MOBILEGL_OK);

    const std::string sideband = "SegmentRef{id=7,kind=Stage}";

    const pid_t pid = ::fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        // Child. No gtest assertions here: a failed expectation in a forked
        // child would report into a copy of the parent's test state.
        ::close(sockets[0]);
        int status = kChildOk;
        ShmSegment segment;
        if (ShmSegment::Create("fdpass", kSegmentSize, segment) != MOBILEGL_OK) {
            status = kChildCreateFailed;
        } else if (segment.Map(false) != MOBILEGL_OK) {
            status = kChildMapFailed;
        } else {
            auto* bytes = static_cast<std::uint8_t*>(segment.Data());
            for (std::uint64_t i = 0; i < kSegmentSize; ++i) {
                bytes[i] = ByteAt(i);
            }
            const MobileGLByteSpan span{sideband.data(), sideband.size()};
            if (FdPassing::SendFd(sockets[1], segment.Fd(), span) != MOBILEGL_OK) {
                status = kChildSendFailed;
            }
        }
        ::close(sockets[1]);
        ::_exit(status);
    }

    // Parent.
    ::close(sockets[1]);

    // A destination smaller than kMaxSidebandBytes is refused BEFORE the
    // datagram is consumed, so the descriptor is not lost by a caller that
    // guessed the size wrong.
    std::vector<std::uint8_t> small(8);
    int fd = -1;
    std::uint64_t required = 0;
    MobileGLMutableByteSpan smallSpan{small.data(), small.size()};
    EXPECT_EQ(FdPassing::ReceiveFd(sockets[0], &fd, smallSpan, &required, 5000),
              MOBILEGL_ERR_BUFFER_TOO_SMALL);
    EXPECT_EQ(required, FdPassing::kMaxSidebandBytes);
    EXPECT_EQ(fd, -1);

    std::vector<std::uint8_t> sidebandBuffer(FdPassing::kMaxSidebandBytes);
    std::uint64_t sidebandSize = 0;
    MobileGLMutableByteSpan sidebandSpan{sidebandBuffer.data(), sidebandBuffer.size()};
    ASSERT_EQ(FdPassing::ReceiveFd(sockets[0], &fd, sidebandSpan, &sidebandSize, 5000),
              MOBILEGL_OK);
    ASSERT_GE(fd, 0);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(sidebandBuffer.data()),
                          static_cast<std::size_t>(sidebandSize)),
              sideband);

    ShmSegment adopted;
    ASSERT_EQ(ShmSegment::Adopt(fd, kSegmentSize, adopted), MOBILEGL_OK);
    EXPECT_TRUE(adopted.Valid());
    ASSERT_EQ(adopted.Map(true), MOBILEGL_OK);
    EXPECT_TRUE(adopted.MappedReadOnly());

    const auto* bytes = static_cast<const std::uint8_t*>(adopted.Data());
    ASSERT_NE(bytes, nullptr);
    std::uint64_t mismatches = 0;
    for (std::uint64_t i = 0; i < kSegmentSize; ++i) {
        if (bytes[i] != ByteAt(i)) {
            ++mismatches;
        }
    }
    EXPECT_EQ(mismatches, 0u);

    int childStatus = 0;
    ASSERT_EQ(::waitpid(pid, &childStatus, 0), pid);
    ASSERT_TRUE(WIFEXITED(childStatus));
    EXPECT_EQ(WEXITSTATUS(childStatus), kChildOk);

    ::close(sockets[0]);
}

TEST(FdPassingTest, ReceiveTimesOutWithNoOffer) {
    int sockets[2] = {-1, -1};
    ASSERT_EQ(FdPassing::CreateSocketPair(sockets), MOBILEGL_OK);

    std::vector<std::uint8_t> sidebandBuffer(FdPassing::kMaxSidebandBytes);
    int fd = -1;
    std::uint64_t sidebandSize = 0;
    MobileGLMutableByteSpan span{sidebandBuffer.data(), sidebandBuffer.size()};
    EXPECT_EQ(FdPassing::ReceiveFd(sockets[0], &fd, span, &sidebandSize, 20), MOBILEGL_ERR_TIMEOUT);
    EXPECT_EQ(fd, -1);

    ::close(sockets[0]);
    ::close(sockets[1]);
}

TEST(FdPassingTest, RejectsBadArguments) {
    int sockets[2] = {-1, -1};
    ASSERT_EQ(FdPassing::CreateSocketPair(sockets), MOBILEGL_OK);

    const std::vector<std::uint8_t> tooBig(FdPassing::kMaxSidebandBytes + 1, 0);
    const MobileGLByteSpan oversized{tooBig.data(), tooBig.size()};
    EXPECT_EQ(FdPassing::SendFd(sockets[1], sockets[0], oversized), MOBILEGL_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(FdPassing::SendFd(sockets[1], -1, MobileGLByteSpan{nullptr, 0}),
              MOBILEGL_ERR_INVALID_ARGUMENT);

    ::close(sockets[0]);
    ::close(sockets[1]);
}

TEST(FdPassingTest, SegmentWithoutASidebandStillCarriesItsDescriptor) {
    int sockets[2] = {-1, -1};
    ASSERT_EQ(FdPassing::CreateSocketPair(sockets), MOBILEGL_OK);

    ShmSegment segment;
    ASSERT_EQ(ShmSegment::Create("nosideband", 4096, segment), MOBILEGL_OK);
    ASSERT_EQ(segment.Map(false), MOBILEGL_OK);
    static_cast<std::uint8_t*>(segment.Data())[0] = 0xA5;

    ASSERT_EQ(FdPassing::SendFd(sockets[1], segment.Fd(), MobileGLByteSpan{nullptr, 0}),
              MOBILEGL_OK);

    std::vector<std::uint8_t> sidebandBuffer(FdPassing::kMaxSidebandBytes);
    int fd = -1;
    std::uint64_t sidebandSize = 123;
    MobileGLMutableByteSpan span{sidebandBuffer.data(), sidebandBuffer.size()};
    ASSERT_EQ(FdPassing::ReceiveFd(sockets[0], &fd, span, &sidebandSize, 5000), MOBILEGL_OK);
    EXPECT_EQ(sidebandSize, 0u);
    ASSERT_GE(fd, 0);

    ShmSegment adopted;
    ASSERT_EQ(ShmSegment::Adopt(fd, 4096, adopted), MOBILEGL_OK);
    ASSERT_EQ(adopted.Map(true), MOBILEGL_OK);
    EXPECT_EQ(static_cast<const std::uint8_t*>(adopted.Data())[0], 0xA5);

    ::close(sockets[0]);
    ::close(sockets[1]);
}

// The `spawn` doorbell rides the same kind of socket as the fd channel, so it
// is covered here rather than beside the in-process one.
TEST(FdPassingTest, SocketDoorbellWakesAParkedWaiter) {
    int sockets[2] = {-1, -1};
    ASSERT_EQ(FdPassing::CreateSocketPair(sockets), MOBILEGL_OK);

    // One end each: the waiter reads its own end, the notifier writes the
    // other, exactly as the two processes will.
    SocketDoorbell waiterBell(sockets[0], kDoorbellWatermarkAdvanced, /*ownsFd=*/false);
    SocketDoorbell notifierBell(sockets[1], kDoorbellWatermarkAdvanced, /*ownsFd=*/false);

    std::atomic<std::uint32_t> parked{0};
    std::atomic<bool> ready{false};
    std::atomic<bool> woke{false};

    std::thread waiter([&] {
        woke.store(waiterBell.Wait(
            parked, [&] { return ready.load(std::memory_order_acquire); }, kDefaultSpinUs, 5000));
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ready.store(true, std::memory_order_release);
    NotifyIfParked(notifierBell, parked);

    waiter.join();
    EXPECT_TRUE(woke.load());
    EXPECT_EQ(parked.load(), 0u);

    ::close(sockets[0]);
    ::close(sockets[1]);
}

TEST(FdPassingTest, SocketDoorbellTimesOutAndRemembersAnEarlyWakeup) {
    int sockets[2] = {-1, -1};
    ASSERT_EQ(FdPassing::CreateSocketPair(sockets), MOBILEGL_OK);

    SocketDoorbell waiterBell(sockets[0], kDoorbellRingAdvanced, /*ownsFd=*/false);
    SocketDoorbell notifierBell(sockets[1], kDoorbellRingAdvanced, /*ownsFd=*/false);

    // Nothing rings: the park has to end on its deadline, not hang.
    const auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(waiterBell.Park(30));
    EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - start)
                  .count(),
              20);

    // A wakeup that arrives before anyone parks is not lost - it is sitting in
    // the socket buffer, so the next Park returns at once.
    notifierBell.Notify();
    EXPECT_TRUE(waiterBell.Park(1000));
    // ...and it was consumed, so the one after that times out again.
    EXPECT_FALSE(waiterBell.Park(10));

    ::close(sockets[0]);
    ::close(sockets[1]);
}

// A doorbell whose peer has hung up must report that, not keep saying "ready".
// Park used to treat any `poll` return > 0 as a wakeup without ever looking at
// revents, and a closed peer leaves a stream socket permanently poll-ready
// with nothing to read - so Doorbell::Wait re-parked in a tight loop at full
// clock, unbounded when the caller passed kWaitForever. That is the pathology
// the bidirectional doorbell exists to prevent, arrived at from the other
// side.
TEST(FdPassingTest, SocketDoorbellStopsParkingWhenThePeerHangsUp) {
    // A SOCK_STREAM pair, not FdPassing::CreateSocketPair's datagram pair:
    // measured on Linux, a closed peer makes a stream end report
    // POLLIN|POLLHUP with recv()==0, while a datagram end reports no readiness
    // at all. The stream shape is what the spawn transport will use, and it is
    // the shape that used to spin.
    int sockets[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);

    SocketDoorbell waiterBell(sockets[0], kDoorbellRingAdvanced, /*ownsFd=*/true);
    ASSERT_EQ(::close(sockets[1]), 0);

    const auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(waiterBell.Park(kWaitForever));
    EXPECT_TRUE(waiterBell.Dead());
    // Latched: no second syscall storm either.
    EXPECT_FALSE(waiterBell.Park(kWaitForever));

    // ...and a Wait with no deadline at all gives up instead of re-parking.
    std::atomic<std::uint32_t> parked{0};
    EXPECT_FALSE(waiterBell.Wait(
        parked, [] { return false; }, /*spinUs=*/0, kWaitForever));
    EXPECT_EQ(parked.load(), 0u);
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - start)
                  .count(),
              1000);
}

TEST(FdPassingTest, SocketDoorbellStillDeliversTheLastRingBeforeAHangup) {
    int sockets[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);

    SocketDoorbell waiterBell(sockets[0], kDoorbellRingAdvanced, /*ownsFd=*/true);
    SocketDoorbell notifierBell(sockets[1], kDoorbellRingAdvanced, /*ownsFd=*/false);

    // Ring, then die. Detecting the hangup must not swallow the wakeup that
    // was already queued - the peer's last publish is the one a waiter is
    // most likely to be blocked on.
    notifierBell.Notify();
    ASSERT_EQ(::close(sockets[1]), 0);

    EXPECT_TRUE(waiterBell.Park(1000));
    EXPECT_TRUE(waiterBell.Dead());
    EXPECT_FALSE(waiterBell.Park(kWaitForever));
}
