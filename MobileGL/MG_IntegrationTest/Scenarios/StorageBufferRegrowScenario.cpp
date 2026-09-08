// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/StorageBufferRegrowScenario.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - glBufferData GROWS A BUFFER THAT IS ALREADY BOUND AT AN INDEXED POINT.
//
// GL says the indexed binding follows the buffer object, so after the store is re-specified the
// shader sees the NEW extent. DirectGLES shadows the indexed bindings so a redundant
// glBindBufferBase can be skipped, and nothing used to invalidate that shadow when the store was
// re-specified underneath it - so on a driver that resolves a whole-buffer indexed binding's
// extent at BIND time (Adreno does; Mali does not) the shader kept seeing the OLD, smaller range.
// Stores past it are dropped and loads return zero, which is exactly what
// KHR-GL43.compute_shader.dispatch-indirect reported: the first iteration's 6 elements correct and
// everything past byte 24 zero, after the same buffer was re-specified from 24 to 96 bytes.
//
// The assertion is deliberately on the WHOLE grown range, so a partial write names the byte the
// stale extent stopped at.
//
// P3a ADDS THE COST OF THAT REGROWTH (gate G10). ARCHITECTURE.md:474 prices a persistently mapped
// store at "one round trip per STORAGE DEFINITION, not one per store" - and, emphatically, not one
// per draw. `map-persistent-roundtrips` (`mpr=` in the summary line) counts every map_persistent
// EMISSION, mint or decline (D-B2), so the claim is directly countable: N definitions of an
// adopted store must publish exactly N, whatever the workload does between them. A regression that
// re-acquires per dispatch reports N x dispatches, which is the failure this case exists to name;
// a regression that stops emitting reports 0.
//
// The second case therefore respecifies a store LARGE ENOUGH TO BE ADOPTED
// (BufferObject::TryAdoptLargeStorage's 16 MiB threshold), several times, with several dispatches
// between the definitions, and reads the one window that covers exactly that workload. It skips -
// visibly, with the reason - on a tree where nothing emits the counter yet.

#include <cstdlib>
#include <string>
#include <vector>

#include "../Harness/HeadlessGL.h"
#include "../Harness/PipeStatsWindow.h"
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

        constexpr const char* kComputeSource = R"(#version 430 core
layout(local_size_x = 1) in;
layout(std430, binding = 0) buffer Output {
    uint g_data[];
};
void main() {
    g_data[gl_GlobalInvocationID.x] = gl_GlobalInvocationID.x + 1u;
}
)";

        constexpr int kSmallElements = 6;  // 24 bytes - the first iteration's size
        constexpr int kLargeElements = 24; // 96 bytes - what the second iteration grows to

        // The G10 case's sizes. Every one of them is past BufferObject::TryAdoptLargeStorage's
        // 16 MiB threshold, because a store below it is never offered for adoption at all and the
        // window would then be asserting that nothing happened. They GROW, which is the scenario's
        // subject: each glBufferData is a new storage definition, so each is one acquisition.
        constexpr int kAdoptedDefinitions = 3;
        constexpr int kAdoptedBaseElements = 5 * 1024 * 1024;  // 20 MiB of uint
        constexpr int kAdoptedGrowthElements = 1024 * 1024;    // + 4 MiB per definition
        // Enough dispatches per definition that "one per definition" and "one per dispatch" are
        // different numbers by a wide margin (3 vs 12), and few enough to stay cheap.
        constexpr int kDispatchesPerDefinition = 4;
        // Only the first elements are dispatched over: the point of the large store is the
        // ADOPTION, not the compute cost.
        constexpr int kDispatchedElements = 6;

        // Set by the MapPersistentRoundtrips. ctest entry and by nothing else; a harness marker,
        // never read by the library. Its absence means an ambient entry, where neither the stats
        // channel nor a private log path is configured - and where the shared log makes a read
        // race a neighbour's bring-up.
        constexpr const char* kLaneMarker = "MGITEST_MPR_LANE";

        bool BuildMarkerIsSet(const char* name) {
            const char* value = std::getenv(name);
            return value != nullptr && value[0] == '1' && value[1] == '\0';
        }

        class StorageBufferRegrowScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                m_program = CompileComputeProgram(kComputeSource);
                ASSERT_NE(m_program, 0u) << m_buildLog;
                glGenBuffers(1, &m_buffer);
            }

            void TearDown() override {
                if (!Ready()) return;
                if (m_buffer != 0) glDeleteBuffers(1, &m_buffer);
                if (m_program != 0) glDeleteProgram(m_program);
            }

            unsigned int CompileComputeProgram(const char* source) {
                const GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
                glShaderSource(shader, 1, &source, nullptr);
                glCompileShader(shader);
                GLint compiled = 0;
                glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
                if (compiled == GL_FALSE) {
                    char log[2048] = {};
                    glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
                    m_buildLog = std::string("compute shader did not compile: ") + log;
                    glDeleteShader(shader);
                    return 0;
                }
                const GLuint program = glCreateProgram();
                glAttachShader(program, shader);
                glLinkProgram(program);
                glDeleteShader(shader);
                GLint linked = 0;
                glGetProgramiv(program, GL_LINK_STATUS, &linked);
                if (linked == GL_FALSE) {
                    char log[2048] = {};
                    glGetProgramInfoLog(program, sizeof(log) - 1, nullptr, log);
                    m_buildLog = std::string("compute program did not link: ") + log;
                    glDeleteProgram(program);
                    return 0;
                }
                return program;
            }

            void RespecifyTo(int elements) {
                const std::vector<unsigned int> zeros(static_cast<std::size_t>(elements), 0u);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_buffer);
                glBufferData(GL_SHADER_STORAGE_BUFFER,
                             static_cast<GLsizeiptr>(zeros.size() * sizeof(unsigned int)), zeros.data(),
                             GL_DYNAMIC_COPY);
            }

            std::vector<unsigned int> DispatchAndRead(int elements) {
                glUseProgram(m_program);
                glDispatchCompute(static_cast<GLuint>(elements), 1, 1);
                glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
                std::vector<unsigned int> values(static_cast<std::size_t>(elements), 0xDEADBEEFu);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_buffer);
                glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                                   static_cast<GLsizeiptr>(values.size() * sizeof(unsigned int)), values.data());
                return values;
            }

            // The Minecraft arena idiom, and the adoption point: a NULL-data definition of a
            // store past the threshold. No host-side vector, so a 28 MiB definition costs
            // nothing on this side of the API.
            void DefineAdoptedStore(int elements) {
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_buffer);
                glBufferData(GL_SHADER_STORAGE_BUFFER,
                             static_cast<GLsizeiptr>(static_cast<GLsizeiptr>(elements) *
                                                     static_cast<GLsizeiptr>(sizeof(unsigned int))),
                             nullptr, GL_DYNAMIC_DRAW);
            }

            // GTEST_SKIP() returns from the function it is written in, so this cannot report
            // through a return value; the caller pairs it with `if (IsSkipped()) return;`.
            void SkipUnlessTheRoundtripCounterIsReadableHere() {
                if (std::getenv(kLaneMarker) == nullptr) {
                    GTEST_SKIP() << "runs only in its own lane: the MapPersistentRoundtrips. ctest entry "
                                    "sets MGITEST_MPR_LANE together with MOBILEGL_PIPE_PUSH's P3a mask, "
                                    "MOBILEGL_PIPE_STATS=1, MOBILEGL_PIPE_STATS_PERIOD=1 and a private "
                                    "MOBILEGL_LOG_FILE_PATH. None of that is configured in the ambient "
                                    "entries, and the ambient log is shared, so a read here would race a "
                                    "neighbour's bring-up.";
                    return;
                }
                if (!BuildMarkerIsSet("MGITEST_PIPE_PUSH_BUILD")) {
                    GTEST_SKIP() << "this library was built without MOBILEGL_PIPE_PUSH, so "
                                    "CallClass::MapPersistentRoundtrips does not exist (PipeStats.h "
                                    "declares it inside the push guard, because growing the enum in a "
                                    "pull build resizes the counter arrays and the name table - a G1 "
                                    "break for a counter that could never leave zero) and the summary "
                                    "line carries no mpr=. The entry is registered here anyway so that "
                                    "`ctest -L integration-gpu` names the same tests in the pull build "
                                    "and the push build (gate G2).";
                    return;
                }
                if (!BuildMarkerIsSet("MGITEST_PIPE_RESOURCE_EMITTER_PRESENT")) {
                    GTEST_SKIP() << "subsystem not implemented on this tree: no source under "
                                    "MobileGL/MG_Impl/Pipe/ names MapPersistentRoundtrips, so nothing "
                                    "emits map_persistent and mpr= is structurally zero. P3a package B "
                                    "owns the client-side resource tracker; this entry arms itself when "
                                    "it lands, whatever file that package puts the emitter in.";
                    return;
                }
                if (PipeStatsWindow::LibraryLogPath().empty()) {
                    GTEST_SKIP() << "the lane configured no MOBILEGL_LOG_FILE_PATH, and the library's "
                                    "summary line is the only channel this module has for reading "
                                    "PipeStats";
                    return;
                }
            }

            unsigned int m_program = 0;
            GLuint m_buffer = 0;
            std::string m_buildLog;
        };

    } // namespace

    TEST_F(StorageBufferRegrowScenario, AGrownStoreIsVisibleThroughItsExistingIndexedBinding) {
        if (!Ready() || IsSkipped()) return;

        // Iteration one: 24 bytes, bound once, six groups.
        RespecifyTo(kSmallElements);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_buffer);
        ASSERT_EQ(FirstGLError(), 0u);

        const std::vector<unsigned int> small = DispatchAndRead(kSmallElements);
        ASSERT_EQ(FirstGLError(), 0u);
        for (int i = 0; i < kSmallElements; ++i) {
            ASSERT_EQ(small[static_cast<std::size_t>(i)], static_cast<unsigned int>(i + 1))
                << "the 24-byte iteration itself did not write element " << i;
        }

        // Iteration two: the SAME buffer grows to 96 bytes with NO new glBindBufferBase, which is
        // what the application is entitled to do and what the shadow used to swallow.
        RespecifyTo(kLargeElements);
        ASSERT_EQ(FirstGLError(), 0u);

        const std::vector<unsigned int> large = DispatchAndRead(kLargeElements);
        EXPECT_EQ(FirstGLError(), 0u);
        for (int i = 0; i < kLargeElements; ++i) {
            EXPECT_EQ(large[static_cast<std::size_t>(i)], static_cast<unsigned int>(i + 1))
                << "element " << i << " (byte " << i * 4 << ") of the grown store came back as "
                << large[static_cast<std::size_t>(i)]
                << "; zero from element " << kSmallElements
                << " on means the shader still saw the pre-growth extent";
        }

        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
    }

    // G10. N storage definitions of an adopted store cost N map-persistent round trips - not one
    // per draw, and not zero.
    //
    // ONE case in this lane, and that is a constraint rather than a preference: it READS the
    // library log, the log is a per-lane resource (the library opens it fopen(path, "w"), so every
    // process in a lane truncates it), and a second reading entry in the same lane would race this
    // one under `ctest -j` with a failure that looks exactly like "the counter was never emitted".
    // The plumbing is therefore asserted first, with its own message, inside this one process.
    TEST_F(StorageBufferRegrowScenario, NStorageDefinitionsCostNMapPersistentRoundtripsNotOnePerDraw) {
        if (!Ready()) return;
        SkipUnlessTheRoundtripCounterIsReadableHere();
        if (IsSkipped()) return;

        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_buffer);
        Gl().EndFrame(); // close the setup window: everything below is one window

        for (int definition = 0; definition < kAdoptedDefinitions; ++definition) {
            DefineAdoptedStore(kAdoptedBaseElements + definition * kAdoptedGrowthElements);
            ASSERT_EQ(FirstGLError(), 0u) << "definition " << definition << " of the adopted store failed";
            for (int dispatch = 0; dispatch < kDispatchesPerDefinition; ++dispatch) {
                glUseProgram(m_program);
                glDispatchCompute(static_cast<GLuint>(kDispatchedElements), 1, 1);
                glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
            }
        }

        // The store is still the one the last definition made, and it still works: a counter
        // assertion over a workload that silently stopped functioning would be measuring nothing.
        std::vector<unsigned int> values(static_cast<std::size_t>(kDispatchedElements), 0xDEADBEEFu);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_buffer);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                           static_cast<GLsizeiptr>(values.size() * sizeof(unsigned int)), values.data());
        EXPECT_EQ(FirstGLError(), 0u);
        for (int i = 0; i < kDispatchedElements; ++i) {
            EXPECT_EQ(values[static_cast<std::size_t>(i)], static_cast<unsigned int>(i + 1))
                << "the adopted store's own dispatch did not write element " << i;
        }

        Gl().EndFrame(); // the swap that emits the window covering exactly the loop above
        const PipeStatsWindow::Window window = PipeStatsWindow::LastFromLaneLog();
        ASSERT_TRUE(window.found) << "no 'MGPipe stats:' line in " << PipeStatsWindow::LibraryLogPath()
                                  << ". This IS a push build (the lane checked MGITEST_PIPE_PUSH_BUILD "
                                     "before getting here), so either MOBILEGL_PIPE_STATS / "
                                     "MOBILEGL_PIPE_STATS_PERIOD did not reach the process or no summary "
                                     "line was emitted at all because nothing reached PipeStats::OnPresent.";
        RecordProperty("stats_line", window.line.c_str());

        const long long roundtrips = PipeStatsWindow::CounterOrAbsent(window, "mpr");
        ASSERT_GE(roundtrips, 0)
            << "the summary line carries no mpr= field, so this build's PipeStats has no "
               "map-persistent-roundtrips counter to read: " << window.line;
        EXPECT_EQ(roundtrips, static_cast<long long>(kAdoptedDefinitions))
            << "an adopted store costs ONE map_persistent per STORAGE DEFINITION "
               "(ARCHITECTURE.md:474). This window defined the store " << kAdoptedDefinitions
            << " times and dispatched " << kDispatchesPerDefinition << " times against each of them, so "
            << kAdoptedDefinitions << " is the whole cost. "
            << (kAdoptedDefinitions * kDispatchesPerDefinition)
            << " would mean an acquisition per DRAW - the regression this counter exists to catch - and 0 "
               "would mean nothing emitted map_persistent at all. It reported: "
            << window.line;

        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
    }
} // namespace MGITest
