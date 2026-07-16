// MobileGL - MobileGL/MG_Test/BackendLoader/BackendLoaderTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include <gtest/gtest.h>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <MG_Util/BackendLoaders/OpenGL/Loader.h>

// ProbeIndirectInstanceIdIncludesBaseInstance is driven against a fake GLES driver:
// a GLESFunctionsTable populated with captureless lambdas backed by the file-scope
// state below (buffer stores, bound targets, always-succeeding compile/link). Each
// test configures the fake's draw behavior to emulate a conforming driver, an
// ANGLE-style baseInstance-leaking driver, or a failing one.
namespace {
    struct FakeDriverState {
        // Behavior knobs, configured per test before running the probe.
        GLint maxVertexSsboBlocks = 4;
        // Emulates ANGLE-on-Vulkan: the draw reads the indirect command's
        // baseInstance word and exposes it through gl_InstanceID.
        bool drawLeaksBaseInstanceWord = false;
        GLenum errorRaisedByDraw = GL_NO_ERROR;

        GLenum pendingError = GL_NO_ERROR;
        std::vector<std::string> extensions;

        GLuint nextBufferId = 1;
        GLuint nextShaderId = 1;
        GLuint nextProgramId = 1;
        GLuint nextVertexArrayId = 1;
        GLuint nextFramebufferId = 1;
        GLuint nextRenderbufferId = 1;

        std::map<GLuint, std::vector<unsigned char>> bufferStores; // buffer id -> data store
        std::map<GLenum, GLuint> boundBuffers;                     // target -> buffer id
        std::map<GLuint, GLuint> boundSsboBases;                   // SSBO binding index -> buffer id

        int createdShaders = 0;
        int createdPrograms = 0;
        int createdBuffers = 0;
        int createdVertexArrays = 0;
        int createdFramebuffers = 0;
        int createdRenderbuffers = 0;
        int aliveShaders = 0;
        int alivePrograms = 0;
        int aliveBuffers = 0;
        int aliveVertexArrays = 0;
        int aliveFramebuffers = 0;
        int aliveRenderbuffers = 0;

        bool drawIssued = false;
    };

    FakeDriverState g_fake;

    void ResetFakeDriver() { g_fake = FakeDriverState{}; }

    std::vector<unsigned char>* StoreOfBufferBoundTo(GLenum target) {
        const auto boundIt = g_fake.boundBuffers.find(target);
        if (boundIt == g_fake.boundBuffers.end() || boundIt->second == 0) {
            return nullptr;
        }
        const auto storeIt = g_fake.bufferStores.find(boundIt->second);
        return storeIt != g_fake.bufferStores.end() ? &storeIt->second : nullptr;
    }

    MobileGL::MG_External::GLESFunctionsTable MakeFakeGLESFunctions() {
        MobileGL::MG_External::GLESFunctionsTable funcs{};

        funcs.glGetIntegerv = [](GLenum pname, GLint* data) {
            switch (pname) {
            case GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS:
                *data = g_fake.maxVertexSsboBlocks;
                break;
            // FillInGLESCapabilities reads the context version before running the
            // baseInstance probe, which requires ES >= 3.1.
            case GL_MAJOR_VERSION:
                *data = 3;
                break;
            case GL_MINOR_VERSION:
                *data = 1;
                break;
            case GL_NUM_EXTENSIONS:
                *data = static_cast<GLint>(g_fake.extensions.size());
                break;
            default:
                // Leave the caller's defaults for every other capability query.
                break;
            }
        };
        funcs.glGetError = []() -> GLenum {
            const GLenum error = g_fake.pendingError;
            g_fake.pendingError = GL_NO_ERROR;
            return error;
        };

        // String and float queries used by FillInGLESCapabilities.
        funcs.glGetString = [](GLenum name) -> const GLubyte* {
            switch (name) {
            case GL_VENDOR:
                return reinterpret_cast<const GLubyte*>("MobileGL Fake Vendor");
            case GL_RENDERER:
                return reinterpret_cast<const GLubyte*>("MobileGL Fake Renderer");
            case GL_VERSION:
                return reinterpret_cast<const GLubyte*>("OpenGL ES 3.1 (MobileGL fake)");
            case GL_SHADING_LANGUAGE_VERSION:
                return reinterpret_cast<const GLubyte*>("OpenGL ES GLSL ES 3.10 (MobileGL fake)");
            default:
                return reinterpret_cast<const GLubyte*>("");
            }
        };
        funcs.glGetStringi = [](GLenum name, GLuint index) -> const GLubyte* {
            if (name != GL_EXTENSIONS || index >= g_fake.extensions.size()) return nullptr;
            return reinterpret_cast<const GLubyte*>(g_fake.extensions[index].c_str());
        };
        funcs.glGetFloatv = [](GLenum pname, GLfloat* data) {
            switch (pname) {
            // Two-component range queries.
            case GL_ALIASED_LINE_WIDTH_RANGE:
            case GL_SMOOTH_LINE_WIDTH_RANGE:
            case GL_ALIASED_POINT_SIZE_RANGE:
            case GL_VIEWPORT_BOUNDS_RANGE:
                data[0] = 0.0f;
                data[1] = 0.0f;
                break;
            default:
                data[0] = 0.0f;
                break;
            }
        };

        // Shader and program objects: compile/link always succeed.
        funcs.glCreateShader = [](GLenum) -> GLuint {
            ++g_fake.createdShaders;
            ++g_fake.aliveShaders;
            return g_fake.nextShaderId++;
        };
        funcs.glShaderSource = [](GLuint, GLsizei, const GLchar* const*, const GLint*) {};
        funcs.glCompileShader = [](GLuint) {};
        funcs.glGetShaderiv = [](GLuint, GLenum pname, GLint* params) {
            if (pname == GL_COMPILE_STATUS) {
                *params = GL_TRUE;
            }
        };
        funcs.glDeleteShader = [](GLuint shader) {
            if (shader != 0) {
                --g_fake.aliveShaders;
            }
        };
        funcs.glCreateProgram = []() -> GLuint {
            ++g_fake.createdPrograms;
            ++g_fake.alivePrograms;
            return g_fake.nextProgramId++;
        };
        funcs.glAttachShader = [](GLuint, GLuint) {};
        funcs.glLinkProgram = [](GLuint) {};
        funcs.glGetProgramiv = [](GLuint, GLenum pname, GLint* params) {
            if (pname == GL_LINK_STATUS) {
                *params = GL_TRUE;
            }
        };
        funcs.glDeleteProgram = [](GLuint program) {
            if (program != 0) {
                --g_fake.alivePrograms;
            }
        };
        funcs.glUseProgram = [](GLuint) {};

        // Buffer objects with byte-accurate data stores.
        funcs.glGenBuffers = [](GLsizei n, GLuint* buffers) {
            for (GLsizei i = 0; i < n; ++i) {
                buffers[i] = g_fake.nextBufferId++;
                ++g_fake.createdBuffers;
                ++g_fake.aliveBuffers;
            }
        };
        funcs.glBindBuffer = [](GLenum target, GLuint buffer) { g_fake.boundBuffers[target] = buffer; };
        funcs.glBufferData = [](GLenum target, GLsizeiptr size, const void* data, GLenum) {
            const GLuint bound = g_fake.boundBuffers[target];
            if (bound == 0) {
                return;
            }
            auto& store = g_fake.bufferStores[bound];
            store.assign((std::size_t)size, 0);
            if (data != nullptr && size > 0) {
                std::memcpy(store.data(), data, (std::size_t)size);
            }
        };
        funcs.glBindBufferBase = [](GLenum target, GLuint index, GLuint buffer) {
            if (target == GL_SHADER_STORAGE_BUFFER) {
                g_fake.boundSsboBases[index] = buffer;
            }
        };
        funcs.glDeleteBuffers = [](GLsizei n, const GLuint* buffers) {
            for (GLsizei i = 0; i < n; ++i) {
                if (buffers[i] != 0) {
                    --g_fake.aliveBuffers;
                    g_fake.bufferStores.erase(buffers[i]);
                }
            }
        };
        funcs.glMapBufferRange = [](GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield) -> void* {
            auto* store = StoreOfBufferBoundTo(target);
            if (store == nullptr || offset < 0 || (std::size_t)(offset + length) > store->size()) {
                return nullptr;
            }
            return store->data() + offset;
        };
        funcs.glUnmapBuffer = [](GLenum) -> GLboolean { return GL_TRUE; };

        // Vertex array objects.
        funcs.glGenVertexArrays = [](GLsizei n, GLuint* arrays) {
            for (GLsizei i = 0; i < n; ++i) {
                arrays[i] = g_fake.nextVertexArrayId++;
                ++g_fake.createdVertexArrays;
                ++g_fake.aliveVertexArrays;
            }
        };
        funcs.glBindVertexArray = [](GLuint) {};
        funcs.glDeleteVertexArrays = [](GLsizei n, const GLuint* arrays) {
            for (GLsizei i = 0; i < n; ++i) {
                if (arrays[i] != 0) {
                    --g_fake.aliveVertexArrays;
                }
            }
        };

        // Framebuffer/renderbuffer objects for the probe's 1x1 draw target.
        funcs.glGenFramebuffers = [](GLsizei n, GLuint* framebuffers) {
            for (GLsizei i = 0; i < n; ++i) {
                framebuffers[i] = g_fake.nextFramebufferId++;
                ++g_fake.createdFramebuffers;
                ++g_fake.aliveFramebuffers;
            }
        };
        funcs.glGenRenderbuffers = [](GLsizei n, GLuint* renderbuffers) {
            for (GLsizei i = 0; i < n; ++i) {
                renderbuffers[i] = g_fake.nextRenderbufferId++;
                ++g_fake.createdRenderbuffers;
                ++g_fake.aliveRenderbuffers;
            }
        };
        funcs.glBindFramebuffer = [](GLenum, GLuint) {};
        funcs.glBindRenderbuffer = [](GLenum, GLuint) {};
        funcs.glRenderbufferStorage = [](GLenum, GLenum, GLsizei, GLsizei) {};
        funcs.glFramebufferRenderbuffer = [](GLenum, GLenum, GLenum, GLuint) {};
        funcs.glDeleteFramebuffers = [](GLsizei n, const GLuint* framebuffers) {
            for (GLsizei i = 0; i < n; ++i) {
                if (framebuffers[i] != 0) {
                    --g_fake.aliveFramebuffers;
                }
            }
        };
        funcs.glDeleteRenderbuffers = [](GLsizei n, const GLuint* renderbuffers) {
            for (GLsizei i = 0; i < n; ++i) {
                if (renderbuffers[i] != 0) {
                    --g_fake.aliveRenderbuffers;
                }
            }
        };

        funcs.glEnable = [](GLenum) {};
        funcs.glDisable = [](GLenum) {};
        funcs.glMemoryBarrier = [](GLbitfield) {};

        // The probe's vertex shader writes the gl_InstanceID it observed into the
        // result SSBO at binding 0. A conforming driver observes 0; a leaking one
        // observes the indirect command's baseInstance word (byte offset 12).
        funcs.glDrawArraysIndirect = [](GLenum, const void*) {
            g_fake.drawIssued = true;
            GLint observedInstanceId = 0;
            if (g_fake.drawLeaksBaseInstanceWord) {
                const auto* command = StoreOfBufferBoundTo(GL_DRAW_INDIRECT_BUFFER);
                if (command != nullptr && command->size() >= 16) {
                    GLuint baseInstance = 0;
                    std::memcpy(&baseInstance, command->data() + 12, sizeof(baseInstance));
                    observedInstanceId = (GLint)baseInstance;
                }
            }
            const auto resultIt = g_fake.boundSsboBases.find(0);
            if (resultIt != g_fake.boundSsboBases.end()) {
                const auto storeIt = g_fake.bufferStores.find(resultIt->second);
                if (storeIt != g_fake.bufferStores.end() && storeIt->second.size() >= sizeof(observedInstanceId)) {
                    std::memcpy(storeIt->second.data(), &observedInstanceId, sizeof(observedInstanceId));
                }
            }
            if (g_fake.errorRaisedByDraw != GL_NO_ERROR) {
                g_fake.pendingError = g_fake.errorRaisedByDraw;
            }
        };

        return funcs;
    }

    MobileGL::MG_External::GLESCapabilities MakeEs31Capabilities() {
        MobileGL::MG_External::GLESCapabilities caps;
        caps.GLESVersion = {3, 1, 0};
        return caps;
    }

    void ExpectProbeReleasedAllObjects() {
        EXPECT_GT(g_fake.createdShaders, 0);
        EXPECT_GT(g_fake.createdPrograms, 0);
        EXPECT_GT(g_fake.createdBuffers, 0);
        EXPECT_GT(g_fake.createdVertexArrays, 0);
        EXPECT_EQ(g_fake.aliveShaders, 0);
        EXPECT_EQ(g_fake.alivePrograms, 0);
        EXPECT_EQ(g_fake.aliveBuffers, 0);
        EXPECT_EQ(g_fake.aliveVertexArrays, 0);
        EXPECT_EQ(g_fake.aliveFramebuffers, 0);
        EXPECT_EQ(g_fake.aliveRenderbuffers, 0);
        EXPECT_TRUE(g_fake.bufferStores.empty());
    }
} // namespace

TEST(IndirectInstanceIdProbe, ConformingDriverReportsZeroBased) {
    ResetFakeDriver();
    const auto funcs = MakeFakeGLESFunctions();
    const auto caps = MakeEs31Capabilities();

    EXPECT_FALSE(MobileGL::MG_Util::BackendLoader::ProbeIndirectInstanceIdIncludesBaseInstance(caps, funcs));

    EXPECT_TRUE(g_fake.drawIssued);
    ExpectProbeReleasedAllObjects();
}

TEST(IndirectInstanceIdProbe, LeakingDriverReportsIncludesBase) {
    ResetFakeDriver();
    g_fake.drawLeaksBaseInstanceWord = true;
    const auto funcs = MakeFakeGLESFunctions();
    const auto caps = MakeEs31Capabilities();

    EXPECT_TRUE(MobileGL::MG_Util::BackendLoader::ProbeIndirectInstanceIdIncludesBaseInstance(caps, funcs));

    EXPECT_TRUE(g_fake.drawIssued);
    ExpectProbeReleasedAllObjects();
}

TEST(IndirectInstanceIdProbe, NoVertexSsboSkipsProbe) {
    ResetFakeDriver();
    g_fake.maxVertexSsboBlocks = 0;
    const auto funcs = MakeFakeGLESFunctions();
    const auto caps = MakeEs31Capabilities();

    EXPECT_FALSE(MobileGL::MG_Util::BackendLoader::ProbeIndirectInstanceIdIncludesBaseInstance(caps, funcs));

    EXPECT_FALSE(g_fake.drawIssued);
    EXPECT_EQ(g_fake.createdBuffers, 0);
    EXPECT_EQ(g_fake.createdPrograms, 0);
}

TEST(IndirectInstanceIdProbe, DrawErrorIsInconclusive) {
    ResetFakeDriver();
    // Even when the driver would leak baseInstance, a draw that raises a GL error
    // must leave the probe inconclusive (false) instead of trusting the result.
    g_fake.drawLeaksBaseInstanceWord = true;
    g_fake.errorRaisedByDraw = GL_INVALID_OPERATION;
    const auto funcs = MakeFakeGLESFunctions();
    const auto caps = MakeEs31Capabilities();

    EXPECT_FALSE(MobileGL::MG_Util::BackendLoader::ProbeIndirectInstanceIdIncludesBaseInstance(caps, funcs));

    EXPECT_TRUE(g_fake.drawIssued);
    ExpectProbeReleasedAllObjects();
}

// End-to-end through the real capability query: FillInGLESCapabilities must run the
// baseInstance probe against the driver it was handed and store the answer in
// caps.IndirectDrawInstanceIdIncludesBaseInstance (the single call site in Loader.cpp).
TEST(IndirectInstanceIdProbe, FillInCapabilitiesWiresProbeResult) {
    // Leaking fake: the probe's true result must land in the caps struct.
    ResetFakeDriver();
    g_fake.drawLeaksBaseInstanceWord = true;
    const auto funcs = MakeFakeGLESFunctions();

    MobileGL::MG_External::GLESCapabilities leakingCaps;
    ASSERT_TRUE(MobileGL::MG_Util::BackendLoader::FillInGLESCapabilities(leakingCaps, funcs));

    EXPECT_TRUE(g_fake.drawIssued);
    EXPECT_TRUE(leakingCaps.IndirectDrawInstanceIdIncludesBaseInstance);
    // The surrounding wiring came from the fake driver too.
    EXPECT_EQ(leakingCaps.GLESVersion.Major, 3);
    EXPECT_EQ(leakingCaps.GLESVersion.Minor, 1);
    EXPECT_EQ(leakingCaps.GLESVendorString, "MobileGL Fake Vendor");
    EXPECT_EQ(leakingCaps.GLESRendererString, "MobileGL Fake Renderer");
    EXPECT_EQ(leakingCaps.GLESVersionString, "OpenGL ES 3.1 (MobileGL fake)");
    EXPECT_EQ(leakingCaps.GLESShadingLanguageVersionString, "OpenGL ES GLSL ES 3.10 (MobileGL fake)");
    ExpectProbeReleasedAllObjects();

    // Conforming fake: the same call site must record false.
    ResetFakeDriver();
    MobileGL::MG_External::GLESCapabilities conformingCaps;
    ASSERT_TRUE(MobileGL::MG_Util::BackendLoader::FillInGLESCapabilities(conformingCaps, funcs));

    EXPECT_TRUE(g_fake.drawIssued);
    EXPECT_FALSE(conformingCaps.IndirectDrawInstanceIdIncludesBaseInstance);
    ExpectProbeReleasedAllObjects();
}

TEST(TextureAnisotropyCapabilities, ExtensionPresenceIsDetectedExactly) {
    ResetFakeDriver();
    g_fake.maxVertexSsboBlocks = 0;
    const auto funcs = MakeFakeGLESFunctions();

    MobileGL::MG_External::GLESCapabilities absentCaps;
    ASSERT_TRUE(MobileGL::MG_Util::BackendLoader::FillInGLESCapabilities(absentCaps, funcs));
    EXPECT_FALSE(absentCaps.SupportsTextureFilterAnisotropy);

    ResetFakeDriver();
    g_fake.maxVertexSsboBlocks = 0;
    g_fake.extensions.emplace_back("GL_EXT_texture_filter_anisotropic");
    MobileGL::MG_External::GLESCapabilities presentCaps;
    ASSERT_TRUE(MobileGL::MG_Util::BackendLoader::FillInGLESCapabilities(presentCaps, funcs));
    EXPECT_TRUE(presentCaps.SupportsTextureFilterAnisotropy);
}
