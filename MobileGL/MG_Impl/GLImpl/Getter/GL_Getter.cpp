// MobileGL - MobileGL/MG_Impl/GLImpl/Getter/GL_Getter.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "GL_Getter.h"
#include <Config.h>
#include <MGGitHash.h>
#include <MG_State/EGLState/Core.h>
#include <MG_State/GLState/Core.h>
#include <MG_State/GLState/ErrorState/ErrorInfo.h>
#include <MG_Util/Converters/GLToStr/GLEnumConverter.h>
#include <MG_Util/Converters/GLToMG/BufferEnumConverter.h>
#include <MG_Util/Converters/MGToGL/FramebufferEnumConverter.h>
#include <MG_Util/Converters/MGToGL/ErrorCodeConverter.h>
#include <MG_Util/Converters/MGToGL/TextureEnumConverter.h>
#include <MG_Util/Converters/MGToStr/GLExtensionConverter.h>
#include <MG_Util/Converters/MGToGL/RenderStateEnumConverter.h>
#include <MG_State/GLState/FramebufferState/FramebufferObject.h>
#include <MG_Util/Texture/TextureFormatProcessor.h>
#include <MG_Backend/BackendObjects.h>
#if !defined(_WIN32)
#include <dlfcn.h>
#endif

namespace MobileGL::MG_Impl::GLImpl {
    namespace {
        bool TryGetNativeContextFlags(GLint* flags) {
#if !defined(_WIN32)
            using GLGetIntegervFn = void (*)(GLenum, GLint*);
            static auto* nextGetIntegerv = reinterpret_cast<GLGetIntegervFn>(dlsym(RTLD_NEXT, "glGetIntegerv"));
            if (nextGetIntegerv) {
                nextGetIntegerv(GL_CONTEXT_FLAGS, flags);
                return true;
            }
#endif
            return false;
        }

        enum class IndexedBufferQueryKind {
            Binding,
            Start,
            Size,
        };

        void CopyFloatsToInts(const GLfloat* src, SizeT count, GLint* dst) {
            for (SizeT i = 0; i < count; ++i) {
                dst[i] = static_cast<GLint>(src[i]);
            }
        }

        constexpr GLint kFrontendMaxComputeUniformComponents = 1024;
        constexpr GLint kFrontendMaxComputeAtomicCounters = 8;
        constexpr GLint kFrontendMaxComputeAtomicCounterBuffers = 8;
        constexpr GLint kFrontendMaxComputeSharedMemorySize = 32768;
        constexpr GLint kFrontendMaxComputeWorkGroupInvocations = 1024;
        constexpr GLint kFrontendMaxCombinedAtomicCounters = 8;
        constexpr GLint kFrontendMaxFragmentAtomicCounters = 8;
        constexpr GLint kFrontendMaxGeometryAtomicCounters = 0;
        constexpr GLint kFrontendMaxTessControlAtomicCounters = 0;
        constexpr GLint kFrontendMaxTessEvaluationAtomicCounters = 0;
        constexpr GLint kFrontendMaxVertexAtomicCounters = 0;
        constexpr GLint kFrontendMaxVertexUniformComponents = 4096;
        constexpr GLint kFrontendMaxVertexUniformVectors = 128;
        constexpr GLint kFrontendMaxVertexUniformBlocks = 14;
        constexpr GLint kFrontendMaxVertexOutputComponents = 64;
        constexpr GLint kFrontendMaxFragmentInputComponents = 128;
        constexpr GLint kFrontendMaxFragmentUniformComponents = 4096;
        constexpr GLint kFrontendMaxFragmentUniformVectors = 256;
        constexpr GLint kFrontendMaxFragmentUniformBlocks = 14;
        constexpr GLint kFrontendMaxGeometryInputComponents = 64;
        constexpr GLint kFrontendMaxGeometryOutputComponents = 128;
        constexpr GLint kFrontendMaxGeometryTextureImageUnits = 16;
        constexpr GLint kFrontendMaxGeometryUniformComponents = 1024;
        constexpr GLint kFrontendMaxGeometryUniformBlocks = 14;
        constexpr GLint kFrontendMaxCombinedUniformBlocks = kFrontendMaxVertexUniformBlocks +
                                                            kFrontendMaxGeometryUniformBlocks +
                                                            kFrontendMaxFragmentUniformBlocks;
        constexpr GLint kFrontendMaxVaryingComponents = 64;
        constexpr GLint kFrontendMaxVaryingVectors = 8;
        constexpr GLint kFrontendMaxProgramTexelOffset = 7;
        constexpr GLint kFrontendMinProgramTexelOffset = -8;
        constexpr GLint kFrontendMaxTransformFeedbackInterleavedComponents = 64;
        constexpr GLint kFrontendMaxTransformFeedbackSeparateAttribs = 4;
        constexpr GLint kFrontendMaxTransformFeedbackSeparateComponents = 4;
        constexpr GLint kFrontendMaxGeometryOutputVertices = 256;
        constexpr GLint kFrontendMaxGeometryTotalOutputComponents = 1024;
        constexpr GLint kFrontendMinUniformBufferBindings = 36;
        constexpr GLint kFrontendSubpixelBits = 4;
        constexpr GLint kFrontendMaxSamples = 4;

        constexpr GLint GetMinComputeWorkGroupCount(GLuint index) {
            return index < 3 ? 65535 : 0;
        }

        constexpr GLint GetMinComputeWorkGroupSize(GLuint index) {
            return index < 2 ? 1024 : (index == 2 ? 64 : 0);
        }

        GLint GetMaxCombinedUniformComponents(GLint maxDefaultUniformComponents, GLint maxUniformBlocks,
                                              GLint maxUniformBlockSizeBytes) {
            return maxDefaultUniformComponents + maxUniformBlocks * (maxUniformBlockSizeBytes / 4);
        }

        bool TryDecodeIndexedBufferQuery(GLenum pname, BufferTarget& bufferTarget, IndexedBufferQueryKind& queryKind) {
            switch (pname) {
            case GL_UNIFORM_BUFFER_BINDING:
                bufferTarget = BufferTarget::Uniform;
                queryKind = IndexedBufferQueryKind::Binding;
                return true;
            case GL_UNIFORM_BUFFER_START:
                bufferTarget = BufferTarget::Uniform;
                queryKind = IndexedBufferQueryKind::Start;
                return true;
            case GL_UNIFORM_BUFFER_SIZE:
                bufferTarget = BufferTarget::Uniform;
                queryKind = IndexedBufferQueryKind::Size;
                return true;
            case GL_TRANSFORM_FEEDBACK_BUFFER_BINDING:
                bufferTarget = BufferTarget::TransformFeedback;
                queryKind = IndexedBufferQueryKind::Binding;
                return true;
            case GL_TRANSFORM_FEEDBACK_BUFFER_START:
                bufferTarget = BufferTarget::TransformFeedback;
                queryKind = IndexedBufferQueryKind::Start;
                return true;
            case GL_TRANSFORM_FEEDBACK_BUFFER_SIZE:
                bufferTarget = BufferTarget::TransformFeedback;
                queryKind = IndexedBufferQueryKind::Size;
                return true;
            case GL_ATOMIC_COUNTER_BUFFER_BINDING:
                bufferTarget = BufferTarget::AtomicCounter;
                queryKind = IndexedBufferQueryKind::Binding;
                return true;
            case GL_ATOMIC_COUNTER_BUFFER_START:
                bufferTarget = BufferTarget::AtomicCounter;
                queryKind = IndexedBufferQueryKind::Start;
                return true;
            case GL_ATOMIC_COUNTER_BUFFER_SIZE:
                bufferTarget = BufferTarget::AtomicCounter;
                queryKind = IndexedBufferQueryKind::Size;
                return true;
            case GL_SHADER_STORAGE_BUFFER_BINDING:
                bufferTarget = BufferTarget::ShaderStorage;
                queryKind = IndexedBufferQueryKind::Binding;
                return true;
            case GL_SHADER_STORAGE_BUFFER_START:
                bufferTarget = BufferTarget::ShaderStorage;
                queryKind = IndexedBufferQueryKind::Start;
                return true;
            case GL_SHADER_STORAGE_BUFFER_SIZE:
                bufferTarget = BufferTarget::ShaderStorage;
                queryKind = IndexedBufferQueryKind::Size;
                return true;
            default:
                return false;
            }
        }

        bool IsIndexedBufferBindingQueryKind(IndexedBufferQueryKind queryKind) {
            return queryKind == IndexedBufferQueryKind::Binding;
        }

        bool IsIndexedBufferRangeQueryKind(IndexedBufferQueryKind queryKind) {
            return queryKind == IndexedBufferQueryKind::Start || queryKind == IndexedBufferQueryKind::Size;
        }

        SizeT GetIndexedBufferQueryPointCount(BufferTarget bufferTarget) {
            const SizeT frontendCount = MG_State::pGLContext->GetBufferBindingPointCount(bufferTarget);
            if (bufferTarget == BufferTarget::ShaderStorage && MG_Backend::pActiveBackendObject) {
                const Int backendCount =
                    MG_Backend::pActiveBackendObject->GetDynamicParameters().MaxShaderStorageBufferBindings;
                return std::min(frontendCount, static_cast<SizeT>(std::max(backendCount, 0)));
            }
            return frontendCount;
        }

        bool TryDecodeDrawBufferQuery(GLenum pname, SizeT& drawBufferIndex) {
            if (pname == GL_DRAW_BUFFER) {
                drawBufferIndex = 0;
                return true;
            }
            if (pname >= GL_DRAW_BUFFER0 && pname <= GL_DRAW_BUFFER15) {
                drawBufferIndex = static_cast<SizeT>(pname - GL_DRAW_BUFFER0);
                return true;
            }
            return false;
        }

        bool TryResolveImplementationColorReadParams(GLint& outFormat, GLint& outType) {
            const auto& readFbo =
                MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Read).GetBoundObject();
            if (!readFbo) return false;

            const auto attachmentType = readFbo->GetReadBuffer();
            if (attachmentType == FramebufferAttachmentType::None) return false;

            const auto& attachment = readFbo->GetAttachment(attachmentType);
            TextureInternalFormat internalFormat = TextureInternalFormat::Unknown;
            if (attachment.IsTexture() && attachment.GetTexture()) {
                internalFormat = attachment.GetTexture()->GetFormat();
            } else if (attachment.IsRenderbuffer() && attachment.GetRenderbuffer()) {
                internalFormat = attachment.GetRenderbuffer()->GetInternalFormat();
            }

            if (internalFormat == TextureInternalFormat::Unknown) return false;

            const GLenum glInternalFormat = MG_Util::ConvertTextureInternalFormatToGLEnum(internalFormat);
            GLenum normalizedInternalFormat = glInternalFormat;
            GLenum format = GL_RGBA;
            GLenum type = GL_UNSIGNED_BYTE;
            MG_Util::TextureFormatProcessor::NormalizePixelFormat(glInternalFormat, PixelFormatNormalizeOptionBit::None,
                                                                  &normalizedInternalFormat, &format, &type);
            outFormat = static_cast<GLint>(format);
            outType = static_cast<GLint>(type);
            return true;
        }

        GLint ResolveDrawFramebufferSampleCount() {
            const auto& drawFbo =
                MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Draw).GetBoundObject();
            if (!drawFbo) return 0;

            GLint maxSamples = 0;
            for (const auto& attachment : drawFbo->GetAllAttachmentObjects()) {
                if (!attachment.IsRenderbuffer() || !attachment.GetRenderbuffer()) continue;
                maxSamples = std::max(maxSamples, static_cast<GLint>(attachment.GetRenderbuffer()->GetSamples()));
            }
            return maxSamples;
        }

        void RecordIndexedOnlyGetterError(const char* functionName, GLenum pname) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", functionName,
                    "pname " + MG_Util::ConvertGLEnumToString(pname) +
                        " is only valid with indexed getter entrypoints."));
        }

        bool ValidateIndexedBufferQueryIndex(GLenum pname, GLuint index, const char* functionName,
                                             BufferTarget bufferTarget) {
            switch (bufferTarget) {
            case BufferTarget::Uniform:
            case BufferTarget::TransformFeedback:
            case BufferTarget::AtomicCounter:
            case BufferTarget::ShaderStorage:
                break;
            default:
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidEnum,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", functionName,
                                                 "pname " + std::to_string(pname) +
                                                     " is not a supported indexed buffer query."));
                return false;
            }

            if (index >= GetIndexedBufferQueryPointCount(bufferTarget)) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>(
                        "MG_Impl/GLImpl", functionName,
                        "index " + std::to_string(index) + " is out of range for indexed buffer query " +
                            std::to_string(pname) + "."));
                return false;
            }

            return true;
        }

        void CopyIntsToBooleans(const GLint* src, SizeT count, GLboolean* dst) {
            for (SizeT i = 0; i < count; ++i) {
                dst[i] = src[i] ? GL_TRUE : GL_FALSE;
            }
        }

        void CopyIntsToFloats(const GLint* src, SizeT count, GLfloat* dst) {
            for (SizeT i = 0; i < count; ++i) {
                dst[i] = static_cast<GLfloat>(src[i]);
            }
        }
    } // namespace

    /* @INSERTION_POINT:FUNCTION_IMPLEMENTATION@ */
    const GLubyte* GetString(GLenum name) {
        static String vendorString;
        static String versionStr;
        static String rendererString;
        static String shadingLanguageVersion;
        static String extensionsString;
        const auto& activeBackendObject = MG_Backend::pActiveBackendObject;

        MGLOG_D("glGetString, name: %s", MG_Util::ConvertGLEnumToString(name).c_str());
        if (!activeBackendObject) {
            MGLOG_E("activeBackendObject is not initialized!");
            return (GLubyte*)"Unknown";
        }

        const auto& rendererInfo = activeBackendObject->GetRendererInfo();

        switch (name) {
        case GL_VENDOR:
            if (rendererInfo.ExtraVendor.has_value()) {
                vendorString = std::format("{}{}", MG_Config::CoreVendor, rendererInfo.ExtraVendor.value());
            } else {
                vendorString = MG_Config::CoreVendor;
            }
            MGLOG_D("vendorString: %s", vendorString.c_str());
            return (const GLubyte*)vendorString.c_str();
        case GL_VERSION: {
            versionStr =
                std::format("{} {} {}, {} Backend, GIT@" GIT_COMMIT_HASH_SHORT,
                            rendererInfo.RendererGLInfo.TargetGLVersion.toString(), MG_Config::ProjectName,
                            MG_Config::CoreVersion.toFormattedString(MG_Config::DefaultVersionStringFormatAttrib),
                            rendererInfo.BackendName);
            MGLOG_D("versionStr: %s", versionStr.c_str());
            return (const GLubyte*)versionStr.c_str();
        }
        case GL_RENDERER: {
            String backendVersionStr = activeBackendObject->GetBackendAPIVersionString();
            rendererString =
                std::format("{} ({}) ({})", rendererInfo.RendererName, MG_Config::CoreName, backendVersionStr);
            MGLOG_D("rendererString: %s", rendererString.c_str());
            return (const GLubyte*)rendererString.c_str();
        }
        case GL_SHADING_LANGUAGE_VERSION:
            shadingLanguageVersion =
                std::format("{} {}", rendererInfo.RendererGLInfo.TargetGLSLVersion.toString({true, false}),
                            MG_Config::ProjectName);
            MGLOG_D("shadingLanguageVersion: %s", shadingLanguageVersion.c_str());
            return (const GLubyte*)shadingLanguageVersion.c_str();
        case GL_EXTENSIONS:
            extensionsString.clear();
            for (const auto& ext : rendererInfo.RendererGLInfo.Extensions) {
                if (!extensionsString.empty()) {
                    extensionsString += " ";
                }
                extensionsString += MG_Util::ConvertGLExtToString(ext);
            }
            return (const GLubyte*)extensionsString.c_str();
        default:
            return (const GLubyte*)"Unknown Enum";
        }
    }

    const GLubyte* GetStringi(GLenum name, GLuint index) {
        MGLOG_D("glGetStringi, name: %s, index: %u", MG_Util::ConvertGLEnumToString(name).c_str(), index);
        if (name != GL_EXTENSIONS) {
            return (const GLubyte*)"";
        }

        const auto& activeBackendObject = MG_Backend::pActiveBackendObject;
        if (!activeBackendObject) {
            MGLOG_E("activeBackendObject is not initialized!");
            return (GLubyte*)"Unknown";
        }
        const auto& rendererInfo = activeBackendObject->GetRendererInfo();

        const auto& exts = rendererInfo.RendererGLInfo.Extensions;
        if (index >= exts.size()) {
            return nullptr;
        }

        static Vector<String> extStrings;
        extStrings.clear();
        extStrings.reserve(exts.size());
        for (const auto& ext : exts) {
            extStrings.emplace_back(MG_Util::ConvertGLExtToString(ext));
        }

        return (const GLubyte*)extStrings[index].c_str();
    }

    void GetBooleanv(GLenum pname, GLboolean* params) {
        MGLOG_D("glGetBooleanv, pname: %s", MG_Util::ConvertGLEnumToString(pname).c_str());
        if (!params) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "params pointer cannot be null"));
            return;
        }

        switch (pname) {
        case GL_BLEND:
            *params = MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::Blend) ? GL_TRUE : GL_FALSE;
            return;
        case GL_BLEND_COLOR: {
            const FloatVec4& blendColor = MG_State::pGLContext->GetBlendColor();
            params[0] = blendColor.x() != 0.0f ? GL_TRUE : GL_FALSE;
            params[1] = blendColor.y() != 0.0f ? GL_TRUE : GL_FALSE;
            params[2] = blendColor.z() != 0.0f ? GL_TRUE : GL_FALSE;
            params[3] = blendColor.w() != 0.0f ? GL_TRUE : GL_FALSE;
            return;
        }
        case GL_COLOR_CLEAR_VALUE: {
            const FloatVec4& clearColor = MG_State::pGLContext->GetClearColor();
            params[0] = clearColor.x() != 0.0f ? GL_TRUE : GL_FALSE;
            params[1] = clearColor.y() != 0.0f ? GL_TRUE : GL_FALSE;
            params[2] = clearColor.z() != 0.0f ? GL_TRUE : GL_FALSE;
            params[3] = clearColor.w() != 0.0f ? GL_TRUE : GL_FALSE;
            return;
        }
        case GL_COLOR_WRITEMASK: {
            BoolVec4 mask = MG_State::pGLContext->GetColorMask();
            params[0] = mask.x() ? GL_TRUE : GL_FALSE;
            params[1] = mask.y() ? GL_TRUE : GL_FALSE;
            params[2] = mask.z() ? GL_TRUE : GL_FALSE;
            params[3] = mask.w() ? GL_TRUE : GL_FALSE;
            return;
        }
        case GL_DEPTH_CLEAR_VALUE:
            *params = MG_State::pGLContext->GetClearDepth() != 0.0f ? GL_TRUE : GL_FALSE;
            return;
        case GL_DEPTH_RANGE: {
            const FloatVec2& depthRange = MG_State::pGLContext->GetDepthRange();
            params[0] = depthRange.x() != 0.0f ? GL_TRUE : GL_FALSE;
            params[1] = depthRange.y() != 0.0f ? GL_TRUE : GL_FALSE;
            return;
        }
        case GL_LINE_WIDTH:
            *params = MG_State::pGLContext->GetLineWidth() != 0.0f ? GL_TRUE : GL_FALSE;
            return;
        case GL_POINT_SIZE:
            *params = MG_State::pGLContext->GetPointSize() != 0.0f ? GL_TRUE : GL_FALSE;
            return;
        case GL_CULL_FACE:
            *params = MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::CullFace) ? GL_TRUE : GL_FALSE;
            return;
        case GL_DEPTH_TEST:
            *params = MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::DepthTest) ? GL_TRUE : GL_FALSE;
            return;
        case GL_DEPTH_WRITEMASK:
            *params = MG_State::pGLContext->GetDepthMask() ? GL_TRUE : GL_FALSE;
            return;
        case GL_DITHER:
            *params = MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::Dither) ? GL_TRUE : GL_FALSE;
            return;
        case GL_MULTISAMPLE:
            *params = MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::Multisample) ? GL_TRUE : GL_FALSE;
            return;
        case GL_POLYGON_OFFSET_FILL:
            *params =
                MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::PolygonOffsetFill) ? GL_TRUE : GL_FALSE;
            return;
        case GL_PRIMITIVE_RESTART_FIXED_INDEX:
            *params = MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::PrimitiveRestartFixedIndex)
                          ? GL_TRUE
                          : GL_FALSE;
            return;
        case GL_RASTERIZER_DISCARD:
            *params =
                MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::RasterizerDiscard) ? GL_TRUE : GL_FALSE;
            return;
        case GL_SAMPLE_ALPHA_TO_COVERAGE:
            *params = MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::SampleAlphaToCoverage)
                          ? GL_TRUE
                          : GL_FALSE;
            return;
        case GL_SAMPLE_ALPHA_TO_ONE:
            *params =
                MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::SampleAlphaToOne) ? GL_TRUE : GL_FALSE;
            return;
        case GL_SAMPLE_COVERAGE:
            *params = MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::SampleCoverage)
                          ? GL_TRUE
                          : GL_FALSE;
            return;
        case GL_SAMPLE_MASK:
            *params =
                MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::SampleMask) ? GL_TRUE : GL_FALSE;
            return;
        case GL_SCISSOR_TEST:
            *params = MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::ScissorTest) ? GL_TRUE : GL_FALSE;
            return;
        case GL_STENCIL_TEST:
            *params = MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::StencilTest) ? GL_TRUE : GL_FALSE;
            return;
        default:
            break;
        }

        GLint ints[4] = {};
        GetIntegerv(pname, ints);
        switch (pname) {
        case GL_COLOR_WRITEMASK:
        case GL_SCISSOR_BOX:
            CopyIntsToBooleans(ints, 4, params);
            return;
        default:
            *params = ints[0] ? GL_TRUE : GL_FALSE;
            return;
        }
    }

    void GetFloatv(GLenum pname, GLfloat* params) {
        MGLOG_D("glGetFloatv, pname: %s", MG_Util::ConvertGLEnumToString(pname).c_str());
        if (!params) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "params pointer cannot be null"));
            return;
        }

        switch (pname) {
        case GL_BLEND_COLOR: {
            const FloatVec4& blendColor = MG_State::pGLContext->GetBlendColor();
            params[0] = blendColor.x();
            params[1] = blendColor.y();
            params[2] = blendColor.z();
            params[3] = blendColor.w();
            return;
        }
        case GL_COLOR_CLEAR_VALUE: {
            const FloatVec4& clearColor = MG_State::pGLContext->GetClearColor();
            params[0] = clearColor.x();
            params[1] = clearColor.y();
            params[2] = clearColor.z();
            params[3] = clearColor.w();
            return;
        }
        case GL_DEPTH_RANGE: {
            const FloatVec2& depthRange = MG_State::pGLContext->GetDepthRange();
            params[0] = depthRange.x();
            params[1] = depthRange.y();
            return;
        }
        case GL_VIEWPORT_BOUNDS_RANGE: {
            const auto& dynamicParameters = MG_Backend::pActiveBackendObject->GetDynamicParameters();
            params[0] = dynamicParameters.ViewportBoundsRangeMin;
            params[1] = dynamicParameters.ViewportBoundsRangeMax;
            return;
        }
        case GL_DEPTH_CLEAR_VALUE:
            params[0] = MG_State::pGLContext->GetClearDepth();
            return;
        case GL_ALIASED_LINE_WIDTH_RANGE: {
            const auto& dynamicParameters = MG_Backend::pActiveBackendObject->GetDynamicParameters();
            params[0] = dynamicParameters.AliasedLineWidthRangeMin;
            params[1] = dynamicParameters.AliasedLineWidthRangeMax;
            return;
        }
        case GL_ALIASED_POINT_SIZE_RANGE:
        case GL_POINT_SIZE_RANGE: {
            const auto& dynamicParameters = MG_Backend::pActiveBackendObject->GetDynamicParameters();
            params[0] = dynamicParameters.PointSizeRangeMin;
            params[1] = dynamicParameters.PointSizeRangeMax;
            return;
        }
        case GL_LINE_WIDTH:
            params[0] = MG_State::pGLContext->GetLineWidth();
            return;
        case GL_POINT_SIZE:
            params[0] = MG_State::pGLContext->GetPointSize();
            return;
        case GL_POLYGON_OFFSET_FACTOR:
            params[0] = MG_State::pGLContext->GetPolygonOffsetFactor();
            return;
        case GL_POLYGON_OFFSET_UNITS:
            params[0] = MG_State::pGLContext->GetPolygonOffsetUnits();
            return;
        case GL_SMOOTH_LINE_WIDTH_RANGE: {
            const auto& dynamicParameters = MG_Backend::pActiveBackendObject->GetDynamicParameters();
            params[0] = dynamicParameters.SmoothLineWidthRangeMin;
            params[1] = dynamicParameters.SmoothLineWidthRangeMax;
            return;
        }
        case GL_SMOOTH_LINE_WIDTH_GRANULARITY:
            params[0] = MG_Backend::pActiveBackendObject->GetDynamicParameters().SmoothLineWidthGranularity;
            return;
        case GL_POINT_SIZE_GRANULARITY:
            params[0] = MG_Backend::pActiveBackendObject->GetDynamicParameters().PointSizeGranularity;
            return;
        case GL_SAMPLE_COVERAGE_VALUE:
            params[0] = MG_State::pGLContext->GetSampleCoverageValue();
            return;
        default:
            break;
        }

        GLint ints[4] = {};
        GetIntegerv(pname, ints);
        switch (pname) {
        case GL_ALIASED_LINE_WIDTH_RANGE:
        case GL_ALIASED_POINT_SIZE_RANGE:
        case GL_SMOOTH_LINE_WIDTH_RANGE:
        case GL_POINT_SIZE_RANGE:
        case GL_MAX_VIEWPORT_DIMS:
            CopyIntsToFloats(ints, 2, params);
            return;
        case GL_SCISSOR_BOX:
        case GL_VIEWPORT:
            CopyIntsToFloats(ints, 4, params);
            return;
        default:
            params[0] = static_cast<GLfloat>(ints[0]);
            return;
        }
    }

    void GetIntegeri_v(GLenum target, GLuint index, GLint* data) {
        if (!data) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "data pointer cannot be null"));
            return;
        }

        BufferTarget bufferTarget = BufferTarget::Unknown;
        IndexedBufferQueryKind queryKind = IndexedBufferQueryKind::Binding;
        if (TryDecodeIndexedBufferQuery(target, bufferTarget, queryKind)) {
            if (!ValidateIndexedBufferQueryIndex(target, index, __func__, bufferTarget)) return;
            const auto& bindingPoint = MG_State::pGLContext->GetBufferBindingPoint(bufferTarget, index);
            const auto& bufferObject = bindingPoint.GetBoundObject();
            if (!bufferObject) {
                *data = 0;
                return;
            }

            switch (queryKind) {
            case IndexedBufferQueryKind::Binding:
                *data = static_cast<GLint>(bufferObject->GetExternalIndex());
                return;
            case IndexedBufferQueryKind::Start:
                if (!bindingPoint.HasExplicitRange()) {
                    *data = 0;
                    return;
                }
                *data = static_cast<GLint>(bindingPoint.GetRange().start);
                return;
            case IndexedBufferQueryKind::Size: {
                if (!bindingPoint.HasExplicitRange()) {
                    *data = 0;
                    return;
                }
                const Range1D range = bindingPoint.GetRange();
                const auto start = std::min(range.start, bufferObject->GetSize());
                const auto end = std::min(range.end, bufferObject->GetSize());
                *data = static_cast<GLint>(end - start);
                return;
            }
            default:
                break;
            }
        }

        auto getIntegeri = MG_Backend::gBackendFunctionsTable.GL.GetIntegeri_v;
        if (target == GL_MAX_COMPUTE_WORK_GROUP_COUNT || target == GL_MAX_COMPUTE_WORK_GROUP_SIZE) {
            if (index >= 3) {
                *data = 0;
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                                 "Compute work group index is out of range."));
                return;
            }

            const GLint minimum = target == GL_MAX_COMPUTE_WORK_GROUP_COUNT
                ? GetMinComputeWorkGroupCount(index)
                : GetMinComputeWorkGroupSize(index);
            GLint backendValue = 0;
            if (getIntegeri) {
                getIntegeri(target, index, &backendValue);
            }
            *data = std::max(backendValue, minimum);
            return;
        }

        if (!getIntegeri) {
            *data = 0;
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Backend does not support indexed integer queries."));
            return;
        }
        getIntegeri(target, index, data);
    }

    void GetInteger64i_v(GLenum target, GLuint index, GLint64* data) {
        if (!data) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "data pointer cannot be null"));
            return;
        }

        BufferTarget bufferTarget = BufferTarget::Unknown;
        IndexedBufferQueryKind queryKind = IndexedBufferQueryKind::Binding;
        if (TryDecodeIndexedBufferQuery(target, bufferTarget, queryKind)) {
            if (!ValidateIndexedBufferQueryIndex(target, index, __func__, bufferTarget)) return;
            const auto& bindingPoint = MG_State::pGLContext->GetBufferBindingPoint(bufferTarget, index);
            const auto& bufferObject = bindingPoint.GetBoundObject();
            if (!bufferObject) {
                *data = 0;
                return;
            }

            const Range1D range = bindingPoint.GetRange();
            switch (queryKind) {
            case IndexedBufferQueryKind::Binding:
                *data = static_cast<GLint64>(bufferObject->GetExternalIndex());
                return;
            case IndexedBufferQueryKind::Start:
                if (!bindingPoint.HasExplicitRange()) {
                    *data = 0;
                    return;
                }
                *data = static_cast<GLint64>(range.start);
                return;
            case IndexedBufferQueryKind::Size: {
                if (!bindingPoint.HasExplicitRange()) {
                    *data = 0;
                    return;
                }
                const auto start = std::min(range.start, bufferObject->GetSize());
                const auto end = std::min(range.end, bufferObject->GetSize());
                *data = static_cast<GLint64>(end - start);
                return;
            }
            default:
                break;
            }
        }

        auto getInteger64i = MG_Backend::gBackendFunctionsTable.GL.GetInteger64i_v;
        if (!getInteger64i) {
            *data = 0;
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Backend does not support indexed integer queries."));
            return;
        }
        getInteger64i(target, index, data);
    }

    void GetInteger64v(GLenum pname, GLint64* params) {
        MGLOG_D("glGetInteger64v, pname: %s", MG_Util::ConvertGLEnumToString(pname).c_str());
        if (!params) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "params pointer cannot be null"));
            return;
        }

        switch (pname) {
        case GL_MAX_SHADER_STORAGE_BLOCK_SIZE:
            if (MG_Backend::pActiveBackendObject) {
                params[0] = static_cast<GLint64>(
                    MG_Backend::pActiveBackendObject->GetDynamicParameters().MaxShaderStorageBlockSize);
            } else {
                params[0] = static_cast<GLint64>(MG_Backend::DynamicBackendParameters{}.MaxShaderStorageBlockSize);
            }
            return;
        case GL_SUBGROUP_SIZE_KHR:
        case GL_SUBGROUP_SUPPORTED_STAGES_KHR:
        case GL_SUBGROUP_SUPPORTED_FEATURES_KHR:
        case GL_SUBGROUP_QUAD_ALL_STAGES_KHR: {
            GLint value = 0;
            GetIntegerv(pname, &value);
            params[0] = static_cast<GLint64>(value);
            return;
        }
        default:
            break;
        }

        GLint ints[4] = {};
        GetIntegerv(pname, ints);

        switch (pname) {
        case GL_BLEND_COLOR:
        case GL_COLOR_CLEAR_VALUE:
        case GL_COLOR_WRITEMASK:
        case GL_SCISSOR_BOX:
        case GL_VIEWPORT:
            for (int i = 0; i < 4; ++i) {
                params[i] = static_cast<GLint64>(ints[i]);
            }
            return;
        case GL_DEPTH_RANGE:
        case GL_ALIASED_POINT_SIZE_RANGE:
        case GL_MAX_VIEWPORT_DIMS:
        case GL_POINT_SIZE_RANGE:
        case GL_VIEWPORT_BOUNDS_RANGE:
            params[0] = static_cast<GLint64>(ints[0]);
            params[1] = static_cast<GLint64>(ints[1]);
            return;
        default:
            params[0] = static_cast<GLint64>(ints[0]);
            return;
        }
    }

    void GetIntegerv(GLenum pname, GLint* params) {
        MGLOG_D("glGetIntegerv, pname: %s", MG_Util::ConvertGLEnumToString(pname).c_str());
        if (!params) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "GetIntegerv", "params pointer cannot be null"));
            return;
        }

        switch (pname) {
        case GL_ACTIVE_TEXTURE:
            *params = MG_State::pGLContext->GetActiveTextureUnit() + GL_TEXTURE0;
            return;
        case GL_ARRAY_BUFFER_BINDING: {
            auto& obj = MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::Vertex).GetBoundObject();
            if (obj)
                *params = (GLint)obj->GetExternalIndex();
            else
                *params = 0;
            return;
        }
        case GL_BLEND:
            *params = MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::Blend) ? GL_TRUE : GL_FALSE;
            return;
        case GL_BLEND_COLOR: {
            const FloatVec4& blendColor = MG_State::pGLContext->GetBlendColor();
            params[0] = static_cast<GLint>(blendColor.x());
            params[1] = static_cast<GLint>(blendColor.y());
            params[2] = static_cast<GLint>(blendColor.z());
            params[3] = static_cast<GLint>(blendColor.w());
            return;
        }
        case GL_BLEND_DST_ALPHA: {
            BlendFactor srcRGB, dstRGB, srcAlpha, dstAlpha;
            MG_State::pGLContext->GetBlendFunc(srcRGB, dstRGB, srcAlpha, dstAlpha);
            *params = static_cast<GLint>(MG_Util::ConvertBlendFactorToGLEnum(dstAlpha));
            return;
        }
        case GL_BLEND_DST_RGB: {
            BlendFactor srcRGB, dstRGB, srcAlpha, dstAlpha;
            MG_State::pGLContext->GetBlendFunc(srcRGB, dstRGB, srcAlpha, dstAlpha);
            *params = static_cast<GLint>(MG_Util::ConvertBlendFactorToGLEnum(dstRGB));
            return;
        }
        case GL_BLEND_EQUATION_RGB: {
            BlendEquation colorEquation = BlendEquation::Add;
            BlendEquation alphaEquation = BlendEquation::Add;
            MG_State::pGLContext->GetBlendEquation(colorEquation, alphaEquation);
            *params = static_cast<GLint>(MG_Util::ConvertBlendEquationToGLEnum(colorEquation));
            return;
        }
        case GL_BLEND_EQUATION_ALPHA: {
            BlendEquation colorEquation = BlendEquation::Add;
            BlendEquation alphaEquation = BlendEquation::Add;
            MG_State::pGLContext->GetBlendEquation(colorEquation, alphaEquation);
            *params = static_cast<GLint>(MG_Util::ConvertBlendEquationToGLEnum(alphaEquation));
            return;
        }
        case GL_BLEND_SRC_ALPHA: {
            BlendFactor srcRGB, dstRGB, srcAlpha, dstAlpha;
            MG_State::pGLContext->GetBlendFunc(srcRGB, dstRGB, srcAlpha, dstAlpha);
            *params = static_cast<GLint>(MG_Util::ConvertBlendFactorToGLEnum(srcAlpha));
            return;
        }
        case GL_BLEND_SRC_RGB: {
            BlendFactor srcRGB, dstRGB, srcAlpha, dstAlpha;
            MG_State::pGLContext->GetBlendFunc(srcRGB, dstRGB, srcAlpha, dstAlpha);
            *params = static_cast<GLint>(MG_Util::ConvertBlendFactorToGLEnum(srcRGB));
            return;
        }
        case GL_COLOR_CLEAR_VALUE: {
            const FloatVec4& clearColor = MG_State::pGLContext->GetClearColor();
            params[0] = static_cast<GLint>(clearColor.x());
            params[1] = static_cast<GLint>(clearColor.y());
            params[2] = static_cast<GLint>(clearColor.z());
            params[3] = static_cast<GLint>(clearColor.w());
            return;
        }
        case GL_COLOR_LOGIC_OP:
            *params = MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::ColorLogicOp) ? GL_TRUE : GL_FALSE;
            return;
        case GL_COLOR_WRITEMASK: {
            BoolVec4 mask = MG_State::pGLContext->GetColorMask();
            params[0] = mask.x() ? GL_TRUE : GL_FALSE;
            params[1] = mask.y() ? GL_TRUE : GL_FALSE;
            params[2] = mask.z() ? GL_TRUE : GL_FALSE;
            params[3] = mask.w() ? GL_TRUE : GL_FALSE;
            return;
        }
        case GL_COMPRESSED_TEXTURE_FORMATS:
            *params = 0; // compressed texture upload entrypoints are still unimplemented
            return;
        case GL_MAX_COMPUTE_UNIFORM_COMPONENTS:
            *params = kFrontendMaxComputeUniformComponents;
            return;
        case GL_MAX_COMPUTE_ATOMIC_COUNTERS:
            *params = kFrontendMaxComputeAtomicCounters;
            return;
        case GL_MAX_COMPUTE_ATOMIC_COUNTER_BUFFERS:
            *params = kFrontendMaxComputeAtomicCounterBuffers;
            return;
        case GL_MAX_COMPUTE_SHARED_MEMORY_SIZE:
            *params = kFrontendMaxComputeSharedMemorySize;
            return;
        case GL_DISPATCH_INDIRECT_BUFFER_BINDING: {
            auto& obj = MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::DispatchIndirect).GetBoundObject();
            *params = obj ? static_cast<GLint>(obj->GetExternalIndex()) : 0;
            return;
        }
        case GL_MAX_DEBUG_GROUP_STACK_DEPTH:
            *params = 0; // debug-group entrypoints are stubbed
            return;
        case GL_MAX_DEBUG_MESSAGE_LENGTH:
            *params = 1024; // debug-message entrypoints are stubbed, but KHR_debug requires a valid limit
            return;
        case GL_DEBUG_GROUP_STACK_DEPTH:
            *params = 0; // debug-group entrypoints are stubbed
            return;
        case GL_CONTEXT_FLAGS: {
            *params = MG_State::pEGLContext ? MG_State::pEGLContext->GetCurrentContextFlags() : 0;
            if (*params == 0) {
                TryGetNativeContextFlags(params);
            }
            return;
        }
        case GL_CULL_FACE:
            *params = MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::CullFace) ? GL_TRUE : GL_FALSE;
            return;
        case GL_CULL_FACE_MODE:
            *params = static_cast<GLint>(MG_Util::ConvertCullFaceModeToGLEnum(MG_State::pGLContext->GetCullFaceMode()));
            return;
        case GL_FRONT_FACE:
            *params = static_cast<GLint>(MG_Util::ConvertFrontFaceModeToGLEnum(MG_State::pGLContext->GetFrontFaceMode()));
            return;
        case GL_CURRENT_PROGRAM: {
            const auto& currentProgram = MG_State::pGLContext->GetCurrentProgram();
            *params = currentProgram ? (GLint)currentProgram->GetExternalIndex() : 0;
            return;
        }
        case GL_DEPTH_CLEAR_VALUE:
            *params = (GLint)MG_State::pGLContext->GetClearDepth();
            return;
        case GL_DEPTH_FUNC:
            *params = (GLint)MG_Util::ConvertDepthTestFuncToGLEnum(MG_State::pGLContext->GetDepthFunc());
            return;
        case GL_DEPTH_RANGE: {
            const FloatVec2& depthRange = MG_State::pGLContext->GetDepthRange();
            params[0] = static_cast<GLint>(depthRange.x());
            params[1] = static_cast<GLint>(depthRange.y());
            return;
        }
        case GL_DEPTH_TEST:
            *params = MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::DepthTest) ? GL_TRUE : GL_FALSE;
            return;
        case GL_DEPTH_WRITEMASK:
            *params = MG_State::pGLContext->GetDepthMask() ? GL_TRUE : GL_FALSE;
            return;
        case GL_DEBUG_OUTPUT:
            *params = MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::DebugOutput) ? GL_TRUE : GL_FALSE;
            return;
        case GL_DEBUG_OUTPUT_SYNCHRONOUS:
            *params = MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::DebugOutputSynchronous)
                          ? GL_TRUE
                          : GL_FALSE;
            return;
        case GL_DITHER:
            *params = MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::Dither) ? GL_TRUE : GL_FALSE;
            return;
        case GL_DOUBLEBUFFER: {
            if (!MG_State::pEGLContext) {
                *params = 0;
                return;
            }
            const auto currentDrawSurface = MG_State::pEGLContext->GetCurrentSurface(EGL_DRAW);
            *params = MG_State::pEGLContext->IsDoubleBufferedSurface(currentDrawSurface) ? GL_TRUE : GL_FALSE;
            return;
        }
        case GL_DRAW_BUFFER:
        case GL_DRAW_BUFFER0:
        case GL_DRAW_BUFFER1:
        case GL_DRAW_BUFFER2:
        case GL_DRAW_BUFFER3:
        case GL_DRAW_BUFFER4:
        case GL_DRAW_BUFFER5:
        case GL_DRAW_BUFFER6:
        case GL_DRAW_BUFFER7:
        case GL_DRAW_BUFFER8:
        case GL_DRAW_BUFFER9:
        case GL_DRAW_BUFFER10:
        case GL_DRAW_BUFFER11:
        case GL_DRAW_BUFFER12:
        case GL_DRAW_BUFFER13:
        case GL_DRAW_BUFFER14:
        case GL_DRAW_BUFFER15:
            if (const auto& fbo = MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Draw)
                                      .GetBoundObject()) {
                SizeT drawBufferIndex = 0;
                const bool decoded = TryDecodeDrawBufferQuery(pname, drawBufferIndex);
                MOBILEGL_ASSERT(decoded, "Draw buffer query enum should have been decoded already: 0x%X", pname);
                if (drawBufferIndex < MG_State::GLState::FramebufferObject::MAX_DRAW_BUFFERS) {
                    *params = static_cast<GLint>(
                        MG_Util::ConvertFramebufferAttachmentTypeToGLEnum(fbo->GetDrawBuffers()[drawBufferIndex]));
                } else {
                    *params = GL_NONE;
                }
            } else {
                *params = 0;
            }
            return;
        case GL_DRAW_FRAMEBUFFER_BINDING: {
            const auto& FBO = MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Draw).GetBoundObject();
            *params = FBO ? (GLint)FBO->GetExternalIndex() : 0;
            return;
        }
        case GL_READ_FRAMEBUFFER_BINDING: {
            const auto& FBO = MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Read).GetBoundObject();
            *params = FBO ? (GLint)FBO->GetExternalIndex() : 0;
            return;
        }
        case GL_ELEMENT_ARRAY_BUFFER_BINDING: {
            if (!MG_State::pGLContext->GetBoundVertexArray()) {
                *params = 0;
                return;
            }
            const auto& bufferObject = MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::Index).GetBoundObject();
            *params = bufferObject ? (GLint)bufferObject->GetExternalIndex() : 0;
            return;
        }
        case GL_FRAGMENT_SHADER_DERIVATIVE_HINT:
            *params = GL_DONT_CARE;
            return;
        case GL_IMPLEMENTATION_COLOR_READ_FORMAT: {
            GLint format = 0;
            GLint type = 0;
            *params = TryResolveImplementationColorReadParams(format, type) ? format : 0;
            return;
        }
        case GL_IMPLEMENTATION_COLOR_READ_TYPE: {
            GLint format = 0;
            GLint type = 0;
            *params = TryResolveImplementationColorReadParams(format, type) ? type : 0;
            return;
        }
        case GL_LINE_SMOOTH:
            *params = MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::LineSmooth) ? GL_TRUE : GL_FALSE;
            return;
        case GL_LINE_SMOOTH_HINT:
            *params = GL_DONT_CARE;
            return;
        case GL_LINE_WIDTH:
            *params = static_cast<GLint>(MG_State::pGLContext->GetLineWidth());
            return;
        case GL_LAYER_PROVOKING_VERTEX:
            *params = GL_LAST_VERTEX_CONVENTION;
            return;
        case GL_LOGIC_OP_MODE:
            *params = static_cast<GLint>(MG_Util::ConvertLogicOperationToGLEnum(MG_State::pGLContext->GetLogicOp()));
            return;
        case GL_MAX_COMBINED_ATOMIC_COUNTERS:
            *params = kFrontendMaxCombinedAtomicCounters;
            return;
        case GL_MAX_COMBINED_UNIFORM_BLOCKS:
            *params = kFrontendMaxCombinedUniformBlocks;
            return;
        case GL_MAX_DUAL_SOURCE_DRAW_BUFFERS:
            *params = 1; // TODO
            return;
        case GL_MAX_ELEMENTS_INDICES:
            *params = 1024 * 1024; // TODO
            return;
        case GL_MAX_ELEMENTS_VERTICES:
            *params = 1024 * 1024; // TODO
            return;
        case GL_MAX_FRAGMENT_ATOMIC_COUNTERS:
            *params = kFrontendMaxFragmentAtomicCounters;
            return;
        case GL_MAX_FRAGMENT_SHADER_STORAGE_BLOCKS:
            *params = 16; // TODO
            return;
        case GL_MAX_FRAGMENT_INPUT_COMPONENTS:
            *params = kFrontendMaxFragmentInputComponents;
            return;
        case GL_MAX_FRAGMENT_IMAGE_UNIFORMS:
            if (MG_Backend::pActiveBackendObject &&
                MG_Backend::pActiveBackendObject->GetBackendType() == BackendType::DirectVulkan) {
                // TODO: Advertise fragment image uniforms on DirectVulkan once image-load-store draw paths do not trap.
                *params = 0;
            } else {
                // TODO: Track per-stage image uniform limits separately instead of reusing the compute/backend stage cap.
                *params = MG_Backend::pActiveBackendObject
                              ? MG_Backend::pActiveBackendObject->GetDynamicParameters().MaxComputeImageUniforms
                              : MG_Backend::DynamicBackendParameters{}.MaxComputeImageUniforms;
            }
            return;
        case GL_MAX_FRAGMENT_UNIFORM_COMPONENTS:
            *params = kFrontendMaxFragmentUniformComponents;
            return;
        case GL_MAX_FRAGMENT_UNIFORM_VECTORS:
            *params = kFrontendMaxFragmentUniformVectors;
            return;
        case GL_MAX_FRAGMENT_UNIFORM_BLOCKS:
            *params = kFrontendMaxFragmentUniformBlocks;
            return;
        case GL_MAX_GEOMETRY_ATOMIC_COUNTERS:
            *params = kFrontendMaxGeometryAtomicCounters;
            return;
        case GL_MAX_GEOMETRY_SHADER_STORAGE_BLOCKS:
            *params = 16; // TODO
            return;
        case GL_MAX_GEOMETRY_INPUT_COMPONENTS:
            *params = kFrontendMaxGeometryInputComponents;
            return;
        case GL_MAX_GEOMETRY_OUTPUT_COMPONENTS:
            *params = kFrontendMaxGeometryOutputComponents;
            return;
        case GL_MAX_GEOMETRY_OUTPUT_VERTICES:
            *params = kFrontendMaxGeometryOutputVertices;
            return;
        case GL_MAX_GEOMETRY_TEXTURE_IMAGE_UNITS:
            *params = kFrontendMaxGeometryTextureImageUnits;
            return;
        case GL_MAX_GEOMETRY_IMAGE_UNIFORMS:
            *params = 0;
            return;
        case GL_MAX_GEOMETRY_TOTAL_OUTPUT_COMPONENTS:
            *params = kFrontendMaxGeometryTotalOutputComponents;
            return;
        case GL_MAX_GEOMETRY_UNIFORM_BLOCKS:
            *params = kFrontendMaxGeometryUniformBlocks;
            return;
        case GL_MAX_GEOMETRY_UNIFORM_COMPONENTS:
            *params = kFrontendMaxGeometryUniformComponents;
            return;
        case GL_MAX_IMAGE_SAMPLES:
            *params = 0; // multisampled image load/store is not exposed by the DirectGLES frontend
            return;
        case GL_MULTISAMPLE:
            *params = MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::Multisample) ? GL_TRUE : GL_FALSE;
            return;
        case GL_MIN_MAP_BUFFER_ALIGNMENT:
            *params = 64; // TODO
            return;
        case GL_MAX_LABEL_LENGTH:
            *params = 256; // TODO
            return;
        case GL_MAX_PROGRAM_TEXEL_OFFSET:
            *params = kFrontendMaxProgramTexelOffset;
            return;
        case GL_MIN_PROGRAM_TEXEL_OFFSET:
            *params = kFrontendMinProgramTexelOffset;
            return;
        case GL_MAX_RECTANGLE_TEXTURE_SIZE:
            *params = 16 * 1024; // TODO
            return;
        case GL_MAX_SERVER_WAIT_TIMEOUT:
            *params = INT_MAX; // TODO
            return;
        case GL_MAX_TESS_CONTROL_ATOMIC_COUNTERS:
            *params = kFrontendMaxTessControlAtomicCounters;
            return;
        case GL_MAX_TESS_EVALUATION_ATOMIC_COUNTERS:
            *params = kFrontendMaxTessEvaluationAtomicCounters;
            return;
        case GL_MAX_TESS_CONTROL_IMAGE_UNIFORMS:
            *params = 0;
            return;
        case GL_MAX_TESS_EVALUATION_IMAGE_UNIFORMS:
            *params = 0;
            return;
        case GL_MAX_TESS_CONTROL_SHADER_STORAGE_BLOCKS:
            *params = 16; // TODO
            return;
        case GL_MAX_TESS_EVALUATION_SHADER_STORAGE_BLOCKS:
            *params = 16; // TODO
            return;
        case GL_MAX_TEXTURE_LOD_BIAS:
            *params = 15; // TODO
            return;
        case GL_MAX_UNIFORM_LOCATIONS:
            *params = 1024 * 4; // TODO
            return;
        case GL_MAX_VARYING_COMPONENTS:
            *params = kFrontendMaxVaryingComponents;
            return;
        case GL_MAX_VARYING_VECTORS:
            *params = kFrontendMaxVaryingVectors;
            return;
        case GL_MAX_VERTEX_ATOMIC_COUNTERS:
            *params = kFrontendMaxVertexAtomicCounters;
            return;
        case GL_MAX_VERTEX_IMAGE_UNIFORMS:
            *params = 0;
            return;
        case GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS:
            *params = 16; // TODO
            return;
        case GL_MAX_VERTEX_UNIFORM_COMPONENTS:
            *params = kFrontendMaxVertexUniformComponents;
            return;
        case GL_MAX_VERTEX_UNIFORM_VECTORS:
            *params = kFrontendMaxVertexUniformVectors;
            return;
        case GL_MAX_VERTEX_OUTPUT_COMPONENTS:
            *params = kFrontendMaxVertexOutputComponents;
            return;
        case GL_MAX_VERTEX_UNIFORM_BLOCKS:
            *params = kFrontendMaxVertexUniformBlocks;
            return;
        case GL_NUM_COMPRESSED_TEXTURE_FORMATS:
            *params = 0; // compressed texture upload entrypoints are still unimplemented
            return;
        case GL_NUM_PROGRAM_BINARY_FORMATS:
            *params = 0;
            return;
        case GL_NUM_SHADER_BINARY_FORMATS:
            *params = 0; // ShaderBinary entrypoints are stubbed
            return;
        case GL_PACK_ALIGNMENT:
            *params = MG_State::pGLContext->GetPixelStoreParam(PixelStoreParam::PackAlignment);
            return;
        case GL_PACK_IMAGE_HEIGHT:
            *params = MG_State::pGLContext->GetPixelStoreParam(PixelStoreParam::PackImageHeight);
            return;
        case GL_PACK_LSB_FIRST:
            *params = MG_State::pGLContext->GetPixelStoreParam(PixelStoreParam::PackLSBFirst);
            return;
        case GL_PACK_ROW_LENGTH:
            *params = MG_State::pGLContext->GetPixelStoreParam(PixelStoreParam::PackRowLength);
            return;
        case GL_PACK_SKIP_IMAGES:
            *params = MG_State::pGLContext->GetPixelStoreParam(PixelStoreParam::PackSkipImages);
            return;
        case GL_PACK_SKIP_PIXELS:
            *params = MG_State::pGLContext->GetPixelStoreParam(PixelStoreParam::PackSkipPixels);
            return;
        case GL_PACK_SKIP_ROWS:
            *params = MG_State::pGLContext->GetPixelStoreParam(PixelStoreParam::PackSkipRows);
            return;
        case GL_PACK_SWAP_BYTES:
            *params = MG_State::pGLContext->GetPixelStoreParam(PixelStoreParam::PackSwapBytes);
            return;
        case GL_PIXEL_PACK_BUFFER_BINDING:
            if (const auto& obj = MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::PixelPack).GetBoundObject()) {
                *params = static_cast<GLint>(obj->GetExternalIndex());
            } else {
                *params = 0;
            }
            return;
        case GL_PIXEL_UNPACK_BUFFER_BINDING:
            if (const auto& obj =
                    MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::PixelUnpack).GetBoundObject()) {
                *params = static_cast<GLint>(obj->GetExternalIndex());
            } else {
                *params = 0;
            }
            return;
        case GL_PARAMETER_BUFFER_BINDING_ARB: {
            auto& obj = MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::Parameter).GetBoundObject();
            *params = obj ? static_cast<GLint>(obj->GetExternalIndex()) : 0;
            return;
        }
        case GL_POINT_FADE_THRESHOLD_SIZE:
            *params = 1;
            return;
        case GL_PRIMITIVE_RESTART:
            *params = MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::PrimitiveRestart) ? GL_TRUE
                                                                                                    : GL_FALSE;
            return;
        case GL_PRIMITIVE_RESTART_INDEX:
            *params = 0; // fixed default; PrimitiveRestartIndex entrypoints are stubbed
            return;
        case GL_PROGRAM_BINARY_FORMATS:
            *params = 0; // program-binary entrypoints are stubbed
            return;
        case GL_PROGRAM_PIPELINE_BINDING:
            *params = 0; // program-pipeline entrypoints are stubbed
            return;
        case GL_PROGRAM_POINT_SIZE:
            *params = MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::ProgramPointSize) ? GL_TRUE : GL_FALSE;
            return;
        case GL_PROVOKING_VERTEX:
            *params = static_cast<GLint>(
                MG_Util::ConvertProvokingVertexModeToGLEnum(MG_State::pGLContext->GetProvokingVertexMode()));
            return;
        case GL_POINT_SIZE:
            *params = static_cast<GLint>(MG_State::pGLContext->GetPointSize());
            return;
        case GL_POLYGON_MODE:
            params[0] = GL_FILL;
            params[1] = GL_FILL;
            return;
        case GL_POLYGON_OFFSET_FACTOR:
            *params = static_cast<GLint>(MG_State::pGLContext->GetPolygonOffsetFactor());
            return;
        case GL_POLYGON_OFFSET_UNITS:
            *params = static_cast<GLint>(MG_State::pGLContext->GetPolygonOffsetUnits());
            return;
        case GL_POLYGON_OFFSET_FILL:
            *params =
                MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::PolygonOffsetFill) ? GL_TRUE : GL_FALSE;
            return;
        case GL_POLYGON_OFFSET_LINE:
            *params =
                MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::PolygonOffsetLine) ? GL_TRUE : GL_FALSE;
            return;
        case GL_POLYGON_OFFSET_POINT:
            *params =
                MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::PolygonOffsetPoint) ? GL_TRUE : GL_FALSE;
            return;
        case GL_POLYGON_SMOOTH:
            *params = MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::PolygonSmooth) ? GL_TRUE : GL_FALSE;
            return;
        case GL_POLYGON_SMOOTH_HINT:
            *params = GL_DONT_CARE;
            return;
        case GL_READ_BUFFER:
            if (const auto& fbo = MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Read)
                                      .GetBoundObject()) {
                *params =
                    static_cast<GLint>(MG_Util::ConvertFramebufferAttachmentTypeToGLEnum(fbo->GetReadBuffer()));
            } else {
                *params = 0;
            }
            return;
        case GL_RENDERBUFFER_BINDING:
            if (const auto& obj =
                    MG_State::pGLContext->GetRenderbufferBindingSlot(RenderbufferTarget::Renderbuffer).GetBoundObject()) {
                *params = static_cast<GLint>(obj->GetExternalIndex());
            } else {
                *params = 0;
            }
            return;
        case GL_SAMPLE_BUFFERS:
            *params = ResolveDrawFramebufferSampleCount() > 0 ? 1 : 0;
            return;
        case GL_SAMPLE_COVERAGE_VALUE:
            *params = static_cast<GLint>(MG_State::pGLContext->GetSampleCoverageValue());
            return;
        case GL_SAMPLE_COVERAGE_INVERT:
            *params = MG_State::pGLContext->GetSampleCoverageInvert() ? GL_TRUE : GL_FALSE;
            return;
        case GL_SAMPLE_ALPHA_TO_COVERAGE:
            *params =
                MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::SampleAlphaToCoverage) ? GL_TRUE : GL_FALSE;
            return;
        case GL_SAMPLE_ALPHA_TO_ONE:
            *params =
                MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::SampleAlphaToOne) ? GL_TRUE : GL_FALSE;
            return;
        case GL_SAMPLE_COVERAGE:
            *params = MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::SampleCoverage) ? GL_TRUE
                                                                                                  : GL_FALSE;
            return;
        case GL_SAMPLE_MASK:
            *params = MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::SampleMask) ? GL_TRUE : GL_FALSE;
            return;
        case GL_SAMPLE_MASK_VALUE:
            *params = static_cast<GLint>(MG_State::pGLContext->GetSampleMaskValue());
            return;
        case GL_SAMPLER_BINDING: {
            Int unit = MG_State::pGLContext->GetActiveTextureUnit();
            const auto& tu = MG_State::pGLContext->GetTextureUnitObject(unit);
            const auto& sampler = tu.GetSamplerObject();
            *params = sampler ? static_cast<GLint>(sampler->GetExternalIndex()) : 0;
            return;
        }
        case GL_SAMPLES:
            *params = ResolveDrawFramebufferSampleCount();
            return;
        case GL_SCISSOR_BOX: {
            const IntVec4& scissorBox = MG_State::pGLContext->GetScissorBox();
            params[0] = scissorBox.x();
            params[1] = scissorBox.y();
            params[2] = scissorBox.z();
            params[3] = scissorBox.w();
            return;
        }
        case GL_SCISSOR_TEST:
            *params = MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::ScissorTest) ? GL_TRUE : GL_FALSE;
            return;
        case GL_SHADER_COMPILER:
            *params = GL_TRUE;
            return;
        case GL_SHADER_STORAGE_BUFFER_BINDING: {
            auto& obj = MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::ShaderStorage).GetBoundObject();
            *params = obj ? static_cast<GLint>(obj->GetExternalIndex()) : 0;
            return;
        }
        case GL_SHADER_STORAGE_BUFFER_START:
            RecordIndexedOnlyGetterError(__func__, pname);
            return;
        case GL_SHADER_STORAGE_BUFFER_SIZE:
            RecordIndexedOnlyGetterError(__func__, pname);
            return;
        case GL_STENCIL_BACK_FAIL:
            *params = static_cast<GLint>(
                MG_Util::ConvertStencilOperationToGLEnum(
                    MG_State::pGLContext->GetStencilState(StencilFace::Back).FailOp));
            return;
        case GL_STENCIL_BACK_FUNC:
            *params = static_cast<GLint>(
                MG_Util::ConvertDepthTestFuncToGLEnum(
                    MG_State::pGLContext->GetStencilState(StencilFace::Back).Func));
            return;
        case GL_STENCIL_BACK_PASS_DEPTH_FAIL:
            *params = static_cast<GLint>(
                MG_Util::ConvertStencilOperationToGLEnum(
                    MG_State::pGLContext->GetStencilState(StencilFace::Back).PassDepthFailOp));
            return;
        case GL_STENCIL_BACK_PASS_DEPTH_PASS:
            *params = static_cast<GLint>(
                MG_Util::ConvertStencilOperationToGLEnum(
                    MG_State::pGLContext->GetStencilState(StencilFace::Back).PassDepthPassOp));
            return;
        case GL_STENCIL_BACK_REF:
            *params = MG_State::pGLContext->GetStencilState(StencilFace::Back).Ref;
            return;
        case GL_STENCIL_BACK_VALUE_MASK:
            *params = static_cast<GLint>(MG_State::pGLContext->GetStencilState(StencilFace::Back).ValueMask);
            return;
        case GL_STENCIL_BACK_WRITEMASK:
            *params = static_cast<GLint>(MG_State::pGLContext->GetStencilState(StencilFace::Back).WriteMask);
            return;
        case GL_STENCIL_CLEAR_VALUE:
            *params = static_cast<GLint>(MG_State::pGLContext->GetClearStencil());
            return;
        case GL_STENCIL_FAIL:
            *params = static_cast<GLint>(
                MG_Util::ConvertStencilOperationToGLEnum(
                    MG_State::pGLContext->GetStencilState(StencilFace::Front).FailOp));
            return;
        case GL_STENCIL_FUNC:
            *params = static_cast<GLint>(
                MG_Util::ConvertDepthTestFuncToGLEnum(
                    MG_State::pGLContext->GetStencilState(StencilFace::Front).Func));
            return;
        case GL_STENCIL_PASS_DEPTH_FAIL:
            *params = static_cast<GLint>(
                MG_Util::ConvertStencilOperationToGLEnum(
                    MG_State::pGLContext->GetStencilState(StencilFace::Front).PassDepthFailOp));
            return;
        case GL_STENCIL_PASS_DEPTH_PASS:
            *params = static_cast<GLint>(
                MG_Util::ConvertStencilOperationToGLEnum(
                    MG_State::pGLContext->GetStencilState(StencilFace::Front).PassDepthPassOp));
            return;
        case GL_STENCIL_REF:
            *params = MG_State::pGLContext->GetStencilState(StencilFace::Front).Ref;
            return;
        case GL_STENCIL_TEST:
            *params = MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::StencilTest) ? GL_TRUE : GL_FALSE;
            return;
        case GL_STENCIL_VALUE_MASK:
            *params = static_cast<GLint>(MG_State::pGLContext->GetStencilState(StencilFace::Front).ValueMask);
            return;
        case GL_STENCIL_WRITEMASK:
            *params = static_cast<GLint>(MG_State::pGLContext->GetStencilState(StencilFace::Front).WriteMask);
            return;
        case GL_STEREO:
            *params = 0; // stereo surfaces are not exposed
            return;
        case GL_TEXTURE_BINDING_1D: {
            Int unit = MG_State::pGLContext->GetActiveTextureUnit();
            auto& tu = MG_State::pGLContext->GetTextureUnitObject(unit);
            const auto& slot = tu.GetBindingSlot(TextureTarget::Texture1D);
            const auto& obj = slot.GetBoundObject();
            *params = obj ? static_cast<GLint>(obj->GetExternalIndex()) : 0;
            return;
        }
        case GL_TEXTURE_BINDING_1D_ARRAY: {
            Int unit = MG_State::pGLContext->GetActiveTextureUnit();
            auto& tu = MG_State::pGLContext->GetTextureUnitObject(unit);
            const auto& slot = tu.GetBindingSlot(TextureTarget::Texture1DArray);
            const auto& obj = slot.GetBoundObject();
            *params = obj ? static_cast<GLint>(obj->GetExternalIndex()) : 0;
            return;
        }
        case GL_TEXTURE_BINDING_2D: {
            Int unit = MG_State::pGLContext->GetActiveTextureUnit();
            auto& tu = MG_State::pGLContext->GetTextureUnitObject(unit);
            const auto& slot = tu.GetBindingSlot(TextureTarget::Texture2D);
            const auto& obj = slot.GetBoundObject();
            *params = obj ? static_cast<GLint>(obj->GetExternalIndex()) : 0;
            MGLOG_D("Get GL_TEXTURE_BINDING_2D: %d", *params);
            return;
        }
        case GL_TEXTURE_BINDING_2D_ARRAY: {
            Int unit = MG_State::pGLContext->GetActiveTextureUnit();
            auto& tu = MG_State::pGLContext->GetTextureUnitObject(unit);
            const auto& slot = tu.GetBindingSlot(TextureTarget::Texture2DArray);
            const auto& obj = slot.GetBoundObject();
            *params = obj ? static_cast<GLint>(obj->GetExternalIndex()) : 0;
            return;
        }
        case GL_TEXTURE_BINDING_2D_MULTISAMPLE: {
            Int unit = MG_State::pGLContext->GetActiveTextureUnit();
            auto& tu = MG_State::pGLContext->GetTextureUnitObject(unit);
            const auto& slot = tu.GetBindingSlot(TextureTarget::Texture2DMultisample);
            const auto& obj = slot.GetBoundObject();
            *params = obj ? static_cast<GLint>(obj->GetExternalIndex()) : 0;
            return;
        }
        case GL_TEXTURE_BINDING_2D_MULTISAMPLE_ARRAY: {
            Int unit = MG_State::pGLContext->GetActiveTextureUnit();
            auto& tu = MG_State::pGLContext->GetTextureUnitObject(unit);
            const auto& slot = tu.GetBindingSlot(TextureTarget::Texture2DMultisampleArray);
            const auto& obj = slot.GetBoundObject();
            *params = obj ? static_cast<GLint>(obj->GetExternalIndex()) : 0;
            return;
        }
        case GL_TEXTURE_BINDING_3D: {
            Int unit = MG_State::pGLContext->GetActiveTextureUnit();
            auto& tu = MG_State::pGLContext->GetTextureUnitObject(unit);
            const auto& slot = tu.GetBindingSlot(TextureTarget::Texture3D);
            const auto& obj = slot.GetBoundObject();
            *params = obj ? static_cast<GLint>(obj->GetExternalIndex()) : 0;
            return;
        }
        case GL_TEXTURE_BINDING_BUFFER: {
            Int unit = MG_State::pGLContext->GetActiveTextureUnit();
            auto& tu = MG_State::pGLContext->GetTextureUnitObject(unit);
            const auto& slot = tu.GetBindingSlot(TextureTarget::TextureBuffer);
            const auto& obj = slot.GetBoundObject();
            *params = obj ? static_cast<GLint>(obj->GetExternalIndex()) : 0;
            return;
        }
        case GL_TEXTURE_BINDING_CUBE_MAP: {
            Int unit = MG_State::pGLContext->GetActiveTextureUnit();
            auto& tu = MG_State::pGLContext->GetTextureUnitObject(unit);
            const auto& slot = tu.GetBindingSlot(TextureTarget::TextureCubeMap);
            const auto& obj = slot.GetBoundObject();
            *params = obj ? static_cast<GLint>(obj->GetExternalIndex()) : 0;
            return;
        }
        case GL_TEXTURE_BINDING_RECTANGLE: {
            Int unit = MG_State::pGLContext->GetActiveTextureUnit();
            auto& tu = MG_State::pGLContext->GetTextureUnitObject(unit);
            const auto& slot = tu.GetBindingSlot(TextureTarget::TextureRectangle);
            const auto& obj = slot.GetBoundObject();
            *params = obj ? static_cast<GLint>(obj->GetExternalIndex()) : 0;
            return;
        }
        case GL_TEXTURE_COMPRESSION_HINT:
            *params = GL_DONT_CARE;
            return;
        case GL_TEXTURE_BUFFER_OFFSET_ALIGNMENT:
            *params = 0; // texture-buffer range entrypoints are stubbed
            return;
        case GL_TIMESTAMP:
            *params = 0; // timer-query entrypoints are stubbed
            return;
        case GL_TRANSFORM_FEEDBACK_BUFFER_BINDING:
            if (const auto& obj =
                    MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::TransformFeedback).GetBoundObject()) {
                *params = static_cast<GLint>(obj->GetExternalIndex());
            } else {
                *params = 0;
            }
            return;
        case GL_TRANSFORM_FEEDBACK_BUFFER_START:
            RecordIndexedOnlyGetterError(__func__, pname);
            return;
        case GL_TRANSFORM_FEEDBACK_BUFFER_SIZE:
            RecordIndexedOnlyGetterError(__func__, pname);
            return;
        case GL_UNIFORM_BUFFER_BINDING:
            if (const auto& obj = MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::Uniform).GetBoundObject()) {
                *params = static_cast<GLint>(obj->GetExternalIndex());
            } else {
                *params = 0;
            }
            return;
        case GL_UNIFORM_BUFFER_SIZE:
            RecordIndexedOnlyGetterError(__func__, pname);
            return;
        case GL_UNIFORM_BUFFER_START:
            RecordIndexedOnlyGetterError(__func__, pname);
            return;
        case GL_UNPACK_ALIGNMENT:
            *params = MG_State::pGLContext->GetPixelStoreParam(PixelStoreParam::UnpackAlignment);
            return;
        case GL_UNPACK_IMAGE_HEIGHT:
            *params = MG_State::pGLContext->GetPixelStoreParam(PixelStoreParam::UnpackImageHeight);
            return;
        case GL_UNPACK_LSB_FIRST:
            *params = MG_State::pGLContext->GetPixelStoreParam(PixelStoreParam::UnpackLSBFirst);
            return;
        case GL_UNPACK_ROW_LENGTH:
            *params = MG_State::pGLContext->GetPixelStoreParam(PixelStoreParam::UnpackRowLength);
            return;
        case GL_UNPACK_SKIP_IMAGES:
            *params = MG_State::pGLContext->GetPixelStoreParam(PixelStoreParam::UnpackSkipImages);
            return;
        case GL_UNPACK_SKIP_PIXELS:
            *params = MG_State::pGLContext->GetPixelStoreParam(PixelStoreParam::UnpackSkipPixels);
            return;
        case GL_UNPACK_SKIP_ROWS:
            *params = MG_State::pGLContext->GetPixelStoreParam(PixelStoreParam::UnpackSkipRows);
            return;
        case GL_UNPACK_SWAP_BYTES:
            *params = MG_State::pGLContext->GetPixelStoreParam(PixelStoreParam::UnpackSwapBytes);
            return;
        case GL_VERTEX_ARRAY_BINDING: {
            const auto& vao = MG_State::pGLContext->GetBoundVertexArray();
            *params = vao ? static_cast<GLint>(vao->GetExternalIndex()) : 0;
            return;
        }
        case GL_VERTEX_BINDING_DIVISOR:
            *params = 0; // vertex-binding entrypoints are stubbed
            return;
        case GL_VERTEX_BINDING_OFFSET:
            *params = 0; // vertex-binding entrypoints are stubbed
            return;
        case GL_VERTEX_BINDING_STRIDE:
            *params = 0; // vertex-binding entrypoints are stubbed
            return;
        case GL_MAX_VERTEX_ATTRIB_RELATIVE_OFFSET:
            *params = 0; // vertex-binding entrypoints are stubbed
            return;
        case GL_MAX_VERTEX_ATTRIB_BINDINGS:
            *params = 0; // vertex-binding entrypoints are stubbed
            return;
        case GL_VIEWPORT: {
            const auto& vp = MG_State::pGLContext->GetViewport();
            params[0] = vp.x();
            params[1] = vp.y();
            params[2] = vp.z();
            params[3] = vp.w();
            return;
        }
        case GL_VIEWPORT_INDEX_PROVOKING_VERTEX:
            *params = GL_LAST_VERTEX_CONVENTION;
            return;
        case GL_MAX_ELEMENT_INDEX:
            *params = 1024 * 1024; // TODO
            return;
        case GL_CONTEXT_PROFILE_MASK:
            *params = GL_CONTEXT_CORE_PROFILE_BIT;
            return;
        default:
            break;
        }

        const auto& activeBackendObject = MG_Backend::pActiveBackendObject;
        if (!activeBackendObject) {
            MGLOG_E("activeBackendObject is not initialized!");
            return;
        }
        const auto& rendererInfo = activeBackendObject->GetRendererInfo();
        const auto& dynamicParameters = activeBackendObject->GetDynamicParameters();

        switch (pname) {
        case GL_ALIASED_LINE_WIDTH_RANGE:
            params[0] = static_cast<GLint>(dynamicParameters.AliasedLineWidthRangeMin);
            params[1] = static_cast<GLint>(dynamicParameters.AliasedLineWidthRangeMax);
            break;
        case GL_ALIASED_POINT_SIZE_RANGE:
        case GL_POINT_SIZE_RANGE:
            params[0] = static_cast<GLint>(dynamicParameters.PointSizeRangeMin);
            params[1] = static_cast<GLint>(dynamicParameters.PointSizeRangeMax);
            break;
        case GL_SUBGROUP_SIZE_KHR:
            *params = static_cast<GLint>(dynamicParameters.SubgroupSize);
            break;
        case GL_SUBGROUP_SUPPORTED_STAGES_KHR:
            *params = static_cast<GLint>(dynamicParameters.SubgroupSupportedStages);
            break;
        case GL_SUBGROUP_SUPPORTED_FEATURES_KHR:
            *params = static_cast<GLint>(dynamicParameters.SubgroupSupportedFeatures);
            break;
        case GL_SUBGROUP_QUAD_ALL_STAGES_KHR:
            *params = dynamicParameters.SubgroupQuadOperationsInAllStages ? GL_TRUE : GL_FALSE;
            break;
        case GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS:
            *params = dynamicParameters.MaxComputeShaderStorageBlocks;
            break;
        case GL_MAX_COMBINED_SHADER_STORAGE_BLOCKS:
            *params = dynamicParameters.MaxCombinedShaderStorageBlocks;
            break;
        case GL_MAX_COMPUTE_UNIFORM_BLOCKS:
            *params = dynamicParameters.MaxComputeUniformBlocks;
            break;
        case GL_MAX_COMPUTE_TEXTURE_IMAGE_UNITS:
            *params = dynamicParameters.MaxComputeTextureImageUnits;
            break;
        case GL_MAX_COMBINED_COMPUTE_UNIFORM_COMPONENTS:
            *params = GetMaxCombinedUniformComponents(kFrontendMaxComputeUniformComponents,
                                                      dynamicParameters.MaxComputeUniformBlocks,
                                                      dynamicParameters.MaxUniformBlockSize);
            break;
        case GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS:
            *params = std::max(dynamicParameters.MaxComputeWorkGroupInvocations,
                               kFrontendMaxComputeWorkGroupInvocations);
            break;
        case GL_MAX_COMPUTE_WORK_GROUP_COUNT:
            GetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT, 0, &params[0]);
            GetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT, 1, &params[1]);
            GetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT, 2, &params[2]);
            break;
        case GL_MAX_COMPUTE_WORK_GROUP_SIZE:
            GetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_SIZE, 0, &params[0]);
            GetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_SIZE, 1, &params[1]);
            GetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_SIZE, 2, &params[2]);
            break;
        case GL_MAJOR_VERSION:
            *params = rendererInfo.RendererGLInfo.TargetGLVersion.Major;
            break;
        case GL_MAX_3D_TEXTURE_SIZE:
            *params = dynamicParameters.Max3DTextureSize;
            break;
        case GL_MAX_ARRAY_TEXTURE_LAYERS:
            *params = dynamicParameters.MaxArrayTextureLayers;
            break;
        case GL_MAX_CLIP_DISTANCES:
            *params = dynamicParameters.MaxClipDistances;
            break;
        case GL_MAX_COLOR_TEXTURE_SAMPLES:
            *params = dynamicParameters.MaxColorTextureSamples;
            break;
        case GL_MAX_COMBINED_FRAGMENT_UNIFORM_COMPONENTS:
            *params = GetMaxCombinedUniformComponents(kFrontendMaxFragmentUniformComponents,
                                                      kFrontendMaxFragmentUniformBlocks,
                                                      dynamicParameters.MaxUniformBlockSize);
            break;
        case GL_MAX_COMBINED_GEOMETRY_UNIFORM_COMPONENTS:
            *params = GetMaxCombinedUniformComponents(kFrontendMaxGeometryUniformComponents,
                                                      kFrontendMaxGeometryUniformBlocks,
                                                      dynamicParameters.MaxUniformBlockSize);
            break;
        case GL_MAX_GEOMETRY_OUTPUT_VERTICES:
            *params = kFrontendMaxGeometryOutputVertices;
            break;
        case GL_MAX_GEOMETRY_TOTAL_OUTPUT_COMPONENTS:
            *params = kFrontendMaxGeometryTotalOutputComponents;
            break;
        case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS:
            *params = dynamicParameters.MaxCombinedTextureImageUnits;
            break;
        case GL_MAX_COMBINED_VERTEX_UNIFORM_COMPONENTS:
            *params = GetMaxCombinedUniformComponents(kFrontendMaxVertexUniformComponents,
                                                      kFrontendMaxVertexUniformBlocks,
                                                      dynamicParameters.MaxUniformBlockSize);
            break;
        case GL_MAX_CUBE_MAP_TEXTURE_SIZE:
            *params = dynamicParameters.MaxCubeMapTextureSize;
            break;
        case GL_MAX_DEPTH_TEXTURE_SAMPLES:
            *params = dynamicParameters.MaxDepthTextureSamples;
            break;
        case GL_MAX_FRAMEBUFFER_WIDTH:
            *params = dynamicParameters.MaxFramebufferWidth;
            break;
        case GL_MAX_FRAMEBUFFER_HEIGHT:
            *params = dynamicParameters.MaxFramebufferHeight;
            break;
        case GL_MAX_FRAMEBUFFER_LAYERS:
            *params = dynamicParameters.MaxFramebufferLayers;
            break;
        case GL_MAX_FRAMEBUFFER_SAMPLES:
            *params = dynamicParameters.MaxFramebufferSamples;
            break;
        case GL_MAX_IMAGE_UNITS:
            *params = dynamicParameters.MaxImageUnits;
            break;
        case GL_MAX_COMBINED_IMAGE_UNITS_AND_FRAGMENT_OUTPUTS:
            *params = dynamicParameters.MaxImageUnits + dynamicParameters.MaxDrawBuffers;
            break;
        case GL_MAX_COMBINED_IMAGE_UNIFORMS:
            *params = dynamicParameters.MaxCombinedImageUniforms;
            break;
        case GL_MAX_COMPUTE_IMAGE_UNIFORMS:
            *params = dynamicParameters.MaxComputeImageUniforms;
            break;
        case GL_MAX_INTEGER_SAMPLES:
            *params = dynamicParameters.MaxIntegerSamples;
            break;
        case GL_MAX_RENDERBUFFER_SIZE:
            *params = dynamicParameters.MaxRenderbufferSize;
            break;
        case GL_MAX_SAMPLE_MASK_WORDS:
            *params = dynamicParameters.MaxSampleMaskWords;
            break;
        case GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS:
            *params = static_cast<GLint>(GetIndexedBufferQueryPointCount(BufferTarget::ShaderStorage));
            break;
        case GL_MAX_TEXTURE_BUFFER_SIZE:
            *params = dynamicParameters.MaxTextureBufferSize;
            break;
        case GL_MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS:
            *params = kFrontendMaxTransformFeedbackInterleavedComponents;
            break;
        case GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS:
            *params = kFrontendMaxTransformFeedbackSeparateAttribs;
            break;
        case GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_COMPONENTS:
            *params = kFrontendMaxTransformFeedbackSeparateComponents;
            break;
        case GL_MAX_TEXTURE_IMAGE_UNITS:
            *params = dynamicParameters.MaxTextureImageUnits;
            break;
        case GL_MAX_TEXTURE_SIZE:
            *params = dynamicParameters.MaxTextureSize;
            break;
        case GL_MAX_UNIFORM_BUFFER_BINDINGS:
            *params = std::max(dynamicParameters.MaxUniformBufferBindings, kFrontendMinUniformBufferBindings);
            break;
        case GL_MAX_UNIFORM_BLOCK_SIZE:
            *params = dynamicParameters.MaxUniformBlockSize;
            break;
        case GL_MAX_VERTEX_ATTRIBS:
            *params = dynamicParameters.MaxVertexAttribs;
            break;
        case GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS:
            *params = dynamicParameters.MaxVertexTextureImageUnits;
            break;
        case GL_MAX_VIEWPORT_DIMS:
            params[0] = dynamicParameters.MaxViewportWidth;
            params[1] = dynamicParameters.MaxViewportHeight;
            break;
        case GL_MAX_VIEWPORTS:
            *params = dynamicParameters.MaxViewports;
            break;
        case GL_MINOR_VERSION:
            *params = rendererInfo.RendererGLInfo.TargetGLVersion.Minor;
            break;
        case GL_NUM_EXTENSIONS:
            *params = static_cast<Int>(rendererInfo.RendererGLInfo.Extensions.size());
            break;
        case GL_POINT_SIZE_GRANULARITY:
            *params = static_cast<GLint>(dynamicParameters.PointSizeGranularity);
            break;
        case GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT:
            *params = static_cast<GLint>(dynamicParameters.UniformBufferOffsetAlignment);
            break;
        case GL_SMOOTH_LINE_WIDTH_RANGE:
            params[0] = static_cast<GLint>(dynamicParameters.SmoothLineWidthRangeMin);
            params[1] = static_cast<GLint>(dynamicParameters.SmoothLineWidthRangeMax);
            break;
        case GL_SMOOTH_LINE_WIDTH_GRANULARITY:
            *params = static_cast<GLint>(dynamicParameters.SmoothLineWidthGranularity);
            break;
        case GL_SUBPIXEL_BITS:
            *params = std::max(dynamicParameters.ViewportSubpixelBits, kFrontendSubpixelBits);
            break;
        case GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT:
            *params = static_cast<Int>(dynamicParameters.UniformBufferOffsetAlignment);
            break;
        case GL_VIEWPORT_BOUNDS_RANGE:
            params[0] = static_cast<GLint>(dynamicParameters.ViewportBoundsRangeMin);
            params[1] = static_cast<GLint>(dynamicParameters.ViewportBoundsRangeMax);
            break;
        case GL_VIEWPORT_SUBPIXEL_BITS:
            *params = std::max(dynamicParameters.ViewportSubpixelBits, kFrontendSubpixelBits);
            break;
        case GL_MAX_COLOR_ATTACHMENTS:
        case GL_MAX_DRAW_BUFFERS:
            *params = pname == GL_MAX_COLOR_ATTACHMENTS ? dynamicParameters.MaxColorAttachments
                                                        : dynamicParameters.MaxDrawBuffers;
            break;
        case GL_MAX_SAMPLES:
            *params = std::max(dynamicParameters.MaxSamples, kFrontendMaxSamples);
            break;
        default:
            MGLOG_E("glGetIntegerv: Invalid enum %s (0x%X)", MG_Util::ConvertGLEnumToString(pname).c_str(), pname);
            MG_State::pGLContext->RecordError(ErrorCode::InvalidEnum,
                                              MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "GetIntegerv",
                                                                           std::format("Invalid enum: 0x{:X}", pname)));

            break;
        }
    }

    GLenum GetError() {
        auto error = MG_State::pGLContext->PopGLError();
        if (!error || !error->get()) {
            return GL_NO_ERROR;
        }
        return MG_Util::ConvertErrorCodeToGLEnum(error->get()->code);
    }
} // namespace MobileGL::MG_Impl::GLImpl
