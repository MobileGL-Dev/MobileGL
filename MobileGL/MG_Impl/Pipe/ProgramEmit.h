// MobileGL - MobileGL/MG_Impl/Pipe/ProgramEmit.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

// The CLIENT side of P4a's program family: create/bind/delete_shader_state,
// set_draw_program, set_dispatch_program and set_global_constants.
//
// WHERE create_shader_state IS EMITTED FROM, and why it is not the tracker's business: the
// tracker's bit-6 shutter reads GetCurrentProgram() and DELIBERATELY NOT GetProgramForDraw(),
// because the tracker must not force a compile just to answer "did the shader move". So the
// tracker keeps its shutter and the EMITTER joins - from the same GetProgramForDraw() /
// GetProgramForDispatch() call the verb is about to make anyway, so no join happens that would
// not have happened. Emitting from the compile pool's terminal continuation is a real
// asynchronous win and is a LATER phase's: in monolith the applier is one function call away,
// so it is unmeasurable here.
//
// WHAT THE SERVER STILL SPECIALISES, so nobody reads create_shader_state as self-contained
// and produces a per-draw rebuild: the draw-FBO clamp masks, the fragColor broadcast count,
// the storage-block binding signature, the atomic-counter set, the live image formats and the
// patch parameters are all inputs a backend program depends on BEYOND the artefacts. This call
// publishes the ARTEFACTS; the server specialises at the verb from the state it holds. The
// clause count does not shrink - its inputs move.
//
// THE ARTEFACTS DO NOT TRAVEL IN MONOLITH. All seven of MGPProgramDesc's blob refs are
// declared with Size 0 and the LinkArtifacts / SpirvArtifacts ride beside the record through
// MGPipeApplyCreateShaderState's companion pointers, so the codec is never called on the hot
// path; the verify build is where it is exercised.
//
// THIS FILE IS CREATED BY THE CONTRACT COMMIT AND FILLED BY THE PACKAGE THAT OWNS IT - see
// FramebufferEmit.h for why, in full.
#if MOBILEGL_PIPE_PUSH
#include <MG_Pipe/MGPipe.h>
#include <MG_Pipe/PipeApply.h>
#include <MG_State/GLState/Core.h>

namespace MobileGL::MG_Pipe {

    // 0 until the emitters below have bodies; see FramebufferEmit.h's note.
    inline constexpr Uint64 kMGPipeWiredProgramSubsystem = 0;

    // STUB AT THE CONTRACT COMMIT: emits nothing, returns 0 payload bytes.
    class MGPipeProgramEmitter {
    public:
        using GLContext = MG_State::GLState::GLContext;

        // create_shader_state (re-issued on the SAME handle whenever the link version moves -
        // Gen moves only on slot reuse), then bind_shader_state and set_draw_program /
        // set_dispatch_program. Two program calls because the frontend has two joins and two
        // PipeInputs slots.
        Uint64 EmitShaderState(GLContext& ctx) {
            (void)ctx;
            return 0;
        }

        // set_global_constants: the DEFAULT UNIFORM BLOCK only, keyed (ShaderCso, Version) and
        // at most once per program per frame. Version is GetUBOContentVersion() and must never
        // be ~0u, which is the backends' "never uploaded" sentinel - the wrap skips it.
        Uint64 EmitGlobalConstants(GLContext& ctx) {
            (void)ctx;
            return 0;
        }

        void Reset() {}
    };

    inline MGPipeProgramEmitter& MGPipeProgramEmitterInstance() {
        // NEVER DESTROYED, for MGPipeTrackerInstance()' reason; heap-constructed and
        // intentionally leaked at exit, and it MUST NOT hold a frontend SharedPtr - that is
        // the exit-order rule, stated over every MGPipe process singleton rather than over the
        // ones a destructor reaches today.
        static MGPipeProgramEmitter* emitter = new MGPipeProgramEmitter();
        return *emitter;
    }
} // namespace MobileGL::MG_Pipe
#endif // MOBILEGL_PIPE_PUSH
