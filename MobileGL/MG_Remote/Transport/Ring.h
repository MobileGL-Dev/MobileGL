// MobileGL - MobileGL/MG_Remote/Transport/Ring.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// RingControl, the shared page at the head of SEG_CMD, and the SPSC producer/consumer over it
// (ARCHITECTURE.md §11.2). Cursors are monotonic byte counts indexed with a power-of-two mask and
// never reset, so a torn read cannot look like an earlier position; ringGeneration is bumped after
// a hard drain. Each ring has two tails because a kRecBorrowSlot record retires only after
// completedFrameSerial; both park flags exist because the doorbell rings in both directions.
// The five watermarks (ARCHITECTURE.md §11.2.1, SessionRings.h's Watermark) are waited on with
// `>=` and may be published late, never early; appliedSeq advances once per applied record.
// kRecPad wrap fillers do not advance seq, the reply-slot id: both sides skip them before counting.

#pragma once

#include "../Protocol/mg_protocol_base.h"
#include "ILink.h" // LinkProgress: the four consumer-written watermarks, lk

#include <atomic>
#include <cstddef>
#include <type_traits>
#include <cstdint>

namespace MobileGL::MG_Remote::Transport {

    // The shared control page. One 4 KiB page so it can be mapped alone, with
    // each contended group on its own cache line.
    struct alignas(4096) RingControl {
        // ---- SEG_CMD cursors ------------------------------------------------
        //
        // cmdHead and submittedSeq share a line because they share a WRITER:
        // the producer publishes the head and then stamps how far it has got.
        // submittedSeq used to sit with the four consumer-written watermarks,
        // which made that group mixed-writer and unaliasable (lk, §8.3).
        alignas(64) std::atomic<std::uint64_t> cmdHead;        // producer: bytes written
        std::atomic<std::uint64_t> submittedSeq;               // producer: records published
        alignas(64) std::atomic<std::uint64_t> cmdAppliedTail; // consumer: bytes decoded/copied out
        std::atomic<std::uint64_t> cmdRetiredTail;             // consumer: borrowed slots released

        // ---- SEG_STAGE cursors ------------------------------------------------
        //
        // Unused: staging is the encoder's linear allocator, which reclaims on retiredSeq, so these
        // stay zero and RingCursorSet::Stage has no producer or consumer (pinned by
        // SessionTest.TheStageCursorTripleStaysDeadAcrossAWholeSession). They keep the page layout
        // for a future ring-shaped segment. Never publish stageHead without a consumer advancing
        // both tails: FreeBytes() would reach zero after the first lap and the producer would hang.
        alignas(64) std::atomic<std::uint64_t> stageHead;
        alignas(64) std::atomic<std::uint64_t> stageAppliedTail;
        std::atomic<std::uint64_t> stageRetiredTail;

        // ---- sequence / frame watermarks -------------------------------------
        //
        // THE CONSUMER-WRITTEN FOUR, AND ONLY THOSE. This is Transport::LinkProgress
        // (ILink.h), named so that a data plane with no shared page can own the
        // same four values and every wait predicate in SessionRings.h keeps ONE
        // implementation at source level. Single-writer by construction now that
        // submittedSeq has moved up to the producer's line; the two event flags
        // stay in the doorbell group below, because the client clears
        // eventRingFull with an RMW on every drain and that traffic may not land
        // on the line the client also spins on.
        alignas(64) LinkProgress Progress;

        // ---- doorbell / generation -------------------------------------------
        alignas(64) std::atomic<std::uint32_t> serverEpoch; // ++ on context loss / server restart
        std::atomic<std::uint32_t> ringGeneration;          // ++ after a hard drain
        std::atomic<std::uint32_t> consumerParked;          // server asleep, producer must ring
        std::atomic<std::uint32_t> producerParked;          // client asleep, server must ring
        std::atomic<std::uint32_t> eventRingFull;           // SEG_EVENT full, server stopped applying
        std::atomic<std::uint32_t> eventDropped;            // dropped lossy events

        // ---- the parked producer's wake target -------------------------------
        //
        // What a parked client is waiting for, so the server rings only when it is there: 0 =
        // any progress (present credit, ring space, events), otherwise the appliedSeq it needs -
        // or, with bit 63 set, the present-ack serial a present-credit wait needs (Ring.cpp).
        // Without it every record the server applied while the client waited for a later one
        // was a socket write and a futile wake-up. Written by the client before it parks (and
        // reset to 0 after), read by the server only after it saw producerParked set; on a link
        // without this shared page it stays 0, which is the old ring-on-any-progress rule.
        alignas(64) std::atomic<std::uint64_t> producerWakeSeq;
    };

    static_assert(sizeof(RingControl) == 4096, "RingControl must be exactly one page");
    // lk (CONTRACT-P6 §8.3). The regroup is free while both peers are the same binary
    // and a LAYOUT BREAK the day they are not, so the shape is pinned here - by asserts
    // that can each go red for the reason they exist, which the first revision of this
    // block did not manage.
    //
    // NOTHING ELSE IN THE TREE PINS ANY OF THIS. The ABI fingerprint
    // (SessionRings.h's AbiFingerprintInputs) takes three sizeofs, the caps geometry, two
    // codec versions, kOpCount, the protocol version and the build stamp - and NO input
    // from this page at all. sizeof(RingControl) is 4096 whatever the members do, because
    // alignas(4096) makes it so. So a silent field reorder here would leave every gate in
    // the tree green and both peers hashing identically.
    static_assert(std::is_standard_layout_v<RingControl> && std::is_standard_layout_v<LinkProgress>,
                  "the offsetof gates below are defined only for standard-layout types");

    // The producer's line: cmdHead and submittedSeq, together and nothing else waiting on them.
    static_assert(offsetof(RingControl, submittedSeq) == offsetof(RingControl, cmdHead) + 8,
                  "submittedSeq sits immediately after cmdHead");
    static_assert(offsetof(RingControl, submittedSeq) / 64 == offsetof(RingControl, cmdHead) / 64,
                  "submittedSeq shares the producer's cache line with cmdHead");

    // The Progress line, and it owns that line ALONE.
    //
    // Written as `serverEpoch == Progress + 64` rather than the obvious
    // `Progress + sizeof(LinkProgress) <= offsetof(serverEpoch)`, because the obvious one
    // CANNOT GO RED: serverEpoch is alignas(64) and is declared next, so the compiler puts
    // it at the following 64-multiple no matter what precedes it, and the `<=` holds for
    // every layout including a wrong one.
    //
    // WHAT THIS PAIR DOES NOT CATCH, stated because a gate nobody can falsify is worse
    // than no gate: a SMALL member inserted between Progress and serverEpoch lands at
    // offset 288, inside Progress's own line - and serverEpoch's alignas(64) absorbs it,
    // so both asserts still pass. Verified by trying it. Up to 32 bytes can be hidden
    // there. Growth INSIDE LinkProgress is caught, by sizeof below; growth BESIDE it is
    // not, and the only thing standing between that and a false-sharing regression is
    // this comment and the group's own.
    static_assert(offsetof(RingControl, Progress) % 64 == 0, "LinkProgress starts a cache line of its own");
    static_assert(offsetof(RingControl, serverEpoch) == offsetof(RingControl, Progress) + 64,
                  "the Progress line holds Progress and nothing else");

    // PER FIELD, which is what §8.3 asks for and what the aggregate asserts above cannot
    // do: the field ORDER inside LinkProgress is a wire-visible fact about a page both
    // roles map, and swapping two of them passes every other check in this file.
    static_assert(offsetof(LinkProgress, appliedSeq) == 0, "LinkProgress field order: appliedSeq");
    static_assert(offsetof(LinkProgress, retiredSeq) == 8, "LinkProgress field order: retiredSeq");
    static_assert(offsetof(LinkProgress, completedFrameSerial) == 16, "LinkProgress field order: completedFrameSerial");
    static_assert(offsetof(LinkProgress, presentAckSerial) == 24, "LinkProgress field order: presentAckSerial");
    static_assert(sizeof(LinkProgress) == 32, "four consumer-written watermarks; the event flags are NOT in here");

    // The two event flags stay in the doorbell group on purpose (see the group's comment).
    // LinkEventFlags mirrors them, so the mirror's shape is pinned too.
    static_assert(offsetof(RingControl, eventDropped) == offsetof(RingControl, eventRingFull) + 4,
                  "the two event flags are adjacent, in that order");
    static_assert(offsetof(RingControl, eventRingFull) / 64 == offsetof(RingControl, serverEpoch) / 64,
                  "the event flags share the doorbell group's line, NOT the Progress line");
    static_assert(offsetof(LinkEventFlags, eventRingFull) == 0 && offsetof(LinkEventFlags, eventDropped) == 4 &&
                      sizeof(LinkEventFlags) == 8,
                  "LinkEventFlags mirrors the pair in RingControl");

    // The page's real content must fit the page it claims. sizeof(RingControl) == 4096
    // above cannot catch a member that grew, because alignas(4096) forces that number;
    // the struct's content ends far short of it and the tail is padding.
    static_assert(offsetof(RingControl, producerWakeSeq) / 64 != offsetof(RingControl, serverEpoch) / 64,
                  "the wake target has a line of its own");
    static_assert(offsetof(RingControl, producerWakeSeq) + sizeof(std::uint64_t) <= 4096,
                  "RingControl's members must fit the page it claims to be");
    static_assert(offsetof(RingControl, eventDropped) + sizeof(std::uint32_t) <= 4096,
                  "RingControl's members must fit the page it claims to be");
    static_assert(alignof(RingControl) == 4096, "RingControl must be page aligned");
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                  "the ring cursors are shared across processes: they must be lock-free");
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
                  "the doorbell flags are shared across processes: they must be lock-free");

    // Per-record header. Prefix-identical to the plan's RecHeader so the
    // generated record catalog drops straight in.
    struct RingRecordHeader {
        std::uint16_t kind;
        std::uint16_t flags;
        std::uint32_t size; // header + payload + alignment padding, multiple of 8
    };
    static_assert(sizeof(RingRecordHeader) == 8, "RecHeader is 8 bytes on the wire");

    // Ring framing flags. MGPipeCallFlags (MG_Pipe/MGPipe.h) is a separate space in the same bits:
    // kVarTail == kRecPad, kHostSpan == kRecBorrowSlot, kReplySlot == kRecVarTail. The encoder
    // translates call flags and never copies them (PipeWireCodec.cpp's FlagSpace asserts). As a
    // backstop, Pop skips a record as a wrap filler only if it carries kRecPad and
    // kind == kRingPadRecordKind, and SessionConsumer names every kRecBorrowSlot it sees, since
    // nothing produces borrowed slots.
    enum RingRecordFlags : std::uint16_t {
        kRecNone = 0,
        kRecNeedsAck = 1u << 0,
        kRecHasBlob = 1u << 1,
        kRecPad = 1u << 2,        // filler to the wrap boundary, no payload meaning
        kRecBorrowSlot = 1u << 3, // slot is borrowed into the GPU timeline; retires late
        kRecVarTail = 1u << 4,
        // P12: THE PEER NEED NOT ANSWER THIS RECORD. Set by the producer for a record whose
        // answer NOBODY WILL EVER READ - not "whose caller is not waiting". The distinction is
        // the whole point: the create window's records are emitted without a wait and their
        // answers are read later by the drain, so they must NOT carry this bit.
        //
        // IT ALIASES kOptional (1<<5) IN THE CALL-FLAG SPACE, and that is harmless for the same
        // reason the other five aliases are: the two spaces share one 16-bit field and are kept
        // apart by the ENCODER'S TRANSLATION, never by stamping (PipeWireCodec.cpp's FlagSpace
        // block names each pair and asserts them). kOptional is in kDropped - it never reaches
        // this field - so nothing can set this bit by accident.
        kRecNoReply = 1u << 5,
    };

    // Reserved kind for the wrap filler. The catalog starts at 1.
    inline constexpr std::uint16_t kRingPadRecordKind = 0;

    inline constexpr std::uint64_t kRingRecordAlignment = 8;

    // Largest ring the 8-byte header can describe. Both a record's size and a
    // wrap filler's size are bounded only by the capacity and are stored in
    // RingRecordHeader::size, which is 32 bits by wire contract: a ring of
    // 4 GiB or more would silently truncate them, and the consumer would then
    // bounds-check the truncated value against the real one. SEG_CMD is 8 MiB
    // and SEG_STAGE 32 MiB today, so this is unreachable - it is the same
    // class of construction-time guard as the power-of-two check beside it.
    inline constexpr std::uint64_t kMaxRingCapacity = 0xFFFFFFFFull;

    // Smallest ring: two record headers. A record may be at most HALF the ring
    // (see RingProducer::Reserve), so a ring of one header could carry nothing
    // at all - not even the smallest record, a bare header.
    inline constexpr std::uint64_t kMinRingCapacity = 2 * sizeof(RingRecordHeader);

    // Which cursor triple a producer/consumer pair drives.
    enum class RingCursorSet : std::uint32_t {
        Cmd = 0,
        Stage = 1,
    };

    // Zeroes every cursor and starts serverEpoch / ringGeneration at 1, so that
    // a zero read is always "uninitialized", never a legal generation.
    void InitRingControl(RingControl& control);

    // head >= appliedTail >= retiredTail, and the ring never holds more than
    // its capacity. False means the shared page is corrupt (or a peer is
    // misbehaving), which is a Fatal{ProtocolCorruption}, never a retry.
    bool RingCursorsValid(const RingControl& control, RingCursorSet cursors, std::uint64_t capacityBytes);

    // Bumps ringGeneration, invalidating every offset either side has cached.
    // Both sides must be quiesced and the ring fully drained
    // (head == appliedTail == retiredTail); otherwise this returns
    // MOBILEGL_ERR_INVALID_ARGUMENT and changes nothing.
    MobileGLResult HardDrainRing(RingControl& control, RingCursorSet cursors);

    // A record as seen by the consumer.
    struct RingRecordView {
        std::uint16_t kind = 0;
        std::uint16_t flags = 0;
        const void* payload = nullptr;
        std::uint64_t payloadSize = 0;
        std::uint64_t cursor = 0; // producer cursor at the START of this record
    };

    // Single producer. Not thread-safe: one writer thread, by construction.
    class RingProducer {
    public:
        RingProducer() = default;
        // `base` is the ring's byte area (NOT the control page) and
        // `capacityBytes` must be a power of two between kMinRingCapacity and
        // kMaxRingCapacity. Anything else leaves Valid() false.
        RingProducer(RingControl* control, void* base, std::uint64_t capacityBytes, RingCursorSet cursors);

        bool Valid() const { return m_control != nullptr; }

        // Bytes still writable before the consumer has to catch up.
        std::uint64_t FreeBytes() const;

        // Reserves room for one record and returns a pointer to its payload, or nullptr when the
        // ring is full. The payload and its tail padding are uninitialized. A record that would
        // straddle the wrap boundary is preceded by a pad record, so every record is contiguous.
        //
        // A record whose total (header + payload, rounded up to 8) exceeds MaxRecordBytes() ==
        // Capacity()/2 is refused with an error log however empty the ring is; the emitter chunks
        // it. Half is the largest total every head offset can place (a wrap pad adds at most
        // total-8 bytes), so nullptr with FreeBytes() >= total means "too big", never "wait".
        void* Reserve(std::uint16_t kind, std::uint16_t flags, std::uint64_t payloadBytes);

        // The largest header+payload total Reserve accepts: Capacity()/2. This
        // is the number the emitter chunks against.
        std::uint64_t MaxRecordBytes() const { return m_capacity / 2; }

        // Makes every reserved record visible to the consumer (release store on
        // the head cursor). Cheap: publishing per record is fine, batching 8-16
        // only amortizes the doorbell store.
        void Publish();

        // Producer-local cursor including records not yet published.
        std::uint64_t LocalHead() const { return m_localHead; }
        std::uint64_t Capacity() const { return m_capacity; }

    private:
        std::uint64_t TailForReclaim() const;
        std::uint8_t* SlotAt(std::uint64_t cursor) const { return m_base + static_cast<std::size_t>(cursor & m_mask); }

        RingControl* m_control = nullptr;
        std::uint8_t* m_base = nullptr;
        std::uint64_t m_capacity = 0;
        std::uint64_t m_mask = 0;
        std::uint64_t m_localHead = 0;
        RingCursorSet m_cursors = RingCursorSet::Cmd;
    };

    // Single consumer. Not thread-safe: one reader thread, by construction.
    class RingConsumer {
    public:
        RingConsumer() = default;
        RingConsumer(RingControl* control, void* base, std::uint64_t capacityBytes, RingCursorSet cursors);

        bool Valid() const { return m_control != nullptr; }

        // Pops the next record, skipping wrap fillers. Returns false when the
        // ring is empty at this moment. A record whose header is impossible
        // (size not 8-aligned, smaller than a header, or larger than what the
        // producer has published) is refused: *outCorrupt is set, which the
        // caller must escalate to Fatal{ProtocolCorruption} rather than retry.
        bool Pop(RingRecordView& out, bool* outCorrupt = nullptr);

        // Publishes the applied cursor, releasing those bytes to the producer.
        void PublishApplied();
        // Publishes the retired cursor. Records without kRecBorrowSlot retire
        // as soon as they are applied; borrowed slots retire on
        // completedFrameSerial, which is why this is a separate call.
        void PublishRetired();
        void PublishRetiredUpTo(std::uint64_t cursor);

        std::uint64_t LocalTail() const { return m_localTail; }
        std::uint64_t Capacity() const { return m_capacity; }

    private:
        RingControl* m_control = nullptr;
        const std::uint8_t* m_base = nullptr;
        std::uint64_t m_capacity = 0;
        std::uint64_t m_mask = 0;
        std::uint64_t m_localTail = 0;
        RingCursorSet m_cursors = RingCursorSet::Cmd;
    };

} // namespace MobileGL::MG_Remote::Transport
