// MobileGL - MobileGL/MG_Impl/Pipe/TextureEmit.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

// The CLIENT side of P4a's texture and renderbuffer family: resource_create from the object's
// constructor, resource_respecify from every storage-defining entry point, set_texture_params
// from the parameter mutators, and resource_subdata from the DRAIN LIST at the validate point.
//
// THE THREE OBJECT CALLS ARE NOT EMITTED FROM HERE'S CALLER, they are emitted from MG_State's
// own mutators - a constructor, a storage definition, a glTexParameter - exactly as P3a's
// buffer family is, because that is where the event happens. Only the sub-data drain runs at
// the validate point, which is the explicit exception ARCHITECTURE.md 5.1 makes for texture
// upload: walking every live texture per verb is the cost the drain list exists to avoid.
//
// THIS FILE IS CREATED BY THE CONTRACT COMMIT AND FILLED BY THE PACKAGE THAT OWNS IT - see
// FramebufferEmit.h for why, in full: PipeFill.cpp is the contract package's for the whole
// phase, so the emitter package edits this header and the value of
// kMGPipeWiredTextureSubsystem below, and never that file.
//
// HEADER-ONLY, for the ownership reason Tracker.h and ResourceTracker.h both state.
#if MOBILEGL_PIPE_PUSH
#include <MG_Pipe/MGPipe.h>
#include <MG_Pipe/PipeApply.h>
#include <MG_State/GLState/Core.h>

namespace MobileGL::MG_Pipe {

    // 0 until the emitter below has a body; see FramebufferEmit.h's note.
    inline constexpr Uint64 kMGPipeWiredTextureSubsystem = 0;

    // STUB AT THE CONTRACT COMMIT: emits nothing, returns 0 payload bytes.
    class MGPipeTextureEmitter {
    public:
        using GLContext = MG_State::GLState::GLContext;

        // The DRAIN LIST, at the validate point: one resource_subdata per dirty
        // (storage owner, upload target, level) that was appended on its FIRST dirty mark and
        // is cleared at emission. Keyed on the STORAGE OWNER from day one - a view and its
        // owner already share one dirty state - so an upload through a view and an upload
        // through the owner land on the same key.
        //
        // The client clears its own dirty flags here, and ONLY for the levels whose record the
        // applier accepted; the applier accumulates the emitted shape into a server-side
        // pending-upload set that survives Espryt's bail arms, which is what stops a bail from
        // losing texels.
        //
        // Returns the bytes that went on the wire, for the per-draw payload histogram.
        Uint64 DrainTextureSubData(GLContext& ctx) {
            (void)ctx;
            return 0;
        }

        void Reset() {}
    };

    inline MGPipeTextureEmitter& MGPipeTextureEmitterInstance() {
        // NEVER DESTROYED, for MGPipeTrackerInstance()' reason.
        static MGPipeTextureEmitter* emitter = new MGPipeTextureEmitter();
        return *emitter;
    }
} // namespace MobileGL::MG_Pipe
#endif // MOBILEGL_PIPE_PUSH
