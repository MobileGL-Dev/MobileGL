// MobileGL - MobileGL/MG_Remote/Client/EmitTables.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The client's emitting function table. Owner: package c1. Signatures by c0.
//
// MG_Backend/Init.cpp:44 assigns gBackendFunctionsTable from the active backend object, and
// 91 MG_Impl/GLImpl sites call through it directly. So a BackendObject_Remote has to return a
// COMPLETE table, and "complete" is a bigger number than R-4's headline:
//
//   GLFunctionsTable                (BackendObject.h:117-292) = 69 function pointers
//                                                             + Bool PrefersCpuXfbPrimitiveAccounting
//   GlobalBackendFunctionsTable     (BackendObject.h:293-299) = the above, + Present, + SetSwapInterval
//                                                             = 71 function pointers in total
//
// R-4's rule, restated over all 71: NO SLOT MAY BE NULL, and no slot may fall through to a
// driver. A null slot is 91 potential null calls; a pass-through slot is a split lane quietly
// running monolith and going green, which is the one outcome every gate in this phase exists
// to prevent. A verb P5 does not implement gets a slot that raises
// Fatal{UnmigratedVerb, "<slot>"} - the same shape as MGPipeInputPoisonFatal, live at every
// log level, MGLOG_F + std::abort.
//
// WHICH SLOTS GET A REAL EMITTER IS DECIDED BY THE VERB CENSUS (R-4), not guessed here:
// ~/w7/notes/p5/verb-census.md. CONTRACT-P5.md §7 carries the resulting THREE-CLASS SPLIT and
// it is not to be re-derived:
//
//   A. ANSWERED LOCALLY from the caps mirror, never emitted and never Fatal (R-15) - two
//      slots, GetIntegeri_v and IsTimerQuerySupported, plus the Bool member
//      PrefersCpuXfbPrimitiveAccounting, which is not a slot. GetIntegeri_v is the one that
//      would otherwise sink the phase: it is reached by the FIRST glCompileShader of every
//      context (CompileEnv.cpp:134-138 <- Core.cpp:39), not by any verb, so a Fatal there
//      aborts every scenario before it draws anything.
//   B. EMITTED in P5 - five slots: Clear, DrawArrays, ReadPixels, BlitFramebuffer, Present.
//      Present has ZERO MG_Impl call sites: it is reached through EGLImpl.cpp:178 ->
//      BackendObject.cpp:396, so mirroring GLImpl will not find it.
//   C. Fatal{UnmigratedVerb} - the remaining 64, SetSwapInterval and GetGpuTimestampNs among
//      them.
//
// AND THE RULE R-4 WOULD OTHERWISE BREAK. 41 of the 69 slots are null-checked at their call
// site, and several of those checks are CAPABILITY PROBES, not safety checks - BeginOcclusionQuery
// (GL_Query.cpp:481, :785), BeginXfbPrimitivesQuery (:534), SubDataResident. With no null slot
// in this table every one of them answers "supported" and the fallback behind it silently
// disappears. A null check on a slot may not survive into the client: it becomes a caps-mirror
// read, which is what ARCHITECTURE.md:114 means by "CallMask replaces 'is this table slot null'".
//
// NOTE the asymmetry this table does not resolve, AND WHERE IT IS RESOLVED (R-17): the
// resource, CSO, framebuffer, texture, sampler and program families do NOT come through here.
// They were emitted from MG_Impl/Pipe/* by 40 direct calls to the 37 MGPipeApply* entry
// points; those call sites now go through the two generated tables
// (MG_Pipe/PipeRoute.h -> MG_Remote/Client/WireTables.cpp). This table covers only the verbs -
// the draws, clears, blits, readbacks, queries, fences and present.

#pragma once
#include <Includes.h>

#include <MG_Backend/BackendObject.h>
// P5b: the clear discriminants below alias MGPipeTypes.h's (CONTRACT-P5B.md f1). The header
// was already in this file's closure through BackendObject.h's neighbours; naming it makes
// the dependency the aliases have explicit.
#include <MG_Pipe/MGPipeTypes.h>
#include <MG_Pipe/MGPipeValueTypes.h>

namespace MobileGL::MG_Remote::Client {

    // MGPClear::Kind and ::ValueClass. P5b MOVED THE NUMBERS INTO MGPipeTypes.h
    // (kMGPipeClearKind* / kMGPipeClearValueClass*, CONTRACT-P5B.md f1) - the single spelling
    // this header and the server's PipeApplier.h each said they were waiting for. These are
    // aliases so c1's EmitClear reads unchanged; f1's four ClearBuffer* emitters and the DSA
    // form name the MG_Pipe constants directly.
    inline constexpr Uint32 kRemoteClearWhole = MG_Pipe::kMGPipeClearKindWhole;
    inline constexpr Uint32 kRemoteClearColor = MG_Pipe::kMGPipeClearKindColor;
    inline constexpr Uint32 kRemoteClearDepth = MG_Pipe::kMGPipeClearKindDepth;
    inline constexpr Uint32 kRemoteClearStencil = MG_Pipe::kMGPipeClearKindStencil;
    inline constexpr Uint32 kRemoteClearDepthStencil = MG_Pipe::kMGPipeClearKindDepthStencil;

    // The table MG_Backend::Init() installs into gBackendFunctionsTable for the remote role.
    // A reference to a never-destroyed block, like every other MG_Remote singleton (ID-8).
    const MG_Backend::GlobalBackendFunctionsTable& RemoteEmitTable();

    // Called by the Fatal slots. Named separately so a death test can filter on it and so
    // that the message wording lives in exactly one place.
    [[noreturn]] void UnmigratedVerbFatal(const char* slot);

    // How many of the 71 slots have a real emitter. Reported at bring-up and asserted by the
    // gate: a table that silently loses an emitter should not be able to look the same as one
    // that never had it.
    Uint32 ImplementedVerbCount();

    // The total the count above is out of. Asserted against the struct in EmitTables.cpp, so
    // a slot added to GLFunctionsTable without a decision here is a build break.
    inline constexpr Uint32 kRemoteEmitSlotCount = 71;

    // The other two thirds of the census, so a case can assert the WHOLE partition rather than
    // only the half that emits. CONTRACT-P5.md §7's three classes are 2 + 5 + 64, and
    // EmitTables.cpp static_asserts that they sum to kRemoteEmitSlotCount: a slot that quietly
    // changes class shows up as a build break in the sum, not as a silent behaviour change.
    Uint32 LocallyAnsweredSlotCount(); // class A - answered from the caps mirror, R-15
    Uint32 UnmigratedSlotCount();      // class C - Fatal{UnmigratedVerb}

    // THE E2 NEGATIVE CONTROLS (t1's debt against c1, BRIEF §7). When set, the named emitter
    // SKIPS its record - it still runs the pre-verb hooks and still returns - so a replay that
    // is really going through the wire loses that verb while a replay that fell through to the
    // driver is unaffected. They are functions rather than knobs in Config.h for two reasons:
    // a control has to be settable from a test process that has already started, and a knob
    // would be a MOBILEGL_IPC_-shaped name for something no operator may ever set.
    //
    // Emissions actually skipped, so a control can assert that it DID something rather than
    // that a picture changed - a control that silently never fired is the third shape of R-16's
    // "a gate that cannot go red for its own reason".
    //
    // WHICH ONE E2'S RETRACE USES, and it is not the clear. Measured on the joint head: with
    // MOBILEGL_IPC_E2_DROP_CLEAR=1 armed and its WARN in the library's own log, the OpenRA
    // retrace under inproc still scored ssim=1.000000 / mismatchPixels=0 (joint-v1.md §3),
    // because OpenRA covers every pixel it clears before the snapshot. Dropping the DRAWS is
    // the control whose observable the golden is actually made of. See EmitTables.cpp's
    // ArmControlKnobs for the trace census that settled it.
    void SetDropClearEmissionForNegativeControl(Bool drop);
    Uint64 DroppedClearEmissions();
    void SetDropDrawEmissionForNegativeControl(Bool drop);
    Uint64 DroppedDrawEmissions();

    // ID-47. The CLIENT refuses a readback whose answer would not fit a reply slot, BEFORE it
    // emits the record, and names the read. THE REFUSAL ITSELF IS s1's -
    // ClientSession::RequireReadPixelsReplyFits, forwarding to
    // ReplySlotPool::RequireReadPixelsFits - and this package does not own a second copy of the
    // message: two spellings of one refusal is how the two sides come to disagree about which
    // reads are legal. What c1 owns is the CALL SITE and the number it passes, which is ID-49's
    // tight extent; the control below is over that, and s1-v3.md §1's cases are over the
    // helper.

    // ID-49. `MGPReadbackInfo::DstSize` is the TIGHT w*h*bytesPerPixel extent - the reply
    // payload - and nothing about the application's pack state crosses the wire. The server
    // reads with a NEUTRAL pack state into that run; this is the number both sides derive.
    Uint64 TightReadbackByteCount(GLsizei width, GLsizei height, GLenum format, GLenum type);

    // ID-49. Scatters the tight rows into the application's pointer per the application's own
    // pack state (ROW_LENGTH, SKIP_*, ALIGNMENT), which only the client holds. Exposed for the
    // same reason as the refusal above: the control drives the function the emitter calls
    // rather than a second copy of GL 4.6 8.4.4's arithmetic. THE GAPS ARE NEVER WRITTEN -
    // they belong to the application - and that is what the control checks with a sentinel.
    void ScatterTightReadbackIntoPackState(const void* tight, void* destination, GLsizei width,
                                           GLsizei height, Uint64 bytesPerPixel,
                                           const PixelStoreParameters& pack);

    // ID-49. True when the destination layout IS the tight layout, which is the only condition
    // under which EmitReadPixels may read the reply straight into the application pointer and
    // skip the bounce. Exported because the FAST PATH and the SCATTER have to agree, and the
    // only honest way to state that is to drive both and compare - a case that tested either
    // alone would pass a predicate that said yes to a layout the scatter would have rearranged.
    Bool ReadbackPackStateIsTightForTest(GLsizei width, Uint64 bytesPerPixel,
                                         const PixelStoreParameters& pack);

    // M2 / codex 11. True exactly when the readback reply is OK and carries the read's own exact
    // extent (CONTRACT-P5 row 23). EmitReadPixels calls this and Fatals by name when it is false
    // - a short OK reply, or a DECLINE/ERROR with a zero payload, is refused rather than scattered
    // as pixels. Exposed so the control drives the production predicate (R-16), not a copy: pass
    // 0=OK / 1=DECLINED / 2=ERROR as `status`.
    Bool ReadbackReplyIsComplete(Int32 status, Uint64 replySize, Uint64 expected);

    // =============================================================================
    // P5b d1 - the draw family's record plan (MG_Remote/CONTRACT-P5B.md §2 d1)
    // =============================================================================
    //
    // The nineteen indexed / instanced / multi-draw / indirect entry points all ride draw_vbo
    // (59), and the ONLY thing that differs per entry point is how the GL arguments become the
    // record's fields. That derivation is split out of the emitters as three pure functions
    // over a snapshot of the bindings the emitter read from the GL context, so a unit case can
    // drive the PRODUCTION derivation with synthetic bindings (R-16) while the integration lane
    // proves the bindings are read from the right slots. The emitters compose these and add
    // nothing but the hooks, the E2 draw-drop control and the staging of a client index array.

    // What the emitter reads from the GL context before it plans a draw.
    struct RemoteDrawBindings {
        // The VAO's GL_ELEMENT_ARRAY_BUFFER: bound or not, and its handle when bound (the same
        // handle set_index_buffer carried at validate). Not bound means `indices` is a client
        // pointer and the emitter stages the bytes (kDrawHasUserIndices).
        Bool ElementBufferBound = false;
        MG_Pipe::MGPipeHandle ElementBuffer = MG_Pipe::kMGPipeNullHandle;
        // The bound GL_DRAW_INDIRECT_BUFFER and GL_PARAMETER_BUFFER, null when unbound.
        MG_Pipe::MGPipeHandle DrawIndirectBuffer = MG_Pipe::kMGPipeNullHandle;
        MG_Pipe::MGPipeHandle ParameterBuffer = MG_Pipe::kMGPipeNullHandle;
        // GL_PRIMITIVE_RESTART / GL_PRIMITIVE_RESTART_FIXED_INDEX and the application's index,
        // carried verbatim (informational in P5b: the backend reads its own barrier-pulled copy).
        Bool PrimitiveRestart = false;
        Uint32 RestartIndex = 0;
        // P5e (vi), ID-82 / CONTRACT-P5E §5.1: TRUE when the bound VAO has at least one ENABLED
        // attribute with no buffer object behind it, i.e. an array whose vertices live in the
        // application's own memory. The server has no such memory: today it dereferences
        // `attrib.Offset` as a raw client pointer from the apply thread
        // (Managers.cpp's SyncClientSideAttributesForDrawArrays), which is legal only while the
        // client is parked behind the record. It rides in MGPDrawInfo::Flags as kDrawClientArrays
        // so BOTH roles can decide from the wire - the client refuses such a draw under
        // run-ahead, and MGPipeBarriered's escalation (ii) keeps it barriered if one ever
        // arrives anyway. Staging the bytes is P8's.
        Bool ClientVertexArrays = false;
    };

    // 1 / 2 / 4 for the three GL index types, 0 for anything else (the frontend has already
    // refused those with INVALID_ENUM before the slot is reached).
    Uint8 RemoteIndexSizeFor(GLenum indexType);

    // The fixed head. `indexSize` 0 = arrays. `instanceCount` is the call's own (1 for a
    // non-instanced entry point) and `baseInstance` its gl_BaseInstance value; the sink reads
    // "instanced" as InstanceCount != 1 || StartInstance != 0, so an instanced call with a
    // count of 1 and no base instance is dispatched as the plain draw it is equivalent to.
    MG_Pipe::MGPDrawInfo PlanDrawInfo(GLenum mode, Uint8 indexSize, GLsizei instanceCount,
                                      GLuint baseInstance, Uint32 numDraws,
                                      const RemoteDrawBindings& bindings);

    // One MGPDrawRange from one (first | indices, count, basevertex). Arrays: Start = first.
    // Indexed with an element buffer bound: Start = offset / IndexSize, and FALSE when the byte
    // offset is not a whole number of indices or does not fit the record's Uint32 Start - the
    // caller refuses by name rather than round. Indexed with no element buffer: Start = 0 and
    // the bytes are the client's (the emitter stages `count * IndexSize` of them).
    Bool PlanDrawRange(const RemoteDrawBindings& bindings, Uint8 indexSize, const void* indicesOrFirst,
                       GLsizei count, GLint baseVertex, MG_Pipe::MGPDrawRange& out);

    // The kDrawIsIndirect second tail: the bound GL_DRAW_INDIRECT_BUFFER handle, the call's
    // `indirect` byte offset, the stride and the draw count; for the *IndirectCount forms the
    // bound GL_PARAMETER_BUFFER handle and the byte offset the call spells as `drawcount`.
    MG_Pipe::MGPDrawIndirect PlanDrawIndirect(const RemoteDrawBindings& bindings, const void* indirect,
                                              GLsizei drawCount, GLsizei stride,
                                              GLintptr parameterOffset, Bool hasParameterBuffer);

} // namespace MobileGL::MG_Remote::Client
