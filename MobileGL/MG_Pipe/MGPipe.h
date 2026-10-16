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
// out of this repository rather than out of other driver interfaces: the boundary already is a
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

    // The fifth column of PipeCalls.def: what the client does after publishing a record of this
    // call to a server that publishes kCapRunAheadApply (ARCHITECTURE.md §11.6). Unscoped so the
    // .def can use bare tokens for both the preprocessor and scripts/gen_pipe.py, which generates
    // MGPipeWaitClassFor(op). Both roles recover the class from the opcode, so
    // MGPipeBarriered(op, payload, applierState) is one function for the emit table and the sink.
    // MGPipeWaitClassFor answers kWaitClassCount for an unknown opcode; a caller holding stream
    // bytes must raise its own Fatal{ProtocolCorruption} rather than make a wait decision.
    enum MGPipeWaitClass : Uint8 {
        kWaitNone = 0,
        kWaitApplied,
        kWaitReply,
        kWaitPresent,
        kWaitClassCount,
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
    // One bit each for the framebuffer, texture-resource, sampler and program families. Bits
    // 0-13 are fixed on (ConfigLoader.cpp refuses a cleared one at startup); the dependencies
    // between bits are in SubsystemDeps.def.
    inline constexpr Uint64 kMGPipeSubsystemFramebuffer = 1ull << 9;       // set_framebuffer_state
    inline constexpr Uint64 kMGPipeSubsystemTextureResources = 1ull << 10; // texture + renderbuffer
                                                                          // resource_*, set_texture_params
    inline constexpr Uint64 kMGPipeSubsystemSamplers = 1ull << 11;         // sampler CSO, sampler view,
                                                                          // the three unit sets
    inline constexpr Uint64 kMGPipeSubsystemPrograms = 1ull << 12;         // shader CSO, draw/dispatch
                                                                          // program, global constants
    // P5e's one (docs/archive/contracts/CONTRACT-P5E.md §1). The INDEXED BUFFER BINDING POINTS: the three
    // dirty bits NewConstBuffers / NewShaderBuffers / NewSoTargets, the set_shader_buffers
    // emitter, and the backend's four binding-point walks. It is its own bit and not part of
    // bit 7's resource family for the reason every other split here has: a UBO/SSBO binding
    // path that regressed and a buffer path that regressed are different findings, and
    // clearing one must not disarm the other.
    //
    // Its dependency row is in SubsystemDeps.def with the other five (P3b/P4b R-5). It used to
    // be stated here instead, fifteen lines away from the block that stated the other three -
    // which is how "THREE OF THEM" above stayed wrong through two phases.
    inline constexpr Uint64 kMGPipeSubsystemBufferBindings = 1ull << 13;
    // bits 14..62 reserved for the later phases, allocated in ROADMAP order.
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
    // P5e's, and P4a's is NOT edited: 0x1fff stays P5e's "everything P4a had and nothing of
    // mine" control, exactly as 0x1ff was P4a's and 0x7f was P3a's. The push default becomes
    // 0x3fff, so the A/B that reproduces today's picture is one bit cleared.
    inline constexpr Uint64 kMGPipeSubsystemsMigratedAtP5e = 0x3fffull; // bits 0..13
    static_assert(kMGPipeSubsystemsMigratedAtP5e ==
                      (kMGPipeSubsystemsMigratedAtP4a | kMGPipeSubsystemBufferBindings),
                  "the P5e phase constant and P5e's subsystem bit have drifted");

    // ---- D-K2's dependency rule, in ONE place (P3b/P4b R-5) --------------------------------
    //
    // The rows, their reasons and the argument for a table rather than six comments are in
    // SubsystemDeps.def. This is the expansion both readers are meant to end up on; the
    // anti-drift property is already live through
    // MG_Test/Backend/DirectGLES/SubsystemDepsTest.cpp, which drives the client check and the
    // server gate at every interesting mask and compares both against these rows.
#include "SubsystemDeps.def"

    struct MGPipeSubsystemDependencyRow {
        Uint64 Family;   // exactly one kMGPipeSubsystem* bit
        Uint64 Requires; // the bits MOBILEGL_PIPE_PUSH must ALSO carry; 0 is a row, not a gap
        const char* Why;
    };

#define MGL_SUBSYSTEM_DEPENDENCY_ROW(family, requires_, why) {family, requires_, why},
    inline constexpr MGPipeSubsystemDependencyRow kMGPipeSubsystemDependencies[] = {
        MGL_SUBSYSTEM_DEPENDENCY_LIST(MGL_SUBSYSTEM_DEPENDENCY_ROW)};
#undef MGL_SUBSYSTEM_DEPENDENCY_ROW

    // The bits `subsystem` additionally needs, or 0 for a family with no row. A family with no
    // row and a family whose row says 0 are the same answer ON PURPOSE: both mean "nothing else
    // is required", and the table covers its families exhaustively so the two cannot be
    // confused for "I forgot to look".
    inline constexpr Uint64 MGPipeSubsystemRequires(Uint64 subsystem) {
        Uint64 required = 0;
        for (const MGPipeSubsystemDependencyRow& row : kMGPipeSubsystemDependencies) {
            if ((subsystem & row.Family) != 0) required |= row.Requires;
        }
        return required;
    }

    // NOT TRANSITIVE, and that is load-bearing: each reader tests the required BITS against
    // MOBILEGL_PIPE_PUSH alone and never the other family's liveness, because Espryt's
    // resolvers classify their arms from that mask alone (PipeFill.cpp's own argument;
    // TextureEmitTest.cpp pins it). A transitive closure here would make the two sides disagree
    // at some mask - the failure this table exists to prevent.
    inline constexpr Bool MGPipeSubsystemDependenciesAreSet(Uint64 subsystem, Uint64 pushMask) {
        const Uint64 required = MGPipeSubsystemRequires(subsystem);
        return (pushMask & required) == required;
    }

    // The ROW'S OWN REASON, so a reader that refuses prints the table's sentence instead of a
    // copy of it (P3b/P4b wave 2-D package D3). Empty for a family whose row requires nothing,
    // which is the same answer as "there is nothing to explain": a reader only asks after
    // MGPipeSubsystemDependenciesAreSet has already said no, and that cannot happen for a row
    // that requires 0.
    inline constexpr const char* MGPipeSubsystemDependencyWhy(Uint64 subsystem) {
        for (const MGPipeSubsystemDependencyRow& row : kMGPipeSubsystemDependencies) {
            if ((subsystem & row.Family) != 0 && row.Requires != 0) return row.Why;
        }
        return "";
    }

    // Rows 1 and 6 became live behaviour when D3 made every reader read the table (before that
    // the bit-7 tests were hard-coded in Managers.cpp and DirectGLES.cpp and these rows were
    // dead data). An edit that drops either row now makes BOTH readers agree, so nothing
    // downstream can catch it; these two lines can.
    static_assert(MGPipeSubsystemRequires(kMGPipeSubsystemVertexInput) == kMGPipeSubsystemResources,
                  "D-K2's vertex-input row (bit 8 requires bit 7) has gone missing");
    static_assert(MGPipeSubsystemRequires(kMGPipeSubsystemBufferBindings) == kMGPipeSubsystemResources,
                  "D-K2's buffer-bindings row (bit 13 requires bit 7) has gone missing");
    static_assert(MGPipeSubsystemRequires(kMGPipeSubsystemTextureResources) ==
                      (kMGPipeSubsystemResources | kMGPipeSubsystemSamplers),
                  "D-K2's fourth row (bit 10 requires bit 11) has gone missing again");
    static_assert(MGPipeSubsystemRequires(kMGPipeSubsystemPrograms) == 0,
                  "bit 12 depends on nothing, and that is a row rather than an absence");
    static_assert(!MGPipeSubsystemDependenciesAreSet(kMGPipeSubsystemTextureResources, 0x7ffull),
                  "0x7ff is the mask whose texture family Espryt refuses; the table must agree");
    static_assert(MGPipeSubsystemDependenciesAreSet(kMGPipeSubsystemTextureResources, 0x1fffull),
                  "0x1fff is P4a's own arm and every row of it must be satisfied");

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
