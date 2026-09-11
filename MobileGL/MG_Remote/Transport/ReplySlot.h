// MobileGL - MobileGL/MG_Remote/Transport/ReplySlot.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// SEG_REPLY: the slot pool the server writes a kReplySlot answer into, and the
// client reads it back out of. Owner: package s1.
//
// THE ID IS THE RECORD SEQUENCE NUMBER (P5 R-3). There is no second id space and
// no allocator: the ten kReplySlot calls carry no MGPReplySlot in their payloads
// (that is why the id has to be DERIVED rather than carried), the wire has no
// per-record seq field (ARCHITECTURE.md:124), so the record's ordinal IS its
// reply-slot id. The slot is addressed `seq % slotCount` and the server STAMPS
// THE SEQ BACK INTO THE SLOT HEADER, which is what makes a wrong-slot read
// detectable rather than merely plausible. Seq is 1-based; 0 means "no record".
//
// THE SLOT HEADER IS CONTRACT-P5 TABLE 0's ROW, verbatim:
//     { Uint64 Seq; Int32 Status; Uint32 Size; }   // 16 bytes, then the payload
//     Status: 0 = OK, 1 = DECLINED, 2 = ERROR
//
// DECLINED IS A REAL ANSWER, NOT A FAILURE. It is how MapPersistent says nullptr
// (R-6) and how the four Bool acceptance entry points - ResourceCreate,
// ResourceRespecify, ResourceSubData, SetTextureParams - say false (R-5). A
// client that folds DECLINED into "the call failed" re-creates ID-39's 66 lost
// uploads from the other side, and a client that folds it into OK accepts a
// pointer the server never handed out.
//
// A REPLY LARGER THAN ONE SLOT IS FATAL, NOT CHUNKED. P5's only large answer is
// ReadPixels, and the client knows its size before it emits the record, so an
// overflow means the two sides disagree about the frame rather than that the
// pool is too small. Chunking is P8's; growing the pool is an operator's.
//
// ORDERING. The client only looks at a slot after it has seen
// RingControl::appliedSeq >= its own seq with an ACQUIRE load, and the server
// advances appliedSeq with a RELEASE store AFTER posting the reply
// (PipeApplier::ApplyOne's order: decode -> stamp -> apply -> post -> advance).
// That pair is what publishes the slot's bytes; the fences below are the
// belt-and-braces for a caller - a unit test, or P9's async pool - that reads a
// slot without going through appliedSeq first.

#pragma once

#include "WireLog.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace MobileGL::MG_Remote::Transport {

    // CONTRACT-P5 table 0, "reply slot header".
    struct ReplySlotHeader {
        std::uint64_t Seq;   // the record ordinal, stamped back for self-check
        std::int32_t Status; // ReplyStatus
        std::uint32_t Size;  // payload bytes following this header
    };
    static_assert(sizeof(ReplySlotHeader) == 16, "the reply slot header is 16 bytes on the wire");
    static_assert(alignof(ReplySlotHeader) == 8, "the reply slot header must not gain padding");

    enum ReplyStatus : std::int32_t {
        kReplyStatusOk = 0,
        // Not an error. MapPersistent's nullptr and the four Bool acceptance
        // returns' `false` both arrive as this.
        kReplyStatusDeclined = 1,
        kReplyStatusError = 2,
    };

    // Eight slots of a 8 MiB SEG_REPLY is 1 MiB per answer.
    //
    // Why eight and not sixty-four: while the verb barrier holds (R-1) the client
    // blocks at every verb boundary, so the in-flight depth is exactly ONE and
    // every extra slot buys nothing but a smaller maximum answer. The trade is
    // the other way round - fewer slots, bigger replies - and P5's only large
    // answer is a blocking ReadPixels. 1 MiB covers a 512x512 RGBA8 read. P9,
    // which is what makes the pool asynchronous, re-chooses this geometry with
    // real depth to size it against.
    inline constexpr std::uint32_t kDefaultReplySlotCount = 8;

    // Slot 0 exists and is used: seq is 1-based, so seq % slotCount hits slot 0
    // on seq == slotCount, not on "no record".
    class ReplySlotPool {
    public:
        ReplySlotPool() = default;

        // `base`/`sizeBytes` are SEG_REPLY's mapping. `slotCount` must be a power
        // of two - the addressing is a mask, and a non-power-of-two modulus on
        // the apply thread is a division in the reply path of every blocking
        // call. Anything else leaves Valid() false rather than half-working.
        ReplySlotPool(void* base, std::uint64_t sizeBytes, std::uint32_t slotCount) {
            if (base == nullptr || slotCount == 0 || (slotCount & (slotCount - 1)) != 0) {
                WireLogError("MG_Remote reply pool: rejected, slotCount %u must be a non-zero power "
                             "of two over a non-null mapping",
                             static_cast<unsigned>(slotCount));
                return;
            }
            const std::uint64_t slotBytes = sizeBytes / slotCount;
            if (slotBytes <= sizeof(ReplySlotHeader)) {
                WireLogError("MG_Remote reply pool: rejected, %llu bytes over %u slots leaves no "
                             "room for a payload past the %llu byte slot header",
                             static_cast<unsigned long long>(sizeBytes),
                             static_cast<unsigned>(slotCount),
                             static_cast<unsigned long long>(sizeof(ReplySlotHeader)));
                return;
            }
            m_base = static_cast<std::uint8_t*>(base);
            m_size = sizeBytes;
            m_slots = slotCount;
            m_mask = slotCount - 1;
            // Truncated to 32 bits deliberately: the header's Size field is
            // 32-bit, so a slot no 32-bit count could describe would let a
            // legal-looking Size name bytes past the slot.
            m_slotBytes = slotBytes > 0xFFFFFFFFull ? 0xFFFFFFFFu
                                                    : static_cast<std::uint32_t>(slotBytes);
        }

        bool Valid() const { return m_base != nullptr; }
        std::uint32_t SlotCount() const { return m_slots; }
        std::uint32_t SlotBytes() const { return m_slotBytes; }
        // What a single answer may carry. The client checks against this BEFORE
        // it emits a ReadPixels, which is the whole reason a fixed slot size is
        // legitimate rather than a guess.
        std::uint32_t MaxReplyBytes() const {
            return m_slotBytes == 0 ? 0u
                                    : m_slotBytes - static_cast<std::uint32_t>(sizeof(ReplySlotHeader));
        }

        // Zeroes every header, so a stale seq from a previous session cannot be
        // mistaken for this session's answer. Called on the server side at Accept.
        void Clear() {
            if (m_base == nullptr) {
                return;
            }
            for (std::uint32_t slot = 0; slot < m_slots; ++slot) {
                ReplySlotHeader header{};
                std::memcpy(m_base + static_cast<std::uint64_t>(slot) * m_slotBytes, &header,
                            sizeof(header));
            }
        }

        // Server side. `size` bytes of `bytes` become the answer for `seq`.
        // An answer larger than one slot is FATAL, never truncated and never
        // chunked - see the file header.
        void Post(std::uint64_t seq, std::int32_t status, const void* bytes, std::uint64_t size) {
            if (m_base == nullptr) {
                WireLogError("MG_Remote reply pool: Post(seq=%llu) on an unconfigured pool",
                             static_cast<unsigned long long>(seq));
                std::abort();
            }
            if (seq == 0) {
                WireLogError("MG_Remote reply pool: seq 0 is \"no record\" and can never name a "
                             "slot (R-3: seq is 1-based)");
                std::abort();
            }
            if (size > MaxReplyBytes()) {
                WireLogError("MG_Remote reply pool: Fatal{ProtocolCorruption} - a %llu byte answer "
                             "for seq %llu does not fit a %u byte slot (payload cap %u). P5 does "
                             "not chunk replies: the client knows an answer's size before it emits "
                             "the record, so this means the two sides disagree about the frame",
                             static_cast<unsigned long long>(size),
                             static_cast<unsigned long long>(seq), static_cast<unsigned>(m_slotBytes),
                             static_cast<unsigned>(MaxReplyBytes()));
                std::abort();
            }
            std::uint8_t* slot = SlotAt(seq);
            if (size != 0 && bytes != nullptr) {
                std::memcpy(slot + sizeof(ReplySlotHeader), bytes, static_cast<std::size_t>(size));
            }
            ReplySlotHeader header{};
            header.Seq = seq;
            header.Status = status;
            header.Size = static_cast<std::uint32_t>(size);
            // The payload must be visible before the stamp that says it is there.
            std::atomic_thread_fence(std::memory_order_release);
            std::memcpy(slot, &header, sizeof(header));
        }

        // Client side. Returns false when the slot does not carry THIS seq - the
        // self-check the stamp exists for. `outBytes` may be null for an answer
        // with no payload (every DECLINE, and the four Bool acceptances).
        //
        // A payload larger than the caller's buffer is a caller bug rather than a
        // wire fault (the caller sized it from the call it made), so it returns
        // false with *outSize set to what was there, the ReceiveFrame shape.
        bool Read(std::uint64_t seq, void* outBytes, std::uint64_t outCapacity,
                  std::int32_t* outStatus, std::uint64_t* outSize) const {
            if (outStatus != nullptr) {
                *outStatus = kReplyStatusError;
            }
            if (outSize != nullptr) {
                *outSize = 0;
            }
            if (m_base == nullptr || seq == 0) {
                return false;
            }
            const std::uint8_t* slot = SlotAt(seq);
            ReplySlotHeader header{};
            std::memcpy(&header, slot, sizeof(header));
            std::atomic_thread_fence(std::memory_order_acquire);
            if (header.Seq != seq) {
                // Not "retry": under the verb barrier the answer is already
                // there by the time appliedSeq passed this record, so a stamp
                // that disagrees is a drifted sequence space (R-9's kRecPad
                // rule) or a wrong-slot read, and both are faults.
                WireLogError("MG_Remote reply pool: slot %llu carries seq %llu, not %llu - the two "
                             "sides' sequence spaces have drifted (a counted kRecPad, R-9) or the "
                             "addressing disagrees",
                             static_cast<unsigned long long>(seq & m_mask),
                             static_cast<unsigned long long>(header.Seq),
                             static_cast<unsigned long long>(seq));
                return false;
            }
            if (header.Size > MaxReplyBytes()) {
                WireLogError("MG_Remote reply pool: slot for seq %llu declares %u payload bytes in "
                             "a %u byte slot",
                             static_cast<unsigned long long>(seq), header.Size,
                             static_cast<unsigned>(m_slotBytes));
                return false;
            }
            if (outStatus != nullptr) {
                *outStatus = header.Status;
            }
            if (outSize != nullptr) {
                *outSize = header.Size;
            }
            if (header.Size == 0) {
                return true;
            }
            if (outBytes == nullptr || outCapacity < header.Size) {
                return false;
            }
            std::memcpy(outBytes, slot + sizeof(ReplySlotHeader), header.Size);
            return true;
        }

    private:
        std::uint8_t* SlotAt(std::uint64_t seq) const {
            return m_base + (seq & m_mask) * static_cast<std::uint64_t>(m_slotBytes);
        }

        std::uint8_t* m_base = nullptr;
        std::uint64_t m_size = 0;
        std::uint32_t m_slots = 0;
        std::uint32_t m_mask = 0;
        std::uint32_t m_slotBytes = 0;
    };

} // namespace MobileGL::MG_Remote::Transport
