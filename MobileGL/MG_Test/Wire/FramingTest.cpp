// MobileGL - MobileGL/MG_Test/Wire/FramingTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The control-channel frame codec, and specifically the two contracts the
// earlier branch's codec got wrong: a bad header must be REPORTED (it used to
// turn into a silent permanent hang) and a too-small destination buffer must
// KEEP the message (it used to fail the call and drop it, wedging the stream).

#include <MG_Remote/Transport/Framing.h>

#include <gtest/gtest.h>

#include <cstring>
#include <numeric>
#include <vector>

using namespace MobileGL::MG_Remote::Transport;

namespace {

    std::vector<std::uint8_t> Pattern(std::size_t size, std::uint8_t seed) {
        std::vector<std::uint8_t> out(size);
        for (std::size_t i = 0; i < size; ++i) {
            out[i] = static_cast<std::uint8_t>(seed + i * 7u);
        }
        return out;
    }

} // namespace

TEST(FramingTest, RoundTripsTwoMessagesFedOneByteAtATime) {
    const std::vector<std::uint8_t> first = Pattern(37, 0x11);
    const std::vector<std::uint8_t> second = Pattern(120, 0x83);

    std::vector<std::uint8_t> stream;
    ASSERT_EQ(AppendFrame(stream, first.data(), first.size()), MOBILEGL_OK);
    ASSERT_EQ(AppendFrame(stream, second.data(), second.size()), MOBILEGL_OK);
    EXPECT_EQ(stream.size(), 2 * kFrameHeaderSize + first.size() + second.size());

    // A stream transport hands over arbitrary fragments; one byte at a time is
    // the worst case and must work.
    FrameReader reader;
    std::vector<std::vector<std::uint8_t>> received;
    for (std::uint8_t byte : stream) {
        ASSERT_EQ(reader.Feed(&byte, 1), MOBILEGL_OK);
        while (reader.HasMessage()) {
            std::vector<std::uint8_t> message;
            ASSERT_EQ(reader.TakeMessage(message), MOBILEGL_OK);
            received.push_back(std::move(message));
        }
    }

    ASSERT_EQ(received.size(), 2u);
    EXPECT_EQ(received[0], first);
    EXPECT_EQ(received[1], second);
    EXPECT_FALSE(reader.Failed());
    EXPECT_EQ(reader.BufferedBytes(), 0u);
}

TEST(FramingTest, MagicIsOnTheWireAsMGLF) {
    std::vector<std::uint8_t> stream;
    const std::uint8_t payload = 0xAB;
    ASSERT_EQ(AppendFrame(stream, &payload, 1), MOBILEGL_OK);
    ASSERT_GE(stream.size(), 4u);
    EXPECT_EQ(stream[0], 'M');
    EXPECT_EQ(stream[1], 'G');
    EXPECT_EQ(stream[2], 'L');
    EXPECT_EQ(stream[3], 'F');
}

TEST(FramingTest, EmptyPayloadRoundTrips) {
    std::vector<std::uint8_t> stream;
    ASSERT_EQ(AppendFrame(stream, nullptr, 0), MOBILEGL_OK);

    FrameReader reader;
    ASSERT_EQ(reader.Feed(stream.data(), stream.size()), MOBILEGL_OK);
    ASSERT_TRUE(reader.HasMessage());
    EXPECT_EQ(reader.PendingMessageSize(), 0u);

    std::vector<std::uint8_t> message{0xFF};
    ASSERT_EQ(reader.TakeMessage(message), MOBILEGL_OK);
    EXPECT_TRUE(message.empty());
}

TEST(FramingTest, BadMagicIsReportedAndLatchesTheReaderDead) {
    std::uint8_t header[8] = {};
    const std::uint32_t wrongMagic = 0xDEADBEEF;
    const std::uint32_t length = 4;
    std::memcpy(header + 0, &wrongMagic, sizeof(wrongMagic));
    std::memcpy(header + 4, &length, sizeof(length));

    FrameReader reader;
    // The failure surfaces at Feed time, not as a message that never arrives.
    EXPECT_EQ(reader.Feed(header, sizeof(header)), MOBILEGL_ERR_PROTOCOL_MISMATCH);
    EXPECT_TRUE(reader.Failed());
    EXPECT_FALSE(reader.HasMessage());

    // And it stays dead: a desynchronized stream is never re-synchronized by
    // feeding it more bytes.
    const std::uint8_t more[4] = {1, 2, 3, 4};
    EXPECT_EQ(reader.Feed(more, sizeof(more)), MOBILEGL_ERR_PROTOCOL_MISMATCH);
    std::vector<std::uint8_t> message;
    EXPECT_EQ(reader.TakeMessage(message), MOBILEGL_ERR_PROTOCOL_MISMATCH);
}

TEST(FramingTest, OversizedLengthIsRejectedBeforeAnyAllocation) {
    std::uint8_t header[8] = {};
    const std::uint32_t magic = kFrameMagic;
    const std::uint32_t length = static_cast<std::uint32_t>(kMaxFramePayloadSize) + 1;
    std::memcpy(header + 0, &magic, sizeof(magic));
    std::memcpy(header + 4, &length, sizeof(length));

    FrameReader reader;
    EXPECT_EQ(reader.Feed(header, sizeof(header)), MOBILEGL_ERR_PROTOCOL_MISMATCH);
    EXPECT_TRUE(reader.Failed());
}

TEST(FramingTest, SendRefusesAPayloadOverTheCap) {
    std::vector<std::uint8_t> stream;
    const std::uint8_t dummy = 0;
    // The size check happens before the payload is touched, so no 64MiB
    // allocation is needed to cover it.
    EXPECT_EQ(AppendFrame(stream, &dummy, kMaxFramePayloadSize + 1), MOBILEGL_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(stream.empty());
}

TEST(FramingTest, BufferTooSmallReportsTheSizeAndKeepsTheMessage) {
    const std::vector<std::uint8_t> payload = Pattern(200, 0x5A);
    std::vector<std::uint8_t> stream;
    ASSERT_EQ(AppendFrame(stream, payload.data(), payload.size()), MOBILEGL_OK);

    FrameReader reader;
    ASSERT_EQ(reader.Feed(stream.data(), stream.size()), MOBILEGL_OK);
    ASSERT_TRUE(reader.HasMessage());

    std::vector<std::uint8_t> small(8);
    std::uint64_t required = 0;
    MobileGLMutableByteSpan smallSpan{small.data(), small.size()};
    EXPECT_EQ(reader.TakeMessage(smallSpan, &required), MOBILEGL_ERR_BUFFER_TOO_SMALL);
    EXPECT_EQ(required, payload.size());

    // Still there. This is the whole point: the old transport dropped it here
    // and the stream never recovered.
    ASSERT_TRUE(reader.HasMessage());

    std::vector<std::uint8_t> big(required);
    std::uint64_t got = 0;
    MobileGLMutableByteSpan bigSpan{big.data(), big.size()};
    ASSERT_EQ(reader.TakeMessage(bigSpan, &got), MOBILEGL_OK);
    EXPECT_EQ(got, payload.size());
    EXPECT_EQ(big, payload);
    EXPECT_FALSE(reader.HasMessage());
}

TEST(FramingTest, TakeWithNoCompleteMessageDoesNotBlockOrCorrupt) {
    const std::vector<std::uint8_t> payload = Pattern(64, 0x22);
    std::vector<std::uint8_t> stream;
    ASSERT_EQ(AppendFrame(stream, payload.data(), payload.size()), MOBILEGL_OK);

    FrameReader reader;
    // Header plus half the payload.
    ASSERT_EQ(reader.Feed(stream.data(), kFrameHeaderSize + 32), MOBILEGL_OK);
    EXPECT_FALSE(reader.HasMessage());
    EXPECT_EQ(reader.PendingMessageSize(), 0u);

    std::vector<std::uint8_t> message;
    EXPECT_EQ(reader.TakeMessage(message), MOBILEGL_ERR_TIMEOUT);

    ASSERT_EQ(reader.Feed(stream.data() + kFrameHeaderSize + 32,
                          stream.size() - kFrameHeaderSize - 32),
              MOBILEGL_OK);
    ASSERT_TRUE(reader.HasMessage());
    ASSERT_EQ(reader.TakeMessage(message), MOBILEGL_OK);
    EXPECT_EQ(message, payload);
}

TEST(FramingTest, ManyMessagesCompactTheBufferInsteadOfGrowing) {
    // Drives the reader past its compaction threshold so the "consumed bytes
    // are reclaimed" path is actually taken.
    const std::vector<std::uint8_t> payload = Pattern(1024, 0x07);
    FrameReader reader;
    for (int i = 0; i < 300; ++i) {
        std::vector<std::uint8_t> stream;
        ASSERT_EQ(AppendFrame(stream, payload.data(), payload.size()), MOBILEGL_OK);
        ASSERT_EQ(reader.Feed(stream.data(), stream.size()), MOBILEGL_OK);
        ASSERT_TRUE(reader.HasMessage());
        std::vector<std::uint8_t> message;
        ASSERT_EQ(reader.TakeMessage(message), MOBILEGL_OK);
        ASSERT_EQ(message, payload);
    }
    EXPECT_EQ(reader.BufferedBytes(), 0u);
}
