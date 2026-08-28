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
#include <array>
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
using MobileGL::MG_Util::SelfTest::ProbeBlitIgnoresDestinationArrayLayer;
using MobileGL::MG_Util::SelfTest::ProbeCopyImageMirrorsPacked16FieldOrder;
using MobileGL::MG_Util::SelfTest::ProbeExplicitVertexInputLocationCeiling;
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

    // The exact text an affected Adreno driver puts in the compile log when it refuses a
    // vertex input's layout(location = N). Quoted rather than paraphrased for the same reason the
    // link log below is: the report shows it to a human, so a probe that stopped capturing it
    // would stop being useful long before it stopped detecting.
    const char* const kAttributeRangeCompileLog =
        "ERROR: 0:2: '' : the location is not within attribute range [0, MAX_ATTRIBUTES-1] \nERROR: 1 compilation errors.  No code generated.";

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

        // Probe 5: GL_MAX_VERTEX_ATTRIBS, and the two separate ceilings the probe has to tell
        // apart - how high `layout(location = N)` may go in the ESSL compiler, and how high
        // glBindAttribLocation may go at link. On an unaffected driver both are above the
        // advertised count.
        GLint maxVertexAttribs = 32;
        int explicitLocationCeiling = 1000;
        int bindAttribLocationCeiling = 1000;
        // Probe 5's inconclusive path: nothing compiles, including the location-0 control.
        bool everyCompileFails = false;
        // Probe 6: a blit writes the destination array layer the framebuffer names, or always
        // layer 0. The second knob is the inconclusive path - a driver that does not honour the
        // SOURCE layer either fails the probe's control.
        bool blitIgnoresDestinationLayer = false;
        bool blitIgnoresSourceLayer = false;

        // Probe 7: the driver's PHYSICAL field order for a 16-bit packed texel at a non-zero
        // mip level of a 2D array is the *_REV mirror of the plain-image order. Modelled at
        // the raw copy, which is the only path that can observe it (uploads and readbacks of
        // the same image decode the driver's own layout consistently): a copy whose SOURCE is
        // such a level delivers the mirrored re-encoding, which the plain 2D readback then
        // decodes with the non-REV order - exactly the 0x0047 -> 0x8C20 arithmetic the
        // affected Mali hands back. The mirror only engages for the allocation the CTS
        // failures were measured on - a THREE-level 30x30x12 array - so a probe that stopped
        // building the triggering shape (fewer levels, other dimensions) stops detecting,
        // which is exactly what these tests are for: on the real driver a 14x14 base's level 1
        // measured clean, and nobody has measured a two-level chain at all.
        bool packed16ArrayMipFieldOrderMirrored = false;
        // The inconclusive path: EVERY array level stores mirrored words, level 0 included, so
        // the probe's level-0 control copy is dirty too and no verdict may be reached.
        bool packed16EveryArrayLevelMirrored = false;
        // Another inconclusive path: the copy silently lands nothing, so the destination keeps
        // its 0xFFFF fill - a value that is neither the word nor its mirror.
        bool packed16CopyDoesNothing = false;

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

        std::map<GLuint, bool> shaderCompiled;
        std::map<GLuint, std::string> shaderInfoLogs;
        // program -> (attribute name -> location) as glBindAttribLocation left it.
        std::map<GLuint, std::map<std::string, GLint>> boundAttribLocations;
        // 2D array texture id -> the byte every texel of each layer holds. Two layers is all the
        // layered-blit probe uses, and one byte per layer is all it distinguishes.
        std::map<GLuint, std::array<GLubyte, 2>> arrayLayerFill;
        // framebuffer id -> the (2D array texture, layer) glFramebufferTextureLayer attached.
        std::map<GLuint, std::pair<GLuint, GLint>> framebufferLayerAttachment;
        // (texture, level) -> the PHYSICAL 16-bit word every texel of that 5551 image holds.
        // One word per level is all the packed16 probe distinguishes: it uploads a uniform
        // fill and reads one texel.
        std::map<std::pair<GLuint, GLint>, GLushort> packedTexelWords;
        // 2D-array texture id -> its allocation shape, as glTexImage3D built it. What the
        // packed16 mirror is gated on: level-0 dimensions plus a mask of the levels actually
        // allocated, so only the measured three-level 30x30x12 chain diverges.
        struct FakeArrayAllocation {
            GLsizei width = 0;
            GLsizei height = 0;
            GLsizei layers = 0;
            unsigned levelMask = 0;
        };
        std::map<GLuint, FakeArrayAllocation> packedArrayAllocations;
        // framebuffer id -> the plain 2D texture glFramebufferTexture2D attached.
        std::map<GLuint, GLuint> framebuffer2DAttachment;
        GLuint boundArrayTexture = 0;
        GLuint boundTexture2D = 0;
        GLuint boundDrawFramebuffer = 0;
        GLuint boundReadFramebuffer = 0;

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

    // The 5_5_5_1 <-> 1_5_5_5_REV field-order mirror: the same fields, packed from the other
    // end of the word. 0x0047 (R,G,B,A = 0,1,3,1) becomes 0x8C20 - the exact pair every
    // failing KHR-GL4x.copy_image body printed on the affected Mali.
    GLushort MirrorPacked5551(GLushort word) {
        const GLushort r = (word >> 11) & 0x1F;
        const GLushort g = (word >> 6) & 0x1F;
        const GLushort b = (word >> 1) & 0x1F;
        const GLushort a = word & 0x1;
        return static_cast<GLushort>((a << 15) | (b << 10) | (g << 5) | r);
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

    // The N in `layout(location = N) in ...`, or -1 when the source declares no such input.
    // Read off the text the probe actually submitted, so a probe that stopped emitting the
    // qualifier would stop being detected here too.
    int ExplicitVertexInputLocationIn(const std::string& source) {
        const std::size_t at = source.find("layout(location = ");
        if (at == std::string::npos) return -1;
        const std::size_t start = at + std::strlen("layout(location = ");
        const std::size_t close = source.find(')', start);
        if (close == std::string::npos) return -1;
        // Only a VERTEX INPUT counts: `layout(location = 0) out vec4` is a different declaration
        // and no driver caps it against GL_MAX_VERTEX_ATTRIBS.
        const std::size_t declaration = source.find_first_not_of(" \t", close + 1);
        if (declaration == std::string::npos || source.compare(declaration, 3, "in ") != 0) return -1;
        return std::atoi(source.c_str() + start);
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
            case GL_MAX_VERTEX_ATTRIBS:
                *data = g_fake.maxVertexAttribs;
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
        funcs.glCompileShader = [](GLuint shader) {
            const std::string& source = SourceOf(shader);
            const int location = ExplicitVertexInputLocationIn(source);
            const bool refused =
                g_fake.everyCompileFails || (location >= 0 && location >= g_fake.explicitLocationCeiling);
            g_fake.shaderCompiled[shader] = !refused;
            g_fake.shaderInfoLogs[shader] = refused ? kAttributeRangeCompileLog : "";
        };
        funcs.glGetShaderiv = [](GLuint shader, GLenum pname, GLint* params) {
            if (pname != GL_COMPILE_STATUS) return;
            const auto it = g_fake.shaderCompiled.find(shader);
            *params = (it == g_fake.shaderCompiled.end() || it->second) ? GL_TRUE : GL_FALSE;
        };
        funcs.glGetShaderInfoLog = [](GLuint shader, GLsizei bufSize, GLsizei*, GLchar* infoLog) {
            if (bufSize <= 0) return;
            const auto it = g_fake.shaderInfoLogs.find(shader);
            const std::string& log = it == g_fake.shaderInfoLogs.end() ? std::string() : it->second;
            const GLsizei copied = static_cast<GLsizei>(
                std::min<std::size_t>(log.size(), static_cast<std::size_t>(bufSize - 1)));
            std::memcpy(infoLog, log.data(), static_cast<std::size_t>(copied));
            infoLog[copied] = '\0';
        };
        funcs.glDeleteShader = [](GLuint shader) {
            if (shader != 0) --g_fake.aliveShaders;
        };
        funcs.glBindAttribLocation = [](GLuint program, GLuint index, const GLchar* name) {
            g_fake.boundAttribLocations[program][name] = static_cast<GLint>(index);
        };
        funcs.glGetAttribLocation = [](GLuint program, const GLchar* name) -> GLint {
            const auto programEntry = g_fake.boundAttribLocations.find(program);
            if (programEntry == g_fake.boundAttribLocations.end()) return -1;
            const auto nameEntry = programEntry->second.find(name);
            if (nameEntry == programEntry->second.end()) return -1;
            return nameEntry->second >= g_fake.bindAttribLocationCeiling ? -1 : nameEntry->second;
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
            // A driver whose glBindAttribLocation ceiling is lower than the location asked for
            // refuses the LINK rather than the compile - which is the half of the vertex-input
            // probe that decides whether the attribute is reachable another way at all.
            bool attributeOutOfRange = false;
            if (const auto it = g_fake.boundAttribLocations.find(program);
                it != g_fake.boundAttribLocations.end()) {
                for (const auto& [attributeName, location] : it->second) {
                    if (location >= g_fake.bindAttribLocationCeiling) attributeOutOfRange = true;
                }
            }
            g_fake.programLinked[program] = !overBudget && !attributeOutOfRange;
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
            if (target == GL_TEXTURE_2D_ARRAY) g_fake.boundArrayTexture = texture;
            if (target == GL_TEXTURE_2D) g_fake.boundTexture2D = texture;
        };
        funcs.glTexStorage3D = [](GLenum target, GLsizei, GLenum, GLsizei, GLsizei, GLsizei) {
            if (target == GL_TEXTURE_2D_ARRAY) g_fake.arrayLayerFill[g_fake.boundArrayTexture] = {0, 0};
        };
        // One byte per layer: the layered-blit probe fills every texel of a layer with the same
        // value and only ever asks which layer a value ended up on.
        funcs.glTexSubImage3D = [](GLenum target, GLint, GLint, GLint, GLint zoffset, GLsizei, GLsizei,
                                   GLsizei, GLenum, GLenum, const void* pixels) {
            if (target != GL_TEXTURE_2D_ARRAY || pixels == nullptr) return;
            auto& fill = g_fake.arrayLayerFill[g_fake.boundArrayTexture];
            if (zoffset >= 0 && static_cast<std::size_t>(zoffset) < fill.size()) {
                fill[static_cast<std::size_t>(zoffset)] = static_cast<const GLubyte*>(pixels)[0];
            }
        };
        funcs.glDeleteTextures = [](GLsizei n, const GLuint* textures) {
            for (GLsizei i = 0; i < n; ++i) {
                if (textures[i] != 0) --g_fake.aliveTextures;
                for (auto it = g_fake.packedTexelWords.begin(); it != g_fake.packedTexelWords.end();) {
                    if (it->first.first == textures[i]) {
                        it = g_fake.packedTexelWords.erase(it);
                    } else {
                        ++it;
                    }
                }
                g_fake.packedArrayAllocations.erase(textures[i]);
            }
        };
        funcs.glTexParameteri = [](GLenum target, GLenum pname, GLint param) {
            if (target == GL_TEXTURE_2D_MULTISAMPLE && pname == GL_TEXTURE_SWIZZLE_A) {
                g_fake.multisampleAlphaSwizzle[g_fake.boundMultisampleTexture] =
                    static_cast<GLenum>(param);
            }
        };
        // The packed16 probe's endpoints. A plain 2D image stores its 5551 words in the
        // canonical (non-REV) order on every knob setting - the defect is confined to array
        // mip levels, and keeping the 2D side clean is what lets the readback below decode
        // with one order and still reproduce the mirror.
        funcs.glTexImage2D = [](GLenum target, GLint level, GLint, GLsizei, GLsizei, GLint, GLenum,
                                GLenum type, const void* pixels) {
            if (target != GL_TEXTURE_2D || type != GL_UNSIGNED_SHORT_5_5_5_1 || pixels == nullptr) return;
            GLushort word = 0;
            std::memcpy(&word, pixels, sizeof(word));
            g_fake.packedTexelWords[{g_fake.boundTexture2D, level}] = word;
        };
        // Records the allocation shape the mirror below is gated on, and the uploaded word.
        funcs.glTexImage3D = [](GLenum target, GLint level, GLint, GLsizei width, GLsizei height,
                                GLsizei depth, GLint, GLenum, GLenum type, const void* pixels) {
            if (target != GL_TEXTURE_2D_ARRAY || type != GL_UNSIGNED_SHORT_5_5_5_1 || pixels == nullptr) return;
            GLushort word = 0;
            std::memcpy(&word, pixels, sizeof(word));
            g_fake.packedTexelWords[{g_fake.boundArrayTexture, level}] = word;
            auto& allocation = g_fake.packedArrayAllocations[g_fake.boundArrayTexture];
            if (level == 0) {
                allocation.width = width;
                allocation.height = height;
                allocation.layers = depth;
            }
            if (level >= 0 && level < 8) allocation.levelMask |= 1u << level;
        };
        // A raw texel-block move: the PHYSICAL word travels. The defect lives here - a source
        // level whose storage keeps the mirrored layout delivers the re-encoded word - and it
        // only exists for the allocation it was measured on: three levels of a 30x30x12 array.
        funcs.glCopyImageSubData = [](GLuint srcName, GLenum, GLint srcLevel, GLint, GLint, GLint,
                                      GLuint dstName, GLenum, GLint dstLevel, GLint, GLint, GLint,
                                      GLsizei, GLsizei, GLsizei) {
            if (g_fake.packed16CopyDoesNothing) return;
            const auto source = g_fake.packedTexelWords.find({srcName, srcLevel});
            if (source == g_fake.packedTexelWords.end()) return;
            GLushort word = source->second;
            const auto allocation = g_fake.packedArrayAllocations.find(srcName);
            const bool measuredShape = allocation != g_fake.packedArrayAllocations.end() &&
                                       allocation->second.width == 30 && allocation->second.height == 30 &&
                                       allocation->second.layers == 12 &&
                                       allocation->second.levelMask == 0b111u;
            if (measuredShape && (g_fake.packed16EveryArrayLevelMirrored ||
                                  (g_fake.packed16ArrayMipFieldOrderMirrored && srcLevel >= 1))) {
                word = MirrorPacked5551(word);
            }
            g_fake.packedTexelWords[{dstName, dstLevel}] = word;
        };
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
        funcs.glBindFramebuffer = [](GLenum target, GLuint framebuffer) {
            if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) {
                g_fake.boundDrawFramebuffer = framebuffer;
            }
            if (target == GL_FRAMEBUFFER || target == GL_READ_FRAMEBUFFER) {
                g_fake.boundReadFramebuffer = framebuffer;
            }
        };
        funcs.glFramebufferTexture2D = [](GLenum target, GLenum, GLenum, GLuint texture, GLint) {
            const GLuint framebuffer = (target == GL_READ_FRAMEBUFFER) ? g_fake.boundReadFramebuffer
                                                                       : g_fake.boundDrawFramebuffer;
            g_fake.framebuffer2DAttachment[framebuffer] = texture;
        };
        funcs.glFramebufferTextureLayer = [](GLenum target, GLenum, GLuint texture, GLint, GLint layer) {
            const GLuint framebuffer = (target == GL_READ_FRAMEBUFFER) ? g_fake.boundReadFramebuffer
                                                                       : g_fake.boundDrawFramebuffer;
            g_fake.framebufferLayerAttachment[framebuffer] = {texture, layer};
        };
        funcs.glReadBuffer = [](GLenum) {};
        // The defect itself: the source layer is read from where the READ framebuffer says (unless
        // that knob is on too), and the result is written to the layer the DRAW framebuffer names -
        // or to layer 0 regardless, which is what an affected driver does.
        funcs.glBlitFramebuffer = [](GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield,
                                     GLenum) {
            const auto source = g_fake.framebufferLayerAttachment.find(g_fake.boundReadFramebuffer);
            const auto destination = g_fake.framebufferLayerAttachment.find(g_fake.boundDrawFramebuffer);
            if (source == g_fake.framebufferLayerAttachment.end() ||
                destination == g_fake.framebufferLayerAttachment.end()) {
                return;
            }
            const GLint sourceLayer = g_fake.blitIgnoresSourceLayer ? 0 : source->second.second;
            const GLint destinationLayer =
                g_fake.blitIgnoresDestinationLayer ? 0 : destination->second.second;
            auto& sourceFill = g_fake.arrayLayerFill[source->second.first];
            auto& destinationFill = g_fake.arrayLayerFill[destination->second.first];
            if (sourceLayer < 0 || static_cast<std::size_t>(sourceLayer) >= sourceFill.size()) return;
            if (destinationLayer < 0 ||
                static_cast<std::size_t>(destinationLayer) >= destinationFill.size()) {
                return;
            }
            destinationFill[static_cast<std::size_t>(destinationLayer)] =
                sourceFill[static_cast<std::size_t>(sourceLayer)];
        };
        funcs.glCheckFramebufferStatus = [](GLenum) -> GLenum { return GL_FRAMEBUFFER_COMPLETE; };
        funcs.glDeleteFramebuffers = [](GLsizei n, const GLuint* framebuffers) {
            for (GLsizei i = 0; i < n; ++i) {
                if (framebuffers[i] != 0) --g_fake.aliveFramebuffers;
                g_fake.framebufferLayerAttachment.erase(framebuffers[i]);
                g_fake.framebuffer2DAttachment.erase(framebuffers[i]);
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
            // A read framebuffer naming a plain 2D texture that holds a 5551 word is the
            // packed16 probe reading its copy destination. The driver decodes its OWN storage
            // with the canonical non-REV order and expands each field by bit replication -
            // which is exactly how the mirrored word 0x8C20 becomes (140, 132, 132, 0).
            if (const auto attached = g_fake.framebuffer2DAttachment.find(g_fake.boundReadFramebuffer);
                attached != g_fake.framebuffer2DAttachment.end() &&
                g_fake.packedTexelWords.count({attached->second, 0}) != 0) {
                // Gated on the texture actually holding a 5551 word, so every OTHER probe that
                // attaches a plain 2D texture keeps the pass/fail readback paths below.
                const GLushort w = g_fake.packedTexelWords[{attached->second, 0}];
                const auto expand5 = [](GLushort v) {
                    return static_cast<GLubyte>((v << 3) | (v >> 2));
                };
                GLubyte* out = static_cast<GLubyte*>(pixels);
                for (std::size_t i = 0; i < texels; ++i) {
                    out[i * 4 + 0] = expand5((w >> 11) & 0x1F);
                    out[i * 4 + 1] = expand5((w >> 6) & 0x1F);
                    out[i * 4 + 2] = expand5((w >> 1) & 0x1F);
                    out[i * 4 + 3] = (w & 0x1) ? 255 : 0;
                }
                return;
            }
            // Answered before anything else below: a read framebuffer that names an array LAYER
            // is the layered-blit probe asking what that layer holds, and its bytes have nothing
            // to do with the pass/fail texel encoding the image probes below share.
            if (const auto layered = g_fake.framebufferLayerAttachment.find(g_fake.boundReadFramebuffer);
                layered != g_fake.framebufferLayerAttachment.end()) {
                const auto& fill = g_fake.arrayLayerFill[layered->second.first];
                const GLint layer = layered->second.second;
                const GLubyte value =
                    (layer >= 0 && static_cast<std::size_t>(layer) < fill.size())
                        ? fill[static_cast<std::size_t>(layer)]
                        : 0;
                GLubyte* out = static_cast<GLubyte*>(pixels);
                for (std::size_t i = 0; i < texels; ++i) {
                    out[i * 4 + 0] = value;
                    out[i * 4 + 1] = value;
                    out[i * 4 + 2] = value;
                    out[i * 4 + 3] = 255;
                }
                return;
            }
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
    EXPECT_FALSE(ProbeBlitIgnoresDestinationArrayLayer(gl))
        << "a probe with no entry points has measured nothing";
    EXPECT_FALSE(ProbeExplicitVertexInputLocationCeiling(gl).detected)
        << "a probe with no entry points has measured nothing";
    EXPECT_FALSE(ProbeGeometryStageSsboWriteAfterEmitDropped(gl))
        << "a probe with no entry points to call must not claim the driver is affected";
    EXPECT_FALSE(ProbeR32FMultisampleSwizzleCorruption(gl));
    EXPECT_FALSE(ProbeImageLocationPerNameBudget(gl).detected);
    EXPECT_FALSE(ProbeCrossStageImageQualifierMergeDropsWrites(gl));
    EXPECT_FALSE(ProbeImageWriteReadCoherencyResidual(gl).detected);
    EXPECT_FALSE(ProbeCopyImageMirrorsPacked16FieldOrder(gl))
        << "a probe with no entry points has measured nothing";
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

// ===================== EXPLICIT VERTEX INPUT LOCATION CEILING =====================

// The clean case, and the one that has to stay cheap: a driver whose compiler accepts the
// highest location it advertises is measured in a single compile and withdraws nothing.
TEST(DriverBugProbes, VertexInputLocationCeilingIsCleanWhenTheAdvertisedMaximumCompiles) {
    ResetFakeDriver();
    g_fake.maxVertexAttribs = 32;
    const MG_External::GLESFunctionsTable gl = MakeFakeGLESFunctions();
    const auto measurement = ProbeExplicitVertexInputLocationCeiling(gl);
    EXPECT_FALSE(measurement.detected);
    EXPECT_EQ(measurement.advertisedMaxVertexAttribs, 32);
    EXPECT_EQ(measurement.usableLocations, 32) << "an unaffected driver must be taken at its word";
    ExpectProbeReleasedEverything();
}

// The defect: 32 advertised, the qualifier refused from 16 up. The bisection has to land on the
// boundary exactly - one off in either direction advertises an attribute that cannot be declared,
// or withdraws one that can.
TEST(DriverBugProbes, VertexInputLocationCeilingIsMeasuredWhenTheQualifierIsCapped) {
    ResetFakeDriver();
    g_fake.maxVertexAttribs = 32;
    g_fake.explicitLocationCeiling = 16;
    const MG_External::GLESFunctionsTable gl = MakeFakeGLESFunctions();
    const auto measurement = ProbeExplicitVertexInputLocationCeiling(gl);
    EXPECT_TRUE(measurement.detected);
    EXPECT_EQ(measurement.advertisedMaxVertexAttribs, 32);
    EXPECT_EQ(measurement.usableLocations, 16);
    EXPECT_TRUE(measurement.bindAttribLocationReachesAdvertisedMax)
        << "this driver caps only the qualifier, so the report may say the attribute is still reachable";
    EXPECT_NE(measurement.driverMessage.find("attribute range"), std::string::npos)
        << "the driver's own wording is what makes the row evidence rather than an assertion";
    ExpectProbeReleasedEverything();
}

// A ceiling that is not a power of two, so a bisection that happened to land on 16 by arithmetic
// rather than by measurement fails here.
TEST(DriverBugProbes, VertexInputLocationCeilingBisectsToAnAwkwardBoundary) {
    ResetFakeDriver();
    g_fake.maxVertexAttribs = 32;
    g_fake.explicitLocationCeiling = 23;
    const MG_External::GLESFunctionsTable gl = MakeFakeGLESFunctions();
    const auto measurement = ProbeExplicitVertexInputLocationCeiling(gl);
    EXPECT_TRUE(measurement.detected);
    EXPECT_EQ(measurement.usableLocations, 23);
    ExpectProbeReleasedEverything();
}

// THE FIRST CONTROL. A compiler that refuses location 0 refuses everything, and a probe that
// read that as "only one location is usable" would withdraw every vertex attribute the device has.
TEST(DriverBugProbes, VertexInputLocationCeilingReportsNothingWhenTheLocationZeroControlFails) {
    ResetFakeDriver();
    g_fake.maxVertexAttribs = 32;
    g_fake.everyCompileFails = true;
    const MG_External::GLESFunctionsTable gl = MakeFakeGLESFunctions();
    const auto measurement = ProbeExplicitVertexInputLocationCeiling(gl);
    EXPECT_FALSE(measurement.detected);
    EXPECT_EQ(measurement.usableLocations, 32)
        << "an inconclusive probe has to leave the advertised count exactly where it found it";
    ExpectProbeReleasedEverything();
}

// THE SECOND CONTROL, which does not change the clamp but does change what the report may claim:
// a driver that cannot reach the location through glBindAttribLocation either has fewer
// attributes than it advertises, rather than merely an unspellable half.
TEST(DriverBugProbes, VertexInputLocationCeilingSaysWhenTheAttributeIsUnreachableAnyWay) {
    ResetFakeDriver();
    g_fake.maxVertexAttribs = 32;
    g_fake.explicitLocationCeiling = 16;
    g_fake.bindAttribLocationCeiling = 16;
    const MG_External::GLESFunctionsTable gl = MakeFakeGLESFunctions();
    const auto measurement = ProbeExplicitVertexInputLocationCeiling(gl);
    EXPECT_TRUE(measurement.detected);
    EXPECT_EQ(measurement.usableLocations, 16);
    EXPECT_FALSE(measurement.bindAttribLocationReachesAdvertisedMax);
    ExpectProbeReleasedEverything();
}

// ===================== LAYERED BLIT DESTINATION =====================

TEST(DriverBugProbes, LayeredBlitDestinationIsCleanWhenTheLayerIsHonoured) {
    ResetFakeDriver();
    const MG_External::GLESFunctionsTable gl = MakeFakeGLESFunctions();
    EXPECT_FALSE(ProbeBlitIgnoresDestinationArrayLayer(gl));
    ExpectProbeReleasedEverything();
}

TEST(DriverBugProbes, LayeredBlitDestinationIsDetectedWhenTheCopyLandsOnLayerZero) {
    ResetFakeDriver();
    g_fake.blitIgnoresDestinationLayer = true;
    const MG_External::GLESFunctionsTable gl = MakeFakeGLESFunctions();
    EXPECT_TRUE(ProbeBlitIgnoresDestinationArrayLayer(gl));
    ExpectProbeReleasedEverything();
}

// THE CONTROL. A driver that ignores the SOURCE layer too cannot address array layers through a
// framebuffer at all - a bigger defect, and one this probe is not entitled to report as its own.
// The control blit onto destination layer 0 is what catches it: the value it looks for lives only
// on the source's layer 1, so a source read pinned to layer 0 never produces it.
TEST(DriverBugProbes, LayeredBlitDestinationReportsNothingWhenTheSourceLayerIsIgnoredToo) {
    ResetFakeDriver();
    g_fake.blitIgnoresDestinationLayer = true;
    g_fake.blitIgnoresSourceLayer = true;
    const MG_External::GLESFunctionsTable gl = MakeFakeGLESFunctions();
    EXPECT_FALSE(ProbeBlitIgnoresDestinationArrayLayer(gl));
    ExpectProbeReleasedEverything();
}

// And the shape that is not this bug at all: a blit that moves nothing anywhere. The probe's
// subject then finds its magic byte on no layer, which is "reached no verdict", not "landed on 0".
TEST(DriverBugProbes, LayeredBlitDestinationReportsNothingWhenTheBlitMovesNothing) {
    ResetFakeDriver();
    const MG_External::GLESFunctionsTable gl = MakeFakeGLESFunctions();
    MG_External::GLESFunctionsTable inert = gl;
    inert.glBlitFramebuffer = [](GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield,
                                 GLenum) {};
    EXPECT_FALSE(ProbeBlitIgnoresDestinationArrayLayer(inert));
    ExpectProbeReleasedEverything();
}

TEST(DriverBugProbes, ImageCoherencyNeedsBothHalvesOfTheSplitPairInOneStage) {
    ResetFakeDriver();
    g_fake.coherencyStrongestShapeFailedTexels = 376;
    g_fake.maxFragmentImageUniforms = 1;
    const MG_External::GLESFunctionsTable gl = MakeFakeGLESFunctions();
    EXPECT_FALSE(ProbeImageWriteReadCoherencyResidual(gl).detected);
}

TEST(DriverBugProbes, Packed16FieldOrderIsCleanOnAConformingDriver) {
    ResetFakeDriver();
    const MG_External::GLESFunctionsTable gl = MakeFakeGLESFunctions();
    EXPECT_FALSE(ProbeCopyImageMirrorsPacked16FieldOrder(gl));
    ExpectProbeReleasedEverything();
}

TEST(DriverBugProbes, Packed16FieldOrderIsDetectedWhenTheArrayMipLevelIsMirrored) {
    ResetFakeDriver();
    g_fake.packed16ArrayMipFieldOrderMirrored = true;
    const MG_External::GLESFunctionsTable gl = MakeFakeGLESFunctions();
    EXPECT_TRUE(ProbeCopyImageMirrorsPacked16FieldOrder(gl));
    ExpectProbeReleasedEverything();
}

// THE CONTROL. A driver whose EVERY array level stores mirrored words fails the level-0
// control copy too - a different (and larger) defect than the one this probe is entitled to
// report, so it must reach no verdict rather than pin the mip-level shape.
TEST(DriverBugProbes, Packed16FieldOrderReportsNothingWhenTheControlIsMirroredToo) {
    ResetFakeDriver();
    g_fake.packed16ArrayMipFieldOrderMirrored = true;
    g_fake.packed16EveryArrayLevelMirrored = true;
    const MG_External::GLESFunctionsTable gl = MakeFakeGLESFunctions();
    EXPECT_FALSE(ProbeCopyImageMirrorsPacked16FieldOrder(gl));
    ExpectProbeReleasedEverything();
}

// And the shape that is not this bug: a copy that lands nothing leaves the destination's
// 0xFFFF fill, which matches neither the word nor its mirror - "reached no verdict".
TEST(DriverBugProbes, Packed16FieldOrderReportsNothingWhenTheCopyLandsNothing) {
    ResetFakeDriver();
    g_fake.packed16ArrayMipFieldOrderMirrored = true;
    g_fake.packed16CopyDoesNothing = true;
    const MG_External::GLESFunctionsTable gl = MakeFakeGLESFunctions();
    EXPECT_FALSE(ProbeCopyImageMirrorsPacked16FieldOrder(gl));
    ExpectProbeReleasedEverything();
}

// The byte arithmetic the fake's mirror encodes, pinned against the QPA evidence. The fake
// models the ARRAY-AS-SOURCE direction (decode 5_5_5_1, re-encode 1_5_5_5_REV): 0x0047 must
// deliver 0x8C20, the exact pair every failing array-as-source copy_image body printed. The
// QPA's array-as-destination bodies show the INVERSE transform (enc_5551 of dec_REV: 0x0007
// delivered as 0x3800), and enc_REV(dec_5551(x)) inverts enc_5551(dec_REV(x)), so feeding
// the delivered word back through the fake's mirror must reproduce the original.
TEST(DriverBugProbes, Packed16MirrorArithmeticMatchesTheDeviceEvidence) {
    EXPECT_EQ(MirrorPacked5551(0x0047), 0x8C20);
    EXPECT_EQ(MirrorPacked5551(0x3800), 0x0007);
}
