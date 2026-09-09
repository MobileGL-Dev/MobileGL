// MobileGL - MobileGL/MG_Pipe/PipeMutation.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#ifndef MOBILEGL_MG_PIPE_MUTATION_H // belt and braces: reachable as <MG_Pipe/..> and <..>
#define MOBILEGL_MG_PIPE_MUTATION_H
// Push-on-mutation (P1 lane finding F2). MGP_FILL copies a verb's may-read set out of the
// live GLContext at the verb boundary; the backend then reads that copy for the whole verb.
// A backend that WRITES a frontend object inside its own verb - Magma synthesising a
// fallback texture for an unbound sampler, materialising a queued clear, or overriding a
// sampler's filter - moves a value the boundary already copied, and every read after that
// point sees a block that no longer equals the live context. That is a real divergence, not
// a harness artefact: the pull build reads the moved value and the push build does not.
//
// The frontend mutator that moves such a value spells MGP_NOTE_MUTATION(Field) right where
// it moves it. The notice refreshes that ONE field in the pushed block when the field
// belongs to the verb currently in flight, so "the pushed block equals the live context at
// every read" stays literally true and the push build keeps pull semantics. It refreshes
// the value only and never the poison stamp, so a withheld stamp (MOBILEGL_PIPE_POISON_OMIT,
// negative control B) stays withheld.
//
// In the pull build the macro is ((void)0) and this header includes nothing, so the pull
// build is byte-identical to a tree without it.
#if MOBILEGL_PIPE_PUSH
#include <MG_Pipe/MGPipe.h>
namespace MobileGL::MG_Pipe {
    // MG_Impl/Pipe/PipeFill.cpp (the client side, the only place that may spell pGLContext).
    // A no-op unless a context is live, a verb has been filled, and `field` is in that verb
    // class's may-read mask; a forwarded (sticky) field has no storage and is never copied.
    void MGPipeNoteFrontendMutation(MGPipeInputField field);

    // ---- the aggregate generations (P2 brief D4, ARCHITECTURE.md 5.2) ----
    //
    // MGP_NOTE_MUTATION answers "a backend moved a frontend value INSIDE its own verb".
    // MGP_NOTE_AGGREGATE answers a different question, which is why it is a second macro
    // and not an overload: "did ANY object of this class move since the last time the
    // tracker looked", collapsed onto one monotonic Uint64 per class so a per-verb dirty
    // walk is a handful of compares rather than a scan over 32 attributes, 16 attachments,
    // 32 texture units and 84 binding points.
    //
    // The counters are members of the owning MG_State container, all guarded by
    // MOBILEGL_PIPE_PUSH so the pull build's state objects do not change size (G1). The
    // bump points sit on OBJECTS, which have no back-pointer to their state, so the macro
    // goes through a free function that finds the live GLContext - the same shape, and for
    // the same reason, as MGP_NOTE_MUTATION (MG_Impl/Pipe/PipeFill.cpp). It costs a global
    // load on a path that has just written object state.
    //
    // Monotonic and never reset: the tracker widens and compares, it never subtracts.
    // Over-firing is free (one extra re-push); under-firing renders stale, which is why
    // every counter here is deliberately COARSER than the state it guards.
    enum class MGPipeAggregate : Uint32 {
        // VertexArrayState: any VAO attribute format / buffer / enable moved.
        VaoAttribute = 0,
        // FramebufferState: any FBO attachment or default-geometry write, or a bind - and, since
        // P4a (fable seam F-3), any STORAGE DEFINITION of a texture or a renderbuffer, because
        // set_framebuffer_state inlines an attachment's format, extent and samples and those
        // setters are the only writers of what it inlines (TextureObject.cpp /
        // RenderbufferObject.cpp, PipePublishDescriptor).
        FramebufferAttachment,
        // TextureState: any texture object CONTENT moved (an upload, a dirty region).
        TextureContent,
        // TextureState: any texture object or sampler object PARAMETER moved.
        TextureParams,
        // BufferState: any buffer object contents moved.
        BufferChange,
        // GLContext: a glVertexAttrib* default value moved. Not one of D4 five: the bit it
        // shutters (NEW_VERTEX_ATTRIB_DEFAULTS) is specified there as a ContentHash over
        // all 32 CurrentVertexAttributeValues, and hashing 768 bytes on EVERY draw does not
        // fit inside the T1 ceiling. The hash still decides whether to EMIT (D11 set-hash
        // suppressor); this decides whether to hash at all.
        VertexAttribDefault,
        Count,
    };

    // MG_Impl/Pipe/PipeFill.cpp. A no-op unless a context is live.
    void MGPipeNoteAggregate(MGPipeAggregate aggregate);

    // ---- P3a: the resource family's emission points (brief D-A1) ----
    //
    // The seven BufferBackendOps hooks already dispatch at the GL call that causes them
    // (ARCHITECTURE.md 5.1 names them as the ONE exception to push-at-validate), so their
    // pipe calls are emitted from the same BufferObject dispatchers rather than from the
    // validate point. That puts the emission inside MG_State, which is why these are
    // DECLARED here beside the two notices and DEFINED in MG_Impl/Pipe/PipeFill.cpp: this
    // header is the one MG_State already includes for exactly this, and the closure gate
    // (check_include_closure.py's mutation-header probe) keeps it a declaration - reaching
    // MG_Impl/Pipe/ResourceTracker.h from BufferObject.cpp would pull the client's tracker
    // into the state machine that calls it.
    //
    // The forward declaration is the whole coupling: none of these needs the definition of
    // BufferObject, and this header must not gain it.
} // namespace MobileGL::MG_Pipe

namespace MobileGL::MG_State::GLState {
    class BufferObject;
    // P4a's five, for the BIRTH half at the tail of this header. Declarations only, exactly as
    // BufferObject is: none of the hooks below needs a definition, and this header must not
    // gain one - reaching a frontend class header from here would put the state machine's own
    // types in front of every mutator that spells MGP_NOTE_MUTATION.
    class ITextureObject;
    class RenderbufferObject;
    class FramebufferObject;
    class SamplerObject;
    class ProgramObject;
}

namespace MobileGL::MG_Pipe {
    // (Features.PipePush & kMGPipeSubsystemResources) != 0 && MGPipeGetResourceOps() != nullptr.
    //
    // BOTH HALVES MATTER. The bit is the operator's per-subsystem A/B; the table is "has a
    // backend taken this family over at all". Until one has, every dispatch below falls
    // through to the BufferBackendOps table it replaces and the tree behaves exactly as it
    // did - which is what lets the client half land on its own.
    Bool MGPipeResourceSubsystemEnabled();
    // The nullable member, asked the way the frontend asks g_bufferBackendOps->ResidentSubData
    // today: one backend deliberately does not implement it and the caller has a different
    // path when it is absent (BufferObject::FillSubData).
    Bool MGPipeResourceOpsHaveSubDataResident();

    // Minted from the constructor and released from the destructor, both unconditionally in
    // a push build: a handle is CLIENT state and set_vertex_buffers names it whether or not
    // the resource family is switched on. The CALLS are what the predicate above gates.
    void MGPipeMintResourceHandle(MG_State::GLState::BufferObject& buffer);
    // In this order, and it is not negotiable (D-L): the destroy resolves the handle, and
    // MGPipeSlotAllocator::Free erases the lifetimeId -> slot mapping it resolves through.
    //
    // RETURNS whether resource_destroy was emitted, which is the LATCH taken at this buffer's
    // create and not a second reading of MGPipeResourceSubsystemEnabled(). The destructor
    // needs that answer to decide whether the legacy OnDestroy still owes a call: asking the
    // predicate twice pairs a create emitted under one registration with a destroy gated on
    // another, and either direction leaks - a live applier record on a slot about to be
    // re-handed-out, or a backend object nobody releases.
    Bool MGPipeEmitResourceDestroyAndFree(MG_State::GLState::BufferObject& buffer);

    // THE VERTEX-ELEMENTS CSO's DEATH, and it is BACKEND-NEUTRAL - which is the whole point.
    // Before this, the only thing that ever returned a VertexElementsCso slot was DirectGLES'
    // StateObjectDeathOps table; under any backend that installs none - DirectVulkan/Magma,
    // which keeps its own age-reclaimed identity table on purpose - every VAO ever created
    // held its slot and its ~1.3 KB applier record for the life of the process, on the shipped
    // 0x1ff mask, and past 65536 slots every create_vertex_elements became a permanent
    // Fatal{ProtocolCorruption}. The client mints the slot, so the client is where the death
    // has to be spoken from.
    //
    // Takes the lifetime id and not the object for StateObjectDeathNotice.h's reason: the last
    // SharedPtr has already dropped by the time this runs, and the lifetime id is what the
    // slot allocator resolves the handle from. Returns whether delete_vertex_elements went
    // out, i.e. whether the applier actually held a record - see the definition for why that
    // is asked rather than assumed.
    Bool MGPipeEmitVertexElementsDestroyAndFree(Uint64 lifetimeId);

    // ---- P4a: ONE CLIENT-SIDE DEATH HELPER PER KIND P4a MINTS (brief D-I1) ----
    //
    // BACKEND-NEUTRAL FROM DAY ONE, and this is the P3a final-review lesson taken forward
    // rather than repeated. Before it, the only thing that ever returned a VertexElementsCso
    // slot was DirectGLES' StateObjectDeathOps table; under a backend that installs none -
    // DirectVulkan/Magma, which keeps its own age-reclaimed identity table on purpose - every
    // VAO ever created held its slot and its applier record for the life of the process, and
    // past 65536 slots every create became a permanent Fatal{ProtocolCorruption}. P4a mints
    // SIX kinds, so the rule is stated once and obeyed six times: whatever mints a handle owns
    // the death of that handle, the client mints all six, and a backend death notice is a
    // redundant SECOND path that must be idempotent - which it is, because it resolves through
    // the same lifetimeId -> slot map these free, and MGPipeSlotAllocator::Free refuses a slot
    // that is not live at that generation.
    //
    // THE ORDER INSIDE EACH IS FIXED AND IS NOT A PACKAGE'S CHOICE:
    //   1. emit the wire delete FIRST - it drops the applier's record while the record still
    //      exists, so a recycled slot cannot inherit a field;
    //   2. raise NotifyStateObjectDestroyed SECOND - it resolves the handle through the
    //      allocator, and a backend told after the Free could no longer find its twin, which
    //      moves the leak from the client to the driver object;
    //   3. free the slot LAST, and a double free on a stale generation is a proven no-op
    //      because Free bumps no generation (the bump rides the next handout).
    //
    // ALL SIX TAKE THE LIFETIME ID rather than the object, for MGPipeEmitVertexElementsDestroy
    // AndFree's reason: they run from a destructor, where the last SharedPtr has already
    // dropped, and the lifetime id is what the slot allocator resolves the handle from. It is
    // also what keeps this header a declaration-only coupling - no frontend class needs
    // forward-declaring for any of them.
    //
    // Each returns whether its wire delete actually went out, which is the LATCH taken at the
    // object's create and not a second reading of the subsystem predicate: an object born
    // while a subsystem bit was clear and destroyed after it was set would otherwise free its
    // slot with the applier's record still Live, on a slot about to be handed out again. The
    // legacy path runs only when the answer is false.

    // ResourceDestroy, and then the SamplerViewCso minted off this same lifetime id (P4a
    // D-F2: one sampler view per ITextureObject). Called from TextureObjectBase's VIRTUAL
    // destructor, so 2D / 3D / cube / buffer / view all announce exactly once.
    Bool MGPipeEmitTextureDestroyAndFree(Uint64 lifetimeId);
    // ResourceDestroy.
    Bool MGPipeEmitRenderbufferDestroyAndFree(Uint64 lifetimeId);
    // NO WIRE CALL AT ALL (D-I2). PipeCalls.def has no framebuffer delete, because a
    // framebuffer is not a resource and is not a CSO - it is STATE, and set_framebuffer_state
    // is the only call that names one - and the catalogue is closed, so P4a does not invent a
    // row. The handle is minted and freed entirely client-side and this helper does steps 2
    // and 3 only. A recycled framebuffer handle is distinguished by Gen, which is inside the
    // record's ContentHash, so it can never be suppressed against its predecessor's record.
    Bool MGPipeEmitFramebufferDestroyAndFree(Uint64 lifetimeId);
    // DeleteSamplerState. Also the path the content-addressed CSO cache's LRU eviction takes,
    // which is why it is addressed by lifetime id and not by "the object that owns it".
    Bool MGPipeEmitSamplerCsoDestroyAndFree(Uint64 lifetimeId);
    // DeleteSamplerView. Called by the texture helper above; a sampler view has no frontend
    // object of its own, so this is the only path there is.
    Bool MGPipeEmitSamplerViewCsoDestroyAndFree(Uint64 lifetimeId);
    // DeleteShaderState, for an ordinary program AND for a program-pipeline COMPOSITE, whose
    // slot has two independent release paths - the pipeline cache's LRU eviction and the
    // composite ProgramObject's own destructor. One helper for both, and the second call is a
    // proven no-op.
    Bool MGPipeEmitShaderCsoDestroyAndFree(Uint64 lifetimeId);

    void MGPipeEmitResourceCreate(MG_State::GLState::BufferObject& buffer);
    void MGPipeEmitResourceRespecify(MG_State::GLState::BufferObject& buffer);
    void MGPipeEmitResourceSubData(MG_State::GLState::BufferObject& buffer, SizeT offset, SizeT size);
    void MGPipeEmitBufferSubDataResident(MG_State::GLState::BufferObject& buffer, SizeT offset,
                                         const void* bytes, SizeT size);
    void MGPipeEmitResourceFlushRange(MG_State::GLState::BufferObject& buffer, SizeT offset, SizeT size,
                                      Uint32 accessFlags);
    void MGPipeEmitResourceReadback(MG_State::GLState::BufferObject& buffer);
    // Returns the coherent host pointer the resource owner donated, or null for a DECLINE -
    // which is a real answer. Every call, mint or decline, is one map-persistent roundtrip.
    void* MGPipeEmitMapPersistent(MG_State::GLState::BufferObject& buffer);

    // ================================================================================
    // P4a: THE BIRTH HALF, one hook per client path MG_State owns (D-C .. D-I)
    // ================================================================================
    //
    // The death helpers above are half a lifetime. The other half is emitted from MG_State
    // too - a texture's create from its constructor, a renderbuffer's respecify from its
    // storage mutators, a texture's params from glTexParameter*, a sampler CSO from the
    // sampler object, a shader CSO from the program - because that is where the event
    // happens, exactly as P3a's buffer family emits from BufferObject's own dispatchers
    // (ARCHITECTURE.md 5.1 names those as the ONE exception to push-at-validate). Only the
    // texture sub-data DRAIN runs at the validate point, and even it is fed from here: the
    // drain list is appended on a level's first dirty mark.
    //
    // WHY THEY ARE DECLARED HERE. This header is the one door MG_State has into the client
    // (check_include_closure.py's mutation-header probe pins it: reaching
    // MG_Impl/Pipe/*Emit.h from a frontend mutator would pull the client's emitters into the
    // state machine that calls them). So a hook a frontend mutator calls is DECLARED here and
    // DEFINED in MG_Impl/Pipe/PipeFill.cpp, which is package A's for the whole phase - the
    // same "declaration here, definition there" split MGPipeMintResourceHandle and
    // MGPipeEmitResourceCreate use, and the reason no file is touched twice.
    //
    // WHAT EACH BODY DOES, and the division is fixed:
    //   * PipeFill.cpp owns the GATE - the subsystem bit in MOBILEGL_PIPE_PUSH *and* the
    //     family's own kMGPipeWired*Subsystem constant, the same pair the validate point's
    //     `wants()` applies to every emission - and the four MINTS, which are pure allocator
    //     work and need no family knowledge;
    //   * the FAMILY EMITTER (MG_Impl/Pipe/<Family>Emit.h, owned by package B or C) owns the
    //     payload build, the handle rule for its own kind and the PUBLICATION LATCH below.
    //     PipeFill.cpp forwards to it through an entry point that is compiled only while that
    //     family's wired constant is non-zero, so this tree links against the STUB emitters
    //     and against the finished ones with no edit to PipeFill.cpp - and a family that sets
    //     its constant without providing the entry point is a COMPILE ERROR in its own commit
    //     rather than a surprise at the merge. The entry point each hook forwards to is named
    //     beside it and spelled out in PipeFill.cpp's contract block.
    //
    // NOTHING CALLS ANY OF THEM AT THE CONTRACT COMMIT. B and C add the call sites in the
    // five MG_State directories C.7 gives them, in the SAME commit that gives the emitter its
    // body - by EDITING an existing constructor/mutator body, never by adding one (G1).

    // ---- the publication latch (D-I1), and it is the ONE answer both halves read ----
    //
    // The create is gated at its call site and the destroy inside the death helper, so the
    // two ask the same question at two different moments. An object born while its subsystem
    // bit was clear and destroyed after it was set would otherwise free its slot with the
    // applier's record still Live - on a slot the allocator is about to hand out again. A
    // slot is NOT evidence of a record either: a backend twin table mints one through
    // MGPipeSlots().Acquire whether or not the subsystem ever asked this client to emit a
    // create, and a delete_* on such a handle is a refused call the applier asserts on.
    //
    // So the emitter latches the answer when its create actually goes out, the death helper
    // reads the latch, and the latch is keyed by {kind, slot, gen} so a recycled slot cannot
    // inherit its predecessor's answer. Defined in PipeFill.cpp beside the six death helpers,
    // declared here because both the helpers and the five emit headers read it.
    void MGPipeNoteHandlePublished(MGPipeKind kind, MGPipeHandle handle);
    Bool MGPipeHandleIsPublished(MGPipeKind kind, MGPipeHandle handle);
    void MGPipeNoteHandleUnpublished(MGPipeKind kind, MGPipeHandle handle);

    // ---- the four mints (pure allocator work, no family knowledge) ----
    //
    // UNCONDITIONAL in a push build, for MGPipeMintResourceHandle's reason: a handle is CLIENT
    // state and other subsystems name these objects by handle whether or not their own family
    // is switched on - MGPSurface::Res names a Texture or a Renderbuffer out of the framebuffer
    // subsystem, MGPBoundView::Texture and MGPImageView::Res name a Texture out of the sampler
    // one. Gating the mint on the family bit would make those emit null handles in exactly the
    // A/B arm that exists to isolate the families. Each costs one free-list pop and one map
    // insert per object and emits nothing.
    void MGPipeMintTextureHandle(MG_State::GLState::ITextureObject& texture);
    void MGPipeMintRenderbufferHandle(MG_State::GLState::RenderbufferObject& renderbuffer);
    // A framebuffer has a handle and NO wire lifetime (D-I2): set_framebuffer_state is the only
    // call that names one, and there is no create or destroy for the kind. The mint is still
    // the object's, so the identity exists before the first validate point that pushes it.
    void MGPipeMintFramebufferHandle(MG_State::GLState::FramebufferObject& framebuffer);
    // Ordinary programs only. A program-pipeline COMPOSITE is minted by the composite resolver
    // out of the reserved band through MGPipeSlotAllocator::AllocateComposite, which is the one
    // door into it, and it is not a frontend construction event.
    void MGPipeMintShaderCsoHandle(MG_State::GLState::ProgramObject& program);

    // ---- textures and renderbuffers: MG_Impl/Pipe/TextureEmit.h, package B ----
    //
    // resource_create from ITextureObject's constructor and RenderbufferObject's;
    // resource_respecify from every storage-defining entry point, including
    // RenderbufferObject::{SetInternalFormat, AllocateStorage, SetSamples}, which publish
    // nothing at all today (D-D2); set_texture_params from the parameter mutators, which is
    // where the READ-attachment-only gap D-E3 closes.
    //
    // Entry points MGPipeTextureEmitter must provide, all taking the frontend object by
    // reference and returning void:
    //   EmitResourceCreate(ITextureObject&)
    //   EmitResourceRespecify(ITextureObject&, MGPipeTextureRespecifyScope, Uint32 uploadTarget,
    //                         Uint32 level)
    //   EmitTextureParams(ITextureObject&)
    //   NoteLevelDirty(ITextureObject& storageOwner, Uint32 uploadTarget, Uint32 level)
    //   EmitRenderbufferCreate(RenderbufferObject&) / EmitRenderbufferRespecify(RenderbufferObject&)
    void MGPipeEmitTextureResourceCreate(MG_State::GLState::ITextureObject& texture);

    // WHICH STORAGE A TEXTURE RESPECIFY REPLACES (P4a final review C-1). The applier scopes
    // its pending-upload clear on this answer and not on the descriptor, because the
    // descriptor cannot give it: AllocateStorage is per (uploadTarget, level) and
    // TruncateMipmapLevels removes every level at or above a cut, while MGPResourceDesc
    // carries only the base extent and the level count. A level the applier had ACCEPTED at
    // one verb (the client's dirty flag already clear, D-D5 step 1) and that a later per-level
    // definition redefined AROUND was dropped by the whole-resource arm with nobody owing its
    // texels - so every respecify states its scope, and "whole resource" is said, never
    // defaulted. The emitter builds wire's MGPRespecifiedLevel from the pair, packed exactly
    // as the drain packs a sub-data record's Target (MGPipePackSubDataTarget), so the key it
    // drops is the key that level's emission made.
    enum class MGPipeTextureRespecifyScope : Uint32 {
        // The whole store is redefined or restated: a format, sample-count or
        // fixed-sample-locations change, an immutable allocation completing
        // (SetImmutableLevels), a texture view's creation. Every pending upload goes.
        WholeResource = 0,
        // ONE (uploadTarget, level) was (re)allocated: glTexImage*D, glCompressedTexImage*D,
        // glCopyTexImage*D, one level of a glTexStorage* loop, one level of a generated-mipmap
        // grow. That level's pending upload goes; every other level's stays. `uploadTarget` and
        // `level` name it.
        OneLevel = 1,
        // The chain was cut: every level of `uploadTarget` at or above `level` is gone and the
        // levels below it are untouched (glGenerateMipmap fitting the chain, a base-level
        // redefinition discarding its tail, glTexStorage* fitting the chain to its level
        // count). `level` is the first level removed; a cut at 0 is the whole resource.
        LevelsFrom = 2,
    };
    void MGPipeEmitTextureResourceRespecify(MG_State::GLState::ITextureObject& texture,
                                            MGPipeTextureRespecifyScope scope, Uint32 uploadTarget,
                                            Uint32 level);
    void MGPipeEmitTextureParams(MG_State::GLState::ITextureObject& texture);
    // The DRAIN LIST's append, on a level's FIRST dirty mark, keyed on the STORAGE OWNER from
    // day one (D-D4: a view and its owner already share one dirty state, so an upload through
    // either lands on the same key). The record itself is emitted at the validate point by
    // MGPipeTextureEmitter::DrainTextureSubData; this is only what puts the level on the list,
    // and walking every live texture per verb is the cost it exists to avoid.
    void MGPipeNoteTextureLevelDirty(MG_State::GLState::ITextureObject& storageOwner, Uint32 uploadTarget,
                                     Uint32 level);
    void MGPipeEmitRenderbufferResourceCreate(MG_State::GLState::RenderbufferObject& renderbuffer);
    void MGPipeEmitRenderbufferResourceRespecify(MG_State::GLState::RenderbufferObject& renderbuffer);

    // ---- D-A4's two sticky bind-mask producers (P4a final review M-A) ----
    //
    // kMGPipeBindSampler is "any texture the sampler-view resolution names in an emitted
    // MGPBoundView" and kMGPipeBindShaderImage "any texture named in an emitted MGPImageView"
    // - both the SAMPLER package's emitters (SamplerEmit.h, ImageEmit.h), which the texture
    // emitter's header includes and which therefore cannot include it back - and, earliest of
    // all, glBindImageTexture's state setter (TextureState.h, MG_State), which may include no
    // emit header at all. So the note goes through this door, exactly as the birth hooks do.
    // Nothing produced either bit before the fix round: ImageBindableHint was always 0, the
    // metadata respecify (ID-18 M4) had no live trigger, and the remint pull the hint exists to
    // prevent was neither prevented nor counted.
    //
    // UNCONDITIONAL IN A PUSH BUILD, like the mints: the mask is CLIENT state the framebuffer
    // emitter ORs into whether or not the texture family is on, and the emission a mask move
    // causes (the metadata respecify) is gated inside the emitter on the family's own pair.
    void MGPipeNoteTextureBoundAs(MGPipeHandle texture, Uint32 bindBit);
    // glBindImageTexture. The hint is the PREVENTION half of the texture-remint stall class -
    // a texture the server knows may be image-bound is allocated image-bindable up front - so it
    // has to reach the applier before the texture's first sync, i.e. at the bind itself, not at
    // the validate point's image walk (which notes it as well, D-A4's letter).
    void MGPipeNoteTextureImageBound(MG_State::GLState::ITextureObject& texture);


    // ---- sampler CSOs and sampler views: MG_Impl/Pipe/SamplerEmit.h, package C ----
    //
    // Entry points MGPipeSamplerEmitter must provide, returning void:
    //   EmitSamplerCso(SamplerObject&)     - D-F1's content-addressed mint-or-share at
    //                                        capacity 256, hashed field-wise over a canonical
    //                                        zero-initialised copy, behind the version-first
    //                                        skip. The HANDLE RULE FOR THIS KIND IS THE
    //                                        EMITTER'S, not this file's: two identical
    //                                        samplers share one CSO, so there is deliberately
    //                                        no per-object mint above, and it is the emitter
    //                                        that decides which lifetime id (if any) owns the
    //                                        slot the death helper will resolve.
    //   EmitSamplerView(ITextureObject&)   - D-F2's ONE view per texture object, minted off
    //                                        the texture's own lifetime id and re-issued on
    //                                        the SAME handle when the restrictions move.
    void MGPipeEmitSamplerCsoCreate(MG_State::GLState::SamplerObject& sampler);
    void MGPipeEmitSamplerViewCreate(MG_State::GLState::ITextureObject& texture);

    // ---- programs: MG_Impl/Pipe/ProgramEmit.h, package C ----
    //
    // Entry point MGPipeProgramEmitter must provide, returning void:
    //   EmitShaderCso(ProgramObject&)
    //
    // Re-issued on the SAME handle whenever the link version moves, exactly as
    // create_vertex_elements is (Gen moves only on slot reuse). D-H4 keeps the TRACKER out of
    // it - bit 6's shutter reads GetCurrentProgram() and deliberately not GetProgramForDraw(),
    // because the tracker must not force a compile to answer "did the shader move" - so the
    // ordinary emission is the validate point's, from the join the verb was going to make
    // anyway. This hook exists for the paths that are NOT a draw: a link that completes off
    // the draw path still owns its own publication.
    void MGPipeEmitShaderCsoCreate(MG_State::GLState::ProgramObject& program);
} // namespace MobileGL::MG_Pipe
#define MGP_NOTE_MUTATION(Field)                                                                                       \
    ::MobileGL::MG_Pipe::MGPipeNoteFrontendMutation(::MobileGL::MG_Pipe::MGPipeInputField::Field)
#define MGP_NOTE_AGGREGATE(Aggregate)                                                                                  \
    ::MobileGL::MG_Pipe::MGPipeNoteAggregate(::MobileGL::MG_Pipe::MGPipeAggregate::Aggregate)
#else
#define MGP_NOTE_MUTATION(Field) ((void)0)
#define MGP_NOTE_AGGREGATE(Aggregate) ((void)0)
#endif
#endif
