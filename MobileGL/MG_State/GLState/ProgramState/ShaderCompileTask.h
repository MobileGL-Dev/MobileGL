// MobileGL - MobileGL/MG_State/GLState/ProgramState/ShaderCompileTask.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <MG_Util/Async/JobNode.h>
#include <MG_Util/ShaderTranspiler/CompileEnv.h>
#include <MG_State/GLState/ProgramState/ShaderPreprocessCache.h>

namespace MobileGL::MG_State::GLState {
    // glslang has no "detach this thread" API in the vendored revision, but TShader::parse
    // leaves the calling thread's TLS pool allocator pointing at the shader's own pool and
    // never restores it. Left there, the next allocation this thread makes - in an unrelated
    // job, or in glslang code reached from a different object - would come out of a pool the
    // GL thread may already have deleted with the TShader. SetThreadPoolAllocator(nullptr)
    // reverts the thread to its own thread_local default and is the documented idiom.
    //
    // A scope guard, so it also runs when a body throws. Declared here rather than kept
    // file-local because stage 4 gave it a second user: ProgramLinkTask's body parses (the
    // claim-CAS loser's re-parse), links and emits SPIR-V, all on a pool thread.
    struct GlslangThreadAllocatorGuard {
        GlslangThreadAllocatorGuard() = default;
        ~GlslangThreadAllocatorGuard();
        GlslangThreadAllocatorGuard(const GlslangThreadAllocatorGuard&) = delete;
        GlslangThreadAllocatorGuard& operator=(const GlslangThreadAllocatorGuard&) = delete;
    };

    // Everything one glCompileShader PRODUCES, in one block.
    //
    // This is exactly the set a single run of the compile pipeline writes, which is what
    // makes "discard the artifacts" a complete invalidation and "move the artifacts" a
    // complete publish. It lives on the job node rather than on ShaderObject: a worker fills
    // it in, and the GL thread reads it through ShaderObject's join gate.
    struct ShaderCompileArtifacts {
        // The CompileEnv snapshot this compile ran against. Held so the consume-once
        // re-parse in ClaimParsedShader() reproduces the original parse exactly, instead of
        // re-reading whatever the backend says now.
        SharedPtr<const MG_Util::ShaderTranspiler::CompileEnv> env;
        SharedPtr<glslang::TShader> shader;
        // The source the parse actually consumed (after PreprocessShaderSource), kept for
        // ClaimParsedShader's re-parse so a later link never depends on the preprocessor
        // being deterministic across backend-state changes.
        String preprocessedSource;
        UnorderedMap<String, Int> explicitUniformLocations;
        UnorderedMap<String, Uint> explicitOpaqueBindings;
        String infoLog;
        Bool compileStatus = false;
    };

    // The unit of asynchronous shader compilation: one glCompileShader's worth of pure CPU
    // work - preprocess, the two lexical rejections, the two lexical extractions, and the
    // glslang parse - with every input it needs owned by the node itself.
    //
    // That ownership is the whole point. The node reads no GL-thread state (the source is a
    // SharedPtr<const String> snapshot, the device limits come from the CompileEnv snapshot,
    // the P0b cross-object memo is shared-owned and internally locked) and writes nothing
    // but its own `artifacts`. So a node whose ShaderObject was re-sourced, deleted, or
    // destroyed while it was still running is safe to simply abandon - no wait, no
    // synchronization with the GL thread beyond the node's own terminal state.
    class ShaderCompileTask final : public MG_Util::Async::JobNode {
    public:
        ShaderCompileTask(const ShaderStage stage, SharedPtr<const String> source, const Uint64 sourceHash,
                          SharedPtr<const MG_Util::ShaderTranspiler::CompileEnv> env,
                          SharedPtr<ShaderPreprocessCache> cache, const Uint externalIndex)
            : stage(stage), source(Move(source)), sourceHash(sourceHash), env(Move(env)), cache(Move(cache)),
              externalIndex(externalIndex) {}

        // ---- inputs: immutable after construction, all owned by the node ----
        const ShaderStage stage;
        // The exact text at enqueue. ShaderObject compares this pointer against its own
        // m_source to decide whether its layer-1 memo is armed, which is why glShaderSource
        // only ever swaps the pointer when the text genuinely differs.
        const SharedPtr<const String> source;
        const Uint64 sourceHash;
        const SharedPtr<const MG_Util::ShaderTranspiler::CompileEnv> env;
        // P0b layer 2, or null. Null is the "no context" case (the default fragment shader,
        // the backends' internal blit/mipmap shaders) and doubles as the marker for
        // "compile inline regardless of the async flag" - see ShaderObject::Compile().
        const SharedPtr<ShaderPreprocessCache> cache;
        const Uint externalIndex; // logs only

        // ---- output: valid iff IsComplete(), immutable afterwards ----
        ShaderCompileArtifacts artifacts;

        // Hands out a link-consumable TShader, exactly once for the stored parse.
        //
        // glslang's mapIO mutates the TShader's aliased intermediate, so the parse this node
        // produced may feed exactly ONE link; every later link (a relink, or the same shader
        // attached to a second program) needs a fresh parse. The claim is a CAS on this
        // shared node rather than a flag on the ShaderObject because from stage 4 the two
        // callers can be two ProgramLinkTasks running on two workers: two programs sharing
        // one shader, linked back to back. Copying the parse out and tracking consumed-ness
        // per program would let both of them decide they were the first, run mapIO over the
        // same intermediate twice, and ship silently corrupt SPIR-V.
        //
        // The CAS loser re-parses artifacts.preprocessedSource against THIS node's own
        // CompileEnv (not against whatever the backend reports now), through the identical
        // CompileShader path - so winner and loser produce byte-identical SPIR-V. Callable
        // only once IsComplete() and compileStatus are true. Returns null only if that
        // re-parse fails, and outReparseLog then carries its diagnostics.
        //
        // Const because the claim is the node's own synchronization, not a mutation of its
        // published artifacts: a claim that is taken and then abandoned (its link was
        // cancelled) costs one extra re-parse later and nothing else.
        SharedPtr<glslang::TShader> ClaimParsedShader(String& outReparseLog) const;

        // Sticky marker for "a ProgramLinkTask has this node in its input snapshot".
        //
        // It exists to keep a cancel from eating a result someone still needs. A pending link
        // holds its dependencies by SharedPtr, so the NODE always outlives the ShaderObject -
        // but Cancel() is not about lifetime, it discards the result. The reachable sequence
        // is the ordinary one: compile, attach, glLinkProgram (enqueued), glDetachShader,
        // glDeleteShader. The detach makes the shader GL-invisible, so the delete frees its
        // name, and ReleaseShaderNameIfOrphaned would cancel a compile the enqueued link is
        // waiting on - turning a link that must report GL_TRUE into GL_FALSE. Set on the GL
        // thread in Link()'s prologue, read on the GL thread by ShaderObject::CancelCompile.
        //
        // Never cleared: the worst case is one stale node compiling to completion for nobody,
        // which is exactly what the pre-stage-3 implementation always did.
        void MarkLinkReferenced() { m_linkReferenced.store(true, std::memory_order_release); }
        Bool IsLinkReferenced() const { return m_linkReferenced.load(std::memory_order_acquire); }

    private:
        void RunBody() override;
        // The real body; RunBody wraps it so a throw becomes a GL-visible compile failure.
        void RunCompilePipeline();

        mutable std::atomic<Bool> m_parseClaimed{false};
        std::atomic<Bool> m_linkReferenced{false};
    };
} // namespace MobileGL::MG_State::GLState
