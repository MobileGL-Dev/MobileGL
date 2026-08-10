// MobileGL - MobileGL/MG_State/GLState/ProgramState/ProgramLinkTask.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <MG_State/GLState/ProgramState/ProgramObject.h>
#include <MG_State/GLState/ProgramState/ShaderCompileTask.h>
#include <MG_Util/Async/JobNode.h>
#include <MG_Util/ShaderTranspiler/CompileEnv.h>

namespace MobileGL::MG_State::GLState {
    // One attached shader, as the link sees it: never the ShaderObject, always a snapshot.
    //
    // The ShaderObject is GL-thread-owned and may be re-sourced, detached or destroyed while
    // this link is still queued; everything below is either immutable or independently owned,
    // so none of that can reach the worker.
    struct LinkShaderInput {
        ShaderStage stage = ShaderStage::Unknown;
        // For the compile-error diagnostic and the compute local_size check, both of which
        // quote the ORIGINAL source rather than the preprocessed one.
        SharedPtr<const String> source;
        // The authoritative compiled state. Null, or non-Complete, both read as "this shader
        // did not compile" - the same verdict ShaderObject's join gate produces.
        SharedPtr<const ShaderCompileTask> compiled;
    };

    // PHASE A of one glLinkProgram: the half that decides what GL can be asked about the
    // program - glslang link + mapIO, the GL-facing reflection surface, fragment-output
    // validation and transform-feedback resolution - with every input it needs snapshotted at
    // enqueue.
    //
    // Every one of the eight ways a link can fail lives here, so once this node has published
    // through EnsureLinkJoined() the program's LINK_STATUS, info log and entire query surface
    // are FINAL and truthful. SPIR-V generation, spirv-opt and the global-UBO routing tables
    // moved to ProgramSpirvTask, which chains behind this node and is joined by only five
    // getters (see ProgramObject::EnsureSpirvJoined).
    //
    // Same ownership rule as ShaderCompileTask: the body reads nothing but `in` (all of it
    // owned or immutable) and writes nothing but `artifacts`. No GL call, no
    // pActiveBackendObject read, no pGLContext->RecordError(); the device limits arrive
    // through the CompileEnv snapshot and diagnostics are deferred to the join.
    //
    // ONE LINK IS ONE HANDLER. RunBody() runs start to finish inside a single pool handler
    // and is the only place `artifacts` is written. Splitting it across handlers to
    // "pipeline" the reflection half would let a cancel land between the halves and publish a
    // program whose SPIR-V and reflection describe different things - so any such split has
    // to be structural: the first half must publish a LINK_STATUS and a query surface that
    // are already final, and a lost second half must degrade to "linked but not drawable",
    // never to a half-published program. (The intermediates' ordering constraint that used to
    // be quoted here is retested and no longer binding; see the ordering note in RunBody.)
    class ProgramLinkTask final : public MG_Util::Async::JobNode {
    public:
        // ---- inputs, snapshotted on the GL thread in ProgramObject::Link()'s prologue ----
        struct Inputs {
            Uint externalIndex = 0; // logs only
            Vector<LinkShaderInput> shaders; // already stage-sorted
            SharedPtr<const MG_Util::ShaderTranspiler::CompileEnv> env;
            // The four "takes effect at the next link" request maps. Snapshotted rather than
            // referenced, which is precisely what makes glBindAttribLocation and friends
            // legal to call over a pending link without cancelling it: the pending link keeps
            // linking the inputs it was given.
            UnorderedMap<String, Uint> explicitAttribLocations; // glBindAttribLocation
            UnorderedMap<String, Uint> explicitFragDataLocation; // glBindFragDataLocation
            UnorderedMap<String, Uint> explicitFragDataIndex; // glBindFragDataLocationIndexed
            Vector<String> requestedXfbVaryings; // glTransformFeedbackVaryings
            GLenum requestedXfbBufferMode = GL_INTERLEAVED_ATTRIBS;
            Int maxFragmentOutputColorNumber = 8; // GL_MAX_DRAW_BUFFERS, stamped in by the entry point
        } in;

        // ---- output: valid iff IsComplete(), immutable afterwards ----
        // Moved (never copied) into the ProgramObject by EnsureLinkJoined().
        ProgramObject::LinkArtifacts artifacts;

        // ---- output: everything ProgramSpirvTask needs to run without this node's
        //      artifacts, filled at the tail of a successful RunBody() ----
        //
        // THIS IS NOT `artifacts` AND MUST NOT BE MERGED INTO IT. The GL thread MOVES
        // `artifacts` out of this node at the join, and phase B runs on a worker afterwards -
        // so phase B may read `spirvHandoff` and `in` (neither is ever touched by the join)
        // and this node's JobState, and nothing else on it. Reading `artifacts` or
        // `diagnostics` from phase B would race the publish.
        struct SpirvHandoff {
            // MANDATORY, and the reason this struct exists at all: TProgram::addShader stores
            // a RAW TShader*, and for the one-shader-per-stage case getIntermediate() returns
            // the TShader's own intermediate rather than a copy. These used to die when
            // RunBody() returned, which was safe only because nothing called getIntermediate()
            // afterwards. GlslangToSpv does exactly that, so phase B has to own them.
            Vector<SharedPtr<glslang::TShader>> shaders;
            // GL enum per entry of `in.shaders`, in the same order (GetSpirvBinaryFromProgram
            // walks it to pick the intermediates).
            Vector<GLenum> shaderTypes;
            // The reflection slice BuildGlobalUboRouting consumes: {program, uniformLocations,
            // uniformIndexInTProgram, tProgramUniformIndexToGl, maxUniformLocation}. Carried
            // as a LinkArtifacts with only those five fields set, so the routing pass can keep
            // calling ProgramObject::IsValidUniformLocation / GetUniformArraySizeByTIndex
            // unchanged. The SharedPtr copy of `program` is also what keeps the TProgram alive
            // for phase B after the join has moved `artifacts` away.
            ProgramObject::LinkArtifacts reflection;

            // The one flag phase B tests before doing anything: false means this link never
            // reached the tail of RunBody (it failed, or was cancelled mid-body).
            Bool ready = false;
        } spirvHandoff;

        // Posts this job once every compile in `deps` is terminal - and not one moment
        // earlier, so the body never waits on anything (invariant I4: no job body may block
        // on another job, or the pool could deadlock with all its workers waiting on each
        // other). `deps` is the subset of the snapshot's compile nodes that were still
        // in flight; an already-terminal one needs no edge.
        //
        // GL thread only, and only after the caller has stored a SharedPtr to this node:
        // OnDepSettled takes shared_from_this().
        void SubmitAfter(const Vector<SharedPtr<ShaderCompileTask>>& deps);

    private:
        void RunBody() override;

        // Runs when one dependency goes terminal - on whichever thread drove it there, which
        // is a pool worker for a compile that finished on one. Non-throwing by construction;
        // see the definition.
        void OnDepSettled();

        // ---- the link body, split exactly as ProgramObject::Link() had it ----
        // Each returns false to abort the link with `artifacts.infoLog` already set, which is
        // GL's definition of a failed link: LINK_STATUS false plus a log, never a GL error.
        Bool ConsumeShaders(Vector<SharedPtr<glslang::TShader>>& outShaders);
        Bool DoReflection(const MG_Util::ShaderTranspiler::CompileEnv& env);
        Bool ValidateFragmentOutputLocations();
        Bool ResolveTransformFeedbackVaryings();
        void ResolveGsTriangleStripCapture(const glslang::TIntermediate* captureIntermediate);

        // Worker-side MGLOG replacement: appended to diagnostics.logLines and replayed by the
        // join, on the GL thread, where a serial implementation would have printed it.
        // Logging straight from a worker interleaves mid-line with the GL thread's output and
        // lands out of order relative to the glLinkProgram that caused it.
        void DeferLog(String line);

        // Counts down to zero exactly once. Starts at deps + 1: the extra guard is released
        // by SubmitAfter itself, so a dependency that settles while the edges are still being
        // registered cannot post the job from under a half-built dependency list.
        std::atomic<Int> m_remainingDeps{0};
    };
} // namespace MobileGL::MG_State::GLState
