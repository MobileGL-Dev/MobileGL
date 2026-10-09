// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/ScenarioIsolationScenario.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - A CASE STARTS FROM A CLEAN SLATE EVEN WHEN THE CASE BEFORE IT DID NOT CLEAN UP.
//
// The harness has ONE context per process, so whatever a case leaves bound is still bound when
// the next case starts. ctest runs one case per process, which hides that; a developer's
// `--gtest_filter=A:B` does not.
//
// The leak that bit: CrossFrameBufferScenario and StorageBufferRegrowScenario (among others) end
// with glDeleteProgram on the program that is still current. GL 4.6 core 7.3 defers that deletion
// until the program stops being current, so GL_CURRENT_PROGRAM still names it. GuiBatchScenario
// then replays AcceleratedRendering's save / switch / restore: it reads GL_CURRENT_PROGRAM, makes
// its compute program current - which is what finally deletes the old one - and restores the
// saved name, which no longer exists. GL_INVALID_VALUE, from MobileGL and from native Mesa alike:
// the implementation is right and the fixture's "clean slate" was not.
//
// The two cases below run in declaration order in ONE process (the
// ScenarioIsolationScenario.CasesInOneProcess registrations in CMakeLists.txt). The first one is
// the careless neighbour; the second asserts what ScenarioTest::SetUp owes every case. Each is
// green on its own, so the ordinary one-case-per-process entries cannot see the bug.

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

        constexpr const char* kVertexSource = R"(#version 330 core
layout(location = 0) in vec2 aPos;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

        constexpr const char* kFragmentSource = R"(#version 330 core
out vec4 fragColor;
void main() {
    fragColor = vec4(1.0);
}
)";

        class ScenarioIsolationScenario : public ScenarioTest {};

        GLint CurrentProgram() {
            GLint current = -1;
            glGetIntegerv(GL_CURRENT_PROGRAM, &current);
            return current;
        }

    } // namespace

    // The careless neighbour: its program is deleted while current and never unbound, and it
    // leaves an error in the queue that nobody reads.
    TEST_F(ScenarioIsolationScenario, ACaseLeavesItsDeletedProgramCurrentAndAnErrorUnread) {
        if (!Ready()) return;
        std::string error;
        const GLuint program = CompileProgram(kVertexSource, kFragmentSource, &error);
        ASSERT_NE(program, 0u) << error;
        glUseProgram(program);
        glDeleteProgram(program);
        // GL 4.6 core 7.3: still current, only flagged.
        EXPECT_EQ(CurrentProgram(), static_cast<GLint>(program));
        GLint deleteStatus = GL_FALSE;
        glGetProgramiv(program, GL_DELETE_STATUS, &deleteStatus);
        EXPECT_EQ(deleteStatus, GL_TRUE);
        EXPECT_EQ(FirstGLError(), 0u);

        glBindBuffer(0x1234, 0); // GL_INVALID_ENUM, left in the queue
    }

    TEST_F(ScenarioIsolationScenario, TheNextCaseStartsWithNoProgramCurrentAndNoErrorPending) {
        if (!Ready()) return;
        const unsigned int pending = FirstGLError();
        EXPECT_EQ(pending, 0u) << "an earlier case's " << GLErrorName(pending) << " reached this one";
        const GLint saved = CurrentProgram();
        EXPECT_EQ(saved, 0) << "an earlier case's program is still current";

        // GuiBatchScenario's prepareBuffers(): save, switch, restore. Switching away is what
        // deletes a flagged program, so restoring a leaked name is GL_INVALID_VALUE.
        std::string error;
        const GLuint program = CompileProgram(kVertexSource, kFragmentSource, &error);
        ASSERT_NE(program, 0u) << error;
        glUseProgram(program);
        glUseProgram(0);
        glUseProgram(static_cast<GLuint>(saved));
        const unsigned int restore = FirstGLError();
        EXPECT_EQ(restore, 0u) << "restoring GL_CURRENT_PROGRAM raised " << GLErrorName(restore);
        glUseProgram(0);
        glDeleteProgram(program);
    }

} // namespace MGITest
