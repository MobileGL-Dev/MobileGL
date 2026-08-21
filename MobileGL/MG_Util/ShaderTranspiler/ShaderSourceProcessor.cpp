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
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <initializer_list>
#include <utility>
#include <Config.h>
#include <MG_Backend/BackendObjects.h>
#include <MG_Util/ShaderTranspiler/CompileEnv.h>
#include <MG_Util/ShaderTranspiler/Types.h>

#include "EsslBuiltinFunctionNames.h"

namespace {
    using MobileGL::SizeT;
    using MobileGL::String;
    using MobileGL::Uint32;
    using MobileGL::Vector;

    bool IsIdentifierChar(char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_';
    }

    bool IsIdentifierStart(char ch) {
        return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_';
    }

    // Return a copy of `source` with every comment and string-literal interior blanked to spaces.
    //
    // The passes that follow answer lexical questions ("is this identifier real code?", "where does
    // the #version line end?"), so comment and literal text has to stop being visible to them - but
    // it must not be *deleted*: replacing the bytes with spaces keeps every offset 1:1 with the
    // original, so an edit collected against the mask applies verbatim to the source, and keeping
    // newlines means glslang's diagnostics still point at the line the application wrote.
    //
    // It also has to be lexically stateful. A banner line such as
    //
    //     //*** lighting pass ***
    //
    // contains "/*" one byte in, and a naive search for that opener treats the rest of the file as
    // an unterminated comment.
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

            // GLSL has no multi-line string literals, so a quote that reaches end of line was never
            // a literal to begin with - most likely an apostrophe in a #error or #pragma message.
            // Ending the region here keeps one stray apostrophe from swallowing the rest of the file
            // for every consumer of this mask: the tokenizer, the #version inspection, and the
            // explicit-location / opaque-binding extractors all go blind past that point otherwise.
            if (ch == '\n' || ch == '\r') {
                region = Region::Code;
                continue;
            }

            masked[pos] = ' ';
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

    struct CodeToken {
        String text;
        SizeT begin = 0;
        SizeT end = 0;
    };

    Vector<CodeToken> TokenizeCode(const String& source) {
        const String masked = MaskCommentsAndQuotedText(source);
        Vector<CodeToken> tokens;
        tokens.reserve(source.size() / 4);

        SizeT pos = 0;
        while (pos < masked.size()) {
            const char ch = masked[pos];
            if (std::isspace(static_cast<unsigned char>(ch))) {
                ++pos;
                continue;
            }

            const SizeT begin = pos;
            if (IsIdentifierStart(ch)) {
                ++pos;
                while (pos < masked.size() && IsIdentifierChar(masked[pos])) {
                    ++pos;
                }
            } else if (std::isdigit(static_cast<unsigned char>(ch))) {
                ++pos;
                while (pos < masked.size()) {
                    const char numberChar = masked[pos];
                    if (!IsIdentifierChar(numberChar) && numberChar != '.') {
                        break;
                    }
                    ++pos;
                }
            } else {
                ++pos;
                if (pos < masked.size()) {
                    const String twoChars = masked.substr(begin, 2);
                    if (twoChars == "==" || twoChars == "!=" || twoChars == "<=" || twoChars == ">=" ||
                        twoChars == "+=" || twoChars == "-=" || twoChars == "<<" || twoChars == ">>" ||
                        twoChars == "++" || twoChars == "--" || twoChars == "&&" || twoChars == "||") {
                        ++pos;
                    }
                }
            }

            tokens.push_back(CodeToken{source.substr(begin, pos - begin), begin, pos});
        }
        return tokens;
    }

    bool IsIdentifierToken(const CodeToken& token) {
        if (token.text.empty() || !IsIdentifierStart(token.text.front())) {
            return false;
        }
        return std::all_of(token.text.begin() + 1, token.text.end(), IsIdentifierChar);
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

    // The GLSL versions MobileGL is willing to normalize. Anything else in a #version line - a number
    // that is not a real language version (329, 331), a bad profile keyword, a float/identifier where
    // the integer belongs, or trailing tokens - is left untouched so glslang rejects it, matching
    // KHR-GL33.shaders.preprocessor.directive.version_*. The set is deliberately generous (every real
    // desktop and ES version) so the normalizer never starts rejecting a form it used to accept.
    bool IsRecognizedGlslVersion(unsigned version) {
        switch (version) {
            case 100: case 110: case 120: case 130: case 140: case 150:
            case 300: case 310: case 320:
            case 330: case 400: case 410: case 420: case 430:
            case 440: case 450: case 460:
                return true;
            default:
                return false;
        }
    }

    struct ShaderLanguageInfo {
        unsigned version = 110;
        MobileGL::ShaderProfile profile = MobileGL::ShaderProfile::Core;
        SizeT versionDirectiveStart = MobileGL::String::npos;
        SizeT versionDirectiveEnd = MobileGL::String::npos;
        bool hasUtf8Bom = false;
        bool enablesGpuShader5 = false;
        // Whether the parsed #version directive is a well-formed one MobileGL should rewrite. A
        // malformed directive (see IsRecognizedGlslVersion) is left alone for glslang to reject.
        bool hasValidVersionDirective = false;

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
                        bool profileTokenValid = true;
                        if (profile.empty() || profile == "core") {
                            info.profile = MobileGL::ShaderProfile::Core;
                        } else if (profile == "es" || profile == "ES") {
                            info.profile = MobileGL::ShaderProfile::ES;
                        } else if (profile == "compatibility") {
                            info.profile = MobileGL::ShaderProfile::Compatibility;
                        } else {
                            // "#version 330 foo": an unrecognized profile keyword. Keep Core for any
                            // downstream routing, but mark the directive malformed.
                            info.profile = MobileGL::ShaderProfile::Core;
                            profileTokenValid = false;
                        }
                        // Comments are already masked to spaces, so anything non-blank left on the
                        // line is real trailing garbage: "#version 330 foobar" / "#version 330.0".
                        SkipDirectiveWhitespace(code, probe, lineEnd);
                        const bool hasTrailingTokens = probe < lineEnd;
                        info.hasValidVersionDirective =
                            IsRecognizedGlslVersion(info.version) && profileTokenValid && !hasTrailingTokens;
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

    // Stamped onto the normalized directive when a legacy (or absent) desktop
    // version was rewritten to 330; consumed by RetargetLegacyVersionDirectiveTo460.
    constexpr const char* kNormalizedLegacyMarker = "/*mobilegl-normalized-legacy*/";

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

        // An explicitly declared modern core version keeps its number: the GL CTS
        // negative-compile cases (reserved names, layout-qualifier forms, missing
        // overloads) rely on the declared version's rules, and raising it would
        // silently legalize them. gpu_shader5 opt-ins keep the 460 escalation -
        // Vulkan glslang's ARB_gpu_shader5 support is not complete enough alone.
        if (info.hasValidVersionDirective && info.version >= 330 && !info.enablesGpuShader5) {
            return "#version " + std::to_string(info.version) + " core\n";
        }

        const bool useLegacyDesktopVersion =
            info.version < 400 && !info.enablesGpuShader5;
        // The trailing marker records that this 330 came from a legacy declaration
        // (or none at all), so the compile-failure retry may re-raise it to 460.
        // An application's own "#version 330" never carries it and keeps strict
        // 3.30 semantics.
        return useLegacyDesktopVersion ? MobileGL::String("#version 330 core ") + kNormalizedLegacyMarker + "\n"
                                       : "#version 460 core\n";
    }

    // Rewrites the #version directive and returns the offset just past it in the rewritten source -
    // the anchor every later injection inserts at.
    //
    // The offset is returned rather than rediscovered because this function is the only place that
    // knows it for free; recovering it costs a whole-source mask plus a line scan
    // (FindAfterVersionDirective -> InspectShaderLanguage). Each branch below leaves the bytes
    // ahead of the directive untouched apart from the BOM erase, and each replacement text is
    // exactly one newline-terminated line, so the arithmetic is exact in all three cases.
    SizeT NormalizeVersionDirective(MobileGL::String& source, const ShaderLanguageInfo& info) {
        const SizeT bomBytes = info.hasUtf8Bom ? 3 : 0;

        // A malformed #version (329, 331, bad profile, float/trailing tokens) is left exactly as the
        // application wrote it so glslang rejects it - rewriting it to "#version 330 core" would
        // silently legalize the CTS directive.version_* rejection cases. Still drop a leading BOM so
        // the reported error is the bad version rather than a stray byte-order mark.
        if (info.HasVersionDirective() && !info.hasValidVersionDirective) {
            if (info.hasUtf8Bom) {
                source.erase(0, 3);
            }
            // The directive keeps its text and only slides left by the erased BOM.
            return info.versionDirectiveEnd - bomBytes;
        }

        const MobileGL::String replacement = GetNormalizedVersionDirective(info);
        if (info.HasVersionDirective()) {
            source.replace(info.versionDirectiveStart, info.versionDirectiveEnd - info.versionDirectiveStart,
                           replacement);
            if (info.hasUtf8Bom) {
                source.erase(0, 3);
            }
            // Only whitespace can precede the directive on its own line, so the replacement occupies
            // the whole rest of that line and ends it.
            return info.versionDirectiveStart - bomBytes + replacement.size();
        }

        if (info.hasUtf8Bom) {
            source.erase(0, 3);
        }
        source.insert(0, replacement);
        return replacement.size();
    }

    // Start of the physical line containing `offset`, never scanning before `lowerBound`.
    SizeT FindPhysicalLineStart(const MobileGL::String& source, SizeT offset, SizeT lowerBound) {
        if (offset == 0) {
            return lowerBound;
        }
        const SizeT newline = source.rfind('\n', offset - 1);
        if (newline == MobileGL::String::npos || newline + 1 < lowerBound) {
            return lowerBound;
        }
        return newline + 1;
    }

    // Half-open [begin, end) byte ranges of the preprocessor directive lines, in source order.
    // A directive is one logical line: a trailing backslash splices the next physical line into it.
    Vector<std::pair<SizeT, SizeT>> FindDirectiveLineRanges(const MobileGL::String& source) {
        Vector<std::pair<SizeT, SizeT>> ranges;

        SizeT lineStart = 0;
        while (lineStart < source.size()) {
            SizeT lineEnd = source.find('\n', lineStart);
            if (lineEnd == MobileGL::String::npos) {
                lineEnd = source.size();
            }

            SizeT probe = lineStart;
            while (probe < lineEnd && std::isspace(static_cast<unsigned char>(source[probe]))) {
                probe++;
            }
            if (probe >= lineEnd || source[probe] != '#') {
                lineStart = lineEnd + 1;
                continue;
            }

            SizeT directiveEnd = lineEnd;
            while (directiveEnd < source.size()) {
                // directiveEnd sits on a '\n'; a backslash immediately before it (modulo the \r of
                // a CRLF file and trailing blanks) splices the following physical line in.
                // The scan must not leave the physical line that directiveEnd terminates: a
                // whitespace-only spliced line would otherwise let the back-scan reach the
                // backslash of the PREVIOUS line and swallow one extra real line of code.
                const SizeT physicalLineStart = FindPhysicalLineStart(source, directiveEnd, lineStart);
                SizeT back = directiveEnd;
                while (back > physicalLineStart && std::isspace(static_cast<unsigned char>(source[back - 1]))) {
                    back--;
                }
                if (back == physicalLineStart || source[back - 1] != '\\') {
                    break;
                }
                SizeT splicedEnd = source.find('\n', directiveEnd + 1);
                if (splicedEnd == MobileGL::String::npos) {
                    splicedEnd = source.size();
                }
                directiveEnd = splicedEnd;
            }

            ranges.push_back({lineStart, directiveEnd});
            lineStart = directiveEnd + 1;
        }

        return ranges;
    }

    bool IsInDirectiveLine(const Vector<std::pair<SizeT, SizeT>>& ranges, SizeT offset) {
        // Ranges are disjoint and sorted, so the only candidate is the last one starting at or
        // before the offset.
        const auto next = std::upper_bound(ranges.begin(), ranges.end(), offset,
                                           [](SizeT value, const std::pair<SizeT, SizeT>& range) {
                                               return value < range.first;
                                           });
        return next != ranges.begin() && offset < std::prev(next)->second;
    }

    // No GLSL type name is a statement keyword, so "<keyword> <builtin> (" is never a definition -
    // it is `return clamp(...)`, `else round(...)`, `do fma(...)`, a `case` label expression. The
    // if/for/while/switch entries cannot precede a call in valid GLSL either (a '(' always follows
    // them directly), and are listed defensively. Sorted for std::binary_search.
    constexpr std::string_view kStatementKeywordsBeforeCall[] = {
        "case", "do", "else", "for", "if", "return", "switch", "while",
    };

    bool IsStatementKeywordToken(const CodeToken& token) {
        return std::binary_search(std::begin(kStatementKeywordsBeforeCall),
                                  std::end(kStatementKeywordsBeforeCall), std::string_view(token.text));
    }

    // A brace counter over raw tokens is preprocessor-blind: it counts the braces of BOTH arms of
    // an #ifdef, so the classic "early return inside one arm, closing brace in each arm" idiom
    // desyncs it. A desynced depth turns statements into apparent top-level definitions, and an
    // over-detection is unrecoverable (the source never reaches the SPIR-V backstop). A file whose
    // braces do not net to zero, or whose running depth ever dips below zero, is therefore not
    // trustworthy for depth-based detection at all.
    bool HasBalancedBraces(const Vector<CodeToken>& tokens) {
        SizeT depth = 0;
        for (const CodeToken& token : tokens) {
            if (token.text.size() != 1) continue;
            if (token.text[0] == '{') {
                depth++;
            } else if (token.text[0] == '}') {
                if (depth == 0) return false;
                depth--;
            }
        }
        return depth == 0;
    }

    // Some shader packs define their own helpers under builtin GLSL names - round(), fma(),
    // min3(), tanh(). Desktop GLSL allows that shadowing; ESSL 3.x forbids the redefinition, so
    // every such helper is renamed to mg_<name> together with all of its call sites.
    //
    // Scope is deliberately NARROW: only kLexicalPreemptRenameNames, the handful of names whose
    // shadowing definitions glslang's relaxed parse rejects outright ("overloaded functions must
    // have the same parameter precision qualifiers"), or which need an extension the declared
    // #version does not enable (fma() at #version 330 wants GL_ARB_gpu_shader5). Those shaders
    // never produce SPIR-V, so only a source-level rename can save them. Everything else is left
    // to the SPIR-V OpName pass in SanitizeAndOptimizeBinary, which is safe by construction -
    // see EsslBuiltinFunctionNames.h for the full failure-layer split. A lexical scan is
    // preprocessor-blind and overload-blind, so widening this table trades a rescue nobody needs
    // for an unrecoverable over-detection risk on every shader that merely calls the builtin.
    //
    // Cost: ONE tokenize for the whole job, and nothing further at all in the overwhelmingly
    // common no-shadowing case. The path this replaces probed the entire source once per
    // candidate name, which measured ~68% of a Complementary-scale pack's compile time.
    void RenameBuiltinShadowingFunctions(MobileGL::String& source) {
        const Vector<CodeToken> tokens = TokenizeCode(source);
        if (tokens.size() < 3) {
            return;
        }
        // Desynced depth -> skip the lexical half entirely and let the backstop handle whatever
        // this file shadows. Missing a definition is recoverable; inventing one is not.
        if (!HasBalancedBraces(tokens)) {
            return;
        }
        const Vector<std::pair<SizeT, SizeT>> directiveRanges = FindDirectiveLineRanges(source);

        // Pass A - collect the shadowed names. A definition or prototype at brace depth 0 reads
        // as "<type-identifier> <builtin-name> (", which is what separates it from a call in a
        // global initializer ("const float PI = radians(180.0);", where the previous token is '=').
        // Token positions ignore layout, so a definition split across lines is found the same way.
        Vector<MobileGL::String> shadowedNames;
        SizeT braceDepth = 0;
        for (SizeT i = 0; i + 1 < tokens.size(); i++) {
            const CodeToken& token = tokens[i];
            if (token.text.size() == 1) {
                if (token.text[0] == '{') {
                    braceDepth++;
                    continue;
                }
                if (token.text[0] == '}') {
                    if (braceDepth > 0) braceDepth--;
                    continue;
                }
            }
            if (braceDepth != 0 || i == 0 || tokens[i + 1].text != "(" || !IsIdentifierToken(tokens[i - 1])) {
                continue;
            }
            // IsIdentifierToken is purely lexical, so "return"/"else"/"do"/"case" pass it. None of
            // them is a return type, so "return round(x)" is a CALL, not a definition.
            // A directive tail ('#endif' tokenizes to '#' + 'endif') is not a return type;
            // without this, a balanced-but-desynced file could see it as one.
            if (IsInDirectiveLine(directiveRanges, tokens[i - 1].begin)) {
                continue;
            }
            if (IsStatementKeywordToken(tokens[i - 1])) {
                continue;
            }
            // "#define FOO fma(x, y, z)" defines FOO, not fma.
            if (!MobileGL::MG_Util::ShaderTranspiler::IsLexicalPreemptRenameName(token.text) ||
                IsInDirectiveLine(directiveRanges, token.begin)) {
                continue;
            }
            if (std::find(shadowedNames.begin(), shadowedNames.end(), token.text) == shadowedNames.end()) {
                shadowedNames.push_back(token.text);
            }
        }

        if (shadowedNames.empty()) {
            return;
        }

        // Pass B - rename the definition, its prototypes and every call. Only a name followed by
        // '(' is the function; the same spelling as a variable must keep its own identity.
        // Directive lines DO participate: a macro body calling the renamed helper has to follow it.
        Vector<SizeT> insertOffsets;
        for (SizeT i = 0; i + 1 < tokens.size(); i++) {
            if (tokens[i + 1].text != "(") {
                continue;
            }
            if (std::find(shadowedNames.begin(), shadowedNames.end(), tokens[i].text) != shadowedNames.end()) {
                insertOffsets.push_back(tokens[i].begin);
            }
        }
        // Back to front, so each recorded offset is still valid when it is used.
        for (auto offset = insertOffsets.rbegin(); offset != insertOffsets.rend(); ++offset) {
            source.insert(*offset, "mg_");
        }
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

    SizeT FindAfterVersionDirective(const MobileGL::String& source) {
        const ShaderLanguageInfo info = InspectShaderLanguage(source);
        return info.HasVersionDirective() ? info.versionDirectiveEnd : 0;
    }

    // Holds the offset just past the #version directive - the anchor every injected declaration is
    // inserted at - across the passes of one PreprocessShaderSource call.
    //
    // Four consumers want that one number, and each used to buy it with its own
    // FindAfterVersionDirective, i.e. its own whole-source mask plus line scan. Taking it once and
    // handing it down turns up to five InspectShaderLanguage sweeps per compile into one.
    //
    // It stays EXACT rather than merely cached. The memo is handed out only while the bytes ahead
    // of the anchor are byte-for-byte what they were when it was taken, and that is precisely the
    // condition under which a fresh FindAfterVersionDirective returns the same answer: the whole
    // version line, and every line the scan looks at before reaching it, lies inside that prefix,
    // so an unchanged prefix means the same directive is still found ending at the same offset.
    // The guard is load-bearing, not decoration - passes really do rewrite ahead of the anchor.
    // NormalizeLineDirectives deletes #line directives that precede the version line, and
    // ModernizeLegacyGLSL's ReplaceIdentifier is raw text and so rewrites inside a leading comment
    // banner. When the guard trips the offset is simply recomputed, which is the pre-memo behavior.
    //
    // The one-argument constructor is that pre-memo behavior in full, for any caller that has a
    // source but no anchor to hand.
    class AfterVersionAnchor {
    public:
        explicit AfterVersionAnchor(const MobileGL::String& source) { Recompute(source); }
        AfterVersionAnchor(const MobileGL::String& source, SizeT offset) { Adopt(source, offset); }

        SizeT Get(const MobileGL::String& source) {
            if (source.size() < m_offset || source.compare(0, m_offset, m_prefix) != 0) {
                Recompute(source);
            }
            return m_offset;
        }

    private:
        void Recompute(const MobileGL::String& source) { Adopt(source, FindAfterVersionDirective(source)); }

        void Adopt(const MobileGL::String& source, SizeT offset) {
            m_offset = offset;
            m_prefix.assign(source, 0, offset);
        }

        SizeT m_offset = 0;
        MobileGL::String m_prefix;
    };

    // GLSL's #line takes integer expressions only, but plenty of shader-pack preprocessors emit the
    // C form with a quoted filename. Deleting every #line outright made those harmless - at the cost
    // of __LINE__ reporting the position in MobileGL's rewritten text rather than the one the pack
    // author wrote, and of every later diagnostic pointing at the wrong line. Dropping just the
    // quoted operand keeps the directive doing its job and still hands glslang something it accepts.
    //
    // `versionEnd` is the after-version anchor for the current `source` (AfterVersionAnchor::Get);
    // this pass only reads the source ahead of its own rewrites, so the plain offset is enough.
    void NormalizeLineDirectives(MobileGL::String& source, SizeT versionEnd) {
        const MobileGL::String masked = MaskCommentsAndQuotedText(source);
        MobileGL::String result;
        result.reserve(source.size());

        SizeT lineStart = 0;
        while (lineStart <= source.size()) {
            SizeT lineEnd = source.find('\n', lineStart);
            const bool lastLine = lineEnd == MobileGL::String::npos;
            if (lastLine) lineEnd = source.size();

            SizeT probe = lineStart;
            while (probe < lineEnd && (source[probe] == ' ' || source[probe] == '\t')) probe++;

            const bool isLineDirective = masked.compare(probe, 5, "#line") == 0 &&
                                         (probe + 5 >= lineEnd || !IsIdentifierChar(source[probe + 5]));
            if (isLineDirective && lineStart < versionEnd) {
                // #version has to be the first token in the shader, so a #line ahead of it could
                // never have taken effect. Drop it rather than hand glslang a source it must reject
                // - some pack preprocessors emit their directives before the version line.
            } else if (isLineDirective) {
                // Keep everything up to the first quote that the masker identified as string text.
                SizeT quotePos = MobileGL::String::npos;
                for (SizeT i = probe + 5; i < lineEnd; i++) {
                    if (source[i] == '"' || source[i] == '\'') {
                        quotePos = i;
                        break;
                    }
                }
                if (quotePos != MobileGL::String::npos) {
                    result.append(source, lineStart, quotePos - lineStart);
                } else {
                    result.append(source, lineStart, lineEnd - lineStart);
                }
            } else {
                result.append(source, lineStart, lineEnd - lineStart);
            }

            if (lastLine) break;
            result.push_back('\n');
            lineStart = lineEnd + 1;
        }

        source = std::move(result);
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

    void FilterUnsupportedGpuShaderInt64(const MobileGL::MG_Util::ShaderTranspiler::CompileEnv& env,
                                         MobileGL::String& source) {
        if (env.IsExtensionAdvertised(MobileGL::E_GL_ARB_gpu_shader_int64)) {
            return;
        }

        // Detect the directive on a comment/string-masked copy so a commented-out
        // "#extension GL_ARB_gpu_shader_int64" is never turned into a synthesized #error. Comments are
        // no longer blanked in the delivered source (glslang handles them), so this pass must mask
        // locally like its siblings. Masking preserves offsets, so edits collected against the scan
        // apply verbatim to `source`; they are applied back-to-front to keep earlier offsets valid.
        const MobileGL::String scan = MaskCommentsAndQuotedText(source);
        struct DirectiveEdit {
            SizeT pos;
            SizeT len;
            MobileGL::String replacement;
        };
        Vector<DirectiveEdit> edits;

        SizeT lineStart = 0;
        while (lineStart < scan.size()) {
            SizeT lineEnd = scan.find('\n', lineStart);
            const bool hasLineBreak = lineEnd != MobileGL::String::npos;
            if (!hasLineBreak) {
                lineEnd = scan.size();
            }

            const MobileGL::String line = scan.substr(lineStart, lineEnd - lineStart);
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
                                edits.push_back({lineStart, replaceLen,
                                                 "#error GL_ARB_gpu_shader_int64 is not advertised by MobileGL\n"});
                            } else if (behavior == "enable" || behavior == "warn") {
                                edits.push_back({lineStart, replaceLen, "\n"});
                            }
                            lineStart = lineEnd + (hasLineBreak ? 1 : 0);
                            continue;
                        }
                    }
                }
            }

            lineStart = lineEnd + (hasLineBreak ? 1 : 0);
        }

        for (auto it = edits.rbegin(); it != edits.rend(); ++it) {
            source.replace(it->pos, it->len, it->replacement);
        }

        ReplaceIdentifier(source, "GL_ARB_gpu_shader_int64", "MG_DISABLED_GL_ARB_gpu_shader_int64");
    }

    // GLSL 4.30 4.1.9 allows an interface-block member array to be left unsized when it is NOT the
    // last member; it is then implicitly sized by the largest constant index the shader uses.
    // glslang implements the SIZING - adoptImplicitArraySizes, at link - but computes the block's
    // member OFFSETS at DECLARATION time (fixBlockUniformOffsets), where the array is still
    // unsized and so contributes zero bytes. Every member after it is therefore laid out on top of
    // it: `vec4 a[]; vec4 b;` puts BOTH at offset 0, and a shader reading `b` gets `a[0]`
    // (KHR-GL43.shader_storage_buffer_object.basic-syntax iteration 6, whose degenerate triangle
    // rasterizes nothing at all).
    //
    // The source level is the only place the two can be reconciled, because the offset pass runs
    // before a single statement has been parsed. Deliberately narrow: it fires only on a `buffer`
    // block (no other block kind may hold an unsized member at all), only on a member that is not
    // the last one, and only when every subscript of that member's name in the source is a decimal
    // literal. Anything outside that shape is left exactly as it was - and the shape itself has no
    // correct behaviour today, so the rewrite cannot take a working case away.
    void SizeNonFinalUnsizedBufferBlockMembers(MobileGL::String& source) {
        // Both tokens must be present for the shape to exist, and "[]" is absent from essentially
        // every real shader source, so this is the whole cost for them.
        if (source.find("[]") == MobileGL::String::npos || source.find("buffer") == MobileGL::String::npos) {
            return;
        }

        const auto isDecimalInteger = [](const String& text) {
            return !text.empty() && std::all_of(text.begin(), text.end(), [](char ch) {
                       return ch >= '0' && ch <= '9';
                   });
        };

        const Vector<CodeToken> tokens = TokenizeCode(source);
        const SizeT count = tokens.size();

        // Pass 1: for every identifier, the largest literal index it is subscripted with (as a
        // count, i.e. index + 1), or -1 once it is subscripted with anything that is not a literal.
        // The declaration's own empty `[]` is neither.
        MobileGL::UnorderedMap<String, long long> subscriptExtent;
        for (SizeT i = 1; i < count; ++i) {
            if (tokens[i].text != "[" || !IsIdentifierToken(tokens[i - 1])) continue;
            if (i + 1 < count && tokens[i + 1].text == "]") continue; // the unsized declarator itself
            long long& extent = subscriptExtent[tokens[i - 1].text];
            if (i + 2 < count && isDecimalInteger(tokens[i + 1].text) && tokens[i + 2].text == "]") {
                if (extent >= 0) {
                    extent = std::max(extent, std::strtoll(tokens[i + 1].text.c_str(), nullptr, 10) + 1);
                }
            } else {
                extent = -1;
            }
        }

        // Pass 2: one edit per repairable member, applied back to front so earlier offsets stand.
        struct SizeEdit {
            SizeT pos;
            String text;
        };
        Vector<SizeEdit> edits;
        for (SizeT i = 0; i < count; ++i) {
            if (tokens[i].text != "buffer") continue;
            SizeT cursor = i + 1;
            // `buffer` is also a member MEMORY qualifier ("buffer vec4 position0;"), which is why
            // the block body has to be found rather than assumed.
            if (cursor < count && IsIdentifierToken(tokens[cursor])) ++cursor;
            if (cursor >= count || tokens[cursor].text != "{") continue;

            const SizeT bodyBegin = cursor + 1;
            SizeT bodyEnd = bodyBegin;
            int depth = 1;
            while (bodyEnd < count) {
                if (tokens[bodyEnd].text == "{") {
                    ++depth;
                } else if (tokens[bodyEnd].text == "}") {
                    --depth;
                    if (depth == 0) break;
                }
                ++bodyEnd;
            }
            if (depth != 0) continue; // unterminated; glslang will have the last word

            Vector<std::pair<SizeT, SizeT>> members; // [begin, end) of each member, ';' excluded
            SizeT memberBegin = bodyBegin;
            for (SizeT m = bodyBegin; m < bodyEnd; ++m) {
                if (tokens[m].text != ";") continue;
                members.emplace_back(memberBegin, m);
                memberBegin = m + 1;
            }

            // The LAST member is deliberately untouched: an unsized array there is a run-time
            // sized array, which is both legal and correctly laid out already.
            for (SizeT index = 0; index + 1 < members.size(); ++index) {
                const SizeT begin = members[index].first;
                const SizeT end = members[index].second;
                if (end < begin + 3) continue;
                if (tokens[end - 1].text != "]" || tokens[end - 2].text != "[") continue;
                if (!IsIdentifierToken(tokens[end - 3])) continue;
                // A multi-declarator member would need one size per declarator; out of scope.
                bool multipleDeclarators = false;
                for (SizeT t = begin; t < end; ++t) {
                    if (tokens[t].text == ",") multipleDeclarators = true;
                }
                if (multipleDeclarators) continue;
                const auto known = subscriptExtent.find(tokens[end - 3].text);
                if (known == subscriptExtent.end() || known->second <= 0) continue;
                edits.push_back({tokens[end - 1].begin, std::to_string(known->second)});
            }
            i = bodyEnd;
        }

        for (auto it = edits.rbegin(); it != edits.rend(); ++it) {
            source.insert(it->pos, it->text);
        }
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

    // `afterVersion` tracks the anchor the two injections below insert at. It is passed as the
    // tracker rather than a bare offset because this pass rewrites identifiers first, and those
    // rewrites are raw text: a leading comment banner mentioning `varying` or `texture2D` moves the
    // anchor, and the tracker notices.
    void ModernizeLegacyGLSL(MobileGL::ShaderStage stage, MobileGL::String& source,
                             AfterVersionAnchor& afterVersion) {
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
                source.insert(afterVersion.Get(source), "out vec4 mg_FragColor;\n");
            }
            if (usesFragData) {
                ReplaceIdentifier(source, "gl_FragData", "mg_FragData");
                source.insert(afterVersion.Get(source), "layout(location = 0) out vec4 mg_FragData[8];\n");
            }
        }
    }

    void InjectDepthRangeBuiltinShim(MobileGL::ShaderStage stage, MobileGL::String& source,
                                     AfterVersionAnchor& afterVersion) {
        if (stage != MobileGL::ShaderStage::Fragment) return;
        if (source.find("gl_DepthRange") == MobileGL::String::npos) return;
        if (source.find("mg_DepthRangeParameters") != MobileGL::String::npos) return;

        constexpr const char* shim =
            "struct mg_DepthRangeParameters { float near; float far; float diff; };\n"
            "const mg_DepthRangeParameters mg_DepthRange = mg_DepthRangeParameters(0.0, 1.0, 1.0);\n"
            "#define gl_DepthRange mg_DepthRange\n";
        source.insert(afterVersion.Get(source), shim);
    }
} // namespace

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            void PreprocessShaderSource(ShaderStage stage, String& source) {
                PreprocessShaderSource(stage, source, *GetCurrentCompileEnv());
            }

            void PreprocessShaderSource(ShaderStage stage, String& source, const CompileEnv& env) {
                // Normalize while the inspector's source span still refers to the untouched input.
                const ShaderLanguageInfo originalLanguage = InspectShaderLanguage(source);

                // Four passes below inject just past the #version directive, and each of them used
                // to locate that anchor for itself - a whole-source mask plus line scan apiece, up
                // to five per compile for one offset. NormalizeVersionDirective hands back the
                // anchor it just created and the tracker keeps it honest from there.
                AfterVersionAnchor afterVersion(source, NormalizeVersionDirective(source, originalLanguage));

                // Comments are left intact for glslang's own preprocessor: a block comment is a single
                // preprocessing token that collapses to one space even across newlines and inside a
                // directive, so blanking it here (which preserved the interior newlines) truncated
                // multi-line #define bodies and broke otherwise-valid shaders (KHR-GL3x.shaders.
                // preprocessor multiline_comment_define / redefine_object / function_redefinition).
                // Every MobileGL pass that must ignore comment/string text already masks them locally
                // via MaskCommentsAndQuotedText/TokenizeCode, so the source we hand glslang keeps them.
                NormalizeLineDirectives(source, afterVersion.Get(source));

                // noperspective is intentionally NOT touched here. It is core in desktop GLSL (1.30+)
                // and maps to the core SPIR-V NoPerspective decoration, which DirectVulkan renders
                // natively and SPIRV-Cross turns into ESSL `noperspective` + the
                // GL_NV_shader_noperspective_interpolation extension. The old naked substring erase
                // both discarded that interpolation (shader packs need it) and corrupted any
                // identifier that merely contained the word. The GLES fallback for devices without
                // the extension lives in the backend, where device capabilities are known.

                FilterUnsupportedGpuShaderInt64(env, source);
                CoerceUniformBlockPackingToStd140(source);
                // After the packing coercion: that one rewrites `packed`/`shared` in place and so
                // cannot move an offset this pass depends on, and reading the block declarations
                // once both qualifiers are normalized keeps the two passes' notions of a block
                // declaration identical.
                SizeNonFinalUnsizedBufferBlockMembers(source);

                RenameBuiltinShadowingFunctions(source);

                ModernizeLegacyGLSL(stage, source, afterVersion);
                InjectDepthRangeBuiltinShim(stage, source, afterVersion);

            }

            Bool RetargetLegacyVersionDirectiveTo460(String& source) {
                // Re-inspect rather than searching for the literal directive: it is not necessarily at
                // offset 0 (a BOM or comments may precede it) and a commented-out "#version" elsewhere
                // must not be mistaken for the real one.
                const ShaderLanguageInfo info = InspectShaderLanguage(source);
                if (!info.HasVersionDirective()) return false;
                // Never rescue a malformed directive to 460: that is precisely what re-legalized the
                // CTS directive.version_* rejection cases after the first compile failed. The shader-
                // pack retry this exists for only ever sees a valid low version (a real "#version 330").
                if (!info.hasValidVersionDirective) return false;
                // Only the set NormalizeVersionDirective downgraded: desktop core below 400. ES and
                // compatibility shaders keep whatever they declared.
                if (info.profile != ShaderProfile::Core || info.version >= 400) return false;
                // Only rescue MobileGL's own legacy normalization (marked on the directive line).
                // An application-declared "#version 330" keeps strict 3.30 semantics: raising it
                // would re-legalize the CTS negative-compile cases (reserved names, arrays of
                // arrays, missing overloads).
                SizeT lineEnd = source.find('\n', info.versionDirectiveStart);
                if (lineEnd == MobileGL::String::npos) {
                    lineEnd = source.size();
                }
                const SizeT markerPos = source.find(kNormalizedLegacyMarker, info.versionDirectiveStart);
                if (markerPos == MobileGL::String::npos || markerPos > lineEnd) {
                    return false;
                }

                source.replace(info.versionDirectiveStart, info.versionDirectiveEnd - info.versionDirectiveStart,
                               "#version 460 core\n");
                return true;
            }

            std::optional<String> FindReservedIdentifierViolation(const String& source) {
                // Reserved anywhere; glslang accepts them as plain identifiers.
                static constexpr const char* kAlwaysReserved[] = {
                    "image1DShadow",
                    "image2DShadow",
                    "image1DArrayShadow",
                    "image2DArrayShadow",
                };
                // Keywords legal only inside a layout(...) qualifier list.
                static constexpr const char* kLayoutOnlyKeywords[] = {
                    "packed",
                    "row_major",
                };

                const auto isIdentChar = [](char c) {
                    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
                };

                const SizeT length = source.size();
                SizeT i = 0;
                Int layoutParenDepth = 0;   // >0 while inside layout(...)
                Bool pendingLayoutParen = false; // saw "layout", awaiting its '('
                while (i < length) {
                    const char c = source[i];
                    // Comments.
                    if (c == '/' && i + 1 < length && source[i + 1] == '/') {
                        while (i < length && source[i] != '\n') ++i;
                        continue;
                    }
                    if (c == '/' && i + 1 < length && source[i + 1] == '*') {
                        i += 2;
                        while (i + 1 < length && !(source[i] == '*' && source[i + 1] == '/')) ++i;
                        i = (i + 1 < length) ? i + 2 : length;
                        continue;
                    }
                    // Preprocessor lines stay out of scope (macro names may shadow anything).
                    if (c == '#' && (i == 0 || source[i - 1] == '\n' ||
                                     source.find_last_not_of(" \t", i - 1) == MobileGL::String::npos ||
                                     source[source.find_last_not_of(" \t", i - 1)] == '\n')) {
                        while (i < length && source[i] != '\n') {
                            if (source[i] == '\\' && i + 1 < length && source[i + 1] == '\n') ++i;
                            ++i;
                        }
                        continue;
                    }
                    if (c == '(') {
                        if (pendingLayoutParen) {
                            layoutParenDepth = 1;
                            pendingLayoutParen = false;
                        } else if (layoutParenDepth > 0) {
                            ++layoutParenDepth;
                        }
                        ++i;
                        continue;
                    }
                    if (c == ')') {
                        if (layoutParenDepth > 0) --layoutParenDepth;
                        ++i;
                        continue;
                    }
                    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                        ++i;
                        continue;
                    }
                    if (isIdentChar(c) && !(c >= '0' && c <= '9')) {
                        const SizeT start = i;
                        while (i < length && isIdentChar(source[i])) ++i;
                        const StringView word(source.data() + start, i - start);
                        if (word == "layout") {
                            pendingLayoutParen = true;
                            continue;
                        }
                        pendingLayoutParen = false;
                        for (const char* reserved : kAlwaysReserved) {
                            if (word == reserved) {
                                return String("ERROR: reserved identifier '") + reserved + "' may not be used.";
                            }
                        }
                        if (layoutParenDepth == 0) {
                            for (const char* keyword : kLayoutOnlyKeywords) {
                                if (word == keyword) {
                                    return String("ERROR: '") + keyword +
                                        "' is a keyword and may not be used as an identifier.";
                                }
                            }
                        }
                        continue;
                    }
                    if (isIdentChar(c)) { // digit-led token: skip the whole number/identifier tail
                        while (i < length && isIdentChar(source[i])) ++i;
                        pendingLayoutParen = false;
                        continue;
                    }
                    pendingLayoutParen = false;
                    ++i;
                }
                return std::nullopt;
            }

            namespace {
                bool IsNonLayoutQualifierKeyword(const String& text) {
                    static const char* kQualifiers[] = {
                        "highp",    "mediump",  "lowp",     "precise",  "const",    "flat",
                        "noperspective", "smooth", "centroid", "sample", "patch",   "invariant",
                        "coherent", "volatile", "restrict", "readonly", "writeonly", "subroutine",
                    };
                    for (const char* qualifier : kQualifiers) {
                        if (text == qualifier) return true;
                    }
                    return false;
                }

                // One GLSL integer literal, spelled the C way: "0x"/"0X" is hexadecimal, a leading
                // '0' is OCTAL, everything else decimal, and a single trailing 'u'/'U' is legal.
                // strtoll with base 0 already implements exactly that detection, so the only work
                // here is deciding what the tail is allowed to be.
                //
                // Never guesses, which is the discipline every caller depends on: a float ("1.0"),
                // an unknown suffix ("3f"), an out-of-range run and a negative value all return
                // false, and the caller skips the declaration rather than recording a wrong number.
                bool ParseGlslIntegerLiteral(const String& text, long long& out) {
                    if (text.empty() || text.front() < '0' || text.front() > '9') return false;
                    errno = 0;
                    char* tail = nullptr;
                    const long long value = std::strtoll(text.c_str(), &tail, 0);
                    if (tail == text.c_str() || errno == ERANGE || value < 0) return false;
                    const String suffix = text.substr(static_cast<SizeT>(tail - text.c_str()));
                    if (!suffix.empty() && suffix != "u" && suffix != "U") return false;
                    out = value;
                    return true;
                }

                // glslang reflects an array-of-arrays default-block uniform as ONE RECORD PER
                // outer-index tuple, carrying the innermost array type: `float u[2][3]` becomes
                // "u[0][0]" and "u[1][0]" (that last "[0]" is EShReflectionBasicArraySuffix). The
                // linker resolves such a name by stripping the single trailing "[0]", so it looks
                // up "u[1]" - a key the root entry alone cannot answer, and the whole declaration
                // silently loses its explicit location.
                //
                // Emit those pre-flattened keys here, next to the root, so the result is
                // order-independent: each carries the location its own element starts at (element
                // i of `float u[2][3]` at location L starts at L + i*3). Identifiers cannot
                // contain brackets, so a synthesized key never collides with a real uniform name,
                // and a 1-D array needs none of this - stripping "[0]" already reaches the root.
                void RecordArrayOfArraysElementLocations(const String& name, const Vector<long long>& dimensions,
                                                         long long baseLocation,
                                                         MobileGL::UnorderedMap<String, MobileGL::Int>& locations) {
                    if (dimensions.size() < 2) return;
                    // A pathological declaration must not be able to blow up the map; past the cap
                    // only the root entry stands, which is what every case used to get.
                    constexpr long long kMaxSynthesizedKeys = 4096;
                    const long long innerSpan = dimensions.back();
                    const SizeT outerDimensions = dimensions.size() - 1;
                    long long elementCount = 1;
                    for (SizeT d = 0; d < outerDimensions; ++d) {
                        elementCount *= dimensions[d];
                        if (elementCount > kMaxSynthesizedKeys) return;
                    }
                    for (long long element = 0; element < elementCount; ++element) {
                        String key = name;
                        long long remainder = element;
                        for (SizeT d = 0; d < outerDimensions; ++d) {
                            long long stride = 1;
                            for (SizeT inner = d + 1; inner < outerDimensions; ++inner) stride *= dimensions[inner];
                            key += "[" + std::to_string(remainder / stride) + "]";
                            remainder %= stride;
                        }
                        locations.emplace(key, static_cast<MobileGL::Int>(
                                                   std::min(baseLocation + element * innerSpan,
                                                            static_cast<long long>(INT_MAX / 2))));
                    }
                }

                // Parses one brace-free depth-0 statement [begin, end) and records its
                // declarators when it is a uniform declaration carrying an integral
                // layout(location = N). Multi-declarator statements assign consecutive
                // locations, each declarator advancing by its array element count
                // (ARB_explicit_uniform_location rules). Anything the narrow grammar does
                // not recognize is skipped, never guessed at.
                void RecordUniformDeclarationLocations(const Vector<CodeToken>& tokens, SizeT begin, SizeT end,
                                                       MobileGL::UnorderedMap<String, MobileGL::Int>& locations) {
                    using MobileGL::Int;
                    long long location = -1;
                    long long literal = 0;
                    bool sawUniform = false;
                    SizeT declaratorBegin = end;

                    for (SizeT k = begin; k < end;) {
                        const String& text = tokens[k].text;
                        if (text == "layout" && k + 1 < end && tokens[k + 1].text == "(") {
                            SizeT j = k + 2;
                            Int parenDepth = 1;
                            while (j < end && parenDepth > 0) {
                                const String& layoutToken = tokens[j].text;
                                if (layoutToken == "(") {
                                    ++parenDepth;
                                } else if (layoutToken == ")") {
                                    --parenDepth;
                                } else if (parenDepth == 1 && layoutToken == "location" && j + 2 < end &&
                                           tokens[j + 1].text == "=" &&
                                           ParseGlslIntegerLiteral(tokens[j + 2].text, literal)) {
                                    location = std::min(literal, static_cast<long long>(INT_MAX / 2));
                                    j += 2;
                                }
                                ++j;
                            }
                            k = j;
                            continue;
                        }
                        if (text == "uniform") {
                            sawUniform = true;
                            ++k;
                            continue;
                        }
                        if (sawUniform && location >= 0 && IsIdentifierToken(tokens[k]) &&
                            !IsNonLayoutQualifierKeyword(text)) {
                            declaratorBegin = k + 1; // 'text' is the type; declarators follow
                            break;
                        }
                        ++k;
                    }

                    if (!sawUniform || location < 0 || declaratorBegin >= end) return;

                    long long nextLocation = location;
                    for (SizeT k = declaratorBegin; k < end;) {
                        if (!IsIdentifierToken(tokens[k])) return; // malformed; record nothing further
                        const String& name = tokens[k].text;
                        ++k;
                        long long span = 1;
                        Vector<long long> dimensions;
                        while (k < end && tokens[k].text == "[") {
                            ++k;
                            long long dimension = 1;
                            if (k < end && ParseGlslIntegerLiteral(tokens[k].text, literal)) {
                                dimension = literal;
                                ++k;
                            }
                            if (k >= end || tokens[k].text != "]") return; // sized by expression; bail out
                            ++k;
                            dimensions.push_back(
                                std::max(1ll, std::min(dimension, static_cast<long long>(INT_MAX / 2))));
                            span *= dimensions.back();
                        }
                        // Keep the first sighting: a duplicate can only come from alternative
                        // preprocessor branches declaring the same name.
                        locations.emplace(name, static_cast<Int>(std::min(
                                                    nextLocation, static_cast<long long>(INT_MAX / 2))));
                        RecordArrayOfArraysElementLocations(name, dimensions, nextLocation, locations);
                        nextLocation += span;
                        if (k >= end) break;
                        if (tokens[k].text == "=") { // skip an initializer up to the declarator comma
                            Int nestingDepth = 0;
                            ++k;
                            while (k < end) {
                                const String& initializerToken = tokens[k].text;
                                if (initializerToken == "(" || initializerToken == "[") {
                                    ++nestingDepth;
                                } else if (initializerToken == ")" || initializerToken == "]") {
                                    --nestingDepth;
                                } else if (initializerToken == "," && nestingDepth == 0) {
                                    break;
                                }
                                ++k;
                            }
                        }
                        if (k >= end) break;
                        if (tokens[k].text != ",") return;
                        ++k;
                    }
                }
                // Parses one brace-free depth-0 statement [begin, end) and records its
                // declarators when it is a sampler/image uniform declaration carrying an
                // integral layout(binding = N). Such a binding is a GL texture/image unit,
                // which the Vulkan-client relaxed parse strips before mapIO can observe it
                // (it is not a valid descriptor binding there), so it is extracted lexically
                // and restored as the uniform's initial unit. Every declarator in the
                // statement shares the qualifier's binding, matching what the GL-client
                // mapIO used to capture from the shared type qualifier. Anything the narrow
                // grammar does not recognize is skipped, never guessed at.
                void RecordOpaqueDeclarationBindings(const Vector<CodeToken>& tokens, SizeT begin, SizeT end,
                                                     MobileGL::UnorderedMap<String, MobileGL::Uint>& bindings) {
                    using MobileGL::Int;
                    long long binding = -1;
                    long long literal = 0;
                    bool sawUniform = false;
                    SizeT declaratorBegin = end;

                    for (SizeT k = begin; k < end;) {
                        const String& text = tokens[k].text;
                        if (text == "layout" && k + 1 < end && tokens[k + 1].text == "(") {
                            SizeT j = k + 2;
                            Int parenDepth = 1;
                            while (j < end && parenDepth > 0) {
                                const String& layoutToken = tokens[j].text;
                                if (layoutToken == "(") {
                                    ++parenDepth;
                                } else if (layoutToken == ")") {
                                    --parenDepth;
                                } else if (parenDepth == 1 && layoutToken == "binding" && j + 2 < end &&
                                           tokens[j + 1].text == "=" &&
                                           ParseGlslIntegerLiteral(tokens[j + 2].text, literal)) {
                                    binding = std::min(literal, static_cast<long long>(INT_MAX / 2));
                                    j += 2;
                                }
                                ++j;
                            }
                            k = j;
                            continue;
                        }
                        if (text == "uniform") {
                            sawUniform = true;
                            ++k;
                            continue;
                        }
                        if (sawUniform && binding >= 0 && IsIdentifierToken(tokens[k]) &&
                            !IsNonLayoutQualifierKeyword(text)) {
                            // 'text' is the type. Only sampler/image opaques carry unit
                            // bindings; on anything else (e.g. atomic_uint, whose binding
                            // is a counter-buffer index) record nothing.
                            if (text.find("sampler") == String::npos && text.find("image") == String::npos) return;
                            declaratorBegin = k + 1;
                            break;
                        }
                        ++k;
                    }

                    if (!sawUniform || binding < 0 || declaratorBegin >= end) return;

                    for (SizeT k = declaratorBegin; k < end;) {
                        if (!IsIdentifierToken(tokens[k])) return; // malformed; record nothing further
                        const String& name = tokens[k].text;
                        ++k;
                        while (k < end && tokens[k].text == "[") {
                            ++k;
                            if (k < end && ParseGlslIntegerLiteral(tokens[k].text, literal)) ++k;
                            if (k >= end || tokens[k].text != "]") return; // sized by expression; bail out
                            ++k;
                        }
                        bindings[name] = static_cast<MobileGL::Uint>(binding);
                        if (k >= end) break;
                        if (tokens[k].text != ",") return; // opaque declarators cannot take initializers
                        ++k;
                    }
                }
            } // namespace

            UnorderedMap<String, Uint> ExtractExplicitOpaqueBindings(const String& source) {
                UnorderedMap<String, Uint> bindings;
                // Fast path: without the qualifier keyword there is nothing to extract.
                if (source.find("binding") == String::npos) return bindings;

                const Vector<CodeToken> tokens = TokenizeCode(source);
                const SizeT count = tokens.size();
                Int braceDepth = 0;
                SizeT pos = 0;
                while (pos < count) {
                    const String& text = tokens[pos].text;
                    if (text == "{") {
                        ++braceDepth;
                        ++pos;
                        continue;
                    }
                    if (text == "}") {
                        if (braceDepth > 0) --braceDepth;
                        ++pos;
                        continue;
                    }
                    if (braceDepth != 0 || text == ";") {
                        ++pos;
                        continue;
                    }

                    // A depth-0 statement runs to its ';'. One that opens a brace instead is
                    // a function definition or an interface/uniform block: a block's binding
                    // is a buffer binding point, not a texture unit, so skip both alike.
                    SizeT statementEnd = pos;
                    while (statementEnd < count && tokens[statementEnd].text != ";" &&
                           tokens[statementEnd].text != "{") {
                        ++statementEnd;
                    }
                    if (statementEnd >= count || tokens[statementEnd].text == "{") {
                        pos = statementEnd;
                        continue;
                    }

                    RecordOpaqueDeclarationBindings(tokens, pos, statementEnd, bindings);
                    pos = statementEnd + 1;
                }
                return bindings;
            }

            namespace {
                // Binding points a storage-block declaration starting at `bufferPos` occupies.
                // One for a scalar instance (and for the "layout(...) buffer;" default-qualifier
                // form, which declares no block at all); the element count for an instance array,
                // whose elements take base, base+1, ... (GLSL 4.30 4.4.5). -1 means "the grammar
                // here is outside this scanner's narrow subset", i.e. do not judge this one.
                long long StorageBlockBindingPointCount(const Vector<CodeToken>& tokens, SizeT bufferPos,
                                                        SizeT count) {
                    SizeT k = bufferPos + 1;
                    if (k < count && IsIdentifierToken(tokens[k])) ++k; // block type name
                    if (k >= count || tokens[k].text != "{") return 1;

                    MobileGL::Int braceDepth = 0;
                    while (k < count) {
                        if (tokens[k].text == "{") {
                            ++braceDepth;
                        } else if (tokens[k].text == "}") {
                            --braceDepth;
                            if (braceDepth == 0) {
                                ++k;
                                break;
                            }
                        }
                        ++k;
                    }
                    if (braceDepth != 0) return -1; // unterminated block: not this scanner's business

                    if (k < count && IsIdentifierToken(tokens[k])) ++k; // instance name
                    if (k >= count || tokens[k].text != "[") return 1;
                    long long elementCount = 0;
                    if (k + 2 < count && ParseGlslIntegerLiteral(tokens[k + 1].text, elementCount) &&
                        tokens[k + 2].text == "]") {
                        return std::max<long long>(1, elementCount);
                    }
                    return -1; // sized by an expression, or unsized
                }
            } // namespace

            std::set<String> ExtractStorageBlocksWithoutExplicitBinding(const String& source) {
                std::set<String> names;
                // Fast path: no storage block, nothing to record. `buffer` as a whole token is
                // what declares one; samplerBuffer/imageBuffer/textureBuffer tokenize as single
                // identifiers and so cannot match below, but this substring test is only a
                // cheap pre-filter and is allowed to be generous.
                if (source.find("buffer") == String::npos) return names;

                const Vector<CodeToken> tokens = TokenizeCode(source);
                const SizeT count = tokens.size();
                Int braceDepth = 0;
                // Block names seen WITH a binding. Subtracted at the end so a name that is
                // declared unqualified in one place and qualified in another is never reported:
                // this scans preprocessor-visible text, so both arms of a #if can be present,
                // and defaulting a block the active arm binds explicitly would be a regression.
                // A name is dropped whenever there is any doubt, never kept.
                std::set<String> qualified;
                // The binding the qualifier run currently being scanned declared, -1 for none.
                // Several layout(...) lists may precede one declaration and the later one wins -
                // the same accumulate-then-consume shape FindShaderStorageBindingViolation uses.
                long long binding = -1;
                long long literal = 0;
                for (SizeT pos = 0; pos < count; ++pos) {
                    const String& text = tokens[pos].text;
                    if (text == "{") {
                        ++braceDepth;
                        binding = -1;
                        continue;
                    }
                    if (text == "}") {
                        if (braceDepth > 0) --braceDepth;
                        continue;
                    }
                    // Only depth-0 declarations are block declarations; `buffer` inside a block
                    // body or a function is a member qualifier or an identifier.
                    if (braceDepth != 0) continue;

                    if (text == "layout" && pos + 1 < count && tokens[pos + 1].text == "(") {
                        SizeT j = pos + 2;
                        Int parenDepth = 1;
                        while (j < count && parenDepth > 0) {
                            const String& layoutToken = tokens[j].text;
                            if (layoutToken == "(") {
                                ++parenDepth;
                            } else if (layoutToken == ")") {
                                --parenDepth;
                            } else if (parenDepth == 1 && layoutToken == "binding" && j + 2 < count &&
                                       tokens[j + 1].text == "=" &&
                                       ParseGlslIntegerLiteral(tokens[j + 2].text, literal)) {
                                binding = std::min(literal, static_cast<long long>(INT_MAX / 2));
                                j += 2;
                            }
                            ++j;
                        }
                        pos = j - 1;
                        continue;
                    }

                    if (text == "buffer") {
                        // Recorded ONLY for the fully recognised shape: a block type name
                        // followed by the body's '{'. The "layout(...) buffer;"
                        // default-qualifier form declares no block and has no name to key on,
                        // and anything else here is grammar this scanner does not judge - both
                        // fall through and keep today's behaviour.
                        if (pos + 2 < count && IsIdentifierToken(tokens[pos + 1]) &&
                            tokens[pos + 2].text == "{") {
                            (binding < 0 ? names : qualified).insert(tokens[pos + 1].text);
                        }
                        binding = -1;
                        continue;
                    }

                    // Qualifiers may sit between the layout list and the `buffer` keyword;
                    // anything else ends the run, so a binding never leaks onto an unrelated
                    // declaration - and, just as importantly, the ABSENCE of one never does.
                    if (!IsNonLayoutQualifierKeyword(text)) binding = -1;
                }
                for (const String& name : qualified) {
                    names.erase(name);
                }
                return names;
            }

            std::optional<String> FindShaderStorageBindingViolation(const String& source, Int maxBindings) {
                // A backend that advertises nothing has no ceiling to enforce.
                if (maxBindings <= 0) return std::nullopt;
                // Fast path: no storage block, nothing to check. Both keywords are required for a
                // violation to exist, and the pair is absent from almost every shader-pack source.
                if (source.find("buffer") == String::npos || source.find("binding") == String::npos) {
                    return std::nullopt;
                }

                const Vector<CodeToken> tokens = TokenizeCode(source);
                const SizeT count = tokens.size();
                // The binding the qualifier run currently being scanned declared, -1 for none.
                // Several layout(...) lists may precede one declaration and the later one wins,
                // which is the same accumulate-then-consume shape the extractors above use.
                long long binding = -1;
                long long literal = 0;
                for (SizeT pos = 0; pos < count; ++pos) {
                    const String& text = tokens[pos].text;
                    if (text == "layout" && pos + 1 < count && tokens[pos + 1].text == "(") {
                        SizeT j = pos + 2;
                        Int parenDepth = 1;
                        while (j < count && parenDepth > 0) {
                            const String& layoutToken = tokens[j].text;
                            if (layoutToken == "(") {
                                ++parenDepth;
                            } else if (layoutToken == ")") {
                                --parenDepth;
                            } else if (parenDepth == 1 && layoutToken == "binding" && j + 2 < count &&
                                       tokens[j + 1].text == "=" &&
                                       ParseGlslIntegerLiteral(tokens[j + 2].text, literal)) {
                                binding = std::min(literal, static_cast<long long>(INT_MAX / 2));
                                j += 2;
                            }
                            ++j;
                        }
                        pos = j - 1;
                        continue;
                    }
                    if (text == "buffer") {
                        const long long points = binding >= 0 ? StorageBlockBindingPointCount(tokens, pos, count) : -1;
                        if (points > 0 && binding + points > static_cast<long long>(maxBindings)) {
                            return "ERROR: invalid value " + std::to_string(binding) +
                                   " for layout specifier 'binding': a shader storage block occupying " +
                                   std::to_string(points) + " binding point(s) from there passes " +
                                   "GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS (" + std::to_string(maxBindings) + ").";
                        }
                        binding = -1;
                        continue;
                    }
                    // Qualifiers may sit between the layout list and the `buffer` keyword; anything
                    // else ends the run, so a binding never leaks onto an unrelated declaration.
                    if (!IsNonLayoutQualifierKeyword(text)) binding = -1;
                }
                return std::nullopt;
            }

            std::optional<String> FindAtomicCounterOffsetViolation(const String& source) {
                // Fast path: both keywords are required for a violation to exist, and the pair is
                // absent from every shader-pack source.
                if (source.find("atomic_uint") == String::npos || source.find("offset") == String::npos) {
                    return std::nullopt;
                }

                constexpr long long kAtomicCounterSize = 4; // one 32-bit word per counter
                const long long maxBufferSize = static_cast<long long>(MAX_ATOMIC_COUNTER_BUFFER_SIZE);
                const Vector<CodeToken> tokens = TokenizeCode(source);
                const SizeT count = tokens.size();
                // The offset the qualifier run currently being scanned declared, -1 for none.
                // Same accumulate-then-consume shape as the storage-binding scan above.
                long long offset = -1;
                long long literal = 0;
                for (SizeT pos = 0; pos < count; ++pos) {
                    const String& text = tokens[pos].text;
                    if (text == "layout" && pos + 1 < count && tokens[pos + 1].text == "(") {
                        SizeT j = pos + 2;
                        Int parenDepth = 1;
                        while (j < count && parenDepth > 0) {
                            const String& layoutToken = tokens[j].text;
                            if (layoutToken == "(") {
                                ++parenDepth;
                            } else if (layoutToken == ")") {
                                --parenDepth;
                            } else if (parenDepth == 1 && layoutToken == "offset" && j + 2 < count &&
                                       tokens[j + 1].text == "=" &&
                                       ParseGlslIntegerLiteral(tokens[j + 2].text, literal)) {
                                offset = literal;
                                j += 2;
                            }
                            ++j;
                        }
                        pos = j - 1;
                        continue;
                    }
                    if (text == "atomic_uint") {
                        // How far the declaration reaches: `atomic_uint c[N]` occupies N words
                        // from the offset. An unparsable or absent declarator (an expression-sized
                        // array, or the "layout(...) uniform atomic_uint;" default-qualifier form,
                        // which declares no counter at all) is left alone rather than guessed at -
                        // over-rejection here would be a compile failure the application cannot
                        // work around.
                        long long elements = 1;
                        SizeT k = pos + 1;
                        if (k < count && IsIdentifierToken(tokens[k])) {
                            ++k;
                            if (k < count && tokens[k].text == "[") {
                                elements = (k + 2 < count && tokens[k + 2].text == "]" &&
                                            ParseGlslIntegerLiteral(tokens[k + 1].text, literal))
                                               ? std::max<long long>(1, literal)
                                               : -1;
                            }
                        } else {
                            elements = -1;
                        }
                        // Clamped so the byte arithmetic below cannot overflow on an absurd
                        // literal; any element count at or past the ceiling already fails.
                        elements = std::min(elements, maxBufferSize);

                        if (offset >= 0 && elements > 0) {
                            if (offset % kAtomicCounterSize != 0) {
                                return "ERROR: invalid value " + std::to_string(offset) +
                                       " for layout specifier 'offset': an atomic counter offset must be a "
                                       "multiple of 4.";
                            }
                            if (offset > maxBufferSize - elements * kAtomicCounterSize) {
                                return "ERROR: invalid value " + std::to_string(offset) +
                                       " for layout specifier 'offset': an atomic counter ending at byte " +
                                       std::to_string(offset + elements * kAtomicCounterSize) +
                                       " passes GL_MAX_ATOMIC_COUNTER_BUFFER_SIZE (" +
                                       std::to_string(maxBufferSize) + ").";
                            }
                        }
                        offset = -1;
                        continue;
                    }
                    // `uniform` and the precision/auxiliary qualifiers may sit between the layout
                    // list and the type keyword; anything else ends the run, so an offset never
                    // leaks onto an unrelated declaration.
                    if (text != "uniform" && !IsNonLayoutQualifierKeyword(text)) offset = -1;
                }
                return std::nullopt;
            }

            UnorderedMap<String, Int> ExtractExplicitUniformLocations(const String& source) {
                UnorderedMap<String, Int> locations;
                // Fast path: without the qualifier keyword there is nothing to extract.
                if (source.find("location") == String::npos) return locations;

                const Vector<CodeToken> tokens = TokenizeCode(source);
                const SizeT count = tokens.size();
                Int braceDepth = 0;
                SizeT pos = 0;
                while (pos < count) {
                    const String& text = tokens[pos].text;
                    if (text == "{") {
                        ++braceDepth;
                        ++pos;
                        continue;
                    }
                    if (text == "}") {
                        if (braceDepth > 0) --braceDepth;
                        ++pos;
                        continue;
                    }
                    if (braceDepth != 0 || text == ";") {
                        ++pos;
                        continue;
                    }

                    // A depth-0 statement runs to its ';'. One that opens a brace instead is a
                    // function definition or an interface/uniform block: neither can declare a
                    // default-block uniform location, so hand the '{' back to the depth tracker.
                    SizeT statementEnd = pos;
                    while (statementEnd < count && tokens[statementEnd].text != ";" &&
                           tokens[statementEnd].text != "{") {
                        ++statementEnd;
                    }
                    if (statementEnd >= count || tokens[statementEnd].text == "{") {
                        pos = statementEnd;
                        continue;
                    }

                    RecordUniformDeclarationLocations(tokens, pos, statementEnd, locations);
                    pos = statementEnd + 1;
                }
                return locations;
            }

        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
