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
//
// ---------------------------------------------------------------------------
// THE FIVE WATERMARKS (P5 R-9). Four of them are Transport::LinkProgress and
// are reached as `control.Progress.<name>`; submittedSeq is the fifth and stays
// at RingControl scope, on the producer's line (lk, CONTRACT-P6 §8.3).
// One sentence each, and they are a contract:
// every one of the five was declared here at P0 and written by nobody but
// InitRingControl, so until P5 there was nothing to disagree with.
//
//   submittedSeq        Advanced by the PRODUCER after it publishes. NOBODY
//                       WAITS ON IT - it is diagnostic, the answer to "how far
//                       ahead of the server is the client right now". It lives
//                       on the producer's own cache line beside cmdHead; it is
//                       the ONE of the five that is not in LinkProgress, and
//                       that is why LinkProgress can be single-writer.
//   appliedSeq          Advanced by the CONSUMER for EVERY SINGLE RECORD it
//                       applies. The client's verb barrier and every reply wait
//                       read it, so it is the one watermark P5 FORBIDS BATCHING:
//                       the sixty-four-record batching this ring was designed
//                       for makes a waiter block on work that already ran, or -
//                       far worse - resume on work that has not.
//   retiredSeq          Advanced by the CONSUMER once it has finished with the
//                       SEG_STAGE bytes a record referenced. The staging
//                       allocator reclaims behind it, and nothing else may.
//   completedFrameSerial Advanced by the SERVER when a present completes. What
//                       recycling and ageing wait on; it trails appliedSeq by
//                       the GPU's own depth and must never be conflated with it.
//   presentAckSerial    Advanced by the SERVER when it returns a present credit.
//                       The client's present throttle waits on it; it is the
//                       only back-pressure that bounds latency rather than bytes.
//
// Every wait on all five is `>=`, never `==`: a waiter that tests equality
// misses the wakeup the moment a producer or consumer moves by more than one.
//
// BATCHING MAY ONLY MAKE A WATERMARK LATE. All five except appliedSeq may be
// published lazily, because a waiter that sees an old value waits longer than
// it had to and is still correct. NONE of them may ever be published EARLY: a
// watermark that reports more than was actually done turns every waiter into a
// silent use of work that has not happened, and there is no checksum anywhere
// on this ring that would catch it.
//
// kRecPad DOES NOT ADVANCE SEQ. A wrap filler is framing, not a record: it has
// no opcode, no payload meaning and no reply slot. Both sides must skip it
// BEFORE counting. If one side counts it and the other does not, the two seq
// spaces drift by one at every wrap - and because seq IS the reply-slot id
// (P5 R-3), a drifted seq silently reads another call's answer rather than
// failing. Nothing on this ring would detect that, which is why the rule is
// stated here rather than left to each side's loop.
// ---------------------------------------------------------------------------

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
        // DEAD IN P5, DELIBERATELY, AND NOBODY MAY WIRE THEM UP HALFWAY.
        //
        // SEG_STAGE is NOT a ring any more. Package w1's encoder owns staging as
        // an ENCODER-LOCAL LINEAR ALLOCATOR: a staged byte run carries no
        // RingRecordHeader, there is no consumer walking SEG_STAGE, and the
        // allocator reclaims on `retiredSeq` - the sequence watermark below -
        // rather than on these three cursors. So all three stay ZERO for the
        // whole of P5, `RingCursorSet::Stage` has no producer and no consumer,
        // and `SessionTest.TheStageCursorTripleStaysDeadAcrossAWholeSession`
        // pins that rather than leaving it to be noticed.
        //
        // They are kept rather than deleted because RingCursorSet, the three
        // cursor accessors in Ring.cpp and RingTest's fixture are all written
        // against a two-triple page, and P8/P11's shadow and adopt segments are
        // the ring-shaped users this triple was reserved for. What is NOT
        // acceptable is the middle state: a producer publishing `stageHead` with
        // nothing advancing the two tails makes FreeBytes() fall to zero the
        // first time the head laps the capacity and never recover, which is a
        // guaranteed hang rather than a slow path. Five watermarks already spent
        // a whole phase declared-and-written-by-nobody; this is the sixth, and
        // it is declared-and-written-by-nobody ON PURPOSE, which is only
        // different if it is written down.
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
        alignas(64) std::atomic<std::uint32_t> serverEpoch;    // ++ on context loss / server restart
        std::atomic<std::uint32_t> ringGeneration;             // ++ after a hard drain
        std::atomic<std::uint32_t> consumerParked;             // server asleep, producer must ring
        std::atomic<std::uint32_t> producerParked;             // client asleep, server must ring
        std::atomic<std::uint32_t> eventRingFull;              // SEG_EVENT full, server stopped applying
        std::atomic<std::uint32_t> eventDropped;               // dropped lossy events
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
    static_assert(offsetof(RingControl, Progress) % 64 == 0,
                  "LinkProgress starts a cache line of its own");
    static_assert(offsetof(RingControl, serverEpoch) == offsetof(RingControl, Progress) + 64,
                  "the Progress line holds Progress and nothing else");

    // PER FIELD, which is what §8.3 asks for and what the aggregate asserts above cannot
    // do: the field ORDER inside LinkProgress is a wire-visible fact about a page both
    // roles map, and swapping two of them passes every other check in this file.
    static_assert(offsetof(LinkProgress, appliedSeq) == 0, "LinkProgress field order: appliedSeq");
    static_assert(offsetof(LinkProgress, retiredSeq) == 8, "LinkProgress field order: retiredSeq");
    static_assert(offsetof(LinkProgress, completedFrameSerial) == 16,
                  "LinkProgress field order: completedFrameSerial");
    static_assert(offsetof(LinkProgress, presentAckSerial) == 24,
                  "LinkProgress field order: presentAckSerial");
    static_assert(sizeof(LinkProgress) == 32,
                  "four consumer-written watermarks; the event flags are NOT in here");

    // The two event flags stay in the doorbell group on purpose (see the group's comment).
    // LinkEventFlags mirrors them, so the mirror's shape is pinned too.
    static_assert(offsetof(RingControl, eventDropped) == offsetof(RingControl, eventRingFull) + 4,
                  "the two event flags are adjacent, in that order");
    static_assert(offsetof(RingControl, eventRingFull) / 64 == offsetof(RingControl, serverEpoch) / 64,
                  "the event flags share the doorbell group's line, NOT the Progress line");
    static_assert(offsetof(LinkEventFlags, eventRingFull) == 0 &&
                      offsetof(LinkEventFlags, eventDropped) == 4 && sizeof(LinkEventFlags) == 8,
                  "LinkEventFlags mirrors the pair in RingControl");

    // The page's real content must fit the page it claims. sizeof(RingControl) == 4096
    // above cannot catch a member that grew, because alignas(4096) forces that number;
    // the struct's content ends far short of it and the tail is padding.
    static_assert(offsetof(RingControl, eventDropped) + sizeof(std::uint32_t) <= 4096,
                  "RingControl's members must fit the page it claims to be");
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

    // THESE ARE RING FLAGS AND THEY ARE NOT MGPipeCallFlags, AND THREE OF THE
    // BITS COLLIDE WITH A DIFFERENT MEANING. `MGPipeCallFlags` (MG_Pipe/MGPipe.h:
    // 42-54) is a SEPARATE SPACE that happens to overlap this one, and an encoder
    // that copies `MGPipeCallFlagsFor(op)` into RingRecordHeader::flags without
    // translating puts a call's bits into a framing field:
    //
    //   bit 0  kNeedsAck  == kRecNeedsAck    same meaning, harmless
    //   bit 1  kHasBlob   == kRecHasBlob     same meaning, harmless
    //   bit 2  kVarTail   == kRecPad         WORST: a var-tail record would read
    //                                        as a WRAP FILLER and be skipped
    //                                        silently by Pop, losing the record
    //                                        with nothing logged anywhere
    //   bit 3  kHostSpan  == kRecBorrowSlot  a host-span record would read as
    //                                        borrowed into the GPU timeline, and
    //                                        the consumer would stop reclaiming
    //                                        ring bytes behind it for ever
    //   bit 4  kReplySlot == kRecVarTail     a blocking call would read as having
    //                                        a tail it does not have
    //   bit 5  kOptional  == (unused here)
    //
    // Translating is the ENCODER's job. Two things on this side make the first
    // two of those survivable anyway rather than trusting it: `Pop` requires a
    // filler to carry BOTH kRecPad AND kind == kRingPadRecordKind, so a real
    // record with bit 2 set is delivered rather than eaten (a call record always
    // has a real opcode kind, the catalogue starts at 1); and SessionConsumer
    // counts and NAMES every kRecBorrowSlot it sees, because P5 produces no
    // borrowed slots at all and the bit arriving means the collision did.
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
        // `capacityBytes` must be a power of two between kMinRingCapacity and
        // kMaxRingCapacity. Anything else leaves Valid() false.
        RingProducer(RingControl* control, void* base, std::uint64_t capacityBytes,
                     RingCursorSet cursors);

        bool Valid() const { return m_control != nullptr; }

        // Bytes still writable before the consumer has to catch up.
        std::uint64_t FreeBytes() const;

        // Reserves room for one record and returns a pointer to its payload,
        // or nullptr when the ring is full. The payload is uninitialized;
        // alignment padding at its tail is NOT zeroed. Emits a pad record
        // automatically when the record would straddle the wrap boundary, so
        // every record is contiguous.
        //
        // A record whose total (header + payload, rounded up to 8) exceeds
        // MaxRecordBytes() == Capacity()/2 is refused outright, with an error
        // log and however empty the ring is: chunking it is the emitter's job
        // (plan section 8.2, the G3 chunking rule). Half is exact, not
        // conservative - it is the largest record EVERY head offset can place,
        // because a wrap pad costs at most total-8 bytes on top of the record
        // and 2*total-8 <= capacity-8 holds exactly up to capacity/2. Above it
        // a record is placeable at some offsets and not at others, and a
        // producer waiting for FreeBytes() >= total stalls forever on an empty
        // ring. So: nullptr with FreeBytes() >= total never means "wait"; it
        // can only mean "too big, chunk".
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
