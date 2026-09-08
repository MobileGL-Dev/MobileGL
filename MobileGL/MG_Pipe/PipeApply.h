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
// P4a: create_shader_state carries the reflection ARCHIVE, and in monolith the archive does
// not travel - the two structs ride beside the record through the entry point's companion
// pointers, exactly as P3a's `const void* initialBytes` does (D-H3, the one Blob rule). So
// this header needs their NAMES and never their definitions; the forward declaration is the
// whole coupling and the closure gate is what keeps it one. The verify build is the only
// place the codec runs, and it runs from PipeApply.cpp.
namespace MobileGL::MG_State::GLState {
    struct LinkArtifacts;
    struct SpirvArtifacts;
} // namespace MobileGL::MG_State::GLState

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

    // THE SLOT A CLIENT MAY NAME IS BOUNDED, and the bound lives here rather than at the
    // client's allocator because the two tables below are grown BY the slot index. An array a
    // handle indexes is the right shape for a dense slot space (MGPipeHandles.h) and the price
    // of that shape is that one corrupt Uint32 in a payload otherwise arrives at an allocator
    // as a four-billion-entry request from inside the bounds gate's own commit. A slot at or
    // above these is Fatal{ProtocolCorruption} - the same verdict as any other record that
    // would make the server act outside its own storage - and never a resize.
    //
    // The two numbers differ because the two records do: a resource record is descriptor-sized
    // and a vertex-elements record carries both unpacked views at ~1.3 KB, so one bound would
    // mean two very different worst cases. Both are far above what a GL application has live
    // at once, and NEITHER IS EVER ALLOCATED BY BEING NAMED: the tables grow to the client's
    // own dense high-water mark and no further, so the bound costs nothing until a record is
    // already corrupt. Package C bounds handle.Slot the same way before
    // BackendSlotTable::EntryAt, which resizes on a client-supplied index too.
    // P4a: THE BOUND IS PER KIND, not per table, and that is what keeps one number honest
    // while the number of tables grows. The slot spaces of kinds Buffer, Texture and
    // Renderbuffer are INDEPENDENT (MGPipeSlotAllocator allocates per kind), so three
    // different objects can hold slot 7; the applier therefore keeps one Vector per resource
    // KIND and indexes it by slot, rather than one Vector indexed by slot alone. Each is
    // bounded by kMGPipeMaxResourceSlots and each grows only to its own dense high-water mark.
    inline constexpr Uint32 kMGPipeMaxResourceSlots = 1u << 20;
    inline constexpr Uint32 kMGPipeMaxVertexElementsSlots = 1u << 16;
    // P4a's three, and the argument is written out for each because the records differ in
    // size. None is ever allocated by being named: the tables grow to the client's own dense
    // high-water mark and no further, so the bound costs nothing until a record is corrupt.
    //
    // A sampler CSO record is a 100-byte value plus a handle, and sampler CSOs are
    // CONTENT-ADDRESSED at capacity 256 on the client, so the live population is bounded by
    // that cache and not by the application. 1<<16 is far above anything a GL program can hold
    // and small enough that a corrupt slot is refused rather than allocated.
    inline constexpr Uint32 kMGPipeMaxSamplerCsoSlots = 1u << 16;
    // A sampler VIEW is identity-addressed one per ITextureObject (P4a D-F2), so its
    // population tracks the texture population exactly and it takes the texture bound.
    inline constexpr Uint32 kMGPipeMaxSamplerViewSlots = 1u << 20;
    // The shader-CSO bound is the SLOT LIMIT ITSELF, because the composite band lives inside
    // that space (MGPipeHandles.h): a bound below it would refuse the very slots
    // AllocateComposite is allowed to hand out.
    inline constexpr Uint32 kMGPipeMaxShaderCsoSlots = kMGPipeShaderCsoSlotLimit;
    static_assert(kMGPipeMaxShaderCsoSlots > kMGPipeShaderCsoCompositeSlotBase,
                  "the ShaderCso bound must contain the composite band, or a composite handle "
                  "is refused as out of range on arrival");

    // ---- P4a's SHAPE bounds, and they are the same argument the slot bounds above make, one
    // level down: every number below arrives inside a payload, every one of them decides how
    // much the applier allocates or how far it indexes, and NONE of them is ever allocated by
    // being named. A record that names one past its bound is Fatal{ProtocolCorruption} - the
    // verdict this file reserves for a record that would make the server act outside its own
    // storage - and never a resize.

    // A sub-data record's mip level. GL's own bound is log2 of the maximum texture size, which
    // no device reports above 2^16, so a level index of 32 addresses a texture no
    // implementation can allocate and is a corrupt record rather than a large one. It is NOT
    // MGPTextureParams::MaxLevel's bound: GL_TEXTURE_MAX_LEVEL defaults to 1000 and is a
    // parameter, not a storage level, so nothing here polices it.
    inline constexpr Uint16 kMGPipeMaxTextureLevels = 32;

    // The pending-upload set (below) is keyed by (UploadTarget, Level) and both halves come
    // off the wire. Levels are bounded above; upload targets are not - a cube face, an array
    // target and a rectangle target are all legal values - so the number of DISTINCT keys one
    // resource may accumulate is bounded here. Six cube faces times 32 levels is 192; 256
    // leaves room for a target space this phase has not enumerated and still refuses the
    // unbounded growth a corrupt Uint16 would otherwise buy.
    inline constexpr Uint32 kMGPipeMaxPendingUploads = 256;

    // The rect list behind one pending entry. The frontend keeps at most MipmapStorage's
    // kMaxDirtyRects = 96 per level and answers "0 rects" for everything it cannot describe
    // that way, which is the model this mirrors: an accumulation that would exceed this
    // collapses to BOX ONLY - the same answer, with the same meaning, and never a dropped
    // region. 256 is that bound with room for several emissions accumulating behind a bail.
    inline constexpr Uint32 kMGPipeMaxPendingUploadRegions = 256;

    // The default uniform block's image, the one allocation P4a adds per program. The size
    // comes from the program's own MGPProgramDesc::GlobalUboSize, so it is checked ONCE at
    // create_shader_state and the set_global_constants that follows can only allocate what the
    // create already declared. 16 MiB is four orders of magnitude above any default uniform
    // block a real program links and still turns a corrupt Uint32 into a refusal.
    inline constexpr Uint32 kMGPipeMaxGlobalConstantsBytes = 16u << 20;

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

        // ---- P4a. Only a record of kind Texture ever carries these; a buffer's stay at
        // their defaults, which is what keeps ONE record type for the discriminated
        // descriptor rather than a second one that would have to be kept in step with it.

        // set_texture_params, per texture OBJECT and independent of any binding - which is
        // the whole point of addressing it by resource: a texture that is only an FBO
        // attachment, only an image-unit binding or only a glCopyImageSubData endpoint has no
        // sampler view to hang its parameters on, and today the READ-attachment case reaches
        // no parameter push at all. ParamsSerial replaces the twin's
        // m_syncedTextureParamsVersion + m_forceTextureParamsResync pair.
        MGPTextureParams Params{};
        Uint64 ParamsSerial = 0;
        // The SamplerViewCso minted for this texture (P4a D-F2: one per ITextureObject,
        // re-issued on the same handle whenever the restrictions move).
        MGPipeHandle ViewCso = kMGPipeNullHandle;
        // THE PENDING-UPLOAD SET, and it is server-side state on purpose (D-D5). The client
        // clears its own dirty flags at EMISSION, for the levels whose record the applier
        // accepted; Espryt's upload loop has bail arms - an incomplete texture returns early,
        // a multisample target refreshes and skips - that today leave the frontend flag set,
        // so a naive move of the clear to the client would lose those texels. The applier
        // accumulates the emitted shape here instead, it survives any number of bails, and
        // Espryt consumes and clears an entry only where it actually uploads.
        //
        // The verify lane's RETAIN MODE is what gates the shape: a consume-and-clear set
        // cannot be recomputed after emission, so the tracker retains the pre-clear set and
        // the comparator compares the emitted (UnionBox, RegionCount, Regions[]) against it
        // field by field.
        //
        // THE SET IS KEYED (UploadTarget, Level) AND EVERY KEY IS INDEPENDENT OF EVERY OTHER.
        // That is not a detail: a respecify redefines ONE level when it arrives from
        // glTexImage*D (MGPipeApplyResourceRespecify's trailing MGPRespecifiedLevel*), so it
        // may only drop that one key - the frontend's AllocateStorage / MarkStorageDirty are
        // per (uploadTarget, level) too, and the other levels' dirty flags were cleared at
        // THEIR emission, so nothing anywhere still owes them.
        //
        // THE ACCUMULATED RECT LIST MAY OVERLAP, AND A CONSUMER MUST TOLERATE THAT. Behind one
        // level the frontend's own model is pairwise disjoint (MipmapStorage keeps it so), but
        // this list CONCATENATES the lists of successive emissions and the applier's gate only
        // asks that each rect be inside the record's own union box - so two emissions that
        // touch the same texels leave two rects that do. Staging N rects therefore uploads
        // those texels twice, which is a cost and never a correctness problem; nothing here
        // de-duplicates and nothing downstream may assume "the frontend's model" means disjoint
        // once the shapes have been accumulated.
        struct PendingUpload {
            Uint16 UploadTarget = 0;
            Uint16 Level = 0;
            MGPBox UnionBox{};
            Vector<MGPSubRegion> Regions;
        };
        Vector<PendingUpload> PendingUploads;
    };

    // ---------------------------------------------------------------------------------
    // P4a: the three new object-record kinds (D-J1)
    // ---------------------------------------------------------------------------------
    //
    // All three follow MGPipeResourceRecord's shape exactly - Gen, Live, a payload and a
    // server-owned monotone Serial - because the body-level idioms are the same ones:
    // a create starts the record OVER rather than editing it (a recycled slot's record must
    // not contribute one field, and Serial stays 0 because a create is not a mutation, so a
    // fresh backend twin starting at 0 agrees without either side publishing anything); the
    // serial moves BEFORE the backend is told; a destroy drops the record whole and keeps the
    // generation, and the CLIENT frees the slot afterwards.

    // create_sampler_state / delete_sampler_state. The parameters cross byte for byte
    // INCLUDING borderColorForm - all three border representations are always numerically
    // populated, so the value alone cannot say which driver entry point to use - and
    // MOBILEGL_PIPE_VERIFY compares them FIELD BY FIELD (PipeFields.def's
    // MGP_FIELDS_SamplerParameters), because the struct has three bytes of trailing padding
    // and a byte comparison of it is a coin flip rather than a gate.
    struct MGPipeSamplerCsoRecord {
        Uint32 Gen = 0;
        Bool Live = false;
        SamplerParameters Params{};
        Uint64 Serial = 0;
    };

    // create_sampler_view / delete_sampler_view: ONLY the view restrictions. Everything a
    // glTexParameter writes lives on set_texture_params instead. Re-issuing on the same
    // handle is how a restriction change travels (Gen moves only on slot reuse); it bumps
    // Serial and does not rebind anything.
    struct MGPipeSamplerViewRecord {
        Uint32 Gen = 0;
        Bool Live = false;
        MGPSamplerView View{};
        Uint64 Serial = 0;
    };

    // create/bind/delete_shader_state, plus set_global_constants' per-program half.
    //
    // THE ARTEFACTS ARE NOT HELD HERE IN MONOLITH: MGPProgramDesc's seven MGPBlobRefs are all
    // declared with Size 0 ("this record does not declare its blob") and the LinkArtifacts /
    // SpirvArtifacts ride beside the record through the entry point's companion pointers, so
    // the applier stores the DESCRIPTOR and the identity and the server reads the frontend's
    // own archive. That is what keeps the codec off the monolith hot path entirely; the verify
    // build is where it is exercised, by serialising, deserialising and field-comparing before
    // storing.
    //
    // GlobalConstants is the one allocation P4a adds per program, it is bounded by
    // Desc.GlobalUboSize, and it is NOT on the hot path: set_global_constants is
    // (ShaderCso, Version) keyed and fires at most once per program per frame.
    struct MGPipeShaderCsoRecord {
        Uint32 Gen = 0;
        Bool Live = false;
        MGPProgramDesc Desc{};
        // GetUBOContentVersion() as last received. ~0u is the backends' "never uploaded"
        // sentinel and the client must never emit it, so it is also what this starts at.
        Uint32 GlobalConstantsVersion = ~Uint32{0};
        Vector<Uint8> GlobalConstants;
        Uint64 GlobalConstantsSerial = 0;
        Uint64 Serial = 0;
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

        // ---- P3a (D-G4). ----
        //
        // THE TWO HALVES BELOW HAVE DIFFERENT LIVES, and MGPipeApplierReset is where the
        // difference is spent: the OBJECT RECORDS describe GL objects and outlive a
        // make-current; the WORKING STATE describes what the next draw fetches with and does
        // not. Reading the whole block as "per context" is what dropped a shared buffer's
        // record at every context switch and made the write that followed it disappear.

        // ---- object records: indexed by MGPipeHandle::Slot of kind Buffer /
        // VertexElementsCso, and NOT part of the working state.
        //
        // A GL object lives in a SHARE GROUP, not in a context: a buffer created before a
        // make-current is the same buffer, with the same storage, after it, and its record is
        // the only thing the backend has left to read that storage's extent and mutation
        // serial out of (D-A4 re-keys IsBufferDrawClean onto exactly those two). Dropping the
        // records at a make-current would therefore make every subsequent glBufferSubData on a
        // pre-existing buffer resolve to nothing and be refused - a lost write, in a build
        // where the refusal's assertion has compiled out.
        //
        // They are cleared by the object's OWN death signal - resource_destroy,
        // delete_vertex_elements, which is what D-L makes the buffer's death crossing - and by
        // MGPipeApplierReleaseObjectRecords when the served context and its applier go away.
        // Nothing else.
        Vector<MGPipeResourceRecord> Resources;
        Vector<MGPipeVertexElementsRecord> VertexElementsCsos;

        // ---- P4a's object records. FIVE MORE TABLES, and the two resource ones are separate
        // Vectors rather than more rows of `Resources` above because the slot space is PER
        // KIND: a Buffer, a Texture and a Renderbuffer can all hold slot 7 at once, so a
        // single slot-indexed table would alias three different objects onto one record. The
        // record TYPE is shared - one discriminated descriptor for buffers, every texture
        // target and renderbuffers - and the bound is shared; only the table is per kind.
        //
        // Like the two above they are share-group state: MGPipeApplierReset does not touch
        // them, and only the object's own death signal and MGPipeApplierReleaseObjectRecords
        // clear them.
        Vector<MGPipeResourceRecord> TextureResources;
        Vector<MGPipeResourceRecord> RenderbufferResources;
        Vector<MGPipeSamplerCsoRecord> SamplerCsos;
        Vector<MGPipeSamplerViewRecord> SamplerViewCsos;
        Vector<MGPipeShaderCsoRecord> ShaderCsos;
        // The ShaderCso COMPOSITE band's records, indexed by (slot - the band's base), for the
        // same reason MGPipeSlotAllocator keeps the band in a table of its own: the band
        // starts at 983040, so one program-pipeline composite in the slot-indexed vector above
        // would grow it to ~983k records of ~240 bytes each. THE SERVER STILL NEVER LEARNS IT
        // IS A COMPOSITE - the split is an indexing detail on this side of the wire, the
        // handle is an ordinary ShaderCso handle, and create/bind/delete_shader_state name it
        // exactly as they name any other program.
        Vector<MGPipeShaderCsoRecord> CompositeShaderCsos;

        // Every call this applier REFUSED because it named a record this applier does not
        // have: an unknown slot, a slot that is not live, or a generation that has moved on
        // under it. The refusal is a defined no-op - nothing stored, nothing dispatched, no
        // serial moved - for the reason written beside kResourceRefusalNote in PipeApply.cpp,
        // but A NO-OP NOBODY CAN SEE IS A DROPPED CALL NOBODY CAN SEE: MOBILEGL_ASSERT compiles
        // out at INFO, which is what all three gate builds and every shipped build are, so
        // these two are how a unit case - and an operator reading a log - observe it in EVERY
        // build. Per context, like the four render-state wire counters above.
        Uint64 RefusedResourceCalls = 0;
        Uint64 RefusedVertexInputCalls = 0;
        // P4a's, in the same shape and for the same reason: every framebuffer, sampler,
        // sampler-view, program and texture-params call this applier refused because it named
        // a record this applier does not have. One counter rather than five, because the five
        // families share one legal refusal sequence (teardown ->
        // MGPipeApplierReleaseObjectRecords -> ~Object -> death notices naming records already
        // dropped) and an operator reading a log wants to know that ANY object call was
        // dropped; the log line names the call and the handle.
        //
        // THE OTHER CLASS IS NOT COUNTED HERE AND MUST NOT BE: a var-tail window outside its
        // bound, or a set_texture_params whose BuiltinSampler is the null handle, would make
        // the backend act outside its own storage or sample an object that does not exist -
        // that is Fatal{ProtocolCorruption}, not a dropped call.
        Uint64 RefusedObjectCalls = 0;

        // ---- working state: what the next draw fetches with. All of it is per context and
        // all of it is cleared by MGPipeApplierReset, EXCEPT the two serials, which only ever
        // advance (see there).

        // The last bind_vertex_elements. Null is legal and means "no VAO bound".
        MGPipeHandle BoundVertexElements = kMGPipeNullHandle;

        // The last set_vertex_buffers, as received: the entries, the window they describe,
        // and the fetch base instance they are valid for.
        Array<MGPVertexBuffer, kMGPipeMaxVertexAttribs> VertexBuffers{};
        Uint32 VertexBufferStart = 0;
        Uint32 VertexBufferCount = 0;
        // The RAW value the client sent (MGPVertexBuffers::BaseInstance). It is NOT a resolved
        // shift: whether the fetch shift has to be emulated at all is a backend capability - a
        // device with native base-instance support shifts nothing - and emulation is
        // server-owned, so the backend arm turns this into a per-attribute byte shift out of
        // each attribute's own stride and divisor. This header sits below MG_Backend and may
        // not ask that question. The client never pre-shifts an offset and never learns the
        // answer.
        Uint32 VertexFetchBaseInstance = 0;
        // Server-owned MGGen, ++ on every applied set_vertex_buffers. It is what retires the
        // backend twin's wrapping-Uint16-plus-identity patches - which means IT MUST NEVER
        // HAND OUT A VALUE TWICE. A reset ADVANCES it (the cleared window is itself a change
        // the twin has to hear about) and never returns it to 0: a counter that restarts walks
        // back through every value it has already stamped into a twin that outlived the
        // switch, and the identity patch that used to close that hole is exactly what D-G4
        // deletes on the twin's side.
        Uint64 VertexBuffersSerial = 0;

        // The last set_index_buffer. Independent of the vertex-elements configuration by
        // design (D5): the index slot is not part of a VAO's configuration version.
        MGPIndexBuffer IndexBuffer{};
        // Advanced, never zeroed, for VertexBuffersSerial's reason.
        Uint64 IndexBufferSerial = 0;

        // Every map_persistent EMISSION, i.e. every acquisition attempt - mint OR decline -
        // because every one of them needs an answer from the resource owner. In monolith the
        // answer is free; under a transport it is a real round trip. The number is therefore
        // the same in both modes and is "one per storage definition", which is what makes it
        // assertable today instead of a counter that can only ever read zero. The counter an
        // operator greps is PipeStats' map-persistent-roundtrips (mpr); this member is the
        // applier-side observable a unit case reads without a stats window.
        Uint64 MapPersistentRoundtrips = 0;

        // ---- P4a's WORKING state. All of it is per context and all of it is cleared by
        // MGPipeApplierReset, EXCEPT the serials, which only ever advance - a counter that
        // restarts walks back through values already stamped into a twin that outlived the
        // switch, and P4a deletes the identity patches that used to close that hole.

        // set_framebuffer_state, per bound target (D-C2). Target = Both writes both. The
        // record is fully resolved: ReadSurface comes from the READ framebuffer's own read
        // buffer, so the shared-FBO case cannot lose it, and DrawBuffers[] is applied only for
        // a record whose Target is not Read.
        MGPFramebufferState DrawFramebuffer{};
        MGPFramebufferState ReadFramebuffer{};
        Uint64 FramebufferSerial = 0;

        // The three kVarTail unit sets, as received. NO STAGE DIMENSION: MobileGL's
        // texture-unit space is one merged array of 192, the same unit may be sampled from two
        // stages, and stage is derived server-side from the reflection archive only where the
        // target API needs it.
        //
        // THE VAR-TAIL WINDOW IS THE BOUND AND ENTRIES OUTSIDE IT ARE NOT CLEARED - the
        // record is "the last set as received", exactly as set_vertex_buffers is, and
        // Start + Count above the bound is Fatal{ProtocolCorruption}.
        Array<MGPBoundView, kMGPipeMaxTextureUnits> BoundSamplerViews{};
        Uint32 SamplerViewStart = 0;
        Uint32 SamplerViewCount = 0;
        Uint64 SamplerViewsSerial = 0;

        Array<MGPipeHandle, kMGPipeMaxTextureUnits> BoundSamplerStates{};
        Uint32 SamplerStateStart = 0;
        Uint32 SamplerStateCount = 0;
        Uint64 SamplerStatesSerial = 0;

        Array<MGPImageView, kMGPipeMaxImageUnits> BoundShaderImages{};
        Uint32 ShaderImageStart = 0;
        Uint32 ShaderImageCount = 0;
        Uint64 ShaderImagesSerial = 0;

        // set_draw_program / set_dispatch_program are two calls because the frontend has two
        // joins and two PipeInputs slots; bind_shader_state is the third, and a null handle is
        // legal in all three and means "nothing bound".
        MGPipeHandle DrawProgram = kMGPipeNullHandle;
        MGPipeHandle DispatchProgram = kMGPipeNullHandle;
        MGPipeHandle BoundShaderCso = kMGPipeNullHandle;
        Uint64 ProgramBindingSerial = 0;
    };

    // The monolith's single applier. Under split there is one per served context.
    MGPipeApplierState& MGPipeApplier();

    // A MAKE-CURRENT, NOT A TEARDOWN - and the distinction is the whole of this function's
    // contract. It runs on every change of the current GLContext (MGPipeTracker::Update resets
    // the tracker whenever the context pointer moves, and the emitter calls this from the
    // first walk that follows), including a make-current BACK to a context that is still alive
    // and whose objects are all still there.
    //
    // So it drops what a returning context may not inherit - the render-state CSOs (whose
    // client-side cache is dropped on the line above it, so both sides start over together),
    // the residual mirror, and the vertex-input WORKING state - and it ADVANCES the two global
    // vertex-input serials rather than zeroing them. It does NOT drop the resource or
    // vertex-elements records: those describe share-group objects that the switch does not
    // destroy, and dropping them is a dropped write on the far side of it.
    //
    // P4a EXTENDS BOTH HALVES AND THE RULE IS UNCHANGED (D-J4). Cleared: the two framebuffer
    // records, the three unit sets, DrawProgram / DispatchProgram / BoundShaderCso - all of it
    // per-context working state - with their serials ADVANCED and never zeroed. Not cleared:
    // texture and renderbuffer resources, sampler CSOs, sampler views, shader CSOs, and the
    // texture params and pending uploads that ride on a resource record, because a texture
    // lives in a share group exactly as a buffer does.
    //
    // AND THEREFORE NO P4a TRACKER NEEDS A RE-PUBLICATION PATH ON FreshlyPrimed, AND NONE MAY
    // HAVE ONE: re-emitting create_sampler_state for a record the applier still holds would
    // move its Serial for nothing. What DOES reset on a fresh context is each emitter's
    // BOUND-HANDLE latch - the framebuffer and unit-set hashes through
    // MGPipeSetHashSuppressor::InvalidateAll, and the program emitter's BoundShaderCso mirror -
    // because those mirror working state this function just cleared.
    void MGPipeApplierReset();

    // THE OTHER SCOPE: the served context is going away and its applier with it, so the object
    // records go too. Under split that is one applier per served context and this is its
    // teardown. In the monolith there is ONE applier behind every context, so this is
    // deliberately wired to NOTHING: a record is cleared by its object's own death signal
    // (resource_destroy, delete_vertex_elements) and the process's exit clears the rest.
    // Calling it on one context's destruction in a monolith would drop every other context's
    // records, which is the C1 hole in its other direction.
    void MGPipeApplierReleaseObjectRecords();

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

    // The scope of one resource_respecify, and it is an APPLIER-SIDE ARGUMENT and not a wire
    // record: it is not in PipeFields.def, it crosses no payload, and the transport reads the
    // scope off the call it is replaying rather than off a field. The two members mirror
    // MGPipeResourceRecord::PendingUpload's key exactly, which is the only thing the applier
    // does with them - so UploadTarget is MGPSubData::Target VERBATIM, the whole packed field
    // (ID-12: low byte = MGPipeResourceTarget, high byte = the cube-face upload target), the
    // same value the emission of that level put in the record. A per-face respecify therefore
    // drops the face it redefines and leaves the other five standing, and a caller that packs
    // the pair differently here than it packs it there simply matches nothing.
    struct MGPRespecifiedLevel {
        Uint16 UploadTarget = 0;
        Uint16 Level = 0;
    };

    // resource_create: mints the record and marks the slot Live. Emitted from the buffer
    // object's CONSTRUCTOR, so a resource exists before anything can name it; storage is
    // defined lazily by the first respecify and a backend tolerates a resource with none.
    void MGPipeApplyResourceCreate(const MGPResourceDesc& desc);
    // resource_respecify: replaces the stored descriptor and bumps Serial. `initialBytes` is
    // the shadow when desc.HasDefinedContent, else null. kNeedsAck on the call,
    // MGPipeResourceRespecifyNeedsAck(desc) per record - only an immutable store acks.
    //
    // P4a: `level` IS THE SCOPE OF THE REDEFINITION, and MGPResourceDesc cannot carry it - the
    // descriptor describes the resource, and a mutable texture redefines its levels ONE
    // glTexImage*D AT A TIME. Null means "this respecify redefines the WHOLE resource" - every
    // glBufferData / glBufferStorage, every glTexStorage*, every texture view - and drops every
    // pending upload, which is right because every level's coordinate system has just been
    // replaced. Non-null names the single (uploadTarget, level) the call redefines and drops
    // ONLY that key: the frontend's AllocateStorage / MarkStorageDirty are per
    // (uploadTarget, level) as well (MG_State/GLState/TextureState/TextureObject.h), so a
    // glTexImage2D(level 1) re-marks level 1 AND NOTHING ELSE, while the levels already
    // emitted had their client dirty flags cleared at THEIR emission (D-D5 step 1) and nothing
    // anywhere still owes them. Clearing the whole set here would lose exactly those texels,
    // silently, in every build - the loss the server-side set exists to prevent.
    //
    // Trailing and defaulted for W1's reason: P3a's buffer call site (PipeFill.cpp:691) and
    // every existing case compile unchanged. PACKAGE B PASSES THE PAIR IT JUST ALLOCATED at
    // every per-level respecify; it has both halves in hand at the AllocateStorage call site.
    void MGPipeApplyResourceRespecify(const MGPResourceDesc& desc, const void* initialBytes,
                                      const MGPRespecifiedLevel* level = nullptr);
    // resource_subdata, buffer half: the destination range rides in the record's box through
    // MGPipeSetSubDataBufferRange, and a false from that helper is where the EMITTER split.
    // The applier stores nothing per record - contents are the backend's - and bumps Serial.
    //
    // P4a: `regions` IS THE CALL'S VARIABLE TAIL - MGPSubRegion[record.RegionCount] - and it is
    // a trailing DEFAULTED parameter rather than a second entry point. The call has carried
    // kVarTail since P2 (PipeCalls.def) and the texture half cannot be applied without it: the
    // applier's pending-upload set is (UnionBox, RegionCount, Regions[]) and the verify lane's
    // retain mode compares all three. The buffer half declares no regions, so P3a's one call
    // site and every existing case are unchanged by the default.
    //
    // THE RETURN IS THE ACCEPTANCE SIGNAL D-D5 STEP 1 NAMES: true when the record was stored -
    // the buffer half landed its range, or the texture half accumulated the shape onto the
    // record - and false when it was refused. THE EMITTER MUST GATE ITS DIRTY-FLAG CLEAR ON IT
    // ("only for levels whose record the applier ACCEPTED"), because the two refusal paths are
    // otherwise invisible to it: a dead or stale handle is a counted no-op and a corrupt record
    // is a Fatal that does NOT move RefusedResourceCalls, so in a shipped push build a refused
    // upload and an accumulated one are indistinguishable from the call site. A client that
    // clears on the strength of having emitted drops those texels for good.
    //
    // THE RESOURCE-TARGET HALF OF record.Target PICKS THE HALF. MGPSubData::Target is PACKED
    // (ID-12): low byte = MGPipeResourceTarget, high byte = the cube-face upload target. The
    // buffer half is the whole field being 0 - the encoding the emitter is held to, since a
    // buffer has no upload target - and the texture half additionally requires the low byte to
    // name a TEXTURE target: Buffer, Renderbuffer and anything at or above
    // MGPipeResourceTarget::Count are Fatal{ProtocolCorruption} rather than an upload onto
    // whatever object holds that slot in the texture slot space.
    Bool MGPipeApplyResourceSubData(const MGPSubData& record, const void* bytes,
                                    const MGPSubRegion* regions = nullptr);
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
    // hdr.BaseInstance is the DRAW's raw base instance and is stored, unresolved, in
    // VertexFetchBaseInstance - the decision whether to emulate the fetch shift is the
    // backend's, for the reason written beside that member. Bumps VertexBuffersSerial.
    void MGPipeApplySetVertexBuffers(const MGPVertexBuffers& hdr, const MGPVertexBuffer* tail);
    // set_index_buffer: an independent call, NOT a subset of the vertex-elements
    // configuration. Bumps IndexBufferSerial.
    void MGPipeApplySetIndexBuffer(const MGPIndexBuffer& record);

    // ---------------------------------------------------------------------------------
    // P4a: the fifteen object and working-state entry points (D-A1, D-B1, D-J1)
    // ---------------------------------------------------------------------------------
    //
    // NOT ONE OF THEM DISPATCHES TO A BACKEND FUNCTION POINTER, and that is the single most
    // important structural decision in P4a rather than an omission. Nothing in these families
    // reaches the backend at GL-call time today - texture storage only marks a level dirty and
    // Espryt allocates lazily at sync, texture params run from SyncTextureObjectToBackend at
    // draw sync, renderbuffer storage is allocated inside SyncToBackend on a four-field cache,
    // a sampler twin is created lazily from the program pass, and the framebuffer, unit sets
    // and program are all resolved at PrepareForDraw. So every call below is either an OBJECT
    // RECORD the applier stores or WORKING STATE the applier stores, and Espryt reads the
    // applier at the sync points it already has, keyed on a server-owned Serial instead of a
    // frontend version. MGPipeResourceOps is therefore UNCHANGED - nine members, same
    // signatures - and P4a adds no backend op table and no op-table member at all.
    //
    // The consequence for the four resource entry points above: they BRANCH on
    // record.Desc.Target. A buffer target dispatches into MGPipeResourceOps exactly as P3a
    // wrote it; every other target stores and returns. The branch is one comparison against
    // kMGPipeResourceTargetBuffer and it is where a mis-typed descriptor becomes visible.
    //
    // AT THE CONTRACT COMMIT EVERY BODY BELOW IS A STUB, exactly as P3a's nine were: the
    // signatures are what the client, the backend and the gates compile against and the
    // records above are what they write into; the bodies land in the three commits that
    // follow this one on the same branch.

    // set_framebuffer_state. Fully resolved - nothing in the record requires a lookup on the
    // far side. `state.Target` says which binding it describes (Draw / Read / Both) and the
    // applier keeps the two records apart; Both writes both. ContentHash covers every field
    // including Fbo and DrawBuffers[8], which is what makes a suppressed record provably mean
    // "the draw-buffer array did not move" and therefore "the fragColor broadcast count did
    // not move".
    void MGPipeApplySetFramebufferState(const MGPFramebufferState& state);

    // create_sampler_state. `parameters` is the client's canonical SamplerParameters copy,
    // beside the record for the one Blob rule's reason; the applier stores it by value.
    void MGPipeApplyCreateSamplerState(const MGPSamplerDesc& desc, const SamplerParameters* parameters);
    // delete_sampler_state: emitted by the CSO cache's LRU eviction and by the frontend
    // sampler object's death helper. Clears Live and drops the record; the client frees the
    // slot afterwards.
    void MGPipeApplyDeleteSamplerState(const MGPHandleOnly& handle);

    // create_sampler_view. Re-issued on the SAME handle whenever the view restrictions move,
    // which is legal because Gen increments only on slot reuse and never on a respecify.
    void MGPipeApplyCreateSamplerView(const MGPSamplerView& view);
    void MGPipeApplyDeleteSamplerView(const MGPHandleOnly& handle);

    // set_texture_params: addressed by RESOURCE and independent of any binding, which is what
    // lets a texture that is only an attachment, only an image-unit binding or only a
    // glCopyImageSubData endpoint carry its parameters at all. params.BuiltinSampler may never
    // be the null handle - every ITextureObject owns a sampler object - so a null is
    // Fatal{ProtocolCorruption} rather than "no sampler".
    void MGPipeApplySetTextureParams(const MGPTextureParams& params);

    // set_sampler_views / bind_sampler_states / set_shader_images: `tail` is hdr.Count entries
    // starting at hdr.Start, and hdr.Start + hdr.Count above the unit bound is
    // Fatal{ProtocolCorruption}. Entries outside the declared window are NOT cleared.
    void MGPipeApplySetSamplerViews(const MGPSamplerViews& hdr, const MGPBoundView* tail);
    void MGPipeApplyBindSamplerStates(const MGPSamplerStates& hdr, const MGPipeHandle* tail);
    void MGPipeApplySetShaderImages(const MGPShaderImages& hdr, const MGPImageView* tail);

    // create_shader_state. THE ARTEFACTS TRAVEL BESIDE THE RECORD, by pointer: all seven of
    // desc.Spirv[] and desc.Reflection are declared with Size 0 ("this record does not declare
    // its blob"), which is what a monolith emission is, and the codec is NOT called - zero
    // serialisation cost on the monolith path. A verify build serialises, deserialises and
    // field-compares before storing, and a mismatch is Fatal{PipeVerifyDiffer, "program-archive"}.
    // Splitting this record for a transport whose ring caps one record at half its capacity is
    // P5's problem, not this entry point's.
    void MGPipeApplyCreateShaderState(const MGPProgramDesc& desc,
                                      const MG_State::GLState::LinkArtifacts* link,
                                      const MG_State::GLState::SpirvArtifacts* spirv);
    void MGPipeApplyBindShaderState(const MGPHandleOnly& handle);
    void MGPipeApplyDeleteShaderState(const MGPHandleOnly& handle);
    void MGPipeApplySetDrawProgram(const MGPHandleOnly& handle);
    void MGPipeApplySetDispatchProgram(const MGPHandleOnly& handle);

    // set_global_constants: the DEFAULT UNIFORM BLOCK only. Keyed (ShaderCso, Version) and
    // emitted at most once per program per frame; `bytes` is MapUBO()'s image, GetUBOSize()
    // long, handed over as a companion pointer with Blob.Size 0. record.Version is
    // GetUBOContentVersion() and may never be ~0u, which is the backends' "never uploaded"
    // sentinel.
    void MGPipeApplySetGlobalConstants(const MGPGlobalConstants& record, const void* bytes);

    // ---------------------------------------------------------------------------------
    // P4a: the named, greppable unmigrated emulations (D-M)
    // ---------------------------------------------------------------------------------
    //
    // ROADMAP.md's P4a row ends "emulation 在 split 下显式 Fatal 直到 P8". In monolith the
    // code paths keep running exactly as today - the Fatal is a SPLIT-only arm - so this costs
    // P4a a named call site per unmigrated emulation and nothing else. P5/P8 give it teeth: a
    // split server that reaches one of these has no client address space to read and must
    // abort loudly rather than degrade silently.
    //
    // Monolith body: (void)name;. The list of names is pinned by
    // PipeCatalogueTest.EveryUnmigratedEmulationIsNamedOnce and the call count is grepped by
    // the purity gate, so a site that quietly disappears is a red gate rather than a surprise
    // at P8.
    void MGPipeUnmigratedEmulation(const char* name);

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
