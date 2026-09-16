// P5b sync migration: a real fence and reply cross the apply thread.
#include "../Harness/ScenarioFixture.h"
#include "../Harness/SplitRuntimePeek.h"
#ifdef GLAPI
#undef GLAPI
#endif
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glcorearb.h>
#undef GL_GLEXT_PROTOTYPES

namespace MGITest {
class SyncWireScenario : public ScenarioTest {};

TEST_F(SyncWireScenario, FenceWaitStatusAndDeletionCrossAndPreserveRenderedPixels) {
    if (!Ready()) return;
    const auto why = SplitRuntimeSkipReason();
    if (!why.empty()) GTEST_SKIP() << why;
    glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    const auto before = PeekSplitRuntime().emitSeq;
    const auto sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    ASSERT_NE(sync, nullptr);
    EXPECT_TRUE(glIsSync(sync));
    GLint status = 0;
    glGetSynciv(sync, GL_SYNC_STATUS, 1, nullptr, &status);
    EXPECT_TRUE(status == GL_SIGNALED || status == GL_UNSIGNALED);
    const auto wait = glClientWaitSync(sync, GL_SYNC_FLUSH_COMMANDS_BIT, 5000000000ull);
    EXPECT_TRUE(wait == GL_ALREADY_SIGNALED || wait == GL_CONDITION_SATISFIED) << wait;
    glWaitSync(sync, 0, GL_TIMEOUT_IGNORED);
    glDeleteSync(sync);
    EXPECT_FALSE(glIsSync(sync));
    EXPECT_GE(PeekSplitRuntime().emitSeq, before + 5);
    GLubyte pixel[4]{};
    glReadPixels(1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    const int expected[4] = {64, 128, 191, 255};
    for (int i = 0; i < 4; ++i) EXPECT_NEAR(pixel[i], expected[i], 1);
    EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
    // This one remains live until full session teardown, exercising server orphan cleanup.
    ASSERT_NE(glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0), nullptr);
}
} // namespace MGITest
