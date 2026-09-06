// MobileGL - MobileGL/MG_State/GLState/StateObjectDeathNotice.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

#if MOBILEGL_PIPE_PUSH
#include <MG_Pipe/MGPipeHandles.h>

// P2 step e2, the frontend half: TELL the backend that a state object died, instead of
// leaving it to discover the death in a garbage sweep.
//
// Today the only kind that announces its own death is Buffer, through BufferBackendOps
// (BufferState/BufferObject.h) - a frontend-declared ops table that the backend fills in at
// context bring-up. This is the same shape for the other six kinds, with two differences that
// follow from what the notice is for:
//
//   * it carries {kind, lifetimeId} and NOT the object, because by the time the last
//     SharedPtr has dropped there is no object left to pass, and the lifetime id is exactly
//     the key the client slot allocator resolves a handle from (ARCHITECTURE.md 4.2);
//   * it is one entry point for every kind rather than one ops table per kind, because the
//     backend's answer is the same for all six: free the slot, drop the twin.
//
// It exists only under MOBILEGL_PIPE_PUSH. A pull build has no slot allocator, no handle and
// nothing that could consume the notice, and G1 requires its symbol set to be byte-for-byte
// the pre-P2 one - so in that build this header declares nothing at all and the call sites
// compile to nothing.
//
// The pointer is written once, at backend bring-up, and read from state-object destructors.
// It is deliberately a plain pointer and not an atomic: the destructors and the bring-up run
// on the context thread, exactly as BufferBackendOps' g_bufferBackendOps does.
namespace MobileGL::MG_State::GLState {

    struct StateObjectDeathOps {
        // The last SharedPtr to the frontend object with this lifetime id has dropped.
        // Called from the object's destructor, so the object must NOT be touched.
        void (*OnDestroyed)(MG_Pipe::MGPipeKind kind, Uint64 lifetimeId) = nullptr;
    };

    inline const StateObjectDeathOps* g_stateObjectDeathOps = nullptr;

    inline void SetStateObjectDeathOps(const StateObjectDeathOps* ops) {
        g_stateObjectDeathOps = ops;
    }

    inline const StateObjectDeathOps* GetStateObjectDeathOps() {
        return g_stateObjectDeathOps;
    }

    inline void NotifyStateObjectDestroyed(MG_Pipe::MGPipeKind kind, Uint64 lifetimeId) {
        const StateObjectDeathOps* ops = g_stateObjectDeathOps;
        if (ops == nullptr || ops->OnDestroyed == nullptr) return;
        ops->OnDestroyed(kind, lifetimeId);
    }

} // namespace MobileGL::MG_State::GLState
#endif // MOBILEGL_PIPE_PUSH
