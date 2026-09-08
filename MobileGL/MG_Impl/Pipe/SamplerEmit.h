// MobileGL - MobileGL/MG_Impl/Pipe/SamplerEmit.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

// The CLIENT side of P4a's sampler family: the content-addressed sampler CSO cache, the
// identity-addressed sampler view per texture object, and the two unit sets
// set_sampler_views and bind_sampler_states. The third unit set, set_shader_images, is
// ImageEmit.h's - the same subsystem bit, a different resolution.
//
// TWO THINGS THIS FILE OWNS THAT ARE EASY TO GET WRONG, both stated where the body will go:
//   * SamplerParameters is 100 bytes with THREE BYTES OF TRAILING PADDING, so the CSO cache
//     hashes and memcmp-confirms over a ZERO-INITIALISED canonical copy built field by field,
//     never over the object's own bytes. Without that the 256-entry cache's hit rate is zero
//     and nobody notices, because the pixels are right.
//   * every emission goes through a VERSION-FIRST SKIP before it hashes anything: the sampler
//     view latches (params version, shape version) per handle, and the two sets latch their
//     SetHashSuppressor slots. A 192-entry walk per verb without a latch is not affordable.
//
// THIS FILE IS CREATED BY THE CONTRACT COMMIT AND FILLED BY THE PACKAGE THAT OWNS IT - see
// FramebufferEmit.h for why, in full. kMGPipeWiredSamplerSubsystem below covers this file AND
// ImageEmit.h: the three unit sets, the sampler CSO and the sampler view are ONE family and
// one subsystem bit, because an operator switching samplers off has to get the whole family's
// legacy arm rather than two thirds of it.
//
// HEADER-ONLY, for the ownership reason Tracker.h and ResourceTracker.h both state.
#if MOBILEGL_PIPE_PUSH
#include <MG_Pipe/MGPipe.h>
#include <MG_Pipe/PipeApply.h>
#include <MG_State/GLState/Core.h>

namespace MobileGL::MG_Pipe {

    // 0 until the emitters below and in ImageEmit.h have bodies; see FramebufferEmit.h's note.
    inline constexpr Uint64 kMGPipeWiredSamplerSubsystem = 0;

    // STUB AT THE CONTRACT COMMIT: emits nothing, returns 0 payload bytes.
    class MGPipeSamplerEmitter {
    public:
        using GLContext = MG_State::GLState::GLContext;

        // set_sampler_views: the PROGRAM-RESOLVED set only, one entry per unit, no stage
        // dimension. Start is 0 and Count is GetMaxTouchedTextureUnit() + 1 clamped to the
        // wire bound - the high-water mark is directly the count argument and is not
        // re-derived.
        Uint64 EmitSamplerViews(GLContext& ctx) {
            (void)ctx;
            return 0;
        }

        // bind_sampler_states: the unit's sampler CSO, or the null handle when the unit has no
        // sampler object - the texture's built-in sampler then applies, exactly as today.
        Uint64 EmitSamplerStates(GLContext& ctx) {
            (void)ctx;
            return 0;
        }

        void Reset() {}
    };

    inline MGPipeSamplerEmitter& MGPipeSamplerEmitterInstance() {
        // NEVER DESTROYED, for MGPipeTrackerInstance()' reason - and this one is named in the
        // phase's own risk list: a new client singleton that held a frontend SharedPtr, or
        // that had a destructor an exit handler could run into a torn-down pipe, is the
        // exit-order UAF P3a closed. Heap-constructed and intentionally leaked at exit.
        static MGPipeSamplerEmitter* emitter = new MGPipeSamplerEmitter();
        return *emitter;
    }
} // namespace MobileGL::MG_Pipe
#endif // MOBILEGL_PIPE_PUSH
