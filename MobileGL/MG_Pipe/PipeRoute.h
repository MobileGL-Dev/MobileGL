// MobileGL - MobileGL/MG_Pipe/PipeRoute.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// Client-side routing of the MGPipe calls. MG_Impl/Pipe emitters call MGPipeRoute<Name>, never
// MGPipeApply<Name>, so every record goes through the installed tables (gMGPipeScreen,
// gMGPipeContext, gMGPipeRouteEscapes): the monolith adapters or the client wire emitters.
// A kReplySlot answer comes back through a thread-local mailbox one entry deep: each route takes
// its answer before returning, so a second post before the take is Fatal{ReplyOverrun}. Taking
// an answer nothing posted is Fatal{ReplyMissing}, never a default: assuming acceptance loses
// uploads, assuming refusal makes the emitter resend for ever. Under split the slot id is the
// record's own sequence number, so the mailbox adds no second id space.

#pragma once
#include <Includes.h>

#include "MGPipe.h"
#include "PipeApply.h"
// P12: MGPipeHandleIsPublished, the latch the respecify predicate below reads.
#include "PipeMutation.h"

namespace MobileGL::MG_Pipe {

    // ---------------------------------------------------------------------------------
    // The reply mailbox
    // ---------------------------------------------------------------------------------

    // Mints the id for one call's answer. Monolith's ids and the wire's seqs are separate
    // spaces on purpose: under split the emitter OVERWRITES this with the record's own seq
    // (R-3: "the reply slot id IS the record sequence number"), so the id a caller finally
    // reads is the wire's, and under monolith it is a local ticket that only has to be unique
    // against the one outstanding call the barrier permits.
    MGPReplySlot MGPipeMintReplySlot();

    // Posts one answer. Called by whichever table is installed - the monolith adapter with
    // the applier's return value, the client emitter with the bytes the server put in
    // SEG_REPLY. `status` is Transport::ReplyStatus' value space (0 OK, 1 DECLINED, 2 ERROR),
    // restated as a plain Int32 so MG_Pipe does not have to see MG_Remote at all.
    void MGPipePostReply(const MGPReplySlot& slot, Int32 status, Uint64 value);

    // Takes the answer for `slot`. Fatals - naming `row` - if nothing was posted, if what was
    // posted belongs to a different slot, or if the answer has already been taken.
    Bool MGPipeTakeReplyBool(const MGPReplySlot& slot, const char* row);
    void* MGPipeTakeReplyPointer(const MGPReplySlot& slot, const char* row);

    // How many answers this thread has taken, and how many of those were DECLINED. Counted
    // rather than inferred, for R-8's reason one level out: "the client accepted everything"
    // and "the client never asked" are otherwise the same observation from outside.
    Uint64 MGPipeRepliesTaken();
    Uint64 MGPipeRepliesDeclined();

    // ---------------------------------------------------------------------------------
    // The escapes: the rows no generated signature can express
    // ---------------------------------------------------------------------------------
    // A hand-written third table, installed with the generated pair. Each row keeps the applier's
    // signature because the monolith arm passes what the wire record does not carry:
    // ResourceRespecify's `initialBytes` and ResourceFlushRange's `bytes` (sent as resource_subdata
    // records instead), MapPersistent's `size`, `seedBytes` and void* result, CreateShaderState's
    // seven SPIR-V/reflection blobs (monolith never serializes them), SetProgramBindings' tails.
    struct MGPipeRouteEscapes {
        Bool (*ResourceRespecify)(const MGPResourceDesc* desc, const void* initialBytes,
                                  const MGPRespecifiedLevel* level);
        void (*ResourceFlushRange)(const MGPFlushRange* record, const void* bytes);
        void* (*MapPersistent)(const MGPHandleOnly* handle, Uint64 size, const void* seedBytes);
        // P5e (pg) grew this row by the STAGE LIST, and it belongs on the route rather than in
        // the two structs: `SpirvArtifacts::generatedSpirv` is one module per shader object and
        // the stage of each lives in `ProgramObject::m_linkedShaderSnapshot`, which is GL-thread
        // state and not an artifact. The client arm frames it in front of the archive; the
        // monolith arm ignores it, because its twin reads the snapshot directly.
        void (*CreateShaderState)(const MGPProgramDesc* desc, const MG_State::GLState::LinkArtifacts* link,
                                  const MG_State::GLState::SpirvArtifacts* spirv, const Uint32* linkedStages,
                                  Uint32 linkedStageCount);
        void (*SetProgramBindings)(const MGPProgramBindings* hdr, const Int32* blockBindings,
                                   const MGPProgramSamplerUnit* samplerUnits,
                                   const MGPProgramStorageOverride* storageOverrides,
                                   const char* const* storageOverrideNames);
    };
    inline MGPipeRouteEscapes gMGPipeRouteEscapes{};

    // ---------------------------------------------------------------------------------
    // The two install functions
    // ---------------------------------------------------------------------------------

    // Installs the monolith adapters into all three tables. Idempotent.
    void MGPipeInstallMonolithTables();

    // The monolith adapters, kept beside the installed tables rather than only written into
    // them. THE SPLIT ARM NEEDS THEM AT RUNTIME, and the reason is table 3's role split made
    // concrete: `gMGPipeScreen` / `gMGPipeContext` are PROCESS globals, and under `inproc` the
    // server role lives in the same process on the apply thread. When that thread runs the
    // server's own backend - the EGL bring-up, InitCapabilities, or the applier itself - it
    // reaches the very same `MG_Impl/Pipe` emitters the client does, and a wire emitter there
    // publishes a record and then waits for the apply thread to apply it. That thread IS the
    // apply thread, so it waits for itself: `Fatal{BarrierTimeout, "ResourceRespecify"}`,
    // logged by `mgl-srv-apply` one barrier budget after bring-up starts.
    //
    // So the client emitters ask "am I the server role right now?" and, if so, run the
    // monolith adapter - which is exactly what `PipeWireCodec` already does on the decode side
    // by calling `MGPipeApply*` directly. The predicate is v1's `ServerLoop::OnApplyThread()`;
    // these three accessors are what makes the other half reachable.
    const MGPipeScreen& MGPipeMonolithScreen();
    const MGPipeContext& MGPipeMonolithContext();
    const MGPipeRouteEscapes& MGPipeMonolithEscapes();

    namespace detail {
        // Installs the monolith tables before main in every binary that includes this header.
        // A static initializer in PipeRoute.cpp is not enough: a test binary linking the static
        // archive may drop PipeRoute.o (CsoCache.h reaches the tables only through an inline
        // thunk), leaving the tables null. This inline variable is emitted in each including
        // translation unit, and its call to MGPipeInstallMonolithTables pulls PipeRoute.o in.
        inline const Bool gMonolithTablesInstalled = (MGPipeInstallMonolithTables(), true);
    } // namespace detail

    // True once the tables hold something. Exposed for the gates only: a probe that armed
    // against an un-installed table would be arming against the pre-migration state.
    Bool MGPipeTablesAreInstalled();

    // Which arm is installed, so a case can assert the arm it thinks it is testing rather than
    // trusting `MOBILEGL_TRANSPORT`. `MG_Config::Transport` says what was ASKED for; this says
    // what the table actually does.
    enum class MGPipeRouteArm : Uint8 {
        kNone,
        kMonolith,
        kClientWire
    };
    MGPipeRouteArm MGPipeInstalledArm();
    void MGPipeNoteInstalledArm(MGPipeRouteArm arm);

    // ---------------------------------------------------------------------------------
    // The call-site names: MGPipeRoute<Name> for each routed MGPipeApply* entry point
    // ---------------------------------------------------------------------------------
    // Each takes the applier's signature by reference, so converting a call site is a rename and a
    // site passing a temporary needs no named local. Three also take a byte count the record does
    // not declare and only the call site knows: SetGlobalConstants (GetUBOSize()), the texture half
    // of ResourceSubData, and BufferSubDataResident. The count has no default, so a site cannot
    // forget it and upload short.

    // ---- screen ------------------------------------------------------------------------
    inline Bool MGPipeRouteResourceCreate(const MGPResourceDesc& desc) {
        MGPReplySlot reply = MGPipeMintReplySlot();
        MGP_ResourceCreate(&desc, &reply);
        return MGPipeTakeReplyBool(reply, "resource_create");
    }
    // Whether a resource_respecify record waits for its answer; the wire emitter reads this same
    // predicate. Off the client wire arm it always does. The buffer half never waits: its callers
    // discard the Bool. The texture half waits until the texture's create is accepted
    // (MGPipeHandleIsPublished; a blocking create at first use), after which acceptance is known
    // because the applier keeps texture records across make-current. A refused create leaves the
    // latch clear, so later records keep waiting for the real answer.
    inline Bool MGPipeResourceRespecifyWantsItsReply(const MGPResourceDesc& desc) {
        if (MGPipeInstalledArm() != MGPipeRouteArm::kClientWire) return true;
        if (static_cast<MGPipeResourceTarget>(desc.Target) == MGPipeResourceTarget::Buffer) return false;
        return !MGPipeHandleIsPublished(MGPipeKind::Texture, desc.Resource);
    }
    inline Bool MGPipeRouteResourceRespecify(const MGPResourceDesc& desc, const void* initialBytes,
                                             const MGPRespecifiedLevel* level = nullptr) {
        return gMGPipeRouteEscapes.ResourceRespecify(&desc, initialBytes, level);
    }
    inline void MGPipeRouteResourceDestroy(const MGPHandleOnly& handle) {
        MGP_ResourceDestroy(&handle);
    }
    inline void* MGPipeRouteMapPersistent(const MGPHandleOnly& handle, Uint64 size, const void* seedBytes) {
        return gMGPipeRouteEscapes.MapPersistent(&handle, size, seedBytes);
    }
    inline void MGPipeRouteUnmapPersistent(const MGPHandleOnly& handle) {
        MGP_UnmapPersistent(&handle);
    }

    // ---- resources ---------------------------------------------------------------------

    // Whether a resource_subdata record waits for the server's answer (ARCHITECTURE.md §13.1.1).
    // The route and the wire emit table both read this predicate, so neither waits for a reply the
    // other did not request. On the wire arm no record waits: MGPipeEmitResourceSubData discards
    // the buffer half's Bool, and a texture record is emitted only when the server consumes the
    // texture family, so acceptance is known; a record the client cancels still answers false.
    // Monolith answers for free and keeps the texture half's answer. The test reads the whole
    // MGPSubData::Target field, as SubDataNamesABuffer does.
    inline Bool MGPipeSubDataWantsItsReply(const MGPSubData& record) {
        // THE WIRE ARM ONLY. Everything above is an argument about a client talking to a server
        // over SEG_CMD/SEG_STAGE; a monolith run inside a split build (every unit and
        // integration-gpu lane is one) keeps the answer it always gave.
        if (MGPipeInstalledArm() == MGPipeRouteArm::kClientWire) return false;
        // THE PULL BUILD PREPROCESSES TO EXACTLY THIS LINE AND NOTHING ELSE, which is what G1
        // compares: the arm test above sits inside the build switch, so the pull arm emits the
        // expression this function has always had.
        return record.Target != kMGPipeResourceTargetBuffer;
    }

    inline Bool MGPipeRouteResourceSubData(const MGPSubData& record, const void* bytes, Uint64 byteCount,
                                           const MGPSubRegion* regions = nullptr) {
        // THE SLOT IS STILL MINTED AND STILL TAKEN, even for the buffer half. The row keeps
        // kReplySlot on the wire, the server still posts, and the mailbox's "every minted slot
        // is taken" rule is what the next row's ReplyOverrun Fatal is looking for - so the
        // half that does not WAIT still has to collect. What changes is that under run-ahead
        // the emit table publishes and returns, and the answer it collects is the emitter's
        // accept-by-construction rather than the server's.
        MGPReplySlot reply = MGPipeMintReplySlot();
        MGP_ResourceSubData(&record, bytes, byteCount, regions, record.RegionCount, &reply);
        const Bool accepted = MGPipeTakeReplyBool(reply, "resource_subdata");
        // A canceled wire upload is declined even when no server reply was requested.
        if (MGPipeInstalledArm() == MGPipeRouteArm::kClientWire) return accepted;
        return MGPipeSubDataWantsItsReply(record) ? accepted : true;
    }
    inline void MGPipeRouteBufferSubDataResident(const MGPSubData& record, const void* bytes, Uint64 byteCount) {
        MGP_BufferSubDataResident(&record, bytes, byteCount);
    }
    inline void MGPipeRouteResourceFlushRange(const MGPFlushRange& record, const void* bytes) {
        gMGPipeRouteEscapes.ResourceFlushRange(&record, bytes);
    }
    inline void MGPipeRouteResourceReadback(const MGPReadback& record) {
        MGPReplySlot reply = MGPipeMintReplySlot();
        MGP_ResourceReadback(&record, &reply);
        // The answer is COMPLETION, not a value: the bytes come back through SEG_EVENT's
        // OnBufferWriteback (table 1 row 22). It is still TAKEN, because an untaken mailbox
        // entry is what the next row's ReplyOverrun Fatal is looking for.
        (void)MGPipeTakeReplyBool(reply, "resource_readback");
    }

    // ---- render state ------------------------------------------------------------------
    inline void MGPipeRouteCreateRenderState(const MGPRenderStateDesc& desc, const void* chunkBytes) {
        MGP_CreateRenderState(&desc, chunkBytes, desc.Blob.Size);
    }
    inline void MGPipeRouteBindRenderState(const MGPBindRenderState& bind) {
        MGP_BindRenderState(&bind);
    }
    inline void MGPipeRouteDeleteRenderState(const MGPHandleOnly& handle) {
        MGP_DeleteRenderState(&handle);
    }
    inline void MGPipeRouteSetDynamicState(const MGPDynamicState& dyn, const void* chunkBytes) {
        MGP_SetDynamicState(&dyn, chunkBytes, dyn.Blob.Size);
    }
    inline void MGPipeRouteSetPixelPackState(const MGPPixelPackState& pack) {
        MGP_SetPixelPackState(&pack);
    }
    inline void MGPipeRouteSetPatchState(const MGPPatchState& patch) {
        MGP_SetPatchState(&patch);
    }
    // P5c (rv, CONTRACT-P5C.md §5.3): emitted ONLY with an active transport (PipeFill.cpp
    // gates on it); under monolith the row is never produced and the fields keep coming
    // through the residual fill (G1). The row IS an ordinary routed set_* otherwise - the
    // monolith adapter exists so the wire emitter's server-role arm has something defined to
    // forward to, and the catalog's null-row accounting (PipeCatalogueTest) counts it.
    inline void MGPipeRouteSetContextValues(const MGPContextValues& values) {
        MGP_SetContextValues(&values);
    }
    inline void MGPipeRouteSetVertexAttribDefaults(const MGPVertexAttribDefaults& hdr, const MGPAttribValue* tail) {
        MGP_SetVertexAttribDefaults(&hdr, tail, hdr.Count);
    }

    // ---- vertex input ------------------------------------------------------------------
    inline void MGPipeRouteCreateVertexElements(const MGPVertexElements& desc, const void* blobBytes) {
        MGP_CreateVertexElements(&desc, blobBytes, desc.Blob.Size);
    }
    inline void MGPipeRouteBindVertexElements(const MGPHandleOnly& handle) {
        MGP_BindVertexElements(&handle);
    }
    inline void MGPipeRouteDeleteVertexElements(const MGPHandleOnly& handle) {
        MGP_DeleteVertexElements(&handle);
    }
    inline void MGPipeRouteSetVertexBuffers(const MGPVertexBuffers& hdr, const MGPVertexBuffer* tail) {
        MGP_SetVertexBuffers(&hdr, tail, hdr.Count);
    }
    inline void MGPipeRouteSetIndexBuffer(const MGPIndexBuffer& record) {
        MGP_SetIndexBuffer(&record);
    }

    // ---- framebuffer, samplers, images -------------------------------------------------
    inline void MGPipeRouteSetFramebufferState(const MGPFramebufferState& state) {
        MGP_SetFramebufferState(&state);
    }
    // The parameters row declares Size 0 today and table 1 row 17 puts it under rule A, so
    // the length is stated here once, from the type the applier stores by value.
    inline void MGPipeRouteCreateSamplerState(const MGPSamplerDesc& desc, const SamplerParameters* parameters) {
        MGP_CreateSamplerState(&desc, parameters, sizeof(SamplerParameters));
    }
    inline void MGPipeRouteDeleteSamplerState(const MGPHandleOnly& handle) {
        MGP_DeleteSamplerState(&handle);
    }
    inline void MGPipeRouteCreateSamplerView(const MGPSamplerView& view) {
        MGP_CreateSamplerView(&view);
    }
    inline void MGPipeRouteDeleteSamplerView(const MGPHandleOnly& handle) {
        MGP_DeleteSamplerView(&handle);
    }
    inline Bool MGPipeRouteSetTextureParams(const MGPTextureParams& params) {
        MGPReplySlot reply = MGPipeMintReplySlot();
        MGP_SetTextureParams(&params, &reply);
        return MGPipeTakeReplyBool(reply, "set_texture_params");
    }
    inline void MGPipeRouteSetSamplerViews(const MGPSamplerViews& hdr, const MGPBoundView* tail) {
        MGP_SetSamplerViews(&hdr, tail, hdr.Count);
    }
    inline void MGPipeRouteBindSamplerStates(const MGPSamplerStates& hdr, const MGPipeHandle* tail) {
        MGP_BindSamplerStates(&hdr, tail, hdr.Count);
    }
    inline void MGPipeRouteSetShaderImages(const MGPShaderImages& hdr, const MGPImageView* tail) {
        MGP_SetShaderImages(&hdr, tail, hdr.Count);
    }

    // ---- indexed buffer binding points (P5e, sb) ----------------------------------------
    //
    // ONE ROUTE FOR THREE RECORDS: the class is a FIELD, not an opcode, so Uniform,
    // ShaderStorage and AtomicCounter all come through here and the applier keys on hdr.Class.
    // The second var-tail (MGHostSpan[HostSpanCount], D-B8) is never present on Espryt -
    // kCapNeedsHostUboBytes is 0 for the whole of P5 - so this wrapper states one tail, and the
    // codec's honesty pass is the guard that says so out loud if a backend ever asks for the
    // other one.
    inline void MGPipeRouteSetShaderBuffers(const MGPShaderBuffers& hdr, const MGPBufferRange* tail) {
        MGP_SetShaderBuffers(&hdr, tail, hdr.Count);
    }

    // ---- programs ----------------------------------------------------------------------
    inline void MGPipeRouteCreateShaderState(const MGPProgramDesc& desc, const MG_State::GLState::LinkArtifacts* link,
                                             const MG_State::GLState::SpirvArtifacts* spirv, const Uint32* linkedStages,
                                             Uint32 linkedStageCount) {
        gMGPipeRouteEscapes.CreateShaderState(&desc, link, spirv, linkedStages, linkedStageCount);
    }
    inline void MGPipeRouteBindShaderState(const MGPHandleOnly& handle) {
        MGP_BindShaderState(&handle);
    }
    inline void MGPipeRouteDeleteShaderState(const MGPHandleOnly& handle) {
        MGP_DeleteShaderState(&handle);
    }
    inline void MGPipeRouteSetDrawProgram(const MGPHandleOnly& handle) {
        MGP_SetDrawProgram(&handle);
    }
    inline void MGPipeRouteSetDispatchProgram(const MGPHandleOnly& handle) {
        MGP_SetDispatchProgram(&handle);
    }
    inline void MGPipeRouteSetGlobalConstants(const MGPGlobalConstants& record, const void* bytes, Uint64 byteCount) {
        MGP_SetGlobalConstants(&record, bytes, byteCount);
    }
    // P5e (pg), opcode 80. The fifth escape; see MGPipeRouteEscapes above for why this row has
    // no generated shape. `storageOverrideNames` is index-aligned with `storageOverrides` and
    // is what the applier copies into the record - the MGHostSpan inside each override element
    // describes where the name lives on the WIRE and is resolved by the decoder, never here.
    inline void MGPipeRouteSetProgramBindings(const MGPProgramBindings& hdr, const Int32* blockBindings,
                                              const MGPProgramSamplerUnit* samplerUnits,
                                              const MGPProgramStorageOverride* storageOverrides,
                                              const char* const* storageOverrideNames) {
        gMGPipeRouteEscapes.SetProgramBindings(&hdr, blockBindings, samplerUnits, storageOverrides,
                                               storageOverrideNames);
    }

} // namespace MobileGL::MG_Pipe
