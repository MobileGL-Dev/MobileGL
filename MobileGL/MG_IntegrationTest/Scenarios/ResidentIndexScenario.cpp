// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/ResidentIndexScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario C - RESIDENT index buffers across frame boundaries.
//
// WHAT THIS FILE DOES AND DOES NOT COVER, stated plainly because the answer is
// not the one it was written to find.
//
// The shipped fix (d7976326) removed cross-frame slice trust from TWO memos: the
// vertex-binding one and the EBO one. StreamedArenaScenario pins the vertex
// half - re-enable that half alone and it fails. Nothing pinned the EBO half,
// and these cases are the result of trying to build something that does.
//
// The EBO memo lives in UploadAndBindIndexBuffer and is recorded ONLY on the
// resident branch, keyed on (BufferObject*, VkBufferResource::sliceEpoch,
// frame serial). To fail with only the EBO revalidation re-enabled, a scenario
// needs a RESIDENT index buffer whose recorded slice stops describing the right
// bytes while the pointer and the epoch still match. Every case below is an
// attempt at that, run against the re-enabled buggy path with the branch
// instrumented to count reaches, acceptances, and - critically - what the
// skipped AcquireResidentSlice WOULD have done. The measurement, over this file
// plus every other scenario in the module:
//
//     reached=89 accepted=81 sliceMoved=0 bytesChanged=0 epochBumped=0
//
// The buggy branch is entered 89 times and serves its recorded slice 81 times,
// and in NOT ONE of those 81 would the acquire have moved the slice, changed a
// byte of it, or bumped the epoch. The skipped work was a no-op every time.
//
// That is not luck, it is the shape of the code. A resident slice is
// `resource->buffer.GetSlice(0, size)` of a dedicated VkBuffer, so it can only
// move when CreateResidentStorage mints new storage - which bumps the epoch. Its
// bytes can only change through Respecify / SubData / FlushMappedRange - each of
// which bumps the epoch as its first act - or through
// BufferObject::SyncPersistentMappedRange, which the acquire calls and the memo
// skips. That last one is the real escape, and it is dead here: it early-outs
// when the backend has adopted the map into coherent GPU storage, and
// AcquirePersistentMap only declines when a host-visible coherent allocation
// FAILS. Instrumented across the whole module: 50 persistent coherent write
// maps, 50 adopted, 0 dispatches. A 96 MiB EBO did not change that either.
//
// So on DirectVulkan as it stands, the EBO half of the fix is not reachable from
// a GL-level test - not because the guard is sound in principle (it is the same
// unsound idea the vertex half shipped corruption with) but because the two
// mechanisms that made the vertex half observable are both absent for indices:
//
//   1. ARENA RELOCATION. The vertex memo records STREAMED slices too, and a
//      streamed slice moves to a new arena block every frame BY DESIGN - the
//      epoch that catches it is bumped inside the very acquire the memo skips.
//      That is what StreamedVertexDataSurvivesArenaRecycling exploits. The index
//      memo is never recorded on the streamed branch, so no index memo ever
//      names an arena offset. Measured: StreamedIndexDataSurvivesArenaRecycling
//      reaches the branch 0 times, and so does PromotedDynamicEbo below (a
//      promoted DYNAMIC_DRAW buffer is SERVED by AcquireResidentSlice but still
//      ROUTED as streamed, so it is not memoised either).
//   2. HOST-MAP SYNC. Dead, as above.
//
// These cases therefore stay as what they honestly are: end-to-end regression
// tests for resident index-buffer freshness across frame boundaries, and the
// standing tripwire for change (1). The moment anyone memoises the streamed or
// promoted index path - the natural next step for the same optimisation - these
// stop being redundant and start failing. Each case says below what it covers.

#include <cstdio>
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

namespace MGITest {
    namespace {

        constexpr const char* kVS = R"(#version 330 core
in vec2 aPos;
in vec3 aColor;
out vec3 vColor;
void main() { vColor = aColor; gl_Position = vec4(aPos, 0.0, 1.0); }
)";
        constexpr const char* kFS = R"(#version 330 core
in vec3 vColor;
out vec4 oColor;
void main() { oColor = vec4(vColor, 1.0); }
)";

        struct V {
            float x, y, r, g, b;
        };
        constexpr int kIdx = 6;
        const GLuint kLeft[kIdx] = {0, 1, 2, 0, 2, 3};
        const GLuint kRight[kIdx] = {4, 5, 6, 4, 6, 7};

        std::vector<V> Scene() {
            return {{-1, -1, 1, 0, 0}, {0, -1, 1, 0, 0}, {0, 1, 1, 0, 0}, {-1, 1, 1, 0, 0},
                    {0, -1, 0, 1, 0},  {1, -1, 0, 1, 0}, {1, 1, 0, 1, 0},  {0, 1, 0, 1, 0}};
        }

        class ResidentIndexScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                std::string err;
                m_program = CompileProgram(kVS, kFS, &err);
                ASSERT_NE(m_program, 0u) << err;
            }
            void TearDown() override {
                if (!Ready()) return;
                if (m_program != 0) glDeleteProgram(m_program);
            }

            // A VAO whose VBO is STATIC_DRAW (so it resolves resident and the
            // vertex memo is recorded) and whose EBO is `eboName`.
            unsigned int MakeVao(unsigned int vbo, unsigned int ebo) {
                unsigned int vao = 0;
                glGenVertexArrays(1, &vao);
                glBindVertexArray(vao);
                glBindBuffer(GL_ARRAY_BUFFER, vbo);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
                glEnableVertexAttribArray(0);
                glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(V), reinterpret_cast<void*>(0));
                glEnableVertexAttribArray(1);
                glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(V), reinterpret_cast<void*>(8));
                glBindVertexArray(0);
                return vao;
            }

            unsigned int MakeStaticVbo() {
                const std::vector<V> vertices = Scene();
                unsigned int vbo = 0;
                glGenBuffers(1, &vbo);
                glBindBuffer(GL_ARRAY_BUFFER, vbo);
                glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(vertices.size() * sizeof(V)), vertices.data(),
                             GL_STATIC_DRAW);
                return vbo;
            }

            void Draw(unsigned int vao) {
                glDisable(GL_DEPTH_TEST);
                glDisable(GL_BLEND);
                glUseProgram(m_program);
                glBindVertexArray(vao);
                glDrawElements(GL_TRIANGLES, kIdx, GL_UNSIGNED_INT, nullptr);
                glBindVertexArray(0);
            }
            void Begin() {
                BindDefaultFramebuffer();
                ClearTo(0, 0, 0, 1);
            }
            Image Read() { return ReadPixels(Gl().Width(), Gl().Height()); }
            void Halves(const Image& image, const char* left, const char* right, const std::string& when) {
                const int w = image.Width(), h = image.Height();
                EXPECT_TRUE(RegionIsMostly(image, 2, w / 2 - 2, 2, h - 2, left, 0.0, when + " [left]"));
                EXPECT_TRUE(RegionIsMostly(image, w / 2 + 2, w - 2, 2, h - 2, right, 0.0, when + " [right]"));
            }

            unsigned int m_program = 0;
        };

        // A: a coherent persistent EBO rewritten on EVERY frame, with no GL call
        // between the write and the draw. This is the only shape in which an
        // application changes index data with nothing for the backend to notice.
        //
        // COVERS: the coherent-persistent index contract end to end.
        // DOES NOT COVER: the EBO memo. Instrumented it reaches the cross-frame
        // branch 11 times and is served its recorded slice all 11 - but the
        // backend adopted the map into that same storage, so the "stale" slice IS
        // where the application's writes landed. It would only discriminate on a
        // stack where AcquirePersistentMap declines (see the file header). A
        // 96 MiB variant was tried to force that and did not: it cost 40s and
        // measured the same zero, so it is not kept.
        TEST_F(ResidentIndexScenario, PersistentCoherentEboWrittenEveryFrame) {
            const unsigned int vbo = MakeStaticVbo();
            unsigned int ebo = 0;
            glGenBuffers(1, &ebo);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
            const GLbitfield storageFlags =
                GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT | GL_DYNAMIC_STORAGE_BIT;
            glBufferStorage(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(sizeof(kLeft)), kLeft, storageFlags);
            if (FirstGLError() != GL_NO_ERROR) GTEST_SKIP() << "no immutable storage";
            auto* map = static_cast<unsigned char*>(glMapBufferRange(
                GL_ELEMENT_ARRAY_BUFFER, 0, GLsizeiptr(sizeof(kLeft)),
                GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT));
            ASSERT_NE(map, nullptr);
            const unsigned int vao = MakeVao(vbo, ebo);

            for (int frame = 0; frame < 12; ++frame) {
                Begin();
                const bool wantRight = (frame % 2) == 1;
                std::memcpy(map, wantRight ? kRight : kLeft, sizeof(kLeft));
                Draw(vao);
                const Image image = Read();
                Halves(image, wantRight ? "black" : "red", wantRight ? "green" : "black",
                       "frame " + std::to_string(frame) + " of a per-frame coherent EBO rewrite");
                Gl().EndFrame();
            }
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
            glUnmapBuffer(GL_ELEMENT_ARRAY_BUFFER);
            glDeleteVertexArrays(1, &vao);
            glDeleteBuffers(1, &ebo);
            glDeleteBuffers(1, &vbo);
        }

        // B: usage escalation. The EBO is memoised as an index buffer, then bound
        // as a VERTEX buffer in a later frame, which forces the backend to
        // recreate its resident storage carrying the extra usage bit. A memo that
        // survived that recreate would name a destroyed VkBuffer.
        //
        // COVERS: that a storage recreate driven by a DIFFERENT binding point
        // retires the index memo. Reaches the branch 5 times.
        TEST_F(ResidentIndexScenario, EboAlsoBoundAsVertexBufferLater) {
            const unsigned int vbo = MakeStaticVbo();
            unsigned int ebo = 0;
            glGenBuffers(1, &ebo);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
            // Big enough to be a legal (if nonsensical) vertex source too.
            std::vector<GLuint> indices(64, 0);
            std::memcpy(indices.data(), kLeft, sizeof(kLeft));
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(indices.size() * 4), indices.data(), GL_STATIC_DRAW);
            const unsigned int vao = MakeVao(vbo, ebo);

            unsigned int vertexUseVao = 0;
            glGenVertexArrays(1, &vertexUseVao);
            glBindVertexArray(vertexUseVao);
            glBindBuffer(GL_ARRAY_BUFFER, ebo); // the EBO, as a vertex source
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(V), reinterpret_cast<void*>(0));
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(V), reinterpret_cast<void*>(8));
            glBindVertexArray(0);

            for (int frame = 0; frame < 6; ++frame) {
                Begin();
                Draw(vao);
                if (frame == 2) Draw(vertexUseVao); // forces the usage escalation
                const Image image = Read();
                if (frame != 2) {
                    Halves(image, "red", "black", "frame " + std::to_string(frame) + " around a usage escalation");
                }
                Gl().EndFrame();
            }
            glDeleteVertexArrays(1, &vertexUseVao);
            glDeleteVertexArrays(1, &vao);
            glDeleteBuffers(1, &ebo);
            glDeleteBuffers(1, &vbo);
        }

        // C: delete the EBO and immediately recreate it, so the frontend
        // BufferObject may well land at the same address - which is all the memo's
        // identity check compares. What stops it is that a fresh resource cannot
        // reproduce an epoch from the process-lifetime counter; this is the test
        // that says so out loud.
        //
        // COVERS: address reuse of a deleted index buffer. Reaches 7, accepts 6 -
        // the one decline is the post-recreate draw.
        TEST_F(ResidentIndexScenario, EboDeletedAndRecreatedAtTheSameName) {
            const unsigned int vbo = MakeStaticVbo();
            unsigned int ebo = 0;
            glGenBuffers(1, &ebo);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(sizeof(kLeft)), kLeft, GL_STATIC_DRAW);
            unsigned int vao = MakeVao(vbo, ebo);

            for (int frame = 0; frame < 4; ++frame) {
                Begin();
                Draw(vao);
                Halves(Read(), "red", "black", "warmup frame " + std::to_string(frame));
                Gl().EndFrame();
            }

            // Same VAO, same GL name, different contents.
            glDeleteVertexArrays(1, &vao);
            glDeleteBuffers(1, &ebo);
            glGenBuffers(1, &ebo);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(sizeof(kRight)), kRight, GL_STATIC_DRAW);
            vao = MakeVao(vbo, ebo);

            for (int frame = 0; frame < 4; ++frame) {
                Begin();
                Draw(vao);
                Halves(Read(), "black", "green", "post-recreate frame " + std::to_string(frame));
                Gl().EndFrame();
            }
            glDeleteVertexArrays(1, &vao);
            glDeleteBuffers(1, &ebo);
            glDeleteBuffers(1, &vbo);
        }

        // D: one resident EBO shared by two VAOs, so two independent memo entries
        // hold the same recorded slice, mutated through one of them and drawn
        // through both across frames.
        //
        // COVERS: that a mutation retires EVERY memo naming the buffer, not just
        // the one whose VAO issued it. Reaches 8, accepts 6.
        TEST_F(ResidentIndexScenario, OneEboTwoVaosMutatedAcrossFrames) {
            const unsigned int vbo = MakeStaticVbo();
            unsigned int ebo = 0;
            glGenBuffers(1, &ebo);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(sizeof(kLeft)), kLeft, GL_STATIC_DRAW);
            const unsigned int vaoA = MakeVao(vbo, ebo);
            const unsigned int vaoB = MakeVao(vbo, ebo);

            for (int frame = 0; frame < 10; ++frame) {
                Begin();
                const bool wantRight = frame >= 5;
                if (frame == 5) {
                    glBindVertexArray(vaoA);
                    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
                    glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, GLsizeiptr(sizeof(kRight)), kRight);
                    glBindVertexArray(0);
                }
                Draw((frame % 2) == 0 ? vaoA : vaoB);
                Halves(Read(), wantRight ? "black" : "red", wantRight ? "green" : "black",
                       "shared-EBO frame " + std::to_string(frame));
                Gl().EndFrame();
            }
            glDeleteVertexArrays(1, &vaoB);
            glDeleteVertexArrays(1, &vaoA);
            glDeleteBuffers(1, &ebo);
            glDeleteBuffers(1, &vbo);
        }

        // E: a DYNAMIC_DRAW EBO left untouched long enough for the streaming path
        // to PROMOTE it onto resident storage, then mutated.
        //
        // COVERS: promoted-buffer index freshness across a frame boundary.
        // DOES NOT COVER: the EBO memo, and this is the useful part - instrumented,
        // it reaches the cross-frame branch ZERO times. A promoted buffer is SERVED
        // by AcquireResidentSlice but still ROUTED through the streamed branch of
        // UploadAndBindIndexBuffer, which never records a memo. That asymmetry is
        // exactly what makes the EBO half of the shipped fix unobservable, and this
        // case is the tripwire: memoise the streamed/promoted index path and the
        // reach stops being zero.
        TEST_F(ResidentIndexScenario, PromotedDynamicEbo) {
            const unsigned int vbo = MakeStaticVbo();
            unsigned int ebo = 0;
            glGenBuffers(1, &ebo);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(sizeof(kLeft)), kLeft, GL_DYNAMIC_DRAW);
            const unsigned int vao = MakeVao(vbo, ebo);

            for (int frame = 0; frame < 10; ++frame) {
                Begin();
                Draw(vao);
                Halves(Read(), "red", "black", "promotion warmup frame " + std::to_string(frame));
                Gl().EndFrame();
            }
            for (int frame = 0; frame < 6; ++frame) {
                Begin();
                if (frame == 0) {
                    glBindVertexArray(vao);
                    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
                    glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, GLsizeiptr(sizeof(kRight)), kRight);
                    glBindVertexArray(0);
                }
                Draw(vao);
                Halves(Read(), "black", "green", "post-promotion frame " + std::to_string(frame));
                Gl().EndFrame();
            }
            glDeleteVertexArrays(1, &vao);
            glDeleteBuffers(1, &ebo);
            glDeleteBuffers(1, &vbo);
        }

    } // namespace
} // namespace MGITest
