// MobileGL - MobileGL/MG_Backend/DirectGLES/ContextEpoch.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// MobileGL - DirectGLES context-owned static state identities (P5f fs).
#pragma once

#if MOBILEGL_BUILD_DISAGGREGATED
#include <Config.h>
#include <MG_Pipe/PipeApply.h>

namespace MobileGL::MG_Backend::DirectGLES {
    extern Uint g_backendContextGeneration;

    struct ContextEpoch {
        Uint Native = 0;
        Uint64 Served = 0;
        Bool Server = false;
        Bool operator==(const ContextEpoch&) const = default;
    };

    inline ContextEpoch CurrentContextEpoch() {
        const Bool server = MG_Config::Transport != MG_Config::TransportMode::Monolith;
        return {g_backendContextGeneration, server ? MG_Pipe::MGPipeApplierContextSerial() : 0, server};
    }
}
#endif
