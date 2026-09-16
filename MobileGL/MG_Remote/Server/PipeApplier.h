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

#include <MG_Backend/BackendObject.h>
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

    // ---- MGPClear's two discriminants ---------------------------------------------------
    //
    // P5b MOVED THEM INTO MGPipeTypes.h (kMGPipeClearKind* / kMGPipeClearValueClass*), which is
    // where this file said they belonged: through P5 the numbers lived here and in the client's
    // EmitTables.h as two hand-minted copies, and a disagreement between them is a clear of the
    // wrong attachment with the wrong value type, which renders plausibly. These are ALIASES so
    // v1's bodies read unchanged; new code names the MG_Pipe constants directly.
    inline constexpr Uint32 kMGPClearKindWhole = MG_Pipe::kMGPipeClearKindWhole;
    inline constexpr Uint32 kMGPClearKindColor = MG_Pipe::kMGPipeClearKindColor;
    inline constexpr Uint32 kMGPClearKindDepth = MG_Pipe::kMGPipeClearKindDepth;
    inline constexpr Uint32 kMGPClearKindStencil = MG_Pipe::kMGPipeClearKindStencil;
    inline constexpr Uint32 kMGPClearKindDepthStencil = MG_Pipe::kMGPipeClearKindDepthStencil;
    inline constexpr Uint32 kMGPClearValueClassFloat = MG_Pipe::kMGPipeClearValueClassFloat;
    inline constexpr Uint32 kMGPClearValueClassInt = MG_Pipe::kMGPipeClearValueClassInt;
    inline constexpr Uint32 kMGPClearValueClassUint = MG_Pipe::kMGPipeClearValueClassUint;

    // ---- the five class-B verbs' consumer ------------------------------------------------
    //
    // Contract §7 class B is Clear (57), Blit (56), ReadPixels (58), DrawVbo (59) and Present
    // (67), and NONE of them has an MGPipeApply* entry point - the 37 that exist are the object
    // and state families. So w1's decoder validates and hands over a checked argument list and
    // stops, and this is the other half: the SERVER'S OWN BACKEND CALL, through the private
    // GlobalBackendFunctionsTable ServerLoop holds. It is not gBackendFunctionsTable, which in
    // a split process is the client's emit table (table 3) - calling THAT here would re-emit
    // the record the server is in the middle of applying, which is an infinite loop that
    // renders nothing and looks like a hang.
    class ServerVerbSink final : public Wire::WireVerbSink {
    public:
        // The server's private backend. Null until ServerLoop::CreateBackend has run, and a
        // verb that arrives before then declines by name rather than dereferencing.
        void SetBackend(MG_Backend::BackendObject* backend);

        Bool OnClear(const MG_Pipe::MGPClear& clear) override;
        Bool OnBlit(const MG_Pipe::MGPBlit& blit) override;
        Bool OnPresent(const MG_Pipe::MGPPresent& present) override;
        Bool OnReadPixels(const MG_Pipe::MGPReadbackInfo& info, Uint64 seq,
                          Wire::ReplySink* replies) override;
        Bool OnDrawVbo(const MG_Pipe::MGPDrawInfo& info, const MG_Pipe::MGPDrawRange* ranges,
                       const MG_Pipe::MGHostSpan* userIndices,
                       const MG_Pipe::MGPDrawIndirect* indirect) override;

        // ---- P5b (MG_Remote/CONTRACT-P5B.md): one override per row a migration package owns.
        // At the contract commit EVERY BODY BELOW IS A STUB that dies
        // Fatal{UnmigratedVerb, "<GL slot>"} by the slot's own name - the same line the client's
        // class-C table raises and the census greps - so a client flipped ahead of its server
        // half aborts by name rather than rendering nothing, and the census on this head is
        // unchanged (the client refuses first). The owning package replaces the body.
        //
        //   i1  OnLaunchGrid ("DispatchCompute"), OnMemoryBarrier, OnResourceCopyRegion
        //       ("CopyImageSubData"), OnBindShaderImage ("BindImageTexture"),
        //       OnSetStorageBlockBinding ("ShaderStorageBlockBinding")
        //   t2  OnBeginStreamOutput / OnEndStreamOutput / OnPauseStreamOutput /
        //       OnResumeStreamOutput ("*TransformFeedback"), OnBindStreamOutput
        //       ("BindTransformFeedback"), OnPatchParameter ("PatchParameteri")
        //   f1  OnGenerateMipmap, OnCopyFramebufferToTexture ("CopyTexImage2D" /
        //       "CopyTexSubImage2D"), and OnClear's four non-Whole kinds (live already)
        //   d1  OnDrawVbo above: the indirect tail, the user-index span, NumDraws > 1 and the
        //       instanced arms - LIVE since d1 v1 (every shape the client's nineteen draw slots
        //       produce dispatches to the backend slot CONTRACT-P5B.md §2 d1 names)
        Bool OnLaunchGrid(const MG_Pipe::MGPGridInfo& grid) override;
        Bool OnMemoryBarrier(const MG_Pipe::MGPMemoryBarrier& barrier) override;
        Bool OnResourceCopyRegion(const MG_Pipe::MGPCopyRegion& copy) override;
        Bool OnBindShaderImage(const MG_Pipe::MGPImageBind& bind) override;
        Bool OnSetStorageBlockBinding(const MG_Pipe::MGPStorageBlockBinding& binding,
                                      const char* name) override;
        Bool OnBeginStreamOutput(const MG_Pipe::MGPStreamOutputBegin& begin) override;
        Bool OnEndStreamOutput(const MG_Pipe::MGPXfbAccounting& accounting) override;
        Bool OnPauseStreamOutput(const MG_Pipe::MGPStreamOutputControl& control) override;
        Bool OnResumeStreamOutput(const MG_Pipe::MGPStreamOutputControl& control) override;
        Bool OnBindStreamOutput(const MG_Pipe::MGPStreamOutputBind& bind) override;
        Bool OnPatchParameter(const MG_Pipe::MGPPatchParameter& patch) override;
        Bool OnGenerateMipmap(const MG_Pipe::MGPMipPlan& plan) override;
        Bool OnCopyFramebufferToTexture(const MG_Pipe::MGPCopyFromFramebuffer& copy) override;

        // Per-verb tallies. The lane asserts these moved, because "the scenario passed" on a
        // split build is also what a scenario that ran entirely on the monolith path looks
        // like (R-16: a probe may not arm against a stub).
        Uint64 Clears() const { return m_clears; }
        Uint64 Draws() const { return m_draws; }
        Uint64 Readbacks() const { return m_readbacks; }
        Uint64 Blits() const { return m_blits; }
        Uint64 Presents() const { return m_presents; }
        Uint64 LastPresentSerial() const { return m_lastPresentSerial; }
        Uint64 ReadbackBytes() const { return m_readbackBytes; }
        // ID-49's tight-size control reads this: the scratch a read_pixels grew to. It must equal
        // the tight w*h*bpp extent of the read, never the client's DstSize - a scratch sized from
        // DstSize is exactly the heap overflow codex 1 found, one field over.
        Uint64 ReadbackScratchBytes() const { return static_cast<Uint64>(m_readbackScratch.size()); }

        // ---- P5b d1: what the LAST draw_vbo record carried, as the sink saw it ---------------
        //
        // Recorded BEFORE the backend is consulted, so a process with no backend object (every
        // unit case) can still assert the wire's fields rather than only that a draw "was
        // declined": the record's head, its first range, its indirect block, and whether a
        // user-index span rode with it and how many bytes it named. Nothing here outlives the
        // call except these copies (rule C: the pointers the sink was handed are not kept).
        struct LastDrawRecord {
            MG_Pipe::MGPDrawInfo Info{};
            MG_Pipe::MGPDrawRange FirstRange{};
            MG_Pipe::MGPDrawIndirect Indirect{};
            Uint64 UserIndexBytes = 0;
            Bool HadUserIndices = false;
            Bool HadIndirect = false;
        };
        const LastDrawRecord& LastDraw() const { return m_lastDraw; }
        // draw_vbo records seen, applied or declined; Draws() above counts only the applied.
        Uint64 DrawRecords() const { return m_drawRecords; }

    private:
        const MG_Backend::GlobalBackendFunctionsTable* Table(const char* verb) const;

        MG_Backend::BackendObject* m_backend = nullptr;
        Uint64 m_clears = 0;
        Uint64 m_draws = 0;
        Uint64 m_readbacks = 0;
        Uint64 m_blits = 0;
        Uint64 m_presents = 0;
        Uint64 m_lastPresentSerial = 0;
        Uint64 m_readbackBytes = 0;
        // ReadPixels' destination. The pixels go into the reply slot, but GLFunctionsTable::
        // ReadPixels writes into a caller buffer, so one staging vector per session sits
        // between them. Grown, never shrunk, and never handed out past the call.
        Vector<Uint8> m_readbackScratch;
        // P5b d1: the multi-draw arrays the glMultiDraw* slots take, rebuilt from the ranges
        // per record (rule C: bounded by NumDraws, owned here, never handed out past the call),
        // and the last-record witness above.
        Vector<GLsizei> m_multiCounts;
        Vector<GLint> m_multiFirsts;
        Vector<const void*> m_multiOffsets;
        Vector<GLint> m_multiBaseVertices;
        LastDrawRecord m_lastDraw{};
        Uint64 m_drawRecords = 0;
    };

    class PipeApplier {
    public:
        PipeApplier() = default;
        PipeApplier(Wire::SegmentTable* segments, ReplyPool* replies);

        // Builds the decoder over the session's control page and points it at this applier's
        // verb sink. Separate from the constructor because ServerSession::Accept constructs the
        // applier before it has decided anything about the apply thread, and the decoder needs
        // the RingControl the constructor was never given.
        //
        // CALLED ON THE APPLY THREAD, ONCE, BEFORE THE FIRST RECORD. PipeWireDecoder is "not
        // thread safe: one decoder on the apply thread, by construction", and its constructor
        // installs the process-wide apply hook.
        void Attach(Transport::RingControl* control, MG_Backend::BackendObject* backend);
        void Detach();
        Bool Attached() const;

        // Decode one record, stamp the verb, apply, post the reply if the call has one. THE
        // CALLER advances appliedSeq by exactly one, through s1's SessionConsumer::ApplyOne,
        // which is that watermark's single writer; P5 FORBIDS BATCHING it (R-9), because the
        // barrier's waiter reads it and a batched watermark promises work that has not run.
        Bool ApplyOne(const Transport::RingRecordView& record);

        // p1's rule, v1's call site. Called at the verb boundary, before the record's applier
        // runs, with the verb the record belongs to.
        void StampVerbBoundary(MG_Pipe::MGPWireOp op);

        // MANDATORY on leaving the applier (p1's M-5, PipeInputs.h's ServerStampedVerb block).
        // The client's MGPipeValidateForVerb / MGPipeLeaveVerb also clear it, which is enough
        // for inproc and NOT enough for a spawned server, where MG_Impl is not in the process:
        // there the flag would latch TRUE for the server's life, every later read anywhere
        // would be judged against the last verb's mask, and the sticky forwards would start
        // aborting under strict on exactly the case their exemption exists for.
        void LeaveApplier();

        // R-7.2's counter, read by the gate. A BARRIER-PULLED field read on the server side
        // increments PipeStats::CallClass::ResidualPulls (short name `rsp`); its value at the
        // end of P5 IS the size of the P6/P7/P8 debt and goes into MEASUREMENTS.
        //
        // IT FORWARDS TO MGPipeResidualPullCount() AND KEEPS NO MEMBER OF ITS OWN. The member
        // c0's signature block declared is deleted rather than wired: the counter is
        // process-wide in PipeInputs.cpp because the reads that increment it happen inside the
        // BACKEND, arbitrarily deep under an MGPipeApply* call, with no PipeApplier in scope.
        // A second copy here could only ever be a number that disagreed with the one the exit
        // gate reads (p1-v1 5).
        Uint64 ResidualPullCount() const;

        // R-11's audit: after a record retires, fill the SEG_STAGE bytes it referenced with
        // 0xDD. Only under MOBILEGL_IPC_AUDIT=1, because it costs a write of every staged byte.
        void PoisonRetiredStageBytes(Uint64 offset, Uint64 size);

        // How many staged bytes the decoder has poisoned, and how many records it has applied -
        // the two numbers the audit lane asserts are non-zero, since an instrumentation that
        // cannot be observed to have run is decoration.
        Uint64 PoisonedStageBytes() const;
        Uint64 DecoderAppliedSeq() const;
        ServerVerbSink& Verbs() { return m_verbs; }
        const ServerVerbSink& Verbs() const { return m_verbs; }

    private:
        Wire::SegmentTable* m_segments = nullptr;
        ReplyPool* m_replies = nullptr;
        Wire::PipeWireDecoder m_decoder;
        ServerVerbSink m_verbs;
        Bool m_attached = false;
    };

} // namespace MobileGL::MG_Remote::Server
