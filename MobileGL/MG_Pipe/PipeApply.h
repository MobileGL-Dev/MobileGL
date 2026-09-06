// MobileGL - MobileGL/MG_Pipe/PipeApply.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

#include "MGPipeRenderStateSpans.h"
#include "MGPipeTypes.h"

// The in-process applier: the SERVER half of the calls P2 emits. Under split this file is
// MG_Remote/Server/PipeApplier (ARCHITECTURE.md 8.3); in the monolith it writes
// MG_Backend/MGPipe/PipeInputs' gPipeInputs directly, so a call and its effect are one
// function call apart and nothing is serialised.
//
// THE SERVER'S PER-CONTEXT WORKING BLOCK *IS* PipeInputs::m_renderState. bind_render_state
// and set_dynamic_state scatter their chunks straight into it, which is why DirectGLES'
// SyncRenderState is not one line changed (ROADMAP.md P2, G5): the block Espryt binds by
// const reference is the assembled block. It is also what makes the MOBILEGL_PIPE_VERIFY
// comparator a real oracle instead of a tautology - the compare-at-read now proves
// "assembled == live", field by field, at every backend read.
//
// This header FORWARD-DECLARES PipeInputs rather than including it: the applier's callers
// (MG_Impl/Pipe) already have it, and MG_Pipe sits below MG_Backend.
//
// Compiled only under MOBILEGL_PIPE_PUSH (CMakeLists.txt), so the pull build gains no symbol.
namespace MobileGL::MG_Pipe {
    struct PipeInputs;

    // ---------------------------------------------------------------------------------
    // The CSO store
    // ---------------------------------------------------------------------------------

    // One record per live render-state CSO, indexed by MGPipeHandle::Slot. It keeps the 396
    // pipeline bytes because an incremental create_render_state names only the chunks that
    // moved against a BaseCso - the rest has to come from somewhere, and that somewhere is
    // the record the client is naming.
    struct MGPipeRenderStateCsoRecord {
        Uint32 Gen = 0;
        Bool Live = false;
        Array<Uint8, kMGPipePipelineChunkBytes> PipelineBytes{};
    };

    struct MGPipeApplierState {
        // Indexed by slot; slot 0 is the reserved null handle and is never live
        // (MGPipeHandles.h kMGPipeFirstAllocatableSlot).
        Vector<MGPipeRenderStateCsoRecord> RenderStateCsos;
        // The last bind, so a rebind of the same handle can be answered without a scatter.
        MGPipeHandle BoundRenderStateCso = kMGPipeNullHandle;
        // The residual block as last received. Compared against the assembled state on every
        // set_residual_value_state; a disagreement is the D9 trip wire.
        ResidualValueBlock Residual{};
        Bool HasResidual = false;
    };

    // The monolith's single applier. Under split there is one per served context.
    MGPipeApplierState& MGPipeApplier();
    // Drops every CSO and the residual mirror. Context teardown, server reset, and the unit
    // tests' per-case fixture.
    void MGPipeApplierReset();

    // ---------------------------------------------------------------------------------
    // The seven apply entry points (ARCHITECTURE.md 5.3, ROADMAP.md P2)
    // ---------------------------------------------------------------------------------

    // create_render_state. `chunkBytes` is the pipeline chunks named by desc.ChunkMask,
    // concatenated in ascending chunk order (MGPipeGatherPipelineChunks' output). A
    // brand-new CSO must name every chunk; an incremental one starts from desc.BaseCso.
    void MGPipeApplyCreateRenderState(const MGPRenderStateDesc& desc, const void* chunkBytes);
    // bind_render_state: 12 bytes, no blob, no hashing. Scatters the record's seven pipeline
    // chunks into the working block and publishes both versions.
    void MGPipeApplyBindRenderState(const MGPBindRenderState& bind);
    // delete_render_state: frees the slot. The client's allocator owns the Gen bump on
    // REUSE; the record only stops being live here. CsoCache's LRU eviction emits this.
    void MGPipeApplyDeleteRenderState(const MGPHandleOnly& handle);
    // set_dynamic_state: the dynamic chunks named by dyn.ChunkMask, concatenated ascending.
    void MGPipeApplySetDynamicState(const MGPDynamicState& dyn, const void* chunkBytes);
    // set_pixel_pack_state. PACK only, deliberately (MGPipeTypes.h, ARCHITECTURE.md 4.6 D5).
    void MGPipeApplySetPixelPackState(const MGPPixelPackState& pack);
    // set_patch_state. The trio also travels in pipeline chunk P0, and the applier asserts
    // under verify that the two carriers agree - the redundancy is a trip wire, not waste.
    void MGPipeApplySetPatchState(const MGPPatchState& patch);
    // set_vertex_attrib_defaults: `tail` is hdr.Count MGPAttribValues for the attributes
    // named by hdr.Mask, in ascending location order.
    void MGPipeApplySetVertexAttribDefaults(const MGPVertexAttribDefaults& hdr, const MGPAttribValue* tail);
    // set_residual_value_state: what has no call of its own. Since P2 that is one Uint64 of
    // capability bits, and every one of them is ALSO answerable from the assembled working
    // block - which is the point. A disagreement is Fatal{PipeResidualDiverged, "<Cap>"}.
    void MGPipeApplySetResidualValueState(const ResidualValueBlock& block);

    // ---------------------------------------------------------------------------------
    // The derivation step (ARCHITECTURE.md 5.3, P2 brief D5)
    // ---------------------------------------------------------------------------------

    // Recomputes every PipeInputs field that is a pure function of the working
    // RenderStateParameters, instead of pulling it out of GLContext a second time. Called by
    // the applier after ANY scatter.
    //
    // The guard is the oracle P1 built: MOBILEGL_PIPE_VERIFY's compare-at-read re-reads each
    // of these from the live context at every backend read, so a transcription error is
    // caught on the first draw that reads it.
    void MGPipeDeriveRenderStateFields(PipeInputs& inputs);
} // namespace MobileGL::MG_Pipe
