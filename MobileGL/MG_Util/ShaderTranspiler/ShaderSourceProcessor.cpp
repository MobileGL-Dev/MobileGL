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
        // Every extension the source NAMES in an "#extension <name> : <behavior>" directive, and
        // the subset whose behavior switches it on. Both are needed and they are not the same
        // question: glslang's ES preamble defines an extension's macro whatever behavior the
        // shader later asks for (it is a preamble, it runs first), while whether gl_NumSamples is
        // a legal identifier depends on the extension actually being ENABLED.
        std::set<MobileGL::String> namedExtensions;
        std::set<MobileGL::String> enabledExtensions;
        // Byte ranges [begin, end) of every #version directive AFTER the first that repeats it
        // exactly - same version number, same profile, both well-formed. See
        // BlankRedundantVersionDirectives for why these are tolerated and nothing else is.
        Vector<std::pair<SizeT, SizeT>> redundantVersionDirectives;

        bool HasVersionDirective() const { return versionDirectiveStart != MobileGL::String::npos; }
    };

    struct ParsedVersionDirective {
        unsigned version = 0;
        MobileGL::ShaderProfile profile = MobileGL::ShaderProfile::Core;
        bool isValid = false;
    };

    // Reads "<digits> [profile]" out of a "#version" directive whose keyword ends at `probe`, and
    // decides whether it is one MobileGL is willing to rewrite. `code` must be the masked source,
    // so a trailing comment has already become blanks.
    bool ParseVersionDirectiveBody(const MobileGL::String& code, SizeT probe, SizeT lineEnd,
                                   ParsedVersionDirective& out) {
        SkipDirectiveWhitespace(code, probe, lineEnd);
        unsigned version = 0;
        bool hasVersionDigits = false;
        while (probe < lineEnd && code[probe] >= '0' && code[probe] <= '9') {
            hasVersionDigits = true;
            version = version * 10 + static_cast<unsigned>(code[probe] - '0');
            probe++;
        }
        if (!hasVersionDigits) return false;

        SkipDirectiveWhitespace(code, probe, lineEnd);
        const MobileGL::String profileToken = ReadDirectiveIdentifier(code, probe, lineEnd);
        bool profileTokenValid = true;
        MobileGL::ShaderProfile profile = MobileGL::ShaderProfile::Core;
        if (profileToken.empty() || profileToken == "core") {
            profile = MobileGL::ShaderProfile::Core;
        } else if (profileToken == "es" || profileToken == "ES") {
            profile = MobileGL::ShaderProfile::ES;
        } else if (profileToken == "compatibility") {
            profile = MobileGL::ShaderProfile::Compatibility;
        } else {
            // "#version 330 foo": an unrecognized profile keyword. Keep Core for any downstream
            // routing, but mark the directive malformed.
            profile = MobileGL::ShaderProfile::Core;
            profileTokenValid = false;
        }
        // Comments are already masked to spaces, so anything non-blank left on the line is real
        // trailing garbage: "#version 330 foobar" / "#version 330.0".
        SkipDirectiveWhitespace(code, probe, lineEnd);
        const bool hasTrailingTokens = probe < lineEnd;

        out.version = version;
        out.profile = profile;
        out.isValid = IsRecognizedGlslVersion(version) && profileTokenValid && !hasTrailingTokens;
        return true;
    }

    ShaderLanguageInfo InspectShaderLanguage(const MobileGL::String& source) {
        const MobileGL::String code = MaskCommentsAndQuotedText(source);
        ShaderLanguageInfo info;
        info.hasUtf8Bom = HasUtf8Bom(source);

        // An exact repeat of the accepted directive, wherever on the line it sits. Recorded for
        // BlankRedundantVersionDirectives; never called before a valid first directive was found,
        // which is what keeps a LONE misplaced #version rejected.
        const auto recordIfRedundant = [&info, &code](SizeT hashPos, SizeT lineEnd) {
            if (!info.hasValidVersionDirective) return;
            SizeT probe = hashPos + 1;
            SkipDirectiveWhitespace(code, probe, lineEnd);
            if (ReadDirectiveIdentifier(code, probe, lineEnd) != "version") return;
            ParsedVersionDirective parsed;
            if (!ParseVersionDirectiveBody(code, probe, lineEnd, parsed)) return;
            if (!parsed.isValid || parsed.version != info.version || parsed.profile != info.profile) return;
            info.redundantVersionDirectives.push_back({hashPos, lineEnd});
        };

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
            if (probe >= lineEnd || code[probe] != '#') {
                // A directive that is not first on its line is not a directive at all - except for
                // the one case glShaderSource creates on its own: two strings each headed by a
                // #version splice the second into the tail of the first. Only an EXACT repeat of
                // the directive already accepted is recognized here; see
                // BlankRedundantVersionDirectives for why that one is tolerated and nothing else.
                //
                // Gated on a directive having been accepted already, so an ordinary shader - which
                // has none of these - pays one memchr per line past its #version line and nothing
                // before it.
                if (info.hasValidVersionDirective) {
                    const SizeT hashPos = code.find('#', probe);
                    if (hashPos != MobileGL::String::npos && hashPos < lineEnd) {
                        recordIfRedundant(hashPos, lineEnd);
                    }
                }
            } else {
                const SizeT directiveStart = probe;
                probe++;
                SkipDirectiveWhitespace(code, probe, lineEnd);
                const MobileGL::String directive = ReadDirectiveIdentifier(code, probe, lineEnd);

                if (directive == "version") {
                    ParsedVersionDirective parsed;
                    if (ParseVersionDirectiveBody(code, probe, lineEnd, parsed)) {
                        if (!info.HasVersionDirective()) {
                            info.version = parsed.version;
                            info.profile = parsed.profile;
                            info.versionDirectiveStart = directiveStart;
                            info.versionDirectiveEnd = lineEnd + (hasLineBreak ? 1 : 0);
                            info.hasValidVersionDirective = parsed.isValid;
                        } else {
                            recordIfRedundant(directiveStart, lineEnd);
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
                        if (!extension.empty()) {
                            info.namedExtensions.insert(extension);
                            if (enablesExtension) info.enabledExtensions.insert(extension);
                        }
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
    // An exact repeat of the #version directive the shader already declared, blanked out.
    //
    // Strictly a repeat: InspectShaderLanguage only records a range here when the FIRST directive
    // was well-formed and the later one is well-formed, names the same version number and the same
    // profile, and is therefore semantically a no-op. Everything else - a differing version, a
    // malformed one, or a lone #version that is simply not first - is left exactly where the
    // application put it, so KHR-GL33.shaders.preprocessor.directive.version_not_first_statement_*
    // and the version_invalid_token_* family keep failing to compile the way they must.
    //
    // Why tolerate even the repeat: glShaderSource concatenates its strings with nothing added
    // between them (GL 4.6 core 7.1), and a caller that puts a #version at the head of BOTH strings
    // gets the second one spliced into the tail of the first - which is exactly what VK-GL-CTS's
    // ShaderImageLoadStoreBase::BuildProgram does (kGLSLPrec ends without a newline, and
    // NegativeUniform's own sources begin with "#version 310 es"). Desktop drivers accept it; the
    // duplicate says nothing new, so honouring it costs no semantics.
    //
    // Blanked rather than erased so that every offset in `info` - which was measured against this
    // same source - stays valid, and so the line count, and with it __LINE__ and every glslang
    // diagnostic, is untouched.
    void BlankRedundantVersionDirectives(MobileGL::String& source, const ShaderLanguageInfo& info) {
        for (const auto& [begin, end] : info.redundantVersionDirectives) {
            if (begin >= source.size() || end > source.size() || begin >= end) continue;
            std::fill(source.begin() + static_cast<std::ptrdiff_t>(begin),
                      source.begin() + static_cast<std::ptrdiff_t>(end), ' ');
        }
    }

    SizeT NormalizeVersionDirective(MobileGL::String& source, const ShaderLanguageInfo& info) {
        const SizeT bomBytes = info.hasUtf8Bom ? 3 : 0;

        // First, while every offset in `info` still refers to the untouched source. Each range
        // lies strictly after the first directive, so nothing below has to account for it.
        BlankRedundantVersionDirectives(source, info);

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

    // Index one past the token that closes the group tokens[open] opens, or the token count when
    // the group is never closed. Nesting of the SAME bracket pair is counted, everything else is
    // skipped, so a '(' inside a '[' run cannot confuse a bracket walk and vice versa.
    SizeT FindGroupEnd(const Vector<CodeToken>& tokens, SizeT open, char opener, char closer) {
        int depth = 0;
        for (SizeT i = open; i < tokens.size(); ++i) {
            if (tokens[i].text.size() != 1) continue;
            if (tokens[i].text[0] == opener) {
                ++depth;
            } else if (tokens[i].text[0] == closer && --depth == 0) {
                return i + 1;
            }
        }
        return tokens.size();
    }

    // Token text joined by single spaces. Token text is comment-free by construction (the
    // tokenizer reads a masked source), so this is how a rewritten declaration is rebuilt without
    // dragging a comment - or a newline - into a line the rewrite promises to keep single-line.
    String JoinTokenText(const Vector<CodeToken>& tokens, SizeT begin, SizeT end) {
        String text;
        for (SizeT i = begin; i < end; ++i) {
            if (!text.empty()) text += ' ';
            text += tokens[i].text;
        }
        return text;
    }

    // Erase a span for the compiler while keeping every later offset - and every LINE NUMBER -
    // exactly where it was, so edits collected against one token scan all stay valid and glslang's
    // diagnostics still point at the line the application wrote.
    void BlankSpan(MobileGL::String& source, SizeT begin, SizeT end) {
        for (SizeT i = begin; i < end && i < source.size(); ++i) {
            if (source[i] != '\n' && source[i] != '\r') {
                source[i] = ' ';
            }
        }
    }

    bool IsParameterQualifierKeyword(const String& text) {
        static constexpr std::string_view kQualifiers[] = {
            "const",    "in",       "out",      "inout",     "highp",     "mediump", "lowp",
            "precise",  "coherent", "volatile", "restrict",  "readonly",  "writeonly",
        };
        return std::find(std::begin(kQualifiers), std::end(kQualifiers), std::string_view(text)) !=
            std::end(kQualifiers);
    }

    // The #if/#ifdef/#ifndef nesting in effect at each offset, as (offset, depth) marks. Every
    // mark takes effect at the END of the directive line that changed the depth.
    Vector<std::pair<SizeT, int>> BuildConditionalDepthMarks(const MobileGL::String& source,
                                                            const Vector<std::pair<SizeT, SizeT>>& ranges) {
        Vector<std::pair<SizeT, int>> marks;
        marks.emplace_back(static_cast<SizeT>(0), 0);
        int depth = 0;
        for (const std::pair<SizeT, SizeT>& range : ranges) {
            SizeT pos = range.first;
            SkipDirectiveWhitespace(source, pos, range.second);
            if (pos >= range.second || source[pos] != '#') continue;
            ++pos;
            SkipDirectiveWhitespace(source, pos, range.second);
            const String name = ReadDirectiveIdentifier(source, pos, range.second);
            if (name == "if" || name == "ifdef" || name == "ifndef") {
                ++depth;
            } else if (name == "endif") {
                if (depth > 0) --depth;
            } else {
                continue;
            }
            marks.emplace_back(range.second, depth);
        }
        return marks;
    }

    int ConditionalDepthAt(const Vector<std::pair<SizeT, int>>& marks, SizeT offset) {
        const auto next = std::upper_bound(marks.begin(), marks.end(), offset,
                                           [](SizeT value, const std::pair<SizeT, int>& mark) {
                                               return value < mark.first;
                                           });
        return next == marks.begin() ? 0 : std::prev(next)->second;
    }

    struct SubroutineParameters {
        Vector<String> declarations; // "in highp float mgl_sr_arg0", ready for a parameter list
        Vector<String> arguments;    // "mgl_sr_arg0", ready for a forwarding call
    };

    // One parameter of a subroutine TYPE declaration, whose name (if it even has one) this rewrite
    // replaces with a generated one. The shape read is
    //     <qualifier>* <typeName> <arrayOfType>? <name>? <arrayOfName>?
    // which is the whole of the GLSL parameter grammar; anything that does not fit is refused so
    // the caller can abandon the rewrite rather than emit a guess.
    bool AppendSubroutineParameter(const Vector<CodeToken>& tokens, SizeT begin, SizeT end, SizeT index,
                                   SubroutineParameters& parameters) {
        if (begin >= end) return false;

        SizeT cursor = begin;
        while (cursor < end && IsParameterQualifierKeyword(tokens[cursor].text)) {
            ++cursor;
        }
        if (cursor >= end || !IsIdentifierToken(tokens[cursor])) return false;
        ++cursor;
        while (cursor < end && tokens[cursor].text == "[") { // "float[4] a"
            const SizeT close = FindGroupEnd(tokens, cursor, '[', ']');
            if (close > end) return false;
            cursor = close;
        }
        const String typeText = JoinTokenText(tokens, begin, cursor);

        String arraySuffix;
        if (cursor < end) { // the declared parameter name, which the generated one replaces
            if (!IsIdentifierToken(tokens[cursor])) return false;
            const SizeT afterName = cursor + 1;
            cursor = afterName;
            while (cursor < end && tokens[cursor].text == "[") { // "float a[4]"
                const SizeT close = FindGroupEnd(tokens, cursor, '[', ']');
                if (close > end) return false;
                cursor = close;
            }
            if (cursor != end) return false;
            arraySuffix = JoinTokenText(tokens, afterName, end);
        }

        const String name = "mgl_sr_arg" + std::to_string(index);
        parameters.declarations.push_back(typeText + " " + name + (arraySuffix.empty() ? "" : " " + arraySuffix));
        parameters.arguments.push_back(name);
        return true;
    }

    // The parameter list between (but not including) the parentheses of a subroutine type
    // declaration. "()" and "(void)" are both the empty list.
    bool ParseSubroutineParameters(const Vector<CodeToken>& tokens, SizeT begin, SizeT end,
                                   SubroutineParameters& parameters) {
        if (begin >= end) return true;
        if (end == begin + 1 && tokens[begin].text == "void") return true;

        SizeT parameterBegin = begin;
        SizeT index = 0;
        for (SizeT i = begin; i <= end; ++i) {
            if (i < end) {
                if (tokens[i].text == "[") { // a comma inside a subscript is not a separator
                    const SizeT close = FindGroupEnd(tokens, i, '[', ']');
                    if (close > end) return false;
                    i = close - 1;
                    continue;
                }
                if (tokens[i].text != ",") continue;
            }
            if (!AppendSubroutineParameter(tokens, parameterBegin, i, index, parameters)) return false;
            ++index;
            parameterBegin = i + 1;
        }
        return true;
    }

    // GLSL subroutines (ARB_shader_subroutine, core since 4.00).
    //
    // glslang refuses the keyword outright once the target is SPIR-V - "'subroutine' : not allowed
    // when generating SPIR-V", "feature not yet implemented" - so a shader that declares one never
    // produces a module at all and the whole program is lost at COMPILE time. That is the entire
    // failure of KHR-GL43.shader_image_size.advanced-nonMS-*: its subroutine-free twin
    // basic-nonMS-* drives the identical image battery through the identical imageSize() calls on
    // the identical targets and passes on every stage.
    //
    // The rewrite is confined to the case where it is provably a no-op on semantics: a subroutine
    // uniform whose type has EXACTLY ONE compatible subroutine. GL 4.3 core 7.9 leaves the value of
    // a subroutine uniform implementation-dependent until glUniformSubroutinesuiv sets it, so with
    // a single compatible subroutine every legal value of that uniform selects the same function
    // and a direct call is indistinguishable from a dispatch under any GL state. A type with two or
    // more compatible subroutines genuinely needs the dynamic selection MobileGL does not implement
    // (glUniformSubroutinesuiv is still a stub, and nothing reflects the subroutine interfaces), so
    // it is left to fail at compile time exactly as it does today rather than silently pinned to
    // one of the alternatives.
    //
    //     subroutine void FuncType(int coord);          ->  (blanked)
    //     subroutine uniform FuncType g_func;           ->  void g_func(int mgl_sr_arg0);
    //     subroutine(FuncType) void Func0(int c) { }    ->  void Func0(int c) { }
    //                                                       ...plus, appended at end of source,
    //                                                       void g_func(int mgl_sr_arg0) {
    //                                                           Func0(mgl_sr_arg0);
    //                                                       }
    //
    // Naming the forwarding function after the subroutine UNIFORM is what leaves every CALL site
    // untouched - "g_func(coord)" already reads as a call - and that name is free precisely because
    // the declaration that held it is gone. The forwarding body has to be appended rather than
    // written in place because the compatible subroutine is routinely defined AFTER the function
    // that calls through the uniform (the CTS shaders define theirs below main()); at end of source
    // every definition it names is already in scope, and a prototype at the old declaration site
    // keeps the call sites legal.
    //
    // All-or-nothing, in the discipline of the scanners below it: an array subroutine uniform, a
    // subroutine token inside a #if arm or a macro body, an unbalanced file, a type that is never
    // declared - anything outside the grammar abandons the whole pass with the source untouched,
    // which is exactly today's behaviour.
    void LowerShaderSubroutines(MobileGL::String& source) {
        if (source.find("subroutine") == MobileGL::String::npos) return;

        const Vector<CodeToken> tokens = TokenizeCode(source);
        const SizeT count = tokens.size();
        if (count < 4 || !HasBalancedBraces(tokens)) return;

        const Vector<std::pair<SizeT, SizeT>> directiveRanges = FindDirectiveLineRanges(source);
        const Vector<std::pair<SizeT, int>> conditionalDepth =
            BuildConditionalDepthMarks(source, directiveRanges);

        struct SubroutineType {
            String returnText; // empty until the type declaration itself is seen
            SubroutineParameters parameters;
            Vector<String> implementations; // compatible subroutines, in declaration order
        };
        struct UniformSite {
            SizeT begin = 0; // first byte of the declaration, layout(...) qualifier included
            SizeT end = 0;   // one past its ';'
            String typeName;
            Vector<String> variables;
        };
        struct BlankEdit {
            SizeT begin;
            SizeT end;
        };

        MobileGL::UnorderedMap<String, SubroutineType> types;
        Vector<UniformSite> uniformSites;
        Vector<BlankEdit> blanks;

        SizeT braceDepth = 0;
        for (SizeT i = 0; i < count; ++i) {
            const CodeToken& token = tokens[i];
            if (token.text.size() == 1) {
                if (token.text[0] == '{') {
                    ++braceDepth;
                    continue;
                }
                if (token.text[0] == '}') {
                    if (braceDepth > 0) --braceDepth;
                    continue;
                }
            }
            if (token.text != "subroutine") continue;

            // Nothing here may reason about a subroutine that is not unconditionally at file
            // scope: the forwarding bodies this appends are unconditional, so a declaration that
            // only exists in one #if arm (or inside a macro body) would have them naming a
            // function that is not there.
            if (braceDepth != 0 || IsInDirectiveLine(directiveRanges, token.begin) ||
                ConditionalDepthAt(conditionalDepth, token.begin) != 0) {
                return;
            }
            if (i + 1 >= count) return;

            // (a) `[layout(...)] subroutine uniform <TypeName> <var>[, <var>]... ;`
            if (tokens[i + 1].text == "uniform") {
                UniformSite site;
                site.begin = token.begin;
                if (i >= 2 && tokens[i - 1].text == ")") {
                    SizeT open = i - 1;
                    int depth = 1;
                    while (depth > 0) {
                        if (open == 0) return;
                        --open;
                        if (tokens[open].text == ")") {
                            ++depth;
                        } else if (tokens[open].text == "(") {
                            --depth;
                        }
                    }
                    if (open == 0 || tokens[open - 1].text != "layout") return;
                    site.begin = tokens[open - 1].begin;
                }

                SizeT cursor = i + 2;
                if (cursor >= count || !IsIdentifierToken(tokens[cursor])) return;
                site.typeName = tokens[cursor].text;
                ++cursor;
                while (true) {
                    if (cursor >= count || !IsIdentifierToken(tokens[cursor])) return;
                    site.variables.push_back(tokens[cursor].text);
                    ++cursor;
                    if (cursor >= count) return;
                    if (tokens[cursor].text == ",") {
                        ++cursor;
                        continue;
                    }
                    // An ARRAY of subroutine uniforms indexes the dispatch itself
                    // ("g_func[i](x)"), which is the dynamic selection this rewrite refuses.
                    if (tokens[cursor].text != ";") return;
                    break;
                }
                site.end = tokens[cursor].end;
                uniformSites.push_back(std::move(site));
                i = cursor;
                continue;
            }

            // (b) `subroutine(<TypeName>, ...) <ret> <name>(<params>) { ... }` - a definition,
            // which only has to shed the qualifier to become an ordinary function.
            if (tokens[i + 1].text == "(") {
                const SizeT listEnd = FindGroupEnd(tokens, i + 1, '(', ')');
                if (listEnd >= count) return;
                Vector<String> listed;
                for (SizeT t = i + 2; t + 1 < listEnd; ++t) {
                    if (tokens[t].text == ",") continue;
                    if (!IsIdentifierToken(tokens[t])) return;
                    listed.push_back(tokens[t].text);
                }
                if (listed.empty()) return;

                SizeT paren = listEnd;
                while (paren < count && tokens[paren].text != "(") {
                    const String& text = tokens[paren].text;
                    if (text == "{" || text == "}" || text == ";" || text == ",") return;
                    ++paren;
                }
                if (paren >= count || paren == listEnd || !IsIdentifierToken(tokens[paren - 1])) return;

                for (const String& typeName : listed) {
                    types[typeName].implementations.push_back(tokens[paren - 1].text);
                }
                blanks.push_back({token.begin, tokens[listEnd - 1].end});
                i = listEnd - 1;
                continue;
            }

            // (c) `subroutine <ret> <TypeName>(<params>);` - the type declaration.
            SizeT paren = i + 1;
            while (paren < count && tokens[paren].text != "(") {
                const String& text = tokens[paren].text;
                if (text == "{" || text == "}" || text == ";" || text == ",") return;
                ++paren;
            }
            if (paren >= count || paren == i + 1 || !IsIdentifierToken(tokens[paren - 1])) return;
            const SizeT listEnd = FindGroupEnd(tokens, paren, '(', ')');
            if (listEnd >= count || tokens[listEnd].text != ";") return;

            SubroutineType& type = types[tokens[paren - 1].text];
            if (!type.returnText.empty()) return; // declared twice; out of scope
            type.returnText = JoinTokenText(tokens, i + 1, paren - 1);
            if (type.returnText.empty()) return;
            if (!ParseSubroutineParameters(tokens, paren + 1, listEnd - 1, type.parameters)) return;
            blanks.push_back({token.begin, tokens[listEnd].end});
            i = listEnd;
        }

        if (blanks.empty() && uniformSites.empty()) return;

        for (const UniformSite& site : uniformSites) {
            const auto known = types.find(site.typeName);
            if (known == types.end() || known->second.returnText.empty()) return;
            if (known->second.implementations.size() != 1) return;
        }

        String appended;
        Vector<std::pair<SizeT, String>> prototypes; // (offset, text), applied back to front
        for (const UniformSite& site : uniformSites) {
            const SubroutineType& type = types.at(site.typeName);
            String parameterList;
            for (const String& declaration : type.parameters.declarations) {
                if (!parameterList.empty()) parameterList += ", ";
                parameterList += declaration;
            }
            String arguments;
            for (const String& argument : type.parameters.arguments) {
                if (!arguments.empty()) arguments += ", ";
                arguments += argument;
            }

            String text;
            for (const String& variable : site.variables) {
                const String signature = type.returnText + " " + variable + "(" + parameterList + ")";
                text += signature + "; ";
                appended += signature + " {\n    " + (type.returnText == "void" ? "" : "return ") +
                    type.implementations.front() + "(" + arguments + ");\n}\n";
            }
            prototypes.emplace_back(site.begin, std::move(text));
        }

        // Blanking first keeps every collected offset valid (it preserves length AND newlines), so
        // only the prototype insertions - which are single-line, and so cost no line numbers - have
        // to run back to front.
        for (const BlankEdit& blank : blanks) {
            BlankSpan(source, blank.begin, blank.end);
        }
        for (const UniformSite& site : uniformSites) {
            BlankSpan(source, site.begin, site.end);
        }
        std::sort(prototypes.begin(), prototypes.end(),
                  [](const std::pair<SizeT, String>& a, const std::pair<SizeT, String>& b) {
                      return a.first < b.first;
                  });
        for (auto it = prototypes.rbegin(); it != prototypes.rend(); ++it) {
            source.insert(it->first, it->second);
        }

        if (!appended.empty()) {
            if (!source.empty() && source.back() != '\n') source += '\n';
            source += appended;
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

    // Whole-identifier search over an already-masked source. A bare find() would fire on
    // "mg_NumSamplesFoo" and on the word inside a comment; this fires only on the token.
    bool MaskedSourceHasIdentifier(const MobileGL::String& masked, MobileGL::StringView identifier) {
        SizeT pos = 0;
        while ((pos = masked.find(identifier.data(), pos, identifier.size())) != MobileGL::String::npos) {
            const SizeT end = pos + identifier.size();
            const bool hasLeftBoundary = pos == 0 || !IsIdentifierChar(masked[pos - 1]);
            const bool hasRightBoundary = end >= masked.size() || !IsIdentifierChar(masked[end]);
            if (hasLeftBoundary && hasRightBoundary) return true;
            pos = end;
        }
        return false;
    }

    // The extension macros glslang's ES preamble defines and its DESKTOP preamble does not
    // (TParseVersions::getPreamble, Versions.cpp). Transcribed rather than derived because the
    // preamble is a string literal inside glslang with no programmatic accessor; the SET is what
    // matters, and it is stable - these are the AEP/OES/EXT names ESSL has carried since 3.10.
    //
    // GL_ES and GL_FRAGMENT_PRECISION_HIGH are DELIBERATELY absent. The shader really is being
    // compiled as desktop by the time this runs, so flipping an `#ifdef GL_ES` branch would hand
    // glslang the ESSL half of a shader written to be portable - which is the branch that does not
    // parse under core 4.60. (GL_FRAGMENT_PRECISION_HIGH is in glslang's desktop preamble anyway.)
    bool IsEsOnlyPreambleExtensionMacro(const MobileGL::String& name, unsigned version) {
        // Guarded by an ES version in glslang's preamble; the rest are unconditional.
        if (name == "GL_NV_shader_noperspective_interpolation") return version >= 300;

        static const std::set<MobileGL::String> kEsOnlyPreambleMacros = {
            "GL_ANDROID_extension_pack_es31a",
            "GL_EXT_YUV_target",
            "GL_EXT_blend_func_extended",
            "GL_EXT_frag_depth",
            "GL_EXT_geometry_point_size",
            "GL_EXT_geometry_shader",
            "GL_EXT_gpu_shader5",
            "GL_EXT_primitive_bounding_box",
            "GL_EXT_shader_implicit_conversions",
            "GL_EXT_shader_io_blocks",
            "GL_EXT_shader_texture_lod",
            "GL_EXT_shadow_samplers",
            "GL_EXT_tessellation_point_size",
            "GL_EXT_tessellation_shader",
            "GL_EXT_texture_buffer",
            "GL_EXT_texture_cube_map_array",
            "GL_OES_EGL_image_external",
            "GL_OES_EGL_image_external_essl3",
            "GL_OES_geometry_point_size",
            "GL_OES_geometry_shader",
            "GL_OES_gpu_shader5",
            "GL_OES_primitive_bounding_box",
            "GL_OES_sample_variables",
            "GL_OES_shader_image_atomic",
            "GL_OES_shader_io_blocks",
            "GL_OES_shader_multisample_interpolation",
            "GL_OES_standard_derivatives",
            "GL_OES_tessellation_point_size",
            "GL_OES_tessellation_shader",
            "GL_OES_texture_3D",
            "GL_OES_texture_buffer",
            "GL_OES_texture_cube_map_array",
            "GL_OES_texture_storage_multisample_2d_array",
        };
        return kEsOnlyPreambleMacros.count(name) != 0;
    }

    // Marker recording that PreprocessShaderSource rewrote an ES-profile source to desktop AND
    // that the source names at least one extension whose macro glslang's ES preamble would have
    // defined. The declared ESSL version rides along because two of those macros are themselves
    // version-gated in glslang.
    //
    // A marker rather than a "#define" block, because the macros CANNOT live in the shader text:
    // glslang rejects "#define GL_..." outright (TParseContext::reservedPpErrorCheck, "names
    // beginning with GL_ can't be (un)defined") for every string the application supplied - but
    // deliberately NOT for the preamble strings, which is where its own ES preamble defines them
    // (CPPdefine's `if (ppToken->loc.string >= 0)` gate; the two preambles sit at string index -2
    // and -1). So the macros have to reach glslang through TShader::setPreamble, and this marker is
    // how the decision - which needs the ORIGINAL profile and version, both gone by then - travels
    // to the compiler. It rides inside the preprocessed source, so the preprocess cache and the
    // translation cache both key on it for free.
    constexpr const char* kEsPreambleMarkerPrefix = "/*mobilegl-es-preamble:";

    // The set of macros named by an ES source that the desktop preamble will not define. Shared by
    // the injector below and by CollectEsPreambleMacroDefines, which re-derives it at compile time
    // from the marker - one whitelist, one version rule, no chance of the two disagreeing.
    MobileGL::String BuildEsPreambleMacroList(const MobileGL::String& source, unsigned esVersion) {
        MobileGL::String macros;
        // std::set iteration order, so the result is deterministic for the caches and for the
        // byte-exact preprocessor tests.
        for (const MobileGL::String& extension : InspectShaderLanguage(source).namedExtensions) {
            if (!IsEsOnlyPreambleExtensionMacro(extension, esVersion)) continue;
            macros += "#define " + extension + " 1\n";
        }
        return macros;
    }

    // GetNormalizedVersionDirective rewrites every ES-profile shader to "#version 460 core", so
    // glslang deduces a desktop profile and emits its DESKTOP preamble - and every ES-only
    // extension macro the shader is entitled to disappears with it. A CTS shader guarded by
    // `#if !GL_OES_sample_variables / this is broken / #endif` then takes the broken branch.
    //
    // The extension BEHAVIOUR survives the rewrite (glslang honours "#extension X : require" under
    // either profile), so this is a preamble-fidelity gap and nothing more; restoring the macros is
    // the whole fix.
    //
    // Strictly limited to extensions the source itself NAMES in an #extension directive. Any macro
    // injected into a desktop parse can flip a preprocessor branch, and the ES preamble carries
    // three dozen of them - defining the lot would rewrite shaders that never asked.
    void MarkEsPreambleExtensionMacros(const ShaderLanguageInfo& info, MobileGL::String& source,
                                       AfterVersionAnchor& afterVersion) {
        // Only where the rewrite actually happened: a malformed directive is left for glslang to
        // reject, and a desktop source already gets the preamble it is entitled to.
        if (info.profile != MobileGL::ShaderProfile::ES) return;
        if (!info.hasValidVersionDirective) return;
        if (info.namedExtensions.empty()) return;

        MobileGL::String macros;
        for (const MobileGL::String& extension : info.namedExtensions) {
            if (!IsEsOnlyPreambleExtensionMacro(extension, info.version)) continue;
            macros += extension;
        }
        // Nothing the desktop preamble is missing: leave the source byte-identical.
        if (macros.empty()) return;

        source.insert(afterVersion.Get(source),
                      MobileGL::String(kEsPreambleMarkerPrefix) + std::to_string(info.version) + "*/\n");
    }

    // gl_NumSamples is legal in this source only where glslang would have declared it with a
    // non-SPIR-V target (Initialize.cpp): desktop from 4.00 core, or from 1.30 with
    // ARB_sample_shading; ESSL from 3.20, or from 3.10 with OES_sample_variables.
    //
    // The gate matters because the shim ends in "#define gl_NumSamples mg_NumSamples", and a
    // #define is not scoped by anything: defining it for a source where the built-in does not
    // exist would silently legalize a shader a conformant implementation rejects.
    bool SourceMayUseSampleVariables(const ShaderLanguageInfo& info) {
        if (!info.HasVersionDirective() || !info.hasValidVersionDirective) return false;
        if (info.profile == MobileGL::ShaderProfile::ES) {
            if (info.version >= 320) return true;
            return info.version >= 310 && info.enabledExtensions.count("GL_OES_sample_variables") != 0;
        }
        if (info.version >= 400) return true;
        return info.version >= 130 && info.enabledExtensions.count("GL_ARB_sample_shading") != 0;
    }

    // gl_NumSamples has no SPIR-V built-in to lower to, so glslang declares it only when it is NOT
    // targeting SPIR-V - both the desktop branch and the ES branch of Initialize.cpp wrap the
    // `uniform int gl_NumSamples;` line in `if (spvVersion.spv == 0)`. MobileGL always targets
    // SPIR-V (ShaderCompiler sets EShTargetSpv on the OpenGL path as well as the Vulkan one), so
    // the symbol is never in the table and every shader that reads it dies at compile time with
    // "'gl_NumSamples' : undeclared identifier".
    //
    // Lower it to a real uniform instead. `uniform int mg_NumSamples;` is a default-block uniform,
    // which the relaxed parse folds into MGL_GLOBAL_UBO - the one buffer BOTH backends already
    // upload per draw - and the draw path writes the current draw framebuffer's sample count into
    // it. Deliberately not a link-time constant: one program may be drawn into framebuffers of
    // different sample counts, and baking the count at link would quietly hand it the wrong one.
    //
    // The alternative - deleting the `spvVersion.spv == 0` guard in the glslang fork - is worse,
    // and not only because it is a fork change: glslang would then place a `gl_`-prefixed member
    // inside MGL_GLOBAL_UBO, and ESSL reserves `gl_`, so the ES driver would reject SPIRV-Cross's
    // output on the DirectGLES path.
    void InjectNumSamplesBuiltinShim(MobileGL::ShaderStage stage, const ShaderLanguageInfo& info,
                                     MobileGL::String& source, AfterVersionAnchor& afterVersion) {
        // gl_NumSamples exists in the fragment stage only, in every profile.
        if (stage != MobileGL::ShaderStage::Fragment) return;
        if (!SourceMayUseSampleVariables(info)) return;
        // Cheap reject before paying for the mask; the token cannot be there if the bytes are not.
        if (source.find("gl_NumSamples") == MobileGL::String::npos) return;

        const MobileGL::String masked = MaskCommentsAndQuotedText(source);
        if (!MaskedSourceHasIdentifier(masked, "gl_NumSamples")) return;
        // Someone already occupies the name - a re-preprocess of an already-shimmed source, or an
        // application that happens to use it. Either way a second declaration would not compile.
        if (MaskedSourceHasIdentifier(masked, MobileGL::MG_Util::ShaderTranspiler::NUM_SAMPLES_UNIFORM_NAME)) {
            return;
        }

        constexpr const char* shim =
            "uniform int mg_NumSamples;\n"
            "#define gl_NumSamples mg_NumSamples\n";
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

                // An ES source rewritten to desktop has lost glslang's ES preamble, and the macros
                // it carried are what the shader's own #if guards read. Keyed off originalLanguage
                // because the directive has already been rewritten by now and no longer says "es";
                // the macros themselves are restored through the compiler's preamble, which is why
                // this only leaves a marker behind (see kEsPreambleMarkerPrefix).
                MarkEsPreambleExtensionMacros(originalLanguage, source, afterVersion);

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

                // Before the builtin-shadowing rename, so the forwarding functions this synthesizes
                // are just as visible to it as the ones the application wrote.
                LowerShaderSubroutines(source);

                RenameBuiltinShadowingFunctions(source);

                ModernizeLegacyGLSL(stage, source, afterVersion);
                InjectDepthRangeBuiltinShim(stage, source, afterVersion);
                InjectNumSamplesBuiltinShim(stage, originalLanguage, source, afterVersion);

            }

            String CollectEsPreambleMacroDefines(const String& preprocessedSource) {
                const SizeT markerStart = preprocessedSource.find(kEsPreambleMarkerPrefix);
                if (markerStart == String::npos) return {};

                SizeT probe = markerStart + std::char_traits<char>::length(kEsPreambleMarkerPrefix);
                unsigned esVersion = 0;
                bool hasDigits = false;
                while (probe < preprocessedSource.size() && preprocessedSource[probe] >= '0' &&
                       preprocessedSource[probe] <= '9') {
                    hasDigits = true;
                    esVersion = esVersion * 10 + static_cast<unsigned>(preprocessedSource[probe] - '0');
                    if (esVersion > 1000) return {}; // absurd; not a marker this pipeline wrote
                    probe++;
                }
                // Only MobileGL's own marker, spelled exactly: a shader that happens to contain the
                // prefix inside a comment of its own must not be able to steer the preamble.
                if (!hasDigits || preprocessedSource.compare(probe, 2, "*/") != 0) return {};

                return BuildEsPreambleMacroList(preprocessedSource, esVersion);
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

            } // namespace

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
                // Several layout(...) lists may precede one declaration and the later one wins:
                // accumulate, then consume at the `buffer` keyword.
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

        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
