// MobileGL - MobileGL/MG_Impl/Pipe/FramebufferEmit.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

// The CLIENT side of P4a's framebuffer family: set_framebuffer_state, emitted at the validate
// point once per bound TARGET that moved, or once with Target = Both when the two bindings
// name the same object.
//
// THIS FILE IS CREATED BY THE CONTRACT COMMIT AND FILLED BY THE PACKAGE THAT OWNS IT, and the
// split is the whole reason it exists this early. MG_Impl/Pipe/PipeFill.cpp is the contract
// package's for the entire phase - it carries Coverage.def's enum-coupled block, the validate
// point and the death helpers - so the emitter package must not edit it. What it edits instead
// is this header: the emitter's BODY, and the value of kMGPipeWiredFramebufferSubsystem below.
// That is what makes "no file is touched twice by two packages" structural rather than a
// convention, and it is what the bb2a236d semantic-merge trap taught (two branches green
// separately, the integrated tree not compiling).
//
// HEADER-ONLY, for the ownership reason Tracker.h and ResourceTracker.h both state: the root
// CMakeLists.txt that would name a new .cpp is the contract package's and is frozen behind the
// tag. MG_Impl/Pipe/PipeFill.cpp is the one translation unit that includes it in the library.
#if MOBILEGL_PIPE_PUSH
#include <MG_Pipe/MGPipe.h>
#include <MG_Pipe/PipeApply.h>
#include <MG_State/GLState/Core.h>

namespace MobileGL::MG_Pipe {

    // WHICH SUBSYSTEM BIT THIS BUILD ACTUALLY EMITS FOR, and it is 0 until the emitter below
    // has a body. PipeFill.cpp ORs the four per-family constants into kMGPipeWiredSubsystems,
    // so the bit is added by the commit that gives the emitters their bodies, with no file
    // touched twice - and a Coverage.def row can never silently drop a field on the floor
    // before the call that carries it exists.
    inline constexpr Uint64 kMGPipeWiredFramebufferSubsystem = 0;

    // set_framebuffer_state. STUB AT THE CONTRACT COMMIT: it emits nothing and returns 0
    // payload bytes, so the validate point's ladder has its final shape and the package that
    // fills this in never edits PipeFill.cpp.
    class MGPipeFramebufferEmitter {
    public:
        using GLContext = MG_State::GLState::GLContext;

        // Returns the bytes that went on the wire, for the per-draw payload histogram.
        Uint64 EmitFramebufferState(GLContext& ctx) {
            (void)ctx;
            return 0;
        }

        // A fresh context: what the server has is no longer what this emitter last sent. Only
        // LATCHES reset here - the applier's object records survive a make-current and
        // re-publishing them would move their serials for nothing.
        void Reset() {}
    };

    inline MGPipeFramebufferEmitter& MGPipeFramebufferEmitterInstance() {
        // NEVER DESTROYED, for MGPipeTrackerInstance()' reason (MG_Impl/Pipe/Tracker.h): the
        // rule covers every MGPipe process singleton, not only the ones a frontend destructor
        // reaches today, and it is what keeps exit() out of a torn-down pipe.
        static MGPipeFramebufferEmitter* emitter = new MGPipeFramebufferEmitter();
        return *emitter;
    }
} // namespace MobileGL::MG_Pipe
#endif // MOBILEGL_PIPE_PUSH
