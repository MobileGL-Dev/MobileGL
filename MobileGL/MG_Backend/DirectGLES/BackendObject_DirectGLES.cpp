// MobileGL - MobileGL/MG_Backend/DirectGLES/BackendObject_DirectGLES.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "BackendObject_DirectGLES.h"
#include "MG_Backend/BackendObject.h"
#include <MG_Backend/DirectGLES/DirectGLES.h>
#include <MG_Backend/DirectGLES/Utils.h>
#include <MG_Util/BackendLoaders/OpenGL/Loader.h>
#include <MG_Util/Classifiers/TextureEnumClassifier.h>
#include <MG_Util/Converters/MGToGL/TextureEnumConverter.h>
#include <MG_Util/Texture/TextureFormatProcessor.h>
#include <format>

namespace MobileGL::MG_Backend::DirectGLES {
    namespace {
        Bool IsReleaseCurrentRequest(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) {
            return dpy == EGL_NO_DISPLAY && draw == EGL_NO_SURFACE && read == EGL_NO_SURFACE && ctx == EGL_NO_CONTEXT;
        }

        void ClearGLErrors(const MG_External::GLESFunctionsTable& gl) {
            if (!gl.glGetError) return;
            while (gl.glGetError() != GL_NO_ERROR) {
            }
        }

        Bool CheckNoGLError(const MG_External::GLESFunctionsTable& gl) {
            return !gl.glGetError || gl.glGetError() == GL_NO_ERROR;
        }

        GLenum GetTextureBindingQuery(TextureTarget target) {
            switch (target) {
            case TextureTarget::Texture2D:
                return GL_TEXTURE_BINDING_2D;
            case TextureTarget::Texture3D:
                return GL_TEXTURE_BINDING_3D;
            case TextureTarget::TextureCubeMap:
                return GL_TEXTURE_BINDING_CUBE_MAP;
            case TextureTarget::Texture2DArray:
                return GL_TEXTURE_BINDING_2D_ARRAY;
            case TextureTarget::TextureCubeMapArray:
                return GL_TEXTURE_BINDING_CUBE_MAP_ARRAY;
            case TextureTarget::Texture2DMultisample:
                return GL_TEXTURE_BINDING_2D_MULTISAMPLE;
            case TextureTarget::Texture2DMultisampleArray:
                return GL_TEXTURE_BINDING_2D_MULTISAMPLE_ARRAY;
            default:
                return GL_UNKNOWN_MGL;
            }
        }

        Bool IsGLESProbeTextureTarget(TextureTarget target) {
            switch (target) {
            case TextureTarget::Texture2D:
            case TextureTarget::Texture3D:
            case TextureTarget::TextureCubeMap:
            case TextureTarget::Texture2DArray:
            case TextureTarget::TextureCubeMapArray:
            case TextureTarget::Texture2DMultisample:
            case TextureTarget::Texture2DMultisampleArray:
                return true;
            default:
                return false;
            }
        }

        Bool IsGLESProbeMultisampleTarget(TextureTarget target) {
            return target == TextureTarget::Texture2DMultisample ||
                   target == TextureTarget::Texture2DMultisampleArray;
        }

        GLenum GetFramebufferAttachment(TextureInternalFormat format) {
            const Bool isDepth = MG_Util::IsDepthFormatInternalFormat(format);
            const Bool isStencil = MG_Util::IsStencilFormatInternalFormat(format);
            if (isDepth && isStencil) return GL_DEPTH_STENCIL_ATTACHMENT;
            if (isDepth) return GL_DEPTH_ATTACHMENT;
            if (isStencil) return GL_STENCIL_ATTACHMENT;
            return GL_COLOR_ATTACHMENT0;
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

        Bool IsDepthOnlyFormat(TextureInternalFormat format) {
            return MG_Util::IsDepthFormatInternalFormat(format) && !MG_Util::IsStencilFormatInternalFormat(format);
        }

        Bool IsFilterableFormat(TextureInternalFormat format) {
            const GLenum glFormat = MG_Util::ConvertTextureInternalFormatToGLEnum(format);
            GLenum normalizedInternalFormat = glFormat;
            GLenum imageFormat = GL_RGBA;
            GLenum imageType = GL_UNSIGNED_BYTE;
            MG_Util::TextureFormatProcessor::NormalizePixelFormat(
                glFormat, PixelFormatNormalizeOptionBit::None, &normalizedInternalFormat, &imageFormat, &imageType);
            return imageFormat != GL_RED_INTEGER && imageFormat != GL_RG_INTEGER && imageFormat != GL_RGB_INTEGER &&
                   imageFormat != GL_RGBA_INTEGER && !MG_Util::IsDepthFormatInternalFormat(format) &&
                   !MG_Util::IsStencilFormatInternalFormat(format);
        }

        FormatCapabilityFlags GetTextureFeatureCaps(TextureInternalFormat format, TextureTarget target) {
            FormatCapabilityFlags caps = FormatCapability::Creatable | FormatCapability::Sampled;
            if (IsFilterableFormat(format)) {
                caps |= FormatCapability::LinearFilter;
            }
            if (!IsGLESProbeMultisampleTarget(target) && !MG_Util::IsStencilFormatInternalFormat(format)) {
                caps |= FormatCapability::GenerateMipmap;
            }
            if (IsDepthOnlyFormat(format)) {
                caps |= FormatCapability::TextureShadow;
            }
            if (IsGLESProbeMultisampleTarget(target)) {
                caps |= FormatCapability::MultisampleTexture;
            }
            return caps;
        }

        FormatCapabilityFlags GetRenderbufferFeatureCaps(TextureInternalFormat format) {
            return FormatCapabilityFlags(FormatCapability::Creatable) | GetAttachmentCaps(format) |
                   FormatCapability::MultisampleRenderbuffer;
        }

        Bool ProbeFramebufferCompletenessForTexture(const MG_External::GLESFunctionsTable& gl,
                                                   TextureTarget target,
                                                   GLuint texture,
                                                   TextureInternalFormat format) {
            GLuint framebuffer = 0;
            GLint prevFramebuffer = 0;
            if (!gl.glGenFramebuffers || !gl.glBindFramebuffer || !gl.glCheckFramebufferStatus ||
                !gl.glDeleteFramebuffers) {
                return false;
            }

            gl.glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFramebuffer);
            gl.glGenFramebuffers(1, &framebuffer);
            gl.glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);

            const GLenum attachment = GetFramebufferAttachment(format);
            switch (target) {
            case TextureTarget::Texture2D:
                gl.glFramebufferTexture2D(GL_FRAMEBUFFER, attachment, GL_TEXTURE_2D, texture, 0);
                break;
            case TextureTarget::TextureCubeMap:
                gl.glFramebufferTexture2D(GL_FRAMEBUFFER, attachment, GL_TEXTURE_CUBE_MAP_POSITIVE_X, texture, 0);
                break;
            case TextureTarget::Texture3D:
            case TextureTarget::Texture2DArray:
            case TextureTarget::TextureCubeMapArray:
            case TextureTarget::Texture2DMultisampleArray:
                if (!gl.glFramebufferTextureLayer) {
                    gl.glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFramebuffer));
                    gl.glDeleteFramebuffers(1, &framebuffer);
                    return false;
                }
                gl.glFramebufferTextureLayer(GL_FRAMEBUFFER, attachment, texture, 0, 0);
                break;
            case TextureTarget::Texture2DMultisample:
                gl.glFramebufferTexture2D(GL_FRAMEBUFFER, attachment, GL_TEXTURE_2D_MULTISAMPLE, texture, 0);
                break;
            default:
                gl.glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFramebuffer));
                gl.glDeleteFramebuffers(1, &framebuffer);
                return false;
            }

            const Bool complete = gl.glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
            gl.glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFramebuffer));
            gl.glDeleteFramebuffers(1, &framebuffer);
            return complete;
        }

        Bool ProbeFramebufferCompletenessForRenderbuffer(const MG_External::GLESFunctionsTable& gl,
                                                        GLuint renderbuffer,
                                                        TextureInternalFormat format) {
            GLuint framebuffer = 0;
            GLint prevFramebuffer = 0;
            if (!gl.glGenFramebuffers || !gl.glBindFramebuffer || !gl.glFramebufferRenderbuffer ||
                !gl.glCheckFramebufferStatus || !gl.glDeleteFramebuffers) {
                return false;
            }

            gl.glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFramebuffer);
            gl.glGenFramebuffers(1, &framebuffer);
            gl.glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
            gl.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GetFramebufferAttachment(format), GL_RENDERBUFFER,
                                         renderbuffer);
            const Bool complete = gl.glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
            gl.glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFramebuffer));
            gl.glDeleteFramebuffers(1, &framebuffer);
            return complete;
        }

        Bool ProbeTexture(const MG_External::GLESFunctionsTable& gl, TextureTarget target, GLenum internalFormat,
                          GLenum imageFormat, GLenum imageType, TextureInternalFormat logicalFormat,
                          Bool* outRenderable) {
            if (!IsGLESProbeTextureTarget(target) || !gl.glGenTextures || !gl.glBindTexture || !gl.glDeleteTextures) {
                return false;
            }

            const GLenum glTarget = MG_Util::ConvertTextureTargetToGLEnum(target);
            const GLenum bindingQuery = GetTextureBindingQuery(target);
            if (glTarget == GL_UNKNOWN_MGL || bindingQuery == GL_UNKNOWN_MGL) {
                return false;
            }

            GLint previousBinding = 0;
            gl.glGetIntegerv(bindingQuery, &previousBinding);
            GLuint texture = 0;
            gl.glGenTextures(1, &texture);
            gl.glBindTexture(glTarget, texture);
            ClearGLErrors(gl);

            const Bool isMultisample = IsGLESProbeMultisampleTarget(target);
            if (isMultisample) {
                if (target == TextureTarget::Texture2DMultisample && gl.glTexStorage2DMultisample) {
                    gl.glTexStorage2DMultisample(glTarget, 1, internalFormat, 1, 1, GL_TRUE);
                } else if (target == TextureTarget::Texture2DMultisampleArray && gl.glTexStorage3DMultisample) {
                    gl.glTexStorage3DMultisample(glTarget, 1, internalFormat, 1, 1, 1, GL_TRUE);
                } else {
                    gl.glBindTexture(glTarget, static_cast<GLuint>(previousBinding));
                    gl.glDeleteTextures(1, &texture);
                    return false;
                }
            } else {
                if (gl.glTexParameteri) {
                    gl.glTexParameteri(glTarget, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                    gl.glTexParameteri(glTarget, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                }
                switch (target) {
                case TextureTarget::Texture2D:
                    gl.glTexImage2D(glTarget, 0, static_cast<GLint>(internalFormat), 2, 2, 0, imageFormat, imageType,
                                    nullptr);
                    break;
                case TextureTarget::TextureCubeMap:
                    for (GLenum face = GL_TEXTURE_CUBE_MAP_POSITIVE_X; face <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z; ++face) {
                        gl.glTexImage2D(face, 0, static_cast<GLint>(internalFormat), 2, 2, 0, imageFormat, imageType,
                                        nullptr);
                    }
                    break;
                case TextureTarget::Texture3D:
                    gl.glTexImage3D(glTarget, 0, static_cast<GLint>(internalFormat), 2, 2, 2, 0, imageFormat,
                                    imageType, nullptr);
                    break;
                case TextureTarget::Texture2DArray:
                    gl.glTexImage3D(glTarget, 0, static_cast<GLint>(internalFormat), 2, 2, 1, 0, imageFormat,
                                    imageType, nullptr);
                    break;
                case TextureTarget::TextureCubeMapArray:
                    gl.glTexImage3D(glTarget, 0, static_cast<GLint>(internalFormat), 2, 2, 6, 0, imageFormat,
                                    imageType, nullptr);
                    break;
                default:
                    break;
                }
            }

            const Bool created = CheckNoGLError(gl);
            Bool renderable = false;
            if (created) {
                renderable = ProbeFramebufferCompletenessForTexture(gl, target, texture, logicalFormat);
            }
            if (outRenderable) {
                *outRenderable = renderable;
            }
            gl.glBindTexture(glTarget, static_cast<GLuint>(previousBinding));
            gl.glDeleteTextures(1, &texture);
            ClearGLErrors(gl);
            return created;
        }

        Bool ProbeRenderbuffer(const MG_External::GLESFunctionsTable& gl,
                               GLenum internalFormat,
                               TextureInternalFormat logicalFormat,
                               Bool multisample,
                               Int samples) {
            if (!gl.glGenRenderbuffers || !gl.glBindRenderbuffer || !gl.glDeleteRenderbuffers) {
                return false;
            }

            GLint prevRenderbuffer = 0;
            gl.glGetIntegerv(GL_RENDERBUFFER_BINDING, &prevRenderbuffer);
            GLuint renderbuffer = 0;
            gl.glGenRenderbuffers(1, &renderbuffer);
            gl.glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
            ClearGLErrors(gl);
            if (multisample) {
                if (!gl.glRenderbufferStorageMultisample) {
                    gl.glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(prevRenderbuffer));
                    gl.glDeleteRenderbuffers(1, &renderbuffer);
                    return false;
                }
                gl.glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, internalFormat, 1, 1);
            } else {
                gl.glRenderbufferStorage(GL_RENDERBUFFER, internalFormat, 1, 1);
            }
            const Bool created = CheckNoGLError(gl);
            const Bool complete = created && ProbeFramebufferCompletenessForRenderbuffer(gl, renderbuffer, logicalFormat);
            gl.glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(prevRenderbuffer));
            gl.glDeleteRenderbuffers(1, &renderbuffer);
            ClearGLErrors(gl);
            return complete;
        }

        Vector<Int> ProbeRenderbufferSampleCounts(const MG_External::GLESFunctionsTable& gl,
                                                  GLenum internalFormat,
                                                  TextureInternalFormat logicalFormat,
                                                  Int maxSamples) {
            Vector<Int> sampleCounts;
            for (Int samples = std::max(maxSamples, 1); samples > 1; samples >>= 1) {
                if (ProbeRenderbuffer(gl, internalFormat, logicalFormat, true, samples)) {
                    sampleCounts.push_back(samples);
                }
            }
            sampleCounts.push_back(1);
            return sampleCounts;
        }

        void ProbeGLESFormatCapabilities(const MG_External::GLESFunctionsTable& gl,
                                         FormatCapabilityCache& cache,
                                         const DynamicBackendParameters& dynamicParameters) {
            cache.Clear();
            for (SizeT formatIndex = 0; formatIndex < kFormatCapabilityFormatCount; ++formatIndex) {
                const auto logicalFormat = static_cast<TextureInternalFormat>(formatIndex);
                GLenum requestedInternalFormat = MG_Util::ConvertTextureInternalFormatToGLEnum(logicalFormat);
                if (requestedInternalFormat == GL_UNKNOWN_MGL) {
                    continue;
                }

                GLenum backendInternalFormat = requestedInternalFormat;
                GLenum imageFormat = GL_RGBA;
                GLenum imageType = GL_UNSIGNED_BYTE;
                TextureImpl::GenerateTextureFormatInfo(logicalFormat, &backendInternalFormat, &imageFormat,
                                                       &imageType);
                const Bool isCaveat = backendInternalFormat != requestedInternalFormat;

                for (SizeT targetIndex = 0; targetIndex < kFormatCapabilityTextureTargetCount; ++targetIndex) {
                    const auto target = static_cast<TextureTarget>(targetIndex);
                    Bool renderable = false;
                    if (!ProbeTexture(gl, target, backendInternalFormat, imageFormat, imageType, logicalFormat,
                                      &renderable)) {
                        continue;
                    }

                    FormatCapabilityFlags caps = GetTextureFeatureCaps(logicalFormat, target);
                    if (renderable) {
                        caps |= GetAttachmentCaps(logicalFormat);
                        if (target == TextureTarget::Texture3D || target == TextureTarget::Texture2DArray ||
                            target == TextureTarget::TextureCubeMapArray ||
                            target == TextureTarget::Texture2DMultisampleArray) {
                            caps |= FormatCapability::FramebufferLayered;
                        }
                    }
                    auto& table = isCaveat ? cache.CaveatCaps : cache.FullCaps;
                    table[targetIndex][formatIndex] |= caps;

                    if (IsGLESProbeMultisampleTarget(target)) {
                        cache.SampleCounts[targetIndex][formatIndex] = {1};
                    }
                }

                const SizeT renderbufferTargetIndex = GetRenderbufferFormatCapabilityTargetIndex();
                if (ProbeRenderbuffer(gl, backendInternalFormat, logicalFormat, false, 1)) {
                    auto& table = isCaveat ? cache.CaveatCaps : cache.FullCaps;
                    table[renderbufferTargetIndex][formatIndex] |= GetRenderbufferFeatureCaps(logicalFormat);

                    const Bool isDepth = MG_Util::IsDepthFormatInternalFormat(logicalFormat);
                    const Bool isStencil = MG_Util::IsStencilFormatInternalFormat(logicalFormat);
                    const Bool isInteger = imageFormat == GL_RED_INTEGER || imageFormat == GL_RG_INTEGER ||
                                           imageFormat == GL_RGB_INTEGER || imageFormat == GL_RGBA_INTEGER;
                    Int maxSamples = dynamicParameters.MaxColorTextureSamples;
                    if (isDepth || isStencil) {
                        maxSamples = dynamicParameters.MaxDepthTextureSamples;
                    } else if (isInteger) {
                        maxSamples = dynamicParameters.MaxIntegerSamples;
                    }
                    cache.SampleCounts[renderbufferTargetIndex][formatIndex] =
                        ProbeRenderbufferSampleCounts(gl, backendInternalFormat, logicalFormat, maxSamples);
                }
            }
        }
    } // namespace

    BackendObject_DirectGLES::~BackendObject_DirectGLES() {
        DestroyEGLContext();
    }

    Bool BackendObject_DirectGLES::InitWindowSurface() {
        // Only use EGL for now
        auto nativeWindow = reinterpret_cast<NativeWindowType>(m_windowHandle.Handle);
        if (!DirectGLES::InitWindowSurface(nativeWindow)) {
            MGLOG_E("Failed to initialize window surface for DirectGLES backend");
            return false;
        }
        return true;
    }

    void BackendObject_DirectGLES::Initialize() {
        MG_Util::BackendLoader::AcquireEGLFunctions(m_EGLFunctions);
        MG_Util::BackendLoader::AcquireGLESFunctions(m_GLESFunctions, m_EGLFunctions.eglGetProcAddress);
        DirectGLES::SetEGLFuncsTable(m_EGLFunctions);
        DirectGLES::SetGLESFuncsTable(m_GLESFunctions);
        m_initialized = true;
    }

    Bool BackendObject_DirectGLES::InitCapabilities() {
        if (!m_initialized) {
            MGLOG_E("DirectGLES backend not initialized");
            return false;
        }

        if (!MG_Util::BackendLoader::FillInGLESCapabilities(m_GLESCapabilities, m_GLESFunctions)) {
            MGLOG_E("Failed to fill in GLES capabilities for DirectGLES backend");
            return false;
        }
        DirectGLES::SetGLESCapabilities(m_GLESCapabilities);
        UpdateDynamicBackendParameters();
        ProbeGLESFormatCapabilities(m_GLESFunctions, MutableFormatCapabilities(), m_dynamicParameters);
        return true;
    }

    Bool BackendObject_DirectGLES::InitializeEGLDisplay(EGLDisplay dpy, EGLint* major, EGLint* minor) {
        if (!m_initialized) {
            MGLOG_E("DirectGLES backend not initialized");
            return false;
        }
        return BackendObject::InitializeEGLDisplay(dpy, major, minor);
    }

    Bool BackendObject_DirectGLES::CreateEGLWindowSurface(const WindowHandle& handle) {
        const std::lock_guard<std::recursive_mutex> lock(m_eglStateMutex);
        if (!m_initialized) {
            MGLOG_E("DirectGLES backend not initialized");
            return false;
        }

        if ((handle.Backend != WindowBackend::Android && handle.Backend != WindowBackend::X11) || !handle.Handle) {
            MGLOG_E("DirectGLES backend only supports Android and X11 native windows");
            return false;
        }

        const Bool sameHandle = m_eglSurfaceInitialized && m_eglSurfaceKind == SurfaceKind::Window &&
                                m_windowHandle.Backend == handle.Backend && m_windowHandle.Handle == handle.Handle;
        if (sameHandle) {
            return true;
        }

        if (m_eglSurfaceInitialized) {
            DestroyEGLContext();
            ResetEGLRuntimeState();
        }

        return BackendObject::CreateEGLWindowSurface(handle);
    }

    Bool BackendObject_DirectGLES::CreateEGLPbufferSurface(EGLint width, EGLint height) {
        const std::lock_guard<std::recursive_mutex> lock(m_eglStateMutex);
        if (!m_initialized) {
            MGLOG_E("DirectGLES backend not initialized");
            return false;
        }

        if (m_eglSurfaceInitialized) {
            DestroyEGLContext();
            ResetEGLRuntimeState();
        }

        return BackendObject::CreateEGLPbufferSurface(width, height);
    }

    Bool BackendObject_DirectGLES::InitPbufferSurface(EGLint width, EGLint height) {
        return DirectGLES::InitPbufferSurface(width, height);
    }

    Bool BackendObject_DirectGLES::MakeEGLCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) {
        if (IsReleaseCurrentRequest(dpy, draw, read, ctx)) {
            return BackendObject::MakeEGLCurrent(dpy, draw, read, ctx);
        }

        return BackendObject::MakeEGLCurrent(dpy, draw, read, ctx);
    }

    Bool BackendObject_DirectGLES::SwapEGLBuffers(EGLDisplay dpy, EGLSurface draw) {
        return BackendObject::SwapEGLBuffers(dpy, draw);
    }

    void BackendObject_DirectGLES::ReleaseEGLResources() {
        const std::lock_guard<std::recursive_mutex> lock(m_eglStateMutex);
        DestroyEGLContext();
        BackendObject::ReleaseEGLResources();
    }

    const RendererInfo& BackendObject_DirectGLES::GetRendererInfo() const {
        static RendererInfo RendererInfo = {
            .RendererName = "Espryt",            // Renderer Name
            .BackendName = "Direct (OpenGL ES)", // Backend Name
            .ExtraVendor = Nullopt,              // Extra vendor
            .RendererGLInfo =
                {
                    .TargetGLVersion = {3, 3, 0},                      // Target OpenGL Version
                    .TargetGLSLVersion = {4, 6, 0},                    // Target Shading Language Version
                    .Extensions = {V_OpenGL30, V_OpenGL31, V_OpenGL32, // OpenGL Extensions
                                   V_OpenGL33, E_GL_ARB_draw_buffers_blend, E_GL_ARB_compute_shader,
                                   E_GL_ARB_shader_storage_buffer_object, E_GL_ARB_shader_image_load_store,
                                   E_GL_ARB_program_interface_query, E_GL_ARB_framebuffer_object,
                                   E_GL_EXT_framebuffer_object, E_GL_ARB_depth_texture, E_GL_ARB_buffer_storage,
                                   E_GL_ARB_texture_storage, E_GL_ARB_direct_state_access,
                                   E_GL_ARB_multi_draw_indirect, E_GL_ARB_indirect_parameters,
                                   E_GL_ARB_shader_draw_parameters},
                    .IsCompatibilityProfile = false // Is Compatibility Profile
                },
            .StaticBackendCapability = {.AllowVSOnlyPrograms = false} // Backend Capability
        };
        return RendererInfo;
    }

    String BackendObject_DirectGLES::GetBackendAPIVersionString() const {
        if (!m_initialized) {
            return "<uninitialized DirectGLES backend>";
        }
        // Format:
        // <OpenGL ES Renderer>, OpenGL ES <OpenGL ES Version>
        String versionString = std::format("{}, OpenGL ES {}.{}", m_GLESCapabilities.GLESRendererString,
                                           m_GLESCapabilities.GLESVersion.Major, m_GLESCapabilities.GLESVersion.Minor);
        return versionString;
    }

    BackendType BackendObject_DirectGLES::GetBackendType() const {
        return BackendType::DirectGLES;
    }

    const GlobalBackendFunctionsTable& BackendObject_DirectGLES::GetBackendFunctions() const {
        static GlobalBackendFunctionsTable funcsTable;
        static Bool funcsTableInitialized = false;
        if (!funcsTableInitialized) {
            funcsTable.Present = DirectGLES::Present;
            funcsTable.GL.DrawArrays = DrawArrays;
            funcsTable.GL.DrawElements = DrawElements;
            funcsTable.GL.DrawElementsBaseVertex = DrawElementsBaseVertex;
            funcsTable.GL.MultiDrawElements = MultiDrawElements;
            funcsTable.GL.MultiDrawElementsBaseVertex = MultiDrawElementsBaseVertex;
            funcsTable.GL.MultiDrawElementsIndirect = MultiDrawElementsIndirect;
            funcsTable.GL.MultiDrawElementsIndirectCount = MultiDrawElementsIndirectCount;
            funcsTable.GL.MultiDrawArraysIndirect = MultiDrawArraysIndirect;
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
            funcsTableInitialized = true;
        }
        return funcsTable;
    }

    const DynamicBackendParameters& BackendObject_DirectGLES::GetDynamicParameters() const {
        return m_dynamicParameters;
    }

    void BackendObject_DirectGLES::UpdateDynamicBackendParameters() {
        m_dynamicParameters.UniformBufferOffsetAlignment = m_GLESCapabilities.UniformBufferOffsetAlignment;
        m_dynamicParameters.AliasedLineWidthRangeMin = m_GLESCapabilities.AliasedLineWidthRangeMin;
        m_dynamicParameters.AliasedLineWidthRangeMax = m_GLESCapabilities.AliasedLineWidthRangeMax;
        m_dynamicParameters.SmoothLineWidthRangeMin = m_GLESCapabilities.SmoothLineWidthRangeMin;
        m_dynamicParameters.SmoothLineWidthRangeMax = m_GLESCapabilities.SmoothLineWidthRangeMax;
        m_dynamicParameters.SmoothLineWidthGranularity = m_GLESCapabilities.SmoothLineWidthGranularity;
        m_dynamicParameters.PointSizeRangeMin = m_GLESCapabilities.PointSizeRangeMin;
        m_dynamicParameters.PointSizeRangeMax = m_GLESCapabilities.PointSizeRangeMax;
        m_dynamicParameters.PointSizeGranularity = m_GLESCapabilities.PointSizeGranularity;
        m_dynamicParameters.Max3DTextureSize = m_GLESCapabilities.Max3DTextureSize;
        m_dynamicParameters.MaxArrayTextureLayers = m_GLESCapabilities.MaxArrayTextureLayers;
        m_dynamicParameters.MaxCubeMapTextureSize = m_GLESCapabilities.MaxCubeMapTextureSize;
        m_dynamicParameters.MaxFramebufferWidth = m_GLESCapabilities.MaxFramebufferWidth;
        m_dynamicParameters.MaxFramebufferHeight = m_GLESCapabilities.MaxFramebufferHeight;
        m_dynamicParameters.MaxFramebufferLayers = m_GLESCapabilities.MaxFramebufferLayers;
        m_dynamicParameters.MaxRenderbufferSize = m_GLESCapabilities.MaxRenderbufferSize;
        m_dynamicParameters.MaxTextureSize = m_GLESCapabilities.MaxTextureSize;
        m_dynamicParameters.MaxColorTextureSamples = m_GLESCapabilities.MaxColorTextureSamples;
        m_dynamicParameters.MaxDepthTextureSamples = m_GLESCapabilities.MaxDepthTextureSamples;
        m_dynamicParameters.MaxFramebufferSamples = m_GLESCapabilities.MaxFramebufferSamples;
        m_dynamicParameters.MaxIntegerSamples = m_GLESCapabilities.MaxIntegerSamples;
        m_dynamicParameters.MaxSamples = m_GLESCapabilities.MaxSamples;
        m_dynamicParameters.MaxSampleMaskWords = m_GLESCapabilities.MaxSampleMaskWords;
        m_dynamicParameters.MaxTextureImageUnits = m_GLESCapabilities.MaxTextureImageUnits;
        m_dynamicParameters.MaxVertexTextureImageUnits = m_GLESCapabilities.MaxVertexTextureImageUnits;
        m_dynamicParameters.MaxComputeTextureImageUnits = m_GLESCapabilities.MaxComputeTextureImageUnits;
        m_dynamicParameters.MaxCombinedTextureImageUnits = m_GLESCapabilities.MaxCombinedTextureImageUnits;
        m_dynamicParameters.MaxVertexAttribs = m_GLESCapabilities.MaxVertexAttribs;
        m_dynamicParameters.MaxComputeShaderStorageBlocks = m_GLESCapabilities.MaxComputeShaderStorageBlocks;
        m_dynamicParameters.MaxCombinedShaderStorageBlocks = m_GLESCapabilities.MaxCombinedShaderStorageBlocks;
        m_dynamicParameters.MaxComputeUniformBlocks = m_GLESCapabilities.MaxComputeUniformBlocks;
        m_dynamicParameters.MaxComputeWorkGroupInvocations = m_GLESCapabilities.MaxComputeWorkGroupInvocations;
        m_dynamicParameters.MaxShaderStorageBufferBindings = m_GLESCapabilities.MaxShaderStorageBufferBindings;
        m_dynamicParameters.MaxTextureBufferSize = m_GLESCapabilities.MaxTextureBufferSize;
        m_dynamicParameters.MaxUniformBufferBindings = m_GLESCapabilities.MaxUniformBufferBindings;
        m_dynamicParameters.MaxUniformBlockSize = m_GLESCapabilities.MaxUniformBlockSize;
        m_dynamicParameters.MaxImageUnits = m_GLESCapabilities.MaxImageUnits;
        m_dynamicParameters.MaxCombinedImageUniforms = m_GLESCapabilities.MaxCombinedImageUniforms;
        m_dynamicParameters.MaxComputeImageUniforms = m_GLESCapabilities.MaxComputeImageUniforms;
        m_dynamicParameters.MaxDrawBuffers = m_GLESCapabilities.MaxDrawBuffers;
        m_dynamicParameters.MaxColorAttachments = m_GLESCapabilities.MaxColorAttachments;
        m_dynamicParameters.MaxClipDistances = m_GLESCapabilities.MaxClipDistances;
        m_dynamicParameters.MaxViewports = m_GLESCapabilities.MaxViewports;
        m_dynamicParameters.MaxViewportWidth = m_GLESCapabilities.MaxViewportWidth;
        m_dynamicParameters.MaxViewportHeight = m_GLESCapabilities.MaxViewportHeight;
        m_dynamicParameters.ViewportBoundsRangeMin = m_GLESCapabilities.ViewportBoundsRangeMin;
        m_dynamicParameters.ViewportBoundsRangeMax = m_GLESCapabilities.ViewportBoundsRangeMax;
        m_dynamicParameters.ViewportSubpixelBits = m_GLESCapabilities.ViewportSubpixelBits;
        m_dynamicParameters.SupportsWideLines =
            m_GLESCapabilities.AliasedLineWidthRangeMax > 1.0f || m_GLESCapabilities.SmoothLineWidthRangeMax > 1.0f;
    }

    const MG_External::GLESFunctionsTable& BackendObject_DirectGLES::GetGLESFunctions() const {
        return m_GLESFunctions;
    }

    const MG_External::EGLFunctionsTable& BackendObject_DirectGLES::GetEGLFunctions() const {
        return m_EGLFunctions;
    }
} // namespace MobileGL::MG_Backend::DirectGLES
