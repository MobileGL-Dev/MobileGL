// MobileGL - MobileGL/MG_Backend/BackendObjects.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include "BackendObject.h"
#include "DirectGLES/BackendObject_DirectGLES.h"
#include "DirectVulkan/BackendObject_DirectVulkan.h"

namespace MobileGL::MG_Backend {
#if MOBILEGL_BUILD_DISAGGREGATED
    // P5 v1. The counterpart of Init()'s single split hook, and a NO-OP in every run that is
    // not split. It exists ONLY in a split build: G1 admits no new symbol in the pull build,
    // so MobileGL/Init.cpp's call sits under the same #if rather than calling a no-op.
    //
    // IT MUST RUN FIRST, BEFORE ANYTHING ELSE IN DestroyImpl. ARCHITECTURE.md §14's order is
    // publish -> the server drains and acks -> stop the apply thread (Kill, then join) -> close
    // the transport -> the client drains the compile pool -> MobileGL::Destroy(); DestroyImpl IS
    // MobileGL::Destroy, so everything before that last arrow has to happen at its top. Two
    // consequences are load-bearing rather than tidy:
    //
    //   * the apply thread is JOINED before PipeStats::Shutdown() dumps, so nothing is writing
    //     a counter while the final line is produced;
    //   * the apply thread is joined before pActiveBackendObject.reset(), so the server's own
    //     BackendObject - which is NOT that global (table 3) - is destroyed on the thread that
    //     owns the context, by ServerLoop::Stop, rather than on the app thread.
    //
    // The sync/query registries stay where they are: DestroyImpl drains them with
    // DestroyAllSyncObjects and DestroyAllQueryObjects before pActiveBackendObject.reset().
    void ShutdownSplitRoles();

    // F1 (P7 wave 2). Init()'s SPLIT ARM, run BEFORE MG_State::Init() rather than after it.
    //
    // Returns true when this process took the split path - whether the bring-up succeeded or
    // was refused by name - and the caller must then NOT call Init() a second time. Returns
    // false under MG_Config::TransportMode::Monolith, where there is nothing to bring up and
    // Init() keeps its own place after MG_State::Init().
    //
    // WHY IT EXISTS AT ALL, rather than MobileGL::Initialize simply calling Init() earlier:
    // the monolith arm of Init() must keep running after MG_State::Init(), because
    // BackendObject_DirectGLES::Initialize() is the tree's own order for it and moving it is a
    // pull-build change G1 forbids. So the split arm - and only the split arm - moves.
    //
    // It exists ONLY in a split build, for the reason InitServerRoleForSpawn states: a
    // definition guarded only on the inside still emits a symbol, and G1 measured that.
    Bool InitSplitRolesBeforeState();
#endif
} // namespace MobileGL::MG_Backend
