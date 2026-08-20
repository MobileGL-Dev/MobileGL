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

#include <set>

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            class ShaderCompiler {
            public:
                static Result<SharedPtr<glslang::TShader>> CompileShader(const ShaderAttrib& attrib);
                static Result<SharedPtr<glslang::TProgram>> LinkProgram(const ProgramAttrib& attrib);
                static Result<Vector<Vector<unsigned>>> GetSpirvBinaryFromProgram(const ProgramBinaryAttrib& attrib);
                static bool SanitizeAndOptimizeBinary(const Vector<Uint32>& inputBinary,
                                                      Vector<uint32_t>& outputBinary,
                                                      bool validateOutput = true,
                                                      bool enableSpirvValidation = false);
                // Demotes DrawIndex/BaseInstance/BaseVertex builtins to plain Private globals
                // (mg_DrawID/mg_BaseInstance/mg_BaseVertex) so SPIRV-Cross can emit ESSL.
                // Only for backends without native draw-parameter support (DirectGLES).
                static bool LowerDrawParametersForEssl(const Vector<Uint32>& inputBinary,
                                                       Vector<uint32_t>& outputBinary,
                                                       bool enableSpirvValidation = false);
                // Demotes the gl_ViewportIndex OUTPUT builtin to a plain Private global named
                // mg_ViewportIndex, so SPIRV-Cross emits an ordinary declaration instead of a bare
                // gl_ViewportIndex that ESSL has no core spelling for. Multi-viewport routing is
                // lost (everything lands in viewport 0) but the stage compiles and the program
                // runs, instead of every draw made with it becoming a silent no-op. Only for the
                // DirectGLES transpile path on a driver WITHOUT GL_OES_viewport_array; gl_Layer is
                // deliberately left alone, being core in ESSL 3.20 geometry shaders.
                static bool LowerViewportIndexForEssl(const Vector<Uint32>& inputBinary,
                                                      Vector<uint32_t>& outputBinary,
                                                      bool enableSpirvValidation = false);
                // Whether the module declares an output decorated BuiltIn ViewportIndex, i.e.
                // whether the pass above has anything to do. The gate that keeps every other
                // stage off an optimizer round trip it does not need.
                static bool DeclaresViewportIndexBuiltin(const Vector<Uint32>& binary);
                // Clamps the Sample image-operand of every multisample fetch to the sample count
                // the BACKEND can really deliver for that image's category, which on Adreno and
                // Mali is 1 for integer formats while the frontend advertises the GL-mandated
                // floor of 4. Without it a `texelFetch(usampler2DMS, coord, 3)` reads past the
                // end of a one-sample allocation. Pass the backend-real per-category ceilings and
                // the advertised maximum (GL_Getter's GetAdvertisedMaxSamples); a category that
                // already reaches the advertised value is left alone. DirectGLES transpile path
                // only. See ClampMultisampleFetchPass.
                static bool ClampMultisampleFetchesForEssl(const Vector<Uint32>& inputBinary,
                                                           Vector<uint32_t>& outputBinary,
                                                           Int32 maxColorSamples,
                                                           Int32 maxIntegerSamples,
                                                           Int32 maxDepthSamples,
                                                           Int32 advertisedMaxSamples,
                                                           bool enableSpirvValidation = false);
                // Whether the module declares any multisampled image type, i.e. whether the pass
                // above has anything to do. The gate that keeps every other stage off an
                // optimizer round trip it does not need.
                static bool DeclaresMultisampledImage(const Vector<Uint32>& binary);
                // Replaces an ARRAY vertex input with one input per element at consecutive
                // locations, seeding a Private copy of the array so indexed reads still work.
                // GLSL ES has no array vertex inputs and SPIRV-Cross refuses the whole module
                // rather than emulating them, so without this the stage never reaches the
                // driver. Only for the DirectGLES transpile path.
                static bool SplitArrayVertexInputsForEssl(const Vector<Uint32>& inputBinary,
                                                          Vector<uint32_t>& outputBinary,
                                                          bool enableSpirvValidation = false);
                // Replaces the named interface BLOCKS with one variable per member, named
                // "<Block>_<member>", shadowing the block itself so the body is untouched. The
                // Adreno ES driver silently captures NOTHING for a transform-feedback varying
                // named as a block member, so a capture list that names one has to be respelled
                // - and the declaration with it. `flattenedBlockNames` reports which blocks
                // this stage actually rewrote, which is what the capture list must follow.
                // Only for the DirectGLES transpile path.
                static bool FlattenXfbInterfaceBlocksForEssl(const Vector<Uint32>& inputBinary,
                                                             const std::set<String>& blockNames,
                                                             std::set<String>& flattenedBlockNames,
                                                             Vector<uint32_t>& outputBinary,
                                                             bool enableSpirvValidation = false);
                // The capture request "StageData.attrib[0]" as the pass above renamed it,
                // "StageData_attrib[0]", or false when it does not name a member of a block
                // that was flattened.
                static bool RewriteXfbCaptureNameForFlattenedBlock(const String& captureName,
                                                                   const std::set<String>& flattenedBlockNames,
                                                                   String& outName);
                // Drops RelaxedPrecision member decorations from uniform-block structs so
                // SPIRV-Cross prints the same (highp) member precision in every stage; ES
                // drivers reject cross-stage uniform blocks whose member precisions differ.
                // Only for the DirectGLES transpile path.
                static bool StripUboMemberRelaxedPrecisionForEssl(const Vector<Uint32>& inputBinary,
                                                                  Vector<uint32_t>& outputBinary,
                                                             bool enableSpirvValidation = false);
                // Removes NoPerspective decorations so SPIRV-Cross emits plain (smooth) ESSL varyings.
                // DirectGLES fallback only, for devices lacking GL_NV_shader_noperspective_interpolation
                // (SPIRV-Cross would otherwise require that extension and the driver would reject it).
                static bool StripNoPerspectiveForEssl(const Vector<Uint32>& inputBinary,
                                                      Vector<uint32_t>& outputBinary,
                                                      bool enableSpirvValidation = false);
                // Emulates noperspective (screen-linear) interpolation via gl_Position.w / gl_FragCoord.w
                // so no NV extension is needed; strips what it cannot emulate. DirectGLES fallback for
                // devices lacking GL_NV_shader_noperspective_interpolation. See EmulateNoPerspectivePass.
                static bool EmulateNoPerspectiveForEssl(const Vector<Uint32>& inputBinary,
                                                        Vector<uint32_t>& outputBinary,
                                                      bool enableSpirvValidation = false);
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
                                                                  Vector<uint32_t>& outputBinary,
                                                             bool enableSpirvValidation = false);
                // Rebases loads of the InstanceIndex builtin to (InstanceIndex - BaseInstance) so
                // shaders see GL's zero-based gl_InstanceID. Vertex shaders only; DirectVulkan
                // backend only (glslang's relaxed mode aliases gl_InstanceID to gl_InstanceIndex,
                // which wrongly includes baseInstance).
                // GL_TEXTURE_RECTANGLE emulated on a plain 2D texture, for every backend:
                // divides the coordinate of each normalized-coordinate lookup by the texture
                // size and rewrites the image type to 2D. See NormalizeRectCoordinatesPass for
                // what it declines and why.
                static bool LowerRectImages(const Vector<Uint32>& inputBinary, Vector<uint32_t>& outputBinary,
                                            bool enableSpirvValidation = false);
                // GL_TEXTURE_1D_ARRAY storage images rewritten to the 2D-array shape the texture
                // is actually stored in on ES, with the layer moved from the coordinate's second
                // component to its third. DirectGLES transpile path only - Vulkan binds a real
                // VK_IMAGE_VIEW_TYPE_1D_ARRAY and must see the module unchanged. Copies the input
                // through untouched when the module declares no such image, which is every shader
                // but a handful. See Lower1DArrayImagesPass for what it declines and why.
                static bool Lower1DArrayImagesForEssl(const Vector<Uint32>& inputBinary,
                                                      Vector<uint32_t>& outputBinary,
                                                      bool enableSpirvValidation = false);
                // Gives each format-less storage image the format bound to its image unit, so
                // the emitted ESSL can carry the format layout qualifier GLSL ES requires of
                // every image and desktop GLSL lets a writeonly declaration omit. `glFormatByName`
                // maps uniform name to the glBindImageTexture format of the unit it addresses.
                // DirectGLES transpile path only - Vulkan takes an Unknown-format storage image
                // natively. See BakeImageFormatsPass for what it declines and why.
                static bool BakeImageFormatsForEssl(const Vector<Uint32>& inputBinary,
                                                    const UnorderedMap<String, Uint>& glFormatByName,
                                                    Vector<uint32_t>& outputBinary,
                                                    bool enableSpirvValidation = false);
                // Whether the module declares a storage image with no format qualifier at all,
                // i.e. whether BakeImageFormatsForEssl could change anything. One module parse,
                // so the ~every shader that declares none pays no optimizer run.
                static bool DeclaresFormatlessStorageImage(const Vector<Uint32>& binary);
                // Whether the GL internal format's image-format spelling is one GLSL ES has in
                // core. False both for a format ES only reaches through GL_NV_image_formats and
                // for one with no image-format spelling at all, so a caller that has to decide
                // whether to emit the extension directive can ask this one question.
                static bool GLInternalFormatIsCoreEsslImageFormat(Uint glInternalFormat);
                // The ESSL layout-qualifier spelling of a GL internal format ("r8ui", "rgba32f"),
                // empty when the format has no image-format spelling at all.
                static String EsslImageFormatSpelling(Uint glInternalFormat);
                // Whether SPIRV-Cross will print that format when it targets ESSL. It throws on
                // the ones it calls desktop-only - taking the whole stage with it - so a caller
                // must not ask BakeImageFormatsForEssl for those, and completes them in the
                // emitted text instead.
                static bool SpirvCrossCanPrintEsslImageFormat(Uint glInternalFormat);
                static bool RebaseInstanceIndexForVulkan(const Vector<Uint32>& inputBinary,
                                                         Vector<uint32_t>& outputBinary,
                                                      bool enableSpirvValidation = false);
                // Builds the non-indexed-draw variant of a vertex shader: every gl_BaseVertex
                // read becomes zero, which is what GL defines for a command carrying no
                // baseVertex parameter while Vulkan's builtin would report firstVertex.
                // See ZeroBaseVertexPass.
                static bool ZeroBaseVertexForVulkan(const Vector<Uint32>& inputBinary,
                                                    Vector<uint32_t>& outputBinary,
                                                    bool enableSpirvValidation = false);
                // Replaces compute gl_NumSubgroups loads with ceil(workgroup invocations /
                // gl_SubgroupSize). DirectVulkan only; this repairs drivers whose builtin
                // disagrees with the subgroup IDs the same dispatch emits (Adreno reports 1
                // while emitting IDs 0..7). The ceil() partition is only spec-guaranteed
                // under VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT, which
                // the caller requests whenever it is legal for the workgroup shape; see
                // DeriveNumSubgroupsPass.
                static bool DeriveNumSubgroupsForVulkan(const Vector<Uint32>& inputBinary,
                                                        Vector<uint32_t>& outputBinary,
                                                        bool enableSpirvValidation = false);
                // Lowers every GL_KHR_shader_subgroup construct in a compute module onto a
                // 32-lane virtual subgroup built from workgroup-shared memory. Last-resort
                // path for devices with NO native subgroup support, opt-in via
                // MOBILEGL_MAGMA_EMULATE_SUBGROUP=1; a device with native subgroup
                // operations always uses them. maxWorkgroupScratchBytes bounds the shared
                // scratch the lowering may add (pass the device's
                // maxComputeSharedMemorySize; 0 falls back to the 16384-byte Vulkan
                // minimum). See EmulateSubgroupsPass.
                static bool EmulateSubgroupsForVulkan(const Vector<Uint32>& inputBinary,
                                                      Vector<uint32_t>& outputBinary,
                                                      Uint32 maxWorkgroupScratchBytes,
                                                      bool enableSpirvValidation = false);
                // Grows iterationRP's under-declared gl_SubgroupID-indexed scratch to the
                // subgroup count the device actually partitions into, fingerprint-gated to
                // that pack's reduction idiom; every other module - and every device whose
                // width the pack already assumed - passes through byte-identical.
                // maxWorkgroupScratchBytes bounds the growth (pass the device's
                // maxComputeSharedMemorySize; 0 falls back to the 16384-byte Vulkan
                // minimum). See FixIterationRPSubgroupScratchPass.
                static bool FixIterationRPSubgroupScratchForVulkan(const Vector<Uint32>& inputBinary,
                                                                   Vector<uint32_t>& outputBinary,
                                                                   Uint32 nativeSubgroupSize,
                                                                   Uint32 maxWorkgroupScratchBytes,
                                                                   bool enableSpirvValidation = false);
                // Inserts the missing workgroup rendezvous between Program 203's two
                // prefixSumCache reductions. Fingerprint-gated to the iterationRP shape;
                // unrelated and already-repaired modules pass through byte-identical.
                static bool FixIterationRPBarrierForVulkan(const Vector<Uint32>& inputBinary,
                                                           Vector<uint32_t>& outputBinary,
                                                           bool enableSpirvValidation = false);
                // Re-declares 64-bit float vertex inputs as their 32-bit unsigned word pair
                // (double -> uvec2, dvec2 -> uvec4) and bitcasts them back to double at entry, so no
                // VK_FORMAT_R64*_SFLOAT is needed - lavapipe advertises none of them for vertex
                // buffers. Vertex stage, DirectVulkan only; pairs with the Float64 case in
                // VertexInputStateFactory::ToVkVertexFormat.
                static bool PackDoubleVertexInputsForVulkan(const Vector<Uint32>& inputBinary,
                                                            Vector<uint32_t>& outputBinary,
                                                      bool enableSpirvValidation = false);
                // Adds the Invariant decoration to every Position builtin output. GL apps
                // routinely rely on cross-program position invariance for multi-pass
                // equality depth tests (e.g. GEQUAL re-draws of the same geometry), and
                // mobile drivers that optimize per-pipeline break that without the
                // decoration. DirectVulkan only.
                static bool DecoratePositionInvariantForVulkan(const Vector<Uint32>& inputBinary,
                                                               Vector<uint32_t>& outputBinary,
                                                             bool enableSpirvValidation = false);
                // Replaces the declared format of float storage images with Unknown and adds the
                // matching SPIR-V capabilities. DirectVulkan uses this only when both Vulkan
                // shaderStorageImage*WithoutFormat features are enabled, allowing the
                // glBindImageTexture format to select the descriptor view at runtime. Integer
                // storage images deliberately keep their declared format for GL-compatible bit
                // reinterpretation paths (for example, R32F storage accessed as r32ui).
                static bool UseUnformattedFloatStorageImagesForVulkan(
                    const Vector<Uint32>& inputBinary, Vector<uint32_t>& outputBinary,
                    bool enableSpirvValidation = false);
                // Rewrites every 64-bit float in the module to a 32-bit one, preserving every
                // block offset and stride exactly (see DemoteFloat64Pass). Already part of
                // SanitizeAndOptimizeBinary, which is where production reaches it; exposed
                // separately so a test can drive the demotion on its own.
                static bool DemoteFloat64ToFloat32(const Vector<Uint32>& inputBinary,
                                                   Vector<uint32_t>& outputBinary,
                                                   bool enableSpirvValidation = false);
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

                // Validation is an explicit immutable option of each compiler operation. The
                // program-link task snapshots MOBILEGL_ENABLE_SPIRV_VALIDATION before it can run
                // on a worker; standalone callers pass true directly. A failure logs the VUID and
                // bumps the latch below WITHOUT changing a wrapper's return value, so validating
                // and shipping configurations preserve identical rendering control flow.

                // Makes validator table lifetime safe before an external final-module validator
                // runs. This has no configuration state; callers invoke it only for an enabled
                // task-local validation option.
                static void PrepareSpirvValidation();

                // The test-lane enforcement signal: total validation failures observed this
                // process. Tests snapshot it, run the operation under scrutiny, and assert
                // on the delta. NoteSpirvValidationFailure is for validation done outside
                // this file (ProgramFactory::ValidateTransformedSpirv); it returns the new
                // total.
                static Uint64 SpirvValidationFailureCount();
                static Uint64 NoteSpirvValidationFailure();

                // True when the module declares any buffer-backed image type - an OpTypeImage with
                // Dim = Buffer. That is the samplerBuffer / isamplerBuffer / usamplerBuffer
                // family and equally the imageBuffer / iimageBuffer / uimageBuffer one: SPIRV-Cross
                // requires GL_EXT_texture_buffer for both, from the same branch, so both are
                // uncompilable on a driver without buffer textures and both belong here.
                // DirectGLES asks before handing the transpiled ESSL to the driver: buffer
                // textures are core in the OpenGL 3.1+ context MobileGL advertises but need
                // ES 3.2 or EXT/OES_texture_buffer on the host, and on a driver without them
                // SPIRV-Cross's `#extension ... : require` makes the shader uncompilable. The
                // check exists so that failure can be reported as the missing capability it is,
                // naming the shader, rather than as a driver info log nobody sees.
                static Bool ModuleDeclaresBufferTextureSampler(const Vector<Uint32>& spirv);

                // True when the module still declares a 64-bit float type. After
                // SanitizeAndOptimizeBinary that can only mean DemoteFloat64Pass declined the
                // module (see its header for the two operations that make it decline), which is
                // what the backends report: no mobile driver can build such a module.
                static Bool ModuleDeclaresFloat64(const Vector<Uint32>& spirv);
            };
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
