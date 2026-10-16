// MobileGL - MobileGL/MG_Remote/Transport/ILink.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The data-plane seam: record reservation and commit, watermarks, replies, events and span
// resolution, implemented by ShmLink and StreamLink (ARCHITECTURE.md §11.9). ITransport carries
// only the control plane and is not part of it.
// Reserve, pop and the wait predicates never dispatch through it: callers cache the ring
// endpoints (CommandsOut/CommandsIn) and the LinkProgress pointer once at attach. The spin
// predicate runs up to 1e5-1e6 times a frame, where a virtual call is a measurable cost.
// Watermark predicates such as Watermark::Reached stay in SessionRings.h.

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
    // The watermarks every wait predicate reads: on ShmLink an alias of the shared control page,
    // on StreamLink process-local, stored by a reader thread from progress messages. Both keep
    // the same rules: a watermark may arrive late, never early (Ring.h), compare with `>=`
    // (Watermark::Reached), and a backwards move is Fatal (AdvanceMonotonic). It holds only
    // consumer-written fields, so it is one RingControl member on its own line (ARCHITECTURE.md
    // §11.2). The event flags stay out: the client clears eventRingFull on every drain, an RMW
    // that would share the cache line the client's spin predicate reads for appliedSeq.
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
        // P12: DOES THIS LINK KEEP AN ANSWER UNTIL ITS READER TAKES IT? A stream does - the
        // answers are the client's own bytes - and a shared pool does NOT: a slot is
        // `seq % slotCount` and the next answer into it wins, so "the watermark passed this seq"
        // says nothing about whether the answer is still there.
        //
        // IT IS A CAPABILITY RATHER THAN A HOPE because a row that DEFERS its answer (the create
        // window) may only run where it is true: measured on the device, a deferral across the
        // ~1,000 records between two creates lost its answer to a bounded buffer every time.
        // A caller that sees false must take the blocking path instead.
        bool RetainsReplies = false;
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

        // P12: DECLARE THAT THIS SEQ'S ANSWER WILL BE READ, so that a link which can retain it
        // does. Called by the producer side of the wire, in seq order, BEFORE the record is
        // published - the answer cannot arrive before that, and the reader stores under the same
        // mutex the declaration takes, so the hand-off needs no second synchronisation.
        //
        // WHY THE DECLARATION AND NOT "RETAIN EVERYTHING": a server that ignores the no-reply bit
        // answers ~55,903 records in one frame where this client reads ~4,501, so retain-on-arrival
        // grows ~11.3 entries per create and any cap is reached in C/11.3 creates - after which the
        // eviction wave lands on precisely the oldest WANTED answer, which is the wedge this exists
        // to avoid. Keyed on the declaration, the retained set is bounded by the CLIENT's own
        // deferral depth instead of by the server's behaviour.
        //
        // A link with RetainsReplies false ignores it (the default), and its callers must not
        // defer: see the capability's own note.
        virtual void DeclareReplyRead(std::uint64_t seq) { (void)seq; }

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
    // PH-7 (4), ID-P7-3. The server's nonce-bound stream link, in the two steps
    // StreamLink::AttachOwnedDeferred / BindDataFd describe: created with its owned memory and
    // no descriptor (Welcome announces those sizes before the data connection exists), then
    // bound to the one data connection that presented the session's nonce. Both halves live
    // here so session code never names the concrete link (scripts/ci/link_seam_purity.py).
    // Binding answers INVALID_ARGUMENT for a link these factories did not make as a stream,
    // and leaves `fd` to the caller whenever it does not answer OK.
    MobileGLResult CreateDeferredStreamLink(const SessionSegmentSizes& sizes, TransportRoleTag role,
                                            std::unique_ptr<ILink>& out);
    MobileGLResult BindStreamLinkDataFd(ILink& link, int fd);

} // namespace MobileGL::MG_Remote::Transport
