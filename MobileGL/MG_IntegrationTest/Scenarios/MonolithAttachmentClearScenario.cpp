// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/MonolithAttachmentClearScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - CLEARING A FRAMEBUFFER THAT HOLDS AN ATTACHMENT, ON THE PUSH-MONOLITH ARM.
//
// THE HOLE THIS CLOSES (INTEGRATOR-DECISIONS-P5E ID-107). P5e landed with 2250 unit entries and
// 113 `integration-split` entries green, and the game then died at startup under
// MOBILEGL_TRANSPORT=monolith inside Lightmap.<init> -> clearColorTexture -> glClear:
// SyncCurrentFBO -> BackendFramebufferObject::SyncToBackend(frontend FBO) ->
// SyncAttachmentSurface -> SyncMipmapsToBackend(null SharedPtr) -> SIGSEGV. Neither gated lane
// could see it, for two reasons that are one reason:
//
//   * the gate runs `-L unit` and `-L integration-split`, and EVERY split entry sets
//     MOBILEGL_TRANSPORT=inproc, which selects the by-handle framebuffer arm
//     (FramebufferRecordArmIsMandatory) - the arm that never reaches the frontend overload at
//     all; and
//   * every P5-era scenario that DOES clear a texture-attached framebuffer is split-only by
//     construction (F1WireScenario and friends call SplitRuntimeSkipReason() in SetUp and skip
//     the moment the runtime is not split), so the ambient monolith registration of those files
//     runs zero cases.
//
// So the arm the phone ships on - MOBILEGL_PIPE_PUSH compiled, transport monolith, framebuffer
// records produced and applied IN PROCESS (P4a D-C2), attachments resolved from those records -
// had no gated entry that attached anything to a framebuffer and cleared it. This file is that
// entry, and its registration block (MG_IntegrationTest/CMakeLists.txt, the
// `DirectGLES.PushMonolithArm.` prefix) pins the transport with a ctest ENVIRONMENT property for
// review finding N-6's reason: a job-level MOBILEGL_TRANSPORT=inproc must not be able to turn
// this lane into a second copy of the split one.
//
// WHAT EACH CASE IS FOR. The three attachment KINDS are the three arms of
// FramebufferImpl::SyncAttachmentSurface, and on the monolith arm each must drive its storage
// from the frontend object the walk is holding rather than from a by-handle seam whose record
// arm is selected by `Transport != Monolith` (CONTRACT-P5E §5.8 / ID-81):
//
//   * a MUTABLE 2D texture (glTexImage2D, the Lightmap shape) - the texture arm, whose storage
//     sync is the one that crashed;
//   * an IMMUTABLE 2D texture (glTexStorage2D) - the same arm with the levels already allocated,
//     so a regression that only shows up while levels are still being defined cannot hide the
//     other half;
//   * a RENDERBUFFER - the sibling arm, whose by-handle form does not dereference anything but
//     does drop the application-visible GL_OUT_OF_MEMORY report, so it is arm-selected too.
//
// The assertion is the PIXEL, not "it did not crash": a clear that reaches a driver framebuffer
// with no attachment raises GL_INVALID_FRAMEBUFFER_OPERATION and writes nothing, which is the
// silent half of the same defect and is what an attach that was refused rather than crashed
// would produce. Both are failures here.
//
// Backend-agnostic on purpose. Nothing below is a DirectGLES fallback: "a clear of a complete
// framebuffer reaches its own attachment" is a property of GL, so both ambient registrations
// assert it and the scenario fails wherever it stops being true.

#include <cstdint>
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

        constexpr int kSize = 8;

        // 0.25 / 0.5 / 0.75 / 1.0 - the F1 lane's own clear values, so a reader comparing the two
        // arms is comparing the same numbers. None of them is 0 or 1, so neither a cleared-to-zero
        // texture nor an untouched one can pass by accident.
        constexpr float kClearR = 0.25f;
        constexpr float kClearG = 0.5f;
        constexpr float kClearB = 0.75f;
        constexpr GLubyte kExpected[4] = {64, 128, 191, 255};

        class MonolithAttachmentClearScenario : public ScenarioTest {
        protected:
            GLuint fbo = 0;
            GLuint texture = 0;
            GLuint renderbuffer = 0;

            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                glGenFramebuffers(1, &fbo);
                glBindFramebuffer(GL_FRAMEBUFFER, fbo);
                glDisable(GL_SCISSOR_TEST);
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                glViewport(0, 0, kSize, kSize);
            }

            void TearDown() override {
                if (!Ready()) return;
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                if (texture) glDeleteTextures(1, &texture);
                if (renderbuffer) glDeleteRenderbuffers(1, &renderbuffer);
                if (fbo) glDeleteFramebuffers(1, &fbo);
                ScenarioTest::TearDown();
            }

            // Clear the bound framebuffer and read back the pixel the F1 lane reads. Separated
            // from the attach so that a failure says which half broke.
            void ClearAndExpectThePixel(const char* what) {
                ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GLenum(GL_FRAMEBUFFER_COMPLETE))
                    << what << ": the framebuffer never came up, so the clear below would prove nothing";
                glClearColor(kClearR, kClearG, kClearB, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
                GLubyte pixel[4] = {0, 0, 0, 0};
                glReadPixels(2, 3, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
                // A clear issued against a driver framebuffer whose attachment point was never
                // filled in raises this and writes nothing - the silent form of the defect.
                EXPECT_EQ(FirstGLError(), 0u) << what << ": " << GLErrorName(FirstGLError());
                for (int i = 0; i < 4; ++i) {
                    EXPECT_NEAR(pixel[i], kExpected[i], 1)
                        << what << ": channel " << i << " of the attachment did not receive the clear";
                }
            }
        };

        // THE DEVICE'S OWN SHAPE: a mutable texture whose level 0 is defined by glTexImage2D and
        // then attached, which is what Lightmap.<init> does before its clearColorTexture. The
        // storage sync behind the attachment is the call that took the null frontend object.
        TEST_F(MonolithAttachmentClearScenario, AClearOfAMutableTextureAttachmentReachesItsPixels) {
            if (!Ready() || IsSkipped()) return;

            glGenTextures(1, &texture);
            glBindTexture(GL_TEXTURE_2D, texture);
            std::vector<GLubyte> zeros(static_cast<std::size_t>(kSize) * kSize * 4, 0);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kSize, kSize, 0, GL_RGBA, GL_UNSIGNED_BYTE, zeros.data());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);

            ClearAndExpectThePixel("mutable 2D texture attachment");
        }

        // The same arm with immutable storage: the levels are allocated before the attach, so a
        // sync that only misbehaves while a level is still being defined cannot account for a
        // green here.
        TEST_F(MonolithAttachmentClearScenario, AClearOfAnImmutableTextureAttachmentReachesItsPixels) {
            if (!Ready() || IsSkipped()) return;

            glGenTextures(1, &texture);
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, kSize, kSize);
            ASSERT_EQ(FirstGLError(), 0u) << "the driver refused the immutable storage itself";
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);

            ClearAndExpectThePixel("immutable 2D texture attachment");
        }

        // The renderbuffer arm of the same function. It cannot crash the way the texture arm did
        // - the by-handle allocation dereferences no frontend object - but it IS the other half of
        // the arm selection, and a regression that routes it back through the by-handle form on
        // the monolith arm silently drops the application's GL_OUT_OF_MEMORY report.
        TEST_F(MonolithAttachmentClearScenario, AClearOfARenderbufferAttachmentReachesItsPixels) {
            if (!Ready() || IsSkipped()) return;

            glGenRenderbuffers(1, &renderbuffer);
            glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, kSize, kSize);
            ASSERT_EQ(FirstGLError(), 0u) << "the driver refused the renderbuffer storage itself";
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, renderbuffer);

            ClearAndExpectThePixel("renderbuffer attachment");
        }

    } // namespace
} // namespace MGITest
