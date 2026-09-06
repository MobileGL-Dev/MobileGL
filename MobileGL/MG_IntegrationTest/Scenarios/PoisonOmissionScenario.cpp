// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/PoisonOmissionScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - NEGATIVE CONTROL B (gate G5): AN OMITTED FILL POINT ABORTS ON THAT VERB, AND ONLY THERE.
//
// The per-verb poison is the half of P1 that makes a forgotten fill row loud instead of silent: the
// filler stamps a generation on every field it copies for a verb, and an accessor whose stamp is not
// this verb's aborts with Fatal{UnmigratedPipeInput, "<Field>@<Verb>"}. A mechanism that can only be
// observed when someone forgets a row is a mechanism nobody can trust, so MOBILEGL_PIPE_POISON_OMIT
// forges the mistake on purpose: it names one (verb, field) pair whose STAMP the filler skips while
// still copying the value, which is indistinguishable from a row that was never written.
//
// The scenario asserts both halves of "on THAT verb, and only there":
//
//   OmittedFieldAbortsOnThatVerb - with MOBILEGL_PIPE_POISON_OMIT=GenerateMipmap:GetActiveTextureUnit,
//                                  a draw must still complete (GetActiveTextureUnit is not in kDraw's
//                                  mask, and the draw's own fields are stamped normally) and the
//                                  following glGenerateMipmap must abort naming exactly that pair.
//   WithoutOmissionCompletes     - the identical sequence with the knob unset runs to completion with
//                                  no Fatal at all. Without this half, "it aborted" would say nothing
//                                  about WHY: a poison that fired on every verb would look just as red.
//
// The knob is process-wide, so the two cases cannot share a lane: the first runs in the PoisonOmitted.
// entries, the second in the ambient Verify. entries (it skips when the knob IS set).
//
// WHY THE SEQUENCE RUNS IN A SEPARATE PROCESS, AND WHY THAT PROCESS IS fork()+execve() AND NOT fork()
// ALONE. The poison reports with MGLOG_F and then std::abort(), in the middle of a GL command - so the
// sequence cannot run in the test process, and the harness's own bring-up pre-flight
// (Harness/HeadlessGL.cpp) already establishes the shape: run it where a SIGABRT is a datum in
// waitpid() instead of a dead lane. But that pre-flight forks BEFORE any context exists, and this case
// cannot: the fixture has already brought one up. A bare fork() of a process holding a live Vulkan
// device inherits the driver's mutexes with no threads to release them, and the child wedges on its
// first submit - measured here as a 120s timeout on DirectVulkan and a clean pass on DirectGLES, which
// is exactly the kind of backend-shaped flake a control must not have. So the child immediately
// execve()s a fresh copy of this same test binary, filtered to the worker case below, which brings up
// its own context from scratch and knows nothing about the parent's.
//
// The child gets its OWN MOBILEGL_LOG_FILE_PATH for the same reason: the library opens its log with
// fopen(path, "w"), so a child sharing the parent's path would truncate the file the parent is about
// to read.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../Harness/HeadlessGL.h"
#include "../Harness/ScenarioFixture.h"

#if !defined(_WIN32) && !defined(__APPLE__) && !defined(__ANDROID__) && __has_include(<sys/wait.h>)
#define MGITEST_POISON_HAVE_FORK 1
#include <csignal>
#include <ctime>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#else
#define MGITEST_POISON_HAVE_FORK 0
#endif

#ifdef GLAPI
#undef GLAPI
#endif
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glcorearb.h>
#undef GL_GLEXT_PROTOTYPES

namespace MGITest {
    namespace {

        // What the PoisonOmitted. ctest entry and the CI negative-control step name. The pair is
        // spelled here so the assertion below is about the exact string the poison contracts to
        // print (ARCHITECTURE.md 9.2: Fatal{UnmigratedPipeInput, "<Field>@<Verb>"}).
        constexpr const char* kOmittedVerb = "GenerateMipmap";
        constexpr const char* kOmittedField = "GetActiveTextureUnit";
        constexpr const char* kFatalPrefix = "Fatal{UnmigratedPipeInput";

        // Set only in the re-executed child, so the worker case below runs in that process and skips
        // everywhere else (including in the ambient lanes, where it is registered like any other case).
        constexpr const char* kChildMarker = "MGITEST_POISON_OMISSION_CHILD";
        constexpr const char* kWorkerFilter =
            "--gtest_filter=PoisonOmissionScenario.TheSequenceThePoisonControlsRun";

        constexpr const char* kVS = R"(#version 330 core
in vec2 aPos;
void main() { gl_Position = vec4(aPos, 0.0, 1.0); }
)";

        constexpr const char* kFS = R"(#version 330 core
out vec4 o_color;
void main() { o_color = vec4(0.25, 0.5, 0.75, 1.0); }
)";

        bool StringKnobIsSet(const char* name) {
            const char* value = std::getenv(name);
            return value != nullptr && *value != '\0';
        }

        std::filesystem::path LibraryLogPath() {
            const char* path = std::getenv("MOBILEGL_LOG_FILE_PATH");
            return (path != nullptr && *path != '\0') ? std::filesystem::path(path)
                                                      : std::filesystem::path();
        }

        // Where the child is told to write ITS log. Empty when the lane configured no log path at
        // all, in which case the signal is the only evidence and the text assertions are skipped.
        std::string ChildLogPath() {
            const std::filesystem::path parent = LibraryLogPath();
            if (parent.empty()) return {};
            return (parent.string() + ".poison-child");
        }

        std::string ReadWholeFile(const std::string& path) {
            if (path.empty()) return {};
            std::ifstream file(path, std::ios::binary);
            if (!file.good()) return {};
            return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        }

        class PoisonOmissionScenario : public ScenarioTest {
        protected:
            // The sequence under test. Deliberately in this order: the DRAW comes first and must
            // survive - if the poison fired there, the "only that verb" half would be false and the
            // SIGABRT the parent waits for would prove nothing.
            void RunSequence() {
                HeadlessGL& gl = Gl();

                std::string error;
                const unsigned int program = CompileProgram(kVS, kFS, &error);
                ASSERT_NE(program, 0u) << error;

                // A two-level texture, so glGenerateMipmap has real work to do and cannot be
                // short-circuited into a no-op by a backend that inspects the level count first.
                GLuint texture = 0;
                glGenTextures(1, &texture);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, texture);
                unsigned char pixels[8 * 8 * 4];
                for (std::size_t i = 0; i < sizeof(pixels); ++i) {
                    pixels[i] = static_cast<unsigned char>(i);
                }
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
                glTexImage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 3);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);

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
                glFinish();
                std::fprintf(stderr, "[itest] poison worker: the draw completed\n");

                // The verb the omission names. Under MOBILEGL_PIPE_POISON_OMIT this must abort.
                glBindTexture(GL_TEXTURE_2D, texture);
                glGenerateMipmap(GL_TEXTURE_2D);
                glFinish();
                std::fprintf(stderr, "[itest] poison worker: glGenerateMipmap returned\n");

                glBindVertexArray(0);
                glDeleteBuffers(1, &vbo);
                glDeleteVertexArrays(1, &vao);
                glDeleteTextures(1, &texture);
            }

#if MGITEST_POISON_HAVE_FORK
            // fork() + execve() of this same binary, filtered to the worker case, with the marker and
            // the child's own log path added to the environment. Everything that allocates happens
            // BEFORE the fork; between fork and execve only async-signal-safe work is done.
            static bool RunSequenceInAChildProcess(int& outStatus, std::string& outReason) {
                std::vector<std::string> env;
                for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
                    const std::string text(*entry);
                    if (text.rfind("MOBILEGL_LOG_FILE_PATH=", 0) == 0) continue;
                    if (text.rfind(std::string(kChildMarker) + "=", 0) == 0) continue;
                    env.push_back(text);
                }
                env.push_back(std::string(kChildMarker) + "=1");
                const std::string childLog = ChildLogPath();
                if (!childLog.empty()) {
                    std::error_code ec;
                    std::filesystem::remove(childLog, ec);
                    env.push_back("MOBILEGL_LOG_FILE_PATH=" + childLog);
                }

                std::vector<char*> envp;
                envp.reserve(env.size() + 1);
                for (std::string& entry : env) envp.push_back(entry.data());
                envp.push_back(nullptr);

                std::string exe = "/proc/self/exe";
                std::string arg0 = "MobileGLIntegrationTest";
                std::string filter = kWorkerFilter;
                char* argv[] = {arg0.data(), filter.data(), nullptr};

                std::fflush(nullptr);
                const pid_t child = fork();
                if (child < 0) {
                    outReason = "fork() failed";
                    return false;
                }
                if (child == 0) {
                    execve(exe.c_str(), argv, envp.data());
                    // execve only returns on failure; _exit, never exit(), because every atexit
                    // handler in this address space belongs to the parent's copy of the world.
                    std::fprintf(stderr, "[itest] poison child: execve(/proc/self/exe) failed\n");
                    _exit(127);
                }

                constexpr int kTimeoutMs = 120000;
                int waitedMs = 0;
                for (;;) {
                    const pid_t reaped = waitpid(child, &outStatus, WNOHANG);
                    if (reaped == child) return true;
                    if (reaped < 0) {
                        outReason = "waitpid on the poison worker failed";
                        return false;
                    }
                    if (waitedMs >= kTimeoutMs) {
                        kill(child, SIGKILL);
                        (void)waitpid(child, &outStatus, 0);
                        outReason = "the poison worker made no progress in 120s and was killed";
                        return false;
                    }
                    timespec nap{0, 10 * 1000 * 1000};
                    nanosleep(&nap, nullptr);
                    waitedMs += 10;
                }
            }

            static std::string DescribeStatus(int status) {
                if (WIFEXITED(status)) return "exited with status " + std::to_string(WEXITSTATUS(status));
                if (WIFSIGNALED(status)) return "died on signal " + std::to_string(WTERMSIG(status));
                return "ended in an unrecognised way";
            }
#endif
        };

        // The worker. It is a normal registered case so that the re-executed child can be selected
        // with nothing but --gtest_filter, and it skips in every process that is not that child.
        TEST_F(PoisonOmissionScenario, TheSequenceThePoisonControlsRun) {
            if (std::getenv(kChildMarker) == nullptr) {
                GTEST_SKIP() << "this case is the body the two poison controls run in a child process; "
                                "it does nothing unless " << kChildMarker << " is set, which only the "
                                "re-exec below does";
            }
            if (!Ready()) return;

            RunSequence();

#if MGITEST_POISON_HAVE_FORK
            // _exit, and not a return into gtest's teardown: this process exists to reach the verb
            // above and its exit status is the datum the parent reads. A normal teardown of a live
            // context could add signals of its own to that answer.
            std::fflush(nullptr);
            _exit(0);
#endif
        }

#if MGITEST_POISON_HAVE_FORK

        TEST_F(PoisonOmissionScenario, OmittedFieldAbortsOnThatVerb) {
            if (!Ready()) return;

            if (!StringKnobIsSet("MOBILEGL_PIPE_POISON_OMIT")) {
                GTEST_SKIP() << "this case is the poison's negative control and needs "
                                "MOBILEGL_PIPE_POISON_OMIT=<Verb>:<Field> for the whole process, which "
                                "is what the PoisonOmitted. ctest entries set";
            }
            const std::string knob = std::getenv("MOBILEGL_PIPE_POISON_OMIT");
            const std::string expectedPair = std::string(kOmittedField) + "@" + kOmittedVerb;
            if (knob != std::string(kOmittedVerb) + ":" + kOmittedField) {
                GTEST_SKIP() << "MOBILEGL_PIPE_POISON_OMIT is " << knob << ", but this case only knows "
                             << "how to provoke " << kOmittedVerb << ":" << kOmittedField;
            }

            int status = 0;
            std::string reason;
            ASSERT_TRUE(RunSequenceInAChildProcess(status, reason)) << reason;

            const std::string childLog = ReadWholeFile(ChildLogPath());
            ASSERT_TRUE(WIFSIGNALED(status))
                << "with the stamp of " << expectedPair << " omitted, the glGenerateMipmap in the child "
                << "had to read a field its verb never filled and abort. It " << DescribeStatus(status)
                << " instead - the poison is not armed (a build without MOBILEGL_PIPE_POISON, a filler "
                   "that stamps what it was told to skip, or a backend that no longer reads the field "
                   "through the accessor). Child log:\n"
                << childLog;
            EXPECT_EQ(WTERMSIG(status), SIGABRT)
                << "the child died on signal " << WTERMSIG(status) << " rather than SIGABRT; the poison "
                   "reports through MGLOG_F + std::abort(), so any other signal is a different crash. "
                   "Child log:\n"
                << childLog;

            if (ChildLogPath().empty()) {
                GTEST_SKIP() << "the abort happened, but the lane set no MOBILEGL_LOG_FILE_PATH, so the "
                                "Fatal's text cannot be read back; the PoisonOmitted. ctest entries set it";
            }
            EXPECT_NE(childLog.find(std::string(kFatalPrefix) + ", \"" + expectedPair + "\""),
                      std::string::npos)
                << "the child aborted, but not with Fatal{UnmigratedPipeInput, \"" << expectedPair
                << "\"} - that message is the whole diagnostic value of the poison. Child log:\n"
                << childLog;
            EXPECT_EQ(childLog.find("@DrawArrays"), std::string::npos)
                << "the draw that ran BEFORE the omitted verb also tripped the poison, so the omission "
                   "is not scoped to its verb: the fill classes are wrong, or the stamps are global. "
                   "Child log:\n"
                << childLog;
        }

        // The sibling control, in the ambient Verify. lanes: the same sequence with the knob UNSET
        // must run to completion and log no Fatal at all.
        TEST_F(PoisonOmissionScenario, WithoutOmissionCompletes) {
            if (!Ready()) return;

            if (AmbientQuirkFromEnvironment("MOBILEGL_PIPE_VERIFY") != AmbientQuirk::On) {
                GTEST_SKIP() << "the poison is only compiled into the push/verify builds; in an ordinary "
                                "build there is nothing for this control to be a control OF";
            }
            if (StringKnobIsSet("MOBILEGL_PIPE_POISON_OMIT")) {
                GTEST_SKIP() << "MOBILEGL_PIPE_POISON_OMIT is armed for this process, so the abort is the "
                                "EXPECTED outcome here; OmittedFieldAbortsOnThatVerb owns that half and "
                                "runs in the PoisonOmitted. lane";
            }

            int status = 0;
            std::string reason;
            ASSERT_TRUE(RunSequenceInAChildProcess(status, reason)) << reason;

            const std::string childLog = ReadWholeFile(ChildLogPath());
            ASSERT_TRUE(WIFEXITED(status))
                << "with no omission armed, a draw followed by glGenerateMipmap must complete; the child "
                << DescribeStatus(status)
                << ". If it aborted, the poison is firing on a field the verb's fill table SHOULD list - "
                   "add the row to MG_Pipe/FillPoints.def, never mark the field sticky. Child log:\n"
                << childLog;
            EXPECT_EQ(WEXITSTATUS(status), 0) << "the child " << DescribeStatus(status)
                                              << ". Child log:\n"
                                              << childLog;
            EXPECT_EQ(childLog.find("Fatal{"), std::string::npos)
                << "an unpoisoned run logged a Fatal:\n"
                << childLog;
        }

#else

        TEST_F(PoisonOmissionScenario, OmittedFieldAbortsOnThatVerb) {
            GTEST_SKIP() << "the poison control needs fork()/execve()/waitpid() to observe a SIGABRT as "
                            "a datum; this platform has none of them";
        }

        TEST_F(PoisonOmissionScenario, WithoutOmissionCompletes) {
            GTEST_SKIP() << "the poison control needs fork()/execve()/waitpid() to observe a SIGABRT as "
                            "a datum; this platform has none of them";
        }

#endif

    } // namespace
} // namespace MGITest
