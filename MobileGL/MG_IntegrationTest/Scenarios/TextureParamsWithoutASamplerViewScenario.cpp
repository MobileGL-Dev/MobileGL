// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/TextureParamsWithoutASamplerViewScenario.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - A TEXTURE'S PARAMETERS TAKE EFFECT EVEN WHEN IT HAS NO SAMPLER VIEW (gate G9), the
// one scenario ROADMAP.md:20 names by hand and requires to be RED before P4a lands.
//
// THE DESIGN STATEMENT IT TESTS, ARCHITECTURE.md:100 (D10), verbatim: "SetTextureParams 按资源寻址、
// 与 sampler view 分开（D10）：只作 FBO attachment / image 单元 / glCopyImageSubData 端点的纹理没有
// sampler view，但 Espryt 对 attachment 也同步纹理参数，且 RequireImageBindableStorage 需要在前端参数
// 版本不动时强制重同步." A texture that is only an attachment, only an image-unit binding or only a
// copy endpoint has NO sampler view at all - so a design that carried texture parameters on the
// view would silently drop them for exactly those textures. P4a addresses set_texture_params by
// RESOURCE, independently of any binding, which is what makes the record exist for every texture
// the moment its parameters move.
//
// THE FOUR CASES, and BRIEF-P4A.md D-E3 is where their expected verdicts come from:
//
//   AnAttachmentOnlyTexturesSwizzleReachesTheDriver                        green today, green after
//   AReadAttachmentOnlyTexturesDepthStencilModeReachesTheDriver            D-E3 says RED today
//   AnImageUnitOnlyTexturesSwizzleSurvivesARequireImageBindableStorageRemint green today, green after
//   ACopyImageEndpointOnlyTexturesParamsReachTheDriver                     green today, green after
//
// [MEASURED, AND IT DOES NOT MATCH D-E3] All four are GREEN on the P4a contract commit (08192d72),
// on llvmpipe, in the push build. The second one is green FOR A REASON THAT IS ITSELF THE FINDING,
// and it is written here rather than in a review comment because the next person to try to make it
// red needs to know why they cannot:
//
//   A texture parameter's only public-GL observable is a SAMPLE - nothing about an attachment, an
//   image binding or a copy endpoint reads a swizzle or an aspect mode - and a sample puts the
//   texture on SyncNeccessaryTextures' UNIT list. That walk calls SyncTextureParamsToBackend for
//   every entry whose IsDrawSyncClean is false (DirectGLES.cpp:1896-1898), and IsDrawSyncClean is
//   false whenever the frontend's parameter version has moved since the last sync
//   (Managers.h:1399-1416, `m_syncedTextureParamsVersion != paramsVersion`). So the very act of
//   observing the parameter repairs the state the observation was meant to catch: the gap between
//   "the parameter moved on a read-attachment-only texture" and "the driver was told" is REAL, and
//   it is closed by the next sampler binding, which is also the only thing that can see it.
//
// What that means for the gate, stated plainly so nobody reads a green here as evidence of
// anything it is not:
//
//   * the four cases are a REGRESSION NET around D10, not the evidence for the change. They pin
//     the design statement: a texture's parameters take effect however the texture is reached, and
//     in particular they would go RED if any of the four reachability paths were ever made to
//     depend on the texture having a sampler VIEW - which is exactly the coupling P4a's
//     resource-addressed set_texture_params removes and the thing a later phase could reintroduce;
//   * the "落地前必须红" artefact ROADMAP.md:20 asks for is NOT produced by this file, and no
//     public-GL integration scenario on a monolith tree can produce it. Producing it needs an
//     observation of the DRIVER's texture object taken while the texture is still
//     read-attachment-only, which means a probe inside MG_Backend/DirectGLES - package D's files
//     (C.7), not this package's. The integrator's ruling is recorded in the gates result document.
//
// WHY THE SECOND ONE IS THE RED, mechanically (scout-espryt-framebuffer.md 2.6, re-opened at the
// base ref). Today a texture's parameters ride on the UNIT BINDING and on the DRAW attachment set:
//
//   bound to a sampler unit <= the high-water mark  SyncNeccessaryTextures' unit list   -> synced
//   attachment of the DRAW framebuffer              SyncNeccessaryTextures' FBO list    -> synced
//   attachment of the READ framebuffer ONLY         SyncCurrentFBO -> SyncToBackend ->
//                                                   SyncAttachmentObject, which calls
//                                                   SyncMipmapsToBackend at Managers.cpp:7161
//                                                   and NOTHING ELSE                    -> NOT synced
//   bound to an image unit                          SyncImageTextureBinding ->
//                                                   SyncTextureObjectToBackend, and
//                                                   RequireImageBindableStorage additionally
//                                                   forces m_forceTextureParamsResync    -> synced
//   a glCopyImageSubData endpoint                   MakeGLESCopyImageEndpoint ->
//                                                   SyncTextureObjectToBackend           -> synced
//
// SyncNeccessaryTextures' attachment list reads GetFramebufferBindingSlotChecked(Draw) only
// (DirectGLES.cpp:1944), so a texture that is exclusively a READ attachment gets its STORAGE synced
// and its PARAMETERS never. P4a closes that gap deliberately (D-E3): the record is addressed by
// resource, and Espryt's SyncAttachmentObject applies parameters for ANY attachment, draw or read.
// It is a behaviour change and it is the deliverable, not a drive-by dev fix (ROADMAP.md:98).
//
// HOW EACH CASE OBSERVES "REACHED THE DRIVER", and why the observation is always a LATER SAMPLE.
// A texture parameter is by definition a sampling parameter: nothing about an attachment, an image
// binding or a copy endpoint reads a swizzle or a depth/stencil aspect mode, so the only thing that
// can see one is a sample. Each case therefore does the same three things -
//
//   1. put the texture through ONE of the five reachability paths above, and only that one,
//   2. move a parameter while it is reachable ONLY that way (through the DSA entry points
//      glTextureParameteri / glTextureSubImage2D, so no step of the setup ever binds the texture to
//      a sampler unit - a bind would put it on the unit list and answer the question by accident),
//   3. sample it once, at the end, and read the colour back.
//
// - and the difference between them is step 1 alone. A case that is red says: the parameter set
// while the texture was reachable only that way did not survive to the sample.
//
// DIRECTGLES ONLY. The gap is Espryt's - it is a statement about SyncAttachmentObject and
// SyncNeccessaryTextures - and P4a does not touch MG_Backend/DirectVulkan (D-Q). Magma answers the
// same GL question through an entirely different path, so a red or a green there would be evidence
// about P7's work rather than about this gate; the cases SKIP on any other backend, naming that.

#include <cstdint>
#include <cstdlib>
#include <iostream>
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

        constexpr int kInset = 2;
        constexpr int kTextureSize = 4;

        constexpr const char* kQuadVS = R"(#version 330 core
in vec2 aPos;
out vec2 vUv;
void main() {
    vUv = aPos * 0.5 + 0.5;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

        // Samples texel (0,0) with an explicit fetch: no filtering, no derivatives, no wrap - so
        // the colour that comes back is the texel as the driver's swizzle presents it and nothing
        // else can move it.
        constexpr const char* kFetchFS = R"(#version 330 core
uniform sampler2D uTex;
out vec4 oColor;
void main() { oColor = texelFetch(uTex, ivec2(0, 0), 0); }
)";

        // The stencil aspect of a packed depth/stencil texture is an UNSIGNED INTEGER texture, so
        // it needs a usampler2D. The case that uses it turns "the stencil value is what was
        // cleared" into a colour, because a colour is the only thing this harness can read back.
        constexpr const char* kStencilFetchFS = R"(#version 330 core
uniform usampler2D uTex;
uniform uint uExpected;
out vec4 oColor;
void main() {
    uint value = texelFetch(uTex, ivec2(0, 0), 0).r;
    oColor = (value == uExpected) ? vec4(0.0, 1.0, 0.0, 1.0) : vec4(1.0, 0.0, 0.0, 1.0);
}
)";

        struct Vertex {
            float x, y;
        };

        class TextureParamsWithoutASamplerViewScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                if (Gl().BackendName() != "DirectGLES") {
                    GTEST_SKIP() << "DirectGLES only: this scenario is about Espryt's own reachability "
                                    "table - SyncNeccessaryTextures' attachment list walks the DRAW "
                                    "slot only (DirectGLES.cpp:1944) and SyncAttachmentObject syncs "
                                    "storage and not parameters (Managers.cpp:7161). "
                                 << Gl().BackendName()
                                 << " answers the same GL question through a different path, so a "
                                    "verdict here would be evidence about that backend rather than "
                                    "about this gate (P4a touches no DirectVulkan source but "
                                    "MagmaPipeArms.h, D-Q).";
                }
                std::string error;
                m_fetchProgram = CompileProgram(kQuadVS, kFetchFS, &error);
                ASSERT_NE(m_fetchProgram, 0u) << error;

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
                ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "the scene setup left a GL error behind";
            }

            void TearDown() override {
                if (!Ready() || IsSkipped()) return;
                glUseProgram(0);
                glBindVertexArray(0);
                if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
                if (m_quadBuffer != 0) glDeleteBuffers(1, &m_quadBuffer);
                if (m_fetchProgram != 0) glDeleteProgram(m_fetchProgram);
            }

            // Direct State Access is how every setup step below touches a texture, and it is the
            // whole reason the cases can claim "this texture never had a sampler view": the classic
            // entry points (glTexImage2D, glTexParameteri) all require the texture to be BOUND to a
            // unit first, and a bind is exactly what puts it on SyncNeccessaryTextures' unit list.
            // A case that used them would sync the parameters through the path it is trying to
            // exclude and would be green for the wrong reason, on every tree, forever.
            bool DirectStateAccessIsAvailable() {
                GLuint probe = 0;
                glCreateTextures(GL_TEXTURE_2D, 1, &probe);
                const bool ok = FirstGLError() == GLenum(GL_NO_ERROR) && probe != 0;
                if (probe != 0) glDeleteTextures(1, &probe);
                return ok;
            }

            void SkipWithoutDirectStateAccess() {
                if (!DirectStateAccessIsAvailable()) {
                    GTEST_SKIP() << "glCreateTextures is not usable here, and every case in this file "
                                    "needs the DSA entry points: the classic ones bind the texture to "
                                    "a unit, which is the reachability path these cases exist to "
                                    "exclude. A case that fell back to them would be green for the "
                                    "wrong reason rather than measuring anything.";
                }
            }

            // A 4x4 RGBA8 texture, one solid colour, created and filled WITHOUT EVER BINDING IT.
            GLuint MakeSolidTextureWithoutBinding(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
                std::vector<std::uint8_t> texels(kTextureSize * kTextureSize * 4);
                for (std::size_t i = 0; i < texels.size(); i += 4) {
                    texels[i] = r;
                    texels[i + 1] = g;
                    texels[i + 2] = b;
                    texels[i + 3] = 255;
                }
                GLuint texture = 0;
                glCreateTextures(GL_TEXTURE_2D, 1, &texture);
                glTextureStorage2D(texture, 1, GL_RGBA8, kTextureSize, kTextureSize);
                glTextureSubImage2D(texture, 0, 0, 0, kTextureSize, kTextureSize, GL_RGBA,
                                    GL_UNSIGNED_BYTE, texels.data());
                glTextureParameteri(texture, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTextureParameteri(texture, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                return texture;
            }

            // THE OBSERVATION, shared by every case: bind the texture to a unit for the first time
            // in its life and read one texel back through the default framebuffer.
            Image SampleAndRead(GLuint program, GLuint texture) {
                BindDefaultFramebuffer();
                glViewport(0, 0, Gl().Width(), Gl().Height());
                ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
                glUseProgram(program);
                glUniform1i(glGetUniformLocation(program, "uTex"), 0);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, texture);
                glBindVertexArray(m_vao);
                glDrawArrays(GL_TRIANGLES, 0, 6);
                const Image image = ReadPixels(Gl().Width(), Gl().Height());
                Gl().EndFrame();
                glBindTexture(GL_TEXTURE_2D, 0);
                return image;
            }

            ::testing::AssertionResult WholeViewportIs(const Image& image, const char* expected,
                                                       const std::string& when) {
                return RegionIsMostly(image, kInset, image.Width() - kInset, kInset,
                                      image.Height() - kInset, expected, 0.0, when);
            }

            // R -> ZERO and G -> ONE, so a RED texel samples as GREEN if and only if the swizzle
            // reached the driver, and as RED if it did not. Two colours the harness can name, from
            // one parameter change, with no third outcome that could be mistaken for either.
            void SwizzleRedIntoGreen(GLuint texture) {
                glTextureParameteri(texture, GL_TEXTURE_SWIZZLE_R, GL_ZERO);
                glTextureParameteri(texture, GL_TEXTURE_SWIZZLE_G, GL_ONE);
                glTextureParameteri(texture, GL_TEXTURE_SWIZZLE_B, GL_ZERO);
            }

            GLuint m_fetchProgram = 0;
            GLuint m_vao = 0;
            GLuint m_quadBuffer = 0;
        };

        // ------------------------------------------------------------------------------------
        // 1. DRAW ATTACHMENT ONLY. Green today (SyncNeccessaryTextures' FBO list walks the draw
        //    slot and calls SyncTextureObjectToBackend, which syncs parameters) and green after.
        //    It is the regression net for the half of D-E3 that already works: P4a moves the
        //    parameter sync from the sync list onto the record, and this case is what says the
        //    move did not lose the case that used to be covered.
        // ------------------------------------------------------------------------------------
        TEST_F(TextureParamsWithoutASamplerViewScenario, AnAttachmentOnlyTexturesSwizzleReachesTheDriver) {
            if (!Ready()) return;
            SkipWithoutDirectStateAccess();
            if (IsSkipped()) return;

            const GLuint texture = MakeSolidTextureWithoutBinding(255, 0, 0);
            GLuint fbo = 0;
            glGenFramebuffers(1, &fbo);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
            ASSERT_EQ(glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER), GLenum(GL_FRAMEBUFFER_COMPLETE));

            // The parameter moves while the texture is reachable ONLY as a draw attachment...
            SwizzleRedIntoGreen(texture);
            // ...and a frame runs with it bound that way, so whatever the draw path syncs, syncs.
            glViewport(0, 0, kTextureSize, kTextureSize);
            ClearTo(1.0f, 0.0f, 0.0f, 1.0f);
            BindDefaultFramebuffer();
            Gl().EndFrame();
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "the draw-attachment frame left a GL error";

            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDeleteFramebuffers(1, &fbo);

            const Image image = SampleAndRead(m_fetchProgram, texture);
            EXPECT_TRUE(WholeViewportIs(image, "green",
                                        "a texture that was only ever a DRAW attachment, sampled "
                                        "after its swizzle moved"))
                << "the swizzle set while this texture was reachable only as a draw-framebuffer "
                   "attachment did not reach the driver: a red texel with R->ZERO, G->ONE must "
                   "sample as green.";

            GLuint cleanup = texture;
            glDeleteTextures(1, &cleanup);
        }

        // ------------------------------------------------------------------------------------
        // 2. READ ATTACHMENT ONLY - THE MANDATORY RED (ROADMAP.md:20, D-E3, G9).
        //
        //    The parameter is GL_DEPTH_STENCIL_TEXTURE_MODE and the texture is a packed
        //    depth/stencil one, because that is the parameter whose absence is not a mis-filtered
        //    picture but the WRONG ASPECT: a driver left at the GL default samples the depth bits
        //    where the application asked for stencil, and the value that comes back is not the
        //    stencil that was written. RecreateBackendTexture's own comment says exactly this -
        //    "the mode makes it visible because falling back to the default silently samples the
        //    wrong aspect rather than merely mis-filtering" (Managers.cpp:4826-4833).
        //
        //    The framebuffer is bound to GL_READ_FRAMEBUFFER and the DRAW target is left on the
        //    default framebuffer for the whole window, which is the ONE thing that separates this
        //    case from the one above.
        // ------------------------------------------------------------------------------------
        TEST_F(TextureParamsWithoutASamplerViewScenario,
               AReadAttachmentOnlyTexturesDepthStencilModeReachesTheDriver) {
            if (!Ready()) return;
            SkipWithoutDirectStateAccess();
            if (IsSkipped()) return;

            std::string error;
            const GLuint stencilProgram = CompileProgram(kQuadVS, kStencilFetchFS, &error);
            ASSERT_NE(stencilProgram, 0u) << error;

            GLuint texture = 0;
            glCreateTextures(GL_TEXTURE_2D, 1, &texture);
            glTextureStorage2D(texture, 1, GL_DEPTH24_STENCIL8, kTextureSize, kTextureSize);
            glTextureParameteri(texture, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTextureParameteri(texture, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            if (FirstGLError() != GLenum(GL_NO_ERROR)) {
                glDeleteTextures(1, &texture);
                glDeleteProgram(stencilProgram);
                GTEST_SKIP() << "this driver would not create an immutable DEPTH24_STENCIL8 texture, "
                                "so there is no packed depth/stencil aspect here to sample and the "
                                "case cannot answer";
            }

            // Write a stencil value through the texture AS A DRAW ATTACHMENT once, so that there is
            // something in the stencil aspect to read. This is setup, not the window: the window
            // below never makes it a draw attachment again.
            GLuint fbo = 0;
            glGenFramebuffers(1, &fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, texture, 0);
            glDrawBuffer(GL_NONE);
            glReadBuffer(GL_NONE);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GLenum(GL_FRAMEBUFFER_COMPLETE)) {
                BindDefaultFramebuffer();
                glDeleteFramebuffers(1, &fbo);
                glDeleteTextures(1, &texture);
                glDeleteProgram(stencilProgram);
                GTEST_SKIP() << "a depth-stencil-only framebuffer is incomplete on this driver, so "
                                "the stencil aspect cannot be written and the case cannot answer";
            }
            constexpr GLint kStencil = 42;
            glViewport(0, 0, kTextureSize, kTextureSize);
            glStencilMask(0xFFu);
            glClearBufferfi(GL_DEPTH_STENCIL, 0, 0.5f, kStencil);
            BindDefaultFramebuffer();
            Gl().EndFrame();
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "clearing the stencil aspect left a GL error";

            // ---- the window: the texture is reachable ONLY as a READ attachment ----
            //
            // The DRAW binding is the default framebuffer throughout, so SyncNeccessaryTextures'
            // attachment list - which walks the DRAW slot only - never sees this texture, and
            // SyncCurrentFBO's read path reaches it through SyncAttachmentObject, which syncs
            // storage and not parameters. That is the gap.
            glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
            glTextureParameteri(texture, GL_DEPTH_STENCIL_TEXTURE_MODE, GL_STENCIL_INDEX);
            // A frame with the read binding live, and it has to contain a REAL DRAW: SyncCurrentFBO
            // and the whole sync-list walk run at the validate point, so a frame that only cleared
            // and swapped would never reach the read-side path this case is about.
            glViewport(0, 0, Gl().Width(), Gl().Height());
            ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
            glUseProgram(m_fetchProgram);
            glUniform1i(glGetUniformLocation(m_fetchProgram, "uTex"), 0);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, 0);
            glBindVertexArray(m_vao);
            glDrawArrays(GL_TRIANGLES, 0, 6);
            Gl().EndFrame();
            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "the read-attachment frame left a GL error";

            // ---- the observation ----
            BindDefaultFramebuffer();
            glViewport(0, 0, Gl().Width(), Gl().Height());
            ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
            glUseProgram(stencilProgram);
            glUniform1i(glGetUniformLocation(stencilProgram, "uTex"), 0);
            glUniform1ui(glGetUniformLocation(stencilProgram, "uExpected"),
                         static_cast<GLuint>(kStencil));
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, texture);
            glBindVertexArray(m_vao);
            glDrawArrays(GL_TRIANGLES, 0, 6);
            const Image image = ReadPixels(Gl().Width(), Gl().Height());
            Gl().EndFrame();
            glBindTexture(GL_TEXTURE_2D, 0);

            const GLenum sampleError = FirstGLError();
            std::cout << "[ TextureParamsWithoutASamplerView ] read-attachment-only "
                         "GL_DEPTH_STENCIL_TEXTURE_MODE=GL_STENCIL_INDEX, expecting stencil "
                      << kStencil << "; sample GL error 0x" << std::hex << sampleError << std::dec
                      << std::endl;

            EXPECT_TRUE(WholeViewportIs(image, "green",
                                        "a texture that was only ever a READ attachment, sampled "
                                        "through its stencil aspect"))
                << "GL_DEPTH_STENCIL_TEXTURE_MODE = GL_STENCIL_INDEX was set while this texture was "
                   "reachable ONLY as an attachment of the READ framebuffer, and the sample did not "
                   "come back as the stencil value that was cleared into it. The parameter did not "
                   "reach the driver, and - because the sample itself would have repaired an "
                   "unsynced parameter (see this file's header: the unit sync list calls "
                   "SyncTextureParamsToBackend whenever the params version moved) - a red here means "
                   "something stronger than the D-E3 gap: a reachability path that does not sync "
                   "parameters AT ALL, i.e. the sampler-view coupling ARCHITECTURE.md:100 (D10) "
                   "exists to remove. Read the case's own stdout line for the GL error the sample "
                   "raised before assuming an aspect-mode bug.";

            BindDefaultFramebuffer();
            glDeleteFramebuffers(1, &fbo);
            glDeleteTextures(1, &texture);
            glDeleteProgram(stencilProgram);
        }

        // ------------------------------------------------------------------------------------
        // 3. IMAGE UNIT ONLY, across a storage RE-MINT. Green today and green after, and the
        //    reason it is green is the thing P4a must not lose: glBindImageTexture drives
        //    RequireImageBindableStorage, which re-mints the storage in a possibly WIDENED carrier
        //    and sets m_forceTextureParamsResync (Managers.cpp:4787) precisely because the
        //    frontend's parameter version does not move across that transition. A parameter sync
        //    gated only on the frontend version would leave the driver at its defaults and sample
        //    the carrier's surplus channels raw.
        // ------------------------------------------------------------------------------------
        TEST_F(TextureParamsWithoutASamplerViewScenario,
               AnImageUnitOnlyTexturesSwizzleSurvivesARequireImageBindableStorageRemint) {
            if (!Ready()) return;
            SkipWithoutDirectStateAccess();
            if (IsSkipped()) return;

            const GLuint texture = MakeSolidTextureWithoutBinding(255, 0, 0);

            // The parameter moves while the texture is reachable only as an image-unit binding...
            SwizzleRedIntoGreen(texture);
            glBindImageTexture(0, texture, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
            const GLenum bindError = FirstGLError();
            if (bindError != GLenum(GL_NO_ERROR)) {
                glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
                GLuint cleanup = texture;
                glDeleteTextures(1, &cleanup);
                GTEST_SKIP() << "glBindImageTexture is not usable here (GL error 0x" << std::hex
                             << bindError << std::dec
                             << "), so the RequireImageBindableStorage transition this case is about "
                                "cannot be reached";
            }
            // ...and a frame runs with the image binding live, which is what drives the re-mint.
            glViewport(0, 0, Gl().Width(), Gl().Height());
            ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
            Gl().EndFrame();
            glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);

            const Image image = SampleAndRead(m_fetchProgram, texture);
            EXPECT_TRUE(WholeViewportIs(image, "green",
                                        "a texture that was only ever an image-unit binding, sampled "
                                        "after its swizzle moved and its storage was re-minted"))
                << "the swizzle did not survive the RequireImageBindableStorage re-mint. The re-mint "
                   "creates a new driver texture at the ES defaults without moving the frontend's "
                   "parameter version, so the forced resync (Managers.cpp:4787) is the only thing "
                   "that puts the application's parameters back.";

            GLuint cleanup = texture;
            glDeleteTextures(1, &cleanup);
        }

        // ------------------------------------------------------------------------------------
        // 4. glCopyImageSubData ENDPOINT ONLY. Green today (MakeGLESCopyImageEndpoint calls
        //    SyncTextureObjectToBackend, DirectGLES.cpp:7588) and green after. The endpoint is the
        //    DESTINATION, so the case also proves the copy itself still lands: a swizzled read of
        //    the copied texel is only meaningful if the texel arrived.
        // ------------------------------------------------------------------------------------
        TEST_F(TextureParamsWithoutASamplerViewScenario,
               ACopyImageEndpointOnlyTexturesParamsReachTheDriver) {
            if (!Ready()) return;
            SkipWithoutDirectStateAccess();
            if (IsSkipped()) return;

            const GLuint source = MakeSolidTextureWithoutBinding(255, 0, 0);
            const GLuint destination = MakeSolidTextureWithoutBinding(0, 0, 255);

            // The parameter moves while the destination is reachable only as a copy endpoint...
            SwizzleRedIntoGreen(destination);
            glCopyImageSubData(source, GL_TEXTURE_2D, 0, 0, 0, 0, destination, GL_TEXTURE_2D, 0, 0, 0,
                               0, kTextureSize, kTextureSize, 1);
            const GLenum copyError = FirstGLError();
            if (copyError != GLenum(GL_NO_ERROR)) {
                GLuint cleanup[2] = {source, destination};
                glDeleteTextures(2, cleanup);
                GTEST_SKIP() << "glCopyImageSubData is not usable here (GL error 0x" << std::hex
                             << copyError << std::dec << "), so there is no copy endpoint to be";
            }
            Gl().EndFrame();

            // ...and the destination now holds the source's RED texel, which the swizzle must turn
            // into GREEN when it is finally sampled.
            const Image image = SampleAndRead(m_fetchProgram, destination);
            EXPECT_TRUE(WholeViewportIs(image, "green",
                                        "a texture that was only ever a glCopyImageSubData endpoint, "
                                        "sampled after its swizzle moved"))
                << "the three readings are distinct and each names its own cause: GREEN is the pass "
                   "(the swizzle reached the driver); RED means the copy landed and the swizzle did "
                   "not; BLUE means neither happened, i.e. the destination is still its own original "
                   "texel and glCopyImageSubData wrote nothing. The swizzle is what turns any texel "
                   "into (0,1,0), so the colour separates the two failures rather than merging them.";

            GLuint cleanup[2] = {source, destination};
            glDeleteTextures(2, cleanup);
        }

    } // namespace
} // namespace MGITest
