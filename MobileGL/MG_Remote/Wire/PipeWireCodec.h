// MobileGL - MobileGL/MG_Remote/Wire/PipeWireCodec.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// G3: the MGPipe record codec. Owner: package w1.
//
// This header is the CONTRACT (MG_Remote/CONTRACT-P5.md) in C++ form; P5's c0 package wrote
// it so the other seven could compile on day one against signatures that cannot then move
// under them. Every body below is a named Fatal until w1 lands the real one.
//
// WHAT THIS LAYER IS, AND WHAT IT IS NOT
//
// It turns one MGPipe call into bytes in SEG_CMD (+ SEG_STAGE), and bytes back into ONE CALL
// OF AN EXISTING MGPipeApply* FREE FUNCTION. It owns NO semantics: MG_Pipe/PipeApply.cpp is
// not edited by this package, and a decoder arm that "handles" a record itself rather than
// delegating is a review failure (R-4's rule, one level down).
//
// THE FIVE HONESTY RULES (R-2), because they are what make `inproc` worth running at all.
// In the same address space every shortcut works: MGHostSpan::Ptr dereferences, a blobref
// whose Offset is a host address resolves, and MGPipeApplyMapPersistent's return value is a
// usable pointer. So the codec is held to the SPAWN rules even when it does not need to be:
//   1. encoder writes MGHostSpan::Ptr == nullptr and points Seg/Offset at SEG_STAGE;
//   2. encoder fills a real Seg, a real in-segment Offset and a NON-ZERO Size for every
//      MGPBlobRef that carries content;
//   3. decoder Fatal{ProtocolCorruption} on: Ptr != nullptr; a content record with
//      Blob.Size == 0; Size != 0 with Seg == kSegNone; Offset + Size past the segment;
//   4. MGPipeApplyMapPersistent returns nullptr under split (R-6; b1's half);
//   5. with MOBILEGL_IPC_AUDIT=1 the server fills a retired record's SEG_STAGE bytes with
//      0xDD, so an implementation that kept a pointer past apply reads 0xDD next frame.
//
// SEQ. The record ordinal IS the sequence number and IS the reply-slot id (R-3): there is no
// per-record seq field on the wire (ARCHITECTURE.md:124) and no second id space. Seq is
// 1-based so that 0 can mean "nothing encoded". A kRecPad wrap filler DOES NOT ADVANCE SEQ -
// both sides must skip it before counting, or every ring wrap offsets the two sides'
// numbering permanently and nothing checksums it (R-9, Ring.h's header).

#pragma once
#include <Includes.h>

#include <MG_Pipe/MGPipe.h>

#include "../Transport/Ring.h"

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
        void Install(SegmentId seg, SegmentView view);
        SegmentView Get(SegmentId seg) const;

        // Bounds-checked resolve. Returns nullptr when seg is unknown, size is 0, or
        // offset + size runs past the segment; the CALLER escalates that to
        // Fatal{ProtocolCorruption} (R-2.3) rather than this returning into a Fatal, so a
        // unit test can exercise the arithmetic without dying.
        const void* Resolve(Uint32 seg, Uint64 offset, Uint64 size) const;

        // Points MG_Pipe::gMGPipeSegmentResolver at this table. Server role only; asserts if
        // a resolver is already installed, because two roles racing on one inline variable is
        // the failure this function exists to make loud.
        void InstallProcessResolver();
        static void UninstallProcessResolver();

    private:
        SegmentView m_views[kSegAdopt + 1];
    };

    // ---- the four Fatal arms, worded once ----------------------------------------------
    //
    // One function so encoder, decoder and every package's own bounds check produce the SAME
    // log line. `what` is the record or field; `detail` is the number that was wrong.
    [[noreturn]] void WireProtocolFatal(const char* what, const char* detail);
    [[noreturn]] void WireProtocolFatalAt(const char* what, Uint64 got, Uint64 expected);

    // R-2.3 arms 1-4 over one record's blobref. Split only; a monolith emission is exempt by
    // construction because it never reaches this layer.
    //
    // ARMS 3 AND 4 ONLY, PLUS ONE THIS FUNCTION HAD TO INVENT. A blobref is honest when it is
    // EITHER fully declared - a real Seg, a non-zero Size and an Offset+Size inside that
    // segment - OR fully absent, which is all three fields zero. The third shape, a Seg or an
    // Offset with Size == 0, is neither, and it is the shape a monolith emitter produces
    // today (Seg = None, Offset = a host address, Size = 0), so under split it has to be
    // Fatal rather than "absent": a decoder that read it as absent would silently drop the
    // bytes of every record an unconverted emitter sent.
    //
    // ARM 2 - "Blob.Size == 0 on a CONTENT record" - is NOT here and cannot be: whether a
    // record carries content is a property of the record's OTHER fields (ChunkMask, the
    // destination range, GlobalUboSize, the stage mask), which this signature does not see.
    // RequireDeclaredBlob below is arm 2, and the decoder's per-op arm calls it exactly where
    // the payload says content is implied.
    void CheckBlobIsHonest(MG_Pipe::MGPWireOp op, const MG_Pipe::MGPBlobRef& blob,
                           const SegmentTable& segments);
    // R-2.3 arm 2: the record's other fields say it carries content, so the blob must be
    // declared. Fatal on an absent blob, then CheckBlobIsHonest on a present one.
    void RequireDeclaredBlob(MG_Pipe::MGPWireOp op, const MG_Pipe::MGPBlobRef& blob,
                             const SegmentTable& segments);
    // R-2.3 arm for MGHostSpan. P5's reduced path should produce ZERO host spans
    // (kCapNeedsHostIndexBytes / kCapNeedsHostUboBytes are both 0 in P5, table 0), so this
    // firing at all is a finding, not just a corruption check.
    void CheckHostSpanIsHonest(const MG_Pipe::MGHostSpan& span);

    // ---- one record's shape, computed ONCE and read by both sides ----------------------
    //
    // THE TAIL CROSS-CHECK LIVES HERE AND NOWHERE ELSE (BRIEF §5 w1, contract table 1 group
    // B). MGP_WIRE_CHECK_BOUNDS only proves `size >= sizeof(MGPWireRec_X)` - IT CANNOT SEE
    // THE TAIL - so a record declaring Count = 4000 while carrying 8 bytes passes it. The
    // encoder computes this layout from the payload it is about to write and REFUSES a caller
    // whose tails disagree; the decoder computes the same layout from the payload it just
    // received and REFUSES a record whose MGPWireRecHeader::Size disagrees. Two readers, one
    // arithmetic, so the two sides cannot drift.
    //
    // EVERY TAIL STARTS 8-BYTE ALIGNED WITHIN THE RECORD, and the encoder zero-fills the gap.
    // Eight of the nine kVarTail rows are already aligned by construction (their payload and
    // element sizes are multiples of 8); DrawVbo is not - MGPDrawRange is TWELVE bytes, so an
    // odd NumDraws leaves the conditional MGHostSpan on a 4-byte boundary, and MGHostSpan
    // holds a pointer and two Uint64s. P5 emits no host span at all, so this rule costs
    // nothing now and is stated now because the phase that arms kDrawHasUserIndices would
    // otherwise have to discover it as a misaligned load on a device.
    struct WireRecordLayout {
        Uint64 PayloadBytes = 0;   // sizeof the op's payload struct
        Uint64 TailOffset[2] = {0, 0}; // from the START of the record, header included
        Uint64 TailBytes[2] = {0, 0};
        Uint32 TailCount = 0;
        Uint64 TotalBytes = 0; // header + payload + gaps + tails, rounded up to 8
    };

    // `payload` must already be known to hold at least the op's payload struct - that is what
    // MGP_WIRE_CHECK_BOUNDS proves, and this function is only ever called after it. Returns
    // false for an opcode outside the catalogue; a count past its own GL bound is Fatal,
    // because a decoder holding such a record has nothing safe left to do with it.
    Bool MGPipeWireRecordLayout(MG_Pipe::MGPWireOp op, const void* payload, WireRecordLayout& out);

    // The catalogue's own spelling of an opcode, for a Fatal line. Out of range is "<opcode>".
    const char* WireOpName(MG_Pipe::MGPWireOp op);

    // One tail array. Two of the 71 rows carry two (SetShaderBuffers, SetStreamOutputTargets)
    // and DrawVbo carries a conditional second one, which is why EncodeRecord's one-tail form
    // could not stay the only one.
    struct WireTail {
        const void* Bytes = nullptr;
        Uint64 Size = 0;
    };

    // ---- encoder -----------------------------------------------------------------------
    //
    // Not thread safe: one encoder per client context, driven by the GL thread, by
    // construction (SPSC is the ring's contract too).
    class PipeWireEncoder {
    public:
        PipeWireEncoder() = default;
        PipeWireEncoder(Transport::RingControl* control, Transport::RingProducer* cmd,
                        Transport::RingProducer* stage, SegmentTable* segments);

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
        // wait: R-10 says P5 does no chunking and must instead PROVE it never needs any, so
        // this is where the proof fails loudly if it is wrong. MaxRecordBytesSeen() is the
        // counter that feeds that proof into MEASUREMENTS.
        Uint64 EncodeRecord(MG_Pipe::MGPWireOp op, const void* payload, Uint64 payloadBytes,
                            const void* varTail = nullptr, Uint64 varTailBytes = 0);

        // The same call for the three rows that carry TWO tails. The one-tail form above is
        // this one with tailCount <= 1; nothing is duplicated between them.
        //
        // The tails a caller hands over are CROSS-CHECKED against the layout the payload
        // itself declares (MGPipeWireRecordLayout): a caller whose Count says 4000 while its
        // tail holds 8 bytes is Fatal HERE, on the producing side, rather than on a peer that
        // can only report a corrupt stream. That is the same arithmetic the decoder runs, so
        // the check is real rather than a restatement of the caller's own belief.
        Uint64 EncodeRecord(MG_Pipe::MGPWireOp op, const void* payload, Uint64 payloadBytes,
                            const WireTail* tails, Uint32 tailCount);

        // Releases every SEG_STAGE run named by a record the apply side has RETIRED
        // (RingControl::retiredSeq, R-9). Called by the client at its verb barrier and
        // whenever StageBytes runs short; the allocator reclaims behind retiredSeq and
        // nothing else may (table 1's "retires" column, R-11).
        //
        // THE MARK IS HELD ON THIS SIDE, NOT ON THE WIRE. A record does not carry where its
        // staged bytes end, so the encoder remembers {seq, stage cursor} per record and
        // reclaims to the newest mark whose seq the server has retired. That is exact, needs
        // no wire field, and does not depend on the verb barrier - so it keeps working when
        // R-1's barrier retires family by family.
        void ReclaimStagedBytes();
        // Staged bytes not yet reclaimed. The number MOBILEGL_IPC_STAGE_MB has to cover.
        Uint64 StagedBytesInFlight() const;

        // Release-stores the head cursor, then rings the consumer doorbell IF PARKED. The
        // order is pinned by RingTest.cpp:446 and must not be swapped: notify-then-publish
        // loses the wakeup.
        void Publish();

        // The highest seq this encoder has produced. The verb barrier (R-1) waits for
        // RingControl::appliedSeq to reach it.
        Uint64 EmitSeq() const;

        // R-10's proof obligation: the largest single record this encoder has written.
        Uint64 MaxRecordBytesSeen() const;

    private:
        // {the record's seq, the SEG_STAGE cursor just past everything that record named}.
        struct StageMark {
            Uint64 Seq = 0;
            Uint64 StageCursor = 0;
        };

        Transport::RingControl* m_control = nullptr;
        Transport::RingProducer* m_cmd = nullptr;
        Transport::RingProducer* m_stage = nullptr;
        SegmentTable* m_segments = nullptr;
        Uint64 m_emitSeq = kInvalidSeq;
        Uint64 m_maxRecordBytes = 0;
        Vector<StageMark> m_stageMarks;
        SizeT m_stageMarkFront = 0;
        Uint64 m_stageReclaimed = 0;
    };

    // ---- decoder -----------------------------------------------------------------------

    // Where a kReplySlot answer goes. Declared HERE and not in Server/ so the codec does not
    // depend on the server session: the decoder's job ends at "produce the answer bytes".
    //
    // The slot is addressed seq % slots and the server writes the seq back into the slot
    // header for self-check (table 0's slot header row). Status: 0 = OK, 1 = DECLINED,
    // 2 = ERROR. DECLINED IS A REAL ANSWER, not a failure - it is how MapPersistent says
    // nullptr (R-6) and how the four Bool acceptance entry points say false (R-5).
    class ReplySink {
    public:
        virtual ~ReplySink() = default;
        static constexpr Int32 kStatusOk = 0;
        static constexpr Int32 kStatusDeclined = 1;
        static constexpr Int32 kStatusError = 2;
        virtual void PostReply(Uint64 seq, Int32 status, const void* bytes, Uint64 size) = 0;
    };

    // ---- the verb sink: the five rows with no MGPipeApply* to delegate to ---------------
    //
    // The decoder owns NO semantics, so every arm ends in an existing MGPipeApply* free
    // function - except five, and they are exactly contract §7's class B: Clear (57), Blit
    // (56), ReadPixels (58), DrawVbo (59) and Present (67). Those are GLFunctionsTable VERBS.
    // MG_Pipe has no applier for any of them (the 37 MGPipeApply* entry points are the object
    // and state families), so for these five P5 writes the first consumer as well as the first
    // producer - and the consumer is the SERVER'S backend call, which is v1's, not the codec's.
    //
    // So the codec does what it can prove and stops there: it bounds-checks, cross-checks the
    // tail, resolves the segments and hands over a DECODED, VALIDATED argument list. With no
    // sink installed those five arms return false ("this build does not implement it"), which
    // is the same answer the other unimplemented rows give.
    //
    // SetShaderBuffers (38) and SetStreamOutputTargets (39) also have no applier entry point,
    // and they deliberately get NO sink method: both are off P5's reduced path (BRIEF §4's
    // exclusion list), so inventing a consumer for them would be building a semantics nobody
    // can test this phase. Their arms validate both tails - which is the part a later phase
    // must not have to re-derive - and return false.
    class WireVerbSink {
    public:
        virtual ~WireVerbSink() = default;
        virtual Bool OnClear(const MG_Pipe::MGPClear& clear) {
            (void)clear;
            return false;
        }
        virtual Bool OnBlit(const MG_Pipe::MGPBlit& blit) {
            (void)blit;
            return false;
        }
        virtual Bool OnPresent(const MG_Pipe::MGPPresent& present) {
            (void)present;
            return false;
        }
        // The pixels go back in the reply slot (contract table 1 row 23: the destination is
        // ALWAYS SEG_REPLY in P5, which is why MGPReadbackInfo gains no Seg field), so the
        // sink is handed the seq and the sink it must answer into.
        virtual Bool OnReadPixels(const MG_Pipe::MGPReadbackInfo& info, Uint64 seq, ReplySink* replies) {
            (void)info;
            (void)seq;
            (void)replies;
            return false;
        }
        // `ranges` is info.NumDraws entries. `userIndices` is null unless the record set
        // kDrawHasUserIndices - which P5 never does, because the reduced path draws from a
        // VBO precisely so no MGHostSpan is produced (table 0's cap-bit row).
        virtual Bool OnDrawVbo(const MG_Pipe::MGPDrawInfo& info, const MG_Pipe::MGPDrawRange* ranges,
                               const MG_Pipe::MGHostSpan* userIndices) {
            (void)info;
            (void)ranges;
            (void)userIndices;
            return false;
        }
    };

    // Not thread safe: one decoder on the apply thread, by construction.
    class PipeWireDecoder {
    public:
        PipeWireDecoder() = default;
        PipeWireDecoder(Transport::RingControl* control, SegmentTable* segments,
                        ReplySink* replies);

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

        // THE DECODER'S OWN TALLY, NOT THE SHARED WATERMARK. Advanced by exactly one per
        // applied non-pad record.
        //
        // RingControl::appliedSeq has exactly ONE writer - s1's SessionConsumer::ApplyOne, +1
        // per record, pads never counted - and this class writes NO RingControl field at all.
        // That is deliberate rather than a division of labour: two writers of a watermark is
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
        Uint64 PoisonedStageBytes() const;

    private:
        Bool ApplyChecked(MG_Pipe::MGPWireOp op, const void* record, Uint64 size);
        const void* ResolveOrFatal(MG_Pipe::MGPWireOp op, const MG_Pipe::MGPBlobRef& blob);
        void NoteResolvedRun(const MG_Pipe::MGPBlobRef& blob);
        void PoisonResolvedRuns();

        friend Bool MGPipeWireRecordApplyThunk(MG_Pipe::MGPWireOp, const void*, Uint64, Uint64);

        Transport::RingControl* m_control = nullptr;
        SegmentTable* m_segments = nullptr;
        ReplySink* m_replies = nullptr;
        WireVerbSink* m_verbs = nullptr;
        Uint64 m_applySeq = kInvalidSeq;
        Bool m_auditPoison = false;
        Uint64 m_poisonedBytes = 0;
        // The SEG_STAGE runs the record being applied resolved, for the 0xDD fill. At most
        // seven (CreateShaderState's blob members) plus one tail.
        MG_Pipe::MGPBlobRef m_resolved[8];
        Uint32 m_resolvedCount = 0;
    };

    // The hook MGPipeApplyWireRecord dispatches to once its generated per-opcode bounds gate
    // has passed. Installed by DecodeAndApply on the thread that decodes.
    Bool MGPipeWireRecordApplyThunk(MG_Pipe::MGPWireOp op, const void* record, Uint64 size,
                                    Uint64 remaining);

} // namespace MobileGL::MG_Remote::Wire
