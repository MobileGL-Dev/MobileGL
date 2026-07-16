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

    bool IsIdentifierStart(char ch) {
        return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_';
    }

    MobileGL::String MaskCommentsAndQuotedText(const MobileGL::String& source) {
        enum class Region { Code, SingleLineComment, MultiLineComment, QuotedText };

        MobileGL::String masked = source;
        Region region = Region::Code;
        char quote = '\0';
        bool escaped = false;

        for (SizeT pos = 0; pos < source.size(); pos++) {
            const char ch = source[pos];
            const char next = pos + 1 < source.size() ? source[pos + 1] : '\0';

            if (region == Region::Code) {
                if (ch == '/' && next == '/') {
                    masked[pos] = ' ';
                    masked[pos + 1] = ' ';
                    pos++;
                    region = Region::SingleLineComment;
                } else if (ch == '/' && next == '*') {
                    masked[pos] = ' ';
                    masked[pos + 1] = ' ';
                    pos++;
                    region = Region::MultiLineComment;
                } else if (ch == '"' || ch == '\'') {
                    masked[pos] = ' ';
                    quote = ch;
                    escaped = false;
                    region = Region::QuotedText;
                }
                continue;
            }

            if (region == Region::SingleLineComment) {
                if (ch == '\n' || ch == '\r') {
                    region = Region::Code;
                } else {
                    masked[pos] = ' ';
                }
                continue;
            }

            if (region == Region::MultiLineComment) {
                if (ch == '*' && next == '/') {
                    masked[pos] = ' ';
                    masked[pos + 1] = ' ';
                    pos++;
                    region = Region::Code;
                } else if (ch != '\n' && ch != '\r') {
                    masked[pos] = ' ';
                }
                continue;
            }

            if (ch != '\n' && ch != '\r') {
                masked[pos] = ' ';
            }
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == quote) {
                region = Region::Code;
            }
        }

        return masked;
    }

    void SkipDirectiveWhitespace(const MobileGL::String& source, SizeT& pos, SizeT lineEnd) {
        while (pos < lineEnd && std::isspace(static_cast<unsigned char>(source[pos]))) {
            pos++;
        }
    }

    MobileGL::String ReadDirectiveIdentifier(const MobileGL::String& source, SizeT& pos, SizeT lineEnd) {
        if (pos >= lineEnd || !IsIdentifierStart(source[pos])) {
            return {};
        }

        const SizeT start = pos++;
        while (pos < lineEnd && IsIdentifierChar(source[pos])) {
            pos++;
        }
        return source.substr(start, pos - start);
    }

    bool HasUtf8Bom(const MobileGL::String& source) {
        return source.size() >= 3 && static_cast<unsigned char>(source[0]) == 0xef &&
               static_cast<unsigned char>(source[1]) == 0xbb && static_cast<unsigned char>(source[2]) == 0xbf;
    }

    struct ShaderLanguageInfo {
        unsigned version = 110;
        MobileGL::ShaderProfile profile = MobileGL::ShaderProfile::Core;
        SizeT versionDirectiveStart = MobileGL::String::npos;
        SizeT versionDirectiveEnd = MobileGL::String::npos;
        bool hasUtf8Bom = false;
        bool enablesGpuShader5 = false;

        bool HasVersionDirective() const { return versionDirectiveStart != MobileGL::String::npos; }
    };

    ShaderLanguageInfo InspectShaderLanguage(const MobileGL::String& source) {
        const MobileGL::String code = MaskCommentsAndQuotedText(source);
        ShaderLanguageInfo info;
        info.hasUtf8Bom = HasUtf8Bom(source);

        SizeT lineStart = 0;
        while (lineStart < code.size()) {
            SizeT lineEnd = code.find('\n', lineStart);
            const bool hasLineBreak = lineEnd != MobileGL::String::npos;
            if (!hasLineBreak) {
                lineEnd = code.size();
            }

            SizeT probe = lineStart;
            if (lineStart == 0 && info.hasUtf8Bom) {
                probe = 3;
            }
            SkipDirectiveWhitespace(code, probe, lineEnd);
            if (probe < lineEnd && code[probe] == '#') {
                const SizeT directiveStart = probe;
                probe++;
                SkipDirectiveWhitespace(code, probe, lineEnd);
                const MobileGL::String directive = ReadDirectiveIdentifier(code, probe, lineEnd);

                if (directive == "version" && !info.HasVersionDirective()) {
                    SkipDirectiveWhitespace(code, probe, lineEnd);
                    unsigned version = 0;
                    bool hasVersionDigits = false;
                    while (probe < lineEnd && code[probe] >= '0' && code[probe] <= '9') {
                        hasVersionDigits = true;
                        version = version * 10 + static_cast<unsigned>(code[probe] - '0');
                        probe++;
                    }
                    if (hasVersionDigits) {
                        info.version = version;
                        info.versionDirectiveStart = directiveStart;
                        info.versionDirectiveEnd = lineEnd + (hasLineBreak ? 1 : 0);
                        SkipDirectiveWhitespace(code, probe, lineEnd);
                        const MobileGL::String profile = ReadDirectiveIdentifier(code, probe, lineEnd);
                        if (profile == "es" || profile == "ES") {
                            info.profile = MobileGL::ShaderProfile::ES;
                        } else if (profile == "compatibility") {
                            info.profile = MobileGL::ShaderProfile::Compatibility;
                        } else {
                            info.profile = MobileGL::ShaderProfile::Core;
                        }
                    }
                } else if (directive == "extension") {
                    SkipDirectiveWhitespace(code, probe, lineEnd);
                    const MobileGL::String extension = ReadDirectiveIdentifier(code, probe, lineEnd);
                    SkipDirectiveWhitespace(code, probe, lineEnd);
                    if (probe < lineEnd && code[probe] == ':') {
                        probe++;
                        SkipDirectiveWhitespace(code, probe, lineEnd);
                        const MobileGL::String behavior = ReadDirectiveIdentifier(code, probe, lineEnd);
                        const bool isGpuShader5 = extension == "GL_ARB_gpu_shader5" ||
                                                  extension == "GL_NV_gpu_shader5";
                        const bool enablesExtension = behavior == "enable" || behavior == "require" ||
                                                      behavior == "warn";
                        // Gate the whole source if it ever opts into either extension. This is deliberately
                        // conservative around conditional directives and keeps legal sample qualifiers intact.
                        info.enablesGpuShader5 = info.enablesGpuShader5 || (isGpuShader5 && enablesExtension);
                    }
                }
            }

            lineStart = lineEnd + (hasLineBreak ? 1 : 0);
        }

        return info;
    }

    MobileGL::String GetNormalizedVersionDirective(const ShaderLanguageInfo& info) {
        if (info.profile == MobileGL::ShaderProfile::ES) {
            // Preserve the pre-existing behavior for standard lowercase "es" directives. MobileGL's Vulkan
            // glslang resource table cannot parse its ESSL built-ins today, even at ESSL 310, whereas the same
            // source is accepted through the normalized desktop core path.
            return "#version 460 core\n";
        }

        // Keep compatibility-profile handling on its pre-existing 460 path. Vulkan glslang does not accept that
        // profile today, and this legacy-sample fix must not broaden or otherwise alter that separate limitation.
        if (info.profile == MobileGL::ShaderProfile::Compatibility) {
            return "#version 460 compatibility\n";
        }

        const bool useLegacyDesktopVersion =
            info.version < 400 && !info.enablesGpuShader5;
        return useLegacyDesktopVersion ? "#version 330 core\n" : "#version 460 core\n";
    }

    void NormalizeVersionDirective(MobileGL::String& source, const ShaderLanguageInfo& info) {
        const MobileGL::String replacement = GetNormalizedVersionDirective(info);
        if (info.HasVersionDirective()) {
            source.replace(info.versionDirectiveStart, info.versionDirectiveEnd - info.versionDirectiveStart,
                           replacement);
            if (info.hasUtf8Bom) {
                source.erase(0, 3);
            }
            return;
        }

        if (info.hasUtf8Bom) {
            source.erase(0, 3);
        }
        source.insert(0, replacement);
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
        const ShaderLanguageInfo info = InspectShaderLanguage(source);
        return info.HasVersionDirective() ? info.versionDirectiveEnd : 0;
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

    // Rewrite the `packed` / `shared` block-packing qualifiers inside layout(...) declarations to
    // `std140`. Desktop GL leaves the memory layout of such blocks to the implementation and the
    // app must query member offsets; MobileGL's SPIR-V pipeline always lays uniform blocks out as
    // std140 (glslang under a SPIR-V target rejects `packed`/`shared` outright and SPIRV-Cross has
    // no other packing for UBOs), so std140 IS this implementation's chosen layout. Rewriting at
    // the source level keeps the validation compile, the reflection the app queries, and the
    // generated SPIR-V all agreeing on that choice. Both replacement tokens are 6 characters, so
    // the rewrite is done in place.
    void CoerceUniformBlockPackingToStd140(MobileGL::String& source) {
        constexpr const char* layoutToken = "layout";
        constexpr SizeT layoutLen = 6;

        SizeT pos = 0;
        while ((pos = source.find(layoutToken, pos)) != MobileGL::String::npos) {
            const bool hasLeftBoundary = pos == 0 || !IsIdentifierChar(source[pos - 1]);
            SizeT probe = pos + layoutLen;
            const bool hasRightBoundary = probe >= source.size() || !IsIdentifierChar(source[probe]);
            if (!hasLeftBoundary || !hasRightBoundary) {
                pos = probe;
                continue;
            }

            while (probe < source.size() && std::isspace(static_cast<unsigned char>(source[probe]))) {
                probe++;
            }
            if (probe >= source.size() || source[probe] != '(') {
                pos = probe;
                continue;
            }

            // Scan the qualifier list; layout qualifier values may contain parenthesized
            // constant expressions, so track nesting until the matching ')'.
            SizeT cursor = probe + 1;
            int depth = 1;
            while (cursor < source.size() && depth > 0) {
                const char ch = source[cursor];
                if (ch == '(') {
                    depth++;
                } else if (ch == ')') {
                    depth--;
                } else if (IsIdentifierChar(ch) && (cursor == 0 || !IsIdentifierChar(source[cursor - 1]))) {
                    SizeT identifierEnd = cursor;
                    while (identifierEnd < source.size() && IsIdentifierChar(source[identifierEnd])) {
                        identifierEnd++;
                    }
                    const SizeT identifierLen = identifierEnd - cursor;
                    if (identifierLen == 6 && (source.compare(cursor, 6, "packed") == 0 ||
                                               source.compare(cursor, 6, "shared") == 0)) {
                        source.replace(cursor, 6, "std140");
                    }
                    cursor = identifierEnd;
                    continue;
                }
                cursor++;
            }
            pos = cursor;
        }
    }

    void ModernizeLegacyGLSL(MobileGL::ShaderStage stage, MobileGL::String& source) {
        // Precision qualifiers (highp/mediump/lowp and default-precision statements) are legal and
        // ignored in the normalized desktop core profiles, so glslang handles them natively.

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
                // Normalize while the inspector's source span still refers to the untouched input. Later passes
                // remove comments and directives, so any subsequent insertion re-inspects the current source.
                const ShaderLanguageInfo originalLanguage = InspectShaderLanguage(source);
                NormalizeVersionDirective(source, originalLanguage);

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

                FilterUnsupportedGpuShaderInt64(source);
                CoerceUniformBlockPackingToStd140(source);

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
