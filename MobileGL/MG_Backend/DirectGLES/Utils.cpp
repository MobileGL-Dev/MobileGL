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
#include "MG_Backend/BackendObjects.h"
#include "MG_Util/Converters/GLToMG/FramebufferEnumConverter.h"
#include "MG_Util/Texture/TextureFormatProcessor.h"

#include <MG_State/GLState/Core.h>
#include <MG_Util/BackendLoaders/OpenGL/Loader.h>
#include <MG_Util/Converters/GLToStr/GLEnumConverter.h>
#include <MG_Util/Converters/MGToGL/TextureEnumConverter.h>
#include <MG_Util/Converters/MGToGL/FramebufferEnumConverter.h>

namespace MobileGL::MG_Backend::DirectGLES {
    namespace {
        Flags<PixelFormatNormalizeOptionBit> GetForcedPixelFormatNormalizeOptions() {
            Flags<PixelFormatNormalizeOptionBit> options;
            if (g_GLESCapabilities.GLESRendererString.find("ANGLE") != String::npos) {
                options |= PixelFormatNormalizeOptionBit::NoRgb16;
                options |= PixelFormatNormalizeOptionBit::NoSnorm16;
                options |= PixelFormatNormalizeOptionBit::NoSnorm8;
            }
            return options;
        }

        Flags<PixelFormatNormalizeOptionBit> GetDriverPixelFormatNormalizeOptions() {
            Flags<PixelFormatNormalizeOptionBit> options = PixelFormatNormalizeOptionBit::NoDepthComponent32;
            options |= PixelFormatNormalizeOptionBit::NoRGBA8Snorm;
            options |= PixelFormatNormalizeOptionBit::NoRGB16Snorm;
            if (!g_GLESCapabilities.SupportsNorm16Texture) {
                options |= PixelFormatNormalizeOptionBit::NoNorm16;
            }
            return options;
        }

        Flags<PixelFormatNormalizeOptionBit> GetRuntimeFallbackNormalizeOptions(GLenum requestedInternalFormat) {
            using namespace MG_Util::TextureFormatProcessor;
            const Flags<PixelFormatNormalizeOptionBit> forcedOptions =
                GetApplicablePixelFormatNormalizeOptions(requestedInternalFormat, GetForcedPixelFormatNormalizeOptions());
            if (forcedOptions) {
                return forcedOptions;
            }
            return GetApplicablePixelFormatNormalizeOptions(requestedInternalFormat,
                                                            GetDriverPixelFormatNormalizeOptions());
        }

        Bool HasCachedFormatCapability(TextureInternalFormat internalFormat,
                                       SizeT targetIndex,
                                       Bool caveat,
                                       FormatCapability capability) {
            if (!pActiveBackendObject || targetIndex >= kFormatCapabilityTargetCount) {
                return false;
            }
            const SizeT formatIndex = static_cast<SizeT>(internalFormat);
            if (formatIndex >= kFormatCapabilityFormatCount) {
                return false;
            }

            const FormatCapabilityCache& cache = pActiveBackendObject->GetFormatCapabilities();
            const FormatCapabilityFlags caps =
                caveat ? cache.CaveatCaps[targetIndex][formatIndex] : cache.FullCaps[targetIndex][formatIndex];
            return HasFormatCapability(caps, capability);
        }

        Bool HasAnyCachedFormatCapability(TextureInternalFormat internalFormat,
                                          Bool caveat,
                                          FormatCapability capability) {
            for (SizeT targetIndex = 0; targetIndex < kFormatCapabilityTargetCount; ++targetIndex) {
                if (HasCachedFormatCapability(internalFormat, targetIndex, caveat, capability)) {
                    return true;
                }
            }
            return false;
        }

        Bool ShouldUseCaveatFormat(TextureInternalFormat internalFormat, SizeT targetIndex) {
            if (targetIndex < kFormatCapabilityTargetCount) {
                const Bool fullCreatable =
                    HasCachedFormatCapability(internalFormat, targetIndex, false, FormatCapability::Creatable);
                const Bool caveatCreatable =
                    HasCachedFormatCapability(internalFormat, targetIndex, true, FormatCapability::Creatable);
                const Bool fullRenderable =
                    HasCachedFormatCapability(internalFormat, targetIndex, false, FormatCapability::FramebufferRenderable);
                const Bool caveatRenderable =
                    HasCachedFormatCapability(internalFormat, targetIndex, true, FormatCapability::FramebufferRenderable);
                return (!fullCreatable && caveatCreatable) || (!fullRenderable && caveatRenderable);
            }

            if (HasAnyCachedFormatCapability(internalFormat, false, FormatCapability::Creatable)) {
                return false;
            }
            return HasAnyCachedFormatCapability(internalFormat, true, FormatCapability::Creatable);
        }

        void GenerateFormatInfo(TextureInternalFormat internalFormat,
                                SizeT targetIndex,
                                GLenum* outInternalFormat,
                                GLenum* outFormat,
                                GLenum* outType) {
            using namespace MobileGL::MG_Util::TextureFormatProcessor;
            const GLenum requestedInternalFormat = MG_Util::ConvertTextureInternalFormatToGLEnum(internalFormat);
            Flags<PixelFormatNormalizeOptionBit> options;
            if (!pActiveBackendObject || ShouldUseCaveatFormat(internalFormat, targetIndex)) {
                options = GetRuntimeFallbackNormalizeOptions(requestedInternalFormat);
            }
            NormalizePixelFormat(requestedInternalFormat, options, outInternalFormat, outFormat, outType);
        }
    } // namespace

    namespace TextureImpl {
        void GenerateTextureFormatInfo(TextureInternalFormat internalFormat, GLenum* outInternalFormat,
                                       GLenum* outFormat, GLenum* outType, TextureTarget target) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            const SizeT targetIndex =
                target == TextureTarget::Unknown ? kFormatCapabilityTargetCount : GetFormatCapabilityTargetIndex(target);
            GenerateFormatInfo(internalFormat, targetIndex, outInternalFormat, outFormat, outType);
        }

        void GenerateRenderbufferFormatInfo(TextureInternalFormat internalFormat, GLenum* outInternalFormat,
                                            GLenum* outFormat, GLenum* outType) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            GenerateFormatInfo(internalFormat, GetRenderbufferFormatCapabilityTargetIndex(), outInternalFormat,
                               outFormat, outType);
        }

        Bool ShouldUseCaveatTextureFormat(TextureInternalFormat internalFormat, TextureTarget target) {
            const SizeT targetIndex =
                target == TextureTarget::Unknown ? kFormatCapabilityTargetCount : GetFormatCapabilityTargetIndex(target);
            return ShouldUseCaveatFormat(internalFormat, targetIndex);
        }

        Bool ShouldUseCaveatRenderbufferFormat(TextureInternalFormat internalFormat) {
            return ShouldUseCaveatFormat(internalFormat, GetRenderbufferFormatCapabilityTargetIndex());
        }
    } // namespace TextureImpl
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

        String ClampSnormFallbackOutputs(String glslCode, GLenum shaderType, Uint32 outputMask) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            if (shaderType != GL_FRAGMENT_SHADER || outputMask == 0) {
                return glslCode;
            }

            const std::regex outputPattern(
                R"(layout\s*\(\s*location\s*=\s*([0-9]+)\s*\)\s*out\s+(?:(?:lowp|mediump|highp)\s+)?vec4\s+([A-Za-z_][A-Za-z0-9_]*)\s*;)");
            std::sregex_iterator outputIt(glslCode.begin(), glslCode.end(), outputPattern);
            std::sregex_iterator outputEnd;
            Vector<String> outputNames;
            for (; outputIt != outputEnd; ++outputIt) {
                const Uint location = static_cast<Uint>(std::stoul((*outputIt)[1].str()));
                if (location < 32 && (outputMask & (1u << location))) {
                    outputNames.push_back((*outputIt)[2].str());
                }
            }
            if (outputNames.empty()) {
                return glslCode;
            }

            const std::regex mainPattern(R"(void\s+main\s*\([^)]*\)\s*\{)");
            std::smatch mainMatch;
            if (!std::regex_search(glslCode, mainMatch, mainPattern)) {
                return glslCode;
            }

            SizeT bracePos = static_cast<SizeT>(mainMatch.position(0) + mainMatch.length(0) - 1);
            Int depth = 0;
            for (SizeT pos = bracePos; pos < glslCode.size(); ++pos) {
                if (glslCode[pos] == '{') {
                    ++depth;
                } else if (glslCode[pos] == '}') {
                    --depth;
                    if (depth == 0) {
                        String clampLine;
                        for (const String& outputName : outputNames) {
                            clampLine += "\n    " + outputName + " = clamp(" + outputName +
                                         ", vec4(-1.0), vec4(1.0));";
                        }
                        clampLine += "\n";
                        glslCode.insert(pos, clampLine);
                        return glslCode;
                    }
                }
            }
            return glslCode;
        }

        String ForceFlatIntegerVaryings(const String& glslCode, GLenum shaderType) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            String result = glslCode;
            const String integerType = R"((?:(?:lowp|mediump|highp)\s+)?(?:u?int|[iu]vec[234])\b)";

            auto addFlatQualifier = [&result, &integerType](const String& qualifier) {
                const std::regex pattern("(layout\\s*\\([^)]*\\)\\s*)(?!(?:flat|smooth|noperspective)\\s)(" +
                                         qualifier + "\\s+" + integerType + ")");
                result = std::regex_replace(result, pattern, "$1flat $2");
            };

            switch (shaderType) {
            case GL_VERTEX_SHADER:
                addFlatQualifier("out");
                break;
            case GL_GEOMETRY_SHADER:
                addFlatQualifier("in");
                addFlatQualifier("out");
                break;
            case GL_FRAGMENT_SHADER:
                addFlatQualifier("in");
                break;
            default:
                break;
            }

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
            for (GLenum err = g_GLESFuncs.glGetError(); err != GL_NO_ERROR; err = g_GLESFuncs.glGetError()) {
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
            case GL_DRAW_FRAMEBUFFER:
                return GL_DRAW_FRAMEBUFFER_BINDING;
            case GL_READ_FRAMEBUFFER:
                return GL_READ_FRAMEBUFFER_BINDING;

            case GL_RENDERBUFFER:
                return GL_RENDERBUFFER_BINDING;

            case GL_VERTEX_ARRAY:
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
