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
// P5e (§2.1): MGPipeBarriered takes an MGPWireOp and reads the
// generated wait-class and call-class tables, all of which live in MGPipe.h. It is a sibling
// header in the same layer - MG_Pipe includes MG_Pipe - so the closure does not move.
#include "MGPipe.h"

// The applier: the server half of every MGPipe call (ARCHITECTURE.md §10.1). In one process the
// routing tables call these entry points directly; under a transport the server's PipeApplier
// decodes each record and calls the same ones. bind_render_state and set_dynamic_state scatter
// their chunks straight into PipeInputs::m_renderState, the block Espryt's SyncRenderState
// reads, so the verify comparator checks the assembled block against live state at every read.
// PipeInputs and the program artifact types are only forward-declared (MG_Pipe sits below
// MG_Backend): entry points take artifacts by pointer and a record holds its archive by
// SharedPtr, whose deleter is captured where it is built; users include ProgramArtifactsCodec.h.
namespace MobileGL::MG_State::GLState {
    struct LinkArtifacts;
    struct SpirvArtifacts;
    struct ProgramArchive;
} // namespace MobileGL::MG_State::GLState

namespace MobileGL::MG_Pipe {
    struct PipeInputs;
    // P5c (tx): MGPipeResourceOps::TextureRespecify names it; the definition is beside the
    // applier entry point that produces one (below, with MGPipeApplyResourceRespecify).
    struct MGPRespecifiedLevel;

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

    // The backend's resource op table, registered at bring-up and cleared at shutdown. Null means
    // no backend consumes resource records (see RefusedNoConsumer and PipeFill.cpp's
    // FamilyIsLive). No frontend type appears here: a resource is an MGPipeHandle, a payload
    // record and, where the call carries content, a companion `const void*` (in one process the
    // client's own shadow, so the call is zero-copy). resource_respecify carries no kHasBlob,
    // because such a record must own an MGPBlobRef and MGPResourceDesc has none.
    // SubDataResident is kOptional and may be null; callers check it.
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
        // P5c (tx): the TEXTURE half of the resource family, appended so every positional
        // initializer of the nine P3a members keeps its meaning. TextureSubData is called
        // from ApplyTextureUpload AFTER the gate, the accumulation and the serial, while
        // `bytes` still names the staged run (SEG_STAGE retires when the record does, so an
        // adoption anywhere later would read dead bytes - rule C); the backend copies the
        // run into the server's staged-texture store (MG_Remote/Server/StagedTextureStore.h)
        // and does nothing in monolith. TextureRespecify is the defined-ness/drop channel:
        // it rides MGPipeApplyResourceRespecify's own scope rules (a named level redefines
        // that level, a whole-resource respecify drops them all, a metadata update is not
        // delivered). TextureDestroy is resource_destroy's texture arm - the applier hands
        // only a buffer to Destroy, and a store keyed by the handle needs the death to drop
        // its key. All three may be null together: a backend that has not adopted the staged
        // shadow leaves them null and keeps the pre-tx shape.
        void (*TextureSubData)(MGPipeHandle res, const MGPSubData& record, const void* bytes,
                               const MGPSubRegion* regions);
        void (*TextureRespecify)(MGPipeHandle res, const MGPResourceDesc& desc, const MGPRespecifiedLevel* level);
        void (*TextureDestroy)(MGPipeHandle res);
        // P11 B2 (T0, B2): the two members a backend that can take a
        // client's AHardwareBuffer as a store registers. Both null = this backend has no T0 and
        // every session it serves runs T2. Appended after the texture trio for the same reason.
        //
        // ImportExternal makes `ahb` (an AHardwareBuffer*, a BLOB of `size` bytes) the resident
        // store of buffer `res` - the twin a monolith map_persistent would have minted - and takes
        // its OWN reference, which it releases only after its own GPU fence for the store's last
        // use has completed. False = not imported: the session answers DECLINED and the store runs
        // T2 as before.
        Bool (*ImportExternal)(MGPipeHandle res, void* ahb, Uint64 size);
        // The POST self-test of the sustained-lock pattern (the AHB API does not promise it): with
        // this backend's context current on the apply thread, allocate a small AHB, hold its CPU
        // lock for the whole test, import it, let the GPU read what the CPU wrote and write a
        // pattern of its own, and read that back through the held pointer (and through the
        // backend's own host map of the import). False with the reason in `why`.
        Bool (*SelfTestExternal)(char* why, Uint64 whyBytes);
        // T0 for a client in this process: the store's own host-visible coherent mapping (the
        // pointer a monolith map_persistent donates), handed to the client as its persistent map.
        // The store takes T0's semantics from here on. Null = not donated: the store runs T2.
        void* (*DonateInProcess)(MGPipeHandle res, Uint64 size);
    };

    // Install / read the table. A null argument uninstalls, which is what a backend does at
    // context teardown and what every build that has not migrated the family sits at.
    void MGPipeSetResourceOps(const MGPipeResourceOps* ops);
    const MGPipeResourceOps* MGPipeGetResourceOps();

    // ---------------------------------------------------------------------------------
    // P3a: the applier's own records (D-G4)
    // ---------------------------------------------------------------------------------

    // Slot bounds. The tables below grow by the slot index a client names, so one corrupt Uint32
    // would otherwise become a four-billion-entry resize: a slot at or above its bound is
    // Fatal{ProtocolCorruption}, never a resize. Naming a slot allocates nothing beyond the
    // client's own dense high-water mark. The bounds differ because the records do (a
    // vertex-elements record is ~1.3 KB). Resource bounds are per kind: Buffer, Texture and
    // Renderbuffer slots are allocated independently (MGPipeSlotAllocator), so each kind has its
    // own table, each bounded by kMGPipeMaxResourceSlots.
    inline constexpr Uint32 kMGPipeMaxResourceSlots = 1u << 20;
    inline constexpr Uint32 kMGPipeMaxVertexElementsSlots = 1u << 16;
    // create_render_state records retain one 396-byte pipeline half each. Keep their
    // slot-indexed table bounded independently so a peer cannot request a huge resize.
    inline constexpr Uint32 kMGPipeMaxRenderStateCsoSlots = 1u << 16;
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
    // ID-19(b): the framebuffer record is now PER OBJECT and its table is slot-indexed like the
    // five above, so it takes a bound on the same terms. A framebuffer record is 304 bytes and
    // an FBO is a CONTAINER object - not shared between contexts, minted a few dozen at a time
    // by a renderer and a few hundred by a shader pack - so 1<<16 is orders of magnitude above
    // any live population and still turns a corrupt Uint32 into a refusal rather than a
    // 4-billion-entry resize.
    inline constexpr Uint32 kMGPipeMaxFramebufferSlots = 1u << 16;

    // THE FOURTH set_framebuffer_state TARGET is the contract's MGPipeFramebufferTarget::Named (c0e):
    // "this record describes the framebuffer it names; no binding changes." Draw / Read / Both
    // write the record AND set the bound handle(s); Named writes the record only, which is how
    // the DSA entry points - BlitNamedFramebuffer and the four ClearNamedFramebuffer* - hand the
    // server a record for a framebuffer bound to neither binding (esprytobj review C-1, ID-19).

    // The two framebuffer BINDINGS, and there are two rather than three: Both and Named are
    // things a RECORD says, not bindings a server has. MGPipeApplierState::BoundFramebuffer is
    // indexed by MGPipeFramebufferTarget::Draw / ::Read, which is what makes package D's
    // "is this framebuffer the one bound to target t" one array compare (ID-19(d)).
    inline constexpr Uint32 kMGPipeFramebufferBindingCount = 2;
    static_assert(static_cast<Uint8>(MGPipeFramebufferTarget::Draw) == 0 &&
                      static_cast<Uint8>(MGPipeFramebufferTarget::Read) == 1,
                  "BoundFramebuffer is indexed by the target byte; Draw and Read must be 0 and 1");

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

    // The rect list behind one pending entry, and the most regions one record may carry. The
    // list is the level's EXACT dirty footprint since P11 M2 (TextureEmit.h, dev 9524c688): one
    // box per disjoint piece of the writes, however many, where it used to be MipmapStorage's
    // bounded cover (kMaxDirtyRects = 96, "0 rects" past it). An accumulation that would exceed
    // this still collapses to BOX ONLY - never a dropped region, since the box covers every
    // texel the list named - but a box is no longer the same answer: it spans the gaps between
    // the writes, and a server whose image may hold GPU writes there uploads the stale shadow
    // over them. So the bound is set far above what a frame's writes to one level come to (a
    // hundred atlas sprites is ~100), and the collapse is the rare case it was before rather
    // than the ordinary one. 4096 regions of 40 bytes is a 160 KiB tail, a small fraction of
    // the half of SEG_CMD one record may take.
    inline constexpr Uint32 kMGPipeMaxPendingUploadRegions = 4096;

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
        // "DOES THIS RESOURCE HAVE A LIVE HOST WRITER RIGHT NOW?"
        //
        // False and written by nobody through P3a and P4a; P5 (b1) is the phase it was waiting
        // for and gives it its producer, with zero new record kinds exactly as planned: the
        // bit rides MGPSubData::HasLiveHostWrites - a byte out of that payload's existing pad -
        // and ApplyBufferWrite assigns it here. In a MONOLITH build nothing sets it and the
        // MOBILEGL_PIPE_VERIFY wire (PinNoLiveHostWrites) still refuses a producer, so the pin
        // survives the phase it was written for instead of being deleted by it.
        //
        // It is what IsBufferDrawCleanByHandle asks under split INSTEAD of the frontend
        // object's IsMapped(), because a spawned server has no frontend object to ask. Getting
        // that substitution wrong once already cost a silent regression - an emulated
        // persistent map read draw-clean for ever - which MG_Test/SanityTest.cpp's
        // DirectGLESBufferDrawProbe pair now pins from both sides.
        Bool HasLiveHostWrites = false;

        // SHARED IMAGES. Nonzero: this
        // texture's level-0 storage IS the server's shared image of this id (an EGLImage target),
        // set by shared_image's Attach and cleared by the next respecify that redefines storage.
        // The backend resolves the id at its texture sync (MG_Remote::Server::SharedImages::Find)
        // and binds that buffer instead of allocating storage; Serial moves with it.
        Uint64 SharedImageId = 0;

        // ---- P4a. Only a record of kind Texture ever carries these; a buffer's stay at
        // their defaults, which is what keeps ONE record type for the discriminated
        // descriptor rather than a second one that would have to be kept in step with it.

        // set_texture_params, per texture OBJECT and independent of any binding - which is
        // the whole point of addressing it by resource: a texture that is only an FBO
        // attachment, only an image-unit binding or only a glCopyImageSubData endpoint has no
        // sampler view to hang its parameters on, and today the READ-attachment case reaches
        // no parameter push at all. ParamsSerial replaces the twin's
        // m_syncedTextureParamsVersion + m_forceTextureParamsResync pair.
        //
        // Params.BuiltinSampler MAY NAME A CSO WHOSE RECORD IS GONE. set_texture_params
        // deliberately does not resolve it (the sampler CSO is content-addressed and shared,
        // D-F1, and the ordering between the two families is the emitter's), and
        // delete_sampler_state does not sweep the textures that name the CSO it drops. So a
        // consumer that follows this handle must expect SamplerCsos[slot] to be dead or
        // recycled and treat that as it treats any other stale handle - it is an ordering fact
        // about the two emitters, not a corrupt record.
        MGPTextureParams Params{};
        Uint64 ParamsSerial = 0;
        // The SamplerViewCso minted for this texture (P4a D-F2: one per ITextureObject,
        // re-issued on the same handle whenever the restrictions move).
        MGPipeHandle ViewCso = kMGPipeNullHandle;
        // The pending-upload set, server-side state (ARCHITECTURE.md §6). The client clears a
        // level's dirty flags once the applier accepts its record, but a backend's upload loop
        // may bail without uploading (an incomplete texture, a multisample target), so emitted
        // shapes accumulate here and the backend consumes and clears an entry only where it
        // uploads. Keys (UploadTarget, Level) are independent: a per-level respecify drops only
        // its own key. Regions concatenate successive emissions and may overlap; staging them
        // uploads some texels twice, which costs time but stays correct, so no consumer may
        // assume them disjoint.
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

    // create/bind/delete_shader_state, plus set_global_constants' per-program half. The record
    // stores the descriptor and adopts the link's artifacts as Archive (see
    // MGPipeApplyCreateShaderState). GlobalConstants is bounded by Desc.GlobalUboSize and written
    // at most once per program per frame, keyed by (ShaderCso, Version).
    // MGPipeProgramStorageOverride is one entry of set_program_bindings' storage-override tail as
    // the record keeps it: the block name is copied because its wire span into SEG_STAGE retires
    // with the record that named it, while the bindings are rebuilt at a later bind.
    struct MGPipeProgramStorageOverride {
        String Name;
        Int32 Binding = 0;
    };

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

        // ---- The program archive the record owns -----------------------------------------
        // A snapshot of this link's artifacts, so every reflection read by the program twin is
        // of memory the record owns: ProgramObject::Link() replaces the frontend's artifacts in
        // place, and a client running ahead may already have relinked. Under a transport the
        // decoder deserializes it from SEG_STAGE; in one process PipeRoute copies it without the
        // codec. A SharedPtr because a program-pipeline composite and its stage programs share
        // one link's artifacts and the twin holds it across a build. Every create replaces it.
        SharedPtr<const MG_State::GLState::ProgramArchive> Archive;

        // ---- P5e's set_program_bindings tails (CONTRACT-P5E.md §1, §5.5) -------------------
        //
        // DECLARED BY c0e, FILLED BY PACKAGE pg. THE THREE POST-LINK MUTABLE REFLECTION FIELDS
        // the archive cannot answer: glUniformBlockBinding, glUniform1i on a sampler uniform and
        // glShaderStorageBlockBinding all move a binding INSIDE the frontend's LinkArtifacts
        // after the link, so a twin that answered a draw from the archive alone would bind the
        // uniform blocks the program was LINKED with rather than the ones it is BOUND with.
        //
        // A RE-ISSUED create_shader_state CLEARS ALL OF THEM, for the reason it clears
        // GlobalConstants: a relink replaces the archive these index into, so a surviving tail
        // would name block indices and uniform locations of a program that no longer exists.
        // The client re-emits set_program_bindings before the create that describes the relink.
        Vector<Int32> BlockBindings;                           // dense, GL uniform-block index order
        Vector<MGPProgramSamplerUnit> SamplerUnits;            // sparse, ascending Location
        Vector<MGPipeProgramStorageOverride> StorageOverrides; // name-keyed by design
        // The client's commutative hash of the override MAP, so the twin's "must I rebuild the
        // storage-block bindings" clause is one Uint64 compare and the name tail is touched only
        // on a rebuild. 0 is "no overrides", which is what an untouched program has.
        Uint64 Signature = 0;
        // Server-owned MGGen, ++ on every applied set_program_bindings for this record. It is
        // what the program twin's clean condition and both texture-unit memos key on (ruling 6),
        // so it obeys VertexBuffersSerial's rule: ADVANCED, never returned to a value it has
        // already handed out.
        Uint64 BindingsSerial = 0;

        // P14: the backend program factory's content hash of THIS record's program, one entry per
        // compile variant (the factory's own memo key). The hash reads the archive's SPIR-V, the
        // block-binding tail and the variant, so an entry is valid exactly while Serial (a relink)
        // and BindingsSerial (set_program_bindings) still hold the values it was taken at. It
        // lives in the record so it dies with it: a recycled slot starts from a fresh record and
        // no other record can ever answer from it. Nothing in MG_Pipe reads it.
        struct BackendHashMemoEntry {
            Uint64 Serial = 0;
            Uint64 BindingsSerial = 0;
            Uint64 Hash = 0;
            // The hash of the variant of this program the factory last handed out for this key:
            // Hash itself, or a variant keyed on draw-time vertex data. Derived from Hash, so it
            // is valid exactly as long.
            Uint64 LastVariantHash = 0;
            Uint32 Key = 0;
            Bool Valid = false;
        };
        mutable Array<BackendHashMemoEntry, 4> BackendHashMemo{};
        mutable Uint32 BackendHashMemoNext = 0;
        // A backend's own derivation from the archive: the locations of its 32-bit int and uint
        // vertex inputs, valid while Serial holds the value it was taken at. It dies with the
        // record like the memo above, and nothing in MG_Pipe reads it.
        struct BackendIntegerInputsMemo {
            Uint64 Serial = 0;
            Uint32 SignedInputs = 0;
            Uint32 UnsignedInputs = 0;
            Bool Valid = false;
        };
        mutable BackendIntegerInputsMemo BackendIntegerInputs{};
    };

    // The vertex-elements CSO as the applier holds it: the unpacked blob, both views, plus
    // the serial the backend's per-VAO twin compares against instead of a wrapping Uint16
    // configuration version plus an identity patch.
    //
    // The 32 is GL's MAX_VERTEX_ATTRIBS as MobileGL advertises it (kMGPipeMaxVertexAttribs,
    // MGPipeTypes.h), which is also the bound the record's two declared counts are checked
    // against before the blob is unpacked.
    // The widest vertex-buffer set a vertex-elements record keeps as its own (see below). A
    // wider set is not kept, and the client, which mirrors this rule, then re-sends it after
    // every bind of that array.
    inline constexpr Uint32 kMGPipeVaoStoredVertexBuffers = 16;

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
        // P14: THE ARRAY'S OWN BUFFER AND ELEMENT BINDINGS, as GL keeps them on the vertex array
        // object. The last set_vertex_buffers (window starting at 0, at most
        // kMGPipeVaoStoredVertexBuffers entries) and the last set_index_buffer applied while this
        // record was bound are kept here, and bind_vertex_elements re-applies them, exactly as if
        // the two records had arrived again - so a client switching between arrays whose
        // bindings did not move sends the bind alone. create_vertex_elements on the handle drops
        // both (the client drops its mirror at the same create), so a kept set never outlives the
        // configuration it was sent for. The client's mirror (VertexInputEmit.h) only ever
        // believes a kept set the server also keeps; where it is unsure it re-sends.
        Bool StoredBuffersValid = false;
        Bool StoredIndexValid = false;
        MGPVertexBuffers StoredBuffersHeader{};
        Array<MGPVertexBuffer, kMGPipeVaoStoredVertexBuffers> StoredBuffers{};
        MGPIndexBuffer StoredIndex{};
        // P14: a backend's own derivation from this record (Magma's resolved vertex input
        // layout), opaque here. It lives and dies with the record - a re-created or recycled
        // record starts without it - and the backend keys it on ContentSerial plus the inputs it
        // read, so it is never believed for a configuration it was not built from.
        mutable SharedPtr<void> BackendMemo;
    };

    // set_framebuffer_state's record, HELD PER FRAMEBUFFER OBJECT and indexed by the handle's
    // slot (ID-19(b)). It is the one record kind in this file whose object has NO WIRE LIFETIME:
    // the catalog has no framebuffer create and no framebuffer destroy, because a framebuffer
    // is state and set_framebuffer_state is the only call that names one (D-I2). So there is
    // nothing to mark dead and nothing to refuse against, and a slot is simply OVERWRITTEN by
    // its successor's record - which is correct rather than merely tolerable, since the record
    // that reaches this table is the description of whatever object holds the slot NOW.
    //
    // `Live` is therefore NOT a lifetime. It means "a record has been written at this slot",
    // which is the only question a reader can ask: it separates a table entry that exists
    // because the vector grew past it from one an emission actually wrote. The GENERATION is
    // still checked on every lookup (P3a contract-review M2), and a mismatch is a LOUD refusal -
    // it means an emitter handed a stale handle, or minted a successor without describing it,
    // which is exactly the seam defect the DSA arm would otherwise turn into a blit into a
    // driver framebuffer with no attachments.
    struct MGPipeFramebufferRecord {
        Uint32 Gen = 0;
        Bool Live = false;
        MGPFramebufferState State{};
    };

    // =====================================================================================
    // The share-group half of the applier's state (design/11-state-ownership.md).
    // =====================================================================================
    // GL objects live in a share group, so these records are keyed by {session, share group}:
    // contexts of one group see one set, unshared contexts see separate sets. A make-current
    // keeps them, because the backend reads a buffer's extent and serial from its record. A
    // record is cleared by its object's death signal, or by MGPipeApplierUnregisterContext when
    // the group's last context goes; MGPipeApplierReset clears only MGPipeWorkingState.
    struct MGPipeObjectRecords {
        // ---- object records: indexed by MGPipeHandle::Slot of kind Buffer /
        // VertexElementsCso, and NOT part of the working state. They are cleared by the object's
        // OWN death signal and by the group's teardown. Nothing else.
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
        // them, and only the object's own death signal and the group's teardown clear them.
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
        // Framebuffer records, keyed by the framebuffer handle's slot (see
        // MGPipeFramebufferRecord), so a framebuffer bound to neither binding - the target of a
        // DSA blit or clear - still has one. MGPipeApplierReset keeps this table: on a fresh
        // context the client re-emits only the two bound records, so a framebuffer described
        // only by name would lose its record. Slots come from one global allocator, so a share
        // group's table answers only for handles its own contexts minted and never aliases.
        Vector<MGPipeFramebufferRecord> FramebufferRecords;
    };

    // The per-context half. Everything here is cleared by MGPipeApplierReset, EXCEPT the serials,
    // which only ever advance (see there).
    struct MGPipeWorkingState {
#if MOBILEGL_PIPE_VERIFY
        // Reference from the pack-state payload, separate from the backend-visible storage.
        // The server has no frontend GLContext to use as a read-time oracle.
        PixelStoreParameters VerifyPixelPack{};
#endif
        // Indexed by slot; slot 0 is the reserved null handle and is never live
        // (MGPipeHandles.h kMGPipeFirstAllocatableSlot).
        Vector<MGPipeRenderStateCsoRecord> RenderStateCsos;
        // The last bind, so a rebind of the same handle can be answered without a scatter.
        MGPipeHandle BoundRenderStateCso = kMGPipeNullHandle;
        // The global chunk bits (MGPipeRenderStateSpans.h numbering) this applier has scattered
        // into PipeInputs::m_renderState since the last reset: which bytes of the working block
        // are the applier's rather than the residual fill loop's. MGPipeApplySetPatchState's
        // trip wire compares against chunk P0 only once P0 is in this ledger: a set_patch_state
        // before a context's first bind has nothing to agree with, and bytes the applier did not
        // scatter are no oracle. set_patch_state's own write stays out of the ledger: comparing
        // that carrier against itself would prove nothing.
        Uint32 ScatteredChunkBits = 0;

        // What the two trip wires last did. A wire nothing can observe is a gate that cannot
        // go red for the reason it exists (ROADMAP.md), and only a poison or verify build
        // aborts: the shipped push build counts and logs, so these counters are how a unit
        // case sees the wire fire in EVERY build rather than in one.
        Uint32 PatchCarrierComparisons = 0; // cumulative, armed set_patch_state calls only
        Uint32 PatchCarrierDivergences = 0; // cumulative

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
        // Sampler, sampler-view, program and texture-params calls refused because they named a
        // record this applier does not have. One counter, because the families share one legal
        // refusal sequence (a teardown drops records before the objects' death notices arrive);
        // the log line names the call and the handle. set_framebuffer_state never moves it: a
        // framebuffer has no wire lifetime, so the call looks nothing up, and a stale lookup on
        // the server's read path counts in StaleFramebufferRecordLookups. A malformed record (a
        // var-tail window out of bounds, a null BuiltinSampler) is Fatal{ProtocolCorruption}.
        Uint64 RefusedObjectCalls = 0;

        // Non-buffer resource, sampler, program and framebuffer calls declined because nothing
        // consumes them: no MGPipeResourceOps is registered and, under a transport, the session's
        // caps do not name the family either. The client clears dirty flags, advances its mirrors
        // and latches suppressors on acceptance, so accepting a record nobody reads loses work.
        // PipeFill.cpp's FamilyIsLive stops these upstream; this catches emitters called directly,
        // such as GL_Framebuffer.cpp's PipePublishFramebufferByName. Not a defect, so not logged.
        // Idempotent death paths (resource_destroy, delete_*) are not counted. Cleared by
        // MGPipeApplierReset.
        Uint64 RefusedNoConsumer = 0;

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

        // WHICH FRAMEBUFFER IS BOUND TO EACH BINDING, and that is ALL this pair is since
        // ID-19(b): the record itself lives in FramebufferRecords above, keyed by the handle.
        // Indexed by MGPipeFramebufferTarget::Draw / ::Read. kMGPipeNullHandle means "nothing
        // described this binding yet", which is what a make-current leaves behind.
        //
        // set_framebuffer_state Draw / Read / Both writes the RECORD at state.Fbo's slot AND
        // sets the handle(s) here; Named (MGPipeFramebufferTarget::Named) writes the record and
        // touches nothing here at all - that is the whole of the fourth target's meaning.
        Array<MGPipeHandle, kMGPipeFramebufferBindingCount> BoundFramebuffer{};
        // ONE SERIAL FOR THE FAMILY, and it moves on EVERY write - a Named record's included,
        // because a twin memoising "the framebuffer state I have seen" has to hear about a
        // named framebuffer's attachments exactly as it hears about a bound one's. It is the
        // number that retires the four g_fboSynced* arrays and the twin's {slot version, object
        // version, backend id generation} triple.
        Uint64 FramebufferSerial = 0;
        // Every FramebufferRecordFor() that found a record at the slot whose GENERATION had
        // moved on. It is NOT RefusedObjectCalls: this is a READ by the server's own sync path
        // and not a call this applier refused, and set_framebuffer_state's counter contract
        // (below) is that no framebuffer call ever moves that one. A non-zero value here is a
        // seam defect - an emitter minted a successor for a recycled slot and never described
        // it, or handed out a handle it had already retired - so it is counted AND logged, and
        // a unit case reads it in every build for the reason the other counters exist.
        //
        // `mutable` because the three accessors below are const: package E holds the applier
        // through a `const auto&` and must keep doing so.
        mutable Uint64 StaleFramebufferRecordLookups = 0;

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

        // ---- P5e: THE INDEXED BUFFER BINDING POINTS (CONTRACT-P5E.md §5.6, rulings 10/11) ---
        //
        // DECLARED BY c0e, FILLED BY PACKAGE sb. One window per CLASS, because the record is
        // per class - a set_shader_buffers names Uniform, ShaderStorage or AtomicCounter and
        // describes THAT target's array. XFB does NOT ride this state: its targets are span-
        // scoped and carry a Generation this payload has no field for, so set_stream_output_
        // targets keeps its own row and XFB stays lockstep (§5.7).
        //
        // THE VAR-TAIL WINDOW IS THE BOUND, exactly as it is for the three unit sets above:
        // Start is 0 by contract, Count is the touched high-water mark, and a binding at or
        // above Count means "nothing bound" - which is what the frontend array's default says
        // too, so the server does not have to clear entries outside the window to be right.
        Array<MGPBufferRange, kMGPipeMaxBufferBindingPoints> BoundShaderBuffers[kMGPipeShaderBufferClassCount]{};
        Uint32 ShaderBufferStart[kMGPipeShaderBufferClassCount] = {0, 0, 0};
        Uint32 ShaderBufferCount[kMGPipeShaderBufferClassCount] = {0, 0, 0};
        // Which entries of classes 1 and 2 the shader may WRITE through. It is what replaces
        // the backend's own GPU-write marking walk (DirectGLES.cpp's three MarkBufferGpuWritten
        // sites), which sb deletes under a transport because the client already owns the
        // GPU-write set and the server's walk is over client memory.
        //
        // THREE WORDS PER CLASS, NOT ONE (P5e, ID-104): the window is 84 points and c0e's
        // single Uint32 could describe only the first 32 of them, so a storage buffer bound at
        // point 32 or above read as read-only here while the record said nothing was wrong.
        // Always through MGPipeShaderBufferMaskHas / ...Set (MGPipeTypes.h), which is the same
        // arithmetic the payload's own mask goes through - one table, both sides.
        Uint32 ShaderBufferWritableMask[kMGPipeShaderBufferClassCount][kMGPipeShaderBufferWritableMaskWords] = {};
        // ONE serial for all three classes, ++ on every applied record and ADVANCED (never
        // zeroed) by MGPipeApplierReset, for VertexBuffersSerial's reason - and the emitter's
        // latch resets with it, or the first emission after a make-current is suppressed as
        // unchanged.
        Uint64 ShaderBuffersSerial = 0;

        // ---- P5e: the applied value the barriered predicate's XFB escalation reads ---------
        //
        // MGPContextValues::IsTransformFeedbackActive as set_context_values last delivered it.
        // It is mirrored HERE, beside the applier's other working state, and not only written
        // into gPipeInputs, because MGPipeBarriered must be computable on the SERVER from
        // applier state alone: the record that carries it precedes the verb on the ring, so
        // both roles evaluate the same predicate over the same value (§2.1, escalation (i)).
        Bool IsTransformFeedbackActive = false;

        // ---- Server-side answers for the three texture shutters (FieldOwnership.def,
        // APPLIER_DERIVED): the PipeInputs accessors answer from these serials under a
        // server-stamped verb. TextureShutterSerial answers GetSamplingResolutionGeneration and
        // GetTextureBindGeneration; it moves on every applied record that can change what they
        // guard (the unit sets, set_texture_params, a resource respecify or destroy,
        // bind_shader_image) and resets advance it, never zero it. Over-firing only costs a
        // re-sync. ContextSerial answers GetTextureContextId; every MGPipeApplierReset moves it.
        Uint64 TextureShutterSerial = 0;
        Uint64 ContextSerial = 0;

        // ---- P5c (hd, CONTRACT-P5C §3.2): THE CURRENT VERB'S OWN HANDLES. ----------------
        //
        // Server state, written by ServerVerbSink at the top of a verb's dispatch and read by
        // the backend DURING THAT SAME VERB, so the apply thread resolves the verb's objects
        // from the handles the record carried instead of probing the client's slot allocator
        // for a frontend object's lifetime id (T2). It is per-verb WORKING state, not object
        // state: every writer overwrites the whole set for its verb (null included), so a
        // value is only ever read between its verb's write and that verb's return, and there
        // is nothing to clear at a reset.
        //
        // Blit: both null = the bound-form blit (the bindings answer, as before). A non-null
        // pair names the read/draw framebuffers of a NAMED blit (MGPBlit::ReadFbo/DrawFbo);
        // the backend must then run its named-blit arm and raise VerbBlitNamedConsumed - the
        // sink reads that flag back so a backend with no named arm is a loud decline rather
        // than a silent blit of whatever is bound.
        MGPipeHandle VerbBlitReadFbo = kMGPipeNullHandle;
        MGPipeHandle VerbBlitDrawFbo = kMGPipeNullHandle;
        Bool VerbBlitNamedConsumed = false;
        // copy_framebuffer_to_texture's destination texture (MGPCopyFromFramebuffer::Dst).
        MGPipeHandle VerbCopyTexDst = kMGPipeNullHandle;
        MGPipeHandle VerbStorageBlockProgram = kMGPipeNullHandle;
        Uint64 BoundStreamOutputLifetimeId = 0;
        Uint64 VerbDeleteStreamOutputLifetimeId = 0;
        UnorderedMap<Uint64, MGPStreamOutputBegin> StreamOutputSpans;
        // generate_mipmap's texture (MGPMipPlan::Res).
        MGPipeHandle VerbMipRes = kMGPipeNullHandle;
        Uint16 VerbMipBaseLevel = 0;
        Uint16 VerbMipLevelCount = 0;
        // The current indirect draw's command buffer and (for the *Count forms) parameter
        // buffer (MGPDrawIndirect::Buffer / ParameterBuffer).
        MGPipeHandle VerbIndirectBuffer = kMGPipeNullHandle;
        MGPipeHandle VerbIndirectParameterBuffer = kMGPipeNullHandle;
        // dispatch_indirect's command buffer (MGPGridInfo::IndirectBuffer).
        MGPipeHandle VerbDispatchIndirectBuffer = kMGPipeNullHandle;

        // P5e: "is the record being applied barriered?" is NOT a member of this struct. It is
        // thread_local beside MGPipeApplierCurrentRecordIsBarriered() in PipeApply.cpp, because
        // it describes which record THIS THREAD is inside rather than per-context state the
        // applier owns - and because a reader (the allocator guard) that had to find the right
        // context's applier first would be asking a harder question than the one it needs
        // answered. The integrator resolved the two packages' duplicate here (ID-103).

        // Every verb's writer calls this FIRST and then sets its own fields, so no field ever
        // outlives the verb that wrote it and a reader can treat non-null as "this verb's".
        void ClearVerbHandles() {
            VerbBlitReadFbo = kMGPipeNullHandle;
            VerbBlitDrawFbo = kMGPipeNullHandle;
            VerbBlitNamedConsumed = false;
            VerbCopyTexDst = kMGPipeNullHandle;
            VerbStorageBlockProgram = kMGPipeNullHandle;
            VerbDeleteStreamOutputLifetimeId = 0;
            VerbMipRes = kMGPipeNullHandle;
            VerbMipBaseLevel = VerbMipLevelCount = 0;
            VerbIndirectBuffer = kMGPipeNullHandle;
            VerbIndirectParameterBuffer = kMGPipeNullHandle;
            VerbDispatchIndirectBuffer = kMGPipeNullHandle;
        }
    };

    // P14: where a new applier's working-state serials start. Each applier (one per served context)
    // gets a range of its own, 2^40 values wide, so no two appliers ever answer with the same serial:
    // the backends key process-wide memos on these serials (a texture sync list on ContextSerial and
    // SamplerViewsSerial, the synced framebuffer binding on FramebufferSerial, ...), and one apply
    // thread serves every context of its session. With every applier counting from 0, two contexts
    // at the same point of their lives answered the same keys, and a draw in one replayed the memo
    // the other had built - its own textures went unsynced and it sampled them empty.
    Uint64 MGPipeNextApplierSerialBase();

    // =====================================================================================
    // P14 S5: ONE CONTEXT'S APPLIER = ITS OWN WORKING STATE + ITS SHARE GROUP'S RECORDS.
    // =====================================================================================
    //
    // THE INHERITANCE IS THE POINT AND NOT A SHORTCUT: everything the backend spells as
    // `MGPipeApplier().X` for a WORKING field keeps working unchanged (there are ~380 such reads
    // in MG_Backend), and every read of an OBJECT record moves to `MGPipeApplier().Objects.X` -
    // which is what makes the two lifetimes visible at exactly the sites where confusing them
    // would be a defect. The reference is to a MGPipeObjectRecords that OUTLIVES this object by
    // construction: the registry owns the group's records and this context's applier together,
    // and drops them in that order.
    //
    // A context that started its own share group still has one - a group of one - so every
    // applier has somewhere for its objects to live and there is no "no group" case.
    struct MGPipeApplierState : MGPipeWorkingState {
        // The share group this context's objects live in.
        MGPipeObjectRecords& Objects;

        // ---- THE OBJECT TABLES, UNDER THE NAMES EVERY CALLER ALREADY SPELLS. --------------
        //
        // These nine are the group's tables, not copies of them: ~200 call sites in
        // MG_Backend, MG_IntegrationTest and MG_Test read `st.Resources`, `state.TextureResources`
        // and their siblings, and the name they use has ALWAYS meant "the share group's record
        // for this object". Binding them here keeps every one of those reads correct by
        // construction - through the group its own context belongs to - instead of turning a
        // keying change into a renaming change. `Objects.X` and `X` therefore name the same
        // vector; this file spells the group half explicitly where it CREATES the two halves,
        // the backend keeps the name it has.
        Vector<MGPipeResourceRecord>& Resources;
        Vector<MGPipeVertexElementsRecord>& VertexElementsCsos;
        Vector<MGPipeResourceRecord>& TextureResources;
        Vector<MGPipeResourceRecord>& RenderbufferResources;
        Vector<MGPipeSamplerCsoRecord>& SamplerCsos;
        Vector<MGPipeSamplerViewRecord>& SamplerViewCsos;
        Vector<MGPipeShaderCsoRecord>& ShaderCsos;
        Vector<MGPipeShaderCsoRecord>& CompositeShaderCsos;
        Vector<MGPipeFramebufferRecord>& FramebufferRecords;

        explicit MGPipeApplierState(MGPipeObjectRecords& objects)
            : Objects(objects), Resources(objects.Resources), VertexElementsCsos(objects.VertexElementsCsos),
              TextureResources(objects.TextureResources), RenderbufferResources(objects.RenderbufferResources),
              SamplerCsos(objects.SamplerCsos), SamplerViewCsos(objects.SamplerViewCsos),
              ShaderCsos(objects.ShaderCsos), CompositeShaderCsos(objects.CompositeShaderCsos),
              FramebufferRecords(objects.FramebufferRecords) {
            const Uint64 base = MGPipeNextApplierSerialBase();
            VertexBuffersSerial = IndexBufferSerial = FramebufferSerial = SamplerViewsSerial = base;
            SamplerStatesSerial = ShaderImagesSerial = ProgramBindingSerial = ShaderBuffersSerial = base;
            TextureShutterSerial = ContextSerial = base;
        }

        // ---- The framebuffer accessors. DrawFramebuffer() / ReadFramebuffer() resolve
        // BoundFramebuffer[t] through FramebufferRecords. Null has three causes: nothing is
        // bound (what a make-current leaves; not an error), no record was written at the slot,
        // or the slot's generation moved on under the handle (an error, counted in
        // StaleFramebufferRecordLookups and logged). Defined in PipeApply.cpp because the
        // logging path needs MG_Util/Debug/Log.h, which must stay out of this header's includes.
        const MGPFramebufferState* FramebufferRecordFor(MGPipeHandle fbo) const;
        const MGPFramebufferState* DrawFramebuffer() const;
        const MGPFramebufferState* ReadFramebuffer() const;
    };

    // The calling thread's applier. On a thread that belongs to a served session this is that
    // session's CURRENT CONTEXT's working state bound to its share group's object records; on
    // every other thread - the client process, the in-process client+server shape, a unit case,
    // the whole monolith - it is the process-wide applier, which is what it has always been and
    // is byte-for-byte the old behavior.
    //
    // P14: INLINE, because the backends ask it per state read (dozens of times per draw): in a
    // process that serves no session - the whole monolith - no resolver is installed and the
    // answer is the process-wide applier, a load and a branch with no call.
    namespace Detail {
        // Mirrors of PipeApply.cpp's g_applierKeyResolver / g_defaultApplier, written only there.
        extern void* g_applierResolverInstalled;
        extern MGPipeApplierState* g_processApplier;
    } // namespace Detail
    MGPipeApplierState& MGPipeApplierForSession();
    inline MGPipeApplierState& MGPipeApplier() {
        if (Detail::g_applierResolverInstalled == nullptr && Detail::g_processApplier != nullptr)
            return *Detail::g_processApplier;
        return MGPipeApplierForSession();
    }

    // P13 W6: the backend's "my capabilities changed" notice, without naming the frontend. A
    // backend whose advertised limits / extensions move after a context exists (Magma's caps
    // re-query) calls MGPipeNotifyCompileEnvChanged(); the frontend registers the hook that
    // invalidates its shader compile environment (PipeFill.cpp, static registration). Under a
    // transport no hook is registered on the server side and the client invalidates on the caps
    // re-publication instead (CONTRACT-P5C §4.2), so the notice is a no-op there.
    using MGPipeCompileEnvChangedHook = void (*)();
    void MGPipeSetCompileEnvChangedHook(MGPipeCompileEnvChangedHook hook);
    void MGPipeNotifyCompileEnvChanged();

    // =====================================================================================
    // Whose applier is it? (docs/Disaggregated/design/11-state-ownership.md)
    // =====================================================================================
    // A resolver the session registry installs once answers for the calling thread, so
    // MG_Backend and MG_Impl never name a session. Working state is keyed by {SessionKey,
    // ContextToken}, object records by {SessionKey, ShareGroupKey}; a share group never spans
    // sessions (EGL ties it to one display, i.e. one client connection). Contexts are registered
    // on the apply thread at CreateContext; an unregistered token gets a group of its own.
    struct MGPipeApplierKey {
        Uint64 SessionKey = 0;
        Uint64 ContextToken = 0;
    };
    // Answers false for "this thread is not a session's" (the process-wide applier). Installed by
    // the session registry; null in every process that serves no session.
    using MGPipeApplierKeyResolver = Bool (*)(MGPipeApplierKey* outKey);
    void MGPipeSetApplierKeyResolver(MGPipeApplierKeyResolver resolver);

    // Mints (or finds) the working state for {session, context} and the object records of the
    // share group it belongs to. `shareGroupToken` is the frontend's own share-group identity
    // (EGLState::CreateContext mints it, a sharing context inherits its parent's); 0 means this
    // context started its own group, and is keyed by the context token in a namespace of its own
    // so it cannot alias a group that happens to carry that number.
    void MGPipeApplierRegisterContext(Uint64 sessionKey, Uint64 contextToken, Uint64 shareGroupToken);
    // Drops {session, context}'s working state, and its share group's object records when the
    // last context of that group has gone (MGPipeApplierReleaseObjectRecords' scope, per group
    // instead of per process).
    void MGPipeApplierUnregisterContext(Uint64 sessionKey, Uint64 contextToken);
    // Every context of a served session, at session teardown.
    void MGPipeApplierReleaseSession(Uint64 sessionKey);

    // The observables that make the keying testable without reading a private map: how many
    // groups one context's session holds, and how many object records the group the CALLING
    // thread is in holds. Both are the numbers a case asserts on to tell "isolated" from
    // "shared" apart from the behavior they exist to explain.
    Uint64 MGPipeApplierShareGroupCountForTesting(Uint64 sessionKey);

    // P14 S6: THE SHARE-GROUP BUCKET A {session, context token} PAIR IS FILED UNDER, answered
    // from the registry rather than re-derived. The backend's twin tables are keyed by the same
    // pair (DirectGLES.h's TwinKey), and a record and the twin describing it must never land in
    // different buckets - so there is ONE spelling of the rule, here. A registered context
    // answers its group; anything else (token 0, the 0 of a session that has not bound one yet,
    // a context no CreateContext ever named) answers the group that context starts itself,
    // exactly as FindApplierForKey mints it. Never fails, so a caller has no third answer to
    // invent.
    Uint64 MGPipeApplierShareGroupKeyFor(Uint64 sessionKey, Uint64 contextToken);

    // ---- The barriered predicate (ARCHITECTURE.md §11.6.1) --------------------------------
    // The server's ApplyOne asks it whether the record it is about to apply may read client
    // memory; the client evaluates the same clauses before it publishes (ClientSession's wait
    // rule, PipeFill.cpp's ClientVerbIsBarriered). It reads only wire-visible data (the static
    // wait class, the payload, applier state set by earlier records), so both sides can agree.
    // Clauses: wait class != kWaitNone; a kCtxVerb inside an open transform-feedback span; a
    // draw_vbo with kDrawClientArrays. A new clause goes here and into the client's mirror in
    // the same change. A null `payload` skips the last clause.
    Bool MGPipeBarriered(MGPWireOp op, const void* payload, const MGPipeApplierState& st);

    // Whether the record the apply thread is INSIDE is a barriered one. Server-private: written
    // by the sink's ApplyOne from MGPipeBarriered before it dispatches, read by
    // MGPipeRefuseAllocatorFromApplyThread (§4.4) to decide whether a frontend-keyed registry
    // probe is a named, accounted debt or a role violation.
    //
    // IT ANSWERS `true` BY DEFAULT AND THAT IS THE LANDED LOCKSTEP ANSWER, not a stub: on a
    // server that does not publish kCapRunAheadApply - Magma always, and Espryt until the
    // integration commit - EVERY record is barriered, the client is parked in its own wait for
    // each of them, and P5C's semantics hold unchanged. An asserting body here would abort the
    // monolith and the lockstep split alike, which is why this one seam is implemented rather
    // than declared: the safe answer and the correct answer are the same answer.
    Bool MGPipeApplierCurrentRecordIsBarriered();
    // The one writer, called by the sink at the top of every record's dispatch. Package id owns
    // the call site (it is the line ra rebases onto); c0e owns the storage so both compile.
    void MGPipeApplierSetCurrentRecordBarriered(Bool barriered);

    // ---- Was the current record barriered only by escalation? ----------------------------
    // True when MGPipeBarriered(op, payload, st) holds but MGPipeWaitClassFor(op) == kWaitNone:
    // the transform-feedback or client-array clause alone barriered it. Separate from the stamp
    // above, which answers "is the client parked behind this record" for the allocator and
    // registry guards. The only reader is the residual-pull check under
    // MOBILEGL_IPC_STRICT_ERRORS (PipeInputs.cpp), which admits a pull on an escalated record.
    // Defaults to false, so a reader that runs before any writer is strict.
    Bool MGPipeApplierCurrentRecordIsBarrieredByEscalation();
    void MGPipeApplierSetCurrentRecordBarrieredByEscalation(Bool escalated);

    // Whether the client implements the run-ahead wait rule MGPipeBarriered describes. The
    // server stamps a record with the predicate only when this and its own run-ahead cap bit
    // both hold (MGPipeApplierStampsBarriered); otherwise the client blocks after every record,
    // so every record is stamped barriered. It moves together with kMGPipeP5eRunAheadReady
    // (MG_Backend/Init.cpp). ApplyOne computes the predicate on every record either way, so it
    // stays exercised.
    inline constexpr Bool kMGPipeP5eClientWaitRuleLanded = true;

    // P5c (rv): the two serials the PipeInputs texture-shutter accessors answer with under a
    // server-stamped verb (FieldOwnership.def, APPLIER_DERIVED). Free functions rather than
    // member reads so PipeInputs.h needs this header's DECLARATIONS only... and because the
    // bump rule - advance, never zero - is stated once, beside the state.
    Uint64 MGPipeApplierTextureShutterSerial();
    Uint64 MGPipeApplierContextSerial();
    // The one writer-side helper: every applier entry point that can move what the frontend's
    // texture bind / sampling-resolution generations guard bumps the shutter serial through
    // this, so the bump rule lives in exactly one place.
    void MGPipeApplierNoteTextureStateMoved();

    // A make-current, not a teardown: runs on every change of the current context (the tracker
    // resets when the context moves), including a switch back to a live context. It clears the
    // working state a returning context may not inherit - render-state CSOs (the client drops
    // its cache too), vertex input, framebuffer bindings, unit sets, buffer binding windows,
    // program handles - and advances their serials rather than zeroing them. Object records
    // survive: they belong to the share group. So on a fresh context emitters re-send only
    // bound-handle state (MGPipeSetHashSuppressor::InvalidateAll); re-sending an object record
    // would move its Serial for nothing.
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
    // set_context_values (P5c rv, CONTRACT-P5C.md §5.3): the residual-value record. Writes
    // every field it carries into gPipeInputs through MGPipeApplyAccess - the server-owned
    // write that retires the eight value-class BARRIER_PULLED rows. Emitted only with an
    // active transport; under monolith there is no producer and the fields keep coming
    // through the residual fill (G1).
    void MGPipeApplySetContextValues(const MGPContextValues& values);
    // set_vertex_attrib_defaults: `tail` is hdr.Count MGPAttribValues for the attributes
    // named by hdr.Mask, in ascending location order.
    void MGPipeApplySetVertexAttribDefaults(const MGPVertexAttribDefaults& hdr, const MGPAttribValue* tail);

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
    // The bodies are in PipeApply.cpp. The signatures are what the client, the backend and the
    // gates compile against, and the records above are what they write into.

    // The scope of one resource_respecify, and it is an APPLIER-SIDE ARGUMENT and not a wire
    // record: it is not in PipeFields.def, it crosses no payload, and the transport reads the
    // scope off the call it is replaying rather than off a field. The two members mirror
    // MGPipeResourceRecord::PendingUpload's key exactly, which is the only thing the applier
    // does with them - so UploadTarget is MGPSubData::Target VERBATIM, the whole packed field
    // (ID-12: low byte = MGPipeResourceTarget, high byte = the cube-face upload target), the
    // same value the emission of that level put in the record. A per-face respecify therefore
    // drops the face it redefines and leaves the other five standing, and a caller that packs
    // the pair differently here than it packs it there simply matches nothing.
    // A NAMED LEVEL IS DROPPED EVEN WHEN EVERY STORAGE-DEFINING FIELD IS UNCHANGED (P4a final
    // review C-1): the pointer is the caller's statement that it reallocated that level, and
    // a non-base level's extent is not in the descriptor. Only a NULL level with unchanged
    // fields is the metadata update that drops nothing (ID-18 M4); the client's mask republish
    // is the one caller of that shape and passes null on purpose.
    struct MGPRespecifiedLevel {
        Uint16 UploadTarget = 0;
        Uint16 Level = 0;
        // P7 PH-4: mutable mip dimensions do not follow from the base descriptor. These fields
        // are carried by the split resource_respecify record; the pull helper remains 4 bytes.
        Uint32 Width = 0;
        Uint32 Height = 0;
        Uint32 Depth = 0;
    };

    inline MGPRespecifiedLevel MGPipeMakeRespecifiedLevel(Uint16 uploadTarget, Uint16 level, Uint32 width = 0,
                                                          Uint32 height = 0, Uint32 depth = 0) {
        MGPRespecifiedLevel value{};
        value.UploadTarget = uploadTarget;
        value.Level = level;
        value.Width = width;
        value.Height = height;
        value.Depth = depth;
        return value;
    }

    // THE THREE ACCEPTANCE RETURNS, AND WHY ALL THREE (ID-18 M3, clientfb review M3). D-D5
    // step 1 says the client clears a level's dirty flags "for the levels whose record the
    // applier ACCEPTED", and the emitter cannot answer that for itself: an `if constexpr` that
    // discarded the call, a dead or stale handle (a counted no-op) and a corrupt record (a Fatal
    // that deliberately moves no counter) are all invisible from the call site, so a client that
    // clears on the strength of having EMITTED drops those texels for good. resource_subdata
    // returns it, and so must the two calls that DEFINE the storage a subsequent upload lands
    // in - a create or a respecify the applier refused leaves no record for the upload to
    // accumulate onto, and B's own bookkeeping (its per-entry descriptor dedupe, its drain list)
    // must not advance past a call that never landed.
    //
    // ALL THREE ARE SOURCE-COMPATIBLE: a Bool return is ignorable, P3a's call sites in
    // MG_Impl/Pipe/PipeFill.cpp discard it, and gen_pipe.py never parses this header - the wire
    // path calls no MGPipeApply* at all (wire review W1), so PipeCalls.def and
    // MobileGL/MG_Pipe/generated do not move.

    // resource_create: mints the record and marks the slot Live. Emitted from the buffer
    // object's CONSTRUCTOR, so a resource exists before anything can name it; storage is
    // defined lazily by the first respecify and a backend tolerates a resource with none.
    //
    // Returns true when the record was minted. False for the three refusals: the reserved slot
    // 0, a descriptor whose target names no resource kind, and a slot at or above
    // kMGPipeMaxResourceSlots.
    Bool MGPipeApplyResourceCreate(const MGPResourceDesc& desc);
    // MGPipeApplyResourceRespecify (below) replaces the stored descriptor and bumps Serial;
    // `initialBytes` is the shadow when desc.HasDefinedContent, else null, and only an immutable
    // buffer store acks (MGPipeResourceRespecifyNeedsAck). A null `level` redefines the whole
    // resource and drops every pending upload; a named level drops only its own key. A null
    // level with unchanged storage-defining fields is a metadata update (how a BindMask or
    // ImageBindableHint change reaches an immutable texture): it drops nothing, moves Serial.
    // Returns false when refused. MGPipeApplyAttachSharedImage makes `texture`'s level-0 storage
    // shared image `imageId` (MGPipeResourceRecord::SharedImageId); false without a live record.
    Bool MGPipeApplyAttachSharedImage(MGPipeHandle texture, Uint64 imageId);

    Bool MGPipeApplyResourceRespecify(const MGPResourceDesc& desc, const void* initialBytes,
                                      const MGPRespecifiedLevel* level = nullptr);
    // MGPipeApplyResourceSubData (below): the buffer half takes its range from the record's box
    // (MGPipeSetSubDataBufferRange); the texture half adds `regions` (MGPSubRegion[RegionCount])
    // to the pending-upload set. record.Target packs the resource target (low byte) and the
    // cube-face upload target; a buffer is the whole field 0, and a non-texture target on the
    // texture half is Fatal{ProtocolCorruption}. Emitters clear dirty flags only on a true return.
    // MGPipeServerOriginatedWriteScope marks a server-produced write (a readback landing in a pack
    // buffer): its bytes live for the call only, so a store aliasing the frontend's shadow lands
    // them in the GL store and marks it GPU-written instead of adopting them. Thread-local.
    Bool MGPipeApplyingServerOriginatedWrite();
    class MGPipeServerOriginatedWriteScope {
    public:
        MGPipeServerOriginatedWriteScope();
        ~MGPipeServerOriginatedWriteScope();
        MGPipeServerOriginatedWriteScope(const MGPipeServerOriginatedWriteScope&) = delete;
        MGPipeServerOriginatedWriteScope& operator=(const MGPipeServerOriginatedWriteScope&) = delete;

    private:
        Bool m_previous;
    };

    Bool MGPipeApplyResourceSubData(const MGPSubData& record, const void* bytes, const MGPSubRegion* regions = nullptr);
    // buffer_subdata_resident: same shape; `bytes` is the application's staging store and is
    // valid for the duration of the call only. The op-table entry may be null.
    void MGPipeApplyBufferSubDataResident(const MGPSubData& record, const void* bytes);
    // resource_flush_range: record.AccessFlags are the application's REAL mapping flags, not
    // a normalized subset - the backend reads them per call to choose its upload shape.
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
    // P11 B2 (T0): the split server's map_persistent for a session that runs T0. The client's
    // AHardwareBuffer `ahb` (`size` bytes) becomes buffer `handle`'s resident store through the
    // backend's ImportExternal. A handle that is not a live buffer of that width, or a backend
    // without the member, declines (false); the codec answers DECLINED and the store runs T2.
    // MapPersistentRoundtrips is not moved: under split the server never counted map_persistent.
    Bool MGPipeApplyAdoptExternal(const MGPHandleOnly& handle, void* ahb, Uint64 size);
    // T0 in one process: buffer `handle`'s store, donated through the backend's DonateInProcess.
    // Null declines (not a live buffer, or a backend without the member); the store runs T2.
    void* MGPipeApplyDonateInProcess(const MGPHandleOnly& handle);

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
    // The object and working-state entry points
    // ---------------------------------------------------------------------------------
    // None calls into the backend: each stores an object record or working state, which the
    // backend reads at the sync points it already has (texture and renderbuffer storage,
    // framebuffer, unit sets and program are resolved at draw sync), keyed on server-owned
    // Serials. The resource entry points above branch on the target: a buffer reaches the buffer
    // members of MGPipeResourceOps, a texture only the optional Texture* members.

    // set_framebuffer_state. Fully resolved; ContentHash covers every field including Fbo and
    // DrawBuffers[8], so a suppressed record means the draw-buffer array, and with it the
    // fragColor broadcast count, did not move. The record is always written at
    // FramebufferRecords[state.Fbo.Slot], overwriting a recycled slot's predecessor; Draw, Read
    // and Both also set BoundFramebuffer, Named (DSA blits and clears) sets neither.
    // FramebufferSerial advances on every record. Fatal{ProtocolCorruption}, never counted: a
    // target above Named, a draw buffer outside Color[], a slot at or above
    // kMGPipeMaxFramebufferSlots, or the null handle (the default framebuffer is {0,1}).
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
    //
    // Returns true when the record took the parameters (P4a final review m-1, audit F-7): the
    // emitter's version latch advances on this answer and on nothing else, the way the
    // sub-data and respecify paths latch on theirs, so a refused record - no consumer, no
    // record for the handle, a null sampler - is re-sent at the next call rather than at the
    // next glTexParameter*. Source-compatible for the same reason the three resource returns
    // are: a Bool is ignorable and gen_pipe never parses this header.
    Bool MGPipeApplySetTextureParams(const MGPTextureParams& params);

    // set_sampler_views / bind_sampler_states / set_shader_images: `tail` is hdr.Count entries
    // starting at hdr.Start, and hdr.Start + hdr.Count above the unit bound is
    // Fatal{ProtocolCorruption}. Entries outside the declared window are NOT cleared.
    void MGPipeApplySetSamplerViews(const MGPSamplerViews& hdr, const MGPBoundView* tail);
    void MGPipeApplyBindSamplerStates(const MGPSamplerStates& hdr, const MGPipeHandle* tail);
    void MGPipeApplySetShaderImages(const MGPShaderImages& hdr, const MGPImageView* tail);

    // ---------------------------------------------------------------------------------
    // P5e's two entry points: DECLARED BY c0e, BODIED BY sb AND pg
    // ---------------------------------------------------------------------------------
    //
    // Both are declared here, with bodies in PipeApply.cpp that ABORT BY NAME, so that every
    // P5e package compiles and links against the same signatures from day one and the family
    // that lands one of them changes a body rather than adding a symbol. An abort and not a
    // silent no-op: a record that reached an unimplemented applier and returned quietly is a
    // dropped binding set, which renders wrong rather than stopping - the exact failure mode
    // ID-39's sixty-six lost uploads had. Nothing routes either opcode until its package lands
    // (PipeCatalogTest pins both table slots null), so the abort is unreachable today.

    // set_shader_buffers, per Class. `tail` is hdr.Count MGPBufferRanges starting at hdr.Start;
    // Class >= kMGPipeShaderBufferClassCount and Start + Count above the capacity are
    // Fatal{ProtocolCorruption, "SetShaderBuffers.Class"} / "SetShaderBuffers.Count". Entries
    // outside the declared window are NOT cleared - the record is "the last set as received",
    // exactly as set_vertex_buffers is.
    void MGPipeApplySetShaderBuffers(const MGPShaderBuffers& hdr, const MGPBufferRange* tail);

    // set_program_bindings. The three tails in the order the record declares them, plus the
    // RESOLVED block names for the override tail: the names ride SEG_STAGE as host spans and
    // SEG_STAGE retires with the record that named them, so the applier must copy them into the
    // record before this call returns (rule C) and the decoder hands over pointers that are
    // valid only for the duration of the call. `storageOverrideNames[i]` belongs to
    // `storageOverrides[i]`, NUL-terminated, exactly as set_storage_block_binding's single name
    // is. Any of the four pointers may be null when its count is 0.
    void MGPipeApplySetProgramBindings(const MGPProgramBindings& hdr, const Int32* blockBindings,
                                       const MGPProgramSamplerUnit* samplerUnits,
                                       const MGPProgramStorageOverride* storageOverrides,
                                       const char* const* storageOverrideNames);

    // create_shader_state. `link` and `spirv` are the program's artifacts and `archive` is the
    // snapshot the record adopts (a null clears the previous one); desc's blob refs are not
    // read. Under a transport the decoder passes its deserialized archive and pointers into it;
    // in one process PipeRoute copies the frontend's artifacts into a new archive without the
    // codec. The verify build round-trips the artifacts through the codec before storing, and a
    // mismatch is Fatal{PipeVerifyDiffer, "program-archive"}. `archive` has no default so that a
    // caller holding one cannot silently omit it.
    void MGPipeApplyCreateShaderState(const MGPProgramDesc& desc, const MG_State::GLState::LinkArtifacts* link,
                                      const MG_State::GLState::SpirvArtifacts* spirv,
                                      SharedPtr<const MG_State::GLState::ProgramArchive> archive);
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
    // ROADMAP.md's P4a row ends "emulations are an explicit Fatal under split until P8". In monolith the
    // code paths keep running exactly as today - the Fatal is a SPLIT-only arm - so this costs
    // P4a a named call site per unmigrated emulation and nothing else. P5/P8 give it teeth: a
    // split server that reaches one of these has no client address space to read and must
    // abort loudly rather than degrade silently.
    //
    // Monolith body: (void)name;. The list of names is pinned by
    // PipeCatalogTest.EveryUnmigratedEmulationIsNamedOnce and the call count is grepped by
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
