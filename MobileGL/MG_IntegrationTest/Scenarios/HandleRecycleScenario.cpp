// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/HandleRecycleScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - THE HANDLE ABA (gate G8): a frontend object that dies and is replaced at the same
// heap address must not inherit the dead object's backend twin, its vertex-input state, or its
// draw memo.
//
// WHY THIS EXISTS. Every backend memo in the tree is keyed, today, on some property of a LIVE
// frontend object: a raw `void*` owner pointer (DirectGLES' StateBackendObjectRegistry and its
// three TwinLookupMemos), a `GetLifetimeId()` (DirectVulkan's VertexInputStateFactory::ComputeHash
// and VaoDrawMemo::vaoLifetimeId), or a weak_ptr expiry test. Track H replaces all of them with an
// {slot, gen} handle. The question this scenario asks is the only one that matters about that
// change: does the NEW key actually stop the aliasing the OLD key stopped? A re-key that quietly
// dropped a guard would produce pixels from a dead object's GPU resources, and there is no other
// gate in this tree that can see it - SSIM over a 40-trace corpus cannot, because no fixture
// destroys and immediately re-creates an object with a byte-identical configuration.
//
// HOW THE ABA IS BUILT, through public GL only:
//   1. an object is created, USED IN A DRAW, and used again for a few frames, so that every
//      per-object memo in both backends is armed against it;
//   2. it is unbound (so the frontend's last SharedPtr drops - a still-bound object keeps living,
//      TextureState.cpp) and deleted;
//   3. a replacement is created IMMEDIATELY, with a byte-identical configuration, so that a
//      content hash over the configuration matches the dead object's, and so that the allocator
//      is as likely as it can be made to hand back the address it just freed;
//   4. the replacement is given DIFFERENT CONTENTS - a different vertex buffer, different texels,
//      a different attachment;
//   5. one draw, one readback. The pixels must come from the replacement.
//
// The allocator is not under our control, so step 3 is a likelihood, not a guarantee, and a
// scenario that silently passed because the address was never reused would prove nothing. The
// public-GL proxy for "the allocator repeated itself" is the GL NAME: MobileGL's name allocators
// hand a deleted name straight back, so `TheReproducerRecyclesEveryName` asserts the recycle
// happened and every other case asserts on the name it got. When a name is NOT recycled the case
// SKIPS with that reason rather than passing - the shape MG_Test/State/ObjectLifetimeIdTest.cpp
// already uses for exactly this ("inconclusive, not proven").
//
// WHAT THAT PROXY COSTS THE CI LANE, WRITTEN DOWN ON PURPOSE. The name is only a proxy: the
// corruption the AbaControl arm asserts needs the freed HEAP BLOCK to be handed back, and public
// GL cannot see that. So on a run where the allocator returns the name but not the block, the two
// arms behave differently - the correctness arms (Handles, Legacy) still expect correct pixels and
// still pass, but AbaControl expects the corruption and FAILS. It does that inside
// `ctest -L integration-gpu`, a lane P2 requires green (gate G2), so this scenario can red a
// required lane for an allocator reason. That is chosen, not overlooked: an arm that skipped
// whenever it could not prove the ABA would also be green on the day the reproducer stopped
// reproducing one, and "green because nothing was tested" is precisely what this file exists to
// prevent. ObjectLifetimeIdTest makes the opposite choice because it is a unit test with no
// always-on lane behind it. If the arm ever does flake, the fix is a stronger address-reuse proxy
// - a backend counter for "a recycled slot was handed back out" - and not a looser assertion.
//
// THREE ARMS, ALL ALWAYS ON (P2 brief D18). The arm is named by MGITEST_HANDLE_ARM, which is a
// HARNESS marker - the library never reads it - and the CMake wiring registers one lane per arm:
//
//   Handles     MOBILEGL_PIPE_PUSH default (Track H bits set), MOBILEGL_PIPE_LEGACY_MEMOS=0.
//               The {slot, gen} key is the only key in the process. Expects correct pixels.
//   Legacy      MOBILEGL_PIPE_PUSH=0. Today's lifetimeId + weak_ptr guards. Expects correct
//               pixels - they work, which is the point: the re-key is not fixing a live bug, it
//               is replacing a guard, and the replacement has to be at least as strong.
//   AbaControl  MOBILEGL_PIPE_PUSH=0 AND MOBILEGL_PIPE_HANDLE_ABA_CONTROL=1. The knob reverts
//               exactly the two guards the re-key replaces (VertexInputStateFactory::ComputeHash
//               hashes attr.Buffer.get() instead of GetLifetimeId(); LookupVaoDrawMemo skips the
//               vaoLifetimeId compare), so this arm expects the CORRUPTION. It is what makes
//               `HandleRecycleScenario green, and red before the re-key` an always-on CI fact
//               instead of a one-off manual demonstration: if the reproducer ever stops
//               reproducing the ABA, this arm fails.
//
// WHY AN ARM CAN SKIP, AND WHY THAT IS NOT A HOLE. Two of the three arms assert something that
// only EXISTS once another P2 package has landed: `Handles` needs the backend's {slot, gen} arm
// (packages C and D) and `AbaControl` needs the knob's consumer (package D). This file is written
// and merged FIRST, against the P2 contract commit, so that the AbaControl red is recorded before
// either backend is touched. Until then those arms have nothing to assert, and the honest report
// for that is a SKIP that names what is missing - never a silently-deleted registration and never
// a green that means "the thing I test does not exist yet".
//
// The skip is decided by the BUILD, not by a hand-maintained list: MG_IntegrationTest/CMakeLists.txt
// greps the backend sources for the subsystem constant and for the knob's name and passes the
// answer in as MGITEST_HANDLE_REKEY_<backend> / MGITEST_HANDLE_ABA_IMPLEMENTED, with a
// CMAKE_CONFIGURE_DEPENDS on those files so the answer cannot go stale. When C and D land, the
// arms arm themselves.
//
// Those two markers are a statement about the SOURCE TREE, and they are set only in a push build,
// because that is the only build in which the thing they name is compiled: the {slot, gen} re-key
// and Features.PipeHandleAbaControl are both `#if MOBILEGL_PIPE_PUSH`. In a pull build the two
// push arms therefore skip on MGITEST_PIPE_PUSH_BUILD before they ever look at a per-arm marker -
// otherwise, once C and D landed, the pull build would run AbaControl against guards that are
// still in force (a hard red on `ctest -L integration-gpu`, which G2 requires green in BOTH
// builds) and Handles against a library with no re-key in it (a green that asserts nothing).

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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

        // ---- the arm -------------------------------------------------------------------

        enum class Arm {
            Handles,   // the {slot, gen} key is the only key
            Legacy,    // today's lifetimeId + weak_ptr guards
            AbaControl // the guards deliberately defeated; the corruption is the assertion
        };

        // Set by the three HandleRecycle. ctest entries and by NOTHING else. It is a harness
        // variable, not a library knob (hence the MGITEST_ prefix): the library never reads it.
        // Its absence means "this process is one of the ~400 ambient entries", where the arm is
        // undefined - MOBILEGL_PIPE_PUSH is at its build default there, which is neither the
        // Legacy arm nor the Handles arm - so the cases skip rather than assert something the
        // lane did not configure. Same shape, and the same reason, as
        // PipeVerifyArmingScenario's MGITEST_PIPE_ARMING_LANE.
        constexpr const char* kArmMarker = "MGITEST_HANDLE_ARM";

        Arm CurrentArm() {
            const char* name = std::getenv(kArmMarker);
            if (name == nullptr) return Arm::Legacy;
            if (std::strcmp(name, "handles") == 0) return Arm::Handles;
            if (std::strcmp(name, "aba") == 0) return Arm::AbaControl;
            return Arm::Legacy;
        }

        // Whether the lane named an arm this file knows. A value that is set but unrecognised is a
        // FAILURE (SetUp below), never a quiet fall-through to Legacy: a typo in a lane's
        // MGITEST_HANDLE_ARM would otherwise downgrade that lane's Handles or AbaControl assertion
        // to the Legacy one, which passes - a lane reporting green for an arm it never ran. Same
        // shape as CsoContentAddressingScenario's FAIL() on an unknown MGITEST_CSO_LANE.
        bool ArmNameIsRecognised() {
            const char* name = std::getenv(kArmMarker);
            return name == nullptr || std::strcmp(name, "handles") == 0 ||
                   std::strcmp(name, "legacy") == 0 || std::strcmp(name, "aba") == 0;
        }

        bool RunningInAHandleRecycleLane() { return std::getenv(kArmMarker) != nullptr; }

        const char* ArmName(Arm arm) {
            switch (arm) {
                case Arm::Handles: return "Handles";
                case Arm::AbaControl: return "AbaControl";
                default: return "Legacy";
            }
        }

        // A build-time marker set by MG_IntegrationTest/CMakeLists.txt. "1" means the thing it
        // names is present in the sources this binary was built from.
        bool BuildMarkerIsSet(const char* name) {
            const char* value = std::getenv(name);
            return value != nullptr && value[0] == '1' && value[1] == '\0';
        }

        // Whichever of the two backend re-keys applies to the process this binary is running as.
        bool ThisBackendsRekeyHasLanded() {
            const std::string& backend = HeadlessGL::Get().BackendName();
            if (backend == "DirectVulkan") return BuildMarkerIsSet("MGITEST_HANDLE_REKEY_DirectVulkan");
            return BuildMarkerIsSet("MGITEST_HANDLE_REKEY_DirectGLES");
        }

        // ---- the scene -----------------------------------------------------------------

        constexpr const char* kColorVS = R"(#version 330 core
in vec2 aPos;
in vec3 aColor;
out vec3 vColor;
void main() {
    vColor = aColor;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

        constexpr const char* kColorFS = R"(#version 330 core
in vec3 vColor;
out vec4 oColor;
void main() { oColor = vec4(vColor, 1.0); }
)";

        constexpr const char* kSampleVS = R"(#version 330 core
in vec2 aPos;
out vec2 vUv;
void main() {
    vUv = aPos * 0.5 + 0.5;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

        constexpr const char* kSampleFS = R"(#version 330 core
in vec2 vUv;
uniform sampler2D uTex;
out vec4 oColor;
void main() { oColor = texture(uTex, vUv); }
)";

        struct Vertex {
            float x, y;
            float r, g, b;
        };

        // A full-viewport quad in one colour. Both buffers are the SAME SIZE and the SAME
        // LAYOUT: only the colour bytes differ, which is what makes a content hash over the
        // vertex-input CONFIGURATION identical between them.
        std::vector<Vertex> Quad(float r, float g, float b) {
            return {
                {-1.0f, -1.0f, r, g, b}, {1.0f, -1.0f, r, g, b}, {1.0f, 1.0f, r, g, b},
                {-1.0f, -1.0f, r, g, b}, {1.0f, 1.0f, r, g, b},  {-1.0f, 1.0f, r, g, b},
            };
        }

        constexpr int kVertexCount = 6;
        // Enough consecutive drawing frames that every per-object memo in both backends is armed
        // against the first object before it is destroyed.
        constexpr int kWarmupFrames = 3;
        // How far inside the viewport the whole-region check starts. The quad covers everything,
        // so the inset is only about primitive edges on the outermost pixel row/column.
        constexpr int kInset = 2;

        void ExpectWholeViewportIs(const Image& image, const char* expected, const std::string& when) {
            EXPECT_TRUE(RegionIsMostly(image, kInset, image.Width() - kInset, kInset, image.Height() - kInset,
                                       expected, 0.0, when));
        }

        // The one thing the whole file turns on: did the pixels come from the REPLACEMENT
        // (`fresh`) or from the object that died (`stale`)? The arm decides which is the pass.
        void ExpectPixelsFor(Arm arm, bool armExpectsCorruption, const Image& image, const char* fresh,
                             const char* stale, const std::string& when) {
            if (arm == Arm::AbaControl && armExpectsCorruption) {
                // The corruption IS the assertion. If this ever goes green-by-being-correct the
                // reproducer has stopped reproducing and the other two arms prove nothing.
                ExpectWholeViewportIs(image, stale, when + " [AbaControl expects the STALE object's pixels: "
                                                          "the two guards are deliberately defeated]");
                return;
            }
            ExpectWholeViewportIs(image, fresh,
                                  when + " [" + ArmName(arm) + " expects the replacement's pixels]");
        }

        class HandleRecycleScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                if (!ArmNameIsRecognised()) {
                    const char* raw = std::getenv(kArmMarker);
                    FAIL() << "unknown " << kArmMarker << " value '" << (raw != nullptr ? raw : "")
                           << "': the arms are handles / legacy / aba. Reading an unrecognised name "
                              "as Legacy would make this lane assert the pre-re-key guards while "
                              "claiming to test something else, and it would pass.";
                }
                m_arm = CurrentArm();
                std::string error;
                m_colorProgram = CompileProgram(kColorVS, kColorFS, &error);
                ASSERT_NE(m_colorProgram, 0u) << error;
                m_sampleProgram = CompileProgram(kSampleVS, kSampleFS, &error);
                ASSERT_NE(m_sampleProgram, 0u) << error;
                ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "program setup left a GL error behind";
                RecordProperty("arm", ArmName(m_arm));
            }

            void TearDown() override {
                if (!Ready()) return;
                if (m_colorProgram != 0) glDeleteProgram(m_colorProgram);
                if (m_sampleProgram != 0) glDeleteProgram(m_sampleProgram);
            }

            // Skips the case when the arm it is running under has nothing to assert on THIS tree.
            // GTEST_SKIP() returns from the function it is written in, so this cannot report
            // through a return value; every caller pairs it with `if (IsSkipped()) return;`.
            void SkipUnlessTheArmIsAssertableHere() {
                if (!RunningInAHandleRecycleLane()) {
                    GTEST_SKIP() << "runs only in its own lane: the three HandleRecycle. ctest entries set "
                                    "MGITEST_HANDLE_ARM (handles / legacy / aba) together with the "
                                    "MOBILEGL_PIPE_PUSH and MOBILEGL_PIPE_LEGACY_MEMOS values that arm means. "
                                    "The ambient entries configure none of that, so there is nothing here to "
                                    "assert.";
                }
                // Both push arms are compiled only under MOBILEGL_PIPE_PUSH, so in a pull build
                // neither has anything to say whatever the source tree contains. This check comes
                // BEFORE the per-arm markers deliberately: those answer "does the source tree
                // implement it", which stops being a statement about this library the moment the
                // library is the pull one. Without it, a pull build would run the Handles arm
                // against a library with no {slot, gen} key (a green asserting nothing) and the
                // AbaControl arm against one whose guards are still in force (a hard red on
                // `ctest -L integration-gpu`, which G2 requires green in BOTH builds).
                // MG_IntegrationTest/CMakeLists.txt already withholds the markers in a pull build;
                // this is the second lock, so a hand-forced environment cannot arm them either.
                if (m_arm != Arm::Legacy && !BuildMarkerIsSet("MGITEST_PIPE_PUSH_BUILD")) {
                    GTEST_SKIP() << "the " << ArmName(m_arm)
                                 << " arm needs a library built with MOBILEGL_PIPE_PUSH, and this one "
                                    "was not: the {slot, gen} re-key and Features.PipeHandleAbaControl "
                                    "are both #if MOBILEGL_PIPE_PUSH (Config.h, ConfigLoader.cpp), so "
                                    "there is nothing here for either arm to assert against. The lane "
                                    "stays registered so that `ctest -L integration-gpu` names the same "
                                    "tests in the pull build and the push build (gate G2); the Legacy "
                                    "arm is the one that is meaningful here, and it runs.";
                }
                switch (m_arm) {
                    case Arm::Handles:
                        if (!ThisBackendsRekeyHasLanded()) {
                            GTEST_SKIP() << "the Handles arm needs the backend's {slot, gen} re-key, and this "
                                            "build does not have it: the build's capability probe found no "
                                            "slot table and no Track H subsystem constant under "
                                            "MobileGL/MG_Backend/"
                                         << Gl().BackendName()
                                         << " (P2 package C for DirectGLES, package D for DirectVulkan). The "
                                            "arm is registered and visible, and arms itself when that "
                                            "package lands in a push build.";
                        }
                        return;
                    case Arm::AbaControl:
                        if (!BuildMarkerIsSet("MGITEST_HANDLE_ABA_IMPLEMENTED")) {
                            GTEST_SKIP() << "the AbaControl arm needs MOBILEGL_PIPE_HANDLE_ABA_CONTROL to have a "
                                            "consumer, and this build has none: MG_Config parses the knob "
                                            "(ConfigLoader.cpp) but no source under MobileGL/MG_Backend/ reads "
                                            "Features.PipeHandleAbaControl, so the two guards the knob is "
                                            "supposed to defeat are still in force and the ABA cannot be "
                                            "reproduced. P2 package D owns that consumer.";
                        }
                        return;
                    default: return;
                }
            }

            // A VBO holding one solid-colour quad.
            GLuint MakeQuadBuffer(float r, float g, float b) {
                const std::vector<Vertex> vertices = Quad(r, g, b);
                GLuint buffer = 0;
                glGenBuffers(1, &buffer);
                glBindBuffer(GL_ARRAY_BUFFER, buffer);
                glBufferData(GL_ARRAY_BUFFER,
                             static_cast<GLsizeiptr>(vertices.size() * sizeof(Vertex)), vertices.data(),
                             GL_STATIC_DRAW);
                return buffer;
            }

            // The attribute configuration, spelled once so the two VAOs are byte-identical.
            void ConfigureQuadVao(GLuint vao, GLuint buffer) {
                glBindVertexArray(vao);
                glBindBuffer(GL_ARRAY_BUFFER, buffer);
                glEnableVertexAttribArray(0);
                glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                                      reinterpret_cast<const void*>(offsetof(Vertex, x)));
                glEnableVertexAttribArray(1);
                glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                                      reinterpret_cast<const void*>(offsetof(Vertex, r)));
            }

            Image DrawQuadAndRead(GLuint vao) {
                BindDefaultFramebuffer();
                ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
                glUseProgram(m_colorProgram);
                glBindVertexArray(vao);
                glDrawArrays(GL_TRIANGLES, 0, kVertexCount);
                const Image image = ReadPixels(Gl().Width(), Gl().Height());
                Gl().EndFrame();
                return image;
            }

            // A 2x2 RGBA8 texture of one colour, with the sampling parameters spelled the same
            // way both times so a parameter-shadow key matches too.
            GLuint MakeSolidTexture(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
                const std::uint8_t texels[16] = {r, g, b, 255, r, g, b, 255, r, g, b, 255, r, g, b, 255};
                GLuint texture = 0;
                glGenTextures(1, &texture);
                glBindTexture(GL_TEXTURE_2D, texture);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
                return texture;
            }

            Image DrawTexturedQuadAndRead(GLuint vao, GLuint texture) {
                BindDefaultFramebuffer();
                ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
                glUseProgram(m_sampleProgram);
                glUniform1i(glGetUniformLocation(m_sampleProgram, "uTex"), 0);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, texture);
                glBindVertexArray(vao);
                glDrawArrays(GL_TRIANGLES, 0, kVertexCount);
                const Image image = ReadPixels(Gl().Width(), Gl().Height());
                Gl().EndFrame();
                return image;
            }

            Arm m_arm = Arm::Legacy;
            GLuint m_colorProgram = 0;
            GLuint m_sampleProgram = 0;
        };

        // ------------------------------------------------------------------------------------
        // The self-check. Without it the three cases below could all be green because the name
        // allocator never repeated itself, i.e. because the ABA never happened.
        // ------------------------------------------------------------------------------------
        TEST_F(HandleRecycleScenario, TheReproducerRecyclesEveryName) {
            if (!Ready()) return;

            GLuint vao = 0;
            glGenVertexArrays(1, &vao);
            glBindVertexArray(vao);
            glBindVertexArray(0);
            glDeleteVertexArrays(1, &vao);
            GLuint vaoAgain = 0;
            glGenVertexArrays(1, &vaoAgain);
            EXPECT_EQ(vao, vaoAgain) << "glGenVertexArrays did not hand the deleted name back, so the "
                                        "vertex-array case below cannot be constructing an ABA";
            glDeleteVertexArrays(1, &vaoAgain);

            GLuint texture = 0;
            glGenTextures(1, &texture);
            glBindTexture(GL_TEXTURE_2D, texture);
            glBindTexture(GL_TEXTURE_2D, 0);
            glDeleteTextures(1, &texture);
            GLuint textureAgain = 0;
            glGenTextures(1, &textureAgain);
            EXPECT_EQ(texture, textureAgain) << "glGenTextures did not hand the deleted name back";
            glDeleteTextures(1, &textureAgain);

            GLuint fbo = 0;
            glGenFramebuffers(1, &fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            BindDefaultFramebuffer();
            glDeleteFramebuffers(1, &fbo);
            GLuint fboAgain = 0;
            glGenFramebuffers(1, &fboAgain);
            EXPECT_EQ(fbo, fboAgain) << "glGenFramebuffers did not hand the deleted name back";
            glDeleteFramebuffers(1, &fboAgain);

            EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
        }

        // ------------------------------------------------------------------------------------
        // 1. The vertex array. This is the case the AbaControl knob targets: DirectVulkan keys
        //    VertexInputStateFactory's cache on the attribute's buffer identity and VaoDrawMemo
        //    on the VAO's, and BOTH the VAO and the buffer are recycled here so that a key built
        //    out of raw addresses matches while the bytes behind it do not.
        // ------------------------------------------------------------------------------------
        TEST_F(HandleRecycleScenario, AVertexArrayAtARecycledAddressDoesNotInheritItsPredecessorsVertexInput) {
            if (!Ready()) return;
            SkipUnlessTheArmIsAssertableHere();
            if (IsSkipped()) return;

            const GLuint redBuffer = MakeQuadBuffer(1.0f, 0.0f, 0.0f);
            GLuint redVao = 0;
            glGenVertexArrays(1, &redVao);
            ConfigureQuadVao(redVao, redBuffer);
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "building the first VAO left a GL error behind";

            for (int frame = 0; frame < kWarmupFrames; ++frame) {
                const Image warm = DrawQuadAndRead(redVao);
                ExpectWholeViewportIs(warm, "red", "warm-up frame " + std::to_string(frame));
            }

            // Unbind FIRST: a still-bound object keeps living, so the last SharedPtr would not
            // drop and there would be no freed block for the replacement to land in.
            glBindVertexArray(0);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            glDeleteVertexArrays(1, &redVao);
            GLuint doomedBuffer = redBuffer;
            glDeleteBuffers(1, &doomedBuffer);

            // The replacement, immediately and in the reverse order of the frees, which is the
            // order a size-classed allocator is most likely to answer from its free lists.
            const GLuint greenBuffer = MakeQuadBuffer(0.0f, 1.0f, 0.0f);
            GLuint greenVao = 0;
            glGenVertexArrays(1, &greenVao);
            ConfigureQuadVao(greenVao, greenBuffer);
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "building the replacement VAO left a GL error behind";

            // The skip below is the LAST thing that can save a run in which the allocator did not
            // repeat itself, and it only sees half of what matters: the names. If the names come
            // back but the heap blocks do not, execution continues into an assertion the
            // AbaControl arm expects to see corrupted pixels from - and that arm then FAILS
            // rather than skipping, in an always-on integration-gpu lane. The header says why that
            // trade is taken deliberately; this is where the consequence lands.
            if (greenVao != redVao || greenBuffer != redBuffer) {
                GTEST_SKIP() << "inconclusive, not proven: the name allocator did not hand both names back "
                                "(vao " << redVao << " -> " << greenVao << ", buffer " << redBuffer << " -> "
                             << greenBuffer << "), so no ABA was constructed";
            }
            RecordProperty("recycled_vao_name", static_cast<int>(greenVao));

            const Image image = DrawQuadAndRead(greenVao);
            ExpectPixelsFor(m_arm, /*armExpectsCorruption=*/true, image, "green", "red",
                            "the draw after the VAO and its buffer were both recycled");

            glBindVertexArray(0);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            glDeleteVertexArrays(1, &greenVao);
            GLuint cleanup = greenBuffer;
            glDeleteBuffers(1, &cleanup);
        }

        // ------------------------------------------------------------------------------------
        // 2. The texture. DirectGLES keeps a backend twin per frontend texture in a registry
        //    keyed on the frontend object's address (StateBackendObjectRegistry + the
        //    UnitSamplerLookupMemo's weak_ptr test); a replacement at the same address must not
        //    sample the dead texture's driver object.
        //
        //    The AbaControl knob does not steer this path, so this case expects the correct
        //    pixels in EVERY arm - stated explicitly rather than by omission.
        // ------------------------------------------------------------------------------------
        TEST_F(HandleRecycleScenario, ATextureAtARecycledAddressDoesNotInheritItsPredecessorsTwin) {
            if (!Ready()) return;
            SkipUnlessTheArmIsAssertableHere();
            if (IsSkipped()) return;

            const GLuint buffer = MakeQuadBuffer(1.0f, 1.0f, 1.0f);
            GLuint vao = 0;
            glGenVertexArrays(1, &vao);
            ConfigureQuadVao(vao, buffer);

            const GLuint redTexture = MakeSolidTexture(255, 0, 0);
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "building the first texture left a GL error behind";
            for (int frame = 0; frame < kWarmupFrames; ++frame) {
                const Image warm = DrawTexturedQuadAndRead(vao, redTexture);
                ExpectWholeViewportIs(warm, "red", "warm-up frame " + std::to_string(frame));
            }

            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, 0);
            GLuint doomed = redTexture;
            glDeleteTextures(1, &doomed);

            const GLuint greenTexture = MakeSolidTexture(0, 255, 0);
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "building the replacement texture left a GL error";
            if (greenTexture != redTexture) {
                GTEST_SKIP() << "inconclusive, not proven: glGenTextures returned " << greenTexture
                             << " rather than the deleted " << redTexture << ", so no ABA was constructed";
            }
            RecordProperty("recycled_texture_name", static_cast<int>(greenTexture));

            const Image image = DrawTexturedQuadAndRead(vao, greenTexture);
            ExpectPixelsFor(m_arm, /*armExpectsCorruption=*/false, image, "green", "red",
                            "the draw after the texture was recycled");

            glBindTexture(GL_TEXTURE_2D, 0);
            GLuint cleanupTexture = greenTexture;
            glDeleteTextures(1, &cleanupTexture);
            glBindVertexArray(0);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            glDeleteVertexArrays(1, &vao);
            GLuint cleanupBuffer = buffer;
            glDeleteBuffers(1, &cleanupBuffer);
        }

        // ------------------------------------------------------------------------------------
        // 3. The framebuffer. The readback is deliberately NOT from the framebuffer under test:
        //    a clear that landed in the WRONG framebuffer would still read back green through
        //    that framebuffer. It is taken from the replacement's own attachment with
        //    glGetTexImage, so "the clear went somewhere else" is visible as a texture that
        //    never became green.
        // ------------------------------------------------------------------------------------
        TEST_F(HandleRecycleScenario, AFramebufferAtARecycledAddressDoesNotInheritItsPredecessorsTwin) {
            if (!Ready()) return;
            SkipUnlessTheArmIsAssertableHere();
            if (IsSkipped()) return;

            // Two attachments that stay alive for the whole case, so the only recycled object is
            // the framebuffer itself.
            GLuint firstAttachment = 0;
            GLuint secondAttachment = 0;
            glGenTextures(1, &firstAttachment);
            glBindTexture(GL_TEXTURE_2D, firstAttachment);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glGenTextures(1, &secondAttachment);
            glBindTexture(GL_TEXTURE_2D, secondAttachment);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glBindTexture(GL_TEXTURE_2D, 0);

            GLuint firstFbo = 0;
            glGenFramebuffers(1, &firstFbo);
            glBindFramebuffer(GL_FRAMEBUFFER, firstFbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, firstAttachment, 0);
            ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GLenum(GL_FRAMEBUFFER_COMPLETE));
            for (int frame = 0; frame < kWarmupFrames; ++frame) {
                glBindFramebuffer(GL_FRAMEBUFFER, firstFbo);
                glViewport(0, 0, 4, 4);
                ClearTo(1.0f, 0.0f, 0.0f, 1.0f);
                BindDefaultFramebuffer();
                Gl().EndFrame();
            }
            BindDefaultFramebuffer();
            glDeleteFramebuffers(1, &firstFbo);

            GLuint secondFbo = 0;
            glGenFramebuffers(1, &secondFbo);
            if (secondFbo != firstFbo) {
                glDeleteFramebuffers(1, &secondFbo);
                GLuint cleanup[2] = {firstAttachment, secondAttachment};
                glDeleteTextures(2, cleanup);
                GTEST_SKIP() << "inconclusive, not proven: glGenFramebuffers returned " << secondFbo
                             << " rather than the deleted " << firstFbo << ", so no ABA was constructed";
            }
            RecordProperty("recycled_framebuffer_name", static_cast<int>(secondFbo));

            glBindFramebuffer(GL_FRAMEBUFFER, secondFbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, secondAttachment, 0);
            ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GLenum(GL_FRAMEBUFFER_COMPLETE));
            glViewport(0, 0, 4, 4);
            ClearTo(0.0f, 1.0f, 0.0f, 1.0f);
            BindDefaultFramebuffer();
            Gl().EndFrame();

            // Read the REPLACEMENT'S attachment, not the framebuffer: that is what makes "the
            // clear landed in the dead framebuffer" visible.
            std::vector<std::uint8_t> texels(4 * 4 * 4, 0);
            glBindTexture(GL_TEXTURE_2D, secondAttachment);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels.data());
            glBindTexture(GL_TEXTURE_2D, 0);
            EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));

            // Every texel of the replacement's attachment must be the green it was cleared to.
            int offenders = 0;
            for (std::size_t i = 0; i < texels.size(); i += 4) {
                if (texels[i] != 0 || texels[i + 1] != 255 || texels[i + 2] != 0) ++offenders;
            }
            EXPECT_EQ(offenders, 0) << "the replacement framebuffer's own attachment is not the colour it was "
                                       "cleared to, so the clear reached a framebuffer this one only shares an "
                                       "address with (first texel rgba="
                                    << static_cast<int>(texels[0]) << "," << static_cast<int>(texels[1]) << ","
                                    << static_cast<int>(texels[2]) << "," << static_cast<int>(texels[3]) << ")";

            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDeleteFramebuffers(1, &secondFbo);
            GLuint cleanup[2] = {firstAttachment, secondAttachment};
            glDeleteTextures(2, cleanup);
        }

    } // namespace
} // namespace MGITest
