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

    // ---------------------------------------------------------------------------------
    // P3a: the handle-shaped resource op table (D-A1)
    // ---------------------------------------------------------------------------------

    // The SECOND backend op table, beside BufferBackendOps. Registered by the active backend
    // at bring-up and cleared at shutdown, exactly as that one is; a null table means "this
    // backend has not taken the resource family over", and the frontend then dispatches the
    // old way, which is what lets the client half land on its own and what keeps a backend
    // whose buffer path is a later phase untouched.
    //
    // NO FRONTEND TYPE APPEARS HERE, and that is the whole point of the conversion: every
    // hook it replaces took a frontend heap reference and four of them read that object's
    // shadow bytes. A resource is an MGPipeHandle plus a payload record plus, where the call
    // carries content, a companion `const void*`.
    //
    // THE COMPANION POINTER IS NOT A NEW IDEA - MGPipeApplyCreateRenderState already carries
    // a blob beside its POD for the same reason: in monolith a blob needs no MGPBlobRef and
    // the pointer is the client's own shadow base, so the call is zero-copy and behaviour is
    // unchanged. How those bytes cross under a real transport is that phase's problem and
    // that phase's flag edit; resource_respecify deliberately does NOT carry kHasBlob here,
    // because a kHasBlob record must own an MGPBlobRef member and MGPResourceDesc has none.
    //
    // SubDataResident MAY BE NULL and stays nullable on purpose: one backend deliberately
    // does not implement it (kOptional in the catalogue), the frontend checks it exactly as
    // it checks the op table it replaces, and giving that backend a real implementation is a
    // behaviour change that belongs in its own change, not in this migration.
    struct MGPipeResourceOps {
        void (*Create)(MGPipeHandle res, const MGPResourceDesc& desc);
        void (*Respecify)(MGPipeHandle res, const MGPResourceDesc& desc, const void* initialBytes);
        void (*SubData)(MGPipeHandle res, const MGPSubData& record, const void* bytes);
        // kOptional: may be null. `bytes` is the application's staging store and is valid for
        // the duration of the call only.
        void (*SubDataResident)(MGPipeHandle res, const MGPSubData& record, const void* bytes);
        void (*FlushRange)(MGPipeHandle res, const MGPFlushRange& record, const void* bytes);
        void (*Readback)(MGPipeHandle res, const MGPReadback& record);
        void (*Destroy)(MGPipeHandle res);
        void* (*MapPersistent)(MGPipeHandle res, Uint64 size, const void* seedBytes);
        void (*UnmapPersistent)(MGPipeHandle res);
    };

    // Install / read the table. A null argument uninstalls, which is what a backend does at
    // context teardown and what every build that has not migrated the family sits at.
    void MGPipeSetResourceOps(const MGPipeResourceOps* ops);
    const MGPipeResourceOps* MGPipeGetResourceOps();

    // ---------------------------------------------------------------------------------
    // P3a: the applier's own records (D-G4)
    // ---------------------------------------------------------------------------------

    // One record per live resource, indexed by MGPipeHandle::Slot, kind Buffer; slot 0 is the
    // reserved null handle and is never live.
    struct MGPipeResourceRecord {
        Uint32 Gen = 0;
        Bool Live = false;
        // The last create/respecify, verbatim. The backend reads its Width / Usage /
        // StorageFlags / HasDefinedContent instead of asking the frontend object.
        MGPResourceDesc Desc{};
        // SERVER-OWNED, monotone, and it never crosses the line: an MGGen-class counter, ++ on
        // every mutation this applier applies (respecify, sub-data, flush range, resident
        // sub-data). It is what replaces the frontend change serial the backend used to
        // mirror, and no MGPipe call may require the client to provide or know one.
        Uint64 Serial = 0;
        // ALWAYS FALSE IN P3a, AND WRITTEN BY NOBODY. It exists so the phase that pushes
        // persistent-mapped host writes can set it with zero new record kinds; a verify build
        // pins that it is false, so that phase cannot land a silent semantic change under it.
        Bool HasLiveHostWrites = false;
    };

    // The vertex-elements CSO as the applier holds it: the unpacked blob, both views, plus
    // the serial the backend's per-VAO twin compares against instead of a wrapping Uint16
    // configuration version plus an identity patch.
    //
    // The 32 is GL's MAX_VERTEX_ATTRIBS as MobileGL advertises it (kMGPipeMaxVertexAttribs,
    // MGPipeTypes.h), which is also the bound the record's two declared counts are checked
    // against before the blob is unpacked.
    struct MGPipeVertexElementsRecord {
        Uint32 Gen = 0;
        Bool Live = false;
        Uint32 AttributeCount = 0;
        Uint32 BindingPointCount = 0;
        Array<MGPVertexAttribWire, kMGPipeMaxVertexAttribs> Attributes{};
        Array<MGPVertexBindingPointWire, kMGPipeMaxVertexAttribs> BindingPoints{};
        // Server-owned MGGen, ++ on every create_vertex_elements applied to this handle -
        // including a RE-create on the same handle, which is how a configuration change
        // travels (the handle is minted per frontend VAO and Gen moves only on slot reuse).
        Uint64 ContentSerial = 0;
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

        // The GLOBAL chunk bits (MGPipeRenderStateSpans.h's numbering) this applier has
        // itself scattered into the working block since the last reset - its own ledger of
        // which bytes of PipeInputs::m_renderState are the APPLIER'S rather than the per-verb
        // fill loop's. Both trip wires arm off it, and that is the whole of their contract:
        //
        //   - with the render-state subsystem OFF (MOBILEGL_PIPE_PUSH bit 0 clear - the
        //     per-subsystem A/B of D14) nothing is ever scattered, the ledger stays empty and
        //     the wires say nothing. The working block is then the fill loop's, published per
        //     VERB CLASS (MG_Pipe/FillPoints.def), so at a kDispatch or kTextureOp verb - the
        //     two classes that publish IsCapabilityEnabled but NOT GetRenderStateParameters -
        //     it still holds the previous draw's bytes and is an oracle for nothing;
        //   - with it ON the applier is the block's only writer (D5 takes an emitted field
        //     out of the fill loop), so the bytes it has scattered are current at every verb
        //     of every class and comparing against them is honest.
        //
        // set_patch_state's own write to the working block deliberately does NOT enter the
        // ledger: that is the OTHER carrier, and a wire comparing against bytes it had just
        // written itself would be a tautology.
        Uint32 ScatteredChunkBits = 0;

        // What the two trip wires last did. A wire nothing can observe is a gate that cannot
        // go red for the reason it exists (ROADMAP.md), and only a poison or verify build
        // aborts: the shipped push build counts and logs, so these counters are how a unit
        // case sees the wire fire in EVERY build rather than in one.
        Uint32 ResidualCapabilitiesCompared = 0; // of the 35, at the last set_residual_value_state
        Uint32 ResidualDivergences = 0;          // cumulative
        Uint32 PatchCarrierComparisons = 0;      // cumulative, armed set_patch_state calls only
        Uint32 PatchCarrierDivergences = 0;      // cumulative

        // ---- P3a (D-G4). All per context, like everything above. ----

        // Indexed by MGPipeHandle::Slot of kind Buffer / VertexElementsCso.
        Vector<MGPipeResourceRecord> Resources;
        Vector<MGPipeVertexElementsRecord> VertexElementsCsos;

        // The last bind_vertex_elements. Null is legal and means "no VAO bound".
        MGPipeHandle BoundVertexElements = kMGPipeNullHandle;

        // The last set_vertex_buffers, as received: the entries, the window they describe,
        // and the fetch base instance they are valid for.
        Array<MGPVertexBuffer, kMGPipeMaxVertexAttribs> VertexBuffers{};
        Uint32 VertexBufferStart = 0;
        Uint32 VertexBufferCount = 0;
        // The RAW value the client sent (MGPVertexBuffers::BaseInstance) resolved by the
        // server's own decision about whether to emulate the fetch shift. The client never
        // pre-shifts an offset and never learns the answer: emulation is server-owned.
        Uint32 VertexFetchBaseInstance = 0;
        // Server-owned MGGen, ++ on every applied set_vertex_buffers. It is what retires the
        // backend twin's wrapping-Uint16-plus-identity patches.
        Uint64 VertexBuffersSerial = 0;

        // The last set_index_buffer. Independent of the vertex-elements configuration by
        // design (D5): the index slot is not part of a VAO's configuration version.
        MGPIndexBuffer IndexBuffer{};
        Uint64 IndexBufferSerial = 0;

        // Every map_persistent EMISSION, i.e. every acquisition attempt - mint OR decline -
        // because every one of them needs an answer from the resource owner. In monolith the
        // answer is free; under a transport it is a real round trip. The number is therefore
        // the same in both modes and is "one per storage definition", which is what makes it
        // assertable today instead of a counter that can only ever read zero. The counter an
        // operator greps is PipeStats' map-persistent-roundtrips (mpr); this member is the
        // applier-side observable a unit case reads without a stats window.
        Uint64 MapPersistentRoundtrips = 0;
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
    // P3a: the nine resource entry points (D-A1, D-A2)
    // ---------------------------------------------------------------------------------
    //
    // These are the ONE exception to push-at-validate: they are applied at the GL call that
    // causes them, from the same dispatchers that call the old op table today, because that
    // is already where those hooks run. Nothing about buffers moves to validate time here.
    //
    // The `bytes` companion of the three content-carrying calls is the client's shadow base,
    // never a copy (see MGPipeResourceOps). A null is a real answer wherever the payload says
    // the content is undefined.
    //
    // AT THE CONTRACT COMMIT EVERY BODY BELOW IS A STUB. The signatures are what the client,
    // the backend and the gates compile against, and the records above are what they write
    // into; the bodies land in the two commits that follow this one on the same branch.

    // resource_create: mints the record and marks the slot Live. Emitted from the buffer
    // object's CONSTRUCTOR, so a resource exists before anything can name it; storage is
    // defined lazily by the first respecify and a backend tolerates a resource with none.
    void MGPipeApplyResourceCreate(const MGPResourceDesc& desc);
    // resource_respecify: replaces the stored descriptor and bumps Serial. `initialBytes` is
    // the shadow when desc.HasDefinedContent, else null. kNeedsAck on the call,
    // MGPipeResourceRespecifyNeedsAck(desc) per record - only an immutable store acks.
    void MGPipeApplyResourceRespecify(const MGPResourceDesc& desc, const void* initialBytes);
    // resource_subdata, buffer half: the destination range rides in the record's box through
    // MGPipeSetSubDataBufferRange, and a false from that helper is where the EMITTER split.
    // The applier stores nothing per record - contents are the backend's - and bumps Serial.
    void MGPipeApplyResourceSubData(const MGPSubData& record, const void* bytes);
    // buffer_subdata_resident: same shape; `bytes` is the application's staging store and is
    // valid for the duration of the call only. The op-table entry may be null.
    void MGPipeApplyBufferSubDataResident(const MGPSubData& record, const void* bytes);
    // resource_flush_range: record.AccessFlags are the application's REAL mapping flags, not
    // a normalised subset - the backend reads them per call to choose its upload shape.
    void MGPipeApplyResourceFlushRange(const MGPFlushRange& record, const void* bytes);
    // resource_readback: whole-buffer by contract. The answer travels back through the
    // reverse channel, and the writeback happens BEFORE the mutation epoch bumps, never
    // after - the ordering is a correctness rule, not a preference.
    void MGPipeApplyResourceReadback(const MGPReadback& record);
    // resource_destroy: clears Live and drops the record, then the backend frees its twin.
    // The CLIENT frees the slot afterwards, in that order, because the allocator forgets the
    // lifetime id on free and a notice resolved twice finds nothing the second time.
    void MGPipeApplyResourceDestroy(const MGPHandleOnly& handle);
    // map_persistent: bumps MapPersistentRoundtrips and asks the backend. Returns the
    // coherent host pointer the resource owner donated, or null for a DECLINE - which is a
    // real answer and the reason the call is kOptional as well as kReplySlot. `seedBytes` is
    // the shadow, still live at this point, for the backends that seed the new store from it.
    void* MGPipeApplyMapPersistent(const MGPHandleOnly& handle, Uint64 size, const void* seedBytes);
    // unmap_persistent: the donation ends. Never emitted by P3a's own paths; the call exists
    // so the pair is complete and the transport has both halves.
    void MGPipeApplyUnmapPersistent(const MGPHandleOnly& handle);

    // ---------------------------------------------------------------------------------
    // P3a: the five vertex-input entry points (D-G, D-H, D-I)
    // ---------------------------------------------------------------------------------

    // create_vertex_elements. `blobBytes` is MGPVertexAttribWire[desc.AttributeCount]
    // immediately followed by MGPVertexBindingPointWire[desc.BindingPointCount], both in
    // ascending index order. The applier REFUSES a record whose declared counts do not
    // describe its own blob, and both counts are bounded by kMGPipeMaxVertexAttribs.
    // Re-issuing on the same handle is how a configuration change travels; it bumps
    // ContentSerial and does not rebind.
    void MGPipeApplyCreateVertexElements(const MGPVertexElements& desc, const void* blobBytes);
    // bind_vertex_elements. The null handle is legal and means "no VAO bound".
    void MGPipeApplyBindVertexElements(const MGPHandleOnly& handle);
    // delete_vertex_elements: emitted from ONE place, the frontend object's death notice.
    void MGPipeApplyDeleteVertexElements(const MGPHandleOnly& handle);
    // set_vertex_buffers: `tail` is hdr.Count MGPVertexBuffer entries starting at hdr.Start.
    // hdr.BaseInstance is the DRAW's raw base instance; the applier resolves whether to shift
    // and stores the answer in VertexFetchBaseInstance. Bumps VertexBuffersSerial.
    void MGPipeApplySetVertexBuffers(const MGPVertexBuffers& hdr, const MGPVertexBuffer* tail);
    // set_index_buffer: an independent call, NOT a subset of the vertex-elements
    // configuration. Bumps IndexBufferSerial.
    void MGPipeApplySetIndexBuffer(const MGPIndexBuffer& record);

    // ---------------------------------------------------------------------------------
    // The derivation step (ARCHITECTURE.md 5.3, P2 brief D5)
    // ---------------------------------------------------------------------------------

    // Recomputes every PipeInputs field that is a pure function of the working
    // RenderStateParameters, instead of pulling it out of GLContext a second time.
    //
    // The oracle is the one P1 built: MOBILEGL_PIPE_VERIFY's compare-at-read re-reads each of
    // these from the live context at every backend read, so a transcription error is caught
    // on the first draw that reads it - on the retrace and integration-verify LANES, which is
    // where the comparator arms (MG_Config::Features.PipeVerify). A unit-test process never
    // runs the config loader, so the unit oracle is a different one:
    // RenderStateSpansTest.DerivationMatchesTheFrontendGetters walks every setter and
    // compares all 29 derived values against the frontend getters they were transcribed from.
    void MGPipeDeriveRenderStateFields(PipeInputs& inputs);

    // The same derivation, SCOPED to the chunks a scatter actually moved (bit i is global
    // chunk i - MGPipeGlobalChunkBitsOf{Pipeline,Dynamic}Mask widens a wire mask to it). This
    // is what the applier calls, and it is why a per-frame glViewport - the D8 case whose
    // whole point is that it sends dynamic chunk D0 alone - does not pay for the 8-wide blend
    // loop, the 16-wide depth-range loop or the 35-arm capability switch. Every guard's chunk
    // set is computed from the boundary table with MGPipeRenderStateChunkBitsCovering, so a
    // boundary move cannot leave one stale, and
    // RenderStateSpansTest.IncrementalChunksKeepEveryDerivedFieldInStep drives the scoped
    // path against the frontend getters family by family.
    void MGPipeDeriveRenderStateFieldsForChunks(PipeInputs& inputs, Uint32 globalChunkBits);
} // namespace MobileGL::MG_Pipe
