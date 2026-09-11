// MobileGL - MobileGL/MG_Remote/Server/PipeApplier.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The server's applier bridge. Owner: package v1, with p1 for the stamp rule. Signatures by c0.
//
// IT IS A BRIDGE, NOT AN APPLIER. The applier already exists and is not edited by this phase:
// MG_Pipe/PipeApply.{h,cpp}, 37 MGPipeApply* free functions. This class owns the three things
// that only exist once records arrive over a wire rather than by direct call:
//
//   1. THE VERB STAMP. This is the phase's prerequisite, and it is not in the ROADMAP row.
//      MGPipeApplyAccess deliberately does not stamp the poison generations
//      (PipeInputs.h:612-618): "a stamp says the filler published this for THIS verb, which is
//      the walk's statement, not the applier's". Under split the filler is in another role, so
//      NOTHING stamps, every FilledGen[] stays 0, MGPipeInputFieldIsFresh returns false for
//      everything, and a pure server aborts on the FIRST read inside SyncRenderState with
//      Fatal{UnmigratedPipeInput, "GetRenderStateParameters@<none>"} - before reaching any
//      interesting case. So: the server stamps at the verb boundary. p1 defines what is
//      stamped and for which verb; v1 places the call. Neither half works alone.
//
//   2. ACCEPTANCE. Four applier entry points return Bool - ResourceCreate, ResourceRespecify,
//      ResourceSubData, SetTextureParams - and MapPersistent returns void*. Those returns are
//      what the CLIENT gates destructive state changes on (clearing per-level dirty flags,
//      latching parameters, adopting a pointer). They go back through the reply slot, id =
//      record seq (R-3), and are collected in the barrier's existing wait (R-5). The client
//      may not recompute any of them.
//
//   3. R-11, THE BORROWED-POINTER RULE. A SEG_STAGE run is valid from publish until retiredSeq
//      passes the record naming it. NO APPLIER ENTRY POINT MAY HOLD A POINTER PAST ITS RETURN.
//      The tree has exactly one violation and it is named: GLESBufferResource::hostBytes
//      (Managers.h:839), written by Ops_H_SubData (Managers.cpp:1980-1983) and Ops_H_FlushRange
//      (:2035), read by six later drains (:2000, :2062, :2080, :2111, :2741, :2843). Under split
//      those two must copy into server-owned storage. MOBILEGL_IPC_AUDIT=1's 0xDD fill (R-2.5)
//      is the mechanical control that says whether they did.

#pragma once
#include <Includes.h>

#include <MG_Pipe/MGPipe.h>

#include "../Transport/Ring.h"
#include "../Wire/PipeWireCodec.h"

namespace MobileGL::MG_Remote::Server {

    // Writes answers into SEG_REPLY at seq % slots, stamping the seq back into the slot header
    // so a wrong-slot read is detectable rather than plausible (table 0's slot header row:
    // {Uint64 Seq; Int32 Status; Uint32 Size;}).
    class ReplyPool final : public Wire::ReplySink {
    public:
        ReplyPool() = default;
        ReplyPool(void* base, Uint64 sizeBytes, Uint32 slotCount, Uint32 slotBytes);

        void PostReply(Uint64 seq, Int32 status, const void* bytes, Uint64 size) override;

        // A reply larger than one slot is Fatal rather than chunked: P5's only large answer is
        // ReadPixels, whose size the client already knows before it emits, so the slot size is
        // chosen from that and an overflow means the two sides disagree about the frame.
        Uint32 SlotBytes() const;

    private:
        Uint8* m_base = nullptr;
        Uint64 m_size = 0;
        Uint32 m_slots = 0;
        Uint32 m_slotBytes = 0;
    };

    class PipeApplier {
    public:
        PipeApplier() = default;
        PipeApplier(Wire::SegmentTable* segments, ReplyPool* replies);

        // Decode one record, stamp the verb, apply, post the reply if the call has one, then
        // advance appliedSeq by exactly one. P5 FORBIDS BATCHING appliedSeq (R-9): the barrier's
        // waiter reads it, and a batched watermark promises work that has not run.
        Bool ApplyOne(const Transport::RingRecordView& record);

        // p1's rule, v1's call site. Called at the verb boundary, before the record's applier
        // runs, with the verb the record belongs to.
        void StampVerbBoundary(MG_Pipe::MGPWireOp op);

        // R-7.2's counter, read by the gate. A BARRIER-PULLED field read on the server side
        // increments PipeStats::CallClass::ResidualPulls (short name `rsp`); its value at the
        // end of P5 IS the size of the P6/P7/P8 debt and goes into MEASUREMENTS.
        Uint64 ResidualPullCount() const;

        // R-11's audit: after a record retires, fill the SEG_STAGE bytes it referenced with
        // 0xDD. Only under MOBILEGL_IPC_AUDIT=1, because it costs a write of every staged byte.
        void PoisonRetiredStageBytes(Uint64 offset, Uint64 size);

    private:
        Wire::SegmentTable* m_segments = nullptr;
        ReplyPool* m_replies = nullptr;
        Wire::PipeWireDecoder m_decoder;
        Uint64 m_residualPulls = 0;
    };

} // namespace MobileGL::MG_Remote::Server
