// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/XfbAfterClipDistanceScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario F - a draw must never read a destroyed object's memoised state.
//
// Distilled from the order-triggered CTS failure: on DirectVulkan, once
// KHR-GLxx.clip_distance.functional had run in the same process, every later
// transform_feedback CAPTURE case failed. It looked like a transform feedback
// bug and is not one. The capture works; the DRAW being captured fetched its
// vertices from the WRONG BUFFER - the one the clip workload had just deleted.
//
// The mechanism, and why the sequence matters. DirectVulkan memoises a VAO's
// resolved Vulkan vertex bindings in a table keyed on the VertexArrayObject's
// heap ADDRESS, validated by a content hash that folds in the bound
// BufferObject's heap ADDRESS. Both are recycled by the allocator, so when the
// workload's VAO and vertex buffer are destroyed and the capture phase's own
// VAO and vertex buffer are allocated onto their addresses under a
// byte-identical attribute layout (one vec4 float array at location 0 - which
// is what both phases use), the key matches, the hash matches, and the memo
// hands the new draw the dead buffer's GPU slice. Nothing about transform
// feedback is involved: capture just makes the wrong vertices legible, because
// the captured record IS the vertex data. The fix gives VertexArrayObject and
// BufferObject never-reused lifetime ids and keys the memo on those.
//
// MOBILEGL_ASYNC_SHADER_COMPILE is not part of the defect. It shifts the
// allocation pattern, so it changes WHICH stop points below land on a recycled
// address - which is why the CTS saw ~100% incidence with it on and ~2% with it
// off, and why the sweep case matters more than any single stop point.
//
// The shapes are the two CTS cases verbatim in structure:
//   * the workload is glcClipDistance.cpp FunctionalTest's inner loop (a program
//     per (redeclaration, clip count), glEnable(GL_CLIP_DISTANCEi), an FBO per
//     primitive type, a draw and a readback), including its early-return
//     behaviour: on failure the test returns WITHOUT running its "clip clean"
//     loop, so GL_CLIP_DISTANCE0..N-1 stay enabled for the rest of the process.
//     That leftover enable state is NOT the carrier (one of the cases below pins
//     that); the object churn is.
//   * the victim is gl3cTransformFeedback3Tests.cpp's skip_components: a
//     gl_SkipComponents capture layout under GL_RASTERIZER_DISCARD, read back
//     out of a buffer pre-filled with -1-i so that "captured nothing" is
//     distinguishable from "captured the wrong thing".

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../Harness/HeadlessGL.h"
#include "../Harness/ScenarioFixture.h"

#ifdef GLAPI
#undef GLAPI
#endif
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glcorearb.h>
#undef GL_GLEXT_PROTOTYPES

#ifndef GL_CLIP_DISTANCE0
#define GL_CLIP_DISTANCE0 0x3000
#endif

namespace MGITest {
    namespace {

        GLuint CompileShader(GLenum type, const std::string& source, std::string* log) {
            const GLuint shader = glCreateShader(type);
            const char* text = source.c_str();
            glShaderSource(shader, 1, &text, nullptr);
            glCompileShader(shader);
            GLint status = GL_FALSE;
            glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
            if (status == GL_FALSE) {
                GLint length = 0;
                glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
                std::vector<char> buffer(static_cast<std::size_t>(length) + 1, '\0');
                glGetShaderInfoLog(shader, length + 1, nullptr, buffer.data());
                if (log != nullptr) *log = buffer.data();
                glDeleteShader(shader);
                return 0;
            }
            return shader;
        }

        // Links a vertex/fragment pair, optionally declaring transform feedback
        // varyings first (glTransformFeedbackVaryings takes effect at the next link,
        // exactly as the CTS uses it).
        GLuint BuildProgram(const std::string& vertexSource, const std::string& fragmentSource,
                            const std::vector<const char*>& xfbVaryings, GLenum bufferMode, std::string* log) {
            const GLuint vertexShader = CompileShader(GL_VERTEX_SHADER, vertexSource, log);
            if (vertexShader == 0) return 0;
            const GLuint fragmentShader = CompileShader(GL_FRAGMENT_SHADER, fragmentSource, log);
            if (fragmentShader == 0) {
                glDeleteShader(vertexShader);
                return 0;
            }
            const GLuint program = glCreateProgram();
            glAttachShader(program, vertexShader);
            glAttachShader(program, fragmentShader);
            if (!xfbVaryings.empty()) {
                glTransformFeedbackVaryings(program, static_cast<GLsizei>(xfbVaryings.size()), xfbVaryings.data(),
                                            bufferMode);
            }
            glLinkProgram(program);
            glDeleteShader(vertexShader);
            glDeleteShader(fragmentShader);
            GLint status = GL_FALSE;
            glGetProgramiv(program, GL_LINK_STATUS, &status);
            if (status == GL_FALSE) {
                GLint length = 0;
                glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
                std::vector<char> buffer(static_cast<std::size_t>(length) + 1, '\0');
                glGetProgramInfoLog(program, length + 1, nullptr, buffer.data());
                if (log != nullptr) *log = buffer.data();
                glDeleteProgram(program);
                return 0;
            }
            return program;
        }

        // ---------------------------------------------------------------- poison

        // glcClipDistance.cpp FunctionalTest::m_vertex_shader_code with the same
        // three substitutions (redeclaration, clip function, array setter).
        std::string ClipVertexSource(bool redeclaration, unsigned clipCount, unsigned clipFunction,
                                     unsigned vertexCount) {
            const std::string count = std::to_string(clipCount);
            std::string source = "#version 400 core\n\n";
            if (redeclaration) {
                source += "out float gl_ClipDistance[" + count + "];\n";
            }
            source += "\n";
            switch (clipFunction) {
            case 0:
                source += "float f(int i)\n{\n    return 0.0;\n}\n";
                break;
            case 1:
                source += "float f(int i)\n{\n    return 0.25 + 0.75 * (float(i) + 1.0) * (float(gl_VertexID) + 1.0)"
                          " / (float(" + count + ") * float(" + std::to_string(vertexCount) + "));\n}\n";
                break;
            default:
                source += "float f(int i)\n{\n    return - 0.25 - 0.75 * (float(i) + 1.0) * (float(gl_VertexID) + 1.0)"
                          " / (float(" + count + ") * float(" + std::to_string(vertexCount) + "));\n}\n";
                break;
            }
            source += "\nin vec4 position;\n\nvoid main()\n{\n";
            if (redeclaration) {
                // Dynamic array setter.
                source += "    for(int i = 0; i < " + count + "; i++)\n    {\n"
                          "        gl_ClipDistance[i] = f(i);\n    }\n";
            } else {
                // Static array setter, at the highest index this iteration enables.
                const std::string index = std::to_string(clipCount - 1);
                source += "    gl_ClipDistance[" + index + "] = f(" + index + ");\n";
            }
            source += "\n    gl_Position  = position;\n}\n";
            return source;
        }

        const char* kClipFragmentSource = R"(#version 400 core

out vec4 color;

void main()
{
    color = vec4(1.0, 0.0, 0.0, 1.0);
}
)";

        // How far into FunctionalTest's loop nest to get before bailing out the way
        // the CTS does on a failed check: return immediately, skipping the "clip
        // clean" loop that would have disabled GL_CLIP_DISTANCEi again.
        struct ClipStopPoint {
            unsigned primitiveIndex = 0; // 0 = POINTS, 1 = LINES, 2 = TRIANGLES
            unsigned clipFunction = 0;
            bool redeclaration = false;
            unsigned clipCount = 1; // 1..8, the iteration that "fails"
        };

        // Runs FunctionalTest's loop nest up to and including `stop`, then returns
        // leaving exactly the state the CTS leaves behind on a failure.
        void RunClipDistanceWorkload(const ClipStopPoint& stop) {
            static const GLenum kPrimitiveTypes[] = {GL_POINTS, GL_LINES, GL_TRIANGLES};
            static const GLsizei kPrimitiveIndices[] = {1, 2, 3};
            static const float kPositions[3][12] = {
                {0.0f, 0.0f, 0.0f, 1.0f},
                {-1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f},
                {-1.0f, -1.0f, 0.0f, 1.0f, 1.0f, -1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f},
            };

            for (unsigned primitiveIndex = 0; primitiveIndex <= stop.primitiveIndex; ++primitiveIndex) {
                const GLenum primitiveType = kPrimitiveTypes[primitiveIndex];
                const GLsizei vertexCount = kPrimitiveIndices[primitiveIndex];
                const GLsizei framebufferSize = (primitiveType == GL_POINTS) ? 1 : 32;

                GLuint colorBuffer = 0;
                GLuint framebuffer = 0;
                glGenRenderbuffers(1, &colorBuffer);
                glBindRenderbuffer(GL_RENDERBUFFER, colorBuffer);
                glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, framebufferSize, framebufferSize);
                glGenFramebuffers(1, &framebuffer);
                glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
                glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, colorBuffer);
                glViewport(0, 0, framebufferSize, framebufferSize);

                const unsigned lastFunction =
                    (primitiveIndex == stop.primitiveIndex) ? stop.clipFunction : 2u;
                for (unsigned clipFunction = 0; clipFunction <= lastFunction; ++clipFunction) {
                    const bool atStopFunction =
                        primitiveIndex == stop.primitiveIndex && clipFunction == stop.clipFunction;
                    for (unsigned redeclaration = 0; redeclaration < 2; ++redeclaration) {
                        const bool atStopRedeclaration =
                            atStopFunction && (redeclaration != 0) == stop.redeclaration;
                        const unsigned lastCount = atStopRedeclaration ? stop.clipCount : 8u;
                        for (unsigned clipCount = 1; clipCount <= lastCount; ++clipCount) {
                            std::string log;
                            const GLuint program =
                                BuildProgram(ClipVertexSource(redeclaration != 0, clipCount, clipFunction,
                                                              static_cast<unsigned>(vertexCount)),
                                             kClipFragmentSource, {}, GL_INTERLEAVED_ATTRIBS, &log);
                            if (program == 0) continue;
                            glUseProgram(program);

                            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                            glClear(GL_COLOR_BUFFER_BIT);

                            glEnable(GL_CLIP_DISTANCE0 + clipCount - 1);

                            GLuint vao = 0;
                            GLuint vbo = 0;
                            glGenVertexArrays(1, &vao);
                            glBindVertexArray(vao);
                            glGenBuffers(1, &vbo);
                            glBindBuffer(GL_ARRAY_BUFFER, vbo);
                            glBufferData(GL_ARRAY_BUFFER,
                                         static_cast<GLsizeiptr>(sizeof(float) * 4 * vertexCount),
                                         kPositions[primitiveIndex], GL_STATIC_DRAW);
                            const GLint location = glGetAttribLocation(program, "position");
                            if (location >= 0) {
                                glEnableVertexAttribArray(static_cast<GLuint>(location));
                                glVertexAttribPointer(static_cast<GLuint>(location), 4, GL_FLOAT, GL_FALSE, 0,
                                                      nullptr);
                            }

                            glDrawArrays(primitiveType, 0, vertexCount);

                            std::vector<unsigned char> pixels(
                                static_cast<std::size_t>(framebufferSize) * framebufferSize * 4, 0);
                            glReadPixels(0, 0, framebufferSize, framebufferSize, GL_RGBA, GL_UNSIGNED_BYTE,
                                         pixels.data());

                            glBindBuffer(GL_ARRAY_BUFFER, 0);
                            glBindVertexArray(0);
                            glUseProgram(0);
                            // MGL_REPRO_KEEPCLIPOBJ leaks the per-iteration objects so
                            // no GL name and no heap address can be recycled into the
                            // capture phase.
                            // Deleting all three is load-bearing, not tidiness: the defect
                            // this scenario pins needs the VAO's AND its vertex buffer's heap
                            // addresses to be freed here so the capture phase's own objects
                            // can be handed the same ones back.
                            glDeleteBuffers(1, &vbo);
                            glDeleteVertexArrays(1, &vao);
                            glDeleteProgram(program);

                            if (atStopRedeclaration && clipCount == stop.clipCount) {
                                // The CTS's early return: the "clip clean" loop below
                                // never runs, so the enables survive.
                                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                                glDeleteFramebuffers(1, &framebuffer);
                                glDeleteRenderbuffers(1, &colorBuffer);
                                return;
                            }
                        }
                        for (unsigned i = 0; i < 8; ++i) {
                            glDisable(GL_CLIP_DISTANCE0 + i);
                        }
                    }
                }

                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glDeleteFramebuffers(1, &framebuffer);
                glDeleteRenderbuffers(1, &colorBuffer);
            }
        }

        // ---------------------------------------------------------------- victim

        // gl3cTransformFeedback3Tests.cpp TransformFeedbackBaseTestCase::m_shader_vert.
        const char* kXfbVertexSource = R"(#version 400 core
in vec4 vertex;
out vec4 value1;
out vec4 value2;
out vec4 value3;
out vec4 value4;

void main (void)
{
    vec4 temp = vertex;

    gl_Position = temp;

    value1 = abs(temp) * 1.0;
    value2 = abs(temp) * 2.0;
    value3 = abs(temp) * 3.0;
    value4 = abs(temp) * 4.0;
}
)";

        const char* kXfbFragmentSource = R"(#version 400 core
out vec4 color;
void main (void)
{
    color = vec4(0.0, 0.0, 0.0, 1.0);
}
)";

        // The skip_components capture layout, verbatim.
        std::vector<const char*> SkipComponentsVaryings() {
            return {"gl_SkipComponents1", "value1",  "gl_SkipComponents2", "gl_SkipComponents1", "value2",
                    "gl_SkipComponents3", "gl_SkipComponents2", "value3", "gl_SkipComponents4", "value4"};
        }

        constexpr unsigned kSkipComponentCount = 4 * 4 + (1 + 2 + 3 + 4 + 1 + 2); // 16 values + 13 skipped
        constexpr unsigned kSkipVertexCount = 6;

        // HOW THE CAPTURE BUFFER'S STORE IS DECLARED, which is the whole subject of the two
        // orphan cases at the end of this file (P3b/P4b espryt D1 slice 1).
        //
        // glBufferData(size, data) DECLARES the content: the application supplied every byte,
        // and under a transport those bytes reach the server as resource_subdata records behind
        // the respecify, so a gap in the staged coverage is a MISSING RECORD. glBufferData(size,
        // NULL) ORPHANS the store instead: the application has said, in the API, that every byte
        // it does not upload afterwards is UNDEFINED. Both are ordinary - "orphan, upload part of
        // it, capture into it" is the streaming idiom - and the scatter is a read-modify-write, so
        // the two shapes are the difference between "read the old bytes" and "there are no old
        // bytes to read, and the API says nobody may look".
        enum class CaptureStoreShape {
            Declared,        // glBufferData(size, prefill): every byte declared and staged.
            OrphanedPartial, // glBufferData(size, NULL) + glBufferSubData over the FIRST HALF.
            OrphanedWhole,   // glBufferData(size, NULL) and nothing staged at all.
        };

        // The floats the OrphanedPartial shape uploads: the first half of the records, so the
        // buffer has a staged region and an unstaged one and the assertions can tell them apart.
        constexpr unsigned kOrphanStagedFloats = (kSkipVertexCount / 2) * kSkipComponentCount;

        // Is this component's offset WITHIN one record a captured varying rather than one of the
        // gl_SkipComponents holes? The holes keep whatever the destination held, which past the
        // staged region is undefined by the application's own declaration and must not be
        // asserted; the varyings are the capture and must be there whatever the store's shape.
        bool SkipComponentsIndexIsCaptured(unsigned indexInRecord) {
            static const unsigned kValueOffsets[4] = {1, 8, 17, 25};
            for (const unsigned offset : kValueOffsets) {
                if (indexInRecord >= offset && indexInRecord < offset + 4) return true;
            }
            return false;
        }

        // The capture RANGE the writeback has to cross when the point of a case is that range's
        // SIZE rather than its contents (P3b/P4b espryt D1 slice 2). One SEG_EVENT record may be
        // at most half the event ring, which defaults to 256 KiB - so 1 MiB is about eight times
        // what a whole-range post can ever carry, and no reasonable retuning of the ring makes it
        // fit.
        constexpr GLsizeiptr kLargeCaptureRangeBytes = 1024 * 1024;

        // Runs skip_components and reports what came back. `outCaptured` is the raw
        // readback so a failure can say whether anything was written at all.
        //
        // `storeBytes` widens the capture BUFFER without widening the capture: the records are the
        // same six, but glBindBufferBase binds the whole store, so the range the backend mirrors
        // back is `storeBytes` wide. That is the only way to reach a writeback of a given size
        // from a test without drawing a proportional number of primitives.
        void RunSkipComponentsCapture(std::vector<float>& outCaptured, std::string* buildLog,
                                      CaptureStoreShape shape = CaptureStoreShape::Declared,
                                      GLsizeiptr storeBytes = 0) {
            outCaptured.clear();

            const GLuint program = BuildProgram(kXfbVertexSource, kXfbFragmentSource, SkipComponentsVaryings(),
                                                GL_INTERLEAVED_ATTRIBS, buildLog);
            ASSERT_NE(program, 0u) << "skip_components program failed to link: " << (buildLog ? *buildLog : "");
            glUseProgram(program);

            const std::vector<float> vertices = {
                -1.0f, -1.0f, -1.0f, 1.0f, 1.0f,  -1.0f, -2.0f, 1.0f, -1.0f, 1.0f, -3.0f, 1.0f,
                1.0f,  1.0f,  4.0f,  1.0f, -1.0f, 1.0f,  5.0f,  1.0f, 1.0f,  -1.0f, 6.0f, 1.0f,
            };

            GLuint vao = 0;
            GLuint vbo = 0;
            glGenVertexArrays(1, &vao);
            glBindVertexArray(vao);
            glGenBuffers(1, &vbo);
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(sizeof(float) * vertices.size()), vertices.data(),
                         GL_STATIC_DRAW);
            const GLint location = glGetAttribLocation(program, "vertex");
            if (location >= 0) {
                glEnableVertexAttribArray(static_cast<GLuint>(location));
                glVertexAttribPointer(static_cast<GLuint>(location), 4, GL_FLOAT, GL_FALSE, 0, nullptr);
            }

            const unsigned floatCount = kSkipVertexCount * kSkipComponentCount;
            const GLsizeiptr byteSize = static_cast<GLsizeiptr>(sizeof(float) * floatCount);
            // The STORE, which is what glBindBufferBase binds and therefore what the backend
            // mirrors back; the capture itself is `byteSize` of it either way.
            const GLsizeiptr storeSize = storeBytes > byteSize ? storeBytes : byteSize;
            const unsigned storeFloats = static_cast<unsigned>(storeSize / sizeof(float));

            GLuint captureBuffer = 0;
            glGenBuffers(1, &captureBuffer);
            glBindBuffer(GL_ARRAY_BUFFER, captureBuffer);
            glBufferData(GL_ARRAY_BUFFER, storeSize, nullptr, GL_STATIC_READ);
            glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, captureBuffer);
            glBindBuffer(GL_ARRAY_BUFFER, 0);

            // The pre-fill that makes "nothing was captured" recognisable.
            std::vector<float> prefill(storeFloats);
            for (unsigned i = 0; i < storeFloats; ++i) {
                prefill[i] = -1.0f - static_cast<float>(i);
            }
            glBindBuffer(GL_ARRAY_BUFFER, captureBuffer);
            switch (shape) {
            case CaptureStoreShape::Declared:
                glBufferData(GL_ARRAY_BUFFER, storeSize, prefill.data(), GL_STATIC_DRAW);
                break;
            case CaptureStoreShape::OrphanedPartial:
                // The streaming idiom: orphan the store, then upload only the part the
                // application cares about keeping.
                glBufferData(GL_ARRAY_BUFFER, storeSize, nullptr, GL_STATIC_DRAW);
                glBufferSubData(GL_ARRAY_BUFFER, 0,
                                static_cast<GLsizeiptr>(sizeof(float) * kOrphanStagedFloats), prefill.data());
                break;
            case CaptureStoreShape::OrphanedWhole:
                glBufferData(GL_ARRAY_BUFFER, storeSize, nullptr, GL_STATIC_DRAW);
                break;
            }
            glBindBuffer(GL_ARRAY_BUFFER, 0);

            glEnable(GL_RASTERIZER_DISCARD);
            glClearColor(0.1f, 0.0f, 0.5f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, captureBuffer);
            glBeginTransformFeedback(GL_TRIANGLES);
            glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(kSkipVertexCount));
            glEndTransformFeedback();
            glDisable(GL_RASTERIZER_DISCARD);

            outCaptured.resize(floatCount);
            glBindBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER, 0, captureBuffer, 0, byteSize);
            const void* mapped = glMapBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER, 0, byteSize, GL_MAP_READ_BIT);
            if (mapped != nullptr) {
                std::memcpy(outCaptured.data(), mapped, static_cast<std::size_t>(byteSize));
                glUnmapBuffer(GL_TRANSFORM_FEEDBACK_BUFFER);
            }

            glDisableVertexAttribArray(0);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            glDeleteBuffers(1, &vbo);
            glDeleteBuffers(1, &captureBuffer);
            glBindVertexArray(0);
            glDeleteVertexArrays(1, &vao);
            glUseProgram(0);
            glDeleteProgram(program);
            glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0);
        }

        // skip_components' expected buffer: the 13 skipped components keep their
        // pre-fill, the 16 captured ones carry |vertex| * n.
        std::vector<float> SkipComponentsExpected() {
            const std::vector<float> vertices = {
                -1.0f, -1.0f, -1.0f, 1.0f, 1.0f,  -1.0f, -2.0f, 1.0f, -1.0f, 1.0f, -3.0f, 1.0f,
                1.0f,  1.0f,  4.0f,  1.0f, -1.0f, 1.0f,  5.0f,  1.0f, 1.0f,  -1.0f, 6.0f, 1.0f,
            };
            const unsigned floatCount = kSkipVertexCount * kSkipComponentCount;
            std::vector<float> expected(floatCount);
            for (unsigned i = 0; i < floatCount; ++i) {
                expected[i] = -1.0f - static_cast<float>(i);
            }
            // Record layout, in floats:
            //   [0]      skip1
            //   [1..4]   value1
            //   [5..7]   skip2 + skip1
            //   [8..11]  value2
            //   [12..16] skip3 + skip2
            //   [17..20] value3
            //   [21..24] skip4
            //   [25..28] value4
            static const unsigned kValueOffsets[4] = {1, 8, 17, 25};
            for (unsigned v = 0; v < kSkipVertexCount; ++v) {
                const unsigned base = v * kSkipComponentCount;
                for (unsigned value = 0; value < 4; ++value) {
                    for (unsigned component = 0; component < 4; ++component) {
                        const float source = vertices[v * 4 + component];
                        expected[base + kValueOffsets[value] + component] =
                            std::fabs(source) * static_cast<float>(value + 1);
                    }
                }
            }
            return expected;
        }

        // Reports the first mismatch, and whether the readback is byte-for-byte the
        // pre-fill (i.e. the capture never happened).
        ::testing::AssertionResult CheckSkipComponents(const std::vector<float>& captured) {
            const std::vector<float> expected = SkipComponentsExpected();
            if (captured.size() != expected.size()) {
                return ::testing::AssertionFailure()
                       << "readback size " << captured.size() << " != " << expected.size();
            }
            bool anyWritten = false;
            for (std::size_t i = 0; i < captured.size(); ++i) {
                if (captured[i] != -1.0f - static_cast<float>(i)) {
                    anyWritten = true;
                    break;
                }
            }
            for (std::size_t i = 0; i < expected.size(); ++i) {
                if (std::fabs(captured[i] - expected[i]) > 0.0125f) {
                    return ::testing::AssertionFailure()
                           << "capture mismatch at index " << i << ": got " << captured[i] << ", expected "
                           << expected[i] << (anyWritten ? "" : " (the whole buffer is still the pre-fill: "
                                                               "NOTHING was captured)");
                }
            }
            return ::testing::AssertionSuccess();
        }

        // The orphaned store's weaker contract, checked exactly as far as the API allows.
        //
        // Below `stagedFloats` the destination's bytes are the application's - it uploaded them -
        // so the FULL assertion holds: the varyings are the capture and the gl_SkipComponents
        // holes still carry the pre-fill, which is the read-modify-write actually having read
        // something. Above it the application declared the bytes undefined, so only the captured
        // varyings are asserted; the holes are whatever the server had and are nobody's to check.
        //
        // WHAT THIS CATCHES, and it is the whole point of the two cases: the failure mode is not a
        // wrong number, it is the pre-fill surviving where a varying belongs - the capture target
        // silently dropped and the buffer left holding its pre-draw bytes under GL_NO_ERROR.
        ::testing::AssertionResult CheckSkipComponentsOrphaned(const std::vector<float>& captured,
                                                              unsigned stagedFloats) {
            const std::vector<float> expected = SkipComponentsExpected();
            if (captured.size() != expected.size()) {
                return ::testing::AssertionFailure()
                       << "readback size " << captured.size() << " != " << expected.size();
            }
            for (std::size_t i = 0; i < expected.size(); ++i) {
                const bool isCaptured =
                    SkipComponentsIndexIsCaptured(static_cast<unsigned>(i) % kSkipComponentCount);
                if (i >= stagedFloats && !isCaptured) continue;
                if (std::fabs(captured[i] - expected[i]) > 0.0125f) {
                    ::testing::AssertionResult failure = ::testing::AssertionFailure();
                    failure << (isCaptured ? "captured varying" : "preserved hole") << " at index " << i
                            << (i < stagedFloats ? " (inside the uploaded region)" : " (past the uploaded region)")
                            << ": got " << captured[i] << ", expected " << expected[i];
                    if (isCaptured && std::fabs(captured[i] - (-1.0f - static_cast<float>(i))) <= 0.0125f) {
                        failure << " - this component is still the PRE-FILL, so the capture never reached this "
                                   "target at all";
                    }
                    return failure;
                }
            }
            return ::testing::AssertionSuccess();
        }

        // THE OTHER WRITEBACK PRODUCER (P3b/P4b espryt D1 slice 2).
        //
        // A capture list with no gl_SkipComponents and one buffer is expressible on ES directly,
        // so the driver writes the application's buffer itself and the mirror-back is
        // ReadbackCapturedRanges' rather than ScatterCapturedRecords'. That is a DIFFERENT
        // OnBufferWriteback call site - it posts from a live glMapBufferRange mapping instead of a
        // staged vector - with the same unbounded record behind it, so it needs its own case or
        // half the fix is untested. `value1` alone is a 16-byte record; the bound range is the
        // whole store, which is what the writeback crosses.
        void RunPlainCaptureIntoLargeRange(std::vector<float>& outCaptured, std::string* buildLog) {
            outCaptured.clear();
            const GLuint program = BuildProgram(kXfbVertexSource, kXfbFragmentSource, {"value1"},
                                                GL_INTERLEAVED_ATTRIBS, buildLog);
            ASSERT_NE(program, 0u) << "plain capture program failed to link: " << (buildLog ? *buildLog : "");
            glUseProgram(program);

            const std::vector<float> vertices = {
                -1.0f, -1.0f, -1.0f, 1.0f, 1.0f,  -1.0f, -2.0f, 1.0f, -1.0f, 1.0f, -3.0f, 1.0f,
                1.0f,  1.0f,  4.0f,  1.0f, -1.0f, 1.0f,  5.0f,  1.0f, 1.0f,  -1.0f, 6.0f, 1.0f,
            };

            GLuint vao = 0;
            GLuint vbo = 0;
            glGenVertexArrays(1, &vao);
            glBindVertexArray(vao);
            glGenBuffers(1, &vbo);
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(sizeof(float) * vertices.size()), vertices.data(),
                         GL_STATIC_DRAW);
            const GLint location = glGetAttribLocation(program, "vertex");
            if (location >= 0) {
                glEnableVertexAttribArray(static_cast<GLuint>(location));
                glVertexAttribPointer(static_cast<GLuint>(location), 4, GL_FLOAT, GL_FALSE, 0, nullptr);
            }

            const unsigned capturedFloats = kSkipVertexCount * 4;
            const unsigned storeFloats = static_cast<unsigned>(kLargeCaptureRangeBytes / sizeof(float));
            std::vector<float> prefill(storeFloats);
            for (unsigned i = 0; i < storeFloats; ++i) prefill[i] = -1.0f - static_cast<float>(i);

            GLuint captureBuffer = 0;
            glGenBuffers(1, &captureBuffer);
            glBindBuffer(GL_ARRAY_BUFFER, captureBuffer);
            glBufferData(GL_ARRAY_BUFFER, kLargeCaptureRangeBytes, prefill.data(), GL_STATIC_READ);
            glBindBuffer(GL_ARRAY_BUFFER, 0);

            glEnable(GL_RASTERIZER_DISCARD);
            glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, captureBuffer);
            glBeginTransformFeedback(GL_TRIANGLES);
            glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(kSkipVertexCount));
            glEndTransformFeedback();
            glDisable(GL_RASTERIZER_DISCARD);

            outCaptured.resize(capturedFloats);
            const GLsizeiptr capturedBytes = static_cast<GLsizeiptr>(sizeof(float) * capturedFloats);
            glBindBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER, 0, captureBuffer, 0, capturedBytes);
            const void* mapped = glMapBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER, 0, capturedBytes, GL_MAP_READ_BIT);
            if (mapped != nullptr) {
                std::memcpy(outCaptured.data(), mapped, static_cast<std::size_t>(capturedBytes));
                glUnmapBuffer(GL_TRANSFORM_FEEDBACK_BUFFER);
            }

            glDisableVertexAttribArray(0);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            glDeleteBuffers(1, &vbo);
            glDeleteBuffers(1, &captureBuffer);
            glBindVertexArray(0);
            glDeleteVertexArrays(1, &vao);
            glUseProgram(0);
            glDeleteProgram(program);
            glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0);
        }

        // value1 == |vertex| * 1, one tight vec4 record per vertex.
        ::testing::AssertionResult CheckPlainCapture(const std::vector<float>& captured) {
            const std::vector<float> vertices = {
                -1.0f, -1.0f, -1.0f, 1.0f, 1.0f,  -1.0f, -2.0f, 1.0f, -1.0f, 1.0f, -3.0f, 1.0f,
                1.0f,  1.0f,  4.0f,  1.0f, -1.0f, 1.0f,  5.0f,  1.0f, 1.0f,  -1.0f, 6.0f, 1.0f,
            };
            if (captured.size() != vertices.size()) {
                return ::testing::AssertionFailure()
                       << "readback size " << captured.size() << " != " << vertices.size();
            }
            for (std::size_t i = 0; i < vertices.size(); ++i) {
                const float expected = std::fabs(vertices[i]);
                if (std::fabs(captured[i] - expected) > 0.0125f) {
                    ::testing::AssertionResult failure = ::testing::AssertionFailure();
                    failure << "capture mismatch at index " << i << ": got " << captured[i] << ", expected "
                            << expected;
                    if (std::fabs(captured[i] - (-1.0f - static_cast<float>(i))) <= 0.0125f) {
                        failure << " - still the pre-fill, so nothing was mirrored back";
                    }
                    return failure;
                }
            }
            return ::testing::AssertionSuccess();
        }

        // The harness turns "no context came up" into a clean skip, and a skip is
        // indistinguishable from a pass in a ctest summary. For this scenario that
        // is a hole rather than a courtesy: the defect it pins is DirectVulkan's
        // alone, and DirectVulkan now comes up headless on any machine at all - a
        // surfaceless EGL platform over a software ICD (lavapipe) is enough. So
        // "DirectVulkan did not initialise" here means the run is MISCONFIGURED,
        // not that the machine has no GPU, and it must not report green.
        //
        // Local on purpose: the harness-wide skip semantics are deliberate
        // (ScenarioFixture.h states the reasoning), and MOBILEGL_ITEST_REQUIRE_GPU
        // is the harness-wide lever for the same intent - but that lever also
        // demands a HARDWARE renderer, which is exactly what a lavapipe-only box
        // cannot offer. This overrides nothing else: only this scenario, only for
        // the backend that can regress, and only for the unusable-harness case.
        class XfbAfterClipDistanceScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                // Ready() is false on the base's skip path AND on its REQUIRE_GPU
                // failure path; the second one has already failed, so leave it alone
                // rather than burying its reason under a second message.
                if (Ready() || HasFatalFailure()) return;
                if (Gl().BackendName() == "DirectVulkan") {
                    FAIL() << "DirectVulkan could not be brought up, so the regression this scenario guards - a "
                              "draw served a destroyed VAO's memoised vertex bindings - was never exercised, and "
                              "that must be a failure rather than a silent skip. Headless bring-up needs only a "
                              "Vulkan ICD and a surfaceless EGL platform (a software ICD such as lavapipe "
                              "qualifies: VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json with "
                              "EGL_PLATFORM=surfaceless). Harness reason: "
                           << Gl().SkipReason();
                }
            }
        };

        // ------------------------------------------- P3b/P4b espryt D1 slice 1: orphaned targets
        //
        // AN ORPHANED CAPTURE TARGET IS A LEGAL TARGET, and under a transport it was being thrown
        // away. The scattered capture is a read-modify-write of the destination's PRE-CAPTURE
        // bytes (the gl_SkipComponents holes have to keep them), and under split those bytes are
        // the SERVER's staged shadow rather than a frontend MappedData(). A buffer the
        // application orphaned and then only partly uploaded has no staged bytes for the rest -
        // legitimately, because it declared them undefined - and the scatter read that as "this
        // target has no pre-capture bytes, discard it", which is a total loss of the varyings
        // under GL_NO_ERROR. The whole-store case (no upload at all) discarded silently; the
        // partial one asked the staged-coverage assertion for a range wider than anything staged
        // and died on Fatal{StageSnapshotTooNarrow, "xfb_scatter_pre_capture"}.
        //
        // The monolith arm has always been right here: `staged` is a value-initialised
        // Vector<Uint8>, so an unreadable source means the scatter runs over zeroes and uploads
        // them - which is byte for byte what it gets from a fresh MappedData(). Skipping the COPY
        // is the whole of the difference between the two arms; skipping the TARGET was the bug.
        //
        // Both cases run on the split arms, where they are the red-once, and on the ambient
        // monolith arms, where they are the control that says the expectation itself is right.
        TEST_F(XfbAfterClipDistanceScenario, OrphanedCaptureTargetWithAPartialUploadKeepsItsVaryings) {
            if (!Ready()) return;
            std::vector<float> captured;
            std::string log;
            RunSkipComponentsCapture(captured, &log, CaptureStoreShape::OrphanedPartial);
            EXPECT_TRUE(CheckSkipComponentsOrphaned(captured, kOrphanStagedFloats));
            EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
        }

        TEST_F(XfbAfterClipDistanceScenario, OrphanedCaptureTargetWithNoUploadAtAllKeepsItsVaryings) {
            if (!Ready()) return;
            std::vector<float> captured;
            std::string log;
            RunSkipComponentsCapture(captured, &log, CaptureStoreShape::OrphanedWhole);
            // Nothing was ever staged, so every hole is undefined and only the varyings are
            // checked - which is still the entire capture.
            EXPECT_TRUE(CheckSkipComponentsOrphaned(captured, 0));
            EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
        }

        // ------------------------------------- P3b/P4b espryt D1 slice 2: a capture range that
        //                                       does not fit one reverse-channel record
        //
        // Under a transport the captured bytes go back to the client INLINE in a SEG_EVENT
        // record, and a ring producer refuses any record above half its capacity OUTRIGHT -
        // always, not "when full", because above half a capacity a record is placeable at some
        // head offsets and not at others and waiting for room would be a hang. SEG_EVENT defaults
        // to 256 KiB. Both XFB writeback producers posted the WHOLE bound range as one record, so
        // a capture target wider than ~128 KiB was Fatal{EventRingOverflow} and the server
        // process died - on a buffer size, with nothing else wrong.
        //
        // A megabyte is about eight times what one record can ever be. The capture is still six
        // records; it is glBindBufferBase over the whole store that makes the MIRRORED RANGE a
        // megabyte, which is what a real capture buffer looks like and what the producer has to
        // survive.
        //
        // TWO CASES BECAUSE THERE ARE TWO PRODUCERS. The skip_components layout is not
        // expressible on ES, so it goes through ScatterCapturedRecords and posts from a staged
        // vector; the plain single-varying layout is, so the driver writes the application's
        // buffer and ReadbackCapturedRanges posts from a live glMapBufferRange mapping. They are
        // different call sites with the same defect behind them.
        TEST_F(XfbAfterClipDistanceScenario, AScatteredCaptureIntoAMegabyteRangeReachesTheApplication) {
            if (!Ready()) return;
            std::vector<float> captured;
            std::string log;
            RunSkipComponentsCapture(captured, &log, CaptureStoreShape::Declared, kLargeCaptureRangeBytes);
            EXPECT_TRUE(CheckSkipComponents(captured));
            EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
        }

        TEST_F(XfbAfterClipDistanceScenario, APlainCaptureIntoAMegabyteRangeReachesTheApplication) {
            if (!Ready()) return;
            std::vector<float> captured;
            std::string log;
            RunPlainCaptureIntoLargeRange(captured, &log);
            EXPECT_TRUE(CheckPlainCapture(captured));
            EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
        }

        // Control: the capture on its own must work.
        TEST_F(XfbAfterClipDistanceScenario, SkipComponentsCaptureAlone) {
            if (!Ready()) return;
            std::vector<float> captured;
            std::string log;
            RunSkipComponentsCapture(captured, &log);
            EXPECT_TRUE(CheckSkipComponents(captured));
        }

        // Bisection step 1: only the leftover GL_CLIP_DISTANCEi enables.
        TEST_F(XfbAfterClipDistanceScenario, SkipComponentsCaptureAfterClipDistanceEnables) {
            if (!Ready()) return;
            for (unsigned i = 0; i < 8; ++i) {
                glEnable(GL_CLIP_DISTANCE0 + i);
            }
            std::vector<float> captured;
            std::string log;
            RunSkipComponentsCapture(captured, &log);
            for (unsigned i = 0; i < 8; ++i) {
                glDisable(GL_CLIP_DISTANCE0 + i);
            }
            EXPECT_TRUE(CheckSkipComponents(captured));
        }

        // Bisection step 2: the whole clip_distance.functional workload, stopped
        // where the CTS stopped in the runs that went on to break the capture.
        TEST_F(XfbAfterClipDistanceScenario, SkipComponentsCaptureAfterClipDistanceWorkloadLines8) {
            if (!Ready()) return;
            RunClipDistanceWorkload({.primitiveIndex = 1, .clipFunction = 0, .redeclaration = false, .clipCount = 8});
            std::vector<float> captured;
            std::string log;
            RunSkipComponentsCapture(captured, &log);
            for (unsigned i = 0; i < 8; ++i) {
                glDisable(GL_CLIP_DISTANCE0 + i);
            }
            EXPECT_TRUE(CheckSkipComponents(captured));
        }

        TEST_F(XfbAfterClipDistanceScenario, SkipComponentsCaptureAfterClipDistanceWorkloadPoints1) {
            if (!Ready()) return;
            RunClipDistanceWorkload({.primitiveIndex = 0, .clipFunction = 0, .redeclaration = true, .clipCount = 1});
            std::vector<float> captured;
            std::string log;
            RunSkipComponentsCapture(captured, &log);
            for (unsigned i = 0; i < 8; ++i) {
                glDisable(GL_CLIP_DISTANCE0 + i);
            }
            EXPECT_TRUE(CheckSkipComponents(captured));
        }

        // A single stop point is not a regression test for this defect: whether the
        // capture phase's VAO and vertex buffer land on the addresses the workload just
        // freed is a function of how much the workload allocated, so the two cases above
        // pin two draws of a lottery. Sweep the grid instead - before the fix, roughly a
        // third of these stop points came back holding the workload's vertex data.
        TEST_F(XfbAfterClipDistanceScenario, SkipComponentsCaptureSurvivesEveryClipWorkloadStopPoint) {
            if (!Ready()) return;
            for (unsigned primitiveIndex = 0; primitiveIndex < 3; ++primitiveIndex) {
                for (unsigned redeclaration = 0; redeclaration < 2; ++redeclaration) {
                    for (const unsigned clipCount : {1u, 4u, 8u}) {
                        RunClipDistanceWorkload({.primitiveIndex = primitiveIndex,
                                                 .clipFunction = 0,
                                                 .redeclaration = redeclaration != 0,
                                                 .clipCount = clipCount});
                        std::vector<float> captured;
                        std::string log;
                        RunSkipComponentsCapture(captured, &log);
                        for (unsigned i = 0; i < 8; ++i) {
                            glDisable(GL_CLIP_DISTANCE0 + i);
                        }
                        EXPECT_TRUE(CheckSkipComponents(captured))
                            << " (stop point: primitive " << primitiveIndex << ", redeclaration " << redeclaration
                            << ", clip count " << clipCount << ")";
                    }
                }
            }
        }

    } // namespace
} // namespace MGITest
