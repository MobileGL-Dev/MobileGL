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
            };
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
