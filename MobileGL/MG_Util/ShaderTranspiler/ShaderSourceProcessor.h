// MobileGL - MobileGL/MG_Util/ShaderTranspiler/ShaderSourceProcessor.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <MG_State/GLState/ProgramState/ShaderObject.h>
#include <MG_Util/ShaderTranspiler/CompileEnv.h>

namespace MobileGL {
    enum class ShaderProfile {
        Core,
        Compatibility,
        ES,
    };

    namespace MG_Util {
        namespace ShaderTranspiler {
            // The whole source-rewriting pipeline. `env` is the compile-time snapshot of
            // everything outside (stage, source) this reads - advertised extensions and the
            // device-quirk inputs - so the transformation is a pure function of its three
            // arguments and can run on a worker thread.
            void PreprocessShaderSource(ShaderStage stage, String& source, const CompileEnv& env);
            // Convenience overload that resolves the current context's env itself. GL thread
            // only, and deliberately not used by the compile pipeline: it exists for the unit
            // tests and diagnostics that drive the preprocessor standalone.
            void PreprocessShaderSource(ShaderStage stage, String& source);

            // Rewrites a "#version 330 core" directive that PreprocessShaderSource normalized down
            // from a legacy desktop version back up to "#version 460 core". Returns false (leaving
            // the source untouched) for anything else: ES, compatibility, or an already-modern
            // declaration. Exists so a shader that only parses under the laxer 460 rules - e.g. it
            // uses 420-era syntax without the matching #extension line, which real drivers tend to
            // accept - can be retried instead of failing to compile.
            Bool RetargetLegacyVersionDirectiveTo460(String& source);

            // GLSL reserves a few names glslang happily accepts as identifiers ("packed",
            // "row_major" outside a layout(...) list, the image*Shadow family). Returns the
            // compile-error text for the first violation, or nullopt for a clean source.
            std::optional<String> FindReservedIdentifierViolation(const String& source);

            // Explicit layout(location = N) qualifiers on default-block uniform declarations,
            // keyed by declared name (no "[0]" suffix). Multi-declarator statements assign
            // consecutive locations, advancing by the array element count.
            //
            // Exists because the single link-compatible parse runs under relaxed Vulkan rules,
            // where glslang's vkRelaxedRemapUniformVariable moves plain uniforms into
            // MGL_GLOBAL_UBO and DISCARDS their location qualifiers ("ignoring layout qualifier
            // for uniform location"); opaque uniforms keep theirs. This lexical side-channel
            // restores the discarded locations to the GL location assigner
            // (ProgramObject::DoReflection). It scans preprocessor-visible text, so a
            // declaration inside an inactive #if branch is still recorded - harmless unless a
            // pack declares the same uniform with different explicit locations in alternative
            // branches (none observed; explicit uniform locations have zero incidence in the
            // shader-pack corpus, this is an ARB_explicit_uniform_location conformance surface).
            UnorderedMap<String, Int> ExtractExplicitUniformLocations(const String& source);

            // Explicit layout(binding = N) on sampler/image uniforms, i.e. their initial
            // texture/image units. The Vulkan-client relaxed parse strips these before
            // mapIO can capture them, so they are recovered lexically (same narrow
            // grammar discipline as ExtractExplicitUniformLocations).
            UnorderedMap<String, Uint> ExtractExplicitOpaqueBindings(const String& source);
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
