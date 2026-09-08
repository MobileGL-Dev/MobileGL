// MobileGL - MobileGL/MG_Impl/Pipe/ImageEmit.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

// The CLIENT side of set_shader_images, the third of P4a's kVarTail unit sets. It rides
// SamplerEmit.h's subsystem bit (kMGPipeWiredSamplerSubsystem): one family, one A/B.
//
// TWO INVARIANTS THAT MUST SURVIVE INTO THE BODY, and they are the kind an optimisation
// deletes:
//   1. THE HIGH-WATER-ZERO EARLY-OUT. An image high-water mark of 0 emits nothing, BEFORE any
//      hash - that is what makes every Minecraft draw pay one integer test for a feature it
//      does not use.
//   2. THE SWEEP'S GATE IS KEYED ON FRONTEND GENERATIONS AND DELIBERATELY NOT ON A BACKEND
//      RE-MINT COUNTER. A texture bound ONLY to an image unit is re-minted INSIDE the sweep,
//      so a server-side epoch would be bumped after the gate had already declined. The
//      client's bit-14 shutter is Mix(Mix(textureContent, textureParams), programImageUnitVersion)
//      - all three FRONTEND counters - so the property is preserved by construction, and it is
//      written here because it is invisible from the shutter itself.
//
// The record carries the APPLICATION's format and access; the bind-format recast (a GL_RG32F
// bind is INVALID_VALUE on 19 of 26 non-core formats on Adreno) and the buffer-texture split
// view stay SERVER-side and unchanged. ContentHash therefore has to cover InternalFormat and
// Access as well as the binding, because the format the shader was built against is live
// glBindImageTexture state and the format-less image bake keys on it.
//
// THIS FILE IS CREATED BY THE CONTRACT COMMIT AND FILLED BY THE PACKAGE THAT OWNS IT - see
// FramebufferEmit.h for why, in full.
#if MOBILEGL_PIPE_PUSH
#include <MG_Pipe/MGPipe.h>
#include <MG_Pipe/PipeApply.h>
#include <MG_State/GLState/Core.h>

namespace MobileGL::MG_Pipe {

    // STUB AT THE CONTRACT COMMIT: emits nothing, returns 0 payload bytes.
    class MGPipeImageEmitter {
    public:
        using GLContext = MG_State::GLState::GLContext;

        Uint64 EmitShaderImages(GLContext& ctx) {
            (void)ctx;
            return 0;
        }

        void Reset() {}
    };

    inline MGPipeImageEmitter& MGPipeImageEmitterInstance() {
        // NEVER DESTROYED, for MGPipeTrackerInstance()' reason; heap-constructed and
        // intentionally leaked at exit, like every other MGPipe process singleton.
        static MGPipeImageEmitter* emitter = new MGPipeImageEmitter();
        return *emitter;
    }
} // namespace MobileGL::MG_Pipe
#endif // MOBILEGL_PIPE_PUSH
