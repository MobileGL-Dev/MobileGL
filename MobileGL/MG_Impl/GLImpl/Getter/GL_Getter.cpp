// MobileGL - MobileGL/MG_Impl/GLImpl/Getter/GL_Getter.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "GL_Getter.h"
#include <cmath>
#include <Config.h>
#include <MGGitHash.h>
#include <MG_Impl/GLImpl/VertexArray/Validators.h>
#include <MG_State/EGLState/Core.h>
#include <MG_State/GLState/Core.h>
#include <MG_State/GLState/ErrorState/ErrorInfo.h>
#include <MG_Util/Converters/GLToStr/GLEnumConverter.h>
#include <MG_Util/Converters/GLToMG/BufferEnumConverter.h>
#include <MG_Util/Converters/GLToMG/RenderStateEnumConverter.h>
#include <MG_Util/Converters/MGToGL/FramebufferEnumConverter.h>
#include <MG_Util/Converters/MGToGL/ErrorCodeConverter.h>
#include <MG_Util/Converters/MGToGL/TextureEnumConverter.h>
#include <MG_Util/Converters/MGToStr/GLExtensionConverter.h>
#include <MG_Util/Converters/MGToGL/RenderStateEnumConverter.h>
#include <MG_State/GLState/FramebufferState/FramebufferObject.h>
#include <MG_Util/Texture/TextureFormatProcessor.h>
#include <MG_Util/Async/ShaderCompilePool.h>
#include <MG_Util/ShaderTranspiler/Types.h>
#include <MG_Backend/BackendObjects.h>

namespace MobileGL::MG_Impl::GLImpl {
    // Declared rather than #included from GL_RenderState.h on purpose: that header also declares
    // a free function named BlendEquation, which would hide the ::MobileGL::BlendEquation enum
    // this file's blend-state queries name unqualified.
    GLboolean IsEnabledi(GLenum target, GLuint index);

    namespace {
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

        // Shared with the glslang resource table for the same reason as the atomic-counter
        // limits below: gl_MaxComputeUniformComponents expands from BuildTBuiltInResource.
        constexpr GLint kFrontendMaxComputeUniformComponents =
            static_cast<GLint>(MG_Util::ShaderTranspiler::MAX_COMPUTE_UNIFORM_COMPONENTS);
        // Every atomic-counter limit is shared with the glslang resource table
        // (BuildTBuiltInResource) through MG_Util/ShaderTranspiler/Types.h: GL 4.6 requires
        // glGetIntegerv and the gl_MaxAtomicCounter* built-in constants to agree, and the two
        // used to be independent tables that disagreed on both the binding count and the buffer
        // size. Never move one of these without the other.
        constexpr GLint kFrontendMaxComputeAtomicCounters =
            static_cast<GLint>(MG_Util::ShaderTranspiler::MAX_ATOMIC_COUNTERS_PER_STAGE);
        constexpr GLint kFrontendMaxComputeAtomicCounterBuffers =
            static_cast<GLint>(MG_Util::ShaderTranspiler::MAX_ATOMIC_COUNTER_BUFFERS_PER_STAGE);
        constexpr GLint kFrontendMaxComputeSharedMemorySize = 32768;
        constexpr GLint kFrontendMaxComputeWorkGroupInvocations = 1024;
        constexpr GLint kFrontendMaxCombinedAtomicCounters =
            static_cast<GLint>(MG_Util::ShaderTranspiler::MAX_ATOMIC_COUNTERS_PER_STAGE);
        constexpr GLint kFrontendMaxCombinedAtomicCounterBuffers =
            static_cast<GLint>(MG_Util::ShaderTranspiler::MAX_ATOMIC_COUNTER_BUFFERS_PER_STAGE);
        constexpr GLint kFrontendMaxFragmentAtomicCounters =
            static_cast<GLint>(MG_Util::ShaderTranspiler::MAX_ATOMIC_COUNTERS_PER_STAGE);
        constexpr GLint kFrontendMaxFragmentAtomicCounterBuffers =
            static_cast<GLint>(MG_Util::ShaderTranspiler::MAX_ATOMIC_COUNTER_BUFFERS_PER_STAGE);
        constexpr GLint kFrontendMaxGeometryAtomicCounters = 0;
        constexpr GLint kFrontendMaxTessControlAtomicCounters = 0;
        constexpr GLint kFrontendMaxTessEvaluationAtomicCounters = 0;
        constexpr GLint kFrontendMaxVertexAtomicCounters = 0;
        // Zero counters means zero buffers to hold them. These have to be ANSWERED rather than
        // left to the default INVALID_ENUM: a well-behaved application queries the limit exactly
        // to find out that the stage cannot do this, and an error instead both leaves its output
        // untouched (so it reads uninitialised memory and may conclude the opposite) and leaves a
        // GL error pending that surfaces at whatever unrelated call checks next.
        constexpr GLint kFrontendMaxGeometryAtomicCounterBuffers = 0;
        constexpr GLint kFrontendMaxTessControlAtomicCounterBuffers = 0;
        constexpr GLint kFrontendMaxTessEvaluationAtomicCounterBuffers = 0;
        constexpr GLint kFrontendMaxVertexAtomicCounterBuffers = 0;
        // GL_MAX_ATOMIC_COUNTER_BUFFER_SIZE: the byte offset ceiling a counter may be declared
        // at. The matching binding count is applied in GetIndexedBufferQueryPointCount, so that
        // the getter, the indexed queries and glBindBufferBase all share one ceiling.
        constexpr GLint kFrontendMaxAtomicCounterBufferSize =
            static_cast<GLint>(MG_Util::ShaderTranspiler::MAX_ATOMIC_COUNTER_BUFFER_SIZE);
        // KHR_debug minima (GL 4.6 table 23.66); the debug entry points are stubs, but the
        // limits they advertise still have to be legal.
        constexpr GLint kFrontendMaxDebugGroupStackDepth = 64;
        constexpr GLint kFrontendMaxDebugLoggedMessages = 1;
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

        // The floors under GL_MAX_COMPUTE_WORK_GROUP_COUNT / _SIZE. Shared with the compile
        // pipeline (CaptureCompileEnv floors the same driver answers at them, and
        // BuildTBuiltInResource expands gl_MaxComputeWorkGroup* from the result), because a
        // shader is allowed to compare the built-in constant against this query.
        constexpr GLint GetMinComputeWorkGroupCount(GLuint index) {
            return index < 3 ? static_cast<GLint>(MG_Util::ShaderTranspiler::MIN_COMPUTE_WORK_GROUP_COUNT[index]) : 0;
        }

        constexpr GLint GetMinComputeWorkGroupSize(GLuint index) {
            return index < 3 ? static_cast<GLint>(MG_Util::ShaderTranspiler::MIN_COMPUTE_WORK_GROUP_SIZE[index]) : 0;
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
            if (bufferTarget == BufferTarget::AtomicCounter) {
                // The counter family's binding count is NOT the state layer's array size: a
                // counter buffer only reaches a shader as a lowered storage block, so what an
                // implementation can serve is the reserved range, and that number is also what
                // glslang compiles a layout(binding = N) atomic_uint against. Clamped here so
                // GL_MAX_ATOMIC_COUNTER_BUFFER_BINDINGS, the indexed getters' index check and
                // glBindBufferBase's all report the same ceiling.
                return std::min(frontendCount,
                                static_cast<SizeT>(MG_Util::ShaderTranspiler::MAX_ATOMIC_COUNTER_BUFFER_BINDINGS));
            }
            return frontendCount;
        }

        // A per-stage or combined BLOCK count is an amount of indexed binding points an
        // application will occupy, and GL 4.6 table 23.64 orders the two accordingly:
        // MAX_UNIFORM_BUFFER_BINDINGS >= MAX_COMBINED_UNIFORM_BLOCKS >= every per-stage count,
        // and the same for the shader-storage family. The two families are answered from
        // unrelated places here - frontend constants, backend dynamic parameters, and a few
        // hard-coded TODOs - so nothing kept them ordered, and a backend that reports Vulkan
        // descriptor-indexing counts advertised 256 compute uniform blocks over 36 binding
        // points. KHR-GL44.multi_bind.dispatch_bind_buffers_base reads the block count and binds
        // that many buffers in ONE glBindBuffersBase, which is then INVALID_OPERATION before it
        // binds anything. Clamping is the only direction available: the binding count is the
        // capacity of the state layer's indexed-binding array, not a number we may inflate.
        GLint ClampBlockCountToBindingPoints(GLint blockCount, BufferTarget bufferTarget) {
            const GLint bindingPoints = static_cast<GLint>(GetIndexedBufferQueryPointCount(bufferTarget));
            return std::min(std::max(blockCount, 0), bindingPoints);
        }

        GLint ClampUniformBlockCount(GLint blockCount) {
            return ClampBlockCountToBindingPoints(blockCount, BufferTarget::Uniform);
        }

        GLint ClampStorageBlockCount(GLint blockCount) {
            return ClampBlockCountToBindingPoints(blockCount, BufferTarget::ShaderStorage);
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
                if (attachment.IsRenderbuffer() && attachment.GetRenderbuffer()) {
                    maxSamples = std::max(maxSamples, static_cast<GLint>(attachment.GetRenderbuffer()->GetSamples()));
                } else if (attachment.IsTexture() && attachment.GetTexture()) {
                    // Multisample texture attachments count too (GL_SAMPLE_BUFFERS must
                    // report 1 for any multisampled draw framebuffer).
                    maxSamples = std::max(maxSamples, static_cast<GLint>(attachment.GetTexture()->GetSamples()));
                }
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

        // GL_TEXTURE_BINDING_* is per-texture-unit state: glGetIntegerv answers for the
        // active unit, glGetIntegeri_v answers for unit `index`. Both need the same
        // pname -> target decode, so it lives here instead of being spelled out twice.
        bool TryDecodeTextureUnitBindingPname(GLenum pname, TextureTarget& outTarget) {
            switch (pname) {
            case GL_TEXTURE_BINDING_1D: outTarget = TextureTarget::Texture1D; return true;
            case GL_TEXTURE_BINDING_1D_ARRAY: outTarget = TextureTarget::Texture1DArray; return true;
            case GL_TEXTURE_BINDING_2D: outTarget = TextureTarget::Texture2D; return true;
            case GL_TEXTURE_BINDING_2D_ARRAY: outTarget = TextureTarget::Texture2DArray; return true;
            case GL_TEXTURE_BINDING_2D_MULTISAMPLE: outTarget = TextureTarget::Texture2DMultisample; return true;
            case GL_TEXTURE_BINDING_2D_MULTISAMPLE_ARRAY:
                outTarget = TextureTarget::Texture2DMultisampleArray;
                return true;
            case GL_TEXTURE_BINDING_3D: outTarget = TextureTarget::Texture3D; return true;
            case GL_TEXTURE_BINDING_BUFFER: outTarget = TextureTarget::TextureBuffer; return true;
            case GL_TEXTURE_BINDING_CUBE_MAP: outTarget = TextureTarget::TextureCubeMap; return true;
            case GL_TEXTURE_BINDING_CUBE_MAP_ARRAY: outTarget = TextureTarget::TextureCubeMapArray; return true;
            case GL_TEXTURE_BINDING_RECTANGLE: outTarget = TextureTarget::TextureRectangle; return true;
            default: return false;
            }
        }

        GLint QueryTextureBindingOnUnit(Int unit, TextureTarget target) {
            auto& textureUnit = MG_State::pGLContext->GetTextureUnitObject(unit);
            const auto& obj = textureUnit.GetBindingSlot(target).GetBoundObject();
            return obj ? static_cast<GLint>(obj->GetExternalIndex()) : 0;
        }

        GLint QuerySamplerBindingOnUnit(Int unit) {
            const auto& textureUnit = MG_State::pGLContext->GetTextureUnitObject(unit);
            const auto& sampler = textureUnit.GetSamplerObject();
            return sampler ? static_cast<GLint>(sampler->GetExternalIndex()) : 0;
        }

        // The ARB_viewport_array indexed rectangles. Each of these is genuinely per-viewport
        // frontend state (RenderStateParameters::Viewports / ScissorBoxes / DepthRanges), so the
        // indexed getters must read the indexed storage - the generic path at the bottom of
        // GetIntegeri_v is a raw backend passthrough that has no case for them and returned
        // zeros, and routing them to the NON-indexed getter (what this used to do) answered every
        // index with viewport 0's value, which is what
        // KHR-GL43.viewport_array.{viewport,scissor,depth_range}_api caught.
        Bool IsIndexedViewportQuery(GLenum target) {
            return target == GL_VIEWPORT || target == GL_SCISSOR_BOX || target == GL_DEPTH_RANGE;
        }

        // Component count of an indexed viewport-array query, so every width of getter writes the
        // caller's whole buffer instead of just element 0 (GL 4.6 core 22.1).
        GLsizei IndexedViewportQueryComponents(GLenum target) {
            return target == GL_DEPTH_RANGE ? 2 : 4;
        }

        // ARB_viewport_array: `index` selects a viewport and MAX_VIEWPORTS bounds it. The bound is
        // the frontend's own state width, which is also exactly what GL_MAX_VIEWPORTS reports -
        // taking it from the backend caps instead would let a device limit of 1 (a Vulkan device
        // without the multiViewport feature) make index 1 illegal even though the state exists.
        Bool ValidateViewportQueryIndex(GLuint index, const char* caller) {
            if (index < RenderStateParameters::MAX_VIEWPORTS) return true;
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "Viewport index is out of range."));
            return false;
        }

        // The indexed viewport/scissor/depth-range state as floats, which is the widest lossless
        // shape MobileGL stores (the viewport really is float state; the scissor box is integral
        // and well inside float's exact range, and every depth range is in [0, 1]). Every indexed
        // getter width funnels through this so they can never disagree with each other.
        void ReadIndexedViewportStateFloat(GLenum target, GLuint index, GLfloat* out) {
            switch (target) {
            case GL_VIEWPORT: {
                const FloatVec4& viewport = MG_State::pGLContext->GetViewportIndexed(index);
                out[0] = viewport.x();
                out[1] = viewport.y();
                out[2] = viewport.z();
                out[3] = viewport.w();
                return;
            }
            case GL_SCISSOR_BOX: {
                const IntVec4& box = MG_State::pGLContext->GetScissorBoxIndexed(index);
                out[0] = static_cast<GLfloat>(box.x());
                out[1] = static_cast<GLfloat>(box.y());
                out[2] = static_cast<GLfloat>(box.z());
                out[3] = static_cast<GLfloat>(box.w());
                return;
            }
            case GL_DEPTH_RANGE: {
                const FloatVec2& range = MG_State::pGLContext->GetDepthRangeIndexed(index);
                out[0] = range.x();
                out[1] = range.y();
                return;
            }
            default:
                MOBILEGL_ASSERT(false, "ReadIndexedViewportStateFloat: unexpected target 0x%x",
                                static_cast<Uint32>(target));
                return;
            }
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

    // GL 4.6 core table 23.53 requires GL_MAX_SAMPLES >= 4, so the driver's value is floored
    // before it is advertised. Every other multisample ceiling MobileGL advertises has to be
    // floored the same way: promising 4 samples globally while answering GL_MAX_INTEGER_SAMPLES
    // 1 - which is exactly what Adreno reports - makes the frontend reject the very count it
    // just told the application to use. The backends clamp the realised count instead.
    GLint GetAdvertisedMaxSamples() {
        if (MG_Backend::pActiveBackendObject == nullptr) {
            return kFrontendMaxSamples;
        }
        return std::max(MG_Backend::pActiveBackendObject->GetDynamicParameters().MaxSamples, kFrontendMaxSamples);
    }

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
            MGLOG_E_ONCE("activeBackendObject is not initialized!");
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
            MGLOG_E_ONCE("activeBackendObject is not initialized!");
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
        case GL_MIN_FRAGMENT_INTERPOLATION_OFFSET:
        case GL_MAX_FRAGMENT_INTERPOLATION_OFFSET:
        case GL_FRAGMENT_INTERPOLATION_OFFSET_BITS: {
            GLfloat value = 0.0f;
            GetFloatv(pname, &value);
            *params = value != 0.0f ? GL_TRUE : GL_FALSE;
            return;
        }
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
        // Viewport 0's rectangle, verbatim. Falling through to the integer width below would
        // round the fractional rectangle a glViewportIndexedf(0, ...) is allowed to set, and
        // glGetFloatv(GL_VIEWPORT) is a lossless query of float state.
        case GL_VIEWPORT: {
            const FloatVec4& viewport = MG_State::pGLContext->GetViewportIndexed(0);
            params[0] = viewport.x();
            params[1] = viewport.y();
            params[2] = viewport.z();
            params[3] = viewport.w();
            return;
        }
        case GL_MIN_FRAGMENT_INTERPOLATION_OFFSET:
        case GL_MAX_FRAGMENT_INTERPOLATION_OFFSET:
        case GL_FRAGMENT_INTERPOLATION_OFFSET_BITS: {
            const auto& dynamicParameters = MG_Backend::pActiveBackendObject->GetDynamicParameters();
            if (pname == GL_MIN_FRAGMENT_INTERPOLATION_OFFSET) {
                params[0] = dynamicParameters.MinFragmentInterpolationOffset;
            } else if (pname == GL_MAX_FRAGMENT_INTERPOLATION_OFFSET) {
                params[0] = dynamicParameters.MaxFragmentInterpolationOffset;
            } else {
                params[0] = static_cast<GLfloat>(dynamicParameters.FragmentInterpolationOffsetBits);
            }
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
        case GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT: {
            // EXT_texture_filter_anisotropic queries this as a float; the integer path below widens
            // from here, so this case is the authoritative one.
            const auto& dynamicParameters = MG_Backend::pActiveBackendObject->GetDynamicParameters();
            params[0] = dynamicParameters.MaxTextureMaxAnisotropy;
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
        case GL_POINT_FADE_THRESHOLD_SIZE:
            // Float state: read it directly so the fractional part is not lost to the integer path.
            params[0] = MG_State::pGLContext->GetPointFadeThresholdSize();
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
                // GL 4.6 core table 23.4/23.5: *_BUFFER_SIZE reports the size glBindBufferRange
                // was ASKED for, verbatim. It is not clamped to the buffer's storage, and it does
                // not follow the buffer when a later glBufferData resizes it - a range may legally
                // name bytes the buffer does not have yet. Clamping it here answered 0 for the
                // common conformance shape of binding a range on a buffer that has no storage
                // yet (KHR-GL43.shader_storage_buffer_object.basic-binding).
                const Range1D range = bindingPoint.GetRange();
                *data = static_cast<GLint>(range.end - range.start);
                return;
            }
            default:
                break;
            }
        }

        // Per-texture-unit bindings: GL 4.6 core table 23.19 makes every GL_TEXTURE_BINDING_*
        // and GL_SAMPLER_BINDING indexed by texture unit. Without this they fell through to
        // the raw backend passthrough at the bottom, which knows nothing about the
        // frontend's binding state.
        if (TextureTarget textureBindingTarget = TextureTarget::Unknown;
            TryDecodeTextureUnitBindingPname(target, textureBindingTarget) || target == GL_SAMPLER_BINDING) {
            GLint maxUnits = 0;
            GetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &maxUnits);
            maxUnits = std::min<GLint>(maxUnits, MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS);
            if (index >= static_cast<GLuint>(std::max(maxUnits, 0))) {
                *data = 0;
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Texture unit index is out of range."));
                return;
            }
            *data = target == GL_SAMPLER_BINDING
                ? QuerySamplerBindingOnUnit(static_cast<Int>(index))
                : QueryTextureBindingOnUnit(static_cast<Int>(index), textureBindingTarget);
            return;
        }

        // GL 4.6 core 22.1: an indexed query answers EVERY indexed state, and GL_SCISSOR_TEST is
        // indexed by viewport just like GL_BLEND is by draw buffer. Without this the integer
        // width fell through to the backend passthrough and answered GL_INVALID_ENUM, which is
        // the sticky error KHR-GL43.viewport_array.queries trips over at its next error check.
        if (MG_Util::ConvertGLEnumToCapabilityInput(target) != CapabilityInput::Unknown) {
            *data = IsEnabledi(target, index);
            return;
        }

        switch (target) {
        // ARB_viewport_array queries the indexed rectangles through glGetIntegeri_v as well
        // (gl4cMultiBindTests and the viewport_array group both do).
        case GL_VIEWPORT:
        case GL_SCISSOR_BOX:
        case GL_DEPTH_RANGE: {
            if (!ValidateViewportQueryIndex(index, __func__)) return;
            GLfloat values[4] = {};
            ReadIndexedViewportStateFloat(target, index, values);
            const GLsizei components = IndexedViewportQueryComponents(target);
            for (GLsizei i = 0; i < components; ++i) {
                // Round, not truncate: glGetIntegerv on floating-point state rounds to nearest
                // (GL 4.6 core 22.2), so a 255.875-wide viewport reads back as 256 and not 255.
                data[i] = static_cast<GLint>(std::lround(values[i]));
            }
            return;
        }
        // The vertex buffer binding points of the vertex array object that is bound. Indexed by
        // binding point, not by attribute (GL 4.6 core 10.3.1).
        case GL_VERTEX_BINDING_BUFFER:
        case GL_VERTEX_BINDING_DIVISOR:
        case GL_VERTEX_BINDING_OFFSET:
        case GL_VERTEX_BINDING_STRIDE: {
            if (index >= VertexArrayImpl::GetMaxVertexAttribBindings()) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                                 "Vertex buffer binding index is out of range."));
                return;
            }
            const auto& vao = MG_State::pGLContext->GetBoundVertexArray();
            if (!vao) {
                *data = 0;
                return;
            }
            const auto& binding = vao->GetBindingPoint(index);
            switch (target) {
            case GL_VERTEX_BINDING_BUFFER:
                *data = binding.Buffer ? static_cast<GLint>(binding.Buffer->GetExternalIndex()) : 0;
                return;
            case GL_VERTEX_BINDING_DIVISOR:
                *data = static_cast<GLint>(binding.Divisor);
                return;
            case GL_VERTEX_BINDING_OFFSET:
                *data = static_cast<GLint>(binding.Offset);
                return;
            default:
                *data = static_cast<GLint>(binding.Stride);
                return;
            }
        }
        case GL_IMAGE_BINDING_NAME:
        case GL_IMAGE_BINDING_LEVEL:
        case GL_IMAGE_BINDING_LAYERED:
        case GL_IMAGE_BINDING_LAYER:
        case GL_IMAGE_BINDING_ACCESS:
        case GL_IMAGE_BINDING_FORMAT: {
            const auto maxImageUnits = static_cast<GLuint>(std::min<GLint>(
                MG_Backend::pActiveBackendObject->GetDynamicParameters().MaxImageUnits,
                MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS));
            if (index >= maxImageUnits) {
                *data = 0;
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Image unit index is out of range."));
                return;
            }

            const auto& binding = MG_State::pGLContext->GetImageTextureBinding(static_cast<Int>(index));
            switch (target) {
            case GL_IMAGE_BINDING_NAME:
                *data = binding.Texture ? static_cast<GLint>(binding.Texture->GetExternalIndex()) : 0;
                return;
            case GL_IMAGE_BINDING_LEVEL:
                *data = binding.Level;
                return;
            case GL_IMAGE_BINDING_LAYERED:
                *data = binding.Layered;
                return;
            case GL_IMAGE_BINDING_LAYER:
                *data = binding.Layer;
                return;
            case GL_IMAGE_BINDING_ACCESS:
                *data = static_cast<GLint>(binding.Access);
                return;
            case GL_IMAGE_BINDING_FORMAT:
                *data = static_cast<GLint>(binding.Format);
                return;
            default:
                break;
            }
        }
        default:
            break;
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

    // GL_ARB_viewport_array's typed indexed getters. They were no-op stubs, which left the
    // caller's output buffer holding whatever was on the stack. The multi-component indexed
    // rectangles are answered from the frontend's own viewport/scissor/depth-range state, via
    // the non-indexed getter of the matching type - GL_DEPTH_RANGE is float state, so putting
    // it through the integer query would round it to 0/1. Everything else MobileGL answers
    // indexed is scalar integer-domain state, where converting the integer query is exact.
    void GetFloati_v(GLenum target, GLuint index, GLfloat* data) {
        if (!data) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "data pointer cannot be null"));
            return;
        }
        if (IsIndexedViewportQuery(target)) {
            if (!ValidateViewportQueryIndex(index, __func__)) return;
            // Verbatim, NOT via the integer width: the viewport is float state and
            // KHR-GL43.viewport_array.viewport_api compares the read-back with ==, so a
            // glViewportIndexedf(i, 0.125f, ...) has to come back as 0.125f exactly.
            ReadIndexedViewportStateFloat(target, index, data);
            return;
        }
        GLint ints[4] = {};
        GetIntegeri_v(target, index, ints);
        data[0] = static_cast<GLfloat>(ints[0]);
    }

    void GetDoublei_v(GLenum target, GLuint index, GLdouble* data) {
        if (!data) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "data pointer cannot be null"));
            return;
        }
        if (IsIndexedViewportQuery(target)) {
            if (!ValidateViewportQueryIndex(index, __func__)) return;
            GLfloat values[4] = {};
            ReadIndexedViewportStateFloat(target, index, values);
            const GLsizei components = IndexedViewportQueryComponents(target);
            for (GLsizei i = 0; i < components; ++i) {
                data[i] = static_cast<GLdouble>(values[i]);
            }
            return;
        }
        GLint ints[4] = {};
        GetIntegeri_v(target, index, ints);
        data[0] = static_cast<GLdouble>(ints[0]);
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
                // Verbatim, unclamped - see the GetIntegeri_v arm.
                *data = static_cast<GLint64>(range.end - range.start);
                return;
            }
            default:
                break;
            }
        }

        // The one indexed pname whose value genuinely needs 64 bits: a vertex buffer binding
        // offset is an intptr, so taking the 32-bit route below would truncate it.
        if (target == GL_VERTEX_BINDING_OFFSET) {
            if (index >= VertexArrayImpl::GetMaxVertexAttribBindings()) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                                 "Vertex buffer binding index is out of range."));
                return;
            }
            const auto& vao = MG_State::pGLContext->GetBoundVertexArray();
            *data = vao ? static_cast<GLint64>(vao->GetBindingPoint(index).Offset) : 0;
            return;
        }

        // Everything else is 32-bit indexed state that the glGetIntegeri_v pname table already
        // owns, and GL 4.6 core 22.1 says every indexed query answers every indexed pname.
        // Handing the leftovers straight to the backend instead made glGetInteger64i_v disagree
        // with glGetIntegeri_v on the very same pname - GL_MAX_COMPUTE_WORK_GROUP_COUNT read
        // back 0 while the 32-bit view said 65535 (KHR-GL43.compute_shader.max), because a
        // frontend-only value simply is not in the driver's table.
        GLint values[4] = {};
        GetIntegeri_v(target, index, values);
        // The viewport-array rectangles are the only multi-component indexed state here; every
        // other pname is scalar, so widening element 0 alone would silently truncate them.
        const GLsizei components = IsIndexedViewportQuery(target) ? IndexedViewportQueryComponents(target) : 1;
        for (GLsizei i = 0; i < components; ++i) {
            data[i] = static_cast<GLint64>(values[i]);
        }
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
        case GL_TIMESTAMP: {
            // Handled here (not via the 32-bit GetIntegerv fallback) so the
            // full 64-bit GPU timestamp survives; LWJGL reads it this way.
            Int64 timestamp = 0;
            if (!MG_Config::Features.DisableTimerQuery) {
                if (const auto getGpuTimestampNs = MG_Backend::gBackendFunctionsTable.GL.GetGpuTimestampNs) {
                    timestamp = getGpuTimestampNs();
                }
            }
            params[0] = static_cast<GLint64>(timestamp);
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

    // glGetDoublev shares GetFloatv's accepted-pname set (and its INVALID_ENUM handling) and widens the
    // result. MobileGL stores no native-double state (depth range/clear are float), so widening from
    // float matches the resolution MobileGL actually holds. Only the pname's own component count is
    // written, never a fixed 4, so a 1-component query cannot overrun the caller's buffer.
    void GetDoublev(GLenum pname, GLdouble* params) {
        if (!params) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "params pointer cannot be null"));
            return;
        }
        GLfloat floats[4] = {};
        GetFloatv(pname, floats);
        GLsizei count = 1;
        switch (pname) {
        case GL_DEPTH_RANGE:
        case GL_VIEWPORT_BOUNDS_RANGE:
        case GL_ALIASED_LINE_WIDTH_RANGE:
        case GL_ALIASED_POINT_SIZE_RANGE:
        case GL_POINT_SIZE_RANGE:
        case GL_SMOOTH_LINE_WIDTH_RANGE:
        case GL_MAX_VIEWPORT_DIMS:
            count = 2;
            break;
        case GL_BLEND_COLOR:
        case GL_COLOR_CLEAR_VALUE:
        case GL_VIEWPORT:
        case GL_SCISSOR_BOX:
        case GL_COLOR_WRITEMASK:
            count = 4;
            break;
        default:
            count = 1;
            break;
        }
        for (GLsizei i = 0; i < count; ++i) {
            params[i] = static_cast<GLdouble>(floats[i]);
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

        // Per-texture-unit bindings: the non-indexed query reports the active unit.
        if (TextureTarget textureBindingTarget = TextureTarget::Unknown;
            TryDecodeTextureUnitBindingPname(pname, textureBindingTarget)) {
            *params = QueryTextureBindingOnUnit(MG_State::pGLContext->GetActiveTextureUnit(), textureBindingTarget);
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
        case GL_CLAMP_READ_COLOR:
            // Tri-state enum (GL_TRUE / GL_FALSE / GL_FIXED_ONLY). glGetIntegerv returns the raw
            // enum; GetFloatv/GetDoublev widen it and GetBooleanv converts nonzero to GL_TRUE, so
            // this single case serves every getter flavor.
            *params = static_cast<GLint>(MG_State::pGLContext->GetClampReadColor());
            return;
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
        case GL_DRAW_INDIRECT_BUFFER_BINDING: {
            auto& obj = MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::DrawIndirect).GetBoundObject();
            *params = obj ? static_cast<GLint>(obj->GetExternalIndex()) : 0;
            return;
        }
        case GL_MAX_SHADER_COMPILER_THREADS_KHR:
            // GL_KHR_parallel_shader_compile (GL_MAX_SHADER_COMPILER_THREADS_ARB is the same
            // 0x91B0). The number of threads MobileGL's compile pool would actually use, so
            // an application sizing its own submission batches gets a real answer.
            //
            // Zero when asynchronous compilation is off, which is the honest reply and the
            // one the extension defines for an implementation with no compiler threads: the
            // extension string is withdrawn in that configuration too, so a conforming
            // application never reaches this query, and one that asks anyway is told there
            // are none rather than being handed a thread count nothing will use.
            *params = MG_Util::Async::AsyncShaderCompileEnabled()
                          ? static_cast<GLint>(MG_Util::Async::ShaderCompilePool::Get().GetThreadCount())
                          : 0;
            return;
        case GL_MAX_DEBUG_GROUP_STACK_DEPTH:
            // KHR_debug floors this at 64 even when the group entry points are stubs: the
            // limit describes how deep glPushDebugGroup may nest, and 0 is not a legal answer.
            *params = kFrontendMaxDebugGroupStackDepth;
            return;
        case GL_MAX_DEBUG_MESSAGE_LENGTH:
            *params = 1024; // debug-message entrypoints are stubbed, but KHR_debug requires a valid limit
            return;
        case GL_MAX_DEBUG_LOGGED_MESSAGES:
            // Size of the message log ring; KHR_debug requires at least 1.
            *params = kFrontendMaxDebugLoggedMessages;
            return;
        case GL_DEBUG_GROUP_STACK_DEPTH:
            *params = 0; // debug-group entrypoints are stubbed
            return;
        case GL_CONTEXT_FLAGS: {
            *params = MG_State::pEGLContext ? MG_State::pEGLContext->GetCurrentContextFlags() : 0;
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
            *params = static_cast<GLint>(MG_State::pGLContext->GetHint(pname));
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
            *params = static_cast<GLint>(MG_State::pGLContext->GetHint(pname));
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
        case GL_MAX_COMBINED_ATOMIC_COUNTER_BUFFERS:
            *params = kFrontendMaxCombinedAtomicCounterBuffers;
            return;
        case GL_MAX_COMBINED_UNIFORM_BLOCKS:
            *params = ClampUniformBlockCount(kFrontendMaxCombinedUniformBlocks);
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
        case GL_MAX_FRAGMENT_ATOMIC_COUNTER_BUFFERS:
            *params = kFrontendMaxFragmentAtomicCounterBuffers;
            return;
        case GL_MAX_FRAGMENT_SHADER_STORAGE_BLOCKS:
            *params = ClampStorageBlockCount(16); // TODO
            return;
        case GL_MAX_FRAGMENT_INPUT_COMPONENTS:
            *params = kFrontendMaxFragmentInputComponents;
            return;
        case GL_MAX_FRAGMENT_IMAGE_UNIFORMS:
            *params = MG_Backend::pActiveBackendObject
                          ? MG_Backend::pActiveBackendObject->GetDynamicParameters().MaxFragmentImageUniforms
                          : MG_Backend::DynamicBackendParameters{}.MaxFragmentImageUniforms;
            return;
        case GL_MAX_FRAGMENT_UNIFORM_COMPONENTS:
            *params = kFrontendMaxFragmentUniformComponents;
            return;
        case GL_MAX_FRAGMENT_UNIFORM_VECTORS:
            *params = kFrontendMaxFragmentUniformVectors;
            return;
        case GL_MAX_FRAGMENT_UNIFORM_BLOCKS:
            *params = ClampUniformBlockCount(kFrontendMaxFragmentUniformBlocks);
            return;
        case GL_MAX_GEOMETRY_ATOMIC_COUNTERS:
            *params = kFrontendMaxGeometryAtomicCounters;
            return;
        case GL_MAX_GEOMETRY_ATOMIC_COUNTER_BUFFERS:
            *params = kFrontendMaxGeometryAtomicCounterBuffers;
            return;
        case GL_MAX_GEOMETRY_SHADER_STORAGE_BLOCKS:
            *params = ClampStorageBlockCount(16); // TODO
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
            *params = MG_Backend::pActiveBackendObject
                          ? MG_Backend::pActiveBackendObject->GetDynamicParameters().MaxGeometryImageUniforms
                          : MG_Backend::DynamicBackendParameters{}.MaxGeometryImageUniforms;
            return;
        case GL_MAX_GEOMETRY_TOTAL_OUTPUT_COMPONENTS:
            *params = kFrontendMaxGeometryTotalOutputComponents;
            return;
        case GL_MAX_GEOMETRY_UNIFORM_BLOCKS:
            *params = ClampUniformBlockCount(kFrontendMaxGeometryUniformBlocks);
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
        case GL_MAX_TESS_CONTROL_ATOMIC_COUNTER_BUFFERS:
            *params = kFrontendMaxTessControlAtomicCounterBuffers;
            return;
        case GL_MAX_TESS_EVALUATION_ATOMIC_COUNTERS:
            *params = kFrontendMaxTessEvaluationAtomicCounters;
            return;
        case GL_MAX_TESS_EVALUATION_ATOMIC_COUNTER_BUFFERS:
            *params = kFrontendMaxTessEvaluationAtomicCounterBuffers;
            return;
        case GL_MAX_TESS_CONTROL_IMAGE_UNIFORMS:
            *params = 0;
            return;
        case GL_MAX_TESS_EVALUATION_IMAGE_UNIFORMS:
            *params = 0;
            return;
        case GL_MAX_TESS_CONTROL_SHADER_STORAGE_BLOCKS:
            *params = ClampStorageBlockCount(16); // TODO
            return;
        case GL_MAX_TESS_EVALUATION_SHADER_STORAGE_BLOCKS:
            *params = ClampStorageBlockCount(16); // TODO
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
        case GL_MAX_VERTEX_ATOMIC_COUNTER_BUFFERS:
            *params = kFrontendMaxVertexAtomicCounterBuffers;
            return;
        case GL_MAX_VERTEX_IMAGE_UNIFORMS:
            *params = MG_Backend::pActiveBackendObject
                          ? MG_Backend::pActiveBackendObject->GetDynamicParameters().MaxVertexImageUniforms
                          : MG_Backend::DynamicBackendParameters{}.MaxVertexImageUniforms;
            return;
        case GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS:
            *params = ClampStorageBlockCount(16); // TODO
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
            *params = ClampUniformBlockCount(kFrontendMaxVertexUniformBlocks);
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
            *params = static_cast<GLint>(std::lround(MG_State::pGLContext->GetPointFadeThresholdSize()));
            return;
        case GL_POINT_SPRITE_COORD_ORIGIN:
            *params = static_cast<GLint>(MG_State::pGLContext->GetPointSpriteCoordOrigin());
            return;
        case GL_PRIMITIVE_RESTART:
            *params = MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::PrimitiveRestart) ? GL_TRUE
                                                                                                    : GL_FALSE;
            return;
        case GL_PRIMITIVE_RESTART_INDEX:
            *params = static_cast<GLint>(MG_State::pGLContext->GetPrimitiveRestartIndex());
            return;
        case GL_PROGRAM_BINARY_FORMATS:
            *params = 0; // program-binary entrypoints are stubbed
            return;
        case GL_PROGRAM_PIPELINE_BINDING:
            *params = static_cast<GLint>(MG_State::pGLContext->GetBoundProgramPipelineName());
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
            params[0] = static_cast<GLint>(MG_State::pGLContext->GetPolygonModeFront());
            params[1] = static_cast<GLint>(MG_State::pGLContext->GetPolygonModeBack());
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
            *params = static_cast<GLint>(MG_State::pGLContext->GetHint(pname));
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
        case GL_SAMPLER_BINDING:
            *params = QuerySamplerBindingOnUnit(MG_State::pGLContext->GetActiveTextureUnit());
            return;
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
        case GL_TEXTURE_COMPRESSION_HINT:
            *params = static_cast<GLint>(MG_State::pGLContext->GetHint(pname));
            return;
        case GL_TEXTURE_BUFFER_OFFSET_ALIGNMENT:
            *params = MG_Backend::pActiveBackendObject->GetDynamicParameters().TextureBufferOffsetAlignment;
            return;
        case GL_TIMESTAMP: {
            Int64 timestamp = 0;
            if (!MG_Config::Features.DisableTimerQuery) {
                if (const auto getGpuTimestampNs = MG_Backend::gBackendFunctionsTable.GL.GetGpuTimestampNs) {
                    timestamp = getGpuTimestampNs();
                }
            }
            // 32-bit query: clamp per the GL state-query conversion rules.
            *params = timestamp > static_cast<Int64>(INT_MAX) ? INT_MAX : static_cast<GLint>(timestamp);
            return;
        }
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
        // glBindBufferBase/Range set the GENERIC binding point too (GL 4.6 core 6.1.1), and this
        // is the one indexed-buffer family whose non-indexed query was never answered - so it
        // fell through to INVALID_ENUM and left the caller's variable holding whatever was in its
        // stack slot. _START/_SIZE stay indexed-only, exactly like their uniform-buffer siblings.
        case GL_ATOMIC_COUNTER_BUFFER_BINDING:
            if (const auto& obj =
                    MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::AtomicCounter).GetBoundObject()) {
                *params = static_cast<GLint>(obj->GetExternalIndex());
            } else {
                *params = 0;
            }
            return;
        case GL_ATOMIC_COUNTER_BUFFER_START:
            RecordIndexedOnlyGetterError(__func__, pname);
            return;
        case GL_ATOMIC_COUNTER_BUFFER_SIZE:
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
        // The vertex buffer binding points are per-binding-index state, so the non-indexed getter
        // has nothing to answer with (GL 4.6 core table 23.4).
        case GL_VERTEX_BINDING_BUFFER:
        case GL_VERTEX_BINDING_DIVISOR:
        case GL_VERTEX_BINDING_OFFSET:
        case GL_VERTEX_BINDING_STRIDE:
            RecordIndexedOnlyGetterError(__func__, pname);
            return;
        case GL_MAX_VERTEX_ATTRIB_RELATIVE_OFFSET:
            *params = static_cast<GLint>(VertexArrayImpl::GetMaxVertexAttribRelativeOffset());
            return;
        case GL_MAX_VERTEX_ATTRIB_BINDINGS:
            *params = static_cast<GLint>(VertexArrayImpl::GetMaxVertexAttribBindings());
            return;
        case GL_MAX_VERTEX_ATTRIB_STRIDE:
            *params = static_cast<GLint>(VertexArrayImpl::GetMaxVertexAttribStride());
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
            // Reports the requested context profile (EGL defaults 3.x contexts to core);
            // MOBILEGL_RELAXED_SEMANTICS loosens behavior without changing the identity.
            *params = MG_State::pEGLContext && MG_State::pEGLContext->IsCurrentContextOpenGLCompatibilityProfile()
                          ? GL_CONTEXT_COMPATIBILITY_PROFILE_BIT
                          : GL_CONTEXT_CORE_PROFILE_BIT;
            return;
        default:
            break;
        }

        const auto& activeBackendObject = MG_Backend::pActiveBackendObject;
        if (!activeBackendObject) {
            MGLOG_E_ONCE("activeBackendObject is not initialized!");
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
            *params = ClampStorageBlockCount(dynamicParameters.MaxComputeShaderStorageBlocks);
            break;
        case GL_MAX_COMBINED_SHADER_STORAGE_BLOCKS:
            *params = ClampStorageBlockCount(dynamicParameters.MaxCombinedShaderStorageBlocks);
            break;
        case GL_MAX_COMPUTE_UNIFORM_BLOCKS:
            *params = ClampUniformBlockCount(dynamicParameters.MaxComputeUniformBlocks);
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
            *params = std::max(dynamicParameters.MaxColorTextureSamples, GetAdvertisedMaxSamples());
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
            *params = std::max(dynamicParameters.MaxDepthTextureSamples, GetAdvertisedMaxSamples());
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
            *params = std::max(dynamicParameters.MaxIntegerSamples, GetAdvertisedMaxSamples());
            break;
        case GL_MAX_RENDERBUFFER_SIZE:
            *params = dynamicParameters.MaxRenderbufferSize;
            break;
        case GL_MAX_SAMPLE_MASK_WORDS:
            *params = dynamicParameters.MaxSampleMaskWords;
            break;
        case GL_PATCH_VERTICES:
            *params = static_cast<GLint>(MG_State::pGLContext->GetPatchVertices());
            break;
        case GL_MAX_PATCH_VERTICES:
            *params = dynamicParameters.MaxPatchVertices;
            break;
        case GL_MAX_TESS_GEN_LEVEL:
            *params = dynamicParameters.MaxTessGenLevel;
            break;
        case GL_MIN_PROGRAM_TEXTURE_GATHER_OFFSET:
            *params = dynamicParameters.MinProgramTextureGatherOffset;
            break;
        case GL_MAX_PROGRAM_TEXTURE_GATHER_OFFSET:
            *params = dynamicParameters.MaxProgramTextureGatherOffset;
            break;
        case GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS:
            *params = static_cast<GLint>(GetIndexedBufferQueryPointCount(BufferTarget::ShaderStorage));
            break;
        case GL_MAX_SHADER_STORAGE_BLOCK_SIZE:
            // 64-bit state (see GetInteger64v); the 32-bit query saturates, per the GL
            // state-query conversion rules.
            *params = static_cast<GLint>(std::min<Uint64>(dynamicParameters.MaxShaderStorageBlockSize,
                                                          static_cast<Uint64>(INT32_MAX)));
            break;
        case GL_MAX_ATOMIC_COUNTER_BUFFER_BINDINGS:
            // NOT the frontend's binding-point array size: GetIndexedBufferQueryPointCount
            // clamps this family to the range a lowered counter block can actually be served
            // from, which is the same number glslang compiles a layout(binding = N) atomic_uint
            // against and the same one glBindBufferBase validates an index against.
            *params = static_cast<GLint>(GetIndexedBufferQueryPointCount(BufferTarget::AtomicCounter));
            break;
        case GL_MAX_ATOMIC_COUNTER_BUFFER_SIZE:
            // The conformance suite splits this evenly across every advertised binding point and
            // binds all of them in one glBindBuffersRange
            // (KHR-GL44.multi_bind.functional_bind_buffers_range), so the pair has to divide -
            // a zero-sized range is INVALID_VALUE before BindBufferRange binds anything. The
            // shared constant is 16384 over 8 binding points, which divides.
            *params = kFrontendMaxAtomicCounterBufferSize;
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
        // ARB_transform_feedback3 limits. The GL CTS queries these before checking
        // whether the extension is advertised and requires no GL error; desktop
        // drivers all accept them, so answer with the separate-attrib capacity and
        // the single vertex stream the backends provide.
        case GL_MAX_TRANSFORM_FEEDBACK_BUFFERS:
            *params = kFrontendMaxTransformFeedbackSeparateAttribs;
            break;
        case GL_MAX_VERTEX_STREAMS:
            *params = 1;
            break;
        case GL_TRANSFORM_FEEDBACK_ACTIVE:
            *params = MG_State::pGLContext->IsTransformFeedbackActive() ? 1 : 0;
            break;
        case GL_TRANSFORM_FEEDBACK_PAUSED:
            *params = MG_State::pGLContext->IsTransformFeedbackPaused() ? 1 : 0;
            break;
        case GL_TRANSFORM_FEEDBACK_BINDING:
            *params = static_cast<GLint>(MG_State::pGLContext->GetBoundTransformFeedbackName());
            break;
        case GL_MAX_TEXTURE_IMAGE_UNITS:
            *params = dynamicParameters.MaxTextureImageUnits;
            break;
        case GL_MAX_TEXTURE_SIZE:
            *params = dynamicParameters.MaxTextureSize;
            break;
        case GL_MAX_UNIFORM_BUFFER_BINDINGS:
            // Never advertise more bindings than the state layer's indexed-binding array can track
            // (BufferState::BufferBindingPointCount): glBindBufferBase rejects indices past that
            // capacity, and the GL CTS per-case state reset calls glBindBufferBase on every
            // advertised index and expects no error. The floor equals the GL 3.3 core minimum
            // (36), so the clamp never under-advertises.
            *params = std::clamp(dynamicParameters.MaxUniformBufferBindings, kFrontendMinUniformBufferBindings,
                                 static_cast<GLint>(MG_State::GLState::BufferBindingPointCount));
            break;
        case GL_MAX_UNIFORM_BLOCK_SIZE:
            *params = dynamicParameters.MaxUniformBlockSize;
            break;
        case GL_MAX_VERTEX_ATTRIBS:
            // Single source of truth with the validators: the value reported here is exactly the bound
            // glVertexAttrib*/glGetVertexAttrib*/glBindAttribLocation enforce, and it never exceeds the
            // state layer's current-value storage capacity.
            *params = static_cast<GLint>(VertexArrayImpl::GetMaxVertexAttribs());
            break;
        case GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS:
            *params = dynamicParameters.MaxVertexTextureImageUnits;
            break;
        case GL_MAX_VIEWPORT_DIMS:
            params[0] = dynamicParameters.MaxViewportWidth;
            params[1] = dynamicParameters.MaxViewportHeight;
            break;
        case GL_MAX_VIEWPORTS:
            // The frontend's own state width, not the backend's device limit. GL 4.3 core
            // requires MAX_VIEWPORTS >= 16 and every indexed viewport entry point validates
            // against RenderStateParameters::MAX_VIEWPORTS, so reporting anything else would
            // either advertise viewports the state cannot hold or reject indices it can. A
            // Vulkan device without the multiViewport feature reports maxViewports == 1, which
            // limits what can be RASTERIZED to more than one rectangle (see the multiViewport
            // gate in VulkanRenderer), not what the GL state can hold; caps.MaxViewports keeps
            // carrying that device number for exactly that decision.
            *params = static_cast<GLint>(RenderStateParameters::MAX_VIEWPORTS);
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
        case GL_MIN_FRAGMENT_INTERPOLATION_OFFSET:
            *params = static_cast<GLint>(std::lround(dynamicParameters.MinFragmentInterpolationOffset));
            break;
        case GL_MAX_FRAGMENT_INTERPOLATION_OFFSET:
            *params = static_cast<GLint>(std::lround(dynamicParameters.MaxFragmentInterpolationOffset));
            break;
        case GL_FRAGMENT_INTERPOLATION_OFFSET_BITS:
            *params = dynamicParameters.FragmentInterpolationOffsetBits;
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
            *params = GetAdvertisedMaxSamples();
            break;
        case GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT:
            // Float state (see GetFloatv); rounded to nearest for the integer query per GL 3.3 6.1.2.
            *params = static_cast<GLint>(std::lround(dynamicParameters.MaxTextureMaxAnisotropy));
            break;
        default:
            MGLOG_D("glGetIntegerv: Invalid enum %s (0x%X)", MG_Util::ConvertGLEnumToString(pname).c_str(), pname);
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

    GLenum GetGraphicsResetStatus() {
        // MobileGL does not implement robustness reset notification, so report GL_NO_ERROR
        // ("no reset detected"). Returning the generic stub's (GLenum)1 makes dEQP read a lost
        // device after every case (gl3cTestPackages.cpp:121) and, under the default
        // --deqp-terminate-on-device-lost=enable, tear the whole CTS run down.
        return GL_NO_ERROR;
    }
} // namespace MobileGL::MG_Impl::GLImpl
