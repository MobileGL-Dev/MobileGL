// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/PersistentCoherentMapScenario.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - A PERSISTENT|WRITE|COHERENT MAPPING, WRITTEN THROUGH WITH NO GL CALL ANNOUNCING IT.
//
// Target C of P5's reduced path, spelled out in full at docs/Disaggregated/ARCHITECTURE.md:500:
// map PERSISTENT|WRITE|COHERENT, write through the pointer, MAKE NO OTHER GL CALL, draw, read
// back and check.
//
// THE "NO OTHER GL CALL" IS THE ENTIRE POINT and it is exit gate E3(b). Under the split shape the
// server has no access to the client's address space, so a coherent mapping that the application
// writes into is bytes nobody told anyone about: the client tracker has to push the dirty blocks
// at EVERY VALIDATE POINT, on its own initiative, because no glBufferSubData, no
// glFlushMappedBufferRange and no unmap will ever come. A scenario that slipped in one announcing
// call - even a glGetError between the write and the draw - would let a "push on the next explicit
// buffer operation" implementation pass, which is the implementation this gate exists to reject.
// So the write/draw pairs below are exactly `std::memcpy(...)` followed by `glDrawArrays(...)`
// with nothing in between, and the sequence is map, write, draw, WRITE AGAIN, draw again, read
// back: the second write is the one that cannot have been carried by anything the first one did.
//
// WHICH ARM IS THIS RUNNING ON. A scenario-sized buffer is far below the 16 MiB
// kLargeBufferAdoptBytes, so BufferObject::TryAdoptLargeStorage never fires here - but
// AcquireMemoryRange has an adoption of its OWN that fires for any PERSISTENT|WRITE map that is
// not FLUSH_EXPLICIT (BufferObject.cpp:645-661). So the same test lands in the ADOPTED arm (the
// resource owner minted host-visible coherent storage, the CPU shadow was released, the
// application writes straight into GPU-visible memory) or in the EMULATED arm (the owner
// declined, the shadow is the truth, and the client has to push blocks) depending on driver and
// build - and those are completely different code paths. MOBILEGL_DISABLE_LARGE_BUFFER_ADOPTION
// does NOT separate them: it guards the 16 MiB path this buffer never reaches.
//
// So the lane DECLARES the arm (MGITEST_PERSISTENT_MAP_ARM, Harness/SplitLane.h) and
// TheMapLandsInTheArmItsLaneDeclares asserts it landed there, reading the library's own summary
// line. R-6 pins the split arm at T2 = emulated. The observable that separates them is
// `pmap` (persistent-map-push bytes, PipeStats ByteClass::PersistentMapPush): a push happens if
// and only if the acquisition was DECLINED, so pmap > 0 IS the black-box spelling of exit gate
// E3(d), "MGPipeApplyMapPersistent never returns a non-null pointer under split". It is the only
// spelling available from here - an adopted inproc store still renders correctly, because the
// address really is valid in this process, which is precisely why E3(d) is worth gating at all -
// and a direct count of declines would need a counter from packages b1/v1.
//
// `mpr` (map-persistent-roundtrips) is counted per ACQUISITION ATTEMPT, mint or decline
// (ARCHITECTURE.md:492), so it is the SAME NUMBER on both arms and in both transports: that is
// what makes exit gate E3(c)'s "mpr equal to the monolith arm's" checkable by one process. Both
// lanes assert the same constant against the same workload, so the equality holds by
// construction and each lane can fail on its own.

#include <array>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../Harness/HeadlessGL.h"
#include "../Harness/PipeStatsWindow.h"
#include "../Harness/ScenarioFixture.h"
#include "../Harness/SplitLane.h"

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
in vec2 aPos;
in vec3 aColor;
out vec3 vColor;
void main() {
    vColor = aColor;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

        constexpr const char* kFragmentSource = R"(#version 330 core
in vec3 vColor;
out vec4 oColor;
void main() { oColor = vec4(vColor, 1.0); }
)";

        struct Vertex {
            float x, y;
            float r, g, b;
        };

        // A full-viewport quad as two triangles, flat-coloured: one region readback then speaks
        // for the whole draw, and an offender pixel is a real disagreement rather than an
        // interpolation difference between two drivers.
        std::array<Vertex, 6> Quad(float r, float g, float b) {
            return {{
                {-1.f, -1.f, r, g, b},
                {1.f, -1.f, r, g, b},
                {1.f, 1.f, r, g, b},
                {-1.f, -1.f, r, g, b},
                {1.f, 1.f, r, g, b},
                {-1.f, 1.f, r, g, b},
            }};
        }

        constexpr GLsizeiptr kQuadBytes = GLsizeiptr(sizeof(Vertex) * 6);
        constexpr GLbitfield kCoherentFlags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;

        // The ctest entry that reads the summary line sets this and nothing else does; the case
        // skips everywhere else rather than racing for the lane's log (PipeStatsWindow.h).
        constexpr const char* kCounterLaneMarker = "MGITEST_PMAP_LANE";

        // Draws issued against the mapping inside the counted window. One mapping, many draws:
        // "one per acquisition" (1) and "one per draw" (kDrawsInTheWindow) have to be different
        // numbers or the assertion cannot tell them apart.
        constexpr int kDrawsInTheWindow = 4;

        // Acquisitions the counted window performs: exactly one glMapBufferRange of exactly one
        // freshly defined immutable store.
        constexpr long long kExpectedRoundtrips = 1;

        bool BuildMarkerIsSet(const char* name) {
            const char* value = std::getenv(name);
            return value != nullptr && value[0] == '1' && value[1] == '\0';
        }

        // A store defined with the three bits and mapped once, or a reason the driver refused.
        // Returns false WITHOUT touching gtest state, so it can be called from a helper: a
        // GTEST_SKIP() inside a value-returning function does not compile (the macro expands to
        // a bare `return`), and every earlier attempt to hide one in a factory grew a second
        // "did it skip" channel that drifted from the first.
        struct CoherentStore {
            unsigned int vbo = 0;
            void* map = nullptr;
            std::string refusal;
        };

        CoherentStore MakeCoherentlyMappedQuadStore() {
            CoherentStore store;
            glGenBuffers(1, &store.vbo);
            glBindBuffer(GL_ARRAY_BUFFER, store.vbo);
            glBufferStorage(GL_ARRAY_BUFFER, kQuadBytes, nullptr, kCoherentFlags);
            if (FirstGLError() != 0u) {
                glDeleteBuffers(1, &store.vbo);
                store.vbo = 0;
                store.refusal =
                    "this driver has no immutable storage with GL_MAP_PERSISTENT_BIT|GL_MAP_COHERENT_BIT";
                return store;
            }
            // THE ONE ACQUISITION. Everything below writes through this pointer and never maps
            // again: AcquireMemoryRange only attempts an adoption while the store is not yet
            // resident, so a second map would emit a second map_persistent and make the counted
            // window's expected mpr a function of how often the test remapped.
            store.map = glMapBufferRange(GL_ARRAY_BUFFER, 0, kQuadBytes, kCoherentFlags);
            if (store.map == nullptr || FirstGLError() != 0u) {
                glDeleteBuffers(1, &store.vbo);
                store.vbo = 0;
                store.map = nullptr;
                store.refusal = "glMapBufferRange(PERSISTENT|WRITE|COHERENT) was refused";
            }
            return store;
        }

        void DescribeAttributes() {
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), nullptr);
            glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                                  reinterpret_cast<void*>(2 * sizeof(float)));
            glEnableVertexAttribArray(0);
            glEnableVertexAttribArray(1);
        }

        class PersistentCoherentMapScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                // Ready() is false both when there is no GPU and when the base SetUp skipped a
                // Split lane that has no client to assert against (ScenarioFixture.h).
                if (!Ready()) return;

                std::string error;
                m_program = CompileProgram(kVertexSource, kFragmentSource, &error);
                ASSERT_NE(m_program, 0u) << error;

                glGenVertexArrays(1, &m_vao);
                glBindVertexArray(m_vao);
                const CoherentStore store = MakeCoherentlyMappedQuadStore();
                if (!store.refusal.empty()) {
                    GTEST_SKIP() << store.refusal;
                }
                m_vbo = store.vbo;
                m_map = store.map;
                DescribeAttributes();
                ASSERT_EQ(FirstGLError(), 0u) << "configuring the VAO over the coherently mapped store";
            }

            void TearDown() override {
                if (!Ready()) return;
                glBindVertexArray(0);
                if (m_vbo != 0) {
                    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
                    if (m_map != nullptr) glUnmapBuffer(GL_ARRAY_BUFFER);
                    glBindBuffer(GL_ARRAY_BUFFER, 0);
                    glDeleteBuffers(1, &m_vbo);
                }
                if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
                if (m_program != 0) glDeleteProgram(m_program);
                m_vbo = m_vao = m_program = 0;
                m_map = nullptr;
            }

            // Everything the draw needs, set once, so that the write/draw pairs below are
            // memcpy + glDrawArrays and nothing else.
            void ArmTheDrawState() {
                HeadlessGL& gl = Gl();
                BindDefaultFramebuffer();
                glViewport(0, 0, gl.Width(), gl.Height());
                glDisable(GL_SCISSOR_TEST);
                glDisable(GL_DEPTH_TEST);
                ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
                glUseProgram(m_program);
                glBindVertexArray(m_vao);
                EXPECT_EQ(FirstGLError(), 0u) << "arming the draw state";
            }

            // THE WHOLE CONTRACT, IN TWO STATEMENTS. Nothing may be inserted between them - no
            // glGetError, no glBindBuffer, no assertion that calls into GL. See the file header.
            static void WriteThroughTheMapThenDraw(void* map, float r, float g, float b) {
                const std::array<Vertex, 6> quad = Quad(r, g, b);
                std::memcpy(map, quad.data(), sizeof(Vertex) * quad.size());
                glDrawArrays(GL_TRIANGLES, 0, 6);
            }

            // The whole surface minus an 8-pixel inset, so a primitive edge cannot contribute an
            // offender.
            ::testing::AssertionResult WholeSurfaceIs(const Image& image, const char* color,
                                                      const std::string& when) {
                return RegionIsMostly(image, 8, image.Width() - 9, 8, image.Height() - 9, color, 0.0, when);
            }

            unsigned int m_program = 0;
            unsigned int m_vao = 0;
            unsigned int m_vbo = 0;
            void* m_map = nullptr;
        };

    } // namespace

    // E3(b): map, write, draw, WRITE AGAIN, draw again, read back. The second write is announced
    // by nothing at all, so an implementation that pushed on an explicit buffer operation fails
    // here and only here.
    TEST_F(PersistentCoherentMapScenario, TwoWritesThroughTheCoherentPointerEachReachTheirOwnDraw) {
        if (!Ready() || IsSkipped()) return;
        ArmTheDrawState();

        WriteThroughTheMapThenDraw(m_map, 0.0f, 1.0f, 0.0f);
        const Image afterFirst = ReadPixels(Gl().Width(), Gl().Height());
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_TRUE(WholeSurfaceIs(afterFirst, "green",
                                   "the first write through a PERSISTENT|WRITE|COHERENT mapping, with "
                                   "no GL call announcing it"));

        // The second write. Whatever carried the first one - an unmap, a flush, the definition
        // itself - is in the past; only a push taken at this draw's validate point can carry it.
        WriteThroughTheMapThenDraw(m_map, 1.0f, 0.0f, 0.0f);
        const Image afterSecond = ReadPixels(Gl().Width(), Gl().Height());
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_TRUE(WholeSurfaceIs(afterSecond, "red",
                                   "the SECOND write through the same mapping, announced by nothing: "
                                   "this is exit gate E3(b), and a 'push on the next explicit buffer "
                                   "operation' implementation reads back the FIRST write's colour here"));
        Gl().EndFrame();
    }

    // The same claim across a frame boundary, because the dirty-block set is per-frame state on
    // the client and a tracker that cleared it at Present without pushing would pass the case
    // above and fail this one.
    TEST_F(PersistentCoherentMapScenario, AWriteAfterAFrameBoundaryReachesTheNextFramesDraw) {
        if (!Ready() || IsSkipped()) return;
        ArmTheDrawState();

        WriteThroughTheMapThenDraw(m_map, 0.0f, 1.0f, 0.0f);
        const Image first = ReadPixels(Gl().Width(), Gl().Height());
        EXPECT_TRUE(WholeSurfaceIs(first, "green", "frame 0's write through the mapping"));
        Gl().EndFrame();

        ArmTheDrawState();
        WriteThroughTheMapThenDraw(m_map, 1.0f, 0.0f, 0.0f);
        const Image second = ReadPixels(Gl().Width(), Gl().Height());
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_TRUE(WholeSurfaceIs(second, "red",
                                   "frame 1's write through the SAME mapping, after a Present"));
        Gl().EndFrame();
    }

    // E3(c) and the black-box half of E3(d). ONE reading case, in a lane of its own, for the
    // reason PipeStatsWindow.h gives: the library truncates its log per process, so two readers
    // in one lane race under `ctest -j`.
    TEST_F(PersistentCoherentMapScenario, TheMapLandsInTheArmItsLaneDeclares) {
        if (!Ready() || IsSkipped()) return;
        if (!BuildMarkerIsSet(kCounterLaneMarker)) {
            GTEST_SKIP() << "not the counting lane: " << kCounterLaneMarker
                         << " is set only by the *.PersistentMapArm.* entries, which give this case "
                            "a private MOBILEGL_LOG_FILE_PATH and a RESOURCE_LOCK on it";
        }
        if (!BuildMarkerIsSet("MGITEST_PIPE_PUSH_BUILD")) {
            GTEST_SKIP() << "pull build: mpr= lives in the cso[ bracket, which is compiled only "
                            "under MOBILEGL_PIPE_PUSH, so the summary line does not carry it";
        }
        if (!BuildMarkerIsSet("MGITEST_PIPE_RESOURCE_EMITTER_PRESENT")) {
            GTEST_SKIP() << "no MG_Impl/Pipe source emits MapPersistentRoundtrips on this tree, so "
                            "mpr= is structurally zero and an assertion about it would be a "
                            "statement about nothing";
        }

        Gl().EndFrame(); // close the setup window; SetUp's own store and map go in it

        // One acquisition inside the counted window: one fresh immutable store, mapped once.
        const CoherentStore store = MakeCoherentlyMappedQuadStore();
        if (!store.refusal.empty()) {
            GTEST_SKIP() << store.refusal;
        }
        glBindVertexArray(m_vao);
        glBindBuffer(GL_ARRAY_BUFFER, store.vbo);
        DescribeAttributes();
        ArmTheDrawState();

        // ... and then a frame's worth of traffic through it. Four writes and four draws must
        // still cost ONE acquisition.
        for (int draw = 0; draw < kDrawsInTheWindow; ++draw) {
            WriteThroughTheMapThenDraw(store.map, 0.0f, 1.0f, 0.0f);
        }
        const Image image = ReadPixels(Gl().Width(), Gl().Height());
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_TRUE(WholeSurfaceIs(image, "green",
                                   "the draws inside the counted window; if they never landed, every "
                                   "number below is a number about nothing"));

        Gl().EndFrame(); // the swap that emits the window covering exactly the work above

        const PipeStatsWindow::Window window = PipeStatsWindow::LastFromLaneLog();
        ASSERT_TRUE(window.found) << "no 'MGPipe stats:' line in " << PipeStatsWindow::LibraryLogPath()
                                  << ": either MOBILEGL_PIPE_STATS / MOBILEGL_PIPE_STATS_PERIOD did "
                                     "not reach the process, or nothing reached PipeStats::OnPresent.";
        RecordProperty("stats_line", window.line.c_str());

        const long long roundtrips = PipeStatsWindow::CounterOrAbsent(window, "mpr");
        ASSERT_GE(roundtrips, 0) << "the summary line carries no mpr= field: " << window.line;
        EXPECT_EQ(roundtrips, kExpectedRoundtrips)
            << "one PERSISTENT|WRITE|COHERENT map of one freshly defined store is ONE acquisition "
               "attempt, mint or decline (ARCHITECTURE.md:492). This window mapped once and drew "
            << kDrawsInTheWindow << " times, so " << kExpectedRoundtrips << " is the whole cost; "
            << kDrawsInTheWindow
            << " would mean the acquisition moved onto the draw path. The number is counted as "
               "ATTEMPTS precisely so that it is the SAME on the adopted and the emulated arm and "
               "under both transports - which is what makes exit gate E3(c)'s 'mpr equal to the "
               "monolith arm's' checkable by one process. It reported: "
            << window.line;

        // The arm. pmap is bytes PUSHED because a persistently mapped range had to be published,
        // and a push happens if and only if the acquisition was DECLINED - so pmap > 0 is the
        // emulated arm and pmap == 0 is the adopted one.
        const double pushedBytes = PipeStatsWindow::CounterAsDoubleOrAbsent(window, "pmap");
        ASSERT_GE(pushedBytes, 0.0) << "the summary line carries no pmap= field: " << window.line;
        const char* observedArm = (pushedBytes > 0.0) ? "emulated" : "adopted";
        RecordProperty("persistent_map_arm", observedArm);
        RecordProperty("persistent_map_push_bytes_per_frame", std::to_string(pushedBytes).c_str());
        RecordProperty("map_persistent_roundtrips", std::to_string(roundtrips).c_str());

        const std::string declared = SplitLane::DeclaredPersistentMapArm();
        if (declared.empty()) {
            // SUCCEED, not GTEST_SKIP. The mpr assertion above is this lane's real claim and it
            // has already been made; the arm is the half it cannot assert, because WHICH arm
            // AcquireMemoryRange takes for a sub-16-MiB map is a property of the driver (llvmpipe
            // mints, a device may decline) and a monolith lane that pinned one would be red on
            // hardware for a reason that is not a defect. A skip here would have thrown away the
            // mpr result with it - and mpr is the number the split lane is compared against.
            SUCCEED() << "this lane declares no MGITEST_PERSISTENT_MAP_ARM, so the arm is RECORDED "
                         "and not asserted, and the assertion this lane does make - one coherent "
                         "map costs one acquisition attempt - passed. Observed arm: "
                      << observedArm << " (pmap=" << pushedBytes << ", mpr=" << roundtrips << "). "
                      << window.line;
            glBindVertexArray(0);
            unsigned int recordedVbo = store.vbo;
            glBindBuffer(GL_ARRAY_BUFFER, recordedVbo);
            glUnmapBuffer(GL_ARRAY_BUFFER);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            glDeleteBuffers(1, &recordedVbo);
            return;
        }
        EXPECT_EQ(declared, std::string(observedArm))
            << "the lane declared the " << declared << " arm and the library took the " << observedArm
            << " one (pmap=" << pushedBytes
            << "). R-6 pins the split lane at T2 = emulated, and the two arms are completely "
               "different code paths: adopted means MGPipeApplyMapPersistent handed back a pointer "
               "and the CPU shadow was released, emulated means it declined and the client has to "
               "push blocks. Under inproc an ADOPTED store still renders correctly - the address is "
               "real in this process - so pixels cannot tell them apart and this counter is the "
               "only thing that can. That is exit gate E3(d) in the only spelling a black-box "
               "scenario has. Line: "
            << window.line;
        if (declared == "emulated") {
            EXPECT_GT(pushedBytes, 0.0)
                << "the emulated arm pushes the mapping's dirty blocks at every validate point, so "
                   "a window with four writes and four draws through a live coherent mapping cannot "
                   "have pushed zero bytes. Zero here with MOBILEGL_IPC_PERSISTENT_BLOCK_KB at its "
                   "default is the push never being wired; zero with it set to 0 is exit gate "
                   "E3(a)'s negative control working as intended. Line: "
                << window.line;
        }

        unsigned int vbo = store.vbo;
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glUnmapBuffer(GL_ARRAY_BUFFER);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glDeleteBuffers(1, &vbo);
    }

} // namespace MGITest
