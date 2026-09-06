// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/CsoContentAddressingScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - THE CSO CONTENT-ADDRESSING NEGATIVE CONTROL (gate G12).
//
// P2's render-state CSO is content-addressed: the client hashes the 396 pipeline bytes, probes a
// 64-entry cache, memcmps a hash hit and reuses the handle. The whole design is measured against
// a knob that turns that off - kMGPipeBehaviourNoCsoContentAddressing, bit 63 of the runtime
// MOBILEGL_PIPE_PUSH bitmask - so that "push is slower" can be told apart from "the CSO design is
// slower" (P2 brief D.4.5). A measurement knob has one characteristic failure mode: it stops
// steering anything and every later number is quietly taken against a switch that does nothing.
// This file is the entry that cannot let that happen.
//
// WHAT IT ASSERTS, per arm, and why those are the right shapes:
//
//   content-addressed (MOBILEGL_PIPE_PUSH=0x7f)
//       A Blaze3D blend toggle - enable / draw / disable / draw, N times, which is the workload
//       the CsoCache exists for (ARCHITECTURE.md 5.1: the push happens at validate rather than in
//       the setter precisely because Blaze3D brackets every batch this way) - visits exactly TWO
//       distinct pipeline subsets. So the mint count must stay small and BOUNDED while the bind
//       count grows with the draws: csom << csob.
//
//   no content addressing (MOBILEGL_PIPE_PUSH=0x800000000000007f)
//       Every pipeline-version change mints a fresh CSO and the map is never probed, so mint and
//       bind must move together: csom == csob. This is the assertion a dead switch fails - with
//       the bit ignored, this arm would report csom << csob just like the other one.
//
//   both arms
//       The PIXELS must not move. The quad is drawn with alpha 1.0 through
//       GL_SRC_ALPHA / GL_ONE_MINUS_SRC_ALPHA, so the blended and unblended draws produce the
//       same colour by construction and the readback is the same image in both arms and after
//       every toggle. "The counters moved and the picture did not" is the whole claim.
//
// HOW THE COUNTERS ARE READ. MG_Util::PipeStats is internal to the library and this module cannot
// link against it (ScenarioFixture.h explains why: on Android this binary links the SHIPPING
// libMobileGL.so, built -fvisibility=hidden). The library's own summary line is the only channel,
// so each lane sets MOBILEGL_PIPE_STATS=1, MOBILEGL_PIPE_STATS_PERIOD=1 - one line per
// eglSwapBuffers - and a MOBILEGL_LOG_FILE_PATH of its OWN. The log path has to be private: the
// library opens it fopen(path, "w"), so every process in a lane truncates it, and a whole-file
// read in a shared lane races a neighbour's bring-up. That is the same rule, and the same
// remedy, as PipeVerifyArmingScenario's arming lane.
//
// The window a summary line reports is "since the previous line" (PipeStats::FormatWindowLine), so
// the workload runs inside ONE frame: a swap before it closes the setup window, and the swap after
// it emits a line whose csom / csob cover the toggle loop and nothing else.
//
// WHY IT CAN SKIP. The counters are minted by the client-side tracker (P2 package B), and this
// file is written against the P2 contract commit, before that package lands. Until the tracker
// exists there is no CSO to mint, csom is structurally 0 and an assertion about its ratio to csob
// would be a statement about nothing. The build answers the question rather than a hand-maintained
// list: MG_IntegrationTest/CMakeLists.txt greps every source under MG_Impl/Pipe/ for the two
// counters' names and passes the answer in as MGITEST_PIPE_TRACKER_PRESENT, with a
// CONFIGURE_DEPENDS on that directory and on each file it finds so the answer cannot go stale.
// It is a CONTENT probe, not a filename probe, precisely so that the owning package keeps control
// of its own file layout - it implements the tracker and the cache header-only today, and a glob
// for `Tracker.cpp` would have kept this control skipping forever after that package landed, with
// a reason that had become false. When an emitter lands the arms arm themselves; until then the
// entries are registered, visible and SKIPPED with the reason - never absent, and never green for
// having asserted nothing.

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
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

        // Set by the two CsoContentAddressing. ctest entries and by nothing else; a harness
        // marker, never read by the library. Its absence means an ambient entry, where neither
        // the stats channel nor a private log path is configured.
        constexpr const char* kLaneMarker = "MGITEST_CSO_LANE";
        constexpr const char* kLaneContentAddressed = "content-addressed";
        constexpr const char* kLaneNoContentAddressing = "no-content-addressing";

        // Toggle pairs per frame. 8 is small enough to keep the frame cheap and large enough that
        // "mints stay bounded" and "mints track binds" are different numbers by a wide margin.
        constexpr int kTogglePairs = 8;
        constexpr int kDrawsPerFrame = kTogglePairs * 2;
        // The blend toggle visits two distinct pipeline subsets, so two CSOs. The bound is
        // deliberately a little looser than 2: a future chunk-table change could legitimately
        // split one of them, and the claim being pinned here is "bounded, not per-draw".
        constexpr long long kMaxDistinctCsos = 4;

        constexpr const char* kVS = R"(#version 330 core
in vec2 aPos;
void main() { gl_Position = vec4(aPos, 0.0, 1.0); }
)";

        constexpr const char* kFS = R"(#version 330 core
out vec4 oColor;
void main() { oColor = vec4(0.0, 1.0, 0.0, 1.0); }
)";

        constexpr int kInset = 2;

        bool BuildMarkerIsSet(const char* name) {
            const char* value = std::getenv(name);
            return value != nullptr && value[0] == '1' && value[1] == '\0';
        }

        std::string LaneName() {
            const char* lane = std::getenv(kLaneMarker);
            return lane != nullptr ? std::string(lane) : std::string();
        }

        std::string LibraryLogPath() {
            const char* path = std::getenv("MOBILEGL_LOG_FILE_PATH");
            return (path != nullptr && *path != '\0') ? std::string(path) : std::string();
        }

        std::string ReadWholeFile(const std::string& path) {
            if (path.empty()) return {};
            std::ifstream file(path, std::ios::binary);
            if (!file.good()) return {};
            return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        }

        // One window's CSO counters, as the library printed them.
        struct CsoWindow {
            bool found = false;
            long long mints = -1;
            long long binds = -1;
            std::string line;
        };

        // Parses `... cso[csom=<N> csob=<M>] ...` out of the LAST "MGPipe stats:" line in the log.
        // The last line, because the window a line reports is "since the previous line" and the
        // caller closes the setup window with a swap before the workload.
        CsoWindow LastCsoWindow(const std::string& log) {
            CsoWindow window;
            const std::string marker = "MGPipe stats:";
            std::size_t at = log.rfind(marker);
            if (at == std::string::npos) return window;
            const std::size_t end = log.find('\n', at);
            window.line = log.substr(at, end == std::string::npos ? std::string::npos : end - at);

            const std::string mintKey = "csom=";
            const std::string bindKey = "csob=";
            const std::size_t mintAt = window.line.find(mintKey);
            const std::size_t bindAt = window.line.find(bindKey);
            if (mintAt == std::string::npos || bindAt == std::string::npos) return window;
            window.mints = std::strtoll(window.line.c_str() + mintAt + mintKey.size(), nullptr, 10);
            window.binds = std::strtoll(window.line.c_str() + bindAt + bindKey.size(), nullptr, 10);
            window.found = true;
            return window;
        }

        class CsoContentAddressingScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                m_lane = LaneName();
                std::string error;
                m_program = CompileProgram(kVS, kFS, &error);
                ASSERT_NE(m_program, 0u) << error;

                const float quad[12] = {-1.0f, -1.0f, 1.0f, -1.0f, 1.0f, 1.0f,
                                        -1.0f, -1.0f, 1.0f, 1.0f,  -1.0f, 1.0f};
                glGenVertexArrays(1, &m_vao);
                glBindVertexArray(m_vao);
                glGenBuffers(1, &m_vbo);
                glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
                glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
                glEnableVertexAttribArray(0);
                glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
                ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "scene setup left a GL error behind";
                RecordProperty("lane", m_lane.empty() ? "ambient" : m_lane.c_str());
            }

            void TearDown() override {
                if (!Ready()) return;
                glBindVertexArray(0);
                glBindBuffer(GL_ARRAY_BUFFER, 0);
                if (m_vbo != 0) glDeleteBuffers(1, &m_vbo);
                if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
                if (m_program != 0) glDeleteProgram(m_program);
            }

            // GTEST_SKIP() returns from the function it is written in, so this cannot report
            // through a return value; every caller pairs it with `if (IsSkipped()) return;`.
            void SkipUnlessTheLaneIsAssertableHere() {
                if (m_lane.empty()) {
                    GTEST_SKIP() << "runs only in its own lane: the two CsoContentAddressing. ctest entries set "
                                    "MGITEST_CSO_LANE together with the MOBILEGL_PIPE_PUSH bitmask, "
                                    "MOBILEGL_PIPE_STATS=1, MOBILEGL_PIPE_STATS_PERIOD=1 and a private "
                                    "MOBILEGL_LOG_FILE_PATH. None of that is configured in the ambient "
                                    "entries, and the ambient log is shared, so a read here would race.";
                    return;
                }
                if (!BuildMarkerIsSet("MGITEST_PIPE_PUSH_BUILD")) {
                    GTEST_SKIP() << "this library was built without MOBILEGL_PIPE_PUSH, so there is no "
                                    "render-state CSO to mint, no cso[] bracket in the summary line and "
                                    "nothing for the content-addressing bit to steer. The entry is "
                                    "registered here anyway so that `ctest -L integration-gpu` names the "
                                    "same tests in the pull build and the push build (gate G2).";
                    return;
                }
                if (!BuildMarkerIsSet("MGITEST_PIPE_TRACKER_PRESENT")) {
                    GTEST_SKIP() << "the CSO counters have no emitter in this build: no source under "
                                    "MobileGL/MG_Impl/Pipe/ names RenderStateCsoMints or "
                                    "RenderStateCsoBinds, so nothing mints or binds a render-state CSO "
                                    "and csom / csob are structurally zero. P2 package B owns the tracker "
                                    "and the CSO cache; this entry arms itself when they land, whatever "
                                    "files that package chooses to put them in.";
                    return;
                }
                if (LibraryLogPath().empty()) {
                    GTEST_SKIP() << "the lane configured no MOBILEGL_LOG_FILE_PATH, and the library's summary "
                                    "line is the only channel this module has for reading PipeStats";
                    return;
                }
            }

            // enable / draw / disable / draw, kTogglePairs times, entirely inside one frame.
            // Returns the readback taken at the end of that frame, before the swap.
            Image RunBlendToggleFrame() {
                BindDefaultFramebuffer();
                ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
                glUseProgram(m_program);
                glBindVertexArray(m_vao);
                glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
                for (int i = 0; i < kTogglePairs; ++i) {
                    glEnable(GL_BLEND);
                    glDrawArrays(GL_TRIANGLES, 0, 6);
                    glDisable(GL_BLEND);
                    glDrawArrays(GL_TRIANGLES, 0, 6);
                }
                const Image image = ReadPixels(Gl().Width(), Gl().Height());
                Gl().EndFrame();
                return image;
            }

            std::string m_lane;
            GLuint m_program = 0;
            GLuint m_vao = 0;
            GLuint m_vbo = 0;
        };

        // ONE case per lane, and that is a hard constraint rather than a style choice.
        //
        // This case READS the library log, and the log is a per-LANE resource: the library opens it
        // fopen(path, "w"), so every process in a lane truncates it. A second case in this lane would
        // therefore race this one under `ctest -j`, and the shape of the failure is a silent, empty
        // read that looks exactly like "the counters were never emitted". Splitting the plumbing
        // assertion into its own case would have bought a clearer failure message and paid for it
        // with a flake in the thing the message is about. The plumbing is asserted first, with its
        // own message, inside this one process instead.
        TEST_F(CsoContentAddressingScenario, TheBlendToggleMintsBoundedlyWithContentAddressingAndPerBindWithout) {
            if (!Ready()) return;
            SkipUnlessTheLaneIsAssertableHere();
            if (IsSkipped()) return;

            Gl().EndFrame(); // close the setup window
            const Image first = RunBlendToggleFrame();
            const CsoWindow window = LastCsoWindow(ReadWholeFile(LibraryLogPath()));
            // The plumbing first, with its own message, so a counter-ratio failure below can never
            // be confused with "the lane never turned the stats channel on".
            ASSERT_TRUE(window.found)
                << "no 'MGPipe stats:' line carrying cso[csom= csob=] in " << LibraryLogPath()
                << ". This IS a push build (the lane checked MGITEST_PIPE_PUSH_BUILD before getting "
                   "here) and the cso[] bracket is unconditional inside that #if, so it cannot be "
                   "missing for a build reason: either MOBILEGL_PIPE_STATS / "
                   "MOBILEGL_PIPE_STATS_PERIOD did not reach the process, or no summary line was "
                   "emitted at all because nothing reached PipeStats::OnPresent.";
            RecordProperty("cso_line", window.line.c_str());

            // Every draw in the frame changed the pipeline subset, so every draw is a bind. This
            // is the denominator both arms are read against; without it, "csom == csob" would also
            // be satisfied by a frame in which neither happened at all.
            ASSERT_GE(window.binds, static_cast<long long>(kDrawsPerFrame))
                << "the toggle frame issued " << kDrawsPerFrame
                << " draws whose pipeline subset alternates, so it must have issued at least that many "
                   "render-state binds. It reported: "
                << window.line;

            if (m_lane == kLaneContentAddressed) {
                EXPECT_LE(window.mints, kMaxDistinctCsos)
                    << "with content addressing on, enable/draw/disable/draw x " << kTogglePairs
                    << " visits two distinct pipeline subsets and must mint a bounded number of CSOs, then "
                       "reuse them. It reported: "
                    << window.line;
                EXPECT_LT(window.mints, window.binds)
                    << "with content addressing on the cache must be answering binds it did not mint. "
                    << window.line;
            } else if (m_lane == kLaneNoContentAddressing) {
                EXPECT_EQ(window.mints, window.binds)
                    << "kMGPipeBehaviourNoCsoContentAddressing (bit 63 of MOBILEGL_PIPE_PUSH) must make every "
                       "bind mint a fresh CSO - the map is never probed and no handle is ever reused. Equal "
                       "counters are the only reading that proves the bit STEERED anything: if it were "
                       "ignored, this arm would report the same bounded mint count as the other one. It "
                       "reported: "
                    << window.line;
            } else {
                FAIL() << "unknown " << kLaneMarker << " value '" << m_lane << "'";
            }

            // ... and the picture is the same in both arms and after every toggle. The quad is
            // opaque, so the blended and unblended draws agree by construction.
            EXPECT_TRUE(RegionIsMostly(first, kInset, first.Width() - kInset, kInset, first.Height() - kInset,
                                       "green", 0.0, "the blend-toggle frame [" + m_lane + "]"));
            const Image second = RunBlendToggleFrame();
            EXPECT_TRUE(second == first)
                << "the second toggle frame does not match the first: " << second.ByteDiffCount(first)
                << " bytes differ. The CSO path must not change what is drawn.";
        }

    } // namespace
} // namespace MGITest
