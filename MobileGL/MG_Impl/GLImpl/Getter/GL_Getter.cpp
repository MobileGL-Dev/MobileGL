// MobileGL - MobileGL/MG_Impl/GLImpl/Getter/GL_Getter.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "GL_Getter.h"
#include "GL/gl.h"
#include "MG_Util/Debug/Log.h"
#include <Config.h>
#include <MGGitHash.h>
#include <MG_State/GLState/Core.h>
#include <MG_State/GLState/ErrorState/ErrorInfo.h>
#include <MG_Util/Converters/GLToStr/GLEnumConverter.h>
#include <MG_Util/Converters/MGToGL/ErrorCodeConverter.h>
#include <MG_Util/Converters/MGToStr/GLExtensionConverter.h>
#include <MG_Util/Converters/MGToGL/RenderStateEnumConverter.h>
#include <MG_State/GLState/FramebufferState/FramebufferObject.h>
#include <MG_Backend/BackendObjects.h>

namespace MobileGL::MG_Impl::GLImpl {
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
            if (vendorString.empty()) {
                if (rendererInfo.ExtraVendor.has_value()) {
                    vendorString = std::format("{}{}", MG_Config::CoreVendor, rendererInfo.ExtraVendor.value());
                } else {
                    vendorString = MG_Config::CoreVendor;
                }
            }
            MGLOG_D("vendorString: %s", vendorString.c_str());
            return (const GLubyte*)vendorString.c_str();
        case GL_VERSION: {
            if (versionStr.empty()) {
                versionStr =
                    std::format("{} {} {}, {} Backend, GIT@" GIT_COMMIT_HASH_SHORT,
                                rendererInfo.RendererGLInfo.TargetGLVersion.toString(), MG_Config::ProjectName,
                                MG_Config::CoreVersion.toFormattedString(MG_Config::DefaultVersionStringFormatAttrib),
                                rendererInfo.BackendName);
            }
            MGLOG_D("versionStr: %s", versionStr.c_str());
            return (const GLubyte*)versionStr.c_str();
        }
        case GL_RENDERER: {
            if (rendererString.empty()) {
                String backendVersionStr = activeBackendObject->GetBackendAPIVersionString();
                rendererString =
                    std::format("{} ({}) ({})", rendererInfo.RendererName, MG_Config::CoreName, backendVersionStr);
            }
            MGLOG_D("rendererString: %s", rendererString.c_str());
            return (const GLubyte*)rendererString.c_str();
        }
        case GL_SHADING_LANGUAGE_VERSION:
            if (shadingLanguageVersion.empty()) {
                shadingLanguageVersion =
                    std::format("{} {}", rendererInfo.RendererGLInfo.TargetGLSLVersion.toString({true, false}),
                                MG_Config::ProjectName);
            }
            MGLOG_D("shadingLanguageVersion: %s", shadingLanguageVersion.c_str());
            return (const GLubyte*)shadingLanguageVersion.c_str();
        case GL_EXTENSIONS:
            if (extensionsString.empty()) {
                for (auto& ext : rendererInfo.RendererGLInfo.Extensions) {
                    extensionsString += MG_Util::ConvertGLExtToString(ext);
                    extensionsString += " ";
                }
                extensionsString.pop_back();
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
        static Bool initialized = false;
        if (!initialized) {
            extStrings.reserve(exts.size());
            for (const auto& ext : exts) {
                extStrings.emplace_back(MG_Util::ConvertGLExtToString(ext));
            }
            initialized = true;
        }

        return (const GLubyte*)extStrings[index].c_str();
    }

    void GetIntegerv(GLenum pname, GLint* params) {
        MGLOG_D("glGetIntegerv, pname: %s", MG_Util::ConvertGLEnumToString(pname).c_str());
        if (!params) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", "GetIntegerv", "params pointer cannot be null"));
            return;
        }

        const auto& activeBackendObject = MG_Backend::pActiveBackendObject;
        if (!activeBackendObject) {
            MGLOG_E("activeBackendObject is not initialized!");
            return;
        }
        const auto& rendererInfo = activeBackendObject->GetRendererInfo();

        switch (pname) {
        case GL_ACTIVE_TEXTURE:
            *params = MG_State::pGLContext->GetActiveTextureUnit() + GL_TEXTURE0;
            break;
        case GL_ALIASED_LINE_WIDTH_RANGE:
            *params = 0; // TODO
            break;
        case GL_ARRAY_BUFFER_BINDING: {
            auto obj = MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::Vertex).GetBoundObject();
            if (obj)
                *params = obj->GetExternalIndex();
            else
                *params = 0;
            break;
        }
        case GL_BLEND:
            *params = MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::Blend) ? GL_TRUE : GL_FALSE;
            break;
        case GL_BLEND_COLOR:
            *params = 0; // TODO
            break;
        case GL_BLEND_DST_ALPHA: {
            BlendFactor srcRGB, dstRGB, srcAlpha, dstAlpha;
            MG_State::pGLContext->GetBlendFunc(srcRGB, dstRGB, srcAlpha, dstAlpha);
            *params = static_cast<GLint>(dstAlpha);
            break;
        }
        case GL_BLEND_DST_RGB: {
            BlendFactor srcRGB, dstRGB, srcAlpha, dstAlpha;
            MG_State::pGLContext->GetBlendFunc(srcRGB, dstRGB, srcAlpha, dstAlpha);
            *params = static_cast<GLint>(dstRGB);
            break;
        }
        case GL_BLEND_EQUATION_RGB:
            *params = 0; // TODO
            break;
        case GL_BLEND_EQUATION_ALPHA:
            *params = 0; // TODO
            break;
        case GL_BLEND_SRC_ALPHA: {
            BlendFactor srcRGB, dstRGB, srcAlpha, dstAlpha;
            MG_State::pGLContext->GetBlendFunc(srcRGB, dstRGB, srcAlpha, dstAlpha);
            *params = static_cast<GLint>(srcAlpha);
            break;
        }
        case GL_BLEND_SRC_RGB: {
            BlendFactor srcRGB, dstRGB, srcAlpha, dstAlpha;
            MG_State::pGLContext->GetBlendFunc(srcRGB, dstRGB, srcAlpha, dstAlpha);
            *params = static_cast<GLint>(srcRGB);
            break;
        }
        case GL_COLOR_CLEAR_VALUE:
            *params = 0; // TODO
            break;
        case GL_COLOR_LOGIC_OP:
            *params = 0; // TODO
            break;
        case GL_COLOR_WRITEMASK: {
            BoolVec4 mask = MG_State::pGLContext->GetColorMask();
            params[0] = mask.x() ? GL_TRUE : GL_FALSE;
            params[1] = mask.y() ? GL_TRUE : GL_FALSE;
            params[2] = mask.z() ? GL_TRUE : GL_FALSE;
            params[3] = mask.w() ? GL_TRUE : GL_FALSE;
            break;
        }
        case GL_COMPRESSED_TEXTURE_FORMATS:
            *params = 0; // TODO
            break;
        case GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS:
            *params = 0; // TODO
            break;
        case GL_MAX_COMBINED_SHADER_STORAGE_BLOCKS:
            *params = 0; // TODO
            break;
        case GL_MAX_COMPUTE_UNIFORM_BLOCKS:
            *params = 0; // TODO
            break;
        case GL_MAX_COMPUTE_TEXTURE_IMAGE_UNITS:
            *params = 0; // TODO
            break;
        case GL_MAX_COMPUTE_UNIFORM_COMPONENTS:
            *params = 0; // TODO
            break;
        case GL_MAX_COMPUTE_ATOMIC_COUNTERS:
            *params = 0; // TODO
            break;
        case GL_MAX_COMPUTE_ATOMIC_COUNTER_BUFFERS:
            *params = 0; // TODO
            break;
        case GL_MAX_COMBINED_COMPUTE_UNIFORM_COMPONENTS:
            *params = 0; // TODO
            break;
        case GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS:
            *params = 0; // TODO
            break;
        case GL_MAX_COMPUTE_WORK_GROUP_COUNT:
            *params = 0; // TODO
            break;
        case GL_MAX_COMPUTE_WORK_GROUP_SIZE:
            *params = 0; // TODO
            break;
        case GL_DISPATCH_INDIRECT_BUFFER_BINDING:
            *params = 0; // TODO
            break;
        case GL_MAX_DEBUG_GROUP_STACK_DEPTH:
            *params = 0; // TODO
            break;
        case GL_DEBUG_GROUP_STACK_DEPTH:
            *params = 0; // TODO
            break;
        case GL_CONTEXT_FLAGS:
            *params = 0; // TODO
            break;
        case GL_CULL_FACE:
            *params = MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::CullFace) ? GL_TRUE : GL_FALSE;
            break;
        case GL_CURRENT_PROGRAM: {
            const auto& currentProgram = MG_State::pGLContext->GetCurrentProgram();
            *params = currentProgram ? currentProgram->GetExternalIndex() : 0;
            break;
        }
        case GL_DEPTH_CLEAR_VALUE:
            *params = MG_State::pGLContext->GetClearDepth();
            break;
        case GL_DEPTH_FUNC:
            *params = MG_Util::ConvertDepthTestFuncToGLEnum(MG_State::pGLContext->GetDepthFunc());
            break;
        case GL_DEPTH_RANGE:
            *params = 0; // TODO
            break;
        case GL_DEPTH_TEST:
            *params = MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::DepthTest) ? GL_TRUE : GL_FALSE;
            break;
        case GL_DEPTH_WRITEMASK:
            *params = MG_State::pGLContext->GetDepthMask() ? GL_TRUE : GL_FALSE;
            break;
        case GL_DITHER:
            *params = MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::Dither) ? GL_TRUE : GL_FALSE;
            break;
        case GL_DOUBLEBUFFER:
            *params = 0; // TODO
            break;
        case GL_DRAW_BUFFER:
            *params = 0; // TODO
            break;
        case GL_DRAW_BUFFER0:
            *params = 0; // TODO
            break;
        case GL_DRAW_FRAMEBUFFER_BINDING: {
            const auto& FBO = MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Draw).GetBoundObject();
            *params = FBO ? FBO->GetExternalIndex() : 0;
            break;
        }
        case GL_READ_FRAMEBUFFER_BINDING: {
            const auto& FBO = MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Read).GetBoundObject();
            *params = FBO ? FBO->GetExternalIndex() : 0;
            break;
        }
        case GL_ELEMENT_ARRAY_BUFFER_BINDING: {
            if (!MG_State::pGLContext->GetBoundVertexArray()) {
                *params = 0;
                break;
            }
            const auto& bufferObject = MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::Index).GetBoundObject();
            *params = bufferObject ? bufferObject->GetExternalIndex() : 0;
            break;
        }
        case GL_FRAGMENT_SHADER_DERIVATIVE_HINT:
            *params = 0; // TODO
            break;
        case GL_IMPLEMENTATION_COLOR_READ_FORMAT:
            *params = 0; // TODO
            break;
        case GL_IMPLEMENTATION_COLOR_READ_TYPE:
            *params = 0; // TODO
            break;
        case GL_LINE_SMOOTH:
            *params = 0; // TODO
            break;
        case GL_LINE_SMOOTH_HINT:
            *params = 0; // TODO
            break;
        case GL_LINE_WIDTH:
            *params = 0; // TODO
            break;
        case GL_LAYER_PROVOKING_VERTEX:
            *params = 0; // TODO
            break;
        case GL_LOGIC_OP_MODE:
            *params = 0; // TODO
            break;
        case GL_MAJOR_VERSION:
            *params = rendererInfo.RendererGLInfo.TargetGLVersion.Major;
            break;
        case GL_MAX_3D_TEXTURE_SIZE:
            *params = 1024 * 16; // TODO
            break;
        case GL_MAX_ARRAY_TEXTURE_LAYERS:
            *params = 2048; // TODO
            break;
        case GL_MAX_CLIP_DISTANCES:
            *params = 8; // TODO
            break;
        case GL_MAX_COLOR_TEXTURE_SAMPLES:
            *params = 16; // TODO
            break;
        case GL_MAX_COMBINED_ATOMIC_COUNTERS:
            *params = 16; // TODO
            break;
        case GL_MAX_COMBINED_FRAGMENT_UNIFORM_COMPONENTS:
            *params = 1024 * 228; // TODO
            break;
        case GL_MAX_COMBINED_GEOMETRY_UNIFORM_COMPONENTS:
            *params = 1024 * 228; // TODO
            break;
        case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS:
            *params = 192; // TODO
            break;
        case GL_MAX_COMBINED_UNIFORM_BLOCKS:
            *params = 70; // TODO
            break;
        case GL_MAX_COMBINED_VERTEX_UNIFORM_COMPONENTS:
            *params = 1024 * 228; // TODO
            break;
        case GL_MAX_CUBE_MAP_TEXTURE_SIZE:
            *params = 1024 * 16; // TODO
            break;
        case GL_MAX_DEPTH_TEXTURE_SAMPLES:
            *params = 16; // TODO
            break;
        case GL_MAX_DUAL_SOURCE_DRAW_BUFFERS:
            *params = 1; // TODO
            break;
        case GL_MAX_ELEMENTS_INDICES:
            *params = 1024 * 1024; // TODO
            break;
        case GL_MAX_ELEMENTS_VERTICES:
            *params = 1024 * 1024; // TODO
            break;
        case GL_MAX_FRAGMENT_ATOMIC_COUNTERS:
            *params = 1024 * 4; // TODO
            break;
        case GL_MAX_FRAGMENT_SHADER_STORAGE_BLOCKS:
            *params = 16; // TODO
            break;
        case GL_MAX_FRAGMENT_INPUT_COMPONENTS:
            *params = 128; // TODO
            break;
        case GL_MAX_FRAGMENT_UNIFORM_COMPONENTS:
            *params = 1024 * 4; // TODO
            break;
        case GL_MAX_FRAGMENT_UNIFORM_VECTORS:
            *params = 256; // TODO
            break;
        case GL_MAX_FRAGMENT_UNIFORM_BLOCKS:
            *params = 14; // TODO
            break;
        case GL_MAX_FRAMEBUFFER_WIDTH:
            *params = 1024 * 16; // TODO
            break;
        case GL_MAX_FRAMEBUFFER_HEIGHT:
            *params = 1024 * 16; // TODO
            break;
        case GL_MAX_FRAMEBUFFER_LAYERS:
            *params = 1024 * 2; // TODO
            break;
        case GL_MAX_FRAMEBUFFER_SAMPLES:
            *params = 16; // TODO
            break;
        case GL_MAX_GEOMETRY_ATOMIC_COUNTERS:
            *params = 14; // TODO
            break;
        case GL_MAX_GEOMETRY_SHADER_STORAGE_BLOCKS:
            *params = 16; // TODO
            break;
        case GL_MAX_GEOMETRY_INPUT_COMPONENTS:
            *params = 128; // TODO
            break;
        case GL_MAX_GEOMETRY_OUTPUT_COMPONENTS:
            *params = 128; // TODO
            break;
        case GL_MAX_GEOMETRY_TEXTURE_IMAGE_UNITS:
            *params = 8; // TODO
            break;
        case GL_MAX_GEOMETRY_UNIFORM_BLOCKS:
            *params = 14; // TODO
            break;
        case GL_MAX_GEOMETRY_UNIFORM_COMPONENTS:
            *params = 1024 * 4; // TODO
            break;
        case GL_MAX_INTEGER_SAMPLES:
            *params = 16; // TODO
            break;
        case GL_MIN_MAP_BUFFER_ALIGNMENT:
            *params = 64; // TODO
            break;
        case GL_MAX_LABEL_LENGTH:
            *params = 256; // TODO
            break;
        case GL_MAX_PROGRAM_TEXEL_OFFSET:
            *params = 7; // TODO
            break;
        case GL_MIN_PROGRAM_TEXEL_OFFSET:
            *params = -8; // TODO
            break;
        case GL_MAX_RECTANGLE_TEXTURE_SIZE:
            *params = 16 * 1024; // TODO
            break;
        case GL_MAX_RENDERBUFFER_SIZE:
            *params = 16 * 1024; // TODO
            break;
        case GL_MAX_SAMPLE_MASK_WORDS:
            *params = 1; // TODO
            break;
        case GL_MAX_SERVER_WAIT_TIMEOUT:
            *params = INT_MAX; // TODO
            break;
        case GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS:
            *params = 16; // TODO
            break;
        case GL_MAX_TESS_CONTROL_ATOMIC_COUNTERS:
            *params = 1024 * 4; // TODO
            break;
        case GL_MAX_TESS_EVALUATION_ATOMIC_COUNTERS:
            *params = 1024 * 4; // TODO
            break;
        case GL_MAX_TESS_CONTROL_SHADER_STORAGE_BLOCKS:
            *params = 16; // TODO
            break;
        case GL_MAX_TESS_EVALUATION_SHADER_STORAGE_BLOCKS:
            *params = 16; // TODO
            break;
        case GL_MAX_TEXTURE_BUFFER_SIZE:
            *params = 1024 * 1024 * 128; // TODO
            break;
        case GL_MAX_TEXTURE_IMAGE_UNITS:
            *params = 32; // TODO
            break;
        case GL_MAX_TEXTURE_LOD_BIAS:
            *params = 15; // TODO
            break;
        case GL_MAX_TEXTURE_SIZE:
            *params = 1024 * 16; // TODO
            break;
        case GL_MAX_UNIFORM_BUFFER_BINDINGS:
            *params = 84; // TODO
            break;
        case GL_MAX_UNIFORM_BLOCK_SIZE:
            *params = 1024 * 24; // TODO
            break;
        case GL_MAX_UNIFORM_LOCATIONS:
            *params = 1024 * 4; // TODO
            break;
        case GL_MAX_VARYING_COMPONENTS:
            *params = 16; // TODO
            break;
        case GL_MAX_VARYING_VECTORS:
            *params = 16; // TODO
            break;
        case GL_MAX_VERTEX_ATOMIC_COUNTERS:
            *params = 1024 * 4; // TODO
            break;
        case GL_MAX_VERTEX_ATTRIBS:
            *params = 16; // TODO
            break;
        case GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS:
            *params = 16; // TODO
            break;
        case GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS:
            *params = 32; // TODO
            break;
        case GL_MAX_VERTEX_UNIFORM_COMPONENTS:
            *params = 1024 * 4; // TODO
            break;
        case GL_MAX_VERTEX_UNIFORM_VECTORS:
            *params = 1024; // TODO
            break;
        case GL_MAX_VERTEX_OUTPUT_COMPONENTS:
            *params = 128; // TODO
            break;
        case GL_MAX_VERTEX_UNIFORM_BLOCKS:
            *params = 14; // TODO
            break;
        case GL_MAX_VIEWPORT_DIMS:
            params[0] = 1024 * 16; // TODO
            params[1] = 1024 * 16; // TODO
            break;
        case GL_MAX_VIEWPORTS:
            *params = 16; // TODO
            break;
        case GL_MINOR_VERSION:
            *params = rendererInfo.RendererGLInfo.TargetGLVersion.Minor;
            break;
        case GL_NUM_COMPRESSED_TEXTURE_FORMATS:
            *params = 18; // TODO
            break;
        case GL_NUM_EXTENSIONS:
            *params = (Int)rendererInfo.RendererGLInfo.Extensions.size();
            break;
        case GL_NUM_PROGRAM_BINARY_FORMATS:
            *params = 2; // TODO
            break;
        case GL_NUM_SHADER_BINARY_FORMATS:
            *params = 0; // TODO
            break;
        case GL_PACK_ALIGNMENT:
            *params = MG_State::pGLContext->GetPixelStoreParam(PixelStoreParam::PackAlignment);
            break;
        case GL_PACK_IMAGE_HEIGHT:
            *params = MG_State::pGLContext->GetPixelStoreParam(PixelStoreParam::PackImageHeight);
            break;
        case GL_PACK_LSB_FIRST:
            *params = MG_State::pGLContext->GetPixelStoreParam(PixelStoreParam::PackLSBFirst);
            break;
        case GL_PACK_ROW_LENGTH:
            *params = MG_State::pGLContext->GetPixelStoreParam(PixelStoreParam::PackRowLength);
            break;
        case GL_PACK_SKIP_IMAGES:
            *params = MG_State::pGLContext->GetPixelStoreParam(PixelStoreParam::PackSkipImages);
            break;
        case GL_PACK_SKIP_PIXELS:
            *params = MG_State::pGLContext->GetPixelStoreParam(PixelStoreParam::PackSkipPixels);
            break;
        case GL_PACK_SKIP_ROWS:
            *params = MG_State::pGLContext->GetPixelStoreParam(PixelStoreParam::PackSkipRows);
            break;
        case GL_PACK_SWAP_BYTES:
            *params = MG_State::pGLContext->GetPixelStoreParam(PixelStoreParam::PackSwapBytes);
            break;
        case GL_PIXEL_PACK_BUFFER_BINDING:
            *params = 0; // TODO
            break;
        case GL_PIXEL_UNPACK_BUFFER_BINDING:
            *params = 0; // TODO
            break;
        case GL_POINT_FADE_THRESHOLD_SIZE:
            *params = 0; // TODO
            break;
        case GL_PRIMITIVE_RESTART_INDEX:
            *params = 0; // TODO
            break;
        case GL_PROGRAM_BINARY_FORMATS:
            *params = 0; // TODO
            break;
        case GL_PROGRAM_PIPELINE_BINDING:
            *params = 0; // TODO
            break;
        case GL_PROGRAM_POINT_SIZE:
            *params = MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::ProgramPointSize) ? GL_TRUE : GL_FALSE;
            break;
        case GL_PROVOKING_VERTEX:
            *params = 0; // TODO
            break;
        case GL_POINT_SIZE:
            *params = 0; // TODO
            break;
        case GL_POINT_SIZE_GRANULARITY:
            *params = 0; // TODO
            break;
        case GL_POINT_SIZE_RANGE:
            *params = 0; // TODO
            break;
        case GL_POLYGON_OFFSET_FACTOR:
            *params = 0; // TODO
            break;
        case GL_POLYGON_OFFSET_UNITS:
            *params = 0; // TODO
            break;
        case GL_POLYGON_OFFSET_FILL:
            *params =
                MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::PolygonOffsetFill) ? GL_TRUE : GL_FALSE;
            break;
        case GL_POLYGON_OFFSET_LINE:
            *params =
                MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::PolygonOffsetLine) ? GL_TRUE : GL_FALSE;
            break;
        case GL_POLYGON_OFFSET_POINT:
            *params =
                MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::PolygonOffsetPoint) ? GL_TRUE : GL_FALSE;
            break;
        case GL_POLYGON_SMOOTH:
            *params = MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::PolygonSmooth) ? GL_TRUE : GL_FALSE;
            break;
        case GL_POLYGON_SMOOTH_HINT:
            *params = 0; // TODO
            break;
        case GL_READ_BUFFER:
            *params = 0; // TODO
            break;
        case GL_RENDERBUFFER_BINDING:
            *params = 0; // TODO
            break;
        case GL_SAMPLE_BUFFERS:
            *params = 0; // TODO
            break;
        case GL_SAMPLE_COVERAGE_VALUE:
            *params = 0; // TODO
            break;
        case GL_SAMPLE_COVERAGE_INVERT:
            *params = 0; // TODO
            break;
        case GL_SAMPLE_MASK_VALUE:
            *params = 0; // TODO
            break;
        case GL_SAMPLER_BINDING: {
            Int unit = MG_State::pGLContext->GetActiveTextureUnit();
            const auto& tu = MG_State::pGLContext->GetTextureUnitObject(unit);
            const auto& sampler = tu.GetSamplerObject();
            *params = sampler ? static_cast<GLint>(sampler->GetExternalIndex()) : 0;
            break;
        }
        case GL_SAMPLES:
            *params = 0; // TODO
            break;
        case GL_SCISSOR_BOX:
            *params = 0; // TODO
            break;
        case GL_SCISSOR_TEST:
            *params = MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::ScissorTest) ? GL_TRUE : GL_FALSE;
            break;
        case GL_SHADER_COMPILER:
            *params = 0; // TODO
            break;
        case GL_SHADER_STORAGE_BUFFER_BINDING:
            *params = 0; // TODO
            break;
        case GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT:
            *params = 0; // TODO
            break;
        case GL_SHADER_STORAGE_BUFFER_START:
            *params = 0; // TODO
            break;
        case GL_SHADER_STORAGE_BUFFER_SIZE:
            *params = 0; // TODO
            break;
        case GL_SMOOTH_LINE_WIDTH_RANGE:
            *params = 0; // TODO
            break;
        case GL_SMOOTH_LINE_WIDTH_GRANULARITY:
            *params = 0; // TODO
            break;
        case GL_STENCIL_BACK_FAIL:
            *params = 0; // TODO
            break;
        case GL_STENCIL_BACK_FUNC:
            *params = 0; // TODO
            break;
        case GL_STENCIL_BACK_PASS_DEPTH_FAIL:
            *params = 0; // TODO
            break;
        case GL_STENCIL_BACK_PASS_DEPTH_PASS:
            *params = 0; // TODO
            break;
        case GL_STENCIL_BACK_REF:
            *params = 0; // TODO
            break;
        case GL_STENCIL_BACK_VALUE_MASK:
            *params = 0; // TODO
            break;
        case GL_STENCIL_BACK_WRITEMASK:
            *params = 0; // TODO
            break;
        case GL_STENCIL_CLEAR_VALUE:
            *params = 0; // TODO
            break;
        case GL_STENCIL_FAIL:
            *params = 0; // TODO
            break;
        case GL_STENCIL_FUNC:
            *params = 0; // TODO
            break;
        case GL_STENCIL_PASS_DEPTH_FAIL:
            *params = 0; // TODO
            break;
        case GL_STENCIL_PASS_DEPTH_PASS:
            *params = 0; // TODO
            break;
        case GL_STENCIL_REF:
            *params = 0; // TODO
            break;
        case GL_STENCIL_TEST:
            *params = MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::StencilTest) ? GL_TRUE : GL_FALSE;
            break;
        case GL_STENCIL_VALUE_MASK:
            *params = 0; // TODO
            break;
        case GL_STENCIL_WRITEMASK:
            *params = 0; // TODO
            break;
        case GL_STEREO:
            *params = 0; // TODO
            break;
        case GL_SUBPIXEL_BITS:
            *params = 0; // TODO
            break;
        case GL_TEXTURE_BINDING_1D:
            *params = 0; // TODO
            break;
        case GL_TEXTURE_BINDING_1D_ARRAY:
            *params = 0; // TODO
            break;
        case GL_TEXTURE_BINDING_2D: {
            Int unit = MG_State::pGLContext->GetActiveTextureUnit();
            auto& tu = MG_State::pGLContext->GetTextureUnitObject(unit);
            const auto& slot = tu.GetBindingSlot(TextureTarget::Texture2D);
            const auto& obj = slot.GetBoundObject();
            *params = obj ? static_cast<GLint>(obj->GetExternalIndex()) : 0;
            MGLOG_D("Get GL_TEXTURE_BINDING_2D: %d", *params);
            break;
        }
        case GL_TEXTURE_BINDING_2D_ARRAY:
            *params = 0; // TODO
            break;
        case GL_TEXTURE_BINDING_2D_MULTISAMPLE:
            *params = 0; // TODO
            break;
        case GL_TEXTURE_BINDING_2D_MULTISAMPLE_ARRAY:
            *params = 0; // TODO
            break;
        case GL_TEXTURE_BINDING_3D: {
            Int unit = MG_State::pGLContext->GetActiveTextureUnit();
            auto& tu = MG_State::pGLContext->GetTextureUnitObject(unit);
            const auto& slot = tu.GetBindingSlot(TextureTarget::Texture3D);
            const auto& obj = slot.GetBoundObject();
            *params = obj ? static_cast<GLint>(obj->GetExternalIndex()) : 0;
            break;
        }
        case GL_TEXTURE_BINDING_BUFFER:
            *params = 0; // TODO
            break;
        case GL_TEXTURE_BINDING_CUBE_MAP: {
            Int unit = MG_State::pGLContext->GetActiveTextureUnit();
            auto& tu = MG_State::pGLContext->GetTextureUnitObject(unit);
            const auto& slot = tu.GetBindingSlot(TextureTarget::TextureCubeMap);
            const auto& obj = slot.GetBoundObject();
            *params = obj ? static_cast<GLint>(obj->GetExternalIndex()) : 0;
            break;
        }
        case GL_TEXTURE_BINDING_RECTANGLE:
            *params = 0; // TODO
            break;
        case GL_TEXTURE_COMPRESSION_HINT:
            *params = 0; // TODO
            break;
        case GL_TEXTURE_BUFFER_OFFSET_ALIGNMENT:
            *params = 0; // TODO
            break;
        case GL_TIMESTAMP:
            *params = 0; // TODO
            break;
        case GL_TRANSFORM_FEEDBACK_BUFFER_BINDING:
            *params = 0; // TODO
            break;
        case GL_TRANSFORM_FEEDBACK_BUFFER_START:
            *params = 0; // TODO
            break;
        case GL_TRANSFORM_FEEDBACK_BUFFER_SIZE:
            *params = 0; // TODO
            break;
        case GL_UNIFORM_BUFFER_BINDING:
            *params = 0; // TODO
            break;
        case GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT:
            *params = (Int)activeBackendObject->GetDynamicParameters().UniformBufferOffsetAlignment;
            break;
        case GL_UNIFORM_BUFFER_SIZE:
            *params = 0; // TODO
            break;
        case GL_UNIFORM_BUFFER_START:
            *params = 0; // TODO
            break;
        case GL_UNPACK_ALIGNMENT:
            *params = MG_State::pGLContext->GetPixelStoreParam(PixelStoreParam::UnpackAlignment);
            break;
        case GL_UNPACK_IMAGE_HEIGHT:
            *params = MG_State::pGLContext->GetPixelStoreParam(PixelStoreParam::UnpackImageHeight);
            break;
        case GL_UNPACK_LSB_FIRST:
            *params = MG_State::pGLContext->GetPixelStoreParam(PixelStoreParam::UnpackLSBFirst);
            break;
        case GL_UNPACK_ROW_LENGTH:
            *params = MG_State::pGLContext->GetPixelStoreParam(PixelStoreParam::UnpackRowLength);
            break;
        case GL_UNPACK_SKIP_IMAGES:
            *params = MG_State::pGLContext->GetPixelStoreParam(PixelStoreParam::UnpackSkipImages);
            break;
        case GL_UNPACK_SKIP_PIXELS:
            *params = MG_State::pGLContext->GetPixelStoreParam(PixelStoreParam::UnpackSkipPixels);
            break;
        case GL_UNPACK_SKIP_ROWS:
            *params = MG_State::pGLContext->GetPixelStoreParam(PixelStoreParam::UnpackSkipRows);
            break;
        case GL_UNPACK_SWAP_BYTES:
            *params = MG_State::pGLContext->GetPixelStoreParam(PixelStoreParam::UnpackSwapBytes);
            break;
        case GL_VERTEX_ARRAY_BINDING: {
            const auto& vao = MG_State::pGLContext->GetBoundVertexArray();
            *params = vao ? static_cast<GLint>(vao->GetExternalIndex()) : 0;
            break;
        }
        case GL_VERTEX_BINDING_DIVISOR:
            *params = 0; // TODO
            break;
        case GL_VERTEX_BINDING_OFFSET:
            *params = 0; // TODO
            break;
        case GL_VERTEX_BINDING_STRIDE:
            *params = 0; // TODO
            break;
        case GL_MAX_VERTEX_ATTRIB_RELATIVE_OFFSET:
            *params = 0; // TODO
            break;
        case GL_MAX_VERTEX_ATTRIB_BINDINGS:
            *params = 0; // TODO
            break;
        case GL_VIEWPORT: {
            const auto& vp = MG_State::pGLContext->GetViewport();
            params[0] = vp.x();
            params[1] = vp.y();
            params[2] = vp.z();
            params[3] = vp.w();
            break;
        }
        case GL_VIEWPORT_BOUNDS_RANGE:
            *params = 0; // TODO
            break;
        case GL_VIEWPORT_INDEX_PROVOKING_VERTEX:
            *params = 0; // TODO
            break;
        case GL_VIEWPORT_SUBPIXEL_BITS:
            *params = 0; // TODO
            break;
        case GL_MAX_ELEMENT_INDEX:
            *params = 1024 * 1024; // TODO
            break;
        case GL_CONTEXT_PROFILE_MASK:
            *params = GL_CONTEXT_CORE_PROFILE_BIT;
            break;
        case GL_MAX_COLOR_ATTACHMENTS:
        case GL_MAX_DRAW_BUFFERS:
            *params = MG_State::GLState::FramebufferObject::MAX_DRAW_BUFFERS; // TODO: use backend value
            break;
        case GL_MAX_SAMPLES:
            *params = 16; // TODO
            break;
        default:
            MGLOG_E("glGetIntegerv: Invalid enum %s (0x%X)", MG_Util::ConvertGLEnumToString(pname).c_str(), pname);
            MG_State::pGLContext->RecordError(ErrorCode::InvalidEnum,
                                              MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", "GetIntegerv",
                                                                           std::format("Invalid enum: 0x{:X}", pname)));

            break;
        }
    }

    GLenum GetError() {
        ErrorCode errorCode = MG_State::pGLContext->PopGLError().value_or(Error{ErrorCode::NoError, nullptr}).code;
        return MG_Util::ConvertErrorCodeToGLEnum(errorCode);
    }
} // namespace MobileGL::MG_Impl::GLImpl
