// MobileGL - MobileGL/MG_Test/SelfTest/PersistentBufferOrderingProbeTest.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include <gtest/gtest.h>
#include <MG_Util/SelfTest/PersistentBufferOrderingProbe.h>

#include <map>
#include <set>

using namespace MobileGL;
using namespace MobileGL::MG_Util::SelfTest;

namespace {
    // Deferred vertex fetch, not canned ReadPixels answers: a broken mapped destination
    // reads its current bytes at Finish instead of the bytes at DrawArrays. ReadPixels also
    // drains these jobs, so inserting an early readback into the probe hides the bug here too.
    struct FakeDriver {
        struct Buffer {
            Bool mapped = false;
            Bool arena = false;
            Bool copied = false;
            Int channel = 0;
            Vector<Uint8> staging;
        };
        struct Draw { GLuint fbo, buffer; Int channel; Bool late; };
        std::map<GLuint, Buffer> buffers;
        std::map<GLuint, GLuint> vaoBuffers;
        std::map<GLuint, Int> colors;
        Vector<Draw> draws;
        std::set<GLuint> live;
        std::map<GLenum, GLint> state = {
            {GL_CURRENT_PROGRAM, 1}, {GL_VERTEX_ARRAY_BINDING, 2}, {GL_ARRAY_BUFFER, 3},
            {GL_COPY_READ_BUFFER, 4}, {GL_COPY_WRITE_BUFFER, 5},
            {GL_DRAW_FRAMEBUFFER_BINDING, 6}, {GL_READ_FRAMEBUFFER_BINDING, 7},
            {GL_TEXTURE_BINDING_2D, 8}, {GL_PIXEL_PACK_BUFFER, 9},
            {GL_PACK_ALIGNMENT, 8}, {GL_PACK_ROW_LENGTH, 31},
            {GL_PACK_SKIP_PIXELS, 4}, {GL_PACK_SKIP_ROWS, 5}};
        std::map<GLenum, GLboolean> enabled = {{GL_BLEND, GL_TRUE}, {GL_SCISSOR_TEST, GL_TRUE},
            {GL_SAMPLE_MASK, GL_TRUE}, {GL_RASTERIZER_DISCARD, GL_TRUE}};
        std::array<GLint, 4> viewport = {3, 4, 5, 6};
        std::array<GLfloat, 4> clear = {.25f, .5f, .75f, 0};
        std::array<GLboolean, 4> mask = {GL_FALSE, GL_TRUE, GL_FALSE, GL_TRUE};
        GLuint next = 100;
        GLenum error = GL_NO_ERROR;
        Bool extension = true, corruptSubData = false, corruptCopy = false;
        Bool corruptUnmapped = false, corruptSerialized = false;
        Bool failMap = false, failAllocation = false, failFramebuffer = false;
        Int failReadbackAt = 0, readbacks = 0;
        Uint arenaAllocations = 0;
        Uint8 pointerSentinel = 0;

        GLuint Create() { live.insert(next); return next++; }
        void Generate(GLsizei count, GLuint* ids) { for (Int i = 0; i < count; ++i) ids[i] = Create(); }
        void Delete(GLsizei count, const GLuint* ids) {
            for (Int i = 0; i < count; ++i) {
                live.erase(ids[i]);
                buffers.erase(ids[i]);
            }
        }
        GLint Get(GLenum name) const {
            const auto found = state.find(name);
            return found == state.end() ? 0 : found->second;
        }
        static Int Channel(const void* data) {
            GLfloat color[3];
            std::memcpy(color, static_cast<const Uint8*>(data) + 2 * sizeof(GLfloat), sizeof(color));
            return color[0] > .5f ? 0 : color[1] > .5f ? 1 : 2;
        }
        void Finish() {
            for (const auto& draw : draws) {
                const auto& buffer = buffers.at(draw.buffer);
                colors[draw.fbo] = draw.late ? buffer.channel : draw.channel;
                if (buffer.mapped && corruptSerialized) colors[draw.fbo] = (draw.channel + 1) % 3;
            }
            draws.clear();
        }
    } driver;

    MG_External::GLESFunctionsTable Table() {
        MG_External::GLESFunctionsTable gl{};
        gl.glGetIntegerv = [](GLenum name, GLint* out) {
            if (name == GL_MAJOR_VERSION) *out = 3;
            else if (name == GL_MINOR_VERSION) *out = 2;
            else if (name == GL_NUM_EXTENSIONS) *out = driver.extension ? 1 : 0;
            else if (name == GL_VIEWPORT) std::copy(driver.viewport.begin(), driver.viewport.end(), out);
            else if (name == GL_ARRAY_BUFFER_BINDING) *out = driver.Get(GL_ARRAY_BUFFER);
            else if (name == GL_PIXEL_PACK_BUFFER_BINDING) *out = driver.Get(GL_PIXEL_PACK_BUFFER);
            else *out = driver.Get(name);
        };
        gl.glGetBooleanv = [](GLenum, GLboolean* out) { std::copy(driver.mask.begin(), driver.mask.end(), out); };
        gl.glGetFloatv = [](GLenum, GLfloat* out) { std::copy(driver.clear.begin(), driver.clear.end(), out); };
        gl.glGetStringi = [](GLenum, GLuint) { return reinterpret_cast<const GLubyte*>("GL_EXT_buffer_storage"); };
        gl.glGetError = []() { return std::exchange(driver.error, GL_NO_ERROR); };
        gl.glIsEnabled = [](GLenum name) -> GLboolean { return driver.enabled[name]; };
        gl.glEnable = [](GLenum name) { driver.enabled[name] = GL_TRUE; };
        gl.glDisable = [](GLenum name) { driver.enabled[name] = GL_FALSE; };
        gl.glCreateShader = [](GLenum) { return driver.Create(); };
        gl.glShaderSource = [](GLuint, GLsizei, const GLchar* const*, const GLint*) {};
        gl.glCompileShader = [](GLuint) {};
        gl.glGetShaderiv = [](GLuint, GLenum, GLint* out) { *out = GL_TRUE; };
        gl.glGetShaderInfoLog = [](GLuint, GLsizei, GLsizei*, GLchar* out) { *out = 0; };
        gl.glDeleteShader = [](GLuint id) { driver.Delete(1, &id); };
        gl.glCreateProgram = []() { return driver.Create(); };
        gl.glAttachShader = [](GLuint, GLuint) {};
        gl.glLinkProgram = [](GLuint) {};
        gl.glGetProgramiv = [](GLuint, GLenum, GLint* out) { *out = GL_TRUE; };
        gl.glGetProgramInfoLog = gl.glGetShaderInfoLog;
        gl.glDeleteProgram = gl.glDeleteShader;
        gl.glUseProgram = [](GLuint id) { driver.state[GL_CURRENT_PROGRAM] = id; };
        gl.glGenBuffers = [](GLsizei count, GLuint* ids) { driver.Generate(count, ids); };
        gl.glBindBuffer = [](GLenum target, GLuint id) { driver.state[target] = id; };
        gl.glBufferStorageEXT = [](GLenum target, GLsizeiptr size, const void*, GLbitfield) {
            auto& buffer = driver.buffers[driver.Get(target)];
            buffer.arena = size >= 16 * 1024 * 1024;
            if (buffer.arena) ++driver.arenaAllocations;
            if (driver.failAllocation) driver.error = GL_OUT_OF_MEMORY;
            if (!buffer.arena) buffer.staging.resize(size);
        };
        gl.glBufferData = [](GLenum target, GLsizeiptr size, const void*, GLenum) {
            driver.buffers[driver.Get(target)].staging.resize(size);
        };
        gl.glMapBufferRange = [](GLenum target, GLintptr offset, GLsizeiptr, GLbitfield) -> void* {
            if (driver.failMap) return nullptr;
            auto& buffer = driver.buffers[driver.Get(target)];
            buffer.mapped = true;
            return buffer.arena ? &driver.pointerSentinel : buffer.staging.data() + offset;
        };
        gl.glUnmapBuffer = [](GLenum) -> GLboolean { return GL_TRUE; }; // Preserve allocation history.
        gl.glBufferSubData = [](GLenum target, GLintptr offset, GLsizeiptr size, const void* data) {
            auto& buffer = driver.buffers[driver.Get(target)];
            if (buffer.arena) {
                buffer.channel = FakeDriver::Channel(data);
                buffer.copied = false;
            } else std::memcpy(buffer.staging.data() + offset, data, size);
        };
        gl.glCopyBufferSubData = [](GLenum read, GLenum write, GLintptr offset, GLintptr, GLsizeiptr) {
            auto& source = driver.buffers[driver.Get(read)];
            auto& dest = driver.buffers[driver.Get(write)];
            dest.channel = FakeDriver::Channel(source.staging.data() + offset);
            dest.copied = true;
        };
        gl.glDeleteBuffers = [](GLsizei count, const GLuint* ids) { driver.Delete(count, ids); };
        gl.glGenVertexArrays = gl.glGenBuffers;
        gl.glBindVertexArray = [](GLuint id) { driver.state[GL_VERTEX_ARRAY_BINDING] = id; };
        gl.glVertexAttribPointer = [](GLuint, GLint, GLenum, GLboolean, GLsizei, const void*) {
            driver.vaoBuffers[driver.Get(GL_VERTEX_ARRAY_BINDING)] = driver.Get(GL_ARRAY_BUFFER);
        };
        gl.glEnableVertexAttribArray = [](GLuint) {};
        gl.glDeleteVertexArrays = gl.glDeleteBuffers;
        gl.glGenTextures = gl.glGenBuffers;
        gl.glBindTexture = [](GLenum, GLuint id) { driver.state[GL_TEXTURE_BINDING_2D] = id; };
        gl.glTexStorage2D = [](GLenum, GLsizei, GLenum, GLsizei, GLsizei) {};
        gl.glDeleteTextures = gl.glDeleteBuffers;
        gl.glGenFramebuffers = gl.glGenBuffers;
        gl.glBindFramebuffer = [](GLenum target, GLuint id) {
            if (target != GL_READ_FRAMEBUFFER) driver.state[GL_DRAW_FRAMEBUFFER_BINDING] = id;
            if (target != GL_DRAW_FRAMEBUFFER) driver.state[GL_READ_FRAMEBUFFER_BINDING] = id;
        };
        gl.glFramebufferTexture2D = [](GLenum, GLenum, GLenum, GLuint, GLint) {};
        gl.glCheckFramebufferStatus = [](GLenum) -> GLenum {
            return driver.failFramebuffer ? GL_FRAMEBUFFER_UNSUPPORTED : GL_FRAMEBUFFER_COMPLETE;
        };
        gl.glDeleteFramebuffers = gl.glDeleteBuffers;
        gl.glViewport = [](GLint x, GLint y, GLsizei w, GLsizei h) { driver.viewport = {x, y, w, h}; };
        gl.glColorMask = [](GLboolean r, GLboolean g, GLboolean b, GLboolean a) { driver.mask = {r, g, b, a}; };
        gl.glClearColor = [](GLfloat r, GLfloat g, GLfloat b, GLfloat a) { driver.clear = {r, g, b, a}; };
        gl.glClear = [](GLbitfield) {};
        gl.glDrawArrays = [](GLenum, GLint, GLsizei) {
            const GLuint id = driver.vaoBuffers.at(driver.Get(GL_VERTEX_ARRAY_BINDING));
            const auto& buffer = driver.buffers.at(id);
            const Bool late = buffer.mapped ? (buffer.copied ? driver.corruptCopy : driver.corruptSubData)
                                            : driver.corruptUnmapped;
            driver.draws.push_back({GLuint(driver.Get(GL_DRAW_FRAMEBUFFER_BINDING)), id, buffer.channel, late});
        };
        gl.glFinish = []() { driver.Finish(); };
        gl.glMemoryBarrier = [](GLbitfield) {};
        gl.glPixelStorei = [](GLenum name, GLint value) { driver.state[name] = value; };
        gl.glReadPixels = [](GLint, GLint, GLsizei width, GLsizei height, GLenum, GLenum, void* data) {
            driver.Finish(); // Models the implicit wait that must NOT occur between subject draws.
            if (++driver.readbacks == driver.failReadbackAt) {
                driver.error = GL_INVALID_OPERATION;
                return;
            }
            EXPECT_EQ(driver.Get(GL_PIXEL_PACK_BUFFER), 0);
            EXPECT_EQ(driver.Get(GL_PACK_ROW_LENGTH), 0);
            const Int channel = driver.colors.at(driver.Get(GL_READ_FRAMEBUFFER_BINDING));
            auto* pixels = static_cast<Uint8*>(data);
            for (Int i = 0; i < width * height; ++i)
                for (Int c = 0; c < 4; ++c) pixels[4 * i + c] = c == channel || c == 3 ? 255 : 0;
        };
        return gl;
    }

    class PersistentBufferOrderingProbeTest : public ::testing::Test {
    protected:
        void SetUp() override { driver = FakeDriver{}; }
        void TearDown() override { EXPECT_TRUE(driver.live.empty()); EXPECT_TRUE(driver.draws.empty()); }
    };
}

TEST_F(PersistentBufferOrderingProbeTest, RequiresExtensionAndCompleteDispatchBeforeAllocating) {
    auto gl = Table();
    driver.extension = false;
    EXPECT_FALSE(ProbePersistentBufferUpdateOrdering(gl).supported);
    driver.extension = true;
    gl.glCopyBufferSubData = nullptr;
    EXPECT_FALSE(ProbePersistentBufferUpdateOrdering(gl).supported);
    EXPECT_EQ(driver.arenaAllocations, 0u);
}

TEST_F(PersistentBufferOrderingProbeTest, OrderedDriverPassesAllUploadsAndRestoresCallerState) {
    const auto saved = driver;
    const auto measurement = ProbePersistentBufferUpdateOrdering(Table());
    ASSERT_TRUE(measurement.supported);
    for (const auto& row : measurement.uploads) {
        EXPECT_TRUE(row.unmapped.Passed());
        EXPECT_TRUE(row.mapped.Passed());
        EXPECT_EQ(row.mapped.frames, 240u); // Three fresh attempts before a negative result.
        EXPECT_EQ(row.finishBoth.status, BufferOrderingProbeStatus::NotRun);
    }
    EXPECT_FALSE(DescribePersistentBufferOrderingBug(measurement));
    EXPECT_EQ(driver.state, saved.state);
    EXPECT_EQ(driver.viewport, saved.viewport);
    EXPECT_EQ(driver.clear, saved.clear);
    EXPECT_EQ(driver.mask, saved.mask);
    for (const auto& [cap, value] : driver.enabled) {
        const auto found = saved.enabled.find(cap);
        EXPECT_EQ(value, found == saved.enabled.end() ? GL_FALSE : found->second);
    }
}

TEST_F(PersistentBufferOrderingProbeTest, DeferredMappedSubDataFetchIsDetectedWithPassingControls) {
    driver.corruptSubData = true;
    const auto measurement = ProbePersistentBufferUpdateOrdering(Table());
    EXPECT_TRUE(measurement.uploads[0].Detected());
    EXPECT_TRUE(measurement.uploads[0].finishBefore.Passed());
    EXPECT_GT(measurement.uploads[0].mapThenUnmap.badFrames, 0u);
    EXPECT_GT(measurement.uploads[0].barrierBefore.badFrames, 0u);
    EXPECT_FALSE(measurement.uploads[1].Detected());
    EXPECT_FALSE(measurement.uploads[2].Detected());
    const auto finding = DescribePersistentBufferOrderingBug(measurement);
    ASSERT_TRUE(finding);
    EXPECT_EQ(finding->verdict, DriverBugVerdict::Unfixable);
    EXPECT_NE(finding->detail.find("SubData:"), String::npos);
    EXPECT_NE(finding->detail.find("MOBILEGL_DISABLE_LARGE_BUFFER_ADOPTION=1"), String::npos);
}

TEST_F(PersistentBufferOrderingProbeTest, DeferredCopyFetchIsDetectedWithBothStagingSources) {
    driver.corruptCopy = true;
    const auto measurement = ProbePersistentBufferUpdateOrdering(Table());
    EXPECT_FALSE(measurement.uploads[0].Detected());
    EXPECT_TRUE(measurement.uploads[1].Detected());
    EXPECT_TRUE(measurement.uploads[2].Detected());
}

TEST_F(PersistentBufferOrderingProbeTest, PostCollectorIncludesTheMeasuredFinding) {
    driver.corruptSubData = true;
    const auto findings = CollectGlesKnownDriverBugs(Table());
    const auto found = std::find_if(findings.begin(), findings.end(), [](const auto& finding) {
        return finding.name == "Persistent-mapped vertex buffers lose upload/draw ordering";
    });
    ASSERT_NE(found, findings.end());
    EXPECT_NE(found->detail.find("never-mapped 0/80"), String::npos);
    EXPECT_EQ(found->verdict, DriverBugVerdict::Unfixable);
}

TEST_F(PersistentBufferOrderingProbeTest, CorruptNeverMappedControlCannotAccusePersistentMapping) {
    driver.corruptSubData = driver.corruptCopy = driver.corruptUnmapped = true;
    const auto measurement = ProbePersistentBufferUpdateOrdering(Table());
    for (const auto& row : measurement.uploads) {
        EXPECT_GT(row.unmapped.badFrames, 0u);
        EXPECT_EQ(row.mapped.status, BufferOrderingProbeStatus::NotRun);
    }
    EXPECT_FALSE(DescribePersistentBufferOrderingBug(measurement));
}

TEST_F(PersistentBufferOrderingProbeTest, CorruptSerializedControlCannotConfirmOrderingDefect) {
    driver.corruptSubData = driver.corruptCopy = driver.corruptSerialized = true;
    const auto measurement = ProbePersistentBufferUpdateOrdering(Table());
    for (const auto& row : measurement.uploads) EXPECT_GT(row.finishBoth.badFrames, 0u);
    EXPECT_FALSE(DescribePersistentBufferOrderingBug(measurement));
}

TEST_F(PersistentBufferOrderingProbeTest, FailedMappingIsInconclusiveAndReleasesResources) {
    driver.failMap = true;
    const auto measurement = ProbePersistentBufferUpdateOrdering(Table());
    EXPECT_EQ(measurement.uploads[0].mapped.status, BufferOrderingProbeStatus::Failed);
    EXPECT_EQ(measurement.uploads[1].unmapped.status, BufferOrderingProbeStatus::Failed);
    EXPECT_FALSE(DescribePersistentBufferOrderingBug(measurement));
}

TEST_F(PersistentBufferOrderingProbeTest, AllocationFailureIsInconclusiveAndRestoresBindings) {
    driver.failAllocation = true;
    const auto saved = driver.state;
    const auto measurement = ProbePersistentBufferUpdateOrdering(Table());
    for (const auto& row : measurement.uploads) {
        EXPECT_EQ(row.unmapped.status, BufferOrderingProbeStatus::Failed);
        EXPECT_EQ(row.unmapped.error, GLenum(GL_OUT_OF_MEMORY));
    }
    EXPECT_FALSE(DescribePersistentBufferOrderingBug(measurement));
    EXPECT_EQ(driver.state, saved);
}

TEST_F(PersistentBufferOrderingProbeTest, ReadbackErrorAfterAMismatchDoesNotProduceAFinding) {
    driver.corruptSubData = true;
    driver.failReadbackAt = 82; // Eighty clean control readbacks, then one corrupt subject FBO.
    const auto measurement = ProbePersistentBufferUpdateOrdering(Table());
    EXPECT_GT(measurement.uploads[0].mapped.badFrames, 0u);
    EXPECT_EQ(measurement.uploads[0].mapped.status, BufferOrderingProbeStatus::Failed);
    EXPECT_EQ(measurement.uploads[0].mapped.error, GLenum(GL_INVALID_OPERATION));
    EXPECT_FALSE(DescribePersistentBufferOrderingBug(measurement));
}

TEST_F(PersistentBufferOrderingProbeTest, IncompleteFramebufferIsInconclusiveAndRestoresBindings) {
    driver.failFramebuffer = true;
    const auto saved = driver.state;
    const auto measurement = ProbePersistentBufferUpdateOrdering(Table());
    EXPECT_FALSE(DescribePersistentBufferOrderingBug(measurement));
    EXPECT_EQ(driver.arenaAllocations, 0u);
    EXPECT_EQ(driver.state, saved);
}
