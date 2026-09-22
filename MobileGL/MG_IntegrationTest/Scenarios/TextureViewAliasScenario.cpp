// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/TextureViewAliasScenario.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - THE OWNER-KEYED EMISSION CURSOR (P3b/P4b, wave 2-D; ROADMAP's "dirty 归属反转 -
// 按存储属主键控的发射游标"). THE VERIFICATION HALF.
//
// THE CODE HALF ALREADY LANDED and this file is the gate it never had:
//
//   * the drain cursor is keyed on the STORAGE OWNER - MG_Impl/Pipe/TextureEmit.h's
//     NoteLevelDirty acquires a handle for whatever ITextureObject& reaches it, and what
//     reaches it is the owner because TextureObjectView forwards MarkStorageDirty /
//     MarkStorageDirtyRegion to the owner's methods (TextureObjectView.cpp:314-329);
//   * the index remap is TextureObjectView::ToOwnerLevel / ToOwnerUploadTarget /
//     ToOwnerRegionOffset (TextureObjectView.h:105-132), so a write through a level- or
//     layer-restricted view names the OWNER's level and the OWNER's layer;
//   * the wire half is MGPSamplerView + MGPResourceDesc::ViewOf (P5e tx2).
//
// WHAT GOES WRONG WITHOUT IT, which is why a scenario is owed rather than a unit case. A view
// and its owner share ONE set of dirty flags (the view owns no MipmapStorage at all) but would
// carry TWO emission cursors if the drain were keyed on the texture the application named.
// Whichever cursor drained first would clear the flag the other still needed, or both would
// ship the same texels twice. Neither failure is visible in the next frame's picture on most
// trees: the level that lost its record is uploaded again by the NEXT whole-level respecify,
// by a glGenerateMipmap, or by the monolith sync path, so the image is right one frame late.
// That is exactly the shape SSIM and every ordinary pixel case go green over.
//
// SO THE CLAIM IS ASSERTED ON BOTH CHANNELS AT ONCE, and neither alone is the gate
// (notes/recovered/wf2/final/part3.md 7.3, "门：新增场景，通过 view 上传、经属主采样（以及反
// 向），跨 draw 边界各一次"):
//
//   PIXELS     an upload through one name must be visible through the other name, in the SAME
//              frame it was issued - and the layers the view does not address must not move.
//   COUNTERS   MGPipeTextureEmitter::SubDataCount() must have moved across the drain, and
//              DrainListSize() must be back to zero after it. A SILENTLY EMPTY DRAIN - the
//              two-cursor regression's actual signature - reads as a perfectly healthy run
//              from the pixel side alone, because the repair paths above hide it.
//
// THE FOUR CASES are the two directions, each in a one-drain and a two-drain shape:
//   (a) upload through a layer-restricted VIEW, sample through the OWNER;
//   (b) upload through the OWNER, sample through the VIEW;
//   (c) (a) across a draw boundary - draw, upload, draw, upload, draw - so the drain runs
//       three times and the counters are read at each one;
//   (d) (b) across the same boundary.
//
// THE RED-ONCE (R-16) this file was written against: make TextureObjectView::
// MarkStorageDirtyRegion call PipeNoteLevelDirty on ITSELF instead of forwarding to the owner -
// i.e. build the two-cursor shape the design rejects. Case (a) then goes red on stale OWNER
// texels AND on a SubDataCount that did not move, which is the pair this file exists to make
// simultaneous. Recorded in docs/Disaggregated/notes/p34b/espryt-d2.md.
//
// VIEWS ARE OPT-IN ON ESPRYT. DirectGLES withholds GL_ARB_texture_view unless
// MOBILEGL_ESPRYT_ENABLE_TEXTURE_VIEW=1 (ConfigLoader.cpp:213, Config.h:72), so every lane that
// runs this file sets it; a lane that does not green-skips through TextureViewUsable() rather
// than failing, the same shape TextureViewScenario uses.

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "../Harness/HeadlessGL.h"
#include "../Harness/ScenarioFixture.h"
#include "../Harness/TextureEmitPeek.h"

#ifdef GLAPI
#undef GLAPI
#endif
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glcorearb.h>
#undef GL_GLEXT_PROTOTYPES

namespace MGITest {
    namespace {

        constexpr int kSize = 64;
        constexpr int kLayers = 4;
        // Deliberately NOT 0: a view whose layer origin was dropped lands on layer 0, and the
        // per-layer seeding below makes that land on a layer this file also reads back.
        constexpr int kViewLayer = 2;
        // The quad covers the whole surface; the inset keeps the assertions off the primitive
        // edge, which is HeadlessGL.h's standing rule for a full-surface draw.
        constexpr int kInset = 2;

        constexpr const char* kVS = R"(#version 330 core
in vec2 aPos;
out vec2 vUv;
void main() {
    vUv = aPos * 0.5 + 0.5;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

        // The VIEW's sampler: an ordinary 2D name over one of the owner's layers.
        constexpr const char* kFS2D = R"(#version 330 core
in vec2 vUv;
uniform sampler2D uTex;
out vec4 oColor;
void main() { oColor = texture(uTex, vUv); }
)";

        // The OWNER's sampler: the array, with the layer named by a uniform so one program
        // serves every layer read-back below.
        constexpr const char* kFSArray = R"(#version 330 core
in vec2 vUv;
uniform sampler2DArray uTex;
uniform int uLayer;
out vec4 oColor;
void main() { oColor = texture(uTex, vec3(vUv, float(uLayer))); }
)";

        struct Vertex {
            float x, y;
        };

        std::string Describe(const Rgba8& c) {
            return "rgba(" + std::to_string(c.r) + "," + std::to_string(c.g) + "," + std::to_string(c.b) +
                   "," + std::to_string(c.a) + ")";
        }

        class TextureViewAliasScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                if (!TextureViewUsable()) {
                    GTEST_SKIP() << "glTextureView is unavailable on backend " << Gl().BackendName()
                                 << " (GL_ARB_texture_view not advertised). On DirectGLES that is "
                                    "the MOBILEGL_ESPRYT_ENABLE_TEXTURE_VIEW=1 opt-in this lane "
                                    "did not set; every lane this scenario is registered in sets it.";
                    return;
                }
                std::string error;
                m_program2D = CompileProgram(kVS, kFS2D, &error);
                ASSERT_NE(m_program2D, 0u) << error;
                m_programArray = CompileProgram(kVS, kFSArray, &error);
                ASSERT_NE(m_programArray, 0u) << error;

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
                glDisable(GL_DEPTH_TEST);
                glDisable(GL_STENCIL_TEST);
                ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "the fixture setup left a GL error behind";
            }

            void TearDown() override {
                if (!Ready()) return;
                glUseProgram(0);
                glBindVertexArray(0);
                glBindTexture(GL_TEXTURE_2D, 0);
                glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
                glBindBuffer(GL_ARRAY_BUFFER, 0);
                for (const GLuint texture : m_textures) {
                    glDeleteTextures(1, &texture);
                }
                m_textures.clear();
                if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
                if (m_quadBuffer != 0) glDeleteBuffers(1, &m_quadBuffer);
                if (m_program2D != 0) glDeleteProgram(m_program2D);
                if (m_programArray != 0) glDeleteProgram(m_programArray);
            }

            // TextureViewScenario's probe, verbatim in intent: a trivial same-format full-range
            // view. A backend that simply does not have the feature skips rather than failing
            // every case in the file.
            bool TextureViewUsable() {
                GLuint storage = 0;
                glGenTextures(1, &storage);
                glBindTexture(GL_TEXTURE_2D, storage);
                glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 1, 1);
                glBindTexture(GL_TEXTURE_2D, 0);
                GLuint view = 0;
                glGenTextures(1, &view);
                while (glGetError() != GL_NO_ERROR) {
                }
                glTextureView(view, GL_TEXTURE_2D, storage, GL_RGBA8, 0, 1, 0, 1);
                const bool usable = glGetError() == GL_NO_ERROR;
                glDeleteTextures(1, &view);
                glDeleteTextures(1, &storage);
                return usable;
            }

            GLuint MakeTexture() {
                GLuint texture = 0;
                glGenTextures(1, &texture);
                m_textures.push_back(texture);
                return texture;
            }

            // An immutable RGBA8 array with NEAREST filtering and one level, seeded per layer by
            // CPU SUB-IMAGE rather than by rendering. Seeding through the GPU would additionally
            // depend on a CPU sub-image reaching a layer whose content the GPU wrote, which is a
            // different question and would make a failure here unattributable - the reason
            // TextureViewScenario.cpp:717-721 gives for the same choice.
            GLuint MakeSeededArray() {
                const GLuint texture = MakeTexture();
                glBindTexture(GL_TEXTURE_2D_ARRAY, texture);
                glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8, kSize, kSize, kLayers);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
                for (int layer = 0; layer < kLayers; ++layer) {
                    UploadLayerThroughOwner(texture, layer, LayerSeed(layer));
                }
                return texture;
            }

            static Rgba8 LayerSeed(int layer) {
                return Rgba8{static_cast<std::uint8_t>(10 + layer * 20),
                             static_cast<std::uint8_t>(200 - layer * 20), 30, 255};
            }

            static std::vector<std::uint8_t> Fill(Rgba8 colour) {
                std::vector<std::uint8_t> texels(static_cast<std::size_t>(kSize) * kSize * 4);
                for (std::size_t i = 0; i < texels.size(); i += 4) {
                    texels[i + 0] = colour.r;
                    texels[i + 1] = colour.g;
                    texels[i + 2] = colour.b;
                    texels[i + 3] = colour.a;
                }
                return texels;
            }

            void UploadLayerThroughOwner(GLuint owner, int layer, Rgba8 colour) {
                const std::vector<std::uint8_t> texels = Fill(colour);
                glBindTexture(GL_TEXTURE_2D_ARRAY, owner);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, layer, kSize, kSize, 1, GL_RGBA,
                                GL_UNSIGNED_BYTE, texels.data());
                glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
            }

            // THE VIEW-SIDE WRITE. A single-layer GL_TEXTURE_2D view of the array: its own level 0
            // is the owner's level <minLevel>, and its own layer 0 is the owner's layer
            // kViewLayer. glTexSubImage2D here is what ToOwnerRegionOffset has to move.
            void UploadThroughView(GLuint view, Rgba8 colour) {
                const std::vector<std::uint8_t> texels = Fill(colour);
                glBindTexture(GL_TEXTURE_2D, view);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kSize, kSize, GL_RGBA, GL_UNSIGNED_BYTE,
                                texels.data());
                glBindTexture(GL_TEXTURE_2D, 0);
            }

            GLuint MakeLayerView(GLuint owner) {
                const GLuint view = MakeTexture();
                glTextureView(view, GL_TEXTURE_2D, owner, GL_RGBA8, 0, 1, kViewLayer, 1);
                EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR))
                    << "2D_ARRAY -> 2D is a legal view pair";
                glBindTexture(GL_TEXTURE_2D, view);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glBindTexture(GL_TEXTURE_2D, 0);
                return view;
            }

            // ---- the two sampling draws. Both go to the default framebuffer, which is what
            // makes them ordinary draws that reach the validate point and run the drain. ----

            Image SampleThroughTheView(GLuint view) {
                BindDefaultFramebuffer();
                glViewport(0, 0, Gl().Width(), Gl().Height());
                glUseProgram(m_program2D);
                glUniform1i(glGetUniformLocation(m_program2D, "uTex"), 0);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, view);
                glBindVertexArray(m_vao);
                glDrawArrays(GL_TRIANGLES, 0, 6);
                return ReadPixels(Gl().Width(), Gl().Height());
            }

            Image SampleThroughTheOwner(GLuint owner, int layer) {
                BindDefaultFramebuffer();
                glViewport(0, 0, Gl().Width(), Gl().Height());
                glUseProgram(m_programArray);
                glUniform1i(glGetUniformLocation(m_programArray, "uTex"), 0);
                glUniform1i(glGetUniformLocation(m_programArray, "uLayer"), layer);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D_ARRAY, owner);
                glBindVertexArray(m_vao);
                glDrawArrays(GL_TRIANGLES, 0, 6);
                return ReadPixels(Gl().Width(), Gl().Height());
            }

            // Whole-region rather than a spot check, for the reason HeadlessGL.h gives: three of
            // four vertices carrying stale data still paints a correct centre pixel.
            void ExpectWholeSurface(const Image& image, Rgba8 expected, const char* what) {
                int offenders = 0;
                Rgba8 firstOffender{};
                int firstX = -1;
                int firstY = -1;
                const int x1 = image.Width() - kInset;
                const int y1 = image.Height() - kInset;
                int total = 0;
                for (int y = kInset; y < y1; ++y) {
                    for (int x = kInset; x < x1; ++x) {
                        ++total;
                        const Rgba8 actual = image.At(x, y);
                        const bool ok = std::abs(int(actual.r) - int(expected.r)) <= 2 &&
                                        std::abs(int(actual.g) - int(expected.g)) <= 2 &&
                                        std::abs(int(actual.b) - int(expected.b)) <= 2 &&
                                        std::abs(int(actual.a) - int(expected.a)) <= 2;
                        if (!ok) {
                            if (offenders == 0) {
                                firstOffender = actual;
                                firstX = x;
                                firstY = y;
                            }
                            ++offenders;
                        }
                    }
                }
                EXPECT_EQ(offenders, 0) << what << ": " << offenders << " of " << total
                                        << " pixels disagree; first at (" << firstX << ", " << firstY
                                        << ") is " << Describe(firstOffender) << ", expected "
                                        << Describe(expected) << " +/- 2";
            }

            // ---- the emitter half -------------------------------------------------------------
            //
            // WHY "THE EMITTER NEVER SAW ANY TEXTURE" IS A SKIP AND "IT SAW TEXTURES BUT EMITTED
            // NO SUB-DATA" IS RED, and the distinction is the whole value of the reading. In a
            // PULL build the emitter does not exist, and in a lane whose MOBILEGL_PIPE_PUSH mask
            // has the texture-resource bit clear (D-K2's rows; the 0x5ff / 0x7ff refusal lanes)
            // it exists but is deliberately not wired - in both the counters are flat and nothing
            // is being claimed. The moment ANY create or respecify has been counted, the emitter
            // is live for this process, and a drain that then put nothing on the wire is the
            // two-cursor regression itself.
            struct EmitWindow {
                bool live = false;
                TextureEmitCountersPeek before{};
                TextureEmitCountersPeek after{};
                unsigned long long SubDataDelta() const { return after.SubDataCount - before.SubDataCount; }
                unsigned long long RefusedDelta() const {
                    return after.RefusedSubDataCount - before.RefusedSubDataCount;
                }
                unsigned long long DescribedDelta() const {
                    return (after.CreateCount - before.CreateCount) +
                           (after.RespecifyCount - before.RespecifyCount);
                }
            };

            bool OpenEmitWindow(EmitWindow& window) {
                window.live = PeekTextureEmitCounters(&window.before);
                return window.live;
            }

            void CloseEmitWindow(EmitWindow& window) {
                if (!window.live) return;
                window.live = PeekTextureEmitCounters(&window.after);
            }

            // The emitter is armed in THIS process if it has ever described a texture to the
            // applier. Checked on the window's `after` reading, because the seeding uploads that
            // arm it happen inside the window.
            bool EmitterIsArmed(const EmitWindow& window) const {
                return window.live && (window.after.CreateCount != 0 || window.after.RespecifyCount != 0);
            }

            void ExpectTheDrainEmitted(const EmitWindow& window, const char* what) {
                if (!window.live) {
                    RecordProperty("emit_counters", TextureEmitPeekSkipReason());
                    return;
                }
                if (!EmitterIsArmed(window)) {
                    // Not "not asserted": a lane where the emitter never described a single
                    // texture cannot have an emission cursor at all, so there is no cursor claim
                    // to make. Recorded so a run where the whole file quietly stopped asserting
                    // is visible in the ctest XML rather than invisible.
                    RecordProperty("emit_cursor", "the client texture emitter described no texture in "
                                                  "this process - the texture-resource subsystem bit "
                                                  "is clear in this lane");
                    return;
                }
                EXPECT_GT(window.SubDataDelta(), 0u)
                    << what << ": the client texture emitter described "
                    << window.DescribedDelta()
                    << " texture(s) across this draw and put NO resource_subdata on the wire. A "
                       "drain that runs and emits nothing is exactly what a per-NAME emission "
                       "cursor produces when a view and its owner each hold one: whichever cursor "
                       "drained first cleared the dirty flag the other still needed. The picture "
                       "can still be right - the next whole-level respecify, a glGenerateMipmap or "
                       "the monolith sync path re-uploads the level one frame later - which is why "
                       "this counter and not the pixels is the load-bearing half here.";
                EXPECT_EQ(window.RefusedDelta(), 0u)
                    << what << ": the applier REFUSED " << window.RefusedDelta()
                    << " of this drain's resource_subdata records. A refusal leaves the level dirty "
                       "and on the drain list (D-D5 step 1), which is the safe direction but not "
                       "the expected one for a glTexSubImage of a texture that has storage.";
                EXPECT_EQ(window.after.DrainListSize, 0u)
                    << what << ": " << window.after.DrainListSize
                    << " (storage owner, upload target, level) key(s) are STILL owed after the "
                       "validate point. A level that bailed - no storage, no shadow, an empty box - "
                       "stays on the list; for this workload every level has immutable storage and "
                       "a non-empty dirty box, so nothing should have bailed.";
            }

            GLuint m_program2D = 0;
            GLuint m_programArray = 0;
            GLuint m_vao = 0;
            GLuint m_quadBuffer = 0;
            std::vector<GLuint> m_textures;
        };

        // ------------------------------------------------------------------------------------
        // (a) UPLOAD THROUGH THE VIEW, SAMPLE THROUGH THE OWNER.
        //
        // The direction that pins ToOwnerRegionOffset: the view's own layer 0 is the owner's
        // layer kViewLayer, so a remap that was dropped marks - and uploads - the owner's layer 0
        // instead, and the two reads below catch it from both sides at once.
        // ------------------------------------------------------------------------------------
        TEST_F(TextureViewAliasScenario, SubImageThroughAViewIsSampledThroughTheOwner) {
            if (!Ready() || IsSkipped()) return;

            constexpr Rgba8 kPainted{255, 0, 255, 255};
            EmitWindow window;
            OpenEmitWindow(window);

            const GLuint owner = MakeSeededArray();
            const GLuint view = MakeLayerView(owner);
            UploadThroughView(view, kPainted);
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "writing through the view raised a GL error";

            const Image throughOwner = SampleThroughTheOwner(owner, kViewLayer);
            CloseEmitWindow(window);
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "the sampling draw raised a GL error";

            ExpectWholeSurface(throughOwner, kPainted,
                               "the owner's layer kViewLayer must carry the texels written through "
                               "the view's own layer 0");
            ExpectTheDrainEmitted(window, "upload through a layer-restricted view");

            // THE NEGATIVE HALF, and it is what separates "the remap was dropped" from "the
            // upload never arrived": a lost layer origin writes the owner's layer 0, which this
            // read would see as an untouched layer that moved.
            const Image layerZero = SampleThroughTheOwner(owner, 0);
            ExpectWholeSurface(layerZero, LayerSeed(0),
                               "the owner's layer 0 is outside the view's window and must not have "
                               "been written");
        }

        // ------------------------------------------------------------------------------------
        // (b) UPLOAD THROUGH THE OWNER, SAMPLE THROUGH THE VIEW.
        //
        // The reverse direction, and the one that fails when the VIEW holds a cursor of its own:
        // the owner's cursor drains and clears the shared flag, and the view's sampler then
        // resolves a resource the record never reached.
        // ------------------------------------------------------------------------------------
        TEST_F(TextureViewAliasScenario, SubImageThroughTheOwnerIsSampledThroughTheView) {
            if (!Ready() || IsSkipped()) return;

            constexpr Rgba8 kPainted{0, 0, 255, 255};
            EmitWindow window;
            OpenEmitWindow(window);

            const GLuint owner = MakeSeededArray();
            const GLuint view = MakeLayerView(owner);
            UploadLayerThroughOwner(owner, kViewLayer, kPainted);
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "writing through the owner raised a GL error";

            const Image throughView = SampleThroughTheView(view);
            CloseEmitWindow(window);
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "the sampling draw raised a GL error";

            ExpectWholeSurface(throughView, kPainted,
                               "the view must read the texels written through the owner's own name "
                               "into the layer the view addresses");
            ExpectTheDrainEmitted(window, "upload through the storage owner");
        }

        // ------------------------------------------------------------------------------------
        // (c) THE SAME, ACROSS DRAW BOUNDARIES, so the drain runs more than once.
        //
        // WHY THE SECOND AND THIRD DRAINS ARE THE POINT. The first drain of a texture's life
        // carries the whole level anyway - the storage definition made it dirty - so a cursor
        // that is keyed on the wrong object still ships the right texels by accident. It is the
        // NEXT drain, where only the sub-region written since the last validate point is owed,
        // that can lose a record and look healthy.
        // ------------------------------------------------------------------------------------
        TEST_F(TextureViewAliasScenario, TheViewUploadDrainRunsAgainAfterADrawBoundary) {
            if (!Ready() || IsSkipped()) return;

            constexpr Rgba8 kFirst{255, 0, 255, 255};
            constexpr Rgba8 kSecond{0, 255, 255, 255};

            const GLuint owner = MakeSeededArray();
            const GLuint view = MakeLayerView(owner);

            // Drain 1: the storage definition's own upload. Read only to close the window.
            const Image seeded = SampleThroughTheOwner(owner, kViewLayer);
            ExpectWholeSurface(seeded, LayerSeed(kViewLayer), "the seeded layer before any view write");
            Gl().EndFrame();

            EmitWindow second;
            OpenEmitWindow(second);
            UploadThroughView(view, kFirst);
            const Image afterFirst = SampleThroughTheOwner(owner, kViewLayer);
            CloseEmitWindow(second);
            ExpectWholeSurface(afterFirst, kFirst, "the first view write, through the owner");
            ExpectTheDrainEmitted(second, "the SECOND drain (first view write after a draw boundary)");
            Gl().EndFrame();

            EmitWindow third;
            OpenEmitWindow(third);
            UploadThroughView(view, kSecond);
            const Image afterSecond = SampleThroughTheOwner(owner, kViewLayer);
            CloseEmitWindow(third);
            ExpectWholeSurface(afterSecond, kSecond, "the second view write, through the owner");
            ExpectTheDrainEmitted(third, "the THIRD drain (second view write after a draw boundary)");

            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "the workload left a GL error behind";
            const Image layerZero = SampleThroughTheOwner(owner, 0);
            ExpectWholeSurface(layerZero, LayerSeed(0),
                               "two writes through the view must still have left layer 0 alone");
        }

        // ------------------------------------------------------------------------------------
        // (d) the owner direction across the same boundary.
        // ------------------------------------------------------------------------------------
        TEST_F(TextureViewAliasScenario, TheOwnerUploadDrainRunsAgainAfterADrawBoundary) {
            if (!Ready() || IsSkipped()) return;

            constexpr Rgba8 kFirst{0, 0, 255, 255};
            constexpr Rgba8 kSecond{255, 255, 0, 255};

            const GLuint owner = MakeSeededArray();
            const GLuint view = MakeLayerView(owner);

            const Image seeded = SampleThroughTheView(view);
            ExpectWholeSurface(seeded, LayerSeed(kViewLayer),
                               "the view must read its layer's seed before any owner write");
            Gl().EndFrame();

            EmitWindow second;
            OpenEmitWindow(second);
            UploadLayerThroughOwner(owner, kViewLayer, kFirst);
            const Image afterFirst = SampleThroughTheView(view);
            CloseEmitWindow(second);
            ExpectWholeSurface(afterFirst, kFirst, "the first owner write, through the view");
            ExpectTheDrainEmitted(second, "the SECOND drain (first owner write after a draw boundary)");
            Gl().EndFrame();

            EmitWindow third;
            OpenEmitWindow(third);
            UploadLayerThroughOwner(owner, kViewLayer, kSecond);
            const Image afterSecond = SampleThroughTheView(view);
            CloseEmitWindow(third);
            ExpectWholeSurface(afterSecond, kSecond, "the second owner write, through the view");
            ExpectTheDrainEmitted(third, "the THIRD drain (second owner write after a draw boundary)");

            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "the workload left a GL error behind";
        }

    } // namespace
} // namespace MGITest
