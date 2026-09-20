// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/TriangleScenario.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - THE SMALLEST THING THAT DRAWS: one VBO, one program, one VAO, a clear, a
// VBO-backed glDrawArrays and a glReadPixels.
//
// This is target B of P5's reduced path (BRIEF-P5 4). It is an ORDINARY GL scenario and runs
// in every lane; the DirectGLES.Split. entries run the same two cases with
// MOBILEGL_TRANSPORT=inproc, where the same body is the smallest workload that crosses a real
// ring. Four things about its shape are decisions rather than defaults, and all four come from
// the measured verb census (~/w7/notes/p5/verb-census.md):
//
// 1. THE DRAW IS VBO-BACKED AND HAS NO CLIENT-ARRAY INDICES. glDrawArrays against a buffer
//    bound to GL_ARRAY_BUFFER, never glDrawElements with a client pointer. That keeps
//    kDrawHasUserIndices' MGHostSpan out of the first IPC frame entirely - the split filling of
//    a host span is P8 - and it is why table 0 can pin kCapNeedsHostIndexBytes and
//    kCapNeedsHostUboBytes at 0 for the whole of P5.
//
// 2. THERE IS NO glFlush, AND ITS ABSENCE IS THE POINT. `Flush` is not a GLFunctionsTable slot
//    at all, and MG_Impl/GLImpl/Exporting/Definitions.cpp:111-112 makes glFlush() and glFinish()
//    LITERALLY EMPTY BODIES - one MGLOG_D and a return. A scenario that called glFlush to order
//    its readback would be ordering nothing and would still pass, which makes the ordering it
//    believes in unfalsifiable. glReadPixels IS the ordering point: it is a blocking readback on
//    both backends today and a SEG_REPLY round trip under split, so the pixels it returns are
//    the pixels the draw produced or the case fails.
//
// 3. THE FIRST CASE ENDS WITH EndFrame(), DELIBERATELY. `Present` has ZERO call sites in
//    MG_Impl - it is reached only through EGLImpl.cpp:178 -> BackendObject.cpp:396 - so a
//    scenario that never swaps never touches it, and B would then exercise a STRICT SUBSET of
//    what ClearThenReadPixelsScenario already covers. BRIEF-P5 4.B lists Present(67) among the
//    catalogue rows this target needs, so the frame boundary is here on purpose: B is A's slot
//    set plus DrawArrays, not minus Present.
//
// 4. THE SECOND CASE REDRAWS ACROSS A FRAME BOUNDARY WITHOUT REBUILDING ANYTHING. The VBO, the
//    program and the VAO outlive the swap and only the clear colour changes. Under split that
//    is the difference between "the client re-declares every object every frame" (which would
//    pass a single-frame test and be the whole cost of the design) and a steady state; under
//    monolith it is the backend's per-frame retire/aging path, which is exactly where P3a's
//    respecify bug lived.

#include <string>
#include <vector>

#include "../Harness/HeadlessGL.h"
#include "../Harness/PipeStatsWindow.h"
#include "../Harness/ScenarioFixture.h"
#include "../Harness/SplitLane.h"
#include "../Harness/SplitRuntimePeek.h"
#include "../Harness/WireLedgerChecks.h"

#ifdef GLAPI
#undef GLAPI
#endif
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glcorearb.h>
#undef GL_GLEXT_PROTOTYPES

namespace MGITest {
    namespace {

        // #version 330 core, because that is what the retrace lane's
        // MESA_GLSL_VERSION_OVERRIDE pins and what every other scenario in this module that does
        // not need a later feature uses.
        constexpr const char* kVertexSource = R"(#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec3 aColor;
out vec3 vColor;
void main() {
    vColor = aColor;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

        constexpr const char* kFragmentSource = R"(#version 330 core
in vec3 vColor;
out vec4 oColor;
void main() { oColor = vec4(vColor, 1.0); }
)";

        struct Vertex {
            float x, y;
            float r, g, b;
        };

        // A single triangle with its base at y = -0.8 and its apex at y = +0.8, so the
        // interior box the cases assert on (the middle 10% of the width, a fifth of the way up)
        // is far from every edge and the corner box they assert the CLEAR on is far outside it.
        // Flat green: one colour over the whole primitive means an offender pixel is a real
        // disagreement rather than an interpolation rounding difference between two drivers.
        constexpr Vertex kTriangle[3] = {
            {-0.8f, -0.8f, 0.0f, 1.0f, 0.0f},
            {0.8f, -0.8f, 0.0f, 1.0f, 0.0f},
            {0.0f, 0.8f, 0.0f, 1.0f, 0.0f},
        };

        class TriangleScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                // Ready() is false both when there is no GPU and when the base SetUp skipped a
                // Split lane that has no client to assert against (ScenarioFixture.h).
                if (!Ready()) return;

                std::string error;
                m_program = CompileProgram(kVertexSource, kFragmentSource, &error);
                ASSERT_NE(m_program, 0u) << error;

                glGenVertexArrays(1, &m_vao);
                glBindVertexArray(m_vao);
                glGenBuffers(1, &m_vbo);
                glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
                glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(sizeof(kTriangle)), kTriangle, GL_STATIC_DRAW);
                glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), nullptr);
                glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                                      reinterpret_cast<void*>(2 * sizeof(float)));
                glEnableVertexAttribArray(0);
                glEnableVertexAttribArray(1);
                ASSERT_EQ(FirstGLError(), 0u) << "building the one VBO and one VAO this scenario has";
            }

            void TearDown() override {
                if (!Ready() || IsSkipped()) return;
                glBindVertexArray(0);
                glBindBuffer(GL_ARRAY_BUFFER, 0);
                if (m_vbo != 0) glDeleteBuffers(1, &m_vbo);
                if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
                if (m_program != 0) glDeleteProgram(m_program);
                m_vbo = m_vao = m_program = 0;
            }

            // Clear, draw, read back. No glFlush between the draw and the readback: see the
            // file header, point 2.
            Image ClearThenDrawThenRead(float clearR, float clearG, float clearB) {
                HeadlessGL& gl = Gl();
                BindDefaultFramebuffer();
                glViewport(0, 0, gl.Width(), gl.Height());
                glDisable(GL_SCISSOR_TEST);
                glDisable(GL_DEPTH_TEST);
                ClearTo(clearR, clearG, clearB, 1.0f);
                glUseProgram(m_program);
                glBindVertexArray(m_vao);
                glDrawArrays(GL_TRIANGLES, 0, 3);
                return ReadPixels(gl.Width(), gl.Height());
            }

            // The box inside the triangle: the middle tenth of the width, a fifth of the way up
            // from the base, which is interior for the vertex set above at any surface size the
            // harness uses.
            void ExpectTriangleInterior(const Image& image, const char* color, const std::string& when) {
                const int w = image.Width();
                const int h = image.Height();
                EXPECT_TRUE(RegionIsMostly(image, (w * 45) / 100, (w * 55) / 100, (h * 20) / 100,
                                           (h * 30) / 100, color, 0.0, when));
            }

            // The bottom-left corner, which is below the triangle's base and left of its left
            // edge, so it carries the clear and nothing else.
            void ExpectClearedCorner(const Image& image, const char* color, const std::string& when) {
                const int w = image.Width();
                const int h = image.Height();
                EXPECT_TRUE(RegionIsMostly(image, 0, (w * 5) / 100, 0, (h * 5) / 100, color, 0.0, when));
            }

            // EXIT GATE E3(e)'s DRIVE LOOP. Ordinary GL through this scenario's own objects -
            // a clear and a VBO-backed draw per iteration, no readback (a readback is a
            // SEG_REPLY round trip per iteration and would make this cost seconds rather than
            // milliseconds) - repeated until the producer has written more bytes into SEG_CMD
            // than the lane's ring holds. Returns the bytes this loop drove.
            //
            // NOTHING HERE TOUCHES THE RING DIRECTLY. The loop's only input is the producer's
            // own head cursor, read through Harness/SplitRuntimePeek, and its only output is
            // GL calls the scenario already makes. R-16: an assertion may not construct the
            // state it observes, and "the workload makes the ring wrap" is a different claim
            // from "a test can make the ring wrap".
            unsigned long long DriveUntilSmallRingOverruns() {
                const unsigned long long before = WireLedger::CmdBytesWritten();
                unsigned long long driven = 0;
                for (unsigned int i = 0; i < WireLedger::kSmallRingLaneMaxIterations; ++i) {
                    ClearTo(0.0f, 0.0f, (i & 1u) ? 1.0f : 0.0f, 1.0f);
                    glUseProgram(m_program);
                    glBindVertexArray(m_vao);
                    glDrawArrays(GL_TRIANGLES, 0, 3);
                    // Cheap: this is a member read on the encoder, not a wire round trip.
                    driven = WireLedger::CmdBytesWritten() - before;
                    if (driven > WireLedger::kSmallRingLaneCmdByteTarget) break;
                }
                return driven;
            }

            unsigned int m_program = 0;
            unsigned int m_vao = 0;
            unsigned int m_vbo = 0;
        };

    } // namespace

    // The reduced path's target B, in one case: GetCaps (reached by the first glCompileShader of
    // the context, not by any verb - verb-census trap 2), Clear, DrawArrays, ReadPixels, Present.
    TEST_F(TriangleScenario, AVboBackedTriangleReachesReadPixels) {
        if (!Ready() || IsSkipped()) return;

        const Image image = ClearThenDrawThenRead(0.0f, 0.0f, 1.0f);
        EXPECT_EQ(FirstGLError(), 0u);
        ExpectTriangleInterior(image, "green", "the interior of a VBO-backed glDrawArrays triangle");
        ExpectClearedCorner(image, "blue", "the corner outside the triangle, which carries the clear");

        // The frame boundary, deliberately (file header, point 3): this is the only thing in the
        // scenario that reaches the Present slot.
        Gl().EndFrame();
    }

    // Steady state: the same VBO, program and VAO across a swap, with only the clear colour
    // changing. Nothing is re-created, so a client that re-declared its objects every frame and
    // a backend that lost them at the frame boundary both show up here and in no single-frame
    // case.
    TEST_F(TriangleScenario, TheSameVboAndVaoRedrawAcrossAFrameBoundary) {
        if (!Ready() || IsSkipped()) return;

        const Image first = ClearThenDrawThenRead(0.0f, 0.0f, 1.0f);
        ExpectTriangleInterior(first, "green", "frame 0's triangle");
        ExpectClearedCorner(first, "blue", "frame 0's clear");
        Gl().EndFrame();

        // Second frame: black clear, nothing else touched.
        const Image second = ClearThenDrawThenRead(0.0f, 0.0f, 0.0f);
        EXPECT_EQ(FirstGLError(), 0u);
        ExpectTriangleInterior(second, "green",
                               "frame 1's triangle, drawn from the SAME VBO and VAO with no "
                               "re-specification of either");
        ExpectClearedCorner(second, "black", "frame 1's clear, which is the only thing that changed");
        Gl().EndFrame();

        // ---- the split lanes' two readings of the wire producer's ledger --------------------
        //
        // They are HERE, at the end of the steady-state case, and not in a case of their own,
        // for a reason that is about the gate and not about tidiness: `integration-split` is a
        // NAMED census (19 ran / 2 skipped by design) and a new entry moves it, so the phase
        // would have to re-baseline a number the joint gate just pinned. The measurement wants
        // this workload anyway - BRIEF 8 item 3 names this case - and a reading taken after the
        // case's own pixel assertions is a reading over a run that is known to have been
        // correct.
        //
        // Both are skipped, loudly and by the same predicate every other split-only assertion
        // uses, in the monolith lanes: there is no encoder there, and every field of the
        // ledger reads 0.
        const std::string skip = SplitLane::SkipReasonForSplitOnlyAssertions();
        if (!skip.empty()) {
            RecordProperty("wire_ledger_skip_reason", skip);
            return;
        }

        // R-10's proof obligation over target B. Published in every split lane, small ring
        // included - the cap moves with MOBILEGL_IPC_RING_MB, so the SmallRing lane is also the
        // arm where a record closest to its cap would show up first.
        WireLedger::ExpectMaxRecordBytesUnderCap(
            "TriangleScenario.TheSameVboAndVaoRedrawAcrossAFrameBoundary");

        // Exit gate E3(e). Only the small-ring lane drives the overrun: at the default 8 MiB
        // the same loop would take eight times as long to say the same thing, and the point of
        // the lane is that IT is the arm with a ring the workload can fill.
        if (SplitLane::IsSmallRingLane()) {
            const unsigned long long driven = DriveUntilSmallRingOverruns();
            // Ordinary uploads, each fitting by itself, jointly exceed the lane's 1 MiB
            // staging segment. Delay only scheduling between applied and retired: the
            // production allocator, not this test, must observe capacity and wait.
            GLuint pressureBuffer = 0;
            glGenBuffers(1, &pressureBuffer);
            glBindBuffer(GL_COPY_WRITE_BUFFER, pressureBuffer);
            std::vector<unsigned char> upload(768 * 1024, 0x5a);
            glBufferData(GL_COPY_WRITE_BUFFER, upload.size(), nullptr, GL_DYNAMIC_DRAW);
            DelaySplitRetirementForTesting(true);
            glBufferSubData(GL_COPY_WRITE_BUFFER, 0, upload.size(), upload.data());
            upload[0] = 0xa5;
            glBufferSubData(GL_COPY_WRITE_BUFFER, 0, upload.size(), upload.data());
            DelaySplitRetirementForTesting(false);
            glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
            glDeleteBuffers(1, &pressureBuffer);
            Gl().EndFrame();
            WireLedger::ExpectSmallRingWrappedAtLeastOnce(
                "TriangleScenario.TheSameVboAndVaoRedrawAcrossAFrameBoundary", driven);
        }
    }

    // ===================================================================================
    // P5e (gl), ID-115 / ID-119: THE STRICT LANE'S POSITIVE CONTROL
    // ===================================================================================
    //
    // WHY THE LANE NEEDS ONE AT ALL. Of the seven entries that passed `integration-split` under
    // MOBILEGL_IPC_STRICT_ERRORS=1 before this phase, three were MOBILEGL_TRANSPORT=monolith -
    // where the server never stamps a verb boundary, so the entire strict mechanism is
    // structurally unreachable - two self-skipped, one was a death test and one was a Python
    // check. Not one was a record-carrying split GL scenario. So "116/116 under strict" and
    // "strict was never armed on anything that draws" were indistinguishable from outside, and
    // a gate that cannot tell those apart is not a gate (ID-115).
    //
    // WHAT IT ASSERTS, AND WHY EACH PART IS NECESSARY.
    //   `vbs` > 0   - the server stamped at least one verb boundary IN THIS ENTRY. Everything
    //                 strict checks is downstream of that stamp. This is the arming proof and
    //                 it is deliberately not rsp: rsp goes to ZERO exactly when the phase
    //                 SUCCEEDS, so a control built on it would start failing on the day the
    //                 debt is paid.
    //   `draws` > 0 - and the entry really drew, so "armed" is not "armed on a clear".
    //   rsp vs draws (ID-119) - the old pin, "rsp = 0 on unbarriered records", was VACUOUS: the
    //                 unbarriered arm of CountBarrierPull is [[noreturn]] and runs before the
    //                 counter, so no implementation could ever have made it false. What is
    //                 checkable is the SHAPE: while the draw path's debt is unretired every draw
    //                 pulls, so rsp scales with draws; when it retires, rsp goes to 0. The
    //                 measured device numbers are the same statement - rsp was ~852 (the draw
    //                 count) under inproc and 0 under monolith. So the assertion is "rsp is 0 or
    //                 it scales with draws", and the retirement is visible as the transition.
    //
    // THE WINDOW IS READ, NOT INFERRED. `found == false` means the stats channel never reached
    // this process, which is a DIFFERENT failure from "the counter read zero" and is reported as
    // one - otherwise a lane that lost MOBILEGL_PIPE_STATS would report the arming proof as a
    // clean zero. The lane entry that runs this case owns its own MOBILEGL_LOG_FILE_PATH for
    // PipeStatsWindow.h's reason: the library opens it "w", so two entries sharing a path under
    // `ctest -j` race into an empty read that looks exactly like "never emitted".
    TEST_F(TriangleScenario, TheServerStampedAVerbBoundaryOnThisDrawingFrame) {
        if (!Ready() || IsSkipped()) return;

        const std::string skip = SplitLane::SkipReasonForSplitOnlyAssertions();
        if (!skip.empty()) {
            // The monolith arms' skip twin (G2/G14), and it is LOUD rather than silent because
            // this case's whole subject is a counter that is zero there BY CONSTRUCTION -
            // nothing stamps a verb boundary in a monolith build, and a case that "passed"
            // by reading that zero would be the exact confusion it exists to prevent.
            RecordProperty("strict_arming_skip_reason", skip);
            GTEST_SKIP() << "the server verb stamp exists only under split: " << skip;
            return;
        }
        // ONE LANE OWNS THIS CASE, and it declares itself by name the way MGITEST_PMAP_LANE does.
        // TriangleScenario.* is discovered by several blocks (the plain split lane, the small-ring
        // lane, the monolith lanes), and every one of them gives its entries a private
        // MOBILEGL_LOG_FILE_PATH - so "is there a log path" cannot distinguish the lane that
        // asked for counters from the lanes that merely have somewhere to log. Reading the window
        // in those would assert on a summary line nobody enabled.
        if (!SplitLane::MarkerIsOne("MGITEST_STRICT_ARMING_LANE")) {
            GTEST_SKIP() << "not the strict-arming lane: this case reads a PipeStats window and "
                            "only DirectGLES.Split.StrictArming. sets MOBILEGL_PIPE_STATS=1, "
                            "MOBILEGL_PIPE_STATS_PERIOD=1 and a log path of its own";
            return;
        }
        if (PipeStatsWindow::LibraryLogPath().empty()) {
            GTEST_SKIP() << "the lane configured no MOBILEGL_LOG_FILE_PATH, and the library's "
                            "summary line is the only channel this module has for PipeStats";
            return;
        }

        // Close the setup window, run the workload, close it again: the LAST line then covers
        // the draw and nothing else (PipeStatsWindow.h's "since the previous line").
        Gl().EndFrame();
        const Image image = ClearThenDrawThenRead(0.0f, 0.0f, 1.0f);
        EXPECT_EQ(FirstGLError(), 0u);
        ExpectTriangleInterior(image, "green", "the drawing frame the counters are taken over");
        Gl().EndFrame();
        // Run-ahead returns from Present before its stats window is published.
        // Wait for this emitted Present, so a fast client cannot sample the
        // preceding empty setup frame and falsely report an unarmed server.
        ASSERT_TRUE(WaitForSplitAppliedForTesting(PeekSplitRuntime().emitSeq));

        const PipeStatsWindow::Window window = PipeStatsWindow::LastFromLaneLog();
        ASSERT_TRUE(window.found)
            << "the library emitted no `MGPipe stats:` line, so this lane's arming proof is "
               "MISSING rather than zero - check MOBILEGL_PIPE_STATS=1, "
               "MOBILEGL_PIPE_STATS_PERIOD=1 and this entry's private MOBILEGL_LOG_FILE_PATH";

        const long long draws = PipeStatsWindow::CounterOrAbsent(window, "draws");
        const long long stamps = PipeStatsWindow::CounterOrAbsent(window, "vbs");
        const long long residual = PipeStatsWindow::CounterOrAbsent(window, "rsp");
        RecordProperty("pipe_stats_window", window.line);
        RecordProperty("server_verb_boundaries", std::to_string(stamps));
        RecordProperty("residual_pulls", std::to_string(residual));

        EXPECT_GT(draws, 0) << "no draw reached the instrumented entry point in the window this "
                               "case took, so there is nothing for the arming proof to be about: "
                            << window.line;
        EXPECT_GT(stamps, 0)
            << "the server stamped NO verb boundary on a drawing split entry. Every strict check "
               "- the poison, rsp, the BARRIER-PULLED verdict - is downstream of that stamp, so "
               "a green strict lane containing only entries like this one would mean the knob "
               "was never armed rather than that the debt was paid (ID-115): "
            << window.line;
        EXPECT_EQ(residual, 0)
            << "P5f exit: a drawing frame still reads client residual state: " << window.line;
    }

} // namespace MGITest
