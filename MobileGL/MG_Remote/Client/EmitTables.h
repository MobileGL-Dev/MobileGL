// MobileGL - MobileGL/MG_Remote/Client/EmitTables.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The client's emit table for the remote role: GlobalBackendFunctionsTable (the 69
// GLFunctionsTable slots plus Present and SetSwapInterval), installed by MG_Backend::Init as
// gBackendFunctionsTable. No slot may be null or fall through to a local driver: each emits a
// verb record, answers from the caps mirror (GetIntegeri_v, reached by every context's first
// glCompileShader, and IsTimerQuerySupported), or forwards to the server (SetSwapInterval).
// UnmigratedVerbFatal names a call shape an emitter cannot represent. Sites that probed a slot
// for null read a cap bit instead (SlotCaps.h). Resource, CSO, texture, sampler and program
// records bypass this table and go through the routed tables (MG_Pipe/PipeRoute.h).

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
    using MG_Record::DroppedClearEmissions;
    using MG_Record::DroppedDrawEmissions;
    using MG_Record::ForEachReadbackBand;
    using MG_Record::InstallMonolithVerbPort;
    using MG_Record::MonolithVerbPortInstalled;
    using MG_Record::PlanDrawIndirect;
    using MG_Record::PlanDrawInfo;
    using MG_Record::PlanDrawRange;
    using MG_Record::PlanReadbackBands;
    using MG_Record::ReadbackBand;
    using MG_Record::ReadbackBandPlan;
    using MG_Record::ReadbackPackStateIsTightForTest;
    using MG_Record::ReadbackReplyIsComplete;
    using MG_Record::RemoteDrawBindings;
    using MG_Record::RemoteIndexSizeFor;
    using MG_Record::ScatterReadbackBandIntoPackState;
    using MG_Record::ScatterTightReadbackIntoPackState;
    using MG_Record::SetDropClearEmissionForNegativeControl;
    using MG_Record::SetDropDrawEmissionForNegativeControl;
    using MG_Record::TightReadbackByteCount;
    using MG_Record::UnmigratedVerbFatal;

    // How many of the 73 slots have a real emitter. Reported at bring-up and asserted by the
    // gate: a table that silently loses an emitter should not be able to look the same as one
    // that never had it.
    Uint32 ImplementedVerbCount();

    // The total the count above is out of. Asserted against the struct in EmitTables.cpp, so
    // a slot added to GLFunctionsTable without a decision here is a build break.
    inline constexpr Uint32 kRemoteEmitSlotCount = 73;

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
