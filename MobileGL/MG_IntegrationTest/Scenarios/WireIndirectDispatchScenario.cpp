// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/WireIndirectDispatchScenario.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// P8-SV: THE MAGMA WIRE ARM ISSUES glDispatchComputeIndirect FROM THE WIRE STORE, AS THE MONOLITH
// ARM DOES (docs/Disaggregated/notes/p8/SV.md).
//
// Before P8-SV the wire arm read the three group counts through ReadWireBuffer and dispatched them
// from the CPU, so a dispatch buffer a shader had written cost a whole-GPU wait per call - the
// dispatch twin of what P8-D removed for indirect draws (WireIndirectDrawScenario.cpp). The cases
// pin both halves:
//   * THE GRID, on every arm and both backends - each dispatch marks exactly the work groups its
//     counts name, and GPU-written counts are the ones the dispatch sees (the buffer starts with
//     OTHER counts, so a stale read shows as the wrong grid);
//   * THE SERVER'S OWN COUNTERS (Harness/WireIndirectPeek.h), where they can be read - Magma on the
//     inproc arm: every call issued as vkCmdDispatchIndirect (`wdsp`), no host wait (`whw`), and the
//     INDIRECT_COMMAND_READ barrier exactly once per shader write (`wibar`).
// The counters are read BEFORE the grids are read back: that readback is a host read of a store a
// shader wrote, so it takes a host wait of its own, which is the application's and not the
// dispatch's.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "../Harness/HeadlessGL.h"
#include "../Harness/ScenarioFixture.h"
#include "../Harness/SplitRuntimePeek.h"
#include "../Harness/WireIndirectPeek.h"

#ifdef GLAPI
#undef GLAPI
#endif
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>

namespace MGITest {
    namespace {

        // Grids up to 4 x 4 x 2: one cell per work group.
        constexpr std::uint32_t kGridX = 4, kGridY = 4, kGridZ = 2;
        constexpr std::uint32_t kCells = kGridX * kGridY * kGridZ;

        // One invocation per work group, and each group marks its own cell.
        constexpr const char* kMarkSource = R"(#version 430 core
layout(local_size_x = 1) in;
layout(std430, binding = 2) buffer Marks { uint marks[]; };
void main() {
    uvec3 g = gl_WorkGroupID;
    atomicAdd(marks[(g.z * 4u + g.y) * 4u + g.x], 1u);
}
)";
        // Copies `source` into `words`, one word per invocation: the compute write under test.
        constexpr const char* kCopySource = R"(#version 430 core
layout(local_size_x = 1) in;
layout(std430, binding = 0) buffer Words { uint words[]; };
layout(std430, binding = 1) readonly buffer Source { uint source[]; };
void main() { words[gl_GlobalInvocationID.x] = source[gl_GlobalInvocationID.x]; }
)";

        using Grid = std::array<std::uint32_t, 3>;

        class WireIndirectDispatchScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                m_magma = Gl().BackendName() == "DirectVulkan";
                m_transport = PeekSplitRuntime().transportName;
                m_mark = CompileCompute(kMarkSource);
                ASSERT_NE(m_mark, 0u);
                m_copy = CompileCompute(kCopySource);
                ASSERT_NE(m_copy, 0u);
                ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "scene setup left a GL error behind";
                // The counters are armed by the first peek, so it has to come before any dispatch.
                if (CountersReadable()) {
                    ASSERT_TRUE(PeekWireIndirectCounters(&m_before))
                        << "Magma on the inproc arm must be able to read the wire arm's own counters";
                }
            }

            void TearDown() override {
                if (!Ready()) return;
                glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, 0);
                for (GLuint& buffer : m_marks) {
                    if (buffer != 0) glDeleteBuffers(1, &buffer);
                }
                for (GLuint* buffer : {&m_parameters, &m_source}) {
                    if (*buffer != 0) glDeleteBuffers(1, buffer);
                    *buffer = 0;
                }
                if (m_mark != 0) glDeleteProgram(m_mark);
                if (m_copy != 0) glDeleteProgram(m_copy);
            }

            static GLuint CompileCompute(const char* source) {
                const GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
                glShaderSource(shader, 1, &source, nullptr);
                glCompileShader(shader);
                GLint ok = GL_FALSE;
                glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
                if (ok != GL_TRUE) {
                    glDeleteShader(shader);
                    return 0;
                }
                const GLuint program = glCreateProgram();
                glAttachShader(program, shader);
                glLinkProgram(program);
                glDeleteShader(shader);
                glGetProgramiv(program, GL_LINK_STATUS, &ok);
                if (ok != GL_TRUE) {
                    glDeleteProgram(program);
                    return 0;
                }
                return program;
            }

            static void Fill(GLuint& name, GLenum target, const std::vector<std::uint32_t>& words) {
                if (name == 0) glGenBuffers(1, &name);
                glBindBuffer(target, name);
                glBufferData(target, static_cast<GLsizeiptr>(words.size() * sizeof(std::uint32_t)), words.data(),
                             GL_DYNAMIC_COPY);
            }

            // The dispatch-indirect store, holding CPU-written counts.
            void Parameters(const std::vector<std::uint32_t>& words) {
                Fill(m_parameters, GL_DISPATCH_INDIRECT_BUFFER, words);
            }

            // The compute write: the parameter store (already holding its poison) receives `words`.
            void ComputeWriteParameters(const std::vector<std::uint32_t>& words) {
                Fill(m_source, GL_SHADER_STORAGE_BUFFER, words);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_parameters);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_source);
                glUseProgram(m_copy);
                glDispatchCompute(static_cast<GLuint>(words.size()), 1, 1);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, 0);
            }

            // One indirect dispatch of the mark program into a fresh, zeroed cell buffer. Returns
            // the index of that buffer for ExpectGrid.
            size_t DispatchIndirect(GLintptr offset) {
                GLuint marks = 0;
                Fill(marks, GL_SHADER_STORAGE_BUFFER, std::vector<std::uint32_t>(kCells, 0u));
                m_marks.push_back(marks);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, marks);
                glUseProgram(m_mark);
                glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, m_parameters);
                glDispatchComputeIndirect(offset);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, 0);
                return m_marks.size() - 1;
            }

            // A round trip that reads no buffer store, so the server has applied every dispatch
            // above before the counters are read - and no host wait of its own is added.
            static void Settle() {
                BindDefaultFramebuffer();
                (void)ReadPixels(1, 1);
            }

            void ExpectGrid(size_t which, const Grid& grid, const std::string& what) {
                glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
                std::vector<std::uint32_t> cells(kCells, 0xDEADu);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_marks[which]);
                glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(kCells * sizeof(std::uint32_t)),
                                   cells.data());
                ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << what << ": readback";
                size_t wrong = 0;
                std::string first;
                for (std::uint32_t z = 0; z < kGridZ; ++z) {
                    for (std::uint32_t y = 0; y < kGridY; ++y) {
                        for (std::uint32_t x = 0; x < kGridX; ++x) {
                            const bool inside = x < grid[0] && y < grid[1] && z < grid[2];
                            const std::uint32_t got = cells[(z * kGridY + y) * kGridX + x];
                            if (got != (inside ? 1u : 0u) && wrong++ == 0) {
                                first = "group (" + std::to_string(x) + "," + std::to_string(y) + "," +
                                        std::to_string(z) + ") = " + std::to_string(got);
                            }
                        }
                    }
                }
                EXPECT_EQ(wrong, 0u) << what << ": expected exactly the " << grid[0] << "x" << grid[1] << "x"
                                     << grid[2] << " grid marked once; first wrong " << first;
            }

            // Magma's server in THIS process: the inproc arm. Spawn and tcp servers are other
            // processes, and the monolith arm never takes the wire path at all.
            bool CountersReadable() const { return m_magma && m_transport == "inproc"; }

            WireIndirectCounters Delta() const {
                WireIndirectCounters now{};
                EXPECT_TRUE(PeekWireIndirectCounters(&now));
                WireIndirectCounters delta{};
                delta.nativeDraws = now.nativeDraws - m_before.nativeDraws;
                delta.cpuExpansions = now.cpuExpansions - m_before.cpuExpansions;
                delta.barriers = now.barriers - m_before.barriers;
                delta.hostWaits = now.hostWaits - m_before.hostWaits;
                delta.indirectWaits = now.indirectWaits - m_before.indirectWaits;
                delta.nativeDispatches = now.nativeDispatches - m_before.nativeDispatches;
                return delta;
            }

            bool m_magma = false;
            std::string m_transport;
            WireIndirectCounters m_before{};
            GLuint m_mark = 0, m_copy = 0, m_parameters = 0, m_source = 0;
            std::vector<GLuint> m_marks;
        };

        // CPU-written counts at two non-zero offsets of one store. On Magma/inproc both calls must
        // be issued natively, with no host wait and no barrier (no shader wrote the store).
        TEST_F(WireIndirectDispatchScenario, CpuWrittenGroupCountsAreReadByTheGpuAtTheirOffsets) {
            if (!Ready() || IsSkipped()) return;
            Parameters({1, 1, 1, 3, 2, 1, 4, 1, 2});
            const size_t first = DispatchIndirect(12);
            const size_t second = DispatchIndirect(24);
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
            Settle();
            if (CountersReadable()) {
                const WireIndirectCounters delta = Delta();
                EXPECT_EQ(delta.nativeDispatches, 2u) << "every indirect dispatch must be vkCmdDispatchIndirect";
                EXPECT_EQ(delta.hostWaits, 0u) << "a host wait was taken with no GPU write to the counts";
                EXPECT_EQ(delta.barriers, 0u) << "no shader wrote this store, so no barrier is due";
            }
            ExpectGrid(first, {3, 2, 1}, "glDispatchComputeIndirect at byte 12");
            ExpectGrid(second, {4, 1, 2}, "glDispatchComputeIndirect at byte 24");
        }

        // Counts a compute shader wrote, behind the application's own GL_COMMAND_BARRIER_BIT. The
        // store starts with OTHER counts, so a stale read dispatches the wrong grid. On
        // Magma/inproc: issued natively and - the point of the change - NO host wait.
        TEST_F(WireIndirectDispatchScenario, ComputeWrittenGroupCountsDispatchWithoutAHostWait) {
            if (!Ready() || IsSkipped()) return;
            Parameters({1, 1, 1});
            ComputeWriteParameters({3, 2, 1});
            glMemoryBarrier(GL_COMMAND_BARRIER_BIT);
            const size_t first = DispatchIndirect(0);

            Parameters({1, 1, 1, 1, 1, 1});
            ComputeWriteParameters({1, 1, 1, 2, 2, 2});
            glMemoryBarrier(GL_COMMAND_BARRIER_BIT);
            const size_t second = DispatchIndirect(12);
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
            Settle();
            if (CountersReadable()) {
                const WireIndirectCounters delta = Delta();
                EXPECT_EQ(delta.nativeDispatches, 2u) << "both dispatches must be issued natively";
                EXPECT_EQ(delta.hostWaits, 0u)
                    << "an indirect dispatch of shader-written counts waited for the GPU on the host";
                EXPECT_EQ(delta.barriers, 2u) << "one INDIRECT_COMMAND_READ barrier per freshly written store";
            }
            ExpectGrid(first, {3, 2, 1}, "compute-written counts at byte 0");
            ExpectGrid(second, {2, 2, 2}, "compute-written counts at byte 12");
        }

        // Flywheel's shape, one call over: the counts are written and dispatched with no
        // glMemoryBarrier between (GL leaves it undefined). The wire arm orders it anyway, once per
        // write, as P8-D does for draws. Magma's split arms only: the monolith arm records no
        // dependency here (P8-D recorded that half for dev), and Espryt's is its driver's.
        TEST_F(WireIndirectDispatchScenario, ShaderWrittenGroupCountsWithoutAnApplicationBarrierAreOrdered) {
            if (!Ready() || IsSkipped()) return;
            if (!m_magma || m_transport == "monolith") {
                GTEST_SKIP() << "the implicit INDIRECT_COMMAND_READ barrier is the Magma wire arm's (P8-D, P8-SV)";
            }
            Parameters({1, 1, 1});
            ComputeWriteParameters({3, 2, 1});
            const size_t first = DispatchIndirect(0);
            const size_t again = DispatchIndirect(0);
            ComputeWriteParameters({1, 2, 2});
            const size_t third = DispatchIndirect(0);
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
            Settle();
            if (CountersReadable()) {
                const WireIndirectCounters delta = Delta();
                EXPECT_EQ(delta.nativeDispatches, 3u);
                EXPECT_EQ(delta.hostWaits, 0u) << "a host wait was taken";
                EXPECT_EQ(delta.barriers, 2u) << "exactly one barrier per write: the second dispatch reads no new write";
            }
            ExpectGrid(first, {3, 2, 1}, "the first dispatch after the write");
            ExpectGrid(again, {3, 2, 1}, "a second dispatch, no write between");
            ExpectGrid(third, {1, 2, 2}, "the dispatch after a second write");
        }

    } // namespace
} // namespace MGITest
