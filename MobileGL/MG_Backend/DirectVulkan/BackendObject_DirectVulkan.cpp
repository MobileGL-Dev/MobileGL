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
            .RendererGLInfo = {.TargetGLVersion = {4, 0, 0},
                               .TargetGLSLVersion = {4, 6, 0},
                               // Baseline advertisement (no shader subgroup, no timer queries); a
                               // live backend reconciles its copy in UpdateAdvertisedExtensions.
                               .Extensions = BuildAdvertisedExtensions(false, false, false),
                               .IsCompatibilityProfile = false},
            .StaticBackendCapability = {.AllowVSOnlyPrograms = false}};
        return rendererInfo;
    }

    Vector<GLExtension> BuildAdvertisedExtensions(Bool shaderSubgroupSupported, Bool timerQueriesSupported,
                                                  Bool anisotropicFilteringSupported) {
        Vector<GLExtension> extensions = {
            V_OpenGL30, V_OpenGL31, V_OpenGL32, V_OpenGL33, V_OpenGL40, E_GL_ARB_draw_buffers_blend,
            E_GL_ARB_compute_shader, E_GL_ARB_shader_storage_buffer_object, E_GL_ARB_shader_image_load_store,
            E_GL_ARB_program_interface_query, E_GL_ARB_framebuffer_object, E_GL_ARB_multi_draw_indirect,
            E_GL_ARB_indirect_parameters, E_GL_EXT_framebuffer_object, E_GL_ARB_depth_texture, E_GL_ARB_buffer_storage,
            E_GL_ARB_texture_storage, E_GL_ARB_texture_storage_multisample, E_GL_ARB_texture_multisample,
            E_GL_ARB_clear_texture, E_GL_ARB_direct_state_access, E_GL_ARB_shader_draw_parameters,
            E_GL_ARB_gpu_shader_int64, E_GL_KHR_debug, E_GL_ARB_gpu_shader5, E_GL_ARB_multi_bind,
            E_GL_ARB_shading_language_420pack, E_GL_ARB_vertex_attrib_binding, E_GL_ARB_shader_image_size,
            E_GL_ARB_explicit_attrib_location,
            // Advertised with GL_NUM_PROGRAM_BINARY_FORMATS = 0, which the
            // extension explicitly permits. It is also the only thing that
            // exposes glProgramParameteri before GL 4.1.
            E_GL_ARB_get_program_binary};
        if (shaderSubgroupSupported && !MG_Config::Features.DisableSubgroup) {
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
            funcsTable.GL.GetInteger64i_v = GetInteger64i_v;
            funcsTable.GL.GetProgramiv = GetProgramiv;
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
        MutableFormatCapabilities().Clear();
    }

    void BackendObject_DirectVulkan::UpdateAdvertisedExtensions() {
        // GL_ARB_timer_query gates MC's F3 GPU% (LWJGL checks the extension
        // string). InitCapabilities runs after InitWindowSurface has created
        // and initialized the renderer, so the advertisement can be gated on
        // real device timestamp support. ApplyVulkanCapabilitiesForTesting may
        // run without a renderer; no timer query is advertised then. Rebuilding
        // the whole list keeps re-runs idempotent.
        m_rendererInfo.RendererGLInfo.Extensions = BuildAdvertisedExtensions(
            m_vulkanCaps.SupportsShaderSubgroup, pVulkanRenderer && pVulkanRenderer->IsTimerQuerySupported(),
            pVulkanRenderer && pVulkanRenderer->IsSamplerAnisotropySupported());
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
        m_dynamicParameters.MaxComputeShaderStorageBlocks = m_vulkanCaps.MaxComputeShaderStorageBlocks;
        m_dynamicParameters.MaxCombinedShaderStorageBlocks = m_vulkanCaps.MaxCombinedShaderStorageBlocks;
        m_dynamicParameters.MaxComputeUniformBlocks = m_vulkanCaps.MaxComputeUniformBlocks;
        m_dynamicParameters.MaxComputeWorkGroupInvocations = m_vulkanCaps.MaxComputeWorkGroupInvocations;
        m_dynamicParameters.MaxShaderStorageBufferBindings = m_vulkanCaps.MaxShaderStorageBufferBindings;
        m_dynamicParameters.MaxTextureBufferSize = m_vulkanCaps.MaxTextureBufferSize;
        m_dynamicParameters.TextureBufferOffsetAlignment = m_vulkanCaps.TextureBufferOffsetAlignment;
        m_dynamicParameters.MaxUniformBufferBindings = m_vulkanCaps.MaxUniformBufferBindings;
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
        m_dynamicParameters.MaxClipDistances = m_vulkanCaps.MaxClipDistances;
        m_dynamicParameters.MaxViewports = m_vulkanCaps.MaxViewports;
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
        m_dynamicParameters.SupportsFloat64VertexAttributes = m_vulkanCaps.SupportsShaderFloat64;
        m_dynamicParameters.MaxShaderStorageBlockSize =
            std::min(m_vulkanCaps.MaxShaderStorageBlockSize, kMaxAdvertisedShaderStorageBlockSize);
        if (m_vulkanCaps.SupportsShaderSubgroup) {
            m_dynamicParameters.SubgroupSize = m_vulkanCaps.SubgroupSize;
            m_dynamicParameters.SubgroupSupportedStages = mapShaderStages(m_vulkanCaps.SubgroupSupportedStages);
            m_dynamicParameters.SubgroupSupportedFeatures =
                mapSubgroupFeatures(m_vulkanCaps.SubgroupSupportedOperations);
            m_dynamicParameters.SubgroupQuadOperationsInAllStages = m_vulkanCaps.SubgroupQuadOperationsInAllStages;
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
