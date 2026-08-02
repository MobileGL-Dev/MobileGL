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
#include <MG_Util/Math/HalfFloat.h>
#include <MG_Util/Math/SmallFloat.h>

#include <cmath>
#include <cctype>
#include <cstring>
#include <regex>

namespace MobileGL::MG_Backend::DirectGLES {
    namespace {
        Flags<PixelFormatNormalizeOptionBit> GetForcedPixelFormatNormalizeOptions() {
            Flags<PixelFormatNormalizeOptionBit> options;
            if (g_GLESCapabilities.IsAngleRenderer) {
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

        Flags<PixelFormatNormalizeOptionBit>
        GetRuntimeFallbackNormalizeOptions(GLenum requestedInternalFormat,
                                           Flags<PixelFormatNormalizeOptionBit> extraOptions) {
            using namespace MG_Util::TextureFormatProcessor;
            const Flags<PixelFormatNormalizeOptionBit> forcedOptions = GetApplicablePixelFormatNormalizeOptions(
                requestedInternalFormat, GetForcedPixelFormatNormalizeOptions() | extraOptions);
            if (forcedOptions) {
                return forcedOptions;
            }
            return GetApplicablePixelFormatNormalizeOptions(
                requestedInternalFormat, GetDriverPixelFormatNormalizeOptions() | extraOptions);
        }

        // Multisample textures can only ever be rendered into, never uploaded to, so a fallback
        // format for them has to stay colour-renderable - a three-channel float fallback is a legal
        // ES texture format but not a legal multisample storage format. Widening to four channels
        // is safe here precisely because there is no transfer path that would have to expand
        // three-channel client data, and the alpha the draw writes for a three-channel source is
        // already the 1.0 the frontend format implies.
        Bool TargetRequiresRenderableFormat(SizeT targetIndex) {
            return targetIndex == static_cast<SizeT>(TextureTarget::Texture2DMultisample) ||
                   targetIndex == static_cast<SizeT>(TextureTarget::Texture2DMultisampleArray);
        }

        Flags<PixelFormatNormalizeOptionBit> GetRenderTargetNormalizeOptions(SizeT targetIndex) {
            Flags<PixelFormatNormalizeOptionBit> options;
            if (!TargetRequiresRenderableFormat(targetIndex)) {
                return options;
            }
            options |= PixelFormatNormalizeOptionBit::NoThreeChannelRenderTarget;
            if (!g_GLESCapabilities.SupportsRenderSnorm || !g_GLESCapabilities.SupportsNorm16Texture) {
                options |= PixelFormatNormalizeOptionBit::NoSnorm16RenderTarget;
            }
            return options;
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
                options = GetRuntimeFallbackNormalizeOptions(requestedInternalFormat,
                                                             GetRenderTargetNormalizeOptions(targetIndex));
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

        Bool BackendTextureFormatAddsAlpha(TextureInternalFormat internalFormat, TextureTarget target) {
            const SizeT targetIndex =
                target == TextureTarget::Unknown ? kFormatCapabilityTargetCount : GetFormatCapabilityTargetIndex(target);
            if (!TargetRequiresRenderableFormat(targetIndex)) {
                return false;
            }
            if (pActiveBackendObject && !ShouldUseCaveatFormat(internalFormat, targetIndex)) {
                return false;
            }
            const GLenum requestedInternalFormat = MG_Util::ConvertTextureInternalFormatToGLEnum(internalFormat);
            const Flags<PixelFormatNormalizeOptionBit> options =
                GetRuntimeFallbackNormalizeOptions(requestedInternalFormat,
                                                   GetRenderTargetNormalizeOptions(targetIndex));
            return static_cast<Bool>(options & PixelFormatNormalizeOptionBit::NoThreeChannelRenderTarget);
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

        String ClampNormFallbackOutputs(String glslCode, GLenum shaderType, Uint32 snormOutputMask,
                                        Uint32 unormOutputMask) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            const Uint32 outputMask = snormOutputMask | unormOutputMask;
            if (shaderType != GL_FRAGMENT_SHADER || outputMask == 0) {
                return glslCode;
            }

            const std::regex outputPattern(
                R"(layout\s*\(\s*location\s*=\s*([0-9]+)\s*\)\s*out\s+(?:(?:lowp|mediump|highp)\s+)?vec4\s+([A-Za-z_][A-Za-z0-9_]*)\s*;)");
            std::sregex_iterator outputIt(glslCode.begin(), glslCode.end(), outputPattern);
            std::sregex_iterator outputEnd;
            struct OutputClamp {
                String Name;
                Bool Signed;
            };
            Vector<OutputClamp> outputClamps;
            for (; outputIt != outputEnd; ++outputIt) {
                const Uint location = static_cast<Uint>(std::stoul((*outputIt)[1].str()));
                if (location < 32 && (outputMask & (1u << location))) {
                    outputClamps.push_back({(*outputIt)[2].str(), static_cast<Bool>(snormOutputMask & (1u << location))});
                }
            }
            if (outputClamps.empty()) {
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
                        for (const OutputClamp& outputClamp : outputClamps) {
                            const String minValue = outputClamp.Signed ? "-1.0" : "0.0";
                            clampLine += "\n    " + outputClamp.Name + " = clamp(" + outputClamp.Name +
                                         ", vec4(" + minValue + "), vec4(1.0));";
                        }
                        clampLine += "\n";
                        glslCode.insert(pos, clampLine);
                        return glslCode;
                    }
                }
            }
            return glslCode;
        }

        String BroadcastLegacyFragColor(String glslCode, GLenum shaderType, Uint drawBufferCount) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            // The name is the marker: ShaderSourceProcessor only emits it when the source
            // wrote gl_FragColor, and such a shader can have no other output.
            static const char* const kLoweredName = "mg_FragColor";
            if (shaderType != GL_FRAGMENT_SHADER || drawBufferCount <= 1) {
                return glslCode;
            }
            static const std::regex declRegex(
                R"(layout\s*\(\s*location\s*=\s*0\s*\)\s*out\s+((?:lowp|mediump|highp)\s+)?vec4\s+mg_FragColor\s*;)");
            std::smatch declMatch;
            if (!std::regex_search(glslCode, declMatch, declRegex)) {
                return glslCode;
            }
            const String precision = declMatch[1].matched ? declMatch[1].str() : String();

            String replicaDecls;
            String replicaCopies;
            for (Uint location = 1; location < drawBufferCount; ++location) {
                const String name = String(kLoweredName) + "_" + std::to_string(location);
                replicaDecls += "\nlayout(location = " + std::to_string(location) + ") out " + precision + "vec4 " +
                                name + ";";
                replicaCopies += "\n    " + name + " = " + kLoweredName + ";";
            }

            static const std::regex mainRegex(R"(void\s+main\s*\([^)]*\)\s*\{)");
            std::smatch mainMatch;
            if (!std::regex_search(glslCode, mainMatch, mainRegex)) {
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
                        glslCode.insert(pos, replicaCopies + "\n");
                        break;
                    }
                }
            }
            glslCode.insert(static_cast<SizeT>(declMatch.position(0)) + declMatch[0].str().size(), replicaDecls);
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
            // Sampler and uniform-block bindings are re-established at draw time through the
            // API, so their layout qualifiers are stripped (they may exceed ES limits). SSBO
            // blocks and image uniforms are different: ES has no glShaderStorageBlockBinding,
            // and image units cannot be set with glUniform1i, so for those declarations the
            // binding qualifier is the only binding mechanism and must be preserved.
            static std::regex bindingRegex(R"(layout\s*\(\s*binding\s*=\s*\d+\s*\)\s*)");
            static std::regex bindingRegex2(R"(layout\s*\(\s*binding\s*=\s*\d+\s*,)");
            static std::regex keepBindingRegex(R"(\b(buffer|[iu]?image[A-Za-z0-9]*)\b)");

            String result;
            result.reserve(glslCode.size());
            SizeT lineStart = 0;
            while (lineStart <= glslCode.size()) {
                SizeT lineEnd = glslCode.find('\n', lineStart);
                const Bool lastLine = lineEnd == String::npos;
                String line = glslCode.substr(lineStart, lastLine ? String::npos : lineEnd - lineStart);

                if (!std::regex_search(line, keepBindingRegex)) {
                    line = std::regex_replace(line, bindingRegex, "");
                    line = std::regex_replace(line, bindingRegex2, "layout(");
                }

                result += line;
                if (lastLine) {
                    break;
                }
                result += '\n';
                lineStart = lineEnd + 1;
            }
            return result;
        }

        namespace {
            // How a lookup carries its level of detail, and how many arguments it takes
            // before the optional bias.
            struct LodLookupForm {
                const char* name;
                Int requiredArgs; // arguments before the optional bias (implicit form)
                Int explicitLodArg; // index of the explicit LOD argument, -1 for implicit
            };

            // texelFetch* is deliberately absent: an integer fetch names its level directly
            // and takes no LOD bias. textureGather has no bias either. textureGrad* derives
            // the LOD from gradients and offers no argument to fold a bias into, so it is
            // left alone rather than rewritten incorrectly.
            constexpr LodLookupForm LOD_LOOKUP_FORMS[] = {
                {"textureProjLodOffset", 0, 2}, {"textureProjOffset", 4, -1}, {"textureProjLod", 0, 2},
                {"textureLodOffset", 0, 2},     {"textureOffset", 3, -1},     {"textureProj", 2, -1},
                {"textureLod", 0, 2},           {"texture", 2, -1},
            };

            // Sampler types with no mip chain, or whose GLSL lookups have no bias overload
            // at all (the array-shadow forms), so nothing can or should be folded in.
            Bool IsBiasableSamplerType(const String& samplerType) {
                if (samplerType.find("MS") != String::npos) return false;      // multisample
                if (samplerType.find("Buffer") != String::npos) return false;  // texture buffer
                if (samplerType.find("Rect") != String::npos) return false;    // rectangle: no mips
                if (samplerType == "sampler2DArrayShadow") return false;
                if (samplerType == "samplerCubeArrayShadow") return false;
                return true;
            }

            Bool IsIdentifierChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

            // Byte offsets of the top-level argument separators and of the closing paren,
            // starting from the '(' at openParen. Empty when the parentheses do not balance.
            Vector<SizeT> SplitCallArguments(const String& code, SizeT openParen) {
                Vector<SizeT> marks;
                Int depth = 0;
                for (SizeT i = openParen; i < code.size(); ++i) {
                    const char c = code[i];
                    if (c == '(' || c == '[') {
                        ++depth;
                    } else if (c == ']') {
                        --depth;
                    } else if (c == ')') {
                        --depth;
                        if (depth == 0) {
                            marks.push_back(i);
                            return marks;
                        }
                    } else if (c == ',' && depth == 1) {
                        marks.push_back(i);
                    }
                }
                return {};
            }
        } // namespace

        String EmulateTextureLodBias(const String& glslCode) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            if (glslCode.find("sampler") == String::npos || glslCode.find("texture") == String::npos) {
                return glslCode;
            }

            // Collect the mip-capable sampler uniforms this shader declares.
            static const std::regex samplerDeclRegex(
                R"(uniform\s+(?:(?:highp|mediump|lowp)\s+)?([iu]?sampler[A-Za-z0-9]*)\s+([A-Za-z_][A-Za-z0-9_]*)\s*;)");
            UnorderedMap<String, String> samplerNames; // name -> bias uniform name
            for (std::sregex_iterator it(glslCode.begin(), glslCode.end(), samplerDeclRegex), end; it != end; ++it) {
                const String samplerType = (*it)[1].str();
                if (!IsBiasableSamplerType(samplerType)) continue;
                const String name = (*it)[2].str();
                samplerNames.emplace(name, String(LOD_BIAS_UNIFORM_PREFIX) + name);
            }
            if (samplerNames.empty()) {
                return glslCode;
            }

            // Rewrite the lookups. Right-to-left so earlier offsets stay valid, and only for
            // samplers named directly as the first argument (SPIRV-Cross never produces an
            // expression there for ES output, which has no separate sampler objects).
            String result = glslCode;
            Vector<String> usedSamplers;
            for (SizeT scan = result.size(); scan-- > 0;) {
                if (result[scan] != 't') continue;
                if (scan > 0 && IsIdentifierChar(result[scan - 1])) continue;

                const LodLookupForm* form = nullptr;
                SizeT openParen = 0;
                for (const auto& candidate : LOD_LOOKUP_FORMS) {
                    const SizeT nameLength = std::strlen(candidate.name);
                    if (result.compare(scan, nameLength, candidate.name) != 0) continue;
                    SizeT after = result.find_first_not_of(" \t", scan + nameLength);
                    if (after == String::npos || result[after] != '(') continue;
                    form = &candidate;
                    openParen = after;
                    break;
                }
                if (form == nullptr) continue;

                const Vector<SizeT> marks = SplitCallArguments(result, openParen);
                if (marks.empty()) continue;
                const SizeT argCount = marks.size();
                const SizeT closeParen = marks.back();

                // First argument must be one of our samplers.
                const SizeT firstArgStart = result.find_first_not_of(" \t", openParen + 1);
                SizeT firstArgEnd = marks.front();
                while (firstArgEnd > firstArgStart && (result[firstArgEnd - 1] == ' ' || result[firstArgEnd - 1] == '\t')) {
                    --firstArgEnd;
                }
                if (firstArgStart == String::npos || firstArgEnd <= firstArgStart) continue;
                const String samplerName = result.substr(firstArgStart, firstArgEnd - firstArgStart);
                const auto samplerIt = samplerNames.find(samplerName);
                if (samplerIt == samplerNames.end()) continue;

                const String& biasName = samplerIt->second;
                if (form->explicitLodArg >= 0) {
                    // Explicit LOD: the bias adds to it, as Vulkan does for
                    // OpImageSampleExplicitLod and as the CTS reference expects.
                    const SizeT lodIndex = static_cast<SizeT>(form->explicitLodArg);
                    if (argCount <= lodIndex) continue;
                    const SizeT lodStart = marks[lodIndex - 1] + 1;
                    const SizeT lodEnd = marks[lodIndex];
                    result.insert(lodEnd, String(") + ") + biasName + ")");
                    result.insert(lodStart, "((");
                } else {
                    const SizeT required = static_cast<SizeT>(form->requiredArgs);
                    if (argCount == required) {
                        result.insert(closeParen, String(", ") + biasName);
                    } else if (argCount == required + 1) {
                        const SizeT biasStart = marks[argCount - 2] + 1;
                        result.insert(closeParen, String(") + ") + biasName + ")");
                        result.insert(biasStart, "((");
                    } else {
                        continue;
                    }
                }
                usedSamplers.push_back(samplerName);
            }
            if (usedSamplers.empty()) {
                return glslCode;
            }

            // Declare the bias uniforms that were actually referenced, right after the
            // sampler declaration line they belong to.
            for (const auto& samplerName : usedSamplers) {
                const String& biasName = samplerNames[samplerName];
                if (result.find(String("float ") + biasName + ";") != String::npos) continue;
                const std::regex declRegex(
                    R"(uniform\s+(?:(?:highp|mediump|lowp)\s+)?[iu]?sampler[A-Za-z0-9]*\s+)" + samplerName + R"(\s*;)");
                std::smatch match;
                if (!std::regex_search(result, match, declRegex)) continue;
                const SizeT declEnd = static_cast<SizeT>(match.position(0)) + match[0].str().size();
                result.insert(declEnd, String("\nuniform highp float ") + biasName + ";");
            }
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

    // ---- Client-format readback conversion helpers -------------------------------------------------
    // ReadPixels/GetTexImage read a guaranteed wide RGBA(_INTEGER) layout from the ES driver and repack
    // it on the CPU into the client's (format, type) layout. Everything here is pure byte shuffling so
    // unit tests can assert the exact packed words; field positions follow GL 3.3 table 3.6 and mirror
    // the GL CTS packed_pixels oracle (glcPackedPixelsTests.cpp pack_UNSIGNED_* helpers).
    namespace ReadbackImpl {
        using MG_Util::DecodeHalfBitsToFloat;
        using MG_Util::EncodeFloatToHalfBits;

        Bool GetReadbackChannelMapping(GLenum format, ReadbackChannelMapping& outMapping) {
            switch (format) {
            case GL_RED:          outMapping = {{0, 0, 0, 0}, 1, false}; return true;
            case GL_RED_INTEGER:  outMapping = {{0, 0, 0, 0}, 1, true};  return true;
            // Desktop-GL single-channel client formats (GL CTS packed_pixels rgba8_format_green/blue):
            // the destination holds one component sourced from the named channel of the wide RGBA read.
            // GL_ALPHA is mapped here from the raw enum because the state layer folds it into Red for the
            // legacy alpha-texture upload hack.
            case GL_GREEN:         outMapping = {{1, 0, 0, 0}, 1, false}; return true;
            case GL_GREEN_INTEGER: outMapping = {{1, 0, 0, 0}, 1, true};  return true;
            case GL_BLUE:          outMapping = {{2, 0, 0, 0}, 1, false}; return true;
            case GL_BLUE_INTEGER:  outMapping = {{2, 0, 0, 0}, 1, true};  return true;
            case GL_ALPHA:         outMapping = {{3, 0, 0, 0}, 1, false}; return true;
            case GL_ALPHA_INTEGER: outMapping = {{3, 0, 0, 0}, 1, true};  return true;
            case GL_RG:           outMapping = {{0, 1, 0, 0}, 2, false}; return true;
            case GL_RG_INTEGER:   outMapping = {{0, 1, 0, 0}, 2, true};  return true;
            case GL_RGB:          outMapping = {{0, 1, 2, 0}, 3, false}; return true;
            case GL_RGB_INTEGER:  outMapping = {{0, 1, 2, 0}, 3, true};  return true;
            case GL_BGR:          outMapping = {{2, 1, 0, 0}, 3, false}; return true;
            case GL_BGR_INTEGER:  outMapping = {{2, 1, 0, 0}, 3, true};  return true;
            case GL_RGBA:         outMapping = {{0, 1, 2, 3}, 4, false}; return true;
            case GL_RGBA_INTEGER: outMapping = {{0, 1, 2, 3}, 4, true};  return true;
            case GL_BGRA:         outMapping = {{2, 1, 0, 3}, 4, false}; return true;
            case GL_BGRA_INTEGER: outMapping = {{2, 1, 0, 3}, 4, true};  return true;
            default:
                return false;
            }
        }

        Bool GetPackedReadbackLayout(GLenum type, PackedReadbackLayout& out) {
            switch (type) {
            // Non-REV types pack the first format component starting at the most significant bit,
            // *_REV types starting at the least significant bit (GL CTS pack_UNSIGNED_SHORT_5_6_5:
            // R bits 15-11; pack_UNSIGNED_SHORT_1_5_5_5_REV: R bits 4-0, A bit 15).
            case GL_UNSIGNED_BYTE_3_3_2:          out = {3, {3, 3, 2, 0},    {5, 2, 0, 0},    1, false}; return true;
            case GL_UNSIGNED_BYTE_2_3_3_REV:      out = {3, {3, 3, 2, 0},    {0, 3, 6, 0},    1, false}; return true;
            case GL_UNSIGNED_SHORT_5_6_5:         out = {3, {5, 6, 5, 0},    {11, 5, 0, 0},   2, false}; return true;
            case GL_UNSIGNED_SHORT_5_6_5_REV:     out = {3, {5, 6, 5, 0},    {0, 5, 11, 0},   2, false}; return true;
            case GL_UNSIGNED_SHORT_4_4_4_4:       out = {4, {4, 4, 4, 4},    {12, 8, 4, 0},   2, false}; return true;
            case GL_UNSIGNED_SHORT_4_4_4_4_REV:   out = {4, {4, 4, 4, 4},    {0, 4, 8, 12},   2, false}; return true;
            case GL_UNSIGNED_SHORT_5_5_5_1:       out = {4, {5, 5, 5, 1},    {11, 6, 1, 0},   2, false}; return true;
            case GL_UNSIGNED_SHORT_1_5_5_5_REV:   out = {4, {5, 5, 5, 1},    {0, 5, 10, 15},  2, false}; return true;
            case GL_UNSIGNED_INT_8_8_8_8:         out = {4, {8, 8, 8, 8},    {24, 16, 8, 0},  4, false}; return true;
            case GL_UNSIGNED_INT_8_8_8_8_REV:     out = {4, {8, 8, 8, 8},    {0, 8, 16, 24},  4, false}; return true;
            case GL_UNSIGNED_INT_10_10_10_2:      out = {4, {10, 10, 10, 2}, {22, 12, 2, 0},  4, false}; return true;
            case GL_UNSIGNED_INT_2_10_10_10_REV:  out = {4, {10, 10, 10, 2}, {0, 10, 20, 30}, 4, false}; return true;
            // Packed-float RGB types: fields hold unsigned small floats; 5_9_9_9_REV's shared 5-bit
            // exponent (bits 31-27) is emitted by EncodeSharedExponentRGB9E5, not a component field.
            case GL_UNSIGNED_INT_10F_11F_11F_REV: out = {3, {11, 11, 10, 0}, {0, 11, 22, 0},  4, true};  return true;
            case GL_UNSIGNED_INT_5_9_9_9_REV:     out = {3, {9, 9, 9, 0},    {0, 9, 18, 0},   4, true};  return true;
            default:
                return false;
            }
        }

        SizeT GetReadbackComponentSize(GLenum type) {
            PackedReadbackLayout packedLayout{};
            if (GetPackedReadbackLayout(type, packedLayout)) {
                return packedLayout.byteSize;
            }
            switch (type) {
            case GL_UNSIGNED_BYTE:
            case GL_BYTE:
                return 1;
            case GL_UNSIGNED_SHORT:
            case GL_SHORT:
            case GL_HALF_FLOAT:
                return 2;
            case GL_UNSIGNED_INT:
            case GL_INT:
            case GL_FLOAT:
                return 4;
            default:
                return 0;
            }
        }

        SizeT GetReadbackDstPixelSize(const ReadbackChannelMapping& mapping, GLenum type) {
            PackedReadbackLayout packedLayout{};
            if (GetPackedReadbackLayout(type, packedLayout)) {
                if (packedLayout.fieldCount != mapping.channelCount) {
                    return 0; // 3-field packed types pair with 3-component formats only, 4 with 4
                }
                if (mapping.isInteger && packedLayout.isFloatPacked) {
                    return 0; // packed-float RGB types never pair with integer formats
                }
                return packedLayout.byteSize;
            }
            if (mapping.isInteger && (type == GL_FLOAT || type == GL_HALF_FLOAT)) {
                return 0;
            }
            const SizeT componentSize = GetReadbackComponentSize(type);
            return componentSize == 0 ? 0 : static_cast<SizeT>(mapping.channelCount) * componentSize;
        }

        namespace {
            void WritePackedReadbackWord(Uint8* dst, Uint32 word, SizeT byteSize) {
                switch (byteSize) {
                case 1: {
                    const auto out = static_cast<Uint8>(word);
                    Memcpy(dst, &out, sizeof(out));
                    break;
                }
                case 2: {
                    const auto out = static_cast<Uint16>(word);
                    Memcpy(dst, &out, sizeof(out));
                    break;
                }
                default:
                    Memcpy(dst, &word, sizeof(word));
                    break;
                }
            }
        } // namespace

        // Shared encoders live in MG_Util/Math/SmallFloat.h so the upload conversion
        // (PixelStoreProcessor) uses byte-identical packing; kept exported here for unit tests.
        Uint32 EncodeFloatToUnsignedF11(Float value) { return MG_Util::EncodeFloatToUnsignedF11(value); }
        Uint32 EncodeFloatToUnsignedF10(Float value) { return MG_Util::EncodeFloatToUnsignedF10(value); }
        Uint32 EncodeSharedExponentRGB9E5(const Float rgb[3]) { return MG_Util::EncodeSharedExponentRGB9E5(rgb); }

        void ConvertWideReadbackRow(const Uint8* src, Uint8* dst, SizeT width, GLenum wideType,
                                    const ReadbackChannelMapping& mapping, GLenum type) {
            PackedReadbackLayout packedLayout{};
            const Bool isPacked = GetPackedReadbackLayout(type, packedLayout);
            const SizeT dstComponentSize = GetReadbackComponentSize(type);
            const SizeT dstPixelBytes = GetReadbackDstPixelSize(mapping, type);
            const SizeT srcPixelBytes = 4 * GetReadbackComponentSize(wideType);

            for (SizeT col = 0; col < width; ++col) {
                const Uint8* srcPixel = src + col * srcPixelBytes;
                Uint8* dstPixel = dst + col * dstPixelBytes;
                if (mapping.isInteger) {
                    Int64 srcValues[4];
                    for (Int c = 0; c < 4; ++c) {
                        srcValues[c] = wideType == GL_INT
                                           ? static_cast<Int64>(reinterpret_cast<const Int32*>(srcPixel)[c])
                                           : static_cast<Int64>(reinterpret_cast<const Uint32*>(srcPixel)[c]);
                    }
                    if (isPacked) {
                        // Integer sources clamp each component to the unsigned range of its field
                        // (GL 3.3 section 4.3.1 final conversion).
                        Uint32 word = 0;
                        for (Int ch = 0; ch < packedLayout.fieldCount; ++ch) {
                            const Int64 fieldMax = (Int64{1} << packedLayout.width[ch]) - 1;
                            const auto v = static_cast<Uint32>(
                                std::clamp<Int64>(srcValues[mapping.sourceChannel[ch]], 0, fieldMax));
                            word |= v << packedLayout.shift[ch];
                        }
                        WritePackedReadbackWord(dstPixel, word, packedLayout.byteSize);
                    } else {
                        for (Int ch = 0; ch < mapping.channelCount; ++ch) {
                            const Int64 v = srcValues[mapping.sourceChannel[ch]];
                            Uint8* dstComponent = dstPixel + static_cast<SizeT>(ch) * dstComponentSize;
                            switch (type) {
                            case GL_UNSIGNED_BYTE:
                                *dstComponent = static_cast<Uint8>(std::clamp<Int64>(v, 0, 255));
                                break;
                            case GL_BYTE: {
                                const auto out = static_cast<Int8>(std::clamp<Int64>(v, -128, 127));
                                Memcpy(dstComponent, &out, sizeof(out));
                                break;
                            }
                            case GL_UNSIGNED_SHORT: {
                                const auto out = static_cast<Uint16>(std::clamp<Int64>(v, 0, 65535));
                                Memcpy(dstComponent, &out, sizeof(out));
                                break;
                            }
                            case GL_SHORT: {
                                const auto out = static_cast<Int16>(std::clamp<Int64>(v, -32768, 32767));
                                Memcpy(dstComponent, &out, sizeof(out));
                                break;
                            }
                            case GL_UNSIGNED_INT: {
                                const auto out = static_cast<Uint32>(std::clamp<Int64>(v, 0, 4294967295LL));
                                Memcpy(dstComponent, &out, sizeof(out));
                                break;
                            }
                            case GL_INT: {
                                const auto out =
                                    static_cast<Int32>(std::clamp<Int64>(v, -2147483648LL, 2147483647LL));
                                Memcpy(dstComponent, &out, sizeof(out));
                                break;
                            }
                            default:
                                break;
                            }
                        }
                    }
                } else {
                    Float srcValues[4];
                    switch (wideType) {
                    case GL_UNSIGNED_BYTE:
                        for (Int c = 0; c < 4; ++c) {
                            srcValues[c] = static_cast<Float>(srcPixel[c]) / 255.0f;
                        }
                        break;
                    case GL_BYTE:
                        for (Int c = 0; c < 4; ++c) {
                            srcValues[c] = std::max(
                                static_cast<Float>(reinterpret_cast<const Int8*>(srcPixel)[c]) / 127.0f, -1.0f);
                        }
                        break;
                    case GL_UNSIGNED_SHORT:
                        for (Int c = 0; c < 4; ++c) {
                            srcValues[c] =
                                static_cast<Float>(reinterpret_cast<const Uint16*>(srcPixel)[c]) / 65535.0f;
                        }
                        break;
                    case GL_SHORT:
                        for (Int c = 0; c < 4; ++c) {
                            srcValues[c] = std::max(
                                static_cast<Float>(reinterpret_cast<const Int16*>(srcPixel)[c]) / 32767.0f, -1.0f);
                        }
                        break;
                    case GL_HALF_FLOAT:
                        for (Int c = 0; c < 4; ++c) {
                            srcValues[c] = DecodeHalfBitsToFloat(reinterpret_cast<const Uint16*>(srcPixel)[c]);
                        }
                        break;
                    default: // GL_FLOAT
                        for (Int c = 0; c < 4; ++c) {
                            srcValues[c] = reinterpret_cast<const Float*>(srcPixel)[c];
                        }
                        break;
                    }
                    if (isPacked) {
                        Uint32 word = 0;
                        if (packedLayout.isFloatPacked) {
                            const Float fields[3] = {srcValues[mapping.sourceChannel[0]],
                                                     srcValues[mapping.sourceChannel[1]],
                                                     srcValues[mapping.sourceChannel[2]]};
                            word = type == GL_UNSIGNED_INT_5_9_9_9_REV
                                       ? EncodeSharedExponentRGB9E5(fields)
                                       : (EncodeFloatToUnsignedF11(fields[0]) << packedLayout.shift[0]) |
                                             (EncodeFloatToUnsignedF11(fields[1]) << packedLayout.shift[1]) |
                                             (EncodeFloatToUnsignedF10(fields[2]) << packedLayout.shift[2]);
                        } else {
                            // Normalized encode: round(clamp(v, 0, 1) * (2^bits - 1)) into each field.
                            for (Int ch = 0; ch < packedLayout.fieldCount; ++ch) {
                                const auto fieldMax = static_cast<Float>((1u << packedLayout.width[ch]) - 1u);
                                const auto v = static_cast<Uint32>(std::llround(
                                    std::clamp(srcValues[mapping.sourceChannel[ch]], 0.0f, 1.0f) * fieldMax));
                                word |= v << packedLayout.shift[ch];
                            }
                        }
                        WritePackedReadbackWord(dstPixel, word, packedLayout.byteSize);
                    } else {
                        for (Int ch = 0; ch < mapping.channelCount; ++ch) {
                            const Float v = srcValues[mapping.sourceChannel[ch]];
                            Uint8* dstComponent = dstPixel + static_cast<SizeT>(ch) * dstComponentSize;
                            switch (type) {
                            case GL_UNSIGNED_BYTE:
                                *dstComponent =
                                    static_cast<Uint8>(std::llround(std::clamp(v, 0.0f, 1.0f) * 255.0));
                                break;
                            case GL_BYTE: {
                                const auto out =
                                    static_cast<Int8>(std::llround(std::clamp(v, -1.0f, 1.0f) * 127.0));
                                Memcpy(dstComponent, &out, sizeof(out));
                                break;
                            }
                            case GL_UNSIGNED_SHORT: {
                                const auto out =
                                    static_cast<Uint16>(std::llround(std::clamp(v, 0.0f, 1.0f) * 65535.0));
                                Memcpy(dstComponent, &out, sizeof(out));
                                break;
                            }
                            case GL_SHORT: {
                                const auto out =
                                    static_cast<Int16>(std::llround(std::clamp(v, -1.0f, 1.0f) * 32767.0));
                                Memcpy(dstComponent, &out, sizeof(out));
                                break;
                            }
                            case GL_UNSIGNED_INT: {
                                const auto out = static_cast<Uint32>(
                                    std::llround(static_cast<Double>(std::clamp(v, 0.0f, 1.0f)) * 4294967295.0));
                                Memcpy(dstComponent, &out, sizeof(out));
                                break;
                            }
                            case GL_INT: {
                                const auto out = static_cast<Int32>(
                                    std::llround(static_cast<Double>(std::clamp(v, -1.0f, 1.0f)) * 2147483647.0));
                                Memcpy(dstComponent, &out, sizeof(out));
                                break;
                            }
                            case GL_FLOAT:
                                Memcpy(dstComponent, &v, sizeof(v));
                                break;
                            case GL_HALF_FLOAT: {
                                const Uint16 out = EncodeFloatToHalfBits(v);
                                Memcpy(dstComponent, &out, sizeof(out));
                                break;
                            }
                            default:
                                break;
                            }
                        }
                    }
                }
            }
        }

        static SizeT AlignReadbackRow(SizeT rowBytes, Int alignment) {
            const SizeT align = alignment > 0 ? static_cast<SizeT>(alignment) : 1;
            return (rowBytes + align - 1) / align * align;
        }

    // Repacks wide RGBA(_INTEGER) rows into the client's (format, type) layout, honoring the
        // client-side PACK parameters and the bound pixel-pack buffer. `wide` holds
        // `sliceHeight * sliceCount` rows of `width` texels (slice-major, tightly stacked),
        // 4 components x GetReadbackComponentSize(wideType) bytes each.
        // applyPackImageParams: GL_PACK_IMAGE_HEIGHT / GL_PACK_SKIP_IMAGES apply only to GetTexImage
        // of 3D/array images; ReadPixels and 2D GetTexImage ignore them (GL 3.3 sections 4.3.1, 6.1.4).
        // Per the GL addressing rules, slice k row j lands at
        // SKIP_IMAGES*imageStride + SKIP_ROWS*rowStride + SKIP_PIXELS*pixelBytes
        //   + k*imageStride + j*rowStride, with imageStride = max(IMAGE_HEIGHT, sliceHeight)*rowStride.
        Bool StoreWideRowsToClient(const Uint8* wide, GLenum wideType, GLsizei width, GLsizei sliceHeight,
                                   GLsizei sliceCount, const ReadbackChannelMapping& mapping, GLenum type,
                                   void* pixels, Bool applyPackImageParams) {
        const SizeT dstPixelBytes = GetReadbackDstPixelSize(mapping, type);
        if (dstPixelBytes == 0) {
            return false;
        }
        PackedReadbackLayout packedLayout{};
        const Bool isPackedType = GetPackedReadbackLayout(type, packedLayout);
        const SizeT dstComponentSize = GetReadbackComponentSize(type);

        const auto& pixelPackBufferObject =
            MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::PixelPack).GetBoundObject();

        // Destination layout is computed from the client-side PACK parameters; only the actual pixel
        // rows are written so skip regions of the destination stay untouched.
        const auto packParams = MG_State::pGLContext->GetPixelStoreParameters(false);
        const SizeT rowPixels = static_cast<SizeT>(packParams.RowLength > 0 ? packParams.RowLength : width);
        const SizeT dstRowStride = AlignReadbackRow(rowPixels * dstPixelBytes, packParams.Alignment);
        const SizeT imageRows =
            applyPackImageParams && packParams.ImageHeight > 0
                ? static_cast<SizeT>(packParams.ImageHeight)
                : static_cast<SizeT>(sliceHeight);
        const SizeT dstImageStride = imageRows * dstRowStride;
        const SizeT skipImages =
            applyPackImageParams ? static_cast<SizeT>(std::max(packParams.SkipImages, 0)) : SizeT{0};
        const SizeT dstSkipOffset = skipImages * dstImageStride +
                                    static_cast<SizeT>(std::max(packParams.SkipRows, 0)) * dstRowStride +
                                    static_cast<SizeT>(std::max(packParams.SkipPixels, 0)) * dstPixelBytes;
        const SizeT dstRowBytes = static_cast<SizeT>(width) * dstPixelBytes;

        const SizeT pboBaseOffset = reinterpret_cast<SizeT>(pixels); // with a PBO, `pixels` is an offset
        if (pixelPackBufferObject) {
            const SizeT requiredSize = pboBaseOffset + dstSkipOffset +
                                    static_cast<SizeT>(sliceCount - 1) * dstImageStride +
                                    static_cast<SizeT>(sliceHeight - 1) * dstRowStride + dstRowBytes;
            if (requiredSize > pixelPackBufferObject->GetSize()) {
                MGLOG_E("Readback conversion: pixel pack buffer is too small");
                return true;
            }
        }

        const SizeT srcComponentSize = GetReadbackComponentSize(wideType);
        const SizeT srcPixelBytes = 4 * srcComponentSize;
        Vector<Uint8> convertedRow(dstRowBytes);

        for (GLsizei slice = 0; slice < sliceCount; ++slice) {
            for (GLsizei row = 0; row < sliceHeight; ++row) {
                const SizeT flatRow = static_cast<SizeT>(slice) * static_cast<SizeT>(sliceHeight) +
                                   static_cast<SizeT>(row);
                const Uint8* srcRow = wide + flatRow * static_cast<SizeT>(width) * srcPixelBytes;
                ConvertWideReadbackRow(srcRow, convertedRow.data(), static_cast<SizeT>(width), wideType,
                                                  mapping, type);

                if (packParams.SwapBytes) {
                    const SizeT groupSize = isPackedType ? packedLayout.byteSize : dstComponentSize;
                    if (groupSize > 1) {
                        for (SizeT offset = 0; offset + groupSize <= dstRowBytes; offset += groupSize) {
                            std::reverse(convertedRow.data() + offset, convertedRow.data() + offset + groupSize);
                        }
                    }
                }

                const SizeT dstOffset = dstSkipOffset + static_cast<SizeT>(slice) * dstImageStride +
                                     static_cast<SizeT>(row) * dstRowStride;
                if (pixelPackBufferObject) {
                    pixelPackBufferObject->WritebackFromBackend({convertedRow.data(), dstRowBytes},
                                                             pboBaseOffset + dstOffset);
                } else {
                    Memcpy(static_cast<Uint8*>(pixels) + dstOffset, convertedRow.data(), dstRowBytes);
                }
            }
        }
            return true;
        }
    } // namespace ReadbackImpl
} // namespace MobileGL::MG_Backend::DirectGLES
