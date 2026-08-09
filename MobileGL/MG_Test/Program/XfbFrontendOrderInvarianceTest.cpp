// MobileGL - MobileGL/MG_Test/Program/XfbFrontendOrderInvarianceTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// The frontend's answer for a transform-feedback program must not depend on what
// was linked before it. This binary asserts exactly that, headlessly: it links a
// clip_distance-shaped program A, then an XFB-shaped program B, and diffs B's
// whole frontend output (xfb varyings and their offsets, strides, buffer mode,
// scattered-capture and geometry-strip verdicts, uniform blocks, attribute and
// uniform counts, and every SPIR-V module byte-for-byte plus its Location /
// Component / Index / Offset / XfbBuffer / XfbStride / BuiltIn / Binding /
// DescriptorSet decorations) against the same B linked with no A ahead of it.
//
// It was written to arbitrate an order-triggered CTS failure - after
// KHR-GLxx.clip_distance.functional ran, every later transform_feedback capture
// case failed on DirectVulkan - and its verdict was NEGATIVE, which is what made
// it worth keeping: B's frontend output is bit-identical under every ordering,
// every flag state (MOBILEGL_ASYNC_SHADER_COMPILE on/off, THREADS unset/1/8) and
// every isolation level below. That ruled out the whole frontend - the P0b
// preprocess cache, the stage-6 adoption map, ProgramState, glslang's shared
// built-in symbol tables, the pool workers' thread_locals - and sent the hunt
// downstream, where the defect actually was: DirectVulkan's per-VAO vertex
// binding memo keyed on recycled heap addresses (see
// MG_IntegrationTest/Scenarios/XfbAfterClipDistanceScenario.cpp). Keep it as the
// standing guard on the negative half of that split: if the frontend ever DOES
// acquire cross-program order sensitivity, this is what says so.
//
// Isolation model - three levels, all in one binary:
//   * FRESH CONTEXT   MG_State::Init() reinstalls pGLContext, which is what
//                     drops the P0b cache, the adoption map and ProgramState.
//                     Process globals (glslang tables, prewarm latch, pool
//                     worker thread_locals) deliberately SURVIVE it, which is
//                     what makes the fresh-context control a bisection step
//                     rather than just a reset.
//   * FRESH PROCESS   ctest runs each gtest case in this binary in the same
//                     process, so the "control first, poisoned second" and
//                     "poisoned first, control second" orderings are split
//                     into two cases whose names sort in opposite orders and
//                     which each capture BOTH snapshots themselves. A truly
//                     fresh process is available by running one case with
//                     --gtest_filter (see the FreshProcess* cases).
//   * CACHE-CLEARED   context kept, but the source text of B is made unique
//                     per run so no P0b/adoption hit is possible at all.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "Config.h"
#include "Includes.h"
#include "Init.h"
#include "MG_Impl/GLImpl/Getter/GL_Getter.h"
#include "MG_Impl/GLImpl/Program/GL_Program.h"
#include "MG_State/GLState/Core.h"
#include "MG_Util/Async/ShaderCompilePool.h"

using namespace MobileGL;
using namespace MobileGL::MG_Impl::GLImpl;

namespace {

    // ---------------------------------------------------------------------------------
    // Flag plumbing (same shape AsyncCompileTest uses)
    // ---------------------------------------------------------------------------------
    class AsyncModeScope {
    public:
        explicit AsyncModeScope(const Bool async)
            : m_saved(MG_Config::Features.AsyncShaderCompile) {
            MG_Config::Features.AsyncShaderCompile =
                async ? MG_Config::QuirkOverride::ForceOn : MG_Config::QuirkOverride::ForceOff;
        }
        ~AsyncModeScope() { MG_Config::Features.AsyncShaderCompile = m_saved; }
        AsyncModeScope(const AsyncModeScope&) = delete;
        AsyncModeScope& operator=(const AsyncModeScope&) = delete;

    private:
        const MG_Config::QuirkOverride m_saved;
    };

    // ---------------------------------------------------------------------------------
    // A: the clip_distance.functional shape
    //   glcClipDistance.cpp, FunctionalTest::m_vertex_shader_code with
    //   CLIP_DISTANCE_REDECLARATION = m_explicit_redeclaration and
    //   CLIP_DISTANCE_SETUP = m_dynamic_array_setter, clip function 0.
    //   ${VERSION} for a KHR-GL40 run is "#version 400".
    // ---------------------------------------------------------------------------------
    String ClipDistanceVs(const int clipCount, const char* version) {
        const String n = std::to_string(clipCount);
        return String(version) +
               "\n"
               "\n"
               "out float gl_ClipDistance[" + n + "];\n"
               "\n"
               "float f(int i)\n"
               "{\n"
               "    return 0.0;\n"
               "}\n"
               "\n"
               "in vec4 position;\n"
               "\n"
               "void main()\n"
               "{\n"
               "    for(int i = 0; i < " + n + "; i++)\n"
               "    {\n"
               "        gl_ClipDistance[i] = f(i);\n"
               "    }\n"
               "\n"
               "    gl_Position  = position;\n"
               "}\n";
    }

    String ClipDistanceFs(const char* version) {
        return String(version) +
               "\n"
               "\n"
               "\n"
               "out highp vec4 color;\n"
               "\n"
               "void main()\n"
               "{\n"
               "    color = vec4(1.0, 0.0, 0.0, 1.0);\n"
               "}\n";
    }

    // ---------------------------------------------------------------------------------
    // B1: the transform_feedback3 skip_components shape
    //   gl3cTransformFeedback3Tests.cpp, TransformFeedbackBaseTestCase::m_shader_vert
    //   at "#version 150", captured with the gl_SkipComponents* varying list.
    //   This is the case whose failure text is the crispest:
    //     "compareArrays(GLfloat):index 1 value -2 != 1"
    // ---------------------------------------------------------------------------------
    String SkipComponentsVs(const char* version, const String& saltComment = String()) {
        return String(version) + "\n" + saltComment +
               "    in vec4 vertex;\n"
               "    out vec4 value1;\n"
               "    out vec4 value2;\n"
               "    out vec4 value3;\n"
               "    out vec4 value4;\n"
               "\n"
               "    void main (void)\n"
               "    {\n"
               "        vec4 temp = vertex;\n"
               "\n"
               "        gl_Position = temp;\n"
               "\n"
               "        value1 = abs(temp) * 1.0;\n"
               "        value2 = abs(temp) * 2.0;\n"
               "        value3 = abs(temp) * 3.0;\n"
               "        value4 = abs(temp) * 4.0;\n"
               "    }\n";
    }

    String SkipComponentsFs(const char* version) {
        return String(version) +
               "\n"
               "    out vec4 fragColor;\n"
               "    void main (void)\n"
               "    {\n"
               "        fragColor = vec4(0.0, 0.0, 0.0, 1.0);\n"
               "    }\n";
    }

    Vector<String> SkipComponentsVaryings() {
        return {"gl_SkipComponents1", "value1", "gl_SkipComponents2", "gl_SkipComponents1", "value2",
                "gl_SkipComponents3", "gl_SkipComponents2", "value3", "gl_SkipComponents4", "value4"};
    }

    // ---------------------------------------------------------------------------------
    // B2: the capture_vertex_interleaved shape
    //   gl3cTransformFeedbackTests.cpp, CaptureVertexInterleaved::
    //   s_vertex_shader_source_code_template at "#version 130", with
    //   MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS/4 - 1 user vec4 outputs
    //   plus gl_Position as the final captured varying.
    // ---------------------------------------------------------------------------------
    String CaptureInterleavedVs(const int userVaryings, const char* version) {
        String declarations;
        String setters;
        for (int i = 0; i < userVaryings; ++i) {
            const String name = "result_" + std::to_string(i);
            declarations += "out vec4 " + name + ";\n";
            setters += "    " + name + " = vec4(" + std::to_string(i * 4) + ".0, " +
                       std::to_string(i * 4 + 1) + ".0, " + std::to_string(i * 4 + 2) + ".0, " +
                       std::to_string(i * 4 + 3) + ".0);\n";
        }
        return String(version) + "\n\n" + declarations + "\n" +
               "void main()\n"
               "{\n" +
               setters +
               "\n"
               "    vec4 position = vec4(0.0);\n"
               "\n"
               "    switch(gl_VertexID)\n"
               "    {\n"
               "        case 0:\n"
               "            position = vec4(-1.0 + 0.0625,  1.0 - 0.0625,  0.0,  1.0);\n"
               "            break;\n"
               "        case 1:\n"
               "            position = vec4( 1.0 - 0.0625,  1.0 - 0.0625,  0.0,  1.0);\n"
               "            break;\n"
               "        case 2:\n"
               "            position = vec4(-1.0 + 0.0625, -1.0 + 0.0625,  0.0,  1.0);\n"
               "            break;\n"
               "        case 3:\n"
               "            position = vec4( 1.0 - 0.0625, -1.0 + 0.0625,  0.0,  1.0);\n"
               "            break;\n"
               "    }\n"
               "\n"
               "    gl_Position = position;\n"
               "}\n";
    }

    String CaptureInterleavedFs(const char* version) {
        return String(version) +
               "\n"
               "\n"
               "out vec4 color;\n"
               "\n"
               "void main()\n"
               "{\n"
               "    color = vec4(0.5);\n"
               "}\n";
    }

    Vector<String> CaptureInterleavedVaryings(const int userVaryings) {
        Vector<String> names;
        for (int i = 0; i < userVaryings; ++i) names.push_back("result_" + std::to_string(i));
        names.push_back("gl_Position");
        return names;
    }

    // ---------------------------------------------------------------------------------
    // B3: the capture_geometry_interleaved shape. The only shape that reaches
    //   ResolveGsTriangleStripCapture, i.e. the gsStripTriangles / gsStripCaptureFixup
    //   artifacts - and triangle_strip is the sub-case that needs the fixup.
    // ---------------------------------------------------------------------------------
    const char* kGeometryBlankVs = "#version 130\n"
                                   "\n"
                                   "void main()\n"
                                   "{\n"
                                   "}\n";

    String CaptureGeometryGs(const int userVaryings, const char* outPrimitive) {
        String declarations;
        String setters;
        for (int i = 0; i < userVaryings; ++i) {
            const String name = "result_" + std::to_string(i);
            declarations += "out vec4 " + name + ";\n";
            setters += "    " + name + " = vec4(" + std::to_string(i * 4) + ".0, " +
                       std::to_string(i * 4 + 1) + ".0, " + std::to_string(i * 4 + 2) + ".0, " +
                       std::to_string(i * 4 + 3) + ".0);\n";
        }
        String source = "#version 150\n"
                        "\n"
                        "layout(points) in;\n"
                        "layout(" +
                        String(outPrimitive) +
                        ", max_vertices = 4) out;\n"
                        "\n" +
                        declarations + "\n" +
                        "void main()\n"
                        "{\n";
        const char* positions[] = {"vec4(-1.0 + 0.0625,  1.0 - 0.0625,  0.0,  1.0)",
                                   "vec4( 1.0 - 0.0625,  1.0 - 0.0625,  0.0,  1.0)",
                                   "vec4(-1.0 + 0.0625, -1.0 + 0.0625,  0.0,  1.0)",
                                   "vec4( 1.0 - 0.0625, -1.0 + 0.0625,  0.0,  1.0)"};
        for (const char* position : positions) {
            source += String("\n    gl_Position = ") + position + ";\n";
            source += setters;
            source += "    EmitVertex();\n";
        }
        source += "}\n";
        return source;
    }

    GLuint BuildProgramWithGeometry(const String& vertexSource, const String& geometrySource,
                                    const String& fragmentSource, const Vector<String>& xfbVaryings);

    // ---------------------------------------------------------------------------------
    // SPIR-V digest: hash + every decoration that could express a slot shift, resolved
    // through OpName so the text is stable across id renumbering.
    // ---------------------------------------------------------------------------------
    constexpr Uint32 kOpName = 5;
    constexpr Uint32 kOpMemberName = 6;
    constexpr Uint32 kOpEntryPoint = 15;
    constexpr Uint32 kOpDecorate = 71;
    constexpr Uint32 kOpMemberDecorate = 72;

    const char* DecorationName(const Uint32 decoration) {
        switch (decoration) {
        case 11: return "BuiltIn";
        case 30: return "Location";
        case 31: return "Component";
        case 32: return "Index";
        case 33: return "Binding";
        case 34: return "DescriptorSet";
        case 35: return "Offset";
        case 36: return "XfbBuffer";
        case 37: return "XfbStride";
        case 38: return "FuncParamAttr";
        default: return nullptr;
        }
    }

    const char* BuiltInName(const Uint32 builtIn) {
        switch (builtIn) {
        case 0: return "Position";
        case 1: return "PointSize";
        case 3: return "ClipDistance";
        case 4: return "CullDistance";
        case 5: return "VertexId";
        case 42: return "VertexIndex";
        default: return nullptr;
        }
    }

    String ReadSpirvString(const Vector<unsigned>& words, const SizeT firstWord, const SizeT endWord,
                           SizeT& outNextWord) {
        String text;
        SizeT w = firstWord;
        for (; w < endWord; ++w) {
            const Uint32 word = words[w];
            Bool done = false;
            for (int b = 0; b < 4; ++b) {
                const char c = static_cast<char>((word >> (8 * b)) & 0xFF);
                if (c == '\0') {
                    done = true;
                    break;
                }
                text.push_back(c);
            }
            if (done) {
                ++w;
                break;
            }
        }
        outNextWord = w;
        return text;
    }

    Uint64 Fnv1a(const Vector<unsigned>& words) {
        Uint64 hash = 1469598103934665603ULL;
        for (const unsigned word : words) {
            for (int b = 0; b < 4; ++b) {
                hash ^= static_cast<Uint64>((word >> (8 * b)) & 0xFF);
                hash *= 1099511628211ULL;
            }
        }
        return hash;
    }

    struct SpirvDigest {
        Uint64 hash = 0;
        SizeT wordCount = 0;
        Vector<String> decorations;
        Vector<String> interfaceNames;
    };

    SpirvDigest DigestSpirv(const Vector<unsigned>& words) {
        SpirvDigest digest;
        digest.hash = Fnv1a(words);
        digest.wordCount = words.size();
        if (words.size() < 5 || words[0] != 0x07230203u) {
            digest.decorations.push_back("<not a SPIR-V module>");
            return digest;
        }

        UnorderedMap<Uint32, String> names;
        Vector<Uint32> interfaceIds;
        struct PendingDecoration {
            Uint32 target;
            Int member; // -1 for OpDecorate
            Uint32 decoration;
            Vector<Uint32> operands;
        };
        Vector<PendingDecoration> pending;

        SizeT w = 5;
        while (w < words.size()) {
            const Uint32 header = words[w];
            const Uint32 wordCount = header >> 16;
            const Uint32 opcode = header & 0xFFFFu;
            if (wordCount == 0 || w + wordCount > words.size()) break;

            if (opcode == kOpName && wordCount >= 3) {
                SizeT next = 0;
                names[words[w + 1]] = ReadSpirvString(words, w + 2, w + wordCount, next);
            } else if (opcode == kOpMemberName && wordCount >= 4) {
                SizeT next = 0;
                const String member = ReadSpirvString(words, w + 3, w + wordCount, next);
                names[words[w + 1]] = names.count(words[w + 1]) ? names[words[w + 1]] : String("<struct>");
                (void)member;
            } else if (opcode == kOpEntryPoint && wordCount >= 4) {
                SizeT next = 0;
                (void)ReadSpirvString(words, w + 3, w + wordCount, next);
                for (SizeT i = next; i < w + wordCount; ++i) interfaceIds.push_back(words[i]);
            } else if (opcode == kOpDecorate && wordCount >= 3) {
                PendingDecoration entry{words[w + 1], -1, words[w + 2], {}};
                for (SizeT i = w + 3; i < w + wordCount; ++i) entry.operands.push_back(words[i]);
                pending.push_back(Move(entry));
            } else if (opcode == kOpMemberDecorate && wordCount >= 4) {
                PendingDecoration entry{words[w + 1], static_cast<Int>(words[w + 2]), words[w + 3], {}};
                for (SizeT i = w + 4; i < w + wordCount; ++i) entry.operands.push_back(words[i]);
                pending.push_back(Move(entry));
            }
            w += wordCount;
        }

        const auto label = [&](const Uint32 id) {
            const auto it = names.find(id);
            if (it != names.end() && !it->second.empty()) return it->second;
            return String("%") + std::to_string(id);
        };

        for (const auto& entry : pending) {
            const char* decorationName = DecorationName(entry.decoration);
            if (decorationName == nullptr) continue; // relocation-irrelevant decorations
            String line = label(entry.target);
            if (entry.member >= 0) line += "[member " + std::to_string(entry.member) + "]";
            line += " ";
            line += decorationName;
            line += " =";
            for (const Uint32 operand : entry.operands) {
                if (entry.decoration == 11) {
                    const char* builtIn = BuiltInName(operand);
                    line += String(" ") + (builtIn != nullptr ? builtIn : std::to_string(operand));
                } else {
                    line += " " + std::to_string(operand);
                }
            }
            digest.decorations.push_back(line);
        }
        std::sort(digest.decorations.begin(), digest.decorations.end());

        for (const Uint32 id : interfaceIds) digest.interfaceNames.push_back(label(id));
        std::sort(digest.interfaceNames.begin(), digest.interfaceNames.end());
        return digest;
    }

    // ---------------------------------------------------------------------------------
    // The snapshot under test
    // ---------------------------------------------------------------------------------
    struct XfbSnapshot {
        GLint linkStatus = GL_FALSE;
        String infoLog;
        GLenum bufferMode = 0;
        Uint32 packedStride = 0;
        Bool needsScattered = false;
        Int varyingNameMaxLength = 0;
        Vector<Uint32> strides;
        Vector<String> varyings;
        GLenum gsInputPrimitive = 0;
        Bool gsStripCaptureFixup = false;
        Vector<Uint32> gsStripTriangles;
        Int uniformBlockCount = 0;
        Vector<Uint> uniformBlockBindings;
        Uint maxUniformLocation = 0;
        GLint activeAttributes = 0;
        GLint activeUniforms = 0;
        Vector<SpirvDigest> spirv;
    };

    String QueryProgramInfoLog(const GLuint program) {
        GLint length = 0;
        GetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
        if (length <= 0) return String();
        std::vector<GLchar> buffer(static_cast<size_t>(length));
        GLsizei written = 0;
        GetProgramInfoLog(program, length, &written, buffer.data());
        return String(buffer.data(), static_cast<size_t>(written));
    }

    XfbSnapshot Capture(const GLuint program) {
        XfbSnapshot snapshot;
        GetProgramiv(program, GL_LINK_STATUS, &snapshot.linkStatus);
        snapshot.infoLog = QueryProgramInfoLog(program);

        const auto& object = MG_State::pGLContext->GetProgramObject(program);
        if (object == nullptr) {
            snapshot.infoLog += "<no program object>";
            return snapshot;
        }
        snapshot.bufferMode = object->GetTransformFeedbackBufferMode();
        snapshot.packedStride = object->GetTransformFeedbackPackedStride();
        snapshot.needsScattered = object->NeedsScatteredTransformFeedbackCapture();
        snapshot.varyingNameMaxLength = object->GetTransformFeedbackVaryingMaxLength();
        snapshot.gsInputPrimitive = object->GetGeometryInputType();
        snapshot.gsStripCaptureFixup = object->HasGsTriangleStripCaptureFixup();
        snapshot.gsStripTriangles = object->GetGsStripTriangles();
        // The rest of ProgramFactory::ComputeHash's input set, so "the backend cache key is
        // unchanged" is something this binary measures rather than assumes.
        snapshot.uniformBlockCount = object->GetActiveUniformBlocksCount();
        for (Int i = 0; i < snapshot.uniformBlockCount; ++i) {
            snapshot.uniformBlockBindings.push_back(object->GetUniformBlockBinding(static_cast<Uint>(i)));
        }
        snapshot.maxUniformLocation = object->GetMaxUniformLocation();
        GetProgramiv(program, GL_ACTIVE_ATTRIBUTES, &snapshot.activeAttributes);
        GetProgramiv(program, GL_ACTIVE_UNIFORMS, &snapshot.activeUniforms);
        for (SizeT i = 0; i < object->GetTransformFeedbackBufferCount(); ++i) {
            snapshot.strides.push_back(object->GetTransformFeedbackStride(static_cast<Uint32>(i)));
        }
        for (const auto& varying : object->GetTransformFeedbackVaryings()) {
            snapshot.varyings.push_back(varying.name + " type=0x" + [&] {
                char buffer[16];
                std::snprintf(buffer, sizeof(buffer), "%04X", static_cast<unsigned>(varying.type));
                return String(buffer);
            }() + " size=" + std::to_string(varying.size) + " buf=" + std::to_string(varying.bufferIndex) +
                                        " off=" + std::to_string(varying.offsetBytes) +
                                        " bytes=" + std::to_string(varying.byteSize) +
                                        " packedOff=" + std::to_string(varying.packedOffsetBytes));
        }
        for (const auto& module : object->GetGeneratedSpirv()) {
            snapshot.spirv.push_back(DigestSpirv(module));
        }
        return snapshot;
    }

    // One text blob per snapshot, so a mismatch shows up as a readable gtest diff.
    String Render(const XfbSnapshot& snapshot, const Bool includeSpirvHash) {
        String text;
        text += "linkStatus       = " + std::to_string(snapshot.linkStatus) + "\n";
        if (!snapshot.infoLog.empty()) text += "infoLog          = " + snapshot.infoLog + "\n";
        text += "xfbBufferMode    = " + std::to_string(snapshot.bufferMode) + "\n";
        text += "xfbPackedStride  = " + std::to_string(snapshot.packedStride) + "\n";
        text += "xfbNeedsScatter  = " + std::to_string(static_cast<int>(snapshot.needsScattered)) + "\n";
        text += "xfbNameMaxLength = " + std::to_string(snapshot.varyingNameMaxLength) + "\n";
        text += "xfbStrides       =";
        for (const Uint32 stride : snapshot.strides) text += " " + std::to_string(stride);
        text += "\n";
        text += "xfbVaryings (" + std::to_string(snapshot.varyings.size()) + "):\n";
        for (const String& varying : snapshot.varyings) text += "  " + varying + "\n";
        text += "gsInputPrimitive = " + std::to_string(snapshot.gsInputPrimitive) + "\n";
        text += "gsStripFixup     = " + std::to_string(static_cast<int>(snapshot.gsStripCaptureFixup)) + "\n";
        text += "gsStripTriangles =";
        for (const Uint32 triangle : snapshot.gsStripTriangles) text += " " + std::to_string(triangle);
        text += "\n";
        text += "uniformBlocks    = " + std::to_string(snapshot.uniformBlockCount) + " bindings:";
        for (const Uint binding : snapshot.uniformBlockBindings) text += " " + std::to_string(binding);
        text += "\n";
        text += "maxUniformLoc    = " + std::to_string(snapshot.maxUniformLocation) + "\n";
        text += "activeAttribs    = " + std::to_string(snapshot.activeAttributes) + "\n";
        text += "activeUniforms   = " + std::to_string(snapshot.activeUniforms) + "\n";
        for (SizeT i = 0; i < snapshot.spirv.size(); ++i) {
            const SpirvDigest& digest = snapshot.spirv[i];
            text += "spirv[" + std::to_string(i) + "] words=" + std::to_string(digest.wordCount);
            if (includeSpirvHash) {
                char buffer[32];
                std::snprintf(buffer, sizeof(buffer), " hash=%016llX",
                              static_cast<unsigned long long>(digest.hash));
                text += buffer;
            }
            text += "\n";
            text += "  interface:";
            for (const String& name : digest.interfaceNames) text += " " + name;
            text += "\n";
            for (const String& decoration : digest.decorations) text += "  " + decoration + "\n";
        }
        return text;
    }

    // ---------------------------------------------------------------------------------
    // Program construction through the real GL entry points
    // ---------------------------------------------------------------------------------
    GLuint BuildProgram(const String& vertexSource, const String& fragmentSource,
                        const Vector<String>& xfbVaryings, const GLenum bufferMode) {
        const GLuint vertexShader = CreateShader(GL_VERTEX_SHADER);
        const char* vertexText = vertexSource.c_str();
        ShaderSource(vertexShader, 1, &vertexText, nullptr);
        CompileShader(vertexShader);

        const GLuint fragmentShader = CreateShader(GL_FRAGMENT_SHADER);
        const char* fragmentText = fragmentSource.c_str();
        ShaderSource(fragmentShader, 1, &fragmentText, nullptr);
        CompileShader(fragmentShader);

        const GLuint program = CreateProgram();
        AttachShader(program, vertexShader);
        AttachShader(program, fragmentShader);
        if (!xfbVaryings.empty()) {
            std::vector<const GLchar*> names;
            names.reserve(xfbVaryings.size());
            for (const String& name : xfbVaryings) names.push_back(name.c_str());
            TransformFeedbackVaryings(program, static_cast<GLsizei>(names.size()), names.data(), bufferMode);
        }
        LinkProgram(program);
        DeleteShader(vertexShader);
        DeleteShader(fragmentShader);
        return program;
    }

    // A, exactly as the CTS builds it for the failing sub-case (1 clip distance, dynamic
    // setter, clip function 0). Returns the program so the caller can keep it alive, which
    // is what the CTS does too (it holds m_program across the whole case).
    GLuint LinkClipDistanceProgram(const int clipCount, const char* version) {
        return BuildProgram(ClipDistanceVs(clipCount, version), ClipDistanceFs(version), {},
                            GL_INTERLEAVED_ATTRIBS);
    }

    GLuint LinkSkipComponentsProgram(const char* version, const String& salt = String()) {
        return BuildProgram(SkipComponentsVs(version, salt), SkipComponentsFs(version),
                            SkipComponentsVaryings(), GL_INTERLEAVED_ATTRIBS);
    }

    GLuint LinkCaptureInterleavedProgram(const int userVaryings, const char* version) {
        return BuildProgram(CaptureInterleavedVs(userVaryings, version), CaptureInterleavedFs(version),
                            CaptureInterleavedVaryings(userVaryings), GL_INTERLEAVED_ATTRIBS);
    }

    GLuint BuildProgramWithGeometry(const String& vertexSource, const String& geometrySource,
                                    const String& fragmentSource, const Vector<String>& xfbVaryings) {
        const auto makeShader = [](const GLenum type, const String& source) {
            const GLuint shader = CreateShader(type);
            const char* text = source.c_str();
            ShaderSource(shader, 1, &text, nullptr);
            CompileShader(shader);
            return shader;
        };
        const GLuint vertexShader = makeShader(GL_VERTEX_SHADER, vertexSource);
        const GLuint geometryShader = makeShader(GL_GEOMETRY_SHADER, geometrySource);
        const GLuint fragmentShader = makeShader(GL_FRAGMENT_SHADER, fragmentSource);

        const GLuint program = CreateProgram();
        AttachShader(program, vertexShader);
        AttachShader(program, geometryShader);
        AttachShader(program, fragmentShader);
        std::vector<const GLchar*> names;
        names.reserve(xfbVaryings.size());
        for (const String& name : xfbVaryings) names.push_back(name.c_str());
        TransformFeedbackVaryings(program, static_cast<GLsizei>(names.size()), names.data(),
                                  GL_INTERLEAVED_ATTRIBS);
        LinkProgram(program);
        DeleteShader(vertexShader);
        DeleteShader(geometryShader);
        DeleteShader(fragmentShader);
        return program;
    }

    GLuint LinkCaptureGeometryProgram(const int userVaryings, const char* outPrimitive) {
        return BuildProgramWithGeometry(kGeometryBlankVs, CaptureGeometryGs(userVaryings, outPrimitive),
                                        CaptureInterleavedFs("#version 130"),
                                        CaptureInterleavedVaryings(userVaryings));
    }

    // Reinstalls pGLContext: new ProgramState, new P0b preprocess cache, new stage-6
    // adoption map. glslang's process globals are untouched on purpose.
    void FreshContext() { MG_State::Init(); }

    class XfbFrontendOrderInvarianceTest : public ::testing::Test {
    protected:
        void SetUp() override { MobileGL::Initialize(); }
        void TearDown() override { FreshContext(); }
    };

    // The two B shapes, run through one lambda so every case tests both.
    struct BCase {
        const char* label;
        GLuint (*link)();
    };

    GLuint LinkSkip150() { return LinkSkipComponentsProgram("#version 150"); }
    GLuint LinkCapture130() { return LinkCaptureInterleavedProgram(15, "#version 130"); }
    GLuint LinkSkip400() { return LinkSkipComponentsProgram("#version 400"); }
    GLuint LinkCapture400() { return LinkCaptureInterleavedProgram(15, "#version 400"); }

    GLuint LinkGeometryPoints() { return LinkCaptureGeometryProgram(15, "points"); }
    GLuint LinkGeometryTriangleStrip() { return LinkCaptureGeometryProgram(15, "triangle_strip"); }

    const BCase kBCases[] = {
        {"skip_components@150", &LinkSkip150},
        {"capture_interleaved@130", &LinkCapture130},
        {"skip_components@400", &LinkSkip400},
        {"capture_interleaved@400", &LinkCapture400},
        {"capture_geometry@points", &LinkGeometryPoints},
        {"capture_geometry@triangle_strip", &LinkGeometryTriangleStrip},
    };

    // ---------------------------------------------------------------------------------
    // The core A/B comparison, parameterized on everything that could matter.
    // ---------------------------------------------------------------------------------
    struct AbResult {
        String control;
        String poisoned;
    };

    AbResult RunAb(const BCase& bCase, const int clipCount, const char* clipVersion,
                   const Bool freshContextForControl, const Bool includeSpirvHash) {
        AbResult result;

        // CONTROL: B alone, in a context that has never seen A.
        if (freshContextForControl) FreshContext();
        {
            const GLuint program = bCase.link();
            result.control = Render(Capture(program), includeSpirvHash);
            DeleteProgram(program);
        }

        // POISONED: A first, then B, in ONE context - the glcts shape.
        FreshContext();
        {
            const GLuint clipProgram = LinkClipDistanceProgram(clipCount, clipVersion);
            GLint clipLinked = GL_FALSE;
            GetProgramiv(clipProgram, GL_LINK_STATUS, &clipLinked);
            // The A program is deliberately kept alive across B's link, exactly as the CTS
            // holds its program object for the duration of the case.
            const GLuint program = bCase.link();
            result.poisoned = Render(Capture(program), includeSpirvHash);
            if (clipLinked != GL_TRUE) {
                result.poisoned += "\n<<< A DID NOT LINK: " + QueryProgramInfoLog(clipProgram) + " >>>\n";
            }
            DeleteProgram(program);
            DeleteProgram(clipProgram);
        }
        return result;
    }

} // namespace

// -------------------------------------------------------------------------------------
// 1. The headline question, both flag states, both B shapes, several clip counts.
// -------------------------------------------------------------------------------------
TEST_F(XfbFrontendOrderInvarianceTest, AsyncOn_ClipDistanceBeforeXfbChangesNothingInTheFrontend) {
    const AsyncModeScope async(true);
    ASSERT_TRUE(MG_Util::Async::AsyncShaderCompileEnabled());

    for (const BCase& bCase : kBCases) {
        for (const int clipCount : {1, 4, 8}) {
            for (const char* clipVersion : {"#version 400", "#version 150", "#version 130"}) {
                const AbResult result = RunAb(bCase, clipCount, clipVersion, true, true);
                EXPECT_EQ(result.control, result.poisoned)
                    << "async=1 B=" << bCase.label << " clipCount=" << clipCount
                    << " clipVersion=" << clipVersion;
            }
        }
    }
}

TEST_F(XfbFrontendOrderInvarianceTest, AsyncOff_ClipDistanceBeforeXfbChangesNothingInTheFrontend) {
    const AsyncModeScope async(false);

    for (const BCase& bCase : kBCases) {
        for (const int clipCount : {1, 4, 8}) {
            for (const char* clipVersion : {"#version 400", "#version 150", "#version 130"}) {
                const AbResult result = RunAb(bCase, clipCount, clipVersion, true, true);
                EXPECT_EQ(result.control, result.poisoned)
                    << "async=0 B=" << bCase.label << " clipCount=" << clipCount
                    << " clipVersion=" << clipVersion;
            }
        }
    }
}

// -------------------------------------------------------------------------------------
// 2. Repetition: the CTS incidence with async off is ~2.4%, i.e. roughly 1 in 40 runs, so
//    a single comparison would miss it. 60 repetitions of the same A->B pair inside one
//    process, each with its own fresh context, is the headless equivalent.
// -------------------------------------------------------------------------------------
TEST_F(XfbFrontendOrderInvarianceTest, RepeatedAbPairsAreBitStable) {
    for (const Bool async : {true, false}) {
        const AsyncModeScope scope(async);
        String reference;
        for (int repetition = 0; repetition < 60; ++repetition) {
            const AbResult result = RunAb(kBCases[0], 1, "#version 400", repetition == 0, true);
            if (repetition == 0) {
                reference = result.control;
                ASSERT_EQ(reference, result.poisoned) << "async=" << async << " first repetition";
            }
            EXPECT_EQ(reference, result.poisoned) << "async=" << async << " repetition " << repetition;
        }
    }
}

// -------------------------------------------------------------------------------------
// 3. Same context, no reset between A and B, and B's source made unique so neither the
//    P0b preprocess cache nor the stage-6 adoption map can serve it. If the divergence
//    survives this, no per-source memo is carrying it.
// -------------------------------------------------------------------------------------
TEST_F(XfbFrontendOrderInvarianceTest, NoMemoHitPossibleForB) {
    for (const Bool async : {true, false}) {
        const AsyncModeScope scope(async);

        FreshContext();
        const GLuint controlProgram = LinkSkipComponentsProgram("#version 150", "// salt control\n");
        const String control = Render(Capture(controlProgram), false);
        DeleteProgram(controlProgram);

        FreshContext();
        const GLuint clipProgram = LinkClipDistanceProgram(1, "#version 400");
        const GLuint poisonedProgram = LinkSkipComponentsProgram("#version 150", "// salt poisoned\n");
        const String poisoned = Render(Capture(poisonedProgram), false);
        DeleteProgram(poisonedProgram);
        DeleteProgram(clipProgram);

        EXPECT_EQ(control, poisoned) << "async=" << async << " (SPIR-V hash excluded: the salt comment "
                                        "is stripped by the preprocessor but ids can still renumber)";
    }
}

// -------------------------------------------------------------------------------------
// 4. Bisection: A and B in one context WITHOUT the reset in between, so ProgramState, the
//    P0b cache and the adoption map all carry over exactly as they do in glcts, compared
//    against A and B separated by a fresh context. A difference here but not in case 1
//    would put the poison in per-context state; no difference in either puts it outside
//    the frontend entirely.
// -------------------------------------------------------------------------------------
TEST_F(XfbFrontendOrderInvarianceTest, PerContextStateBisection) {
    for (const Bool async : {true, false}) {
        const AsyncModeScope scope(async);

        // (a) A, fresh context, then B: per-context state cleared, process globals kept.
        FreshContext();
        const GLuint clipA = LinkClipDistanceProgram(1, "#version 400");
        DeleteProgram(clipA);
        FreshContext();
        const GLuint separated = LinkSkipComponentsProgram("#version 150");
        const String separatedText = Render(Capture(separated), true);
        DeleteProgram(separated);

        // (b) A then B, same context, A kept alive.
        FreshContext();
        const GLuint clipB = LinkClipDistanceProgram(1, "#version 400");
        const GLuint together = LinkSkipComponentsProgram("#version 150");
        const String togetherText = Render(Capture(together), true);
        DeleteProgram(together);
        DeleteProgram(clipB);

        EXPECT_EQ(separatedText, togetherText) << "async=" << async;
    }
}

// -------------------------------------------------------------------------------------
// 5. The interleaving glcts actually produces: many cases in a row, A somewhere in the
//    middle, every B compared against the very first B. This is the one that catches a
//    poison that needs more than one link to develop.
// -------------------------------------------------------------------------------------
TEST_F(XfbFrontendOrderInvarianceTest, LongCaseSequenceLikeGlcts) {
    for (const Bool async : {true, false}) {
        const AsyncModeScope scope(async);
        FreshContext();

        String reference;
        Vector<GLuint> keepAlive;
        for (int step = 0; step < 12; ++step) {
            if (step == 4) {
                // The clip_distance case: every clip count, both setters' shapes.
                for (const int clipCount : {1, 2, 4, 8}) {
                    keepAlive.push_back(LinkClipDistanceProgram(clipCount, "#version 400"));
                }
            }
            const GLuint program = LinkSkipComponentsProgram("#version 150");
            const String text = Render(Capture(program), true);
            if (step == 0) {
                reference = text;
            } else {
                EXPECT_EQ(reference, text) << "async=" << async << " step " << step;
            }
            keepAlive.push_back(program);
        }
        for (const GLuint program : keepAlive) DeleteProgram(program);
    }
}

// -------------------------------------------------------------------------------------
// 6. Fresh-process controls. Run exactly one of these with --gtest_filter to get a
//    process that has linked nothing else, then diff the two printed blobs by hand:
//      ./XfbFrontendOrderInvarianceTest --gtest_filter='*FreshProcessControlB*'
//      ./XfbFrontendOrderInvarianceTest --gtest_filter='*FreshProcessAThenB*'
//    Both print their snapshot to stdout; they never fail on their own.
// -------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------
// 7. The one shape only async can produce: A's link is still IN FLIGHT when B's shaders
//    are compiled and B is linked. Nothing joins A until after B has published. If the
//    poison rode a worker thread_local (glslang's pool allocator, its TLS parse context)
//    rather than any per-context container, this is where it would show.
//    N copies of A are enqueued first so the pool really has a backlog.
// -------------------------------------------------------------------------------------
TEST_F(XfbFrontendOrderInvarianceTest, BLinksWhileAIsStillInFlight) {
    const AsyncModeScope async(true);
    ASSERT_TRUE(MG_Util::Async::AsyncShaderCompileEnabled());

    FreshContext();
    const GLuint controlProgram = LinkSkipComponentsProgram("#version 150");
    const String control = Render(Capture(controlProgram), true);
    DeleteProgram(controlProgram);

    for (int repetition = 0; repetition < 20; ++repetition) {
        FreshContext();
        Vector<GLuint> clipPrograms;
        // Enqueued, never read: every one of these links is outstanding while B goes
        // through compile + link on the same pool.
        for (const int clipCount : {1, 2, 3, 4, 5, 6, 7, 8}) {
            clipPrograms.push_back(LinkClipDistanceProgram(clipCount, "#version 400"));
        }
        const GLuint program = LinkSkipComponentsProgram("#version 150");
        const String poisoned = Render(Capture(program), true);
        EXPECT_EQ(control, poisoned) << "repetition " << repetition;
        DeleteProgram(program);
        for (const GLuint clipProgram : clipPrograms) DeleteProgram(clipProgram);
    }
}

// Sanity: every shape this binary compares must actually LINK, otherwise "control ==
// poisoned" is the trivially true statement that two failures look alike.
TEST_F(XfbFrontendOrderInvarianceTest, EveryShapeActuallyLinks) {
    const AsyncModeScope async(true);
    for (const int clipCount : {1, 2, 4, 8}) {
        for (const char* version : {"#version 400", "#version 150", "#version 130"}) {
            FreshContext();
            const GLuint program = LinkClipDistanceProgram(clipCount, version);
            GLint linked = GL_FALSE;
            GetProgramiv(program, GL_LINK_STATUS, &linked);
            EXPECT_EQ(linked, GL_TRUE) << "A clipCount=" << clipCount << " " << version << ": "
                                       << QueryProgramInfoLog(program);
            DeleteProgram(program);
        }
    }
    for (const BCase& bCase : kBCases) {
        FreshContext();
        const GLuint program = bCase.link();
        GLint linked = GL_FALSE;
        GetProgramiv(program, GL_LINK_STATUS, &linked);
        EXPECT_EQ(linked, GL_TRUE) << "B " << bCase.label << ": " << QueryProgramInfoLog(program);
        std::printf("=== B shape %s ===\n%s\n", bCase.label, Render(Capture(program), true).c_str());
        DeleteProgram(program);
    }
}

// Writes B's raw SPIR-V modules next to the binary so they can be run through spirv-dis
// by hand. MOBILEGL_XFB_INVARIANCE_DUMP_DIR selects the directory; unset means no dump.
TEST_F(XfbFrontendOrderInvarianceTest, DumpBSpirvForDisassembly) {
    const char* directory = std::getenv("MOBILEGL_XFB_INVARIANCE_DUMP_DIR");
    if (directory == nullptr) {
        GTEST_SKIP() << "set MOBILEGL_XFB_INVARIANCE_DUMP_DIR to dump";
    }
    const AsyncModeScope async(true);
    struct Dump {
        const char* tag;
        Bool withClipDistanceFirst;
    };
    for (const Dump& dump : {Dump{"control", false}, Dump{"poisoned", true}}) {
        for (const BCase& bCase : kBCases) {
            FreshContext();
            GLuint clipProgram = 0;
            if (dump.withClipDistanceFirst) clipProgram = LinkClipDistanceProgram(1, "#version 400");
            const GLuint program = bCase.link();
            const auto& object = MG_State::pGLContext->GetProgramObject(program);
            const auto& modules = object->GetGeneratedSpirv();
            for (SizeT i = 0; i < modules.size(); ++i) {
                String path = String(directory) + "/" + dump.tag + "-" + bCase.label + "-" +
                              std::to_string(i) + ".spv";
                std::replace(path.begin() + std::strlen(directory) + 1, path.end(), '@', '_');
                std::FILE* file = std::fopen(path.c_str(), "wb");
                ASSERT_NE(file, nullptr) << path;
                std::fwrite(modules[i].data(), sizeof(unsigned), modules[i].size(), file);
                std::fclose(file);
            }
            DeleteProgram(program);
            if (clipProgram != 0) DeleteProgram(clipProgram);
        }
    }
}

TEST_F(XfbFrontendOrderInvarianceTest, FreshProcessControlB) {
    const AsyncModeScope async(true);
    const GLuint program = LinkSkipComponentsProgram("#version 150");
    std::printf("=== FreshProcessControlB ===\n%s\n", Render(Capture(program), true).c_str());
    DeleteProgram(program);
}

TEST_F(XfbFrontendOrderInvarianceTest, FreshProcessAThenB) {
    const AsyncModeScope async(true);
    const GLuint clipProgram = LinkClipDistanceProgram(1, "#version 400");
    const GLuint program = LinkSkipComponentsProgram("#version 150");
    std::printf("=== FreshProcessAThenB ===\n%s\n", Render(Capture(program), true).c_str());
    DeleteProgram(program);
    DeleteProgram(clipProgram);
}
