// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/ResourceSubsystemControlScenario.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - THE P3a SUBSYSTEM A/B IS REAL (gate G12).
//
// P3a migrates two subsystems: kMGPipeSubsystemResources (bit 7, the resource_* family) and
// kMGPipeSubsystemVertexInput (bit 8, vertex elements / buffers / index). The push build's default
// mask becomes kMGPipeSubsystemsMigratedAtP3a = 0x1ff, and P2's 0x7f survives as the control that
// clears exactly those two bits - MGPipe.h:79's rule that every phase's constant keeps meaning what
// it meant, so an operator's recorded mask is still readable a phase later.
//
// That A/B is what every "push vs pull" number in MEASUREMENTS.md is taken against, and it has one
// characteristic failure mode: the bits stop steering anything, both arms run the same code, and
// every later comparison is quietly taken against a switch that does nothing. This file is the
// entry that cannot let that happen.
//
// WHAT IT ASSERTS, per arm:
//
//   on  (MOBILEGL_PIPE_PUSH=0x1ff)
//       The client emits map_persistent for every definition of a store past
//       BufferObject::TryAdoptLargeStorage's 16 MiB threshold, so the window's
//       map-persistent-roundtrips (`mpr=`) equals the number of definitions in it - one per
//       storage definition, mint or decline (D-B2).
//
//   off (MOBILEGL_PIPE_PUSH=0x7f, P2's default = P3a's subsystems cleared)
//       The frontend dispatch falls through to the legacy BufferBackendOps arm, nothing is emitted
//       through the resource family, and mpr= must read ZERO. This is the reading a dead switch
//       fails: with bit 7 ignored, this arm would report the same non-zero count as the other one.
//
//   both arms
//       THE PIXELS MUST NOT MOVE. The arena is filled with one solid-colour quad and drawn, and
//       both arms must read back that colour. "The counters moved and the picture did not" is the
//       whole claim - a switch that changed what is drawn would not be an A/B, it would be a bug.
//
// WHY IT CAN SKIP. The counter is emitted by the client-side resource tracker (P3a package B), and
// this file is written against the P3a contract commit, before that package lands. Until then
// nothing emits map_persistent, mpr= is structurally zero in BOTH arms, and an assertion about the
// difference would be a statement about nothing. The build answers the question rather than a
// hand-maintained list: MG_IntegrationTest/CMakeLists.txt greps every source under MG_Impl/Pipe/
// for the counter's name and passes the answer in as MGITEST_PIPE_RESOURCE_EMITTER_PRESENT, with a
// CONFIGURE_DEPENDS on that directory and on each file it finds so the answer cannot go stale. It
// is a CONTENT probe, not a filename probe, so the owning package keeps control of its own file
// layout - P3a's new client files are headers (D-N), and a glob for `ResourceTracker.cpp` would
// have kept this control skipping forever with a reason that had become false.
//
// DirectGLES ONLY, and that is the honest scope: P3a migrates Espryt's buffer and VAO paths.
// Magma's buffer path is P7 and registers no MGPipeResourceOps, so a DirectVulkan lane here would
// be measuring the client emitter against a backend that has not been asked to change - which is
// a real question, but it is P7's, not this control's.

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

        // Set by the two ResourceSubsystemControl. ctest entries and by nothing else; a harness
        // marker, never read by the library. Its absence means an ambient entry, where neither the
        // stats channel nor a private log path is configured.
        constexpr const char* kLaneMarker = "MGITEST_RESOURCE_SUBSYSTEM_LANE";
        constexpr const char* kLaneOn = "on";
        constexpr const char* kLaneOff = "off";

        // Past BufferObject::TryAdoptLargeStorage's 16 MiB threshold, so the store is offered for
        // adoption at all; the vertex payload sits deep inside it so a clamped or aliased adopted
        // range would miss it. Same shape as LargeArenaAdoptionScenario, deliberately: this
        // control's workload has to be one the buffer path really takes.
        constexpr GLsizeiptr kArenaBytes = GLsizeiptr(20) * 1024 * 1024;
        constexpr GLintptr kVertexOffset = GLintptr(16) * 1024 * 1024;
        // Two definitions and several draws each, so "one per definition", "one per draw" and
        // "none at all" are three different numbers.
        //
        // TWO ARENAS, EACH DEFINED ONCE, rather than one arena defined twice, and that is a
        // deliberate detour around a bug that is not this branch's: re-specifying an adopted
        // store while a VAO's attributes still read it leaves the backend VAO bound to the
        // retired store, which `dev`'s d7655247 ("rebind VAOs when an adopted buffer is
        // respecified - the immediate retire path forgot the buffer-id generation") fixes. That
        // commit is NOT in feat/disaggregated's history, and this workload reproduced it as a
        // hard SIGSEGV inside the vertex fetch on the first draw after the second definition.
        // A control that carried a pre-existing `dev` crash would be red for a reason that has
        // nothing to do with the subsystem bits it is measuring, so it defines a second arena
        // instead - which is the same number of STORAGE DEFINITIONS, and therefore the same
        // count, by ARCHITECTURE.md:474's own wording. The finding is recorded for the
        // integrator rather than fixed here (ROADMAP.md:88: unrelated fixes do not ride the
        // split work).
        constexpr int kDefinitionsInTheWindow = 2;
        constexpr int kDrawsPerDefinition = 3;
        constexpr int kInset = 2;

        constexpr const char* kVS = R"(#version 330 core
in vec2 aPos;
in vec3 aColor;
out vec3 vColor;
void main() {
    vColor = aColor;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

        constexpr const char* kFS = R"(#version 330 core
in vec3 vColor;
out vec4 oColor;
void main() { oColor = vec4(vColor, 1.0); }
)";

        struct Vertex {
            float x, y;
            float r, g, b;
        };

        std::vector<Vertex> Quad(float r, float g, float b) {
            return {
                {-1.0f, -1.0f, r, g, b}, {1.0f, -1.0f, r, g, b}, {1.0f, 1.0f, r, g, b},
                {-1.0f, -1.0f, r, g, b}, {1.0f, 1.0f, r, g, b},  {-1.0f, 1.0f, r, g, b},
            };
        }

        bool BuildMarkerIsSet(const char* name) {
            const char* value = std::getenv(name);
            return value != nullptr && value[0] == '1' && value[1] == '\0';
        }

        std::string LaneName() {
            const char* lane = std::getenv(kLaneMarker);
            return lane != nullptr ? std::string(lane) : std::string();
        }

        class ResourceSubsystemControlScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                m_lane = LaneName();
                std::string error;
                m_program = CompileProgram(kVS, kFS, &error);
                ASSERT_NE(m_program, 0u) << error;

                // The VAO only. The arenas are created and defined inside the counted window -
                // the window a summary line reports is "since the previous line", so a
                // definition taken in SetUp would be counted in a window this case does not
                // control - and their attribute pointers are declared only once each store
                // exists, because an attribute whose offset is 16 MiB into a store that has not
                // been defined yet is a range no driver has to accept.
                glGenVertexArrays(1, &m_vao);
                glBindVertexArray(m_vao);
                RecordProperty("lane", m_lane.empty() ? "ambient" : m_lane.c_str());
            }

            void TearDown() override {
                if (!Ready()) return;
                glUseProgram(0);
                glBindVertexArray(0);
                glBindBuffer(GL_ARRAY_BUFFER, 0);
                for (GLuint& arena : m_arenas) {
                    if (arena != 0) glDeleteBuffers(1, &arena);
                    arena = 0;
                }
                if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
                if (m_program != 0) glDeleteProgram(m_program);
            }

            // GTEST_SKIP() returns from the function it is written in, so this cannot report
            // through a return value; the caller pairs it with `if (IsSkipped()) return;`.
            void SkipUnlessTheLaneIsAssertableHere() {
                if (m_lane.empty()) {
                    GTEST_SKIP() << "runs only in its own lane: the two ResourceSubsystemControl. ctest "
                                    "entries set MGITEST_RESOURCE_SUBSYSTEM_LANE together with the "
                                    "MOBILEGL_PIPE_PUSH bitmask that arm means, MOBILEGL_PIPE_STATS=1, "
                                    "MOBILEGL_PIPE_STATS_PERIOD=1 and a private MOBILEGL_LOG_FILE_PATH. "
                                    "None of that is configured in the ambient entries, and the ambient "
                                    "log is shared, so a read here would race.";
                    return;
                }
                if (!BuildMarkerIsSet("MGITEST_PIPE_PUSH_BUILD")) {
                    GTEST_SKIP() << "this library was built without MOBILEGL_PIPE_PUSH: there are no "
                                    "subsystem bits to clear, CallClass::MapPersistentRoundtrips does "
                                    "not exist and the summary line carries no mpr=. The entry is "
                                    "registered here anyway so that `ctest -L integration-gpu` names the "
                                    "same tests in the pull build and the push build (gate G2).";
                    return;
                }
                if (!BuildMarkerIsSet("MGITEST_PIPE_RESOURCE_EMITTER_PRESENT")) {
                    GTEST_SKIP() << "subsystem not implemented on this tree: no source under "
                                    "MobileGL/MG_Impl/Pipe/ names MapPersistentRoundtrips, so nothing "
                                    "emits map_persistent, mpr= is structurally zero in BOTH arms and "
                                    "the difference between them is not observable yet. P3a package B "
                                    "owns the client-side resource tracker; this control arms itself "
                                    "when it lands, whatever file that package puts the emitter in.";
                    return;
                }
                if (PipeStatsWindow::LibraryLogPath().empty()) {
                    GTEST_SKIP() << "the lane configured no MOBILEGL_LOG_FILE_PATH, and the library's "
                                    "summary line is the only channel this module has for reading "
                                    "PipeStats";
                    return;
                }
            }

            // ONE storage definition - the NULL-data glBufferData past the adoption threshold,
            // which is Minecraft's arena-creation idiom and the adoption point - then the
            // attribute pointers into it and a few draws. Entirely inside one frame, so one
            // summary window covers exactly this.
            void DefineAnArenaAndDrawFromIt(int index, float r, float g, float b) {
                glGenBuffers(1, &m_arenas[static_cast<std::size_t>(index)]);
                glBindBuffer(GL_ARRAY_BUFFER, m_arenas[static_cast<std::size_t>(index)]);
                glBufferData(GL_ARRAY_BUFFER, kArenaBytes, nullptr, GL_DYNAMIC_DRAW);
                const std::vector<Vertex> vertices = Quad(r, g, b);
                glBufferSubData(GL_ARRAY_BUFFER, kVertexOffset,
                                GLsizeiptr(vertices.size() * sizeof(Vertex)), vertices.data());
                glBindVertexArray(m_vao);
                glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                                      reinterpret_cast<void*>(kVertexOffset));
                glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                                      reinterpret_cast<void*>(kVertexOffset + 2 * sizeof(float)));
                glEnableVertexAttribArray(0);
                glEnableVertexAttribArray(1);
                glUseProgram(m_program);
                for (int draw = 0; draw < kDrawsPerDefinition; ++draw) {
                    glDrawArrays(GL_TRIANGLES, 0, 6);
                }
            }

            std::string m_lane;
            GLuint m_program = 0;
            GLuint m_vao = 0;
            std::vector<GLuint> m_arenas =
                std::vector<GLuint>(static_cast<std::size_t>(kDefinitionsInTheWindow), 0u);
        };

        // ONE case per lane, and it is a constraint rather than a preference: this case READS the
        // library log, the log is a per-LANE resource (the library opens it fopen(path, "w"), so
        // every process in a lane truncates it), and a second case here would race this one under
        // `ctest -j` with a failure indistinguishable from "the counter was never emitted". The
        // plumbing is asserted first, with its own message, inside this one process.
        TEST_F(ResourceSubsystemControlScenario, ClearingTheP3aBitsStopsTheEmissionsAndNotThePixels) {
            if (!Ready()) return;
            SkipUnlessTheLaneIsAssertableHere();
            if (IsSkipped()) return;

            BindDefaultFramebuffer();
            Gl().EndFrame(); // close the setup window: everything below is one window

            ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
            for (int definition = 0; definition < kDefinitionsInTheWindow; ++definition) {
                DefineAnArenaAndDrawFromIt(definition, 0.0f, 1.0f, 0.0f);
                ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR))
                    << "arena definition " << definition << " left a GL error behind";
            }
            const Image image = ReadPixels(Gl().Width(), Gl().Height());
            Gl().EndFrame(); // the swap that emits the window covering exactly the work above

            const PipeStatsWindow::Window window = PipeStatsWindow::LastFromLaneLog();
            ASSERT_TRUE(window.found)
                << "no 'MGPipe stats:' line in " << PipeStatsWindow::LibraryLogPath()
                << ". This IS a push build (the lane checked MGITEST_PIPE_PUSH_BUILD before getting "
                   "here), so either MOBILEGL_PIPE_STATS / MOBILEGL_PIPE_STATS_PERIOD did not reach the "
                   "process, or no summary line was emitted at all because nothing reached "
                   "PipeStats::OnPresent.";
            RecordProperty("stats_line", window.line.c_str());

            const long long roundtrips = PipeStatsWindow::CounterOrAbsent(window, "mpr");
            ASSERT_GE(roundtrips, 0)
                << "the summary line carries no mpr= field, so this build's PipeStats has no "
                   "map-persistent-roundtrips counter to read: "
                << window.line;

            if (m_lane == kLaneOn) {
                EXPECT_EQ(roundtrips, static_cast<long long>(kDefinitionsInTheWindow))
                    << "with bits 7|8 SET the resource family is the path a store definition takes, so "
                       "each of the " << kDefinitionsInTheWindow
                    << " definitions in this window is one map_persistent emission (mint or decline - "
                       "both need an answer from the resource owner, D-B2). "
                    << (kDefinitionsInTheWindow * kDrawsPerDefinition)
                    << " would mean an acquisition per draw, and 0 would mean the emission never "
                       "happened on the arm that is supposed to do it. It reported: "
                    << window.line;
            } else if (m_lane == kLaneOff) {
                EXPECT_EQ(roundtrips, 0)
                    << "with bits 7|8 CLEARED (MOBILEGL_PIPE_PUSH=0x7f, P2's default) the frontend "
                       "dispatch must fall through to the legacy BufferBackendOps arm and emit nothing "
                       "through the resource family, so mpr= must be zero. A non-zero count here is the "
                       "dead-switch reading: the bits are being ignored, both arms run the same code, "
                       "and every push-vs-pull number taken against this A/B is measuring one arm twice. "
                       "It reported: "
                    << window.line;
            } else {
                FAIL() << "unknown " << kLaneMarker << " value '" << m_lane
                       << "': the arms are on / off. Reading an unrecognised name as either would make "
                          "this lane assert the other arm's expectation while claiming to test this one.";
            }

            // ... and the picture is the same whichever arm ran. The arena is drawn with one solid
            // colour, so both arms must read back exactly that.
            EXPECT_TRUE(RegionIsMostly(image, kInset, image.Width() - kInset, kInset,
                                       image.Height() - kInset, "green", 0.0,
                                       "the arena draw [" + m_lane + "]"))
                << "the subsystem bits changed what is DRAWN, which is not an A/B - the handle path and "
                   "the legacy path must produce the same pixels from the same arena.";
        }

    } // namespace
} // namespace MGITest
