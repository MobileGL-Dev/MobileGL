// MobileGL - MobileGL/MG_Test/Wire/RingTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The SEG_CMD/SEG_STAGE SPSC ring: layout of the shared control page, cursor
// invariants, wrap-around, backpressure, the generation bump after a hard
// drain, and a real two-thread producer/consumer run.

#include <MG_Remote/Transport/Ring.h>

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

using namespace MobileGL::MG_Remote::Transport;

namespace {

    // A ring plus its control page, sized like a small SEG_CMD.
    class RingFixture {
    public:
        explicit RingFixture(std::uint64_t capacity, RingCursorSet cursors = RingCursorSet::Cmd)
            : m_bytes(static_cast<std::size_t>(capacity)), m_capacity(capacity) {
            InitRingControl(m_control);
            m_producer = RingProducer(&m_control, m_bytes.data(), capacity, cursors);
            m_consumer = RingConsumer(&m_control, m_bytes.data(), capacity, cursors);
            m_cursors = cursors;
        }

        RingControl& Control() { return m_control; }
        RingProducer& Producer() { return m_producer; }
        RingConsumer& Consumer() { return m_consumer; }
        std::uint64_t Capacity() const { return m_capacity; }
        bool Invariants() const { return RingCursorsValid(m_control, m_cursors, m_capacity); }

        // Writes one record whose payload is `size` bytes of a recognisable
        // pattern seeded by `seed`.
        bool WriteRecord(std::uint16_t kind, std::uint64_t size, std::uint8_t seed) {
            void* payload = m_producer.Reserve(kind, kRecNone, size);
            if (payload == nullptr) {
                return false;
            }
            auto* bytes = static_cast<std::uint8_t*>(payload);
            for (std::uint64_t i = 0; i < size; ++i) {
                bytes[i] = static_cast<std::uint8_t>(seed + i);
            }
            m_producer.Publish();
            return true;
        }

        static bool CheckPattern(const RingRecordView& view, std::uint64_t size, std::uint8_t seed) {
            const auto* bytes = static_cast<const std::uint8_t*>(view.payload);
            for (std::uint64_t i = 0; i < size; ++i) {
                if (bytes[i] != static_cast<std::uint8_t>(seed + i)) {
                    return false;
                }
            }
            return true;
        }

    private:
        alignas(4096) RingControl m_control{};
        std::vector<std::uint8_t> m_bytes;
        RingProducer m_producer;
        RingConsumer m_consumer;
        std::uint64_t m_capacity;
        RingCursorSet m_cursors = RingCursorSet::Cmd;
    };

} // namespace

TEST(RingTest, ControlPageLayoutIsTheSharedContract) {
    // The page is mapped by two processes; its size and alignment are wire
    // contract, not an implementation detail.
    EXPECT_EQ(sizeof(RingControl), 4096u);
    EXPECT_EQ(alignof(RingControl), 4096u);
    EXPECT_EQ(sizeof(RingRecordHeader), 8u);

    alignas(4096) RingControl control{};
    InitRingControl(control);
    // Zero is reserved for "uninitialized" on both generations.
    EXPECT_EQ(control.serverEpoch.load(), 1u);
    EXPECT_EQ(control.ringGeneration.load(), 1u);
    EXPECT_EQ(control.cmdHead.load(), 0u);
    EXPECT_EQ(control.stageHead.load(), 0u);
    EXPECT_EQ(control.consumerParked.load(), 0u);
    EXPECT_EQ(control.producerParked.load(), 0u);
    EXPECT_EQ(control.eventRingFull.load(), 0u);
    EXPECT_EQ(control.eventDropped.load(), 0u);

    // Each contended group on its own cache line.
    const auto offset = [&control](const void* member) {
        return reinterpret_cast<const std::uint8_t*>(member) -
               reinterpret_cast<const std::uint8_t*>(&control);
    };
    EXPECT_EQ(offset(&control.cmdHead) % 64, 0);
    EXPECT_EQ(offset(&control.cmdAppliedTail) % 64, 0);
    EXPECT_EQ(offset(&control.stageHead) % 64, 0);
    EXPECT_EQ(offset(&control.stageAppliedTail) % 64, 0);
    EXPECT_EQ(offset(&control.appliedSeq) % 64, 0);
    EXPECT_EQ(offset(&control.serverEpoch) % 64, 0);
    // cmdHead and cmdAppliedTail are written by different processes: they must
    // not share a line.
    EXPECT_NE(offset(&control.cmdHead) / 64, offset(&control.cmdAppliedTail) / 64);
}

TEST(RingTest, RejectsANonPowerOfTwoCapacity) {
    alignas(4096) RingControl control{};
    InitRingControl(control);
    std::vector<std::uint8_t> bytes(1000);
    RingProducer producer(&control, bytes.data(), 1000, RingCursorSet::Cmd);
    EXPECT_FALSE(producer.Valid());
    EXPECT_EQ(producer.Reserve(1, kRecNone, 8), nullptr);
}

TEST(RingTest, RoundTripsRecordsInOrder) {
    RingFixture ring(4096);
    ASSERT_TRUE(ring.WriteRecord(1, 16, 0x10));
    ASSERT_TRUE(ring.WriteRecord(2, 24, 0x20));
    EXPECT_TRUE(ring.Invariants());

    RingRecordView view{};
    bool corrupt = false;
    ASSERT_TRUE(ring.Consumer().Pop(view, &corrupt));
    EXPECT_FALSE(corrupt);
    EXPECT_EQ(view.kind, 1u);
    EXPECT_EQ(view.payloadSize, 16u);
    EXPECT_TRUE(RingFixture::CheckPattern(view, 16, 0x10));

    ASSERT_TRUE(ring.Consumer().Pop(view, &corrupt));
    EXPECT_EQ(view.kind, 2u);
    EXPECT_EQ(view.payloadSize, 24u);
    EXPECT_TRUE(RingFixture::CheckPattern(view, 24, 0x20));

    EXPECT_FALSE(ring.Consumer().Pop(view, &corrupt));
    ring.Consumer().PublishRetired();
    EXPECT_TRUE(ring.Invariants());
    EXPECT_EQ(ring.Control().cmdAppliedTail.load(), ring.Control().cmdHead.load());
    EXPECT_EQ(ring.Control().cmdRetiredTail.load(), ring.Control().cmdHead.load());
}

TEST(RingTest, PayloadIsPaddedToTheRecordAlignment) {
    RingFixture ring(4096);
    ASSERT_TRUE(ring.WriteRecord(7, 3, 0x77));
    RingRecordView view{};
    ASSERT_TRUE(ring.Consumer().Pop(view));
    // 8 (header) + 3 rounded up to 16 -> 8 bytes of payload space.
    EXPECT_EQ(view.payloadSize, 8u);
    EXPECT_TRUE(RingFixture::CheckPattern(view, 3, 0x77));
}

TEST(RingTest, WrapsWithoutSplittingARecord) {
    // Small ring, records that do not divide it evenly, so the wrap boundary
    // lands mid-record and the pad path is exercised many times.
    RingFixture ring(256);
    std::uint8_t seed = 0;
    for (int i = 0; i < 200; ++i) {
        const std::uint64_t size = 24 + (i % 5) * 8;
        ASSERT_TRUE(ring.WriteRecord(static_cast<std::uint16_t>(1 + (i % 3)), size, seed))
            << "record " << i;
        RingRecordView view{};
        bool corrupt = false;
        ASSERT_TRUE(ring.Consumer().Pop(view, &corrupt)) << "record " << i;
        ASSERT_FALSE(corrupt);
        EXPECT_EQ(view.kind, static_cast<std::uint16_t>(1 + (i % 3)));
        // Contiguity: the payload never straddles the end of the mapping.
        EXPECT_TRUE(RingFixture::CheckPattern(view, size, seed)) << "record " << i;
        ring.Consumer().PublishRetired();
        ASSERT_TRUE(ring.Invariants());
        seed = static_cast<std::uint8_t>(seed + 13);
    }
    // Cursors are monotonic byte counts, so they are far past the capacity.
    EXPECT_GT(ring.Control().cmdHead.load(), ring.Capacity());
}

TEST(RingTest, FullRingRefusesAndRecoversWhenTheConsumerRetires) {
    RingFixture ring(256);
    int written = 0;
    while (ring.WriteRecord(1, 24, static_cast<std::uint8_t>(written))) {
        ++written;
        ASSERT_LT(written, 100);
    }
    EXPECT_GT(written, 0);
    // Backpressure, not corruption.
    EXPECT_TRUE(ring.Invariants());
    EXPECT_LT(ring.Producer().FreeBytes(), 32u);

    RingRecordView view{};
    ASSERT_TRUE(ring.Consumer().Pop(view));
    // Applied alone does not free a slot that may still be borrowed by the GPU
    // timeline: reclaim follows the retired cursor.
    ring.Consumer().PublishApplied();
    EXPECT_EQ(ring.Producer().FreeBytes(), 0u);
    ring.Consumer().PublishRetired();
    EXPECT_GT(ring.Producer().FreeBytes(), 0u);
    EXPECT_TRUE(ring.WriteRecord(1, 24, 0xEE));
}

TEST(RingTest, RecordLargerThanTheRingIsRefused) {
    RingFixture ring(256);
    EXPECT_EQ(ring.Producer().Reserve(1, kRecNone, 4096), nullptr);
    EXPECT_TRUE(ring.Invariants());
}

TEST(RingTest, HardDrainBumpsTheGenerationOnlyWhenQuiesced) {
    RingFixture ring(256);
    ASSERT_TRUE(ring.WriteRecord(1, 32, 0x01));
    const std::uint32_t before = ring.Control().ringGeneration.load();

    // Records still in flight: the drain is refused and nothing changes.
    EXPECT_EQ(HardDrainRing(ring.Control(), RingCursorSet::Cmd), MOBILEGL_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ring.Control().ringGeneration.load(), before);

    RingRecordView view{};
    ASSERT_TRUE(ring.Consumer().Pop(view));
    ring.Consumer().PublishRetired();
    EXPECT_EQ(HardDrainRing(ring.Control(), RingCursorSet::Cmd), MOBILEGL_OK);
    EXPECT_EQ(ring.Control().ringGeneration.load(), before + 1);
    // Cursors stay monotonic across the drain - only the generation moves.
    EXPECT_EQ(ring.Control().cmdHead.load(), ring.Control().cmdAppliedTail.load());
    EXPECT_GT(ring.Control().cmdHead.load(), 0u);
}

TEST(RingTest, CorruptHeaderIsRefusedRatherThanDispatched) {
    // SEG_CMD is written by the peer process, so a compile-time size assert on
    // the record catalogue proves nothing about what is actually in the
    // mapping. Hand-build a ring whose first header is impossible (a size that
    // is not a multiple of 8) and check the consumer refuses it instead of
    // dispatching into undefined behaviour.
    alignas(4096) RingControl control{};
    InitRingControl(control);
    std::vector<std::uint8_t> bytes(256, 0);
    RingRecordHeader bad{};
    bad.kind = 5;
    bad.flags = kRecNone;
    bad.size = 13; // not 8-aligned
    std::memcpy(bytes.data(), &bad, sizeof(bad));
    control.cmdHead.store(64, std::memory_order_release);

    RingConsumer consumer(&control, bytes.data(), bytes.size(), RingCursorSet::Cmd);
    RingRecordView view{};
    bool corrupt = false;
    EXPECT_FALSE(consumer.Pop(view, &corrupt));
    EXPECT_TRUE(corrupt);

    // A record claiming more bytes than the producer has published is the same
    // class of violation and is refused the same way.
    bad.size = 128;
    std::memcpy(bytes.data(), &bad, sizeof(bad));
    RingConsumer second(&control, bytes.data(), bytes.size(), RingCursorSet::Cmd);
    corrupt = false;
    EXPECT_FALSE(second.Pop(view, &corrupt));
    EXPECT_TRUE(corrupt);
}

TEST(RingTest, SpscProducerConsumerThreadsAgreeOnEveryRecord) {
    constexpr int kRecords = 20000;
    RingFixture ring(4096);

    std::atomic<bool> failed{false};
    std::atomic<int> consumed{0};

    std::thread consumer([&] {
        int next = 0;
        while (next < kRecords) {
            RingRecordView view{};
            bool corrupt = false;
            if (!ring.Consumer().Pop(view, &corrupt)) {
                if (corrupt) {
                    failed.store(true);
                    return;
                }
                std::this_thread::yield();
                continue;
            }
            const std::uint32_t expectedKind = static_cast<std::uint16_t>(1 + (next % 7));
            if (view.kind != expectedKind || view.payloadSize < sizeof(std::uint32_t)) {
                failed.store(true);
                return;
            }
            std::uint32_t value = 0;
            std::memcpy(&value, view.payload, sizeof(value));
            if (value != static_cast<std::uint32_t>(next)) {
                failed.store(true);
                return;
            }
            ++next;
            consumed.store(next, std::memory_order_relaxed);
            // Retire as we go; a consumer that never retires would deadlock the
            // producer, which is exactly the contract being pinned.
            ring.Consumer().PublishRetired();
        }
    });

    for (int i = 0; i < kRecords; ++i) {
        const std::uint64_t payloadSize = sizeof(std::uint32_t) + (i % 4) * 8;
        void* payload = nullptr;
        while ((payload = ring.Producer().Reserve(static_cast<std::uint16_t>(1 + (i % 7)),
                                                  kRecNone, payloadSize)) == nullptr) {
            if (failed.load()) {
                break;
            }
            std::this_thread::yield();
        }
        if (payload == nullptr) {
            break;
        }
        const std::uint32_t value = static_cast<std::uint32_t>(i);
        std::memcpy(payload, &value, sizeof(value));
        ring.Producer().Publish();
    }

    consumer.join();
    EXPECT_FALSE(failed.load());
    EXPECT_EQ(consumed.load(), kRecords);
    EXPECT_TRUE(ring.Invariants());
    EXPECT_EQ(ring.Control().cmdRetiredTail.load(), ring.Control().cmdHead.load());
}
