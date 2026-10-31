// MobileGL - MobileGL/MG_Remote/Wire/PipeWireCodec.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The MGPipe record codec: one MGPipe call becomes a record in SEG_CMD (its blob bytes in
// SEG_STAGE), and a decoded record becomes one call of an existing MGPipeApply* function. The
// codec owns no semantics; a decoder arm must delegate, never handle a record itself. It follows
// the cross-process rules even in one address space (ARCHITECTURE.md §4.2).
// Seq is the record ordinal, 1-based (0 = nothing encoded), and also addresses the record's
// reply; there is no seq field on the wire. A kRecPad wrap filler does not advance seq: both
// sides skip it before counting, or their numbering drifts apart at every wrap, undetected.

#pragma once
#include <Includes.h>

#include <MG_Pipe/MGPipe.h>
#include <MG_Pipe/PipeVerbSink.h>

#include "../Transport/Ring.h"
#include "../Transport/Doorbell.h"

namespace MobileGL::MG_Remote::Wire {

    // ---- table 0: the segment id space -------------------------------------------------
    //
    // The SAME VALUES as Protocol::SegmentKind (protocol.fbs:36-44); PipeWireCodec.cpp
    // static_asserts the two agree, which is the only place the flatbuffers header and this
    // enum meet. 0 is ALWAYS "no segment" and is never a real segment id, which is what lets
    // MGPBlobRef{Seg == 0, Size != 0} be a detectable fault rather than a legal shape.
    enum SegmentId : Uint32 {
        kSegNone = 0,
        kSegCmd = 1,    // client-owned command ring (RingControl + records)
        kSegStage = 2,  // client-owned bulk staging: every blob and every var-tail's bytes
        kSegReply = 3,  // server-owned reply pool, addressed seq % slots (R-3)
        kSegEvent = 4,  // server-owned event ring (the reverse channel)
        kSegShadow = 5, // client-owned per-object shadow (P8+)
        kSegAdopt = 6,  // server-owned adopted store, client RW (P11)
    };

    // Seq is 1-based. 0 is "no record", never a valid reply-slot id.
    inline constexpr Uint64 kInvalidSeq = 0;

    // One mapped segment as this ROLE sees it. Two roles in one process have two different
    // SegmentTables over the same memory on purpose: a client that can resolve SEG_REPLY as
    // if it owned it is the inproc cheat R-2 exists to kill.
    struct SegmentView {
        void* Base = nullptr;
        Uint64 Size = 0;
    };

    // ---- the per-role segment table, and the process resolver hook ---------------------
    //
    // gMGPipeSegmentResolver (MG_Pipe/MGPipeHostSpan.h:47) is a plain non-atomic inline
    // variable and there is exactly ONE of it per process, so under inproc the two roles
    // cannot both install their own into it. TABLE 3's ruling: the resolver is installed by
    // the SERVER role only, before the apply thread starts, and the client never resolves a
    // span at all (it only ever writes Ptr = nullptr). Install() therefore takes the role.
    class SegmentTable {
    public:
        void AttachLink(Transport::ILink* link);
        void Install(SegmentId seg, SegmentView view);
        SegmentView Get(SegmentId seg) const;

        // Bounds-checked resolve. Returns nullptr when seg is unknown, size is 0, or
        // offset + size runs past the segment; the CALLER escalates that to
        // Fatal{ProtocolCorruption} (R-2.3) rather than this returning into a Fatal, so a
        // unit test can exercise the arithmetic without dying.
        const void* Resolve(Uint32 seg, Uint64 offset, Uint64 size) const;

        // Points MG_Pipe::gMGPipeSegmentResolver at this table. Server role only; asserts if
        // a DIFFERENT table is already installed ON THE CALLING THREAD, because one thread
        // serving two sessions at once is the failure this function exists to make loud.
        //
        // P14 S2: THE TABLE IS PER THREAD, THE HOOK IS PER PROCESS. Several sessions in one
        // process each have their own table and their own threads, so a second session's install
        // is neither a race nor an error: it installs the one process-wide thunk (idempotent) and
        // binds the calling thread to ITS table. g_processResolverTable stays as the fallback for
        // a thread that bound nothing - the inproc one-process client+server shape, whose apply
        // thread is created after the app thread's Accept and does not bind.
        void InstallProcessResolver();
        // Unbinds the CALLING thread and clears the process-wide fallback. The hook itself stays
        // installed: another session may still be live, and a null answer from a table-less thunk
        // is exactly what a null hook answered.
        static void UninstallProcessResolver();
        // P14 S2: binds the CALLING thread to this table (ThreadSessionScope's two calls).
        void BindThreadResolver();
        static void UnbindThreadResolver();

    private:
        Transport::ILink* m_link = nullptr;
        SegmentView m_views[kSegAdopt + 1];
    };

    // ---- the four Fatal arms, worded once ----------------------------------------------
    //
    // One function so encoder, decoder and every package's own bounds check produce the SAME
    // log line. `what` is the record or field; `detail` is the number that was wrong.
    [[noreturn]] void WireProtocolFatal(const char* what, const char* detail);
    [[noreturn]] void WireProtocolFatalAt(const char* what, Uint64 got, Uint64 expected);

    // PH-1 (3), ID-P7-1: THE SAME TWO LINES, THROUGH THE LATCH. Byte-identical to the two above
    // (same `Fatal{ProtocolCorruption, "<what>"}` wording), but through MG_Remote::SessionLatch:
    // in the spawn / TCP session child the fault latches and these RETURN false, so the decode
    // path declines the record and the session closes by name; everywhere else (inproc, the
    // client, a unit case) SessionLatch is SessionFail and these do not return. Used ONLY on the
    // decode path, where the bytes are the peer's - an encoder-side check is a local bug and
    // keeps the [[noreturn]] spelling.
    Bool WireProtocolLatch(const char* what, const char* detail);
    Bool WireProtocolLatchAt(const char* what, Uint64 got, Uint64 expected);

    // Checks one record's blobref (split only; monolith emissions never reach this layer).
    // Honest is fully declared (real Seg, non-zero Size, Offset+Size inside the segment) or fully
    // absent (all zero). A Seg or Offset with Size == 0 is the monolith emitter's shape and is
    // rejected, since reading it as absent would drop the record's bytes (ARCHITECTURE.md §4.2).
    // "A content record needs a declared blob" depends on the payload's other fields, so that is
    // RequireDeclaredBlob, called by the decoder where the payload implies content.
    // Returns true when honest; false when a fault was latched (armed session child only;
    // elsewhere the check does not return). Decode-path callers return on false.
    Bool CheckBlobIsHonest(MG_Pipe::MGPWireOp op, const MG_Pipe::MGPBlobRef& blob, const SegmentTable& segments);
    // R-2.3 arm 2: the record's other fields say it carries content, so the blob must be
    // declared. Fatal on an absent blob, then CheckBlobIsHonest on a present one.
    Bool RequireDeclaredBlob(MG_Pipe::MGPWireOp op, const MG_Pipe::MGPBlobRef& blob, const SegmentTable& segments);
    // R-2.3 arms 1 and 3 for MGHostSpan. Our client produces ZERO host spans
    // (kCapNeedsHostIndexBytes / kCapNeedsHostUboBytes are RESERVED since P8-F, never set), so
    // this firing at all is a finding, not just a corruption check.
    //
    // IT CANNOT DO ARM 4 - it has no segment table - so a span that names a real segment and a
    // run PAST THE END OF IT passes this function. Use the overload below on any path that has
    // a table; this one exists because c0 shipped the signature and other packages compile
    // against it.
    Bool CheckHostSpanIsHonest(const MG_Pipe::MGHostSpan& span);
    // All four arms. Arm 4 is the one the signature above cannot express: a span is only
    // honest if its Offset+Size actually lies inside the segment it names, and the promise
    // WireVerbSink's header makes - that OnDrawVbo is handed a VALIDATED argument list - is
    // false without it. Latent in P5 (nothing emits a span) and armed at P8, which is exactly
    // when nobody will be reading this file.
    Bool CheckHostSpanIsHonest(const MG_Pipe::MGHostSpan& span, const SegmentTable& segments);

    // A legal segment run must also contain every index the draw will consume. Shared by
    // the encoder, decoder and sink so a direct sink call cannot bypass the extent gate.
    // PH-1 (3): answers like the arms above (false = latched).
    Bool CheckDrawUserIndices(const MG_Pipe::MGPDrawInfo& info, const MG_Pipe::MGPDrawRange* ranges,
                              const MG_Pipe::MGHostSpan& span);

    // ---- one record's shape, computed once and read by both sides ----------------------
    // MGP_WIRE_CHECK_BOUNDS only proves the payload fits, not the tails: a record declaring
    // Count = 4000 with 8 tail bytes passes it. The encoder computes this layout from the payload
    // and refuses tails that disagree; the decoder computes it from the received payload and
    // refuses a record whose header Size disagrees. Every tail starts 8-byte aligned within the
    // record (the encoder zero-fills the gap): MGPDrawRange is 12 bytes, so DrawVbo's second
    // tail, an MGHostSpan, could otherwise land on a 4-byte boundary. set_program_bindings uses
    // three tails, hence kMaxWireRecordTails.
    inline constexpr Uint32 kMaxWireRecordTails = 3;

    struct WireRecordLayout {
        Uint64 PayloadBytes = 0;                            // sizeof the op's payload struct
        Uint64 TailOffset[kMaxWireRecordTails] = {0, 0, 0}; // from the START of the record,
                                                            // header included
        Uint64 TailBytes[kMaxWireRecordTails] = {0, 0, 0};
        Uint32 TailCount = 0;
        Uint64 TotalBytes = 0; // header + payload + gaps + tails, rounded up to 8
        // P5b: WHAT THE SECOND TAIL IS. SetShaderBuffers' second tail is MGHostSpan[HostSpanCount]
        // and DrawVbo's is EITHER the user-index MGHostSpan (kDrawHasUserIndices) OR one
        // MGPDrawIndirect (kDrawIsIndirect, CONTRACT-P5B.md d1). The encoder's host-span
        // honesty pass reads this rather than "tail 2 exists", because a 40-byte indirect
        // block read as spans is one span and a quarter of garbage.
        Bool SecondTailIsHostSpans = false;
    };

    // `payload` must already be known to hold at least the op's payload struct - that is what
    // MGP_WIRE_CHECK_BOUNDS proves, and this function is only ever called after it. Returns
    // false for an opcode outside the catalog; a count past its own GL bound is Fatal,
    // because a decoder holding such a record has nothing safe left to do with it - and in an
    // armed session child (PH-1 (3)) that Fatal latches instead and this returns false too, which
    // is why the decoder asks the opcode question itself before calling.
    Bool MGPipeWireRecordLayout(MG_Pipe::MGPWireOp op, const void* payload, WireRecordLayout& out);

    // The catalog's own spelling of an opcode, for a Fatal line. Out of range is "<opcode>".
    const char* WireOpName(MG_Pipe::MGPWireOp op);

    // P13 W5: the tail is MG_Pipe's (PipeVerbSink.h) - the record arm's in-process port speaks it too.
    using WireTail = ::MobileGL::MG_Pipe::MGPipeVerbTail;

    // ---- encoder -----------------------------------------------------------------------
    //
    // Not thread safe: one encoder per client context, driven by the GL thread, by
    // construction (SPSC is the ring's contract too).
    class PipeWireEncoder {
    public:
        PipeWireEncoder() = default;
        PipeWireEncoder(Transport::ILink* link, SegmentTable* segments);
        PipeWireEncoder(Transport::RingControl* control, Transport::RingProducer* cmd, Transport::RingProducer* stage,
                        SegmentTable* segments);

        Bool Valid() const;

        // Copies `size` bytes into SEG_STAGE and returns the blobref that names them:
        // {Seg = kSegStage, Offset = in-segment byte offset, Size = size}. R-2.2 - Size is
        // NEVER 0 for a content blob, and a 0-size call is a programming error that Fatals
        // rather than returning an empty ref, because "the record declared no blob" and "the
        // record declared an empty blob" must not be spelled the same way on a wire.
        //
        // The bytes are valid until retiredSeq passes the record that names them (R-11).
        MG_Pipe::MGPBlobRef StageBytes(const void* bytes, Uint64 size);

        // Writes one record: header (op, MGPipeCallFlagsFor(op), total size), then the fixed
        // payload, then the variable tail. Returns the record's SEQ, which is also its
        // reply-slot id (R-3), or kInvalidSeq if the ring refused it.
        //
        // A record larger than RingProducer::MaxRecordBytes() is Fatal{RingOverrun}, NOT a
        // wait: R-10's content rows cut their blobs at the stage chunk budget, so this bound is
        // about a record's own bytes and this is where one that grew past it fails loudly.
        // MaxRecordBytesSeen() is the counter that feeds that bound into MEASUREMENTS.
        Uint64 EncodeRecord(MG_Pipe::MGPWireOp op, const void* payload, Uint64 payloadBytes,
                            const void* varTail = nullptr, Uint64 varTailBytes = 0);

        // The same call for the rows that carry more than one tail - two for
        // set_shader_buffers, set_stream_output_targets and draw_vbo's conditional second,
        // three for P5e's set_program_bindings. The one-tail form above is this one with
        // tailCount <= 1; nothing is duplicated between them.
        //
        // The tails a caller hands over are CROSS-CHECKED against the layout the payload
        // itself declares (MGPipeWireRecordLayout): a caller whose Count says 4000 while its
        // tail holds 8 bytes is Fatal HERE, on the producing side, rather than on a peer that
        // can only report a corrupt stream. That is the same arithmetic the decoder runs, so
        // the check is real rather than a restatement of the caller's own belief.
        Uint64 EncodeRecord(MG_Pipe::MGPWireOp op, const void* payload, Uint64 payloadBytes, const WireTail* tails,
                            Uint32 tailCount, Bool noReply = false);

        // `noReply` (P12) stamps Transport::kRecNoReply, which tells the server it may SKIP the
        // answer for this record. IT IS NOT "the caller is not waiting" - that is `wantReply` on
        // ClientSession::EmitAndWaitTails, and the two are deliberately different questions: the
        // create window emits without waiting and READS the answer later, so its records must not
        // carry this. Only a record whose answer nobody will ever read may set it, and the client
        // is where that is known - the server cannot recover it from the opcode, because the same
        // row answers both ways (set_texture_params waits for an unconfirmed object and does not
        // for a confirmed one).

        // Releases every SEG_STAGE run named by a record the apply side has retired
        // (RingControl::retiredSeq, only read here). Called from Publish() and when StageBytes
        // runs short; nothing else reclaims stage bytes. The encoder keeps a {seq, stage cursor}
        // mark per record and reclaims to the newest retired mark, so no wire field is needed.
        // RingControl's stage cursor triple is unused by both sides: never attach a RingConsumer
        // to RingCursorSet::Stage. Its local tail would never move, so PublishApplied /
        // PublishRetired would walk both cursors backwards under this allocator.
        void ReclaimStagedBytes();
        // Staged bytes not yet reclaimed. The number MOBILEGL_IPC_STAGE_MB has to cover.
        Uint64 StagedBytesInFlight() const;
        // The {seq, cursor} marks the queue is STILL STORING, consumed ones included - not the
        // number in flight. Bounded by twice the records in flight, and exposed so a case can
        // assert that bound rather than trust the comment, because an unbounded queue here is
        // a steady-state leak no SSIM comparison and no two-scenario lane would ever see.
        //
        // It reports the storage deliberately: the in-flight count stays at one or two while
        // the container behind it grows for ever, so a control written against that number
        // goes green through exactly the leak it exists to catch.
        SizeT StageMarksHeld() const;

        // Release-stores the head cursor, then rings the consumer doorbell IF PARKED. The
        // order is pinned by RingTest.cpp:446 and must not be swapped: notify-then-publish
        // loses the wakeup.
        void Publish();

        // The highest seq this encoder has produced. The verb barrier (R-1) waits for
        // RingControl::appliedSeq to reach it.
        Uint64 EmitSeq() const;

        // R-10's proof obligation: the largest single record this encoder has written.
        Uint64 MaxRecordBytesSeen() const;
        // The op whose record set that maximum, by name, or "none" before any record. Published
        // beside the number so R-10's integrator decision names a row rather than a size.
        const char* MaxRecordOpName() const;

        // THE CAP THAT NUMBER IS PROVED AGAINST, read from the ring rather than recomputed.
        // RingProducer::MaxRecordBytes() == Capacity()/2, and Capacity() is
        // MOBILEGL_IPC_RING_MB. Published beside MaxRecordBytesSeen() so a reader never has to
        // multiply an environment variable to know whether the proof holds - which is the one
        // arithmetic step between "4 MiB" and "half of the ring this process actually got".
        // 0 when this encoder has no command ring (a default-constructed one).
        Uint64 MaxRecordBytesCap() const;

        // ---- the producer's ring and staging counters --------------------------------------
        // CmdWraps(): times the SEG_CMD head crossed a multiple of the ring capacity. Guaranteed
        // once a workload writes more than the ring holds, so a test may assert it.
        // CmdWrapPads(): kRecPad fillers laid when a record would straddle that boundary. Recorded,
        // not asserted: a record stream whose stride divides the capacity wraps with zero pads.
        // StageReclaimWaits(): allocations that had to wait for retiredSeq after reclaiming what
        // was already retired; one per allocation, however many marks it waited for.
        Uint64 CmdWraps() const;
        Uint64 CmdWrapPads() const;
        Uint64 StageReclaimWaits() const;
        // The live session supplies its shutdown-aware producer doorbell. Standalone codecs
        // without a consumer cannot wait for retirement and retain the named refusal.
        void SetLink(Transport::ILink* link) { m_link = link; }
        void SetStageRetirementDoorbell(Transport::Doorbell* bell) { m_stageRetirementBell = bell; }
        using CancellationHook = Bool (*)(void* self);
        void SetCancellationState(const std::atomic<bool>* lost, CancellationHook hook, void* self) {
            m_sessionLost = lost;
            m_cancellationHook = hook;
            m_cancellationSelf = self;
        }
        // The fast check is used on encode; the doorbell probe only runs while
        // staging or at an unsuccessful wait boundary.
        Bool Canceled() const {
            return m_canceled || (m_sessionLost && m_sessionLost->load(std::memory_order_acquire));
        }
        Bool CheckCancellation();
        void SetStageWaitTimeoutMs(Uint32 timeoutMs) { m_stageWaitTimeoutMs = timeoutMs; }

        // P5e (ra, CONTRACT-P5E §2.6): THE STAGE BELL IS A CLIENT WAIT, so it has to drain the
        // reverse channel like every other one. The encoder cannot drain it itself - SEG_EVENT
        // and its consumers belong to the session - so the session installs a hook and this
        // layer only says WHEN. Without it the flow-control deadlock is one buffer upload
        // wide: the server stops applying on a full event ring, retiredSeq stops moving, and a
        // client parked here for staged bytes never drains the ring that would release it.
        // A raw function pointer rather than std::function: this header sits below MG_Impl and
        // the call is on a path that is already about to park.
        using StageWaitHook = void (*)(void* self);
        void SetStageWaitHook(StageWaitHook hook, void* self) {
            m_stageWaitHook = hook;
            m_stageWaitSelf = self;
        }

        // Bytes this encoder has ever written into SEG_CMD, pad fillers included: the
        // producer's monotonic head cursor. It is the DENOMINATOR the wrap count only means
        // anything against - "0 wraps" is a defect when the run pushed more bytes than the ring
        // holds and a tautology when it pushed fewer, and only this number tells those apart.
        // It is also how E3(e)'s lane knows when it has driven enough work, without guessing a
        // record size. 0 when there is no command ring.
        Uint64 CmdBytesWritten() const;

    private:
        // {the record's seq, the SEG_STAGE cursor just past everything that record named}.
        struct StageMark {
            Uint64 Seq = 0;
            Uint64 StageCursor = 0;
        };

        // The SEG_STAGE linear allocator (BRIEF §5 w1's own wording). Monotonic byte counters
        // over the segment; the in-segment offset is `cursor % capacity` and a run that would
        // straddle the end skips to the boundary, exactly as a ring does, but WITHOUT touching
        // RingControl - see ReclaimStagedBytes above.
        Transport::ILink* m_link = nullptr;
        Uint8* StageAllocate(Uint64 size);

        // AN EMPTY STAGE STARTS OVER AT ZERO, so that the wrap skip is only ever charged
        // against bytes that are really still in flight. Head and tail are monotonic, so once
        // everything has retired they are EQUAL BUT NOT ZERO, and `head % capacity` is
        // wherever the last run happened to end - a wrap skip charged against that offset
        // costs the suffix a second time and refused a blob the whole segment could hold, with
        // a message that reported zero bytes in flight while it did so. Rebasing also rewrites
        // the marks still held: a mark stores an ABSOLUTE head cursor and a later reclaim
        // assigns it to m_stageTail, so leaving a stale one behind would drive the tail past
        // the head and underflow StagedBytesInFlight().
        void RebaseEmptyStage();

        Transport::LinkProgress* m_progress = nullptr;
        Transport::LinkSignals m_signals{};
        Transport::RingProducer* m_cmd = nullptr;
        Transport::RingProducer* m_stage = nullptr;
        SegmentTable* m_segments = nullptr;
        Uint64 m_emitSeq = kInvalidSeq;
        Uint64 m_maxRecordBytes = 0;
        MG_Pipe::MGPWireOp m_maxRecordOp = MG_Pipe::MGPWireOp::kOpCount;
        Uint64 m_cmdWraps = 0;
        Uint64 m_cmdWrapPads = 0;
        Uint64 m_stageReclaimWaits = 0;
        Transport::Doorbell* m_stageRetirementBell = nullptr;
        const std::atomic<bool>* m_sessionLost = nullptr;
        CancellationHook m_cancellationHook = nullptr;
        void* m_cancellationSelf = nullptr;
        Bool m_canceled = false;
        Uint32 m_stageWaitTimeoutMs = 120000;

        // P5e (ra): the session's event drain, called around the stage-bell park (§2.6).
        StageWaitHook m_stageWaitHook = nullptr;
        void* m_stageWaitSelf = nullptr;
        Vector<StageMark> m_stageMarks;
        SizeT m_stageMarkFront = 0;
        Uint8* m_stageBase = nullptr;
        Uint64 m_stageCapacity = 0;
        Uint64 m_stageHead = 0; // monotonic bytes allocated
        Uint64 m_stageTail = 0; // monotonic bytes reclaimed
    };

    // ---- decoder -----------------------------------------------------------------------

    // P13 W5: the reply sink and the verb sink are MG_Pipe's (PipeVerbSink.h): the record arm
    // applies the same verbs in a library without a transport, through the same interface.
    using ReplySink = ::MobileGL::MG_Pipe::MGPipeReplySink;
    using QueryResultReply = ::MobileGL::MG_Pipe::MGPipeQueryResultReply;
    using WireVerbSink = ::MobileGL::MG_Pipe::MGPipeVerbSink;

    // Not thread safe: one decoder on the apply thread, by construction.
    class PipeWireDecoder {
    public:
        PipeWireDecoder() = default;
        PipeWireDecoder(Transport::ILink* link, SegmentTable* segments, ReplySink* replies);
        PipeWireDecoder(Transport::RingControl* control, SegmentTable* segments, ReplySink* replies);

        Bool Valid() const;

        // Decodes ONE record and calls the matching MGPipeApply* free function.
        //
        // TWO BOUNDS CHECKS, NOT ONE. The generated MGP_WIRE_CHECK_BOUNDS only proves
        // `size >= sizeof(MGPWireRec_X)` - IT CANNOT SEE THE TAIL, so a record declaring
        // Count = 4000 while carrying 8 bytes passes it today. The decoder must recompute the
        // total from the declared count(s) and require it to EQUAL MGPWireRecHeader::Size.
        // The three double-tailed shapes are SetShaderBuffers (MGPBufferRange[Count] then
        // MGHostSpan[HostSpanCount]), SetStreamOutputTargets (MGPBufferRange[Count] then
        // Uint32[Count]) and DrawVbo (MGPDrawRange[NumDraws] then a conditional MGHostSpan).
        //
        // Returns whether the record was applied. False is reserved for a record this build
        // deliberately does not implement; a MALFORMED record never returns, it Fatals.
        //
        // A kRecPad record must be skipped by the CALLER before this is reached; passing one
        // here Fatals, because a pad that reached the decoder has already been counted.
        Bool DecodeAndApply(const Transport::RingRecordView& record);

        // PH-1 (3): THE PRE-GATE ON ITS OWN, for a caller that reads the record before it hands
        // it to DecodeAndApply. PipeApplier::ApplyOne stamps the verb boundary and computes
        // MGPipeBarriered - which reads payload fields (MGPDrawInfo::Flags) - BEFORE the decode,
        // so a record shorter than its own type, or with an opcode no row names, has to be refused
        // ahead of that read, not only inside DecodeAndApply. True: the record may be read (and
        // DecodeAndApply asks the same questions again, with the same answer). False: an armed
        // session child latched it by name, and it is COUNTED here as a declined record so this
        // decoder's tally stays level with appliedSeq (R-9). Unarmed it always answers true: the
        // generated gate keeps its own death. A pad is let through for DecodeAndApply's own arm.
        Bool AdmitOrDecline(const Transport::RingRecordView& record);

        // THE DECODER'S OWN TALLY, NOT THE SHARED WATERMARK. Advanced by exactly one per
        // applied non-pad record.
        //
        // RingControl::appliedSeq has exactly ONE writer - s1's SessionConsumer::ApplyOne, +1
        // per record, pads never counted - and this class writes NO RingControl field at all.
        // That is deliberate rather than a division of labor: two writers of a watermark is
        // how a waiter resumes on a record the server has not run, which is what R-9's "never
        // publish a watermark early" forbids, and there is no checksum on this ring that would
        // catch it.
        //
        // Keeping a private count beside the session's is what makes the batching ban
        // CHECKABLE instead of merely stated: after every record the two numbers must agree,
        // and a single counter could not tell a batched publish from an honest one.
        Uint64 AppliedSeq() const;

        // v1 installs the backend bridge for contract §7's five class-B verbs. Null - the
        // default - makes those five arms return false rather than invent a semantics.
        void SetVerbSink(WireVerbSink* sink);
        WireVerbSink* VerbSink() const;

        // R-2.5 / rule C's mechanical control: with MOBILEGL_IPC_AUDIT=1 every SEG_STAGE byte
        // this decoder resolved for a record is overwritten with 0xDD once the applier has
        // RETURNED, so an applier that kept the pointer reads 0xDD on the next frame instead
        // of bytes that happen to still be there. Off by default; the run is exact - the
        // decoder poisons what it resolved, not a conservative window.
        void SetAuditPoison(Bool enabled);
        Bool AuditPoison() const;
        // How many staged bytes this decoder has poisoned. Zero with the audit off, and the
        // number a t1 lane asserts is non-zero with it on: an instrumentation that cannot be
        // observed to have run is decoration.
        //
        // WHAT IT ACTUALLY COVERS is "the blob runs the arm resolved", which today is AT MOST
        // ONE per record: tails live in SEG_CMD and are never noted, and CreateShaderState's
        // six per-stage runs are Fatal rather than resolved, so the one archive is the only
        // multi-kilobyte run in the catalog that reaches it. The array is eight deep so a
        // later phase that declares more can fill it without a code change - and overflowing
        // it is Fatal rather than a silent drop, because a poison that quietly stopped
        // covering a run is the same failure as no poison at all.
        Uint64 PoisonedStageBytes() const;

        // R-5's acceptance answer for the four Bool-returning appliers - ResourceCreate,
        // ResourceRespecify, ResourceSubData, SetTextureParams.
        //
        // IT DOES NOT RIDE A REPLY SLOT, and that is a contract conflict this package could
        // not settle on its own. CONTRACT-P5.md table 0's reply-slot-header row says DECLINED
        // "is how the four Bool acceptance entry points say false (R-5)", but PipeCalls.def
        // gives none of those four `kReplySlot` - and table 0 ALSO says kMGPipeCallFlags is
        // what "every package" reads, so s1 will size ReplyPool from it. Writing
        // SEG_REPLY[seq % slots] for a record the pool never reserved a slot for overwrites a
        // waiter's answer, and because the slot header stamps the writer's seq for self-check,
        // the waiter's check then fails FOR EVER and the barrier hangs rather than returning
        // something wrong. So the decoder posts a reply only for rows whose flags say
        // kReplySlot (PostReply itself Fatals otherwise) and exposes the acceptance here
        // instead. The integrator rules on which half of the contract moves.
        Bool LastAcceptanceKnown() const;
        Bool LastAcceptance() const;
        Uint64 AcceptedRecords() const;
        Uint64 DeclinedRecords() const;
        // P12: HOW MANY ANSWERS WERE WRITTEN AND HOW MANY WERE SKIPPED, counted where the
        // decision is made. This is the pair a deferral's viability is read off: only the
        // ANSWERED records compete for the reply pool, so `AnsweredRecords() / creates` is how
        // many replies stand between two deferred answers. Without it the client can see that
        // its drain failed but not whether the server was ever told it could stop answering -
        // and a wire change whose effect is invisible is a wire change nobody can debug.
        Uint64 AnsweredRecords() const;
        Uint64 SkippedAnswers() const;

        // Points MG_Pipe::gMGPipeWireRecordApply at this layer's thunk. Called once from the
        // constructor and modeled on SegmentTable::InstallProcessResolver, which is the same
        // shape for the same reason; Uninstall belongs beside that one at teardown. The thunk
        // is inert without a decoder on the calling thread, so installing it early changes
        // nothing for a monolith caller of MGPipeApplyWireRecord.
        static void InstallApplyHook();
        static void UninstallApplyHook();

    private:
        Bool ApplyChecked(MG_Pipe::MGPWireOp op, const void* record, Uint64 size);
        // nullptr only when the session latched a named fault (PH-1 (3)); unarmed it dies.
        const void* ResolveOrFatal(MG_Pipe::MGPWireOp op, const MG_Pipe::MGPBlobRef& blob);
        Bool NoteResolvedRun(MG_Pipe::MGPWireOp op, const MG_Pipe::MGPBlobRef& blob);
        void PoisonResolvedRuns();
        // The ONLY way this class answers a record. Fatals if `op` carries no kReplySlot -
        // see LastAcceptanceKnown() for why that is a Fatal and not a log line.
        void PostReply(MG_Pipe::MGPWireOp op, Uint64 seq, Int32 status, const void* bytes, Uint64 size);

        friend Bool MGPipeWireRecordApplyThunk(MG_Pipe::MGPWireOp, const void*, Uint64, Uint64);

        Bool m_valid = false;
        SegmentTable* m_segments = nullptr;
        ReplySink* m_replies = nullptr;
        WireVerbSink* m_verbs = nullptr;
        Uint64 m_applySeq = kInvalidSeq;
        Bool m_auditPoison = false;
        Uint64 m_poisonedBytes = 0;
        Bool m_lastAcceptanceKnown = false;
        Bool m_lastAcceptance = false;
        Uint64 m_accepted = 0;
        Uint64 m_declined = 0;
        Uint64 m_answered = 0;
        Uint64 m_skipped = 0;
        // The SEG_STAGE runs the record being applied resolved, for the 0xDD fill. Eight deep
        // so CreateShaderState's seven blob members plus a tail would fit if a later phase
        // declares them; today at most one run is ever noted (see PoisonedStageBytes).
        MG_Pipe::MGPBlobRef m_resolved[8];
        Uint32 m_resolvedCount = 0;
    };

    // The hook MGPipeApplyWireRecord dispatches to once its generated per-opcode bounds gate
    // has passed. Inert unless a decoder is active on the calling thread.
    Bool MGPipeWireRecordApplyThunk(MG_Pipe::MGPWireOp op, const void* record, Uint64 size, Uint64 remaining);

} // namespace MobileGL::MG_Remote::Wire
