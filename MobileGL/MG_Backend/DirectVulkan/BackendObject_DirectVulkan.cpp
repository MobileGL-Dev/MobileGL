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
#include "SubgroupSupportPolicy.h"
#include "MG_State/GLState/FramebufferState/FramebufferObject.h"
#include "MG_State/GLState/Core.h"
#include "MG_State/GLState/TextureState/TextureState.h"
#include "MG_Util/Classifiers/TextureEnumClassifier.h"
#include "MG_Util/Converters/MGToGL/TextureEnumConverter.h"
#include "MG_Util/Converters/MGToStr/TextureEnumConverter.h"
#include "MG_Util/Converters/MGToVk/TextureEnumConverter.h"
#include "MG_Util/Texture/TextureFormatProcessor.h"
#include "MG_Util/Async/ShaderCompilePool.h"

#include <Config.h>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace MobileGL::MG_Backend::DirectVulkan {
    namespace {
        Bool IsR11G11B10FFallbackEnabled() {
            return MG_Config::Features.MagmaR11G11B10FFallback;
        }

        Bool IsReleaseCurrentRequest(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) {
            (void)dpy;
            return draw == EGL_NO_SURFACE && read == EGL_NO_SURFACE && ctx == EGL_NO_CONTEXT;
        }

        Bool IsFormatIndexValid(TextureInternalFormat format) {
            return format != TextureInternalFormat::Unknown && static_cast<Int>(format) >= 0 &&
                   static_cast<SizeT>(format) < kFormatCapabilityFormatCount;
        }

        Bool IsLayeredTarget(TextureTarget target) {
            return target == TextureTarget::Texture3D || target == TextureTarget::Texture1DArray ||
                   target == TextureTarget::Texture2DArray || target == TextureTarget::TextureCubeMap ||
                   target == TextureTarget::TextureCubeMapArray || target == TextureTarget::Texture2DMultisampleArray;
        }

        Bool IsMultisampleTarget(TextureTarget target) {
            return target == TextureTarget::Texture2DMultisample || target == TextureTarget::Texture2DMultisampleArray;
        }

        Bool IsTextureBufferTarget(TextureTarget target) {
            return target == TextureTarget::TextureBuffer;
        }

        Bool IsIntegerInternalFormat(TextureInternalFormat format) {
            const GLenum glFormat = MG_Util::ConvertTextureInternalFormatToGLEnum(format);
            GLenum normalizedInternalFormat = glFormat;
            GLenum imageFormat = GL_RGBA;
            GLenum imageType = GL_UNSIGNED_BYTE;
            MG_Util::TextureFormatProcessor::NormalizePixelFormat(glFormat, PixelFormatNormalizeOptionBit::None,
                                                                  &normalizedInternalFormat, &imageFormat, &imageType);
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

        FormatCapabilityFlags BuildVulkanCaps(TextureInternalFormat logicalFormat, TextureTarget target,
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
            const Bool depthStencilRenderable = (features & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
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
            // Legacy low-bit-depth formats with no (or rarely supported) native Vulkan
            // encoding; a wider normalized fallback keeps at least the required precision.
            case TextureInternalFormat::R3G3B2:
            case TextureInternalFormat::RGB4:
            case TextureInternalFormat::RGB5:
            case TextureInternalFormat::RGBA2:
            case TextureInternalFormat::RGBA4:
            case TextureInternalFormat::RGB5A1:
                return TextureInternalFormat::RGBA8;
            case TextureInternalFormat::RGB10:
                return TextureInternalFormat::RGB10A2;
            case TextureInternalFormat::RGB12:
            case TextureInternalFormat::RGBA12:
                return TextureInternalFormat::RGBA16;
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
            case TextureInternalFormat::R11FG11FB10F:
                if (IsR11G11B10FFallbackEnabled()) {
                    return TextureInternalFormat::RGBA16F;
                }
                return Nullopt;
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

        Bool HasNewCaveatFormatCaps(FormatCapabilityFlags nativeCaps, FormatCapabilityFlags fallbackCaps) {
            for (FormatCapability capability : kReportedFormatCapabilities) {
                if (HasFormatCapability(fallbackCaps, capability) && !HasFormatCapability(nativeCaps, capability)) {
                    return true;
                }
            }
            return false;
        }

        void LogVulkanFormatCaveat(TextureInternalFormat logicalFormat, SizeT targetIndex,
                                   TextureInternalFormat fallbackFormat) {
            MGLOG_D(
                "Caveat: %s %s not fully supported. Reason: native Vulkan format is not fully supported. Fallback: %s",
                GetFormatCapabilityTargetName(targetIndex).c_str(),
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

        void PopulateFormatCapabilitiesImpl(VkPhysicalDevice physicalDevice,
                                            PFN_vkGetPhysicalDeviceFormatProperties getFormatProperties,
                                            const MG_External::VulkanCapabilities& capabilities,
                                            FormatCapabilityCache& cache) {
            cache.Clear();
            if (physicalDevice == VK_NULL_HANDLE || getFormatProperties == nullptr) {
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
                    getFormatProperties(physicalDevice, nativeFormat, &nativeProperties);
                }

                VkFormatProperties fallbackProperties{};
                if (fallbackFormat != VK_FORMAT_UNDEFINED && fallbackFormat != nativeFormat) {
                    getFormatProperties(physicalDevice, fallbackFormat, &fallbackProperties);
                }

                for (SizeT targetIndex = 0; targetIndex < kFormatCapabilityTextureTargetCount; ++targetIndex) {
                    const auto target = static_cast<TextureTarget>(targetIndex);
                    const VkFormatFeatureFlags nativeFeatures = IsTextureBufferTarget(target)
                                                                    ? nativeProperties.bufferFeatures
                                                                    : nativeProperties.optimalTilingFeatures;
                    FormatCapabilityFlags nativeCaps = BuildVulkanCaps(logicalFormat, target, nativeFeatures);
                    cache.FullCaps[targetIndex][formatIndex] |= nativeCaps;

                    const VkFormatFeatureFlags fallbackFeatures = IsTextureBufferTarget(target)
                                                                      ? fallbackProperties.bufferFeatures
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
                        Int maxSamples = capabilities.MaxColorTextureSamples;
                        if (isDepth || isStencil) {
                            maxSamples = capabilities.MaxDepthTextureSamples;
                        } else if (isInteger) {
                            maxSamples = capabilities.MaxIntegerSamples;
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
                    FormatCapabilityFlags fallbackRenderbufferCaps = BuildVulkanCaps(
                        logicalFormat, TextureTarget::Texture2D, fallbackProperties.optimalTilingFeatures);
                    fallbackRenderbufferCaps &= FormatCapability::Creatable;
                    if ((fallbackProperties.optimalTilingFeatures &
                         (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) !=
                        0) {
                        fallbackRenderbufferCaps |= GetAttachmentCaps(logicalFormat);
                        fallbackRenderbufferCaps |= FormatCapability::MultisampleRenderbuffer;
                    }
                    cache.CaveatCaps[renderbufferTargetIndex][formatIndex] |= fallbackRenderbufferCaps;
                    if (fallbackLogicalFormat && HasNewCaveatFormatCaps(renderbufferCaps, fallbackRenderbufferCaps)) {
                        LogVulkanFormatCaveat(logicalFormat, renderbufferTargetIndex, *fallbackLogicalFormat);
                    }
                }

                const FormatCapabilityFlags rbCaps = cache.FullCaps[renderbufferTargetIndex][formatIndex] |
                                                     cache.CaveatCaps[renderbufferTargetIndex][formatIndex];
                if (HasFormatCapability(rbCaps, FormatCapability::MultisampleRenderbuffer)) {
                    cache.SampleCounts[renderbufferTargetIndex][formatIndex] =
                        BuildSampleCounts(capabilities.MaxFramebufferSamples);
                }
            }
        }
    } // namespace

    void PopulateFormatCapabilities(VkPhysicalDevice physicalDevice,
                                    PFN_vkGetPhysicalDeviceFormatProperties getFormatProperties,
                                    const MG_External::VulkanCapabilities& capabilities, FormatCapabilityCache& cache) {
        PopulateFormatCapabilitiesImpl(physicalDevice, getFormatProperties, capabilities, cache);
    }

    BackendObject_DirectVulkan::~BackendObject_DirectVulkan() = default;

    BackendObject_DirectVulkan::BackendObject_DirectVulkan() : m_rendererInfo{GetRendererIdentity()} {}

    Bool BackendObject_DirectVulkan::InitWindowSurface() {
        if (!m_windowHandle.Handle) {
            MGLOG_E("Cannot initialize DirectVulkan window surface: native window handle is null");
            return false;
        }

        auto nativeWindow = reinterpret_cast<NativeWindowType>(m_windowHandle.Handle);

        // Any renderer instance this assignment replaces is destroyed here;
        // fence/timer-query handles stamped with the old generation go stale.
        BumpRendererGeneration();
        pVulkanRenderer = MakeUnique<MG_Backend::DirectVulkan::VulkanRenderer>(nativeWindow);
        MOBILEGL_ASSERT(pVulkanRenderer != nullptr, "InitWindowSurface: VulkanRenderer creation failed");
        pVulkanRenderer->Initialize();
        return true;
    }

    Bool BackendObject_DirectVulkan::InitPbufferSurface(EGLint width, EGLint height) {
        VulkanRendererConfig config;
        config.SurfaceWidth = static_cast<Uint32>(std::max<EGLint>(width, 1));
        config.SurfaceHeight = static_cast<Uint32>(std::max<EGLint>(height, 1));
        // Any renderer instance this assignment replaces is destroyed here;
        // fence/timer-query handles stamped with the old generation go stale.
        BumpRendererGeneration();
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
        if (MG_State::pGLContext) {
            MG_State::pGLContext->InvalidateCompileEnv();
        }
        PopulateFormatCapabilities(physicalDevice.handle, vkGetPhysicalDeviceFormatProperties, m_vulkanCaps,
                                   MutableFormatCapabilities());
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

    Bool BackendObject_DirectVulkan::CreateEGLWindowSurface(EGLSurface surface, const WindowHandle& handle) {
        const std::lock_guard<std::recursive_mutex> lock(m_eglStateMutex);
        if (!m_initialized) {
            MGLOG_E("DirectVulkan backend not initialized");
            return false;
        }
        if (!handle.Handle || (handle.Backend != WindowBackend::Android && handle.Backend != WindowBackend::X11 &&
                               handle.Backend != WindowBackend::MetalLayer && handle.Backend != WindowBackend::Win32)) {
            MGLOG_E("DirectVulkan backend only supports Android, X11, CAMetalLayer, and Win32 native windows");
            return false;
        }

        return RegisterEGLWindowSurface(surface, handle);
    }

    Bool BackendObject_DirectVulkan::ResizeEGLWindowSurface(EGLSurface surface, Uint32 width, Uint32 height) {
        const std::lock_guard<std::recursive_mutex> lock(m_eglStateMutex);
        if (!m_initialized) {
            MGLOG_E("DirectVulkan backend not initialized");
            return false;
        }
        if (!BackendObject::ResizeEGLWindowSurface(surface, width, height)) {
            return false;
        }
        if (pVulkanRenderer && m_eglSurface == surface) {
            pVulkanRenderer->RequestSwapchainResize(width, height);
        }
        return true;
    }

    Bool BackendObject_DirectVulkan::CreateEGLPbufferSurface(EGLSurface surface, EGLint width, EGLint height) {
        const std::lock_guard<std::recursive_mutex> lock(m_eglStateMutex);
        if (!m_initialized) {
            MGLOG_E("DirectVulkan backend not initialized");
            return false;
        }
        return RegisterEGLPbufferSurface(surface, width, height);
    }

    Bool BackendObject_DirectVulkan::MakeEGLCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) {
        const std::lock_guard<std::recursive_mutex> lock(m_eglStateMutex);
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

    void BackendObject_DirectVulkan::ReleaseEGLSurface(EGLSurface surface) {
        const std::lock_guard<std::recursive_mutex> lock(m_eglStateMutex);
        BackendObject::ReleaseEGLSurface(surface);
    }

    void BackendObject_DirectVulkan::ReleaseEGLResources() {
        const std::lock_guard<std::recursive_mutex> lock(m_eglStateMutex);
        // Outstanding fence/timer-query handles now refer to a dead renderer;
        // treat them as signaled/available with zero results from here on.
        BumpRendererGeneration();
        pVulkanRenderer.reset();
        // The reflection cache is file-scope, not renderer-owned; without this the
        // deleted programs' reflection strings survive full context teardown.
        ClearProgramResourceCaches();
        BackendObject::ReleaseEGLResources();
    }

    void BackendObject_DirectVulkan::OnEGLSurfaceReleased(EGLSurface surface) {
        (void)surface;
        // Outstanding fence/timer-query handles now refer to a dead renderer;
        // treat them as signaled/available with zero results from here on.
        BumpRendererGeneration();
        pVulkanRenderer.reset();
        // The reflection cache is file-scope, not renderer-owned; without this the
        // deleted programs' reflection strings survive full context teardown.
        ClearProgramResourceCaches();
    }

    const RendererInfo& BackendObject_DirectVulkan::GetRendererInfo() const {
        return m_rendererInfo;
    }

    String BackendObject_DirectVulkan::GetBackendAPIVersionString() const {
        if (!m_initialized) {
            return "<uninitialized DirectVulkan backend>";
        }
        return FormatBackendAPIVersionString(m_vulkanCaps.DeviceName, m_vulkanCaps.VulkanAPIVersion.toString(),
                                             m_vulkanCaps.DriverVersionString);
    }

    const RendererInfo& GetRendererIdentity() {
        static const RendererInfo rendererInfo = {
            .RendererName = "Magma",
            .BackendName = "Direct (Vulkan)",
            .ExtraVendor = Nullopt,
            .RendererGLInfo = {.TargetGLVersion = {4, 6, 0},
                               .TargetGLSLVersion = {4, 6, 0},
                               // Baseline advertisement (no runtime-gated capabilities); a live
                               // backend reconciles its copy in UpdateAdvertisedExtensions.
                               .Extensions = BuildAdvertisedExtensions(false, false, false, false, false),
                               .IsCompatibilityProfile = false},
            .StaticBackendCapability = {.AllowVSOnlyPrograms = false}};
        return rendererInfo;
    }

    Vector<GLExtension> BuildAdvertisedExtensions(Bool shaderSubgroupSupported, Bool timerQueriesSupported,
                                                  Bool anisotropicFilteringSupported,
                                                  Bool nonZeroIndirectBaseInstanceSupported,
                                                  Bool cubeMapArraySupported) {
        Vector<GLExtension> extensions = {
            // The version tokens have to reach the version the backend actually claims:
            // TargetGLVersion is {4,6,0}, and a list that stopped at OpenGL40 told an
            // application feature-detecting off these tokens the opposite of what
            // GL_MAJOR_VERSION / GL_MINOR_VERSION told it.
            V_OpenGL30, V_OpenGL31, V_OpenGL32, V_OpenGL33, V_OpenGL40, V_OpenGL41, V_OpenGL42, V_OpenGL43,
            V_OpenGL44, V_OpenGL45, V_OpenGL46,
            E_GL_ARB_draw_buffers_blend,
            E_GL_ARB_compute_shader, E_GL_ARB_shader_storage_buffer_object, E_GL_ARB_shader_image_load_store,
            E_GL_ARB_clear_buffer_object, E_GL_ARB_program_interface_query, E_GL_ARB_framebuffer_object, E_GL_ARB_draw_indirect,
            E_GL_ARB_multi_draw_indirect,
            E_GL_ARB_indirect_parameters, E_GL_EXT_framebuffer_object, E_GL_ARB_depth_texture, E_GL_ARB_buffer_storage,
            E_GL_ARB_texture_storage, E_GL_ARB_texture_storage_multisample, E_GL_ARB_texture_multisample,
            E_GL_ARB_clear_texture, E_GL_ARB_direct_state_access, E_GL_ARB_shader_draw_parameters,
            E_GL_ARB_gpu_shader_int64, E_GL_KHR_debug, E_GL_ARB_gpu_shader5, E_GL_ARB_multi_bind,
            E_GL_ARB_shading_language_420pack, E_GL_ARB_vertex_attrib_binding, E_GL_ARB_shader_image_size,
            E_GL_ARB_explicit_attrib_location,
            // Core since GL 3.1 and implemented for every version advertised here. The string
            // matters because applications gate the ENTRY POINTS on it rather than on the
            // version: a caller that finds the extension missing never resolves
            // glGetUniformBlockIndex / glUniformBlockBinding, and one that then uses uniform
            // blocks anyway calls through a null pointer.
            E_GL_ARB_uniform_buffer_object,
            // Sampling the stencil aspect through DEPTH_STENCIL_TEXTURE_MODE. Core from 4.3,
            // so on a 4.0 context the string is the only way to reach it.
            E_GL_ARB_stencil_texturing,
            // Unconditional, unlike DirectGLES: a GL texture view is a second set of VkImageViews
            // over the same VkImage with a sub-range and possibly a reinterpreted VkFormat, which
            // is core Vulkan on every device MobileGL runs on. Format-reinterpreting views need
            // VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT on the image, which SyncTextureResource sets for
            // every immutable-storage texture (see the comment there).
            E_GL_ARB_texture_view,
            // Core since 3.2 and implemented here on both backends - glDrawElementsBaseVertex,
            // glDrawRangeElementsBaseVertex, glDrawElementsInstancedBaseVertex and
            // glMultiDrawElementsBaseVertex all reach real per-draw vertex rebasing. The string
            // was simply never emitted, which left KHR-GL4*.draw_elements_base_vertex_tests
            // NotSupported on a feature that works.
            E_GL_ARB_draw_elements_base_vertex,
            // The whole sync-object family is real and core since 3.2: glFenceSync, glIsSync,
            // glDeleteSync, glClientWaitSync, glWaitSync and glGetSynciv all live in GLImpl over a
            // backend fence (a VkFence here, an EGLSync/GLsync on DirectGLES), and glGetInteger64v
            // answers GL_MAX_SERVER_WAIT_TIMEOUT. The string matters for the same reason
            // ARB_uniform_buffer_object's does: LWJGL builds GLCapabilities from the extension
            // list, and a caller that finds GL_ARB_sync missing never resolves the entry points -
            // then calls through null if it uses fences anyway. Nothing in the CTS gates on this
            // string, so it is advertised on the strength of the implementation, not a test unlock.
            E_GL_ARB_sync,
            // Atomic counters, core since 4.2. glGetActiveAtomicCounterBufferiv and the whole
            // GL_ATOMIC_COUNTER_BUFFER_* query family are real in GLImpl, and the counter buffer
            // now reaches the shader on BOTH backends - Magma resolves the lowered
            // gl_AtomicCounterBlock_<N> from the atomic-counter binding points rather than the
            // shader-storage ones (see ResolveStorageBufferDescriptor). Withheld here until that
            // landed, because the counter silently read whatever was bound as SSBO N instead.
            E_GL_ARB_shader_atomic_counters,
            // glVertexAttribDivisor, core since 3.3 and real on both backends. Applications
            // (Better Clouds' GLCompat among them) accept the extension string as an
            // ALTERNATIVE to a 3.3 context when deciding whether instanced rendering is
            // available, so withholding it makes MobileGL look less capable than it is.
            E_GL_ARB_instanced_arrays,
            // Core GL 3.0-4.3 plumbing that has been real here for as long as the backend has
            // existed, and that was simply never named. None of these unlocks a single CTS case -
            // the conformance suite reaches all of them through the version - so they are
            // advertised for the OTHER consumer of this list: LWJGL builds GLCapabilities from the
            // string set, and an application that gates its ENTRY POINTS on the string rather than
            // on the version never resolves them and then calls through null. Each is backed by
            // the entry points named beside it. Kept identical to the DirectGLES block so the two
            // backends do not disagree about what MobileGL is.
            //
            // glBindVertexArray / glGenVertexArrays / glDeleteVertexArrays / glIsVertexArray.
            E_GL_ARB_vertex_array_object,
            // The 14 glSamplerParameter* / glGetSamplerParameter* entry points, including the
            // integer-valued Iiv/Iuiv forms.
            E_GL_ARB_sampler_objects,
            // glMapBufferRange + glFlushMappedBufferRange, which ARB_buffer_storage's persistent
            // maps are already built on top of.
            E_GL_ARB_map_buffer_range,
            // glCopyBufferSubData plus the GL_COPY_READ_BUFFER / GL_COPY_WRITE_BUFFER targets.
            E_GL_ARB_copy_buffer,
            // glCopyImageSubData, wired to a real backend hook on both backends.
            E_GL_ARB_copy_image,
            // GL_TEXTURE_SWIZZLE_{R,G,B,A,RGBA}, which map onto a VkImageView's component swizzle.
            E_GL_ARB_texture_swizzle,
            // GL_INT_2_10_10_10_REV / GL_UNSIGNED_INT_2_10_10_10_REV on glVertexAttribPointer plus
            // the eight glVertexAttribP* entry points.
            E_GL_ARB_vertex_type_2_10_10_10_rev,
            // The R/RG internal formats. Named separately from the float ones because an
            // application may check either.
            E_GL_ARB_texture_rg,
            // GL_DEPTH_COMPONENT32F and GL_DEPTH32F_STENCIL8.
            E_GL_ARB_depth_buffer_float,
            // The floating-point colour formats. Unlike the rest of this block this string DOES
            // gate CTS cases - KHR-GL4*.internalformat.texture2d.*{16f,32f} is keyed on it with no
            // core-version fallback, so eight cases per version list were NotSupported on formats
            // the backend has always had.
            E_GL_ARB_texture_float,
            // glViewportArrayv / glViewportIndexedf{,v} / glScissorArrayv / glScissorIndexed{,v} /
            // glDepthRangeArrayv / glDepthRangeIndexed / glGetFloati_v / glGetDoublei_v, over the
            // 16 viewports GL_MAX_VIEWPORTS reports.
            E_GL_ARB_viewport_array,
            // Advertised with GL_NUM_PROGRAM_BINARY_FORMATS = 0, which the
            // extension explicitly permits. It is also the only thing that
            // exposes glProgramParameteri before GL 4.1.
            E_GL_ARB_get_program_binary};
        // Vulkan's drawIndirectFirstInstance feature is optional. Direct base-instance calls work
        // without it, but ARB_base_instance also promises non-zero firstInstance in GPU indirect
        // commands; the renderer supplies true only when that word is legal and gl_InstanceID can
        // be rebased to OpenGL's zero-based semantics.
        if (nonZeroIndirectBaseInstanceSupported) {
            extensions.push_back(E_GL_ARB_base_instance);
        }
        if (shaderSubgroupSupported && !MG_Config::Features.MagmaDisableSubgroup) {
            extensions.push_back(E_GL_KHR_shader_subgroup);
        }
        // GL_KHR_parallel_shader_compile is MobileGL's own capability, not the Vulkan
        // device's: the compiler threads belong to MobileGL's shader pool and
        // glCompileShader/glLinkProgram are serviced entirely inside the frontend, so there
        // is no device feature to condition this on.
        //
        // Gated on the async flag deliberately, and this is the whole reason the gate
        // exists. Advertising the string is the one part of asynchronous compilation that a
        // recorded trace can never cover: Iris and Sodium change their SUBMISSION SCHEDULE
        // the moment they see it - they enqueue whole pipeline batches and poll
        // GL_COMPLETION_STATUS_KHR instead of compiling one program at a time - so
        // MOBILEGL_ASYNC_SHADER_COMPILE=0 has to withdraw the application-visible behaviour
        // change as well as the threading, or the kill switch would only be half a switch.
        if (MG_Util::Async::AsyncShaderCompileEnabled()) {
            extensions.push_back(E_GL_KHR_parallel_shader_compile);
        }
        // GL_ARB_gpu_shader_fp64 is opt-in (MOBILEGL_ADVERTISE_FP64), and stays opt-in even on a
        // device that HAS shaderFloat64. Every `double` in a shader compiles and runs either way
        // - narrowed to 32 bits where the device has no 64-bit floats, kept whole where it does -
        // so an application that simply uses doubles needs nothing advertised. What the extension
        // additionally promises is the whole GL_ARB_gpu_shader_fp64 SURFACE (glUniform*d
        // conformance, the fp64 built-ins, the state queries), and turning the string on is a
        // decision about all of it rather than about the shader path alone.
        if (MG_Config::Features.AdvertiseFp64) {
            extensions.push_back(E_GL_ARB_gpu_shader_fp64);
        }
        // GL_ARB_timer_query gates MC's F3 GPU% (LWJGL checks the extension string);
        // only advertised when the device actually supports timestamp queries and the
        // MOBILEGL_DISABLE_TIMERQUERY escape hatch is off.
        if (timerQueriesSupported && !MG_Config::Features.DisableTimerQuery) {
            extensions.push_back(E_GL_ARB_timer_query);
        }
        // Only advertised when the samplerAnisotropy device feature was granted: without it the
        // sampler state is accepted but never applied, and an app trusting the string (LWJGL builds
        // GLCapabilities from it) would think it enabled anisotropic filtering.
        if (anisotropicFilteringSupported) {
            extensions.push_back(E_GL_EXT_texture_filter_anisotropic);
            extensions.push_back(E_GL_ARB_texture_filter_anisotropic);
        }
        // A cube map array is a 6n-layer VkImage viewed as VK_IMAGE_VIEW_TYPE_CUBE_ARRAY, and that
        // view type cannot be created without the imageCubeArray device feature - so the string
        // follows the feature, not the version, exactly as the per-layer attachment bit does.
        //
        // Named for the application's benefit rather than the suite's: measured on Adreno 830,
        // KHR-GL43.texture_gather.plain-gather-*-cube-array already passed without the string, so
        // this unlocks no conformance case. It is advertised because the feature is real and
        // because an application that feature-detects cube map arrays off the string (rather than
        // off the 4.0 version) would otherwise decline a path this backend serves.
        if (cubeMapArraySupported) {
            extensions.push_back(E_GL_ARB_texture_cube_map_array);
        }
        return extensions;
    }

    String FormatBackendAPIVersionString(const String& deviceName, const String& vulkanApiVersionString,
                                         const String& driverVersionString) {
        // Format:
        // <GPU Name>, Vulkan <Vulkan Version>, Driver <Driver Version>
        return deviceName + ", Vulkan " + vulkanApiVersionString + ", Driver " + driverVersionString;
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
            funcsTable.GL.MultiDrawArrays = MultiDrawArrays;
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
            funcsTable.GL.ClearNamedFramebufferiv = ClearNamedFramebufferiv;
            funcsTable.GL.ClearNamedFramebufferuiv = ClearNamedFramebufferuiv;
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
            funcsTable.GL.ShaderStorageBlockBinding = ShaderStorageBlockBinding;
            funcsTable.GL.FenceSync = FenceSync;
            funcsTable.GL.ClientWaitSync = ClientWaitSync;
            funcsTable.GL.WaitSync = WaitSync;
            funcsTable.GL.DeleteSync = DeleteSync;
            funcsTable.GL.GetSyncStatus = GetSyncStatus;
            // Optional timer-query group: left null (the frontend then falls
            // back) when disabled via MOBILEGL_DISABLE_TIMERQUERY. The hooks
            // themselves additionally degrade to null handles when the device
            // lacks timestamp support.
            if (!MG_Config::Features.DisableTimerQuery) {
                funcsTable.GL.IsTimerQuerySupported = IsTimerQuerySupported;
                funcsTable.GL.BeginTimeElapsedQuery = BeginTimeElapsedQuery;
                funcsTable.GL.EndTimeElapsedQuery = EndTimeElapsedQuery;
                funcsTable.GL.QueryCounterTimestamp = QueryCounterTimestamp;
                funcsTable.GL.IsQueryResultAvailable = IsQueryResultAvailable;
                funcsTable.GL.GetQueryResult64 = GetQueryResult64;
                funcsTable.GL.DeleteBackendQuery = DeleteBackendQuery;
                funcsTable.GL.GetGpuTimestampNs = GetGpuTimestampNs;
            }
            // Occlusion queries share the handle-based result/delete entries, which must
            // exist even when timer queries are disabled.
            funcsTable.GL.BeginOcclusionQuery = BeginOcclusionQuery;
            funcsTable.GL.EndOcclusionQuery = EndOcclusionQuery;
            funcsTable.GL.BeginXfbPrimitivesQuery = BeginXfbPrimitivesQuery;
            funcsTable.GL.EndXfbPrimitivesQuery = EndXfbPrimitivesQuery;
            funcsTable.GL.IsQueryResultAvailable = IsQueryResultAvailable;
            funcsTable.GL.GetQueryResult64 = GetQueryResult64;
            funcsTable.GL.DeleteBackendQuery = DeleteBackendQuery;
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
        if (MG_State::pGLContext) {
            MG_State::pGLContext->InvalidateCompileEnv();
        }
        MutableFormatCapabilities().Clear();
    }

    void BackendObject_DirectVulkan::UpdateAdvertisedExtensions() {
        // GL_ARB_timer_query gates MC's F3 GPU% (LWJGL checks the extension
        // string). InitCapabilities runs after InitWindowSurface has created
        // and initialized the renderer, so the advertisement can be gated on
        // real device timestamp support. ApplyVulkanCapabilitiesForTesting may
        // run without a renderer; no timer query is advertised then. Rebuilding
        // the whole list keeps re-runs idempotent.
        // The opt-in emulated compute path (SubgroupSupportPolicy.h) carries the
        // extension by itself on devices with no native subgroup support at all; a
        // device with native subgroups always advertises - and uses - those.
        const Bool subgroupSupportAdvertised =
            m_vulkanCaps.SupportsShaderSubgroup ||
            ShouldEmulateSubgroups(m_vulkanCaps.SupportsShaderSubgroup);
        m_rendererInfo.RendererGLInfo.Extensions = BuildAdvertisedExtensions(
            subgroupSupportAdvertised, pVulkanRenderer && pVulkanRenderer->IsTimerQuerySupported(),
            pVulkanRenderer && pVulkanRenderer->IsSamplerAnisotropySupported(),
            pVulkanRenderer && pVulkanRenderer->IsNonZeroIndirectBaseInstanceSupported(),
            m_vulkanCaps.SupportsImageCubeArray);
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
        m_dynamicParameters.ShaderStorageBufferOffsetAlignment = m_vulkanCaps.ShaderStorageBufferOffsetAlignment;
        m_dynamicParameters.AliasedLineWidthRangeMin = m_vulkanCaps.AliasedLineWidthRangeMin;
        m_dynamicParameters.AliasedLineWidthRangeMax = m_vulkanCaps.AliasedLineWidthRangeMax;
        // Without the samplerAnisotropy feature the limit is unusable, so report 1.0 (no anisotropy)
        // rather than a maximum the sampler manager will never apply.
        m_dynamicParameters.MaxTextureMaxAnisotropy =
            (pVulkanRenderer && pVulkanRenderer->IsSamplerAnisotropySupported()) ? m_vulkanCaps.MaxSamplerAnisotropy
                                                                                 : 1.0f;
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
        const Int maxSupportedTextureUnits = static_cast<Int>(MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS);
        // GL_MAX_TEXTURE_IMAGE_UNITS is a *per-stage* sampler limit. Adreno/Qualcomm report a huge
        // maxPerStageDescriptorSampledImages (descriptor-indexing scale), so clamping it only to our
        // combined array capacity (192) still advertises 192 per stage. Host code treats this value as
        // an array bound: Minecraft's Blaze3D GlStateManager.TEXTURES[] holds 128 entries and Iris
        // iterates [0, GL_MAX_TEXTURE_IMAGE_UNITS) over it (CompositeRenderer.renderAll), so any value
        // > 128 throws ArrayIndexOutOfBoundsException. Match desktop drivers (32) for the per-stage
        // limits while keeping the combined limit at our texture-unit array capacity.
        constexpr Int maxPerStageTextureUnits =
            static_cast<Int>(MG_State::GLState::TextureState::MAX_PER_STAGE_TEXTURE_IMAGE_UNITS);
        m_dynamicParameters.MaxTextureImageUnits = std::min(m_vulkanCaps.MaxTextureImageUnits, maxPerStageTextureUnits);
        m_dynamicParameters.MaxVertexTextureImageUnits =
            std::min(m_vulkanCaps.MaxVertexTextureImageUnits, maxPerStageTextureUnits);
        m_dynamicParameters.MaxComputeTextureImageUnits =
            std::min(m_vulkanCaps.MaxComputeTextureImageUnits, maxPerStageTextureUnits);
        m_dynamicParameters.MaxCombinedTextureImageUnits =
            std::min(m_vulkanCaps.MaxCombinedTextureImageUnits, maxSupportedTextureUnits);
        // Never advertise more attributes than the state layer can store: the current-value array and
        // the Uint32 attribute masks the draw path passes around are both bounded by MAX_VERTEX_ATTRIBS.
        m_dynamicParameters.MaxVertexAttribs = std::min(
            m_vulkanCaps.MaxVertexAttribs, static_cast<Int>(MG_State::GLState::VertexArrayObject::MAX_VERTEX_ATTRIBS));
        // Vulkan descriptor limits are not GL limits, and a GL application reads an advertised
        // limit as an amount it may actually USE. Adreno answers the per-stage/per-set descriptor
        // queries at descriptor-indexing scale - the same driver whose
        // GL_MAX_SHADER_STORAGE_BLOCK_SIZE is clamped from 2147483647 further down - so
        // KHR-GL44.multi_bind.dispatch_bind_buffers_base read GL_MAX_COMPUTE_UNIFORM_BLOCKS,
        // created that many buffers and spliced that many UBO declarations into a single compute
        // shader: ~14 s of allocation, then death on std::bad_alloc. Its sibling
        // dispatch_bind_buffers_range hard-codes 4 buffers and passes, which is the clean
        // discriminator. Every ceiling below is far above what any desktop driver advertises for
        // these (84-96 for the binding families) and far below a descriptor-indexing count, so it
        // can only lower a limit that was never usable in the first place. The zero floor is not
        // decoration: a driver reporting UINT32_MAX used to arrive here as -1.
        const auto clampLimit = [](const char* name, Int reported, Int ceiling) {
            const Int clamped = std::min(std::max(reported, 0), ceiling);
            if (clamped != reported) {
                MGLOG_I("DirectVulkan: clamped %s from %d to %d", name, reported, clamped);
            }
            return clamped;
        };
        // GL 4.6 required minimums, for the record: MAX_COMPUTE_UNIFORM_BLOCKS 12,
        // MAX_COMPUTE/COMBINED_SHADER_STORAGE_BLOCKS 8, MAX_SHADER_STORAGE_BUFFER_BINDINGS 8,
        // MAX_UNIFORM_BUFFER_BINDINGS 84, MAX_TEXTURE_BUFFER_SIZE 65536.
        constexpr Int kMaxAdvertisedBufferBlocks = 256;
        constexpr Int kMaxAdvertisedTextureBufferSize = 1 << 27; // texels; what desktop GL reports
        m_dynamicParameters.MaxComputeShaderStorageBlocks =
            clampLimit("GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS", m_vulkanCaps.MaxComputeShaderStorageBlocks,
                       kMaxAdvertisedBufferBlocks);
        m_dynamicParameters.MaxCombinedShaderStorageBlocks =
            clampLimit("GL_MAX_COMBINED_SHADER_STORAGE_BLOCKS", m_vulkanCaps.MaxCombinedShaderStorageBlocks,
                       kMaxAdvertisedBufferBlocks);
        m_dynamicParameters.MaxComputeUniformBlocks =
            clampLimit("GL_MAX_COMPUTE_UNIFORM_BLOCKS", m_vulkanCaps.MaxComputeUniformBlocks,
                       kMaxAdvertisedBufferBlocks);
        m_dynamicParameters.MaxComputeWorkGroupInvocations = m_vulkanCaps.MaxComputeWorkGroupInvocations;
        m_dynamicParameters.MaxShaderStorageBufferBindings =
            clampLimit("GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS", m_vulkanCaps.MaxShaderStorageBufferBindings,
                       kMaxAdvertisedBufferBlocks);
        // Per-stage GL_MAX_*_SHADER_STORAGE_BLOCKS. Vulkan has one descriptor limit for every
        // stage (maxPerStageDescriptorStorageBuffers, which is what MaxComputeShaderStorageBlocks
        // carries), so the stage limits differ only by whether the stage can have blocks at all.
        //
        // Deliberately NOT gated on vertexPipelineStoresAndAtomics, unlike the per-stage image
        // uniforms below. That gate reads as the obvious one and is wrong here in practice: a
        // Mali-G925-Immortalis reports vertexPipelineStoresAndAtomics=false (supported AND
        // enabled) and yet runs all 433 KHR-GL43.constant_expressions.*_tess_* cases correctly
        // through this backend - those write their result through a storage block declared in a
        // tessellation stage. Gating would report 0 and turn 433 passing cases into
        // "unsupported", removing function that demonstrably works.
        //
        // The asymmetry with DirectGLES is real and is the point. There, 0 prevents a program
        // the driver refuses outright at link time; the honest limit converts a silent
        // wrong-render into a capability an application can route around. Here there is no such
        // failure to prevent, so the limit stays at what the device can address. If a Vulkan
        // device is ever found that genuinely rejects such a pipeline, the gate belongs at
        // pipeline creation where the rejection is observable, not on a feature bit this driver
        // reports inaccurately.
        {
            const Int maxPerStageStorageBlocks =
                std::min(std::max(m_dynamicParameters.MaxComputeShaderStorageBlocks, 0),
                         std::min(std::max(m_dynamicParameters.MaxCombinedShaderStorageBlocks, 0),
                                  std::max(m_dynamicParameters.MaxShaderStorageBufferBindings, 0)));
            m_dynamicParameters.MaxVertexShaderStorageBlocks = maxPerStageStorageBlocks;
            m_dynamicParameters.MaxTessControlShaderStorageBlocks = maxPerStageStorageBlocks;
            m_dynamicParameters.MaxTessEvaluationShaderStorageBlocks = maxPerStageStorageBlocks;
            // The one hard capability in the set: no geometry stage means no blocks in it.
            m_dynamicParameters.MaxGeometryShaderStorageBlocks =
                m_vulkanCaps.SupportsGeometryShader ? maxPerStageStorageBlocks : 0;
            m_dynamicParameters.MaxFragmentShaderStorageBlocks = maxPerStageStorageBlocks;
        }
        m_dynamicParameters.MaxTextureBufferSize = clampLimit(
            "GL_MAX_TEXTURE_BUFFER_SIZE", m_vulkanCaps.MaxTextureBufferSize, kMaxAdvertisedTextureBufferSize);
        m_dynamicParameters.TextureBufferOffsetAlignment = m_vulkanCaps.TextureBufferOffsetAlignment;
        m_dynamicParameters.MaxUniformBufferBindings = clampLimit(
            "GL_MAX_UNIFORM_BUFFER_BINDINGS", m_vulkanCaps.MaxUniformBufferBindings, kMaxAdvertisedBufferBlocks);
        m_dynamicParameters.MaxUniformBlockSize = m_vulkanCaps.MaxUniformBlockSize;
        m_dynamicParameters.MaxImageUnits = std::max(std::min(m_vulkanCaps.MaxImageUnits, maxSupportedTextureUnits), 0);
        m_dynamicParameters.MaxCombinedImageUniforms = std::max(m_vulkanCaps.MaxCombinedImageUniforms, 0);
        const Int maxPerStageImageUniforms =
            std::min(m_dynamicParameters.MaxImageUnits, m_dynamicParameters.MaxCombinedImageUniforms);
        // Vulkan uses one descriptor limit for every stage, but non-compute stores/atomics are
        // optional device features. VulkanRenderer enables each feature whenever the physical
        // device reports it, so these are the exact limits the logical device can compile and run.
        m_dynamicParameters.MaxVertexImageUniforms =
            m_vulkanCaps.SupportsVertexPipelineStoresAndAtomics ? maxPerStageImageUniforms : 0;
        m_dynamicParameters.MaxGeometryImageUniforms =
            m_vulkanCaps.SupportsVertexPipelineStoresAndAtomics && m_vulkanCaps.SupportsGeometryShader
                ? maxPerStageImageUniforms
                : 0;
        m_dynamicParameters.MaxFragmentImageUniforms =
            m_vulkanCaps.SupportsFragmentStoresAndAtomics ? maxPerStageImageUniforms : 0;
        m_dynamicParameters.MaxComputeImageUniforms =
            std::min(std::max(m_vulkanCaps.MaxComputeImageUniforms, 0), maxPerStageImageUniforms);
        const Int maxSupportedDrawBuffers = static_cast<Int>(MG_State::GLState::FramebufferObject::MAX_DRAW_BUFFERS);
        m_dynamicParameters.MaxDrawBuffers = std::min(m_vulkanCaps.MaxDrawBuffers, maxSupportedDrawBuffers);
        m_dynamicParameters.MaxColorAttachments = std::min(m_vulkanCaps.MaxColorAttachments, maxSupportedDrawBuffers);
        // Same shape as the image-uniform limits three lines above: maxClipDistances is reported
        // by every device, but declaring ClipDistance in a module needs the shaderClipDistance
        // FEATURE, which VulkanRenderer enables exactly where the physical device has it. Without
        // it the limit describes a capacity no shader may use, so report none.
        m_dynamicParameters.MaxClipDistances =
            m_vulkanCaps.SupportsShaderClipDistance ? std::max(m_vulkanCaps.MaxClipDistances, 0) : 0;
        // The cull pair, gated on its own feature. shaderCullDistance is separate from
        // shaderClipDistance and VulkanRenderer enables it independently, so it gets its own
        // gate rather than riding on the clip one.
        m_dynamicParameters.MaxCullDistances =
            m_vulkanCaps.SupportsShaderCullDistance ? std::max(m_vulkanCaps.MaxCullDistances, 0) : 0;
        // GL 4.6 core 11.1.3.10: the combined limit is at least as large as either half. A device
        // with only one of the two features must not report a combined capacity that implies the
        // other, so the gate is "either feature" and the value never drops below what is enabled.
        m_dynamicParameters.MaxCombinedClipAndCullDistances =
            (m_vulkanCaps.SupportsShaderClipDistance || m_vulkanCaps.SupportsShaderCullDistance)
                ? std::max({m_vulkanCaps.MaxCombinedClipAndCullDistances, m_dynamicParameters.MaxClipDistances,
                            m_dynamicParameters.MaxCullDistances})
                : 0;
        m_dynamicParameters.MaxViewports = m_vulkanCaps.MaxViewports;
        // Assigned explicitly rather than left to the struct's defaults, like every other
        // parameter here, so a second fill cannot inherit a stale value. GL_UNDEFINED_VERTEX is
        // the truthful answer for DirectVulkan and a legal one (GL 4.6 table 23.65): which vertex
        // provokes is chosen per pipeline by VulkanRenderer::SelectProvokingVertexMode out of
        // VK_EXT_provoking_vertex, provokingVertexModePerPipeline and the topology, so there is no
        // one convention to name. Vulkan's own default is FIRST, which is the opposite of the
        // GL_LAST_VERTEX_CONVENTION this used to claim unconditionally.
        m_dynamicParameters.LayerProvokingVertex = GL_UNDEFINED_VERTEX;
        m_dynamicParameters.ViewportIndexProvokingVertex = GL_UNDEFINED_VERTEX;
        m_dynamicParameters.MaxViewportWidth = m_vulkanCaps.MaxViewportWidth;
        m_dynamicParameters.MaxViewportHeight = m_vulkanCaps.MaxViewportHeight;
        m_dynamicParameters.ViewportBoundsRangeMin = m_vulkanCaps.ViewportBoundsRangeMin;
        m_dynamicParameters.ViewportBoundsRangeMax = m_vulkanCaps.ViewportBoundsRangeMax;
        m_dynamicParameters.ViewportSubpixelBits = m_vulkanCaps.ViewportSubpixelBits;
        m_dynamicParameters.MinFragmentInterpolationOffset =
            std::isfinite(m_vulkanCaps.MinFragmentInterpolationOffset) &&
                    m_vulkanCaps.MinFragmentInterpolationOffset <= -0.5f
                ? m_vulkanCaps.MinFragmentInterpolationOffset
                : -0.5f;
        m_dynamicParameters.MaxFragmentInterpolationOffset = 0.4375f;
        m_dynamicParameters.FragmentInterpolationOffsetBits = 4;
        if (m_vulkanCaps.FragmentInterpolationOffsetBits >= 4 &&
            std::isfinite(m_vulkanCaps.MaxFragmentInterpolationOffset)) {
            const Float requiredMaxOffset = 0.5f - std::ldexp(1.0f, -m_vulkanCaps.FragmentInterpolationOffsetBits);
            if (m_vulkanCaps.MaxFragmentInterpolationOffset >= requiredMaxOffset) {
                m_dynamicParameters.MaxFragmentInterpolationOffset = m_vulkanCaps.MaxFragmentInterpolationOffset;
                m_dynamicParameters.FragmentInterpolationOffsetBits = m_vulkanCaps.FragmentInterpolationOffsetBits;
            }
        }
        m_dynamicParameters.SupportsWideLines = m_vulkanCaps.SupportsWideLines;
        // A 2D or 2D multisample array texture is a VK_IMAGE_TYPE_2D image whose GL depth IS its
        // arrayLayers, so a GL layer is a Vulkan array layer with nothing to translate.
        // ResolveAttachmentBaseArrayLayer already passes the attachment's layer through. The other
        // layered targets are declared separately as their own machinery lands.
        {
            using DynParams = MG_Backend::DynamicBackendParameters;
            m_dynamicParameters.PerLayerFramebufferAttachmentTargets |=
                DynParams::PerLayerFramebufferAttachmentBit(TextureTarget::Texture2DArray) |
                DynParams::PerLayerFramebufferAttachmentBit(TextureTarget::Texture2DMultisampleArray);
            // A cube map array is one 2D image with arrayLayers = 6 * cubeCount, so a GL layer is a
            // Vulkan array layer here too - but the image cannot be created without imageCubeArray.
            // A 3D texture's GL layer is a z slice, which only a 2D view over a 2D-array-compatible
            // image can name. Optimistic: a format that refuses the flag is caught at image creation
            // and declines the slice view there, which the clear path handles as a soft miss.
            if (m_vulkanCaps.Supports2DArrayCompatible3DImages) {
                m_dynamicParameters.PerLayerFramebufferAttachmentTargets |=
                    DynParams::PerLayerFramebufferAttachmentBit(TextureTarget::Texture3D);
            }
            if (m_vulkanCaps.SupportsImageCubeArray) {
                m_dynamicParameters.PerLayerFramebufferAttachmentTargets |=
                    DynParams::PerLayerFramebufferAttachmentBit(TextureTarget::TextureCubeMapArray);
            }
        }
        // The device feature the whole fp64 story hangs off. With it, a module keeps its
        // OpCapability Float64 and real doubles reach the driver; without it the transpile
        // narrows every 64-bit float to 32 (ShaderTranspiler::DemoteFloat64Pass), because
        // VUID-VkShaderModuleCreateInfo-pCode-08740 forbids the capability outright and no
        // pipeline could be built from such a module. lavapipe reports it; Adreno and Mali both
        // report VK_FALSE, so on every real mobile device this is false and the demotion runs
        // exactly as it always has.
        m_dynamicParameters.SupportsShaderFloat64 = m_vulkanCaps.SupportsShaderFloat64;
        // shaderTessellationAndGeometryPointSize, both stage families from the one feature.
        // False arms the shared phase-B point-size demotion, whose modules then carry no
        // TessellationPointSize/GeometryPointSize capability and build without the feature.
        // MOBILEGL_POINT_SIZE_DEMOTION=1 pretends it is absent so the demotion can be
        // exercised on a healthy driver (lavapipe advertises the feature); =0 restores the
        // detected answer's declines.
        {
            Bool supportsStagePointSize = m_vulkanCaps.SupportsTessellationAndGeometryPointSize;
            switch (MG_Config::Features.PointSizeDemotion) {
            case MG_Config::QuirkOverride::ForceOn:
                MGLOG_I("DirectVulkan: MOBILEGL_POINT_SIZE_DEMOTION=1 - treating tessellation/geometry "
                        "gl_PointSize as unhosted so the demotion runs on this driver");
                supportsStagePointSize = false;
                break;
            case MG_Config::QuirkOverride::ForceOff:
                MGLOG_I("DirectVulkan: MOBILEGL_POINT_SIZE_DEMOTION=0 - keeping the built-in and the "
                        "plain declines regardless of the device feature");
                supportsStagePointSize = true;
                break;
            case MG_Config::QuirkOverride::Auto:
                break;
            }
            m_dynamicParameters.SupportsTessellationPointSize = supportsStagePointSize;
            m_dynamicParameters.SupportsGeometryPointSize = supportsStagePointSize;
        }
        // Never, on any device, and DELIBERATELY NOT COUPLED to the line above even though it
        // once tracked the same feature. It used to, because a `dvec` input needed Float64 to
        // exist in the module at all; a 64-bit vertex FETCH was already impossible
        // (VK_FORMAT_R64*_SFLOAT is optional and lavapipe reports zero bufferFeatures for all
        // four), so the attribute arrived as its 32-bit word pair and PackDoubleVertexInputsPass
        // bitcast it back.
        //
        // Re-coupling it does not work, and the reason is worth recording because it is not
        // obvious: this flag decides the VkFormat from the VAO ATTRIBUTE alone, and the attribute
        // does not know what the shader declared. glVertexAttribFormat(GL_DOUBLE) against a plain
        // `in vec4` is not only legal but the common case
        // (KHR-GL43.vertex_attrib_binding.basic-input-case4 does exactly that, and case5 adds
        // normalized=GL_TRUE), and advanced-bindingUpdate feeds a dvec3 the same way - GL defines
        // all of them as "doubles in memory, converted to float". Turning the flag on turns the
        // narrowing OFF for every one of them and the attributes come back unfetched.
        //
        // What keeps the two halves honest instead is a per-MODULE decision: a vertex module that
        // declares a 64-bit float INPUT is demoted whole, even where the backend has native fp64,
        // so `dvec` inputs are `vec` inputs on this backend exactly as they always were. See
        // ShaderCompiler::SanitizeAndOptimizeBinary.
        m_dynamicParameters.SupportsFloat64VertexAttributes = false;
        m_dynamicParameters.MaxShaderStorageBlockSize =
            std::min(m_vulkanCaps.MaxShaderStorageBlockSize, kMaxAdvertisedShaderStorageBlockSize);
        if (m_vulkanCaps.SupportsShaderSubgroup) {
            m_dynamicParameters.SubgroupSize = m_vulkanCaps.SubgroupSize;
            m_dynamicParameters.SubgroupSupportedStages = mapShaderStages(m_vulkanCaps.SubgroupSupportedStages);
            m_dynamicParameters.SubgroupSupportedFeatures =
                mapSubgroupFeatures(m_vulkanCaps.SubgroupSupportedOperations);
            m_dynamicParameters.SubgroupQuadOperationsInAllStages = m_vulkanCaps.SubgroupQuadOperationsInAllStages;
        } else if (ShouldEmulateSubgroups(m_vulkanCaps.SupportsShaderSubgroup)) {
            // MOBILEGL_MAGMA_EMULATE_SUBGROUP on a device with no native subgroups: the
            // advertised values describe the 32-lane virtual subgroup the compute
            // lowering implements (SubgroupSupportPolicy.h / EmulateSubgroupsPass).
            // GL requires the advertisement and the execution to agree, and on this
            // path the emulation is what executes; only the compute stage is offered.
            m_dynamicParameters.SubgroupSize = kEmulatedSubgroupSize;
            m_dynamicParameters.SubgroupSupportedStages = kEmulatedSubgroupStages;
            m_dynamicParameters.SubgroupSupportedFeatures = kEmulatedSubgroupFeatures;
            m_dynamicParameters.SubgroupQuadOperationsInAllStages = false;
            MGLOG_I("DirectVulkan: emulating 32-lane compute subgroups "
                    "(MOBILEGL_MAGMA_EMULATE_SUBGROUP, no native subgroup support)");
        } else {
            m_dynamicParameters.SubgroupSize = 0;
            m_dynamicParameters.SubgroupSupportedStages = 0;
            m_dynamicParameters.SubgroupSupportedFeatures = 0;
            m_dynamicParameters.SubgroupQuadOperationsInAllStages = false;
        }
        if (m_dynamicParameters.MaxShaderStorageBlockSize != m_vulkanCaps.MaxShaderStorageBlockSize) {
            MGLOG_I("DirectVulkan: clamped GL_MAX_SHADER_STORAGE_BLOCK_SIZE from %zu to %zu",
                    m_vulkanCaps.MaxShaderStorageBlockSize, m_dynamicParameters.MaxShaderStorageBlockSize);
        }
        switch (m_vulkanCaps.VendorId) {
        case 0x5143u: // VK_VENDOR_ID: Qualcomm
            m_dynamicParameters.GpuVendor = GpuVendorKind::Qualcomm;
            break;
        case 0x13B5u: // ARM
            m_dynamicParameters.GpuVendor = GpuVendorKind::Arm;
            break;
        case 0x10DEu: // NVIDIA
            m_dynamicParameters.GpuVendor = GpuVendorKind::Nvidia;
            break;
        case 0x1002u: // AMD
            m_dynamicParameters.GpuVendor = GpuVendorKind::Amd;
            break;
        case 0x8086u: // Intel
            m_dynamicParameters.GpuVendor = GpuVendorKind::Intel;
            break;
        case 0x1010u: // Imagination
            m_dynamicParameters.GpuVendor = GpuVendorKind::ImgTec;
            break;
        case 0x10005u: // Mesa software (lavapipe)
        case 0x1AE0u:  // Google (SwiftShader)
            m_dynamicParameters.GpuVendor = GpuVendorKind::Software;
            break;
        default:
            m_dynamicParameters.GpuVendor = GpuVendorKind::Unknown;
            break;
        }
    }
} // namespace MobileGL::MG_Backend::DirectVulkan
