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

#include <algorithm>
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
                options = GetRuntimeFallbackNormalizeOptions(
                    requestedInternalFormat,
                    TextureImpl::GetRenderTargetNormalizeOptions(g_GLESCapabilities, targetIndex));
            }
            NormalizePixelFormat(requestedInternalFormat, options, outInternalFormat, outFormat, outType);
        }
    } // namespace

    namespace TextureImpl {
        // Every image that can back a colour attachment needs a colour-renderable storage format,
        // and ES has no renderable three-channel format at all: a three-channel float fallback is
        // a legal ES texture but neither legal multisample storage nor a legal attachment, so
        // GL_RGB8_SNORM / GL_RGB16F / ... have to be widened to four channels for any of them.
        // This used to cover the multisample pair alone, on the grounds that only those can never
        // be uploaded to; the transfer paths now expand three-channel client data themselves
        // (Managers.cpp PrepareFallbackUpload) and hide the added alpha again on sample and
        // readback, so the same substitution is available everywhere.
        //
        // The widening only ever *happens* where the driver refuses the native form (see
        // PopulateFormatCapabilitiesImpl: outside multisample storage it rides the driver branch,
        // behind the native probe), so a driver that does render to a three-channel image keeps
        // allocating it byte for byte.
        //
        // Do NOT read that as "nothing changes off-device". Measured on Mesa 26.1.6 llvmpipe
        // (the headless CI driver), an ES 3.2 GL_TEXTURE_2D colour attachment is COMPLETE for
        // GL_RGB8 and GL_RGB16F but INCOMPLETE_ATTACHMENT for GL_RGB8_SNORM, GL_SRGB8 and every
        // RGB integer format, and UNSUPPORTED for GL_RGB32F. Those eight formats therefore DO
        // take the widened path on llvmpipe, which is where the retrace fixtures and the glcts
        // green suites run - the substitution is driver-conditional, not desktop-exempt.
        //
        // A buffer texture is the one image that can never be an attachment; its storage is the
        // buffer object's, and widening it would misdescribe the application's data.
        Bool TargetRequiresRenderableFormat(SizeT targetIndex) {
            if (targetIndex >= kFormatCapabilityTargetCount) {
                return false;
            }
            if (targetIndex == kFormatCapabilityRenderbufferTargetIndex) {
                return true;
            }
            return static_cast<TextureTarget>(targetIndex) != TextureTarget::TextureBuffer;
        }

        Flags<PixelFormatNormalizeOptionBit> GetRenderTargetNormalizeOptions(
            const MG_External::GLESCapabilities& capabilities, SizeT targetIndex) {
            Flags<PixelFormatNormalizeOptionBit> options;
            if (!TargetRequiresRenderableFormat(targetIndex)) {
                return options;
            }
            options |= PixelFormatNormalizeOptionBit::NoThreeChannelRenderTarget;
            if (!capabilities.SupportsRenderSnorm || !capabilities.SupportsNorm16Texture) {
                options |= PixelFormatNormalizeOptionBit::NoSnorm16RenderTarget;
            }
            return options;
        }

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

        namespace {
            Bool BackendFormatAddsAlpha(TextureInternalFormat internalFormat, SizeT targetIndex) {
                if (!TargetRequiresRenderableFormat(targetIndex)) {
                    return false;
                }
                if (pActiveBackendObject && !ShouldUseCaveatFormat(internalFormat, targetIndex)) {
                    return false;
                }
                const GLenum requestedInternalFormat = MG_Util::ConvertTextureInternalFormatToGLEnum(internalFormat);
                const Flags<PixelFormatNormalizeOptionBit> options = GetRuntimeFallbackNormalizeOptions(
                    requestedInternalFormat, GetRenderTargetNormalizeOptions(g_GLESCapabilities, targetIndex));
                return static_cast<Bool>(options & PixelFormatNormalizeOptionBit::NoThreeChannelRenderTarget);
            }
        } // namespace

        Bool BackendTextureFormatAddsAlpha(TextureInternalFormat internalFormat, TextureTarget target) {
            const SizeT targetIndex =
                target == TextureTarget::Unknown ? kFormatCapabilityTargetCount : GetFormatCapabilityTargetIndex(target);
            return BackendFormatAddsAlpha(internalFormat, targetIndex);
        }

        Bool BackendRenderbufferFormatAddsAlpha(TextureInternalFormat internalFormat) {
            return BackendFormatAddsAlpha(internalFormat, GetRenderbufferFormatCapabilityTargetIndex());
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

            // Every stage that has an integer interface at all, on BOTH sides. Interpolation is
            // only ever consumed at a fragment input, so the qualifier is semantically inert on
            // a tessellation or geometry interface - but an ES linker still compares the two
            // sides of every interface and rejects a program whose producer says `flat` and
            // whose consumer does not. Covering only the stages that "need" it left exactly two
            // holes, and a program that used tessellation fell into both:
            //   vertex `flat out uint` -> tess-control `in uint`   (producer flat, consumer not)
            //   tess-eval `out uint`   -> geometry `flat in uint`  (consumer flat, producer not)
            // Adreno answers "output ... interpolation mismatch with other stage" and the whole
            // program fails to link, which is a draw that silently paints nothing.
            //
            // Adding rather than stripping, because a fragment input's `flat` is load-bearing
            // (ESSL forbids an interpolated integer) and would have to be put back for the last
            // stage before the fragment shader anyway - so "everything integer is flat" is the
            // one rule that is consistent no matter which stages a program happens to have.
            switch (shaderType) {
            case GL_VERTEX_SHADER:
                addFlatQualifier("out");
                break;
            case GL_TESS_CONTROL_SHADER:
            case GL_TESS_EVALUATION_SHADER:
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

        String RetargetTextureBufferExtension(String glslCode,
                                              MG_External::GLESCapabilities::TextureBufferTier tier) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            // SPIRV-Cross hardcodes the EXT spelling: CompilerGLSL::type_to_glsl emits
            // require_extension_internal("GL_EXT_texture_buffer") for any Dim=Buffer image
            // whenever it targets ESSL below 320, with no OES alternative and no way to
            // configure it. GL_OES_texture_buffer is functionally identical but is a separate
            // directive, and `#extension <name> : require` on a name the driver does not
            // advertise is a hard compile error - so on an OES-only driver the emitted shader
            // fails to compile for the sake of one token.
            //
            // Line comments are excluded by the directive check below; a `#extension` line inside
            // a /* */ block is not, and would be rewritten. That is harmless (it stays a comment)
            // and is not worth a preprocessor-aware scan here.
            //
            // Deliberately a directive rewrite and nothing more. The alternative - teaching the
            // SPIR-V to stop asking for the extension - is not available: the requirement is
            // synthesized by SPIRV-Cross from the image type itself, not carried in the module,
            // so there is nothing upstream to strip. Everything about the shader body that
            // actually uses the buffer texture is identical between the two extensions.
            using Tier = MG_External::GLESCapabilities::TextureBufferTier;
            if (tier != Tier::ExtensionOES) {
                return glslCode;
            }
            static constexpr const char* kExtName = "GL_EXT_texture_buffer";
            static constexpr const char* kOesName = "GL_OES_texture_buffer";
            constexpr SizeT kExtNameLength = 21; // strlen("GL_EXT_texture_buffer")
            static_assert(sizeof("GL_EXT_texture_buffer") - 1 == kExtNameLength, "name length drifted");
            static_assert(sizeof("GL_OES_texture_buffer") - 1 == kExtNameLength,
                          "the two spellings must be the same length for the in-place replace");

            // Only rewrite the name where it is the whole subject of an #extension directive.
            // Two separate guards, both load-bearing:
            //   * the directive check, so a line-comment mentioning the name is left alone;
            //   * the identifier-boundary check, because GL_EXT_texture_buffer is a PREFIX of
            //     GL_EXT_texture_buffer_object - a different, real extension that SPIRV-Cross
            //     emits from the same `case DimBuffer:` on its legacy-desktop branch. Without
            //     the boundary this pass would silently rewrite a request for that extension
            //     into a request for a GL_OES_texture_buffer_object that does not exist.
            const auto isIdentifierChar = [](char c) {
                return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
            };
            SizeT searchFrom = 0;
            while (true) {
                const SizeT hit = glslCode.find(kExtName, searchFrom);
                if (hit == String::npos) {
                    break;
                }
                searchFrom = hit + kExtNameLength;

                // Identifier boundary on both sides, so the name is not a fragment of a longer one.
                if (hit > 0 && isIdentifierChar(glslCode[hit - 1])) {
                    continue;
                }
                if (hit + kExtNameLength < glslCode.size() && isIdentifierChar(glslCode[hit + kExtNameLength])) {
                    continue;
                }

                // Walk back to the start of the line and require that it is an #extension
                // directive, allowing whitespace between '#' and the keyword.
                SizeT lineStart = glslCode.rfind('\n', hit);
                lineStart = (lineStart == String::npos) ? 0 : lineStart + 1;
                SizeT cursor = lineStart;
                while (cursor < hit && std::isspace(static_cast<unsigned char>(glslCode[cursor]))) {
                    ++cursor;
                }
                if (cursor >= hit || glslCode[cursor] != '#') {
                    continue;
                }
                ++cursor;
                while (cursor < hit && std::isspace(static_cast<unsigned char>(glslCode[cursor]))) {
                    ++cursor;
                }
                if (glslCode.compare(cursor, 9, "extension") != 0) {
                    continue;
                }
                glslCode.replace(hit, kExtNameLength, kOesName);
            }
            return glslCode;
        }

        String RequestExtendedImageFormats(String glslCode, Bool needed) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            // GLSL ES core has thirteen image formats; GL has forty. SPIRV-Cross prints whatever
            // format the OpTypeImage carries and asks for no extension for it, so an r8ui or
            // rg16f image - declared as such, or baked from the bound one - reaches the driver as
            // a format its core language does not know. GL_NV_image_formats is the only thing
            // that adds them, and it has to be requested by name.
            //
            // The caller decides `needed`: it knows which formats are in play (from the uniform
            // reflection and the image-unit bindings) and whether the driver advertises the
            // extension at all - `#extension` on an unadvertised name is itself a hard error, so
            // this must never be emitted speculatively.
            static constexpr const char* kDirective = "#extension GL_NV_image_formats : require\n";
            static constexpr const char* kExtName = "GL_NV_image_formats";
            if (!needed || glslCode.find(kExtName) != String::npos) {
                return glslCode;
            }
            // After the #version line, which must stay first. Everything else about the header is
            // order-insensitive, and ForceSupporterOutput's scan for the LAST #extension
            // directive still finds whichever one that is.
            const SizeT versionPos = glslCode.find("#version");
            if (versionPos == String::npos) {
                return kDirective + glslCode;
            }
            const SizeT lineEnd = glslCode.find('\n', versionPos);
            if (lineEnd == String::npos) {
                return glslCode + "\n" + kDirective;
            }
            glslCode.insert(lineEnd + 1, kDirective);
            return glslCode;
        }

        String BakeImageFormatQualifiers(String glslCode,
                                         const UnorderedMap<String, String>& esslFormatByUniformName) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            if (esslFormatByUniformName.empty() || glslCode.find("image") == String::npos) {
                return glslCode;
            }
            // Same declaration shape RebindImageUniformsToFrontendUnits matches, and for the same
            // reason: one line, one image uniform, the name in group 3.
            static const std::regex imageDeclRegex(
                R"((layout\s*\(([^)]*)\)\s*)?uniform\s+(?:(?:readonly|writeonly|coherent|volatile|restrict|highp|mediump|lowp)\s+)*[iu]?image[A-Za-z0-9]+\s+([A-Za-z_][A-Za-z0-9_]*)\s*(\[[^\]]*\])?\s*;)");
            // Every image format spelling GLSL has, so a declaration that already carries one is
            // recognised whatever it says - the caller's map is consulted only for declarations
            // with NO format, never to override a written one.
            static const std::regex existingFormatRegex(
                R"(\b(rgba32f|rgba16f|rg32f|rg16f|r11f_g11f_b10f|r32f|r16f|rgba16|rgb10_a2|rg16|rg8|r16|r8|rgba16_snorm|rgba8_snorm|rg16_snorm|rg8_snorm|r16_snorm|r8_snorm|rgba32i|rgba16i|rgba8i|rg32i|rg16i|rg8i|r32i|r16i|r8i|rgba32ui|rgba16ui|rgba8ui|rgb10_a2ui|rg32ui|rg16ui|rg8ui|r32ui|r16ui|r8ui)\b)");

            String result;
            result.reserve(glslCode.size());
            SizeT lineStart = 0;
            while (lineStart <= glslCode.size()) {
                const SizeT lineEnd = glslCode.find('\n', lineStart);
                const Bool lastLine = lineEnd == String::npos;
                String line = glslCode.substr(lineStart, lastLine ? String::npos : lineEnd - lineStart);

                std::smatch match;
                if (std::regex_search(line, match, imageDeclRegex)) {
                    const String name = match[3].str();
                    const auto formatIt = esslFormatByUniformName.find(name);
                    const String layoutContents = match[2].matched ? match[2].str() : String();
                    if (formatIt != esslFormatByUniformName.end() && !formatIt->second.empty() &&
                        !std::regex_search(layoutContents, existingFormatRegex)) {
                        if (match[1].matched) {
                            const SizeT layoutOpen = line.find('(', match.position(1));
                            line.insert(layoutOpen + 1, formatIt->second + ", ");
                        } else {
                            line.insert(match.position(0), "layout(" + formatIt->second + ") ");
                        }
                    }
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
            Bool IsImagePassIdentifierChar(char c) {
                return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
            }

            // Occurrences of `identifier` in `code` that are whole identifiers, i.e. not the
            // tail or head of a longer one. "goku" must not find "goku_hd" or "my_goku".
            SizeT CountIdentifierOccurrences(const String& code, const String& identifier) {
                if (identifier.empty()) return 0;
                SizeT count = 0;
                for (SizeT pos = code.find(identifier); pos != String::npos;
                     pos = code.find(identifier, pos + 1)) {
                    if (pos > 0 && IsImagePassIdentifierChar(code[pos - 1])) continue;
                    const SizeT after = pos + identifier.size();
                    if (after < code.size() && IsImagePassIdentifierChar(code[after])) continue;
                    ++count;
                }
                return count;
            }

            Bool ContainsIdentifier(const String& code, const String& identifier) {
                return CountIdentifierOccurrences(code, identifier) > 0;
            }

            // The image format layout qualifiers ESSL accepts (GLSL ES 3.20 4.4.7 table 4.6 -
            // the ES-legal subset of what SPIRV-Cross's format_to_glsl can print). The
            // readonly/writeonly rule only applies to a declaration that carries one of them.
            Bool IsImageFormatQualifier(const String& token) {
                static constexpr StringView FORMATS[] = {
                    "rgba32f",      "rgba16f",      "rg32f",       "rg16f",        "r11f_g11f_b10f",
                    "r32f",         "r16f",         "rgba16",      "rgb10_a2",     "rgba8",
                    "rg16",         "rg8",          "r16",         "r8",           "rgba16_snorm",
                    "rgba8_snorm",  "rg16_snorm",   "rg8_snorm",   "r16_snorm",    "r8_snorm",
                    "rgba32i",      "rgba16i",      "rgba8i",      "rg32i",        "rg16i",
                    "rg8i",         "r32i",         "r16i",        "r8i",          "rgba32ui",
                    "rgba16ui",     "rgb10_a2ui",   "rgba8ui",     "rg32ui",       "rg16ui",
                    "rg8ui",        "r32ui",        "r16ui",       "r8ui",
                };
                for (const StringView format : FORMATS) {
                    if (token == format) return true;
                }
                return false;
            }

            // "Except for image variables qualified with the format qualifiers r32f, r32i, and
            // r32ui, image variables must specify either memory qualifier readonly or the
            // memory qualifier writeonly." (GLSL ES 3.20 4.10)
            Bool IsMemoryQualifierExemptImageFormat(const String& token) {
                return token == "r32f" || token == "r32i" || token == "r32ui";
            }

            // Comma-separated contents of a layout(...) list, each entry trimmed.
            Vector<String> SplitLayoutQualifierList(const String& layout) {
                Vector<String> tokens;
                SizeT start = 0;
                while (start <= layout.size()) {
                    SizeT comma = layout.find(',', start);
                    const Bool last = comma == String::npos;
                    String token = layout.substr(start, last ? String::npos : comma - start);
                    const SizeT first = token.find_first_not_of(" \t\r\n");
                    if (first == String::npos) {
                        token.clear();
                    } else {
                        token = token.substr(first, token.find_last_not_of(" \t\r\n") - first + 1);
                    }
                    if (!token.empty()) tokens.push_back(Move(token));
                    if (last) break;
                    start = comma + 1;
                }
                return tokens;
            }

            // Trims both ends and collapses every internal whitespace run to one space, so a
            // qualifier list or array suffix can be spliced back into a rebuilt declaration
            // whatever the original spacing was.
            String NormalizeDeclarationSpacing(const String& text) {
                String out;
                out.reserve(text.size());
                Bool pendingSpace = false;
                for (const char c : text) {
                    if (std::isspace(static_cast<unsigned char>(c))) {
                        pendingSpace = !out.empty();
                        continue;
                    }
                    if (pendingSpace) out += ' ';
                    pendingSpace = false;
                    out += c;
                }
                return out;
            }

            // How an image builtin touches the image it is handed.
            enum class ImageBuiltinAccess { None, Load, Store, Unknown };

            ImageBuiltinAccess ClassifyImageBuiltin(const String& name) {
                if (name == "imageStore") return ImageBuiltinAccess::Store;
                if (name == "imageLoad") return ImageBuiltinAccess::Load;
                // imageAtomic* both reads and writes, but ES only defines the atomics on
                // r32i/r32ui/r32f images - exactly the formats the rule above exempts - so this
                // pass has already skipped any declaration they can legally appear on. Load is
                // enough to keep the classification total without ever being acted upon.
                if (name.compare(0, 11, "imageAtomic") == 0) return ImageBuiltinAccess::Load;
                if (name == "imageSize" || name == "imageSamples") return ImageBuiltinAccess::None;
                // Some other identifier that starts with "image" and is being called: not a
                // shape this pass can reason about, so it poisons the declaration instead of
                // being guessed at.
                return ImageBuiltinAccess::Unknown;
            }

            struct ImageUniformDecl {
                String name;
                String writeName;   // the writeonly half's name, when split
                String layout;      // raw contents of layout(...)
                String qualifiers;  // memory/precision qualifiers, normalized, no trailing space
                String type;        // image2D, uimage2DArray, ...
                String arraySuffix; // "" or "[7]"
                SizeT declStart = 0;
                SizeT declLength = 0;
                SizeT referenceCount = 0; // uses this pass recognized and accounted for
                Bool loaded = false;
                Bool stored = false;
                Bool unknownUse = false;
                Bool split = false;
            };

            // A rebuilt declaration. Keeps SPIRV-Cross's own word order (`uniform readonly
            // highp image2D`) so the image-rebinding regex in Managers.cpp still matches what
            // comes out of here, whichever order the two passes end up running in.
            String BuildImageDeclaration(const ImageUniformDecl& decl, const char* memoryQualifier,
                                         const String& variableName) {
                String out = "layout(" + decl.layout + ") uniform ";
                out += memoryQualifier;
                out += ' ';
                if (!decl.qualifiers.empty()) {
                    out += decl.qualifiers;
                    out += ' ';
                }
                out += decl.type;
                out += ' ';
                out += variableName;
                out += decl.arraySuffix;
                out += ';';
                return out;
            }

            // A name for the writeonly half that no identifier in the shader (and no other
            // half already minted) can collide with.
            String MakeImageWriteAliasName(const String& name, const String& source,
                                           const Vector<String>& taken) {
                String candidate = String(IMAGE_WRITE_ALIAS_PREFIX) + name;
                // "__" anywhere in an identifier is reserved (GLSL ES 3.20 3.7), which a name
                // that already starts with '_' would otherwise produce.
                for (SizeT doubled = candidate.find("__"); doubled != String::npos;
                     doubled = candidate.find("__", doubled)) {
                    candidate.erase(doubled, 1);
                }
                auto isTaken = [&](const String& identifier) {
                    if (ContainsIdentifier(source, identifier)) return true;
                    for (const auto& other : taken) {
                        if (other == identifier) return true;
                    }
                    return false;
                };
                while (isTaken(candidate)) candidate += 'X';
                return candidate;
            }

            struct ImageSourceEdit {
                SizeT start;
                SizeT length;
                String text;
            };
        } // namespace

        String SplitReadWriteImageUniforms(const String& glslCode) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            if (glslCode.find("image") == String::npos) {
                return glslCode;
            }

            // layout(...) uniform <memory/precision qualifiers> <image type> <name>[array];
            // The qualifier alternation is order-free even though SPIRV-Cross emits a fixed
            // order (to_qualifiers_glsl: storage, then coherent/restrict/readonly/writeonly,
            // then precision), and the array group is repeated so a hypothetical multi-
            // dimensional image array survives the round trip intact.
            static const std::regex imageDeclRegex(
                R"(layout\s*\(([^)]*)\)\s*uniform\s+)"
                R"(((?:(?:readonly|writeonly|coherent|volatile|restrict|highp|mediump|lowp)\s+)*))"
                R"(([iu]?image[A-Za-z0-9_]*)\s+([A-Za-z_][A-Za-z0-9_]*)\s*((?:\[[^\]]*\]\s*)*);)");

            Vector<ImageUniformDecl> decls;
            for (std::sregex_iterator it(glslCode.begin(), glslCode.end(), imageDeclRegex), last; it != last; ++it) {
                const std::smatch& match = *it;
                const String qualifiers = match[2].str();
                // Already legal: SPIRV-Cross decided one way, leave it alone.
                if (ContainsIdentifier(qualifiers, "readonly") || ContainsIdentifier(qualifiers, "writeonly")) {
                    continue;
                }

                Bool hasFormat = false;
                Bool exemptFormat = false;
                for (const String& token : SplitLayoutQualifierList(match[1].str())) {
                    if (!IsImageFormatQualifier(token)) continue;
                    hasFormat = true;
                    exemptFormat = IsMemoryQualifierExemptImageFormat(token);
                }
                // No format qualifier at all is a different (and, in ES, unconditionally
                // illegal) shape that GL_EXT_shader_image_load_formatted would be needed for;
                // SPIRV-Cross refuses to emit it for an ES target, so nothing to do here.
                if (!hasFormat || exemptFormat) continue;

                ImageUniformDecl decl;
                decl.layout = match[1].str();
                decl.qualifiers = NormalizeDeclarationSpacing(qualifiers);
                decl.type = match[3].str();
                decl.name = match[4].str();
                decl.arraySuffix = NormalizeDeclarationSpacing(match[5].str());
                decl.declStart = static_cast<SizeT>(match.position(0));
                decl.declLength = match[0].str().size();
                decls.push_back(Move(decl));
            }
            if (decls.empty()) {
                return glslCode;
            }

            auto findDecl = [&decls](const String& name) -> SizeT {
                for (SizeT i = 0; i < decls.size(); ++i) {
                    if (decls[i].name == name) return i;
                }
                return decls.size();
            };

            // Walk every `image*(` call and attribute its first argument to a declaration.
            struct StoreSite {
                SizeT declIndex;
                SizeT start;
                SizeT length;
            };
            Vector<StoreSite> storeSites;
            for (SizeT pos = glslCode.find("image"); pos != String::npos; pos = glslCode.find("image", pos + 1)) {
                if (pos > 0 && IsImagePassIdentifierChar(glslCode[pos - 1])) continue; // uimage2D, myimageFoo
                SizeT tokenEnd = pos;
                while (tokenEnd < glslCode.size() && IsImagePassIdentifierChar(glslCode[tokenEnd])) ++tokenEnd;
                const String builtin = glslCode.substr(pos, tokenEnd - pos);

                const SizeT openParen = glslCode.find_first_not_of(" \t\r\n", tokenEnd);
                if (openParen == String::npos || glslCode[openParen] != '(') continue; // a type, not a call

                const SizeT argStart = glslCode.find_first_not_of(" \t\r\n", openParen + 1);
                if (argStart == String::npos) continue;
                if (!std::isalpha(static_cast<unsigned char>(glslCode[argStart])) && glslCode[argStart] != '_') {
                    continue; // an expression, not a bare variable - it names no image of ours
                }
                SizeT argEnd = argStart;
                while (argEnd < glslCode.size() && IsImagePassIdentifierChar(glslCode[argEnd])) ++argEnd;

                const SizeT declIndex = findDecl(glslCode.substr(argStart, argEnd - argStart));
                if (declIndex == decls.size()) continue;
                ImageUniformDecl& decl = decls[declIndex];
                ++decl.referenceCount;

                // The operand has to be the bare variable, optionally subscripted. Anything
                // else (a member access, a call result) is a shape this pass cannot rewrite.
                SizeT after = glslCode.find_first_not_of(" \t\r\n", argEnd);
                if (after != String::npos && glslCode[after] == '[') {
                    Int depth = 0;
                    SizeT scan = after;
                    for (; scan < glslCode.size(); ++scan) {
                        if (glslCode[scan] == '[') ++depth;
                        else if (glslCode[scan] == ']' && --depth == 0) break;
                    }
                    after = scan >= glslCode.size() ? String::npos
                                                    : glslCode.find_first_not_of(" \t\r\n", scan + 1);
                }
                const char nextChar = after == String::npos ? '\0' : glslCode[after];
                if (nextChar != ',' && nextChar != ')') {
                    decl.unknownUse = true;
                    continue;
                }

                switch (ClassifyImageBuiltin(builtin)) {
                case ImageBuiltinAccess::Load:
                    decl.loaded = true;
                    break;
                case ImageBuiltinAccess::Store:
                    decl.stored = true;
                    storeSites.push_back({declIndex, argStart, argEnd - argStart});
                    break;
                case ImageBuiltinAccess::None:
                    break;
                default:
                    decl.unknownUse = true;
                    break;
                }
            }

            // Every mention of the name has to be one this pass saw, or the split would leave
            // a store pointing at the readonly half. One occurrence is the declaration itself.
            for (auto& decl : decls) {
                if (CountIdentifierOccurrences(glslCode, decl.name) != decl.referenceCount + 1) {
                    decl.unknownUse = true;
                }
            }

            Vector<ImageSourceEdit> edits;
            Vector<String> takenAliases;
            for (auto& decl : decls) {
                if (decl.unknownUse) continue; // leave it exactly as it was; no guessing
                if (decl.loaded && decl.stored) {
                    decl.writeName = MakeImageWriteAliasName(decl.name, glslCode, takenAliases);
                    takenAliases.push_back(decl.writeName);
                    decl.split = true;
                    edits.push_back({decl.declStart, decl.declLength,
                                     BuildImageDeclaration(decl, "readonly", decl.name) + "\n" +
                                         BuildImageDeclaration(decl, "writeonly", decl.writeName)});
                } else if (decl.stored) {
                    edits.push_back({decl.declStart, decl.declLength,
                                     BuildImageDeclaration(decl, "writeonly", decl.name)});
                } else {
                    // Loaded only, or only ever handed to imageSize (or unused): readonly is
                    // the qualifier that keeps every one of those legal.
                    edits.push_back({decl.declStart, decl.declLength,
                                     BuildImageDeclaration(decl, "readonly", decl.name)});
                }
            }
            for (const StoreSite& site : storeSites) {
                const ImageUniformDecl& decl = decls[site.declIndex];
                if (!decl.split) continue;
                edits.push_back({site.start, site.length, decl.writeName});
            }
            if (edits.empty()) {
                return glslCode;
            }

            // Back to front, so an earlier edit's offsets stay valid.
            std::sort(edits.begin(), edits.end(),
                      [](const ImageSourceEdit& a, const ImageSourceEdit& b) { return a.start > b.start; });
            String result = glslCode;
            for (const ImageSourceEdit& edit : edits) {
                result.replace(edit.start, edit.length, edit.text);
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

        String EmulateTextureLodBias(const String& glslCode, Bool avoidExplicitLodBias) {
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
                if (form->explicitLodArg >= 0 && avoidExplicitLodBias) {
                    // The lookup already names its level; leaving it alone keeps a constant
                    // LOD constant. Costs the bias on explicit-LOD lookups only.
                    continue;
                }
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
                MGLOG_D("-> GLES Error: %s", MG_Util::ConvertGLEnumToString(err).c_str());
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

        // Walks the client-side destination the PACK parameters describe and hands each row to
        // `fillRow(slice, row, dstRow)`, which writes width * dstPixelBytes bytes of finished client
        // texels. Shared by the converting and the raw-word stores so both address the destination -
        // and feed the bound pixel-pack buffer - identically.
        // applyPackImageParams: GL_PACK_IMAGE_HEIGHT / GL_PACK_SKIP_IMAGES apply only to GetTexImage
        // of 3D/array images; ReadPixels and 2D GetTexImage ignore them (GL 3.3 sections 4.3.1, 6.1.4).
        // Per the GL addressing rules, slice k row j lands at
        // SKIP_IMAGES*imageStride + SKIP_ROWS*rowStride + SKIP_PIXELS*pixelBytes
        //   + k*imageStride + j*rowStride, with imageStride = max(IMAGE_HEIGHT, sliceHeight)*rowStride.
        template <typename FillRow>
        static Bool StoreClientRows(SizeT dstPixelBytes, SizeT swapGroupSize, GLsizei width, GLsizei sliceHeight,
                                    GLsizei sliceCount, void* pixels, Bool applyPackImageParams, FillRow&& fillRow) {
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
                    MGLOG_E_ONCE("Readback conversion: pixel pack buffer is too small");
                    return true;
                }
            }

            Vector<Uint8> convertedRow(dstRowBytes);

            for (GLsizei slice = 0; slice < sliceCount; ++slice) {
                for (GLsizei row = 0; row < sliceHeight; ++row) {
                    fillRow(slice, row, convertedRow.data());

                    if (packParams.SwapBytes && swapGroupSize > 1) {
                        for (SizeT offset = 0; offset + swapGroupSize <= dstRowBytes; offset += swapGroupSize) {
                            std::reverse(convertedRow.data() + offset, convertedRow.data() + offset + swapGroupSize);
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
            if (pixelPackBufferObject) {
                // WritebackFromBackend bumps change serials with no backend op; re-open
                // the buffer draw-clean memos (once for the whole row loop).
                BufferImpl::BumpBufferMutationEpoch();
            }
            return true;
        }

        // Repacks wide RGBA(_INTEGER) rows into the client's (format, type) layout, honoring the
        // client-side PACK parameters and the bound pixel-pack buffer. `wide` holds
        // `sliceHeight * sliceCount` rows of `width` texels (slice-major, tightly stacked),
        // 4 components x GetReadbackComponentSize(wideType) bytes each.
        Bool StoreWideRowsToClient(const Uint8* wide, GLenum wideType, GLsizei width, GLsizei sliceHeight,
                                   GLsizei sliceCount, const ReadbackChannelMapping& mapping, GLenum type,
                                   void* pixels, Bool applyPackImageParams) {
            const SizeT dstPixelBytes = GetReadbackDstPixelSize(mapping, type);
            if (dstPixelBytes == 0) {
                return false;
            }
            PackedReadbackLayout packedLayout{};
            const Bool isPackedType = GetPackedReadbackLayout(type, packedLayout);
            const SizeT swapGroupSize = isPackedType ? packedLayout.byteSize : GetReadbackComponentSize(type);
            const SizeT srcPixelBytes = 4 * GetReadbackComponentSize(wideType);

            return StoreClientRows(dstPixelBytes, swapGroupSize, width, sliceHeight, sliceCount, pixels,
                                   applyPackImageParams,
                                   [&](GLsizei slice, GLsizei row, Uint8* dstRow) {
                                       const SizeT flatRow = static_cast<SizeT>(slice) *
                                                                 static_cast<SizeT>(sliceHeight) +
                                                             static_cast<SizeT>(row);
                                       const Uint8* srcRow =
                                           wide + flatRow * static_cast<SizeT>(width) * srcPixelBytes;
                                       ConvertWideReadbackRow(srcRow, dstRow, static_cast<SizeT>(width), wideType,
                                                              mapping, type);
                                   });
        }

        Bool StorePackedWordsToClient(const Uint8* srcWords, GLsizei width, GLsizei sliceHeight, GLsizei sliceCount,
                                      GLenum type, void* pixels, Bool applyPackImageParams) {
            PackedReadbackLayout packedLayout{};
            if (!GetPackedReadbackLayout(type, packedLayout) || packedLayout.byteSize != 4) {
                return false;
            }
            const SizeT srcRowBytes = static_cast<SizeT>(width) * 4;

            return StoreClientRows(4, packedLayout.byteSize, width, sliceHeight, sliceCount, pixels,
                                   applyPackImageParams,
                                   [&](GLsizei slice, GLsizei row, Uint8* dstRow) {
                                       const SizeT flatRow = static_cast<SizeT>(slice) *
                                                                 static_cast<SizeT>(sliceHeight) +
                                                             static_cast<SizeT>(row);
                                       Memcpy(dstRow, srcWords + flatRow * srcRowBytes, srcRowBytes);
                                   });
        }
    } // namespace ReadbackImpl
} // namespace MobileGL::MG_Backend::DirectGLES
