// MobileGL - MobileGL/MG_Backend/DirectVulkan/BackendObject_DirectVulkan.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "BackendObject_DirectVulkan.h"
#include "MG_Backend/BackendObject.h"
#include "DirectVulkan.h"
#include "MG_State/GLState/FramebufferState/FramebufferObject.h"
#include "MG_State/GLState/TextureState/TextureState.h"
#include "MG_Util/Classifiers/TextureEnumClassifier.h"
#include "MG_Util/Converters/MGToGL/TextureEnumConverter.h"
#include "MG_Util/Converters/MGToStr/TextureEnumConverter.h"
#include "MG_Util/Converters/MGToVk/TextureEnumConverter.h"
#include "MG_Util/Texture/TextureFormatProcessor.h"

namespace MobileGL::MG_Backend::DirectVulkan {
    namespace {
        Bool IsReleaseCurrentRequest(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) {
            return dpy == EGL_NO_DISPLAY && draw == EGL_NO_SURFACE && read == EGL_NO_SURFACE && ctx == EGL_NO_CONTEXT;
        }

        Bool IsFormatIndexValid(TextureInternalFormat format) {
            return format != TextureInternalFormat::Unknown && static_cast<Int>(format) >= 0 &&
                   static_cast<SizeT>(format) < kFormatCapabilityFormatCount;
        }

        Bool IsLayeredTarget(TextureTarget target) {
            return target == TextureTarget::Texture3D || target == TextureTarget::Texture1DArray ||
                   target == TextureTarget::Texture2DArray || target == TextureTarget::TextureCubeMap ||
                   target == TextureTarget::TextureCubeMapArray ||
                   target == TextureTarget::Texture2DMultisampleArray;
        }

        Bool IsMultisampleTarget(TextureTarget target) {
            return target == TextureTarget::Texture2DMultisample ||
                   target == TextureTarget::Texture2DMultisampleArray;
        }

        Bool IsTextureBufferTarget(TextureTarget target) {
            return target == TextureTarget::TextureBuffer;
        }

        Bool IsIntegerInternalFormat(TextureInternalFormat format) {
            const GLenum glFormat = MG_Util::ConvertTextureInternalFormatToGLEnum(format);
            GLenum normalizedInternalFormat = glFormat;
            GLenum imageFormat = GL_RGBA;
            GLenum imageType = GL_UNSIGNED_BYTE;
            MG_Util::TextureFormatProcessor::NormalizePixelFormat(
                glFormat, PixelFormatNormalizeOptionBit::None, &normalizedInternalFormat, &imageFormat, &imageType);
            return imageFormat == GL_RED_INTEGER || imageFormat == GL_RG_INTEGER || imageFormat == GL_RGB_INTEGER ||
                   imageFormat == GL_RGBA_INTEGER;
        }

        FormatCapabilityFlags GetAttachmentCaps(TextureInternalFormat format) {
            FormatCapabilityFlags caps = FormatCapability::FramebufferRenderable;
            const Bool isDepth = MG_Util::IsDepthFormatInternalFormat(format);
            const Bool isStencil = MG_Util::IsStencilFormatInternalFormat(format);
            if (!isDepth && !isStencil) {
                caps |= FormatCapability::ColorAttachment;
            }
            if (isDepth) {
                caps |= FormatCapability::DepthAttachment;
            }
            if (isStencil) {
                caps |= FormatCapability::StencilAttachment;
            }
            return caps;
        }

        FormatCapabilityFlags BuildVulkanCaps(TextureInternalFormat logicalFormat,
                                              TextureTarget target,
                                              VkFormatFeatureFlags features) {
            FormatCapabilityFlags caps;
            const Bool isDepth = MG_Util::IsDepthFormatInternalFormat(logicalFormat);
            const Bool isStencil = MG_Util::IsStencilFormatInternalFormat(logicalFormat);
            const Bool isInteger = IsIntegerInternalFormat(logicalFormat);

            if (IsTextureBufferTarget(target)) {
                if ((features & VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT) != 0) {
                    caps |= FormatCapability::Creatable;
                    caps |= FormatCapability::Sampled;
                    caps |= FormatCapability::TextureBuffer;
                }
                return caps;
            }

            const Bool sampled = (features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
            const Bool linearFilter = (features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
            const Bool colorRenderable = (features & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0;
            const Bool depthStencilRenderable =
                (features & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
            const Bool renderable = (isDepth || isStencil) ? depthStencilRenderable : colorRenderable;

            if (sampled || renderable) {
                caps |= FormatCapability::Creatable;
            }
            if (sampled) {
                caps |= FormatCapability::Sampled;
                if (linearFilter && !isInteger && !isStencil) {
                    caps |= FormatCapability::LinearFilter;
                }
                if (!isStencil && (features & VK_FORMAT_FEATURE_BLIT_SRC_BIT) != 0 &&
                    (features & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0) {
                    caps |= FormatCapability::GenerateMipmap;
                }
                if (!isInteger && !isDepth && !isStencil) {
                    caps |= FormatCapability::TextureGather;
                }
                if (isDepth && !isStencil) {
                    caps |= FormatCapability::TextureShadow;
                }
            }
            if (renderable) {
                caps |= GetAttachmentCaps(logicalFormat);
                if (IsLayeredTarget(target)) {
                    caps |= FormatCapability::FramebufferLayered;
                }
            }
            if (IsMultisampleTarget(target)) {
                caps |= FormatCapability::MultisampleTexture;
            }
            return caps;
        }

        Optional<TextureInternalFormat> ResolveVulkanFallbackLogicalFormat(TextureInternalFormat format) {
            switch (format) {
            case TextureInternalFormat::RGB:
            case TextureInternalFormat::RGB8:
                return TextureInternalFormat::RGBA8;
            case TextureInternalFormat::SRGB8:
                return TextureInternalFormat::SRGB8Alpha8;
            case TextureInternalFormat::RGB8Snorm:
                return TextureInternalFormat::RGBA8Snorm;
            case TextureInternalFormat::RGB16:
                return TextureInternalFormat::RGBA16;
            case TextureInternalFormat::RGB16Snorm:
                return TextureInternalFormat::RGBA16Snorm;
            case TextureInternalFormat::RGB16F:
                return TextureInternalFormat::RGBA16F;
            case TextureInternalFormat::RGB32F:
                return TextureInternalFormat::RGBA32F;
            case TextureInternalFormat::RGB8I:
                return TextureInternalFormat::RGBA8I;
            case TextureInternalFormat::RGB8UI:
                return TextureInternalFormat::RGBA8UI;
            case TextureInternalFormat::RGB16I:
                return TextureInternalFormat::RGBA16I;
            case TextureInternalFormat::RGB16UI:
                return TextureInternalFormat::RGBA16UI;
            case TextureInternalFormat::RGB32I:
                return TextureInternalFormat::RGBA32I;
            case TextureInternalFormat::RGB32UI:
                return TextureInternalFormat::RGBA32UI;
            default:
                return Nullopt;
            }
        }

        Optional<VkFormat> ResolveVulkanFallbackFormat(TextureInternalFormat format) {
            const Optional<TextureInternalFormat> fallbackLogicalFormat = ResolveVulkanFallbackLogicalFormat(format);
            if (!fallbackLogicalFormat) {
                return Nullopt;
            }
            return MG_Util::ConvertTextureInternalFormatToVkEnum(*fallbackLogicalFormat);
        }

        constexpr FormatCapability kVulkanProbeCapabilities[] = {
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

        Bool HasNewCaveatFormatCaps(FormatCapabilityFlags nativeCaps, FormatCapabilityFlags fallbackCaps) {
            for (FormatCapability capability : kVulkanProbeCapabilities) {
                if (HasFormatCapability(fallbackCaps, capability) &&
                    !HasFormatCapability(nativeCaps, capability)) {
                    return true;
                }
            }
            return false;
        }

        String GetFormatCapabilityTargetNameForLog(SizeT targetIndex) {
            if (targetIndex == GetRenderbufferFormatCapabilityTargetIndex()) {
                return "Renderbuffer";
            }
            if (targetIndex < kFormatCapabilityTextureTargetCount) {
                return MG_Util::ConvertTextureTargetToString(static_cast<TextureTarget>(targetIndex));
            }
            return "Unknown";
        }

        void LogVulkanFormatCaveat(TextureInternalFormat logicalFormat,
                                   SizeT targetIndex,
                                   TextureInternalFormat fallbackFormat) {
            MGLOG_I("Caveat: %s %s not fully supported. Reason: native Vulkan format is not fully supported. Fallback: %s",
                    GetFormatCapabilityTargetNameForLog(targetIndex).c_str(),
                    MG_Util::ConvertTextureInternalFormatToString(logicalFormat).c_str(),
                    MG_Util::ConvertTextureInternalFormatToString(fallbackFormat).c_str());
        }

        Vector<Int> BuildSampleCounts(Int maxSamples) {
            Vector<Int> counts;
            for (Int samples = std::max(maxSamples, 1); samples > 1; samples >>= 1) {
                counts.push_back(samples);
            }
            counts.push_back(1);
            return counts;
        }

        void FillVulkanFormatCapabilities(VkPhysicalDevice physicalDevice,
                                          const DynamicBackendParameters& dynamicParameters,
                                          FormatCapabilityCache& cache) {
            cache.Clear();
            if (physicalDevice == VK_NULL_HANDLE) {
                return;
            }

            for (SizeT formatIndex = 0; formatIndex < kFormatCapabilityFormatCount; ++formatIndex) {
                const auto logicalFormat = static_cast<TextureInternalFormat>(formatIndex);
                if (!IsFormatIndexValid(logicalFormat)) {
                    continue;
                }

                VkFormat nativeFormat = MG_Util::ConvertTextureInternalFormatToVkEnum(logicalFormat);
                const Optional<TextureInternalFormat> fallbackLogicalFormat =
                    ResolveVulkanFallbackLogicalFormat(logicalFormat);
                VkFormat fallbackFormat = ResolveVulkanFallbackFormat(logicalFormat).value_or(VK_FORMAT_UNDEFINED);

                VkFormatProperties nativeProperties{};
                if (nativeFormat != VK_FORMAT_UNDEFINED) {
                    vkGetPhysicalDeviceFormatProperties(physicalDevice, nativeFormat, &nativeProperties);
                }

                VkFormatProperties fallbackProperties{};
                if (fallbackFormat != VK_FORMAT_UNDEFINED && fallbackFormat != nativeFormat) {
                    vkGetPhysicalDeviceFormatProperties(physicalDevice, fallbackFormat, &fallbackProperties);
                }

                for (SizeT targetIndex = 0; targetIndex < kFormatCapabilityTextureTargetCount; ++targetIndex) {
                    const auto target = static_cast<TextureTarget>(targetIndex);
                    const VkFormatFeatureFlags nativeFeatures =
                        IsTextureBufferTarget(target) ? nativeProperties.bufferFeatures
                                                      : nativeProperties.optimalTilingFeatures;
                    FormatCapabilityFlags nativeCaps = BuildVulkanCaps(logicalFormat, target, nativeFeatures);
                    cache.FullCaps[targetIndex][formatIndex] |= nativeCaps;

                    const VkFormatFeatureFlags fallbackFeatures =
                        IsTextureBufferTarget(target) ? fallbackProperties.bufferFeatures
                                                      : fallbackProperties.optimalTilingFeatures;
                    FormatCapabilityFlags fallbackCaps = BuildVulkanCaps(logicalFormat, target, fallbackFeatures);
                    if (fallbackFormat != VK_FORMAT_UNDEFINED && fallbackFormat != nativeFormat) {
                        cache.CaveatCaps[targetIndex][formatIndex] |= fallbackCaps;
                        if (fallbackLogicalFormat && HasNewCaveatFormatCaps(nativeCaps, fallbackCaps)) {
                            LogVulkanFormatCaveat(logicalFormat, targetIndex, *fallbackLogicalFormat);
                        }
                    }

                    if (HasFormatCapability(nativeCaps | fallbackCaps, FormatCapability::MultisampleTexture)) {
                        const Bool isDepth = MG_Util::IsDepthFormatInternalFormat(logicalFormat);
                        const Bool isStencil = MG_Util::IsStencilFormatInternalFormat(logicalFormat);
                        const Bool isInteger = IsIntegerInternalFormat(logicalFormat);
                        Int maxSamples = dynamicParameters.MaxColorTextureSamples;
                        if (isDepth || isStencil) {
                            maxSamples = dynamicParameters.MaxDepthTextureSamples;
                        } else if (isInteger) {
                            maxSamples = dynamicParameters.MaxIntegerSamples;
                        }
                        cache.SampleCounts[targetIndex][formatIndex] = BuildSampleCounts(maxSamples);
                    }
                }

                const SizeT renderbufferTargetIndex = GetRenderbufferFormatCapabilityTargetIndex();
                FormatCapabilityFlags renderbufferCaps =
                    BuildVulkanCaps(logicalFormat, TextureTarget::Texture2D, nativeProperties.optimalTilingFeatures);
                renderbufferCaps &= FormatCapability::Creatable;
                if ((nativeProperties.optimalTilingFeatures &
                     (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) != 0) {
                    renderbufferCaps |= GetAttachmentCaps(logicalFormat);
                    renderbufferCaps |= FormatCapability::MultisampleRenderbuffer;
                }
                cache.FullCaps[renderbufferTargetIndex][formatIndex] |= renderbufferCaps;

                if (fallbackFormat != VK_FORMAT_UNDEFINED && fallbackFormat != nativeFormat) {
                    FormatCapabilityFlags fallbackRenderbufferCaps =
                        BuildVulkanCaps(logicalFormat, TextureTarget::Texture2D,
                                        fallbackProperties.optimalTilingFeatures);
                    fallbackRenderbufferCaps &= FormatCapability::Creatable;
                    if ((fallbackProperties.optimalTilingFeatures &
                         (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) !=
                        0) {
                        fallbackRenderbufferCaps |= GetAttachmentCaps(logicalFormat);
                        fallbackRenderbufferCaps |= FormatCapability::MultisampleRenderbuffer;
                    }
                    cache.CaveatCaps[renderbufferTargetIndex][formatIndex] |= fallbackRenderbufferCaps;
                    if (fallbackLogicalFormat &&
                        HasNewCaveatFormatCaps(renderbufferCaps, fallbackRenderbufferCaps)) {
                        LogVulkanFormatCaveat(logicalFormat, renderbufferTargetIndex, *fallbackLogicalFormat);
                    }
                }

                const FormatCapabilityFlags rbCaps = cache.FullCaps[renderbufferTargetIndex][formatIndex] |
                                                     cache.CaveatCaps[renderbufferTargetIndex][formatIndex];
                if (HasFormatCapability(rbCaps, FormatCapability::MultisampleRenderbuffer)) {
                    cache.SampleCounts[renderbufferTargetIndex][formatIndex] =
                        BuildSampleCounts(dynamicParameters.MaxFramebufferSamples);
                }
            }
        }
    } // namespace

    BackendObject_DirectVulkan::~BackendObject_DirectVulkan() = default;

    BackendObject_DirectVulkan::BackendObject_DirectVulkan():
        m_rendererInfo{
            .RendererName = "Magma",
            .BackendName = "Direct (Vulkan)",
            .ExtraVendor = Nullopt,
            .RendererGLInfo =
                {
                    .TargetGLVersion = {3, 3, 0},
                    .TargetGLSLVersion = {4, 6, 0},
                    .Extensions = {V_OpenGL30, V_OpenGL31, V_OpenGL32,
                                   V_OpenGL33, E_GL_ARB_draw_buffers_blend, E_GL_ARB_compute_shader,
                                   E_GL_ARB_shader_storage_buffer_object, E_GL_ARB_shader_image_load_store,
                                   E_GL_ARB_program_interface_query, E_GL_ARB_framebuffer_object,
                                   E_GL_ARB_multi_draw_indirect, E_GL_ARB_indirect_parameters,
                                   E_GL_EXT_framebuffer_object, E_GL_ARB_depth_texture, E_GL_ARB_buffer_storage,
                                   E_GL_ARB_texture_storage, E_GL_ARB_direct_state_access,
                                   E_GL_ARB_shader_draw_parameters, E_GL_ARB_gpu_shader_int64},
                    .IsCompatibilityProfile = false
                },
            .StaticBackendCapability = {.AllowVSOnlyPrograms = false}} {}

    Bool BackendObject_DirectVulkan::InitWindowSurface() {
        if (!m_windowHandle.Handle) {
            MGLOG_E("Cannot initialize DirectVulkan window surface: native window handle is null");
            return false;
        }

        auto nativeWindow = reinterpret_cast<NativeWindowType>(m_windowHandle.Handle);

        pVulkanRenderer = MakeUnique<MG_Backend::DirectVulkan::VulkanRenderer>(nativeWindow);
        MOBILEGL_ASSERT(pVulkanRenderer != nullptr, "InitWindowSurface: VulkanRenderer creation failed");
        pVulkanRenderer->Initialize();
        return true;
    }

    Bool BackendObject_DirectVulkan::InitPbufferSurface(EGLint width, EGLint height) {
        VulkanRendererConfig config;
        config.SurfaceWidth = static_cast<Uint32>(std::max<EGLint>(width, 1));
        config.SurfaceHeight = static_cast<Uint32>(std::max<EGLint>(height, 1));
        pVulkanRenderer = MakeUnique<MG_Backend::DirectVulkan::VulkanRenderer>(NativeWindowType{}, config);
        MOBILEGL_ASSERT(pVulkanRenderer != nullptr, "InitPbufferSurface: VulkanRenderer creation failed");
        pVulkanRenderer->Initialize();
        return true;
    }

    void BackendObject_DirectVulkan::Initialize() {
        m_initialized = true;
    }

    Bool BackendObject_DirectVulkan::InitCapabilities() {
        if (!m_initialized) {
            MGLOG_E("Cannot initialize capabilities before backend is initialized");
            return false;
        }
        if (!pVulkanRenderer) {
            MGLOG_E("Cannot initialize capabilities: Vulkan renderer has not been created");
            return false;
        }

        const auto& physicalDevice = pVulkanRenderer->GetPhysicalDevice();
        if (!MG_Util::BackendLoader::QueryVulkanCapabilities(m_vulkanCaps, pVulkanRenderer->GetInstance(),
                                                             physicalDevice.handle)) {
            MGLOG_W("DirectVulkan: failed to query extended Vulkan capabilities, using basic properties");
            MG_Util::BackendLoader::FillInVulkanCapabilities(m_vulkanCaps, physicalDevice.properties);
        }
        UpdateDynamicBackendParameters();
        UpdateAdvertisedExtensions();
        FillVulkanFormatCapabilities(physicalDevice.handle, m_dynamicParameters, MutableFormatCapabilities());
        PrintFormatCapabilities(GetFormatCapabilities());
        return true;
    }

    Bool BackendObject_DirectVulkan::InitializeEGLDisplay(EGLDisplay dpy, EGLint* major, EGLint* minor) {
        if (!m_initialized) {
            MGLOG_E("DirectVulkan backend not initialized");
            return false;
        }
        return BackendObject::InitializeEGLDisplay(dpy, major, minor);
    }

    Bool BackendObject_DirectVulkan::CreateEGLWindowSurface(const WindowHandle& handle) {
        const std::lock_guard<std::recursive_mutex> lock(m_eglStateMutex);
        if (!m_initialized) {
            MGLOG_E("DirectVulkan backend not initialized");
            return false;
        }
        if (!handle.Handle || (handle.Backend != WindowBackend::Android && handle.Backend != WindowBackend::X11)) {
            MGLOG_E("DirectVulkan backend only supports Android and X11 native windows");
            return false;
        }

        const Bool sameHandle = m_eglSurfaceInitialized && m_eglSurfaceKind == SurfaceKind::Window &&
                                m_windowHandle.Backend == handle.Backend && m_windowHandle.Handle == handle.Handle;
        if (sameHandle) {
            return true;
        }

        if (m_eglSurfaceInitialized || pVulkanRenderer) {
            pVulkanRenderer.reset();
            ResetEGLRuntimeState();
        }

        return BackendObject::CreateEGLWindowSurface(handle);
    }

    Bool BackendObject_DirectVulkan::CreateEGLPbufferSurface(EGLint width, EGLint height) {
        const std::lock_guard<std::recursive_mutex> lock(m_eglStateMutex);
        if (!m_initialized) {
            MGLOG_E("DirectVulkan backend not initialized");
            return false;
        }
        if (m_eglSurfaceInitialized && m_eglSurfaceKind == SurfaceKind::Pbuffer) {
            return true;
        }
        if (m_eglSurfaceInitialized || pVulkanRenderer) {
            pVulkanRenderer.reset();
            ResetEGLRuntimeState();
        }
        return BackendObject::CreateEGLPbufferSurface(width, height);
    }

    Bool BackendObject_DirectVulkan::MakeEGLCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) {
        const std::lock_guard<std::recursive_mutex> lock(m_eglStateMutex);
        if (IsReleaseCurrentRequest(dpy, draw, read, ctx)) {
            return BackendObject::MakeEGLCurrent(dpy, draw, read, ctx);
        }
        if (!pVulkanRenderer) {
            MGLOG_E("DirectVulkan renderer is not initialized");
            return false;
        }
        return BackendObject::MakeEGLCurrent(dpy, draw, read, ctx);
    }

    Bool BackendObject_DirectVulkan::SwapEGLBuffers(EGLDisplay dpy, EGLSurface draw) {
        const std::lock_guard<std::recursive_mutex> lock(m_eglStateMutex);
        if (!pVulkanRenderer) {
            MGLOG_E("DirectVulkan renderer is not initialized");
            return false;
        }
        return BackendObject::SwapEGLBuffers(dpy, draw);
    }

    void BackendObject_DirectVulkan::ReleaseEGLResources() {
        const std::lock_guard<std::recursive_mutex> lock(m_eglStateMutex);
        pVulkanRenderer.reset();
        BackendObject::ReleaseEGLResources();
    }

    const RendererInfo& BackendObject_DirectVulkan::GetRendererInfo() const {
        return m_rendererInfo;
    }

    String BackendObject_DirectVulkan::GetBackendAPIVersionString() const {
        if (!m_initialized) {
            return "<uninitialized DirectVulkan backend>";
        }
        // Format:
        // <GPU Name>, Vulkan <Vulkan Version>, Driver <Driver Version>
        String str = m_vulkanCaps.DeviceName + ", Vulkan " + m_vulkanCaps.VulkanAPIVersion.toString() + ", Driver " +
                     m_vulkanCaps.DriverVersionString;
        return str;
    }

    BackendType BackendObject_DirectVulkan::GetBackendType() const {
        return BackendType::DirectVulkan;
    }

    const GlobalBackendFunctionsTable& BackendObject_DirectVulkan::GetBackendFunctions() const {
        static GlobalBackendFunctionsTable funcsTable;
        static Bool funcsTableInitialized = false;
        if (!funcsTableInitialized) {
            funcsTable.Present = Present;
            funcsTable.GL.DrawArrays = DrawArrays;
            funcsTable.GL.DrawElements = DrawElements;
            funcsTable.GL.DrawElementsBaseVertex = DrawElementsBaseVertex;
            funcsTable.GL.MultiDrawElements = MultiDrawElements;
            funcsTable.GL.MultiDrawElementsBaseVertex = MultiDrawElementsBaseVertex;
            funcsTable.GL.MultiDrawElementsIndirect = MultiDrawElementsIndirect;
            funcsTable.GL.MultiDrawArraysIndirect = MultiDrawArraysIndirect;
            funcsTable.GL.MultiDrawElementsIndirectCount = MultiDrawElementsIndirectCount;
            funcsTable.GL.MultiDrawArraysIndirectCount = MultiDrawArraysIndirectCount;
            funcsTable.GL.DrawRangeElementsBaseVertex = DrawRangeElementsBaseVertex;
            funcsTable.GL.DrawRangeElements = DrawRangeElements;
            funcsTable.GL.DrawElementsInstancedBaseVertexBaseInstance = DrawElementsInstancedBaseVertexBaseInstance;
            funcsTable.GL.DrawElementsInstancedBaseVertex = DrawElementsInstancedBaseVertex;
            funcsTable.GL.DrawElementsInstancedBaseInstance = DrawElementsInstancedBaseInstance;
            funcsTable.GL.DrawElementsInstanced = DrawElementsInstanced;
            funcsTable.GL.DrawArraysInstancedBaseInstance = DrawArraysInstancedBaseInstance;
            funcsTable.GL.DrawArraysInstanced = DrawArraysInstanced;
            funcsTable.GL.DrawElementsIndirect = DrawElementsIndirect;
            funcsTable.GL.DrawArraysIndirect = DrawArraysIndirect;
            funcsTable.GL.Clear = Clear;
            funcsTable.GL.ClearBufferfi = ClearBufferfi;
            funcsTable.GL.ClearBufferfv = ClearBufferfv;
            funcsTable.GL.ClearBufferuiv = ClearBufferuiv;
            funcsTable.GL.ClearBufferiv = ClearBufferiv;
            funcsTable.GL.ClearNamedFramebufferfv = ClearNamedFramebufferfv;
            funcsTable.GL.ClearNamedFramebufferfi = ClearNamedFramebufferfi;
            funcsTable.GL.BlitFramebuffer = BlitFramebuffer;
            funcsTable.GL.BlitNamedFramebuffer = BlitNamedFramebuffer;
            funcsTable.GL.CopyTexImage2D = CopyTexImage2D;
            funcsTable.GL.CopyTexSubImage2D = CopyTexSubImage2D;
            funcsTable.GL.CopyImageSubData = CopyImageSubData;
            funcsTable.GL.GenerateMipmap = GenerateMipmap;
            funcsTable.GL.ReadPixels = ReadPixels;
            funcsTable.GL.GetTexImage = GetTexImage;
            funcsTable.GL.GetTextureImage = GetTextureImage;
            funcsTable.GL.DispatchCompute = DispatchCompute;
            funcsTable.GL.DispatchComputeIndirect = DispatchComputeIndirect;
            funcsTable.GL.MemoryBarrier = MemoryBarrier;
            funcsTable.GL.MemoryBarrierByRegion = MemoryBarrierByRegion;
            funcsTable.GL.BindImageTexture = BindImageTexture;
            funcsTable.GL.GetIntegeri_v = GetIntegeri_v;
            funcsTable.GL.GetInteger64i_v = GetInteger64i_v;
            funcsTable.GL.GetProgramiv = GetProgramiv;
            funcsTable.GL.GetProgramInterfaceiv = GetProgramInterfaceiv;
            funcsTable.GL.GetProgramResourceIndex = GetProgramResourceIndex;
            funcsTable.GL.GetProgramResourceName = GetProgramResourceName;
            funcsTable.GL.GetProgramResourceiv = GetProgramResourceiv;
            funcsTable.GL.GetProgramResourceLocation = GetProgramResourceLocation;
            funcsTable.GL.GetProgramResourceLocationIndex = GetProgramResourceLocationIndex;
            funcsTable.GL.ShaderStorageBlockBinding = ShaderStorageBlockBinding;
            funcsTableInitialized = true;
        }
        return funcsTable;
    }

    const DynamicBackendParameters& BackendObject_DirectVulkan::GetDynamicParameters() const {
        return m_dynamicParameters;
    }

    void BackendObject_DirectVulkan::ApplyVulkanCapabilitiesForTesting(
        const MG_External::VulkanCapabilities& capabilities) {
        m_vulkanCaps = capabilities;
        UpdateDynamicBackendParameters();
        UpdateAdvertisedExtensions();
        MutableFormatCapabilities().Clear();
    }

    void BackendObject_DirectVulkan::UpdateAdvertisedExtensions() {
        auto& extensions = m_rendererInfo.RendererGLInfo.Extensions;
        extensions.erase(std::remove(extensions.begin(), extensions.end(), E_GL_KHR_shader_subgroup),
                         extensions.end());

        if (m_vulkanCaps.SupportsShaderSubgroup) {
            extensions.push_back(E_GL_KHR_shader_subgroup);
        }
    }

    void BackendObject_DirectVulkan::UpdateDynamicBackendParameters() {
        const auto mapShaderStages = [](Uint32 vkStages) {
            Uint32 glStages = 0;
            if ((vkStages & VK_SHADER_STAGE_VERTEX_BIT) != 0) glStages |= GL_VERTEX_SHADER_BIT;
            if ((vkStages & VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT) != 0) glStages |= GL_TESS_CONTROL_SHADER_BIT;
            if ((vkStages & VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT) != 0) {
                glStages |= GL_TESS_EVALUATION_SHADER_BIT;
            }
            if ((vkStages & VK_SHADER_STAGE_GEOMETRY_BIT) != 0) glStages |= GL_GEOMETRY_SHADER_BIT;
            if ((vkStages & VK_SHADER_STAGE_FRAGMENT_BIT) != 0) glStages |= GL_FRAGMENT_SHADER_BIT;
            if ((vkStages & VK_SHADER_STAGE_COMPUTE_BIT) != 0) glStages |= GL_COMPUTE_SHADER_BIT;
            return glStages;
        };

        const auto mapSubgroupFeatures = [](Uint32 vkFeatures) {
            Uint32 glFeatures = 0;
            if ((vkFeatures & VK_SUBGROUP_FEATURE_BASIC_BIT) != 0) {
                glFeatures |= GL_SUBGROUP_FEATURE_BASIC_BIT_KHR;
            }
            if ((vkFeatures & VK_SUBGROUP_FEATURE_VOTE_BIT) != 0) {
                glFeatures |= GL_SUBGROUP_FEATURE_VOTE_BIT_KHR;
            }
            if ((vkFeatures & VK_SUBGROUP_FEATURE_ARITHMETIC_BIT) != 0) {
                glFeatures |= GL_SUBGROUP_FEATURE_ARITHMETIC_BIT_KHR;
            }
            if ((vkFeatures & VK_SUBGROUP_FEATURE_BALLOT_BIT) != 0) {
                glFeatures |= GL_SUBGROUP_FEATURE_BALLOT_BIT_KHR;
            }
            if ((vkFeatures & VK_SUBGROUP_FEATURE_SHUFFLE_BIT) != 0) {
                glFeatures |= GL_SUBGROUP_FEATURE_SHUFFLE_BIT_KHR;
            }
            if ((vkFeatures & VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT) != 0) {
                glFeatures |= GL_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT_KHR;
            }
            if ((vkFeatures & VK_SUBGROUP_FEATURE_CLUSTERED_BIT) != 0) {
                glFeatures |= GL_SUBGROUP_FEATURE_CLUSTERED_BIT_KHR;
            }
            if ((vkFeatures & VK_SUBGROUP_FEATURE_QUAD_BIT) != 0) {
                glFeatures |= GL_SUBGROUP_FEATURE_QUAD_BIT_KHR;
            }
            return glFeatures;
        };

        static constexpr SizeT kMaxAdvertisedShaderStorageBlockSize = 512ull * 1024ull * 1024ull;
        m_dynamicParameters.UniformBufferOffsetAlignment = m_vulkanCaps.UniformBufferOffsetAlignment;
        m_dynamicParameters.AliasedLineWidthRangeMin = m_vulkanCaps.AliasedLineWidthRangeMin;
        m_dynamicParameters.AliasedLineWidthRangeMax = m_vulkanCaps.AliasedLineWidthRangeMax;
        m_dynamicParameters.SmoothLineWidthRangeMin = m_vulkanCaps.SmoothLineWidthRangeMin;
        m_dynamicParameters.SmoothLineWidthRangeMax = m_vulkanCaps.SmoothLineWidthRangeMax;
        m_dynamicParameters.SmoothLineWidthGranularity = m_vulkanCaps.SmoothLineWidthGranularity;
        m_dynamicParameters.PointSizeRangeMin = m_vulkanCaps.PointSizeRangeMin;
        m_dynamicParameters.PointSizeRangeMax = m_vulkanCaps.PointSizeRangeMax;
        m_dynamicParameters.PointSizeGranularity = m_vulkanCaps.PointSizeGranularity;
        m_dynamicParameters.Max3DTextureSize = m_vulkanCaps.Max3DTextureSize;
        m_dynamicParameters.MaxArrayTextureLayers = m_vulkanCaps.MaxArrayTextureLayers;
        m_dynamicParameters.MaxCubeMapTextureSize = m_vulkanCaps.MaxCubeMapTextureSize;
        m_dynamicParameters.MaxFramebufferWidth = m_vulkanCaps.MaxFramebufferWidth;
        m_dynamicParameters.MaxFramebufferHeight = m_vulkanCaps.MaxFramebufferHeight;
        m_dynamicParameters.MaxFramebufferLayers = m_vulkanCaps.MaxFramebufferLayers;
        m_dynamicParameters.MaxRenderbufferSize = m_vulkanCaps.MaxRenderbufferSize;
        m_dynamicParameters.MaxTextureSize = m_vulkanCaps.MaxTextureSize;
        m_dynamicParameters.MaxColorTextureSamples = m_vulkanCaps.MaxColorTextureSamples;
        m_dynamicParameters.MaxDepthTextureSamples = m_vulkanCaps.MaxDepthTextureSamples;
        m_dynamicParameters.MaxFramebufferSamples = m_vulkanCaps.MaxFramebufferSamples;
        m_dynamicParameters.MaxIntegerSamples = m_vulkanCaps.MaxIntegerSamples;
        m_dynamicParameters.MaxSamples = m_vulkanCaps.MaxSamples;
        m_dynamicParameters.MaxSampleMaskWords = m_vulkanCaps.MaxSampleMaskWords;
        const Int maxSupportedTextureUnits =
            static_cast<Int>(MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS);
        m_dynamicParameters.MaxTextureImageUnits = std::min(m_vulkanCaps.MaxTextureImageUnits, maxSupportedTextureUnits);
        m_dynamicParameters.MaxVertexTextureImageUnits =
            std::min(m_vulkanCaps.MaxVertexTextureImageUnits, maxSupportedTextureUnits);
        m_dynamicParameters.MaxComputeTextureImageUnits =
            std::min(m_vulkanCaps.MaxComputeTextureImageUnits, maxSupportedTextureUnits);
        m_dynamicParameters.MaxCombinedTextureImageUnits =
            std::min(m_vulkanCaps.MaxCombinedTextureImageUnits, maxSupportedTextureUnits);
        m_dynamicParameters.MaxVertexAttribs = m_vulkanCaps.MaxVertexAttribs;
        m_dynamicParameters.MaxComputeShaderStorageBlocks = m_vulkanCaps.MaxComputeShaderStorageBlocks;
        m_dynamicParameters.MaxCombinedShaderStorageBlocks = m_vulkanCaps.MaxCombinedShaderStorageBlocks;
        m_dynamicParameters.MaxComputeUniformBlocks = m_vulkanCaps.MaxComputeUniformBlocks;
        m_dynamicParameters.MaxComputeWorkGroupInvocations = m_vulkanCaps.MaxComputeWorkGroupInvocations;
        m_dynamicParameters.MaxShaderStorageBufferBindings = m_vulkanCaps.MaxShaderStorageBufferBindings;
        m_dynamicParameters.MaxTextureBufferSize = m_vulkanCaps.MaxTextureBufferSize;
        m_dynamicParameters.MaxUniformBufferBindings = m_vulkanCaps.MaxUniformBufferBindings;
        m_dynamicParameters.MaxUniformBlockSize = m_vulkanCaps.MaxUniformBlockSize;
        m_dynamicParameters.MaxImageUnits = m_vulkanCaps.MaxImageUnits;
        m_dynamicParameters.MaxCombinedImageUniforms = m_vulkanCaps.MaxCombinedImageUniforms;
        m_dynamicParameters.MaxComputeImageUniforms = m_vulkanCaps.MaxComputeImageUniforms;
        const Int maxSupportedDrawBuffers =
            static_cast<Int>(MG_State::GLState::FramebufferObject::MAX_DRAW_BUFFERS);
        m_dynamicParameters.MaxDrawBuffers = std::min(m_vulkanCaps.MaxDrawBuffers, maxSupportedDrawBuffers);
        m_dynamicParameters.MaxColorAttachments = std::min(m_vulkanCaps.MaxColorAttachments, maxSupportedDrawBuffers);
        m_dynamicParameters.MaxClipDistances = m_vulkanCaps.MaxClipDistances;
        m_dynamicParameters.MaxViewports = m_vulkanCaps.MaxViewports;
        m_dynamicParameters.MaxViewportWidth = m_vulkanCaps.MaxViewportWidth;
        m_dynamicParameters.MaxViewportHeight = m_vulkanCaps.MaxViewportHeight;
        m_dynamicParameters.ViewportBoundsRangeMin = m_vulkanCaps.ViewportBoundsRangeMin;
        m_dynamicParameters.ViewportBoundsRangeMax = m_vulkanCaps.ViewportBoundsRangeMax;
        m_dynamicParameters.ViewportSubpixelBits = m_vulkanCaps.ViewportSubpixelBits;
        m_dynamicParameters.SupportsWideLines = m_vulkanCaps.SupportsWideLines;
        m_dynamicParameters.MaxShaderStorageBlockSize =
            std::min(m_vulkanCaps.MaxShaderStorageBlockSize, kMaxAdvertisedShaderStorageBlockSize);
        if (m_vulkanCaps.SupportsShaderSubgroup) {
            m_dynamicParameters.SubgroupSize = m_vulkanCaps.SubgroupSize;
            m_dynamicParameters.SubgroupSupportedStages = mapShaderStages(m_vulkanCaps.SubgroupSupportedStages);
            m_dynamicParameters.SubgroupSupportedFeatures = mapSubgroupFeatures(m_vulkanCaps.SubgroupSupportedOperations);
            m_dynamicParameters.SubgroupQuadOperationsInAllStages = m_vulkanCaps.SubgroupQuadOperationsInAllStages;
        } else {
            m_dynamicParameters.SubgroupSize = 0;
            m_dynamicParameters.SubgroupSupportedStages = 0;
            m_dynamicParameters.SubgroupSupportedFeatures = 0;
            m_dynamicParameters.SubgroupQuadOperationsInAllStages = false;
        }
        if (m_dynamicParameters.MaxShaderStorageBlockSize != m_vulkanCaps.MaxShaderStorageBlockSize) {
            MGLOG_I("DirectVulkan: clamped GL_MAX_SHADER_STORAGE_BLOCK_SIZE from %zu to %zu",
                    m_vulkanCaps.MaxShaderStorageBlockSize,
                    m_dynamicParameters.MaxShaderStorageBlockSize);
        }
    }
} // namespace MobileGL::MG_Backend::DirectVulkan
