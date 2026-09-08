// MobileGL - MobileGL/MG_Pipe/MGPipe.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

#include "MGPipeCallbacks.h"
#include "MGPipeHandles.h"
#include "MGPipeHostSpan.h"
#include "MGPipeTypes.h"

// The MGPipe boundary (plan B section 4).
//
// The two interface tables are FUNCTION-POINTER STRUCTS, not virtual bases. Three reasons
// out of this repository rather than out of gallium: the boundary already is a
// function-pointer struct sitting on one hook point in MG_Backend/Init.cpp; a nullptr entry
// already means "not implemented, frontend falls back", which is exactly what a
// not-yet-migrated subsystem needs to say while it keeps pulling; and MG_Test already
// substitutes this table to mock a backend. The rare EGL and caps surface stays on
// pActiveBackendObject's virtual functions.
namespace MobileGL::MG_Pipe {
    // Unscoped on purpose: PipeCalls.def spells these as bare tokens so the same file can
    // be read by the C++ preprocessor and by scripts/gen_pipe.py.
    enum MGPipeCallClass : Uint8 {
        kScreen,
        kCtxCso,
        kCtxState,
        kCtxObject,
        kCtxVerb,
        kCtxQuery,
        kCallClassCount,
    };

    enum MGPipeCallFlags : Uint32 {
        kNone = 0,
        // The caller must not proceed until the server has acknowledged. Rare by design.
        kNeedsAck = 1u << 0,
        // Carries an MGPBlobRef.
        kHasBlob = 1u << 1,
        // Carries a variable-length array after the fixed payload.
        kVarTail = 1u << 2,
        // Carries an MGHostSpan - the one shape that changes with the transport.
        kHostSpan = 1u << 3,
        // Answers into an MGPReplySlot; never blocks.
        kReplySlot = 1u << 4,
        // May be null in a backend's table. A null entry is a real answer ("this backend
        // does not implement it"), not an error: DirectVulkan deliberately leaves
        // buffer_subdata_resident unregistered, and SetSwapInterval likewise.
        kOptional = 1u << 5,
    };

    // The pipeline/dynamic split of RenderStateParameters, defined exactly once (section
    // 4.5.2): MG_Pipe/MGPipeRenderStateSpans.{h,cpp}, which landed with P2 and computes
    // every chunk boundary with offsetof. Include that header to use it; what stays here
    // is the generated member list at the bottom of this file, which is what the chunk
    // table was derived from.

    // ---- MOBILEGL_PIPE_PUSH's runtime bitmask (Config.h Features.PipePush) ----
    //
    // One bit per SUBSYSTEM, so an A/B is per subsystem rather than all-or-nothing, and
    // bit 63 for the one BEHAVIOUR the design has to be measured against. Bits are
    // allocated in ROADMAP order and never reused: an operator's recorded 0x7f has to keep
    // meaning what it meant.
    //
    // A clear subsystem bit means "keep pulling", which after P2 is only a valid control
    // while MOBILEGL_PIPE_LEGACY_MEMOS compiles the pre-handle arm beside it.
    inline constexpr Uint64 kMGPipeSubsystemRenderState = 1ull << 0;
    inline constexpr Uint64 kMGPipeSubsystemPixelPack = 1ull << 1;
    inline constexpr Uint64 kMGPipeSubsystemPatchState = 1ull << 2;
    inline constexpr Uint64 kMGPipeSubsystemVertexAttribDefaults = 1ull << 3;
    inline constexpr Uint64 kMGPipeSubsystemResidualValues = 1ull << 4;
    inline constexpr Uint64 kMGPipeSubsystemEsprytSlots = 1ull << 5;      // Track H, Espryt 0b
    inline constexpr Uint64 kMGPipeSubsystemMagmaVertexInput = 1ull << 6; // Track H, Magma subsystem 4
    // P3a's two. Resources is the seven BufferBackendOps hooks turned into the handle-shaped
    // resource_* family; VertexInput is vertex elements, vertex buffers and the index buffer.
    // They are separate bits because they are separate A/Bs: a buffer path that regressed and
    // a vertex path that regressed are different findings, and clearing one must not disarm
    // the other.
    inline constexpr Uint64 kMGPipeSubsystemResources = 1ull << 7;
    inline constexpr Uint64 kMGPipeSubsystemVertexInput = 1ull << 8;
    // P4a's four. FOUR AND NOT ONE, for P3a's reason one level out: a framebuffer path that
    // regressed, a texture path that regressed, a sampler path that regressed and a program
    // path that regressed are four different findings, and clearing one must not disarm the
    // other three.
    //
    // THREE OF THEM HAVE A DEPENDENCY and it is diagnosed at the first use, never half-run -
    // one Resolve<Family>SubsystemArm per family beside the backend's existing
    // ResolveResourceSubsystemArm, modelled on the bit-8-requires-bit-7 refusal it already
    // ships, and lazy rather than at bring-up because a pre-flight child dying on a signal
    // makes a whole lane SKIP green: bit 11 requires bit 10 because
    // every MGPBoundView::Texture and MGPImageView::Res names a Texture handle and only bit 10
    // puts one in the slot table; bit 9 requires bit 10 because MGPSurface::Res does; and bit
    // 10 requires bit 7 because a buffer texture's BufferForTexBuffer names a Buffer handle.
    // The mirror pairs (10 without 11, 10 without 9, 7 without 10) are all fine, and are
    // stated as such because an unreachable branch that says something different is how the
    // reachable one drifts. Bit 12 depends on nothing.
    inline constexpr Uint64 kMGPipeSubsystemFramebuffer = 1ull << 9;       // set_framebuffer_state
    inline constexpr Uint64 kMGPipeSubsystemTextureResources = 1ull << 10; // texture + renderbuffer
                                                                          // resource_*, set_texture_params
    inline constexpr Uint64 kMGPipeSubsystemSamplers = 1ull << 11;         // sampler CSO, sampler view,
                                                                          // the three unit sets
    inline constexpr Uint64 kMGPipeSubsystemPrograms = 1ull << 12;         // shader CSO, draw/dispatch
                                                                          // program, global constants
    // bits 13..62 reserved for the later phases, allocated in ROADMAP order.
    // NOT a subsystem, a BEHAVIOUR: turn OFF client-side content addressing of CSOs, so
    // every pipeline-version change mints a fresh CSO and the map is never probed. This is
    // the negative control the whole CSO design is measured against (ROADMAP.md P2).
    inline constexpr Uint64 kMGPipeBehaviourNoCsoContentAddressing = 1ull << 63;
    // The default of a push build with the knob unset (ConfigLoader.cpp). Each phase's
    // constant STAYS, because it is the A/B control for the phase after it: P3a's
    // "everything P2 had and nothing of mine" arm is spelled MOBILEGL_PIPE_PUSH=0x7f.
    inline constexpr Uint64 kMGPipeSubsystemsMigratedAtP2 = 0x7full;   // bits 0..6
    inline constexpr Uint64 kMGPipeSubsystemsMigratedAtP3a = 0x1ffull; // bits 0..8
    // P4a's, and the two above are NOT edited: 0x1ff is P4a's T2 arm and its "everything P3a
    // had and nothing of mine" control, exactly as 0x7f was P3a's.
    inline constexpr Uint64 kMGPipeSubsystemsMigratedAtP4a = 0x1fffull; // bits 0..12
    static_assert(kMGPipeSubsystemsMigratedAtP4a ==
                      (kMGPipeSubsystemsMigratedAtP3a | kMGPipeSubsystemFramebuffer |
                       kMGPipeSubsystemTextureResources | kMGPipeSubsystemSamplers |
                       kMGPipeSubsystemPrograms),
                  "the P4a phase constant and P4a's four subsystem bits have drifted");

    // The catalogue itself. Only macros, so it is safe to expand inside the namespace, and
    // consumers (the unit test, later the transport) get MGP_CALL_LIST from this header.
#include "PipeCalls.def"

    // G1: the two interface tables. A null entry means "not implemented" (section 4.1).
#include "generated/PipeTables.inc"

    // The installed tables. Zero-initialized, so an un-installed MGPipe is every entry
    // null - which is precisely the pre-migration state.
    inline MGPipeScreen gMGPipeScreen{};
    inline MGPipeContext gMGPipeContext{};

    // G2: monolith thunks. These are what MG_Impl call sites move onto, replacing
    // gBackendFunctionsTable.GL.* one name at a time.
#include "generated/PipeThunks.inc"

    // G3: wire records, their size assertions, and the applier's bounds precondition.
#include "generated/PipeWire.inc"

    // G4: the MOBILEGL_PIPE_VERIFY field-wise comparators.
#include "generated/PipeVerify.inc"

    // G5: PipeInputs field ids and the per-verb poison generations.
#include "generated/PipeFilled.inc"

    // G5b: the verb enum (one per GLFunctionsTable entry), the verb classes and their
    // may-read field masks - what MGPipeFillForVerb fills and what a poison build lets a
    // verb read (FillPoints.def).
#include "generated/PipeFillPoints.inc"

    // G6: the backend read inventory's coverage table.
#include "generated/PipeCoverage.inc"

    // G7: the render-state pipeline subset, by member name.
#include "generated/PipeSpanTable.inc"
} // namespace MobileGL::MG_Pipe
