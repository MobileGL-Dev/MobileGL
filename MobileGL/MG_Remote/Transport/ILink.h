// MobileGL - MobileGL/MG_Remote/Transport/ILink.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The DATA-plane seam. Declared by package c6, implemented by package lk.
// Authority: CONTRACT-P6.md §8.
//
// WHY THIS EXISTS AND WHY IT IS NOT ITransport. ITransport.h:16-20 says in its
// own header that everything on the hot path bypasses it: it carries the
// handshake, surface ops, resync, aux requests and fatals, and that scope is
// already correct for a socket. Below it there was NO interface of any kind -
// Ring.h, ReplySlot.h and EventRing.h each take a raw `void* base` plus a
// capacity, so "swap the transport" had nothing to swap. This header is that
// missing thing, and ITransport is neither extended nor changed by it.
//
// NOTHING INCLUDES THIS FILE IN c6. That is the whole of c6's behaviour
// guarantee: the seam is declared, StreamLink.h is declared against it (which
// is the adequacy proof - see below), and not one call site moves. Package lk
// implements ShmLink by relocating today's SessionSegments / Ring /
// ReplySlotPool / EventRing behind it, line for line, and only then rewrites
// the call sites.
//
// THE SEAM IS SETUP-TIME, NOT HOT-PATH. No method here is called per record,
// per publish, or inside a spin predicate. The producer takes a LinkArena once
// at attach and reserves through it non-virtually; every wait predicate reads
// LinkProgress through a raw pointer taken once. Three grounds, the second
// decisive (CONTRACT-P6.md §8.2):
//
//   1. Two virtuals per record at the 4000 records/frame SEG_CMD is sized
//      against is ~8000 indirect calls/frame, about 0.5% of the measured
//      15.72 ms client frame - affordable, but the only lane that could
//      measure it has +/-4% dispersion, EIGHT TIMES the effect. A gate that
//      can only ever print "within noise" is ID-124's failure mode one level
//      down.
//   2. Doorbell::Wait is a non-virtual template whose spin loop calls ready()
//      up to SpinItersPerUs() * spinUs times; at 916 waits/frame with 99.8%
//      resolved inside the spin, the predicate runs 1e5-1e6 times per frame
//      and reads the watermarks straight off the shared page. That is the
//      31.58%-of-the-GL-thread path. A virtual there is not a rounding
//      question.
//   3. The encode path already depends on four inline accessors around its one
//      Reserve; virtualising Reserve alone forces either four more virtuals or
//      moving R-10's wrap counters behind the seam, and those are deliberately
//      encoder-owned.
//
// So lk's gate is an objdump call-count of the encode path, not a wall clock:
// after lk that path has no more out-of-line calls than before. That is
// falsifiable; "within noise" is not.
//
// WHAT THIS HEADER DELIBERATELY DOES NOT DO.
//   - It does not define Watermark::Reached or any other predicate that
//     SessionRings.h already defines. One definition, one owner.
//   - It does not relocate the bodies of RingProducer::Reserve or
//     RingConsumer::Pop. Those log (MGLOG_E / MGLOG_D) and use Align8 and
//     <cstring>; no header under Transport/ pulls the logger in, and this one
//     does not start. They stay out of line and move to ShmLink.cpp in lk.
//   - It declares no function it does not define. Every entry point here is
//     pure virtual or a POD accessor.
//   - It does not regroup RingControl. That regroup is real and necessary
//     (CONTRACT-P6.md §8.3) but its rename blast radius is 109 production
//     sites, five of them behind a non-Android #if, so it is lk's with a
//     published per-file itemisation - not a side effect of declaring a seam.

#pragma once

#include "../Protocol/mg_protocol_base.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace MobileGL::MG_Remote::Transport {

    class Doorbell;
    class SessionSegments;
    struct SessionSegmentSizes;
    class RingProducer;
    class RingConsumer;
    class EventRingProducer;
    class EventRingConsumer;

    // Which end of the link this instance is. Deliberately NOT ITransport's
    // TransportRole: that enum's InProcess value names a control-plane
    // hand-off, and a data plane's two ends are producer and consumer whatever
    // the control plane did.
    enum class TransportRoleTag : std::uint32_t {
        ClientProducer = 1,
        ServerConsumer = 2,
    };

    // ---- the progress block -------------------------------------------------
    //
    // The watermarks every wait predicate reads. On ShmLink this aliases the
    // shared control page and the loads are cross-process acquire loads; on
    // StreamLink it is process-local and a reader thread stores into it from
    // decoded progress messages.
    //
    // THE RULES SURVIVE THE MECHANISM VERBATIM, and that is the point of naming
    // the block rather than the page: late-never-early (Ring.h's "BATCHING MAY ONLY MAKE A WATERMARK LATE"), `>=`
    // never `==` (SessionRings.h's Watermark::Reached), monotone with a
    // backwards move Fatal (Ring.cpp's AdvanceMonotonic). A stream link changes
    // only HOW a value arrives, never what it means.
    //
    // WHY THESE FOUR AND NOT THE WHOLE PAGE, AND WHY THE EVENT FLAGS ARE NOT
    // HERE. RingControl's watermark cache line HELD, until lk regrouped it, {appliedSeq,
    // submittedSeq, retiredSeq, completedFrameSerial, presentAckSerial}, and
    // submittedSeq is PRODUCER-written (Ring.h's "THE FIVE WATERMARKS" block,
    // "Advanced by the PRODUCER after it publishes") while the other four are consumer-written - so the
    // line has MIXED WRITERS as it stands and no single-writer block can be
    // aliased over it. lk's regroup moves submittedSeq out to the producer's
    // own line, beside cmdHead, which is the other thing the producer writes;
    // what is left IS this struct, and it IS a named member of RingControl,
    // pinned there by per-field offsetof static_asserts (Ring.h).
    //
    // THE TWO EVENT FLAGS ARE DELIBERATELY EXCLUDED, and an earlier draft of
    // this header had them in and was wrong. eventRingFull has TWO writers:
    // the server latches it with store(1) when SEG_EVENT fills
    // (EventRing.h:140) and the CLIENT clears it with an exchange(0) on EVERY
    // DRAIN (EventRing.h:200). Promoting it here would put a per-drain RMW by
    // the client on the same cache line as appliedSeq - which the server
    // release-stores every 64 records and the client's spin predicate reads
    // 1e5-1e6 times per frame. That is new false sharing on the hottest line
    // in the system, and it would land in exactly the numbers lk's gate has to
    // hold constant. They stay in the doorbell/generation group, where mixed
    // writers already live beside consumerParked/producerParked, and cross the
    // seam through EventFlags() instead.
    //
    // It was free to regroup while both peers are the same binary; a wire break the day a second build exists,
    // which is why it is booked now rather than later.
    struct LinkProgress {
        std::atomic<std::uint64_t> appliedSeq;            // records applied
        std::atomic<std::uint64_t> retiredSeq;            // GPU finished
        std::atomic<std::uint64_t> completedFrameSerial;  // releases *RetiredTail / SEG_ADOPT
        std::atomic<std::uint64_t> presentAckSerial;      // returns present credit
    };

    // The reverse channel's two flags. Separated from LinkProgress for the
    // false-sharing reason above, not for a taxonomic one: both roles read
    // eventRingFull in a park predicate, and both roles write it.
    struct LinkEventFlags {
        std::atomic<std::uint32_t> eventRingFull;  // server latches, client clears
        std::atomic<std::uint32_t> eventDropped;   // server-only counter
    };

    // ---- the producer's handle ----------------------------------------------
    //
    // POD state, taken ONCE at attach and used non-virtually thereafter. The
    // reserve/commit algorithm is NOT inlined here (see the header comment):
    // ShmLink keeps today's out-of-line body, StreamLink writes its own.
    // `Base` is null on a link that has no mapping, and a link that publishes
    // that shape must answer every span through ResolveSpan.
    struct LinkArena {
        void* Base = nullptr;
        std::uint64_t Capacity = 0;
        std::uint64_t Mask = 0;
        std::uint64_t LocalHead = 0;
    };

    // The consumer's mirror of the same.
    struct LinkCursor {
        const void* Base = nullptr;
        std::uint64_t Capacity = 0;
        std::uint64_t Mask = 0;
        std::uint64_t LocalTail = 0;
    };

    // A run of bulk bytes named on the wire. On ShmLink `Offset` indexes the
    // shared SEG_STAGE mapping; on StreamLink it indexes the trailing staged
    // region of the batch that carried the record. MGPBlobRef's spelling and
    // every honesty check over it (R-2) are unchanged either way - which is the
    // property that lets the whole record catalogue cross a boundary with no
    // mapping without a single record layout changing.
    struct LinkSpan {
        std::uint64_t Offset = 0;
        std::uint64_t Size = 0;
    };

    // Which segment a span or a view belongs to. Mirrors SegmentKind's wire
    // values; declared here so ILink does not pull the protocol headers in.
    enum class LinkSegment : std::uint32_t {
        Cmd = 1,
        Stage = 2,
        Reply = 3,
        Event = 4,
    };

    // What a link can do. A caller may not infer any of these from the
    // transport's role, and a link may not answer "sometimes" - rule G's
    // second clause is that a transport DECLARES what it supports and may not
    // invent a third answer.
    struct LinkCapabilities {
        bool SharedMapping = false;   // peers see one set of bytes; no copy on read
        bool DescriptorPassing = false;
        bool PublishIsDelivery = false; // false => Flush() is mandatory before any wait
        std::uint64_t MaxRecordBytes = 0;
        std::uint64_t MaxReplyBytes = 0;
    };

    // Non-owning synchronization addresses, acquired once at attachment. They
    // denote process-local atomics on a stream and shared atomics on a mapping.
    struct LinkSignals {
        std::atomic<std::uint64_t>* CmdHead = nullptr;
        std::atomic<std::uint64_t>* SubmittedSeq = nullptr;
        std::atomic<std::uint32_t>* ConsumerParked = nullptr;
        std::atomic<std::uint32_t>* ProducerParked = nullptr;
        std::atomic<std::uint32_t>* EventRingFull = nullptr;
    };

    // -------------------------------------------------------------------------
    // The seam itself.
    //
    // Nineteen entry points, every one taken at attach, at a park boundary, or
    // once per control operation. None on the hot path.
    // -------------------------------------------------------------------------
    class ILink {
    public:
        virtual ~ILink() = default;

        ILink(const ILink&) = delete;
        ILink& operator=(const ILink&) = delete;

        // ---- setup ----------------------------------------------------------

        virtual LinkCapabilities Capabilities() const = 0;

        // Setup views. Ownership remains in the link; callers cache the ring
        // endpoint once so reserve/pop and spin predicates remain non-virtual.
        virtual SessionSegments& Memory() = 0;
        virtual void InitializeEndpoints() = 0;
        LinkSignals Signals();
        std::uint64_t SegmentSize(LinkSegment segment);
        virtual RingProducer& CommandsOut() = 0;
        virtual RingConsumer& CommandsIn() = 0;
        virtual EventRingProducer& EventsOut() = 0;
        virtual EventRingConsumer& EventsIn() = 0;
        virtual void BindDoorbells(Doorbell*, Doorbell*) {}


        // The watermark block. Taken once; every wait predicate reads through
        // the returned pointer with no further virtual dispatch. Never null on
        // an attached link.
        virtual LinkProgress* Progress() = 0;

        // The reverse channel's flags. A separate ACCESSOR, for the reason
        // LinkEventFlags documents. NOT a separate cache line: they sit in the
        // doorbell/generation group beside serverEpoch, which is where mixed
        // writers already live.
        virtual LinkEventFlags* EventFlags() = 0;

        // The command-record arena (producer side) and cursor (consumer side).
        // Exactly one of the two is valid on a given peer.
        virtual LinkArena* RecordArena() = 0;
        virtual LinkCursor* RecordCursor() = 0;

        // ---- bulk bytes -----------------------------------------------------

        // Reserve `bytes` of staging and return where the record should name
        // it. MOBILEGL_ERR_RING_OVERRUN when the request cannot be satisfied at
        // any occupancy; the caller's existing back-pressure path is unchanged.
        virtual MobileGLResult StageBytes(std::uint64_t bytes, LinkSpan* outSpan,
                                          void** outWritePtr) = 0;

        // Resolve a span the peer named. On a shared mapping this is pointer
        // arithmetic over the peer's segment; on a stream it points into the
        // reassembly buffer of the batch that carried the record, and the
        // lifetime rule ("valid until retiredSeq passes the record") is
        // restated against that buffer rather than against the mapping.
        //
        // THIS IS THE METHOD WRITING StreamLink REVEALED. The brief's list -
        // progress transmission, send window, flush - did not name it, and
        // without it a stream link has no way to answer the eleven
        // ResolveOrFatal call sites in the decoder.
        virtual MobileGLResult ResolveSpan(LinkSegment segment, LinkSpan span,
                                           const void** outPtr) = 0;

        // ---- replies --------------------------------------------------------

        virtual MobileGLResult PostReply(std::uint64_t seq, std::int32_t status,
                                         const void* payload, std::uint64_t size) = 0;
        virtual MobileGLResult ReadReply(std::uint64_t seq, std::int32_t* outStatus,
                                         const void** outPayload, std::uint64_t* outSize) = 0;

        // ---- the reverse channel --------------------------------------------

        virtual LinkArena* EventArena() = 0;
        virtual LinkCursor* EventCursor() = 0;
        virtual std::uint64_t EventPublishedHead() const = 0;
        virtual MobileGLResult WaitForEventDelivery(std::uint64_t head, std::uint32_t timeoutMs) = 0;

        // ---- wakeups --------------------------------------------------------
        //
        // THESE TWO RETIRE CONTRACT-P5 §3.9. That ruling kept the doorbell
        // accessors on the session "so that this stays one switch in one file
        // rather than two virtuals every transport has to invent a home for"
        // (ServerSession.cpp:709-724) - and the switch it produced aborts for
        // every role but InProcess, so a spawn apply thread dies in
        // ConsumerDoorbell before it pops its first record. The seam is the
        // home the ruling said did not exist. lk deletes both accessors rather
        // than growing them a socket arm.
        virtual Doorbell& ConsumerBell() = 0;
        virtual Doorbell& ProducerBell() = 0;

        // ---- publication -----------------------------------------------------

        // Make everything published so far visible to the peer. On a shared
        // mapping publish IS delivery and this is a no-op; on a stream it is a
        // write(), and CONTRACT-P6.md requires every transition into a wait to
        // call it first, or a kWaitReply record sitting in a send buffer while
        // the client parks is a hang rather than a slowdown.
        virtual MobileGLResult Flush() = 0;

        // Push the local progress block to the peer. A no-op on a shared
        // mapping, where a release-store IS the transmission. Named separately
        // from Flush because the two have different triggers: Flush is about
        // records the peer has not seen, this is about watermarks the peer is
        // waiting on. The second method writing StreamLink revealed.
        virtual MobileGLResult FlushProgress() = 0;

        // The encoder keeps its established stage allocator; the link tracks only the
        // newly written spans that must precede command delivery.
        virtual void NoteStage(LinkSpan) {}
        // Called after local progress advances. Stream links coalesce transmission;
        // the explicit FlushProgress at every park remains mandatory.
        virtual void ProgressChanged() {}
        virtual bool PeerHungUp() const { return false; }

        // ---- lifecycle -------------------------------------------------------

        virtual bool Attached() const = 0;
        virtual void Detach() = 0;
        virtual TransportRoleTag Role() const = 0;

    protected:
        ILink() = default;
    };

    // Setup factories keep concrete link selection out of codec/session code.
    std::unique_ptr<ILink> CreateSharedLink(TransportRoleTag role);
    MobileGLResult CreateStreamLink(int fd, const SessionSegmentSizes& sizes,
                                   TransportRoleTag role, std::unique_ptr<ILink>& out);

} // namespace MobileGL::MG_Remote::Transport
