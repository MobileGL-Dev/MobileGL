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
    // Everything one glCompileShader PRODUCES, in one block.
    //
    // This is exactly the set a single run of the compile pipeline writes, which is what
    // makes "discard the artifacts" a complete invalidation and "move the artifacts" a
    // complete publish. It lives on the job node rather than on ShaderObject: a worker fills
    // it in, and the GL thread reads it through ShaderObject's join gate.
    struct ShaderCompileArtifacts {
        // The CompileEnv snapshot this compile ran against. Held so the consume-once
        // re-parse in TakeShaderForLink() reproduces the original parse exactly, instead of
        // re-reading whatever the backend says now.
        SharedPtr<const MG_Util::ShaderTranspiler::CompileEnv> env;
        SharedPtr<glslang::TShader> shader;
        // The source the parse actually consumed (after PreprocessShaderSource), kept for
        // TakeShaderForLink's re-parse so a later link never depends on the preprocessor
        // being deterministic across backend-state changes.
        String preprocessedSource;
        UnorderedMap<String, Int> explicitUniformLocations;
        UnorderedMap<String, Uint> explicitOpaqueBindings;
        // GL-thread-owned, and the one field here a worker never touches: TakeShaderForLink
        // flips it after the join. Stage 4 replaces it with an atomic claim on this node,
        // because two ProgramLinkTasks for two programs sharing this shader can then race
        // for the parse on two workers.
        Bool shaderConsumedByLink = false;
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

    private:
        void RunBody() override;
        // The real body; RunBody wraps it so a throw becomes a GL-visible compile failure.
        void RunCompilePipeline();
    };
} // namespace MobileGL::MG_State::GLState
