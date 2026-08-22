// MobileGL - MobileGL/MG_Test/SelfTest/DriverBugProbesTest.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include <gtest/gtest.h>

#include <MG_Util/SelfTest/DriverBugProbes.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace MobileGL;
using MobileGL::MG_Util::SelfTest::CollectGlesKnownDriverBugs;
using MobileGL::MG_Util::SelfTest::DriverBugVerdict;
using MobileGL::MG_Util::SelfTest::ProbeCrossStageImageQualifierMergeDropsWrites;
using MobileGL::MG_Util::SelfTest::ProbeGeometryStageSsboWriteAfterEmitDropped;
using MobileGL::MG_Util::SelfTest::ProbeImageLocationPerNameBudget;
using MobileGL::MG_Util::SelfTest::ProbeImageWriteReadCoherencyResidual;
using MobileGL::MG_Util::SelfTest::ProbeR32FMultisampleSwizzleCorruption;

namespace {
    // A driver table with nothing resolved. Every probe has to treat this as "cannot tell",
    // never as "affected".
    MG_External::GLESFunctionsTable EmptyFunctionTable() {
        return MG_External::GLESFunctionsTable{};
    }

    // ===================== THE FAKE DRIVER =====================
    //
    // Same idea as the fake GLES table BackendLoaderTest drives the gl_InstanceID probe with:
    // captureless lambdas over one file-scope state, with per-test knobs that turn each defect
    // on and off. It is deliberately a MODEL of the defect rather than a canned answer - the
    // fake reads the shader text the probe actually submitted and reproduces what the affected
    // driver does with it, so a probe that stopped building the triggering shape would stop
    // detecting, which is exactly what these tests are for.
    //
    // These tests call the Probe* functions directly rather than through
    // CollectGlesKnownDriverBugs(): the collector goes through the once-per-process memos, and a
    // memo latched by one test would decide the answer for every later one.

    // The exact text an affected Adreno driver puts in the info log for this refusal.
    const char* const kImageLocationLinkLog =
        "Error: Image Image location or component exceeds max allowed.\nError: Linking failed.";

    struct FakeDriver {
        // ---- limits the probes gate on -------------------------------------
        GLint maxColorTextureSamples = 4;
        GLint maxImageUnits = 8;
        GLint maxVertexImageUniforms = 8;
        GLint maxFragmentImageUniforms = 8;
        GLint maxGeometryImageUniforms = 3;
        // The landed geometry probe reads this; zero keeps it inert so it cannot interfere.
        GLint maxGeometrySsboBlocks = 0;
        bool geometryImageLimitQueryRaisesError = false;
        bool colorTextureSamplesQueryRaisesError = false;

        // ---- defect knobs ---------------------------------------------------
        // Probe 1: a swizzled-alpha, non-zero-sample .w fetch reads garbage from the second
        // sampling program onward.
        bool msaaSwizzledAlphaCorrupted = false;
        // Probe 1's inconclusive path: EVERY sampled read is wrong, including the controls.
        bool msaaEveryReadWrong = false;
        // Probe 2: the link fails once the program declares more distinct image uniform NAMES
        // than this.
        int distinctImageNameBudget = 1000;
        // Probe 3: a same-name coherent writeonly/readonly pair loses the writing stage's store.
        bool sameNameImagePairDropsWrites = false;
        // Probe 3's inconclusive path: the renamed control loses it too.
        bool everyVertexImageWriteDropped = false;
        // Probe 4: how many texels the in-invocation dependent read misses under the STRONGEST
        // shape, how many it misses under the shape MobileGL emits today, and whether the
        // two-draw control misses them too.
        int coherencyStrongestShapeFailedTexels = 0;
        int coherencyEmittedShapeFailedTexels = 0;
        int coherencyControlFailedTexels = 0;

        // ---- object bookkeeping ---------------------------------------------
        GLenum pendingError = GL_NO_ERROR;
        GLuint nextShaderId = 1;
        GLuint nextProgramId = 1;
        GLuint nextTextureId = 1;
        GLuint nextFramebufferId = 1;
        GLuint nextVertexArrayId = 1;
        int aliveShaders = 0;
        int alivePrograms = 0;
        int aliveTextures = 0;
        int aliveFramebuffers = 0;
        int aliveVertexArrays = 0;

        std::map<GLuint, std::string> shaderSources;
        std::map<GLuint, std::vector<GLuint>> programShaders;
        std::map<GLuint, bool> programLinked;
        std::map<GLuint, std::string> programInfoLogs;
        // texture id -> GL_TEXTURE_SWIZZLE_A
        std::map<GLuint, GLenum> multisampleAlphaSwizzle;

        GLuint boundMultisampleTexture = 0;
        GLuint currentProgram = 0;
        // How many programs that sample a multisample texture have been linked so far. The
        // corruption starts at the second.
        int sampledMultisampleProgramCount = 0;
        // Set by glDrawArrays, consumed by glReadPixels.
        GLfloat lastSampledValue = 1.0f;
        int lastFailedTexelCount = 0;
    };

    FakeDriver g_fake;

    void ResetFakeDriver() { g_fake = FakeDriver{}; }

    const std::string& SourceOf(GLuint shader) {
        static const std::string empty;
        const auto it = g_fake.shaderSources.find(shader);
        return it == g_fake.shaderSources.end() ? empty : it->second;
    }

    bool Contains(const std::string& haystack, const char* needle) {
        return haystack.find(needle) != std::string::npos;
    }

    // Every `image2D <name>` the program declares, across all its stages.
    std::vector<std::string> DeclaredImageNames(GLuint program) {
        std::vector<std::string> names;
        const auto attached = g_fake.programShaders.find(program);
        if (attached == g_fake.programShaders.end()) return names;
        for (const GLuint shader : attached->second) {
            const std::string& source = SourceOf(shader);
            std::size_t at = 0;
            while ((at = source.find("image2D ", at)) != std::string::npos) {
                at += std::strlen("image2D ");
                const std::size_t end = source.find_first_of(";,)", at);
                if (end == std::string::npos) break;
                std::string name = source.substr(at, end - at);
                while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();
                if (std::find(names.begin(), names.end(), name) == names.end()) {
                    names.push_back(name);
                }
                at = end;
            }
        }
        return names;
    }

    std::string StageSourceContaining(GLuint program, const char* needle) {
        const auto attached = g_fake.programShaders.find(program);
        if (attached == g_fake.programShaders.end()) return {};
        for (const GLuint shader : attached->second) {
            const std::string& source = SourceOf(shader);
            if (Contains(source, needle)) return source;
        }
        return {};
    }

    // The uniform name in `... image2D <name>;` of the first declaration in `source`.
    std::string FirstImageNameIn(const std::string& source) {
        const std::size_t at = source.find("image2D ");
        if (at == std::string::npos) return {};
        const std::size_t start = at + std::strlen("image2D ");
        const std::size_t end = source.find(';', start);
        if (end == std::string::npos) return {};
        return source.substr(start, end - start);
    }

    // Whatever the sampling vertex shader asked for: `texelFetch(mg_probeSampler, ivec2(0), N).C`.
    void ParseSampledFetch(const std::string& source, int& sampleIndex, char& component) {
        sampleIndex = -1;
        component = '?';
        const std::size_t at = source.find("texelFetch(mg_probeSampler, ivec2(0), ");
        if (at == std::string::npos) return;
        const std::size_t start = at + std::strlen("texelFetch(mg_probeSampler, ivec2(0), ");
        sampleIndex = std::atoi(source.c_str() + start);
        const std::size_t dot = source.find(").", start);
        if (dot != std::string::npos && dot + 2 < source.size()) component = source[dot + 2];
    }

    MG_External::GLESFunctionsTable MakeFakeGLESFunctions() {
        MG_External::GLESFunctionsTable funcs{};

        funcs.glGetError = []() -> GLenum {
            const GLenum error = g_fake.pendingError;
            g_fake.pendingError = GL_NO_ERROR;
            return error;
        };
        funcs.glGetIntegerv = [](GLenum pname, GLint* data) {
            switch (pname) {
            case GL_MAX_COLOR_TEXTURE_SAMPLES:
                if (g_fake.colorTextureSamplesQueryRaisesError) {
                    g_fake.pendingError = GL_INVALID_ENUM;
                } else {
                    *data = g_fake.maxColorTextureSamples;
                }
                break;
            case GL_MAX_IMAGE_UNITS:
                *data = g_fake.maxImageUnits;
                break;
            case GL_MAX_VERTEX_IMAGE_UNIFORMS:
                *data = g_fake.maxVertexImageUniforms;
                break;
            case GL_MAX_FRAGMENT_IMAGE_UNIFORMS:
                *data = g_fake.maxFragmentImageUniforms;
                break;
            case GL_MAX_GEOMETRY_IMAGE_UNIFORMS:
                if (g_fake.geometryImageLimitQueryRaisesError) {
                    g_fake.pendingError = GL_INVALID_ENUM;
                } else {
                    *data = g_fake.maxGeometryImageUniforms;
                }
                break;
            case GL_MAX_GEOMETRY_SHADER_STORAGE_BLOCKS:
                *data = g_fake.maxGeometrySsboBlocks;
                break;
            default:
                break;
            }
        };
        funcs.glGetIntegeri_v = [](GLenum, GLuint, GLint* data) { *data = 0; };
        funcs.glGetFloatv = [](GLenum, GLfloat* data) {
            data[0] = 0.0f;
            data[1] = 0.0f;
            data[2] = 0.0f;
            data[3] = 0.0f;
        };
        funcs.glIsEnabled = [](GLenum) -> GLboolean { return GL_FALSE; };
        funcs.glEnable = [](GLenum) {};
        funcs.glDisable = [](GLenum) {};
        funcs.glFinish = []() {};
        funcs.glMemoryBarrier = [](GLbitfield) {};
        funcs.glPixelStorei = [](GLenum, GLint) {};
        funcs.glViewport = [](GLint, GLint, GLsizei, GLsizei) {};
        funcs.glClear = [](GLbitfield) {};
        funcs.glClearColor = [](GLfloat, GLfloat, GLfloat, GLfloat) {};
        funcs.glActiveTexture = [](GLenum) {};

        // ---- shaders and programs -------------------------------------------
        funcs.glCreateShader = [](GLenum) -> GLuint {
            ++g_fake.aliveShaders;
            return g_fake.nextShaderId++;
        };
        funcs.glShaderSource = [](GLuint shader, GLsizei count, const GLchar* const* strings,
                                  const GLint*) {
            std::string source;
            for (GLsizei i = 0; i < count; ++i) {
                if (strings[i] != nullptr) source += strings[i];
            }
            g_fake.shaderSources[shader] = std::move(source);
        };
        funcs.glCompileShader = [](GLuint) {};
        funcs.glGetShaderiv = [](GLuint, GLenum pname, GLint* params) {
            if (pname == GL_COMPILE_STATUS) *params = GL_TRUE;
        };
        funcs.glGetShaderInfoLog = [](GLuint, GLsizei bufSize, GLsizei*, GLchar* infoLog) {
            if (bufSize > 0) infoLog[0] = '\0';
        };
        funcs.glDeleteShader = [](GLuint shader) {
            if (shader != 0) --g_fake.aliveShaders;
        };
        funcs.glCreateProgram = []() -> GLuint {
            ++g_fake.alivePrograms;
            return g_fake.nextProgramId++;
        };
        funcs.glAttachShader = [](GLuint program, GLuint shader) {
            g_fake.programShaders[program].push_back(shader);
        };
        funcs.glLinkProgram = [](GLuint program) {
            const std::vector<std::string> names = DeclaredImageNames(program);
            const bool overBudget = static_cast<int>(names.size()) > g_fake.distinctImageNameBudget;
            g_fake.programLinked[program] = !overBudget;
            g_fake.programInfoLogs[program] = overBudget ? kImageLocationLinkLog : "";
            if (!overBudget && !StageSourceContaining(program, "texelFetch(mg_probeSampler").empty()) {
                ++g_fake.sampledMultisampleProgramCount;
            }
        };
        funcs.glGetProgramiv = [](GLuint program, GLenum pname, GLint* params) {
            if (pname != GL_LINK_STATUS) return;
            const auto it = g_fake.programLinked.find(program);
            *params = (it == g_fake.programLinked.end() || it->second) ? GL_TRUE : GL_FALSE;
        };
        funcs.glGetProgramInfoLog = [](GLuint program, GLsizei bufSize, GLsizei*, GLchar* infoLog) {
            if (bufSize <= 0) return;
            const auto it = g_fake.programInfoLogs.find(program);
            const std::string& log = it == g_fake.programInfoLogs.end() ? std::string() : it->second;
            const GLsizei copied = static_cast<GLsizei>(
                std::min<std::size_t>(log.size(), static_cast<std::size_t>(bufSize - 1)));
            std::memcpy(infoLog, log.data(), static_cast<std::size_t>(copied));
            infoLog[copied] = '\0';
        };
        funcs.glDeleteProgram = [](GLuint program) {
            if (program != 0) --g_fake.alivePrograms;
        };
        funcs.glUseProgram = [](GLuint program) { g_fake.currentProgram = program; };
        funcs.glGetUniformLocation = [](GLuint, const GLchar*) -> GLint { return 0; };
        funcs.glUniform1i = [](GLint, GLint) {};

        // ---- textures, framebuffers, vertex arrays ---------------------------
        funcs.glGenTextures = [](GLsizei n, GLuint* textures) {
            for (GLsizei i = 0; i < n; ++i) {
                textures[i] = g_fake.nextTextureId++;
                ++g_fake.aliveTextures;
            }
        };
        funcs.glBindTexture = [](GLenum target, GLuint texture) {
            if (target == GL_TEXTURE_2D_MULTISAMPLE) g_fake.boundMultisampleTexture = texture;
        };
        funcs.glDeleteTextures = [](GLsizei n, const GLuint* textures) {
            for (GLsizei i = 0; i < n; ++i) {
                if (textures[i] != 0) --g_fake.aliveTextures;
            }
        };
        funcs.glTexParameteri = [](GLenum target, GLenum pname, GLint param) {
            if (target == GL_TEXTURE_2D_MULTISAMPLE && pname == GL_TEXTURE_SWIZZLE_A) {
                g_fake.multisampleAlphaSwizzle[g_fake.boundMultisampleTexture] =
                    static_cast<GLenum>(param);
            }
        };
        funcs.glTexImage2D = [](GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum,
                                const void*) {};
        funcs.glTexSubImage2D = [](GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum,
                                   const void*) {};
        funcs.glTexStorage2D = [](GLenum, GLsizei, GLenum, GLsizei, GLsizei) {};
        funcs.glTexStorage2DMultisample = [](GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLboolean) {};
        funcs.glGenFramebuffers = [](GLsizei n, GLuint* framebuffers) {
            for (GLsizei i = 0; i < n; ++i) {
                framebuffers[i] = g_fake.nextFramebufferId++;
                ++g_fake.aliveFramebuffers;
            }
        };
        funcs.glBindFramebuffer = [](GLenum, GLuint) {};
        funcs.glFramebufferTexture2D = [](GLenum, GLenum, GLenum, GLuint, GLint) {};
        funcs.glCheckFramebufferStatus = [](GLenum) -> GLenum { return GL_FRAMEBUFFER_COMPLETE; };
        funcs.glDeleteFramebuffers = [](GLsizei n, const GLuint* framebuffers) {
            for (GLsizei i = 0; i < n; ++i) {
                if (framebuffers[i] != 0) --g_fake.aliveFramebuffers;
            }
        };
        funcs.glGenVertexArrays = [](GLsizei n, GLuint* arrays) {
            for (GLsizei i = 0; i < n; ++i) {
                arrays[i] = g_fake.nextVertexArrayId++;
                ++g_fake.aliveVertexArrays;
            }
        };
        funcs.glBindVertexArray = [](GLuint) {};
        funcs.glDeleteVertexArrays = [](GLsizei n, const GLuint* arrays) {
            for (GLsizei i = 0; i < n; ++i) {
                if (arrays[i] != 0) --g_fake.aliveVertexArrays;
            }
        };
        funcs.glBindImageTexture = [](GLuint, GLuint, GLint, GLboolean, GLint, GLenum, GLenum) {};

        // ---- the draw, where the defects live --------------------------------
        funcs.glDrawArrays = [](GLenum, GLint, GLsizei) {
            const GLuint program = g_fake.currentProgram;
            const std::string sampling = StageSourceContaining(program, "texelFetch(mg_probeSampler");
            if (!sampling.empty()) {
                int sampleIndex = -1;
                char component = '?';
                ParseSampledFetch(sampling, sampleIndex, component);
                const GLenum swizzle = g_fake.multisampleAlphaSwizzle.count(
                                           g_fake.boundMultisampleTexture) != 0
                                           ? g_fake.multisampleAlphaSwizzle[g_fake.boundMultisampleTexture]
                                           : GL_ALPHA;
                // An R32F texel filled with (1, 0, 0, -) reads 1.0 through both the ALPHA and the
                // RED swizzle sources, which is why one expected constant covers every shape.
                g_fake.lastSampledValue = 1.0f;
                if (g_fake.msaaEveryReadWrong) {
                    g_fake.lastSampledValue = 0.0f;
                } else if (g_fake.msaaSwizzledAlphaCorrupted && swizzle == GL_RED && component == 'w' &&
                           sampleIndex != 0 && g_fake.sampledMultisampleProgramCount >= 2) {
                    // Uninitialised memory: a value that is neither the answer nor the clear.
                    g_fake.lastSampledValue = -1.34954e-17f;
                }
                return;
            }

            // Matched on the access qualifier alone, not on "coherent writeonly": the strongest
            // coherency shape spells it "coherent volatile writeonly".
            const std::string writeStage = StageSourceContaining(program, "writeonly");
            const std::string readStage = StageSourceContaining(program, "readonly");
            if (!writeStage.empty() && !readStage.empty() && Contains(readStage, "memoryBarrierImage")) {
                // The coherency probe: one invocation stores and then reads back. `volatile` is
                // what tells the strongest shape apart from the one MobileGL emits today, and
                // giving them separate knobs is what lets a test pin the case where only the
                // emitted shape is wrong - a fixable defect that must not be reported here.
                g_fake.lastFailedTexelCount = Contains(readStage, "coherent volatile")
                                                  ? g_fake.coherencyStrongestShapeFailedTexels
                                                  : g_fake.coherencyEmittedShapeFailedTexels;
                return;
            }
            if (!writeStage.empty() && readStage.empty()) {
                // The coherency control's store half; the load half decides the result.
                g_fake.lastFailedTexelCount = 0;
                return;
            }
            if (writeStage.empty() && !readStage.empty()) {
                g_fake.lastFailedTexelCount = g_fake.coherencyControlFailedTexels;
                return;
            }
            if (!writeStage.empty() && !readStage.empty()) {
                // The qualifier-merge pair: the stores are lost when the two halves share a name.
                const bool sharedName =
                    FirstImageNameIn(writeStage) == FirstImageNameIn(readStage) &&
                    !FirstImageNameIn(writeStage).empty();
                const bool lost = g_fake.everyVertexImageWriteDropped ||
                                  (g_fake.sameNameImagePairDropsWrites && sharedName);
                g_fake.lastFailedTexelCount = lost ? 1 << 20 : 0;
                return;
            }
            g_fake.lastFailedTexelCount = 0;
        };
        funcs.glReadPixels = [](GLint, GLint, GLsizei width, GLsizei height, GLenum format, GLenum type,
                                void* pixels) {
            const std::size_t texels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
            if (format == GL_RED && type == GL_FLOAT) {
                GLfloat* out = static_cast<GLfloat*>(pixels);
                for (std::size_t i = 0; i < texels; ++i) out[i] = g_fake.lastSampledValue;
                return;
            }
            GLubyte* out = static_cast<GLubyte*>(pixels);
            const std::size_t failed =
                std::min<std::size_t>(texels, static_cast<std::size_t>(g_fake.lastFailedTexelCount));
            for (std::size_t i = 0; i < texels; ++i) {
                const bool ok = i >= failed;
                out[i * 4 + 0] = ok ? 0 : 255;
                out[i * 4 + 1] = ok ? 255 : 0;
                out[i * 4 + 2] = 0;
                out[i * 4 + 3] = 255;
            }
        };

        return funcs;
    }

    void ExpectProbeReleasedEverything() {
        EXPECT_EQ(g_fake.aliveShaders, 0);
        EXPECT_EQ(g_fake.alivePrograms, 0);
        EXPECT_EQ(g_fake.aliveTextures, 0);
        EXPECT_EQ(g_fake.aliveFramebuffers, 0);
        EXPECT_EQ(g_fake.aliveVertexArrays, 0);
    }
} // namespace

// The rule the whole section depends on: a probe that cannot run reports NO bug. If an
// unrunnable probe answered "affected", every device without the entry points - every desktop
// build, every unit-test process - would grow a driver-bug row it has no evidence for, and the
// section would stop meaning "this device has these bugs".
TEST(DriverBugProbes, AProbeThatCannotRunReportsNoBug) {
    const MG_External::GLESFunctionsTable gl = EmptyFunctionTable();
    EXPECT_FALSE(ProbeGeometryStageSsboWriteAfterEmitDropped(gl))
        << "a probe with no entry points to call must not claim the driver is affected";
    EXPECT_FALSE(ProbeR32FMultisampleSwizzleCorruption(gl));
    EXPECT_FALSE(ProbeImageLocationPerNameBudget(gl).detected);
    EXPECT_FALSE(ProbeCrossStageImageQualifierMergeDropsWrites(gl));
    EXPECT_FALSE(ProbeImageWriteReadCoherencyResidual(gl).detected);
}

// The section lists only bugs the device HAS, so a driver nothing could be probed on renders
// nothing at all rather than a list of reassurances.
TEST(DriverBugProbes, CollectsNoFindingsWhenNothingCanBeProbed) {
    const MG_External::GLESFunctionsTable gl = EmptyFunctionTable();
    EXPECT_TRUE(CollectGlesKnownDriverBugs(gl).empty());
}

// Every finding the table can produce is a bug that is PRESENT, which is why the vocabulary is
// FIXED/UNFIXABLE and not PASS/FAIL. This latches that no probe can smuggle in a "not affected"
// row by returning a finding with an empty name or detail - the screen renders both.
TEST(DriverBugProbes, EveryFindingCarriesANameAndAnExplanation) {
    const MG_External::GLESFunctionsTable gl = EmptyFunctionTable();
    for (const auto& finding : CollectGlesKnownDriverBugs(gl)) {
        EXPECT_FALSE(finding.name.empty());
        EXPECT_FALSE(finding.detail.empty()) << finding.name << " must say what MobileGL does about it";
        EXPECT_TRUE(finding.verdict == DriverBugVerdict::Fixed ||
                    finding.verdict == DriverBugVerdict::Unfixable);
    }
}

// ===================== R32F MULTISAMPLE SWIZZLE =====================

TEST(DriverBugProbes, R32FMultisampleSwizzleIsCleanOnAConformingDriver) {
    ResetFakeDriver();
    const MG_External::GLESFunctionsTable gl = MakeFakeGLESFunctions();
    EXPECT_FALSE(ProbeR32FMultisampleSwizzleCorruption(gl));
    ExpectProbeReleasedEverything();
}

TEST(DriverBugProbes, R32FMultisampleSwizzleIsDetectedFromTheSecondProgramOnward) {
    ResetFakeDriver();
    g_fake.msaaSwizzledAlphaCorrupted = true;
    const MG_External::GLESFunctionsTable gl = MakeFakeGLESFunctions();
    EXPECT_TRUE(ProbeR32FMultisampleSwizzleCorruption(gl));
    ExpectProbeReleasedEverything();
}

// The control rule, made executable: a driver on which even the default-swizzle, sample-zero and
// .x reads are wrong is broken in some larger way, and the probe may not name the alpha swizzle
// as the cause.
TEST(DriverBugProbes, R32FMultisampleSwizzleReportsNothingWhenTheControlsAreWrongToo) {
    ResetFakeDriver();
    g_fake.msaaSwizzledAlphaCorrupted = true;
    g_fake.msaaEveryReadWrong = true;
    const MG_External::GLESFunctionsTable gl = MakeFakeGLESFunctions();
    EXPECT_FALSE(ProbeR32FMultisampleSwizzleCorruption(gl))
        << "with every read wrong the probe has no evidence that the alpha swizzle is the variable";
}

TEST(DriverBugProbes, R32FMultisampleSwizzleNeedsMoreThanOneSample) {
    ResetFakeDriver();
    g_fake.msaaSwizzledAlphaCorrupted = true;
    g_fake.maxColorTextureSamples = 1;
    const MG_External::GLESFunctionsTable gl = MakeFakeGLESFunctions();
    EXPECT_FALSE(ProbeR32FMultisampleSwizzleCorruption(gl));
}

// ===================== IMAGE LOCATION PER NAME =====================

TEST(DriverBugProbes, ImageLocationBudgetIsCleanWhenNamesDoNotCost) {
    ResetFakeDriver();
    const MG_External::GLESFunctionsTable gl = MakeFakeGLESFunctions();
    const auto measurement = ProbeImageLocationPerNameBudget(gl);
    EXPECT_FALSE(measurement.detected);
    ExpectProbeReleasedEverything();
}

TEST(DriverBugProbes, ImageLocationBudgetIsDetectedWhenOnlyTheSharedNamesLink) {
    ResetFakeDriver();
    // Four image uniforms per stage: twelve distinct names in the subject, four in the control.
    g_fake.distinctImageNameBudget = 5;
    const MG_External::GLESFunctionsTable gl = MakeFakeGLESFunctions();
    const auto measurement = ProbeImageLocationPerNameBudget(gl);
    EXPECT_TRUE(measurement.detected);
    EXPECT_EQ(measurement.perStageImageUniforms, g_fake.maxGeometryImageUniforms + 1);
    EXPECT_EQ(measurement.subjectDistinctNames, measurement.perStageImageUniforms * 3);
    EXPECT_EQ(measurement.controlDistinctNames, measurement.perStageImageUniforms);
    EXPECT_NE(measurement.driverMessage.find("exceeds max allowed"), String::npos)
        << "the report quotes the driver rather than paraphrasing it";
    ExpectProbeReleasedEverything();
}

// The control rule again: when the shared-name program is refused too, the shape is simply too
// big for this driver and the refusal is honest.
TEST(DriverBugProbes, ImageLocationBudgetReportsNothingWhenTheControlAlsoFails) {
    ResetFakeDriver();
    g_fake.distinctImageNameBudget = 2;
    const MG_External::GLESFunctionsTable gl = MakeFakeGLESFunctions();
    EXPECT_FALSE(ProbeImageLocationPerNameBudget(gl).detected);
}

TEST(DriverBugProbes, ImageLocationBudgetNeedsAGeometryStageThatCanHoldImages) {
    ResetFakeDriver();
    g_fake.distinctImageNameBudget = 5;
    g_fake.maxGeometryImageUniforms = 0;
    const MG_External::GLESFunctionsTable gl = MakeFakeGLESFunctions();
    EXPECT_FALSE(ProbeImageLocationPerNameBudget(gl).detected);
}

TEST(DriverBugProbes, ImageLocationBudgetStaysSilentOnAContextWithoutTheGeometryLimit) {
    ResetFakeDriver();
    g_fake.distinctImageNameBudget = 5;
    g_fake.geometryImageLimitQueryRaisesError = true;
    const MG_External::GLESFunctionsTable gl = MakeFakeGLESFunctions();
    EXPECT_FALSE(ProbeImageLocationPerNameBudget(gl).detected)
        << "a pre-ES-3.2 context has no geometry stage to build the shape out of";
}

// ===================== CROSS-STAGE QUALIFIER MERGE =====================

TEST(DriverBugProbes, QualifierMergeIsCleanWhenTheDriverKeepsTheStore) {
    ResetFakeDriver();
    const MG_External::GLESFunctionsTable gl = MakeFakeGLESFunctions();
    EXPECT_FALSE(ProbeCrossStageImageQualifierMergeDropsWrites(gl));
    ExpectProbeReleasedEverything();
}

TEST(DriverBugProbes, QualifierMergeIsDetectedWhenOnlyTheSharedNameLosesTheStore) {
    ResetFakeDriver();
    g_fake.sameNameImagePairDropsWrites = true;
    const MG_External::GLESFunctionsTable gl = MakeFakeGLESFunctions();
    EXPECT_TRUE(ProbeCrossStageImageQualifierMergeDropsWrites(gl));
    ExpectProbeReleasedEverything();
}

// A driver that loses the RENAMED store too cannot write images from the vertex stage at all -
// a different and much larger claim, which this probe may not make.
TEST(DriverBugProbes, QualifierMergeReportsNothingWhenTheRenamedControlAlsoFails) {
    ResetFakeDriver();
    g_fake.sameNameImagePairDropsWrites = true;
    g_fake.everyVertexImageWriteDropped = true;
    const MG_External::GLESFunctionsTable gl = MakeFakeGLESFunctions();
    EXPECT_FALSE(ProbeCrossStageImageQualifierMergeDropsWrites(gl));
}

TEST(DriverBugProbes, QualifierMergeNeedsVertexStageImageUniforms) {
    ResetFakeDriver();
    g_fake.sameNameImagePairDropsWrites = true;
    g_fake.maxVertexImageUniforms = 0;
    const MG_External::GLESFunctionsTable gl = MakeFakeGLESFunctions();
    EXPECT_FALSE(ProbeCrossStageImageQualifierMergeDropsWrites(gl));
}

// ===================== IMAGE COHERENCY RESIDUAL =====================

TEST(DriverBugProbes, ImageCoherencyIsCleanWhenTheDependentReadObservesTheStore) {
    ResetFakeDriver();
    const MG_External::GLESFunctionsTable gl = MakeFakeGLESFunctions();
    const auto measurement = ProbeImageWriteReadCoherencyResidual(gl);
    EXPECT_FALSE(measurement.detected);
    ExpectProbeReleasedEverything();
}

TEST(DriverBugProbes, ImageCoherencyResidualIsDetectedAndQuantified) {
    ResetFakeDriver();
    g_fake.coherencyStrongestShapeFailedTexels = 376;
    g_fake.coherencyEmittedShapeFailedTexels = 418;
    const MG_External::GLESFunctionsTable gl = MakeFakeGLESFunctions();
    const auto measurement = ProbeImageWriteReadCoherencyResidual(gl);
    EXPECT_TRUE(measurement.detected);
    EXPECT_EQ(measurement.mismatchedTexels, 376);
    EXPECT_EQ(measurement.emittedShapeMismatchedTexels, 418)
        << "the row reports what applications get, not only what is theoretically reachable";
    EXPECT_GT(measurement.totalTexels, 418) << "the report needs a denominator to quote a rate";
    ExpectProbeReleasedEverything();
}

// The reason the subject is the STRONGEST shape and not the one MobileGL emits. Mesa llvmpipe
// misses every texel with `coherent` + memoryBarrierImage() and none once the pair is also
// `volatile` - a defect MobileGL could fix by emitting a different shape, which is not what
// UNFIXABLE means and does not belong in this section.
TEST(DriverBugProbes, ImageCoherencyReportsNothingWhenAStrongerShapeWouldFixIt) {
    ResetFakeDriver();
    g_fake.coherencyStrongestShapeFailedTexels = 0;
    g_fake.coherencyEmittedShapeFailedTexels = 4096;
    const MG_External::GLESFunctionsTable gl = MakeFakeGLESFunctions();
    EXPECT_FALSE(ProbeImageWriteReadCoherencyResidual(gl).detected)
        << "a driver the volatile shape satisfies has a fixable defect, not an unfixable one";
}

// The control rule once more: a driver whose glFinish-separated two-draw dependency is ALSO
// dirty has a bigger defect than an in-invocation ordering residual, and this probe must not
// dress that up as one.
TEST(DriverBugProbes, ImageCoherencyReportsNothingWhenTheFinishSeparatedControlIsDirtyToo) {
    ResetFakeDriver();
    g_fake.coherencyStrongestShapeFailedTexels = 376;
    g_fake.coherencyControlFailedTexels = 4096;
    const MG_External::GLESFunctionsTable gl = MakeFakeGLESFunctions();
    EXPECT_FALSE(ProbeImageWriteReadCoherencyResidual(gl).detected);
}

TEST(DriverBugProbes, ImageCoherencyNeedsBothHalvesOfTheSplitPairInOneStage) {
    ResetFakeDriver();
    g_fake.coherencyStrongestShapeFailedTexels = 376;
    g_fake.maxFragmentImageUniforms = 1;
    const MG_External::GLESFunctionsTable gl = MakeFakeGLESFunctions();
    EXPECT_FALSE(ProbeImageWriteReadCoherencyResidual(gl).detected);
}
