// MobileGL - MobileGL/BackendCaps.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <MG_State/GLState/TextureState/TextureEnum.h>

// The vocabulary every layer uses to talk about a backend's capabilities: which backend it is,
// what each texture format supports, and the probed limits and features. It holds values only;
// the backend itself is MG_Backend/BackendObject.h.
namespace MobileGL {
    enum class BackendType {
        DirectGLES,
        DirectVulkan,
        BackendTypeCount,
        Unknown = -1
    };

    namespace MG_Backend {
        enum class FormatCapability : Uint64 {
            Creatable = 1ull << 0,

            Sampled = 1ull << 1,
            LinearFilter = 1ull << 2,
            GenerateMipmap = 1ull << 3,
            TextureGather = 1ull << 4,
            TextureShadow = 1ull << 5,

            FramebufferRenderable = 1ull << 6,
            FramebufferLayered = 1ull << 7,
            MultisampleTexture = 1ull << 8,
            MultisampleRenderbuffer = 1ull << 9,

            ColorAttachment = 1ull << 10,
            DepthAttachment = 1ull << 11,
            StencilAttachment = 1ull << 12,

            TextureBuffer = 1ull << 13
        };

        using FormatCapabilityFlags = Flags<FormatCapability>;

        inline constexpr Array<FormatCapability, 14> kReportedFormatCapabilities = {
            FormatCapability::Creatable,
            FormatCapability::Sampled,
            FormatCapability::LinearFilter,
            FormatCapability::GenerateMipmap,
            FormatCapability::TextureGather,
            FormatCapability::TextureShadow,
            FormatCapability::FramebufferRenderable,
            FormatCapability::FramebufferLayered,
            FormatCapability::MultisampleTexture,
            FormatCapability::MultisampleRenderbuffer,
            FormatCapability::ColorAttachment,
            FormatCapability::DepthAttachment,
            FormatCapability::StencilAttachment,
            FormatCapability::TextureBuffer,
        };

        inline constexpr SizeT kFormatCapabilityTextureTargetCount =
            static_cast<SizeT>(TextureTarget::TextureTargetCount);
        inline constexpr SizeT kFormatCapabilityRenderbufferTargetIndex = kFormatCapabilityTextureTargetCount;
        inline constexpr SizeT kFormatCapabilityTargetCount = kFormatCapabilityTextureTargetCount + 1;
        inline constexpr SizeT kFormatCapabilityFormatCount =
            static_cast<SizeT>(TextureInternalFormat::TextureInternalFormatCount);

        using FormatCapabilityTable =
            Array<Array<FormatCapabilityFlags, kFormatCapabilityFormatCount>, kFormatCapabilityTargetCount>;
        using FormatSampleCountTable =
            Array<Array<Vector<Int>, kFormatCapabilityFormatCount>, kFormatCapabilityTargetCount>;

        struct FormatCapabilityCache {
            FormatCapabilityTable FullCaps{};
            FormatCapabilityTable CaveatCaps{};
            FormatSampleCountTable SampleCounts{};

            void Clear();
        };

        Bool HasFormatCapability(FormatCapabilityFlags caps, FormatCapability capability);
        SizeT GetFormatCapabilityTargetIndex(TextureTarget target);
        SizeT GetRenderbufferFormatCapabilityTargetIndex();
        const char* GetFormatCapabilityName(FormatCapability capability);
        String GetFormatCapabilityTargetName(SizeT targetIndex);
        void PrintFormatCapabilities(const FormatCapabilityCache& cache);

        // Device features that decide whether an extension string may be advertised, beyond the
        // per-backend positional flags of BuildAdvertisedExtensions. Each is the PROBED driver
        // capability (an ES version / extension on DirectGLES, a VkPhysicalDeviceFeatures bit on
        // DirectVulkan), never a vendor guess. Defaults describe the no-device case.
        struct AdvertisedFeatureSupport {
            // ES 3.2 or GL_EXT_tessellation_shader; VkPhysicalDeviceFeatures::tessellationShader.
            Bool TessellationShader = false;
            // At least one cull distance: GL_EXT_clip_cull_distance with a non-zero
            // GL_MAX_CULL_DISTANCES; VkPhysicalDeviceFeatures::shaderCullDistance with a non-zero
            // maxCullDistances.
            Bool CullDistance = false;
            // GL_EXT_clip_control on DirectGLES. DirectVulkan applies clip control in its own
            // position fixup and does not read this.
            Bool ClipControl = false;
        };

        // Coarse GPU vendor identity for gating device-specific quirks. Detected from the
        // Vulkan physical-device vendorID or the GLES GL_VENDOR/GL_RENDERER strings; stays
        // Unknown when detection is inconclusive, in which case auto-gated quirks stay off.
        enum class GpuVendorKind : Uint8 {
            Unknown = 0,
            Qualcomm,
            Arm,
            Nvidia,
            Amd,
            Intel,
            ImgTec,
            // Software rasterizers (llvmpipe/lavapipe, SwiftShader).
            Software,
        };

        struct DynamicBackendParameters {
            Uint64 UniformBufferOffsetAlignment = 256;
            // GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, which is a SEPARATE limit from the
            // uniform one and is routinely larger: Adreno 830 reports 32 for uniform buffers and
            // 64 for storage buffers. Answering the storage query with the uniform value let an
            // application bind a storage range at an offset the driver cannot address, which it
            // accepted without error and then wrote somewhere else entirely.
            Uint64 ShaderStorageBufferOffsetAlignment = 256;
            // GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT. 1.0 means the backend cannot filter anisotropically,
            // which is also why the extension is not advertised in that case.
            Float MaxTextureMaxAnisotropy = 1.0f;
            Float AliasedLineWidthRangeMin = 1.0f;
            Float AliasedLineWidthRangeMax = 1.0f;
            Float SmoothLineWidthRangeMin = 1.0f;
            Float SmoothLineWidthRangeMax = 1.0f;
            Float SmoothLineWidthGranularity = 1.0f;
            Float PointSizeRangeMin = 1.0f;
            Float PointSizeRangeMax = 1.0f;
            Float PointSizeGranularity = 1.0f;
            Int Max3DTextureSize = 16384;
            Int MaxArrayTextureLayers = 2048;
            Int MaxCubeMapTextureSize = 16384;
            Int MaxFramebufferWidth = 16384;
            Int MaxFramebufferHeight = 16384;
            Int MaxFramebufferLayers = 2048;
            Int MaxRenderbufferSize = 16384;
            Int MaxTextureSize = 16384;
            Int MaxColorTextureSamples = 1;
            Int MaxDepthTextureSamples = 1;
            Int MaxFramebufferSamples = 1;
            Int MaxIntegerSamples = 1;
            Int MaxSamples = 1;
            Int MaxSampleMaskWords = 1;
            // Tessellation limits; defaults are the GL 4.0 core minimums.
            Int MaxPatchVertices = 32;
            Int MaxTessGenLevel = 64;
            // GL_MIN/MAX_PROGRAM_TEXTURE_GATHER_OFFSET. Defaults are the GL 4.0 core
            // minimums, which every ES 3.1 driver also guarantees.
            Int MinProgramTextureGatherOffset = -8;
            Int MaxProgramTextureGatherOffset = 7;
            Int MaxTextureImageUnits = 32;
            Int MaxVertexTextureImageUnits = 32;
            Int MaxComputeTextureImageUnits = 32;
            Int MaxCombinedTextureImageUnits = 192;
            Int MaxVertexAttribs = 16;
            Int MaxComputeShaderStorageBlocks = 8;
            Int MaxCombinedShaderStorageBlocks = 32;
            // Per-stage GL_MAX_*_SHADER_STORAGE_BLOCKS. Zero is a legal answer for the four
            // non-compute, non-fragment stages and these defaults are the spec minimums, not
            // placeholders: GL 4.6 table 23.64 and ES 3.2 table 21.44 both set the minimum for
            // vertex, tessellation control, tessellation evaluation and geometry at 0, and only
            // fragment (8 in GL, 4 in ES) and compute are guaranteed to have any. Every real ARM
            // GLES driver takes that allowance - a Mali-G925 reports 0 for all four - so a
            // backend that cannot honour a graphics-stage storage block MUST report 0 here
            // rather than a hopeful number. Advertising a non-zero count the driver will refuse
            // does not make the block work; it only moves the failure from an honest
            // "unsupported" at query time to a backend link error the frontend never surfaces,
            // after which every draw with that program silently renders nothing.
            Int MaxVertexShaderStorageBlocks = 0;
            Int MaxTessControlShaderStorageBlocks = 0;
            Int MaxTessEvaluationShaderStorageBlocks = 0;
            Int MaxGeometryShaderStorageBlocks = 0;
            Int MaxFragmentShaderStorageBlocks = 8;
            Int MaxComputeUniformBlocks = 12;
            Int MaxComputeWorkGroupInvocations = 128;
            // GL_MAX_COMPUTE_WORK_GROUP_COUNT / GL_MAX_COMPUTE_WORK_GROUP_SIZE, one value per
            // axis. These six, with the invocations limit above, are the only indexed limits a
            // backend genuinely OWNS - the device answers them (glGetIntegeri_v on DirectGLES,
            // VkPhysicalDeviceLimits::maxComputeWorkGroupCount/Size on DirectVulkan) - and so
            // the only ones that survive the retirement of the GetIntegeri_v table entry: they
            // cross the MGPipe boundary inside MGPCaps, by inclusion of this struct (plan B
            // section 4.4.1). Every other indexed pname names frontend state. RAW driver
            // answers, like the invocations limit: GL_Getter and the compile environment floor
            // them at the shared MIN_COMPUTE_WORK_GROUP_* minimums themselves. The defaults are
            // the GL 4.3 core minimums (table 23.60) and describe the no-backend case, as
            // MaxClipDistances' does.
            Int MaxComputeWorkGroupCount[3] = {65535, 65535, 65535};
            Int MaxComputeWorkGroupSize[3] = {1024, 1024, 64};
            Int MaxShaderStorageBufferBindings = 8;
            Int MaxTextureBufferSize = 65536;
            // GL_TEXTURE_BUFFER_OFFSET_ALIGNMENT; 1 means the offset is unconstrained.
            Int TextureBufferOffsetAlignment = 1;
            Int MaxUniformBufferBindings = 24;
            Int MaxUniformBlockSize = 16384;
            Int MaxImageUnits = 8;
            Int MaxCombinedImageUniforms = 8;
            Int MaxVertexImageUniforms = 0;
            Int MaxTessControlImageUniforms = 0;
            Int MaxTessEvaluationImageUniforms = 0;
            Int MaxGeometryImageUniforms = 0;
            Int MaxFragmentImageUniforms = 8;
            Int MaxComputeImageUniforms = 8;
            Int MaxDrawBuffers = 8;
            Int MaxColorAttachments = 8;
            // GL_MAX_CLIP_DISTANCES. Zero is a legal answer here, not a placeholder, and a
            // backend that cannot host a clip distance MUST report it: advertising eight the
            // backend will refuse does not make gl_ClipDistance work, it only moves the failure
            // from an honest "unsupported" at query time to a backend shader-compile error the
            // frontend never surfaces, after which every draw with that program silently renders
            // nothing. DirectGLES fills it from GL_EXT_clip_cull_distance, DirectVulkan from the
            // shaderClipDistance device feature. The DEFAULT stays at the GL 4.3 core minimum
            // because it describes the no-backend case (standalone shader compiles, unit tests),
            // where there is no device to be honest about and BuildTBuiltInResource still has to
            // hand glslang a workable gl_MaxClipDistances.
            Int MaxClipDistances = 8;
            // GL_MAX_CULL_DISTANCES and GL_MAX_COMBINED_CLIP_AND_CULL_DISTANCES, under exactly
            // the contract stated for MaxClipDistances above: ZERO IS A LEGAL ANSWER and a
            // backend that cannot host a cull distance MUST report it. The failure this prevents
            // is worse than the clip one, because cull distance discards the whole primitive:
            // glslang bounds gl_CullDistance[i] against maxCullDistances and expands
            // gl_MaxCullDistances from it, SPIRV-Cross then emits
            // `#extension GL_EXT_clip_cull_distance : require` into the ESSL, and a host driver
            // without that extension rejects the program in an info log nobody surfaces. These
            // used to be bare 8s inside BuildTBuiltInResource with no backend consulted at all.
            // The DEFAULTS are the GL 4.5 core minimums for the same reason MaxClipDistances'
            // is: they describe the no-backend case (standalone compiles, unit tests).
            Int MaxCullDistances = 8;
            Int MaxCombinedClipAndCullDistances = 8;
            Int MaxViewports = 16;
            // GL_LAYER_PROVOKING_VERTEX / GL_VIEWPORT_INDEX_PROVOKING_VERTEX: which vertex of a
            // primitive supplies gl_Layer and gl_ViewportIndex. GL 4.6 table 23.65 makes
            // GL_UNDEFINED_VERTEX a legal answer for both, and it is the honest default - naming
            // a convention is a statement about behaviour, so a backend that does not pin one
            // must not claim it does. DirectGLES fills the layer one from the ES 3.2 query and
            // the viewport one from GL_OES_viewport_array, and leaves UNDEFINED where the
            // capability is absent: without the viewport array extension only viewport 0 is ever
            // rasterized, so no convention selects anything. DirectVulkan keeps UNDEFINED for
            // both - which vertex provokes is decided per pipeline by
            // VulkanRenderer::SelectProvokingVertexMode out of VK_EXT_provoking_vertex,
            // provokingVertexModePerPipeline and the topology, so no single convention is true
            // of the backend.
            Uint32 LayerProvokingVertex = GL_UNDEFINED_VERTEX;
            Uint32 ViewportIndexProvokingVertex = GL_UNDEFINED_VERTEX;
            Int MaxViewportWidth = 16384;
            Int MaxViewportHeight = 16384;
            Float ViewportBoundsRangeMin = 0.0f;
            Float ViewportBoundsRangeMax = 0.0f;
            Int ViewportSubpixelBits = 0;
            // GL 4.x fragment-interpolation offset limits. These defaults are the
            // core minimums and are replaced by live GLES/Vulkan device limits.
            Float MinFragmentInterpolationOffset = -0.5f;
            // For four fractional bits the greatest required legal offset is
            // 0.5 - 2^-4 = 0.4375 (GL 4.6 table 23.70).
            Float MaxFragmentInterpolationOffset = 0.4375f;
            Int FragmentInterpolationOffsetBits = 4;
            Bool SupportsWideLines = false;
            // Whether a framebuffer whose depth and stencil attachments are distinct
            // images can be rendered to. GL only requires support when both refer to the
            // same image and lets an implementation answer GL_FRAMEBUFFER_UNSUPPORTED
            // otherwise, which is what DirectVulkan (one combined attachment) and the
            // real ES drivers behind DirectGLES both do. Defaults to true so a backend
            // that never sets it keeps the permissive behaviour.
            Bool SupportsDistinctDepthStencilAttachments = true;
            // Whether attaching a single layer of a 3D or array texture to a framebuffer actually
            // renders to that layer. DirectGLES hands the layer straight to
            // glFramebufferTextureLayer, so it does; DirectVulkan maps a GL layer onto a Vulkan
            // array layer with no notion of a 3D depth slice, so it does not yet. Defaults to false
            // so a backend that never sets it gets the conservative answer.
            // Which layered texture targets this backend can attach ONE layer of to a framebuffer
            // and then really clear, render and read back that layer. Bit (1u << TextureTarget) is
            // set for each supported target. Deliberately per target rather than one flag: the three
            // ways a GL layer maps onto Vulkan are independent capabilities. A 2D or 2D multisample
            // array layer IS a VkImage array layer and needs nothing extra; a 3D texture's layer is
            // a z slice, which needs a 2D-array-compatible image and a per-slice clear that
            // vkCmdClearColorImage cannot express; a cube map array needs an image shape and the
            // imageCubeArray feature before it can be attached at any layer at all. Defaults to 0 so
            // a backend that never sets it gets the conservative answer.
            Uint32 PerLayerFramebufferAttachmentTargets = 0;

            static constexpr Uint32 PerLayerFramebufferAttachmentBit(TextureTarget target) {
                return (static_cast<Int>(target) >= 0 &&
                        static_cast<Int>(target) < static_cast<Int>(TextureTarget::TextureTargetCount))
                           ? (1u << static_cast<Uint32>(target))
                           : 0u;
            }

            Bool SupportsPerLayerFramebufferAttachment(TextureTarget target) const {
                const Uint32 bit = PerLayerFramebufferAttachmentBit(target);
                return bit != 0 && (PerLayerFramebufferAttachmentTargets & bit) != 0;
            }
            // Whether this backend can CONSUME a shader module that still declares 64-bit floats,
            // i.e. whether `double` survives the transpile instead of being narrowed to `float`
            // (ShaderTranspiler::DemoteFloat64Pass). Detected, never assumed:
            //   * DirectVulkan sets it from VkPhysicalDeviceFeatures::shaderFloat64, the feature
            //     VUID-VkShaderModuleCreateInfo-pCode-08740 requires before a module declaring
            //     OpCapability Float64 may be created at all. lavapipe has it; Adreno and Mali
            //     both report VK_FALSE, so no real mobile device does.
            //   * DirectGLES can NEVER have it. GLSL ES has no 64-bit float type in any version
            //     or extension, so SPIRV-Cross cannot emit one ("FP64 not supported in ES
            //     profile") and the demotion there is mathematically mandatory, always.
            // Defaults to false so a backend that never sets it - and the no-backend case, which
            // is what standalone shader compiles and the unit tests run under - keeps the
            // demotion, which is the behaviour that works everywhere.
            Bool SupportsShaderFloat64 = false;
            // Whether glVertexAttribLFormat / glVertexArrayAttribLFormat can be honoured, i.e.
            // whether a 64-bit vertex attribute can actually reach a shader unconverted. Detected,
            // never assumed: DirectVulkan needs VkPhysicalDeviceFeatures::shaderFloat64 (the
            // attribute travels as its 32-bit word pair, so no VK_FORMAT_R64* is required, but the
            // bitcast result is Float64); DirectGLES can never have it, ESSL having no fp64 type at
            // all. Defaults to false so a backend that never sets it gets the conservative answer.
            //
            // INDEPENDENT of SupportsShaderFloat64, and it has to be: this flag decides a VkFormat
            // from the VAO ATTRIBUTE alone, which does not know what type the shader declared, and
            // glVertexAttribFormat(GL_DOUBLE) feeding a plain `in vec4` is both legal and common
            // (KHR-GL43.vertex_attrib_binding.basic-input-case4/5, advanced-bindingUpdate). A
            // backend with native fp64 that still cannot FETCH 64 bits keeps this false and relies
            // on the per-MODULE rule in ShaderCompiler::SanitizeAndOptimizeBinary instead: a vertex
            // module that declares a 64-bit float INPUT is demoted whole, so the two shader-side
            // halves (PackDoubleVertexInputsPass and VertexInputStateFactory::ToVkVertexFormat)
            // still see one consistent world.
            Bool SupportsFloat64VertexAttributes = false;
            // Whether a TESSELLATION stage of this backend may access gl_PointSize - i.e.
            // whether a module declaring OpCapability TessellationPointSize can reach the
            // driver at all. DirectVulkan sets both this and the geometry twin from the one
            // shaderTessellationAndGeometryPointSize feature; DirectGLES sets them
            // independently from the EXT/OES_tessellation_point_size /
            // geometry_point_size extension pairs (PointSizeTier), which really do come
            // separately. When absent, ProgramSpirvTask demotes the built-in to an ordinary
            // varying program-wide (ShaderCompiler::
            // DemoteTessellationGeometryPointSizeForProgram); MOBILEGL_POINT_SIZE_DEMOTION
            // overrides the detection in either direction at backend init.
            //
            // Defaults TRUE, deliberately against the house "assume absent" rule: false
            // ARMS a rewrite, so the conservative no-backend answer (standalone compiles,
            // unit tests) is the one that leaves modules untouched. A backend that never
            // sets it gets standard modules and, at worst, the old honest declines.
            Bool SupportsTessellationPointSize = true;
            // The geometry-stage twin (OpCapability GeometryPointSize).
            Bool SupportsGeometryPointSize = true;
            Uint64 MaxShaderStorageBlockSize = 128 * 1024 * 1024;
            Uint32 SubgroupSize = 0;
            Uint32 SubgroupSupportedStages = 0;
            Uint32 SubgroupSupportedFeatures = 0;
            // GL_SAMPLE_POSITION (GL 4.6 core 14.3.1.1) as the device actually places the samples
            // of a 2-, 4-, 8- and 16-sample pixel: (x, y) pairs in [0, 1], the N-sample row starting
            // at pair N - 2. Bit log2(N) of SamplePositionCountMask says row N was filled in; a row
            // the backend could not determine stays unset rather than being guessed. Read and
            // written through Get/SetSamplePositions only.
            static constexpr Int kMaxSamplePositionCount = 16;
            Float SamplePositions[60] = {}; // 2 * (2 + 4 + 8 + 16)
            Uint32 SamplePositionCountMask = 0;
            Bool SubgroupQuadOperationsInAllStages = false;
            GpuVendorKind GpuVendor = GpuVendorKind::Unknown;
            // GL_EXT_fragment_shading_rate and its two companions. Each is a statement about the
            // device, so all of them default to "absent" and the extension strings are advertised
            // only from these: DirectGLES fills them from the driver's own
            // EXT_fragment_shading_rate(_primitive, _attachment), DirectVulkan from
            // VK_KHR_fragment_shading_rate's pipeline / primitive / attachment features.
            //   SupportsFragmentShadingRate          glShadingRateEXT and gl_ShadingRateEXT
            //   SupportsPrimitiveFragmentShadingRate gl_PrimitiveShadingRateEXT, combiner op 0
            //   SupportsAttachmentFragmentShadingRate the R8UI rate attachment, combiner op 1
            Bool SupportsFragmentShadingRate = false;
            Bool SupportsPrimitiveFragmentShadingRate = false;
            Bool SupportsAttachmentFragmentShadingRate = false;
            // The four GL_FRAGMENT_SHADING_RATE_*_SUPPORTED_EXT booleans, as the device answers them.
            Bool FragmentShadingRateNonTrivialCombiners = false;
            Bool FragmentShadingRateWithShaderDepthStencilWrites = false;
            Bool FragmentShadingRateWithSampleMask = false;
            Bool FragmentShadingRatePrimitiveWithMultiViewport = false;
            Bool FragmentShadingRateAttachmentWithDefaultFramebuffer = false;
            // What glGetFragmentShadingRatesEXT answers, one mask per sample count 1, 2, 4 .. 64
            // (index = log2(samples)): bit (log2(w) << 2 | log2(h)) is set when that rate is
            // honoured exactly at that sample count, i.e. gl_ShadingRateEXT reads it back
            // unclamped. Zero masks mean no rate at all, which is what a sample count the device
            // cannot shade coarsely answers.
            Uint16 FragmentShadingRateMasks[7] = {};
            // The GL_{MIN,MAX}_FRAGMENT_SHADING_RATE_ATTACHMENT_TEXEL_{WIDTH,HEIGHT}_EXT,
            // _ASPECT_RATIO_EXT and _LAYERS_EXT limits. Meaningful only with the attachment.
            Int MinFragmentShadingRateAttachmentTexelWidth = 0;
            Int MinFragmentShadingRateAttachmentTexelHeight = 0;
            Int MaxFragmentShadingRateAttachmentTexelWidth = 0;
            Int MaxFragmentShadingRateAttachmentTexelHeight = 0;
            Int MaxFragmentShadingRateAttachmentTexelAspectRatio = 0;
            Int MaxFragmentShadingRateAttachmentLayers = 0;

            static constexpr Bool IsSamplePositionCount(Int samples) {
                return samples >= 2 && samples <= kMaxSamplePositionCount && (samples & (samples - 1)) == 0;
            }
            static constexpr Uint32 SamplePositionCountBit(Int samples) {
                return 1u << static_cast<Uint32>(std::countr_zero(static_cast<Uint32>(samples)));
            }
            // `xy` holds `samples` (x, y) pairs.
            void SetSamplePositions(Int samples, const Float* xy) {
                if (!IsSamplePositionCount(samples)) return;
                for (Int i = 0; i < 2 * samples; ++i)
                    SamplePositions[2 * (samples - 2) + i] = xy[i];
                SamplePositionCountMask |= SamplePositionCountBit(samples);
            }
            Bool GetSamplePosition(Int samples, Int index, Float* outXY) const {
                if (!IsSamplePositionCount(samples) || index < 0 || index >= samples ||
                    (SamplePositionCountMask & SamplePositionCountBit(samples)) == 0) {
                    return false;
                }
                outXY[0] = SamplePositions[2 * (samples - 2 + index)];
                outXY[1] = SamplePositions[2 * (samples - 2 + index) + 1];
                return true;
            }
        };
    } // namespace MG_Backend
} // namespace MobileGL
