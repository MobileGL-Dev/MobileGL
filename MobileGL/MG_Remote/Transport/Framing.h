// MobileGL - MobileGL/MG_Remote/Transport/Framing.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// Control-channel wire framing: [u32 magic 'MGLF'][u32 payloadLength][payload].
// Length excludes the 8-byte header and is capped at 64 MiB.
//
// Two defects of the earlier branch's codec are fixed here, and both are the
// reason this file is not a copy of it:
//
//  1. Its Feed() unconditionally returned OK and its header peek merely
//     returned false on a bad magic or an oversized length. A corrupt or
//     desynchronized stream therefore turned into a silent, permanent hang -
//     the reader kept waiting for a message that could never be parsed, with
//     no error anywhere. Here a violation latches a failed state, is logged at
//     ERROR, and every later call returns MOBILEGL_ERR_PROTOCOL_MISMATCH.
//
//  2. Its receive path failed the call and consumed the message when the
//     caller's buffer was too small, wedging the stream. Here
//     MOBILEGL_ERR_BUFFER_TOO_SMALL reports the required size and KEEPS the
//     message queued.
//
// The reader is a plain byte-stream reassembler: it never assumes a read()
// returned a whole frame.

#pragma once

#include "../Protocol/mg_protocol_base.h"

#include <MG_Util/Debug/Log.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace MobileGL::MG_Remote::Transport {

    // 'MGLF', little-endian on the wire (both ends are the same machine).
    inline constexpr std::uint32_t kFrameMagic = 0x464C474Du;
    inline constexpr std::uint64_t kFrameHeaderSize = 8;
    inline constexpr std::uint64_t kMaxFramePayloadSize = 64ull * 1024 * 1024;

    // Compaction threshold: consumed bytes are dropped from the front once
    // enough of them accumulate, so a long-lived reader neither memmoves per
    // message nor grows without bound.
    inline constexpr std::uint64_t kFrameReaderCompactThreshold = 64ull * 1024;

    // Appends one framed message to `out`.
    inline MobileGLResult AppendFrame(std::vector<std::uint8_t>& out, const void* payload,
                                      std::uint64_t size) {
        if (size > kMaxFramePayloadSize) {
            MGLOG_E("MG_Remote framing: refusing to send a %llu byte payload (cap %llu); bulk "
                    "bytes belong in shm",
                    static_cast<unsigned long long>(size),
                    static_cast<unsigned long long>(kMaxFramePayloadSize));
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        }
        if (size != 0 && payload == nullptr) {
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        }
        std::uint8_t header[kFrameHeaderSize];
        const std::uint32_t magic = kFrameMagic;
        const std::uint32_t length = static_cast<std::uint32_t>(size);
        std::memcpy(header + 0, &magic, sizeof(magic));
        std::memcpy(header + 4, &length, sizeof(length));
        out.insert(out.end(), header, header + kFrameHeaderSize);
        const auto* bytes = static_cast<const std::uint8_t*>(payload);
        out.insert(out.end(), bytes, bytes + size);
        return MOBILEGL_OK;
    }

    // Incremental frame extractor over a raw byte stream.
    class FrameReader {
    public:
        // Feeds raw stream bytes. Validates the frame header the moment enough
        // bytes for one exist - a bad magic or an oversized length is reported
        // here, not swallowed.
        MobileGLResult Feed(const void* data, std::uint64_t size) {
            if (m_failed) {
                return MOBILEGL_ERR_PROTOCOL_MISMATCH;
            }
            if (size != 0) {
                if (data == nullptr) {
                    return MOBILEGL_ERR_INVALID_ARGUMENT;
                }
                const auto* bytes = static_cast<const std::uint8_t*>(data);
                m_buffer.insert(m_buffer.end(), bytes, bytes + size);
            }
            return ParseHeader();
        }

        bool Failed() const { return m_failed; }

        bool HasMessage() const {
            return !m_failed && m_haveHeader && Available() >= kFrameHeaderSize + m_pendingSize;
        }

        // Size of the next complete message, or 0 when none is complete yet.
        std::uint64_t PendingMessageSize() const { return HasMessage() ? m_pendingSize : 0; }

        std::uint64_t BufferedBytes() const { return Available(); }

        // Copies the next complete message out.
        //   MOBILEGL_OK                   - copied, *outSize set, message consumed
        //   MOBILEGL_ERR_BUFFER_TOO_SMALL - *outSize = required size, message KEPT
        //   MOBILEGL_ERR_TIMEOUT          - no complete message buffered
        //   MOBILEGL_ERR_PROTOCOL_MISMATCH- the stream is latched failed
        MobileGLResult TakeMessage(MobileGLMutableByteSpan buffer, std::uint64_t* outSize) {
            if (m_failed) {
                return MOBILEGL_ERR_PROTOCOL_MISMATCH;
            }
            if (!HasMessage()) {
                return MOBILEGL_ERR_TIMEOUT;
            }
            if (outSize != nullptr) {
                *outSize = m_pendingSize;
            }
            if (buffer.size < m_pendingSize) {
                // The message stays queued; the caller retries with a big
                // enough buffer.
                return MOBILEGL_ERR_BUFFER_TOO_SMALL;
            }
            if (m_pendingSize != 0) {
                if (buffer.data == nullptr) {
                    return MOBILEGL_ERR_INVALID_ARGUMENT;
                }
                std::memcpy(buffer.data, m_buffer.data() + m_readPos + kFrameHeaderSize,
                            static_cast<std::size_t>(m_pendingSize));
            }
            Consume();
            return MOBILEGL_OK;
        }

        // Convenience overload that sizes the destination itself.
        MobileGLResult TakeMessage(std::vector<std::uint8_t>& out) {
            if (m_failed) {
                return MOBILEGL_ERR_PROTOCOL_MISMATCH;
            }
            if (!HasMessage()) {
                return MOBILEGL_ERR_TIMEOUT;
            }
            const auto* first = m_buffer.data() + m_readPos + kFrameHeaderSize;
            out.assign(first, first + m_pendingSize);
            Consume();
            return MOBILEGL_OK;
        }

    private:
        std::uint64_t Available() const { return m_buffer.size() - m_readPos; }

        MobileGLResult ParseHeader() {
            if (m_haveHeader || Available() < kFrameHeaderSize) {
                return MOBILEGL_OK;
            }
            std::uint32_t magic = 0;
            std::uint32_t length = 0;
            std::memcpy(&magic, m_buffer.data() + m_readPos, sizeof(magic));
            std::memcpy(&length, m_buffer.data() + m_readPos + 4, sizeof(length));
            if (magic != kFrameMagic) {
                m_failed = true;
                MGLOG_E("MG_Remote framing: bad frame magic 0x%08X (expected 0x%08X); the control "
                        "stream is desynchronized and this transport is now dead",
                        magic, kFrameMagic);
                return MOBILEGL_ERR_PROTOCOL_MISMATCH;
            }
            if (length > kMaxFramePayloadSize) {
                m_failed = true;
                MGLOG_E("MG_Remote framing: frame length %u exceeds the %llu byte cap; refusing to "
                        "allocate on a peer-supplied length",
                        length, static_cast<unsigned long long>(kMaxFramePayloadSize));
                return MOBILEGL_ERR_PROTOCOL_MISMATCH;
            }
            m_pendingSize = length;
            m_haveHeader = true;
            return MOBILEGL_OK;
        }

        void Consume() {
            m_readPos += kFrameHeaderSize + m_pendingSize;
            m_pendingSize = 0;
            m_haveHeader = false;
            if (m_readPos == m_buffer.size()) {
                m_buffer.clear();
                m_readPos = 0;
            } else if (m_readPos >= kFrameReaderCompactThreshold) {
                m_buffer.erase(m_buffer.begin(),
                               m_buffer.begin() + static_cast<std::ptrdiff_t>(m_readPos));
                m_readPos = 0;
            }
            // Header of the next message may already be buffered.
            (void)ParseHeader();
        }

        std::vector<std::uint8_t> m_buffer;
        std::uint64_t m_readPos = 0;
        std::uint64_t m_pendingSize = 0;
        bool m_haveHeader = false;
        bool m_failed = false;
    };

} // namespace MobileGL::MG_Remote::Transport
