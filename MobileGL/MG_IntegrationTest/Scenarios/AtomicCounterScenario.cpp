// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/AtomicCounterScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - ATOMIC COUNTERS, END TO END.
//
// GL_ATOMIC_COUNTER_BUFFER does not exist in ES, and glslang does not hand one to a backend
// either: its Vulkan-relaxed parse rewrites every atomic_uint into a uint member of a
// synthesized gl_AtomicCounterBlock_<N> STORAGE block. Making counters work therefore means
// closing two open ends that used to be missing entirely -
//
//   * the block's shader-storage binding, which the IO mapper picked at random and which had no
//     relation to the GL binding point N the application bound its buffer to (and could alias an
//     SSBO the application binds itself), is moved to a slot reserved at the top of the driver's
//     range; and
//   * the buffer bound at GL_ATOMIC_COUNTER_BUFFER point N, which nothing in the ES backend ever
//     read, is re-issued as a shader-storage binding at that reserved slot.
//
// Neither end alone is observable: with only the first the shader increments a block nobody
// bound a buffer to, with only the second the buffer lands where the shader does not look. The
// only thing that proves both is the VALUE, so every assertion here reads the counter back.
//
// Compute rather than a draw on purpose: the invocation count is exactly what was dispatched,
// while a fragment stage's is a property of the rasterizer (helper invocations, early depth).
// Conformance cases behind this: KHR-GL42/GL43.shader_atomic_counters.basic-usage-cs,
// .advanced-usage-multi-stage and .advanced-usage-draw-update-draw.

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

        // Two counters share binding 0 at DIFFERENT offsets and a third sits alone on binding 1.
        // The offsets are what separates "the buffer arrived" from "the buffer arrived and the
        // block is laid out the way GL says": a lowering that packed the members in declaration
        // order without honouring `offset` would still pass a single-counter check.
        constexpr const char* kCounterComputeSource = R"(#version 430 core
layout(local_size_x = 4) in;
layout(binding = 0, offset = 0) uniform atomic_uint g_first;
layout(binding = 0, offset = 4) uniform atomic_uint g_second;
layout(binding = 1, offset = 0) uniform atomic_uint g_other;
void main() {
    atomicCounterIncrement(g_first);
    atomicCounterIncrement(g_second);
    atomicCounterIncrement(g_second);
    atomicCounterIncrement(g_other);
}
)";

        constexpr int kLocalSizeX = 4;
        constexpr int kWorkGroups = 2;
        constexpr unsigned int kInvocations = kLocalSizeX * kWorkGroups;

        // Deliberately non-zero: the shader adds to whatever the application uploaded, so a seed
        // that survives is also proof that the buffer's CPU-side contents reached the driver.
        constexpr unsigned int kSeedFirst = 5;
        constexpr unsigned int kSeedSecond = 100;
        constexpr unsigned int kSeedOther = 7;

        class AtomicCounterScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                GLint counters = 0;
                glGetIntegerv(GL_MAX_COMPUTE_ATOMIC_COUNTERS, &counters);
                GLint buffers = 0;
                glGetIntegerv(GL_MAX_COMPUTE_ATOMIC_COUNTER_BUFFERS, &buffers);
                if (counters < 3 || buffers < 2) {
                    GTEST_SKIP() << "GL_MAX_COMPUTE_ATOMIC_COUNTERS is " << counters
                                 << " and GL_MAX_COMPUTE_ATOMIC_COUNTER_BUFFERS is " << buffers
                                 << "; this needs 3 and 2";
                }
                m_program = CompileComputeProgram(kCounterComputeSource);
                ASSERT_NE(m_program, 0u) << m_buildLog;
            }

            void TearDown() override {
                if (!Ready()) return;
                glUseProgram(0);
                if (!m_buffers.empty()) glDeleteBuffers(static_cast<GLsizei>(m_buffers.size()), m_buffers.data());
                if (m_program != 0) glDeleteProgram(m_program);
                m_buffers.clear();
                m_program = 0;
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

            // A counter buffer of `count` uints, seeded and bound to atomic-counter point
            // `binding`.
            GLuint MakeCounterBuffer(GLuint binding, const std::vector<unsigned int>& seed) {
                GLuint buffer = 0;
                glGenBuffers(1, &buffer);
                glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, buffer);
                glBufferData(GL_ATOMIC_COUNTER_BUFFER,
                             static_cast<GLsizeiptr>(seed.size() * sizeof(unsigned int)), seed.data(),
                             GL_DYNAMIC_DRAW);
                glBindBufferBase(GL_ATOMIC_COUNTER_BUFFER, binding, buffer);
                glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, 0);
                m_buffers.push_back(buffer);
                return buffer;
            }

            std::vector<unsigned int> ReadCounters(GLuint buffer, int count) {
                std::vector<unsigned int> values(static_cast<std::size_t>(count), 0xDEADBEEFu);
                glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, buffer);
                glGetBufferSubData(GL_ATOMIC_COUNTER_BUFFER, 0,
                                   static_cast<GLsizeiptr>(values.size() * sizeof(unsigned int)), values.data());
                glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, 0);
                return values;
            }

            void Dispatch() {
                glUseProgram(m_program);
                glDispatchCompute(kWorkGroups, 1, 1);
                glMemoryBarrier(GL_ATOMIC_COUNTER_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
            }

            unsigned int m_program = 0;
            std::string m_buildLog;
            std::vector<GLuint> m_buffers;
        };

    } // namespace

    // The counter values a dispatch leaves behind, per binding point and per offset within one
    // binding. Nothing in the ES backend used to touch BufferTarget::AtomicCounter at all, so
    // before the wiring landed every one of these read back its seed unchanged.
    TEST_F(AtomicCounterScenario, DispatchIncrementsTheBoundCounterBuffers) {
        if (!Ready() || IsSkipped()) return;

        const GLuint zero = MakeCounterBuffer(0, {kSeedFirst, kSeedSecond});
        const GLuint one = MakeCounterBuffer(1, {kSeedOther});
        ASSERT_EQ(FirstGLError(), 0u) << "binding the counter buffers raised a GL error";

        Dispatch();
        EXPECT_EQ(FirstGLError(), 0u) << "the dispatch raised a GL error";

        const std::vector<unsigned int> zeroValues = ReadCounters(zero, 2);
        const std::vector<unsigned int> oneValues = ReadCounters(one, 1);
        EXPECT_EQ(FirstGLError(), 0u) << "reading the counters back raised a GL error";

        EXPECT_EQ(zeroValues[0], kSeedFirst + kInvocations)
            << "binding 0 offset 0 read back " << zeroValues[0] << "; " << kSeedFirst
            << " means the shader's increments never reached the buffer the application bound";
        EXPECT_EQ(zeroValues[1], kSeedSecond + 2 * kInvocations)
            << "binding 0 offset 4 read back " << zeroValues[1] << "; the seed means the counter at a NON-ZERO "
            << "offset was not carried through the lowering, even though offset 0 was";
        EXPECT_EQ(oneValues[0], kSeedOther + kInvocations)
            << "binding 1 read back " << oneValues[0] << "; a counter buffer past the first binding point "
            << "resolves to a different reserved slot and is where an off-by-one shows up";
    }

    // A second dispatch continues from where the first left off, and a re-seed between them is
    // visible to the shader. Both halves of the buffer's traffic have to work, in both
    // directions: the increments are only observable through the readback path, and the re-seed
    // is only observable if the upload reaches the driver AFTER the buffer has been GPU-written.
    TEST_F(AtomicCounterScenario, CountersAccumulateAcrossDispatchesAndFollowAReseed) {
        if (!Ready() || IsSkipped()) return;

        const GLuint zero = MakeCounterBuffer(0, {0u, 0u});
        MakeCounterBuffer(1, {0u});
        ASSERT_EQ(FirstGLError(), 0u);

        Dispatch();
        Dispatch();
        std::vector<unsigned int> values = ReadCounters(zero, 2);
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_EQ(values[0], 2 * kInvocations) << "two dispatches did not accumulate";
        EXPECT_EQ(values[1], 4 * kInvocations) << "two dispatches did not accumulate at offset 4";

        const unsigned int reseed[2] = {1000u, 2000u};
        glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, zero);
        glBufferSubData(GL_ATOMIC_COUNTER_BUFFER, 0, sizeof(reseed), reseed);
        glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, 0);
        ASSERT_EQ(FirstGLError(), 0u) << "re-seeding the counter buffer raised a GL error";

        Dispatch();
        values = ReadCounters(zero, 2);
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_EQ(values[0], reseed[0] + kInvocations) << "the re-seeded value did not reach the shader";
        EXPECT_EQ(values[1], reseed[1] + 2 * kInvocations) << "the re-seeded value at offset 4 did not reach the shader";
    }

    // A CPU glBufferSubData issued AFTER a dispatch, read back with NO further GPU work in
    // between. Each backend has its own way to invert this pair, and both are pinned here.
    // DirectGLES queues app SubData ranges for the draw-time staged-copy flush (the upload
    // ring) instead of uploading in place, and readback of a GPU-written buffer overwrites
    // the frontend shadow with the driver copy - so if the readback path forgets to flush the
    // queued range first, the newer CPU write is REVERTED by the readback and offset 0 reads
    // the dispatch's value instead of the reseed. DirectVulkan adopts the buffer into
    // coherent GPU memory the moment the dispatch resolves its descriptor, so the SubData
    // write lands in the very bytes the GPU reads - while the dispatch still sits recorded in
    // the deferred frame command buffer. Unless the frontend retires that pending work before
    // writing the adopted store (BufferObject::UploadSubData), the dispatch executes ON TOP
    // of the reseed and offset 0 reads reseed + increments instead of the reseed. Offset 4
    // pins the other direction for both: the upload must leave bytes outside its range - the
    // dispatch's results - untouched.
    TEST_F(AtomicCounterScenario, SubDataAfterDispatchSurvivesAnImmediateReadback) {
        if (!Ready() || IsSkipped()) return;

        const GLuint zero = MakeCounterBuffer(0, {0u, 0u});
        MakeCounterBuffer(1, {0u});
        ASSERT_EQ(FirstGLError(), 0u);

        Dispatch();

        const unsigned int reseed = 4242u;
        glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, zero);
        glBufferSubData(GL_ATOMIC_COUNTER_BUFFER, 0, sizeof(reseed), &reseed);
        glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, 0);
        ASSERT_EQ(FirstGLError(), 0u) << "re-seeding the counter buffer raised a GL error";

        const std::vector<unsigned int> values = ReadCounters(zero, 2);
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_EQ(values[0], reseed)
            << "offset 0 read back " << values[0] << "; the dispatch's value (" << kInvocations
            << ") means the readback ran before the queued SubData range was flushed and reverted it";
        EXPECT_EQ(values[1], 2 * kInvocations)
            << "offset 4 read back " << values[1] << "; the SubData flush must leave bytes outside its "
            << "range untouched";
    }

} // namespace MGITest
