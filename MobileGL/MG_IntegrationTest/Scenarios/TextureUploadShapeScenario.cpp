// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/TextureUploadShapeScenario.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - THE TEXTURE UPLOAD SHAPE, RECORDED (BRIEF-P4A.md D-D4). NOT A GATE IN P4a.
//
// WHAT IT IS FOR. SSIM is completely blind to the difference between "one union box" and "N
// separate rects", and that difference is the Mali upload cliff: Mali prices an upload by the
// number of JOBS, and ~100 one-rect sprite jobs against one union box measured +6 ms/frame
// (ARCHITECTURE.md:249-251). Every P4a gate can be green while the emission shape has silently
// inverted, so the shape needs a number - and there are TWO numbers, deliberately:
//
//   tex[emit= box= rect= jobs=]   the SERVER's count, Espryt's own, which has existed since P2
//   emit[ctu=]                    the CLIENT's count of the same records (CallClass::
//                                 ClientTextureUploadEmissions, minted by the P4a contract commit)
//
// The two agreeing is the whole reason both are printed (D-L). An emission-shape divergence between
// the client that decides the rect model and the server that pays the GPU cost is then a difference
// of two published numbers rather than something only a device can see.
//
// WHY IT IS RECORDED AND NOT GATED, and this is a scope decision rather than a hedge. ROADMAP.md:23
// puts "dirty 归属反转（按存储属主键控的发射游标）" in the P3b/P4b cell: P4a lands the flat drain
// list, and the per-storage-owner emission cursor with view/owner index remapping - which is what
// actually decides the shape for a texture uploaded through a VIEW - is the next phase's. Gating a
// shape the phase has not finished deciding would either pin today's shape as if it were the
// answer, or fail on a change that is the point of the next phase. So P4a BUILDS the scenario (it
// is meaningless without P4a's records) and runs it as a RECORDED comparison; P3b/P4b turns it into
// a gate with the Mali frame-time delta published beside it (D-D4).
//
// WHAT IT THEREFORE ASSERTS, and it is not nothing:
//
//   1. the numbers could be READ AT ALL - the counters exist, the window covers the workload, and
//      the workload really uploaded (a run that uploaded nothing would record four zeroes and look
//      exactly like a healthy run whose emitter had been switched off);
//   2. the internal ARITHMETIC of the server's own bracket holds: emit == box + rect, and
//      jobs >= emit, because a box emission is one job and a rect-list emission is N;
//   3. the client and the server agree on the RECORD COUNT when both are non-zero (ctu == emit).
//      Not "when the client is non-zero": on the P4a contract tree the client emits nothing and
//      that is a SKIP-shaped observation, not a divergence.
//
// Everything else - which shape each texture took, and whether that is the right shape - is
// RECORDED with RecordProperty and printed, for MEASUREMENTS.md and for the P3b/P4b gate to be
// written against.
//
// THE WORKLOAD is the shape the decision is about: one texture receiving MANY SMALL SCATTERED
// SUB-REGIONS per frame (the sprite-atlas / chunk-renderer shape), and one receiving a single large
// contiguous one. The first is where the box-versus-rect choice is made - MipmapStorage's 96-rect
// cascade and the summedArea*4 >= unionArea*3 union-box fallback - and the second is the control
// that must always be one box whatever the policy is.
//
// DIRECTGLES ONLY. The server-side counters are Espryt's (Managers.cpp:6360-6395); Magma's upload
// path is P7 and contributes nothing to them, so a DirectVulkan lane would record a bracket of
// zeroes and call it a shape.

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "../Harness/HeadlessGL.h"
#include "../Harness/PipeStatsWindow.h"
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

        // Set by the TextureUploadShape. ctest entry and by nothing else; a harness marker, never
        // read by the library.
        constexpr const char* kLaneMarker = "MGITEST_TEXTURE_UPLOAD_SHAPE_LANE";

        constexpr int kInset = 2;
        constexpr int kAtlasSize = 64;
        // Enough scattered rects that the box-versus-rect policy has a real decision to make: the
        // rect cascade caps at MipmapStorage::kMaxDirtyRects = 96, and the union-box fallback fires
        // on summedArea*4 >= unionArea*3, so a handful of rects would take neither branch
        // interestingly.
        constexpr int kScatteredRects = 40;
        constexpr int kRectSize = 2;
        constexpr int kFrames = 3;

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

        bool BuildMarkerIsSet(const char* name) {
            const char* value = std::getenv(name);
            return value != nullptr && value[0] == '1' && value[1] == '\0';
        }

        class TextureUploadShapeScenario : public ScenarioTest {
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
            }

            void TearDown() override {
                if (!Ready()) return;
                glUseProgram(0);
                glBindVertexArray(0);
                glBindTexture(GL_TEXTURE_2D, 0);
                glBindBuffer(GL_ARRAY_BUFFER, 0);
                if (m_scattered != 0) glDeleteTextures(1, &m_scattered);
                if (m_contiguous != 0) glDeleteTextures(1, &m_contiguous);
                if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
                if (m_quadBuffer != 0) glDeleteBuffers(1, &m_quadBuffer);
                if (m_program != 0) glDeleteProgram(m_program);
            }

            void SkipUnlessTheLaneIsAssertableHere() {
                if (std::getenv(kLaneMarker) == nullptr) {
                    GTEST_SKIP() << "runs only in its own lane: the TextureUploadShape. ctest entry "
                                    "sets " << kLaneMarker
                                 << " together with MOBILEGL_PIPE_STATS=1, "
                                    "MOBILEGL_PIPE_STATS_PERIOD=1 and a private "
                                    "MOBILEGL_LOG_FILE_PATH. None of that is configured in the "
                                    "ambient entries, and the ambient log is shared, so a read here "
                                    "would race.";
                    return;
                }
                if (Gl().BackendName() != "DirectGLES") {
                    GTEST_SKIP() << "DirectGLES only: the upload-shape counters are Espryt's "
                                    "(Managers.cpp:6360-6395) and " << Gl().BackendName()
                                 << " contributes nothing to them, so this lane would record a "
                                    "bracket of zeroes and call it a shape.";
                    return;
                }
                if (!BuildMarkerIsSet("MGITEST_PIPE_PUSH_BUILD")) {
                    GTEST_SKIP() << "this library was built without MOBILEGL_PIPE_PUSH: the client's "
                                    "half of the comparison (CallClass::ClientTextureUploadEmissions, "
                                    "the ctu= field) does not exist there, and a one-sided reading is "
                                    "not the comparison this scenario is for. The entry is registered "
                                    "in every build so that `ctest -L integration-gpu` names the same "
                                    "tests in the pull build and the push build (gate G2).";
                    return;
                }
                if (PipeStatsWindow::LibraryLogPath().empty()) {
                    GTEST_SKIP() << "the lane configured no MOBILEGL_LOG_FILE_PATH, and the library's "
                                    "summary line is the only channel this module has for reading "
                                    "PipeStats";
                    return;
                }
            }

            GLuint MakeAtlas(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
                std::vector<std::uint8_t> texels(static_cast<std::size_t>(kAtlasSize) * kAtlasSize * 4);
                for (std::size_t i = 0; i < texels.size(); i += 4) {
                    texels[i] = r;
                    texels[i + 1] = g;
                    texels[i + 2] = b;
                    texels[i + 3] = 255;
                }
                GLuint texture = 0;
                glGenTextures(1, &texture);
                glBindTexture(GL_TEXTURE_2D, texture);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kAtlasSize, kAtlasSize, 0, GL_RGBA,
                             GL_UNSIGNED_BYTE, texels.data());
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
                return texture;
            }

            Image DrawSampled(GLuint texture) {
                BindDefaultFramebuffer();
                glViewport(0, 0, Gl().Width(), Gl().Height());
                glUseProgram(m_program);
                glUniform1i(glGetUniformLocation(m_program, "uTex"), 0);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, texture);
                glBindVertexArray(m_vao);
                glDrawArrays(GL_TRIANGLES, 0, 6);
                return ReadPixels(Gl().Width(), Gl().Height());
            }

            GLuint m_program = 0;
            GLuint m_vao = 0;
            GLuint m_quadBuffer = 0;
            GLuint m_scattered = 0;
            GLuint m_contiguous = 0;
        };

        TEST_F(TextureUploadShapeScenario, TheEmittedUploadShapeIsRecordedAndTheTwoSidesAgree) {
            if (!Ready()) return;
            SkipUnlessTheLaneIsAssertableHere();
            if (IsSkipped()) return;

            m_scattered = MakeAtlas(0, 255, 0);
            m_contiguous = MakeAtlas(0, 255, 0);
            // The first draw of each texture uploads its whole level, which is not the shape this
            // scenario is about; it happens in the SETUP window, before the one that is read.
            DrawSampled(m_scattered);
            DrawSampled(m_contiguous);
            BindDefaultFramebuffer();
            Gl().EndFrame();

            // ---- the counted window ----
            Image lastScattered;
            Image lastContiguous;
            for (int frame = 0; frame < kFrames; ++frame) {
                // MANY SMALL SCATTERED RECTS: the shape whose box-versus-rect decision is the whole
                // subject. They are spread over the atlas on a coarse stride so that their union
                // box is most of the texture and their summed area is a small fraction of it -
                // which is the input the summedArea*4 >= unionArea*3 fallback is written for.
                glBindTexture(GL_TEXTURE_2D, m_scattered);
                const std::uint8_t patch[kRectSize * kRectSize * 4] = {
                    0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255};
                for (int rect = 0; rect < kScatteredRects; ++rect) {
                    const int x = ((rect * 7) % (kAtlasSize / kRectSize)) * kRectSize;
                    const int y = ((rect * 5) % (kAtlasSize / kRectSize)) * kRectSize;
                    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, kRectSize, kRectSize, GL_RGBA,
                                    GL_UNSIGNED_BYTE, patch);
                }
                lastScattered = DrawSampled(m_scattered);

                // ONE LARGE CONTIGUOUS REGION: the control. Whatever the policy is, this is one
                // box and one job, and a reading where it is not says the policy has stopped
                // looking at the region at all.
                glBindTexture(GL_TEXTURE_2D, m_contiguous);
                std::vector<std::uint8_t> band(static_cast<std::size_t>(kAtlasSize) * 8 * 4);
                for (std::size_t i = 0; i < band.size(); i += 4) {
                    band[i] = 0;
                    band[i + 1] = 255;
                    band[i + 2] = 0;
                    band[i + 3] = 255;
                }
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kAtlasSize, 8, GL_RGBA, GL_UNSIGNED_BYTE,
                                band.data());
                lastContiguous = DrawSampled(m_contiguous);
            }
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "the upload workload left a GL error behind";
            Gl().EndFrame(); // the swap that emits the window covering exactly the work above

            const PipeStatsWindow::Window window = PipeStatsWindow::LastFromLaneLog();
            ASSERT_TRUE(window.found)
                << "no 'MGPipe stats:' line in " << PipeStatsWindow::LibraryLogPath()
                << ", so the shape could not be read at all";
            RecordProperty("stats_line", window.line.c_str());

            const long long emissions = PipeStatsWindow::CounterOrAbsent(window, "emit");
            const long long box = PipeStatsWindow::CounterOrAbsent(window, "box");
            const long long rect = PipeStatsWindow::CounterOrAbsent(window, "rect");
            const long long jobs = PipeStatsWindow::CounterOrAbsent(window, "jobs");
            const long long clientEmissions = PipeStatsWindow::CounterOrAbsent(window, "ctu");
            ASSERT_GE(emissions, 0) << "the summary line carries no tex[emit=]: " << window.line;
            ASSERT_GE(box, 0) << "no box=: " << window.line;
            ASSERT_GE(rect, 0) << "no rect=: " << window.line;
            ASSERT_GE(jobs, 0) << "no jobs=: " << window.line;

            // THE RECORD. This is the deliverable of this scenario in P4a: a number, in the ctest
            // XML and in the log, for MEASUREMENTS.md and for the P3b/P4b gate to be written
            // against. Printed as well as recorded, because a RecordProperty is invisible in a
            // console run.
            std::cout << "[ TextureUploadShape ] backend=" << Gl().BackendName() << " frames=" << kFrames
                      << " scattered_rects_per_frame=" << kScatteredRects
                      << " server[emit=" << emissions << " box=" << box << " rect=" << rect
                      << " jobs=" << jobs << "] client[ctu=" << clientEmissions << "]" << std::endl;
            RecordProperty("server_emissions", static_cast<int>(emissions));
            RecordProperty("server_box_emissions", static_cast<int>(box));
            RecordProperty("server_rect_emissions", static_cast<int>(rect));
            RecordProperty("server_upload_jobs", static_cast<int>(jobs));
            RecordProperty("client_emissions", static_cast<int>(clientEmissions));

            // 1. the workload really uploaded. Without this the three assertions below are all
            //    0 == 0 and a run whose emitter was switched off records the same "healthy" shape
            //    as one that worked.
            ASSERT_GT(emissions, 0)
                << "the server counted no texture upload emission at all over " << kFrames
                << " frames of " << kScatteredRects
                << " sub-regions each plus a contiguous band. Either the uploads never reached the "
                   "backend or the counter stopped counting; in both cases every shape number below "
                   "would be a zero that means nothing. "
                << window.line;

            // 2. the server bracket's own arithmetic.
            EXPECT_EQ(box + rect, emissions)
                << "tex[box=] + tex[rect=] must be tex[emit=]: every emission takes exactly one of "
                   "the two shapes. " << window.line;
            EXPECT_GE(jobs, emissions)
                << "tex[jobs=] must be at least tex[emit=]: a box emission is one driver upload job "
                   "and a rect-list emission is N. " << window.line;

            // 3. the two sides agree, WHEN THERE ARE TWO SIDES - and "there are two sides" is
            //    answered by the BUILD, not by the number (review F-m7).
            //
            //    ctu= IS ALWAYS PRESENT IN A PUSH BUILD: PipeStats.cpp writes the field whether or
            //    not anything ever incremented the counter, so `clientEmissions > 0` conflated
            //    three different trees - "no client emitter exists", "the emitter exists and
            //    emitted nothing", and "the counter was not published at all" - into one branch
            //    that asserts nothing and prints a sentence that is only true of the first. Once
            //    package B's texture emitter lands, an emitter that STOPPED emitting would read
            //    exactly like no emitter at all and this case would have gone green over it, which
            //    is the failure mode the whole scenario exists to make impossible.
            //
            //    So the discriminator is MGITEST_PIPE_CLIENT_TEXTURE_UPLOAD_EMITTER_PRESENT, the
            //    build's own content probe for a MG_Impl/Pipe source that emits
            //    CallClass::ClientTextureUploadEmissions - the same mechanism as every other arming
            //    decision in this package - and each side of it asserts something real.
            const bool clientEmitterExists =
                BuildMarkerIsSet("MGITEST_PIPE_CLIENT_TEXTURE_UPLOAD_EMITTER_PRESENT");
            ASSERT_GE(clientEmissions, 0)
                << "the summary line carries no ctu= field at all, in a push build, where PipeStats "
                   "publishes it unconditionally. The client half of the comparison cannot be read: "
                << window.line;
            RecordProperty("client_emitter_present", clientEmitterExists ? 1 : 0);
            if (clientEmitterExists) {
                EXPECT_GT(clientEmissions, 0)
                    << "a MG_Impl/Pipe source emits CallClass::ClientTextureUploadEmissions on this "
                       "tree, and the SERVER counted " << emissions
                    << " texture upload emissions for this workload, but the client counted NONE. An "
                       "emitter that has stopped emitting reads exactly like no emitter at all in "
                       "this field, which is why this case asks the build rather than the number. "
                    << window.line;
                EXPECT_EQ(clientEmissions, emissions)
                    << "the CLIENT counted " << clientEmissions
                    << " texture upload records and the SERVER counted " << emissions
                    << " for the same workload in the same window. The two counting the same records "
                       "is the entire reason both are published (D-L): a divergence here is an "
                       "emission-shape divergence that SSIM is blind to and that costs ~+6 ms/frame "
                       "on Mali when it goes the wrong way. "
                    << window.line;
            } else {
                // Not merely "not asserted": on a tree with no client emitter the counter must be
                // ZERO, and a non-zero one would mean the probe is looking for the wrong symbol -
                // i.e. that the arming decision above is wrong and every future run of this case
                // is mis-armed.
                EXPECT_EQ(clientEmissions, 0)
                    << "no MG_Impl/Pipe source emits CallClass::ClientTextureUploadEmissions on this "
                       "tree, yet the client counted " << clientEmissions
                    << " of them. Something is incrementing that counter which this build's probe "
                       "cannot see, so the probe is looking for the wrong symbol and this case's "
                       "arming decision is unreliable in both directions. " << window.line;
                std::cout << "[ TextureUploadShape ] no P4a client emitter has landed on this tree "
                             "(the build's ClientTextureUploadEmissions probe found none), so this "
                             "run records the SERVER shape only and pins ctu=0. That is the expected "
                             "reading on the contract tree and it is not a divergence."
                          << std::endl;
                RecordProperty("client_side", "absent");
            }

            // The pixels, so that a recorded shape cannot be the shape of a workload that drew
            // nothing.
            EXPECT_TRUE(RegionIsMostly(lastScattered, kInset, lastScattered.Width() - kInset, kInset,
                                       lastScattered.Height() - kInset, "green", 0.0,
                                       "the scattered-rect atlas"));
            EXPECT_TRUE(RegionIsMostly(lastContiguous, kInset, lastContiguous.Width() - kInset, kInset,
                                       lastContiguous.Height() - kInset, "green", 0.0,
                                       "the contiguous-band atlas"));
        }

    } // namespace
} // namespace MGITest
