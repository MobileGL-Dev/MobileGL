// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/LargeArenaAdoptionScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - MESH-ARENA-SIZED BUFFERS, END TO END.
//
// A buffer store of at least 16MiB is adopted into the backend's persistently and
// coherently mapped GPU storage the moment it is defined (BufferObject::
// TryAdoptLargeStorage): the CPU shadow is dropped and every later write lands
// directly in GPU-visible memory with no per-write driver call. Minecraft 26.3
// streams chunk meshes into 128MB vertex arenas with plain glNamedBufferSubData -
// on Mali, every driver-mediated route for that write into a busy mutable store
// either parks the calling thread or ghost-copies the whole arena on a driver
// worker (~167ms per touched arena: the recurring in-world hiccup this adoption
// removed). Every existing buffer scenario uses stores far below the threshold,
// so without this file the adopted path would have zero coverage.
//
// What is pinned, deliberately through the same API mix Minecraft uses:
//   * a glBufferSubData written AFTER the arena was drawn (in flight) reaches the
//     next draw - the write-visibility contract adoption must not weaken;
//   * GetBufferSubData reads back the latest CPU write - the shadow IS the map;
//   * a compute-shader write through an SSBO binding of the same arena is read
//     back - the GPU-written path for adopted stores (glFinish + direct read).
//
// P3a (gate G10, G12) adds a fourth case and two more lanes, and neither of them
// changes what the three above assert:
//
//   * AnAdoptionCostsExactlyOneMapPersistentRoundtrip counts the acquisition.
//     ARCHITECTURE.md:474 prices the adopted store at one round trip per STORAGE
//     DEFINITION; `map-persistent-roundtrips` counts every map_persistent
//     emission, mint or decline (D-B2), so one definition plus a frame of draws
//     must publish exactly one. It reads the library's summary line, so it needs
//     a lane with the stats channel and a private log path, and it SKIPS - with
//     the reason - anywhere else and on any tree that does not emit the counter.
//   * the three original cases are registered TWICE MORE, with P3a's resource and
//     vertex-input subsystem bits set and cleared, because this file is where an
//     adopted store's whole life is exercised: definition, in-flight SubData,
//     readback and a GPU write. If the handle path and the legacy BufferBackendOps
//     path disagree about any of it, one of the two arms goes red here.

#include <array>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../Harness/HeadlessGL.h"
#include "../Harness/PersistentMapPeek.h"
#include "../Harness/PipeStatsWindow.h"
#include "../Harness/ScenarioFixture.h"
#include "../Harness/SplitLane.h"

#ifdef GLAPI
#undef GLAPI
#endif
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glcorearb.h>
#undef GL_GLEXT_PROTOTYPES

namespace MGITest {
    namespace {

        // Comfortably past the 16MiB adoption threshold, and the vertex payload sits
        // deep inside the store so an implementation that quietly clamped or aliased
        // the adopted range would miss it.
        constexpr GLsizeiptr kArenaBytes = GLsizeiptr(24) * 1024 * 1024;
        constexpr GLintptr kVertexOffset = GLintptr(20) * 1024 * 1024;

        constexpr const char* kVertexSource = R"(#version 430 core
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec3 a_color;
out vec3 v_color;
void main() {
    v_color = a_color;
    gl_Position = vec4(a_pos, 0.0, 1.0);
}
)";

        constexpr const char* kFragmentSource = R"(#version 430 core
in vec3 v_color;
out vec4 o_color;
void main() { o_color = vec4(v_color, 1.0); }
)";

        constexpr const char* kMarkerComputeSource = R"(#version 430 core
layout(local_size_x = 1) in;
layout(std430, binding = 0) buffer Arena { uint word; };
void main() { word = 0xC0FFEEu; }
)";

        // Set by the MapPersistentRoundtrips. ctest entry and by nothing else; a harness marker,
        // never read by the library.
        constexpr const char* kLaneMarker = "MGITEST_MPR_LANE";
        // P11 A1: set (to the tier the lane names, "T0") only by the `.AdoptTier0.` tcp entries,
        // which also set MOBILEGL_IPC_ADOPT_TIER=0; a harness marker, never read by the library.
        constexpr const char* kAdoptTierRefusalMarker = "MGITEST_ADOPT_TIER_REFUSAL";
        // P11 B2: set by the shared-segment `.AdoptTier0.` / `.AdoptT0Disallowed.` entries to the reason word the
        // server gives for not granting T0 ("no AHardwareBuffer" on a host, "in process" where the backend cannot hand an
        // in-process client its stores, "disallowed" with its allow switch off).
        constexpr const char* kAdoptT0FallbackMarker = "MGITEST_ADOPT_T0_FALLBACK";
        // Draws issued against the arena inside the counted window. One definition, many draws:
        // "one per definition" (1) and "one per draw" (kDrawsInTheWindow) have to be different
        // numbers or the assertion cannot tell them apart.
        constexpr int kDrawsInTheWindow = 5;

        bool BuildMarkerIsSet(const char* name) {
            const char* value = std::getenv(name);
            return value != nullptr && value[0] == '1' && value[1] == '\0';
        }

        struct Vertex {
            float x, y;
            float r, g, b;
        };

        // A full-viewport quad, colored uniformly so one center readback speaks for
        // the whole draw.
        std::vector<Vertex> QuadVertices(float r, float g, float b) {
            return {
                {-1.f, -1.f, r, g, b}, {1.f, -1.f, r, g, b}, {1.f, 1.f, r, g, b},
                {-1.f, -1.f, r, g, b}, {1.f, 1.f, r, g, b},  {-1.f, 1.f, r, g, b},
            };
        }

        class LargeArenaAdoptionScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                m_program = LinkProgram(kVertexSource, kFragmentSource);
                ASSERT_NE(m_program, 0u) << m_buildLog;

                glGenVertexArrays(1, &m_vao);
                glBindVertexArray(m_vao);
                glGenBuffers(1, &m_arena);
                glBindBuffer(GL_ARRAY_BUFFER, m_arena);
                // The NULL-data definition is the adoption point (and Minecraft's
                // arena-creation idiom).
                glBufferData(GL_ARRAY_BUFFER, kArenaBytes, nullptr, GL_DYNAMIC_DRAW);
                ConfigureVertexArray(m_vao);
            }

            void ConfigureVertexArray(GLuint vao) {
                glBindVertexArray(vao);
                glBindBuffer(GL_ARRAY_BUFFER, m_arena);
                glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                                      reinterpret_cast<void*>(kVertexOffset));
                glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                                      reinterpret_cast<void*>(kVertexOffset + 2 * sizeof(float)));
                glEnableVertexAttribArray(0);
                glEnableVertexAttribArray(1);
            }

            void TearDown() override {
                if (!Ready()) return;
                glUseProgram(0);
                glBindVertexArray(0);
                if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
                if (m_arena != 0) glDeleteBuffers(1, &m_arena);
                if (m_program != 0) glDeleteProgram(m_program);
                if (m_compute != 0) glDeleteProgram(m_compute);
                m_vao = 0;
                m_arena = 0;
                m_program = 0;
                m_compute = 0;
            }

            unsigned int CompileStage(GLenum stage, const char* source) {
                const GLuint shader = glCreateShader(stage);
                glShaderSource(shader, 1, &source, nullptr);
                glCompileShader(shader);
                GLint compiled = 0;
                glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
                if (compiled == GL_FALSE) {
                    char log[2048] = {};
                    glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
                    m_buildLog = std::string("shader did not compile: ") + log;
                    glDeleteShader(shader);
                    return 0;
                }
                return shader;
            }

            unsigned int LinkProgram(const char* vs, const char* fs) {
                const GLuint v = CompileStage(GL_VERTEX_SHADER, vs);
                if (v == 0) return 0;
                const GLuint f = CompileStage(GL_FRAGMENT_SHADER, fs);
                if (f == 0) {
                    glDeleteShader(v);
                    return 0;
                }
                const GLuint program = glCreateProgram();
                glAttachShader(program, v);
                glAttachShader(program, f);
                glLinkProgram(program);
                glDeleteShader(v);
                glDeleteShader(f);
                GLint linked = 0;
                glGetProgramiv(program, GL_LINK_STATUS, &linked);
                if (linked == GL_FALSE) {
                    char log[2048] = {};
                    glGetProgramInfoLog(program, sizeof(log) - 1, nullptr, log);
                    m_buildLog = std::string("program did not link: ") + log;
                    glDeleteProgram(program);
                    return 0;
                }
                return program;
            }

            void UploadQuad(float r, float g, float b) {
                const auto vertices = QuadVertices(r, g, b);
                glBindBuffer(GL_ARRAY_BUFFER, m_arena);
                glBufferSubData(GL_ARRAY_BUFFER, kVertexOffset,
                                GLsizeiptr(vertices.size() * sizeof(Vertex)), vertices.data());
            }

            void DrawQuad(GLuint vao = 0) {
                glViewport(0, 0, Gl().Width(), Gl().Height());
                glClearColor(0.f, 0.f, 0.f, 1.f);
                glClear(GL_COLOR_BUFFER_BIT);
                glUseProgram(m_program);
                glBindVertexArray(vao != 0 ? vao : m_vao);
                glDrawArrays(GL_TRIANGLES, 0, 6);
            }

            // GTEST_SKIP() returns from the function it is written in, so this cannot report
            // through a return value; the caller pairs it with `if (IsSkipped()) return;`.
            void SkipUnlessTheRoundtripCounterIsReadableHere() {
                if (std::getenv(kLaneMarker) == nullptr) {
                    GTEST_SKIP() << "runs only in its own lane: the MapPersistentRoundtrips. ctest entry "
                                    "sets MGITEST_MPR_LANE together with MOBILEGL_PIPE_PUSH's P3a mask, "
                                    "MOBILEGL_PIPE_STATS=1, MOBILEGL_PIPE_STATS_PERIOD=1 and a private "
                                    "MOBILEGL_LOG_FILE_PATH. The ambient entries and the two subsystem "
                                    "arms configure none of that, and their log is shared - a read there "
                                    "would race a neighbour's bring-up.";
                    return;
                }
                if (!BuildMarkerIsSet("MGITEST_PIPE_PUSH_BUILD")) {
                    GTEST_SKIP() << "this library was built without MOBILEGL_PIPE_PUSH, so "
                                    "CallClass::MapPersistentRoundtrips does not exist and the summary "
                                    "line carries no mpr=. The entry stays registered so that "
                                    "`ctest -L integration-gpu` names the same tests in both builds (G2).";
                    return;
                }
                if (!BuildMarkerIsSet("MGITEST_PIPE_RESOURCE_EMITTER_PRESENT")) {
                    GTEST_SKIP() << "subsystem not implemented on this tree: no source under "
                                    "MobileGL/MG_Impl/Pipe/ names MapPersistentRoundtrips, so nothing "
                                    "emits map_persistent and mpr= is structurally zero. P3a package B "
                                    "owns that emitter; this entry arms itself when it lands.";
                    return;
                }
                if (PipeStatsWindow::LibraryLogPath().empty()) {
                    GTEST_SKIP() << "the lane configured no MOBILEGL_LOG_FILE_PATH, and the library's "
                                    "summary line is the only channel this module has for reading "
                                    "PipeStats";
                    return;
                }
            }

            std::array<unsigned char, 4> CenterPixel() {
                std::array<unsigned char, 4> px = {0, 0, 0, 0};
                glReadPixels(Gl().Width() / 2, Gl().Height() / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                             px.data());
                return px;
            }

            // P11 A2: the arena's tier as the frontend object records it, against the lane's
            // declaration. Adopted = the resource owner minted a persistent coherent mapping and
            // the CPU shadow was released; emulated = the owner declined (T2) and the shadow is
            // the truth. Pixels cannot tell them apart - every case above is green on both.
            void ExpectDeclaredArm(const std::string& declared, const char* when) {
                bool adopted = false;
                ASSERT_TRUE(PeekBufferIsAdoptedPersistentMap(m_arena, &adopted))
                    << "no frontend BufferObject behind the arena " << when;
                const std::string landed = adopted ? "adopted" : "emulated";
                RecordProperty(std::string("arena_arm_") + (m_armPeeks++ == 0 ? "defined" : "used"),
                               landed.c_str());
                EXPECT_EQ(declared, landed)
                    << "the lane declared the " << declared << " arm and the " << (kArenaBytes >> 20)
                    << " MiB arena is " << landed << " " << when
                    << ". Monolith lanes adopt on both backends (Espryt's handle arm mints through "
                       "Ops_H_MapPersistentTracked, Magma's legacy AcquirePersistentMap); split lanes "
                       "run T2, where every acquisition declines.";
            }

            int m_armPeeks = 0;

            unsigned int m_program = 0;
            unsigned int m_compute = 0;
            unsigned int m_vao = 0;
            unsigned int m_arena = 0;
            std::string m_buildLog;
        };

    } // namespace

    // The Minecraft shape: the arena is drawn, the frame retires, and a
    // glBufferSubData rewrites the SAME vertex bytes while the previous frame's
    // draw may still be in flight. The next draw must show the NEW bytes.
    TEST_F(LargeArenaAdoptionScenario, SubDataAfterAnInFlightDrawReachesTheNextDraw) {
        if (!Ready() || IsSkipped()) return;

        UploadQuad(1.f, 0.f, 0.f);
        DrawQuad();
        auto px = CenterPixel();
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_GT(px[0], 200) << "the first draw from the adopted arena never landed";
        EXPECT_LT(px[1], 50);

        Gl().EndFrame();

        UploadQuad(0.f, 1.f, 0.f);
        DrawQuad();
        px = CenterPixel();
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_GT(px[1], 200) << "the cross-frame rewrite of the adopted arena did not reach the draw; "
                                 "the old color means the write went to bytes the draw no longer reads";
        EXPECT_LT(px[0], 50) << "the draw still shows the previous frame's bytes";
    }

    // P11 B2: THE SAME REWRITE WITH NO READBACK BETWEEN THE TWO DRAWS. The case above reads
    // pixels after the first draw, which retires it before the SubData, so a store that took the
    // write IN PLACE (straight into pages the GPU reads - a T0 import is the client's
    // AHardwareBuffer, an adopted store is a coherent map) would still pass it. Here the first
    // draw is only queued when the arena is rewritten: it must still fetch the bytes it was
    // issued against (red, left half) and the second draw the new ones (green, right half).
    TEST_F(LargeArenaAdoptionScenario, ADrawQueuedBeforeASubDataKeepsItsOwnBytes) {
        if (!Ready() || IsSkipped()) return;

        const GLint w = Gl().Width();
        const GLint h = Gl().Height();
        UploadQuad(1.f, 0.f, 0.f);
        glViewport(0, 0, w, h);
        glClearColor(0.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(m_program);
        glBindVertexArray(m_vao);
        glEnable(GL_SCISSOR_TEST);
        glScissor(0, 0, w / 2, h);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        UploadQuad(0.f, 1.f, 0.f);
        glScissor(w / 2, 0, w - w / 2, h);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glDisable(GL_SCISSOR_TEST);

        std::array<unsigned char, 4> left = {0, 0, 0, 0};
        std::array<unsigned char, 4> right = {0, 0, 0, 0};
        glReadPixels(w / 4, h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, left.data());
        glReadPixels(w / 2 + w / 4, h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, right.data());
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_GT(left[0], 200) << "the draw queued BEFORE the rewrite lost its bytes: the SubData reached "
                                   "pages that draw had not read yet (a write that landed in place instead "
                                   "of GPU-ordered)";
        EXPECT_LT(left[1], 50) << "the draw queued before the rewrite fetched the rewritten bytes";
        EXPECT_GT(right[1], 200) << "the draw after the rewrite did not see the new bytes";
        EXPECT_LT(right[0], 50) << "the draw after the rewrite still fetched the old bytes";
    }

    // Respecifying a frontend buffer preserves its VAO attachments even when the
    // backend replaces the adopted store's GL name. Keep every attribute binding
    // unchanged so a stale backend VAO cannot be repaired by a frontend rebind.
    TEST_F(LargeArenaAdoptionScenario, RespecifiedVertexArenaKeepsVaoBindings) {
        if (!Ready() || IsSkipped()) return;

        UploadQuad(1.f, 0.f, 0.f);
        DrawQuad();
        ASSERT_GT(CenterPixel()[0], 200);
        ASSERT_EQ(FirstGLError(), 0u);

        GLuint otherVao = 0;
        glGenVertexArrays(1, &otherVao);
        ConfigureVertexArray(otherVao);
        DrawQuad(otherVao);
        EXPECT_GT(CenterPixel()[0], 200);
        EXPECT_EQ(FirstGLError(), 0u);

        constexpr std::array<GLsizeiptr, 3> sizes = {
            kArenaBytes, kArenaBytes + 4096, kArenaBytes - 4096,
        };
        constexpr std::array<std::array<float, 3>, 3> colors = {{
            {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}, {1.f, 0.f, 0.f},
        }};
        for (std::size_t i = 0; i < sizes.size(); ++i) {
            SCOPED_TRACE(sizes[i]);
            glBindBuffer(GL_ARRAY_BUFFER, m_arena);
            glBufferData(GL_ARRAY_BUFFER, sizes[i], nullptr, GL_DYNAMIC_DRAW);
            UploadQuad(colors[i][0], colors[i][1], colors[i][2]);
            // The unbound VAO can retain the deleted store; the current VAO's
            // attachments can be cleared by deletion. Both must be repaired.
            for (GLuint vao : {m_vao, otherVao}) {
                SCOPED_TRACE(vao);
                DrawQuad(vao);
                const auto px = CenterPixel();
                EXPECT_EQ(FirstGLError(), 0u);
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    if (colors[i][channel] != 0.f) {
                        EXPECT_GT(px[channel], 200) << "VAO did not fetch the replacement vertex store";
                    } else {
                        EXPECT_LT(px[channel], 50) << "VAO still fetched the previous vertex store";
                    }
                }
            }
        }
        glDeleteVertexArrays(1, &otherVao);
    }

    // P11 B2: A RESPECIFY WHILE A DRAW OF THE OLD STORE IS STILL QUEUED. The case above reads pixels
    // before every respecify, so the old store is retired by then and a server that freed it at
    // once would still pass. Here the first draw is only queued when the arena is redefined: the
    // old store (a T0 import: the client's AHardwareBuffer, which the client lets go of at this
    // very respecify) must outlive it, and the draw must show the bytes it was issued against.
    TEST_F(LargeArenaAdoptionScenario, ARespecifyWithADrawQueuedKeepsThatDrawsStore) {
        if (!Ready() || IsSkipped()) return;

        const GLint w = Gl().Width();
        const GLint h = Gl().Height();
        UploadQuad(1.f, 0.f, 0.f);
        glViewport(0, 0, w, h);
        glClearColor(0.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(m_program);
        glBindVertexArray(m_vao);
        glEnable(GL_SCISSOR_TEST);
        glScissor(0, 0, w / 2, h);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindBuffer(GL_ARRAY_BUFFER, m_arena);
        glBufferData(GL_ARRAY_BUFFER, kArenaBytes, nullptr, GL_DYNAMIC_DRAW);
        UploadQuad(0.f, 1.f, 0.f);
        glScissor(w / 2, 0, w - w / 2, h);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glDisable(GL_SCISSOR_TEST);

        std::array<unsigned char, 4> left = {0, 0, 0, 0};
        std::array<unsigned char, 4> right = {0, 0, 0, 0};
        glReadPixels(w / 4, h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, left.data());
        glReadPixels(w / 2 + w / 4, h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, right.data());
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_GT(left[0], 200) << "the draw queued before the respecify lost the store it was issued "
                                   "against (freed before that draw ran)";
        EXPECT_LT(left[1], 50) << "the draw queued before the respecify fetched the new store's bytes";
        EXPECT_GT(right[1], 200) << "the draw after the respecify did not fetch the new store";
        EXPECT_LT(right[0], 50) << "the draw after the respecify still fetched the old store";
    }

    TEST_F(LargeArenaAdoptionScenario, RespecifiedIndexArenaKeepsVaoBinding) {
        if (!Ready() || IsSkipped()) return;

        auto vertices = QuadVertices(1.f, 0.f, 0.f);
        const auto green = QuadVertices(0.f, 1.f, 0.f);
        vertices.insert(vertices.end(), green.begin(), green.end());
        glBindBuffer(GL_ARRAY_BUFFER, m_arena);
        glBufferSubData(GL_ARRAY_BUFFER, kVertexOffset,
                        GLsizeiptr(vertices.size() * sizeof(Vertex)), vertices.data());

        GLuint indices = 0;
        glGenBuffers(1, &indices);
        glBindVertexArray(m_vao);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indices);
        // Redefine through COPY_WRITE_BUFFER so the element binding slot never
        // changes. The small final store also exercises returning to shadow storage.
        glBindBuffer(GL_COPY_WRITE_BUFFER, indices);
        constexpr std::array<GLsizeiptr, 4> sizes = {
            kArenaBytes, kArenaBytes, kArenaBytes + 4096, 4096,
        };
        for (std::size_t i = 0; i < sizes.size(); ++i) {
            SCOPED_TRACE(sizes[i]);
            const GLuint first = (i % 2) == 0 ? 0u : 6u;
            const std::array<GLuint, 6> elements = {
                first, first + 1, first + 2, first + 3, first + 4, first + 5,
            };
            glBufferData(GL_COPY_WRITE_BUFFER, sizes[i], nullptr, GL_DYNAMIC_DRAW);
            glBufferSubData(GL_COPY_WRITE_BUFFER, 0, sizeof(elements), elements.data());
            glViewport(0, 0, Gl().Width(), Gl().Height());
            glClearColor(0.f, 0.f, 0.f, 1.f);
            glClear(GL_COLOR_BUFFER_BIT);
            glUseProgram(m_program);
            glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
            const auto px = CenterPixel();
            EXPECT_EQ(FirstGLError(), 0u);
            EXPECT_GT(px[first == 0 ? 0 : 1], 200) << "VAO did not fetch the replacement index store";
            EXPECT_LT(px[first == 0 ? 1 : 0], 50) << "VAO still fetched the previous index store";
        }
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
        glDeleteBuffers(1, &indices);
    }

    // The shadow IS the mapping: a readback straight after a CPU write must hand
    // back exactly those bytes.
    TEST_F(LargeArenaAdoptionScenario, ReadbackSeesTheLatestCpuWrite) {
        if (!Ready() || IsSkipped()) return;

        const auto vertices = QuadVertices(0.25f, 0.5f, 0.75f);
        glBindBuffer(GL_ARRAY_BUFFER, m_arena);
        glBufferSubData(GL_ARRAY_BUFFER, kVertexOffset,
                        GLsizeiptr(vertices.size() * sizeof(Vertex)), vertices.data());
        std::vector<Vertex> read(vertices.size());
        glGetBufferSubData(GL_ARRAY_BUFFER, kVertexOffset,
                           GLsizeiptr(read.size() * sizeof(Vertex)), read.data());
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_EQ(0, std::memcmp(read.data(), vertices.data(), read.size() * sizeof(Vertex)))
            << "GetBufferSubData of the adopted arena returned different bytes than the SubData wrote";
    }

    // A GPU write through an SSBO binding of the adopted arena must be visible to
    // a CPU readback - the path that waits out the GPU and reads the coherent
    // mapping directly.
    TEST_F(LargeArenaAdoptionScenario, GpuWriteIntoTheArenaIsReadBack) {
        if (!Ready() || IsSkipped()) return;

        GLint maxComputeStorageBlocks = 0;
        glGetIntegerv(GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS, &maxComputeStorageBlocks);
        if (maxComputeStorageBlocks < 1) {
            GTEST_SKIP() << "no compute shader storage blocks on this driver";
        }
        const GLuint compute = CompileStage(GL_COMPUTE_SHADER, kMarkerComputeSource);
        ASSERT_NE(compute, 0u) << m_buildLog;
        m_compute = glCreateProgram();
        glAttachShader(m_compute, compute);
        glLinkProgram(m_compute);
        glDeleteShader(compute);
        GLint linked = 0;
        glGetProgramiv(m_compute, GL_LINK_STATUS, &linked);
        ASSERT_EQ(linked, GL_TRUE);

        const unsigned int seed = 0u;
        glBindBuffer(GL_ARRAY_BUFFER, m_arena);
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(seed), &seed);
        glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 0, m_arena, 0, sizeof(unsigned int));
        glUseProgram(m_compute);
        glDispatchCompute(1, 1, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

        unsigned int marker = 0;
        glGetBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(marker), &marker);
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_EQ(marker, 0xC0FFEEu)
            << "the compute write into the adopted arena did not reach the CPU readback";
    }

    // G10, the per-adoption half: ONE storage definition of an arena costs ONE map_persistent
    // emission, however many draws read it afterwards.
    //
    // The arena SetUp defined is deliberately re-defined inside the counted window rather than
    // measured from outside it: the window a summary line reports is "since the previous line",
    // so the definition has to happen between the two swaps that bracket it, and a case that
    // counted SetUp's definition would be reading a window it did not control.
    //
    // ONE reading case per lane, for the reason PipeStatsWindow.h gives: the library truncates the
    // log per process, so two readers in a lane race under `ctest -j`.
    TEST_F(LargeArenaAdoptionScenario, AnAdoptionCostsExactlyOneMapPersistentRoundtrip) {
        if (!Ready() || IsSkipped()) return;
        SkipUnlessTheRoundtripCounterIsReadableHere();
        if (IsSkipped()) return;

        Gl().EndFrame(); // close the setup window, SetUp's own definition included

        // One definition of a store past the 16 MiB adoption threshold, taken by RE-SPECIFYING
        // SetUp's arena while m_vao's attributes are still pointing into it - and the attributes
        // are deliberately NOT re-declared afterwards, so the draws below can only land if the
        // backend VAO followed the new store on its own.
        //
        // That is the hard shape on purpose. It was routed around in the first cut of this file
        // because feat/disaggregated did not yet carry `dev`'s d7655247 ("rebind VAOs when an
        // adopted buffer is respecified - the immediate retire path forgot the buffer-id
        // generation") and the workload was a hard SIGSEGV inside the vertex fetch on the first
        // draw after the re-specification. ID-9 merged that fix (feat/disaggregated 5cb826b0) and
        // requires it to hold in BOTH the legacy and the handle arm of the respecify/retire path,
        // so this workload counts the path rather than avoiding it: under the
        // ResourceSubsystemOn./Off. lanes the same body runs on both arms, and a handle arm that
        // re-implemented the retire without the rebind is a crash here rather than a silent
        // divergence found on device.
        glBindBuffer(GL_ARRAY_BUFFER, m_arena);
        glBufferData(GL_ARRAY_BUFFER, kArenaBytes, nullptr, GL_DYNAMIC_DRAW);
        ASSERT_EQ(FirstGLError(), 0u) << "re-specifying the arena inside the counted window failed";

        // ... and then a frame's worth of traffic against it, of the shape the arena exists for:
        // a SubData per draw, every one of which lands in the adopted mapping and none of which
        // may acquire it again.
        for (int draw = 0; draw < kDrawsInTheWindow; ++draw) {
            UploadQuad(0.f, 1.f, 0.f);
            DrawQuad();
        }
        const auto px = CenterPixel();
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_GT(px[1], 200) << "the draws inside the counted window never landed, so the count below "
                                 "would be a number about nothing";

        Gl().EndFrame(); // the swap that emits the window covering exactly the work above
        const PipeStatsWindow::Window window = PipeStatsWindow::LastFromLaneLog();
        ASSERT_TRUE(window.found) << "no 'MGPipe stats:' line in " << PipeStatsWindow::LibraryLogPath()
                                  << ": either MOBILEGL_PIPE_STATS / MOBILEGL_PIPE_STATS_PERIOD did not "
                                     "reach the process, or nothing reached PipeStats::OnPresent.";
        RecordProperty("stats_line", window.line.c_str());

        const long long roundtrips = PipeStatsWindow::CounterOrAbsent(window, "mpr");
        ASSERT_GE(roundtrips, 0) << "the summary line carries no mpr= field: " << window.line;
        EXPECT_EQ(roundtrips, 1)
            << "one storage definition of an adopted arena is one map_persistent emission "
               "(ARCHITECTURE.md:474, D-B2: mint OR decline, both need an answer from the resource "
               "owner). This window defined the arena once and drew from it "
            << kDrawsInTheWindow << " times, so 1 is the whole cost; " << kDrawsInTheWindow
            << " would mean the acquisition moved onto the draw path - the ~167 ms/arena hiccup this "
               "adoption removed, re-introduced - and 0 would mean the emission stopped happening. It "
               "reported: "
            << window.line;
    }

    // P11 A2 (MG_Remote/CONTRACT-P11.md §2): WHICH TIER THE ARENA REALLY LANDED IN. The cases above
    // are green whether the arena adopted or stayed emulated, so the lane declares the arm
    // (MGITEST_PERSISTENT_MAP_ARM) and this case asserts it, the way
    // PersistentCoherentMapScenario.TheMapLandsInTheArmItsLaneDeclares does for a sub-16-MiB map.
    // Asked twice: after the NULL-data definition (TryAdoptLargeStorage's door) and after a frame
    // of use that ends in an SSBO binding and a dispatch (the storage-binding door,
    // EnsureGpuResidentStorage, which Magma's server walk opens). Lanes that declare nothing - the
    // ambient entries, which run under whatever transport the job exports - skip.
    TEST_F(LargeArenaAdoptionScenario, TheArenaLandsInTheTierItsLaneDeclares) {
        if (!Ready() || IsSkipped()) return;
        const std::string declared = SplitLane::DeclaredPersistentMapArm();
        if (declared.empty()) {
            GTEST_SKIP() << "this lane declares no arm: MGITEST_PERSISTENT_MAP_ARM is set only by the "
                            "lanes that pin their transport (monolith: adopted; the split arms: "
                            "emulated)";
        }
        if (!PersistentMapPeekAvailable()) {
            GTEST_SKIP() << "this lane declares the " << declared << " arm but Harness/PersistentMapPeek "
                            "cannot look in this build (on Android this module links the shipping "
                            "library, built -fvisibility=hidden). 'Could not look' is not 'it was "
                         << declared << "'.";
        }
        ExpectDeclaredArm(declared, "right after its NULL-data definition");
        if (HasFatalFailure()) return;

        UploadQuad(1.f, 0.f, 0.f);
        DrawQuad();
        EXPECT_GT(CenterPixel()[0], 200) << "the draw from the arena never landed";
        Gl().EndFrame();

        GLint maxComputeStorageBlocks = 0;
        glGetIntegerv(GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS, &maxComputeStorageBlocks);
        if (maxComputeStorageBlocks >= 1) {
            const GLuint compute = CompileStage(GL_COMPUTE_SHADER, kMarkerComputeSource);
            ASSERT_NE(compute, 0u) << m_buildLog;
            m_compute = glCreateProgram();
            glAttachShader(m_compute, compute);
            glLinkProgram(m_compute);
            glDeleteShader(compute);
            glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 0, m_arena, 0, sizeof(unsigned int));
            glUseProgram(m_compute);
            glDispatchCompute(1, 1, 1);
            glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
            unsigned int marker = 0;
            glBindBuffer(GL_ARRAY_BUFFER, m_arena);
            glGetBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(marker), &marker);
            EXPECT_EQ(marker, 0xC0FFEEu) << "the dispatch through the arena's SSBO binding never landed";
        } else {
            RecordProperty("arena_arm_used", "not asked: no compute shader storage blocks");
        }
        EXPECT_EQ(FirstGLError(), 0u);
        ExpectDeclaredArm(declared, "after a frame of draws, an SSBO binding and a dispatch");
    }

    // P11 A1 (MG_Remote/CONTRACT-P11.md §1): THE TCP LANE WITH MOBILEGL_IPC_ADOPT_TIER=0. A stream
    // data plane has no memory the two sides share, so the client refuses the knob by name at its
    // handshake - exactly one `Refuse{AdoptTierOnStream, "T0"}` line in its log - and the session
    // runs T2: the other cases in the same `.AdoptTier0.` lane are green and the arena is emulated.
    // Before A1 this lane ran T2 silently. Everywhere else it skips.
    TEST_F(LargeArenaAdoptionScenario, AStreamSessionRefusesTheAdoptTierOnceByName) {
        if (!Ready() || IsSkipped()) return;
        const char* tier = std::getenv(kAdoptTierRefusalMarker);
        if (tier == nullptr || tier[0] == '\0') {
            GTEST_SKIP() << "runs only in the `.AdoptTier0.` tcp entries, which set "
                            "MOBILEGL_IPC_ADOPT_TIER=0 and " << kAdoptTierRefusalMarker << "=T0";
        }
        if (PipeStatsWindow::LibraryLogPath().empty()) {
            GTEST_SKIP() << "the lane configured no MOBILEGL_LOG_FILE_PATH, and the client's log is "
                            "where the refusal is written";
        }
        // A frame of the arena's work first: a refusal is only worth reading from a session that
        // then ran - and an armed split lane requires the case to move the record ordinal anyway.
        UploadQuad(0.f, 1.f, 0.f);
        DrawQuad();
        EXPECT_GT(CenterPixel()[1], 200) << "the session that refused the tier did not render";
        EXPECT_EQ(FirstGLError(), 0u);
        Gl().EndFrame();

        const std::string log = PipeStatsWindow::ReadWholeFile(PipeStatsWindow::LibraryLogPath());
        const std::string line = std::string("Refuse{AdoptTierOnStream, \"") + tier + "\"}";
        std::size_t count = 0;
        for (auto at = log.find(line); at != std::string::npos; at = log.find(line, at + 1)) ++count;
        RecordProperty("adopt_tier_refusals", static_cast<int>(count));
        EXPECT_EQ(count, 1u) << "the client names MOBILEGL_IPC_ADOPT_TIER on a stream exactly once, at "
                                "its handshake (" << line << "), in " << PipeStatsWindow::LibraryLogPath();
        EXPECT_EQ(log.find("Fatal{UnimplementedAdoptTier"), std::string::npos) << log;
    }

    // P11 B2 (MG_Remote/CONTRACT-P11.md B2): A SHARED-SEGMENT SESSION THAT ASKS T0 AND CANNOT RUN IT.
    // MOBILEGL_IPC_ADOPT_TIER=0 over shared segments asks T0 in the Hello (A1 made it die at the
    // handshake; ruling ID-P11-14 made it this). The lane's server cannot grant it - a host has no
    // AHardwareBuffer, or `.AdoptT0Disallowed.` turned its allow switch off - so it refuses by name
    // at its first native bind, publishes no kCapAdoptT0, and the client falls back to T2 with ONE
    // named line at its first map_persistent. Never a Fatal; the other cases in the lane are green
    // and the arena is emulated. Everywhere else it skips.
    TEST_F(LargeArenaAdoptionScenario, ASessionThatCannotRunT0FallsBackToT2OnceByName) {
        if (!Ready() || IsSkipped()) return;
        const char* why = std::getenv(kAdoptT0FallbackMarker);
        if (why == nullptr || why[0] == '\0') {
            GTEST_SKIP() << "runs only in the shared-segment `.AdoptTier0.` / `.AdoptT0Disallowed.` entries, "
                            "which set MOBILEGL_IPC_ADOPT_TIER=0 and " << kAdoptT0FallbackMarker;
        }
        if (PipeStatsWindow::LibraryLogPath().empty()) {
            GTEST_SKIP() << "the lane configured no MOBILEGL_LOG_FILE_PATH, and the two logs are where the "
                            "fallback is written";
        }
        // A frame of the arena's work first: the fallback is only worth reading from a session that
        // then ran on T2 - and the client decides at its first map_persistent, which SetUp's
        // definition of the arena has already made.
        UploadQuad(0.f, 1.f, 0.f);
        DrawQuad();
        EXPECT_GT(CenterPixel()[1], 200) << "the session that fell back to T2 did not render";
        EXPECT_EQ(FirstGLError(), 0u);
        Gl().EndFrame();

        const auto countOf = [](const std::string& text, const std::string& needle) {
            std::size_t n = 0;
            for (auto at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++n;
            return n;
        };
        const std::string client = PipeStatsWindow::ReadWholeFile(PipeStatsWindow::LibraryLogPath());
        const std::string server = PipeStatsWindow::ReadServerLogSince(PipeStatsWindow::LogMark{});
        const std::string clientLine = "Refuse{AdoptT0Unavailable, \"no kCapAdoptT0\"}";
        const std::string serverLine = std::string("Refuse{AdoptT0Unavailable, \"") + why + "\"}";
        RecordProperty("t0_client_fallbacks", static_cast<int>(countOf(client, clientLine)));
        RecordProperty("t0_server_refusals", static_cast<int>(countOf(server, serverLine)));
        EXPECT_EQ(countOf(client, clientLine), 1u)
            << "the client names a T0 it asked for and cannot run exactly once, at its first map_persistent ("
            << clientLine << "), in " << PipeStatsWindow::LibraryLogPath();
        EXPECT_EQ(countOf(server, serverLine), 1u)
            << "the server names why it did not grant T0 exactly once, at its first native bind (" << serverLine
            << "), in " << PipeStatsWindow::ServerLibraryLogPath();
        EXPECT_EQ(client.find("Fatal{"), std::string::npos) << client;
        EXPECT_EQ(server.find("Fatal{"), std::string::npos) << server;
    }

} // namespace MGITest
