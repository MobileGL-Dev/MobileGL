// SPDX-License-Identifier: LGPL-3.0-only
#include <MG_Remote/Transport/StreamLink.h>
#include <MG_Remote/Transport/SessionRings.h>
#include <gtest/gtest.h>
#include <array>
#include <chrono>
#include <cstring>
#include <thread>
#include <sys/socket.h>
#include <unistd.h>

using namespace MobileGL::MG_Remote::Transport;
namespace {
    class StreamPair : public testing::Test {
    protected:
        SessionSegments clientMemory, serverMemory;
        StreamLink client, server;
        RingProducer commands;
        RingConsumer consumer;
        SessionProducer publish;
        SessionConsumer apply;
        void SetUp() override {
            SessionSegmentSizes sizes;
            sizes.CmdRingBytes = 1024;
            sizes.StageBytes = 4096;
            sizes.ReplyBytes = 4096;
            sizes.EventRingBytes = 1024;
            ASSERT_EQ(clientMemory.CreatePrivate(sizes, MemoryRole::Client), MOBILEGL_OK);
            ASSERT_EQ(serverMemory.CreatePrivate(sizes, MemoryRole::Server), MOBILEGL_OK);
            int sockets[2];
            ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);
            ASSERT_EQ(client.Attach(sockets[0], clientMemory, TransportRoleTag::ClientProducer), MOBILEGL_OK);
            ASSERT_EQ(server.Attach(sockets[1], serverMemory, TransportRoleTag::ServerConsumer), MOBILEGL_OK);
            commands = RingProducer(clientMemory.CmdControl(), clientMemory.CmdRingBase(), 1024, RingCursorSet::Cmd);
            consumer = RingConsumer(serverMemory.CmdControl(), serverMemory.CmdRingBase(), 1024, RingCursorSet::Cmd);
            publish.Attach(clientMemory.CmdControl(), &commands, &client.ConsumerBell(), &client.ProducerBell(), 0);
            publish.SetLink(&client);
            apply.Attach(serverMemory.CmdControl(), &consumer, &server.ProducerBell(), &server.ConsumerBell(), 0);
            apply.SetLink(&server);
        }
        void TearDown() override {
            client.Detach();
            server.Detach();
        }
        void Queue(std::uint64_t seq, std::uint64_t value) {
            auto* p = commands.Reserve(1, 0, sizeof value);
            ASSERT_NE(p, nullptr);
            std::memcpy(p, &value, sizeof value);
            publish.PublishAndNotify(seq);
        }
        template <class F>
        bool Eventually(F ready) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!ready() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::yield();
            return ready();
        }
    };

    TEST_F(StreamPair, StagingArrivesBeforeItsCommandAndLastRecordProgressFlushesOnIdle) {
        const std::array<unsigned char, 8> bytes{1, 2, 3, 4, 5, 6, 7, 8};
        std::memcpy(static_cast<unsigned char*>(clientMemory.StageBase()) + 24, bytes.data(), bytes.size());
        client.NoteStage({24, bytes.size()});
        Queue(1, 24);
        ASSERT_EQ(client.Flush(), MOBILEGL_OK);
        ASSERT_EQ(apply.WaitForWork(2000), SessionWait::Reached);
        ASSERT_TRUE(apply.ApplyOne([&](const RingRecordView& v) {
            std::uint64_t offset;
            std::memcpy(&offset, v.payload, sizeof offset);
            const void* p = nullptr;
            ASSERT_EQ(server.ResolveSpan(LinkSegment::Stage, {offset, bytes.size()}, &p), MOBILEGL_OK);
            EXPECT_EQ(std::memcmp(p, bytes.data(), bytes.size()), 0);
        }));
        apply.RetireThrough(1);
        // This is the production idle discipline, not a polling sender thread.
        EXPECT_EQ(apply.WaitForWork(1), SessionWait::TimedOut);
        EXPECT_EQ(publish.WaitForApplied(1, 2000), SessionWait::Reached);
        EXPECT_EQ(client.Progress()->retiredSeq.load(), 1u);
    }

    TEST_F(StreamPair, RetiredByteCursorCrossesManyWrapsWithoutStageRingCursors) {
        for (std::uint64_t seq = 1; seq <= 160; ++seq) {
            Queue(seq, seq);
            ASSERT_EQ(client.Flush(), MOBILEGL_OK);
            ASSERT_EQ(apply.WaitForWork(2000), SessionWait::Reached);
            ASSERT_TRUE(apply.ApplyOne([&](const RingRecordView& v) {
                std::uint64_t value;
                std::memcpy(&value, v.payload, sizeof value);
                EXPECT_EQ(value, seq);
            }));
            apply.RetireThrough(seq);
            ASSERT_EQ(server.FlushProgress(), MOBILEGL_OK);
            ASSERT_EQ(publish.WaitForApplied(seq, 2000), SessionWait::Reached);
        }
        EXPECT_GT(commands.LocalHead(), 1024u);
        EXPECT_EQ(commands.FreeBytes(), 1024u);
        EXPECT_EQ(clientMemory.CmdControl()->stageHead.load(), 0u);
        EXPECT_EQ(clientMemory.CmdControl()->stageRetiredTail.load(), 0u);
    }

    TEST_F(StreamPair, UnwantedReplyDoesNotBlockLaterReplyAndAppliedPublication) {
        Queue(1, 1);
        Queue(2, 2);
        ASSERT_EQ(client.Flush(), MOBILEGL_OK);
        ASSERT_EQ(apply.WaitForWork(2000), SessionWait::Reached);
        ASSERT_TRUE(apply.ApplyOne([&](const RingRecordView&) {
            const std::uint64_t value = 11;
            EXPECT_EQ(server.PostReply(1, 0, &value, sizeof value), MOBILEGL_OK);
        }));
        ASSERT_TRUE(apply.ApplyOne([&](const RingRecordView&) {
            const std::uint64_t value = 22;
            EXPECT_EQ(server.PostReply(2, 0, &value, sizeof value), MOBILEGL_OK);
        }));
        apply.RetireThrough(2);
        ASSERT_EQ(server.FlushProgress(), MOBILEGL_OK);
        ASSERT_EQ(publish.WaitForApplied(2, 2000), SessionWait::Reached);
        std::int32_t status;
        const void* p = nullptr;
        std::uint64_t size = 0;
        ASSERT_EQ(client.ReadReply(2, &status, &p, &size), MOBILEGL_OK);
        ASSERT_EQ(size, sizeof(std::uint64_t));
        std::uint64_t value;
        std::memcpy(&value, p, sizeof value);
        EXPECT_EQ(value, 22u);
    }

    TEST_F(StreamPair, EventDrainReturnsCreditAndClearsRemoteFullLatch) {
        EventRingProducer events(serverMemory.EventControl(), serverMemory.CmdControl(), serverMemory.EventRingBase(),
                                 1024);
        EventRingConsumer read(clientMemory.EventControl(), clientMemory.CmdControl(), clientMemory.EventRingBase(),
                               1024, clientMemory.EventSegmentBase());
        read.SetLink(&client);
        auto* p = events.Reserve(kEventGlError, 64);
        ASSERT_NE(p, nullptr);
        std::memset(p, 0, 64);
        events.Ring().Publish();
        serverMemory.CmdControl()->eventRingFull.store(1);
        ASSERT_EQ(server.FlushProgress(), MOBILEGL_OK);
        ASSERT_TRUE(Eventually([&] { return clientMemory.CmdControl()->eventRingFull.load() != 0; }));
        RingRecordView record;
        ASSERT_TRUE(read.Pop(record));
        read.Drained();
        ASSERT_TRUE(Eventually([&] { return serverMemory.CmdControl()->eventRingFull.load() == 0; }));
        EXPECT_EQ(events.Ring().FreeBytes(), 1024u);
    }

    TEST_F(StreamPair, PeerEofKillsBothLocalBellsAndWakesAWaiter) {
        server.Detach();
        ASSERT_TRUE(Eventually([&] { return client.PeerHungUp(); }));
        EXPECT_TRUE(client.ProducerBell().Dead());
        EXPECT_TRUE(client.ConsumerBell().Dead());
        EXPECT_EQ(publish.WaitForApplied(10, 2000), SessionWait::ShutDown);
    }

    void WriteFrame(int fd, unsigned kind, std::uint64_t a, const void* payload, std::uint32_t size) {
        unsigned char h[28]{};
        auto put = [&](unsigned at, std::uint64_t value, unsigned width) {
            for (unsigned i = 0; i < width; ++i)
                h[at + i] = value >> (8 * i);
        };
        put(0, 0x444c474d, 4);
        put(4, size, 4);
        h[8] = kind;
        put(12, a, 8);
        ASSERT_EQ(::send(fd, h, sizeof h, MSG_NOSIGNAL), sizeof h);
        if (size) ASSERT_EQ(::send(fd, payload, size, MSG_NOSIGNAL), size);
    }

    TEST(StreamLinkDeath, StageOutsideNegotiatedWindowIsNamedProtocolCorruption) {
        EXPECT_DEATH(
            {
                SessionSegments memory;
                SessionSegmentSizes sizes;
                sizes.StageBytes = 64;
                memory.CreatePrivate(sizes, MemoryRole::Server);
                int sockets[2];
                socketpair(AF_UNIX, SOCK_STREAM, 0, sockets);
                StreamLink link;
                link.Attach(sockets[0], memory, TransportRoleTag::ServerConsumer);
                unsigned char byte = 1;
                WriteFrame(sockets[1], 2, 64, &byte, 1);
                std::this_thread::sleep_for(std::chrono::seconds(1));
            },
            "ProtocolCorruption.*window");
    }

    TEST(StreamLinkDeath, RegressingProgressIsNamedProtocolCorruption) {
        EXPECT_DEATH(
            {
                SessionSegments memory;
                SessionSegmentSizes sizes;
                memory.CreatePrivate(sizes, MemoryRole::Client);
                memory.CmdControl()->submittedSeq.store(3);
                int sockets[2];
                socketpair(AF_UNIX, SOCK_STREAM, 0, sockets);
                StreamLink link;
                link.Attach(sockets[0], memory, TransportRoleTag::ClientProducer);
                unsigned char progress[64]{};
                progress[0] = 2;
                WriteFrame(sockets[1], 4, 0, progress, 64);
                progress[0] = 1;
                WriteFrame(sockets[1], 4, 0, progress, 64);
                std::this_thread::sleep_for(std::chrono::seconds(1));
            },
            "ProtocolCorruption.*backwards");
    }
} // namespace
