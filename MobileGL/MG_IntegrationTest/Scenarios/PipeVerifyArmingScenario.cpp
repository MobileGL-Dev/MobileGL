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
//
// The observable is the library's own log, because MG_Config is not reachable from this module
// (on Android it links the SHIPPING libMobileGL.so, built -fvisibility=hidden) and the arming
// signal is a latched MGLOG_I. The ctest entry sets MOBILEGL_LOG_FILE_PATH; this only reads it.
//
// Note on scope: the log file is opened with fopen(path, "w") at the first log write of a process
// (MG_Util/Debug/Log.cpp, InitFile), so the file holds THIS process's lines and nothing else - a
// whole-file search cannot be satisfied by a sibling ctest entry of the same lane. The arming line
// is latched at the FIRST fill of the process, which may be the harness bring-up rather than this
// test's draw, so the arming search is whole-file on purpose; the divergence search is restricted
// to the bytes this case appended, which is where a differ belongs.

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

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
                const char* path = std::getenv("MOBILEGL_LOG_FILE_PATH");
                return (path != nullptr && *path != '\0') ? std::filesystem::path(path)
                                                          : std::filesystem::path();
            }

            static std::uintmax_t LibraryLogSize() {
                std::error_code ec;
                const std::filesystem::path path = LibraryLogPath();
                if (path.empty()) return 0;
                const std::uintmax_t size = std::filesystem::file_size(path, ec);
                return ec ? 0 : size;
            }

            static std::string LibraryLogSince(std::uintmax_t offset) {
                const std::filesystem::path path = LibraryLogPath();
                if (path.empty()) return {};
                std::ifstream file(path, std::ios::binary);
                if (!file.good()) return {};
                file.seekg(static_cast<std::streamoff>(offset));
                return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            }

            static std::string LibraryLog() { return LibraryLogSince(0); }

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

            const std::uintmax_t before = LibraryLogSize();
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
            const std::uintmax_t before = LibraryLogSize();
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

    } // namespace
} // namespace MGITest
