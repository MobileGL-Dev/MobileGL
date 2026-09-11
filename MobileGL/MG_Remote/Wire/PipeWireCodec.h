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
    void CheckBlobIsHonest(MG_Pipe::MGPWireOp op, const MG_Pipe::MGPBlobRef& blob,
                           const SegmentTable& segments);
    // R-2.3 arm for MGHostSpan. P5's reduced path should produce ZERO host spans
    // (kCapNeedsHostIndexBytes / kCapNeedsHostUboBytes are both 0 in P5, table 0), so this
    // firing at all is a finding, not just a corruption check.
    void CheckHostSpanIsHonest(const MG_Pipe::MGHostSpan& span);

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
        Transport::RingControl* m_control = nullptr;
        Transport::RingProducer* m_cmd = nullptr;
        Transport::RingProducer* m_stage = nullptr;
        SegmentTable* m_segments = nullptr;
        Uint64 m_emitSeq = kInvalidSeq;
        Uint64 m_maxRecordBytes = 0;
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

        // Advanced by exactly one per applied non-pad record. P5 FORBIDS BATCHING IT (R-9):
        // the verb barrier's waiter reads it, and a batched watermark makes the client wait
        // for records the server has not run.
        Uint64 AppliedSeq() const;

    private:
        Transport::RingControl* m_control = nullptr;
        SegmentTable* m_segments = nullptr;
        ReplySink* m_replies = nullptr;
        Uint64 m_applySeq = kInvalidSeq;
    };

} // namespace MobileGL::MG_Remote::Wire
