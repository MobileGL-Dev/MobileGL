// MobileGL - MobileGL/MG_State/GLState/ProgramState/ShaderObject.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

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
            ShaderObject(const ShaderStage stage, Uint externalIndex,
                         ShaderPreprocessCache* preprocessCache = nullptr)
                : m_stage(stage), m_externalIndex(externalIndex), m_preprocessCache(preprocessCache) {}
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
            const SharedPtr<glslang::TShader>& GetCompiledShader() const { return m_shader; }
            const String& GetInfoLog() const { return m_infoLog; }
            const UnorderedMap<String, Uint>& GetUniformLocations() const { return m_uniforms; }
            // Explicit layout(location = N) qualifiers on this shader's default-block
            // uniforms, captured lexically at Compile() because the relaxed parse drops
            // them from reflection (see ExtractExplicitUniformLocations).
            const UnorderedMap<String, Int>& GetExplicitUniformLocations() const {
                return m_explicitUniformLocations;
            }
            // Explicit layout(binding = N) on sampler/image uniforms - their initial
            // texture/image units - captured lexically for the same reason (see
            // ExtractExplicitOpaqueBindings).
            const UnorderedMap<String, Uint>& GetExplicitOpaqueBindings() const { return m_explicitOpaqueBindings; }
            Bool GetCompileStatus() const { return m_compileStatus; }
            Bool GetDeleteStatus() const { return m_deleteStatus; }

            // True while this object holds the outcome (success OR failure) of a previous
            // Compile() of exactly the source it currently holds - i.e. while the P0b
            // layer-1 memo is armed and a glCompileShader would be a no-op. Diagnostics
            // and tests only; nothing in the GL frontend branches on it.
            Bool HasMemoizedCompile() const { return m_hasCompiledState; }

        private:
            void InvalidateCompiledState();
            // ---- P0b layer 1: per-object no-op recompile ----
            // True iff `candidate` is byte-identical to the source that produced the
            // compiled state this object is currently holding. The stored hash and length
            // are only a fast reject; the answer is always confirmed against the full
            // stored text, so no behaviour rides on a 64-bit hash.
            Bool SourceMatchesCompiledState(const String& candidate) const;
            // Arms the layer-1 memo for the source that Compile() just processed.
            void RememberCompiledSource(Uint64 sourceHash);

            const Uint m_externalIndex = 0;
            const ShaderStage m_stage;
            String m_source;
            // The source Compile() actually parsed (after PreprocessShaderSource), kept
            // for TakeShaderForLink's re-parse so a later link never depends on the
            // preprocessor being deterministic across backend-state changes.
            String m_preprocessedSource;
            SharedPtr<glslang::TShader> m_shader;
            UnorderedMap<String, Uint> m_uniforms;
            UnorderedMap<String, Int> m_explicitUniformLocations;
            UnorderedMap<String, Uint> m_explicitOpaqueBindings;
            Bool m_shaderConsumedByLink = false;

            // P0b layer 2: the owning context's cross-object memo, or null. Not owned.
            ShaderPreprocessCache* const m_preprocessCache = nullptr;
            // P0b layer 1. m_hasCompiledState is the invariant "m_source is byte-identical
            // to the source that produced m_compileStatus/m_infoLog/m_shader"; it is armed
            // at the end of every Compile() and disarmed by InvalidateCompiledState().
            Bool m_hasCompiledState = false;
            Uint64 m_compiledSourceHash = 0;
            SizeT m_compiledSourceLength = 0;

            String m_infoLog;
            Bool m_deleteStatus = false;
            Bool m_compileStatus = false;
        };
    } // namespace MG_State::GLState
} // namespace MobileGL
