// MobileGL - MobileGL/MG_Util/ShaderTranspiler/ShaderSourceProcessor.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "ShaderSourceProcessor.h"

#include <algorithm>
#include <cctype>
#include <MG_Backend/BackendObjects.h>

namespace {
    using MobileGL::SizeT;

    bool IsIdentifierChar(char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_';
    }

    bool HasSingleLineFunctionDefinition(const MobileGL::String& source, const MobileGL::String& functionName) {
        SizeT lineStart = 0;
        while (lineStart < source.size()) {
            SizeT lineEnd = source.find('\n', lineStart);
            if (lineEnd == MobileGL::String::npos) {
                lineEnd = source.size();
            }

            SizeT functionPos = source.find(functionName, lineStart);
            while (functionPos != MobileGL::String::npos && functionPos < lineEnd) {
                const bool hasLeftBoundary = functionPos == 0 || !IsIdentifierChar(source[functionPos - 1]);
                const SizeT functionEnd = functionPos + functionName.size();
                const bool hasRightBoundary = functionEnd >= source.size() || !IsIdentifierChar(source[functionEnd]);
                if (hasLeftBoundary && hasRightBoundary) {
                    SizeT probe = functionEnd;
                    while (probe < lineEnd && std::isspace(static_cast<unsigned char>(source[probe]))) {
                        probe++;
                    }
                    if (probe < lineEnd && source[probe] == '(') {
                        const SizeT closingParen = source.find(')', probe);
                        if (closingParen != MobileGL::String::npos && closingParen < lineEnd) {
                            probe = closingParen + 1;
                            while (probe < lineEnd && std::isspace(static_cast<unsigned char>(source[probe]))) {
                                probe++;
                            }
                            if (probe < lineEnd && source[probe] == '{') {
                                return true;
                            }
                        }
                    }
                }

                functionPos = source.find(functionName, functionPos + functionName.size());
            }

            lineStart = lineEnd + 1;
        }

        return false;
    }

    void RenameFunctionInvocations(MobileGL::String& source, const MobileGL::String& from, const MobileGL::String& to) {
        SizeT pos = 0;
        while ((pos = source.find(from, pos)) != MobileGL::String::npos) {
            const bool hasLeftBoundary = pos == 0 || !IsIdentifierChar(source[pos - 1]);
            const SizeT end = pos + from.size();
            const bool hasRightBoundary = end >= source.size() || !IsIdentifierChar(source[end]);

            SizeT probe = end;
            while (probe < source.size() && std::isspace(static_cast<unsigned char>(source[probe]))) {
                probe++;
            }

            if (hasLeftBoundary && hasRightBoundary && probe < source.size() && source[probe] == '(') {
                source.replace(pos, from.size(), to);
                pos += to.size();
                continue;
            }

            pos = end;
        }
    }

    void RenameBuiltinShadowingFunction(MobileGL::String& source, const char* from, const char* to) {
        const MobileGL::String fromName = from;
        if (!HasSingleLineFunctionDefinition(source, fromName)) {
            return;
        }

        RenameFunctionInvocations(source, fromName, to);
    }

    void ReplaceIdentifier(MobileGL::String& source, const MobileGL::String& from, const MobileGL::String& to) {
        SizeT pos = 0;
        while ((pos = source.find(from, pos)) != MobileGL::String::npos) {
            const bool hasLeftBoundary = pos == 0 || !IsIdentifierChar(source[pos - 1]);
            const SizeT end = pos + from.size();
            const bool hasRightBoundary = end >= source.size() || !IsIdentifierChar(source[end]);
            if (hasLeftBoundary && hasRightBoundary) {
                source.replace(pos, from.size(), to);
                pos += to.size();
            } else {
                pos = end;
            }
        }
    }

    void RemoveDefineForIdentifier(MobileGL::String& source, const MobileGL::String& identifier) {
        SizeT lineStart = 0;
        while (lineStart < source.size()) {
            SizeT lineEnd = source.find('\n', lineStart);
            const bool hasLineBreak = lineEnd != MobileGL::String::npos;
            if (!hasLineBreak) {
                lineEnd = source.size();
            }

            SizeT probe = lineStart;
            while (probe < lineEnd && std::isspace(static_cast<unsigned char>(source[probe]))) {
                probe++;
            }
            if (probe < lineEnd && source[probe] == '#') {
                probe++;
                while (probe < lineEnd && std::isspace(static_cast<unsigned char>(source[probe]))) {
                    probe++;
                }

                constexpr const char* defineToken = "define";
                constexpr SizeT defineLen = 6;
                const bool hasDefine = probe + defineLen <= lineEnd &&
                                       source.compare(probe, defineLen, defineToken) == 0 &&
                                       (probe + defineLen == lineEnd ||
                                        !IsIdentifierChar(source[probe + defineLen]));
                if (hasDefine) {
                    probe += defineLen;
                    while (probe < lineEnd && std::isspace(static_cast<unsigned char>(source[probe]))) {
                        probe++;
                    }

                    const bool hasIdentifier = probe + identifier.size() <= lineEnd &&
                                               source.compare(probe, identifier.size(), identifier) == 0 &&
                                               (probe + identifier.size() == lineEnd ||
                                                !IsIdentifierChar(source[probe + identifier.size()]));
                    if (hasIdentifier) {
                        source.erase(lineStart, lineEnd - lineStart + (hasLineBreak ? 1 : 0));
                        continue;
                    }
                }
            }

            lineStart = lineEnd + (hasLineBreak ? 1 : 0);
        }
    }

    SizeT FindAfterVersionDirective(const MobileGL::String& source) {
        const SizeT versionPos = source.find("#version");
        if (versionPos == MobileGL::String::npos) {
            return 0;
        }
        const SizeT lineEnd = source.find('\n', versionPos);
        return lineEnd == MobileGL::String::npos ? source.size() : lineEnd + 1;
    }

    bool IsExtensionAdvertised(MobileGL::GLExtension extension) {
        const auto& activeBackendObject = MobileGL::MG_Backend::pActiveBackendObject;
        if (!activeBackendObject) {
            return true;
        }

        const auto& extensions = activeBackendObject->GetRendererInfo().RendererGLInfo.Extensions;
        return std::find(extensions.begin(), extensions.end(), extension) != extensions.end();
    }

    MobileGL::String TrimDirectiveToken(const MobileGL::String& token) {
        SizeT start = 0;
        while (start < token.size() && std::isspace(static_cast<unsigned char>(token[start]))) {
            start++;
        }

        SizeT end = token.size();
        while (end > start && std::isspace(static_cast<unsigned char>(token[end - 1]))) {
            end--;
        }
        return token.substr(start, end - start);
    }

    void FilterUnsupportedGpuShaderInt64(MobileGL::String& source) {
        if (IsExtensionAdvertised(MobileGL::E_GL_ARB_gpu_shader_int64)) {
            return;
        }

        SizeT lineStart = 0;
        while (lineStart < source.size()) {
            SizeT lineEnd = source.find('\n', lineStart);
            const bool hasLineBreak = lineEnd != MobileGL::String::npos;
            if (!hasLineBreak) {
                lineEnd = source.size();
            }

            const MobileGL::String line = source.substr(lineStart, lineEnd - lineStart);
            SizeT probe = 0;
            while (probe < line.size() && std::isspace(static_cast<unsigned char>(line[probe]))) {
                probe++;
            }

            if (probe < line.size() && line[probe] == '#') {
                probe++;
                while (probe < line.size() && std::isspace(static_cast<unsigned char>(line[probe]))) {
                    probe++;
                }

                constexpr const char* extensionToken = "extension";
                constexpr SizeT extensionLen = 9;
                const bool hasExtensionDirective =
                    probe + extensionLen <= line.size() &&
                    line.compare(probe, extensionLen, extensionToken) == 0 &&
                    (probe + extensionLen == line.size() || !IsIdentifierChar(line[probe + extensionLen]));
                if (hasExtensionDirective) {
                    probe += extensionLen;
                    while (probe < line.size() && std::isspace(static_cast<unsigned char>(line[probe]))) {
                        probe++;
                    }

                    constexpr const char* int64Extension = "GL_ARB_gpu_shader_int64";
                    constexpr SizeT int64ExtensionLen = 23;
                    const bool hasInt64Extension =
                        probe + int64ExtensionLen <= line.size() &&
                        line.compare(probe, int64ExtensionLen, int64Extension) == 0 &&
                        (probe + int64ExtensionLen == line.size() ||
                         !IsIdentifierChar(line[probe + int64ExtensionLen]));
                    if (hasInt64Extension) {
                        probe += int64ExtensionLen;
                        while (probe < line.size() && std::isspace(static_cast<unsigned char>(line[probe]))) {
                            probe++;
                        }

                        if (probe < line.size() && line[probe] == ':') {
                            probe++;
                            const MobileGL::String behavior = TrimDirectiveToken(line.substr(probe));
                            const SizeT replaceLen = lineEnd - lineStart + (hasLineBreak ? 1 : 0);
                            if (behavior == "require") {
                                const MobileGL::String replacement =
                                    "#error GL_ARB_gpu_shader_int64 is not advertised by MobileGL\n";
                                source.replace(lineStart, replaceLen, replacement);
                                lineStart += replacement.size();
                            } else if (behavior == "enable" || behavior == "warn") {
                                source.replace(lineStart, replaceLen, "\n");
                                lineStart++;
                            } else {
                                lineStart = lineEnd + (hasLineBreak ? 1 : 0);
                            }
                            continue;
                        }
                    }
                }
            }

            lineStart = lineEnd + (hasLineBreak ? 1 : 0);
        }

        ReplaceIdentifier(source, "GL_ARB_gpu_shader_int64", "MG_DISABLED_GL_ARB_gpu_shader_int64");
    }

    void ModernizeLegacyGLSL(MobileGL::ShaderStage stage, MobileGL::String& source) {
        RemoveDefineForIdentifier(source, "HIGHP_OR_DEFAULT");
        RemoveDefineForIdentifier(source, "MEDIUMP_OR_DEFAULT");
        RemoveDefineForIdentifier(source, "LOWP_OR_DEFAULT");
        ReplaceIdentifier(source, "HIGHP_OR_DEFAULT", "");
        ReplaceIdentifier(source, "MEDIUMP_OR_DEFAULT", "");
        ReplaceIdentifier(source, "LOWP_OR_DEFAULT", "");
        ReplaceIdentifier(source, "highp", "");
        ReplaceIdentifier(source, "mediump", "");
        ReplaceIdentifier(source, "lowp", "");

        ReplaceIdentifier(source, "texture2D", "texture");
        ReplaceIdentifier(source, "texture2DProj", "textureProj");
        ReplaceIdentifier(source, "textureCube", "texture");
        ReplaceIdentifier(source, "texture3D", "texture");

        if (stage == MobileGL::ShaderStage::Vertex) {
            ReplaceIdentifier(source, "attribute", "in");
            ReplaceIdentifier(source, "varying", "out");
            return;
        }

        if (stage == MobileGL::ShaderStage::Fragment) {
            ReplaceIdentifier(source, "varying", "in");
            const bool usesFragColor = source.find("gl_FragColor") != MobileGL::String::npos;
            const bool usesFragData = source.find("gl_FragData") != MobileGL::String::npos;
            if (usesFragColor) {
                ReplaceIdentifier(source, "gl_FragColor", "mg_FragColor");
                source.insert(FindAfterVersionDirective(source), "out vec4 mg_FragColor;\n");
            }
            if (usesFragData) {
                ReplaceIdentifier(source, "gl_FragData", "mg_FragData");
                source.insert(FindAfterVersionDirective(source), "layout(location = 0) out vec4 mg_FragData[8];\n");
            }
        }
    }

    void InjectDepthRangeBuiltinShim(MobileGL::ShaderStage stage, MobileGL::String& source) {
        if (stage != MobileGL::ShaderStage::Fragment) return;
        if (source.find("gl_DepthRange") == MobileGL::String::npos) return;
        if (source.find("mg_DepthRangeParameters") != MobileGL::String::npos) return;

        constexpr const char* shim =
            "struct mg_DepthRangeParameters { float near; float far; float diff; };\n"
            "const mg_DepthRangeParameters mg_DepthRange = mg_DepthRangeParameters(0.0, 1.0, 1.0);\n"
            "#define gl_DepthRange mg_DepthRange\n";
        source.insert(FindAfterVersionDirective(source), shim);
    }
} // namespace

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            void PreprocessShaderSource(ShaderStage stage, String& source) {
                // remove multi-line comment
                size_t commentStartPos = source.find("/*");
                while (commentStartPos != String::npos) {
                    size_t commentEndPos = source.find("*/", commentStartPos);
                    if (commentEndPos == String::npos) {
                        source.erase(commentStartPos);
                        break;
                    }
                    // + length of "*/"
                    source = source.replace(commentStartPos, commentEndPos - commentStartPos + 2, "");
                    commentStartPos = source.find("/*", commentStartPos);
                }

                // remove #line directives
                SizeT linedirPos = source.find("#line");
                while (linedirPos != String::npos) {
                    SizeT newlinePos = source.find('\n', linedirPos);
                    if (newlinePos == String::npos) {
                        source.erase(linedirPos);
                        break;
                    }

                    // Preserve a line break so adjacent preprocessor directives do not merge.
                    source = source.replace(linedirPos, newlinePos - linedirPos + 1, "\n");
                    linedirPos = source.find("#line", linedirPos);
                }

                // remove "noperspective"
                const char* str_np = "noperspective";
                const SizeT len_np = strlen(str_np);
                SizeT noperspectivePos = source.find(str_np);
                while (noperspectivePos != String::npos) {
                    // + length of "\n"
                    source = source.replace(noperspectivePos, len_np, "");
                    noperspectivePos = source.find(str_np);
                }

                // force #version
                ShaderProfile profile = ShaderProfile::Core;
                SizeT versionPos = source.find("#version");
                SizeT lineEnd = source.find('\n', versionPos);

                if (versionPos != String::npos) {
                    String versionLine = source.substr(versionPos, lineEnd - versionPos);

                    if (versionLine.find("ES") != String::npos)
                        profile = ShaderProfile::ES;
                    else if (versionLine.find("compatibility") != String::npos)
                        profile = ShaderProfile::Compatibility;
                    else
                        profile = ShaderProfile::Core;
                } else {
                    profile = ShaderProfile::Core;
                    source.insert(0, "#version 460 core\n");
                    versionPos = 0;
                    lineEnd = source.find('\n', versionPos);
                }

                SizeT firstLineEnd = lineEnd;

                if (profile != ShaderProfile::ES) {
                    constexpr const char* versionDirectiveCore = "#version 460 core\n";
                    constexpr const char* versionDirectiveCompat = "#version 460 compatibility\n";

                    const char* replacement =
                        (profile == ShaderProfile::Compatibility) ? versionDirectiveCompat : versionDirectiveCore;

                    if (firstLineEnd != String::npos) {
                        source.replace(versionPos, firstLineEnd - versionPos + 1, replacement);
                    } else {
                        source = replacement;
                    }
                }

                FilterUnsupportedGpuShaderInt64(source);

                // Some shader packs define helpers with built-in GLSL names such as round(), tanh(), or fma().
                // These may pass OpenGL-style validation but fail when recompiled for Vulkan/SPIR-V generation.
                RenameBuiltinShadowingFunction(source, "round", "mg_round");
                RenameBuiltinShadowingFunction(source, "tanh", "mg_tanh");
                RenameBuiltinShadowingFunction(source, "fma", "mg_fma");
                RenameBuiltinShadowingFunction(source, "min3", "mg_min3");
                RenameBuiltinShadowingFunction(source, "max3", "mg_max3");
                ModernizeLegacyGLSL(stage, source);
                InjectDepthRangeBuiltinShim(stage, source);
            }

        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
