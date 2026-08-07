// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/CrossFrameBufferScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario B - "the draw rendered last frame's buffer".
//
// The shipped bug (DirectVulkan, TryBindResolvedVertexBindings and the EBO
// memo in UploadAndBindIndexBuffer): both memos revalidated themselves ACROSS a
// frame boundary by comparing recorded per-buffer slice epochs, and on a match
// skipped the per-frame buffer acquire. The acquire is the frame's content-sync
// point; skipping it trusted the BumpSliceEpoch call-site inventory to cover
// every way a buffer's GPU copy can go stale, and at least one path escaped it.
// Result: a draw in a later frame renders from a STALE buffer slice - random
// triangles in Minecraft/Sodium on Adreno, corrupted journeymap and
// common-mods retraces.
//
// What pins it: mutate a buffer AFTER a frame boundary and BEFORE the next
// draw, then prove the pixels show the NEW content. Every mutation API gets its
// own test case, so a failure names the culprit rather than saying "buffers".
// The index buffer is covered too: the EBO memo had exactly the same hole.
//
// The scene is deliberately trivial and entirely buffer-driven:
//
//     vertices 0..3   left half of the viewport,  RED
//     vertices 4..7   right half of the viewport, GREEN
//     indices  A      {0,1,2, 0,2,3}  -> the left, red quad
//     indices  B      {4,5,6, 4,6,7}  -> the right, green quad
//
// A vertex-buffer test rewrites the left quad's colour red -> green and expects
// the left half to turn green. An index-buffer test rewrites the indices
// A -> B and expects the picture to jump from a red left half to a green right
// half. Either way "stale" and "fresh" are different colours in different
// places; no thresholds, no interpretation.
//
// Two families of scenario live here, and they catch different halves of the
// same rule:
//
//   CrossFrameBufferScenario - one case per buffer-mutation API. Every one of
//     these APIs is supposed to retire the memo; today they all do (each notify
//     path bumps the slice epoch), so these pass on the buggy revision too.
//     They are the standing statement of the contract: whatever a future memo
//     keys on, a write through ANY of these APIs must reach the next frame's
//     draw. They are also where a coherent persistent write - the one shape
//     that changes a buffer with no GL call at all - is pinned.
//
//   StreamedArenaScenario - the case that actually caught the shipped bug. It
//     attacks the other half of the rule: a buffer nobody wrote at all, whose
//     GPU-side bytes moved out from under the memo anyway.

#include <cstdio>
#include <cstring>
#include <functional>
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

        constexpr const char* kVertexSource = R"(#version 330 core
in vec2 aPos;
in vec3 aColor;
out vec3 vColor;
void main() {
    vColor = aColor;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

        constexpr const char* kFragmentSource = R"(#version 330 core
in vec3 vColor;
out vec4 oColor;
void main() {
    oColor = vec4(vColor, 1.0);
}
)";

        struct Vertex {
            float x, y;
            float r, g, b;
        };

        constexpr int kLeftQuadFirstVertex = 0;
        constexpr int kLeftQuadVertexCount = 4;
        constexpr int kIndexCount = 6;

        // Enough consecutive frames drawing the same VAO that any per-(VAO, frame)
        // memo is fully armed before the mutation lands.
        constexpr int kWarmupFrames = 3;

        std::vector<Vertex> SceneVertices(bool leftQuadIsGreen) {
            const float lr = leftQuadIsGreen ? 0.0f : 1.0f;
            const float lg = leftQuadIsGreen ? 1.0f : 0.0f;
            return {
                // 0..3: left half
                {-1.0f, -1.0f, lr, lg, 0.0f},
                {0.0f, -1.0f, lr, lg, 0.0f},
                {0.0f, 1.0f, lr, lg, 0.0f},
                {-1.0f, 1.0f, lr, lg, 0.0f},
                // 4..7: right half
                {0.0f, -1.0f, 0.0f, 1.0f, 0.0f},
                {1.0f, -1.0f, 0.0f, 1.0f, 0.0f},
                {1.0f, 1.0f, 0.0f, 1.0f, 0.0f},
                {0.0f, 1.0f, 0.0f, 1.0f, 0.0f},
            };
        }

        const GLuint kIndicesLeftQuad[kIndexCount] = {0, 1, 2, 0, 2, 3};
        const GLuint kIndicesRightQuad[kIndexCount] = {4, 5, 6, 4, 6, 7};

        // How far inside each half the whole-region checks start. The two quads
        // meet on a pixel boundary, so a couple of pixels of margin makes "every
        // single pixel in the region" an achievable demand.
        constexpr int kHalfInset = 2;

        // Asserts the left and right halves of the viewport, with a message that
        // says what the app had asked GL to draw by then.
        //
        // This counts EVERY pixel in each half rather than sampling its centre.
        // Sampling two pixels was demonstrably too weak: a draw in which three of
        // the left quad's four vertices still carry stale data paints a centre
        // pixel of exactly the expected colour and passed the old assertion. That
        // case is now a standing negative control - see
        // PartialStalenessIsCaughtByWholeRegionChecks below, which constructs it
        // deliberately and proves the region scan reports it.
        void ExpectHalves(const Image& image, const char* expectedLeft, const char* expectedRight,
                          const std::string& when) {
            const int w = image.Width();
            const int h = image.Height();
            EXPECT_TRUE(RegionIsMostly(image, kHalfInset, w / 2 - kHalfInset, kHalfInset, h - kHalfInset, expectedLeft,
                                       0.0, when + " [left half]"));
            EXPECT_TRUE(RegionIsMostly(image, w / 2 + kHalfInset, w - kHalfInset, kHalfInset, h - kHalfInset,
                                       expectedRight, 0.0, when + " [right half]"));
        }

        // How the app hands the new bytes to GL. Each is its own test case.
        enum class Mutation {
            SubData,            // glBufferSubData
            MapWriteUnmap,      // glMapBufferRange(WRITE) + glUnmapBuffer
            PersistentFlush,    // write through a persistent map + glFlushMappedBufferRange
            PersistentCoherent, // write through a COHERENT persistent map, no GL call at all
            OrphanReupload,     // glBufferData(NULL) then a full re-upload
            CopySubData,        // glCopyBufferSubData from a staging buffer
        };

        bool NeedsImmutableStorage(Mutation mutation) {
            return mutation == Mutation::PersistentFlush || mutation == Mutation::PersistentCoherent;
        }

        // The coherent variant is the one shape in which an application changes a
        // buffer's contents with NO GL call whatsoever - the write lands in the
        // mapping and that is the end of it. Sodium's chunk streaming is written
        // this way, and it is the case a per-buffer "has anything changed?" epoch
        // cannot see on its own.
        bool NeedsCoherentMapping(Mutation mutation) {
            return mutation == Mutation::PersistentCoherent;
        }

        class CrossFrameBufferScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                std::string error;
                m_program = CompileProgram(kVertexSource, kFragmentSource, &error);
                ASSERT_NE(m_program, 0u) << error;
                ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "program setup left a GL error behind";
            }

            void TearDown() override {
                if (!Ready()) return;
                ReleaseBuffers();
                if (m_program != 0) glDeleteProgram(m_program);
            }

            // Builds the VAO/VBO/EBO. `immutable` switches to glBufferStorage plus a
            // persistent mapping of both buffers, which is the only shape in which the
            // persistent-write mutation is legal.
            void BuildScene(bool immutable, bool coherent = false) {
                const std::vector<Vertex> vertices = SceneVertices(/*leftQuadIsGreen=*/false);
                m_vertexBytes = GLsizeiptr(vertices.size() * sizeof(Vertex));
                m_indexBytes = GLsizeiptr(sizeof(kIndicesLeftQuad));

                glGenVertexArrays(1, &m_vao);
                glBindVertexArray(m_vao);

                glGenBuffers(1, &m_vbo);
                glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
                glGenBuffers(1, &m_ebo);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);

                if (immutable) {
                    const GLbitfield storageFlags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_DYNAMIC_STORAGE_BIT |
                                                    (coherent ? GL_MAP_COHERENT_BIT : 0);
                    glBufferStorage(GL_ARRAY_BUFFER, m_vertexBytes, vertices.data(), storageFlags);
                    glBufferStorage(GL_ELEMENT_ARRAY_BUFFER, m_indexBytes, kIndicesLeftQuad, storageFlags);
                    const GLenum storageError = FirstGLError();
                    if (storageError != GL_NO_ERROR) {
                        m_storageUnsupported = true;
                        m_storageError = storageError;
                        return;
                    }
                    const GLbitfield mapFlags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT |
                                                (coherent ? GL_MAP_COHERENT_BIT : GL_MAP_FLUSH_EXPLICIT_BIT);
                    m_vertexMap =
                        static_cast<unsigned char*>(glMapBufferRange(GL_ARRAY_BUFFER, 0, m_vertexBytes, mapFlags));
                    m_indexMap = static_cast<unsigned char*>(
                        glMapBufferRange(GL_ELEMENT_ARRAY_BUFFER, 0, m_indexBytes, mapFlags));
                    if (m_vertexMap == nullptr || m_indexMap == nullptr) {
                        m_storageUnsupported = true;
                        m_storageError = FirstGLError();
                        return;
                    }
                } else {
                    glBufferData(GL_ARRAY_BUFFER, m_vertexBytes, vertices.data(), GL_STATIC_DRAW);
                    glBufferData(GL_ELEMENT_ARRAY_BUFFER, m_indexBytes, kIndicesLeftQuad, GL_STATIC_DRAW);
                }

                glEnableVertexAttribArray(0);
                glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(0));
                glEnableVertexAttribArray(1);
                glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(8));
                glBindVertexArray(0);

                glGenBuffers(1, &m_staging);
                ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "scene setup left a GL error behind";
            }

            void ReleaseBuffers() {
                if (m_vertexMap != nullptr || m_indexMap != nullptr) {
                    glBindVertexArray(m_vao);
                    if (m_vertexMap != nullptr) {
                        glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
                        glUnmapBuffer(GL_ARRAY_BUFFER);
                    }
                    if (m_indexMap != nullptr) {
                        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
                        glUnmapBuffer(GL_ELEMENT_ARRAY_BUFFER);
                    }
                    glBindVertexArray(0);
                    m_vertexMap = nullptr;
                    m_indexMap = nullptr;
                }
                if (m_staging != 0) glDeleteBuffers(1, &m_staging);
                if (m_ebo != 0) glDeleteBuffers(1, &m_ebo);
                if (m_vbo != 0) glDeleteBuffers(1, &m_vbo);
                if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
                m_staging = m_ebo = m_vbo = m_vao = 0;
            }

            void DrawScene() {
                glDisable(GL_DEPTH_TEST);
                glDisable(GL_BLEND);
                glUseProgram(m_program);
                glBindVertexArray(m_vao);
                glDrawElements(GL_TRIANGLES, kIndexCount, GL_UNSIGNED_INT, nullptr);
                glBindVertexArray(0);
            }

            void BeginFrame() {
                BindDefaultFramebuffer();
                ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
            }

            Image ReadFrame() { return ReadPixels(Gl().Width(), Gl().Height()); }

            // ---- the mutations ---------------------------------------------
            // Each writes `newBytes` over the first `rangeBytes` of `buffer`;
            // `wholeBytes`/`wholeSize` are the full contents an orphan+re-upload
            // needs. `target` is the binding point the buffer normally lives at.
            void ApplyMutation(Mutation mutation, GLenum target, GLuint buffer, unsigned char* persistentMap,
                               const void* newBytes, GLsizeiptr rangeBytes, const void* wholeBytes,
                               GLsizeiptr wholeSize) {
                // The element-array binding is VAO state, so mutating the EBO happens
                // with the scene's VAO bound - exactly as an application would.
                glBindVertexArray(m_vao);
                switch (mutation) {
                case Mutation::SubData: {
                    glBindBuffer(target, buffer);
                    glBufferSubData(target, 0, rangeBytes, newBytes);
                    break;
                }
                case Mutation::MapWriteUnmap: {
                    glBindBuffer(target, buffer);
                    void* mapped =
                        glMapBufferRange(target, 0, rangeBytes, GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT);
                    ASSERT_NE(mapped, nullptr) << "glMapBufferRange(WRITE) returned null";
                    std::memcpy(mapped, newBytes, std::size_t(rangeBytes));
                    ASSERT_EQ(glUnmapBuffer(target), GLboolean(GL_TRUE)) << "glUnmapBuffer reported data loss";
                    break;
                }
                case Mutation::PersistentFlush: {
                    ASSERT_NE(persistentMap, nullptr) << "no persistent mapping for this buffer";
                    std::memcpy(persistentMap, newBytes, std::size_t(rangeBytes));
                    glBindBuffer(target, buffer);
                    glFlushMappedBufferRange(target, 0, rangeBytes);
                    break;
                }
                case Mutation::PersistentCoherent: {
                    // Deliberately no GL call: a coherent persistent mapping is a
                    // promise that the write alone is enough.
                    ASSERT_NE(persistentMap, nullptr) << "no persistent mapping for this buffer";
                    std::memcpy(persistentMap, newBytes, std::size_t(rangeBytes));
                    break;
                }
                case Mutation::OrphanReupload: {
                    glBindBuffer(target, buffer);
                    glBufferData(target, wholeSize, nullptr, GL_STATIC_DRAW);
                    glBufferSubData(target, 0, wholeSize, wholeBytes);
                    break;
                }
                case Mutation::CopySubData: {
                    glBindBuffer(GL_COPY_READ_BUFFER, m_staging);
                    glBufferData(GL_COPY_READ_BUFFER, rangeBytes, newBytes, GL_STATIC_DRAW);
                    glBindBuffer(GL_COPY_WRITE_BUFFER, buffer);
                    glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, rangeBytes);
                    glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
                    glBindBuffer(GL_COPY_READ_BUFFER, 0);
                    break;
                }
                }
                glBindVertexArray(0);
                ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "the mutation itself raised a GL error";
            }

            // ---- the story -------------------------------------------------
            // Steady state for a few frames, one frame boundary, then the
            // mutation, then the draw that must show the new content.
            void RunAcrossFrameBoundary(Mutation mutation, const std::function<void()>& mutate,
                                        const char* expectedLeftAfter, const char* expectedRightAfter) {
                ASSERT_NO_FATAL_FAILURE(BuildScene(NeedsImmutableStorage(mutation), NeedsCoherentMapping(mutation)));
                if (m_storageUnsupported) {
                    GTEST_SKIP() << "immutable/persistent buffer storage is unavailable on this stack ("
                                 << GLErrorName(m_storageError) << "); the persistent-map mutation cannot "
                                 << "be expressed here";
                }

                for (int frame = 0; frame < kWarmupFrames; ++frame) {
                    BeginFrame();
                    DrawScene();
                    Gl().EndFrame();
                }

                BeginFrame();
                DrawScene();
                const Image before = ReadFrame();
                ExpectHalves(before, "red", "black", "steady state before the mutation");
                ASSERT_FALSE(::testing::Test::HasFailure())
                    << "the scenario never reached its steady state, so nothing after this means anything";

                // >>> a genuine frame boundary. Everything below happens in the NEXT
                // frame, which is the whole point: a mutation inside one frame proves
                // nothing about a memo that revalidates itself across frames.
                Gl().EndFrame();

                BeginFrame();
                ASSERT_NO_FATAL_FAILURE(mutate());
                DrawScene();
                const Image after = ReadFrame();
                Gl().EndFrame();

                ExpectHalves(after, expectedLeftAfter, expectedRightAfter,
                             "the draw after the mutation drew STALE buffer content");
                EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
            }

            // The two things a scenario mutates.
            void MutateVertexColorsToGreen(Mutation mutation) {
                const std::vector<Vertex> updated = SceneVertices(/*leftQuadIsGreen=*/true);
                const GLsizeiptr leftQuadBytes = GLsizeiptr(kLeftQuadVertexCount * sizeof(Vertex));
                ApplyMutation(mutation, GL_ARRAY_BUFFER, m_vbo, m_vertexMap, updated.data() + kLeftQuadFirstVertex,
                              leftQuadBytes, updated.data(), m_vertexBytes);
            }

            void MutateIndicesToRightQuad(Mutation mutation) {
                ApplyMutation(mutation, GL_ELEMENT_ARRAY_BUFFER, m_ebo, m_indexMap, kIndicesRightQuad, m_indexBytes,
                              kIndicesRightQuad, m_indexBytes);
            }

            unsigned int m_program = 0;
            unsigned int m_vao = 0;
            unsigned int m_vbo = 0;
            unsigned int m_ebo = 0;
            unsigned int m_staging = 0;
            GLsizeiptr m_vertexBytes = 0;
            GLsizeiptr m_indexBytes = 0;
            unsigned char* m_vertexMap = nullptr;
            unsigned char* m_indexMap = nullptr;
            bool m_storageUnsupported = false;
            unsigned int m_storageError = 0;
        };

        // ---- vertex buffer: the left quad must turn green ------------------

        TEST_F(CrossFrameBufferScenario, VertexBufferSubData) {
            RunAcrossFrameBoundary(
                Mutation::SubData, [&] { MutateVertexColorsToGreen(Mutation::SubData); }, "green", "black");
        }

        TEST_F(CrossFrameBufferScenario, VertexMapWriteUnmap) {
            RunAcrossFrameBoundary(
                Mutation::MapWriteUnmap, [&] { MutateVertexColorsToGreen(Mutation::MapWriteUnmap); }, "green", "black");
        }

        TEST_F(CrossFrameBufferScenario, VertexPersistentMapFlush) {
            RunAcrossFrameBoundary(
                Mutation::PersistentFlush, [&] { MutateVertexColorsToGreen(Mutation::PersistentFlush); }, "green",
                "black");
        }

        TEST_F(CrossFrameBufferScenario, VertexPersistentCoherentWrite) {
            RunAcrossFrameBoundary(
                Mutation::PersistentCoherent, [&] { MutateVertexColorsToGreen(Mutation::PersistentCoherent); }, "green",
                "black");
        }

        TEST_F(CrossFrameBufferScenario, VertexOrphanAndReupload) {
            RunAcrossFrameBoundary(
                Mutation::OrphanReupload, [&] { MutateVertexColorsToGreen(Mutation::OrphanReupload); }, "green",
                "black");
        }

        TEST_F(CrossFrameBufferScenario, VertexCopyBufferSubData) {
            RunAcrossFrameBoundary(
                Mutation::CopySubData, [&] { MutateVertexColorsToGreen(Mutation::CopySubData); }, "green", "black");
        }

        // ---- index buffer: the picture must jump to the right, green quad --
        // The EBO memo had the same cross-frame hole as the vertex one, and no
        // vertex-only test can see it.

        TEST_F(CrossFrameBufferScenario, IndexBufferSubData) {
            RunAcrossFrameBoundary(
                Mutation::SubData, [&] { MutateIndicesToRightQuad(Mutation::SubData); }, "black", "green");
        }

        TEST_F(CrossFrameBufferScenario, IndexMapWriteUnmap) {
            RunAcrossFrameBoundary(
                Mutation::MapWriteUnmap, [&] { MutateIndicesToRightQuad(Mutation::MapWriteUnmap); }, "black", "green");
        }

        TEST_F(CrossFrameBufferScenario, IndexPersistentMapFlush) {
            RunAcrossFrameBoundary(
                Mutation::PersistentFlush, [&] { MutateIndicesToRightQuad(Mutation::PersistentFlush); }, "black",
                "green");
        }

        // Kept, with its coverage stated exactly, because it is the one case in
        // this file that is served a stale slice by the buggy revision and passes
        // anyway - and a test that reads as coverage without being coverage is
        // worse than no test.
        //
        // COVERS: the coherent-persistent index contract - a write into a coherent
        // persistent mapping, with no GL call at all, must reach the next frame's
        // draw. That is a real contract and this is the only case that states it
        // for indices.
        //
        // DOES NOT COVER: the EBO cross-frame memo. Instrumented against the
        // re-enabled buggy path, it enters the cross-frame branch 4 times and is
        // served its recorded slice all 4 times - and still passes, because the
        // backend adopted the persistent map into that very storage
        // (AcquirePersistentMap succeeded), so the application's writes landed in
        // the bytes the "stale" slice names. It would only discriminate on a stack
        // where that adoption is declined and the CPU shadow stays authoritative;
        // measured over this whole module, 50 of 50 coherent persistent write maps
        // were adopted. See ResidentIndexScenario.cpp for the full account.
        TEST_F(CrossFrameBufferScenario, IndexPersistentCoherentWrite) {
            RunAcrossFrameBoundary(
                Mutation::PersistentCoherent, [&] { MutateIndicesToRightQuad(Mutation::PersistentCoherent); }, "black",
                "green");
        }

        TEST_F(CrossFrameBufferScenario, IndexOrphanAndReupload) {
            RunAcrossFrameBoundary(
                Mutation::OrphanReupload, [&] { MutateIndicesToRightQuad(Mutation::OrphanReupload); }, "black",
                "green");
        }

        TEST_F(CrossFrameBufferScenario, IndexCopyBufferSubData) {
            RunAcrossFrameBoundary(
                Mutation::CopySubData, [&] { MutateIndicesToRightQuad(Mutation::CopySubData); }, "black", "green");
        }

        // ---- a self-test of the assertions, not of MobileGL ------------------
        //
        // Every case above leans on ExpectHalves. ExpectHalves used to sample the
        // centre pixel of each half - two pixels for a 12288-pixel readback - and
        // that is measurably too weak to stand behind a claim about buffer
        // freshness: a quad whose four vertices are only PARTLY updated still
        // paints a sampled centre the expected colour, because the centre is a
        // barycentric blend dominated by the vertices that DID update.
        //
        // So construct that case on purpose. Update the left quad's colour to
        // green in the buffer but leave exactly one of its four vertices holding
        // the old red, once for each vertex, and check two things:
        //
        //   - the whole-region scan reports every one of the four (the tightening
        //     is real, and this test fails the moment someone loosens it back to
        //     sampling);
        //   - at least one of the four is invisible to a single centre sample
        //     (the blind spot was real, and this records which vertices it hid).
        //
        // Nothing here calls a memo path; it is the assertion itself under test.
        TEST_F(CrossFrameBufferScenario, PartialStalenessIsCaughtByWholeRegionChecks) {
            ASSERT_NO_FATAL_FAILURE(BuildScene(/*immutable=*/false));

            const std::vector<Vertex> allGreen = SceneVertices(/*leftQuadIsGreen=*/true);
            const std::vector<Vertex> allRed = SceneVertices(/*leftQuadIsGreen=*/false);
            const GLsizeiptr leftQuadBytes = GLsizeiptr(kLeftQuadVertexCount * sizeof(Vertex));

            int centreSampleMissed = 0;
            std::string missedVertices;
            for (int staleVertex = 0; staleVertex < kLeftQuadVertexCount; ++staleVertex) {
                // Every left-quad vertex turns green except this one.
                std::vector<Vertex> partial(allGreen.begin(), allGreen.begin() + kLeftQuadVertexCount);
                partial[std::size_t(staleVertex)] = allRed[std::size_t(staleVertex)];

                glBindVertexArray(m_vao);
                glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
                glBufferSubData(GL_ARRAY_BUFFER, 0, leftQuadBytes, partial.data());
                glBindVertexArray(0);
                ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "the partial update itself raised a GL error";

                BeginFrame();
                DrawScene();
                const Image image = ReadFrame();
                Gl().EndFrame();

                const int w = image.Width();
                const int h = image.Height();
                const RegionScan scan =
                    ScanRegion(image, kHalfInset, w / 2 - kHalfInset, kHalfInset, h - kHalfInset, "green");
                EXPECT_GT(scan.offenders, 0)
                    << "vertex " << staleVertex << " of the left quad kept its stale red colour and the "
                    << "whole-region scan saw nothing wrong across " << scan.total << " pixels - the assertion "
                    << "is not tight enough to stand behind any freshness claim in this file";

                // What the old two-pixel form of ExpectHalves would have concluded.
                if (std::strcmp(image.ColorName(w / 4, h / 2), "green") == 0) {
                    ++centreSampleMissed;
                    if (!missedVertices.empty()) missedVertices += ",";
                    missedVertices += std::to_string(staleVertex);
                }
            }

            EXPECT_GT(centreSampleMissed, 0)
                << "no single-vertex staleness was invisible to a centre sample, so this negative control "
                << "is no longer demonstrating anything - re-derive it before trusting it";
            if (centreSampleMissed > 0) {
                RecordProperty("centre_sample_blind_to_stale_vertices", missedVertices);
                std::fprintf(stderr,
                             "[itest] whole-region scan caught all %d single-stale-vertex cases; a centre "
                             "sample alone was blind to %d of them (vertices %s)\n",
                             kLeftQuadVertexCount, centreSampleMissed, missedVertices.c_str());
            }
            EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
        }

        // ---- the same bug, seen from the other side --------------------------
        //
        // The mutation cases above ask "did the new bytes reach the GPU?". This
        // one asks the question a STREAMED buffer forces: "do the old bytes even
        // still exist?".
        //
        // A GL_STREAM_DRAW / GL_DYNAMIC_DRAW buffer is not given permanent GPU
        // storage. Every frame its contents are copied into that frame's
        // transient upload arena, which is a bump allocator reset at the start of
        // each frame slot - so a slice handed out in frame N names bytes that
        // frame N+frames-in-flight hands to whoever uploads first. A memo that
        // revalidates across a frame boundary and skips the acquire never
        // re-uploads, so it keeps binding an offset the arena has since given
        // away: the draw reads whatever the next tenant put there. That is the
        // "random triangles" shape of this bug - the buffer nobody touched is the
        // one that renders wrong.
        //
        // The scene makes the next tenant deterministic instead of arbitrary: a
        // second streamed object of exactly the same size is uploaded and drawn
        // FIRST in every frame, so it lands on precisely the bytes the memo still
        // points at. A draw that renders the decoy's geometry instead of its own
        // is unmissable.

        class StreamedArenaScenario : public ScenarioTest {
        protected:
            static constexpr int kQuietFrames = 2; // frames in which only the subject draws
            static constexpr int kChurnFrames = 8; // > frames-in-flight, so the ring wraps

            struct StreamedObject {
                unsigned int vao = 0;
                unsigned int vbo = 0;
                unsigned int ebo = 0;
            };

            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                std::string error;
                m_program = CompileProgram(kVertexSource, kFragmentSource, &error);
                ASSERT_NE(m_program, 0u) << error;
                ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
            }

            void TearDown() override {
                if (!Ready()) return;
                for (StreamedObject* object : {&m_subject, &m_decoy}) {
                    if (object->ebo != 0) glDeleteBuffers(1, &object->ebo);
                    if (object->vbo != 0) glDeleteBuffers(1, &object->vbo);
                    if (object->vao != 0) glDeleteVertexArrays(1, &object->vao);
                    *object = StreamedObject{};
                }
                if (m_program != 0) glDeleteProgram(m_program);
            }

            // GL_STREAM_DRAW is what puts a buffer on the transient arena
            // (ShouldUseTransientVertexIndexBuffer) - and what Minecraft uses for
            // exactly this kind of geometry.
            void BuildStreamedObject(StreamedObject& object, const std::vector<Vertex>& vertices,
                                     const GLuint (&indices)[kIndexCount]) {
                glGenVertexArrays(1, &object.vao);
                glBindVertexArray(object.vao);
                glGenBuffers(1, &object.vbo);
                glBindBuffer(GL_ARRAY_BUFFER, object.vbo);
                glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(vertices.size() * sizeof(Vertex)), vertices.data(),
                             GL_STREAM_DRAW);
                glGenBuffers(1, &object.ebo);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, object.ebo);
                glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(sizeof(indices)), indices, GL_STREAM_DRAW);
                glEnableVertexAttribArray(0);
                glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(0));
                glEnableVertexAttribArray(1);
                glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(8));
                glBindVertexArray(0);
            }

            void Draw(const StreamedObject& object) {
                glDisable(GL_DEPTH_TEST);
                glDisable(GL_BLEND);
                glUseProgram(m_program);
                glBindVertexArray(object.vao);
                glDrawElements(GL_TRIANGLES, kIndexCount, GL_UNSIGNED_INT, nullptr);
                glBindVertexArray(0);
            }

            // Re-uploading the decoy is what forces it onto a fresh arena slice
            // this frame - i.e. what makes it the arena's next tenant.
            void RestreamDecoy(const std::vector<Vertex>& vertices, const GLuint (&indices)[kIndexCount]) {
                glBindVertexArray(m_decoy.vao);
                glBindBuffer(GL_ARRAY_BUFFER, m_decoy.vbo);
                glBufferSubData(GL_ARRAY_BUFFER, 0, GLsizeiptr(vertices.size() * sizeof(Vertex)), vertices.data());
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_decoy.ebo);
                glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, GLsizeiptr(sizeof(indices)), indices);
                glBindVertexArray(0);
            }

            unsigned int m_program = 0;
            StreamedObject m_subject;
            StreamedObject m_decoy;
        };

        // Vertex data. Subject and decoy differ in geometry AND colour, so a
        // subject draw that reads the decoy's arena bytes paints the decoy's quad.
        TEST_F(StreamedArenaScenario, StreamedVertexDataSurvivesArenaRecycling) {
            const std::vector<Vertex> full = SceneVertices(/*leftQuadIsGreen=*/false);
            const std::vector<Vertex> subjectVertices(full.begin(), full.begin() + 4);   // left, red
            const std::vector<Vertex> decoyVertices(full.begin() + 4, full.begin() + 8); // right, green
            ASSERT_EQ(subjectVertices.size(), decoyVertices.size());                     // same arena footprint

            BuildStreamedObject(m_subject, subjectVertices, kIndicesLeftQuad);
            BuildStreamedObject(m_decoy, decoyVertices, kIndicesLeftQuad);
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "scene setup left a GL error behind";

            // Quiet frames: the subject is the only thing uploading, so its data
            // sits at the head of the arena and its memo records that offset.
            for (int frame = 0; frame < kQuietFrames; ++frame) {
                BindDefaultFramebuffer();
                ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
                Draw(m_subject);
                Gl().EndFrame();
            }

            // Churn frames: the decoy re-streams and draws first every frame. The
            // subject is never touched again - it must still render itself.
            for (int frame = 0; frame < kChurnFrames; ++frame) {
                BindDefaultFramebuffer();
                ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
                RestreamDecoy(decoyVertices, kIndicesLeftQuad);
                Draw(m_decoy);
                Draw(m_subject);
                const Image image = ReadPixels(Gl().Width(), Gl().Height());
                ExpectHalves(image, "red", "green",
                             "churn frame " + std::to_string(frame) +
                                 ": the untouched streamed vertex buffer rendered someone else's arena bytes");
                Gl().EndFrame();
            }
            EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
        }

        // Index data. Both objects carry the SAME eight vertices, so only the
        // element buffer can decide which half is drawn - this isolates the EBO
        // memo, which had its own copy of the cross-frame hole.
        //
        // COVERS: that an untouched streamed index buffer still renders its own
        // geometry after the arena it lives in has been recycled by another
        // object - the index-side statement of the invariant the vertex case
        // above actually catches.
        //
        // DOES NOT COVER: the EBO cross-frame memo. Instrumented against the
        // re-enabled buggy path this case reaches that branch ZERO times: the memo
        // is recorded only on the RESIDENT index path (UploadAndBindIndexBuffer
        // stores it in the arm after AcquireResidentSlice), and a streamed EBO
        // never gets there. So it passes on the buggy revision exactly as it does
        // on the fixed one, and it is not evidence about the fix.
        //
        // It stays because it is the tripwire for the change that would make the
        // EBO memo dangerous: memoise the streamed index path - the obvious next
        // step for the same optimisation - and the reach stops being zero and this
        // test fails on the first churn frame. See ResidentIndexScenario.cpp.
        TEST_F(StreamedArenaScenario, StreamedIndexDataSurvivesArenaRecycling) {
            const std::vector<Vertex> shared = SceneVertices(/*leftQuadIsGreen=*/false);

            BuildStreamedObject(m_subject, shared, kIndicesLeftQuad); // draws the left, red quad
            BuildStreamedObject(m_decoy, shared, kIndicesRightQuad);  // draws the right, green quad
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "scene setup left a GL error behind";

            for (int frame = 0; frame < kQuietFrames; ++frame) {
                BindDefaultFramebuffer();
                ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
                Draw(m_subject);
                Gl().EndFrame();
            }

            for (int frame = 0; frame < kChurnFrames; ++frame) {
                BindDefaultFramebuffer();
                ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
                RestreamDecoy(shared, kIndicesRightQuad);
                Draw(m_decoy);
                Draw(m_subject);
                const Image image = ReadPixels(Gl().Width(), Gl().Height());
                ExpectHalves(image, "red", "green",
                             "churn frame " + std::to_string(frame) +
                                 ": the untouched streamed index buffer rendered someone else's arena bytes");
                Gl().EndFrame();
            }
            EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
        }

    } // namespace
} // namespace MGITest
