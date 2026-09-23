// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/PipeVerifyArmingScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - THE MOBILEGL_PIPE_VERIFY COMPARATOR IS ARMED, AND SAYS SO, AND CAN GO RED.
//
// The third CI mode (ARCHITECTURE.md 13.2-(2)) runs the whole integration suite with two state
// models in one address space: the PipeInputs block the frontend fills at every verb boundary,
// and a SnapshotFromGLContext() taken from the live GLContext. A green run of that mode is only
// worth something if the comparator was actually RUNNING - and "MOBILEGL_PIPE_VERIFY=1 against a
// library that was not built with -DMOBILEGL_PIPE_VERIFY=ON" is a no-op that looks exactly like a
// clean pass. That is the failure mode this scenario exists to make impossible:
//
//   Armed                    - the environment says the comparator is on for this process, so the
//                              library must SAY it armed. It asserts a library observable against
//                              the environment, the same shape UnlocatedIoBlockScenario's arming
//                              case and AsyncCompileScenario::ExtensionStringMatchesTheConfiguration
//                              use. A lane whose library never armed FAILS here; it never passes.
//   CorruptedFieldIsReported - the negative control for the comparator itself (gate G4). With
//                              MOBILEGL_PIPE_VERIFY_CORRUPT naming a field, the snapshot arm is
//                              perturbed before the entry compare, so a comparator that works must
//                              report Fatal{PipeVerifyDiffer, "<Field>@<Verb>"}. A comparator that
//                              compares nothing stays quiet and this case goes red.
//   CorruptedFieldIsReportedOnTheServerRead
//                            - the same control aimed at the OTHER comparator (P7 wave 3, V1).
//                              The entry compare runs on the client thread at a verb boundary; the
//                              compare-at-read hook runs on the server's apply thread at every
//                              backend read, and until this case existed nothing could tell a hook
//                              that had stopped comparing from a hook with nothing to say. It
//                              reads the SERVER role's half of the log, because with the knob
//                              armed the client reports the same field and a union search would be
//                              satisfied by the arm that is not under test.
//
// The observable is the library's own log, because MG_Config is not reachable from this module
// (on Android it links the SHIPPING libMobileGL.so, built -fvisibility=hidden) and the arming
// signal is a latched MGLOG_I. The ctest entry sets MOBILEGL_LOG_FILE_PATH; this only reads it.
//
// Note on scope, and why Armed runs in a lane of its own. The log file is opened with
// fopen(path, "w") at the first log write of a process (MG_Util/Debug/Log.cpp, InitFile), so each
// process TRUNCATES it. That is fine for one process and false for many: in the ambient Verify.
// lane, 400-odd sibling entries share the one MOBILEGL_LOG_FILE_PATH, and CI runs that lane with
// `ctest -j 4`, so a neighbour's bring-up can truncate the file between this case's draw and its
// read. Every existing scenario in this suite that reads the library log (UnlocatedIoBlockScenario,
// the primgen reroute, the point-size demotion) is registered in a FILTERED lane with a log path of
// its own for exactly that reason, and this case now follows them: it runs in the VerifyArming.
// entries, which set MGITEST_PIPE_ARMING_LANE=1 and their own log, and skips everywhere else.
//
// What that proves, stated honestly: the arming line is a property of (this library, this
// environment), not of an individual test body, and the VerifyArming. entry runs the same library
// with the same MOBILEGL_PIPE_VERIFY=1 as its ~400 ambient siblings. One process per backend is
// therefore the whole of the evidence available for "the lane armed" - the per-process claim the
// shared log CANNOT support, because it only ever holds the last writer.
//
// Within the process: the arming line is latched at the FIRST fill, which may be the harness
// bring-up rather than this test's draw, so the arming search is whole-file on purpose; the
// divergence search is restricted to the bytes this case appended, which is where a differ belongs.

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "../Harness/PipeStatsWindow.h"
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

        // The three strings the comparator contracts to print (the brief's D8 reporting shape).
        // They are spelled here once so a rename of either half is one compile-visible edit.
        constexpr const char* kArmedLine = "MGPipe: verify armed";
        constexpr const char* kDifferPrefix = "Fatal{PipeVerifyDiffer";
        constexpr const char* kUnmigratedPrefix = "Fatal{UnmigratedPipeInput";

        // Set by the VerifyArming. ctest entries and by nothing else. It is a HARNESS variable, not
        // a library knob (hence the MGITEST_ prefix): the library never reads it. It exists because
        // this case reads a log file, and a log file is a per-LANE resource - see the note at the
        // top of the file.
        constexpr const char* kArmingLaneMarker = "MGITEST_PIPE_ARMING_LANE";

        constexpr const char* kVS = R"(#version 330 core
in vec2 aPos;
void main() { gl_Position = vec4(aPos, 0.0, 1.0); }
)";

        constexpr const char* kFS = R"(#version 330 core
out vec4 o_color;
void main() { o_color = vec4(0.25, 0.5, 0.75, 1.0); }
)";

        // Reads the environment the way MG_ConfigLoader does (ScenarioFixture.h documents the
        // rule); a string knob is "set" when it is present and non-empty, which is exactly what
        // MG_ConfigLoader's QueryEnvVariable turns into a non-empty Features member.
        bool StringKnobIsSet(const char* name) {
            const char* value = std::getenv(name);
            return value != nullptr && *value != '\0';
        }

        class PipeVerifyArmingScenario : public ScenarioTest {
        protected:
            // The library log this process is writing, or an empty path when none was configured.
            static std::filesystem::path LibraryLogPath() {
                // P6: the path is a BASE NAME and the library writes one log per role; PipeStatsWindow
            // derives the suffix, so the rule lives in one place.
            return std::filesystem::path(MGITest::PipeStatsWindow::LibraryLogPath());
            }

            // ONE MARK PER ROLE. The library writes a log per role, so "how long is the log
            // right now" is two numbers; a single scalar applied to the concatenation would slide
            // by whatever the other role wrote in between and start the read mid-line.
            static MGITest::PipeStatsWindow::LogMark LibraryLogMark() {
                return MGITest::PipeStatsWindow::MarkLaneLog();
            }

            // BOTH ROLES. The arming diagnostics this case looks for are emitted by the
            // BACKEND, and under inproc the backend runs on the apply thread - the server role -
            // so the line lands in the server's log. Reading only the client's found nothing and
            // reported the emulation unarmed, which accused the product of a defect the reader
            // had invented.
            static std::string LibraryLogSince(const MGITest::PipeStatsWindow::LogMark& mark) {
                return MGITest::PipeStatsWindow::ReadLaneLogSince(mark);
            }

            static std::string LibraryLog() { return MGITest::PipeStatsWindow::ReadLaneLog(); }

            // One frame that crosses several verb boundaries: a clear (kClear), a draw (kDraw) and
            // a readback (kReadback). Three of the nine fill classes, so an entry compare that only
            // ran for one of them still has something to say.
            void DrawOneFrame() {
                HeadlessGL& gl = Gl();
                std::string error;
                const unsigned int program = CompileProgram(kVS, kFS, &error);
                ASSERT_NE(program, 0u) << error;

                static const float kQuad[] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
                GLuint vao = 0;
                GLuint vbo = 0;
                glGenVertexArrays(1, &vao);
                glBindVertexArray(vao);
                glGenBuffers(1, &vbo);
                glBindBuffer(GL_ARRAY_BUFFER, vbo);
                glBufferData(GL_ARRAY_BUFFER, sizeof(kQuad), kQuad, GL_STATIC_DRAW);
                glEnableVertexAttribArray(0);
                glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);

                BindDefaultFramebuffer();
                glViewport(0, 0, gl.Width(), gl.Height());
                ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
                glUseProgram(program);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

                Rgba8 pixel{};
                glReadPixels(gl.Width() / 2, gl.Height() / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &pixel);

                glBindVertexArray(0);
                glDeleteBuffers(1, &vbo);
                glDeleteVertexArrays(1, &vao);
                m_centre = pixel;
            }

            Rgba8 m_centre{};
        };

        // THE CASE THAT FAILS A LANE WHOSE LIBRARY NEVER ARMED.
        //
        // Every other entry in the integration-verify lane renders the same frames it renders in the
        // ambient lane and would be just as green against a library with no comparator compiled in -
        // which is precisely how a verify lane goes green having verified nothing. This case is the
        // one that cannot: the environment pins MOBILEGL_PIPE_VERIFY=1, therefore the library must
        // have said "MGPipe: verify armed" in its own log, and if it did not, the mode is not running.
        TEST_F(PipeVerifyArmingScenario, Armed) {
            if (!Ready()) return;

            if (!StringKnobIsSet(kArmingLaneMarker)) {
                GTEST_SKIP() << "this case reads the library's log file, so it runs in the VerifyArming. "
                                "lane, which owns a log path no other entry writes to. In the ambient "
                                "Verify. lane 400-odd entries share one path and each truncates it "
                                "(Log.cpp opens it \"w\"), so a whole-file read here would race a "
                                "neighbour under ctest -j 4. Set by the ctest entry, never by hand.";
            }
            if (AmbientQuirkFromEnvironment("MOBILEGL_PIPE_VERIFY") != AmbientQuirk::On) {
                GTEST_SKIP() << "this case needs MOBILEGL_PIPE_VERIFY=1 for the whole process, which is "
                                "what the Verify. ctest entries set; with the variable unset the "
                                "comparator is dormant even in a build that compiled it in";
            }
            if (LibraryLogPath().empty()) {
                GTEST_SKIP() << "MOBILEGL_PIPE_VERIFY is pinned on but MOBILEGL_LOG_FILE_PATH is not "
                                "set, so the library has nowhere to record that it armed; the Verify. "
                                "ctest entries set both";
            }
            if (StringKnobIsSet("MOBILEGL_PIPE_VERIFY_CORRUPT")) {
                GTEST_SKIP() << "MOBILEGL_PIPE_VERIFY_CORRUPT is armed in this process, so a divergence "
                                "is the EXPECTED outcome and asserting on its absence here would be "
                                "backwards; the VerifyCorrupted. lane owns that half";
            }

            const MGITest::PipeStatsWindow::LogMark before = LibraryLogMark();
            ASSERT_NO_FATAL_FAILURE(DrawOneFrame());
            EXPECT_EQ(FirstGLError(), 0u);

            // Whole file, not just the appended bytes: the arming line is latched at the FIRST fill
            // of the process, which may already have happened during the harness bring-up. The file
            // is truncated at this process's first log write, so it still carries nothing else.
            const std::string whole = LibraryLog();
            EXPECT_NE(whole.find(kArmedLine), std::string::npos)
                << "MOBILEGL_PIPE_VERIFY=1 is set for this process and a frame was cleared, drawn and "
                   "read back, and the library never reported arming the comparator. Either this "
                   "library was not built with -DMOBILEGL_PIPE_VERIFY=ON (in which case the whole lane "
                   "is verifying nothing), or the arming MGLOG_I is gone. Log:\n"
                << whole;

            const std::string appended = LibraryLogSince(before);
            EXPECT_EQ(appended.find(kDifferPrefix), std::string::npos)
                << "the comparator reported a push/pull divergence on an ordinary frame:\n"
                << appended;
            EXPECT_EQ(appended.find(kUnmigratedPrefix), std::string::npos)
                << "a backend read a field the verb's fill table does not list (add the row to "
                   "MG_Pipe/FillPoints.def, never mark the field sticky):\n"
                << appended;
        }

        // NEGATIVE CONTROL A (gate G4): a deliberately corrupted snapshot field must turn a green
        // verify run red, naming that field and the verb it diverged on.
        //
        // It runs in its own lane (VerifyCorrupted.) because the knob is process-wide, and with
        // MOBILEGL_PIPE_VERIFY_FATAL=0 so the process survives its own divergence and this case can
        // read the report back out of the log. The CI step that runs the SAME knob against the
        // ambient lane - where FATAL keeps its default - asserts the other half: there, the
        // divergence must abort and ctest must go red.
        TEST_F(PipeVerifyArmingScenario, CorruptedFieldIsReported) {
            if (!Ready()) return;

            if (!StringKnobIsSet("MOBILEGL_PIPE_VERIFY_CORRUPT")) {
                GTEST_SKIP() << "this case is the comparator's negative control and needs "
                                "MOBILEGL_PIPE_VERIFY_CORRUPT=<FieldName> for the whole process, which "
                                "is what the VerifyCorrupted. ctest entries set";
            }
            if (AmbientQuirkFromEnvironment("MOBILEGL_PIPE_VERIFY") != AmbientQuirk::On) {
                GTEST_SKIP() << "MOBILEGL_PIPE_VERIFY_CORRUPT is set but MOBILEGL_PIPE_VERIFY is not, so "
                                "the comparator is dormant and there is nothing to corrupt";
            }
            if (LibraryLogPath().empty()) {
                GTEST_SKIP() << "MOBILEGL_LOG_FILE_PATH is not set, so the library has nowhere to report "
                                "the divergence; the VerifyCorrupted. ctest entries set both";
            }

            const std::string knob = std::getenv("MOBILEGL_PIPE_VERIFY_CORRUPT");
            const MGITest::PipeStatsWindow::LogMark before = LibraryLogMark();
            ASSERT_NO_FATAL_FAILURE(DrawOneFrame());

            const std::string appended = LibraryLogSince(before);
            const std::string expected = std::string(kDifferPrefix) + ", \"" + knob + "@";
            EXPECT_NE(appended.find(expected), std::string::npos)
                << "MOBILEGL_PIPE_VERIFY_CORRUPT=" << knob
                << " perturbs that field in the snapshot arm before every entry compare, so a working "
                   "comparator must have reported " << expected << "...\". It reported nothing, which "
                   "means the comparator is not comparing - and every green entry in this lane is "
                   "green for no reason. Log appended by this case:\n"
                << appended;
        }

        // NEGATIVE CONTROL A ON THE OTHER ARM (P7 wave 3, V1): the COMPARE-AT-READ hook, on the
        // SERVER's apply thread, inside the ID-49 neutral pack window.
        //
        // WHY THE CASE ABOVE DOES NOT COVER IT. MOBILEGL_PIPE_VERIFY_CORRUPT used to perturb only
        // EntryCompare's snapshot, so every red it could produce came from the CLIENT thread at a
        // verb boundary and said `where=entry`. The hook that runs on every backend read - the
        // whole of the split lane's per-read work, and the only comparator the server's apply
        // thread ever runs - had no falsifier: a hook that had stopped comparing looked exactly
        // like a hook with nothing to report, and the lane would have been just as green.
        //
        // WHY THIS FIELD AND THIS VERB. GetPixelStoreParameters@ReadPixels is where the hook does
        // its one piece of ARM-SPECIFIC reasoning (PipeFill.cpp's
        // ServerReadsInsideTheNeutralPackWindow): the applier reads the backend with the neutral
        // pack, so inside that window the oracle for the pack half is the neutral pack rather than
        // the live context. A control aimed anywhere else would leave exactly that branch unproven.
        //
        // THE SERVER HALF, NOT THE UNION. With this knob armed the client's own entry compare
        // reports `GetPixelStoreParameters@...` too, in the client's log, so a whole-lane search
        // would be satisfied without the server having run anything at all. The assertion below
        // reads the server role's half and nothing else.
        //
        // MOBILEGL_PIPE_VERIFY_FATAL=0, for CorruptedFieldIsReported's reason and one more: the
        // client's entry compare on the ReadPixels verb fires FIRST (kReadback's fill mask carries
        // this field, FillPoints.def), so with FATAL at its default the process would abort before
        // the server ever reached the read.
        TEST_F(PipeVerifyArmingScenario, CorruptedFieldIsReportedOnTheServerRead) {
            if (!Ready()) return;

            constexpr const char* kPackField = "GetPixelStoreParameters";
            const char* knob = std::getenv("MOBILEGL_PIPE_VERIFY_CORRUPT");
            if (knob == nullptr || std::string(knob) != kPackField) {
                GTEST_SKIP() << "this case is the compare-at-read hook's negative control and needs "
                                "MOBILEGL_PIPE_VERIFY_CORRUPT=" << kPackField << " for the whole "
                                "process, which is what the VerifySplitReadCorrupted. ctest entries "
                                "set; no other field is read by the server inside a window whose "
                                "oracle the hook rewrites";
            }
            if (AmbientQuirkFromEnvironment("MOBILEGL_PIPE_VERIFY") != AmbientQuirk::On) {
                GTEST_SKIP() << "MOBILEGL_PIPE_VERIFY_CORRUPT is set but MOBILEGL_PIPE_VERIFY is not, so "
                                "the comparator is dormant and there is nothing to corrupt";
            }
            if (LibraryLogPath().empty()) {
                GTEST_SKIP() << "MOBILEGL_LOG_FILE_PATH is not set, so the library has nowhere to report "
                                "the divergence; the VerifySplitReadCorrupted. ctest entries set both";
            }
            if (MGITest::PipeStatsWindow::ServerLibraryLogPath().empty()) {
                GTEST_SKIP() << "this build writes no server-role log, so there is no half in which the "
                                "server's own report could be told apart from the client's; the hook's "
                                "read arm is registered on the split (inproc) lane only";
            }

            const MGITest::PipeStatsWindow::LogMark before = LibraryLogMark();
            ASSERT_NO_FATAL_FAILURE(DrawOneFrame());

            const std::string server = MGITest::PipeStatsWindow::ReadServerLogSince(before);
            const std::string expected =
                std::string(kDifferPrefix) + ", \"" + kPackField + "@ReadPixels\"";
            const std::size_t at = server.find(expected);
            ASSERT_NE(at, std::string::npos)
                << "MOBILEGL_PIPE_VERIFY_CORRUPT=" << kPackField << " perturbs the hook's ORACLE at "
                   "every backend read of that field, and the server's read_pixels reads it under "
                   "the server's own stamp - so the server role's log had to carry " << expected
                << ", ...}. It carried nothing: either the compare-at-read hook is not running on "
                   "the apply thread, or the neutral-pack window swallowed the perturbation, and "
                   "in both cases every per-read green in this lane is green for no reason. Server "
                   "half of the log appended by this case:\n"
                << server;

            const std::size_t eol = server.find('\n', at);
            const std::string line =
                server.substr(at, eol == std::string::npos ? std::string::npos : eol - at);
            EXPECT_NE(line.find("where=read"), std::string::npos)
                << "the server half reported the corrupted field, but not from the compare-at-read "
                   "arm - `where=` says which comparator spoke, and only `read` is this case's "
                   "subject. Line:\n"
                << line;
        }

    } // namespace
} // namespace MGITest
