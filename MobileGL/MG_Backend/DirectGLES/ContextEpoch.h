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
