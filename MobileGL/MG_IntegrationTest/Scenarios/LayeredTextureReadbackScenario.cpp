// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/LayeredTextureReadbackScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - READING EVERY LAYER OF A 1D-ARRAY / CUBE-MAP-ARRAY LEVEL BACK.
//
// glGetTexImage has no ES equivalent, so Espryt serves it by attaching the level to a scratch
// READ framebuffer and reading it with glReadPixels. Two of the targets it has to answer for do
// not fit that shape the way the others do, and both came back as zeroes in
// KHR-GL4x.shader_image_load_store.basic-allTargets-* and .non-layered_binding:
//
//   * GL_TEXTURE_1D_ARRAY carries its LAYERS in the state-side height - that is what
//     glTexImage2D(GL_TEXTURE_1D_ARRAY, w, layers) means - while the ES texture behind it is a 2D
//     array of height 1 with the layers in depth. The readback used the state-side shape, so it
//     asked layer 0 for a `layers`-row rectangle that layer does not have: row 0 was the only one
//     that could be right, and everything past it was whatever reading outside an attachment
//     produces.
//   * GL_TEXTURE_CUBE_MAP_ARRAY has no glFramebufferTexture2D target token at all, so the 2D
//     attach it used to take errored, the scratch FBO stayed incomplete, and every read fell
//     through to the CPU shadow - which holds what was UPLOADED, i.e. the seed, not what the
//     shader stored.
//
// Both cases store from a compute dispatch (so the only copy of the data is the GPU one and a
// stale shadow cannot pass) and then read the whole level back in one glGetTexImage, checking
// every layer separately so a failure names which one. r32ui throughout: it is a core GLSL ES
// image format, so nothing here can be confused with the missing-format story that
// ImageFormatQualifierScenario covers.
//
// Magma reads these back through its own path and is unaffected by the ES attachment rules, so
// both cases run on both backends and must agree.

#include <cstddef>
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

        constexpr int kExtent = 4;
        constexpr int kArrayLayers = 3;     // enough that "layer 0 only" is visibly wrong
        constexpr int kCubeLayerFaces = 12; // two cubes, which is what the conformance case uses
        // A value no store writes, so "the store never landed" and "the store wrote the wrong
        // thing" cannot be confused - and so a readback served from the stale CPU shadow is
        // recognisable on sight.
        constexpr GLuint kSeed = 0xFEEDBEEFu;
        // Deliberately not 0: the unit has to travel through glUniform1i and be baked into the
        // generated ESSL, so a defect there cannot hide behind the default.
        constexpr GLint kImageUnit = 1;

        GLuint Expected1DArrayTexel(int x, int layer) {
            return 1000u + static_cast<GLuint>(layer) * 100u + static_cast<GLuint>(x);
        }

        GLuint ExpectedCubeArrayTexel(int x, int y, int layerFace) {
            return 1000u + static_cast<GLuint>(layerFace) * 100u + static_cast<GLuint>(y) * 10u +
                   static_cast<GLuint>(x);
        }

        // One invocation per texel, and the value it writes is a function of its coordinate - so
        // a layer read from the wrong slice does not merely differ, it says which slice it came
        // from.
        const char* k1DArrayStoreSource = R"(#version 430 core

layout (local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout (r32ui) writeonly uniform uimage1DArray uni_image;

void main()
{
    uint x = gl_GlobalInvocationID.x;
    uint layer = gl_GlobalInvocationID.z;
    imageStore(uni_image, ivec2(int(x), int(layer)), uvec4(1000u + layer * 100u + x, 0u, 0u, 0u));
}
)";

        const char* kCubeArrayStoreSource = R"(#version 430 core

layout (local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout (r32ui) writeonly uniform uimageCubeArray uni_image;

void main()
{
    uint x = gl_GlobalInvocationID.x;
    uint y = gl_GlobalInvocationID.y;
    uint layerFace = gl_GlobalInvocationID.z;
    imageStore(uni_image, ivec3(int(x), int(y), int(layerFace)),
               uvec4(1000u + layerFace * 100u + y * 10u + x, 0u, 0u, 0u));
}
)";

        class LayeredTextureReadbackScenario : public ScenarioTest {
        protected:
            void TearDown() override {
                if (!Ready()) return;
                glUseProgram(0);
                for (GLuint p : m_programs) glDeleteProgram(p);
                for (GLuint t : m_textures) glDeleteTextures(1, &t);
                m_programs.clear();
                m_textures.clear();
                GLint maxImageUnits = 0;
                glGetIntegerv(GL_MAX_IMAGE_UNITS, &maxImageUnits);
                for (GLint unit = 0; unit < maxImageUnits; ++unit) {
                    glBindImageTexture(static_cast<GLuint>(unit), 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32UI);
                }
                while (glGetError() != GL_NO_ERROR) {
                }
            }

            bool ImagesAreUsable() const {
                GLint maxImageUnits = 0;
                glGetIntegerv(GL_MAX_IMAGE_UNITS, &maxImageUnits);
                GLint maxComputeImageUniforms = 0;
                glGetIntegerv(GL_MAX_COMPUTE_IMAGE_UNIFORMS, &maxComputeImageUniforms);
                while (glGetError() != GL_NO_ERROR) {
                }
                return maxImageUnits > kImageUnit && maxComputeImageUniforms >= 1;
            }

            GLuint MakeComputeProgram(const char* source) {
                const GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
                glShaderSource(shader, 1, &source, nullptr);
                glCompileShader(shader);
                GLint compiled = GL_FALSE;
                glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
                if (compiled == GL_FALSE) {
                    char log[4096] = {};
                    glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
                    ADD_FAILURE() << "the compute shader did not compile: " << log;
                    glDeleteShader(shader);
                    return 0;
                }
                const GLuint program = glCreateProgram();
                m_programs.push_back(program);
                glAttachShader(program, shader);
                glLinkProgram(program);
                glDeleteShader(shader);
                GLint linked = GL_FALSE;
                glGetProgramiv(program, GL_LINK_STATUS, &linked);
                if (linked == GL_FALSE) {
                    char log[4096] = {};
                    glGetProgramInfoLog(program, sizeof(log) - 1, nullptr, log);
                    ADD_FAILURE() << "the compute program did not link: " << log;
                    return 0;
                }
                return program;
            }

            GLuint TrackTexture() {
                GLuint texture = 0;
                glGenTextures(1, &texture);
                m_textures.push_back(texture);
                return texture;
            }

            // layered = GL_TRUE, i.e. the whole level: that is what makes every layer reachable
            // from one dispatch, and it is what glBindImageTextures is specified to pass.
            bool DispatchStore(GLuint program, GLuint texture, GLsizei groupsX, GLsizei groupsY, GLsizei groupsZ) {
                glBindImageTexture(static_cast<GLuint>(kImageUnit), texture, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_R32UI);
                if (const GLenum error = FirstGLError()) {
                    ADD_FAILURE() << "glBindImageTexture errored with " << GLErrorName(error);
                    return false;
                }
                glUseProgram(program);
                const GLint location = glGetUniformLocation(program, "uni_image");
                if (location < 0) {
                    ADD_FAILURE() << "the image uniform was not reflected";
                    return false;
                }
                glUniform1i(location, kImageUnit);
                if (const GLenum error = FirstGLError()) {
                    ADD_FAILURE() << "assigning the image unit errored with " << GLErrorName(error);
                    return false;
                }
                glDispatchCompute(groupsX, groupsY, groupsZ);
                glMemoryBarrier(GL_ALL_BARRIER_BITS);
                glUseProgram(0);
                if (const GLenum error = FirstGLError()) {
                    ADD_FAILURE() << "the dispatch errored with " << GLErrorName(error);
                    return false;
                }
                return true;
            }

            std::vector<GLuint> m_programs;
            std::vector<GLuint> m_textures;
        };

        // The 1D-array half. A layer past the first is the whole test: layer 0 lines up with the
        // ES image's only row whichever way the axes are read, so a readback that never swapped
        // them still got it right and only the deeper layers came back wrong.
        TEST_F(LayeredTextureReadbackScenario, GetTexImageReturnsEveryLayerOfA1DArray) {
            if (!Ready()) return;
            if (!ImagesAreUsable()) GTEST_SKIP() << "no compute image uniforms";

            const GLuint program = MakeComputeProgram(k1DArrayStoreSource);
            if (program == 0) return;

            const GLuint texture = TrackTexture();
            glBindTexture(GL_TEXTURE_1D_ARRAY, texture);
            glTexParameteri(GL_TEXTURE_1D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_1D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            const std::vector<GLuint> seed(static_cast<std::size_t>(kExtent) * kArrayLayers, kSeed);
            glTexImage2D(GL_TEXTURE_1D_ARRAY, 0, GL_R32UI, kExtent, kArrayLayers, 0, GL_RED_INTEGER, GL_UNSIGNED_INT,
                         seed.data());
            ASSERT_EQ(FirstGLError(), 0u) << "creating the R32UI 1D-array texture errored";

            if (!DispatchStore(program, texture, kExtent, 1, kArrayLayers)) return;

            std::vector<GLuint> texels(seed.size(), 0u);
            glBindTexture(GL_TEXTURE_1D_ARRAY, texture);
            glGetTexImage(GL_TEXTURE_1D_ARRAY, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, texels.data());
            ASSERT_EQ(FirstGLError(), 0u) << "reading the 1D-array level back errored";

            // GL hands a 1D array back as a plain two-dimensional image whose ROWS are the
            // layers, so the destination index is layer * width + x.
            for (int layer = 0; layer < kArrayLayers; ++layer) {
                for (int x = 0; x < kExtent; ++x) {
                    const std::size_t index = static_cast<std::size_t>(layer) * kExtent + x;
                    EXPECT_EQ(texels[index], Expected1DArrayTexel(x, layer))
                        << "layer " << layer << " texel " << x << " read back "
                        << (texels[index] == kSeed ? "the seed (the store never reached it, or the readback came "
                                                     "from the stale CPU shadow)"
                                                   : "an unexpected value");
                }
            }
        }

        // The cube-map-array half. glFramebufferTexture2D has no token for the target, so the
        // scratch FBO used to stay incomplete and every read - including layer 0 - was answered
        // from the CPU shadow; the seed is what makes that visible rather than merely wrong.
        TEST_F(LayeredTextureReadbackScenario, GetTexImageReturnsEveryLayerFaceOfACubeMapArray) {
            if (!Ready()) return;
            if (!ImagesAreUsable()) GTEST_SKIP() << "no compute image uniforms";

            const GLuint program = MakeComputeProgram(kCubeArrayStoreSource);
            if (program == 0) return;

            const GLuint texture = TrackTexture();
            glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, texture);
            glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            const std::vector<GLuint> seed(static_cast<std::size_t>(kExtent) * kExtent * kCubeLayerFaces, kSeed);
            glTexImage3D(GL_TEXTURE_CUBE_MAP_ARRAY, 0, GL_R32UI, kExtent, kExtent, kCubeLayerFaces, 0, GL_RED_INTEGER,
                         GL_UNSIGNED_INT, seed.data());
            ASSERT_EQ(FirstGLError(), 0u) << "creating the R32UI cube-map-array texture errored";

            if (!DispatchStore(program, texture, kExtent, kExtent, kCubeLayerFaces)) return;

            std::vector<GLuint> texels(seed.size(), 0u);
            glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, texture);
            glGetTexImage(GL_TEXTURE_CUBE_MAP_ARRAY, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, texels.data());
            ASSERT_EQ(FirstGLError(), 0u) << "reading the cube-map-array level back errored";

            for (int layerFace = 0; layerFace < kCubeLayerFaces; ++layerFace) {
                for (int y = 0; y < kExtent; ++y) {
                    for (int x = 0; x < kExtent; ++x) {
                        const std::size_t index =
                            (static_cast<std::size_t>(layerFace) * kExtent + y) * kExtent + x;
                        EXPECT_EQ(texels[index], ExpectedCubeArrayTexel(x, y, layerFace))
                            << "layer-face " << layerFace << " texel (" << x << ", " << y << ") read back "
                            << (texels[index] == kSeed ? "the seed (the store never reached it, or the readback "
                                                         "came from the stale CPU shadow)"
                                                       : "an unexpected value");
                    }
                }
            }
        }

    } // namespace
} // namespace MGITest
