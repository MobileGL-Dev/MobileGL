// MobileGL - MobileGL/MG_Backend/DirectGLES/Utils.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "DirectGLES.h"
#include "Utils.h"
#include "Managers.h"
#include "MG_Util/Converters/GLToMG/FramebufferEnumConverter.h"
#include "MG_Util/Texture/TextureFormatProcessor.h"

#include <MG_State/GLState/Core.h>
#include <MG_Util/BackendLoaders/OpenGL/Loader.h>
#include <MG_Util/Converters/GLToStr/GLEnumConverter.h>
#include <MG_Util/Converters/MGToGL/TextureEnumConverter.h>
#include <MG_Util/Converters/MGToGL/FramebufferEnumConverter.h>

namespace MobileGL::MG_Backend::DirectGLES {
    namespace BufferImpl {} // namespace BufferImpl

    namespace VertexArrayImpl {} // namespace VertexArrayImpl

    namespace TextureImpl {
        void GenerateTextureFormatInfo(TextureInternalFormat internalFormat, GLenum* outInternalFormat,
                                       GLenum* outFormat, GLenum* outType) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            using namespace MobileGL::MG_Util::TextureFormatProcessor;
            auto options = (g_GLESCapabilities.SupportsNorm16Texture) ? PixelFormatNormalizeOptionBit::None
                                                                      : PixelFormatNormalizeOptionBit::NoNorm16;
            NormalizePixelFormat(MG_Util::ConvertTextureInternalFormatToGLEnum(internalFormat), options,
                                 outInternalFormat, outFormat, outType);
        }
    } // namespace TextureImpl

    namespace FramebufferImpl {} // namespace FramebufferImpl

    namespace PrgramImpl {
        String ProcessOutColorLocations(const String& glslCode) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            const static std::regex pattern(R"(\n(out highp vec4 outColor)(\d+);)");
            const String replacement = "\nlayout(location=$2) $1$2;";
            return std::regex_replace(glslCode, pattern, replacement);
        }

        String ForceSupporterOutput(const String& glslCode) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            Bool hasPrecisionFloat =
                glslCode.find("precision ") != String::npos && glslCode.find("float;") != String::npos;
            Bool hasPrecisionInt = glslCode.find("precision ") != String::npos && glslCode.find("int;") != String::npos;

            String result = glslCode;
            String precisionFloat;
            String precisionInt;

            if (hasPrecisionFloat && hasPrecisionInt) {
                std::istringstream iss(result);
                std::vector<String> lines;
                String line;
                while (std::getline(iss, line)) {
                    Bool isPrecisionLine = (line.find("precision ") != String::npos) &&
                                           (line.find("float;") != String::npos || line.find("int;") != String::npos);
                    if (!isPrecisionLine) {
                        lines.push_back(line);
                    }
                }
                result.clear();
                for (SizeT i = 0; i < lines.size(); ++i) {
                    if (i != 0) result += '\n';
                    result += lines[i];
                }
                precisionFloat = "precision highp float;\n";
                precisionInt = "precision highp int;\n";
            } else {
                precisionFloat = hasPrecisionFloat ? "" : "precision highp float;\n";
                precisionInt = hasPrecisionInt ? "" : "precision highp int;\n";
            }

            SizeT lastExtensionPos = result.rfind("#extension");
            SizeT insertionPos = 0;

            if (lastExtensionPos != String::npos) {
                SizeT nextNewline = result.find('\n', lastExtensionPos);
                if (nextNewline != String::npos) {
                    insertionPos = nextNewline + 1;
                } else {
                    insertionPos = result.length();
                }
            } else {
                SizeT firstNewline = result.find('\n');
                if (firstNewline != String::npos) {
                    insertionPos = firstNewline + 1;
                } else {
                    result = precisionFloat + precisionInt + result;
                    return result;
                }
            }

            result.insert(insertionPos, precisionFloat + precisionInt);
            return result;
        }

        String RemoveLayoutBinding(const String& glslCode) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            static std::regex bindingRegex(R"(layout\s*\(\s*binding\s*=\s*\d+\s*\)\s*)");
            String result = std::regex_replace(glslCode, bindingRegex, "");
            static std::regex bindingRegex2(R"(layout\s*\(\s*binding\s*=\s*\d+\s*,)");
            result = std::regex_replace(result, bindingRegex2, "layout(");
            return result;
        }
    } // namespace PrgramImpl

    namespace Utils {
        void CheckGLESError() {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            while (GLenum err = g_GLESFuncs.glGetError() != GL_NO_ERROR) {
                MGLOG_E("-> GLES Error: %s", MG_Util::ConvertGLEnumToString(err).c_str());
            }
        }

        GLenum GetBindingQuery(GLenum target, bool isTexture) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            switch (target) {
            case GL_TEXTURE_BUFFER:
                return isTexture ? GL_TEXTURE_BINDING_BUFFER : GL_TEXTURE_BUFFER_BINDING;

            case GL_ARRAY_BUFFER:
                return GL_ARRAY_BUFFER_BINDING;
            case GL_ATOMIC_COUNTER_BUFFER:
                return GL_ATOMIC_COUNTER_BUFFER_BINDING;
            case GL_COPY_READ_BUFFER:
                return GL_COPY_READ_BUFFER_BINDING;
            case GL_COPY_WRITE_BUFFER:
                return GL_COPY_WRITE_BUFFER_BINDING;
            case GL_DISPATCH_INDIRECT_BUFFER:
                return GL_DISPATCH_INDIRECT_BUFFER_BINDING;
            case GL_DRAW_INDIRECT_BUFFER:
                return GL_DRAW_INDIRECT_BUFFER_BINDING;
            case GL_ELEMENT_ARRAY_BUFFER:
                return GL_ELEMENT_ARRAY_BUFFER_BINDING;
            case GL_PIXEL_PACK_BUFFER:
                return GL_PIXEL_PACK_BUFFER_BINDING;
            case GL_PIXEL_UNPACK_BUFFER:
                return GL_PIXEL_UNPACK_BUFFER_BINDING;
            case GL_QUERY_BUFFER:
                return GL_QUERY_BUFFER_BINDING;
            case GL_SHADER_STORAGE_BUFFER:
                return GL_SHADER_STORAGE_BUFFER_BINDING;
            case GL_TRANSFORM_FEEDBACK_BUFFER:
                return GL_TRANSFORM_FEEDBACK_BUFFER_BINDING;
            case GL_UNIFORM_BUFFER:
                return GL_UNIFORM_BUFFER_BINDING;

            case GL_FRAMEBUFFER:
                return GL_FRAMEBUFFER_BINDING;
            case GL_DRAW_FRAMEBUFFER:
                return GL_DRAW_FRAMEBUFFER_BINDING;
            case GL_READ_FRAMEBUFFER:
                return GL_READ_FRAMEBUFFER_BINDING;

            case GL_RENDERBUFFER:
                return GL_RENDERBUFFER_BINDING;

            case GL_VERTEX_ARRAY:
                return GL_VERTEX_ARRAY_BINDING;
            case GL_VERTEX_ARRAY_BINDING:
                return GL_VERTEX_ARRAY_BINDING;

            case GL_PROGRAM_PIPELINE:
                return GL_PROGRAM_PIPELINE_BINDING;

            case GL_PROGRAM:
                return GL_CURRENT_PROGRAM;

            case GL_SAMPLER:
                return GL_SAMPLER_BINDING;

            case GL_TEXTURE:
                return GL_TEXTURE_BINDING_2D;
            case GL_TEXTURE_1D:
                return GL_TEXTURE_BINDING_1D;
            case GL_TEXTURE_1D_ARRAY:
                return GL_TEXTURE_BINDING_1D_ARRAY;
            case GL_TEXTURE_2D:
                return GL_TEXTURE_BINDING_2D;
            case GL_TEXTURE_2D_ARRAY:
                return GL_TEXTURE_BINDING_2D_ARRAY;
            case GL_TEXTURE_2D_MULTISAMPLE:
                return GL_TEXTURE_BINDING_2D_MULTISAMPLE;
            case GL_TEXTURE_2D_MULTISAMPLE_ARRAY:
                return GL_TEXTURE_BINDING_2D_MULTISAMPLE_ARRAY;
            case GL_TEXTURE_3D:
                return GL_TEXTURE_BINDING_3D;
            case GL_TEXTURE_CUBE_MAP:
                return GL_TEXTURE_BINDING_CUBE_MAP;
            case GL_TEXTURE_CUBE_MAP_ARRAY:
                return GL_TEXTURE_BINDING_CUBE_MAP_ARRAY;
            case GL_TEXTURE_RECTANGLE:
                return GL_TEXTURE_BINDING_RECTANGLE;

            case GL_TRANSFORM_FEEDBACK:
                return GL_TRANSFORM_FEEDBACK_BINDING;

            case GL_SAMPLES_PASSED:
                return GL_SAMPLES_PASSED;
            case GL_PRIMITIVES_GENERATED:
                return GL_PRIMITIVES_GENERATED;

            case GL_DEBUG_OUTPUT:
                return GL_DEBUG_OUTPUT;
            case GL_DEBUG_OUTPUT_SYNCHRONOUS:
                return GL_DEBUG_OUTPUT_SYNCHRONOUS;

            default:
                return 0;
            }
        }
    } // namespace Utils
} // namespace MobileGL::MG_Backend::DirectGLES
