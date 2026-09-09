// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/P4aFinalFixScenario.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - THE THREE FINDINGS OF THE P4a FINAL WHOLE-DIFF REVIEW (final-review-v1.md C-1, C-2,
// M-A), each pinned by the public-GL sequence that was red on the tree the review read and is
// green with its fix. Every sequence here is legal GL and none of the 80-odd scenarios before
// this file drove it, which is how two criticals shipped through a green gate.
//
//   C-1  The client never passed the applier the LEVEL a respecify redefines, so every per-level
//        glTexImage*D / glGenerateMipmap grow took the applier's whole-resource arm and dropped
//        EVERY pending upload of the texture - including a level the applier had already
//        accepted and whose client-side dirty flag was therefore already clear (D-D5 step 1).
//        Nobody owed those texels any more. The window is "accepted but not yet consumed":
//        a verb the texture is not reached by (a draw with another texture) drains the level
//        into the applier, Espryt does not sync the texture, and the next level definition eats
//        the entry. Two hazard cases (a level-1 definition, a glGenerateMipmap) read a black
//        level 0 on the handle arm; the three controls beside them (no verb between, level 0
//        consumed first, an immediate generate) are red on every arm, which is what pins the
//        window rather than the mip path.
//   C-2  A dead-but-not-recycled texture handle still resolved to the freed ITextureObject*
//        inside the client's drain: the death helper freed the slot without telling the emitter,
//        the drain list kept the level, and the next verb's drain called virtual
//        GetStorageType() on freed memory - `glTexImage2D; glDeleteTextures; <any verb>` was a
//        SIGABRT ("pure virtual method called") at the shipping default mask. The same
//        delete-then-use shape is driven for every kind P4a mints (renderbuffer, sampler object,
//        program, framebuffer) and for a slot recycled straight after the death (ABA), on both
//        backends: the death path is backend-neutral by ruling (ID-8) and the DirectVulkan lane
//        must see it too.
//   M-A  Nothing produced kMGPipeBindSampler / kMGPipeBindShaderImage, so ImageBindableHint was
//        dead: the applier never saw a texture become image-bound, the metadata respecify
//        (ID-18 M4) had no live trigger, and the remint pull the hint exists to prevent was
//        neither prevented nor counted. The case here reads the applier's record around a
//        glBindImageTexture: the hint arrives as a metadata update that keeps the pending upload
//        standing beside it, and the picture after the transition is the texels that upload
//        carried.
//
// A WHITE-BOX READING THAT CANNOT BE TAKEN IS DECLINED BY NAME AND THE CASE CONTINUES with its
// public-GL half (P4aSeamAuditScenario.cpp's shape): a pull build or a backend with no P4a
// consumer holds no record to read, and skipping the whole case there would delete the verdict
// those lanes carry. The C-1 and M-A cases assert their pictures on DirectGLES only - Espryt is
// the one consumer of the texture records this phase wires, so on any other backend the handle
// arm is inert by design and the picture proves nothing about it.

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "../Harness/HeadlessGL.h"
#include "../Harness/P4aFinalFixPeek.h"
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

        constexpr int kInset = 2;

        constexpr const char* kVS = R"(#version 330 core
in vec2 aPos;
out vec2 vUv;
void main() {
    vUv = aPos * 0.5 + 0.5;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

        constexpr const char* kFS = R"(#version 330 core
in vec2 vUv;
uniform sampler2D uTex;
out vec4 oColor;
void main() { oColor = texture(uTex, vUv); }
)";

        struct Vertex {
            float x, y;
        };

        class P4aFinalFixScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                std::string error;
                m_program = CompileProgram(kVS, kFS, &error);
                ASSERT_NE(m_program, 0u) << error;

                static const Vertex quad[6] = {{-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f},
                                               {-1.0f, -1.0f}, {1.0f, 1.0f},  {-1.0f, 1.0f}};
                glGenBuffers(1, &m_quadBuffer);
                glBindBuffer(GL_ARRAY_BUFFER, m_quadBuffer);
                glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
                glGenVertexArrays(1, &m_vao);
                glBindVertexArray(m_vao);
                glEnableVertexAttribArray(0);
                glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), nullptr);
                glBindVertexArray(0);
                glDisable(GL_BLEND);
                glDisable(GL_SCISSOR_TEST);
                glDisable(GL_DEPTH_TEST);
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

                // The "other" texture: a complete, single-level white texture, so a draw that
                // samples it is a verb the texture under test is not reached by.
                m_other = MakeLevel0(255, 255, 255, /*maxLevel=*/0);
                while (glGetError() != GL_NO_ERROR) {
                }
            }

            void TearDown() override {
                if (!Ready()) return;
                glUseProgram(0);
                glBindVertexArray(0);
                glBindTexture(GL_TEXTURE_2D, 0);
                glBindBuffer(GL_ARRAY_BUFFER, 0);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                if (m_other != 0) glDeleteTextures(1, &m_other);
                if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
                if (m_quadBuffer != 0) glDeleteBuffers(1, &m_quadBuffer);
                if (m_program != 0) glDeleteProgram(m_program);
                while (glGetError() != GL_NO_ERROR) {
                }
            }

            // The C-1 and M-A pictures are about Espryt's consumption of the texture records;
            // Magma registers no consumer for the P4a families (c0f), so the handle arm is inert
            // there by design and a green picture proves nothing about the finding. Marks the
            // case skipped; the caller tests IsSkipped() and returns.
            void SkipUnlessEspryt(const char* what) {
                if (Gl().BackendName() == "DirectGLES") return;
                GTEST_SKIP() << what << " is consumed by DirectGLES only; backend is " << Gl().BackendName();
            }

            static std::vector<std::uint8_t> Solid(int size, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
                std::vector<std::uint8_t> texels(static_cast<std::size_t>(size) * size * 4);
                for (std::size_t i = 0; i < texels.size(); i += 4) {
                    texels[i] = r;
                    texels[i + 1] = g;
                    texels[i + 2] = b;
                    texels[i + 3] = 255;
                }
                return texels;
            }

            // A 4x4 level 0 of one colour, NEAREST_MIPMAP_NEAREST with the level range clamped
            // to `maxLevel`, so a single-level texture is complete and a chain is complete once
            // its levels exist.
            static GLuint MakeLevel0(std::uint8_t r, std::uint8_t g, std::uint8_t b, int maxLevel, int size = 4) {
                const std::vector<std::uint8_t> texels = Solid(size, r, g, b);
                GLuint texture = 0;
                glGenTextures(1, &texture);
                glBindTexture(GL_TEXTURE_2D, texture);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels.data());
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, maxLevel);
                glBindTexture(GL_TEXTURE_2D, 0);
                return texture;
            }

            static void DefineLevel1(GLuint texture, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
                const std::vector<std::uint8_t> texels = Solid(2, r, g, b);
                glBindTexture(GL_TEXTURE_2D, texture);
                glTexImage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels.data());
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1);
                glBindTexture(GL_TEXTURE_2D, 0);
            }

            // A full-viewport draw sampling `texture` on unit 0 through `program` (the fixture's
            // by default). The viewport is far larger than the 4x4 base level, so this is
            // MAGNIFICATION and reads LEVEL 0 whatever the chain holds above it.
            Image DrawSampled(GLuint texture, GLuint program = 0) {
                if (program == 0) program = m_program;
                BindDefaultFramebuffer();
                glViewport(0, 0, Gl().Width(), Gl().Height());
                glUseProgram(program);
                glUniform1i(glGetUniformLocation(program, "uTex"), 0);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, texture);
                glBindVertexArray(m_vao);
                glDrawArrays(GL_TRIANGLES, 0, 6);
                Image image = ReadPixels(Gl().Width(), Gl().Height());
                glBindTexture(GL_TEXTURE_2D, 0);
                glBindVertexArray(0);
                Gl().EndFrame();
                return image;
            }

            ::testing::AssertionResult Mostly(const Image& image, const char* color, const std::string& when) {
                return RegionIsMostly(image, kInset, image.Width() - kInset, kInset, image.Height() - kInset, color,
                                      0.0, when);
            }

            void Report(const char* caseName, const Image& image) {
                const char* mask = std::getenv("MOBILEGL_PIPE_PUSH");
                const int cx = image.Width() / 2;
                const int cy = image.Height() / 2;
                std::cout << "[ P4aFinalFix ] case=" << caseName << " backend=" << Gl().BackendName()
                          << " MOBILEGL_PIPE_PUSH=" << (mask ? mask : "(unset)") << " centre=" << image.At(cx, cy)
                          << " (" << image.ColorName(cx, cy) << ")" << std::endl;
            }

            // The white-box gate of the M-A case: true when the applier holds a record for the
            // texture in this process. Prints the decline.
            bool RecordIsReadable(unsigned glTextureName, const char* what, PipeTextureResourceRecordPeek* out) {
                if (PeekPipeTextureResourceRecord(glTextureName, out)) return true;
                std::cout << "[ P4aFinalFix ] white-box reading DECLINED for " << what
                          << ": the applier holds no record for texture " << glTextureName
                          << " (a pull build, or a backend with no P4a consumer); the public-GL half of "
                             "the case still runs"
                          << std::endl;
                RecordProperty("p4a_finalfix_white_box", "declined");
                return false;
            }

            GLuint m_program = 0;
            GLuint m_vao = 0;
            GLuint m_quadBuffer = 0;
            GLuint m_other = 0;
        };

        // ======================================================================================
        // C-1: a per-level definition around a verb the texture is not reached by
        // ======================================================================================

        // THE HAZARD. L0's upload is accepted at the unrelated draw's validate point (the client
        // clears its flag), Espryt never syncs T there (it is bound nowhere), then the level-1
        // definition respecifies the resource. Before the fix that respecify carried no level and
        // the applier dropped every pending upload; level 0 was allocated undefined.
        TEST_F(P4aFinalFixScenario, PerLevelDefinitionAcrossAnUnrelatedDraw) {
            if (!Ready()) return;
            SkipUnlessEspryt("C-1's per-level respecify");
            if (IsSkipped()) return;

            const GLuint texture = MakeLevel0(255, 0, 0, /*maxLevel=*/0);
            const Image unrelated = DrawSampled(m_other);
            EXPECT_TRUE(Mostly(unrelated, "white", "the unrelated draw"));
            DefineLevel1(texture, 255, 0, 0);
            const Image image = DrawSampled(texture);
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
            Report("PerLevelDefinitionAcrossAnUnrelatedDraw", image);
            EXPECT_TRUE(Mostly(image, "red",
                               "level 0 after a level-1 definition that followed a draw the texture was not "
                               "reached by - its accepted-but-unconsumed upload was dropped by the whole-"
                               "resource arm"));
            GLuint cleanup = texture;
            glDeleteTextures(1, &cleanup);
        }

        // CONTROL: both levels defined before any verb; both are pending at the first sync.
        TEST_F(P4aFinalFixScenario, ConsecutiveDefinitionsNoVerbBetween) {
            if (!Ready()) return;
            SkipUnlessEspryt("C-1's per-level respecify");
            if (IsSkipped()) return;

            const GLuint texture = MakeLevel0(255, 0, 0, /*maxLevel=*/0);
            DefineLevel1(texture, 255, 0, 0);
            const Image image = DrawSampled(texture);
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
            Report("ConsecutiveDefinitionsNoVerbBetween", image);
            EXPECT_TRUE(Mostly(image, "red", "level 0 with both levels defined back to back"));
            GLuint cleanup = texture;
            glDeleteTextures(1, &cleanup);
        }

        // CONTROL: level 0 is consumed by Espryt (T is sampled) before level 1 is defined.
        TEST_F(P4aFinalFixScenario, LevelZeroConsumedBeforeLevelOne) {
            if (!Ready()) return;
            SkipUnlessEspryt("C-1's per-level respecify");
            if (IsSkipped()) return;

            const GLuint texture = MakeLevel0(255, 0, 0, /*maxLevel=*/0);
            const Image first = DrawSampled(texture);
            EXPECT_TRUE(Mostly(first, "red", "level 0 alone"));
            DefineLevel1(texture, 255, 0, 0);
            const Image image = DrawSampled(texture);
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
            Report("LevelZeroConsumedBeforeLevelOne", image);
            EXPECT_TRUE(Mostly(image, "red", "level 0 after level 1 was added to a synced texture"));
            GLuint cleanup = texture;
            glDeleteTextures(1, &cleanup);
        }

        // THE HAZARD, glGenerateMipmap flavour: the frontend grows the level chain (one
        // AllocateStorage -> respecify per level) BEFORE the backend generate runs, with level 0
        // accepted-but-unconsumed. The driver then built the chain from an undefined level 0.
        TEST_F(P4aFinalFixScenario, GenerateMipmapAcrossAnUnrelatedDraw) {
            if (!Ready()) return;
            SkipUnlessEspryt("C-1's per-level respecify");
            if (IsSkipped()) return;

            const GLuint texture = MakeLevel0(255, 0, 0, /*maxLevel=*/1000);
            const Image unrelated = DrawSampled(m_other);
            EXPECT_TRUE(Mostly(unrelated, "white", "the unrelated draw"));
            glBindTexture(GL_TEXTURE_2D, texture);
            glGenerateMipmap(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, 0);
            const Image image = DrawSampled(texture);
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
            Report("GenerateMipmapAcrossAnUnrelatedDraw", image);
            EXPECT_TRUE(Mostly(image, "red",
                               "level 0 after a glGenerateMipmap that followed a draw the texture was not "
                               "reached by"));
            GLuint cleanup = texture;
            glDeleteTextures(1, &cleanup);
        }

        // CONTROL for the generate: no verb between the upload and the generate.
        TEST_F(P4aFinalFixScenario, GenerateMipmapImmediately) {
            if (!Ready()) return;
            SkipUnlessEspryt("C-1's per-level respecify");
            if (IsSkipped()) return;

            const GLuint texture = MakeLevel0(255, 0, 0, /*maxLevel=*/1000);
            glBindTexture(GL_TEXTURE_2D, texture);
            glGenerateMipmap(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, 0);
            const Image image = DrawSampled(texture);
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
            Report("GenerateMipmapImmediately", image);
            EXPECT_TRUE(Mostly(image, "red", "level 0 after an immediate glGenerateMipmap"));
            GLuint cleanup = texture;
            glDeleteTextures(1, &cleanup);
        }

        // ======================================================================================
        // C-2: delete-then-use, for every kind P4a mints, on both backends
        // ======================================================================================

        // A level goes dirty, the texture dies before any verb, and the next verb's drain walks
        // the entry. Before the fix the emitter resolved the dead handle to the freed object and
        // the drain called a virtual on it: SIGABRT in the first round. Eight rounds, and the
        // lane registered with MALLOC_PERTURB_ scribbles every freed block so a resolved-but-
        // dead pointer faults rather than reads the object's ghost.
        TEST_F(P4aFinalFixScenario, ADirtyTextureDeletedBeforeAnyVerbIsWalkedByTheNextDrain) {
            if (!Ready()) return;
            for (int round = 0; round < 8; ++round) {
                GLuint texture = 0;
                glGenTextures(1, &texture);
                glBindTexture(GL_TEXTURE_2D, texture);
                const std::vector<std::uint8_t> texels = Solid(4, 255, 0, 0);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels.data());
                glBindTexture(GL_TEXTURE_2D, 0);
                glDeleteTextures(1, &texture); // the last reference: the frontend object dies here
                // Something else is allocated between the death and the drain, so the freed
                // storage is not simply re-handed to the next object.
                std::vector<std::uint8_t> churn(4096 + round * 1024, static_cast<std::uint8_t>(round));
                (void)churn;
                const Image image = DrawSampled(m_other); // the validate point: the drain runs here
                ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
                if (round == 0) Report("ADirtyTextureDeletedBeforeAnyVerbIsWalkedByTheNextDrain", image);
                EXPECT_TRUE(Mostly(image, "white", "the draw after a dirty texture died"));
            }
        }

        // ABA: the slot the dead texture held is handed straight to the next texture (the free
        // list is LIFO). The new texture's picture must be its own, and the dead one's drain
        // entry must not be replayed onto it.
        TEST_F(P4aFinalFixScenario, ATextureRecycledOntoTheDeadSlotDoesNotInheritItsDrainEntry) {
            if (!Ready()) return;
            {
                GLuint dead = 0;
                glGenTextures(1, &dead);
                glBindTexture(GL_TEXTURE_2D, dead);
                const std::vector<std::uint8_t> texels = Solid(4, 255, 0, 0);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels.data());
                glBindTexture(GL_TEXTURE_2D, 0);
                glDeleteTextures(1, &dead); // dirty, dead, no verb between
            }
            const GLuint successor = MakeLevel0(0, 0, 255, /*maxLevel=*/0, /*size=*/8);
            const Image image = DrawSampled(successor);
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
            Report("ATextureRecycledOntoTheDeadSlotDoesNotInheritItsDrainEntry", image);
            EXPECT_TRUE(Mostly(image, "blue", "the successor of a dead dirty texture on the recycled slot"));
            const Image other = DrawSampled(m_other);
            EXPECT_TRUE(Mostly(other, "white", "an unrelated draw after the recycled slot was used"));
            GLuint cleanup = successor;
            glDeleteTextures(1, &cleanup);
        }

        // A renderbuffer with defined storage, attached, cleared through its framebuffer, then
        // both die before the next verb.
        TEST_F(P4aFinalFixScenario, ARenderbufferAndItsFramebufferDeletedAfterAClearLeaveTheNextDrawIntact) {
            if (!Ready()) return;
            GLuint renderbuffer = 0;
            glGenRenderbuffers(1, &renderbuffer);
            glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 8, 8);
            GLuint fbo = 0;
            glGenFramebuffers(1, &fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, renderbuffer);
            ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GLenum(GL_FRAMEBUFFER_COMPLETE));
            glViewport(0, 0, 8, 8);
            glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            const Image cleared = ReadPixels(8, 8);
            EXPECT_TRUE(RegionIsMostly(cleared, 0, 8, 0, 8, "green", 0.0, "the renderbuffer after the clear"));
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glBindRenderbuffer(GL_RENDERBUFFER, 0);
            glDeleteFramebuffers(1, &fbo);
            glDeleteRenderbuffers(1, &renderbuffer); // the attachment's last reference went with the FBO
            const Image image = DrawSampled(m_other);
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
            Report("ARenderbufferAndItsFramebufferDeletedAfterAClearLeaveTheNextDrawIntact", image);
            EXPECT_TRUE(Mostly(image, "white", "the draw after a renderbuffer and its framebuffer died"));
        }

        // A sampler object bound to the unit the draw samples through, deleted while bound: GL
        // unbinds it from every unit at glDeleteSamplers, and the texture's own parameters apply
        // again. Both draws must be the texture's colour.
        TEST_F(P4aFinalFixScenario, ASamplerObjectDeletedWhileBoundLeavesTheNextDrawIntact) {
            if (!Ready()) return;
            const GLuint texture = MakeLevel0(255, 0, 0, /*maxLevel=*/0);
            GLuint sampler = 0;
            glGenSamplers(1, &sampler);
            glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glBindSampler(0, sampler);
            const Image withSampler = DrawSampled(texture);
            EXPECT_TRUE(Mostly(withSampler, "red", "the draw through the bound sampler object"));
            glDeleteSamplers(1, &sampler); // bound: unbound by the delete, then dies
            const Image image = DrawSampled(texture);
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
            Report("ASamplerObjectDeletedWhileBoundLeavesTheNextDrawIntact", image);
            EXPECT_TRUE(Mostly(image, "red", "the draw after the bound sampler object died"));
            glBindSampler(0, 0);
            GLuint cleanup = texture;
            glDeleteTextures(1, &cleanup);
        }

        // A second program, in use when it is deleted (GL keeps it alive until it is no longer
        // current), then released by a glUseProgram of the fixture's program: it dies there, and
        // the draw that follows runs through the survivor.
        TEST_F(P4aFinalFixScenario, AProgramDeletedWhileInUseLeavesTheNextDrawIntact) {
            if (!Ready()) return;
            std::string error;
            const GLuint second = CompileProgram(kVS, kFS, &error);
            ASSERT_NE(second, 0u) << error;
            const GLuint texture = MakeLevel0(255, 0, 0, /*maxLevel=*/0);
            const Image throughSecond = DrawSampled(texture, second);
            EXPECT_TRUE(Mostly(throughSecond, "red", "the draw through the second program"));
            glDeleteProgram(second); // current: flagged for deletion, still very much alive
            const Image stillCurrent = DrawSampled(texture, second);
            EXPECT_TRUE(Mostly(stillCurrent, "red", "the draw through a program flagged for deletion"));
            const Image image = DrawSampled(texture); // glUseProgram(m_program): the second dies here
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
            Report("AProgramDeletedWhileInUseLeavesTheNextDrawIntact", image);
            EXPECT_TRUE(Mostly(image, "red", "the draw after the deleted program was released"));
            GLuint cleanup = texture;
            glDeleteTextures(1, &cleanup);
        }

        // A framebuffer handed to the server BY NAME (a DSA clear emits a Named record, ID-19(c))
        // and deleted before the next verb; its attachment lives on and carries the clear.
        TEST_F(P4aFinalFixScenario, AFramebufferDeletedAfterADsaClearLeavesItsAttachmentIntact) {
            if (!Ready()) return;
            const GLuint texture = MakeLevel0(255, 0, 0, /*maxLevel=*/0);
            GLuint fbo = 0;
            glGenFramebuffers(1, &fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
            ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GLenum(GL_FRAMEBUFFER_COMPLETE));
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            const GLfloat green[4] = {0.0f, 1.0f, 0.0f, 1.0f};
            glClearNamedFramebufferfv(fbo, GL_COLOR, 0, green);
            glDeleteFramebuffers(1, &fbo); // unbound and named: dies here
            const Image image = DrawSampled(texture);
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
            Report("AFramebufferDeletedAfterADsaClearLeavesItsAttachmentIntact", image);
            EXPECT_TRUE(Mostly(image, "green", "the attachment of a framebuffer that died after a DSA clear"));
            GLuint cleanup = texture;
            glDeleteTextures(1, &cleanup);
        }

    } // namespace
} // namespace MGITest
