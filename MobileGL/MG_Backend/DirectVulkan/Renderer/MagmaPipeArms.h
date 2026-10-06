// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/MagmaPipeArms.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

#include <Config.h>
// kMGPipeSubsystem* - the runtime bitmask's named bits - and MGPipeHandle itself. Both are
// header-only constant/POD declarations, and both are push-only, so the pull build's include
// graph is unchanged (G1).
#include <MG_Pipe/MGPipe.h>
#include <MG_Pipe/MGPipeHandles.h>

#include <cstdlib>

// Magma's arm selector for the P2 Track H / render-state re-keys (P2 brief D14). (The {slot, gen}
// mint that used to live here keyed the frontend arm's VAO memos and went with that arm in P13
// W6; the record arm keys every object on the applier's own handles.)
//
// Two switches decide which arm a re-keyed site runs, and they are NOT the same switch:
//
//   MOBILEGL_PIPE_PUSH (compile)          - is the pushed state there to be keyed on at all
//   Features.PipePush  (runtime bitmask)  - is THIS subsystem migrated in THIS run
//   MOBILEGL_PIPE_LEGACY_MEMOS (compile)  - is the pre-handle arm compiled beside it
//   Features.PipeLegacyMemos (runtime)    - may the pre-handle arm be ENTERED in this run
//
// ARCHITECTURE.md 9.6's point: once a handle wave lands, a clear MOBILEGL_PIPE_PUSH bit is
// only a valid A/B while the legacy arm is still compiled, because with the bit clear the
// backend would otherwise still run the re-keyed code. So a clear bit selects the legacy
// arm, and a run that has explicitly disabled the legacy arm may not fall into it.
//
// D14 spends that last sentence at STARTUP, not per draw: "a Track-H subsystem whose bit is
// clear is a startup Fatal{PipeLegacyMemosDisabled}". Nothing in the draw path aborts, and
// nothing outside Track H consults the legacy-memo lever at all - see
// MagmaPipeValidateSubsystemConfiguration below for both halves of that rule.
//
// The whole header is inert in a pull build: MOBILEGL_PIPE_PUSH is 0 there, every helper
// below is behind it, and the pull build's translation units are byte-identical (G1).
namespace MobileGL::MG_Backend::DirectVulkan {

    // Is `subsystemBit` (MG_Pipe/MGPipe.h's kMGPipeSubsystem*) migrated in this run?
    inline Bool MagmaPipeSubsystemOn(Uint64 subsystemBit) {
        return (MG_Config::Features.PipePush & subsystemBit) != 0;
    }

    // ---------------------------------------------------------------------------------
    // D14's startup gate
    // ---------------------------------------------------------------------------------
    //
    // Called once from VulkanRenderer::Initialize(), i.e. only when Magma is the backend
    // that is actually running. It answers exactly one question and it answers it before the
    // first draw: is there an arm for Magma's Track-H subsystem in this configuration?
    //
    // Three deliberate boundaries, each of which the per-draw shape this replaces got wrong:
    //
    //  * ONLY Magma's own Track-H bit is checked. Espryt's bit 5 is Espryt's business (a
    //    DirectVulkan run does not execute one line of DirectGLES' re-key), so
    //    MOBILEGL_PIPE_PUSH=0x20 must not kill a Magma run, and MOBILEGL_PIPE_PUSH=0x40 must
    //    not kill an Espryt one.
    //  * bit 0 (kMGPipeSubsystemRenderState) is NOT Track H and is NOT fatal. It is not a
    //    memo re-key at all: it decides where the pipeline memo's STATE KEY comes from, and
    //    a clear bit there simply means the client is not pushing render-state CSOs in this
    //    run, which GetOrCreatePipeline answers with its own state hash. D14 labels bits 5
    //    and 6 "Track H" and labels bit 0 nothing of the sort.
    //  * it is Fatal at STARTUP, once, not on a draw. A per-draw abort inside
    //    GetOrCreatePipeline turns a configuration mistake into a mid-frame crash and puts a
    //    branch nobody needs on the hottest path in the backend.
    //
    // [declared deviation from D14, review v2 minor 2] D14's runtime row reads "false: the
    // legacy arm is never entered", and D14's compile-switch row names ComputePipelineStateHash
    // as part of the pre-handle arm. Those two together would make MOBILEGL_PIPE_LEGACY_MEMOS=0
    // with bit 0 CLEAR a contradiction: the pipeline memo has no CSO handle to key on, so it
    // keys on a state hash, and in a build that compiles the pre-handle arm that hash IS
    // ComputePipelineStateHash. Magma does not make that fatal - bit 0 is not Track H, and
    // there is a correct answer (the state hash) where for bits 5/6 there is none - but it no
    // longer does it SILENTLY: the combination is named once, at startup, right here.
    inline void MagmaPipeValidateSubsystemConfiguration() {
        if (!MagmaPipeSubsystemOn(MG_Pipe::kMGPipeSubsystemRenderState)) {
            MGLOG_W("MGPipe: MOBILEGL_PIPE_LEGACY_MEMOS=0 with kMGPipeSubsystemRenderState (bit 0 "
                    "of MOBILEGL_PIPE_PUSH) clear - Magma's pipeline memo has no CSO handle to key "
                    "on, so every draw whose pipeline-state version moved runs the pre-handle STATE "
                    "HASH instead. That is not a Track-H subsystem and not fatal, but it is not the "
                    "handle arm either: set bit 0 (MOBILEGL_PIPE_PUSH=0x%llx) if this run was meant "
                    "to measure it.",
                    static_cast<unsigned long long>(MG_Config::Features.PipePush |
                                                    MG_Pipe::kMGPipeSubsystemRenderState));
        }
        if (MagmaPipeSubsystemOn(MG_Pipe::kMGPipeSubsystemMagmaVertexInput)) return;
        const char* const why =
            "this build has cmake -DMOBILEGL_PIPE_LEGACY_MEMOS=OFF, which compiles no such arm";
        MGLOG_F("MGPipe: Fatal{PipeLegacyMemosDisabled} Magma's Track-H subsystem "
                "(kMGPipeSubsystemMagmaVertexInput, bit 6 of MOBILEGL_PIPE_PUSH) is clear, so the "
                "vertex-input cache and the VAO draw memo want the pre-handle arm - but %s. Set "
                "bit 6 (MOBILEGL_PIPE_PUSH=0x%llx, or the default 0x%llx), or allow the legacy arm.",
                why,
                static_cast<unsigned long long>(MG_Config::Features.PipePush |
                                                MG_Pipe::kMGPipeSubsystemMagmaVertexInput),
                static_cast<unsigned long long>(MG_Pipe::kMGPipeSubsystemsMigratedAtP2));
        std::abort();
    }
} // namespace MobileGL::MG_Backend::DirectVulkan
