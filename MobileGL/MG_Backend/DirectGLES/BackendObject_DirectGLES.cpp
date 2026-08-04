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
#include <MG_Backend/DirectGLES/Managers.h>
#include <MG_Backend/DirectGLES/Utils.h>
#include <MG_Util/BackendLoaders/OpenGL/Loader.h>
#include <MG_Util/Classifiers/TextureEnumClassifier.h>
#include <MG_Util/Converters/GLToMG/TextureEnumConverter.h>
#include <MG_Util/Converters/GLToStr/GLEnumConverter.h>
#include <MG_Util/Converters/MGToGL/TextureEnumConverter.h>
#include <MG_Util/Converters/MGToStr/TextureEnumConverter.h>
#include <MG_Util/Texture/TextureFormatProcessor.h>
#include <Config.h>
#include <algorithm>
#include <cmath>
#include <format>

namespace MobileGL::MG_Backend::DirectGLES {
    namespace {
        Bool IsReleaseCurrentRequest(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) {
            (void)dpy;
            return draw == EGL_NO_SURFACE && read == EGL_NO_SURFACE && ctx == EGL_NO_CONTEXT;
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

        struct GLESProbeFormatInfo {
            GLenum InternalFormat = GL_UNKNOWN_MGL;
            GLenum ImageFormat = GL_RGBA;
            GLenum ImageType = GL_UNSIGNED_BYTE;
            String Reason;
        };

        GLESProbeFormatInfo BuildNativeProbeFormatInfo(GLenum requestedInternalFormat) {
            GLESProbeFormatInfo info;
            info.InternalFormat = requestedInternalFormat;
            MG_Util::TextureFormatProcessor::NormalizePixelFormat(
                requestedInternalFormat, PixelFormatNormalizeOptionBit::None, nullptr, &info.ImageFormat,
                &info.ImageType);
            return info;
        }

        Flags<PixelFormatNormalizeOptionBit> GetForcedPixelFormatNormalizeOptions(
            const MG_External::GLESCapabilities& capabilities) {
            Flags<PixelFormatNormalizeOptionBit> options;
            if (capabilities.IsAngleRenderer) {
                options |= PixelFormatNormalizeOptionBit::NoRgb16;
                options |= PixelFormatNormalizeOptionBit::NoSnorm16;
                options |= PixelFormatNormalizeOptionBit::NoSnorm8;
            }
            return options;
        }

        Flags<PixelFormatNormalizeOptionBit> GetDriverPixelFormatNormalizeOptions(
            const MG_External::GLESCapabilities& capabilities) {
            Flags<PixelFormatNormalizeOptionBit> options = PixelFormatNormalizeOptionBit::NoDepthComponent32;
            options |= PixelFormatNormalizeOptionBit::NoRGBA8Snorm;
            options |= PixelFormatNormalizeOptionBit::NoRGB16Snorm;
            if (!capabilities.SupportsNorm16Texture) {
                options |= PixelFormatNormalizeOptionBit::NoNorm16;
            }
            return options;
        }

        String BuildPixelFormatFallbackReason(Flags<PixelFormatNormalizeOptionBit> options, Bool forced) {
            Vector<String> reasons;
            if (options & PixelFormatNormalizeOptionBit::NoNorm16) {
                reasons.push_back("EXT_texture_norm16 not supported");
            }
            if (options & PixelFormatNormalizeOptionBit::NoRgb16) {
                reasons.push_back(forced ? "RGB16 fallback forced by backend policy"
                                         : "RGB16 native path is not supported");
            }
            if (options & PixelFormatNormalizeOptionBit::NoSnorm16) {
                reasons.push_back(forced ? "SNORM16 fallback forced by backend policy"
                                         : "SNORM16 native path is not supported");
            }
            if (options & PixelFormatNormalizeOptionBit::NoSnorm8) {
                reasons.push_back(forced ? "SNORM8 fallback forced by backend policy"
                                         : "SNORM8 native path is not supported");
            }
            if (options & PixelFormatNormalizeOptionBit::NoRGBA8Snorm) {
                reasons.push_back(forced ? "RGBA8_SNORM fallback forced by backend policy"
                                         : "RGBA8_SNORM render target path is not supported");
            }
            if (options & PixelFormatNormalizeOptionBit::NoRGB16Snorm) {
                reasons.push_back(forced ? "RGB16_SNORM fallback forced by backend policy"
                                         : "RGB16_SNORM render target path is not supported");
            }
            if (options & PixelFormatNormalizeOptionBit::NoDepthComponent32) {
                reasons.push_back("GL_DEPTH_COMPONENT32 native probe failed on OpenGL ES");
            }
            if (options & PixelFormatNormalizeOptionBit::NoThreeChannelRenderTarget) {
                reasons.push_back("no three-channel multisample storage format on OpenGL ES");
            }
            if (options & PixelFormatNormalizeOptionBit::NoSnorm16RenderTarget) {
                reasons.push_back("EXT_render_snorm not supported");
            }

            String reason;
            for (SizeT i = 0; i < reasons.size(); ++i) {
                if (i != 0) reason += "; ";
                reason += reasons[i];
            }
            return reason.empty() ? "Native format probe failed" : reason;
        }

        String ConvertFallbackInternalFormatToString(GLenum internalFormat) {
            const TextureInternalFormat logicalFormat = MG_Util::ConvertGLEnumToTextureInternalFormat(internalFormat);
            if (logicalFormat != TextureInternalFormat::Unknown) {
                return MG_Util::ConvertTextureInternalFormatToString(logicalFormat);
            }
            return MG_Util::ConvertGLEnumToString(internalFormat);
        }

        void LogGLESFormatCaveat(TextureInternalFormat logicalFormat,
                                 SizeT targetIndex,
                                 const GLESProbeFormatInfo& fallbackInfo) {
            MGLOG_D("Caveat: %s %s not fully supported. Reason: %s. Fallback: %s",
                    GetFormatCapabilityTargetName(targetIndex).c_str(),
                    MG_Util::ConvertTextureInternalFormatToString(logicalFormat).c_str(),
                    fallbackInfo.Reason.c_str(),
                    ConvertFallbackInternalFormatToString(fallbackInfo.InternalFormat).c_str());
        }

        Bool BuildFallbackProbeFormatInfo(GLenum requestedInternalFormat,
                                          Flags<PixelFormatNormalizeOptionBit> options,
                                          Bool forced,
                                          GLESProbeFormatInfo& outInfo) {
            const Flags<PixelFormatNormalizeOptionBit> applicableOptions =
                MG_Util::TextureFormatProcessor::GetApplicablePixelFormatNormalizeOptions(requestedInternalFormat,
                                                                                          options);
            if (!applicableOptions) {
                return false;
            }

            MG_Util::TextureFormatProcessor::NormalizePixelFormat(requestedInternalFormat, applicableOptions,
                                                                  &outInfo.InternalFormat, &outInfo.ImageFormat,
                                                                  &outInfo.ImageType);
            outInfo.Reason = BuildPixelFormatFallbackReason(applicableOptions, forced);
            return outInfo.InternalFormat != GL_UNKNOWN_MGL;
        }

        FormatCapabilityFlags BuildTextureCapsFromProbe(TextureInternalFormat logicalFormat,
                                                        TextureTarget target,
                                                        Bool renderable) {
            FormatCapabilityFlags caps = GetTextureFeatureCaps(logicalFormat, target);
            if (renderable) {
                caps |= GetAttachmentCaps(logicalFormat);
                if (target == TextureTarget::Texture3D || target == TextureTarget::Texture2DArray ||
                    target == TextureTarget::TextureCubeMapArray ||
                    target == TextureTarget::Texture2DMultisampleArray) {
                    caps |= FormatCapability::FramebufferLayered;
                }
            }
            return caps;
        }

        void AddFullFormatCaps(FormatCapabilityCache& cache,
                               SizeT targetIndex,
                               SizeT formatIndex,
                               FormatCapabilityFlags caps) {
            cache.FullCaps[targetIndex][formatIndex] |= caps;
        }

        Bool AddCaveatFormatCaps(FormatCapabilityCache& cache,
                                 SizeT targetIndex,
                                 SizeT formatIndex,
                                 FormatCapabilityFlags caps) {
            Bool added = false;
            for (FormatCapability capability : kReportedFormatCapabilities) {
                if (HasFormatCapability(caps, capability) &&
                    !HasFormatCapability(cache.FullCaps[targetIndex][formatIndex], capability)) {
                    cache.CaveatCaps[targetIndex][formatIndex] |= capability;
                    added = true;
                }
            }
            return added;
        }

        Int GetGLESFormatMaxSamples(const MG_External::GLESCapabilities& capabilities,
                                    TextureInternalFormat logicalFormat,
                                    GLenum imageFormat) {
            const Bool isDepth = MG_Util::IsDepthFormatInternalFormat(logicalFormat);
            const Bool isStencil = MG_Util::IsStencilFormatInternalFormat(logicalFormat);
            const Bool isInteger = imageFormat == GL_RED_INTEGER || imageFormat == GL_RG_INTEGER ||
                                   imageFormat == GL_RGB_INTEGER || imageFormat == GL_RGBA_INTEGER;
            if (isDepth || isStencil) {
                return capabilities.MaxDepthTextureSamples;
            }
            if (isInteger) {
                return capabilities.MaxIntegerSamples;
            }
            return capabilities.MaxColorTextureSamples;
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

        // Whether the driver renders to a framebuffer whose depth and stencil come from
        // two different renderbuffers. GL only requires support when both attachments are
        // the same image, and ES drivers commonly answer GL_FRAMEBUFFER_UNSUPPORTED here;
        // reporting COMPLETE from the frontend and then rendering into a framebuffer the
        // driver refuses leaves the results silently empty.
        Bool ProbeDistinctDepthStencilAttachments(const MG_External::GLESFunctionsTable& gl) {
            if (!gl.glGenFramebuffers || !gl.glBindFramebuffer || !gl.glFramebufferRenderbuffer ||
                !gl.glCheckFramebufferStatus || !gl.glDeleteFramebuffers || !gl.glGenRenderbuffers ||
                !gl.glBindRenderbuffer || !gl.glRenderbufferStorage || !gl.glDeleteRenderbuffers) {
                return true;
            }

            GLint prevFramebuffer = 0, prevRenderbuffer = 0;
            gl.glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFramebuffer);
            gl.glGetIntegerv(GL_RENDERBUFFER_BINDING, &prevRenderbuffer);

            GLuint framebuffer = 0;
            GLuint renderbuffers[2] = {0, 0};
            gl.glGenFramebuffers(1, &framebuffer);
            gl.glGenRenderbuffers(2, renderbuffers);
            gl.glBindRenderbuffer(GL_RENDERBUFFER, renderbuffers[0]);
            gl.glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, 4, 4);
            gl.glBindRenderbuffer(GL_RENDERBUFFER, renderbuffers[1]);
            gl.glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, 4, 4);
            gl.glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
            gl.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, renderbuffers[0]);
            gl.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, renderbuffers[1]);
            const Bool supported = gl.glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

            gl.glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFramebuffer));
            gl.glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(prevRenderbuffer));
            gl.glDeleteFramebuffers(1, &framebuffer);
            gl.glDeleteRenderbuffers(2, renderbuffers);
            return supported;
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

        void PopulateFormatCapabilitiesImpl(const MG_External::GLESFunctionsTable& gl,
                                            const MG_External::GLESCapabilities& capabilities,
                                            FormatCapabilityCache& cache) {
            cache.Clear();
            const Flags<PixelFormatNormalizeOptionBit> forcedOptions =
                GetForcedPixelFormatNormalizeOptions(capabilities);
            const Flags<PixelFormatNormalizeOptionBit> driverOptions =
                GetDriverPixelFormatNormalizeOptions(capabilities);

            for (SizeT formatIndex = 0; formatIndex < kFormatCapabilityFormatCount; ++formatIndex) {
                const auto logicalFormat = static_cast<TextureInternalFormat>(formatIndex);
                GLenum requestedInternalFormat = MG_Util::ConvertTextureInternalFormatToGLEnum(logicalFormat);
                if (requestedInternalFormat == GL_UNKNOWN_MGL) {
                    continue;
                }

                const GLESProbeFormatInfo nativeInfo = BuildNativeProbeFormatInfo(requestedInternalFormat);
                GLESProbeFormatInfo outerFallbackInfo;
                const Bool outerHasForcedFallback =
                    BuildFallbackProbeFormatInfo(requestedInternalFormat, forcedOptions, true, outerFallbackInfo);
                if (!outerHasForcedFallback) {
                    BuildFallbackProbeFormatInfo(requestedInternalFormat, driverOptions, false, outerFallbackInfo);
                }

                for (SizeT targetIndex = 0; targetIndex < kFormatCapabilityTextureTargetCount; ++targetIndex) {
                    const auto target = static_cast<TextureTarget>(targetIndex);
                    // A multisample texture can only ever be rendered into, so its storage format
                    // has to stay colour-renderable; the ordinary fallback for a three-channel
                    // format is a three-channel one, which ES accepts as a texture but rejects as
                    // multisample storage. Recompute the fallback per target so those formats get
                    // widened here and nowhere else.
                    Flags<PixelFormatNormalizeOptionBit> targetOptions;
                    if (IsGLESProbeMultisampleTarget(target)) {
                        targetOptions |= PixelFormatNormalizeOptionBit::NoThreeChannelRenderTarget;
                        if (!capabilities.SupportsRenderSnorm || !capabilities.SupportsNorm16Texture) {
                            targetOptions |= PixelFormatNormalizeOptionBit::NoSnorm16RenderTarget;
                        }
                    }
                    GLESProbeFormatInfo fallbackInfo = outerFallbackInfo;
                    Bool hasForcedFallback = outerHasForcedFallback;
                    if (targetOptions) {
                        hasForcedFallback = BuildFallbackProbeFormatInfo(
                            requestedInternalFormat, forcedOptions | targetOptions, true, fallbackInfo);
                        if (!hasForcedFallback) {
                            BuildFallbackProbeFormatInfo(requestedInternalFormat, driverOptions | targetOptions,
                                                         false, fallbackInfo);
                        }
                    }

                    // 1D, 1D-array and rectangle textures live on an ES target (see
                    // TextureImpl::MapToBackendTextureTarget), so they have to be probed there too -
                    // probing the desktop-only target itself always failed, which left those slots
                    // of the cache empty and stopped any fallback format from being selected for
                    // them (a GL_DEPTH_COMPONENT32 1D texture then got no storage at all).
                    const TextureTarget probeTarget = TextureImpl::MapToBackendTextureTarget(target);

                    Bool shouldProbeFallback = hasForcedFallback;
                    if (!hasForcedFallback) {
                        Bool nativeRenderable = false;
                        const Bool nativeCreated =
                            ProbeTexture(gl, probeTarget, nativeInfo.InternalFormat, nativeInfo.ImageFormat,
                                         nativeInfo.ImageType, logicalFormat, &nativeRenderable);
                        if (nativeCreated) {
                            AddFullFormatCaps(cache, targetIndex, formatIndex,
                                              BuildTextureCapsFromProbe(logicalFormat, target, nativeRenderable));
                            if (IsGLESProbeMultisampleTarget(target)) {
                                cache.SampleCounts[targetIndex][formatIndex] = {1};
                            }
                        }
                        shouldProbeFallback = !nativeCreated || !nativeRenderable;
                    }

                    if (shouldProbeFallback && fallbackInfo.InternalFormat != GL_UNKNOWN_MGL) {
                        Bool fallbackRenderable = false;
                        const Bool fallbackCreated =
                            ProbeTexture(gl, probeTarget, fallbackInfo.InternalFormat, fallbackInfo.ImageFormat,
                                         fallbackInfo.ImageType, logicalFormat, &fallbackRenderable);
                        if (fallbackCreated) {
                            if (AddCaveatFormatCaps(cache, targetIndex, formatIndex,
                                                    BuildTextureCapsFromProbe(logicalFormat, target,
                                                                              fallbackRenderable))) {
                                LogGLESFormatCaveat(logicalFormat, targetIndex, fallbackInfo);
                            }
                            if (IsGLESProbeMultisampleTarget(target)) {
                                cache.SampleCounts[targetIndex][formatIndex] = {1};
                            }
                        }
                    }
                }

                const SizeT renderbufferTargetIndex = GetRenderbufferFormatCapabilityTargetIndex();
                Bool shouldProbeFallbackRenderbuffer = outerHasForcedFallback;
                if (!outerHasForcedFallback) {
                    const Bool nativeRenderbufferComplete =
                        ProbeRenderbuffer(gl, nativeInfo.InternalFormat, logicalFormat, false, 1);
                    if (nativeRenderbufferComplete) {
                        AddFullFormatCaps(cache, renderbufferTargetIndex, formatIndex,
                                          GetRenderbufferFeatureCaps(logicalFormat));
                        const Int maxSamples =
                            GetGLESFormatMaxSamples(capabilities, logicalFormat, nativeInfo.ImageFormat);
                        cache.SampleCounts[renderbufferTargetIndex][formatIndex] =
                            ProbeRenderbufferSampleCounts(gl, nativeInfo.InternalFormat, logicalFormat, maxSamples);
                    } else {
                        shouldProbeFallbackRenderbuffer = true;
                    }
                }
                if (shouldProbeFallbackRenderbuffer && outerFallbackInfo.InternalFormat != GL_UNKNOWN_MGL &&
                    ProbeRenderbuffer(gl, outerFallbackInfo.InternalFormat, logicalFormat, false, 1)) {
                    if (AddCaveatFormatCaps(cache, renderbufferTargetIndex, formatIndex,
                                            GetRenderbufferFeatureCaps(logicalFormat))) {
                        LogGLESFormatCaveat(logicalFormat, renderbufferTargetIndex, outerFallbackInfo);
                    }
                    const Int maxSamples =
                        GetGLESFormatMaxSamples(capabilities, logicalFormat, outerFallbackInfo.ImageFormat);
                    cache.SampleCounts[renderbufferTargetIndex][formatIndex] =
                        ProbeRenderbufferSampleCounts(gl, outerFallbackInfo.InternalFormat, logicalFormat, maxSamples);
                }
            }
        }

        // The advertised renderer info must be mutable after its first use:
        // E_GL_ARB_timer_query can only be decided once the ES capabilities
        // are known, long after the list is first read (see
        // UpdateAdvertisedTimerQueryExtension below).
        RendererInfo& MutableRendererInfo() {
            static RendererInfo rendererInfo = {
                .RendererName = "Espryt",            // Renderer Name
                .BackendName = "Direct (OpenGL ES)", // Backend Name
                .ExtraVendor = Nullopt,              // Extra vendor
                .RendererGLInfo =
                    {
                        .TargetGLVersion = {3, 3, 0},   // GL target version
                        .TargetGLSLVersion = {4, 6, 0}, // Target Shading Language Version
                        // Baseline advertisement (no timer queries / anisotropy yet); reconciled
                        // once the ES capabilities exist, see UpdateAdvertisedCapabilityExtensions.
                        .Extensions = BuildAdvertisedExtensions(false, false),
                        .IsCompatibilityProfile = false // Is Compatibility Profile
                    },
                .StaticBackendCapability = {.AllowVSOnlyPrograms = false} // Backend Capability
            };
            return rendererInfo;
        }

        // GL_ARB_timer_query gates MC's F3 GPU% (LWJGL checks the extension
        // string via glGetStringi plus non-null glQueryCounter and
        // glGetQueryObject(u)i64v entries). GetRendererInfo is first invoked
        // from LogBackendInfo() during MG_Backend::Init, BEFORE any ES
        // context or capabilities exist, so the advertisement cannot be baked
        // into the static initializer above; it is reconciled here at the end
        // of InitCapabilities instead (mirroring DirectVulkan's mutable
        // m_rendererInfo + UpdateAdvertisedExtensions). InitCapabilities
        // completes inside the first MakeEGLCurrent on a context, so an app
        // thread can only observe the extension string after the
        // advertisement for its context has settled; rebuilding the whole
        // list keeps the re-run after a context recreation idempotent.
        void UpdateAdvertisedCapabilityExtensions(Bool anisotropicFilteringSupported) {
            MutableRendererInfo().RendererGLInfo.Extensions =
                BuildAdvertisedExtensions(AreTimerQueriesSupported(), anisotropicFilteringSupported);
        }
    } // namespace

    void PopulateFormatCapabilities(const MG_External::GLESFunctionsTable& gl,
                                    const MG_External::GLESCapabilities& capabilities,
                                    FormatCapabilityCache& cache) {
        PopulateFormatCapabilitiesImpl(gl, capabilities, cache);
    }

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
        BufferImpl::RegisterBufferBackendOps();
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
        // Now that g_GLESCapabilities knows about GL_EXT_disjoint_timer_query and
        // GL_EXT_texture_filter_anisotropic, reconcile the advertisement (see the comment on
        // UpdateAdvertisedCapabilityExtensions for why it cannot happen when the extension
        // list is first built).
        UpdateAdvertisedCapabilityExtensions(m_GLESCapabilities.SupportsTextureFilterAnisotropy);
        UpdateDynamicBackendParameters();
        PopulateFormatCapabilities(m_GLESFunctions, m_GLESCapabilities, MutableFormatCapabilities());
        PrintFormatCapabilities(GetFormatCapabilities());
        return true;
    }

    Bool BackendObject_DirectGLES::InitializeEGLDisplay(EGLDisplay dpy, EGLint* major, EGLint* minor) {
        if (!m_initialized) {
            MGLOG_E("DirectGLES backend not initialized");
            return false;
        }
        return BackendObject::InitializeEGLDisplay(dpy, major, minor);
    }

    Bool BackendObject_DirectGLES::CreateEGLWindowSurface(EGLSurface surface, const WindowHandle& handle) {
        const std::lock_guard<std::recursive_mutex> lock(m_eglStateMutex);
        if (!m_initialized) {
            MGLOG_E("DirectGLES backend not initialized");
            return false;
        }

        if ((handle.Backend != WindowBackend::Android &&
             handle.Backend != WindowBackend::X11 &&
             handle.Backend != WindowBackend::MetalLayer &&
             handle.Backend != WindowBackend::Win32) ||
            !handle.Handle) {
            MGLOG_E("DirectGLES backend only supports Android, X11, CAMetalLayer, and Win32 native windows");
            return false;
        }

        const Bool sameHandle = m_eglSurfaceInitialized && m_eglSurface == surface &&
                                m_eglSurfaceKind == SurfaceKind::Window && m_windowHandle.Backend == handle.Backend &&
                                m_windowHandle.Handle == handle.Handle;
        if (sameHandle) {
            return true;
        }

        if (m_eglSurfaceInitialized) {
            DestroyEGLContext();
            ResetEGLRuntimeState();
        }

        return BackendObject::CreateEGLWindowSurface(surface, handle);
    }

    Bool BackendObject_DirectGLES::CreateEGLPbufferSurface(EGLSurface surface, EGLint width, EGLint height) {
        const std::lock_guard<std::recursive_mutex> lock(m_eglStateMutex);
        if (!m_initialized) {
            MGLOG_E("DirectGLES backend not initialized");
            return false;
        }

        if (m_eglSurfaceInitialized && m_eglSurface == surface && m_eglSurfaceKind == SurfaceKind::Pbuffer) {
            return true;
        }

        if (m_eglSurfaceInitialized) {
            DestroyEGLContext();
            ResetEGLRuntimeState();
        }

        return BackendObject::CreateEGLPbufferSurface(surface, width, height);
    }

    Bool BackendObject_DirectGLES::InitPbufferSurface(EGLint width, EGLint height) {
        return DirectGLES::InitPbufferSurface(width, height);
    }

    Bool BackendObject_DirectGLES::MakeEGLCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) {
        const std::lock_guard<std::recursive_mutex> lock(m_eglStateMutex);
        if (IsReleaseCurrentRequest(dpy, draw, read, ctx)) {
            if (!DirectGLES::ReleaseCurrent()) {
                return false;
            }
            return BackendObject::MakeEGLCurrent(dpy, draw, read, ctx);
        }

        if (!m_initialized) {
            MGLOG_E("DirectGLES backend not initialized");
            return false;
        }
        if (!m_eglDisplayInitialized || m_eglDisplay != dpy) {
            MGLOG_E("MakeEGLCurrent failed: EGL display mismatch or not initialized");
            return false;
        }
        if (!m_eglSurfaceInitialized) {
            MGLOG_E("MakeEGLCurrent failed: EGL surface is not initialized");
            return false;
        }
        if (draw == EGL_NO_SURFACE || read == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT) {
            MGLOG_E("MakeEGLCurrent failed: draw/read/context is invalid");
            return false;
        }

        if (!DirectGLES::MakeCurrent()) {
            return false;
        }

        if (!BackendObject::MakeEGLCurrent(dpy, draw, read, ctx)) {
            (void)DirectGLES::ReleaseCurrent();
            return false;
        }
        return true;
    }

    Bool BackendObject_DirectGLES::SwapEGLBuffers(EGLDisplay dpy, EGLSurface draw) {
        return BackendObject::SwapEGLBuffers(dpy, draw);
    }

    void BackendObject_DirectGLES::ReleaseEGLSurface(EGLSurface surface) {
        const std::lock_guard<std::recursive_mutex> lock(m_eglStateMutex);
        BackendObject::ReleaseEGLSurface(surface);
    }

    void BackendObject_DirectGLES::ReleaseEGLResources() {
        const std::lock_guard<std::recursive_mutex> lock(m_eglStateMutex);
        DestroyEGLContext();
        BackendObject::ReleaseEGLResources();
    }

    void BackendObject_DirectGLES::OnEGLSurfaceReleased(EGLSurface surface) {
        (void)surface;
        DestroyEGLContext();
    }

    const RendererInfo& BackendObject_DirectGLES::GetRendererInfo() const {
        return MutableRendererInfo();
    }

    String BackendObject_DirectGLES::GetBackendAPIVersionString() const {
        if (!m_initialized) {
            return "<uninitialized DirectGLES backend>";
        }
        return FormatBackendAPIVersionString(m_GLESCapabilities.GLESRendererString,
                                             m_GLESCapabilities.GLESVersion.Major,
                                             m_GLESCapabilities.GLESVersion.Minor);
    }

    const RendererInfo& GetRendererIdentity() {
        return MutableRendererInfo();
    }

    Vector<GLExtension> BuildAdvertisedExtensions(Bool timerQueriesSupported, Bool anisotropicFilteringSupported) {
        Vector<GLExtension> extensions = {V_OpenGL30, V_OpenGL31, V_OpenGL32,
                                          V_OpenGL33, E_GL_ARB_draw_buffers_blend, E_GL_ARB_compute_shader,
                                          E_GL_ARB_shader_storage_buffer_object, E_GL_ARB_shader_image_load_store,
                                          E_GL_ARB_program_interface_query, E_GL_ARB_framebuffer_object,
                                          E_GL_EXT_framebuffer_object, E_GL_ARB_depth_texture, E_GL_ARB_buffer_storage,
                                          E_GL_ARB_texture_storage, E_GL_ARB_texture_storage_multisample,
                                          E_GL_ARB_clear_texture, E_GL_ARB_direct_state_access,
                                          E_GL_ARB_multi_draw_indirect, E_GL_ARB_indirect_parameters,
                                          E_GL_ARB_shader_draw_parameters, E_GL_ARB_gpu_shader5, E_GL_ARB_multi_bind,
                                          E_GL_ARB_shading_language_420pack, E_GL_ARB_vertex_attrib_binding,
                                          // Both are core from GL 3.2/3.3 on and implemented here for
                                          // every advertised version, but an app targeting 3.0/3.1
                                          // only reaches them through the extension string - the CTS
                                          // picks a whole different shader for draw_buffers without
                                          // explicit_attrib_location. DirectVulkan advertises both.
                                          E_GL_ARB_explicit_attrib_location, E_GL_ARB_texture_multisample,
                                          E_GL_ARB_shader_image_size};
        // Only advertised when the device driver actually has usable timer queries
        // (GL_EXT_disjoint_timer_query plus its entry points) and the
        // MOBILEGL_DISABLE_TIMERQUERY escape hatch is off.
        if (timerQueriesSupported && !MG_Config::Features.DisableTimerQuery) {
            extensions.push_back(E_GL_ARB_timer_query);
        }
        // Only advertised when the host ES driver actually filters anisotropically: the sampler
        // state is accepted regardless, but forwarding it would be a no-op without the extension,
        // and an app that trusts the string (LWJGL builds GLCapabilities from it) would silently
        // get plain trilinear.
        if (anisotropicFilteringSupported) {
            extensions.push_back(E_GL_EXT_texture_filter_anisotropic);
            extensions.push_back(E_GL_ARB_texture_filter_anisotropic);
        }
        return extensions;
    }

    String FormatBackendAPIVersionString(const String& glesRendererString, Int glesMajor, Int glesMinor) {
        // Format:
        // <OpenGL ES Renderer>, OpenGL ES <OpenGL ES Version>
        return std::format("{}, OpenGL ES {}.{}", glesRendererString, glesMajor, glesMinor);
    }

    BackendType BackendObject_DirectGLES::GetBackendType() const {
        return BackendType::DirectGLES;
    }

    const GlobalBackendFunctionsTable& BackendObject_DirectGLES::GetBackendFunctions() const {
        static GlobalBackendFunctionsTable funcsTable;
        static Bool funcsTableInitialized = false;
        if (!funcsTableInitialized) {
            funcsTable.Present = DirectGLES::Present;
            funcsTable.SetSwapInterval = DirectGLES::SetSwapInterval;
            funcsTable.GL.DrawArrays = DrawArrays;
            funcsTable.GL.DrawElements = DrawElements;
            funcsTable.GL.DrawElementsBaseVertex = DrawElementsBaseVertex;
            funcsTable.GL.MultiDrawArrays = MultiDrawArrays;
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
            funcsTable.GL.FenceSync = FenceSync;
            funcsTable.GL.ClientWaitSync = ClientWaitSync;
            funcsTable.GL.WaitSync = WaitSync;
            funcsTable.GL.DeleteSync = DeleteSync;
            funcsTable.GL.GetSyncStatus = GetSyncStatus;
            // Optional timer-query group: left null (the frontend then falls
            // back) when disabled via MOBILEGL_DISABLE_TIMERQUERY. The hooks
            // themselves additionally degrade to null handles / zero results
            // when GL_EXT_disjoint_timer_query or its entry points are
            // missing, or when the calling thread does not own the ES
            // context.
            if (!MG_Config::Features.DisableTimerQuery) {
                // AreTimerQueriesSupported is a pure capability read (no
                // current ES context required, false until the caps are
                // filled in), which is exactly the dynamic support check
                // the frontend wants from IsTimerQuerySupported.
                funcsTable.GL.IsTimerQuerySupported = AreTimerQueriesSupported;
                funcsTable.GL.BeginTimeElapsedQuery = BeginTimeElapsedQuery;
                funcsTable.GL.EndTimeElapsedQuery = EndTimeElapsedQuery;
                funcsTable.GL.QueryCounterTimestamp = QueryCounterTimestamp;
                funcsTable.GL.GetGpuTimestampNs = GetGpuTimestampNs;
            }
            // Occlusion queries are core ES3 (independent of MOBILEGL_DISABLE_TIMERQUERY)
            // and share the handle-based result/delete entries, which must exist even
            // when the timer-query group above is disabled.
            funcsTable.GL.BeginOcclusionQuery = BeginOcclusionQuery;
            funcsTable.GL.EndOcclusionQuery = EndOcclusionQuery;
            // Real driver primitive counters: the frontend's CPU accounting cannot see a
            // geometry shader's amplification.
            funcsTable.GL.BeginXfbPrimitivesQuery = BeginXfbPrimitivesQuery;
            funcsTable.GL.EndXfbPrimitivesQuery = EndXfbPrimitivesQuery;
            funcsTable.GL.IsQueryResultAvailable = IsQueryResultAvailable;
            funcsTable.GL.GetQueryResult64 = GetQueryResult64;
            funcsTable.GL.DeleteBackendQuery = DeleteBackendQuery;
            // Transform feedback is captured by the real ES driver rather than
            // reconstructed from the draw recording, so the frontend has to hand the
            // span boundaries over.
            funcsTable.GL.BeginTransformFeedback = XfbImpl::BeginTransformFeedback;
            funcsTable.GL.EndTransformFeedback = XfbImpl::EndTransformFeedback;
            funcsTable.GL.PauseTransformFeedback = XfbImpl::PauseTransformFeedback;
            funcsTable.GL.ResumeTransformFeedback = XfbImpl::ResumeTransformFeedback;
            funcsTable.GL.BindTransformFeedback = XfbImpl::BindTransformFeedback;
            funcsTable.GL.DeleteTransformFeedback = XfbImpl::DeleteTransformFeedback;
            funcsTableInitialized = true;
        }
        return funcsTable;
    }

    const DynamicBackendParameters& BackendObject_DirectGLES::GetDynamicParameters() const {
        return m_dynamicParameters;
    }

    void BackendObject_DirectGLES::ApplyGLESCapabilitiesForTesting(
        const MG_External::GLESCapabilities& capabilities) {
        m_GLESCapabilities = capabilities;
        UpdateDynamicBackendParameters();
    }

    void BackendObject_DirectGLES::UpdateDynamicBackendParameters() {
        m_dynamicParameters.UniformBufferOffsetAlignment = m_GLESCapabilities.UniformBufferOffsetAlignment;
        m_dynamicParameters.MaxTextureMaxAnisotropy = m_GLESCapabilities.MaxTextureMaxAnisotropy;
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
        m_dynamicParameters.MinProgramTextureGatherOffset = m_GLESCapabilities.MinProgramTextureGatherOffset;
        m_dynamicParameters.MaxProgramTextureGatherOffset = m_GLESCapabilities.MaxProgramTextureGatherOffset;
        // Clamp the advertised sampler limits the same way the DirectVulkan backend does: per-stage
        // GL_MAX_TEXTURE_IMAGE_UNITS must never exceed host-side fixed arrays sized off it (e.g.
        // Minecraft's 128-entry Blaze3D GlStateManager.TEXTURES[], iterated by Iris), and the combined
        // limit must stay within our texture-unit state array capacity.
        m_dynamicParameters.MaxTextureImageUnits =
            std::min(m_GLESCapabilities.MaxTextureImageUnits,
                     static_cast<Int>(MG_State::GLState::TextureState::MAX_PER_STAGE_TEXTURE_IMAGE_UNITS));
        m_dynamicParameters.MaxVertexTextureImageUnits =
            std::min(m_GLESCapabilities.MaxVertexTextureImageUnits,
                     static_cast<Int>(MG_State::GLState::TextureState::MAX_PER_STAGE_TEXTURE_IMAGE_UNITS));
        m_dynamicParameters.MaxComputeTextureImageUnits =
            std::min(m_GLESCapabilities.MaxComputeTextureImageUnits,
                     static_cast<Int>(MG_State::GLState::TextureState::MAX_PER_STAGE_TEXTURE_IMAGE_UNITS));
        m_dynamicParameters.MaxCombinedTextureImageUnits =
            std::min(m_GLESCapabilities.MaxCombinedTextureImageUnits,
                     static_cast<Int>(MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS));
        // Never advertise more attributes than the state layer can store: the current-value array and
        // the Uint32 attribute masks the draw path passes around are both bounded by MAX_VERTEX_ATTRIBS.
        m_dynamicParameters.MaxVertexAttribs =
            std::min(m_GLESCapabilities.MaxVertexAttribs,
                     static_cast<Int>(MG_State::GLState::VertexArrayObject::MAX_VERTEX_ATTRIBS));
        m_dynamicParameters.MaxComputeShaderStorageBlocks = m_GLESCapabilities.MaxComputeShaderStorageBlocks;
        m_dynamicParameters.MaxCombinedShaderStorageBlocks = m_GLESCapabilities.MaxCombinedShaderStorageBlocks;
        m_dynamicParameters.MaxComputeUniformBlocks = m_GLESCapabilities.MaxComputeUniformBlocks;
        m_dynamicParameters.MaxComputeWorkGroupInvocations = m_GLESCapabilities.MaxComputeWorkGroupInvocations;
        m_dynamicParameters.MaxShaderStorageBufferBindings = m_GLESCapabilities.MaxShaderStorageBufferBindings;
        m_dynamicParameters.MaxTextureBufferSize = m_GLESCapabilities.MaxTextureBufferSize;
        m_dynamicParameters.MaxUniformBufferBindings = m_GLESCapabilities.MaxUniformBufferBindings;
        m_dynamicParameters.MaxUniformBlockSize = m_GLESCapabilities.MaxUniformBlockSize;
        const Int maxSupportedTextureUnits =
            static_cast<Int>(MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS);
        m_dynamicParameters.MaxImageUnits =
            std::max(std::min(m_GLESCapabilities.MaxImageUnits, maxSupportedTextureUnits), 0);
        m_dynamicParameters.MaxCombinedImageUniforms = std::max(m_GLESCapabilities.MaxCombinedImageUniforms, 0);
        const auto clampStageImageUniforms = [this](Int stageLimit) {
            return std::min({std::max(stageLimit, 0), m_dynamicParameters.MaxImageUnits,
                             m_dynamicParameters.MaxCombinedImageUniforms});
        };
        m_dynamicParameters.MaxVertexImageUniforms =
            clampStageImageUniforms(m_GLESCapabilities.MaxVertexImageUniforms);
        m_dynamicParameters.MaxGeometryImageUniforms =
            clampStageImageUniforms(m_GLESCapabilities.MaxGeometryImageUniforms);
        m_dynamicParameters.MaxFragmentImageUniforms =
            clampStageImageUniforms(m_GLESCapabilities.MaxFragmentImageUniforms);
        m_dynamicParameters.MaxComputeImageUniforms =
            clampStageImageUniforms(m_GLESCapabilities.MaxComputeImageUniforms);
        m_dynamicParameters.SupportsDistinctDepthStencilAttachments =
            ProbeDistinctDepthStencilAttachments(DirectGLES::g_GLESFuncs);
        m_dynamicParameters.MaxDrawBuffers = m_GLESCapabilities.MaxDrawBuffers;
        m_dynamicParameters.MaxColorAttachments = m_GLESCapabilities.MaxColorAttachments;
        m_dynamicParameters.MaxClipDistances = m_GLESCapabilities.MaxClipDistances;
        m_dynamicParameters.MaxViewports = m_GLESCapabilities.MaxViewports;
        m_dynamicParameters.MaxViewportWidth = m_GLESCapabilities.MaxViewportWidth;
        m_dynamicParameters.MaxViewportHeight = m_GLESCapabilities.MaxViewportHeight;
        m_dynamicParameters.ViewportBoundsRangeMin = m_GLESCapabilities.ViewportBoundsRangeMin;
        m_dynamicParameters.ViewportBoundsRangeMax = m_GLESCapabilities.ViewportBoundsRangeMax;
        m_dynamicParameters.ViewportSubpixelBits = m_GLESCapabilities.ViewportSubpixelBits;
        m_dynamicParameters.MinFragmentInterpolationOffset =
            std::isfinite(m_GLESCapabilities.MinFragmentInterpolationOffset) &&
                    m_GLESCapabilities.MinFragmentInterpolationOffset <= -0.5f
                ? m_GLESCapabilities.MinFragmentInterpolationOffset
                : -0.5f;
        m_dynamicParameters.MaxFragmentInterpolationOffset = 0.4375f;
        m_dynamicParameters.FragmentInterpolationOffsetBits = 4;
        if (m_GLESCapabilities.FragmentInterpolationOffsetBits >= 4 &&
            std::isfinite(m_GLESCapabilities.MaxFragmentInterpolationOffset)) {
            const Float requiredMaxOffset =
                0.5f - std::ldexp(1.0f, -m_GLESCapabilities.FragmentInterpolationOffsetBits);
            if (m_GLESCapabilities.MaxFragmentInterpolationOffset >= requiredMaxOffset) {
                m_dynamicParameters.MaxFragmentInterpolationOffset =
                    m_GLESCapabilities.MaxFragmentInterpolationOffset;
                m_dynamicParameters.FragmentInterpolationOffsetBits =
                    m_GLESCapabilities.FragmentInterpolationOffsetBits;
            }
        }
        m_dynamicParameters.SupportsWideLines =
            m_GLESCapabilities.AliasedLineWidthRangeMax > 1.0f || m_GLESCapabilities.SmoothLineWidthRangeMax > 1.0f;

        const auto containsAny = [](const String& haystack, std::initializer_list<const char*> needles) {
            return std::any_of(needles.begin(), needles.end(), [&](const char* needle) {
                return haystack.find(needle) != String::npos;
            });
        };
        const String vendorAndRenderer =
            m_GLESCapabilities.GLESVendorString + " " + m_GLESCapabilities.GLESRendererString;
        if (containsAny(vendorAndRenderer, {"llvmpipe", "SwiftShader", "softpipe"})) {
            // Check software rasterizers first: ANGLE-on-llvmpipe reports both.
            m_dynamicParameters.GpuVendor = GpuVendorKind::Software;
        } else if (containsAny(vendorAndRenderer, {"Qualcomm", "Adreno"})) {
            m_dynamicParameters.GpuVendor = GpuVendorKind::Qualcomm;
        } else if (containsAny(vendorAndRenderer, {"Mali", "ARM"})) {
            m_dynamicParameters.GpuVendor = GpuVendorKind::Arm;
        } else if (containsAny(vendorAndRenderer, {"NVIDIA"})) {
            m_dynamicParameters.GpuVendor = GpuVendorKind::Nvidia;
        } else if (containsAny(vendorAndRenderer, {"AMD", "Radeon"})) {
            m_dynamicParameters.GpuVendor = GpuVendorKind::Amd;
        } else if (containsAny(vendorAndRenderer, {"Intel"})) {
            m_dynamicParameters.GpuVendor = GpuVendorKind::Intel;
        } else if (containsAny(vendorAndRenderer, {"Imagination", "PowerVR"})) {
            m_dynamicParameters.GpuVendor = GpuVendorKind::ImgTec;
        } else {
            m_dynamicParameters.GpuVendor = GpuVendorKind::Unknown;
        }
    }

    const MG_External::GLESFunctionsTable& BackendObject_DirectGLES::GetGLESFunctions() const {
        return m_GLESFunctions;
    }

    const MG_External::EGLFunctionsTable& BackendObject_DirectGLES::GetEGLFunctions() const {
        return m_EGLFunctions;
    }
} // namespace MobileGL::MG_Backend::DirectGLES
