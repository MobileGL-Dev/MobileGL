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
#include <functional>
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

        // P4a's version of the SAME question, and it is a THIRD question rather than a rewording
        // of either above. The P2 marker answers "is this backend's VERTEX-INPUT memo keyed on
        // {slot, gen}"; the P3a one answers it for the BUFFER. P4a re-keys six more object classes
        // - texture, renderbuffer, framebuffer, sampler CSO, sampler view and shader CSO - and
        // their four subsystem bits (kMGPipeSubsystem{Framebuffer,TextureResources,Samplers,
        // Programs}, MGPipe.h) are what a backend has to name to honour MOBILEGL_PIPE_PUSH's
        // default mask. A P4a case that read either older marker would report the Handles arm as
        // armed on a tree where nothing about a texture is keyed on a handle - green for a re-key
        // that does not exist, which is the one outcome this file exists to prevent. Set by
        // MG_IntegrationTest/CMakeLists.txt from a content probe over each backend's own
        // directory, exactly like its two predecessors.
        bool ThisBackendsObjectRekeyHasLanded() {
            const std::string& backend = HeadlessGL::Get().BackendName();
            if (backend == "DirectVulkan") {
                return BuildMarkerIsSet("MGITEST_HANDLE_REKEY_OBJECTS_DirectVulkan");
            }
            return BuildMarkerIsSet("MGITEST_HANDLE_REKEY_OBJECTS_DirectGLES");
        }

        // "Does MOBILEGL_PIPE_HANDLE_ABA_CONTROL steer THIS backend's P4a OBJECT keys?" - the
        // question that decides whether the AbaControl arm of a P4a case expects the corruption
        // or the correct pixels, and it is deliberately narrow.
        //
        // WHY IT IS NOT THE EXISTING MGITEST_HANDLE_ABA_IMPLEMENTED, and this is the P4a finding
        // the file records rather than works around. That marker says "some DirectVulkan source
        // reads Features.PipeHandleAbaControl", and today exactly one does:
        // MagmaPipeArms.h's MagmaPipeAbaControlDefeatsIdentity, whose consumers are Magma's
        // VERTEX-INPUT keys. Magma mints {slot, gen} for two kinds only - VertexElementsCso and
        // Buffer (MagmaPipeIdentityTables) - so there is no texture, framebuffer, sampler, view or
        // program key on that backend for the knob to defeat, and P4a does not add one: Magma's
        // object paths are P7 (BRIEF-P4A.md D-Q), and MG_Backend/DirectVulkan/** is untouched in
        // P4a apart from MagmaPipeArms.h's own statement of this fact. On DirectGLES the knob has
        // no consumer at all.
        //
        // So on this tree the six P4a cases below run their AbaControl arm with the knob INERT.
        // The honest report for that is the arm asserting the correct pixels and SAYING that it is
        // not controlling anything here - never a lane that expects a corruption nothing can
        // produce, which would be a hard red on an always-on integration-gpu lane, which is
        // exactly the failure this file's header records having had once. The moment a backend
        // grows a Features.PipeHandleAbaControl consumer over its P4a object slot tables (one `if`
        // in GetOrCreate / FindByHandle, the way MagmaPipeClaimSlotMemos is Magma's for vertex
        // input), the probe finds it and every one of the six flips to expecting the corruption.
        bool ObjectAbaControlIsWiredHere() {
            const std::string& backend = HeadlessGL::Get().BackendName();
            const bool knob = backend == "DirectVulkan"
                                  ? BuildMarkerIsSet("MGITEST_HANDLE_ABA_OBJECTS_DirectVulkan")
                                  : BuildMarkerIsSet("MGITEST_HANDLE_ABA_OBJECTS_DirectGLES");
            // Both halves, because either alone is a lie: a knob consumer with no object re-key
            // has nothing to defeat, and an object re-key with no knob consumer cannot be defeated.
            return knob && ThisBackendsObjectRekeyHasLanded();
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

            // P4a's version of the gate above, for the six object kinds. Only the Handles arm
            // needs it: `Legacy` asserts today's address/weak_ptr guards (which exist on every
            // tree) and `AbaControl` decides what it expects from ObjectAbaControlIsWiredHere().
            void SkipUnlessTheObjectHandlePathIsAssertableHere(const char* kindName) {
                if (m_arm != Arm::Handles) return;
                if (!ThisBackendsObjectRekeyHasLanded()) {
                    GTEST_SKIP() << "subsystem not implemented on this tree: the " << kindName
                                 << "'s Handles arm needs this backend to be keyed on {slot, gen} "
                                    "for P4a's object families, and the build's capability probe "
                                    "found no source under MobileGL/MG_Backend/"
                                 << Gl().BackendName()
                                 << " naming any of kMGPipeSubsystem{Framebuffer, TextureResources, "
                                    "Samplers, Programs}. Until they land, this object's backend "
                                    "twin is still reached from the frontend object, so there is no "
                                    "{slot, gen} key here to assert about (P4a packages D and E for "
                                    "DirectGLES; Magma's object paths are P7). The entry stays "
                                    "registered and visible, and arms itself when that package "
                                    "lands in a push build.";
                }
            }

            // Says, on EVERY arm and whether or not the case passes, whether the AbaControl arm is
            // controlling anything for this kind on this backend - the same reason ExpectPixelsFor
            // prints what it observed. A reader of a green AbaControl entry must not have to infer
            // which of the two it was.
            bool ObjectAbaExpectation(const char* kindName) {
                const bool wired = ObjectAbaControlIsWiredHere();
                if (m_arm == Arm::AbaControl) {
                    std::cout << "[ HandleRecycle ] aba_control kind=" << kindName
                              << " backend=" << Gl().BackendName() << " wired=" << (wired ? "1" : "0")
                              << (wired ? " (the knob defeats this kind's identity: the corruption IS "
                                          "the assertion)"
                                        : " (no Features.PipeHandleAbaControl consumer over this "
                                          "backend's P4a object slot tables, so the knob is inert "
                                          "here and this arm asserts the correct pixels - it is not "
                                          "a control for this kind yet)")
                              << std::endl;
                    RecordProperty("aba_control_wired", wired ? 1 : 0);
                }
                return wired;
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

            // ---- G8b: the leak shape, spelled once ------------------------------------------
            //
            // DestroyedVertexArraysReturnTheirVertexElementsSlots below is the original; P4a adds
            // one case per kind it mints, and seven copies of a twenty-line assertion block is how
            // six of them quietly stop asserting the same thing. So the block lives here and each
            // case supplies only its own churn round.
            //
            // `round(checkPixels, observe)` must create ONE object of the kind, put it through
            // whatever makes the client mint its slot (which for every P4a kind means reaching a
            // validate point - an object created and destroyed without a draw has no record and no
            // slot), call `observe()` WHILE THE OBJECT IS STILL ALIVE, and then destroy it.
            //
            // `observe()` is where peakLive is sampled, and it has to be inside the round rather
            // than after it: sampled after the destroy it would only ever see the resting count,
            // and the "deaths arrive at the destructor rather than late" assertion below would be
            // vacuous - which is the one of the three that catches a death path that works but
            // runs at the wrong time (a deferred queue, a frame-boundary sweep).
            //
            // `maxInFlight` is how many slots of the kind one round may legitimately hold at its
            // peak: 1 where the round creates one object, more where the round creates several of
            // the same kind (a pipeline composite's round also creates its two stage programs, and
            // all three are ShaderCsos).
            using ChurnRound = std::function<void(bool checkPixels, const std::function<void()>& observe)>;
            void AssertChurnReturnsEverySlot(PipeSlotKind kind, const char* kindName,
                                             const char* owner, unsigned maxInFlight,
                                             const ChurnRound& round) {
                if (m_arm != Arm::Handles) {
                    GTEST_SKIP() << "the client mints a " << kindName
                                 << " slot only when its subsystem is on, and only the Handles arm "
                                    "pins the shipping mask (0x1fff). The Legacy and AbaControl "
                                    "lanes run MOBILEGL_PIPE_PUSH=0, where there is no allocator to "
                                    "leak from.";
                }
                unsigned probe = 0;
                if (!MGITest::PeekPipeSlotLiveCount(kind, &probe)) {
                    GTEST_SKIP() << "the client slot allocator is out of reach from this module (a "
                                    "pull build has none, and the Android link resolves no internal "
                                    "symbol), so 'could not look' would be reported as 'did not "
                                    "leak'";
                }

                // TWO WARM-UP ROUNDS BEFORE THE BASELINE IS TAKEN, so what is measured is growth
                // WITH the churn and not the one-off cost of drawing at all. The first rounds in a
                // process mint slots that legitimately never come back inside this case - the
                // default vertex array's, the scene's own program's - and the second round is what
                // proves the steady state has been reached, since a per-round leak would still be
                // growing at that point.
                unsigned peakLive = 0;
                const std::function<void()> observe = [&]() {
                    unsigned live = 0;
                    if (MGITest::PeekPipeSlotLiveCount(kind, &live) && live > peakLive) peakLive = live;
                };
                round(/*checkPixels=*/true, observe);
                round(/*checkPixels=*/false, observe);
                peakLive = 0;

                unsigned liveBefore = 0;
                unsigned highWaterBefore = 0;
                ASSERT_TRUE(MGITest::PeekPipeSlotLiveCount(kind, &liveBefore));
                ASSERT_TRUE(MGITest::PeekPipeSlotHighWater(kind, &highWaterBefore));

                constexpr int kChurn = 48;
                for (int i = 0; i < kChurn; ++i) round(/*checkPixels=*/false, observe);
                ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "the churn left a GL error behind";

                unsigned liveAfter = 0;
                unsigned highWaterAfter = 0;
                ASSERT_TRUE(MGITest::PeekPipeSlotLiveCount(kind, &liveAfter));
                ASSERT_TRUE(MGITest::PeekPipeSlotHighWater(kind, &highWaterAfter));
                std::cout << "[ HandleRecycle ] backend=" << Gl().BackendName() << " " << kindName
                          << " live " << liveBefore << " -> " << liveAfter << " (peak " << peakLive
                          << "), high water " << highWaterBefore << " -> " << highWaterAfter
                          << " over " << kChurn << " create/draw/destroy rounds" << std::endl;

                // NOTHING WAS EVER MINTED, which is not "did not leak" and must not be reported as
                // one. On the P4a contract tree the client emits nothing for any of these kinds -
                // the five emit headers are the contract's stubs (contract-v1 D1) - so every
                // assertion below would be 0 == 0 and the case would be a green that asserts about
                // a kind it never saw. This is the same rule as the peek returning false, applied
                // to the other way of not being able to look, and it arms itself the moment the
                // owning package's emitter lands.
                if (highWaterAfter == 0 && peakLive == 0 && liveAfter == 0) {
                    GTEST_SKIP() << "subsystem not implemented on this tree: the client minted no "
                                 << kindName
                                 << " slot at all over " << (kChurn + 2)
                                 << " create/draw/destroy rounds, so there is nothing here that "
                                    "could leak and a green would assert nothing. P4a package "
                                 << owner
                                 << " owns the emitter that mints it; this case arms itself when it "
                                    "lands.";
                }

                EXPECT_EQ(liveAfter, liveBefore)
                    << kChurn << " " << kindName
                    << " objects were created, drawn with and destroyed and "
                    << (liveAfter - liveBefore)
                    << " slots never came back. Each one holds a SlotState, a lifetime-id map node "
                       "and the applier's record for the life of the process, and past the kind's "
                       "slot bound every create trips Fatal{ProtocolCorruption} for good "
                       "(PipeApply.h:97-113). This is P3a's C-1 defect, which is why every P4a kind "
                       "frees its slot from the frontend destructor through one client-side helper "
                       "(D-I1) rather than from a backend death table. Backend "
                    << Gl().BackendName();
                EXPECT_EQ(highWaterAfter, highWaterBefore)
                    << "the " << kindName
                    << " slot space grew with the churn instead of recycling the slot the warm-up "
                       "rounds already handed out; the frees are not reaching the allocator's free "
                       "list";
                EXPECT_LE(peakLive > liveBefore ? peakLive - liveBefore : 0u, maxInFlight)
                    << "more than " << maxInFlight << " churned " << kindName
                    << " object(s) were live at the allocator at once, so the deaths are arriving "
                       "late rather than at the destructor";
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
        //    P4a MADE THIS A REAL ABA CONTROL (G8, D-I2). Until P4a the case expected the correct
        //    pixels on EVERY arm, with the note that "the AbaControl knob does not steer this
        //    path" - true, and vacuous the moment P4a re-keys the texture twin on {slot, gen}: a
        //    control that only defeats guards nobody ships says nothing about the key that does.
        //    So the expectation is now ObjectAbaExpectation()'s answer - "expect the corruption"
        //    exactly where the knob really reaches this kind on this backend, "expect the correct
        //    pixels, and SAY that this arm is not a control here" where it does not. See
        //    ObjectAbaControlIsWiredHere for why the second is today's answer on both backends and
        //    for the one change that flips it.
        // ------------------------------------------------------------------------------------
        TEST_F(HandleRecycleScenario, ATextureAtARecycledAddressDoesNotInheritItsPredecessorsTwin) {
            if (!Ready()) return;
            SkipUnlessTheArmIsAssertableHere();
            if (IsSkipped()) return;
            SkipUnlessTheObjectHandlePathIsAssertableHere("texture");
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
            ExpectPixelsFor(m_arm, /*armExpectsCorruption=*/ObjectAbaExpectation("texture"), image,
                            "green", "red", "the draw after the texture was recycled");

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
        //
        //    P4a MADE THIS A REAL ABA CONTROL TOO (G8, D-I2), and it needed one more thing than
        //    the texture case did: somewhere for the corruption to be VISIBLE. "The replacement's
        //    attachment never became green" is only half a verdict - it does not say where the
        //    clear went. So the dead framebuffer's attachment is cleared to RED in the warm-up and
        //    read back beside the replacement's at the end: FRESH is (second green, first red),
        //    STALE is (first green) - the clear reached a framebuffer this one only shares an
        //    address with. A framebuffer is the kind with a handle and NO wire lifetime (D-I2):
        //    the applier learns of it only through set_framebuffer_state keyed by Fbo, and a
        //    recycled handle is told apart by Gen, which is inside ContentHash - so the knob has
        //    to defeat the identity half of that memo key for this to reproduce at all.
        // ------------------------------------------------------------------------------------
        TEST_F(HandleRecycleScenario, AFramebufferAtARecycledAddressDoesNotInheritItsPredecessorsTwin) {
            if (!Ready()) return;
            SkipUnlessTheArmIsAssertableHere();
            if (IsSkipped()) return;
            SkipUnlessTheObjectHandlePathIsAssertableHere("framebuffer");
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

            // Read BOTH attachments, not the framebuffer: that is what makes "the clear landed in
            // the dead framebuffer" visible as a place rather than as an absence.
            const auto readAttachment = [&](GLuint texture) {
                std::vector<std::uint8_t> texels(4 * 4 * 4, 0);
                glBindTexture(GL_TEXTURE_2D, texture);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels.data());
                glBindTexture(GL_TEXTURE_2D, 0);
                return texels;
            };
            const auto offendersAgainst = [](const std::vector<std::uint8_t>& texels, int r, int g,
                                             int b) {
                int offenders = 0;
                for (std::size_t i = 0; i < texels.size(); i += 4) {
                    if (texels[i] != r || texels[i + 1] != g || texels[i + 2] != b) ++offenders;
                }
                return offenders;
            };
            const std::vector<std::uint8_t> replacementTexels = readAttachment(secondAttachment);
            const std::vector<std::uint8_t> deadTexels = readAttachment(firstAttachment);
            EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));

            const int replacementIsNotGreen = offendersAgainst(replacementTexels, 0, 255, 0);
            const int deadIsNotRed = offendersAgainst(deadTexels, 255, 0, 0);
            const bool sawStale = deadIsNotRed != 0 && offendersAgainst(deadTexels, 0, 255, 0) == 0;
            std::cout << "[ HandleRecycle ] arm=" << ArmName(m_arm) << " framebuffer observed="
                      << (sawStale ? "STALE (the clear reached the DEAD framebuffer's attachment)"
                                   : (replacementIsNotGreen == 0
                                          ? "FRESH (the clear reached the replacement's own "
                                            "attachment)"
                                          : "NEITHER"))
                      << std::endl;

            if (ObjectAbaExpectation("framebuffer") && m_arm == Arm::AbaControl) {
                // The corruption IS the assertion: with the identity half of the framebuffer memo
                // key defeated, the replacement inherits the dead framebuffer's record and its
                // clear lands on the dead one's attachment.
                EXPECT_NE(deadIsNotRed, 0)
                    << "[AbaControl expects the STALE framebuffer] the DEAD framebuffer's "
                       "attachment is still the red it was cleared to in the warm-up, so the "
                       "replacement's clear went to its own attachment after all and the ABA was "
                       "not reproduced - the control has stopped controlling anything.";
            } else {
                EXPECT_EQ(replacementIsNotGreen, 0)
                    << "the replacement framebuffer's own attachment is not the colour it was "
                       "cleared to, so the clear reached a framebuffer this one only shares an "
                       "address with (first texel rgba="
                    << static_cast<int>(replacementTexels[0]) << ","
                    << static_cast<int>(replacementTexels[1]) << ","
                    << static_cast<int>(replacementTexels[2]) << ","
                    << static_cast<int>(replacementTexels[3]) << ")";
                EXPECT_EQ(deadIsNotRed, 0)
                    << "the DEAD framebuffer's attachment changed colour, so the replacement's "
                       "clear reached the framebuffer it only shares a name with";
            }

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

        // ====================================================================================
        // P4a (G8, G8b). Six more kinds, and the same two questions about each of them.
        //
        // WHAT IS NEW HERE, stated once so the cases below can stay short. P4a mints Texture,
        // Renderbuffer, Framebuffer, SamplerCso, SamplerViewCso and ShaderCso handles on the
        // client and frees every one of them from the frontend object's own destructor, through
        // one helper per kind, in one fixed order: emit the wire delete, raise the death notice,
        // free the slot (D-I1). That shape exists because of P3a's C-1 - a client-minted CSO whose
        // only free was a backend death table Magma does not install, which leaked a slot and a
        // ~1.3 KB record per VAO until the process Fatal'd - so the leak cases below run on the
        // DirectVulkan Handles lane as well as the DirectGLES one, exactly as ID-8 requires.
        //
        // Two kinds carry a wrinkle the others do not:
        //   * FRAMEBUFFER has a handle and NO wire lifetime (D-I2). There is no framebuffer row in
        //     the call catalogue at all: the applier learns of one only through
        //     set_framebuffer_state keyed by Fbo, and the death helper does the notice and the
        //     free and emits nothing. The allocator is therefore the ONLY observable of a
        //     framebuffer's lifetime, which is what the leak case reads.
        //   * SHADERCSO covers ordinary programs AND the program-pipeline COMPOSITES minted from
        //     the reserved high band (D-H7). A composite's slot has TWO independent release paths
        //     - the pipeline cache's LRU eviction and the composite ProgramObject's destructor -
        //     which is why it gets a case of its own; the second free is a proven no-op
        //     (SlotAllocator.cpp:117-119) and this is where "proven" is measured rather than
        //     asserted in a comment.
        // ====================================================================================

        // ------------------------------------------------------------------------------------
        // 4. The renderbuffer. Espryt keeps a backend twin per frontend renderbuffer in the same
        //    address-keyed registry the texture uses, and P4a re-keys it on {slot, gen}.
        //
        //    The replacement is deliberately a DIFFERENT SIZE, because a renderbuffer cannot be
        //    sampled and so has no colour of its own to inherit: what a stale twin gets wrong is
        //    the STORAGE, and the storage's extent is the one property a readback can see. 8x8
        //    green against a dead 4x4 red: fresh reads green everywhere, and a replacement that
        //    inherited the dead 4x4 driver renderbuffer either leaves the outer ring unwritten or
        //    makes the framebuffer incomplete - both of which this case reports as STALE.
        // ------------------------------------------------------------------------------------
        TEST_F(HandleRecycleScenario, ARenderbufferAtARecycledAddressDoesNotInheritItsPredecessorsStorage) {
            if (!Ready()) return;
            SkipUnlessTheArmIsAssertableHere();
            if (IsSkipped()) return;
            SkipUnlessTheObjectHandlePathIsAssertableHere("renderbuffer");
            if (IsSkipped()) return;

            GLuint fbo = 0;
            glGenFramebuffers(1, &fbo);

            GLuint deadRenderbuffer = 0;
            glGenRenderbuffers(1, &deadRenderbuffer);
            glBindRenderbuffer(GL_RENDERBUFFER, deadRenderbuffer);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 4, 4);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
                                      deadRenderbuffer);
            ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GLenum(GL_FRAMEBUFFER_COMPLETE));
            for (int frame = 0; frame < kWarmupFrames; ++frame) {
                glBindFramebuffer(GL_FRAMEBUFFER, fbo);
                glViewport(0, 0, 4, 4);
                ClearTo(1.0f, 0.0f, 0.0f, 1.0f);
                BindDefaultFramebuffer();
                Gl().EndFrame();
            }

            // Detach BEFORE deleting: an attached renderbuffer is kept alive by the frontend
            // FramebufferAttachmentObject's SharedPtr, so glDeleteRenderbuffers would only unname
            // it and the object would not die here at all.
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, 0);
            BindDefaultFramebuffer();
            glBindRenderbuffer(GL_RENDERBUFFER, 0);
            glDeleteRenderbuffers(1, &deadRenderbuffer);

            GLuint liveRenderbuffer = 0;
            glGenRenderbuffers(1, &liveRenderbuffer);
            if (liveRenderbuffer != deadRenderbuffer) {
                glDeleteRenderbuffers(1, &liveRenderbuffer);
                glDeleteFramebuffers(1, &fbo);
                GTEST_SKIP() << "inconclusive, not proven: glGenRenderbuffers returned "
                             << liveRenderbuffer << " rather than the deleted " << deadRenderbuffer
                             << ", so no ABA was constructed";
            }
            RecordProperty("recycled_renderbuffer_name", static_cast<int>(liveRenderbuffer));

            glBindRenderbuffer(GL_RENDERBUFFER, liveRenderbuffer);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 8, 8);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
                                      liveRenderbuffer);
            const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            glViewport(0, 0, 8, 8);
            ClearTo(0.0f, 1.0f, 0.0f, 1.0f);
            const Image image = ReadPixels(8, 8);
            BindDefaultFramebuffer();
            Gl().EndFrame();

            const bool complete = status == GLenum(GL_FRAMEBUFFER_COMPLETE);
            const bool allGreen = complete && static_cast<bool>(RegionIsMostly(image, 0, 8, 0, 8,
                                                                              "green", 0.0,
                                                                              "the replacement "
                                                                              "renderbuffer"));
            std::cout << "[ HandleRecycle ] arm=" << ArmName(m_arm)
                      << " renderbuffer observed=" << (allGreen ? "FRESH (8x8 all green)"
                                                                : "STALE (the replacement did not "
                                                                  "get its own 8x8 storage)")
                      << " fbo_status=0x" << std::hex << status << std::dec << std::endl;

            if (ObjectAbaExpectation("renderbuffer") && m_arm == Arm::AbaControl) {
                EXPECT_FALSE(allGreen)
                    << "[AbaControl expects the STALE renderbuffer] the replacement got its own 8x8 "
                       "storage with the identity half of the twin key deliberately defeated, so "
                       "the ABA was not reproduced and this control is controlling nothing.";
            } else {
                EXPECT_EQ(status, GLenum(GL_FRAMEBUFFER_COMPLETE))
                    << "the framebuffer went incomplete after the recycled renderbuffer was given "
                       "8x8 storage, which is what a driver object inherited from the dead 4x4 "
                       "renderbuffer looks like";
                EXPECT_TRUE(RegionIsMostly(image, 0, 8, 0, 8, "green", 0.0,
                                           "the replacement renderbuffer's own 8x8 storage"))
                    << "the replacement renderbuffer did not read back as its own storage, so the "
                       "clear reached a renderbuffer it only shares a name with";
            }

            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glBindRenderbuffer(GL_RENDERBUFFER, 0);
            glDeleteRenderbuffers(1, &liveRenderbuffer);
            glDeleteFramebuffers(1, &fbo);
        }

        // ------------------------------------------------------------------------------------
        // 5. The sampler object (SamplerCso). P4a content-addresses sampler CSOs at capacity 256
        //    and hashes them field-wise over a canonical zero-initialised copy (D-F1), so two
        //    sampler objects with identical parameters are ONE CSO by design - which is why the
        //    ABA here is built out of two samplers whose parameters DIFFER.
        //
        //    The observable is the BORDER COLOUR, sampled at a constant UV outside [0,1] with
        //    GL_CLAMP_TO_BORDER, so every fragment reads the border and the whole viewport is one
        //    colour. That is deliberate: the border colour is the sampler parameter with the
        //    fewest other paths to the driver (it is not in the texture's own parameter set the
        //    way a filter effectively is), and borderColorForm is the very field G7's second
        //    negative control drops.
        // ------------------------------------------------------------------------------------
        TEST_F(HandleRecycleScenario, ASamplerAtARecycledAddressDoesNotInheritItsPredecessorsParameters) {
            if (!Ready()) return;
            SkipUnlessTheArmIsAssertableHere();
            if (IsSkipped()) return;
            SkipUnlessTheObjectHandlePathIsAssertableHere("sampler object");
            if (IsSkipped()) return;

            const GLuint buffer = MakeQuadBuffer(1.0f, 1.0f, 1.0f);
            GLuint vao = 0;
            glGenVertexArrays(1, &vao);
            ConfigureQuadVao(vao, buffer);
            // The texture is BLUE and is never what the case reads: every fragment samples outside
            // [0,1], so what comes back is the sampler's border colour and nothing else.
            const GLuint texture = MakeSolidTexture(0, 0, 255);

            const auto makeBorderSampler = [](float r, float g, float b) {
                GLuint sampler = 0;
                glGenSamplers(1, &sampler);
                const float border[4] = {r, g, b, 1.0f};
                glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
                glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
                glSamplerParameterfv(sampler, GL_TEXTURE_BORDER_COLOR, border);
                return sampler;
            };
            // Sampled at a constant UV well outside [0,1]: one colour for the whole viewport.
            static const char* kBorderFS = R"(#version 330 core
uniform sampler2D uTex;
out vec4 oColor;
void main() { oColor = texture(uTex, vec2(4.0, 4.0)); }
)";
            std::string error;
            const GLuint borderProgram = CompileProgram(kSampleVS, kBorderFS, &error);
            ASSERT_NE(borderProgram, 0u) << error;

            const auto drawThroughSampler = [&](GLuint sampler) {
                BindDefaultFramebuffer();
                ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
                glUseProgram(borderProgram);
                glUniform1i(glGetUniformLocation(borderProgram, "uTex"), 0);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, texture);
                glBindSampler(0, sampler);
                glBindVertexArray(vao);
                glDrawArrays(GL_TRIANGLES, 0, kVertexCount);
                const Image image = ReadPixels(Gl().Width(), Gl().Height());
                Gl().EndFrame();
                return image;
            };

            const GLuint redSampler = makeBorderSampler(1.0f, 0.0f, 0.0f);
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "building the first sampler left a GL error";
            for (int frame = 0; frame < kWarmupFrames; ++frame) {
                const Image warm = drawThroughSampler(redSampler);
                ExpectWholeViewportIs(warm, "red", "sampler warm-up frame " + std::to_string(frame));
            }

            glBindSampler(0, 0);
            GLuint doomed = redSampler;
            glDeleteSamplers(1, &doomed);

            const GLuint greenSampler = makeBorderSampler(0.0f, 1.0f, 0.0f);
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "building the replacement sampler left a GL error";
            if (greenSampler != redSampler) {
                glDeleteSamplers(1, &greenSampler);
                glDeleteTextures(1, const_cast<GLuint*>(&texture));
                glBindVertexArray(0);
                glDeleteVertexArrays(1, &vao);
                glDeleteBuffers(1, const_cast<GLuint*>(&buffer));
                glDeleteProgram(borderProgram);
                GTEST_SKIP() << "inconclusive, not proven: glGenSamplers returned " << greenSampler
                             << " rather than the deleted " << redSampler
                             << ", so no ABA was constructed";
            }
            RecordProperty("recycled_sampler_name", static_cast<int>(greenSampler));

            const Image image = drawThroughSampler(greenSampler);
            ExpectPixelsFor(m_arm, /*armExpectsCorruption=*/ObjectAbaExpectation("sampler object"),
                            image, "green", "red",
                            "the draw after the sampler object was recycled");

            glBindSampler(0, 0);
            glBindTexture(GL_TEXTURE_2D, 0);
            GLuint cleanupSampler = greenSampler;
            glDeleteSamplers(1, &cleanupSampler);
            GLuint cleanupTexture = texture;
            glDeleteTextures(1, &cleanupTexture);
            glBindVertexArray(0);
            glDeleteVertexArrays(1, &vao);
            GLuint cleanupBuffer = buffer;
            glDeleteBuffers(1, &cleanupBuffer);
            glDeleteProgram(borderProgram);
        }

        // ------------------------------------------------------------------------------------
        // 6. The sampler VIEW (SamplerViewCso), which is a different question from either of the
        //    two above and is the reason it gets a case rather than a comment.
        //
        //    A sampler view is the RESOLVED (texture, sampler) pair - what the unit actually
        //    samples - and P4a addresses it by IDENTITY per texture object (D-F2, the deviation
        //    from ARCHITECTURE.md:63's content-addressed 4096-entry cache, which is P7's). So the
        //    view's key names the texture; the case recycles the TEXTURE while an explicitly bound
        //    SAMPLER OBJECT stays alive and unchanged across the window, so what a stale view
        //    would inherit is the dead texture through a live sampler - which the texture case
        //    above cannot construct, because there the unit has no sampler object bound at all and
        //    the built-in sampler travels with the texture.
        // ------------------------------------------------------------------------------------
        TEST_F(HandleRecycleScenario, ATextureAtARecycledAddressDoesNotInheritItsPredecessorsSamplerView) {
            if (!Ready()) return;
            SkipUnlessTheArmIsAssertableHere();
            if (IsSkipped()) return;
            SkipUnlessTheObjectHandlePathIsAssertableHere("sampler view");
            if (IsSkipped()) return;

            const GLuint buffer = MakeQuadBuffer(1.0f, 1.0f, 1.0f);
            GLuint vao = 0;
            glGenVertexArrays(1, &vao);
            ConfigureQuadVao(vao, buffer);

            // One sampler object for the whole case: it is the half of the view's identity that
            // must NOT move, so that a stale view can only come from the texture half.
            GLuint sampler = 0;
            glGenSamplers(1, &sampler);
            glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

            const auto drawSampled = [&](GLuint texture) {
                BindDefaultFramebuffer();
                ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
                glUseProgram(m_sampleProgram);
                glUniform1i(glGetUniformLocation(m_sampleProgram, "uTex"), 0);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, texture);
                glBindSampler(0, sampler);
                glBindVertexArray(vao);
                glDrawArrays(GL_TRIANGLES, 0, kVertexCount);
                const Image image = ReadPixels(Gl().Width(), Gl().Height());
                Gl().EndFrame();
                return image;
            };

            const GLuint redTexture = MakeSolidTexture(255, 0, 0);
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "building the first texture left a GL error";
            for (int frame = 0; frame < kWarmupFrames; ++frame) {
                const Image warm = drawSampled(redTexture);
                ExpectWholeViewportIs(warm, "red", "sampler-view warm-up frame " + std::to_string(frame));
            }

            glBindSampler(0, 0);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, 0);
            GLuint doomed = redTexture;
            glDeleteTextures(1, &doomed);

            const GLuint greenTexture = MakeSolidTexture(0, 255, 0);
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "building the replacement texture left a GL error";
            if (greenTexture != redTexture) {
                GLuint cleanup = greenTexture;
                glDeleteTextures(1, &cleanup);
                glDeleteSamplers(1, &sampler);
                glBindVertexArray(0);
                glDeleteVertexArrays(1, &vao);
                glDeleteBuffers(1, const_cast<GLuint*>(&buffer));
                GTEST_SKIP() << "inconclusive, not proven: glGenTextures returned " << greenTexture
                             << " rather than the deleted " << redTexture
                             << ", so no ABA was constructed for the sampler view";
            }
            RecordProperty("recycled_sampler_view_texture_name", static_cast<int>(greenTexture));

            const Image image = drawSampled(greenTexture);
            ExpectPixelsFor(m_arm, /*armExpectsCorruption=*/ObjectAbaExpectation("sampler view"),
                            image, "green", "red",
                            "the draw after the sampler view's texture was recycled under a live "
                            "sampler object");

            glBindSampler(0, 0);
            glBindTexture(GL_TEXTURE_2D, 0);
            GLuint cleanupTexture = greenTexture;
            glDeleteTextures(1, &cleanupTexture);
            glDeleteSamplers(1, &sampler);
            glBindVertexArray(0);
            glDeleteVertexArrays(1, &vao);
            GLuint cleanupBuffer = buffer;
            glDeleteBuffers(1, &cleanupBuffer);
        }

        // ------------------------------------------------------------------------------------
        // 7. The program (ShaderCso). Espryt reaches a program's backend twin through
        //    g_programTwinLookupMemo (DirectGLES.cpp:134-135), which is keyed on the frontend
        //    ProgramObject; P4a re-keys it on the shader CSO's {slot, gen} and gives the program a
        //    create_shader_state / delete_shader_state lifetime of its own.
        //
        //    A program's colour is BAKED INTO ITS SOURCE here rather than passed as a uniform, so
        //    "the draw used the dead program" is the only thing the pixels can mean: a uniform
        //    would be re-set on the replacement and would hide exactly the inheritance the case is
        //    about.
        // ------------------------------------------------------------------------------------
        TEST_F(HandleRecycleScenario, AProgramAtARecycledAddressDoesNotInheritItsPredecessorsShaderCso) {
            if (!Ready()) return;
            SkipUnlessTheArmIsAssertableHere();
            if (IsSkipped()) return;
            SkipUnlessTheObjectHandlePathIsAssertableHere("program");
            if (IsSkipped()) return;

            static const char* kRedFS = R"(#version 330 core
out vec4 oColor;
void main() { oColor = vec4(1.0, 0.0, 0.0, 1.0); }
)";
            static const char* kGreenFS = R"(#version 330 core
out vec4 oColor;
void main() { oColor = vec4(0.0, 1.0, 0.0, 1.0); }
)";
            const GLuint buffer = MakeQuadBuffer(1.0f, 1.0f, 1.0f);
            GLuint vao = 0;
            glGenVertexArrays(1, &vao);
            ConfigureQuadVao(vao, buffer);

            const auto drawWith = [&](GLuint program) {
                BindDefaultFramebuffer();
                ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
                glUseProgram(program);
                glBindVertexArray(vao);
                glDrawArrays(GL_TRIANGLES, 0, kVertexCount);
                const Image image = ReadPixels(Gl().Width(), Gl().Height());
                Gl().EndFrame();
                return image;
            };

            std::string error;
            const GLuint redProgram = CompileProgram(kColorVS, kRedFS, &error);
            ASSERT_NE(redProgram, 0u) << error;
            for (int frame = 0; frame < kWarmupFrames; ++frame) {
                const Image warm = drawWith(redProgram);
                ExpectWholeViewportIs(warm, "red", "program warm-up frame " + std::to_string(frame));
            }

            // Unbind first: a program that is still current is kept alive by the frontend, so
            // glDeleteProgram would only flag it and the object would not die here.
            glUseProgram(0);
            GLuint doomed = redProgram;
            glDeleteProgram(doomed);

            const GLuint greenProgram = CompileProgram(kColorVS, kGreenFS, &error);
            ASSERT_NE(greenProgram, 0u) << error;
            if (greenProgram != redProgram) {
                glDeleteProgram(greenProgram);
                glBindVertexArray(0);
                glDeleteVertexArrays(1, &vao);
                glDeleteBuffers(1, const_cast<GLuint*>(&buffer));
                GTEST_SKIP() << "inconclusive, not proven: glCreateProgram returned " << greenProgram
                             << " rather than the deleted " << redProgram
                             << ", so no ABA was constructed";
            }
            RecordProperty("recycled_program_name", static_cast<int>(greenProgram));

            const Image image = drawWith(greenProgram);
            ExpectPixelsFor(m_arm, /*armExpectsCorruption=*/ObjectAbaExpectation("program"), image,
                            "green", "red", "the draw after the program was recycled");

            glUseProgram(0);
            glDeleteProgram(greenProgram);
            glBindVertexArray(0);
            glDeleteVertexArrays(1, &vao);
            GLuint cleanupBuffer = buffer;
            glDeleteBuffers(1, &cleanupBuffer);
        }

        // ====================================================================================
        // G8b: one leak case per kind P4a mints. See AssertChurnReturnsEverySlot for the shape and
        // for why a kind that was never minted SKIPS rather than passing.
        // ====================================================================================

        TEST_F(HandleRecycleScenario, DestroyedTexturesReturnTheirTextureSlots) {
            if (!Ready()) return;
            SkipUnlessTheArmIsAssertableHere();
            if (IsSkipped()) return;

            const GLuint buffer = MakeQuadBuffer(1.0f, 1.0f, 1.0f);
            GLuint vao = 0;
            glGenVertexArrays(1, &vao);
            ConfigureQuadVao(vao, buffer);

            AssertChurnReturnsEverySlot(
                PipeSlotKind::Texture, "Texture", "B (clientfb)", /*maxInFlight=*/1u,
                [&](bool checkPixels, const std::function<void()>& observe) {
                    const GLuint texture = MakeSolidTexture(0, 255, 0);
                    const Image image = DrawTexturedQuadAndRead(vao, texture);
                    if (checkPixels) {
                        // One picture check, so a green here cannot mean "the draws never happened
                        // and therefore nothing was ever minted".
                        ExpectWholeViewportIs(image, "green", "the first churned texture's draw");
                    }
                    observe();
                    glActiveTexture(GL_TEXTURE0);
                    glBindTexture(GL_TEXTURE_2D, 0);
                    GLuint doomed = texture;
                    glDeleteTextures(1, &doomed);
                });

            glBindVertexArray(0);
            glDeleteVertexArrays(1, &vao);
            GLuint cleanupBuffer = buffer;
            glDeleteBuffers(1, &cleanupBuffer);
        }

        TEST_F(HandleRecycleScenario, DestroyedTexturesReturnTheirSamplerViewSlots) {
            if (!Ready()) return;
            SkipUnlessTheArmIsAssertableHere();
            if (IsSkipped()) return;

            const GLuint buffer = MakeQuadBuffer(1.0f, 1.0f, 1.0f);
            GLuint vao = 0;
            glGenVertexArrays(1, &vao);
            ConfigureQuadVao(vao, buffer);

            // A sampler view is identity-addressed PER TEXTURE OBJECT (D-F2), so the churn that
            // exercises it is a texture churn - and the view's slot is released by the TEXTURE's
            // death helper (D-I1: the texture helper emits ResourceDestroy, DeleteSamplerView and
            // DeleteSamplerState), which is precisely why it needs a case of its own: a helper
            // that forgot one of its three frees leaks only that kind and nothing else moves.
            AssertChurnReturnsEverySlot(
                PipeSlotKind::SamplerViewCso, "SamplerViewCso", "C (clientsp)", /*maxInFlight=*/1u,
                [&](bool checkPixels, const std::function<void()>& observe) {
                    const GLuint texture = MakeSolidTexture(0, 255, 0);
                    const Image image = DrawTexturedQuadAndRead(vao, texture);
                    if (checkPixels) {
                        ExpectWholeViewportIs(image, "green", "the first churned sampler view's draw");
                    }
                    observe();
                    glActiveTexture(GL_TEXTURE0);
                    glBindTexture(GL_TEXTURE_2D, 0);
                    GLuint doomed = texture;
                    glDeleteTextures(1, &doomed);
                });

            glBindVertexArray(0);
            glDeleteVertexArrays(1, &vao);
            GLuint cleanupBuffer = buffer;
            glDeleteBuffers(1, &cleanupBuffer);
        }

        TEST_F(HandleRecycleScenario, DestroyedRenderbuffersReturnTheirRenderbufferSlots) {
            if (!Ready()) return;
            SkipUnlessTheArmIsAssertableHere();
            if (IsSkipped()) return;

            GLuint fbo = 0;
            glGenFramebuffers(1, &fbo);

            AssertChurnReturnsEverySlot(
                PipeSlotKind::Renderbuffer, "Renderbuffer", "B (clientfb)", /*maxInFlight=*/1u,
                [&](bool checkPixels, const std::function<void()>& observe) {
                    GLuint renderbuffer = 0;
                    glGenRenderbuffers(1, &renderbuffer);
                    glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
                    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 4, 4);
                    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
                    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
                                              renderbuffer);
                    glViewport(0, 0, 4, 4);
                    ClearTo(0.0f, 1.0f, 0.0f, 1.0f);
                    if (checkPixels) {
                        const Image image = ReadPixels(4, 4);
                        ExpectWholeViewportIs(image, "green", "the first churned renderbuffer's clear");
                    }
                    observe();
                    // Detach before deleting: an attached renderbuffer is kept alive by the
                    // frontend attachment's SharedPtr and would not die inside the round.
                    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, 0);
                    BindDefaultFramebuffer();
                    Gl().EndFrame();
                    glBindRenderbuffer(GL_RENDERBUFFER, 0);
                    glDeleteRenderbuffers(1, &renderbuffer);
                });

            glDeleteFramebuffers(1, &fbo);
        }

        TEST_F(HandleRecycleScenario, DestroyedFramebuffersReturnTheirFramebufferSlots) {
            if (!Ready()) return;
            SkipUnlessTheArmIsAssertableHere();
            if (IsSkipped()) return;

            // One attachment for the whole case: only the framebuffer churns, so a texture slot
            // that failed to come back cannot be scored against this kind.
            GLuint attachment = 0;
            glGenTextures(1, &attachment);
            glBindTexture(GL_TEXTURE_2D, attachment);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glBindTexture(GL_TEXTURE_2D, 0);

            // A FRAMEBUFFER HAS NO WIRE LIFETIME AT ALL (D-I2): no create_*, no destroy row, and a
            // death helper that raises the notice and frees the slot and emits nothing. The
            // allocator is therefore the only observable this kind has, which makes this case the
            // whole of its lifetime coverage rather than a supplement to a wire assertion.
            AssertChurnReturnsEverySlot(
                PipeSlotKind::Framebuffer, "Framebuffer", "B (clientfb)", /*maxInFlight=*/1u,
                [&](bool checkPixels, const std::function<void()>& observe) {
                    GLuint fbo = 0;
                    glGenFramebuffers(1, &fbo);
                    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
                    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                           attachment, 0);
                    glViewport(0, 0, 4, 4);
                    ClearTo(0.0f, 1.0f, 0.0f, 1.0f);
                    if (checkPixels) {
                        const Image image = ReadPixels(4, 4);
                        ExpectWholeViewportIs(image, "green", "the first churned framebuffer's clear");
                    }
                    observe();
                    BindDefaultFramebuffer();
                    Gl().EndFrame();
                    glDeleteFramebuffers(1, &fbo);
                });

            glDeleteTextures(1, &attachment);
        }

        TEST_F(HandleRecycleScenario, DestroyedSamplersReturnTheirSamplerCsoSlots) {
            if (!Ready()) return;
            SkipUnlessTheArmIsAssertableHere();
            if (IsSkipped()) return;

            const GLuint buffer = MakeQuadBuffer(1.0f, 1.0f, 1.0f);
            GLuint vao = 0;
            glGenVertexArrays(1, &vao);
            ConfigureQuadVao(vao, buffer);
            const GLuint texture = MakeSolidTexture(0, 255, 0);

            // EVERY ROUND'S SAMPLER CARRIES A DIFFERENT PARAMETER SET, and that is not decoration:
            // sampler CSOs are CONTENT-addressed at capacity 256 (D-F1), so 48 identical samplers
            // would legitimately be ONE CSO and the case would assert nothing about the death
            // path. The LOD bias moves per round, which changes the hash and nothing else.
            int round = 0;
            AssertChurnReturnsEverySlot(
                PipeSlotKind::SamplerCso, "SamplerCso", "C (clientsp)", /*maxInFlight=*/1u,
                [&](bool checkPixels, const std::function<void()>& observe) {
                    GLuint sampler = 0;
                    glGenSamplers(1, &sampler);
                    glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                    glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                    glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    glSamplerParameterf(sampler, GL_TEXTURE_MIN_LOD,
                                        -static_cast<float>(round % 16));
                    ++round;

                    BindDefaultFramebuffer();
                    ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
                    glUseProgram(m_sampleProgram);
                    glUniform1i(glGetUniformLocation(m_sampleProgram, "uTex"), 0);
                    glActiveTexture(GL_TEXTURE0);
                    glBindTexture(GL_TEXTURE_2D, texture);
                    glBindSampler(0, sampler);
                    glBindVertexArray(vao);
                    glDrawArrays(GL_TRIANGLES, 0, kVertexCount);
                    const Image image = ReadPixels(Gl().Width(), Gl().Height());
                    Gl().EndFrame();
                    if (checkPixels) {
                        ExpectWholeViewportIs(image, "green", "the first churned sampler's draw");
                    }
                    observe();
                    glBindSampler(0, 0);
                    glDeleteSamplers(1, &sampler);
                });

            glBindTexture(GL_TEXTURE_2D, 0);
            GLuint cleanupTexture = texture;
            glDeleteTextures(1, &cleanupTexture);
            glBindVertexArray(0);
            glDeleteVertexArrays(1, &vao);
            GLuint cleanupBuffer = buffer;
            glDeleteBuffers(1, &cleanupBuffer);
        }

        TEST_F(HandleRecycleScenario, DestroyedProgramsReturnTheirShaderCsoSlots) {
            if (!Ready()) return;
            SkipUnlessTheArmIsAssertableHere();
            if (IsSkipped()) return;

            const GLuint buffer = MakeQuadBuffer(1.0f, 1.0f, 1.0f);
            GLuint vao = 0;
            glGenVertexArrays(1, &vao);
            ConfigureQuadVao(vao, buffer);

            // A DIFFERENT SOURCE PER ROUND, for D-F1's reason applied to programs: a shader CSO is
            // keyed on (ShaderCso, Version) and a program archive is content-addressed, so 48
            // identical programs could legitimately be one CSO. The constant in the fragment
            // shader moves per round.
            int round = 0;
            AssertChurnReturnsEverySlot(
                PipeSlotKind::ShaderCso, "ShaderCso", "C (clientsp)", /*maxInFlight=*/1u,
                [&](bool checkPixels, const std::function<void()>& observe) {
                    const std::string fs = "#version 330 core\nout vec4 oColor;\nvoid main() { "
                                           "oColor = vec4(0.0, 1.0, 0.0, 1.0) + vec4(" +
                                           std::to_string(round) + ".0 * 0.0); }\n";
                    ++round;
                    std::string error;
                    const GLuint program = CompileProgram(kColorVS, fs.c_str(), &error);
                    ASSERT_NE(program, 0u) << error;
                    BindDefaultFramebuffer();
                    ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
                    glUseProgram(program);
                    glBindVertexArray(vao);
                    glDrawArrays(GL_TRIANGLES, 0, kVertexCount);
                    const Image image = ReadPixels(Gl().Width(), Gl().Height());
                    Gl().EndFrame();
                    if (checkPixels) {
                        ExpectWholeViewportIs(image, "green", "the first churned program's draw");
                    }
                    observe();
                    glUseProgram(0);
                    glDeleteProgram(program);
                });

            glBindVertexArray(0);
            glDeleteVertexArrays(1, &vao);
            GLuint cleanupBuffer = buffer;
            glDeleteBuffers(1, &cleanupBuffer);
        }

        // ------------------------------------------------------------------------------------
        // The COMPOSITE's own leak case (G8b, D-H7). It is a separate case and not a second phase
        // of the one above for the reason the band exists at all: a composite's ShaderCso slot
        // comes out of the reserved high band (kMGPipeShaderCsoCompositeSlotBase = 983040) through
        // the allocator's ONE door into it, AllocateComposite, and it is released by TWO
        // independent paths - ProgramPipelineObject::GetCachedDrawProgram's LRU eviction and the
        // composite ProgramObject's own destructor. The second free is a no-op only because Free
        // refuses a slot that is not live at that generation (SlotAllocator.cpp:117-119); if it
        // ever stopped being one, this is where a double free or a never-freed band slot shows up,
        // and nowhere else - the band is sparse against the ordinary program space by design, so
        // an ordinary program's leak case cannot see it.
        // ------------------------------------------------------------------------------------
        TEST_F(HandleRecycleScenario, EvictedPipelineCompositesReturnTheirShaderCsoSlots) {
            if (!Ready()) return;
            SkipUnlessTheArmIsAssertableHere();
            if (IsSkipped()) return;

            const GLuint buffer = MakeQuadBuffer(1.0f, 1.0f, 1.0f);
            GLuint vao = 0;
            glGenVertexArrays(1, &vao);
            ConfigureQuadVao(vao, buffer);

            static const char* kSeparableVS = R"(#version 410 core
in vec2 aPos;
in vec3 aColor;
out gl_PerVertex { vec4 gl_Position; };
out vec3 vColor;
void main() {
    vColor = aColor;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";
            int round = 0;
            AssertChurnReturnsEverySlot(
                PipeSlotKind::ShaderCso, "ShaderCso (pipeline composites)", "C (clientsp)",
                // TWO in flight: a round creates two stage programs and the composite the pipeline
                // flattens them into, and the two stage programs are ordinary ShaderCsos of the
                // same kind. The composite is the third, and it is the one whose two release paths
                // this case is about.
                /*maxInFlight=*/3u,
                [&](bool checkPixels, const std::function<void()>& observe) {
                    // A DIFFERENT FRAGMENT STAGE PER ROUND: the composite is keyed on
                    // ProgramPipelineObject::ComputeDrawProgramSignature(), the per-stage
                    // {lifetimeId, GetLinkVersion()} array, so a round that rebuilt an identical
                    // pipeline out of the same two programs would legitimately reuse one composite
                    // and the case would assert nothing about the release paths.
                    const std::string fsSource =
                        "#version 410 core\nin vec3 vColor;\nout vec4 oColor;\nvoid main() { oColor "
                        "= vec4(vColor, 1.0) + vec4(" + std::to_string(round) + ".0 * 0.0); }\n";
                    ++round;
                    const char* vsSource = kSeparableVS;
                    const char* fsSourcePtr = fsSource.c_str();
                    const GLuint vs = glCreateShaderProgramv(GL_VERTEX_SHADER, 1, &vsSource);
                    const GLuint fs = glCreateShaderProgramv(GL_FRAGMENT_SHADER, 1, &fsSourcePtr);
                    GLuint pipeline = 0;
                    glGenProgramPipelines(1, &pipeline);
                    glUseProgramStages(pipeline, GL_VERTEX_SHADER_BIT, vs);
                    glUseProgramStages(pipeline, GL_FRAGMENT_SHADER_BIT, fs);

                    BindDefaultFramebuffer();
                    ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
                    glUseProgram(0);
                    glBindProgramPipeline(pipeline);
                    glBindVertexArray(vao);
                    glDrawArrays(GL_TRIANGLES, 0, kVertexCount);
                    const Image image = ReadPixels(Gl().Width(), Gl().Height());
                    Gl().EndFrame();
                    if (checkPixels) {
                        // The draw has to actually happen: GetProgramForDraw() flattens the
                        // pipeline into a composite at the validate point and nowhere else, so a
                        // round whose draw was dropped mints no composite and the case would be
                        // measuring an empty churn.
                        ExpectWholeViewportIs(image, "white", "the first churned composite's draw");
                    }
                    observe();
                    glBindProgramPipeline(0);
                    glDeleteProgramPipelines(1, &pipeline);
                    glDeleteProgram(vs);
                    glDeleteProgram(fs);
                });

            glBindVertexArray(0);
            glDeleteVertexArrays(1, &vao);
            GLuint cleanupBuffer = buffer;
            glDeleteBuffers(1, &cleanupBuffer);
        }

    } // namespace
} // namespace MGITest
