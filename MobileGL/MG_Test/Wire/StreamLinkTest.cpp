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
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <cstdio>

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
        RingRecordView view;
        ASSERT_TRUE(consumer.Pop(view));
        [&](const RingRecordView& v) {
            std::uint64_t offset;
            std::memcpy(&offset, v.payload, sizeof offset);
            const void* p = nullptr;
            ASSERT_EQ(server.ResolveSpan(LinkSegment::Stage, {offset, bytes.size()}, &p), MOBILEGL_OK);
            EXPECT_EQ(std::memcmp(p, bytes.data(), bytes.size()), 0);
        }(view);
        // Advance locally without a periodic-progress callback: this isolates
        // flush-on-idle so removing it cannot be masked by the one-ms timer.
        Watermark::AdvanceApplied(*serverMemory.CmdControl(), 1);
        Watermark::AdvanceRetired(*serverMemory.CmdControl(), 1);
        consumer.PublishRetired();
        // This is the production idle discipline, not a polling sender thread.
        EXPECT_EQ(apply.WaitForWork(1), SessionWait::TimedOut);
        EXPECT_EQ(publish.WaitForApplied(1, 2000), SessionWait::Reached);
        // applied and retired are independent watermarks. The reader publishes
        // them in dependency order, and an applied waiter can run between stores.
        EXPECT_TRUE(client.ProducerBell().Wait(
            clientMemory.CmdControl()->producerParked,
            [&] { return client.Progress()->retiredSeq.load(std::memory_order_acquire) >= 1; }, 0, 2000));
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
        // P12: AN ANSWER IS KEPT ONLY IF SOMEBODY DECLARED IT WILL BE READ. Seq 1 is deliberately
        // NOT declared - this case is about an unwanted reply not disturbing a later one - and
        // seq 2 is, because the case reads it. Both records cross either way: the declaration
        // changes whether the LINK keeps the answer, never whether the server sends it.
        client.DeclareReplyRead(2);
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

    // ---- P12: AND THE REPLY A READER COMES BACK FOR IS READABLE BY ITS SEQ ----
    //
    // The case above reads the NEWEST answer, which is the only one a single-slot link could ever
    // hand back. The create window defers up to four answers and takes them in order, so the OLDEST
    // of them has to still be readable after newer ones have arrived - and against a
    // replace-on-arrival slot it was not: every drain read failed PROTOCOL_MISMATCH for ever, the
    // window stayed full, and every create fell back to the blocking path. Measured on the device
    // before this: 4,536 creates, 4,536 of them blocking, `pending` pinned at 4, refusals 0, wall
    // clock 29.0 s against 30.1 s with the window off. This is the transport half of that fix.
    TEST_F(StreamPair, AnOlderReplyIsStillReadableByItsSeq) {
        Queue(1, 1);
        Queue(2, 2);
        Queue(3, 3);
        ASSERT_EQ(client.Flush(), MOBILEGL_OK);
        ASSERT_EQ(apply.WaitForWork(2000), SessionWait::Reached);
        for (std::uint64_t seq = 1; seq <= 3; ++seq) client.DeclareReplyRead(seq);
        for (std::uint64_t seq = 1; seq <= 3; ++seq) {
            ASSERT_TRUE(apply.ApplyOne([&](const RingRecordView&) {
                const std::uint64_t value = seq * 11;
                EXPECT_EQ(server.PostReply(seq, 0, &value, sizeof value), MOBILEGL_OK);
            }));
        }
        apply.RetireThrough(3);
        ASSERT_EQ(server.FlushProgress(), MOBILEGL_OK);
        ASSERT_EQ(publish.WaitForApplied(3, 2000), SessionWait::Reached);
        // OLDEST FIRST, which is the order the window's drain takes them in.
        for (std::uint64_t seq = 1; seq <= 3; ++seq) {
            std::int32_t status = -1;
            const void* p = nullptr;
            std::uint64_t size = 0;
            ASSERT_EQ(client.ReadReply(seq, &status, &p, &size), MOBILEGL_OK) << "seq " << seq;
            ASSERT_EQ(size, sizeof(std::uint64_t)) << "seq " << seq;
            std::uint64_t value = 0;
            std::memcpy(&value, p, sizeof value);
            EXPECT_EQ(value, seq * 11) << "seq " << seq;
            EXPECT_EQ(status, 0) << "seq " << seq;
        }
    }

    // =====================================================================================
    // P12: THE DECLARED-ANSWER STORE - WHAT A BOUNDED BUFFER OF "THE LAST N" COULD NOT DO
    // =====================================================================================
    //
    // The create window defers an answer across an unbounded amount of other traffic (measured on
    // the device: ~1,000 records between two creates) and a buffer of the last N answers has always
    // moved on by the time the reader comes back - `want=310` on every miss while the ring spanned
    // [1175..1504]. Ring 8 -> 1024 as an experiment made every deferred answer come back, and these
    // two cases are what replaces that stand-in: an answer is kept because somebody DECLARED it
    // will be read, so the retained set is bounded by the client's own deferral depth.
    TEST_F(StreamPair, ADeclaredAnswerSurvivesFarMoreInterveningAnswersThanAnyFixedBuffer) {
        constexpr std::uint64_t kIntervening = 64;
        client.DeclareReplyRead(1);
        const std::uint64_t wanted = 0x5EED;
        ASSERT_EQ(server.PostReply(1, 0, &wanted, sizeof wanted), MOBILEGL_OK);
        // SIXTY-FOUR answers nobody declared. Under "keep the last N" - any N this link has ever
        // had - seq 1 is gone by the end of this loop; under a declaration it is untouched, which
        // is the whole difference between a bound and a probability.
        for (std::uint64_t seq = 2; seq <= kIntervening + 1; ++seq) {
            const std::uint64_t junk = seq;
            ASSERT_EQ(server.PostReply(seq, 0, &junk, sizeof junk), MOBILEGL_OK);
        }
        EXPECT_TRUE(Eventually([&] { return client.SkippedUnwanted() >= kIntervening; }))
            << "the undeclared answers were not dropped: " << client.SkippedUnwanted();
        std::int32_t status = -1;
        const void* p = nullptr;
        std::uint64_t size = 0;
        ASSERT_EQ(client.ReadReply(1, &status, &p, &size), MOBILEGL_OK)
            << "the declared answer did not survive " << kIntervening << " intervening ones";
        ASSERT_EQ(size, sizeof(std::uint64_t));
        std::uint64_t value = 0;
        std::memcpy(&value, p, sizeof value);
        EXPECT_EQ(value, wanted);
        EXPECT_EQ(status, 0);
        // AND TAKING IT RELEASED IT: the store holds only what is still outstanding, which is why
        // its capacity can be tied to the window's depth rather than to the server's answer rate.
        EXPECT_EQ(client.RetainedOutstanding(), 0u);
        EXPECT_EQ(client.RetainedTotal(), 1u);
    }

    TEST_F(StreamPair, AnAnswerNobodyDeclaredIsNotKeptAtAll) {
        const std::uint64_t value = 7;
        ASSERT_EQ(server.PostReply(1, 0, &value, sizeof value), MOBILEGL_OK);
        EXPECT_TRUE(Eventually([&] { return client.SkippedUnwanted() >= 1; }));
        EXPECT_EQ(client.RetainedOutstanding(), 0u)
            << "an undeclared answer was retained: the store would grow with the SERVER's answer"
               " rate, which is what makes any capacity a matter of luck";
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

    TEST_F(StreamPair, PeerEofReleasesAnAlreadyWaitingReplyReader) {
        std::atomic<bool> finished{false};
        MobileGLResult result = MOBILEGL_OK;
        std::thread waiting([&] {
            std::int32_t status;
            const void* payload;
            std::uint64_t size;
            result = client.ReadReply(1, &status, &payload, &size);
            finished.store(true, std::memory_order_release);
        });
        server.Detach();
        EXPECT_TRUE(Eventually([&] { return finished.load(std::memory_order_acquire); }));
        waiting.join();
        EXPECT_EQ(result, MOBILEGL_ERR_TRANSPORT_CLOSED);
    }

    TEST_F(StreamPair, AppliedWithoutItsReplyIsRefusedWithoutAnUnboundedWait) {
        Queue(1, 1);
        ASSERT_EQ(client.Flush(), MOBILEGL_OK);
        ASSERT_EQ(apply.WaitForWork(2000), SessionWait::Reached);
        ASSERT_TRUE(apply.ApplyOne([](const RingRecordView&) {}));
        apply.RetireThrough(1);
        ASSERT_EQ(server.FlushProgress(), MOBILEGL_OK);
        ASSERT_EQ(publish.WaitForApplied(1, 2000), SessionWait::Reached);
        std::int32_t status;
        const void* p;
        std::uint64_t size;
        EXPECT_EQ(client.ReadReply(1, &status, &p, &size), MOBILEGL_ERR_PROTOCOL_MISMATCH);
    }

    TEST_F(StreamPair, ControlFenceWaitsForEventBytesOnTheIndependentDataConnection) {
        EventRingProducer events(serverMemory.EventControl(), serverMemory.CmdControl(), serverMemory.EventRingBase(),
                                 1024);
        auto* payload = events.Reserve(kEventGlError, 8);
        ASSERT_NE(payload, nullptr);
        std::memset(payload, 0, 8);
        events.Ring().Publish();
        const auto fence = server.EventPublishedHead();
        EXPECT_EQ(client.WaitForEventDelivery(fence, 1), MOBILEGL_ERR_TIMEOUT);
        ASSERT_EQ(server.Flush(), MOBILEGL_OK);
        EXPECT_EQ(client.WaitForEventDelivery(fence, 2000), MOBILEGL_OK);
        EXPECT_GE(client.EventPublishedHead(), fence);
    }

    TEST(StreamLinkTcp, RecordsSixtyFourMiBStageBurstThroughputOnRealLoopbackTcp) {
        int listener = ::socket(AF_INET, SOCK_STREAM, 0);
        ASSERT_GE(listener, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ASSERT_EQ(::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof address), 0);
        ASSERT_EQ(::listen(listener, 1), 0);
        socklen_t addressBytes = sizeof address;
        ASSERT_EQ(::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &addressBytes), 0);
        int clientFd = ::socket(AF_INET, SOCK_STREAM, 0);
        ASSERT_GE(clientFd, 0);
        ASSERT_EQ(::connect(clientFd, reinterpret_cast<sockaddr*>(&address), sizeof address), 0);
        int serverFd = ::accept(listener, nullptr, nullptr);
        ASSERT_GE(serverFd, 0);
        ::close(listener);
        int enabled = 1;
        ASSERT_EQ(::setsockopt(clientFd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof enabled), 0);
        ASSERT_EQ(::setsockopt(serverFd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof enabled), 0);
        SessionSegmentSizes sizes;
        sizes.CmdRingBytes = 1024;
        sizes.EventRingBytes = 1024;
        sizes.StageBytes = 64ull * 1024 * 1024;
        sizes.ReplyBytes = 4096;
        std::unique_ptr<ILink> client, server;
        ASSERT_EQ(CreateStreamLink(clientFd, sizes, TransportRoleTag::ClientProducer, client), MOBILEGL_OK);
        ASSERT_EQ(CreateStreamLink(serverFd, sizes, TransportRoleTag::ServerConsumer, server), MOBILEGL_OK);
        client->InitializeEndpoints();
        server->InitializeEndpoints();
        std::memset(client->Memory().StageBase(), 0x5a, static_cast<std::size_t>(sizes.StageBytes));
        client->NoteStage({0, sizes.StageBytes});
        SessionProducer producer;
        producer.Attach(*client, 0);
        SessionConsumer consumer;
        consumer.Attach(*server, 0);
        auto* record = client->CommandsOut().Reserve(1, 0, sizeof(std::uint64_t));
        ASSERT_NE(record, nullptr);
        std::memcpy(record, &sizes.StageBytes, sizeof(std::uint64_t));
        producer.PublishAndNotify(1);
        const auto start = std::chrono::steady_clock::now();
        ASSERT_EQ(client->Flush(), MOBILEGL_OK);
        ASSERT_EQ(consumer.WaitForWork(10000), SessionWait::Reached);
        ASSERT_TRUE(consumer.ApplyOne([&](const RingRecordView&) {
            const auto* bytes = static_cast<const unsigned char*>(server->Memory().StageBase());
            for (std::uint64_t offset = 0; offset < sizes.StageBytes; offset += 4096)
                EXPECT_EQ(bytes[offset], 0x5a);
            EXPECT_EQ(bytes[sizes.StageBytes - 1], 0x5a);
        }));
        consumer.RetireThrough(1);
        ASSERT_EQ(server->FlushProgress(), MOBILEGL_OK);
        ASSERT_EQ(producer.WaitForApplied(1, 10000), SessionWait::Reached);
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const double mibPerSecond = 64.0 / elapsed;
        RecordProperty("tcp_stage_bytes", std::to_string(sizes.StageBytes));
        RecordProperty("tcp_stage_mib_per_second", std::to_string(mibPerSecond));
        std::printf("P6.5 Stage burst control=none data=tcp-loopback bytes=%llu seconds=%.6f MiB/s=%.3f\n",
                    static_cast<unsigned long long>(sizes.StageBytes), elapsed, mibPerSecond);
    }

    // PH-7 (4), ID-P7-3, through the Transport factory pair ServerSession::Accept uses so that
    // session code never names the concrete link (scripts/ci/link_seam_purity.py). In the
    // production order: created without a descriptor, endpoints initialised, then bound once.
    // A second bind, and a bind onto a link the factory did not make as a stream, are refused
    // and leave the descriptor with the caller; the bound link then carries a record.
    TEST(StreamLinkFactory, DeferredServerLinkBindsOnceAndThenCarriesARecord) {
        SessionSegmentSizes sizes;
        sizes.CmdRingBytes = 1024;
        sizes.EventRingBytes = 1024;
        sizes.StageBytes = 4096;
        sizes.ReplyBytes = 4096;
        std::unique_ptr<ILink> server;
        ASSERT_EQ(CreateDeferredStreamLink(sizes, TransportRoleTag::ServerConsumer, server), MOBILEGL_OK);
        ASSERT_NE(server, nullptr);
        EXPECT_FALSE(server->Capabilities().PublishIsDelivery);
        server->InitializeEndpoints();

        int sockets[2];
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);
        std::unique_ptr<ILink> client;
        ASSERT_EQ(CreateStreamLink(sockets[0], sizes, TransportRoleTag::ClientProducer, client), MOBILEGL_OK);
        client->InitializeEndpoints();
        ASSERT_EQ(BindStreamLinkDataFd(*server, sockets[1]), MOBILEGL_OK);

        int spare[2];
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, spare), 0);
        EXPECT_EQ(BindStreamLinkDataFd(*server, spare[0]), MOBILEGL_ERR_INVALID_ARGUMENT);
        auto shared = CreateSharedLink(TransportRoleTag::ServerConsumer);
        ASSERT_NE(shared, nullptr);
        EXPECT_EQ(BindStreamLinkDataFd(*shared, spare[1]), MOBILEGL_ERR_INVALID_ARGUMENT);
        EXPECT_EQ(::close(spare[0]), 0);
        EXPECT_EQ(::close(spare[1]), 0);

        SessionProducer producer;
        producer.Attach(*client, 0);
        SessionConsumer consumer;
        consumer.Attach(*server, 0);
        const std::uint64_t value = 0x5a5a5a5aull;
        auto* record = client->CommandsOut().Reserve(1, 0, sizeof value);
        ASSERT_NE(record, nullptr);
        std::memcpy(record, &value, sizeof value);
        producer.PublishAndNotify(1);
        ASSERT_EQ(client->Flush(), MOBILEGL_OK);
        ASSERT_EQ(consumer.WaitForWork(2000), SessionWait::Reached);
        ASSERT_TRUE(consumer.ApplyOne([&](const RingRecordView& v) {
            std::uint64_t seen = 0;
            std::memcpy(&seen, v.payload, sizeof seen);
            EXPECT_EQ(seen, value);
        }));
        consumer.RetireThrough(1);
        ASSERT_EQ(server->FlushProgress(), MOBILEGL_OK);
        EXPECT_EQ(producer.WaitForApplied(1, 2000), SessionWait::Reached);
    }

    void WriteFrame(int fd, unsigned kind, std::uint64_t a, const void* payload, std::uint32_t size) {
        unsigned char h[28]{};
        auto put = [&](unsigned at, std::uint64_t value, unsigned width) {
            for (unsigned i = 0; i < width; ++i)
                h[at + i] = value >> (8 * i);
        };
        put(0, 0x444c474d, 4);
        put(4, size + 20, 4);
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
