// MobileGL - MobileGL/MG_State/GLState/ProgramState/ShaderObject.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <MG_Util/ShaderTranspiler/CompileEnv.h>

namespace MobileGL {
    enum class ShaderStage {
        Vertex,
        TessControl,
        TessEval,
        Geometry,
        Fragment,
        Compute,
        ShaderStageCount,
        Unknown = -1
    };

    namespace MG_State::GLState {
        // P0b layer 2. Declared, not included: the cache keys on ShaderStage, so including
        // its header here would be circular.
        class ShaderPreprocessCache;

        class ShaderObject {
        public:
            // `preprocessCache` is the owning context's cross-object memo (P0b layer 2);
            // null is fully supported and simply means "no sharing" - that is what the
            // context-less internal shader objects (the default FS, the blit pipeline) use.
            // Shared ownership rather than a raw pointer: once compiles run on a worker the
            // job outlives neither the object nor the context deterministically, and the
            // cache has to stay alive for whoever is still reading it.
            ShaderObject(const ShaderStage stage, Uint externalIndex,
                         SharedPtr<ShaderPreprocessCache> preprocessCache = nullptr)
                : m_stage(stage), m_externalIndex(externalIndex), m_preprocessCache(Move(preprocessCache)) {}
            void SetShaderSource(const String& source);
            void SetShaderSource(String&& source);
            void Compile();
            void MarkAsDeleted();

            // Hands out a link-consumable TShader. glslang's mapIO mutates the TShader's
            // aliased intermediate, so the parse stored by Compile() may feed exactly one
            // link; every later link (relink, or the same shader attached to a second
            // program) gets a fresh parse of the stored preprocessed source through the
            // byte-identical CompileShader path (including the legacy-460 retry). Only
            // callable while GetCompileStatus() is true. Returns null only if that
            // re-parse fails - outReparseLog then carries its diagnostics.
            SharedPtr<glslang::TShader> TakeShaderForLink(String& outReparseLog);

            Uint GetExternalIndex() const { return m_externalIndex; }
            ShaderStage GetShaderStage() const { return m_stage; }
            const String& GetShaderSource() const { return m_source; }
            const SharedPtr<glslang::TShader>& GetCompiledShader() const { return Compiled().shader; }
            const String& GetInfoLog() const { return Compiled().infoLog; }
            const UnorderedMap<String, Uint>& GetUniformLocations() const { return Compiled().uniforms; }
            // Explicit layout(location = N) qualifiers on this shader's default-block
            // uniforms, captured lexically at Compile() because the relaxed parse drops
            // them from reflection (see ExtractExplicitUniformLocations).
            const UnorderedMap<String, Int>& GetExplicitUniformLocations() const {
                return Compiled().explicitUniformLocations;
            }
            // Explicit layout(binding = N) on sampler/image uniforms - their initial
            // texture/image units - captured lexically for the same reason (see
            // ExtractExplicitOpaqueBindings).
            const UnorderedMap<String, Uint>& GetExplicitOpaqueBindings() const {
                return Compiled().explicitOpaqueBindings;
            }
            Bool GetCompileStatus() const { return Compiled().compileStatus; }
            Bool GetDeleteStatus() const { return m_deleteStatus; }

            // Blocks until a pending compile (P1 stage 3 onwards) has published its
            // artifacts. Public for the few sites that must join without reading anything.
            // A no-op today - nothing is ever pending.
            void JoinCompile() const { EnsureCompileJoined(); }

            // True while this object holds the outcome (success OR failure) of a previous
            // Compile() of exactly the source it currently holds - i.e. while the P0b
            // layer-1 memo is armed and a glCompileShader would be a no-op. Diagnostics
            // and tests only; nothing in the GL frontend branches on it.
            //
            // Deliberately does NOT join: the memo bookkeeping below is GL-thread-owned and
            // says nothing about whether a worker has finished, which is exactly the
            // property GL_COMPLETION_STATUS_KHR needs when stage 3 lands.
            Bool HasMemoizedCompile() const { return m_hasCompiledState; }

        private:
            // ---- P1: everything a compile PRODUCES, in one block ----
            //
            // Same rule as ProgramObject::LinkArtifacts: this is exactly what
            // InvalidateCompiledState() clears, i.e. exactly what one run of Compile()
            // writes. Stage 3 lifts this struct wholesale into ShaderCompileTask, where a
            // worker fills it in and the GL thread reads it through the same gate.
            struct CompileArtifacts {
                // The CompileEnv snapshot this compile ran against. Held so the
                // consume-once re-parse in TakeShaderForLink() reproduces the original
                // parse exactly, instead of re-reading whatever the backend says now.
                SharedPtr<const MG_Util::ShaderTranspiler::CompileEnv> env;
                SharedPtr<glslang::TShader> shader;
                // The source Compile() actually parsed (after PreprocessShaderSource), kept
                // for TakeShaderForLink's re-parse so a later link never depends on the
                // preprocessor being deterministic across backend-state changes.
                String preprocessedSource;
                UnorderedMap<String, Uint> uniforms;
                UnorderedMap<String, Int> explicitUniformLocations;
                UnorderedMap<String, Uint> explicitOpaqueBindings;
                Bool shaderConsumedByLink = false;
                String infoLog;
                Bool compileStatus = false;
            };

            // ---- The one and only join gate for compile output (P1 invariant I5) ----
            // Blocks until a pending compile has published into m_compiled. Today nothing
            // is ever pending - glCompileShader still runs the whole body inline - so this
            // is an unconditional no-op. It exists NOW so that every reader of compile
            // output is already routed through it when stage 3 makes it block.
            //
            // Defined inline (not in ShaderObject.cpp): called from every Compiled() read,
            // and the project never builds with LTO, so an out-of-line empty body would be
            // a real cross-TU call at each of those call sites instead of folding away.
            void EnsureCompileJoined() const {}
            CompileArtifacts& Compiled() {
                EnsureCompileJoined();
                return m_compiled;
            }
            const CompileArtifacts& Compiled() const {
                EnsureCompileJoined();
                return m_compiled;
            }

            void InvalidateCompiledState();
            // ---- P0b layer 1: per-object no-op recompile ----
            // True iff `candidate` is byte-identical to the source that produced the
            // compiled state this object is currently holding. The stored hash and length
            // are only a fast reject; the answer is always confirmed against the full
            // stored text, so no behaviour rides on a 64-bit hash.
            Bool SourceMatchesCompiledState(const String& candidate) const;
            // Arms the layer-1 memo for the source that Compile() just processed.
            void RememberCompiledSource(Uint64 sourceHash);

            // ---- GL-thread-owned state: never produced by a compile, so it never joins ----
            const Uint m_externalIndex = 0;
            const ShaderStage m_stage;
            // glShaderSource text. A worker only ever reads the snapshot handed to it, so
            // GL_SHADER_SOURCE_LENGTH and glGetShaderSource never join.
            String m_source;

            // P0b layer 2: the owning context's cross-object memo, or null.
            const SharedPtr<ShaderPreprocessCache> m_preprocessCache;
            // P0b layer 1. m_hasCompiledState is the invariant "m_source is byte-identical
            // to the source that produced the compile artifacts"; it is armed at the end of
            // every Compile() and disarmed by InvalidateCompiledState(). Stage 3 replaces
            // all three with a pointer compare against the in-flight job's source snapshot.
            Bool m_hasCompiledState = false;
            Uint64 m_compiledSourceHash = 0;
            SizeT m_compiledSourceLength = 0;

            Bool m_deleteStatus = false;

            // ---- Compile OUTPUT ---- reachable only through Compiled().
            CompileArtifacts m_compiled;
        };
    } // namespace MG_State::GLState
} // namespace MobileGL
