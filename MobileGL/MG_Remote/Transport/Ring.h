// MobileGL - MobileGL/MG_Remote/Transport/Ring.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// SEG_CMD / SEG_STAGE ring control and the SPSC producer/consumer over it.
//
// RingControl is the shared page at the head of SEG_CMD, laid out exactly as
// the inherited transport design (plan section 8.1, referring the earlier
// plan's section 6.2) specifies:
//
//   - TWO independent cursor triples, one for SEG_CMD and one for SEG_STAGE.
//     The stage ring needs its own because "SEG_STAGE has less than a quarter
//     left" is a publish trigger and that occupancy cannot be derived from the
//     command ring's cursors, and because a stage slot retires on a different
//     event than a command record does.
//   - THREE separate sequence watermarks. Conflating them is the classic bug:
//     appliedSeq releases *AppliedTail, submittedSeq releases staging,
//     retiredSeq / completedFrameSerial release *RetiredTail and adopted
//     stores.
//   - TWO tails per ring, not one. Once the server borrows a ring slot into
//     the GPU timeline instead of copying it out again, that slot can only be
//     recycled after completedFrameSerial; a single tail would silently
//     degrade to conservative reclaim the day borrowing lands.
//   - Both park flags, because the doorbell is bidirectional: without the
//     server->client direction every client wait degenerates into a
//     cross-process spin on one shared cache line (a whole 16.6ms frame of a
//     big core, on a phone, competing with the GPU and the game's JVM).
//
// Cursors are monotonically increasing byte counts; the ring is indexed with a
// power-of-two mask. They are never reset, so a torn read can never look like
// a valid earlier position. ringGeneration is bumped after a hard drain to
// invalidate every cached offset.
//
// Record framing inside the ring is the 8-byte header below, which is the
// layout the plan's RecHeader already fixes ({u16 kind, u16 flags, u32 size},
// size including the header and a multiple of 8). The record CATALOGUE
// (Records.def / PipeCalls.def) is a separate deliverable; the ring itself
// only needs kind/flags/size, so it can carry the real records the day they
// land without changing shape.

#pragma once

#include "../Protocol/mg_protocol_base.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace MobileGL::MG_Remote::Transport {

    // The shared control page. One 4 KiB page so it can be mapped alone, with
    // each contended group on its own cache line.
    struct alignas(4096) RingControl {
        // ---- SEG_CMD cursors ------------------------------------------------
        alignas(64) std::atomic<std::uint64_t> cmdHead;        // producer: bytes written
        alignas(64) std::atomic<std::uint64_t> cmdAppliedTail; // consumer: bytes decoded/copied out
        std::atomic<std::uint64_t> cmdRetiredTail;             // consumer: borrowed slots released

        // ---- SEG_STAGE cursors ----------------------------------------------
        alignas(64) std::atomic<std::uint64_t> stageHead;
        alignas(64) std::atomic<std::uint64_t> stageAppliedTail;
        std::atomic<std::uint64_t> stageRetiredTail;

        // ---- sequence / frame watermarks -------------------------------------
        alignas(64) std::atomic<std::uint64_t> appliedSeq; // records applied
        std::atomic<std::uint64_t> submittedSeq;           // handed to the driver
        std::atomic<std::uint64_t> retiredSeq;             // GPU finished
        std::atomic<std::uint64_t> completedFrameSerial;
        std::atomic<std::uint64_t> presentAckSerial;

        // ---- doorbell / generation -------------------------------------------
        alignas(64) std::atomic<std::uint32_t> serverEpoch;    // ++ on context loss / server restart
        std::atomic<std::uint32_t> ringGeneration;             // ++ after a hard drain
        std::atomic<std::uint32_t> consumerParked;             // server asleep, producer must ring
        std::atomic<std::uint32_t> producerParked;             // client asleep, server must ring
        std::atomic<std::uint32_t> eventRingFull;              // SEG_EVENT full, server stopped applying
        std::atomic<std::uint32_t> eventDropped;               // dropped lossy events
    };

    static_assert(sizeof(RingControl) == 4096, "RingControl must be exactly one page");
    static_assert(alignof(RingControl) == 4096, "RingControl must be page aligned");
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                  "the ring cursors are shared across processes: they must be lock-free");
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
                  "the doorbell flags are shared across processes: they must be lock-free");

    // Per-record header. Prefix-identical to the plan's RecHeader so the
    // generated record catalogue drops straight in.
    struct RingRecordHeader {
        std::uint16_t kind;
        std::uint16_t flags;
        std::uint32_t size; // header + payload + alignment padding, multiple of 8
    };
    static_assert(sizeof(RingRecordHeader) == 8, "RecHeader is 8 bytes on the wire");

    enum RingRecordFlags : std::uint16_t {
        kRecNone = 0,
        kRecNeedsAck = 1u << 0,
        kRecHasBlob = 1u << 1,
        kRecPad = 1u << 2,       // filler to the wrap boundary, no payload meaning
        kRecBorrowSlot = 1u << 3, // slot is borrowed into the GPU timeline; retires late
        kRecVarTail = 1u << 4,
    };

    // Reserved kind for the wrap filler. The catalogue starts at 1.
    inline constexpr std::uint16_t kRingPadRecordKind = 0;

    inline constexpr std::uint64_t kRingRecordAlignment = 8;

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
    bool RingCursorsValid(const RingControl& control, RingCursorSet cursors,
                          std::uint64_t capacityBytes);

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
        // `capacityBytes` must be a power of two.
        RingProducer(RingControl* control, void* base, std::uint64_t capacityBytes,
                     RingCursorSet cursors);

        bool Valid() const { return m_control != nullptr; }

        // Bytes still writable before the consumer has to catch up.
        std::uint64_t FreeBytes() const;

        // Reserves room for one record and returns a pointer to its payload,
        // or nullptr when the ring is full (or the record cannot fit at all).
        // The payload is uninitialized; alignment padding at its tail is NOT
        // zeroed. Emits a pad record automatically when the record would
        // straddle the wrap boundary, so every record is contiguous.
        void* Reserve(std::uint16_t kind, std::uint16_t flags, std::uint64_t payloadBytes);

        // Makes every reserved record visible to the consumer (release store on
        // the head cursor). Cheap: publishing per record is fine, batching 8-16
        // only amortizes the doorbell store.
        void Publish();

        // Producer-local cursor including records not yet published.
        std::uint64_t LocalHead() const { return m_localHead; }
        std::uint64_t Capacity() const { return m_capacity; }

    private:
        std::uint64_t TailForReclaim() const;
        std::uint8_t* SlotAt(std::uint64_t cursor) const {
            return m_base + static_cast<std::size_t>(cursor & m_mask);
        }

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
        RingConsumer(RingControl* control, void* base, std::uint64_t capacityBytes,
                     RingCursorSet cursors);

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
