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
        // FramebufferState: any FBO attachment or default-geometry write, or a bind.
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
    void MGPipeEmitResourceDestroyAndFree(MG_State::GLState::BufferObject& buffer);

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
