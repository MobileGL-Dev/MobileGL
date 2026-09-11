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
// NOTE the asymmetry this table does not resolve: the resource, CSO, framebuffer, texture,
// sampler and program families do NOT come through here. They are emitted from
// MG_Impl/Pipe/* by direct MGPipeApply* calls (37 entry points, 41 call sites), and under
// split each of those becomes an encode. This table covers only the verbs - the draws,
// clears, blits, readbacks, queries, fences and present.

#pragma once
#include <Includes.h>

#include <MG_Backend/BackendObject.h>

namespace MobileGL::MG_Remote::Client {

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

} // namespace MobileGL::MG_Remote::Client
