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

#include <algorithm>

#include <MG_Backend/DirectGLES/BackendObject_DirectGLES.h>
#include <MG_Backend/DirectVulkan/BackendObject_DirectVulkan.h>
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
        GLint glesMajorVersion = 3;
        GLint glesMinorVersion = 1;
        GLint maxVertexImageUniforms = 2;
        GLint maxGeometryImageUniforms = 3;
        GLint maxFragmentImageUniforms = 4;
        GLint maxComputeImageUniforms = 5;
        bool maxGeometryImageUniformsQueried = false;
        // Per-stage GL_MAX_*_SHADER_STORAGE_BLOCKS. The vertex and fragment pnames are ES 3.1,
        // but the tessellation and geometry ones only exist from ES 3.2 on, so asking for them
        // on an older context raises GL_INVALID_ENUM - the same shape as the buffer-texture and
        // anisotropy probes. The "queried" flags are what pin that gating; the "raises error"
        // knob is what pins the drain.
        GLint maxTessControlSsboBlocks = 6;
        GLint maxTessEvaluationSsboBlocks = 7;
        GLint maxGeometrySsboBlocks = 8;
        GLint maxFragmentSsboBlocks = 9;
        bool tessAndGeometrySsboBlocksQueried = false;
        bool perStageSsboBlockQueryRaisesError = false;
        // GL_MAX_CLIP_DISTANCES. Not ES core in any version - it exists only as
        // GL_MAX_CLIP_DISTANCES_EXT under GL_EXT_clip_cull_distance - so asking a driver without
        // the extension raises GL_INVALID_ENUM and leaves the out-param untouched. The "queried"
        // flag is what pins the gating; the "raises error" knob is what pins the drain.
        GLint maxClipDistances = 8;
        bool maxClipDistancesQueried = false;
        bool clipDistanceQueryRaisesError = false;
        GLfloat minFragmentInterpolationOffset = -0.75f;
        GLfloat maxFragmentInterpolationOffset = 0.625f;
        GLint fragmentInterpolationOffsetBits = 6;
        bool fragmentInterpolationLimitsQueried = false;
        bool fragmentInterpolationQueryRaisesError = false;
        // Emulates ANGLE-on-Vulkan: the draw reads the indirect command's
        // baseInstance word and exposes it through gl_InstanceID.
        bool drawLeaksBaseInstanceWord = false;
        GLenum errorRaisedByDraw = GL_NO_ERROR;

        GLenum pendingError = GL_NO_ERROR;
        std::vector<std::string> extensions;

        // GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT the fake reports, and whether it was ever asked:
        // querying it on a driver without the extension would raise GL_INVALID_ENUM.
        GLfloat maxTextureMaxAnisotropy = 16.0f;
        bool maxTextureMaxAnisotropyQueried = false;

        // Buffer textures. GL_MAX_TEXTURE_BUFFER_SIZE is only a legal pname once they exist, so
        // asking on a driver without them raises GL_INVALID_ENUM - the same shape as the
        // anisotropy probe above. The three entry-point knobs are separate because the
        // unsuffixed name is the ES 3.2 CORE spelling while an EXT/OES driver exports the
        // suffixed one: a resolver that only looks for the core name declares every extension
        // driver unsupported, which is exactly the bug these knobs exist to pin.
        GLint maxTextureBufferSize = 131072;
        bool maxTextureBufferSizeQueried = false;
        bool textureBufferSizeQueryRaisesError = false;
        bool hasCoreTexBufferEntryPoint = true;
        bool hasExtTexBufferEntryPoint = false;
        bool hasOesTexBufferEntryPoint = false;

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
                if (g_fake.perStageSsboBlockQueryRaisesError) {
                    g_fake.pendingError = GL_INVALID_ENUM;
                } else {
                    *data = g_fake.maxVertexSsboBlocks;
                }
                break;
            case GL_MAX_FRAGMENT_SHADER_STORAGE_BLOCKS:
                if (g_fake.perStageSsboBlockQueryRaisesError) {
                    g_fake.pendingError = GL_INVALID_ENUM;
                } else {
                    *data = g_fake.maxFragmentSsboBlocks;
                }
                break;
            case GL_MAX_TESS_CONTROL_SHADER_STORAGE_BLOCKS:
                g_fake.tessAndGeometrySsboBlocksQueried = true;
                *data = g_fake.maxTessControlSsboBlocks;
                break;
            case GL_MAX_TESS_EVALUATION_SHADER_STORAGE_BLOCKS:
                g_fake.tessAndGeometrySsboBlocksQueried = true;
                *data = g_fake.maxTessEvaluationSsboBlocks;
                break;
            case GL_MAX_GEOMETRY_SHADER_STORAGE_BLOCKS:
                g_fake.tessAndGeometrySsboBlocksQueried = true;
                *data = g_fake.maxGeometrySsboBlocks;
                break;
            case GL_MAX_VERTEX_IMAGE_UNIFORMS:
                *data = g_fake.maxVertexImageUniforms;
                break;
            case GL_MAX_GEOMETRY_IMAGE_UNIFORMS:
                g_fake.maxGeometryImageUniformsQueried = true;
                *data = g_fake.maxGeometryImageUniforms;
                break;
            case GL_MAX_FRAGMENT_IMAGE_UNIFORMS:
                *data = g_fake.maxFragmentImageUniforms;
                break;
            case GL_MAX_COMPUTE_IMAGE_UNIFORMS:
                *data = g_fake.maxComputeImageUniforms;
                break;
            case GL_MAX_CLIP_DISTANCES:
                g_fake.maxClipDistancesQueried = true;
                if (g_fake.clipDistanceQueryRaisesError) {
                    g_fake.pendingError = GL_INVALID_ENUM;
                } else {
                    *data = g_fake.maxClipDistances;
                }
                break;
            case GL_FRAGMENT_INTERPOLATION_OFFSET_BITS:
                g_fake.fragmentInterpolationLimitsQueried = true;
                if (g_fake.fragmentInterpolationQueryRaisesError) {
                    g_fake.pendingError = GL_INVALID_ENUM;
                } else {
                    *data = g_fake.fragmentInterpolationOffsetBits;
                }
                break;
            case GL_MAX_TEXTURE_BUFFER_SIZE:
                g_fake.maxTextureBufferSizeQueried = true;
                if (g_fake.textureBufferSizeQueryRaisesError) {
                    g_fake.pendingError = GL_INVALID_ENUM;
                } else {
                    *data = g_fake.maxTextureBufferSize;
                }
                break;
            // FillInGLESCapabilities reads the context version before running the
            // baseInstance probe, which requires ES >= 3.1.
            case GL_MAJOR_VERSION:
                *data = g_fake.glesMajorVersion;
                break;
            case GL_MINOR_VERSION:
                *data = g_fake.glesMinorVersion;
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
            case GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT:
                g_fake.maxTextureMaxAnisotropyQueried = true;
                data[0] = g_fake.maxTextureMaxAnisotropy;
                break;
            case GL_MIN_FRAGMENT_INTERPOLATION_OFFSET:
                g_fake.fragmentInterpolationLimitsQueried = true;
                if (g_fake.fragmentInterpolationQueryRaisesError) {
                    g_fake.pendingError = GL_INVALID_ENUM;
                } else {
                    data[0] = g_fake.minFragmentInterpolationOffset;
                }
                break;
            case GL_MAX_FRAGMENT_INTERPOLATION_OFFSET:
                g_fake.fragmentInterpolationLimitsQueried = true;
                if (g_fake.fragmentInterpolationQueryRaisesError) {
                    g_fake.pendingError = GL_INVALID_ENUM;
                } else {
                    data[0] = g_fake.maxFragmentInterpolationOffset;
                }
                break;
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

        // Buffer-texture entry points, each present only when its knob says so. A real loader
        // resolves the suffixed names only on a driver whose support is that extension.
        funcs.glTexBuffer = g_fake.hasCoreTexBufferEntryPoint
                                ? static_cast<MobileGL::MG_External::GLES::glTexBuffer_PTR>(
                                      [](GLenum, GLenum, GLuint) {})
                                : nullptr;
        funcs.glTexBufferEXT = g_fake.hasExtTexBufferEntryPoint
                                   ? static_cast<MobileGL::MG_External::GLES::glTexBufferEXT_PTR>(
                                         [](GLenum, GLenum, GLuint) {})
                                   : nullptr;
        funcs.glTexBufferOES = g_fake.hasOesTexBufferEntryPoint
                                   ? static_cast<MobileGL::MG_External::GLES::glTexBufferOES_PTR>(
                                         [](GLenum, GLenum, GLuint) {})
                                   : nullptr;

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
        // The probe reads its vertex storage-block gate from caps rather than re-querying the
        // driver (FillInGLESCapabilities resolves the per-stage limits before calling it), so a
        // caps struct handed to the probe directly has to carry what the fake reports.
        caps.MaxVertexShaderStorageBlocks = g_fake.maxVertexSsboBlocks;
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

TEST(ImageUniformCapabilities, QueriesRealPerStageLimitsAndConservativelyGatesGeometry) {
    const auto funcs = MakeFakeGLESFunctions();

    ResetFakeDriver();
    g_fake.maxVertexSsboBlocks = 0;
    MobileGL::MG_External::GLESCapabilities es31Caps;
    ASSERT_TRUE(MobileGL::MG_Util::BackendLoader::FillInGLESCapabilities(es31Caps, funcs));
    EXPECT_EQ(es31Caps.MaxVertexImageUniforms, g_fake.maxVertexImageUniforms);
    EXPECT_EQ(es31Caps.MaxGeometryImageUniforms, 0);
    EXPECT_EQ(es31Caps.MaxFragmentImageUniforms, g_fake.maxFragmentImageUniforms);
    EXPECT_EQ(es31Caps.MaxComputeImageUniforms, g_fake.maxComputeImageUniforms);
    EXPECT_FALSE(g_fake.maxGeometryImageUniformsQueried);

    ResetFakeDriver();
    g_fake.maxVertexSsboBlocks = 0;
    g_fake.glesMinorVersion = 2;
    MobileGL::MG_External::GLESCapabilities es32Caps;
    ASSERT_TRUE(MobileGL::MG_Util::BackendLoader::FillInGLESCapabilities(es32Caps, funcs));
    EXPECT_EQ(es32Caps.MaxVertexImageUniforms, g_fake.maxVertexImageUniforms);
    EXPECT_EQ(es32Caps.MaxGeometryImageUniforms, g_fake.maxGeometryImageUniforms);
    EXPECT_EQ(es32Caps.MaxFragmentImageUniforms, g_fake.maxFragmentImageUniforms);
    EXPECT_EQ(es32Caps.MaxComputeImageUniforms, g_fake.maxComputeImageUniforms);
    EXPECT_TRUE(g_fake.maxGeometryImageUniformsQueried);
}

// The per-stage GL_MAX_*_SHADER_STORAGE_BLOCKS probes. These decide whether an application is
// told it may declare a storage block in a graphics stage, and on a driver that cannot serve one
// a wrong answer is not a cosmetic mis-report: the program is built, the driver refuses it at
// link time, the frontend reports LINK_STATUS true anyway, and every draw with it renders
// nothing. A Mali-G925-Immortalis reports 0 for vertex, both tessellation stages and geometry.
TEST(PerStageStorageBlockCapabilities, TakesTheDriverValuesAndGatesTessAndGeometryOnEs32) {
    const auto funcs = MakeFakeGLESFunctions();

    // ES 3.1: the tessellation and geometry pnames do not exist, so they must not be asked for
    // and the stages must report the spec minimum of 0 rather than a hopeful driver number.
    ResetFakeDriver();
    g_fake.maxVertexSsboBlocks = 3;
    MobileGL::MG_External::GLESCapabilities es31Caps;
    ASSERT_TRUE(MobileGL::MG_Util::BackendLoader::FillInGLESCapabilities(es31Caps, funcs));
    EXPECT_EQ(es31Caps.MaxVertexShaderStorageBlocks, 3);
    EXPECT_EQ(es31Caps.MaxFragmentShaderStorageBlocks, g_fake.maxFragmentSsboBlocks);
    EXPECT_EQ(es31Caps.MaxTessControlShaderStorageBlocks, 0);
    EXPECT_EQ(es31Caps.MaxTessEvaluationShaderStorageBlocks, 0);
    EXPECT_EQ(es31Caps.MaxGeometryShaderStorageBlocks, 0);
    EXPECT_FALSE(g_fake.tessAndGeometrySsboBlocksQueried);

    // ES 3.2: all five are real pnames and all five driver values must come through verbatim.
    ResetFakeDriver();
    g_fake.maxVertexSsboBlocks = 3;
    g_fake.glesMinorVersion = 2;
    MobileGL::MG_External::GLESCapabilities es32Caps;
    ASSERT_TRUE(MobileGL::MG_Util::BackendLoader::FillInGLESCapabilities(es32Caps, funcs));
    EXPECT_EQ(es32Caps.MaxVertexShaderStorageBlocks, 3);
    EXPECT_EQ(es32Caps.MaxTessControlShaderStorageBlocks, g_fake.maxTessControlSsboBlocks);
    EXPECT_EQ(es32Caps.MaxTessEvaluationShaderStorageBlocks, g_fake.maxTessEvaluationSsboBlocks);
    EXPECT_EQ(es32Caps.MaxGeometryShaderStorageBlocks, g_fake.maxGeometrySsboBlocks);
    EXPECT_EQ(es32Caps.MaxFragmentShaderStorageBlocks, g_fake.maxFragmentSsboBlocks);
    EXPECT_TRUE(g_fake.tessAndGeometrySsboBlocksQueried);
}

// Zero has to survive the round trip intact. It is the answer that matters most - it is what
// ARM's driver actually reports - so a probe that silently substituted a floor would put the
// bug straight back.
TEST(PerStageStorageBlockCapabilities, AZeroFromTheDriverIsReportedAsZero) {
    const auto funcs = MakeFakeGLESFunctions();

    ResetFakeDriver();
    g_fake.glesMinorVersion = 2;
    g_fake.maxVertexSsboBlocks = 0;
    g_fake.maxTessControlSsboBlocks = 0;
    g_fake.maxTessEvaluationSsboBlocks = 0;
    g_fake.maxGeometrySsboBlocks = 0;
    g_fake.maxFragmentSsboBlocks = 16;

    MobileGL::MG_External::GLESCapabilities maliLikeCaps;
    ASSERT_TRUE(MobileGL::MG_Util::BackendLoader::FillInGLESCapabilities(maliLikeCaps, funcs));

    EXPECT_EQ(maliLikeCaps.MaxVertexShaderStorageBlocks, 0);
    EXPECT_EQ(maliLikeCaps.MaxTessControlShaderStorageBlocks, 0);
    EXPECT_EQ(maliLikeCaps.MaxTessEvaluationShaderStorageBlocks, 0);
    EXPECT_EQ(maliLikeCaps.MaxGeometryShaderStorageBlocks, 0);
    EXPECT_EQ(maliLikeCaps.MaxFragmentShaderStorageBlocks, 16);
}

// A rejected query must leave no error behind for the application's first glGetError to find,
// and must fall back to the spec minimums rather than to whatever the untouched out-param held.
TEST(PerStageStorageBlockCapabilities, ARejectedQueryIsDrainedAndFallsBackToTheSpecMinimums) {
    const auto funcs = MakeFakeGLESFunctions();

    ResetFakeDriver();
    g_fake.perStageSsboBlockQueryRaisesError = true;
    g_fake.maxVertexSsboBlocks = 12;
    g_fake.maxFragmentSsboBlocks = 12;

    MobileGL::MG_External::GLESCapabilities caps;
    ASSERT_TRUE(MobileGL::MG_Util::BackendLoader::FillInGLESCapabilities(caps, funcs));

    EXPECT_EQ(caps.MaxVertexShaderStorageBlocks, 0);
    EXPECT_EQ(caps.MaxFragmentShaderStorageBlocks, 4);
    EXPECT_EQ(g_fake.pendingError, static_cast<GLenum>(GL_NO_ERROR));
}

// GL_MAX_CLIP_DISTANCES is the same defect as the per-stage storage blocks above, one pname
// over: the query does not exist without GL_EXT_clip_cull_distance, so an unguarded probe left
// an optimistic 8 behind on every ARM driver. Advertising eight clip planes a driver cannot host
// does not make gl_ClipDistance work - SPIRV-Cross emits it behind an `#extension ... : require`
// the ESSL compiler rejects, DirectGLES has nowhere to put the per-distance enables, and the
// draw renders nothing while LINK_STATUS says everything is fine.
TEST(ClipDistanceCapabilities, NoExtensionMeansNoClipDistancesAndNoQuery) {
    const auto funcs = MakeFakeGLESFunctions();

    ResetFakeDriver();
    g_fake.maxVertexSsboBlocks = 0;

    MobileGL::MG_External::GLESCapabilities caps;
    ASSERT_TRUE(MobileGL::MG_Util::BackendLoader::FillInGLESCapabilities(caps, funcs));

    EXPECT_FALSE(caps.SupportsClipDistance);
    EXPECT_EQ(caps.MaxClipDistances, 0);
    EXPECT_FALSE(g_fake.maxClipDistancesQueried)
        << "GL_MAX_CLIP_DISTANCES is not ES core; asking for it without the extension only leaks "
           "a GL_INVALID_ENUM";
}

// The other half of the same claim, and the one that keeps this from being a blanket zero: a
// driver that HAS the extension must have its real limit come through untouched. Adreno does,
// and it passes the clip-distance conformance cases on the strength of it.
TEST(ClipDistanceCapabilities, TheExtensionIsQueriedAndItsLimitIsReportedVerbatim) {
    ResetFakeDriver();
    g_fake.maxVertexSsboBlocks = 0;
    g_fake.extensions.emplace_back("GL_EXT_clip_cull_distance");
    g_fake.maxClipDistances = 6;
    const auto funcs = MakeFakeGLESFunctions();

    MobileGL::MG_External::GLESCapabilities caps;
    ASSERT_TRUE(MobileGL::MG_Util::BackendLoader::FillInGLESCapabilities(caps, funcs));

    EXPECT_TRUE(caps.SupportsClipDistance);
    EXPECT_TRUE(g_fake.maxClipDistancesQueried);
    EXPECT_EQ(caps.MaxClipDistances, 6);
}

// A driver that advertises the extension and then refuses the query is a driver fault, not a
// missing feature - but the answer has to be the honest zero either way, and the error must not
// be left for the application's first glGetError to find.
TEST(ClipDistanceCapabilities, ARejectedQueryIsDrainedAndReportsZero) {
    ResetFakeDriver();
    g_fake.maxVertexSsboBlocks = 0;
    g_fake.extensions.emplace_back("GL_EXT_clip_cull_distance");
    g_fake.clipDistanceQueryRaisesError = true;
    const auto funcs = MakeFakeGLESFunctions();

    MobileGL::MG_External::GLESCapabilities caps;
    ASSERT_TRUE(MobileGL::MG_Util::BackendLoader::FillInGLESCapabilities(caps, funcs));

    EXPECT_TRUE(g_fake.maxClipDistancesQueried);
    EXPECT_EQ(caps.MaxClipDistances, 0);
    EXPECT_EQ(funcs.glGetError(), GL_NO_ERROR) << "the failed query must not leave an error behind";
}

TEST(FragmentInterpolationCapabilities, QueriesOnlyWhenSupportedAndPreservesDriverLimits) {
    const auto funcs = MakeFakeGLESFunctions();

    ResetFakeDriver();
    g_fake.maxVertexSsboBlocks = 0;
    MobileGL::MG_External::GLESCapabilities unsupportedCaps;
    ASSERT_TRUE(MobileGL::MG_Util::BackendLoader::FillInGLESCapabilities(unsupportedCaps, funcs));
    EXPECT_FALSE(unsupportedCaps.SupportsShaderMultisampleInterpolation);
    EXPECT_FALSE(g_fake.fragmentInterpolationLimitsQueried);
    EXPECT_FLOAT_EQ(unsupportedCaps.MinFragmentInterpolationOffset, -0.5f);
    EXPECT_FLOAT_EQ(unsupportedCaps.MaxFragmentInterpolationOffset, 0.4375f);
    EXPECT_EQ(unsupportedCaps.FragmentInterpolationOffsetBits, 4);

    ResetFakeDriver();
    g_fake.maxVertexSsboBlocks = 0;
    g_fake.extensions.emplace_back("GL_OES_shader_multisample_interpolation");
    // A stale error from an earlier capability probe must not make the optional
    // interpolation query look like it failed.
    g_fake.pendingError = GL_INVALID_OPERATION;
    MobileGL::MG_External::GLESCapabilities supportedCaps;
    ASSERT_TRUE(MobileGL::MG_Util::BackendLoader::FillInGLESCapabilities(supportedCaps, funcs));
    EXPECT_TRUE(supportedCaps.SupportsShaderMultisampleInterpolation);
    EXPECT_TRUE(g_fake.fragmentInterpolationLimitsQueried);
    EXPECT_FLOAT_EQ(supportedCaps.MinFragmentInterpolationOffset, g_fake.minFragmentInterpolationOffset);
    EXPECT_FLOAT_EQ(supportedCaps.MaxFragmentInterpolationOffset, g_fake.maxFragmentInterpolationOffset);
    EXPECT_EQ(supportedCaps.FragmentInterpolationOffsetBits, g_fake.fragmentInterpolationOffsetBits);
    EXPECT_EQ(funcs.glGetError(), GL_NO_ERROR);
}

// Buffer textures are core in the OpenGL 3.1+ context MobileGL advertises but need ES 3.2 or
// EXT/OES_texture_buffer on the host. The tier decides three things at once: whether glTexBuffer
// may be called at all, which #extension directive the emitted ESSL must carry, and whether
// GL_MAX_TEXTURE_BUFFER_SIZE is a driver answer or MobileGL's own floor.
using TextureBufferTier = MobileGL::MG_External::GLESCapabilities::TextureBufferTier;

TEST(BufferTextureCapabilities, Es32ResolvesToCoreAndTakesTheDriverLimit) {
    ResetFakeDriver();
    g_fake.maxVertexSsboBlocks = 0;
    g_fake.glesMinorVersion = 2;
    const auto funcs = MakeFakeGLESFunctions();

    MobileGL::MG_External::GLESCapabilities caps;
    ASSERT_TRUE(MobileGL::MG_Util::BackendLoader::FillInGLESCapabilities(caps, funcs));

    EXPECT_EQ(caps.TextureBufferSupport, TextureBufferTier::CoreEs32);
    EXPECT_TRUE(caps.MaxTextureBufferSizeIsDriverReported);
    EXPECT_EQ(caps.MaxTextureBufferSize, g_fake.maxTextureBufferSize);
    EXPECT_TRUE(g_fake.maxTextureBufferSizeQueried);
}

// The regression this pins: an ES 3.1 driver whose support is GL_EXT_texture_buffer exports
// glTexBufferEXT and NOT the unsuffixed core name. A resolver that requires the core pointer
// declares this driver unsupported and then refuses to compile shaders it could have run.
TEST(BufferTextureCapabilities, Es31WithExtResolvesThroughTheSuffixedEntryPoint) {
    ResetFakeDriver();
    g_fake.maxVertexSsboBlocks = 0;
    g_fake.extensions.emplace_back("GL_EXT_texture_buffer");
    g_fake.hasCoreTexBufferEntryPoint = false;
    g_fake.hasExtTexBufferEntryPoint = true;
    const auto funcs = MakeFakeGLESFunctions();

    MobileGL::MG_External::GLESCapabilities caps;
    ASSERT_TRUE(MobileGL::MG_Util::BackendLoader::FillInGLESCapabilities(caps, funcs));

    EXPECT_EQ(caps.TextureBufferSupport, TextureBufferTier::ExtensionEXT);
    EXPECT_TRUE(caps.MaxTextureBufferSizeIsDriverReported);
    EXPECT_EQ(caps.MaxTextureBufferSize, g_fake.maxTextureBufferSize);
}

TEST(BufferTextureCapabilities, Es31WithOesResolvesThroughTheSuffixedEntryPoint) {
    ResetFakeDriver();
    g_fake.maxVertexSsboBlocks = 0;
    g_fake.extensions.emplace_back("GL_OES_texture_buffer");
    g_fake.hasCoreTexBufferEntryPoint = false;
    g_fake.hasOesTexBufferEntryPoint = true;
    const auto funcs = MakeFakeGLESFunctions();

    MobileGL::MG_External::GLESCapabilities caps;
    ASSERT_TRUE(MobileGL::MG_Util::BackendLoader::FillInGLESCapabilities(caps, funcs));

    // The tier, not just a boolean: it is what selects the OES spelling of the #extension
    // directive SPIRV-Cross hardcodes as EXT.
    EXPECT_EQ(caps.TextureBufferSupport, TextureBufferTier::ExtensionOES);
    EXPECT_TRUE(caps.MaxTextureBufferSizeIsDriverReported);
}

// EXT wins over OES on a driver advertising both, because SPIRV-Cross emits the EXT spelling
// natively and that tier needs no directive rewriting at all.
TEST(BufferTextureCapabilities, ExtIsPreferredWhenBothExtensionsArePresent) {
    ResetFakeDriver();
    g_fake.maxVertexSsboBlocks = 0;
    g_fake.extensions.emplace_back("GL_OES_texture_buffer");
    g_fake.extensions.emplace_back("GL_EXT_texture_buffer");
    g_fake.hasExtTexBufferEntryPoint = true;
    g_fake.hasOesTexBufferEntryPoint = true;
    const auto funcs = MakeFakeGLESFunctions();

    MobileGL::MG_External::GLESCapabilities caps;
    ASSERT_TRUE(MobileGL::MG_Util::BackendLoader::FillInGLESCapabilities(caps, funcs));

    EXPECT_EQ(caps.TextureBufferSupport, TextureBufferTier::ExtensionEXT);
}

// The motivating driver (the emulator SDK's ANGLE: ES 3.1, neither extension). The pname is
// never asked - it would raise GL_INVALID_ENUM - and the floor MobileGL keeps advertising is
// flagged as not being a driver answer, because an OpenGL 4.x context may not report 0.
TEST(BufferTextureCapabilities, Es31WithNeitherExtensionIsUnsupportedAndNeverQueriesTheLimit) {
    ResetFakeDriver();
    g_fake.maxVertexSsboBlocks = 0;
    const auto funcs = MakeFakeGLESFunctions();

    MobileGL::MG_External::GLESCapabilities caps;
    ASSERT_TRUE(MobileGL::MG_Util::BackendLoader::FillInGLESCapabilities(caps, funcs));

    EXPECT_EQ(caps.TextureBufferSupport, TextureBufferTier::None);
    EXPECT_FALSE(caps.MaxTextureBufferSizeIsDriverReported);
    EXPECT_FALSE(g_fake.maxTextureBufferSizeQueried);
    EXPECT_EQ(caps.MaxTextureBufferSize, 65536) << "the OpenGL 3.1 spec floor, not the fake's limit";
}

// An extension string with no entry point behind it is not support. This is the ES analogue of
// the multi-draw stub hazard: eglGetProcAddress may hand back live-looking pointers, so the
// two signals are required together.
TEST(BufferTextureCapabilities, AnExtensionStringWithoutAnEntryPointIsNotSupport) {
    ResetFakeDriver();
    g_fake.maxVertexSsboBlocks = 0;
    g_fake.extensions.emplace_back("GL_EXT_texture_buffer");
    g_fake.hasCoreTexBufferEntryPoint = false;
    g_fake.hasExtTexBufferEntryPoint = false;
    const auto funcs = MakeFakeGLESFunctions();

    MobileGL::MG_External::GLESCapabilities caps;
    ASSERT_TRUE(MobileGL::MG_Util::BackendLoader::FillInGLESCapabilities(caps, funcs));

    EXPECT_EQ(caps.TextureBufferSupport, TextureBufferTier::None);
    EXPECT_FALSE(caps.MaxTextureBufferSizeIsDriverReported);
}

// A driver that claims buffer textures and then refuses the query is a driver bug. The floor
// stands in, and the flag says the number was not the driver's - the POST row and the
// capability log both branch on exactly that.
TEST(BufferTextureCapabilities, ARejectedLimitQueryIsDrainedAndMarkedAsNotDriverReported) {
    ResetFakeDriver();
    g_fake.maxVertexSsboBlocks = 0;
    g_fake.glesMinorVersion = 2;
    g_fake.textureBufferSizeQueryRaisesError = true;
    const auto funcs = MakeFakeGLESFunctions();

    MobileGL::MG_External::GLESCapabilities caps;
    ASSERT_TRUE(MobileGL::MG_Util::BackendLoader::FillInGLESCapabilities(caps, funcs));

    EXPECT_EQ(caps.TextureBufferSupport, TextureBufferTier::CoreEs32);
    EXPECT_TRUE(g_fake.maxTextureBufferSizeQueried);
    EXPECT_FALSE(caps.MaxTextureBufferSizeIsDriverReported);
    EXPECT_EQ(caps.MaxTextureBufferSize, 65536);
    EXPECT_EQ(funcs.glGetError(), GL_NO_ERROR) << "the failed query must not leave an error behind";
}

// A stale error from an earlier probe must not be mistaken for this query failing.
TEST(BufferTextureCapabilities, AStaleErrorDoesNotDiscardTheDriverLimit) {
    ResetFakeDriver();
    g_fake.maxVertexSsboBlocks = 0;
    g_fake.glesMinorVersion = 2;
    g_fake.pendingError = GL_INVALID_OPERATION;
    const auto funcs = MakeFakeGLESFunctions();

    MobileGL::MG_External::GLESCapabilities caps;
    ASSERT_TRUE(MobileGL::MG_Util::BackendLoader::FillInGLESCapabilities(caps, funcs));

    EXPECT_TRUE(caps.MaxTextureBufferSizeIsDriverReported);
    EXPECT_EQ(caps.MaxTextureBufferSize, g_fake.maxTextureBufferSize);
}

TEST(FragmentInterpolationCapabilities, QueryErrorIsDrainedAndFallsBackToCoreMinimums) {
    ResetFakeDriver();
    g_fake.maxVertexSsboBlocks = 0;
    g_fake.extensions.emplace_back("GL_OES_shader_multisample_interpolation");
    g_fake.fragmentInterpolationQueryRaisesError = true;
    const auto funcs = MakeFakeGLESFunctions();

    MobileGL::MG_External::GLESCapabilities caps;
    ASSERT_TRUE(MobileGL::MG_Util::BackendLoader::FillInGLESCapabilities(caps, funcs));

    EXPECT_TRUE(g_fake.fragmentInterpolationLimitsQueried);
    EXPECT_FLOAT_EQ(caps.MinFragmentInterpolationOffset, -0.5f);
    EXPECT_FLOAT_EQ(caps.MaxFragmentInterpolationOffset, 0.4375f);
    EXPECT_EQ(caps.FragmentInterpolationOffsetBits, 4);
    EXPECT_EQ(funcs.glGetError(), GL_NO_ERROR);
}

// The extension string is what apps gate on (LWJGL builds GLCapabilities from it), so advertising
// it on a driver that cannot filter anisotropically would leave them silently on trilinear.
TEST(TextureAnisotropyCapabilities, ExtensionIsAdvertisedOnlyWhenTheHostDriverSupportsIt) {
    const auto contains = [](const MobileGL::Vector<MobileGL::GLExtension>& extensions,
                             MobileGL::GLExtension wanted) {
        return std::find(extensions.begin(), extensions.end(), wanted) != extensions.end();
    };

    const auto without = MobileGL::MG_Backend::DirectGLES::BuildAdvertisedExtensions(false, false, false, false);
    EXPECT_FALSE(contains(without, MobileGL::E_GL_EXT_texture_filter_anisotropic));
    EXPECT_FALSE(contains(without, MobileGL::E_GL_ARB_texture_filter_anisotropic));

    const auto with = MobileGL::MG_Backend::DirectGLES::BuildAdvertisedExtensions(false, true, false, false);
    EXPECT_TRUE(contains(with, MobileGL::E_GL_EXT_texture_filter_anisotropic));
    EXPECT_TRUE(contains(with, MobileGL::E_GL_ARB_texture_filter_anisotropic));

    // Same rule on the Vulkan backend, where the gate is the samplerAnisotropy device feature.
    const auto vkWithout = MobileGL::MG_Backend::DirectVulkan::BuildAdvertisedExtensions(false, false, false, false);
    EXPECT_FALSE(contains(vkWithout, MobileGL::E_GL_EXT_texture_filter_anisotropic));
    const auto vkWith = MobileGL::MG_Backend::DirectVulkan::BuildAdvertisedExtensions(false, false, true, false);
    EXPECT_TRUE(contains(vkWith, MobileGL::E_GL_EXT_texture_filter_anisotropic));
    EXPECT_TRUE(contains(vkWith, MobileGL::E_GL_ARB_texture_filter_anisotropic));
}

// Minecraft 26.3 checks ARB_draw_indirect before it considers the already-advertised
// ARB_multi_draw_indirect, then separately requires ARB_base_instance before enabling its terrain
// indirect path. Pin both strings and, just as importantly, the non-zero firstInstance gate.
TEST(IndirectDrawAdvertisement, MatchesEachBackendsUsableCommandSemantics) {
    const auto contains = [](const MobileGL::Vector<MobileGL::GLExtension>& extensions,
                             MobileGL::GLExtension wanted) {
        return std::find(extensions.begin(), extensions.end(), wanted) != extensions.end();
    };

    const auto esWithoutIndirect =
        MobileGL::MG_Backend::DirectGLES::BuildAdvertisedExtensions(false, false, false, false);
    EXPECT_FALSE(contains(esWithoutIndirect, MobileGL::E_GL_ARB_draw_indirect));
    EXPECT_FALSE(contains(esWithoutIndirect, MobileGL::E_GL_ARB_base_instance));

    const auto esWithoutBaseInstance =
        MobileGL::MG_Backend::DirectGLES::BuildAdvertisedExtensions(false, false, true, false);
    EXPECT_TRUE(contains(esWithoutBaseInstance, MobileGL::E_GL_ARB_draw_indirect));
    EXPECT_FALSE(contains(esWithoutBaseInstance, MobileGL::E_GL_ARB_base_instance));

    const auto esWithBoth =
        MobileGL::MG_Backend::DirectGLES::BuildAdvertisedExtensions(false, false, true, true);
    EXPECT_TRUE(contains(esWithBoth, MobileGL::E_GL_ARB_draw_indirect));
    EXPECT_TRUE(contains(esWithBoth, MobileGL::E_GL_ARB_base_instance));

    const auto vkWithoutBaseInstance =
        MobileGL::MG_Backend::DirectVulkan::BuildAdvertisedExtensions(false, false, false, false);
    EXPECT_TRUE(contains(vkWithoutBaseInstance, MobileGL::E_GL_ARB_draw_indirect));
    EXPECT_FALSE(contains(vkWithoutBaseInstance, MobileGL::E_GL_ARB_base_instance));

    const auto vkWithBoth =
        MobileGL::MG_Backend::DirectVulkan::BuildAdvertisedExtensions(false, false, false, true);
    EXPECT_TRUE(contains(vkWithBoth, MobileGL::E_GL_ARB_draw_indirect));
    EXPECT_TRUE(contains(vkWithBoth, MobileGL::E_GL_ARB_base_instance));
}

TEST(TextureAnisotropyCapabilities, MaxAnisotropyIsQueriedOnlyWhenTheExtensionIsPresent) {
    ResetFakeDriver();
    g_fake.maxVertexSsboBlocks = 0;
    const auto funcs = MakeFakeGLESFunctions();

    MobileGL::MG_External::GLESCapabilities absentCaps;
    ASSERT_TRUE(MobileGL::MG_Util::BackendLoader::FillInGLESCapabilities(absentCaps, funcs));
    // Never probed (it would be GL_INVALID_ENUM), and reported as "no anisotropy".
    EXPECT_FALSE(g_fake.maxTextureMaxAnisotropyQueried);
    EXPECT_FLOAT_EQ(absentCaps.MaxTextureMaxAnisotropy, 1.0f);

    ResetFakeDriver();
    g_fake.maxVertexSsboBlocks = 0;
    g_fake.maxTextureMaxAnisotropy = 16.0f;
    g_fake.extensions.emplace_back("GL_EXT_texture_filter_anisotropic");
    MobileGL::MG_External::GLESCapabilities presentCaps;
    ASSERT_TRUE(MobileGL::MG_Util::BackendLoader::FillInGLESCapabilities(presentCaps, funcs));
    EXPECT_TRUE(g_fake.maxTextureMaxAnisotropyQueried);
    EXPECT_FLOAT_EQ(presentCaps.MaxTextureMaxAnisotropy, 16.0f);
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

// eglGetProcAddress may return a non-NULL stub for an entry point the context does not
// implement (NVIDIA's ES driver does exactly that for glMultiDrawElementsBaseVertexEXT and
// the stub silently drops draws), so a resolved pointer must NEVER flip these flags on its
// own: the extension string is the authority, and the pointer only confirms callability.
TEST(MultiDrawCapabilities, PointerAloneNeverCountsAsSupport) {
    ResetFakeDriver();
    g_fake.maxVertexSsboBlocks = 0;
    auto funcs = MakeFakeGLESFunctions();
    // Simulate the stub hazard: every pointer resolved, no extension advertised.
    funcs.glMultiDrawArraysIndirectEXT = [](GLenum, const void*, GLsizei, GLsizei) {};
    funcs.glMultiDrawElementsIndirectEXT = [](GLenum, GLenum, const void*, GLsizei, GLsizei) {};
    funcs.glMultiDrawElementsBaseVertexEXT = [](GLenum, const GLsizei*, GLenum, const void* const*,
                                                GLsizei, const GLint*) {};

    MobileGL::MG_External::GLESCapabilities stubCaps;
    ASSERT_TRUE(MobileGL::MG_Util::BackendLoader::FillInGLESCapabilities(stubCaps, funcs));
    EXPECT_FALSE(stubCaps.SupportsMultiDrawIndirect);
    EXPECT_FALSE(stubCaps.SupportsMultiDrawElementsBaseVertex);

    // The real NVIDIA shape: both draw_elements_base_vertex extensions advertised but
    // GL_EXT_multi_draw_arrays missing, so glMultiDrawElementsBaseVertexEXT (added only by
    // their interaction with GL_EXT_multi_draw_arrays) is still a stub.
    ResetFakeDriver();
    g_fake.maxVertexSsboBlocks = 0;
    g_fake.extensions.emplace_back("GL_EXT_draw_elements_base_vertex");
    g_fake.extensions.emplace_back("GL_OES_draw_elements_base_vertex");
    MobileGL::MG_External::GLESCapabilities nvidiaShapedCaps;
    ASSERT_TRUE(MobileGL::MG_Util::BackendLoader::FillInGLESCapabilities(nvidiaShapedCaps, funcs));
    EXPECT_FALSE(nvidiaShapedCaps.SupportsMultiDrawElementsBaseVertex);

    // Fully supported: extensions advertised and pointers resolved.
    ResetFakeDriver();
    g_fake.maxVertexSsboBlocks = 0;
    g_fake.extensions.emplace_back("GL_EXT_multi_draw_indirect");
    g_fake.extensions.emplace_back("GL_OES_draw_elements_base_vertex");
    g_fake.extensions.emplace_back("GL_EXT_multi_draw_arrays");
    MobileGL::MG_External::GLESCapabilities supportedCaps;
    ASSERT_TRUE(MobileGL::MG_Util::BackendLoader::FillInGLESCapabilities(supportedCaps, funcs));
    EXPECT_TRUE(supportedCaps.SupportsMultiDrawIndirect);
    EXPECT_TRUE(supportedCaps.SupportsMultiDrawElementsBaseVertex);
}

TEST(MultiDrawCapabilities, ExtensionWithoutResolvedPointerIsNotSupport) {
    // Extensions advertised but the loader could not resolve the entry points (default fake
    // table leaves them null): the flags must stay false so no caller dereferences null.
    ResetFakeDriver();
    g_fake.maxVertexSsboBlocks = 0;
    g_fake.extensions.emplace_back("GL_EXT_multi_draw_indirect");
    g_fake.extensions.emplace_back("GL_EXT_draw_elements_base_vertex");
    g_fake.extensions.emplace_back("GL_EXT_multi_draw_arrays");
    const auto funcs = MakeFakeGLESFunctions();

    MobileGL::MG_External::GLESCapabilities caps;
    ASSERT_TRUE(MobileGL::MG_Util::BackendLoader::FillInGLESCapabilities(caps, funcs));
    EXPECT_FALSE(caps.SupportsMultiDrawIndirect);
    EXPECT_FALSE(caps.SupportsMultiDrawElementsBaseVertex);
}

TEST(DrawIndirectCapabilities, RequiresEs31AndBothCoreEntryPoints) {
    ResetFakeDriver();
    g_fake.maxVertexSsboBlocks = 0;
    auto funcs = MakeFakeGLESFunctions();
    funcs.glDrawElementsIndirect = [](GLenum, GLenum, const void*) {};

    MobileGL::MG_External::GLESCapabilities supportedCaps;
    ASSERT_TRUE(MobileGL::MG_Util::BackendLoader::FillInGLESCapabilities(supportedCaps, funcs));
    EXPECT_TRUE(supportedCaps.SupportsDrawIndirect);

    // The same pointers on an ES 3.0 context are not core entry points and cannot back the
    // desktop extension contract.
    ResetFakeDriver();
    g_fake.maxVertexSsboBlocks = 0;
    g_fake.glesMinorVersion = 0;
    MobileGL::MG_External::GLESCapabilities es30Caps;
    ASSERT_TRUE(MobileGL::MG_Util::BackendLoader::FillInGLESCapabilities(es30Caps, funcs));
    EXPECT_FALSE(es30Caps.SupportsDrawIndirect);

    ResetFakeDriver();
    g_fake.maxVertexSsboBlocks = 0;
    const auto missingElements = MakeFakeGLESFunctions();
    MobileGL::MG_External::GLESCapabilities missingEntryPointCaps;
    ASSERT_TRUE(
        MobileGL::MG_Util::BackendLoader::FillInGLESCapabilities(missingEntryPointCaps, missingElements));
    EXPECT_FALSE(missingEntryPointCaps.SupportsDrawIndirect);
}

TEST(BaseInstanceCapabilities, RequiresTheExtensionAndAllThreeEntryPoints) {
    ResetFakeDriver();
    g_fake.maxVertexSsboBlocks = 0;
    auto funcs = MakeFakeGLESFunctions();
    funcs.glDrawArraysInstancedBaseInstanceEXT = [](GLenum, GLint, GLsizei, GLsizei, GLuint) {};
    funcs.glDrawElementsInstancedBaseInstanceEXT =
        [](GLenum, GLsizei, GLenum, const void*, GLsizei, GLuint) {};
    funcs.glDrawElementsInstancedBaseVertexBaseInstanceEXT =
        [](GLenum, GLsizei, GLenum, const void*, GLsizei, GLint, GLuint) {};

    // Resolved stubs alone must never make the capability true.
    MobileGL::MG_External::GLESCapabilities pointersOnlyCaps;
    ASSERT_TRUE(MobileGL::MG_Util::BackendLoader::FillInGLESCapabilities(pointersOnlyCaps, funcs));
    EXPECT_FALSE(pointersOnlyCaps.SupportsBaseInstance);

    ResetFakeDriver();
    g_fake.maxVertexSsboBlocks = 0;
    g_fake.extensions.emplace_back("GL_EXT_base_instance");
    MobileGL::MG_External::GLESCapabilities supportedCaps;
    ASSERT_TRUE(MobileGL::MG_Util::BackendLoader::FillInGLESCapabilities(supportedCaps, funcs));
    EXPECT_TRUE(supportedCaps.SupportsBaseInstance);

    ResetFakeDriver();
    g_fake.maxVertexSsboBlocks = 0;
    g_fake.extensions.emplace_back("GL_EXT_base_instance");
    funcs.glDrawElementsInstancedBaseInstanceEXT = nullptr;
    MobileGL::MG_External::GLESCapabilities missingEntryPointCaps;
    ASSERT_TRUE(
        MobileGL::MG_Util::BackendLoader::FillInGLESCapabilities(missingEntryPointCaps, funcs));
    EXPECT_FALSE(missingEntryPointCaps.SupportsBaseInstance);
}
