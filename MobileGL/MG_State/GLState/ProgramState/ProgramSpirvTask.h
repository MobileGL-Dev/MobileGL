// MobileGL - MobileGL/MG_State/GLState/ProgramState/ProgramSpirvTask.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <MG_State/GLState/ProgramState/ProgramLinkTask.h>
#include <MG_Util/Async/JobNode.h>

namespace MobileGL::MG_State::GLState {
    // PHASE B of one glLinkProgram: GlslangToSpv, spirv-opt, and the SPIRV-Cross pass that
    // builds the glUniform*-to-scratch routing tables. Chained behind exactly one
    // ProgramLinkTask and joined by exactly five ProgramObject getters (GetGeneratedSpirv,
    // GetUniformOffset, MapUBO, GetUBOData, GetUBOSize), so ~120 other getters and the whole
    // GL query surface stay on the phase-A gate and answer without waiting for any of this.
    //
    // ---- what this node may read, and what it may not ----
    // It holds the phase-A node by SharedPtr and reads `phaseA->spirvHandoff` plus
    // `phaseA->in`. It must NEVER read `phaseA->artifacts` or `phaseA->diagnostics`: the GL
    // thread MOVES the artifacts out of the node at the phase-A join and DRAINS the
    // diagnostics there, and both of those can happen while this body runs. The handoff exists
    // precisely so this node has a copy of everything it needs that the join does not touch.
    // (The general JobNode rule - a terminal node is immutable, so its outputs need no further
    // synchronization - covers everything except the two members the join consumes.)
    //
    // ---- lifetime ----
    // The handoff owns the Vector<SharedPtr<glslang::TShader>>, and that is mandatory rather
    // than tidy: glslang::TProgram stores raw TShader* and, for the one-shader-per-stage case,
    // BORROWS each stage's TIntermediate from its TShader. GlslangToSpv reads exactly those
    // intermediates. Before the split the shaders died when ProgramLinkTask::RunBody returned,
    // which was safe only because nothing called getIntermediate() afterwards.
    //
    // ---- failure ----
    // A cancel (relink, teardown, program destruction) or an optimizer failure publishes
    // spirvStatus = false rather than a half-built program. GL cannot retract a LINK_STATUS it
    // already reported true, so such a program stays linked and fully queryable; it is just
    // not drawable, which the backends express through their existing link-status gates.
    class ProgramSpirvTask final : public MG_Util::Async::JobNode {
    public:
        // ---- output: valid iff IsComplete(), immutable afterwards ----
        // Moved (never copied) into the ProgramObject by EnsureSpirvJoined().
        ProgramObject::SpirvArtifacts artifacts;

        // Posts this job when `phaseA` goes terminal - and not one moment earlier, so the body
        // never waits on anything (invariant I4: no job body may block on another job). A
        // single dependency needs no counter, just the one continuation; it runs inline right
        // here if `phaseA` is already terminal, which is the same case
        // ProgramLinkTask::SubmitAfter already reasons about.
        //
        // GL thread only, and only after the caller has stored a SharedPtr to this node: the
        // continuation takes shared_from_this().
        void SubmitAfter(const SharedPtr<ProgramLinkTask>& phaseA);

        // The async-off / glMaxShaderCompilerThreadsKHR(0) path: run the body on the calling
        // thread, right now, against an ALREADY-TERMINAL phase A. Deliberately not routed
        // through SubmitAfter, whose continuation would Post() to a pool that is merely
        // unused rather than stopped - that would move the work off-thread in the one mode
        // whose contract is "byte-identical to the synchronous implementation".
        void RunInlineAfter(const SharedPtr<ProgramLinkTask>& phaseA);

    private:
        void RunBody() override;

        void GenerateSpirv(const ProgramLinkTask::SpirvHandoff& handoff, Uint externalIndex,
                           Bool deferOutputValidationForDirectVulkan, Bool enableSpirvValidation,
                           Bool nativeFloat64, Bool demoteTessellationPointSize,
                           Bool demoteGeometryPointSize);
        void BuildGlobalUboRouting(const ProgramLinkTask::SpirvHandoff& handoff, Uint externalIndex);

        // Worker-side MGLOG replacement, replayed by the join on the GL thread. Same reason as
        // ProgramLinkTask::DeferLog.
        void DeferLog(String line);

        SharedPtr<ProgramLinkTask> m_phaseA;
    };
} // namespace MobileGL::MG_State::GLState
