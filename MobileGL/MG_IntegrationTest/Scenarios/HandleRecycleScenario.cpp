// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/HandleRecycleScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - THE HANDLE ABA (gate G8): a frontend object that dies and is replaced at the same
// heap address must not inherit the dead object's backend twin, its vertex-input state, its
// buffer contents, or its draw memo.
//
// P3a ADDS TWO WINDOWS to the four P2 wrote, because it re-keys two more object classes. The
// BUFFER (ABufferAtARecycledAddressDoesNotInheritItsPredecessorsContents) is the resource_*
// family's: a store that dies and is replaced at the same slot must not hand the replacement's
// draw the dead store's bytes. The two-buffer VERTEX SET
// (AVertexArrayAtARecycledAddressDoesNotInheritItsPredecessorsVertexBufferSet) is
// set_vertex_buffers': the per-binding buffer identities are a record of their own, separate
// from the elements blob, and a recycled VAO must not inherit its predecessor's.
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
//      content hash over the configuration matches the dead object's;
//   4. the replacement is given DIFFERENT CONTENTS - a different vertex buffer, different texels,
//      a different attachment;
//   5. one draw, one readback. The pixels must come from the replacement.
//
// The public-GL proxy for "the allocator repeated itself" is the GL NAME: MobileGL's name
// allocators hand a deleted name straight back, so `TheReproducerRecyclesEveryName` asserts the
// recycle happened and every other case asserts on the name it got. When a name is NOT recycled
// the case SKIPS with that reason rather than passing - the shape
// MG_Test/State/ObjectLifetimeIdTest.cpp already uses for exactly this ("inconclusive, not
// proven").
//
// WHAT THE NAME PROXY DOES NOT BUY, MEASURED RATHER THAN ASSUMED. The name comes back; the C++
// HEAP BLOCK does not. A VertexArrayObject is 3920 bytes - past glibc's tcache - so its chunk goes
// to the unsorted bin and is split by the very next allocation the replacement path makes; four
// create/delete cycles in one run of this file produced four distinct addresses about a mebibyte
// apart, and the same is true of the BufferObject. An earlier revision of this file left the
// AbaControl arm's collision to that allocator, and the consequence was the failure mode this file
// exists to prevent, in its most literal form: with nothing colliding, the replacement inherited
// nothing, the arm asserted stale pixels, saw fresh ones, and went RED in an always-on
// integration-gpu lane while every guard it was supposed to be defeating was still standing.
//
// So the AbaControl arm no longer asks the allocator for the collision - MOBILEGL_PIPE_HANDLE_ABA_CONTROL
// manufactures it, by replacing the object identity in each key with a constant (see
// MagmaPipeArms.h's MagmaPipeAbaControlDefeatsIdentity). That is the strongest form of "the
// allocator handed the block back", it is deterministic, and - the reason it matters - it defeats
// the {slot, gen} GENERATION as well as the retired lifetime id, so the control covers the key P2
// actually ships instead of only the one it replaced.
//
// AND THE ABA HAPPENS INSIDE ONE FRAME, which is not a detail. The only backend structure that can
// hand a draw a dead object's GPU slice is VulkanRenderer::ResolvedVertexBindings, and it refuses
// to be trusted across a frame boundary by design ("NO cross-frame trust"). Every other memo the
// recycle can poison holds LAYOUT, which is byte-identical between the two objects by construction
// and so cannot be seen in pixels. A reproducer that puts a frame boundary between the arming draw
// and the recycled draw therefore cannot produce wrong pixels no matter how completely the keys
// collide - it would be asserting a fact about the frame gate, not about identity.
//
// THREE ARMS, ALL ALWAYS ON (P2 brief D18). The arm is named by MGITEST_HANDLE_ARM, which is a
// HARNESS marker - the library never reads it - and the CMake wiring registers one lane per arm:
//
//   Handles     MOBILEGL_PIPE_PUSH default (Track H bits set), MOBILEGL_PIPE_LEGACY_MEMOS=0.
//               The {slot, gen} key is the only key in the process. Expects correct pixels.
//   Legacy      MOBILEGL_PIPE_PUSH=0. Today's lifetimeId + weak_ptr guards. Expects correct
//               pixels - they work, which is the point: the re-key is not fixing a live bug, it
//               is replacing a guard, and the replacement has to be at least as strong.
//   AbaControl  MOBILEGL_PIPE_HANDLE_ABA_CONTROL=1, on TWO lanes: one with MOBILEGL_PIPE_PUSH=0
//               (the pre-handle arm, D18's lane verbatim) and one on the handle arm
//               (MOBILEGL_PIPE_LEGACY_MEMOS=0). The knob defeats the object-identity half of
//               every vertex-input memo key on whichever arm is running - the pre-handle
//               (address, lifetime id) pair and the handle arm's {slot, gen} generation - so both
//               lanes expect the CORRUPTION. Two lanes rather than one because the guard P2 SHIPS
//               is the generation: a control that only defeated the retired guards would be green
//               forever without saying anything about the re-key, which is exactly how this arm
//               went vacuous once packages C and D landed.
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
#include <iostream>
#include <string>
#include <vector>

#include "../Harness/HeadlessGL.h"
#include "../Harness/PipeSlotPeek.h"
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

        // The SAME question for the BUFFER, and it is a different question. The marker above
        // answers "is this backend's VERTEX-INPUT memo keyed on {slot, gen}", which P2 landed;
        // a buffer only travels as a handle once the resource_* family does (P3a for Espryt,
        // P7 for Magma), and until then a buffer's backend twin is still resolved from the
        // frontend object. A buffer case that read the P2 marker would therefore report the
        // Handles arm as armed on a tree where nothing about a buffer is keyed on a handle -
        // green for a re-key that does not exist, which is the one outcome this file exists to
        // prevent. Set by MG_IntegrationTest/CMakeLists.txt from a content probe for
        // MGPipeResourceOps under each backend's own directory.
        bool ThisBackendsResourceRekeyHasLanded() {
            const std::string& backend = HeadlessGL::Get().BackendName();
            if (backend == "DirectVulkan") {
                return BuildMarkerIsSet("MGITEST_HANDLE_REKEY_RESOURCES_DirectVulkan");
            }
            return BuildMarkerIsSet("MGITEST_HANDLE_REKEY_RESOURCES_DirectGLES");
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
            // Say which of the two was actually observed, on EVERY arm and whether or not the case
            // passes. The arm's expectation is only half the evidence, and a reader of the CI log
            // should not have to infer the other half from the exit status - least of all for a
            // control whose whole claim is "the corruption is still reproducible here".
            const bool sawStale = static_cast<bool>(RegionIsMostly(
                image, kInset, image.Width() - kInset, kInset, image.Height() - kInset, stale, 0.0, when));
            const bool sawFresh = static_cast<bool>(RegionIsMostly(
                image, kInset, image.Width() - kInset, kInset, image.Height() - kInset, fresh, 0.0, when));
            const bool expectsStale = arm == Arm::AbaControl && armExpectsCorruption;
            std::cout << "[ HandleRecycle ] arm=" << ArmName(arm) << " expected="
                      << (expectsStale ? "STALE" : "FRESH") << " observed="
                      << (sawStale ? "STALE" : (sawFresh ? "FRESH" : "NEITHER")) << " (stale=" << stale
                      << ", fresh=" << fresh << ") - " << when << std::endl;
            if (expectsStale) {
                // The corruption IS the assertion. If this ever goes green-by-being-correct the
                // reproducer has stopped reproducing and the other two arms prove nothing.
                ExpectWholeViewportIs(image, stale, when + " [AbaControl expects the STALE object's pixels: "
                                                          "the identity half of every key is deliberately "
                                                          "defeated]");
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

            // The buffer case's extra gate, on top of the arm gate above. Only the Handles arm
            // needs it: `Legacy` asserts today's guards (which exist on every tree) and
            // `AbaControl` is gated on the knob's consumer already.
            void SkipUnlessTheResourceHandlePathIsAssertableHere() {
                if (m_arm != Arm::Handles) return;
                if (!ThisBackendsResourceRekeyHasLanded()) {
                    GTEST_SKIP() << "subsystem not implemented on this tree: the buffer's Handles arm "
                                    "needs this backend's resource_* op table, and the build's capability "
                                    "probe found no source under MobileGL/MG_Backend/"
                                 << Gl().BackendName()
                                 << " naming MGPipeResourceOps. Until it lands, a buffer's backend twin is "
                                    "still resolved from the frontend BufferObject, so there is no "
                                    "{slot, gen} buffer key here to assert about (P3a package C for "
                                    "DirectGLES; Magma's buffer path is P7). The entry stays registered "
                                    "and visible, and arms itself when that package lands in a push "
                                    "build.";
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

            // The same quad, split across TWO buffers - positions in one, colours in the other,
            // one MGPVertexBuffer entry each. What travels in a P3a `set_vertex_buffers` is the
            // per-binding BUFFER IDENTITY (D-H3: Res, Offset 0, the resolved stride and
            // divisor); the formats live in the vertex-elements blob and are byte-identical
            // between the two VAOs by construction. Splitting the set is what lets a PARTIAL
            // inheritance be seen: with one buffer, a stale set and a stale everything look the
            // same in the pixels.
            GLuint MakePositionBuffer() {
                const float positions[12] = {-1.0f, -1.0f, 1.0f, -1.0f, 1.0f, 1.0f,
                                             -1.0f, -1.0f, 1.0f, 1.0f,  -1.0f, 1.0f};
                GLuint buffer = 0;
                glGenBuffers(1, &buffer);
                glBindBuffer(GL_ARRAY_BUFFER, buffer);
                glBufferData(GL_ARRAY_BUFFER, sizeof(positions), positions, GL_STATIC_DRAW);
                return buffer;
            }

            GLuint MakeColorBuffer(float r, float g, float b) {
                const float colors[18] = {r, g, b, r, g, b, r, g, b, r, g, b, r, g, b, r, g, b};
                GLuint buffer = 0;
                glGenBuffers(1, &buffer);
                glBindBuffer(GL_ARRAY_BUFFER, buffer);
                glBufferData(GL_ARRAY_BUFFER, sizeof(colors), colors, GL_STATIC_DRAW);
                return buffer;
            }

            void ConfigureSplitQuadVao(GLuint vao, GLuint positionBuffer, GLuint colorBuffer) {
                glBindVertexArray(vao);
                glBindBuffer(GL_ARRAY_BUFFER, positionBuffer);
                glEnableVertexAttribArray(0);
                glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
                glBindBuffer(GL_ARRAY_BUFFER, colorBuffer);
                glEnableVertexAttribArray(1);
                glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
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
        //    on the VAO's. Only the VAO is recycled here: both buffers are created before the
        //    window and neither is deleted inside it, because buffer traffic in the window moves
        //    VkBufferManager's slice-epoch counter and that gate is not an identity gate (see
        //    the two MakeQuadBuffer calls). What is recycled is the GL NAME; the heap block is
        //    not handed back, which is why the knob - not the allocator - constructs the
        //    AbaControl arms' collision.
        // ------------------------------------------------------------------------------------
        TEST_F(HandleRecycleScenario, AVertexArrayAtARecycledAddressDoesNotInheritItsPredecessorsVertexInput) {
            if (!Ready()) return;
            SkipUnlessTheArmIsAssertableHere();
            if (IsSkipped()) return;

            // BOTH buffers are created, and both are DRAWN WITH, before the recycle happens.
            // Creating a buffer - or touching one for the first time - moves VkBufferManager's
            // manager-wide slice-epoch counter, and a moved counter sends the resolved-bindings
            // memo into a revalidation that re-reads every binding from the live VAO. That gate is
            // not an identity gate and it is not what this case is about, so both buffers are
            // realised up front and the ABA window contains no buffer traffic at all.
            const GLuint redBuffer = MakeQuadBuffer(1.0f, 0.0f, 0.0f);
            const GLuint greenBuffer = MakeQuadBuffer(0.0f, 1.0f, 0.0f);

            GLuint primerVao = 0;
            glGenVertexArrays(1, &primerVao);
            ConfigureQuadVao(primerVao, greenBuffer);
            const Image primed = DrawQuadAndRead(primerVao);
            ExpectWholeViewportIs(primed, "green", "priming the replacement's buffer");

            GLuint redVao = 0;
            glGenVertexArrays(1, &redVao);
            ConfigureQuadVao(redVao, redBuffer);
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "building the first VAO left a GL error behind";

            for (int frame = 0; frame < kWarmupFrames; ++frame) {
                const Image warm = DrawQuadAndRead(redVao);
                ExpectWholeViewportIs(warm, "red", "warm-up frame " + std::to_string(frame));
            }

            // ---- the ABA window: ONE frame, two draws ----
            //
            // The arming draw and the recycled draw share a frame because
            // ResolvedVertexBindings - the only memo that carries a GPU slice rather than a
            // layout - declines across frames by design. See the header.
            BindDefaultFramebuffer();
            ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
            glUseProgram(m_colorProgram);
            glBindVertexArray(redVao);
            glDrawArrays(GL_TRIANGLES, 0, kVertexCount);

            // Unbind FIRST: a still-bound object keeps living, so the last SharedPtr would not
            // drop and the object would not die here at all.
            glBindVertexArray(0);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            glDeleteVertexArrays(1, &redVao);

            // The replacement, immediately, byte-identically configured, and reading the OTHER
            // buffer - so its pixels differ from its predecessor's by exactly the thing a stale
            // vertex binding would get wrong.
            GLuint greenVao = 0;
            glGenVertexArrays(1, &greenVao);
            ConfigureQuadVao(greenVao, greenBuffer);
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "building the replacement VAO left a GL error behind";

            glBindVertexArray(greenVao);
            glDrawArrays(GL_TRIANGLES, 0, kVertexCount);
            const Image image = ReadPixels(Gl().Width(), Gl().Height());
            Gl().EndFrame();

            // The name proxy. It no longer constructs the AbaControl arm's collision - the knob
            // does that, deterministically, because the heap block is never handed back (header) -
            // but it is still what makes this a RECYCLE rather than two unrelated objects, and it
            // is what the Handles and Legacy arms are asserting is not enough to inherit anything.
            if (greenVao != redVao) {
                GTEST_SKIP() << "inconclusive, not proven: glGenVertexArrays returned " << greenVao
                             << " rather than the deleted " << redVao << ", so no ABA was constructed";
            }
            RecordProperty("recycled_vao_name", static_cast<int>(greenVao));

            ExpectPixelsFor(m_arm, /*armExpectsCorruption=*/true, image, "green", "red",
                            "the draw after the VAO was recycled inside one frame");

            glBindVertexArray(0);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            GLuint cleanupVaos[2] = {greenVao, primerVao};
            glDeleteVertexArrays(2, cleanupVaos);
            GLuint cleanupBuffers[2] = {redBuffer, greenBuffer};
            glDeleteBuffers(2, cleanupBuffers);
        }

        // ------------------------------------------------------------------------------------
        // 1a. The same recycle, over a vertex-input SET of TWO buffers - the shape a P3a
        //     `set_vertex_buffers` actually has.
        //
        //     The case above swaps the ONE buffer its VAO reads, so "inherited the dead VAO's
        //     vertex input" and "inherited the dead VAO's everything" are the same picture. P3a
        //     splits that record in two: the formats travel once per configuration, in
        //     `create_vertex_elements`' blob, and the per-binding BUFFER IDENTITIES travel in
        //     `set_vertex_buffers` (D-G2, D-H3). So the replacement here shares its predecessor's
        //     POSITION buffer and differs in the COLOUR buffer alone: the elements blob is
        //     byte-identical between the two VAOs, exactly one entry of the buffer set moved, and
        //     a replacement that inherited the dead VAO's set draws its own geometry in the dead
        //     VAO's colour. A single-buffer window cannot produce that picture.
        //
        //     A SEPARATE CASE RATHER THAN A SECOND WINDOW IN THE ONE ABOVE, and the reason is
        //     measured. gtest_discover_tests registers one ctest entry per case, so a case is a
        //     PROCESS; the AbaControl knob collapses every VAO in a process onto one memo entry,
        //     and that entry carries the resolved vertex-input LAYOUT as well as the bindings.
        //     Two windows in one process therefore means the second window's VAOs inherit the
        //     first window's layout - and these VAOs deliberately do NOT share the first's
        //     (interleaved stride 20 there, two tight arrays here). Run as a second phase, the
        //     AbaControl arms read the split VAOs' vertices through the interleaved layout and
        //     every draw in the phase, priming and warm-up included, came back as garbage
        //     (measured: "should be all green ... first offender is blue"). That is not the
        //     identity claim failing, it is the arm's own knob poisoning the setup - so the
        //     window gets a process of its own, where every VAO carries the same layout and the
        //     buffer identity is again the only thing that differs.
        // ------------------------------------------------------------------------------------
        TEST_F(HandleRecycleScenario,
               AVertexArrayAtARecycledAddressDoesNotInheritItsPredecessorsVertexBufferSet) {
            if (!Ready()) return;
            SkipUnlessTheArmIsAssertableHere();
            if (IsSkipped()) return;

            const GLuint positionBuffer = MakePositionBuffer();
            const GLuint redColorBuffer = MakeColorBuffer(1.0f, 0.0f, 0.0f);
            const GLuint greenColorBuffer = MakeColorBuffer(0.0f, 1.0f, 0.0f);

            // Same rule as the case above: every buffer is realised and DRAWN WITH before the
            // window, so the window contains no buffer traffic and the slice-epoch gate - which
            // is not an identity gate - is not what decides the verdict.
            GLuint splitPrimerVao = 0;
            glGenVertexArrays(1, &splitPrimerVao);
            ConfigureSplitQuadVao(splitPrimerVao, positionBuffer, greenColorBuffer);
            const Image splitPrimed = DrawQuadAndRead(splitPrimerVao);
            ExpectWholeViewportIs(splitPrimed, "green", "priming the split replacement's colour buffer");

            GLuint splitRedVao = 0;
            glGenVertexArrays(1, &splitRedVao);
            ConfigureSplitQuadVao(splitRedVao, positionBuffer, redColorBuffer);
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "building the split VAO left a GL error behind";
            for (int frame = 0; frame < kWarmupFrames; ++frame) {
                const Image warm = DrawQuadAndRead(splitRedVao);
                ExpectWholeViewportIs(warm, "red", "split warm-up frame " + std::to_string(frame));
            }

            BindDefaultFramebuffer();
            ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
            glUseProgram(m_colorProgram);
            glBindVertexArray(splitRedVao);
            glDrawArrays(GL_TRIANGLES, 0, kVertexCount);

            glBindVertexArray(0);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            glDeleteVertexArrays(1, &splitRedVao);

            GLuint splitGreenVao = 0;
            glGenVertexArrays(1, &splitGreenVao);
            ConfigureSplitQuadVao(splitGreenVao, positionBuffer, greenColorBuffer);
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR))
                << "building the split replacement VAO left a GL error behind";
            glBindVertexArray(splitGreenVao);
            glDrawArrays(GL_TRIANGLES, 0, kVertexCount);
            const Image splitImage = ReadPixels(Gl().Width(), Gl().Height());
            Gl().EndFrame();

            if (splitGreenVao != splitRedVao) {
                GTEST_SKIP() << "inconclusive, not proven: glGenVertexArrays returned " << splitGreenVao
                             << " rather than the deleted " << splitRedVao
                             << ", so no ABA was constructed for the split vertex-buffer set";
            }
            RecordProperty("recycled_split_vao_name", static_cast<int>(splitGreenVao));

            ExpectPixelsFor(m_arm, /*armExpectsCorruption=*/true, splitImage, "green", "red",
                            "the draw after a VAO reading a two-buffer vertex set was recycled inside "
                            "one frame");

            glBindVertexArray(0);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            GLuint splitCleanupVaos[2] = {splitGreenVao, splitPrimerVao};
            glDeleteVertexArrays(2, splitCleanupVaos);
            GLuint splitCleanupBuffers[3] = {positionBuffer, redColorBuffer, greenColorBuffer};
            glDeleteBuffers(3, splitCleanupBuffers);
        }


        // ------------------------------------------------------------------------------------
        // 1b. The BUFFER (P3a). Today a buffer's backend twin is reached from the frontend
        //     object: Espryt keys GLESBufferResource off the BufferObject's SharedPtr and
        //     re-probes it per draw through IsBufferDrawClean, and Magma mixes the
        //     BufferObject's lifetime id into VertexInputStateFactory's content hash - the guard
        //     commit 66b3b6e2 added after a destroyed buffer's GPU slice was bound for its
        //     successor's draw. P3a replaces that reachability with a client-minted handle: the
        //     store lives in a slot table, ~BufferObject emits `resource_destroy` and THEN frees
        //     the slot (D-L), and the next buffer is handed the same slot with Gen + 1.
        //
        //     This case is the pixel-level question about that swap: does a buffer created
        //     immediately after another one died, at the same GL name and the same {slot}, get
        //     its own bytes? It is the buffer twin of the vertex-array case above and it is
        //     written FIRST, against the P3a contract commit, so that whatever the Legacy arm
        //     reports here is on the record before package C touches a backend.
        //
        //     THE VAO IS NOT RECYCLED HERE - it is created once and outlives the whole case.
        //     Only the buffer dies. That is also why the VAO's attributes are PARKED on a
        //     buffer that never dies before the delete: a VAO attribute holds a
        //     SharedPtr<BufferObject> (MGPipeValueTypes.h:516), so while the VAO still points at
        //     the doomed buffer the frontend object cannot die, glDeleteBuffers only unnames it,
        //     and there would be no recycle to construct at all.
        // ------------------------------------------------------------------------------------
        TEST_F(HandleRecycleScenario, ABufferAtARecycledAddressDoesNotInheritItsPredecessorsContents) {
            if (!Ready()) return;
            SkipUnlessTheArmIsAssertableHere();
            if (IsSkipped()) return;
            SkipUnlessTheResourceHandlePathIsAssertableHere();
            if (IsSkipped()) return;

            // Both survivors are realised and drawn with before the window, for the reason the
            // vertex-array case gives: a buffer touched for the first time moves the manager-wide
            // slice epoch, and that gate is not an identity gate.
            const GLuint parkingBuffer = MakeQuadBuffer(0.0f, 0.0f, 1.0f);
            GLuint vao = 0;
            glGenVertexArrays(1, &vao);
            ConfigureQuadVao(vao, parkingBuffer);
            const Image parked = DrawQuadAndRead(vao);
            ExpectWholeViewportIs(parked, "blue", "priming the parking buffer");

            const GLuint redBuffer = MakeQuadBuffer(1.0f, 0.0f, 0.0f);
            ConfigureQuadVao(vao, redBuffer);
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "building the first buffer left a GL error behind";

            for (int frame = 0; frame < kWarmupFrames; ++frame) {
                const Image warm = DrawQuadAndRead(vao);
                ExpectWholeViewportIs(warm, "red", "warm-up frame " + std::to_string(frame));
            }

            // ---- the ABA window: ONE frame, two draws ----
            BindDefaultFramebuffer();
            ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
            glUseProgram(m_colorProgram);
            glBindVertexArray(vao);
            glDrawArrays(GL_TRIANGLES, 0, kVertexCount);

            // Let go of the doomed buffer - from the VAO's attributes and from the binding point -
            // and only then delete it, so the frontend object really dies here.
            ConfigureQuadVao(vao, parkingBuffer);
            glBindVertexArray(0);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            GLuint doomed = redBuffer;
            glDeleteBuffers(1, &doomed);

            // The replacement, immediately, with the same size and the same layout - so a store
            // pooled by size, a twin resolved by identity or a content hash over the
            // configuration all match the dead buffer's - and DIFFERENT CONTENTS, which is the
            // only thing that differs and the only thing the pixels can show.
            const GLuint greenBuffer = MakeQuadBuffer(0.0f, 1.0f, 0.0f);
            ConfigureQuadVao(vao, greenBuffer);
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR))
                << "building the replacement buffer left a GL error behind";

            glBindVertexArray(vao);
            glDrawArrays(GL_TRIANGLES, 0, kVertexCount);
            const Image image = ReadPixels(Gl().Width(), Gl().Height());
            Gl().EndFrame();

            if (greenBuffer != redBuffer) {
                GTEST_SKIP() << "inconclusive, not proven: glGenBuffers returned " << greenBuffer
                             << " rather than the deleted " << redBuffer << ", so no ABA was constructed";
            }
            RecordProperty("recycled_buffer_name", static_cast<int>(greenBuffer));

            // The AbaControl arm expects the CORRUPTION here, and that this window can produce
            // it at all is a measurement rather than an assumption. MOBILEGL_PIPE_HANDLE_ABA_CONTROL
            // has two consumers, both Magma's, and the one that decides this case is
            // VertexInputStateFactory::ComputeHash's `bufferKey = 0`: with the BUFFER's identity
            // gone from the vertex-input content hash, TryBindResolvedVertexBindings accepts a
            // binding resolved from the dead buffer as proof that it still reads the live one -
            // the exact defect that hash was fixed for. The open question was whether the
            // guards the control deliberately leaves standing would mask it, because one of
            // them, VkBufferManager's manager-wide slice epoch, MOVES when the replacement is
            // created and the replacement is created INSIDE this window by construction (a
            // buffer ABA cannot be built without creating a buffer in it). It does not: on the
            // contract tree both AbaControl lanes read the dead buffer's colour over 100% of
            // the viewport ("first offender at (2,2) is red rgba(255,0,0,255)"). So the control
            // reaches the buffer path too, and the line below records what was observed on
            // EVERY arm, whether or not the case passes.
            //
            // What it does NOT reach is Espryt - the knob has no DirectGLES consumer, which is
            // why the AbaControl arm is registered on DirectVulkan lanes only - nor the
            // {slot, gen} GENERATION, for the reason MagmaPipeAbaControlDefeatsIdentity gives.
            // Package C's buffer re-key is where a Features.PipeHandleAbaControl consumer over
            // the resource slot table would go, the way MagmaPipeClaimSlotMemos is Magma's.
            ExpectPixelsFor(m_arm, /*armExpectsCorruption=*/true, image, "green", "red",
                            "the draw after the buffer was recycled inside one frame");

            glBindVertexArray(0);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            glDeleteVertexArrays(1, &vao);
            GLuint cleanupBuffers[2] = {parkingBuffer, greenBuffer};
            glDeleteBuffers(2, cleanupBuffers);
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

        // ------------------------------------------------------------------------------------
        // P3a C-1: the recycle has to give the SLOT back, not only refuse to alias.
        //
        // Everything above asks "did the replacement inherit the dead object's state". This asks
        // the other half of the same identity contract, which no case in this file could see:
        // when the dead object is not replaced at all, does the client hand its {slot, gen}
        // back? Until C-1 the answer under DirectVulkan was NO. The mint is the client's
        // (MGPipeVertexInputEmitter::EmitVertexElements acquires a VertexElementsCso slot at
        // every validate point with a VAO bound) and the only free in the tree was Espryt's
        // StateObjectDeathOps consumer - so under Magma, which installs none deliberately
        // (MagmaPipeArms.h: "an allocator here would grow by one SlotState plus one map node
        // per object EVER created, for the life of the process, on a platform with an LMK"),
        // every VAO ever created held its slot and its ~1.3 KB applier record until the process
        // died, on the shipped 0x1ff mask, until create_vertex_elements began tripping
        // Fatal{ProtocolCorruption} permanently at kMGPipeMaxVertexElementsSlots.
        //
        // THE OBSERVABLE IS THE ALLOCATOR, not pixels: this leak produces correct pictures the
        // whole way to the fatal, which is exactly why the four cases above ran green over it.
        // It runs on BOTH backends' Handles lanes; DirectVulkan is where it was red.
        TEST_F(HandleRecycleScenario, DestroyedVertexArraysReturnTheirVertexElementsSlots) {
            if (!Ready()) return;
            SkipUnlessTheArmIsAssertableHere();
            if (IsSkipped()) return;
            if (m_arm != Arm::Handles) {
                GTEST_SKIP() << "the client mints a VertexElementsCso slot only when the vertex-input "
                                "subsystem is on, and only the Handles arm pins it (0x1ff). The Legacy "
                                "and AbaControl lanes run MOBILEGL_PIPE_PUSH=0, where there is no "
                                "allocator to leak from.";
            }

            unsigned probe = 0;
            if (!MGITest::PeekPipeSlotLiveCount(MGITest::PipeSlotKind::VertexElementsCso, &probe)) {
                GTEST_SKIP() << "the client slot allocator is out of reach from this module (a pull "
                                "build has none, and the Android link resolves no internal symbol), so "
                                "'could not look' would be reported as 'did not leak'";
            }

            // One shared VBO: the case is about the VAOs, and a per-round buffer would churn the
            // Buffer kind's slots alongside them and blur which allocator answered.
            const GLuint buffer = MakeQuadBuffer(0.0f, 1.0f, 0.0f);

            // One round: create a vertex array, DRAW with it - which is what mints the slot and
            // publishes the applier's record; a VAO that never reaches a validate point has
            // neither - unbind it and delete it. glGenVertexArrays hands the same name back every
            // time, exactly as a chunk renderer's does, so a death path that keyed on the GL NAME
            // rather than on the lifetime id would look correct here too, which is why the
            // assertion is on the allocator and not on the name.
            unsigned peakLive = 0;
            const auto round = [&](const char* when, bool checkPixels) {
                GLuint vao = 0;
                glGenVertexArrays(1, &vao);
                ConfigureQuadVao(vao, buffer);
                const Image image = DrawQuadAndRead(vao);
                if (checkPixels) {
                    // One picture check, so a green here cannot mean "the draws never happened
                    // and therefore nothing was ever minted".
                    ExpectWholeViewportIs(image, "green", when);
                }
                unsigned live = 0;
                if (MGITest::PeekPipeSlotLiveCount(MGITest::PipeSlotKind::VertexElementsCso, &live) &&
                    live > peakLive) {
                    peakLive = live;
                }
                glBindVertexArray(0);
                glDeleteVertexArrays(1, &vao);
            };

            // TWO WARM-UP ROUNDS BEFORE THE BASELINE IS TAKEN, so what is measured is growth WITH
            // the churn and not the one-off cost of drawing at all. The first draws in a process
            // mint slots that legitimately never come back inside this case - the DEFAULT vertex
            // array's above all, which this scenario's frames bind and which lives as long as the
            // context does - and the second round is what proves the steady state has been
            // reached, since a per-round leak would still be growing at that point. Sampling
            // before them would score a one-off as the churn's leak; sampling after makes the
            // assertion the exact one that matters: "N more create/destroy cycles cost ZERO more
            // slots", with no slack in it.
            round("the first warm-up draw", /*checkPixels=*/true);
            round("the second warm-up draw", /*checkPixels=*/false);
            peakLive = 0;

            unsigned liveBefore = 0;
            unsigned highWaterBefore = 0;
            ASSERT_TRUE(MGITest::PeekPipeSlotLiveCount(MGITest::PipeSlotKind::VertexElementsCso,
                                                       &liveBefore));
            ASSERT_TRUE(MGITest::PeekPipeSlotHighWater(MGITest::PipeSlotKind::VertexElementsCso,
                                                       &highWaterBefore));

            constexpr int kChurn = 48;
            for (int i = 0; i < kChurn; ++i) round("a churn draw", /*checkPixels=*/false);
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "the churn left a GL error behind";

            unsigned liveAfter = 0;
            unsigned highWaterAfter = 0;
            ASSERT_TRUE(MGITest::PeekPipeSlotLiveCount(MGITest::PipeSlotKind::VertexElementsCso,
                                                       &liveAfter));
            ASSERT_TRUE(MGITest::PeekPipeSlotHighWater(MGITest::PipeSlotKind::VertexElementsCso,
                                                       &highWaterAfter));
            std::cout << "[ HandleRecycle ] backend=" << Gl().BackendName() << " VertexElementsCso live "
                      << liveBefore << " -> " << liveAfter << " (peak " << peakLive << "), high water "
                      << highWaterBefore << " -> " << highWaterAfter << " over " << kChurn
                      << " create/draw/destroy rounds" << std::endl;

            EXPECT_EQ(liveAfter, liveBefore)
                << kChurn << " vertex arrays were created, drawn with and destroyed and " << (liveAfter - liveBefore)
                << " VertexElementsCso slots never came back. Each one holds a SlotState, a "
                   "lifetime-id map node and the applier's ~1.3 KB record for the life of the "
                   "process, and past kMGPipeMaxVertexElementsSlots every create_vertex_elements "
                   "trips Fatal{ProtocolCorruption} for good. Backend "
                << Gl().BackendName();
            EXPECT_EQ(highWaterAfter, highWaterBefore)
                << "the CSO slot space grew with the churn instead of recycling the one slot the "
                   "warm-up round already handed out; the frees are not reaching the allocator's "
                   "free list";
            EXPECT_LE(peakLive - liveBefore, 1u)
                << "more than one churned vertex array was live at the allocator at once, so the "
                   "deaths are arriving late rather than at the destructor";

            glDeleteBuffers(1, &buffer);
        }

    } // namespace
} // namespace MGITest
