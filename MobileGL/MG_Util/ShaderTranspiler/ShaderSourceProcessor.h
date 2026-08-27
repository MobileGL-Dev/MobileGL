// MobileGL - MobileGL/MG_Util/ShaderTranspiler/ShaderSourceProcessor.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <set>

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

            // The "#define <EXT> 1" lines an ES-profile source needs restored after
            // PreprocessShaderSource rewrote its #version to desktop, or "" for every other source.
            //
            // glslang defines the OES/AEP extension macros only in its ES preamble
            // (TParseVersions::getPreamble), selected by the profile it deduces from the directive -
            // so the rewrite silently takes them away and the shader's own
            // "#if !GL_OES_sample_variables" guard flips. They cannot simply be written into the
            // shader text: "#define GL_..." is a hard error for every application-supplied string
            // (TParseContext::reservedPpErrorCheck). They therefore go into glslang's CUSTOM
            // PREAMBLE, which sits at string index -1 and is exempt from that check by the same
            // gate that exempts glslang's own preamble - hence a separate function called by the
            // compiler rather than another injection pass.
            //
            // Deliberately narrow: only extensions the source itself NAMES in an #extension
            // directive, and never GL_ES or GL_FRAGMENT_PRECISION_HIGH. The shader really is being
            // compiled as desktop now, and flipping "#ifdef GL_ES" branches would break far more
            // than it fixes - which is why this is a partial fix by construction. The clean
            // long-term fix is to stop rewriting ES sources to desktop at all; the comment in
            // GetNormalizedVersionDirective records why that has not happened.
            String CollectEsPreambleMacroDefines(const String& preprocessedSource);

            // GLSL reserves a few names glslang happily accepts as identifiers ("packed",
            // "row_major" outside a layout(...) list, the image*Shadow family). Returns the
            // compile-error text for the first violation, or nullopt for a clean source.
            std::optional<String> FindReservedIdentifierViolation(const String& source);

            // NO SIDE-CHANNEL EXTRACTORS LIVE HERE ANY MORE. Three of them did - explicit
            // default-block uniform locations, explicit sampler/image bindings, and the storage
            // blocks that declared no binding - each recovering something MobileGL's
            // Vulkan-client relaxed parse destroys. All three are now taken from glslang at the
            // point of destruction instead:
            //   * uniform locations: a snapshot inside vkRelaxedRemapUniformVariable, read back
            //     through CollectExplicitUniformLocations (ShaderCompiler.h);
            //   * opaque bindings and unqualified storage blocks:
            //     TMglGlslIoResolver::reserverResourceSlot, which mapIO calls while the
            //     qualifier still says what the shader declared.
            // The rewrites below stay lexical by construction - they exist to make glslang
            // ACCEPT input it would otherwise reject, so they cannot be built on its parse.

            // A shader storage block whose layout(binding = N) reaches or passes
            // GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS is a compile-time error in GL 4.3 core 4.4.5,
            // and an arrayed block instance takes CONSECUTIVE points, so the last element is what
            // has to fit. Returns the compile-error text for the first violation, or nullopt for
            // a clean source. `maxBindings` is what glGetIntegerv answers for that pname; a
            // non-positive value means "nothing to check against" and every declaration passes.
            //
            // THE ONE SCAN THAT COULD NOT MOVE TO GLSLANG, and the reason is structural rather
            // than a matter of where the check is written. glslang has no resource limit for this
            // ceiling at all - Include/ResourceLimits.h carries maxAtomicCounterBindings,
            // maxCombinedTextureImageUnits and forty others, but nothing for uniform-block or
            // storage-block binding points - so there is no number for a parse-time check to
            // compare against, and the relaxed Vulkan rules MobileGL parses under would exempt it
            // anyway (ParseHelper.cpp layoutTypeCheck gates its binding ceilings on
            // `spvVersion.vulkan == 0`). Reading the AST post-parse from MobileGL is possible and
            // would be strictly better - a macro-spelled binding would finally be checked - but
            // the limit is a per-device number that CompileEnv deliberately keeps OUT of
            // frontendFingerprint (see its classification), so the L1c parse-verdict key would
            // have to grow it before any such verdict could be memoized. That is a cache-key
            // change in exchange for a new REJECTION surface, which is the one direction that
            // cannot be validated without device time.
            //
            // Consequence, and it is deliberate: a binding this scanner cannot read as a literal
            // is not judged. Under-rejection, never over-rejection.
            std::optional<String> FindShaderStorageBindingViolation(const String& source, Int maxBindings);
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
