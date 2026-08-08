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
#include <climits>
#include <cstdlib>
#include <initializer_list>
#include <utility>
#include <Config.h>
#include <MG_Backend/BackendObjects.h>

#include "EsslBuiltinFunctionNames.h"

namespace {
    using MobileGL::SizeT;
    using MobileGL::String;
    using MobileGL::Vector;

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

    // Blank out block comments in place, leaving line comments and every other byte where it is.
    //
    // The passes that follow scan the source as raw text, so block comments have to stop being
    // visible to them - but they must not be *deleted*: replacing the bytes with spaces keeps every
    // later offset valid and keeps newlines, so glslang's diagnostics still point at the line the
    // application wrote. It also has to be lexically aware. A banner line such as
    //
    //     //*** lighting pass ***
    //
    // contains "/*" one byte in, and a naive search for that opener treats the rest of the file as
    // an unterminated comment.
    void BlankBlockComments(MobileGL::String& source) {
        enum class Region { Code, SingleLineComment, MultiLineComment, QuotedText };

        Region region = Region::Code;
        char quote = '\0';
        bool escaped = false;

        for (SizeT pos = 0; pos < source.size(); pos++) {
            const char ch = source[pos];
            const char next = pos + 1 < source.size() ? source[pos + 1] : '\0';

            if (region == Region::Code) {
                if (ch == '/' && next == '/') {
                    pos++;
                    region = Region::SingleLineComment;
                } else if (ch == '/' && next == '*') {
                    source[pos] = ' ';
                    source[pos + 1] = ' ';
                    pos++;
                    region = Region::MultiLineComment;
                } else if (ch == '"' || ch == '\'') {
                    quote = ch;
                    escaped = false;
                    region = Region::QuotedText;
                }
                continue;
            }

            if (region == Region::SingleLineComment) {
                if (ch == '\n' || ch == '\r') region = Region::Code;
                continue;
            }

            if (region == Region::MultiLineComment) {
                if (ch == '*' && next == '/') {
                    source[pos] = ' ';
                    source[pos + 1] = ' ';
                    pos++;
                    region = Region::Code;
                } else if (ch != '\n' && ch != '\r') {
                    source[pos] = ' ';
                }
                continue;
            }

            // GLSL has no multi-line string literals, so a quote that reaches end of line was never
            // a literal to begin with - most likely an apostrophe in a #error or #pragma message.
            // Ending the region here keeps one stray apostrophe from swallowing the rest of the file.
            if (ch == '\n' || ch == '\r') {
                region = Region::Code;
            } else if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == quote) {
                region = Region::Code;
            }
        }
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

    class TokenCursor {
    public:
        TokenCursor(const Vector<CodeToken>& tokens, SizeT position) : m_tokens(tokens), m_position(position) {}

        bool Consume(const char* expected) {
            if (m_position >= m_tokens.size() || m_tokens[m_position].text != expected) {
                return false;
            }
            ++m_position;
            return true;
        }

        bool ConsumeAnyIdentifier(String& identifier) {
            if (m_position >= m_tokens.size() || !IsIdentifierToken(m_tokens[m_position])) {
                return false;
            }
            identifier = m_tokens[m_position++].text;
            return true;
        }

        bool ConsumeAnyIdentifier() {
            if (m_position >= m_tokens.size() || !IsIdentifierToken(m_tokens[m_position])) {
                return false;
            }
            ++m_position;
            return true;
        }

        bool ConsumeIdentifier(const String& expected) {
            if (m_position >= m_tokens.size() || !IsIdentifierToken(m_tokens[m_position]) ||
                m_tokens[m_position].text != expected) {
                return false;
            }
            ++m_position;
            return true;
        }

        SizeT Position() const { return m_position; }

    private:
        const Vector<CodeToken>& m_tokens;
        SizeT m_position;
    };

    SizeT CountToken(const Vector<CodeToken>& tokens, const String& tokenText) {
        return static_cast<SizeT>(std::count_if(tokens.begin(), tokens.end(),
                                                [&](const CodeToken& token) { return token.text == tokenText; }));
    }

    bool HasIdentifierWithPrefixOutsideAllowed(const Vector<CodeToken>& tokens, const String& prefix,
                                               std::initializer_list<const char*> allowedIdentifiers) {
        return std::any_of(tokens.begin(), tokens.end(), [&](const CodeToken& token) {
            if (!IsIdentifierToken(token) || !token.text.starts_with(prefix)) {
                return false;
            }
            return std::none_of(allowedIdentifiers.begin(), allowedIdentifiers.end(),
                                [&](const char* allowed) { return token.text == allowed; });
        });
    }

    bool MatchTokenSequence(const Vector<CodeToken>& tokens, SizeT position,
                            std::initializer_list<const char*> expected) {
        if (position + expected.size() > tokens.size()) {
            return false;
        }
        for (const char* token : expected) {
            if (tokens[position++].text != token) {
                return false;
            }
        }
        return true;
    }

    struct LinearPrefixScanMatch {
        SizeT sharedArraySizeBegin = 0;
        SizeT sharedArraySizeEnd = 0;
        SizeT scanBegin = 0;
        SizeT scanEnd = 0;
        String cache;
        String importance;
        String prefixSum;
        String loopLength;
        String loopIndex;
        String sum;
    };

    bool ParseLinearPrefixScanTemplate(const Vector<CodeToken>& tokens, LinearPrefixScanMatch& match) {
        // The workaround deliberately recognizes one complete algorithm, not merely the
        // subgroupInclusiveAdd token. Changing scratch storage is only safe when that storage is
        // private to this scan and the workgroup has exactly 1024 X invocations.
        SizeT localSizeDeclarationCount = 0;
        for (SizeT i = 0; i < tokens.size(); ++i) {
            if (MatchTokenSequence(tokens, i, {"layout", "(", "local_size_x", "=", "1024", ")", "in", ";"})) {
                ++localSizeDeclarationCount;
            }
        }
        if (localSizeDeclarationCount != 1) {
            return false;
        }

        SizeT sharedDeclarationIndex = String::npos;
        SizeT sharedDeclarationCount = 0;
        String cacheName;
        for (SizeT i = 0; i + 6 < tokens.size(); ++i) {
            if (tokens[i].text != "shared" || tokens[i + 1].text != "float" || !IsIdentifierToken(tokens[i + 2]) ||
                tokens[i + 3].text != "[" || tokens[i + 4].text != "64" || tokens[i + 5].text != "]" ||
                tokens[i + 6].text != ";") {
                continue;
            }
            ++sharedDeclarationCount;
            sharedDeclarationIndex = i;
            cacheName = tokens[i + 2].text;
        }
        if (sharedDeclarationCount != 1) {
            return false;
        }

        SizeT scanTokenIndex = String::npos;
        SizeT scanCount = 0;
        for (SizeT i = 0; i + 7 < tokens.size(); ++i) {
            if (tokens[i].text == "float" && IsIdentifierToken(tokens[i + 1]) && tokens[i + 2].text == "=" &&
                tokens[i + 3].text == "subgroupInclusiveAdd" && tokens[i + 4].text == "(" &&
                IsIdentifierToken(tokens[i + 5]) && tokens[i + 6].text == ")" && tokens[i + 7].text == ";") {
                ++scanCount;
                scanTokenIndex = i;
            }
        }
        if (scanCount != 1 || sharedDeclarationIndex >= scanTokenIndex) {
            return false;
        }

        TokenCursor cursor(tokens, scanTokenIndex);
        String prefixSum;
        String importance;
        String loopLength;
        String loopIndex;
        String sum;
        if (!cursor.Consume("float") || !cursor.ConsumeAnyIdentifier(prefixSum) || !cursor.Consume("=") ||
            !cursor.Consume("subgroupInclusiveAdd") || !cursor.Consume("(") ||
            !cursor.ConsumeAnyIdentifier(importance) || !cursor.Consume(")") || !cursor.Consume(";") ||
            !cursor.Consume("if") || !cursor.Consume("(") || !cursor.Consume("gl_SubgroupInvocationID") ||
            !cursor.Consume("==") || !cursor.Consume("gl_SubgroupSize") || !cursor.Consume("-") ||
            !cursor.Consume("1u") || !cursor.Consume(")") || !cursor.ConsumeIdentifier(cacheName) ||
            !cursor.Consume("[") || !cursor.Consume("gl_SubgroupID") || !cursor.Consume("]") || !cursor.Consume("=") ||
            !cursor.ConsumeIdentifier(prefixSum) || !cursor.Consume(";") || !cursor.Consume("barrier") ||
            !cursor.Consume("(") || !cursor.Consume(")") || !cursor.Consume(";") || !cursor.Consume("uint") ||
            !cursor.ConsumeAnyIdentifier(loopLength) || !cursor.Consume("=") || !cursor.Consume("uint") ||
            !cursor.Consume("(") || !cursor.Consume("findMSB") || !cursor.Consume("(") ||
            !cursor.Consume("gl_NumSubgroups") || !cursor.Consume(")") || !cursor.Consume(")") ||
            !cursor.Consume(";") || !cursor.ConsumeIdentifier(loopLength) || !cursor.Consume("+=") ||
            !cursor.Consume("uint") || !cursor.Consume("(") || !cursor.Consume("gl_NumSubgroups") ||
            !cursor.Consume("-") || !cursor.Consume("(") || !cursor.Consume("1u") || !cursor.Consume("<<") ||
            !cursor.Consume("(") || !cursor.ConsumeIdentifier(loopLength) || !cursor.Consume("-") ||
            !cursor.Consume("1u") || !cursor.Consume(")") || !cursor.Consume(")") || !cursor.Consume(">") ||
            !cursor.Consume("0u") || !cursor.Consume(")") || !cursor.Consume(";") || !cursor.Consume("for") ||
            !cursor.Consume("(") || !cursor.Consume("uint") || !cursor.ConsumeAnyIdentifier(loopIndex) ||
            !cursor.Consume("=") || !cursor.Consume("0") || !cursor.Consume(";") ||
            !cursor.ConsumeIdentifier(loopIndex) || !cursor.Consume("<") || !cursor.ConsumeIdentifier(loopLength) ||
            !cursor.Consume(";") || !cursor.ConsumeIdentifier(loopIndex) || !cursor.Consume("++") ||
            !cursor.Consume(")") || !cursor.Consume("{") || !cursor.Consume("if") || !cursor.Consume("(") ||
            !cursor.Consume("(") || !cursor.Consume("gl_SubgroupID") || !cursor.Consume("&") || !cursor.Consume("(") ||
            !cursor.Consume("1u") || !cursor.Consume("<<") || !cursor.ConsumeIdentifier(loopIndex) ||
            !cursor.Consume(")") || !cursor.Consume(")") || !cursor.Consume(">") || !cursor.Consume("0u") ||
            !cursor.Consume(")") || !cursor.Consume("{") || !cursor.ConsumeIdentifier(prefixSum) ||
            !cursor.Consume("+=") || !cursor.ConsumeIdentifier(cacheName) || !cursor.Consume("[") ||
            !cursor.Consume("(") || !cursor.Consume("gl_SubgroupID") || !cursor.Consume(">>") ||
            !cursor.ConsumeIdentifier(loopIndex) || !cursor.Consume("<<") || !cursor.ConsumeIdentifier(loopIndex) ||
            !cursor.Consume(")") || !cursor.Consume("-") || !cursor.Consume("1u") || !cursor.Consume("]") ||
            !cursor.Consume(";") || !cursor.Consume("if") || !cursor.Consume("(") ||
            !cursor.Consume("gl_SubgroupInvocationID") || !cursor.Consume("==") || !cursor.Consume("gl_SubgroupSize") ||
            !cursor.Consume("-") || !cursor.Consume("1u") || !cursor.Consume(")") ||
            !cursor.ConsumeIdentifier(cacheName) || !cursor.Consume("[") || !cursor.Consume("gl_SubgroupID") ||
            !cursor.Consume("]") || !cursor.Consume("=") || !cursor.ConsumeIdentifier(prefixSum) ||
            !cursor.Consume(";") || !cursor.Consume("}") || !cursor.Consume("barrier") || !cursor.Consume("(") ||
            !cursor.Consume(")") || !cursor.Consume(";") || !cursor.Consume("}") || !cursor.Consume("if") ||
            !cursor.Consume("(") || !cursor.Consume("gl_LocalInvocationID") || !cursor.Consume(".") ||
            !cursor.Consume("x") || !cursor.Consume("==") || !cursor.Consume("uint") || !cursor.Consume("(") ||
            !cursor.Consume("1024") || !cursor.Consume("-") || !cursor.Consume("1") || !cursor.Consume(")") ||
            !cursor.Consume(")") || !cursor.ConsumeIdentifier(cacheName) || !cursor.Consume("[") ||
            !cursor.Consume("0") || !cursor.Consume("]") || !cursor.Consume("=") ||
            !cursor.ConsumeIdentifier(prefixSum) || !cursor.Consume(";") || !cursor.Consume("barrier") ||
            !cursor.Consume("(") || !cursor.Consume(")") || !cursor.Consume(";") || !cursor.Consume("float") ||
            !cursor.ConsumeAnyIdentifier(sum) || !cursor.Consume("=") || !cursor.ConsumeIdentifier(cacheName) ||
            !cursor.Consume("[") || !cursor.Consume("0") || !cursor.Consume("]") || !cursor.Consume(";")) {
            return false;
        }
        const SizeT scanEndToken = cursor.Position() - 1;

        // Require the scan's immediate consumer as well. This makes the match specific to a
        // linear distribution warp, and avoids changing unrelated prefix scans which may rely on
        // the implementation's native subgroup partitioning.
        if (!cursor.Consume("float") || !cursor.ConsumeAnyIdentifier() || !cursor.Consume("=") ||
            !cursor.Consume("(") || !cursor.ConsumeIdentifier(prefixSum) || !cursor.Consume("-") ||
            !cursor.ConsumeIdentifier(importance) || !cursor.Consume(")") || !cursor.Consume("/") ||
            !cursor.ConsumeIdentifier(sum) || !cursor.Consume("-") || !cursor.Consume("float") ||
            !cursor.Consume("(") || !cursor.Consume("gl_LocalInvocationID") || !cursor.Consume(".") ||
            !cursor.Consume("x") || !cursor.Consume("+") || !cursor.Consume("1u") || !cursor.Consume(")") ||
            !cursor.Consume("/") || !cursor.Consume("float") || !cursor.Consume("(") || !cursor.Consume("1024") ||
            !cursor.Consume(")") || !cursor.Consume(";")) {
            return false;
        }

        // No other use may share the scratch array, and no additional subgroup operation or
        // builtin may silently retain native-64 semantics after this module becomes virtual-32.
        if (CountToken(tokens, cacheName) != 6 || CountToken(tokens, "subgroupInclusiveAdd") != 1 ||
            CountToken(tokens, "gl_SubgroupInvocationID") != 2 || CountToken(tokens, "gl_SubgroupSize") != 2 ||
            CountToken(tokens, "gl_SubgroupID") != 4 || CountToken(tokens, "gl_NumSubgroups") != 2 ||
            CountToken(tokens, "gl_LocalInvocationID") != 2 || CountToken(tokens, "barrier") != 3 ||
            CountToken(tokens, "findMSB") != 1 ||
            HasIdentifierWithPrefixOutsideAllowed(tokens, "subgroup", {"subgroupInclusiveAdd"}) ||
            HasIdentifierWithPrefixOutsideAllowed(
                tokens, "gl_Subgroup",
                {"gl_SubgroupInvocationID", "gl_SubgroupSize", "gl_SubgroupID", "gl_NumSubgroups"}) ||
            // ARB/NV spellings of lane-width-sensitive builtins and functions
            // (gl_SubGroupSizeARB, ballotARB, gl_WarpSizeNV, shuffleNV, ...) must block the
            // rewrite just like their KHR counterparts: they would silently keep native-width
            // semantics in a module rewritten to the virtual 32-lane model.
            HasIdentifierWithPrefixOutsideAllowed(tokens, "gl_SubGroup", {}) ||
            HasIdentifierWithPrefixOutsideAllowed(tokens, "gl_Warp", {}) ||
            HasIdentifierWithPrefixOutsideAllowed(tokens, "gl_Thread", {}) ||
            HasIdentifierWithPrefixOutsideAllowed(tokens, "gl_SMID", {}) ||
            HasIdentifierWithPrefixOutsideAllowed(tokens, "ballot", {}) ||
            HasIdentifierWithPrefixOutsideAllowed(tokens, "shuffle", {}) ||
            HasIdentifierWithPrefixOutsideAllowed(tokens, "readInvocation", {}) ||
            HasIdentifierWithPrefixOutsideAllowed(tokens, "readFirstInvocation", {}) ||
            HasIdentifierWithPrefixOutsideAllowed(tokens, "anyInvocation", {}) ||
            HasIdentifierWithPrefixOutsideAllowed(tokens, "allInvocations", {})) {
            return false;
        }

        // The scan must be at the top level of the sole main() body. Its existing barriers already
        // require uniform control flow; this check prevents us from introducing extra barriers in
        // a nested branch or loop.
        SizeT mainOpenBrace = String::npos;
        SizeT mainCloseBrace = String::npos;
        SizeT mainCount = 0;
        for (SizeT i = 0; i + 4 < tokens.size(); ++i) {
            if (!MatchTokenSequence(tokens, i, {"void", "main", "(", ")", "{"})) {
                continue;
            }
            ++mainCount;
            mainOpenBrace = i + 4;
            int depth = 1;
            for (SizeT j = mainOpenBrace + 1; j < tokens.size(); ++j) {
                if (tokens[j].text == "{")
                    ++depth;
                else if (tokens[j].text == "}" && --depth == 0) {
                    mainCloseBrace = j;
                    break;
                }
            }
        }
        if (mainCount != 1 || mainCloseBrace == String::npos || scanTokenIndex <= mainOpenBrace ||
            scanEndToken >= mainCloseBrace) {
            return false;
        }
        int depthAtScan = 1;
        for (SizeT i = mainOpenBrace + 1; i < scanTokenIndex; ++i) {
            if (tokens[i].text == "{")
                ++depthAtScan;
            else if (tokens[i].text == "}")
                --depthAtScan;
        }
        if (depthAtScan != 1) {
            return false;
        }

        constexpr const char* injectedNames[] = {"mglPrefixScanLane",  "mglVirtualSubgroupInvocation",
                                                 "mglVirtualSubgroup", "mglVirtualSubgroupBase",
                                                 "mglPrefixLane",      "mglVirtualSubgroupCount"};
        for (const char* injectedName : injectedNames) {
            if (CountToken(tokens, injectedName) != 0) {
                return false;
            }
        }

        match.sharedArraySizeBegin = tokens[sharedDeclarationIndex + 4].begin;
        match.sharedArraySizeEnd = tokens[sharedDeclarationIndex + 4].end;
        match.scanBegin = tokens[scanTokenIndex].begin;
        match.scanEnd = tokens[scanEndToken].end;
        match.cache = std::move(cacheName);
        match.importance = std::move(importance);
        match.prefixSum = std::move(prefixSum);
        match.loopLength = std::move(loopLength);
        match.loopIndex = std::move(loopIndex);
        match.sum = std::move(sum);
        return true;
    }

    String BuildLinearPrefixScanReplacement(const LinearPrefixScanMatch& match) {
        String replacement;
        replacement.reserve(1800);
        replacement += "uint mglPrefixScanLane = gl_LocalInvocationID.x;\n";
        replacement += "uint mglVirtualSubgroupInvocation = mglPrefixScanLane & 31u;\n";
        replacement += "uint mglVirtualSubgroup = mglPrefixScanLane >> 5u;\n";
        replacement += "const uint mglVirtualSubgroupCount = 32u;\n";
        replacement += match.cache + "[mglPrefixScanLane] = " + match.importance + ";\n";
        replacement += "barrier();\n";
        replacement += "float " + match.prefixSum + " = 0.0f;\n";
        replacement += "uint mglVirtualSubgroupBase = mglVirtualSubgroup << 5u;\n";
        replacement += "for (uint mglPrefixLane = mglVirtualSubgroupBase; "
                       "mglPrefixLane <= mglPrefixScanLane; ++mglPrefixLane) {\n";
        replacement += match.prefixSum + " += " + match.cache + "[mglPrefixLane];\n";
        replacement += "}\n";
        replacement += "barrier();\n";
        replacement += "if (mglVirtualSubgroupInvocation == 31u) " + match.cache +
                       "[mglVirtualSubgroup] = " + match.prefixSum + ";\n";
        replacement += "barrier();\n";
        replacement += "uint " + match.loopLength + " = uint(findMSB(mglVirtualSubgroupCount));\n";
        replacement +=
            match.loopLength + " += uint(mglVirtualSubgroupCount - (1u << (" + match.loopLength + " - 1u)) > 0u);\n";
        replacement += "for (uint " + match.loopIndex + " = 0u; " + match.loopIndex + " < " + match.loopLength +
                       "; ++" + match.loopIndex + ") {\n";
        replacement += "if ((mglVirtualSubgroup & (1u << " + match.loopIndex + ")) > 0u) {\n";
        replacement += match.prefixSum + " += " + match.cache + "[(mglVirtualSubgroup >> " + match.loopIndex + " << " +
                       match.loopIndex + ") - 1u];\n";
        replacement += "if (mglVirtualSubgroupInvocation == 31u) " + match.cache +
                       "[mglVirtualSubgroup] = " + match.prefixSum + ";\n";
        replacement += "}\nbarrier();\n}\n";
        replacement += "if (mglPrefixScanLane == 1023u) " + match.cache + "[0] = " + match.prefixSum + ";\n";
        replacement += "barrier();\n";
        replacement += "float " + match.sum + " = " + match.cache + "[0];";
        return replacement;
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

    void NormalizeVersionDirective(MobileGL::String& source, const ShaderLanguageInfo& info) {
        // A malformed #version (329, 331, bad profile, float/trailing tokens) is left exactly as the
        // application wrote it so glslang rejects it - rewriting it to "#version 330 core" would
        // silently legalize the CTS directive.version_* rejection cases. Still drop a leading BOM so
        // the reported error is the bad version rather than a stray byte-order mark.
        if (info.HasVersionDirective() && !info.hasValidVersionDirective) {
            if (info.hasUtf8Bom) {
                source.erase(0, 3);
            }
            return;
        }

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

    // GLSL's #line takes integer expressions only, but plenty of shader-pack preprocessors emit the
    // C form with a quoted filename. Deleting every #line outright made those harmless - at the cost
    // of __LINE__ reporting the position in MobileGL's rewritten text rather than the one the pack
    // author wrote, and of every later diagnostic pointing at the wrong line. Dropping just the
    // quoted operand keeps the directive doing its job and still hands glslang something it accepts.
    void NormalizeLineDirectives(MobileGL::String& source) {
        const MobileGL::String masked = MaskCommentsAndQuotedText(source);
        const SizeT versionEnd = FindAfterVersionDirective(source);
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
            Bool RewriteLinearSubgroupPrefixScanForVulkan(ShaderStage stage, Uint32 nativeSubgroupSize,
                                                          String& source) {
                constexpr Uint32 capturedSubgroupSize = 32;
                if (stage != ShaderStage::Compute || nativeSubgroupSize <= capturedSubgroupSize ||
                    nativeSubgroupSize % capturedSubgroupSize != 0) {
                    return false;
                }

                // Vulkan subgroup widths are powers of two. Keep the workaround restricted to
                // wider widths which are a power-of-two multiple of the captured 32-lane model.
                const Uint32 subgroupScale = nativeSubgroupSize / capturedSubgroupSize;
                if ((subgroupScale & (subgroupScale - 1u)) != 0u) {
                    return false;
                }

                const Vector<CodeToken> tokens = TokenizeCode(source);
                LinearPrefixScanMatch match;
                if (!ParseLinearPrefixScanTemplate(tokens, match)) {
                    // Diagnosability: when the trigger op is present but the template no longer
                    // matches (e.g. the pack shipped a new shader revision), the affected device
                    // silently falls back to the driver's miscompiled path. Make that visible.
                    if (CountToken(tokens, "subgroupInclusiveAdd") > 0) {
                        MGLOG_W("%s: subgroupInclusiveAdd present but the linear prefix-scan template "
                                "did not match; the wide-subgroup rewrite was NOT applied",
                                __func__);
                    }
                    return false;
                }

                const String replacement = BuildLinearPrefixScanReplacement(match);
                source.replace(match.scanBegin, match.scanEnd - match.scanBegin, replacement);
                // The declaration occurs before the replaced scan, so its original offsets remain
                // valid after the first replacement.
                source.replace(match.sharedArraySizeBegin, match.sharedArraySizeEnd - match.sharedArraySizeBegin,
                               "1024");
                return true;
            }

            namespace {
                struct ShaderSourceQuirkContext {
                    ShaderStage stage = ShaderStage::Unknown;
                    BackendType backend = BackendType::Unknown;
                    MG_Backend::GpuVendorKind vendor = MG_Backend::GpuVendorKind::Unknown;
                    Uint32 subgroupSize = 0;
                };

                // Device-quirk registry. Every entry is a narrowly scoped source rewrite that
                // works around a specific driver defect. A quirk runs when its env override
                // forces it on, or when the override is Auto and DeviceApplies matches the
                // detected device. ForceOn bypasses only the device gate - each Apply keeps
                // its own structural safety checks. Add new per-device workarounds here
                // instead of open-coding them in PreprocessShaderSource.
                struct ShaderSourceQuirk {
                    const char* name;
                    MG_Config::QuirkOverride (*GetOverride)();
                    Bool (*DeviceApplies)(const ShaderSourceQuirkContext&);
                    Bool (*Apply)(const ShaderSourceQuirkContext&, String&);
                };

                constexpr ShaderSourceQuirk kShaderSourceQuirks[] = {
                    {
                        // MOBILEGL_QUIRK_SUBGROUP_PREFIX_SCAN
                        "subgroup-prefix-scan-rewrite",
                        [] { return MG_Config::Features.SubgroupPrefixScanQuirk; },
                        [](const ShaderSourceQuirkContext& ctx) {
                            // Qualcomm's Vulkan driver miscompiles the recognized float
                            // InclusiveScan pattern for native subgroups wider than the
                            // captured 32 lanes; other vendors compile it correctly and
                            // should keep their native scan.
                            return ctx.backend == BackendType::DirectVulkan &&
                                   ctx.vendor == MG_Backend::GpuVendorKind::Qualcomm;
                        },
                        [](const ShaderSourceQuirkContext& ctx, String& source) {
                            return RewriteLinearSubgroupPrefixScanForVulkan(ctx.stage, ctx.subgroupSize,
                                                                            source);
                        },
                    },
                };

                void ApplyShaderSourceQuirks(ShaderStage stage, String& source) {
                    const auto& activeBackend = MG_Backend::pActiveBackendObject;
                    if (!activeBackend) {
                        return;
                    }
                    const auto& dynamicParameters = activeBackend->GetDynamicParameters();
                    const ShaderSourceQuirkContext quirkContext{
                        stage,
                        activeBackend->GetBackendType(),
                        dynamicParameters.GpuVendor,
                        dynamicParameters.SubgroupSize,
                    };
                    for (const ShaderSourceQuirk& quirk : kShaderSourceQuirks) {
                        const MG_Config::QuirkOverride quirkOverride = quirk.GetOverride();
                        if (quirkOverride == MG_Config::QuirkOverride::ForceOff) {
                            continue;
                        }
                        if (quirkOverride == MG_Config::QuirkOverride::Auto &&
                            !quirk.DeviceApplies(quirkContext)) {
                            continue;
                        }
                        if (quirk.Apply(quirkContext, source)) {
                            MGLOG_I("ApplyShaderSourceQuirks: applied '%s'%s", quirk.name,
                                    quirkOverride == MG_Config::QuirkOverride::ForceOn ? " (forced on)" : "");
                        }
                    }
                }
            } // namespace

            void PreprocessShaderSource(ShaderStage stage, String& source) {
                // Normalize while the inspector's source span still refers to the untouched input. Later passes
                // remove comments and directives, so any subsequent insertion re-inspects the current source.
                const ShaderLanguageInfo originalLanguage = InspectShaderLanguage(source);
                NormalizeVersionDirective(source, originalLanguage);

                // Comments are left intact for glslang's own preprocessor: a block comment is a single
                // preprocessing token that collapses to one space even across newlines and inside a
                // directive, so blanking it here (which preserved the interior newlines) truncated
                // multi-line #define bodies and broke otherwise-valid shaders (KHR-GL3x.shaders.
                // preprocessor multiline_comment_define / redefine_object / function_redefinition).
                // Every MobileGL pass that must ignore comment/string text already masks them locally
                // via MaskCommentsAndQuotedText/TokenizeCode, so the source we hand glslang keeps them.
                NormalizeLineDirectives(source);

                // noperspective is intentionally NOT touched here. It is core in desktop GLSL (1.30+)
                // and maps to the core SPIR-V NoPerspective decoration, which DirectVulkan renders
                // natively and SPIRV-Cross turns into ESSL `noperspective` + the
                // GL_NV_shader_noperspective_interpolation extension. The old naked substring erase
                // both discarded that interpolation (shader packs need it) and corrupted any
                // identifier that merely contained the word. The GLES fallback for devices without
                // the extension lives in the backend, where device capabilities are known.

                FilterUnsupportedGpuShaderInt64(source);
                CoerceUniformBlockPackingToStd140(source);

                RenameBuiltinShadowingFunctions(source);

                ModernizeLegacyGLSL(stage, source);
                InjectDepthRangeBuiltinShim(stage, source);

                ApplyShaderSourceQuirks(stage, source);
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

                bool IsDecimalIntegerToken(const String& text) {
                    if (text.empty()) return false;
                    return std::all_of(text.begin(), text.end(),
                                       [](char ch) { return ch >= '0' && ch <= '9'; });
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
                                           tokens[j + 1].text == "=" && IsDecimalIntegerToken(tokens[j + 2].text)) {
                                    location = std::min(std::strtoll(tokens[j + 2].text.c_str(), nullptr, 10),
                                                        static_cast<long long>(INT_MAX / 2));
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
                        while (k < end && tokens[k].text == "[") {
                            ++k;
                            long long dimension = 1;
                            if (k < end && IsDecimalIntegerToken(tokens[k].text)) {
                                dimension = std::strtoll(tokens[k].text.c_str(), nullptr, 10);
                                ++k;
                            }
                            if (k >= end || tokens[k].text != "]") return; // sized by expression; bail out
                            ++k;
                            span *= std::max(1ll, std::min(dimension, static_cast<long long>(INT_MAX / 2)));
                        }
                        // Keep the first sighting: a duplicate can only come from alternative
                        // preprocessor branches declaring the same name.
                        locations.emplace(name, static_cast<Int>(std::min(
                                                    nextLocation, static_cast<long long>(INT_MAX / 2))));
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
                                           tokens[j + 1].text == "=" && IsDecimalIntegerToken(tokens[j + 2].text)) {
                                    binding = std::min(std::strtoll(tokens[j + 2].text.c_str(), nullptr, 10),
                                                       static_cast<long long>(INT_MAX / 2));
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
                            if (k < end && IsDecimalIntegerToken(tokens[k].text)) ++k;
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
