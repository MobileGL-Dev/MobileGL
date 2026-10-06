// MobileGL - MobileGL/MG_Pipe/PipeVerbSink.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P13 W5: THE VERB SINK AND ITS REPLIES, WITHOUT THE WIRE. These were the wire codec's
// (MG_Remote/Wire/PipeWireCodec.h, which keeps their old names as aliases): the decoder hands each
// validated verb to a sink, and the sink answers through a reply sink. The record arm's in-process
// port does exactly the same in a library that has no wire, so the interfaces live here.

#pragma once

#include <Includes.h>
#include <MG_Pipe/MGPipe.h>

namespace MobileGL::MG_Pipe {

    // One tail array. Two of the rows carry two (SetShaderBuffers, SetStreamOutputTargets),
    // DrawVbo carries a conditional second one and P5e's SetProgramBindings carries three,
    // which is why EncodeRecord's one-tail form could not stay the only one.
    struct MGPipeVerbTail {
        const void* Bytes = nullptr;
        Uint64 Size = 0;
    };

    // Where a kReplySlot answer goes. Declared HERE and not in Server/ so the codec does not
    // depend on the server session: the decoder's job ends at "produce the answer bytes".
    //
    // The slot is addressed seq % slots and the server writes the seq back into the slot
    // header for self-check (table 0's slot header row). Status: 0 = OK, 1 = DECLINED,
    // 2 = ERROR. DECLINED IS A REAL ANSWER, not a failure - it is how MapPersistent says
    // nullptr (R-6) and how the four Bool acceptance entry points say false (R-5).
    class MGPipeReplySink {
    public:
        virtual ~MGPipeReplySink() = default;
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
    struct MGPipeQueryResultReply {
        Uint64 Value = 0;
        Uint32 Produced = 0;
        Uint32 Reserved = 0;
    };
    static_assert(sizeof(MGPipeQueryResultReply) == 16);

    class MGPipeVerbSink {
    public:
        virtual ~MGPipeVerbSink() = default;
        // P11 B2 (CONTRACT-P11.md B2): map_persistent in a session that runs T0. True = the sink
        // handled it and `status` is the answer (OK: the client's AHardwareBuffer is now the
        // store; DECLINED: the store runs T2); false = not a T0 session, and the codec gives
        // today's constant decline. `seq` is the record's, which is what the store's Offer names.
        virtual Bool OnMapPersistent(const MGPHandleOnly&, Uint64 /*seq*/, Int32& /*status*/) {
            return false;
        }
        virtual Bool OnFenceCreate(const MGPHandleOnly&) { return false; }
        virtual Bool OnFenceDestroy(const MGPHandleOnly&) { return false; }
        virtual Bool OnFenceStatus(const MGPHandleOnly&, Uint32&) { return false; }
        virtual Bool OnFenceWait(const MGPFenceWait&, Uint32&) { return false; }
        virtual Bool OnFenceWaitServer(const MGPFenceWait&) { return false; }
        virtual Bool OnQueryCreate(const MGPQueryDesc&) { return false; }
        virtual Bool OnQueryBegin(const MGPQueryDesc&) { return false; }
        virtual Bool OnQueryEnd(const MGPQueryDesc&) { return false; }
        virtual Bool OnQueryCounter(const MGPQueryDesc&) { return false; }
        virtual Bool OnQueryAvailable(const MGPHandleOnly&, Uint32&) { return false; }
        virtual Bool OnQueryResult(const MGPQueryResultRequest&, MGPipeQueryResultReply&) { return false; }
        virtual Bool OnQueryDestroy(const MGPHandleOnly&) { return false; }
        virtual Bool OnQueryTimestamp(const MGPTimestampRequest&, Int64&) { return false; }
        virtual Bool OnDeleteStreamOutput(const MGPStreamOutputBind&) { return false; }
        virtual Bool OnClear(const MGPClear& clear) {
            (void)clear;
            return false;
        }
        virtual Bool OnBlit(const MGPBlit& blit) {
            (void)blit;
            return false;
        }
        virtual Bool OnPresent(const MGPPresent& present) {
            (void)present;
            return false;
        }
        // The pixels go back in the reply slot (contract table 1 row 23: the destination is
        // ALWAYS SEG_REPLY in P5, which is why MGPReadbackInfo gains no Seg field), so the
        // sink is handed the seq and the sink it must answer into.
        virtual Bool OnReadPixels(const MGPReadbackInfo& info, Uint64 seq, MGPipeReplySink* replies) {
            (void)info;
            (void)seq;
            (void)replies;
            return false;
        }
        virtual Bool OnGetTextureImage(const MGPReadbackInfo&, Uint64, MGPipeReplySink*) {
            return false;
        }
        // P9 (CONTRACT-P9.md §1): the same two reads with a pack buffer as the destination. No
        // seq and no sink - nothing is answered; the rows land in `Dst` on this side.
        virtual Bool OnReadPixelsToBuffer(const MGPReadbackToBuffer&) { return false; }
        virtual Bool OnGetTextureImageToBuffer(const MGPReadbackToBuffer&) { return false; }
        // `ranges` is info.NumDraws entries. `userIndices` is null unless the record set
        // kDrawHasUserIndices; `indirect` is null unless it set kDrawIsIndirect (P5b d1,
        // CONTRACT-P5B.md). The layout refuses a record that sets both, so at most one of the
        // two is non-null. The span is VALIDATED (all four R-2 arms, the segment-range one
        // included) and names a SEG_STAGE run the client staged; the sink resolves it through
        // MGPipeHostBytes and never holds the pointer past its return (rule C).
        virtual Bool OnDrawVbo(const MGPDrawInfo& info, const MGPDrawRange* ranges,
                               const MGHostSpan* userIndices,
                               const MGPDrawIndirect* indirect) {
            (void)info;
            (void)ranges;
            (void)userIndices;
            (void)indirect;
            return false;
        }

        // ---- P5b (MG_Remote/CONTRACT-P5B.md): the rows the four migration packages consume.
        //
        // Every one below is a GLFunctionsTable verb with NO MGPipeApply* entry point - the
        // census's correction - so, exactly like the five above, the codec validates and hands
        // over and the SERVER'S sink (Server/PipeApplier.cpp's ServerVerbSink) makes the
        // backend call. The default bodies return false ("this build does not implement it");
        // ServerVerbSink's stubs die Fatal{UnmigratedVerb, "<GL slot>"} by name until the owning
        // package lands the real body, so a client that flips a slot ahead of its server half
        // aborts with the same line the census greps rather than rendering nothing.
        //
        //   i1  OnLaunchGrid, OnMemoryBarrier, OnResourceCopyRegion, OnBindShaderImage,
        //       OnSetStorageBlockBinding
        //   t2  OnBeginStreamOutput, OnEndStreamOutput, OnPauseStreamOutput,
        //       OnResumeStreamOutput, OnBindStreamOutput, OnPatchParameter
        //   f1  OnGenerateMipmap, OnCopyFramebufferToTexture (and OnClear's non-Whole kinds)
        //   d1  OnDrawVbo's indirect tail and user-index span (above)
        virtual Bool OnLaunchGrid(const MGPGridInfo& grid) {
            (void)grid;
            return false;
        }
        virtual Bool OnMemoryBarrier(const MGPMemoryBarrier& barrier) {
            (void)barrier;
            return false;
        }
        virtual Bool OnResourceCopyRegion(const MGPCopyRegion& copy) {
            (void)copy;
            return false;
        }
        virtual Bool OnBindShaderImage(const MGPImageBind& bind) {
            (void)bind;
            return false;
        }
        // `name` is the NUL-terminated block name the decoder copied out of the record's
        // SEG_STAGE blob; valid for the call only.
        virtual Bool OnSetStorageBlockBinding(const MGPStorageBlockBinding& binding,
                                              const char* name) {
            (void)binding;
            (void)name;
            return false;
        }
        virtual Bool OnBeginStreamOutput(const MGPStreamOutputBegin& begin) {
            (void)begin;
            return false;
        }
        virtual Bool OnEndStreamOutput(const MGPXfbAccounting& accounting) {
            (void)accounting;
            return false;
        }
        virtual Bool OnPauseStreamOutput(const MGPStreamOutputControl& control) {
            (void)control;
            return false;
        }
        virtual Bool OnResumeStreamOutput(const MGPStreamOutputControl& control) {
            (void)control;
            return false;
        }
        virtual Bool OnBindStreamOutput(const MGPStreamOutputBind& bind) {
            (void)bind;
            return false;
        }
        virtual Bool OnPatchParameter(const MGPPatchParameter& patch) {
            (void)patch;
            return false;
        }
        virtual Bool OnGenerateMipmap(const MGPMipPlan& plan) {
            (void)plan;
            return false;
        }
        virtual Bool OnCopyFramebufferToTexture(const MGPCopyFromFramebuffer& copy) {
            (void)copy;
            return false;
        }

        // ---- P5c (MG_Remote/CONTRACT-P5C.md §5): the two control records, opcodes 77..78.
        //
        // Same hand-over shape as the P5b rows: the codec validates the record (a fixed-size
        // POD, no blob, no tail) and the SERVER'S sink does the work - OnApplierReset runs the
        // server's own MGPipeApplierReset() after asserting ContextSerial against the
        // session's (§5.1: ASSERTED, never dispatched on, P5c has one context per session),
        // and OnObjectDeath releases the kind's twin table by the handle the record carried
        // (§5.2). The default bodies return false ("this build does not implement it"), so a
        // unit decoder without a server declines by the same answer every other unimplemented
        // row gives.
        virtual Bool OnApplierReset(const MGPApplierReset& reset) {
            (void)reset;
            return false;
        }
        virtual Bool OnObjectDeath(const MGPHandleOnly& death) {
            (void)death;
            return false;
        }

        // ---- P14 S1 (docs/Disaggregated/design/11-state-ownership.md): bind_context, opcode 84.
        //
        // The applier_reset hand-over shape, one plane over: a fixed-size POD the bounds gate has
        // already proved, no blob, no tail, no reply, so the codec validates nothing further and
        // the SERVER'S sink does the work - the session's current context token becomes the
        // record's. It is not an assertion like OnApplierReset's serial: the token is minted by
        // the client's EGL layer and the server has no independent count of it, so an unknown
        // token is the sink's own refusal to make and not a shape the codec can check.
        virtual Bool OnBindContext(const MGPBindContext& bind) {
            (void)bind;
            return false;
        }

        // ---- shared images, opcode 85 (docs/Disaggregated/notes/anland/plan-ahb-dmabuf.md).
        // `seq` is the record's reply-slot id, which an Import's descriptor names on the aux
        // socket. True fills `reply`; false is a DECLINED answer.
        virtual Bool OnSharedImage(const MGPSharedImageOp& op, Uint64 seq,
                                   MGPSharedImageReply& reply) {
            (void)op;
            (void)seq;
            (void)reply;
            return false;
        }
    };

} // namespace MobileGL::MG_Pipe
