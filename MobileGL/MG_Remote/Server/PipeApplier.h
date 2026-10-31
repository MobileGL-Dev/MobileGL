// MobileGL - MobileGL/MG_Remote/Server/PipeApplier.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The server's applier bridge. The appliers are MG_Pipe/PipeApply's MGPipeApply* functions; this
// class adds what exists only when records arrive over a wire:
//  - the verb stamp: the server stamps PipeInputs itself at each verb boundary
//    (StampVerbBoundary); the client's stamps cannot stand in for it (ARCHITECTURE.md §9.2.1);
//  - acceptance: the Bool appliers and MapPersistent answer through the record's reply slot, and
//    the client gates destructive state changes on that answer, never on its own guess;
//  - borrowed pointers: a SEG_STAGE run is valid until retiredSeq passes its record, so no applier
//    keeps a pointer past its return; MOBILEGL_IPC_AUDIT=1 poisons retired bytes to prove it.

#pragma once
#include <Includes.h>

#include <MG_Backend/BackendObject.h>
#include <MG_Pipe/MGPipe.h>

#include "../Transport/Ring.h"
#include "../Wire/PipeWireCodec.h"
#include <MG_Backend/Record/RecordVerbSink.h>

#include <deque>
#include <utility>

namespace MobileGL::MG_Remote::Server {

    // P5e (gl, ID-111): DOES THIS SERVER PUBLISH kCapRunAheadApply? Asked of the server's OWN
    // CallMask and not of a build constant - that bit IS the sentence "the client may run ahead
    // of this apply", and Magma never sets it (MGPipeRunAheadCapBitsFor returns kCapNone for
    // every backend but DirectGLES, ID-90). A session with no CallMask yet answers false: no
    // client can have latched run-ahead against a snapshot that was never published.
    //
    // PUBLIC because the red-once has to be able to say "the session I built lacks bit 10" in
    // its own words rather than inferring it from the stamp it is testing (ID-102's rule: the
    // action under test is the probe itself).
    Bool MGPipeServerPublishesRunAhead();

    // The barriered stamp, as a pure function: ApplyOne stamps whether the client is parked
    // behind this record before anything can ask. The wire's answer is used only when the
    // client's wait rule is in force and this server publishes kCapRunAheadApply; otherwise every
    // record is barriered. A server without that bit has a lockstep client, and stamping its
    // kWaitNone draws unbarriered while that client is parked would make CountBarrierPull abort.
    // Pure so a unit test can pin "no run-ahead bit, never runs ahead" for any input.
    constexpr Bool MGPipeApplierStampsBarriered(Bool clientWaitRuleLanded, Bool serverPublishesRunAhead,
                                                Bool wireSaysBarriered) {
        return (clientWaitRuleLanded && serverPublishesRunAhead) ? wireSaysBarriered : true;
    }

    // Writes answers into SEG_REPLY at seq % slots, stamping the seq back into the slot header
    // so a wrong-slot read is detectable rather than plausible (table 0's slot header row:
    // {Uint64 Seq; Int32 Status; Uint32 Size;}).
    class ReplyPool final : public Wire::ReplySink {
    public:
        ReplyPool() = default;
        ReplyPool(void* base, Uint64 sizeBytes, Uint32 slotCount, Uint32 slotBytes);

        void SetLink(Transport::ILink* link) { m_link = link; }
        void PostReply(Uint64 seq, Int32 status, const void* bytes, Uint64 size) override;

        // A reply larger than one slot is Fatal rather than chunked: P5's only large answer is
        // ReadPixels, whose size the client already knows before it emits, so the slot size is
        // chosen from that and an overflow means the two sides disagree about the frame.
        Uint32 SlotBytes() const;

        // P12: DID THE ANSWER ACTUALLY GO OUT? PostedReplies counts the ones the link took,
        // FailedReplies the ones it refused. THE PAIR EXISTS BECAUSE THE DECODER'S TALLY CANNOT
        // ANSWER IT: m_answered is incremented BEFORE PostReply, so it counts ATTEMPTS. Without
        // these two, a client that cannot read an answer has no way to tell "the server never
        // sent it" from "it was sent and the bounded buffer dropped it" - and those two need
        // opposite fixes, one in the transport and one in the deferral. (Found the hard way: a
        // device run had the server report 4,521 answers and the client receive 4,501, and
        // nothing in the tree could say which twenty were which.)
        Uint64 PostedReplies() const { return m_posted; }
        Uint64 FailedReplies() const { return m_failed; }

    private:
        Transport::ILink* m_link = nullptr;
        Uint8* m_base = nullptr;
        Uint64 m_size = 0;
        Uint32 m_slots = 0;
        Uint32 m_slotBytes = 0;
        Uint64 m_posted = 0;
        Uint64 m_failed = 0;
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
    class ServerVerbSink final : public MG_Record::RecordVerbSink {
    public:
        // P13 W5: the record verbs (clear, blit, readbacks, draws, dispatch, copies, images,
        // XFB, mipmaps) are MG_Record::RecordVerbSink's - the record arm applies them in a
        // library without a transport too. What stays here is the session's: fences and
        // queries, present, the T0 map, the control records and shared images.

        // P11 B2: T0's map_persistent - ServerSession::AdoptStoreT0 (it owns the aux socket).
        Bool OnMapPersistent(const MG_Pipe::MGPHandleOnly& handle, Uint64 seq, Int32& status,
                             Uint64& inProcessBase) override;
        Bool OnFenceCreate(const MG_Pipe::MGPHandleOnly&) override;
        Bool OnFenceDestroy(const MG_Pipe::MGPHandleOnly&) override;
        Bool OnFenceStatus(const MG_Pipe::MGPHandleOnly&, Uint32&) override;
        Bool OnFenceWait(const MG_Pipe::MGPFenceWait&, Uint32&) override;
        Bool OnFenceWaitServer(const MG_Pipe::MGPFenceWait&) override;
        void ReleaseFences();
        // P10 (CONTRACT-P10.md §1): post kEventFenceSignaled for each client fence, in creation
        // order, that the GPU has finished, up to the first one still pending. `flush` = the
        // apply thread is going idle: submit what the driver holds first. Apply thread only.
        Uint32 ReportSignaledFences(Bool flush);
        // True while a created fence has not been reported: the loop then parks with a deadline
        // instead of for ever, so a fence that finishes while no record arrives is still seen.
        Bool HasUnreportedFences() const { return !m_unreportedFences.empty(); }
        Uint64 FencesReported() const { return m_fencesReported; }
        Uint64 FenceIdleFlushes() const { return m_fenceIdleFlushes; }
        Bool OnQueryCreate(const MG_Pipe::MGPQueryDesc&) override;
        Bool OnQueryBegin(const MG_Pipe::MGPQueryDesc&) override;
        Bool OnQueryEnd(const MG_Pipe::MGPQueryDesc&) override;
        Bool OnQueryCounter(const MG_Pipe::MGPQueryDesc&) override;
        Bool OnQueryAvailable(const MG_Pipe::MGPHandleOnly&, Uint32&) override;
        Bool OnQueryResult(const MG_Pipe::MGPQueryResultRequest&, Wire::QueryResultReply&) override;
        Bool OnQueryDestroy(const MG_Pipe::MGPHandleOnly&) override;
        Bool OnQueryTimestamp(const MG_Pipe::MGPTimestampRequest&, Int64&) override;
        void ReleaseQueries();
        Bool OnPresent(const MG_Pipe::MGPPresent& present) override;

        // ---- P5c ct (§5): the two control records ----------------
        //
        // REAL BODIES FROM THE DAY THE ROWS EXIST, not the P5b stub shape: the rows were
        // appended by the same package that lands these bodies, so there is no window in
        // which a client can emit ahead of its server half.
        //
        // OnApplierReset (§5.1): the make-current edge's server half. ContextSerial is
        // ASSERTED against this session's own count of applier_reset records - one context
        // per session in P5c, so the legal sequence is 0, 1, 2, ... - and only then is
        // MGPipeApplierReset() run, here, on the apply thread that owns g_applier.
        // OnObjectDeath (§5.2): a null handle never crosses (the client emits nothing for an
        // object its allocator cannot resolve), so one arriving is
        // Fatal{ProtocolCorruption, "ObjectDeath.Handle"}; otherwise the per-kind release
        // runs by the record's handle (SlotTables.h's ReleaseTwinByHandle).
        Bool OnApplierReset(const MG_Pipe::MGPApplierReset& reset) override;
        Bool OnObjectDeath(const MG_Pipe::MGPHandleOnly& death) override;

        // ---- P14 S1 (docs/Disaggregated/design/11-state-ownership.md): bind_context, opcode 84.
        //
        // The session's current context token becomes the record's, on the apply thread. Unlike
        // OnApplierReset's serial the token is NOT an assertion about a count the two ends both
        // hold - the client mints it and the server keeps the table CreateContext built - so the
        // refusal here is "this session never created that token", a named ProtocolCorruption
        // rather than a silent bind. Token 0 (the release) is always legal.
        Bool OnBindContext(const MG_Pipe::MGPBindContext& bind) override;

        // shared_image, opcode 85: the
        // session's image table (allocate / import / release) and the two GL-ordered operations
        // (present the default framebuffer into an image, attach an image to a texture).
        Bool OnSharedImage(const MG_Pipe::MGPSharedImageOp& op, Uint64 seq,
                           MG_Pipe::MGPSharedImageReply& reply) override;

    private:
        // A backend change ends every fence and query this sink holds (they are the old
        // backend's natives).
        void OnBackendChanged() override;
        // The wire's user-index span is checked against this session's segments.
        Bool CheckUserIndices(const MG_Pipe::MGPDrawInfo& info, const MG_Pipe::MGPDrawRange* ranges,
                              const MG_Pipe::MGHostSpan& span) override {
            return Wire::CheckDrawUserIndices(info, ranges, span);
        }

        struct FenceEntry {
            Uint32 Gen = 0;
            Bool Live = false;
            MG_Backend::BackendSyncHandle Native = nullptr;
        };
        FenceEntry& FindFence(MG_Pipe::MGPipeHandle handle);
        UnorderedMap<Uint32, FenceEntry> m_fences;
        // P10: created and not yet reported signaled, oldest first.
        std::deque<MG_Pipe::MGPipeHandle> m_unreportedFences;
        Uint64 m_fencesReported = 0;
        Uint64 m_fenceIdleFlushes = 0;
        struct QueryEntry {
            Uint32 Gen = 0;
            Uint32 Kind = 0;
            Bool Live = false;
            Bool Active = false;
            MG_Backend::BackendQueryHandle Native = nullptr;
            MG_Backend::BackendSyncHandle Completion = nullptr;
        };
        QueryEntry& FindQuery(MG_Pipe::MGPipeHandle handle);
        void EndNativeQuery(QueryEntry& entry);
        Bool QueryGpuComplete(QueryEntry& entry, Bool wait);
        UnorderedMap<Uint32, QueryEntry> m_queries;
        Uint64 m_presents = 0;
        // m_applierResetSerial is both the expected ContextSerial of the next record and the
        // count of accepted resets: the serial is the session's own count of applier_reset
        // records (CONTRACT-P5C.md §1: asserted, never dispatched on).
        Uint64 m_applierResetSerial = 0;
        // The newest native fence (shared_image NativeFence) this session handed out, kept so a
        // later fence with nothing left to export still answers with a descriptor: by then this
        // one has signaled (the backend waited the work out), and a signaled sync_file stays so.
        struct OwnedFd {
            int fd = -1;
            OwnedFd() = default;
            OwnedFd(const OwnedFd&) = delete;
            OwnedFd& operator=(const OwnedFd&) = delete;
            OwnedFd(OwnedFd&& other) noexcept : fd(std::exchange(other.fd, -1)) {}
            OwnedFd& operator=(OwnedFd&& other) noexcept;
            ~OwnedFd();
            void Reset(int next);
        };
        OwnedFd m_lastNativeFence;
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
        void Attach(Transport::ILink* link, MG_Backend::BackendObject* backend);
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
