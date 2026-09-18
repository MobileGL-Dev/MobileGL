// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/CtWireScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P5c ct (MG_Remote/CONTRACT-P5C.md §5): the two control records, end to end over inproc.
//
// applier_reset (§5.1): this process's first validate primes the pipe tracker, and the
// FreshlyPrimed edge is the record's producer (MG_Impl/Pipe/PipeFill.cpp). What the scenario
// asserts is the SERVER sink's own counters - moved by nobody else - plus the pixels, so a
// reset that never crossed is red by the tally and a client that fell through to the driver
// is red by ScenarioFixture's emit-ordinal rule.
//
// object_death (§5.2): a texture and a framebuffer die, the deaths cross, and the slots
// recycle. The picture after recycling is the behavioural half: a stale twin answering for
// the recycled object renders the DEAD object's content (or errors), which is the exact
// shape LiveGenAt's generation check exists to refuse.
//
// RED ONCE for the reset (executed, recorded in the package report): reverting PipeFill's
// FreshlyPrimed arm to the GL-thread direct MGPipeApplierReset() call aborts this whole lane
// with Fatal{RoleViolation, "g_applier"} out of the applier's layer-2 guard. The automated
// half of that control is TheDirectApplierResetCallOnTheGLThreadIsRoleViolation below, and
// the guard's two non-Fatal arms are pinned in PipeWireCodecTest's CtWireFatals suite.

#include "../Harness/ScenarioFixture.h"
#include "../Harness/SplitRuntimePeek.h"

#include <MG_Pipe/PipeApply.h>
#include <MG_Remote/Server/ServerSession.h>

#if !defined(_WIN32)
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <sstream>
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

    MobileGL::MG_Remote::Server::ServerVerbSink& ServerVerbs() {
        return MobileGL::MG_Remote::Server::ServerSessionInstance().Applier().Verbs();
    }

    class CtWireScenario : public ScenarioTest {
    protected:
        void SetUp() override {
            ScenarioTest::SetUp();
            if (!Ready()) return;
            const auto why = SplitRuntimeSkipReason();
            if (!why.empty()) GTEST_SKIP() << why;
        }

        // One 4x4 RGBA8 texture whose every texel is `rgba`, uploaded so the object crosses
        // (no handle, no record - CONTRACT-P5C.md §5.2: an object that never crossed emits
        // nothing at death, and this case needs a real death).
        GLuint MakeSolidTexture(const GLubyte rgba[4]) {
            GLuint texture = 0;
            glGenTextures(1, &texture);
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            GLubyte texels[4 * 4 * 4];
            for (int i = 0; i < 4 * 4; ++i) {
                for (int c = 0; c < 4; ++c) texels[i * 4 + c] = rgba[c];
            }
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, texels);
            return texture;
        }

        // The texture's level-0 content, read back through a throwaway FBO, as four bytes.
        // The FBO is bound AND deleted here, so it never leaks a framebuffer death into the
        // tally a case is watching.
        void ReadTextureLevel(GLuint texture, GLubyte out[4]) {
            GLuint fbo = 0;
            glGenFramebuffers(1, &fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
            glReadPixels(1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDeleteFramebuffers(1, &fbo);
        }
    };

    TEST_F(CtWireScenario, ApplierResetCrossesAtThePrimedEdgeInSerialOrder) {
        if (!Ready()) return;
        // The first verb of the process primes the tracker; the edge is the producer. The
        // clear also gives the case its pixels, so a lane that emitted nothing is red twice
        // (here and in the fixture's emit-ordinal rule).
        glClearColor(0.2f, 0.4f, 0.6f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        EXPECT_GE(ServerVerbs().ApplierResets(), 1u)
            << "the FreshlyPrimed edge emitted no applier_reset; the server's g_applier was "
               "never reset for this session";
        // The serial sequence is the session's own count (§1: asserted, never dispatched
        // on): every record the sink accepted carried the serial it expected, or the counter
        // and the accepted tally would disagree.
        EXPECT_EQ(ServerVerbs().ExpectedApplierResetSerial(), ServerVerbs().ApplierResets())
            << "an applier_reset record was dropped or replayed on the wire";

        GLubyte pixel[4]{};
        glReadPixels(1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "Ct.ApplierReset.error";
        const int expected[4] = {51, 102, 153, 255};
        for (int i = 0; i < 4; ++i) EXPECT_NEAR(pixel[i], expected[i], 1) << "Ct.ApplierReset.pixels";
    }

    TEST_F(CtWireScenario, TextureDeathCrossesAndTheRecycledSlotAnswersTheNewObject) {
        if (!Ready()) return;
        // The object_death producer is Espryt-side (OnFrontendStateObjectDestroyed,
        // CONTRACT-P5C.md §5.2); Magma installs no StateObjectDeathOps (P7), so under
        // DirectVulkan there is no death record to watch and the case has nothing to prove.
        if (HeadlessGL::Get().BackendName() != "DirectGLES") {
            GTEST_SKIP() << "object_death is produced by the DirectGLES death-notice ops; "
                            "DirectVulkan has none until P7";
        }
        const GLubyte red[4] = {255, 0, 0, 255};
        const GLubyte green[4] = {0, 255, 0, 255};

        // A texture lives and crosses (the upload emits its records). It is also READ BACK
        // once before it dies: the read forces the server-side sync that creates the twin -
        // and, one level down, resolves the Espryt slot arm that INSTALLS the death-notice
        // ops, which no process has before its first twin lookup. The red picture is the
        // pre-death control the recycle below is read against.
        GLuint texture = MakeSolidTexture(red);
        GLubyte before[4]{};
        ReadTextureLevel(texture, before);
        const int redExpected[4] = {255, 0, 0, 255};
        for (int i = 0; i < 4; ++i) ASSERT_NEAR(before[i], redExpected[i], 1) << "Ct.TextureDeath.before";

        // The object dies unbound so the destructor - and the death record - fire at the
        // delete, and the tally is read AFTER the EmitAndWait the delete blocked on.
        glBindTexture(GL_TEXTURE_2D, 0);
        const MobileGL::Uint64 deathsBefore = ServerVerbs().ObjectDeaths();
        glDeleteTextures(1, &texture);
        EXPECT_GT(ServerVerbs().ObjectDeaths(), deathsBefore)
            << "the texture's death produced no object_death record; the server's twin was "
               "never told to let go";

        // The slot recycles forward: a new texture (the frontend hands the same GL name back
        // more often than not, which is exactly the ABA shape) must answer with ITS content,
        // not the dead twin's. Red then green, read back as pixels: a stale twin is red.
        texture = MakeSolidTexture(green);
        GLubyte pixel[4]{};
        ReadTextureLevel(texture, pixel);
        glBindTexture(GL_TEXTURE_2D, 0);
        glDeleteTextures(1, &texture);
        ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "Ct.TextureDeath.error";
        const int expected[4] = {0, 255, 0, 255};
        for (int i = 0; i < 4; ++i) EXPECT_NEAR(pixel[i], expected[i], 1)
            << "Ct.TextureDeath.pixels - the recycled texture read back the dead twin's content";
    }

    TEST_F(CtWireScenario, FramebufferDeathCrossesAndTheRecycledSlotAnswersTheNewObject) {
        if (!Ready()) return;
        // Same producer reason as the texture case above: object_death is emitted by the
        // DirectGLES death-notice ops; DirectVulkan has none until P7.
        if (HeadlessGL::Get().BackendName() != "DirectGLES") {
            GTEST_SKIP() << "object_death is produced by the DirectGLES death-notice ops; "
                            "DirectVulkan has none until P7";
        }
        // Framebuffer is the kind object_death EXISTS for: it has no other wire delete
        // opcode (CONTRACT-P5C.md §5.2). The renderbuffer goes along so the FBO has storage.
        GLuint fbo = 0, renderbuffer = 0;
        glGenFramebuffers(1, &fbo);
        glGenRenderbuffers(1, &renderbuffer);
        glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 4, 4);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, renderbuffer);
        ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GLenum(GL_FRAMEBUFFER_COMPLETE))
            << "Ct.FramebufferDeath.setup";
        glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        GLubyte pixel[4]{};
        glReadPixels(1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        const int blue[4] = {0, 0, 255, 255};
        for (int i = 0; i < 4; ++i) ASSERT_NEAR(pixel[i], blue[i], 1) << "Ct.FramebufferDeath.first";

        // Die unbound, framebuffer first so the attachment's own death is a separate record.
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
        const MobileGL::Uint64 deathsBefore = ServerVerbs().ObjectDeaths();
        glDeleteFramebuffers(1, &fbo);
        // THE FENCE (MOBILEGL_IPC_BATCH_WAITS, default on): object_death is a kCtxObject
        // value-class record - published WITHOUT waiting for its own apply (production-safe:
        // the in-order ring applies it before any record that recycles the slot). The
        // server-side counter this assertion reads only moves at apply time, so it needs a
        // wait boundary: a clear is kCtxVerb and still waits, and its wait covers the death.
        glClear(GL_COLOR_BUFFER_BIT);
        EXPECT_GT(ServerVerbs().ObjectDeaths(), deathsBefore)
            << "the framebuffer's death produced no object_death record - and no other "
               "opcode can carry it";
        glDeleteRenderbuffers(1, &renderbuffer);

        // Recycle: a new FBO and a new backing store, cleared to a different colour. A stale
        // twin answering for the recycled framebuffer renders the blue of the dead one.
        glGenFramebuffers(1, &fbo);
        glGenRenderbuffers(1, &renderbuffer);
        glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 4, 4);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, renderbuffer);
        ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GLenum(GL_FRAMEBUFFER_COMPLETE))
            << "Ct.FramebufferDeath.recycle";
        glClearColor(1.0f, 1.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glReadPixels(1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
        glDeleteRenderbuffers(1, &renderbuffer);
        ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "Ct.FramebufferDeath.error";
        const int yellow[4] = {255, 255, 0, 255};
        for (int i = 0; i < 4; ++i) EXPECT_NEAR(pixel[i], yellow[i], 1)
            << "Ct.FramebufferDeath.pixels - the recycled framebuffer read back the dead twin's content";
    }

#if !defined(_WIN32)
    TEST_F(CtWireScenario, TheDirectApplierResetCallOnTheGLThreadIsRoleViolation) {
        if (!Ready()) return;
        // A verb in the PARENT, so the fixture's emit-ordinal rule has something to see: the
        // death statement below runs only in the child, and a parent that emitted nothing
        // is the shape that rule exists to fail.
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        // THE RED-ONCE FOR §5.1, AUTOMATED. This process has a LIVE inproc session, and the
        // GL-thread direct call the applier_reset record replaced is a named Fatal there
        // (CONTRACT-P5C.md §5.1 / §6 layer 2) - which is exactly the shape reverting
        // PipeFill's FreshlyPrimed arm would take, so the guard is what turns that revert
        // red. EXPECT_EXIT re-runs the case in a child process: the child brings up its own
        // session (its SetUp is this same fixture's) and aborts at the statement. The two
        // NON-Fatal arms - monolith, and a transport with no client session - are the unit
        // suite's CtWireFatals.TheDirectApplierResetCallSurvivesMonolithAndAWirelessTransport.
        //
        // The regex is ".*" because the library's Fatal line goes to its log file, not to
        // the stderr a death test matches (ServerLoopEglTest's own EXPECT_EXIT does the
        // same); the NAME is asserted from the log below, which the child truncated and
        // wrote before it died.
        EXPECT_EXIT(MobileGL::MG_Pipe::MGPipeApplierReset(), ::testing::KilledBySignal(SIGABRT), ".*");
        if (const char* logPath = std::getenv("MOBILEGL_LOG_FILE_PATH"); logPath != nullptr) {
            std::ifstream in(logPath, std::ios::binary);
            std::ostringstream log;
            log << in.rdbuf();
            EXPECT_NE(log.str().find("Fatal{RoleViolation, \"g_applier\"}"), std::string::npos)
                << "the child aborted, but not with the layer-2 guard's own line:\n"
                << log.str();
        }
    }
#endif

} // namespace
} // namespace MGITest
