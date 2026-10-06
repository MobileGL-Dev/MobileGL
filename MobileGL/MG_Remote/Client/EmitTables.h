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
//      them. (That was P5's partition. P5b..P10 moved every one of them to B; the last,
//      SetSwapInterval, is a forward to the server's control channel since P10, and class C is
//      empty. UnmigratedVerbFatal below remains the named refusal of a class-B emitter's
//      unrepresentable SHAPE - "<slot>+<QUALIFIER>".)
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
#include <MG_Impl/Pipe/Verb/VerbPort.h>

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

    // P13 W5: the verb emitters, the monolith verb port, the E2 controls and the readback / draw
    // planners are MG_Record's (MG_Impl/Pipe/Verb/VerbPort.h) - the record arm runs them in a
    // library without a transport. Re-exported here under the names the remote table's users
    // and its unit cases already spell.
    using MG_Record::UnmigratedVerbFatal;
    using MG_Record::InstallMonolithVerbPort;
    using MG_Record::MonolithVerbPortInstalled;
    using MG_Record::SetDropClearEmissionForNegativeControl;
    using MG_Record::DroppedClearEmissions;
    using MG_Record::SetDropDrawEmissionForNegativeControl;
    using MG_Record::DroppedDrawEmissions;
    using MG_Record::TightReadbackByteCount;
    using MG_Record::ScatterTightReadbackIntoPackState;
    using MG_Record::ReadbackBand;
    using MG_Record::ReadbackBandPlan;
    using MG_Record::PlanReadbackBands;
    using MG_Record::ForEachReadbackBand;
    using MG_Record::ScatterReadbackBandIntoPackState;
    using MG_Record::ReadbackPackStateIsTightForTest;
    using MG_Record::ReadbackReplyIsComplete;
    using MG_Record::RemoteDrawBindings;
    using MG_Record::RemoteIndexSizeFor;
    using MG_Record::PlanDrawInfo;
    using MG_Record::PlanDrawRange;
    using MG_Record::PlanDrawIndirect;

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

    // ---- P10 (CONTRACT-P10.md §1): fence polls answered from the reverse channel ----------
    //
    // The event drain's half: the server reported `fence` signaled (kEventFenceSignaled). Every
    // later glClientWaitSync / glGetSynciv(GL_SYNC_STATUS) on it answers without a round trip.
    // glFinish's wait for the GPU (a server fence created, waited and destroyed).
    void EmitFinishWait();
    void NoteFenceSignaledByServer(MG_Pipe::MGPipeHandle fence);

    // What the fence polls cost, for the lanes. `LocalAnswers` = polls answered with no record
    // (signaled or not); `Escalations` = zero-timeout polls that had gone unanswered
    // MOBILEGL_IPC_POLL_ESCALATE times in a row and took a round trip; `RoundTrips` = every
    // fence answer that crossed (escalations, non-zero timeouts, POLL_ESCALATE=0);
    // `ServerReports` = kEventFenceSignaled records consumed.
    struct FencePollCounters {
        Uint64 LocalAnswers = 0;
        Uint64 Escalations = 0;
        Uint64 RoundTrips = 0;
        Uint64 ServerReports = 0;
    };
    FencePollCounters ReadFencePollCounters();

} // namespace MobileGL::MG_Remote::Client
