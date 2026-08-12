// MobileGL - MobileGL/MG_Util/ShaderTranspiler/ShaderCompiler.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include "SpvcSession.h"
#include "glslang/TVarEntryInfo.h"
#include "glslang/TMglGlslIoResolver.h"

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            class ShaderCompiler {
            public:
                static Result<SharedPtr<glslang::TShader>> CompileShader(const ShaderAttrib& attrib);
                static Result<SharedPtr<glslang::TProgram>> LinkProgram(const ProgramAttrib& attrib);
                static Result<Vector<Vector<unsigned>>> GetSpirvBinaryFromProgram(const ProgramBinaryAttrib& attrib);
                static bool SanitizeAndOptimizeBinary(const Vector<Uint32>& inputBinary,
                                                      Vector<uint32_t>& outputBinary);
                // Demotes DrawIndex/BaseInstance/BaseVertex builtins to plain Private globals
                // (mg_DrawID/mg_BaseInstance/mg_BaseVertex) so SPIRV-Cross can emit ESSL.
                // Only for backends without native draw-parameter support (DirectGLES).
                static bool LowerDrawParametersForEssl(const Vector<Uint32>& inputBinary,
                                                       Vector<uint32_t>& outputBinary);
                // Drops RelaxedPrecision member decorations from uniform-block structs so
                // SPIRV-Cross prints the same (highp) member precision in every stage; ES
                // drivers reject cross-stage uniform blocks whose member precisions differ.
                // Only for the DirectGLES transpile path.
                static bool StripUboMemberRelaxedPrecisionForEssl(const Vector<Uint32>& inputBinary,
                                                                  Vector<uint32_t>& outputBinary);
                // Removes NoPerspective decorations so SPIRV-Cross emits plain (smooth) ESSL varyings.
                // DirectGLES fallback only, for devices lacking GL_NV_shader_noperspective_interpolation
                // (SPIRV-Cross would otherwise require that extension and the driver would reject it).
                static bool StripNoPerspectiveForEssl(const Vector<Uint32>& inputBinary,
                                                      Vector<uint32_t>& outputBinary);
                // Emulates noperspective (screen-linear) interpolation via gl_Position.w / gl_FragCoord.w
                // so no NV extension is needed; strips what it cannot emulate. DirectGLES fallback for
                // devices lacking GL_NV_shader_noperspective_interpolation. See EmulateNoPerspectivePass.
                static bool EmulateNoPerspectiveForEssl(const Vector<Uint32>& inputBinary,
                                                        Vector<uint32_t>& outputBinary);
                // Makes every index into a fragment-output array a constant integral
                // expression, which is what GLSL ES requires and SPIR-V does not. Runs the
                // stock folding chain first (loop unrolling folds the loop-derived indices
                // real shaders use), and lowers whatever is left - a genuinely dynamic index -
                // to a switch over the array's range. DirectGLES transpile path only: the
                // original module is legal for Vulkan, and no other stage is constrained this
                // way. Copies the input through untouched when no fragment output is indexed
                // dynamically, which is every shader but a handful.
                // See LegalizeFragmentOutputIndexPass.
                static bool LegalizeFragmentOutputIndexingForEssl(const Vector<Uint32>& inputBinary,
                                                                  Vector<uint32_t>& outputBinary);
                // Rebases loads of the InstanceIndex builtin to (InstanceIndex - BaseInstance) so
                // shaders see GL's zero-based gl_InstanceID. Vertex shaders only; DirectVulkan
                // backend only (glslang's relaxed mode aliases gl_InstanceID to gl_InstanceIndex,
                // which wrongly includes baseInstance).
                // GL_TEXTURE_RECTANGLE emulated on a plain 2D texture, for every backend:
                // divides the coordinate of each normalized-coordinate lookup by the texture
                // size and rewrites the image type to 2D. See NormalizeRectCoordinatesPass for
                // what it declines and why.
                static bool LowerRectImages(const Vector<Uint32>& inputBinary, Vector<uint32_t>& outputBinary);
                static bool RebaseInstanceIndexForVulkan(const Vector<Uint32>& inputBinary,
                                                         Vector<uint32_t>& outputBinary);
                // Re-declares 64-bit float vertex inputs as their 32-bit unsigned word pair
                // (double -> uvec2, dvec2 -> uvec4) and bitcasts them back to double at entry, so no
                // VK_FORMAT_R64*_SFLOAT is needed - lavapipe advertises none of them for vertex
                // buffers. Vertex stage, DirectVulkan only; pairs with the Float64 case in
                // VertexInputStateFactory::ToVkVertexFormat.
                static bool PackDoubleVertexInputsForVulkan(const Vector<Uint32>& inputBinary,
                                                            Vector<uint32_t>& outputBinary);
                // Adds the Invariant decoration to every Position builtin output. GL apps
                // routinely rely on cross-program position invariance for multi-pass
                // equality depth tests (e.g. GEQUAL re-draws of the same geometry), and
                // mobile drivers that optimize per-pipeline break that without the
                // decoration. DirectVulkan only.
                static bool DecoratePositionInvariantForVulkan(const Vector<Uint32>& inputBinary,
                                                               Vector<uint32_t>& outputBinary);
                // Replaces the declared format of float storage images with Unknown and adds the
                // matching SPIR-V capabilities. DirectVulkan uses this only when both Vulkan
                // shaderStorageImage*WithoutFormat features are enabled, allowing the
                // glBindImageTexture format to select the descriptor view at runtime. Integer
                // storage images deliberately keep their declared format for GL-compatible bit
                // reinterpretation paths (for example, R32F storage accessed as r32ui).
                static bool UseUnformattedFloatStorageImagesForVulkan(
                    const Vector<Uint32>& inputBinary, Vector<uint32_t>& outputBinary);
                static Result<String> DecompileShader(SpvcSession& session);

                // Parses one trivial shader in each configuration the production path can
                // reach, on the calling thread, so the built-in symbol tables those
                // configurations need are already cached before any worker asks for one.
                //
                // Why it matters: glslang builds a built-in TSymbolTable per distinct
                // (version, spvVersion, profile, source) combination, and does it under a
                // process-wide lock held for the whole build. Without this, the first
                // parallel compiles of a shaderpack load all pile up behind that lock and
                // show no speedup at all - which is easy to misread as asynchronous
                // compilation not working. Call once, from the GL thread, right after
                // glslang::InitializeProcess(). Idempotent and cheap on repeat.
                //
                // Only worth its cost when compiles can actually run in parallel, so the GL
                // frontend calls it only when asynchronous compilation is enabled: a
                // synchronous build would pay for three throwaway parses at every
                // eglInitialize to prewarm tables the first real compile builds anyway.
                static void PrewarmBuiltins();
                // Clears the "already prewarmed" latch. MUST be called wherever
                // glslang::FinalizeProcess() is, and for the same reason: finalizing deletes
                // the cached built-in tables the latch is asserting the existence of. Without
                // it, the second eglInitialize of a process comes back up unwarmed and with
                // no way left to warm it.
                static void ResetPrewarmLatch();

                // Test-environment SPIR-V validation. When enabled, every Optimizer wrapper
                // in this file validates its OUTPUT binary - the bytes a driver can actually
                // receive - and a failure logs the VUID (via MGLOG_I; see the consumer for
                // why not MGLOG_E) and bumps the failure latch below WITHOUT changing the
                // wrapper's return value: control flow must stay identical between the
                // validating and shipping configurations, or fail-open call sites would make
                // the two render differently. Resolved lazily from MOBILEGL_VALIDATE_SPIRV;
                // defaults on for desktop/CI/WSL builds and off for device (__ANDROID__)
                // builds. The setter wins over the environment and is safe to call from test
                // fixtures at any time.
                static bool SpirvValidationEnabled();
                static void SetSpirvValidationEnabled(bool enabled);

                // The test-lane enforcement signal: total validation failures observed this
                // process. Tests snapshot it, run the operation under scrutiny, and assert
                // on the delta. NoteSpirvValidationFailure is for validation done outside
                // this file (ProgramFactory::ValidateTransformedSpirv); it returns the new
                // total.
                static Uint64 SpirvValidationFailureCount();
                static Uint64 NoteSpirvValidationFailure();
            };
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
