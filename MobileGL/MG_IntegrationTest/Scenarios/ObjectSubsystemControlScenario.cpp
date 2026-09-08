// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/ObjectSubsystemControlScenario.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - THE P4a SUBSYSTEM A/B IS REAL, AND ITS DEPENDENCY REFUSALS ARE EXERCISED (gate G12).
//
// P4a migrates FOUR subsystems (D-K1, MG_Pipe/MGPipe.h):
//
//   bit  9  kMGPipeSubsystemFramebuffer       set_framebuffer_state
//   bit 10  kMGPipeSubsystemTextureResources  texture + renderbuffer resource_*, set_texture_params
//   bit 11  kMGPipeSubsystemSamplers          sampler CSO, sampler view, the three unit sets
//   bit 12  kMGPipeSubsystemPrograms          shader CSO, draw/dispatch program, global constants
//
// so the push build's default mask becomes kMGPipeSubsystemsMigratedAtP4a = 0x1fff, and P3a's
// 0x1ff survives as the control that clears exactly those four - MGPipe.h's rule that every phase's
// constant keeps meaning what it meant, so an operator's recorded mask is still readable a phase
// later. THE OFF LANE IS 0x1ff AND NOT A HAND-PICKED PATTERN, for that reason.
//
// That A/B is what every "push vs pull" number in MEASUREMENTS.md is taken against, and it has one
// characteristic failure mode: the bits stop steering anything, both arms run the same code, and
// every later comparison is quietly taken against a switch that does nothing. This file is the
// entry that cannot let that happen. It is the P4a analogue of ResourceSubsystemControlScenario and
// deliberately its twin in shape.
//
// WHAT IT ASSERTS, per lane:
//
//   on  (MOBILEGL_PIPE_PUSH=0x1fff)
//       The client emits P4a's records for the workload: a framebuffer state per bound target that
//       moved, the three unit sets, and the client-side texture upload record. The window's
//       emit[fbe= sve= sse= sie= ctu=] bracket therefore carries a NON-ZERO total.
//
//   off (MOBILEGL_PIPE_PUSH=0x1ff, P3a's default = P4a's four subsystems cleared)
//       The frontend dispatch falls through to the legacy MGB_CTX-reading arms, nothing is emitted
//       through any of the four families, and every one of those five counters must read ZERO.
//       This is the reading a dead switch fails: with the bits ignored, this lane would report the
//       same non-zero counts as the other one.
//
//   refused (MOBILEGL_PIPE_PUSH=0x9ff = bits 0..8 plus bit 11, samplers, WITHOUT bit 10)
//       D-K2's dependency refusal. Every MGPBoundView::Texture and MGPImageView::Res names a
//       Texture handle and only bit 10 populates the texture slot table, so a sampler subsystem
//       without it would miss every lookup and walk on without unbinding. The bring-up logs ONE
//       error naming BOTH bits, refuses bit 11 and runs the legacy sampler arm - modelled on the
//       bit-8-requires-bit-7 refusal that already ships (Managers.cpp:2393-2410). The assertion is
//       that the refusal is NAMED and that the run then produces the same pixels as any other
//       lane: a refusal that half-ran, or that aborted, would both be failures here.
//
//   every lane
//       THE PIXELS MUST NOT MOVE. The workload draws one solid-colour quad through a texture, an
//       explicit sampler object and a user framebuffer, and every lane must read back that colour.
//       "The counters moved and the picture did not" is the whole claim - a switch that changed
//       what is drawn would not be an A/B, it would be a bug.
//
// WHY IT CAN SKIP. The counters are emitted by the client-side emitters P4a packages B and C own,
// and this file is written against the P4a contract commit, before either lands. Until then nothing
// emits, the five counters are structurally zero in BOTH lanes, and an assertion about the
// difference would be a statement about nothing. The build answers the question rather than a
// hand-maintained list: MG_IntegrationTest/CMakeLists.txt greps every source under MG_Impl/Pipe/
// for the counters' names and passes the answer in as MGITEST_PIPE_OBJECT_EMITTER_PRESENT, with a
// CONFIGURE_DEPENDS on that directory and on each file it finds so the answer cannot go stale. It
// is a CONTENT probe, not a filename probe, so the owning packages keep control of their own file
// layout - P4a's new client files are headers (D-P), and a glob for a named .cpp would have kept
// this control skipping forever with a reason that had become false.
//
// DIRECTGLES ONLY, and that is the honest scope: P4a migrates Espryt's framebuffer, texture,
// sampler and program paths. Magma's are P7 (D-Q) and register nothing here, so a DirectVulkan lane
// would be measuring the client emitters against a backend nobody asked to change.

#include <cstdint>
#include <cstdlib>
#include <cstring>
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

        // Set by the three ObjectSubsystemControl. ctest entries and by nothing else; a harness
        // marker, never read by the library. Its absence means an ambient entry, where neither the
        // stats channel nor a private log path is configured.
        constexpr const char* kLaneMarker = "MGITEST_OBJECT_SUBSYSTEM_LANE";
        constexpr const char* kLaneOn = "on";
        constexpr const char* kLaneOff = "off";
        constexpr const char* kLaneRefused = "refused";

        constexpr int kInset = 2;
        constexpr int kTextureSize = 4;
        // Enough frames that a per-frame emitter and a per-draw emitter read differently, and few
        // enough that one summary window covers exactly this.
        constexpr int kDrawsInTheWindow = 4;

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

        std::string LaneName() {
            const char* lane = std::getenv(kLaneMarker);
            return lane != nullptr ? std::string(lane) : std::string();
        }

        class ObjectSubsystemControlScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                m_lane = LaneName();
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
                RecordProperty("lane", m_lane.empty() ? "ambient" : m_lane.c_str());
            }

            void TearDown() override {
                if (!Ready()) return;
                glUseProgram(0);
                glBindVertexArray(0);
                glBindSampler(0, 0);
                glBindBuffer(GL_ARRAY_BUFFER, 0);
                if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
                if (m_quadBuffer != 0) glDeleteBuffers(1, &m_quadBuffer);
                if (m_program != 0) glDeleteProgram(m_program);
            }

            // GTEST_SKIP() returns from the function it is written in, so this cannot report
            // through a return value; every caller pairs it with `if (IsSkipped()) return;`.
            void SkipUnlessTheLaneIsAssertableHere(bool needsTheEmitters) {
                if (m_lane.empty()) {
                    GTEST_SKIP() << "runs only in its own lane: the three ObjectSubsystemControl. "
                                    "ctest entries set " << kLaneMarker
                                 << " together with the MOBILEGL_PIPE_PUSH bitmask that arm means, "
                                    "MOBILEGL_PIPE_STATS=1, MOBILEGL_PIPE_STATS_PERIOD=1 and a "
                                    "private MOBILEGL_LOG_FILE_PATH. None of that is configured in "
                                    "the ambient entries, and the ambient log is shared, so a read "
                                    "here would race.";
                    return;
                }
                if (!BuildMarkerIsSet("MGITEST_PIPE_PUSH_BUILD")) {
                    GTEST_SKIP() << "this library was built without MOBILEGL_PIPE_PUSH: there are no "
                                    "subsystem bits to clear, P4a's five CallClass members do not "
                                    "exist and the summary line carries no emit[...] bracket. The "
                                    "entry is registered here anyway so that `ctest -L "
                                    "integration-gpu` names the same tests in the pull build and the "
                                    "push build (gate G2).";
                    return;
                }
                if (needsTheEmitters && !BuildMarkerIsSet("MGITEST_PIPE_OBJECT_EMITTER_PRESENT")) {
                    GTEST_SKIP() << "subsystem not implemented on this tree: no source under "
                                    "MobileGL/MG_Impl/Pipe/ emits FramebufferEmissions, so nothing "
                                    "sends a P4a record, every counter in the emit[] bracket is "
                                    "structurally zero in BOTH lanes and the difference between them "
                                    "is not observable yet. P4a packages B (framebuffer, texture) "
                                    "and C (sampler, image, program) own those emitters; this "
                                    "control arms itself when they land, whatever files they use.";
                    return;
                }
                if (PipeStatsWindow::LibraryLogPath().empty()) {
                    GTEST_SKIP() << "the lane configured no MOBILEGL_LOG_FILE_PATH, and the library's "
                                    "own log is the only channel this module has for reading "
                                    "PipeStats and the bring-up's refusal line";
                    return;
                }
            }

            // The workload, and every one of P4a's four families is in it exactly once per draw:
            // a USER FRAMEBUFFER with a texture attachment (bit 9), a TEXTURE with parameters and
            // an upload (bit 10), an explicit SAMPLER OBJECT on the unit (bit 11) and a PROGRAM
            // with a default-uniform-block write (bit 12). A lane that steered only one of the four
            // would move only its own counter, which is why they are counted separately.
            void RunTheWorkload() {
                std::vector<std::uint8_t> texels(kTextureSize * kTextureSize * 4);
                for (std::size_t i = 0; i < texels.size(); i += 4) {
                    texels[i] = 0;
                    texels[i + 1] = 255;
                    texels[i + 2] = 0;
                    texels[i + 3] = 255;
                }
                glGenTextures(1, &m_texture);
                glBindTexture(GL_TEXTURE_2D, m_texture);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kTextureSize, kTextureSize, 0, GL_RGBA,
                             GL_UNSIGNED_BYTE, texels.data());
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);

                glGenSamplers(1, &m_sampler);
                glSamplerParameteri(m_sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glSamplerParameteri(m_sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glSamplerParameteri(m_sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glSamplerParameteri(m_sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

                // The user framebuffer, drawn into once per iteration so that the framebuffer
                // record has a reason to move: the binding alternates between it and the default
                // framebuffer, which is exactly what a per-target set_framebuffer_state counts.
                glGenTextures(1, &m_attachment);
                glBindTexture(GL_TEXTURE_2D, m_attachment);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kTextureSize, kTextureSize, 0, GL_RGBA,
                             GL_UNSIGNED_BYTE, nullptr);
                glBindTexture(GL_TEXTURE_2D, 0);
                glGenFramebuffers(1, &m_fbo);
                glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                       m_attachment, 0);
                BindDefaultFramebuffer();

                for (int draw = 0; draw < kDrawsInTheWindow; ++draw) {
                    // Into the user framebuffer...
                    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
                    glViewport(0, 0, kTextureSize, kTextureSize);
                    glUseProgram(m_program);
                    glUniform1i(glGetUniformLocation(m_program, "uTex"), 0);
                    glActiveTexture(GL_TEXTURE0);
                    glBindTexture(GL_TEXTURE_2D, m_texture);
                    glBindSampler(0, m_sampler);
                    glBindVertexArray(m_vao);
                    glDrawArrays(GL_TRIANGLES, 0, 6);
                    // ...and into the default one, which is what the case reads back.
                    BindDefaultFramebuffer();
                    glViewport(0, 0, Gl().Width(), Gl().Height());
                    glDrawArrays(GL_TRIANGLES, 0, 6);
                    // One sub-region upload per iteration, so the client-side texture upload
                    // counter (ctu) has something to count and the server's tex[emit=] has the
                    // same something.
                    const std::uint8_t green[4] = {0, 255, 0, 255};
                    glBindTexture(GL_TEXTURE_2D, m_texture);
                    glTexSubImage2D(GL_TEXTURE_2D, 0, draw % kTextureSize, 0, 1, 1, GL_RGBA,
                                    GL_UNSIGNED_BYTE, green);
                }
            }

            void ReleaseTheWorkload() {
                glBindSampler(0, 0);
                glBindTexture(GL_TEXTURE_2D, 0);
                BindDefaultFramebuffer();
                if (m_fbo != 0) glDeleteFramebuffers(1, &m_fbo);
                if (m_sampler != 0) glDeleteSamplers(1, &m_sampler);
                if (m_texture != 0) glDeleteTextures(1, &m_texture);
                if (m_attachment != 0) glDeleteTextures(1, &m_attachment);
                m_fbo = m_sampler = m_texture = m_attachment = 0;
            }

            std::string m_lane;
            GLuint m_program = 0;
            GLuint m_vao = 0;
            GLuint m_quadBuffer = 0;
            GLuint m_texture = 0;
            GLuint m_attachment = 0;
            GLuint m_sampler = 0;
            GLuint m_fbo = 0;
        };

        // ONE case per lane, and it is a constraint rather than a preference: this case READS the
        // library log, the log is a per-LANE resource (the library opens it fopen(path, "w"), so
        // every process in a lane truncates it), and a second case in the same lane would race this
        // one under `ctest -j` with a failure indistinguishable from "the counter was never
        // emitted". The CMake registration gives each lane a TEST_FILTER naming one case.
        TEST_F(ObjectSubsystemControlScenario, ClearingTheP4aBitsStopsTheEmissionsAndNotThePixels) {
            if (!Ready()) return;
            SkipUnlessTheLaneIsAssertableHere(/*needsTheEmitters=*/true);
            if (IsSkipped()) return;
            if (m_lane == kLaneRefused) {
                GTEST_SKIP() << "the refusal lane runs ASamplerBitWithoutTheTextureBitIsRefusedAndNamed "
                                "instead: at 0x9ff the sampler subsystem is refused at bring-up, so "
                                "the emission counts are neither the on-lane's nor the off-lane's and "
                                "asserting either would be reading a third arm as if it were one of "
                                "the two.";
            }

            BindDefaultFramebuffer();
            Gl().EndFrame(); // close the setup window: everything below is one window

            ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
            RunTheWorkload();
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "the workload left a GL error behind";
            const Image image = ReadPixels(Gl().Width(), Gl().Height());
            Gl().EndFrame(); // the swap that emits the window covering exactly the work above

            const PipeStatsWindow::Window window = PipeStatsWindow::LastFromLaneLog();
            ASSERT_TRUE(window.found)
                << "no 'MGPipe stats:' line in " << PipeStatsWindow::LibraryLogPath()
                << ". This IS a push build (the lane checked MGITEST_PIPE_PUSH_BUILD before getting "
                   "here), so either MOBILEGL_PIPE_STATS / MOBILEGL_PIPE_STATS_PERIOD did not reach "
                   "the process, or no summary line was emitted at all because nothing reached "
                   "PipeStats::OnPresent.";
            RecordProperty("stats_line", window.line.c_str());

            // The five counters of the emit[] bracket, read individually so that a lane which
            // steered one family and not another says WHICH.
            const long long framebuffer = PipeStatsWindow::CounterOrAbsent(window, "fbe");
            const long long samplerViews = PipeStatsWindow::CounterOrAbsent(window, "sve");
            const long long samplerStates = PipeStatsWindow::CounterOrAbsent(window, "sse");
            const long long shaderImages = PipeStatsWindow::CounterOrAbsent(window, "sie");
            const long long clientUploads = PipeStatsWindow::CounterOrAbsent(window, "ctu");
            ASSERT_GE(framebuffer, 0)
                << "the summary line carries no fbe= field, so this build's PipeStats has no P4a "
                   "emission counters to read: "
                << window.line;
            ASSERT_GE(samplerViews, 0) << "no sve= field: " << window.line;
            ASSERT_GE(samplerStates, 0) << "no sse= field: " << window.line;
            ASSERT_GE(shaderImages, 0) << "no sie= field: " << window.line;
            ASSERT_GE(clientUploads, 0) << "no ctu= field: " << window.line;
            const long long total = framebuffer + samplerViews + samplerStates + shaderImages +
                                    clientUploads;

            if (m_lane == kLaneOn) {
                EXPECT_GT(total, 0)
                    << "with bits 9|10|11|12 SET the four P4a families are the path this workload "
                       "takes - a user framebuffer bound and unbound "
                    << kDrawsInTheWindow
                    << " times, a texture with parameters and a sub-region upload per iteration, an "
                       "explicit sampler object on the unit and a program with a default-uniform "
                       "write - so the window's emit[] bracket must carry something. All five "
                       "reading zero means the emitters never ran on the arm that is supposed to run "
                       "them. It reported: "
                    << window.line;
                // The framebuffer family on its own, because it is the one that would be hidden by
                // a large upload count: a suppressor that stopped suppressing shows up as fbe
                // tracking the DRAW count, and a family that never emitted shows up as zero.
                EXPECT_GT(framebuffer, 0)
                    << "fbe= is zero on the ON lane: set_framebuffer_state never went out even "
                       "though the workload bound a user framebuffer and the default framebuffer "
                    << kDrawsInTheWindow << " times each. " << window.line;
            } else if (m_lane == kLaneOff) {
                EXPECT_EQ(total, 0)
                    << "with bits 9|10|11|12 CLEARED (MOBILEGL_PIPE_PUSH=0x1ff, P3a's default) the "
                       "frontend dispatch must fall through to the legacy MGB_CTX-reading arms and "
                       "emit nothing through any of the four P4a families, so every counter in the "
                       "emit[] bracket must be zero. A non-zero count here is the dead-switch "
                       "reading: the bits are being ignored, both arms run the same code, and every "
                       "push-vs-pull number taken against this A/B is measuring one arm twice. It "
                       "reported: "
                    << window.line;
            } else {
                FAIL() << "unknown " << kLaneMarker << " value '" << m_lane
                       << "': the arms are on / off / refused. Reading an unrecognised name as "
                          "either would make this lane assert the other arm's expectation while "
                          "claiming to test this one.";
            }

            // ... and the picture is the same whichever arm ran.
            EXPECT_TRUE(RegionIsMostly(image, kInset, image.Width() - kInset, kInset,
                                       image.Height() - kInset, "green", 0.0,
                                       "the sampled draw [" + m_lane + "]"))
                << "the subsystem bits changed what is DRAWN, which is not an A/B - the handle path "
                   "and the legacy path must produce the same pixels from the same texture, sampler "
                   "and framebuffer.";

            ReleaseTheWorkload();
        }

        // ------------------------------------------------------------------------------------
        // D-K2's dependency refusal, in the direction that has to be refused.
        //
        // 0x9ff is bits 0..8 (everything P3a shipped) plus bit 11 (samplers) and WITHOUT bit 10
        // (texture resources). Every MGPBoundView::Texture and every MGPImageView::Res names a
        // Texture handle, and only bit 10 populates the texture slot table, so with bit 11 alone
        // every lookup would miss and the unit walk would `continue` without unbinding - a
        // half-run subsystem, which ROADMAP.md:7 forbids as loudly as a dead switch. The bring-up
        // logs ONE error naming both bits, refuses bit 11, and runs the legacy sampler arm.
        //
        // TWO ASSERTIONS, and the second is the one that stops this from being a log-scraping test:
        // the refusal is NAMED in the library's own log, and the run then draws the same picture as
        // every other lane. A refusal that aborted the process, and a refusal that silently let the
        // half-configured arm run, are both failures - and they look completely different here.
        // ------------------------------------------------------------------------------------
        TEST_F(ObjectSubsystemControlScenario, ASamplerBitWithoutTheTextureBitIsRefusedAndNamed) {
            if (!Ready()) return;
            // needsTheEmitters=false: the refusal is a BRING-UP decision made from the bitmask
            // alone, so it is assertable before any emitter exists - which is exactly what makes it
            // the one P4a control that is not vacuous on the contract tree.
            SkipUnlessTheLaneIsAssertableHere(/*needsTheEmitters=*/false);
            if (IsSkipped()) return;
            if (m_lane != kLaneRefused) {
                GTEST_SKIP() << "runs only in the refusal lane (MOBILEGL_PIPE_PUSH=0x9ff): the "
                                "on/off lanes configure a mask whose dependencies are all satisfied, "
                                "so there is no refusal there to find and a search for one would "
                                "report a healthy lane as red.";
            }
            // The refusal is decided from the bitmask, but it is a BACKEND's decision: D-K2 puts it
            // in ResolveSamplersSubsystemArm(), beside the bit-8-requires-bit-7 refusal that
            // already ships, and that function is package D's (Managers.cpp). A backend that does
            // not yet honour P4a's mask at all cannot refuse a dependency inside it, so on such a
            // tree there is nothing here to find and this case SKIPS rather than reporting the
            // absence of an unimplemented subsystem as a failure. The marker is the same one
            // HandleRecycle's P4a cases read - "does any source under this backend name one of the
            // four P4a subsystem constants" - because naming the constant is exactly what honouring
            // the mask means.
            {
                const std::string& backend = Gl().BackendName();
                const std::string marker =
                    "MGITEST_HANDLE_REKEY_OBJECTS_" + (backend == "DirectVulkan"
                                                           ? std::string("DirectVulkan")
                                                           : std::string("DirectGLES"));
                if (!BuildMarkerIsSet(marker.c_str())) {
                    GTEST_SKIP() << "subsystem not implemented on this tree: no source under "
                                    "MobileGL/MG_Backend/"
                                 << backend
                                 << " names any of kMGPipeSubsystem{Framebuffer, TextureResources, "
                                    "Samplers, Programs}, so this backend does not honour P4a's mask "
                                    "and cannot refuse a dependency inside it. D-K2's refusal lives "
                                    "in ResolveSamplersSubsystemArm() beside the bit-8-requires-bit-7 "
                                    "one that already ships (Managers.cpp:2393-2410), which is P4a "
                                    "package D's file; this control arms itself when that lands. The "
                                    "lane itself is not wasted: the library came up under 0x9ff, "
                                    "which on a tree with no P4a arm is P3a's mask plus one inert "
                                    "bit, and a mask that aborted a bring-up would have failed this "
                                    "entry before the skip.";
                }
            }

            BindDefaultFramebuffer();
            ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
            RunTheWorkload();
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR))
                << "the workload left a GL error behind on the refused lane, which would mean the "
                   "refusal did not fall back cleanly to the legacy arm";
            const Image image = ReadPixels(Gl().Width(), Gl().Height());
            Gl().EndFrame();

            const std::string log = PipeStatsWindow::ReadWholeFile(PipeStatsWindow::LibraryLogPath());
            ASSERT_FALSE(log.empty())
                << "the library wrote nothing to " << PipeStatsWindow::LibraryLogPath()
                << ", so the refusal cannot be read back. MOBILEGL_LOG_FILE_PATH is the only channel "
                   "this module has for the library's own report.";
            // Named, not merely present: the refusal has to say which bit it refused AND which bit
            // it needed, because "a sampler bit was ignored" without the dependency is a message an
            // operator cannot act on. Both spellings are accepted - the constant's name and the
            // hexadecimal mask - so the assertion does not pin the message's wording.
            const bool namesTheSampler = log.find("Sampler") != std::string::npos ||
                                         log.find("sampler") != std::string::npos ||
                                         log.find("0x800") != std::string::npos;
            const bool namesTheTexture = log.find("TextureResources") != std::string::npos ||
                                         log.find("texture resource") != std::string::npos ||
                                         log.find("0x400") != std::string::npos;
            EXPECT_TRUE(namesTheSampler && namesTheTexture)
                << "MOBILEGL_PIPE_PUSH=0x9ff sets the sampler subsystem (bit 11) without the texture "
                   "resource subsystem (bit 10) it depends on, and the library's log names neither "
                   "of them. D-K2 requires ONE MGLOG_E naming both bits and a fall back to the "
                   "legacy sampler arm; a mask that is silently half-honoured is the failure this "
                   "case exists to catch, and it is invisible in the pixels by construction. The log "
                   "was " << log.size() << " bytes.";

            EXPECT_TRUE(RegionIsMostly(image, kInset, image.Width() - kInset, kInset,
                                       image.Height() - kInset, "green", 0.0,
                                       "the sampled draw [refused]"))
                << "the refused configuration did not draw what every other lane draws. A refusal is "
                   "supposed to run the LEGACY arm, which is the arm that ships in a pull build - so "
                   "the pixels are the one thing it may not change.";

            ReleaseTheWorkload();
        }

    } // namespace
} // namespace MGITest
